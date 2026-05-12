#include "BlueprintReaderRuntimeServer.h"
#include "BlueprintReaderRuntimeJson.h"
#include "BlueprintRuntimeIntrospector.h"

#include "Async/Async.h"
#include "Common/TcpListener.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/RunnableThread.h"
#include "HAL/ThreadSafeBool.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Misc/DateTime.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "SocketSubsystem.h"
#include "Sockets.h"

DEFINE_LOG_CATEGORY_STATIC(LogBlueprintReaderRuntime, Log, All);

namespace BlueprintReaderRuntime
{

// ---------------------------------------------------------------------------
// Connection worker — one per accepted socket.
// ---------------------------------------------------------------------------

class FRuntimeConnectionRunnable : public FRunnable
{
public:
	FRuntimeConnectionRunnable(FSocket* InSocket, FString InExpectedToken)
		: Socket(InSocket), ExpectedToken(MoveTemp(InExpectedToken))
	{}

	~FRuntimeConnectionRunnable() override
	{
		if (Socket)
		{
			if (ISocketSubsystem* Sub = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM))
			{
				Sub->DestroySocket(Socket);
			}
			Socket = nullptr;
		}
	}

	bool Init() override { return Socket != nullptr; }

	uint32 Run() override
	{
		// 1. Hello.
		if (!SendFrame(TEXT("{\"type\":\"hello\",\"version\":\"1\"}"))) return 0;

		// 2. Auth.
		FString AuthRaw;
		if (!ReadFrame(AuthRaw))
		{
			UE_LOG(LogBlueprintReaderRuntime, Warning, TEXT("Connection closed before auth"));
			return 0;
		}
		TSharedPtr<FJsonObject> AuthMsg = ParseJson(AuthRaw);
		FString PresentedToken;
		if (!AuthMsg.IsValid() ||
			!AuthMsg->TryGetStringField(TEXT("token"), PresentedToken) ||
			PresentedToken != ExpectedToken)
		{
			SendFrame(TEXT("{\"type\":\"auth_fail\"}"));
			UE_LOG(LogBlueprintReaderRuntime, Warning, TEXT("Auth failed; closing"));
			return 0;
		}
		SendFrame(TEXT("{\"type\":\"auth_ok\"}"));
		UE_LOG(LogBlueprintReaderRuntime, Display, TEXT("Runtime client authenticated"));

		// 3. Op loop.
		while (!bStopRequested)
		{
			FString FrameRaw;
			if (!ReadFrame(FrameRaw)) break;
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

			// Parse args once into a lookup map. Each arg has the
			// shape `-Key=Value` (or `-Key` for bool-flag args).
			FString OpName;
			TMap<FString, FString> ArgMap;
			for (const TSharedPtr<FJsonValue>& V : *ArgsArray)
			{
				if (!V.IsValid() || V->Type != EJson::String) continue;
				FString S = V->AsString();
				if (S.StartsWith(TEXT("-"))) S.RightChopInline(1);
				int32 EqIdx = INDEX_NONE;
				if (S.FindChar(TEXT('='), EqIdx))
				{
					FString Key = S.Left(EqIdx);
					FString Val = S.RightChop(EqIdx + 1);
					if (Key.Equals(TEXT("Op"), ESearchCase::IgnoreCase))
					{
						OpName = MoveTemp(Val);
					}
					else
					{
						ArgMap.Add(MoveTemp(Key), MoveTemp(Val));
					}
				}
				else
				{
					ArgMap.Add(S, TEXT("1"));
				}
			}

			// Dispatch to the game thread — UE reflection + Asset
			// Registry queries assume single-threaded access for
			// asset loading side effects. Block worker thread until
			// the dispatch produces a result.
			FString JsonResult;
			int32 Code = 0;
			FEvent* Done = FPlatformProcess::GetSynchEventFromPool(false);
			AsyncTask(ENamedThreads::GameThread,
				[&JsonResult, &Code, OpName, ArgMap, Done]()
				{
					HandleOp(OpName, ArgMap, JsonResult, Code);
					Done->Trigger();
				});
			Done->Wait();
			FPlatformProcess::ReturnSynchEventToPool(Done);

			// Splice the result JSON straight into the response frame —
			// avoids the double-serialization the editor live server
			// goes through via the on-disk temp file.
			FString Out = FString::Printf(
				TEXT("{\"type\":\"result\",\"id\":%d,\"code\":%d,\"json\":%s}\n"),
				RequestId, Code, *JsonResult);
			SendRaw(Out);
		}
		return 0;
	}

