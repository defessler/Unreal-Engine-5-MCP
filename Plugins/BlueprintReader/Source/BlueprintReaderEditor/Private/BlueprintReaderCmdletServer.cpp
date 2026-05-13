#include "BlueprintReaderCmdletServer.h"

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

#include <cstdio>  // std::snprintf for PID-into-lockfile

// Raw Win32 lifetime-lock — UE's FArchive writers don't hold an
// OS-level exclusive lock, which is the whole reason this file uses
// `CreateFileW(dwShareMode=0)` directly. Mirrors the pattern in
// mcp-server/src/util/SingleInstanceLock.cpp.
#if PLATFORM_WINDOWS
    #include "Windows/AllowWindowsPlatformTypes.h"
    #include <windows.h>
    #include "Windows/HideWindowsPlatformTypes.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogBlueprintReaderCmdlet, Log, All);

namespace BlueprintReader
{

// Re-uses the same entry point the live server uses to dispatch into
// BlueprintReaderCommandlet.cpp's RunOneOp. Both servers feed
// `-Op=...` flag strings; the only difference is the transport.
extern int32 RunOneOpFromLiveServer(const FString& Params);

namespace
{

// Module-level singleton.
TUniquePtr<FCmdletServer> GCmdletServer;

// Per-connection state — owned by the worker FRunnable for its lifetime.
class FCmdletConnectionRunnable : public FRunnable
{
public:
    FCmdletConnectionRunnable(FSocket* InSocket, FString InExpectedToken)
        : Socket(InSocket), ExpectedToken(MoveTemp(InExpectedToken)) {}

    ~FCmdletConnectionRunnable() override
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
            UE_LOG(LogBlueprintReaderCmdlet, Warning, TEXT("Connection closed before auth"));
            return 0;
        }
        TSharedPtr<FJsonObject> AuthMsg = ParseJson(AuthRaw);
        FString PresentedToken;
        if (!AuthMsg.IsValid() ||
            !AuthMsg->TryGetStringField(TEXT("token"), PresentedToken) ||
            PresentedToken != ExpectedToken)
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
            // somewhere we can read it back. Cmdlet mode uses a per-call
            // temp file just like the daemon does — RunOneOp's existing
            // EmitJson path stays unchanged.
            const FString OutPath = FPaths::Combine(
                FPaths::ProjectIntermediateDir(),
                FString::Printf(TEXT("bpr-cmdlet-%s.json"), *FGuid::NewGuid().ToString(EGuidFormats::DigitsLower)));
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
// and joined in destructor. Stored in a list inside FCmdletServer so
// Stop() can join all active connections.
struct FCmdletConnection
{
    TUniquePtr<FCmdletConnectionRunnable> Runnable;
    TUniquePtr<FRunnableThread> Thread;
};

// Cmdlet-server-internal state. We keep connections in a list under a
// mutex so Stop() can shut them all down even while Accept fires.
struct FServerState
{
    FCriticalSection Mu;
    TArray<TUniquePtr<FCmdletConnection>> Connections;
};
TUniquePtr<FServerState> GState;

} // anonymous namespace

FCmdletServer::FCmdletServer() {}
FCmdletServer::~FCmdletServer() { Stop(); }

// Helper: try to bind+listen on a given port. On success populates
// OutSocket + OutBoundPort and returns true. On failure logs the
// reason, destroys any partial socket, and returns false so the
// caller can retry with a different port.
static bool TryBindAndListen(ISocketSubsystem* Sub, int32 PortToTry,
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
    if (!TryBindAndListen(SocketSubsystem, RequestedPort, ListenerSocket, BoundPort))
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
        if (!TryBindAndListen(SocketSubsystem, 0, ListenerSocket, BoundPort))
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

    GState = MakeUnique<FServerState>();
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
    return true;
}

void FCmdletServer::Stop()
{
    // Signal the daemon main loop to exit. Set this before tearing
    // down anything else so any code path that polls WantsShutdown()
    // sees the request immediately.
    bShuttingDown = true;
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

    if (!FFileHelper::SaveStringToFile(Json, *Path,
            FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
            &IFileManager::Get(),
            FILEWRITE_EvenIfReadOnly))
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
    if (!IFileManager::Get().FileExists(*Path)) return 0;
    FString Body;
    if (!FFileHelper::LoadFileToString(Body, *Path)) return 0;

    TSharedPtr<FJsonObject> Obj;
    TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Body);
    if (!FJsonSerializer::Deserialize(Reader, Obj) || !Obj.IsValid()) return 0;
    int32 Port = 0;
    Obj->TryGetNumberField(TEXT("port"), Port);

    if (Port < 1024 || Port > 65535) return 0;
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
    if (!FFileHelper::SaveStringToFile(Json, *Path,
            FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
            &IFileManager::Get(), FILEWRITE_EvenIfReadOnly))
    {
        UE_LOG(LogBlueprintReaderCmdlet, Verbose,
            TEXT("Could not write port cache %s; next launch will be ephemeral"),
            *Path);
    }
}

void FCmdletServer::DeleteHandshakeFile()
{
    const FString Path = HandshakeFilePath();
    if (!IFileManager::Get().FileExists(*Path)) return;
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
#else
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
#endif
}

void FCmdletServer::ReleaseLifetimeLock()
{
    if (LifetimeLockHandle == nullptr) return;
#if PLATFORM_WINDOWS
    HANDLE H = static_cast<HANDLE>(LifetimeLockHandle);
    CloseHandle(H);
#else
    // The non-Windows fallback above stores an IFileHandle* — delete it.
    IFileHandle* Handle = static_cast<IFileHandle*>(LifetimeLockHandle);
    delete Handle;
#endif
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

    auto Runnable = MakeUnique<FCmdletConnectionRunnable>(Socket, ExpectedToken);
    auto* RunnablePtr = Runnable.Get();
    auto Thread = TUniquePtr<FRunnableThread>(FRunnableThread::Create(
        RunnablePtr, TEXT("BlueprintReaderCmdletConnection")));
    if (!Thread)
    {
        UE_LOG(LogBlueprintReaderCmdlet, Error, TEXT("Failed to spawn connection thread"));
        return false;
    }
    auto Conn = MakeUnique<FCmdletConnection>();
    Conn->Runnable = MoveTemp(Runnable);
    Conn->Thread = MoveTemp(Thread);
    if (GState.IsValid())
    {
        FScopeLock Lock(&GState->Mu);
        GState->Connections.Add(MoveTemp(Conn));
    }
    return true;
}

FCmdletServer* GetCmdletServer()
{
    if (!GCmdletServer.IsValid()) GCmdletServer = MakeUnique<FCmdletServer>();
    return GCmdletServer.Get();
}

} // namespace BlueprintReader
