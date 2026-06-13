#include "BlueprintReaderCmdletServer.h"

#include "BlueprintReaderLiveServer.h"   // UX-P4a: GameThreadHeartbeatAgeMs()
#include "BlueprintReaderModalChannel.h" // TEST-2 P1a: modal side-channel
#include "Policies/CondensedJsonPrintPolicy.h"  // TEST-2 P1a: single-line modal_result
#include "DaemonProgress.h"
#include "Async/Async.h"
#include "Common/TcpListener.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/RunnableThread.h"
#include "HAL/ThreadSafeBool.h"      // FThreadSafeBool — bStopRequested member
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

#include <cstdio>  // std::snprintf for PID-into-lockfile

// Raw Win32 lifetime-lock — UE's FArchive writers don't hold an
// OS-level exclusive lock, which is the whole reason this file uses
// `CreateFileW(dwShareMode=0)` directly. Mirrors the pattern in
// mcp-server/src/util/SingleInstanceLock.cpp.
//
// NOMINMAX before <windows.h> stops the Windows `min`/`max` macros from
// clobbering `std::numeric_limits<T>::max()` etc. in engine headers
// (MovieScene, RelativePtr, ...) that get pulled in transitively.
// AllowWindowsPlatformTypes wraps the raw include so UE's type-system
// macros (DWORD / HANDLE / etc.) round-trip cleanly.
#if PLATFORM_WINDOWS
	#ifndef NOMINMAX
		#define NOMINMAX
	#endif    // NOMINMAX
	#include "Windows/AllowWindowsPlatformTypes.h"
	#include <windows.h>
	#include "Windows/HideWindowsPlatformTypes.h"
#endif    // PLATFORM_WINDOWS

DEFINE_LOG_CATEGORY_STATIC(LogBlueprintReaderCmdlet, Log, All);

namespace BlueprintReader
{

// Re-uses the same entry point the live server uses to dispatch into
// BlueprintReaderCommandlet.cpp's RunOneOp. Both servers feed
// `-Op=...` flag strings; the only difference is the transport.
extern int32 RunOneOpFromLiveServer(uint64 ConnectionId, const FString& Params);
extern void  FlushBatchForConnection(uint64 ConnectionId);

namespace
{

// Module-level singleton.
TUniquePtr<FCmdletServer> GCmdletServer;

// Per-connection state — owned by the worker FRunnable for its lifetime.
class FCmdletConnectionRunnable : public FRunnable
{
public:
	FCmdletConnectionRunnable(FSocket* InSocket, FString InExpectedToken,
							  FCmdletServer* InServer, uint64 InConnectionId)
		: Socket(InSocket), ExpectedToken(MoveTemp(InExpectedToken)),
		  Server(InServer), ConnectionId(InConnectionId) {}

	~FCmdletConnectionRunnable() override
	{
		if (Socket)
		{
			ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
			if (SocketSubsystem)
			{
				SocketSubsystem->DestroySocket(Socket);
			}
			Socket = nullptr;
		}
	}

	bool Init() override { return Socket != nullptr; }

