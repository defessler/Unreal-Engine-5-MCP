#include "BlueprintReaderLiveServer.h"

#include "Async/Async.h"
#include "Common/TcpListener.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/RunnableThread.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Sockets.h"
#include "SocketSubsystem.h"

DEFINE_LOG_CATEGORY_STATIC(LogBlueprintReaderLive, Log, All);

namespace BlueprintReader
{

// Forward decl from BlueprintReaderCommandlet.cpp's anonymous namespace.
// We can't include it (it's in an unnamed namespace), so the live server
// re-implements the dispatch by writing the args to a temp file via
// commandline and... actually the simpler path is to *fork* RunOneOp's
// dispatch table here. But that duplicates the EOp enum.
//
// Cleanest: expose an entry point from BlueprintReaderCommandlet.cpp.
// But editing that file's namespace is invasive. For v1 we shell out to
// the same dispatch by formatting a Params string and invoking it
// through a C++ entry point we add to the commandlet's TU.
//
// The hack: use the existing RunOneOp by spawning the commandlet... no,
// that's circular and defeats the live-backend purpose.
//
// The right answer: add a public function to BlueprintReaderCommandlet
// — `int32 RunOneOpFromLiveServer(const FString& Params)`. That just
// calls into the existing internal RunOneOp. We declare it extern here.
extern int32 RunOneOpFromLiveServer(uint64 ConnectionId, const FString& Params);
extern void  FlushBatchForConnection(uint64 ConnectionId);

namespace
{

// Module-level singleton.
TUniquePtr<FLiveServer> GLiveServer;

// Monotonic counter handing each accepted socket a unique ConnectionId.
// Starts at 1 (FThreadSafeCounter64::Increment returns post-increment
// value) — 0 stays reserved for "no session" (legacy direct callers).
// Lives at file-scope because FLiveServer is single-instance per editor
// process and has no need for the counter to be per-server-instance.
FThreadSafeCounter64 GLiveNextConnectionId;

// Per-connection state — owned by the worker FRunnable for its lifetime.
class FLiveConnectionRunnable : public FRunnable
{
public:
	FLiveConnectionRunnable(FSocket* InSocket, FString InExpectedToken,
							uint64 InConnectionId)
		: Socket(InSocket), ExpectedToken(MoveTemp(InExpectedToken)),
		  ConnectionId(InConnectionId) {}

	~FLiveConnectionRunnable() override
	{
		if (Socket)
		{
			ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
			if (SocketSubsystem) SocketSubsystem->DestroySocket(Socket);
			Socket = nullptr;
		}
	}

	bool Init() override { return Socket != nullptr; }