	void Stop() override { bStopRequested = true; }

private:
	// One dispatch table for all read ops. Output goes into JsonResult
	// (already a JSON literal, ready to splice into the response frame)
	// and Code is the result code (0 = ok, -1 = error, -2 = unsupported).
	static void HandleOp(const FString& OpName,
		const TMap<FString, FString>& ArgMap,
		FString& OutJson, int32& OutCode)
	{
		auto GetArg = [&](const TCHAR* Key, const FString& Fallback = FString())
		{
			if (const FString* V = ArgMap.Find(Key)) return *V;
			return Fallback;
		};

		const bool bPretty = false;  // wire is compact JSON

		if (OpName.Equals(TEXT("List"), ESearchCase::IgnoreCase))
		{
			TArray<FBPRRAssetSummary> Assets =
				FBlueprintRuntimeIntrospector::ListBlueprints(GetArg(TEXT("Path")));
			OutJson = FBlueprintReaderRuntimeJson::WriteListString(Assets, bPretty);
			OutCode = 0;
			return;
		}

		if (OpName.Equals(TEXT("Read"), ESearchCase::IgnoreCase))
		{
			const FString Asset = GetArg(TEXT("Asset"));
			TOptional<FBPRRBlueprint> BP = FBlueprintRuntimeIntrospector::Read(Asset);
			if (!BP.IsSet())
			{
				OutJson = FString::Printf(
					TEXT("{\"error\":\"asset not found: %s\"}"), *Asset);
				OutCode = -1;
				return;
			}
			OutJson = FBlueprintReaderRuntimeJson::WriteObjectString(
				FBlueprintReaderRuntimeJson::BlueprintToJson(*BP), bPretty);
			OutCode = 0;
			return;
		}

		if (OpName.Equals(TEXT("Variables"), ESearchCase::IgnoreCase) ||
			OpName.Equals(TEXT("Components"), ESearchCase::IgnoreCase) ||
			OpName.Equals(TEXT("Functions"), ESearchCase::IgnoreCase))
		{
			const FString Asset = GetArg(TEXT("Asset"));
			TOptional<FBPRRBlueprint> BP = FBlueprintRuntimeIntrospector::Read(Asset);
			if (!BP.IsSet())
			{
				OutJson = FString::Printf(
					TEXT("{\"error\":\"asset not found: %s\"}"), *Asset);
				OutCode = -1;
				return;
			}
			// Take advantage of the full-BP JSON shape — extract the
			// subset the client asked for.
			TSharedRef<FJsonObject> Full = FBlueprintReaderRuntimeJson::BlueprintToJson(*BP);
			FString SubKey = OpName.ToLower();  // "variables" | "components" | "functions"
			const TArray<TSharedPtr<FJsonValue>>* Subset = nullptr;
			if (Full->TryGetArrayField(SubKey, Subset) && Subset)
			{
				FString Out;
				TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
					TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
				FJsonSerializer::Serialize(*Subset, Writer);
				OutJson = MoveTemp(Out);
				OutCode = 0;
				return;
			}
			OutJson = TEXT("[]");
			OutCode = 0;
			return;
		}

		// Anything else — including all the editor-write ops + the
		// transpile/decompile family — isn't reachable from a cooked
		// build. Tell the client clearly so it can fall back.
		OutJson = FString::Printf(
			TEXT("{\"error\":\"op '%s' not supported in runtime mode "
			     "(packaged builds expose read-shape ops only)\","
			     "\"runtime_only\":true}"), *OpName);
		OutCode = -2;
	}

	// ---- JSON / framing helpers ------------------------------------------

	static FString MakeError(int32 Id, const FString& Msg)
	{
		FString Escaped = Msg.Replace(TEXT("\""), TEXT("\\\""));
		return FString::Printf(
			TEXT("{\"type\":\"error\",\"id\":%d,\"error\":\"%s\"}"),
			Id, *Escaped);
	}

	TSharedPtr<FJsonObject> ParseJson(const FString& Raw) const
	{
		TSharedPtr<FJsonObject> Out;
		TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Raw);
		FJsonSerializer::Deserialize(Reader, Out);
		return Out;
	}

	bool SendFrame(const FString& Line)
	{
		return SendRaw(Line + TEXT("\n"));
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
			if (!Socket->Send(Bytes + Sent, Total - Sent, Wrote) || Wrote == 0)
			{
				return false;
			}
			Sent += Wrote;
		}
		return true;
	}

	bool ReadFrame(FString& Out)
	{
		Out.Reset();
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
			uint8 Buf[4096];
			int32 BytesRead = 0;
			if (!Socket->Recv(Buf, sizeof(Buf), BytesRead) || BytesRead == 0)
			{
				return false;
			}
			FUTF8ToTCHAR Conv((const ANSICHAR*)Buf, BytesRead);
			PendingBuffer.AppendChars(Conv.Get(), Conv.Length());
		}
	}

	FSocket* Socket = nullptr;
	FString ExpectedToken;
	FString PendingBuffer;
	FThreadSafeBool bStopRequested = false;
};