	uint32 Run() override
	{
		// Notify the server on every exit path (auth failure, EOF,
		// explicit stop, etc.) so the idle-shutdown counter stays
		// accurate. RAII guard avoids having to remember it at every
		// `return 0` below.
		struct FNotifyDisconnect
		{
			FCmdletServer* Srv;
			uint64         ConnId;
			~FNotifyDisconnect() { if (Srv) Srv->OnClientDisconnected(ConnId); }
		} DisconnectGuard{Server, ConnectionId};

		// 1. Send hello.
		if (!SendFrame(MakeHello()))
		{
			return 0;
		}

		// 2. Read auth frame.
		FString AuthRaw;
		if (!ReadFrame(AuthRaw))
		{
			UE_LOG(LogBlueprintReaderCmdlet, Warning, TEXT("Connection closed before auth"));
			return 0;
		}
		TSharedPtr<FJsonObject> AuthMsg = ParseJson(AuthRaw);
		FString PresentedToken;
		// REL-24: constant-time compare (see LiveServer — same rationale).
		auto TokensEqualConstantTime = [](const FString& A, const FString& B)
		{
			if (A.Len() != B.Len())
			{
				return false;
			}
			uint32 Diff = 0;
			for (int32 i = 0; i < A.Len(); ++i)
			{
				Diff |= static_cast<uint32>(A[i]) ^ static_cast<uint32>(B[i]);
			}
			return Diff == 0;
		};
		if (!AuthMsg.IsValid() ||
			!AuthMsg->TryGetStringField(TEXT("token"), PresentedToken) ||
			!TokensEqualConstantTime(PresentedToken, ExpectedToken))
		{
			UE_LOG(LogBlueprintReaderCmdlet, Warning, TEXT("Auth failed; closing connection"));
			SendFrame(TEXT("{\"type\":\"auth_fail\"}"));
			return 0;
		}
		SendFrame(TEXT("{\"type\":\"auth_ok\"}"));
		UE_LOG(LogBlueprintReaderCmdlet, Display, TEXT("Cmdlet client authenticated"));

		// 3. Loop: read op frames, dispatch on game thread, write result.
		while (!bStopRequested)
		{
			FString FrameRaw;
			if (!ReadFrame(FrameRaw))
			{
				break;  // EOF / error
			}
			TSharedPtr<FJsonObject> Msg = ParseJson(FrameRaw);
			if (!Msg.IsValid())
			{
				SendFrame(MakeError(0, TEXT("malformed JSON frame")));
				continue;
			}
			FString Type;
			Msg->TryGetStringField(TEXT("type"), Type);

			// UX-P4a: answer a health/ping frame INLINE on this worker thread
			// (no game-thread dispatch) so it returns even when the daemon's game
			// thread is wedged. Reads only the lock-free heartbeat atomic.
			if (Type == TEXT("health") || Type == TEXT("ping"))
			{
				int32 HealthId = 0;
				Msg->TryGetNumberField(TEXT("id"), HealthId);
				const int64 AgeMs = BlueprintReader::GameThreadHeartbeatAgeMs();
				SendRaw(FString::Printf(
					TEXT("{\"type\":\"health\",\"id\":%d,\"game_thread_age_ms\":%lld,\"pid\":%u}\n"),
					HealthId, static_cast<long long>(AgeMs),
					FPlatformProcess::GetCurrentProcessId()));
				continue;
			}

			// TEST-2 P1a: `modal` side-channel frame — enqueue + wait on the worker
			// thread, drained on the game thread (idle ticker or modal-loop
			// delegate), so it answers even while a modal wedges op dispatch.
			if (Type == TEXT("modal"))
			{
				int32 ModalId = 0; Msg->TryGetNumberField(TEXT("id"), ModalId);
				FString Action; Msg->TryGetStringField(TEXT("action"), Action);
				FString ButtonPath; Msg->TryGetStringField(TEXT("button_path"), ButtonPath);
				int32 TimeoutMs = 5000; Msg->TryGetNumberField(TEXT("timeout_ms"), TimeoutMs);
				TSharedPtr<FJsonObject> Res =
					BlueprintReader::SubmitModalCommand(Action, ButtonPath, TimeoutMs);
				Res->SetStringField(TEXT("type"), TEXT("modal_result"));
				Res->SetNumberField(TEXT("id"), ModalId);
				FString Serialized;
				// Condensed (single-line) — the wire is newline-delimited JSON.
				TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
					TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Serialized);
				FJsonSerializer::Serialize(Res.ToSharedRef(), Writer);
				SendRaw(Serialized + TEXT("\n"));
				continue;
			}

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
			//
			// Quote the VALUE of a `-Key=Value` arg when it contains whitespace
			// and isn't already quoted: FParse::Value stops an unquoted value at
			// the first space, silently truncating pin names ("Dimension 1",
			// "Array Element", friendly names with spaces), comment text, and any
			// other space-bearing value — which broke wire_pins / set_pin_default /
			// add_node Comment over the daemon path.
			FString Params;
			for (const TSharedPtr<FJsonValue>& V : *ArgsArray)
			{
				if (V.IsValid() && V->Type == EJson::String)
				{
					if (!Params.IsEmpty())
					{
						Params.AppendChar(TEXT(' '));
					}
					FString Arg = V->AsString();
					int32 EqIdx = INDEX_NONE;
					if (Arg.FindChar(TEXT('='), EqIdx))
					{
						const FString Key = Arg.Left(EqIdx + 1);   // includes '='
						const FString Val = Arg.Mid(EqIdx + 1);
						const bool bHasSpace = Val.Contains(TEXT(" ")) || Val.Contains(TEXT("\t"));
						const bool bQuoted   = Val.Len() >= 2 && Val.StartsWith(TEXT("\"")) && Val.EndsWith(TEXT("\""));
						if (bHasSpace && !bQuoted)
						{
							Arg = Key + TEXT("\"") + Val + TEXT("\"");
						}
					}
					Params.Append(Arg);
				}
			}

			// REL-7: graceful daemon shutdown. Handled HERE on the worker
			// thread — deliberately NOT via the game-thread dispatch below —
			// so the MCP server's quit request works even while a long op has
			// the game thread busy. The polling loop notices WantsShutdown()
			// as soon as it comes back around; a truly wedged game thread is
			// covered by the caller's terminate-after-timeout fallback.
			// (The LIVE editor server has no such op — a tool must never be
			// able to close the user's editor.)
			if (Params.StartsWith(TEXT("-Op=Quit")) && Server)
			{
				UE_LOG(LogBlueprintReaderCmdlet, Display,
					TEXT("REL-7: Quit op received — requesting graceful shutdown"));
				Server->RequestShutdown();
				SendRaw(FString::Printf(
					TEXT("{\"type\":\"result\",\"id\":%d,\"code\":0,")
					TEXT("\"json\":{\"ok\":true,\"quitting\":true}}\n"),
					RequestId));
				continue;
			}

			// PERF-1: use an in-memory sentinel path instead of a temp file.
			// EmitJson writes to the sentinel path "__MEM__:<ptr>", which the
			// commandlet detects and copies directly into *pJsonOut instead of
			// touching disk. This eliminates the two filesystem operations
			// (write + read + delete) per call on the warm-daemon path.
			FString JsonBody;
			const FString OutPath = FString::Printf(
				TEXT("__MEM__:%llu"), reinterpret_cast<uint64>(&JsonBody));
			Params.Append(FString::Printf(TEXT(" -Out=\"%s\" -Compact"), *OutPath));

			// Dispatch to the game thread and block until done. Pass
			// the connection id so per-session batch state lands in the
			// right registry slot (RunOneOpFromLiveServer installs the
			// FConnectionScope internally).
			//
			// Lifetime contract: JsonBody lives on the connection-thread stack.
			// DoneEvent->Wait() blocks until the game-thread lambda completes,
			// so JsonBody is always alive when the lambda writes into it.
			int32 Code = -1;
			FEvent* DoneEvent = FPlatformProcess::GetSynchEventFromPool(false);
			const uint64 ConnId = ConnectionId;
			// Register a progress queue for this op so the game-thread dispatch
			// (FScopedProgressCapture) can push FScopedSlowTask progress to us.
			BlueprintReader::RegisterProgressQueue(ConnId);
			AsyncTask(ENamedThreads::GameThread, [&Code, Params, DoneEvent, ConnId]()
			{
				Code = RunOneOpFromLiveServer(ConnId, Params);
				DoneEvent->Trigger();
			});
			// Poll-drain progress while the op runs: send {"type":"progress"}
			// frames before the result. ~50ms latency; one drain after completion
			// catches any final frame. A normal (fast, no-slow-task) op just
			// drains nothing and the result follows immediately.
			auto DrainAndSendProgress = [this, RequestId, ConnId]()
			{
				for (const BlueprintReader::FDaemonProgressEntry& P : BlueprintReader::DrainProgress(ConnId))
				{
					FString Msg = P.Message;
					Msg.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
					Msg.ReplaceInline(TEXT("\""), TEXT("\\\""));
					Msg.ReplaceInline(TEXT("\n"), TEXT(" "));
					Msg.ReplaceInline(TEXT("\r"), TEXT(" "));
					SendRaw(FString::Printf(
						TEXT("{\"type\":\"progress\",\"id\":%d,\"progress\":%f,\"total\":%f,\"message\":\"%s\"}\n"),
						RequestId, P.Current, P.Total, *Msg));
				}
			};
			while (!DoneEvent->Wait(5))   // PERF-2: was 50 ms; 5 ms gives 10× throughput on rapid-fire reads
			{
				DrainAndSendProgress();
			}
			DrainAndSendProgress();
			FPlatformProcess::ReturnSynchEventToPool(DoneEvent);
			BlueprintReader::UnregisterProgressQueue(ConnId);

			// PERF-1: JsonBody was written directly by EmitJson via the
			// __MEM__ sentinel. No disk I/O needed; JsonBody is already set.
			// Fallback: if EmitJson couldn't resolve the sentinel (e.g. a
			// legacy code path didn't recognise it), try reading the temp
			// file from the old path.
			if (JsonBody.IsEmpty())
			{
				if (FFileHelper::LoadFileToString(JsonBody, *OutPath))
				{
					IFileManager::Get().Delete(*OutPath);
				}
				else
				{
					JsonBody = TEXT("{}");
				}
			}

			// Frame the response. JsonBody is already a JSON literal —
			// splice it into the result frame as-is (avoids double-
			// serialization). Newline-delimited.
			FString Out = FString::Printf(
				TEXT("{\"type\":\"result\",\"id\":%d,\"code\":%d,\"json\":%s}\n"),
				RequestId, Code, *JsonBody);
			SendRaw(Out);
		}

