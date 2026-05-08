#include "BlueprintReaderLiveServer.h"

#include "Async/Async.h"
#include "Common/TcpListener.h"
#include "Dom/JsonObject.h"
#include "HAL/PlatformProcess.h"
#include "HAL/RunnableThread.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "Misc/Paths.h"
#include "Misc/Guid.h"
#include "Misc/FileHelper.h"
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

    // Resolve port: explicit arg > env var > disabled.
    int32 ResolvedPort = Port;
    if (ResolvedPort == 0)
    {
        FString EnvPort = FPlatformMisc::GetEnvironmentVariable(TEXT("BP_READER_LIVE_PORT"));
        if (!EnvPort.IsEmpty()) ResolvedPort = FCString::Atoi(*EnvPort);
    }
    if (ResolvedPort <= 0)
    {
        UE_LOG(LogBlueprintReaderLive, Display,
            TEXT("BP_READER_LIVE_PORT not set; live server disabled"));
        return false;
    }

    ExpectedToken = FPlatformMisc::GetEnvironmentVariable(TEXT("BP_READER_LIVE_TOKEN"));
    if (ExpectedToken.IsEmpty())
    {
        UE_LOG(LogBlueprintReaderLive, Error,
            TEXT("BP_READER_LIVE_TOKEN not set — refusing to start live server "
                 "(unauthenticated localhost write access is unsafe). Pick a "
                 "random string, set the env var in BOTH the editor process "
                 "and the MCP server's process before launch."));
        return false;
    }

    // Loopback-only bind.
    FIPv4Endpoint Endpoint(FIPv4Address(127, 0, 0, 1), (uint16)ResolvedPort);
    Listener = MakeUnique<FTcpListener>(Endpoint);
    if (!Listener->Init())
    {
        UE_LOG(LogBlueprintReaderLive, Error,
            TEXT("FLiveServer: failed to bind 127.0.0.1:%d (port in use?)"), ResolvedPort);
        Listener.Reset();
        return false;
    }
    Listener->OnConnectionAccepted().BindRaw(this, &FLiveServer::OnIncomingConnection);
    BoundPort = ResolvedPort;

    GState = MakeUnique<FServerState>();
    UE_LOG(LogBlueprintReaderLive, Display,
        TEXT("FLiveServer listening on 127.0.0.1:%d"), ResolvedPort);
    return true;
}

void FLiveServer::Stop()
{
    if (Listener.IsValid())
    {
        Listener->Stop();
        Listener.Reset();
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