// ---------------------------------------------------------------------------
// Listener state — connections list owned by the server, joined on Stop().
// ---------------------------------------------------------------------------

struct FRuntimeConnection
{
	TUniquePtr<FRuntimeConnectionRunnable> Runnable;
	TUniquePtr<FRunnableThread> Thread;
};

struct FServerState
{
	FCriticalSection Mu;
	TArray<TUniquePtr<FRuntimeConnection>> Connections;
};

static TUniquePtr<FServerState> GState;
static TUniquePtr<FRuntimeServer> GRuntimeServer;

FRuntimeServer* GetRuntimeServer() { return GRuntimeServer.Get(); }

// ---------------------------------------------------------------------------
// FRuntimeServer impl
// ---------------------------------------------------------------------------

FRuntimeServer::FRuntimeServer() {}
FRuntimeServer::~FRuntimeServer() { Stop(); }

bool FRuntimeServer::Start(int32 Port)
{
	if (Listener.IsValid())
	{
		UE_LOG(LogBlueprintReaderRuntime, Warning, TEXT("FRuntimeServer::Start called twice"));
		return true;
	}

	// Resolve port.
	int32 RequestedPort = Port;
	if (RequestedPort == 0)
	{
		FString EnvPort = FPlatformMisc::GetEnvironmentVariable(TEXT("BP_READER_RUNTIME_PORT"));
		if (!EnvPort.IsEmpty()) RequestedPort = FCString::Atoi(*EnvPort);
	}

	// Resolve token.
	ExpectedToken = FPlatformMisc::GetEnvironmentVariable(TEXT("BP_READER_RUNTIME_TOKEN"));
	if (ExpectedToken.IsEmpty())
	{
		ExpectedToken = FGuid::NewGuid().ToString(EGuidFormats::Digits)
		              + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	}

	ISocketSubsystem* Sub = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (!Sub)
	{
		UE_LOG(LogBlueprintReaderRuntime, Error, TEXT("No socket subsystem"));
		return false;
	}
	ListenerSocket = Sub->CreateSocket(NAME_Stream, TEXT("BPRRuntime"),
	                                    FNetworkProtocolTypes::IPv4);
	if (!ListenerSocket)
	{
		UE_LOG(LogBlueprintReaderRuntime, Error, TEXT("CreateSocket failed"));
		return false;
	}
	TSharedRef<FInternetAddr> BindAddr = Sub->CreateInternetAddr();
	BindAddr->SetIp(0x7F000001);  // 127.0.0.1
	BindAddr->SetPort(FMath::Max(0, RequestedPort));
	if (!ListenerSocket->Bind(*BindAddr))
	{
		UE_LOG(LogBlueprintReaderRuntime, Error,
			TEXT("Bind to 127.0.0.1:%d failed"), RequestedPort);
		Sub->DestroySocket(ListenerSocket);
		ListenerSocket = nullptr;
		return false;
	}
	if (!ListenerSocket->Listen(8))
	{
		UE_LOG(LogBlueprintReaderRuntime, Error, TEXT("Listen() failed"));
		Sub->DestroySocket(ListenerSocket);
		ListenerSocket = nullptr;
		return false;
	}
	TSharedRef<FInternetAddr> BoundAddr = Sub->CreateInternetAddr();
	ListenerSocket->GetAddress(*BoundAddr);
	BoundPort = BoundAddr->GetPort();

	Listener = MakeUnique<FTcpListener>(*ListenerSocket);
	Listener->OnConnectionAccepted().BindRaw(this, &FRuntimeServer::OnIncomingConnection);

	GState = MakeUnique<FServerState>();
	UE_LOG(LogBlueprintReaderRuntime, Display,
		TEXT("Runtime BP listener on 127.0.0.1:%d"), BoundPort);

	HandshakeWritten = WriteHandshakeFile();
	return true;
}

void FRuntimeServer::Stop()
{
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

bool FRuntimeServer::OnIncomingConnection(FSocket* Socket, const FIPv4Endpoint& Endpoint)
{
	// Loopback-only gate. Listener already bound to 127.0.0.1 so this
	// is defense-in-depth.
	if (Endpoint.Address != FIPv4Address(127, 0, 0, 1))
	{
		UE_LOG(LogBlueprintReaderRuntime, Warning,
			TEXT("Rejecting non-loopback connection from %s"), *Endpoint.ToString());
		if (ISocketSubsystem* Sub = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM))
		{
			Sub->DestroySocket(Socket);
		}
		return true;  // keep listener alive
	}

	if (!GState.IsValid()) return true;

	auto Runnable = MakeUnique<FRuntimeConnectionRunnable>(Socket, ExpectedToken);
	auto Thread = TUniquePtr<FRunnableThread>(FRunnableThread::Create(
		Runnable.Get(), TEXT("BPRRuntimeConn"), 0, TPri_BelowNormal));

	auto Conn = MakeUnique<FRuntimeConnection>();
	Conn->Runnable = MoveTemp(Runnable);
	Conn->Thread = MoveTemp(Thread);

	FScopeLock Lock(&GState->Mu);
	GState->Connections.Add(MoveTemp(Conn));
	return true;
}