	uint32 Run() override
	{
		// RAII: on every exit path (auth failure, EOF, error) flush any
		// batched edits this connection had pending. Mirrors the cmdlet
		// server's disconnect plumbing — commit-partial-on-disconnect
		// (Task 4.4) is the same in either transport.
		struct FFlushOnExit
		{
			uint64 ConnId;
			~FFlushOnExit() { FlushBatchForConnection(ConnId); }
		} FlushGuard{ConnectionId};

		// 1. Send hello.
		if (!SendFrame(MakeHello())) return 0;

		// 2. Read auth frame.
		FString AuthRaw;
		if (!ReadFrame(AuthRaw))
		{
			UE_LOG(LogBlueprintReaderLive, Warning, TEXT("Connection closed before auth"));
			return 0;
		}
		TSharedPtr<FJsonObject> AuthMsg = ParseJson(AuthRaw);
		FString PresentedToken;
		if (!AuthMsg.IsValid() ||
			!AuthMsg->TryGetStringField(TEXT("token"), PresentedToken) ||
			PresentedToken != ExpectedToken)
		{
			UE_LOG(LogBlueprintReaderLive, Warning, TEXT("Auth failed; closing connection"));
			SendFrame(TEXT("{\"type\":\"auth_fail\"}"));
			return 0;
		}
		SendFrame(TEXT("{\"type\":\"auth_ok\"}"));
		UE_LOG(LogBlueprintReaderLive, Display, TEXT("Live client authenticated"));

		// 3. Loop: read op frames, dispatch on game thread, write result.
		while (!bStopRequested)
		{
			FString FrameRaw;
			if (!ReadFrame(FrameRaw)) break;  // EOF / error
			TSharedPtr<FJsonObject> Msg = ParseJson(FrameRaw);
			if (!Msg.IsValid())
			{
				SendFrame(MakeError(0, TEXT("malformed JSON frame")));
				continue;
			}
			FString Type;
			Msg->TryGetStringField(TEXT("type"), Type);
			if (Type != TEXT("op"))
			{
				SendFrame(MakeError(0, FString::Printf(
					TEXT("expected type=\"op\", got %s"), *Type)));
				continue;
			}
			int32 RequestId = 0;
			Msg->TryGetNumberField(TEXT("id"), RequestId);
			const TArray<TSharedPtr<FJsonValue>>* ArgsArray = nullptr;
			if (!Msg->TryGetArrayField(TEXT("args"), ArgsArray) || !ArgsArray)
			{
				SendFrame(MakeError(RequestId, TEXT("op frame missing args[]")));
				continue;
			}

			// Build the Params string the commandlet's RunOneOp expects.
			// Each arg comes in as `-Op=Foo`, `-Asset=/Game/...`, etc.
			// Concatenate with spaces; the commandlet uses FParse which
			// tokenizes on whitespace.
			FString Params;
			for (const TSharedPtr<FJsonValue>& V : *ArgsArray)
			{
				if (V.IsValid() && V->Type == EJson::String)
				{
					if (!Params.IsEmpty()) Params.AppendChar(TEXT(' '));
					Params.Append(V->AsString());
				}
			}

			// Add a unique -Out= path so the dispatch writes its JSON
			// somewhere we can read it back. Live mode uses a per-call
			// temp file just like the daemon does — RunOneOp's existing
			// EmitJson path stays unchanged.
			const FString OutPath = FPaths::Combine(
				FPaths::ProjectIntermediateDir(),
				FString::Printf(TEXT("bpr-live-%s.json"), *FGuid::NewGuid().ToString(EGuidFormats::DigitsLower)));
			Params.Append(FString::Printf(TEXT(" -Out=\"%s\" -Compact"), *OutPath));

			// Dispatch to the game thread and block until done. Pass
			// the connection id so per-session batch state lands in the
			// right registry slot (RunOneOpFromLiveServer installs the
			// FConnectionScope internally).
			int32 Code = -1;
			FEvent* DoneEvent = FPlatformProcess::GetSynchEventFromPool(false);
			const uint64 ConnId = ConnectionId;
			AsyncTask(ENamedThreads::GameThread, [&Code, Params, DoneEvent, ConnId]()
			{
				Code = RunOneOpFromLiveServer(ConnId, Params);
				DoneEvent->Trigger();
			});
			DoneEvent->Wait();
			FPlatformProcess::ReturnSynchEventToPool(DoneEvent);

			// Read the JSON the dispatch wrote to OutPath.
			FString JsonBody;
			if (FFileHelper::LoadFileToString(JsonBody, *OutPath))
			{
				IFileManager::Get().Delete(*OutPath);
			}
			else
			{
				JsonBody = TEXT("{}");
			}

			// Frame the response. JsonBody is already a JSON literal —
			// splice it into the result frame as-is (avoids double-
			// serialization). Newline-delimited.
			FString Out = FString::Printf(
				TEXT("{\"type\":\"result\",\"id\":%d,\"code\":%d,\"json\":%s}\n"),
				RequestId, Code, *JsonBody);
			SendRaw(Out);
		}

		UE_LOG(LogBlueprintReaderLive, Display, TEXT("Live connection closing"));
		return 0;
	}

	void Stop() override { bStopRequested = true; }

private:
	static FString MakeHello()
	{
		return TEXT("{\"type\":\"hello\",\"version\":\"1\"}");
	}
	static FString MakeError(int32 Id, const FString& Msg)
	{
		// Escape any quotes in Msg conservatively — RunOneOp's outputs
		// already serialize properly; this only fires on protocol errors
		// we generate, so the message text is controlled here.
		FString Escaped = Msg.Replace(TEXT("\""), TEXT("\\\""));
		return FString::Printf(
			TEXT("{\"type\":\"error\",\"id\":%d,\"error\":\"%s\"}"),
			Id, *Escaped);
	}

	TSharedPtr<FJsonObject> ParseJson(const FString& Raw)
	{
		TSharedPtr<FJsonObject> Out;
		TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Raw);
		if (!FJsonSerializer::Deserialize(Reader, Out)) return nullptr;
		return Out;
	}