		UE_LOG(LogBlueprintReaderCmdlet, Display, TEXT("Cmdlet connection closing"));
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
		if (!FJsonSerializer::Deserialize(Reader, Out))
		{
			return nullptr;
		}
		return Out;
	}

	bool SendFrame(const FString& JsonLine)
	{
		return SendRaw(JsonLine + TEXT("\n"));
	}
	bool SendRaw(const FString& Text)
	{
		if (!Socket)
		{
			return false;
		}
		FTCHARToUTF8 Conv(*Text);
		const uint8* Bytes = (const uint8*)Conv.Get();
		int32 Total = Conv.Length();
		int32 Sent = 0;
		while (Sent < Total)
		{
			int32 Wrote = 0;
			if (!Socket->Send(Bytes + Sent, Total - Sent, Wrote) || Wrote == 0)
			{
				return false;
			}
			Sent += Wrote;
		}
		return true;
	}

	// Read a newline-delimited frame. Buffers data across reads.
	bool ReadFrame(FString& Out)
	{
		// REL-17: hard cap on a single frame — an endless newline-less stream
		// would otherwise grow PendingBuffer until the daemon OOMs.
		constexpr int32 MaxFrameChars = 10 * 1024 * 1024;
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
			if (PendingBuffer.Len() > MaxFrameChars)
			{
				UE_LOG(LogBlueprintReaderCmdlet, Warning,
					TEXT("Client sent an oversized frame (>%d chars, no newline) "
						 "— disconnecting to protect the daemon."), MaxFrameChars);
				return false;  // treated as connection-closed by the caller
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
	FCmdletServer* Server = nullptr;  // back-pointer for OnClientDisconnected
	uint64  ConnectionId = 0;         // assigned by AllocateConnectionId
	FString PendingBuffer;
	FThreadSafeBool bStopRequested = false;
};

// One thread per connection; thread + runnable are owned by this struct
// and joined in destructor. Stored in a list inside FCmdletServer so
// Stop() can join all active connections.
struct FCmdletConnection
{
	TUniquePtr<FCmdletConnectionRunnable> Runnable;
	TUniquePtr<FRunnableThread> Thread;
};

// Cmdlet-server-internal state. We keep connections in a list under a
// mutex so Stop() can shut them all down even while Accept fires.
//
// Note: prefixed with "Cmdlet" rather than the simpler `FServerState`
// because BlueprintReaderLiveServer.cpp has a sibling FServerState in
// its own anonymous namespace. Anonymous namespaces do NOT shield
// against ODR when adaptive-unity builds merge the two .cpp files
// into a single translation unit. Same goes for GCmdletState and
// CmdletTryBindAndListen below — do not "clean up" the prefix.
struct FCmdletServerState
{
	FCriticalSection Mu;
	TArray<TUniquePtr<FCmdletConnection>> Connections;
};
TUniquePtr<FCmdletServerState> GCmdletState;

} // anonymous namespace

// H3: off-game-thread daemon liveness watchdog. Defined in namespace BlueprintReader
// (not the anonymous inner namespace) so the friend declaration in the header can
// resolve it. Reads only atomics + wall-clock, never UObjects — safe on any thread.
class FDaemonWatchdog : public FRunnable
{
public:
	explicit FDaemonWatchdog(FCmdletServer& InServer) : Server(InServer) {}

	bool Init() override { return true; }

	uint32 Run() override
	{
		while (!bStop)
		{
			FPlatformProcess::Sleep(5.0f);  // poll every 5 s
			if (bStop) { break; }

			const int64 Now         = FDateTime::UtcNow().ToUnixTimestamp();
			const int64 LastTick    = Server.LastGameThreadTickUnix.GetValue();
			const int64 Started     = Server.StartedAtUnix;
			const int32 MaxLifetime = Server.MaxLifetimeSeconds;
			const int32 WedgeSecs   = Server.WedgeTimeoutSeconds;

			bool bForce = false;
			if (MaxLifetime > 0 && Started > 0 && Now - Started >= MaxLifetime)
			{
				UE_LOG(LogBlueprintReaderCmdlet, Warning,
					TEXT("FDaemonWatchdog: max-lifetime %d s exceeded (%lld s running) - force-exiting"),
					MaxLifetime, static_cast<long long>(Now - Started));
				bForce = true;
			}
			if (!bForce && WedgeSecs > 0 && LastTick > 0 && Now - LastTick >= WedgeSecs)
			{
				UE_LOG(LogBlueprintReaderCmdlet, Warning,
					TEXT("FDaemonWatchdog: game thread wedged (%lld s silent) - force-exiting"),
					static_cast<long long>(Now - LastTick));
				bForce = true;
			}
			if (bForce)
			{
				Server.TerminateOnSignal();
				FPlatformMisc::RequestExit(/*bForce=*/true);
				// Hard fallback in case RequestExit doesn't terminate immediately.
				FPlatformProcess::Sleep(3.0f);
				::ExitProcess(1);
			}
		}
		return 0;
	}