FString FRuntimeServer::HandshakeFilePath()
{
	// Distinct filename from the editor's bp-reader-live.json — a
	// packaged game running alongside an open editor would otherwise
	// trample each other's handshakes.
	return FPaths::Combine(FPaths::ProjectSavedDir(),
	                       TEXT("bp-reader-runtime.json"));
}

bool FRuntimeServer::WriteHandshakeFile()
{
	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetNumberField(TEXT("version"), 1);
	Obj->SetStringField(TEXT("host"), TEXT("127.0.0.1"));
	Obj->SetNumberField(TEXT("port"), BoundPort);
	Obj->SetStringField(TEXT("token"), ExpectedToken);
	Obj->SetNumberField(TEXT("pid"), FPlatformProcess::GetCurrentProcessId());
	Obj->SetStringField(TEXT("started_at"), FDateTime::UtcNow().ToIso8601());
	Obj->SetStringField(TEXT("source"), TEXT("runtime"));  // distinguishes from editor handshake
	Obj->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());

	FString Out;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Obj, Writer);

	const FString Path = HandshakeFilePath();
	if (!FFileHelper::SaveStringToFile(Out, *Path,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		UE_LOG(LogBlueprintReaderRuntime, Warning,
			TEXT("Failed to write handshake file: %s"), *Path);
		return false;
	}
	UE_LOG(LogBlueprintReaderRuntime, Display,
		TEXT("Wrote runtime handshake: %s (port=%d)"), *Path, BoundPort);
	return true;
}

void FRuntimeServer::DeleteHandshakeFile()
{
	const FString Path = HandshakeFilePath();
	IFileManager::Get().Delete(*Path, /*bRequireExists*/false);
}

// ---------------------------------------------------------------------------
// Opt-in startup wired via CVar + env var.
// ---------------------------------------------------------------------------
//
// Default OFF. Shipping games shouldn't open a port silently. Two
// opt-in mechanisms: the CVar (set via console / DefaultGame.ini /
// command-line `-execcmds="bp.reader.listen 1"`), and the env var
// `BP_READER_RUNTIME_LISTEN=1` (the form CI / launchers use).

static void RestartListenerForCVar(IConsoleVariable*);
static FAutoConsoleVariable GCVarListen(
	TEXT("bp.reader.listen"),
	0,
	TEXT("Start the BlueprintReader runtime TCP listener so an MCP "
	     "server can introspect this game's UClass reflection data. "
	     "OFF by default. Localhost-only, token-auth. Set to 1 to "
	     "start, 0 to stop."),
	FConsoleVariableDelegate::CreateStatic(&RestartListenerForCVar),
	ECVF_Default);

static void StartIfRequested()
{
	const int32 CVarVal = GCVarListen->GetInt();
	FString EnvVar = FPlatformMisc::GetEnvironmentVariable(TEXT("BP_READER_RUNTIME_LISTEN"));
	const bool bEnvOn = EnvVar == TEXT("1") || EnvVar.Equals(TEXT("true"), ESearchCase::IgnoreCase);
	if (CVarVal == 0 && !bEnvOn) return;

	if (!GRuntimeServer.IsValid()) GRuntimeServer = MakeUnique<FRuntimeServer>();
	if (!GRuntimeServer->IsListening())
	{
		GRuntimeServer->Start();
	}
}

static void StopIfRunning()
{
	if (GRuntimeServer.IsValid())
	{
		GRuntimeServer->Stop();
	}
}

static void RestartListenerForCVar(IConsoleVariable* Var)
{
	if (Var->GetInt() != 0)
	{
		StartIfRequested();
	}
	else
	{
		StopIfRunning();
	}
}

// Called from FBlueprintReaderRuntimeModule::StartupModule via a
// PostEngineInit hook so the asset registry is ready.
void OnPostEngineInit_StartRuntimeListener()
{
	StartIfRequested();
}

void OnModuleShutdown_StopRuntimeListener()
{
	StopIfRunning();
	GRuntimeServer.Reset();
}

} // namespace BlueprintReaderRuntime