	bool SendFrame(const FString& JsonLine)
	{
		return SendRaw(JsonLine + TEXT("\n"));
	}
	bool SendRaw(const FString& Text)
	{
		if (!Socket) return false;
		FTCHARToUTF8 Conv(*Text);
		const uint8* Bytes = (const uint8*)Conv.Get();
		int32 Total = Conv.Length();
		int32 Sent = 0;
		while (Sent < Total)
		{
			int32 Wrote = 0;
			if (!Socket->Send(Bytes + Sent, Total - Sent, Wrote) || Wrote == 0) return false;
			Sent += Wrote;
		}
		return true;
	}

	// Read a newline-delimited frame. Buffers data across reads.
	bool ReadFrame(FString& Out)
	{
		Out.Reset();
		// Drain any leftover from prior reads first (line feed boundary).
		while (true)
		{
			int32 NewlineIdx = INDEX_NONE;
			if (PendingBuffer.FindChar(TEXT('\n'), NewlineIdx))
			{
				Out = PendingBuffer.Left(NewlineIdx);
				PendingBuffer = PendingBuffer.RightChop(NewlineIdx + 1);
				Out.TrimStartAndEndInline();
				return true;
			}
			// Read more bytes.
			uint8 Buf[4096];
			int32 BytesRead = 0;
			if (!Socket->Recv(Buf, sizeof(Buf), BytesRead) || BytesRead == 0)
			{
				return false;  // connection closed
			}
			FUTF8ToTCHAR Conv((const ANSICHAR*)Buf, BytesRead);
			PendingBuffer.AppendChars(Conv.Get(), Conv.Length());
		}
	}

	FSocket* Socket = nullptr;
	FString ExpectedToken;
	uint64  ConnectionId = 0;  // allocated by FLiveServer::OnIncomingConnection
	FString PendingBuffer;
	FThreadSafeBool bStopRequested = false;
};

// One thread per connection; thread + runnable are owned by this struct
// and joined in destructor. Stored in a list inside FLiveServer so
// Stop() can join all active connections.
struct FLiveConnection
{
	TUniquePtr<FLiveConnectionRunnable> Runnable;
	TUniquePtr<FRunnableThread> Thread;
};

// Live-server-internal state. We keep connections in a list under a
// mutex so Stop() can shut them all down even while Accept fires.
struct FServerState
{
	FCriticalSection Mu;
	TArray<TUniquePtr<FLiveConnection>> Connections;
};
TUniquePtr<FServerState> GState;

} // anonymous namespace

FLiveServer::FLiveServer() {}
FLiveServer::~FLiveServer() { Stop(); }

// Helper: try to bind+listen on a given port. On success populates
// OutSocket + OutBoundPort and returns true. On failure logs the
// reason, destroys any partial socket, and returns false so the
// caller can retry with a different port.
static bool TryBindAndListen(ISocketSubsystem* Sub, int32 PortToTry,
							 FSocket*& OutSocket, int32& OutBoundPort)
{
	OutSocket = Sub->CreateSocket(NAME_Stream, TEXT("BPRLive"),
								  FNetworkProtocolTypes::IPv4);
	if (!OutSocket)
	{
		UE_LOG(LogBlueprintReaderLive, Error, TEXT("FLiveServer: CreateSocket failed"));
		return false;
	}
	// SO_REUSEADDR before bind: lets us reclaim the same port immediately
	// after a previous editor instance exits, even while its socket is
	// still in TIME_WAIT (~30-60 s window). Without this, killing the
	// editor and relaunching almost always falls back to ephemeral
	// because the cached port is technically "in use" by the kernel.
	// Loopback-only bind makes the security implication negligible —
	// the only thing that can bind to 127.0.0.1:<our port> is something
	// running with our user credentials, which already has full access
	// to our process anyway.
	OutSocket->SetReuseAddr(true);
	TSharedRef<FInternetAddr> BindAddr = Sub->CreateInternetAddr();
	BindAddr->SetIp(0x7F000001);  // 127.0.0.1 in network-order-agnostic form
	BindAddr->SetPort(FMath::Max(0, PortToTry));
	if (!OutSocket->Bind(*BindAddr))
	{
		// Common: port-in-use after a crashed editor that didn't free
		// the cached port (TIME_WAIT) or a real port conflict. Caller
		// decides whether to fall back to ephemeral.
		UE_LOG(LogBlueprintReaderLive, Log,
			TEXT("FLiveServer: bind to 127.0.0.1:%d declined"), PortToTry);
		Sub->DestroySocket(OutSocket);
		OutSocket = nullptr;
		return false;
	}
	if (!OutSocket->Listen(8))
	{
		UE_LOG(LogBlueprintReaderLive, Error, TEXT("FLiveServer: listen() failed"));
		Sub->DestroySocket(OutSocket);
		OutSocket = nullptr;
		return false;
	}
	TSharedRef<FInternetAddr> BoundAddr = Sub->CreateInternetAddr();
	OutSocket->GetAddress(*BoundAddr);
	OutBoundPort = BoundAddr->GetPort();
	return true;
}