	void Stop() override { bStop = true; }

private:
	FCmdletServer&  Server;
	FThreadSafeBool bStop = false;
};

FCmdletServer::FCmdletServer() {}
FCmdletServer::~FCmdletServer() { Stop(); }

bool FCmdletServer::WantsShutdown() const
{
	if (bShuttingDown)
	{
		return true;
	}

	// Hard max-lifetime backstop (off by default). Fires regardless of
	// activity — covers the case where a wedged-open connection keeps
	// ActiveConnections > 0 so the idle path below can never trigger.
	if (MaxLifetimeSeconds > 0 && StartedAtUnix > 0)
	{
		const int64 Now = FDateTime::UtcNow().ToUnixTimestamp();
		if (Now - StartedAtUnix >= static_cast<int64>(MaxLifetimeSeconds))
		{
			return true;
		}
	}

	// Idle check: only fire once at least one client has fully
	// connected + disconnected. Otherwise a fresh daemon with no
	// clients yet would shut itself down immediately on the first
	// poll, which is the opposite of useful — we want the daemon
	// to stay up so the first MCP-server probe finds it.
	if (ActiveConnections.GetValue() == 0)
	{
		const int64 SinceLast = LastDisconnectAtUnix.GetValue();
		if (SinceLast > 0)
		{
			const int64 Now = FDateTime::UtcNow().ToUnixTimestamp();
			if (Now - SinceLast >= static_cast<int64>(IdleSeconds))
			{
				return true;
			}
		}
		else if (GraceSeconds > 0 && StartedAtUnix > 0)
		{
			// No client has ever connected. If we've been up longer than
			// the startup grace period the MCP server that spawned us
			// must have crashed before completing the TCP handshake —
			// self-exit so we don't linger as an orphan process.
			const int64 Now = FDateTime::UtcNow().ToUnixTimestamp();
			if (Now - StartedAtUnix >= static_cast<int64>(GraceSeconds))
			{
				return true;
			}
		}
	}
	return false;
}

void FCmdletServer::OnClientDisconnected(uint64 ConnectionId)
{
	// Task 4.4: commit-partial on disconnect. Flush any pending batched
	// edits the connection had open. FlushBatchForConnection schedules a
	// synchronous game-thread compile/save (or skips per
	// BP_READER_BATCH_ON_DISCONNECT=discard). No-op when ConnectionId==0
	// (early-failure path before a real session was established).
	//
	// Skip the flush if we're shutting down. FlushBatchForConnection
	// dispatches via AsyncTask(ENamedThreads::GameThread, ...) and waits
	// on FEvent::Wait(). During shutdown the game thread (= the daemon's
	// main loop) is parked in Stop()'s thread-join waiting for THIS
	// runnable to exit. That's a classic deadlock: game thread waits for
	// runnable, runnable waits for AsyncTask, AsyncTask needs game
	// thread to pump. Skipping the flush sacrifices in-flight pending
	// edits — acceptable: the daemon is exiting and the BP state lives
	// only in memory anyway. The behavior matches
	// BP_READER_BATCH_ON_DISCONNECT=discard semantics for the shutdown
	// edge case. Issue #86.
	if (!bShuttingDown)
	{
		FlushBatchForConnection(ConnectionId);
	}

	// Decrement first, then sample. FThreadSafeCounter::Decrement
	// returns the new value, so when it hits zero we know we're the
	// last connection out and the idle window should start.
	const int32 Remaining = ActiveConnections.Decrement();
	if (Remaining <= 0)
	{
		// Clamp at zero — defensive against any stray underflow path.
		// FThreadSafeCounter::Set returns the previous value, ignore it.
		if (Remaining < 0)
		{
			ActiveConnections.Set(0);
		}
		LastDisconnectAtUnix.Set(FDateTime::UtcNow().ToUnixTimestamp());
	}
}

uint64 FCmdletServer::AllocateConnectionId()
{
	// FThreadSafeCounter64::Increment returns the value AFTER the +1,
	// so the first call returns 1 — perfect: 0 is reserved for "no
	// session" (legacy callers).
	return static_cast<uint64>(NextConnectionId.Increment());
}

// Helper: try to bind+listen on a given port. On success populates
// OutSocket + OutBoundPort and returns true. On failure logs the
// reason, destroys any partial socket, and returns false so the
// caller can retry with a different port.
static bool CmdletTryBindAndListen(ISocketSubsystem* Sub, int32 PortToTry,
							 FSocket*& OutSocket, int32& OutBoundPort)
{
	OutSocket = Sub->CreateSocket(NAME_Stream, TEXT("BPRCmdlet"),
								  FNetworkProtocolTypes::IPv4);
	if (!OutSocket)
	{
		UE_LOG(LogBlueprintReaderCmdlet, Error, TEXT("FCmdletServer: CreateSocket failed"));
		return false;
	}
	// SO_REUSEADDR before bind: lets us reclaim the same port immediately
	// after a previous daemon exits, even while its socket is still in
	// TIME_WAIT (~30-60 s window). Without this, killing the daemon and
	// relaunching almost always falls back to ephemeral because the
	// cached port is technically "in use" by the kernel. Loopback-only
	// bind makes the security implication negligible.
	OutSocket->SetReuseAddr(true);
	TSharedRef<FInternetAddr> BindAddr = Sub->CreateInternetAddr();
	BindAddr->SetIp(0x7F000001);  // 127.0.0.1 in network-order-agnostic form
	BindAddr->SetPort(FMath::Max(0, PortToTry));
	if (!OutSocket->Bind(*BindAddr))
	{
		UE_LOG(LogBlueprintReaderCmdlet, Log,
			TEXT("FCmdletServer: bind to 127.0.0.1:%d declined"), PortToTry);
		Sub->DestroySocket(OutSocket);
		OutSocket = nullptr;
		return false;
	}
	if (!OutSocket->Listen(8))
	{
		UE_LOG(LogBlueprintReaderCmdlet, Error, TEXT("FCmdletServer: listen() failed"));
		Sub->DestroySocket(OutSocket);
		OutSocket = nullptr;
		return false;
	}
	TSharedRef<FInternetAddr> BoundAddr = Sub->CreateInternetAddr();
	OutSocket->GetAddress(*BoundAddr);
	OutBoundPort = BoundAddr->GetPort();
	return true;
}

bool FCmdletServer::Start(int32 Port)
{
	if (Listener.IsValid())
	{
		UE_LOG(LogBlueprintReaderCmdlet, Warning, TEXT("FCmdletServer::Start called twice"));
		return true;
	}

	// Hard opt-out short-circuit before any work.
	{
		FString Disabled = FPlatformMisc::GetEnvironmentVariable(TEXT("BP_READER_CMDLET_DISABLED"));
		if (Disabled == TEXT("1") || Disabled.Equals(TEXT("true"), ESearchCase::IgnoreCase))
		{
			UE_LOG(LogBlueprintReaderCmdlet, Display,
				TEXT("BP_READER_CMDLET_DISABLED=1; cmdlet server skipped"));
			return false;
		}
	}

	// Idle-shutdown timeout — override the 300s default via env. Reject
	// values < 5s to prevent flapping (a too-small idle window means
	// the daemon dies between back-to-back tool calls and respawns
	// each time, defeating the point). Unparseable values are ignored
	// silently — daemon still starts with the default.
	{
		FString IdleStr = FPlatformMisc::GetEnvironmentVariable(TEXT("BP_READER_DAEMON_IDLE_SECONDS"));
		if (!IdleStr.IsEmpty())
		{
			const int32 Parsed = FCString::Atoi(*IdleStr);
			if (Parsed >= 5)
			{
				IdleSeconds = Parsed;
				UE_LOG(LogBlueprintReaderCmdlet, Display,
					TEXT("FCmdletServer: idle shutdown after %d s (env override)"), IdleSeconds);
			}
			else
			{
				UE_LOG(LogBlueprintReaderCmdlet, Warning,
					TEXT("FCmdletServer: BP_READER_DAEMON_IDLE_SECONDS=%s rejected (must be >= 5); using default %d s"),
					*IdleStr, IdleSeconds);
			}
		}
	}

	// Hard max-lifetime backstop. Default 3600 s; WantsShutdown() fires after
	// this many wall-clock seconds regardless of active connections, covering
	// the case where a wedged connection keeps ActiveConnections > 0 forever.
	// Set BP_READER_DAEMON_MAX_LIFETIME_SECONDS=0 to disable entirely.
	{
		StartedAtUnix = FDateTime::UtcNow().ToUnixTimestamp();
		FString MaxStr = FPlatformMisc::GetEnvironmentVariable(TEXT("BP_READER_DAEMON_MAX_LIFETIME_SECONDS"));
		if (!MaxStr.IsEmpty())
		{
			const int32 Parsed = FCString::Atoi(*MaxStr);
			if (Parsed == 0)
			{
				MaxLifetimeSeconds = 0;
				UE_LOG(LogBlueprintReaderCmdlet, Display,
					TEXT("FCmdletServer: hard max lifetime disabled (BP_READER_DAEMON_MAX_LIFETIME_SECONDS=0)"));
			}
			else if (Parsed > 0)
			{
				MaxLifetimeSeconds = Parsed;
				UE_LOG(LogBlueprintReaderCmdlet, Display,
					TEXT("FCmdletServer: hard max lifetime %d s (env override)"), MaxLifetimeSeconds);
			}
			else
			{
				UE_LOG(LogBlueprintReaderCmdlet, Warning,
					TEXT("FCmdletServer: BP_READER_DAEMON_MAX_LIFETIME_SECONDS=%s rejected (must be >= 0); using default %d s"),
					*MaxStr, MaxLifetimeSeconds);
			}
		}
		else
		{
			UE_LOG(LogBlueprintReaderCmdlet, Verbose,
				TEXT("FCmdletServer: hard max lifetime %d s (default)"), MaxLifetimeSeconds);
		}
	}

	// Startup grace period. If no client connects within GraceSeconds of
	// daemon start, WantsShutdown() exits the daemon — prevents orphaned
	// daemons from a parent MCP server that crashed before TCP handshake.
	{
		FString GraceStr = FPlatformMisc::GetEnvironmentVariable(TEXT("BP_READER_DAEMON_GRACE_SECONDS"));
		if (!GraceStr.IsEmpty())
		{
			const int32 Parsed = FCString::Atoi(*GraceStr);
			if (Parsed >= 0)
			{
				GraceSeconds = Parsed;
				UE_LOG(LogBlueprintReaderCmdlet, Display,
					TEXT("FCmdletServer: startup grace period %d s (env override)"), GraceSeconds);
			}
			else
			{
				UE_LOG(LogBlueprintReaderCmdlet, Warning,
					TEXT("FCmdletServer: BP_READER_DAEMON_GRACE_SECONDS=%s rejected (must be >= 0); using default %d s"),
					*GraceStr, GraceSeconds);
			}
		}
	}

	// Acquire the lifetime lock FIRST. If another daemon is already
	// running against this project we want to bail out before we even
	// touch the socket subsystem — racing the bind makes the failure
	// mode less obvious. This is the multi-session design's "one
	// daemon per project" contract; the MCP-server side spins until
	// the existing daemon publishes a handshake and connects to it.
	if (!AcquireLifetimeLock())
	{
		UE_LOG(LogBlueprintReaderCmdlet, Display,
			TEXT("FCmdletServer: another daemon is already running for this project; refusing to start"));
		return false;
	}

	// Resolve port. Priority:
	//   1. Explicit `Port` arg (test / programmatic override)
	//   2. `BP_READER_CMDLET_PORT` env var
	//   3. Persistent cache from a previous successful bind
	//   4. 0 → kernel picks an ephemeral port
	int32 RequestedPort = Port;
	bool bPortFromExplicitSource = (Port != 0);
	if (RequestedPort == 0)
	{
		FString EnvPort = FPlatformMisc::GetEnvironmentVariable(TEXT("BP_READER_CMDLET_PORT"));
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
			UE_LOG(LogBlueprintReaderCmdlet, Display,
				TEXT("FCmdletServer: trying cached port %d from previous run"), Cached);
		}
	}

