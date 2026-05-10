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
extern int32 RunOneOpFromLiveServer(const FString& Params);

namespace
{

// Module-level singleton.
TUniquePtr<FLiveServer> GLiveServer;

// Per-connection state — owned by the worker FRunnable for its lifetime.
class FLiveConnectionRunnable : public FRunnable
{
public:
    FLiveConnectionRunnable(FSocket* InSocket, FString InExpectedToken)
        : Socket(InSocket), ExpectedToken(MoveTemp(InExpectedToken)) {}

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

            // Dispatch to the game thread and block until done.
            int32 Code = -1;
            FEvent* DoneEvent = FPlatformProcess::GetSynchEventFromPool(false);
            AsyncTask(ENamedThreads::GameThread, [&Code, Params, DoneEvent]()
            {
                Code = RunOneOpFromLiveServer(Params);
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

    // Resolve port: explicit arg > env var > 0 (kernel picks ephemeral).
    int32 RequestedPort = Port;
    if (RequestedPort == 0)
    {
        FString EnvPort = FPlatformMisc::GetEnvironmentVariable(TEXT("BP_READER_LIVE_PORT"));
        if (!EnvPort.IsEmpty()) RequestedPort = FCString::Atoi(*EnvPort);
    }
    // RequestedPort 0 means "let the kernel pick a free port" — no longer
    // a "disabled" sentinel.

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

    // Loopback-only bind. We bind the socket ourselves (rather than
    // using FTcpListener's FIPv4Endpoint constructor) so we can pass
    // port 0 and discover the kernel-picked port via GetAddress.
    ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    if (!SocketSubsystem)
    {
        UE_LOG(LogBlueprintReaderLive, Error, TEXT("FLiveServer: no socket subsystem"));
        return false;
    }
    ListenerSocket = SocketSubsystem->CreateSocket(NAME_Stream, TEXT("BPRLive"),
                                                   FNetworkProtocolTypes::IPv4);
    if (!ListenerSocket)
    {
        UE_LOG(LogBlueprintReaderLive, Error, TEXT("FLiveServer: CreateSocket failed"));
        return false;
    }
    TSharedRef<FInternetAddr> BindAddr = SocketSubsystem->CreateInternetAddr();
    BindAddr->SetIp(0x7F000001);  // 127.0.0.1 in network-order-agnostic form
    BindAddr->SetPort(FMath::Max(0, RequestedPort));
    if (!ListenerSocket->Bind(*BindAddr))
    {
        UE_LOG(LogBlueprintReaderLive, Error,
            TEXT("FLiveServer: bind to 127.0.0.1:%d failed (port in use?)"), RequestedPort);
        SocketSubsystem->DestroySocket(ListenerSocket);
        ListenerSocket = nullptr;
        return false;
    }
    if (!ListenerSocket->Listen(8))
    {
        UE_LOG(LogBlueprintReaderLive, Error, TEXT("FLiveServer: listen() failed"));
        SocketSubsystem->DestroySocket(ListenerSocket);
        ListenerSocket = nullptr;
        return false;
    }
    // Discover the actual bound port — needed when RequestedPort was 0.
    TSharedRef<FInternetAddr> BoundAddr = SocketSubsystem->CreateInternetAddr();
    ListenerSocket->GetAddress(*BoundAddr);
    BoundPort = BoundAddr->GetPort();

    // Hand the prebound socket to FTcpListener. Note: FSocket& ctor
    // does NOT take ownership — we destroy ListenerSocket in Stop().
    Listener = MakeUnique<FTcpListener>(*ListenerSocket);
    Listener->OnConnectionAccepted().BindRaw(this, &FLiveServer::OnIncomingConnection);

    GState = MakeUnique<FServerState>();
    UE_LOG(LogBlueprintReaderLive, Display,
        TEXT("FLiveServer listening on 127.0.0.1:%d"), BoundPort);

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

    auto Runnable = MakeUnique<FLiveConnectionRunnable>(Socket, ExpectedToken);
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

} // namespace BlueprintReader