bool FLiveServer::Start(int32 Port)
{
	if (Listener.IsValid())
	{
		UE_LOG(LogBlueprintReaderLive, Warning, TEXT("FLiveServer::Start called twice"));
		return true;
	}

	// Hard opt-out short-circuit before any work.
	{
		FString Disabled = FPlatformMisc::GetEnvironmentVariable(TEXT("BP_READER_LIVE_DISABLED"));
		if (Disabled == TEXT("1") || Disabled.Equals(TEXT("true"), ESearchCase::IgnoreCase))
		{
			UE_LOG(LogBlueprintReaderLive, Display,
				TEXT("BP_READER_LIVE_DISABLED=1; live server skipped"));
			return false;
		}
	}

	// Resolve port. Priority:
	//   1. Explicit `Port` arg (test / programmatic override)
	//   2. `BP_READER_LIVE_PORT` env var
	//   3. Persistent cache from a previous successful bind (this is
	//      what gives "relaunch keeps the same port" behavior — MCP
	//      clients that already know the port don't need to re-probe
	//      the handshake file after every editor restart)
	//   4. 0 → kernel picks an ephemeral port
	//
	// The cache is best-effort: if the cached port is now occupied
	// (another editor instance, a process that grabbed it during the
	// shutdown gap, port still in TIME_WAIT after a crash), we fall
	// back to ephemeral and overwrite the cache with the new port.
	int32 RequestedPort = Port;
	bool bPortFromExplicitSource = (Port != 0);
	if (RequestedPort == 0)
	{
		FString EnvPort = FPlatformMisc::GetEnvironmentVariable(TEXT("BP_READER_LIVE_PORT"));
		if (!EnvPort.IsEmpty())
		{
			RequestedPort = FCString::Atoi(*EnvPort);
			bPortFromExplicitSource = true;
		}
	}
	int32 CachedPortAttempted = 0;
	if (RequestedPort == 0 && !bPortFromExplicitSource)
	{
		const int32 Cached = ReadCachedPort();
		if (Cached > 0)
		{
			RequestedPort = Cached;
			CachedPortAttempted = Cached;
			UE_LOG(LogBlueprintReaderLive, Display,
				TEXT("FLiveServer: trying cached port %d from previous run"), Cached);
		}
	}

	// Resolve token: env var override > random 128-bit GUID.
	ExpectedToken = FPlatformMisc::GetEnvironmentVariable(TEXT("BP_READER_LIVE_TOKEN"));
	if (ExpectedToken.IsEmpty())
	{
		// Two GUIDs concatenated → 256 bits. Overkill for an auth token
		// on a localhost-only socket, but the cost is zero and it makes
		// brute-force not even worth thinking about.
		ExpectedToken = FGuid::NewGuid().ToString(EGuidFormats::Digits)
					  + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	}

	ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (!SocketSubsystem)
	{
		UE_LOG(LogBlueprintReaderLive, Error, TEXT("FLiveServer: no socket subsystem"));
		return false;
	}

	// First bind attempt — at RequestedPort (cached, env-var, or 0).
	if (!TryBindAndListen(SocketSubsystem, RequestedPort, ListenerSocket, BoundPort))
	{
		// Bind failure paths:
		//  - Explicit source (env var / arg) → don't second-guess the
		//    user; surface the error so they notice the port conflict.
		//  - Cached source → silently fall back to ephemeral. The cache
		//    is a hint, not a requirement, and dropping back is the
		//    whole point of having a fallback.
		//  - Otherwise (port was already 0) → unrecoverable.
		if (bPortFromExplicitSource || CachedPortAttempted == 0)
		{
			UE_LOG(LogBlueprintReaderLive, Error,
				TEXT("FLiveServer: bind failed on requested port %d"), RequestedPort);
			return false;
		}
		UE_LOG(LogBlueprintReaderLive, Display,
			TEXT("FLiveServer: cached port %d unavailable; falling back to ephemeral"),
			CachedPortAttempted);
		if (!TryBindAndListen(SocketSubsystem, 0, ListenerSocket, BoundPort))
		{
			UE_LOG(LogBlueprintReaderLive, Error,
				TEXT("FLiveServer: ephemeral fallback also failed"));
			return false;
		}
	}

	// Hand the prebound socket to FTcpListener. Note: FSocket& ctor
	// does NOT take ownership — we destroy ListenerSocket in Stop().
	Listener = MakeUnique<FTcpListener>(*ListenerSocket);
	Listener->OnConnectionAccepted().BindRaw(this, &FLiveServer::OnIncomingConnection);

	GState = MakeUnique<FServerState>();
	UE_LOG(LogBlueprintReaderLive, Display,
		TEXT("FLiveServer listening on 127.0.0.1:%d"), BoundPort);

	// Persist the bound port for the next launch. Skip when an env var
	// or explicit arg forced the port — that caller wants control and
	// shouldn't have their next run silently overridden by us. When
	// the cached port worked, the write is a no-op-shaped overwrite
	// (same value); harmless.
	if (!bPortFromExplicitSource)
	{
		WriteCachedPort(BoundPort);
	}

	// Drop the handshake file so the MCP server can discover port +
	// token automatically. Failure here is non-fatal — explicit env-var
	// configuration still works.
	HandshakeWritten = WriteHandshakeFile();
	return true;
}