	// Resolve token: env var override > random 256-bit GUID-pair.
	ExpectedToken = FPlatformMisc::GetEnvironmentVariable(TEXT("BP_READER_CMDLET_TOKEN"));
	if (ExpectedToken.IsEmpty())
	{
		ExpectedToken = FGuid::NewGuid().ToString(EGuidFormats::Digits)
					  + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	}

	ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (!SocketSubsystem)
	{
		UE_LOG(LogBlueprintReaderCmdlet, Error, TEXT("FCmdletServer: no socket subsystem"));
		ReleaseLifetimeLock();
		return false;
	}

	// First bind attempt — at RequestedPort (cached, env-var, or 0).
	if (!CmdletTryBindAndListen(SocketSubsystem, RequestedPort, ListenerSocket, BoundPort))
	{
		// Bind failure paths:
		//  - Explicit source (env var / arg) → don't second-guess the
		//    user; surface the error so they notice the port conflict.
		//  - Cached source → silently fall back to ephemeral.
		//  - Otherwise (port was already 0) → unrecoverable.
		if (bPortFromExplicitSource || CachedPortAttempted == 0)
		{
			UE_LOG(LogBlueprintReaderCmdlet, Error,
				TEXT("FCmdletServer: bind failed on requested port %d"), RequestedPort);
			ReleaseLifetimeLock();
			return false;
		}
		UE_LOG(LogBlueprintReaderCmdlet, Display,
			TEXT("FCmdletServer: cached port %d unavailable; falling back to ephemeral"),
			CachedPortAttempted);
		if (!CmdletTryBindAndListen(SocketSubsystem, 0, ListenerSocket, BoundPort))
		{
			UE_LOG(LogBlueprintReaderCmdlet, Error,
				TEXT("FCmdletServer: ephemeral fallback also failed"));
			ReleaseLifetimeLock();
			return false;
		}
	}

	// Hand the prebound socket to FTcpListener. Note: FSocket& ctor
	// does NOT take ownership — we destroy ListenerSocket in Stop().
	Listener = MakeUnique<FTcpListener>(*ListenerSocket);
	Listener->OnConnectionAccepted().BindRaw(this, &FCmdletServer::OnIncomingConnection);

	GCmdletState = MakeUnique<FCmdletServerState>();
	UE_LOG(LogBlueprintReaderCmdlet, Display,
		TEXT("FCmdletServer listening on 127.0.0.1:%d"), BoundPort);

	// Persist the bound port for the next launch. Skip when an env var
	// or explicit arg forced the port.
	if (!bPortFromExplicitSource)
	{
		WriteCachedPort(BoundPort);
	}

	// Drop the handshake file so the MCP server can discover port +
	// token automatically. Failure here is non-fatal — explicit env-var
	// configuration still works.
	HandshakeWritten = WriteHandshakeFile();

	// H3: spawn the off-game-thread liveness watchdog. Default wedge timeout
	// is 120 s; set BP_READER_DAEMON_WEDGE_SECONDS=0 to disable.
	{
		FString WedgeStr = FPlatformMisc::GetEnvironmentVariable(TEXT("BP_READER_DAEMON_WEDGE_SECONDS"));
		if (!WedgeStr.IsEmpty())
		{
			const int32 Parsed = FCString::Atoi(*WedgeStr);
			WedgeTimeoutSeconds = (Parsed >= 0) ? Parsed : WedgeTimeoutSeconds;
			UE_LOG(LogBlueprintReaderCmdlet, Display,
				TEXT("FCmdletServer: game-thread wedge timeout %d s"), WedgeTimeoutSeconds);
		}
		FDaemonWatchdog* WatchdogRunnable = new FDaemonWatchdog(*this);
		FRunnableThread* RawThread = FRunnableThread::Create(
			WatchdogRunnable, TEXT("BPR-DaemonWatchdog"), 0, TPri_BelowNormal);
		if (RawThread)
		{
			WatchdogThread = TUniquePtr<FRunnableThread>(RawThread);
		}
		else
		{
			delete WatchdogRunnable;
			UE_LOG(LogBlueprintReaderCmdlet, Warning,
				TEXT("FCmdletServer: failed to start liveness watchdog thread"));
		}
	}

	return true;
}

void FCmdletServer::TerminateOnSignal()
{
	// Best-effort synchronous cleanup invoked from the Win32
	// console-control handler thread. We deliberately skip listener
	// / connection thread joins (the main thread will do those in
	// Stop() if the polling loop gets there; if the OS hard-kills
	// us first, the kernel reaps them on process exit). What we DO
	// need to do synchronously is delete the handshake file and
	// release the OS-level lock — these are the on-disk artifacts
	// that would otherwise mislead the NEXT daemon launch's "is one
	// already running?" probe.
	//
	// Idempotent: safe to call after Stop() or twice in a row.
	bShuttingDown = true;
	if (HandshakeWritten)
	{
		DeleteHandshakeFile();
		HandshakeWritten = false;
	}
	// ReleaseLifetimeLock CloseHandle's the OS-level lock handle and
	// best-effort-deletes the lock file. Closing the handle releases
	// the exclusive lock so the next daemon launch can acquire it
	// even before the OS gets around to closing all our handles on
	// process exit.
	ReleaseLifetimeLock();
}

void FCmdletServer::Stop()
{
	// Signal the daemon main loop to exit. Set this before tearing
	// down anything else so any code path that polls WantsShutdown()
	// sees the request immediately.
	bShuttingDown = true;

	// H3: stop the liveness watchdog before anything else so it doesn't
	// race a normal clean shutdown and force-exit while we're tearing down.
	if (WatchdogThread.IsValid())
	{
		WatchdogThread->Kill(/*bShouldWait=*/true);
		WatchdogThread.Reset();
	}
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
	if (GCmdletState.IsValid())
	{
		FScopeLock Lock(&GCmdletState->Mu);
		for (auto& Conn : GCmdletState->Connections)
		{
			if (Conn->Runnable)
			{
				Conn->Runnable->Stop();
			}
		}
		for (auto& Conn : GCmdletState->Connections)
		{
			if (Conn->Thread)
			{
				Conn->Thread->WaitForCompletion();
			}
		}
		GCmdletState->Connections.Empty();
		GCmdletState.Reset();
	}
	BoundPort = 0;
	ExpectedToken.Reset();

	// Release the lifetime lock LAST — until it's released the next
	// daemon launch will refuse to start. Doing this after the
	// handshake + listener teardown means any in-flight client that
	// races a relaunch will at worst see a "no handshake yet" gap
	// rather than two daemons trying to bind the same port.
	ReleaseLifetimeLock();
}

FString FCmdletServer::HandshakeFilePath()
{
	// <ProjectDir>/Saved/bp-reader-cmdlet.json — colocated with UE's
	// existing Saved/ tree which is gitignored by default. The MCP
	// server walks up from the .uproject to find this same file.
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("bp-reader-cmdlet.json"));
}