void FLiveServer::Stop()
{
	// Drop the handshake file FIRST so MCP-server probes immediately
	// start failing rather than racing the listener teardown.
	if (HandshakeWritten)
	{
		DeleteHandshakeFile();
		HandshakeWritten = false;
	}
	if (Listener.IsValid())
	{
		Listener->Stop();
		Listener.Reset();
	}
	if (ListenerSocket)
	{
		if (ISocketSubsystem* Sub = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM))
		{
			Sub->DestroySocket(ListenerSocket);
		}
		ListenerSocket = nullptr;
	}
	if (GState.IsValid())
	{
		FScopeLock Lock(&GState->Mu);
		for (auto& Conn : GState->Connections)
		{
			if (Conn->Runnable) Conn->Runnable->Stop();
		}
		for (auto& Conn : GState->Connections)
		{
			if (Conn->Thread) Conn->Thread->WaitForCompletion();
		}
		GState->Connections.Empty();
		GState.Reset();
	}
	BoundPort = 0;
	ExpectedToken.Reset();
}

FString FLiveServer::HandshakeFilePath()
{
	// <ProjectDir>/Saved/bp-reader-live.json — colocated with UE's
	// existing Saved/ tree which is gitignored by default. The MCP
	// server walks up from the .uproject to find this same file.
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("bp-reader-live.json"));
}

bool FLiveServer::WriteHandshakeFile()
{
	const FString Path = HandshakeFilePath();
	// Ensure Saved/ exists (it almost always does, but the editor can
	// be launched against a project where it's been wiped).
	const FString Dir = FPaths::GetPath(Path);
	IFileManager::Get().MakeDirectory(*Dir, /*Tree=*/true);

	// Serialize a compact JSON line. fmt-style printf: BoundPort and
	// process id are ints; ExpectedToken is the GUID hex string we
	// generated. ISO-8601 UTC timestamp for diagnostics.
	const FDateTime Now = FDateTime::UtcNow();
	const uint32 Pid = FPlatformProcess::GetCurrentProcessId();
	const FString Json = FString::Printf(
		TEXT("{\"version\":1,\"host\":\"127.0.0.1\",\"port\":%d,")
		TEXT("\"token\":\"%s\",\"pid\":%u,\"started_at\":\"%s\"}\n"),
		BoundPort, *ExpectedToken, Pid, *Now.ToIso8601());

	if (!FFileHelper::SaveStringToFile(Json, *Path,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
			&IFileManager::Get(),
			FILEWRITE_EvenIfReadOnly))
	{
		UE_LOG(LogBlueprintReaderLive, Warning,
			TEXT("Failed to write handshake file %s — MCP server will need "
				 "BP_READER_LIVE_PORT/TOKEN env vars set explicitly"),
			*Path);
		return false;
	}
	UE_LOG(LogBlueprintReaderLive, Display,
		TEXT("Wrote live-handshake file: %s (port=%d)"), *Path, BoundPort);
	return true;
}

FString FLiveServer::PortCacheFilePath()
{
	// Separate file from the handshake — the handshake is deleted on
	// editor shutdown so MCP probes fail fast against a dead editor;
	// the port cache survives so the next launch can reuse the port.
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("bp-reader-live-port.json"));
}