bool FCmdletServer::WriteHandshakeFile()
{
	const FString Path = HandshakeFilePath();
	// Ensure Saved/ exists (it almost always does, but the daemon can
	// be launched against a project where it's been wiped).
	const FString Dir = FPaths::GetPath(Path);
	IFileManager::Get().MakeDirectory(*Dir, /*Tree=*/true);

	const FDateTime Now = FDateTime::UtcNow();
	const uint32 Pid = FPlatformProcess::GetCurrentProcessId();
	const FString Json = FString::Printf(
		TEXT("{\"version\":1,\"host\":\"127.0.0.1\",\"port\":%d,")
		TEXT("\"token\":\"%s\",\"pid\":%u,\"started_at\":\"%s\"}\n"),
		BoundPort, *ExpectedToken, Pid, *Now.ToIso8601());

	// REL-8: write-to-temp + rename so a concurrently-polling MCP client can
	// never read a partially-written handshake. Same-volume rename is atomic
	// to readers.
	const FString Tmp = Path + TEXT(".tmp");
	if (!FFileHelper::SaveStringToFile(Json, *Tmp,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
			&IFileManager::Get(),
			FILEWRITE_EvenIfReadOnly) ||
		!IFileManager::Get().Move(*Path, *Tmp, /*Replace=*/true, /*EvenIfReadOnly=*/true))
	{
		UE_LOG(LogBlueprintReaderCmdlet, Warning,
			TEXT("Failed to write handshake file %s — MCP server will need "
				 "BP_READER_CMDLET_PORT/TOKEN env vars set explicitly"),
			*Path);
		return false;
	}
	UE_LOG(LogBlueprintReaderCmdlet, Display,
		TEXT("Wrote cmdlet-handshake file: %s (port=%d)"), *Path, BoundPort);
	return true;
}

FString FCmdletServer::PortCacheFilePath()
{
	// Separate file from the handshake — the handshake is deleted on
	// daemon shutdown so MCP probes fail fast against a dead daemon;
	// the port cache survives so the next launch can reuse the port.
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("bp-reader-cmdlet-port.json"));
}

int32 FCmdletServer::ReadCachedPort()
{
	const FString Path = PortCacheFilePath();
	if (!IFileManager::Get().FileExists(*Path))
	{
		return 0;
	}
	FString Body;
	if (!FFileHelper::LoadFileToString(Body, *Path))
	{
		return 0;
	}

	TSharedPtr<FJsonObject> Obj;
	TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Body);
	if (!FJsonSerializer::Deserialize(Reader, Obj) || !Obj.IsValid())
	{
		return 0;
	}
	int32 Port = 0;
	Obj->TryGetNumberField(TEXT("port"), Port);

	if (Port < 1024 || Port > 65535)
	{
		return 0;
	}
	return Port;
}

void FCmdletServer::WriteCachedPort(int32 Port)
{
	if (Port < 1024 || Port > 65535)
	{
		return;
	}
	const FString Path = PortCacheFilePath();
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), /*Tree=*/true);
	const FString Json = FString::Printf(TEXT("{\"port\":%d}\n"), Port);
	// REL-8: temp + rename (see WriteHandshakeFile).
	const FString Tmp = Path + TEXT(".tmp");
	if (!FFileHelper::SaveStringToFile(Json, *Tmp,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
			&IFileManager::Get(), FILEWRITE_EvenIfReadOnly) ||
		!IFileManager::Get().Move(*Path, *Tmp, /*Replace=*/true, /*EvenIfReadOnly=*/true))
	{
		UE_LOG(LogBlueprintReaderCmdlet, Verbose,
			TEXT("Could not write port cache %s; next launch will be ephemeral"),
			*Path);
	}
}

void FCmdletServer::DeleteHandshakeFile()
{
	const FString Path = HandshakeFilePath();
	if (!IFileManager::Get().FileExists(*Path))
	{
		return;
	}
	if (!IFileManager::Get().Delete(*Path, /*RequireExists=*/false,
									/*EvenReadOnly=*/true,
									/*Quiet=*/true))
	{
		UE_LOG(LogBlueprintReaderCmdlet, Warning,
			TEXT("Failed to delete handshake file %s; the MCP server may "
				 "see stale port/token until the file is removed."),
			*Path);
	}
}

FString FCmdletServer::LifetimeLockFilePath()
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("bp-reader-cmdlet.lock"));
}

bool FCmdletServer::AcquireLifetimeLock()
{
	if (LifetimeLockHandle != nullptr)
	{
		// Already held — Start() was called twice. Idempotent success.
		return true;
	}
	const FString LockPath = LifetimeLockFilePath();
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(LockPath), /*Tree=*/true);

#if PLATFORM_WINDOWS
	// CreateFileW with dwShareMode=0 → exclusive open. A second daemon
	// process attempting CreateFileW on the same path while we hold
	// the handle gets ERROR_SHARING_VIOLATION and we treat that as
	// "another daemon is running, abort." UE's `FArchive` writers
	// (IFileManager::CreateFileWriter) do NOT do this — they open
	// with FILE_SHARE_READ | FILE_SHARE_WRITE, so a second daemon
	// could open them in parallel and the probe would silently miss.
	HANDLE H = CreateFileW(*LockPath,
						   GENERIC_READ | GENERIC_WRITE,
						   /*dwShareMode=*/0,
						   /*lpSecurityAttributes=*/nullptr,
						   OPEN_ALWAYS,
						   FILE_ATTRIBUTE_NORMAL,
						   nullptr);
	if (H == INVALID_HANDLE_VALUE)
	{
		const DWORD Err = GetLastError();
		UE_LOG(LogBlueprintReaderCmdlet, Error,
			TEXT("Could not acquire %s (err=%lu) — another daemon is already running"),
			*LockPath, Err);
		LifetimeLockHandle = nullptr;
		return false;
	}
	LifetimeLockHandle = static_cast<void*>(H);

	// PID write is diagnostic-only and only readable after an abnormal
	// exit (the lock file is deleted on clean shutdown); lock semantics
	// do not depend on it.
	const uint32 Pid = FPlatformProcess::GetCurrentProcessId();
	char Buf[32];
	const int N = std::snprintf(Buf, sizeof(Buf), "%u\n", Pid);
	if (N > 0)
	{
		DWORD Written = 0;
		// Cast WriteFile return to void: payload is diagnostic-only and
		// lock semantics don't depend on it succeeding.
		(void)WriteFile(H, Buf, static_cast<DWORD>(N), &Written, nullptr);
		FlushFileBuffers(H);
	}
	return true;