int32 FLiveServer::ReadCachedPort()
{
	const FString Path = PortCacheFilePath();
	if (!IFileManager::Get().FileExists(*Path)) return 0;
	FString Body;
	if (!FFileHelper::LoadFileToString(Body, *Path)) return 0;

	// Minimal one-key JSON, parsed by the same JsonReader pattern the
	// rest of the plugin uses. Tolerant of pretty-printed or compact
	// forms; any malformed payload returns 0 (treat as cache miss).
	TSharedPtr<FJsonObject> Obj;
	TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Body);
	if (!FJsonSerializer::Deserialize(Reader, Obj) || !Obj.IsValid()) return 0;
	int32 Port = 0;
	Obj->TryGetNumberField(TEXT("port"), Port);

	// Sanity: must be a legal user-space TCP port. Reject negatives,
	// zero (means "ephemeral", not a real cache), and privileged
	// ports (<1024) which we should never have bound to in the first
	// place but defend against corrupted caches.
	if (Port < 1024 || Port > 65535) return 0;
	return Port;
}

void FLiveServer::WriteCachedPort(int32 Port)
{
	if (Port < 1024 || Port > 65535)
	{
		// Don't overwrite the cache with garbage. The bind path
		// shouldn't ever produce an out-of-range port, but if it
		// somehow did we'd rather keep last good value than persist
		// bad data.
		return;
	}
	const FString Path = PortCacheFilePath();
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), /*Tree=*/true);
	const FString Json = FString::Printf(TEXT("{\"port\":%d}\n"), Port);
	if (!FFileHelper::SaveStringToFile(Json, *Path,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
			&IFileManager::Get(), FILEWRITE_EvenIfReadOnly))
	{
		UE_LOG(LogBlueprintReaderLive, Verbose,
			TEXT("Could not write port cache %s; next launch will be ephemeral"),
			*Path);
	}
}

void FLiveServer::DeleteHandshakeFile()
{
	const FString Path = HandshakeFilePath();
	if (!IFileManager::Get().FileExists(*Path)) return;
	if (!IFileManager::Get().Delete(*Path, /*RequireExists=*/false,
									/*EvenReadOnly=*/true,
									/*Quiet=*/true))
	{
		UE_LOG(LogBlueprintReaderLive, Warning,
			TEXT("Failed to delete handshake file %s; the MCP server may "
				 "see stale port/token until the file is removed."),
			*Path);
	}
}

bool FLiveServer::OnIncomingConnection(FSocket* Socket, const FIPv4Endpoint& Endpoint)
{
	// Defense-in-depth: FTcpListener bound to loopback already filters,
	// but double-check before handing off.
	if (Endpoint.Address != FIPv4Address(127, 0, 0, 1))
	{
		UE_LOG(LogBlueprintReaderLive, Warning,
			TEXT("Rejecting non-loopback connection from %s"), *Endpoint.ToString());
		return false;
	}
	UE_LOG(LogBlueprintReaderLive, Display,
		TEXT("Live client connected from %s"), *Endpoint.ToString());

	// Allocate a unique ConnectionId for this client's batch state.
	// Post-increment so the first connection gets 1 (0 = "no session").
	const uint64 ConnId = static_cast<uint64>(GLiveNextConnectionId.Increment());
	auto Runnable = MakeUnique<FLiveConnectionRunnable>(Socket, ExpectedToken, ConnId);
	auto* RunnablePtr = Runnable.Get();
	auto Thread = TUniquePtr<FRunnableThread>(FRunnableThread::Create(
		RunnablePtr, TEXT("BlueprintReaderLiveConnection")));
	if (!Thread)
	{
		UE_LOG(LogBlueprintReaderLive, Error, TEXT("Failed to spawn connection thread"));
		return false;
	}
	auto Conn = MakeUnique<FLiveConnection>();
	Conn->Runnable = MoveTemp(Runnable);
	Conn->Thread = MoveTemp(Thread);
	if (GState.IsValid())
	{
		FScopeLock Lock(&GState->Mu);
		GState->Connections.Add(MoveTemp(Conn));
	}
	return true;
}

FLiveServer* GetLiveServer()
{
	if (!GLiveServer.IsValid()) GLiveServer = MakeUnique<FLiveServer>();
	return GLiveServer.Get();
}

}    // namespace BlueprintReader