#else    // PLATFORM_WINDOWS
	// **Non-Windows fallback is currently a no-op lock**: it succeeds
	// even when another daemon is already running, because
	// `IFileManager::CreateFileWriter` opens with
	// `FILE_SHARE_READ | FILE_SHARE_WRITE`. Two daemons launched
	// simultaneously on Linux/Mac will both believe they own the lock.
	// Fix before shipping any non-Windows daemon — switch to
	// `::open(O_CREAT|O_RDWR)` + `::flock(LOCK_EX|LOCK_NB)` mirroring
	// the pattern in `mcp-server/src/util/SingleInstanceLock.cpp`.
	IFileHandle* Handle = IFileManager::Get().CreateFileWriter(*LockPath, FILEWRITE_None);
	if (!Handle)
	{
		UE_LOG(LogBlueprintReaderCmdlet, Error,
			TEXT("Could not acquire %s — another daemon is already running"),
			*LockPath);
		LifetimeLockHandle = nullptr;
		return false;
	}
	LifetimeLockHandle = static_cast<void*>(Handle);
	return true;
#endif    // PLATFORM_WINDOWS
}

void FCmdletServer::ReleaseLifetimeLock()
{
	if (LifetimeLockHandle == nullptr)
	{
		return;
	}
#if PLATFORM_WINDOWS
	HANDLE H = static_cast<HANDLE>(LifetimeLockHandle);
	CloseHandle(H);
#else    // PLATFORM_WINDOWS
	// The non-Windows fallback above stores an IFileHandle* — delete it.
	IFileHandle* Handle = static_cast<IFileHandle*>(LifetimeLockHandle);
	delete Handle;
#endif    // PLATFORM_WINDOWS
	LifetimeLockHandle = nullptr;

	// Best-effort: delete the lock file so the next launch sees a
	// clean slate. If we crashed instead of cleanly shutting down,
	// the file persists, but the OS releases the handle on process
	// exit so the next daemon can still acquire the lock — only the
	// stale PID inside the file is misleading, and it'll be
	// overwritten on the next successful acquire.
	const FString Path = LifetimeLockFilePath();
	IFileManager::Get().Delete(*Path, /*RequireExists=*/false,
							   /*EvenReadOnly=*/true,
							   /*Quiet=*/true);
}

bool FCmdletServer::OnIncomingConnection(FSocket* Socket, const FIPv4Endpoint& Endpoint)
{
	// Defense-in-depth: FTcpListener bound to loopback already filters,
	// but double-check before handing off.
	if (Endpoint.Address != FIPv4Address(127, 0, 0, 1))
	{
		UE_LOG(LogBlueprintReaderCmdlet, Warning,
			TEXT("Rejecting non-loopback connection from %s"), *Endpoint.ToString());
		return false;
	}
	UE_LOG(LogBlueprintReaderCmdlet, Display,
		TEXT("Cmdlet client connected from %s"), *Endpoint.ToString());

	// Early-out if the daemon is tearing down: don't spawn a worker we
	// can't track. Without this check, a thread could start with the
	// socket and runnable, then on `Conn` getting dropped (because
	// GCmdletState went invalid by the Connections.Add point below),
	// the runnable + socket get destructed under the thread while it's
	// actively using them. Use-after-free. Issue #86.
	if (bShuttingDown || !GCmdletState.IsValid())
	{
		ISocketSubsystem* Sub = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		if (Sub)
		{
			Sub->DestroySocket(Socket);
		}
		return false;
	}

	// Increment the active-connection counter BEFORE spawning the
	// worker thread so a fast-disconnect race (Run starts + exits
	// before we record the connect) can't underflow the counter.
	// OnClientDisconnected matches this with a fetch_sub in the
	// runnable's exit guard.
	ActiveConnections.Increment();

	const uint64 ConnId = AllocateConnectionId();
	auto Runnable = MakeUnique<FCmdletConnectionRunnable>(Socket, ExpectedToken, this, ConnId);
	auto* RunnablePtr = Runnable.Get();
	auto Thread = TUniquePtr<FRunnableThread>(FRunnableThread::Create(
		RunnablePtr, TEXT("BlueprintReaderCmdletConnection")));
	if (!Thread)
	{
		UE_LOG(LogBlueprintReaderCmdlet, Error, TEXT("Failed to spawn connection thread"));
		// Thread never started, so the runnable's exit guard won't fire.
		// Roll back the increment manually to keep the counter honest.
		// Pass ConnId=0 here — nothing was ever batched against it, so
		// there's no batch state to flush.
		OnClientDisconnected(0);
		return false;
	}
	auto Conn = MakeUnique<FCmdletConnection>();
	Conn->Runnable = MoveTemp(Runnable);
	Conn->Thread = MoveTemp(Thread);
	// Re-check state under the lock. The shutdown path checks
	// bShuttingDown under the same lock before calling Stop(), so if we
	// win the race here our connection is tracked properly; if we lose
	// we need to clean up rather than leak. The signal-the-runnable-to-
	// stop path is the safe shutdown — the worker's Run() will see
	// bStopRequested and exit, then the thread joins normally.
	{
		FScopeLock Lock(&GCmdletState->Mu);
		if (bShuttingDown)
		{
			// State now invalid; signal the runnable to stop. Its dtor
			// (when Conn unwinds at end of this function) will destroy
			// the socket; the thread will exit naturally.
			Conn->Runnable->Stop();
			// Don't add to Connections — Stop() will reap from the
			// listener's accept side too.
			OnClientDisconnected(ConnId);
			return false;
		}
		GCmdletState->Connections.Add(MoveTemp(Conn));
	}
	return true;
}

FCmdletServer* GetCmdletServer()
{
	if (!GCmdletServer.IsValid())
	{
		GCmdletServer = MakeUnique<FCmdletServer>();
	}
	return GCmdletServer.Get();
}

}    // namespace BlueprintReader
