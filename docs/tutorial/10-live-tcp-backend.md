# Chapter 10 — In-process TCP server

The commandlet daemon from Chapter 9 made per-call latency cheap, but
it didn't fix the second problem: **the developer already has the
editor open**. When the user has `LyraStarterGame.uproject` open in
`UnrealEditor.exe` and the MCP server spawns its own
`UnrealEditor-Cmd.exe`, both processes load the same `.uasset` files
and compete for write locks on the same on-disk packages.

Symptoms when the two collide:

- The commandlet sees the editor's in-memory copy of a Blueprint as
  the on-disk version (stale).
- The commandlet writes a `.uasset`; the editor's in-memory version
  is still the old shape; the next editor-side save overwrites the
  commandlet's change with stale data.
- The commandlet fails to write because the editor holds an
  exclusive handle on the same `.uasset`.

The right answer is to skip spawning entirely when the editor is
already open and route the calls **through** that editor instead.
We do that with a TCP listener inside the editor's
`BlueprintReaderEditor` module.

## Architecture

```
+-----------------+         TCP/127.0.0.1         +------------------+
|  MCP server     | <---------------------------> |  UnrealEditor    |
|  (bp-reader-mcp)|        newline JSON           |  + BPR plugin    |
+-----------------+                               +------------------+
       ^                                                  |
       |  reads handshake from                            |  writes handshake to
       v                                                  v
                <Project>/Saved/bp-reader-live.json
                  { port, token, pid, started_at }
```

The plugin's `BlueprintReaderEditor` module starts a `FTcpListener`
on `StartupModule`, binds 127.0.0.1 on an ephemeral (kernel-picked)
port, and publishes the port + an auth token to a handshake file
that the MCP server can read.

The MCP server, when configured for the `live` backend, reads that
handshake file, opens a TCP connection, authenticates with the
published token, and then issues op frames.

## Wire protocol

Newline-delimited JSON. One frame per line. Three frame types
between server and client:

```jsonc
// Server → client, immediately on connect.
{"type":"hello","version":"1"}

// Client → server, response to hello.
{"type":"auth","token":"<the token from bp-reader-live.json>"}

// Server → client, on auth success.
{"type":"auth_ok"}

// (Or, on bad token:)
{"type":"auth_fail"}

// Client → server, per call.
{"type":"op","id":42,"args":["-Op=List","-Path=/Game"]}

// Server → client, per result.
{"type":"result","id":42,"code":0,"json":{...the same JSON the commandlet wrote to its temp file...}}

// (Or, on protocol error:)
{"type":"error","id":42,"error":"op frame missing args[]"}
```

Note the args shape: it's the same `-Op=...` flag list the
commandlet client builds for the daemon. Same dispatcher on the
plugin side. Same wire-format JSON on the way back. This means
**every** read and write op the commandlet supports works over the
live transport automatically — no per-op live code paths.

## Plugin side: the listener

`BlueprintReaderLiveServer.cpp` is one C++ file plus a header. It
exports a `FLiveServer` class with `Start(port)` and `Stop()`. The
module's `StartupModule` calls `GetLiveServer()->Start(0)`;
`ShutdownModule` calls `Stop()`.

### Bind

```cpp
// BlueprintReaderLiveServer.cpp — TryBindAndListen
OutSocket = Sub->CreateSocket(NAME_Stream, TEXT("BPRLive"),
                              FNetworkProtocolTypes::IPv4);

// SO_REUSEADDR before bind: lets us reclaim the same port immediately
// after a previous editor instance exits, even while its socket is
// still in TIME_WAIT (~30-60 s window). Without this, killing the
// editor and relaunching almost always falls back to ephemeral
// because the cached port is technically "in use" by the kernel.
OutSocket->SetReuseAddr(true);

TSharedRef<FInternetAddr> BindAddr = Sub->CreateInternetAddr();
BindAddr->SetIp(0x7F000001);  // 127.0.0.1
BindAddr->SetPort(FMath::Max(0, PortToTry));
OutSocket->Bind(*BindAddr);
OutSocket->Listen(8);
```

Loopback-only is non-negotiable: anything that can reach the listener
runs as our user and has full file access anyway. Binding to
`0.0.0.0` would expose an op-execution surface to the LAN.

### Port resolution policy

`FLiveServer::Start` picks a port in this priority order:

1. Explicit `Port` arg (programmatic / test override).
2. `BP_READER_LIVE_PORT` env var.
3. The persistent port cache (`<Project>/Saved/bp-reader-live-port.json`)
   from a previous successful bind. This is the "relaunch keeps the
   same port" path so existing MCP clients don't have to re-probe the
   handshake file after every editor restart.
4. `0` → kernel picks an ephemeral port.

When the cached port is now in use (another editor, a process that
grabbed it during shutdown, TIME_WAIT after a crash), bind fails and
we silently fall back to ephemeral. The cache write at the end of a
successful bind only happens when the port source was non-explicit —
we don't second-guess a user who set an env var.

### Auth token

```cpp
ExpectedToken = FPlatformMisc::GetEnvironmentVariable(TEXT("BP_READER_LIVE_TOKEN"));
if (ExpectedToken.IsEmpty())
{
    // Two GUIDs concatenated → 256 bits. Overkill for an auth token
    // on a localhost-only socket, but the cost is zero and it makes
    // brute-force not even worth thinking about.
    ExpectedToken = FGuid::NewGuid().ToString(EGuidFormats::Digits)
                  + FGuid::NewGuid().ToString(EGuidFormats::Digits);
}
```

The token defends against the scenario where another process on the
same machine (a different user account, a sandboxed app) tries to
talk to our listener. The token is in the handshake file, which
lives under the user-account's project dir — anything that can read
the file already has filesystem access to the project, so the bar
is "no privilege escalation past what filesystem ACLs already
grant."

### Handshake file

```cpp
// FLiveServer::WriteHandshakeFile
const FString Path = FPaths::Combine(FPaths::ProjectSavedDir(),
                                     TEXT("bp-reader-live.json"));
const FString Json = FString::Printf(
    TEXT("{\"version\":1,\"host\":\"127.0.0.1\",\"port\":%d,")
    TEXT("\"token\":\"%s\",\"pid\":%u,\"started_at\":\"%s\"}\n"),
    BoundPort, *ExpectedToken, Pid, *Now.ToIso8601());
FFileHelper::SaveStringToFile(Json, *Path,
    FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
    &IFileManager::Get(), FILEWRITE_EvenIfReadOnly);
```

`Stop()` deletes the file before tearing down the listener — that
ordering matters. MCP-server probes hitting a freshly-dead editor
should see "no handshake file" (skip the live attempt entirely)
rather than "stale port, connection refused" (try and fail, then
fall back).

### Single-instance enforcement

A second editor launched against the same project would also try to
bind. The persistent port cache handles the common case (kernel
refuses the second bind because the first one holds the port), but
a more aggressive guard is also possible. In practice the bind
failure plus the SO_REUSEADDR + ephemeral-fallback chain is enough —
the second editor ends up with its own ephemeral port and
overwrites the handshake file. Whichever editor was started last
wins the MCP traffic.

### Per-connection thread

`FLiveConnectionRunnable` is a `FRunnable` spawned per incoming
connection. It handles hello / auth then enters the op loop:

```cpp
// 1. Send hello.
if (!SendFrame(MakeHello())) return 0;

// 2. Read auth frame.
FString AuthRaw;
if (!ReadFrame(AuthRaw)) return 0;
TSharedPtr<FJsonObject> AuthMsg = ParseJson(AuthRaw);
FString PresentedToken;
if (!AuthMsg.IsValid() ||
    !AuthMsg->TryGetStringField(TEXT("token"), PresentedToken) ||
    PresentedToken != ExpectedToken)
{
    SendFrame(TEXT("{\"type\":\"auth_fail\"}"));
    return 0;
}
SendFrame(TEXT("{\"type\":\"auth_ok\"}"));
```

After auth, the runnable loops on read-frame, dispatch-on-game-thread,
write-result. The game-thread hop is the load-bearing detail —
modifying `UBlueprint` objects from a worker thread crashes UE. We
hand off via `AsyncTask`:

```cpp
int32 Code = -1;
FEvent* DoneEvent = FPlatformProcess::GetSynchEventFromPool(false);
AsyncTask(ENamedThreads::GameThread, [&Code, Params, DoneEvent]()
{
    Code = RunOneOpFromLiveServer(Params);
    DoneEvent->Trigger();
});
DoneEvent->Wait();
FPlatformProcess::ReturnSynchEventToPool(DoneEvent);
```

`RunOneOpFromLiveServer` is a thin extern in
`BlueprintReaderCommandlet.cpp` that forwards into the same
anonymous-namespace `RunOneOp` the daemon uses. One dispatcher, two
transports. Adding a new op to the commandlet automatically makes
it available to the live transport.

### Dispatch trick: temp file plus splice

`RunOneOp` writes JSON to whatever `-Out=` path you give it. The
live runnable gives each op a unique temp path, runs the op, reads
the file back, and splices the JSON straight into the frame
without re-parsing:

```cpp
const FString OutPath = FPaths::Combine(
    FPaths::ProjectIntermediateDir(),
    FString::Printf(TEXT("bpr-live-%s.json"),
                    *FGuid::NewGuid().ToString(EGuidFormats::DigitsLower)));
Params.Append(FString::Printf(TEXT(" -Out=\"%s\" -Compact"), *OutPath));

// ...dispatch on game thread...

FString JsonBody;
if (FFileHelper::LoadFileToString(JsonBody, *OutPath))
{
    IFileManager::Get().Delete(*OutPath);
}
else
{
    JsonBody = TEXT("{}");
}

// JsonBody is already a JSON literal — splice it into the result
// frame as-is (avoids double-serialization).
FString Out = FString::Printf(
    TEXT("{\"type\":\"result\",\"id\":%d,\"code\":%d,\"json\":%s}\n"),
    RequestId, Code, *JsonBody);
SendRaw(Out);
```

Splicing means the editor-side never has to parse the JSON it
already serialized. The MCP-server side will parse exactly once.

## MCP server side: `LiveBlueprintReader`

`LiveBlueprintReader` (in `Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/backends/`) implements
`IBlueprintReader` over the TCP wire format. It owns one socket and
keeps it open across calls.

### Connect

```cpp
// LiveBlueprintReader.cpp — TryConnectAndHandshake
ScopedSocket sock(ConnectOnce(cfg_.host, cfg_.port));
if (!sock) return {false, /*retryWorthwhile=*/true,
                   fmt::format("connect to {}:{} failed", cfg_.host, cfg_.port)};

auto& buf = BufFor(sock.s).b;
buf.clear();

std::string hello = RecvLine(sock.s, buf);
auto helloJson = nlohmann::json::parse(hello, nullptr, false);
if (!helloJson.is_object() || helloJson.value("type", "") != "hello") {
    return {false, false, fmt::format("expected hello frame, got: {}", hello)};
}

nlohmann::json authMsg = { {"type", "auth"}, {"token", cfg_.token} };
SendAll(sock.s, (authMsg.dump() + "\n").data(), authMsg.dump().size() + 1);

std::string authResp = RecvLine(sock.s, buf);
auto authJson = nlohmann::json::parse(authResp, nullptr, false);
if (authJson.value("type", "") != "auth_ok") {
    return {false, true, fmt::format("auth failed: {}", authResp)};
}

socket_ = static_cast<intptr_t>(sock.release());
handshakeOk_ = true;
```

The `ScopedSocket` RAII guard is important — on any failure path the
socket closes automatically. We only `release()` it once the
handshake fully succeeds.

### Per-call

Once connected, calls are simple framing:

```cpp
nlohmann::json LiveBlueprintReader::RunOp(const std::vector<std::string>& args)
{
    std::lock_guard lock(mu_);
    EnsureConnected();
    SocketType s = static_cast<SocketType>(socket_);

    int id = nextRequestId_++;
    nlohmann::json frame = {
        {"type", "op"},
        {"id", id},
        {"args", args},
    };
    std::string line = frame.dump() + "\n";
    SendAll(s, line.data(), line.size());

    auto& buf = BufFor(s).b;
    std::string response = RecvLine(s, buf);
    auto j = nlohmann::json::parse(response, nullptr, false);

    if (j.value("type", "") == "error") {
        throw BlueprintReaderError(fmt::format("live op '{}': {}",
            args.empty() ? "<unknown>" : args[0],
            j.value("error", "unspecified error")));
    }
    int code = j.value("code", -1);
    if (code != 0) throw BlueprintReaderError(/* ... */);
    auto jit = j.find("json");
    return (jit == j.end()) ? nlohmann::json::object() : *jit;
}
```

`RunOp` returns the inner `json` payload, the same shape the
commandlet path returns. From there, the type-typed methods
(`ListBlueprints`, `ReadBlueprint`, etc.) just call `.get<T>()`
through nlohmann's deserialization adapters.

### Handshake-file self-refresh

Editors restart. When they do, the listener binds on a new
ephemeral port (or with a new token even if the port is the same
via SO_REUSEADDR + persistent cache). The handshake file is
rewritten; the MCP server's cached `cfg_` is now stale.

We handle this by re-reading the handshake file on connect-refused
or auth-failed and retrying **once**:

```cpp
// LiveBlueprintReader::EnsureConnected
AttemptResult r = TryConnectAndHandshake();
if (!r.ok && r.retryWorthwhile && RefreshFromHandshakeFile()) {
    r = TryConnectAndHandshake();
}
if (!r.ok) {
    throw BlueprintReaderError(fmt::format(
        "LiveBlueprintReader: {} — is the editor running with "
        "BP_READER_LIVE_PORT/TOKEN published in Saved/bp-reader-live.json?",
        r.error));
}
```

The `retryWorthwhile` flag in the attempt result distinguishes
"connect refused" / "auth failed" (the editor probably restarted —
a fresh handshake might fix it) from "got a hello but it wasn't
JSON-shaped" (we connected to something that isn't our listener —
no point retrying).

```cpp
// RefreshFromHandshakeFile
bool LiveBlueprintReader::RefreshFromHandshakeFile() {
    if (cfg_.handshakeFilePath.empty()) return false;
    std::error_code ec;
    if (!std::filesystem::exists(cfg_.handshakeFilePath, ec)) return false;
    std::ifstream f(cfg_.handshakeFilePath);
    std::stringstream ss; ss << f.rdbuf();
    nlohmann::json j;
    try { j = nlohmann::json::parse(ss.str()); }
    catch (...) { return false; }
    std::string newHost  = j.value("host",  std::string("127.0.0.1"));
    int         newPort  = j.value("port",  0);
    std::string newToken = j.value("token", std::string());
    if (newPort <= 0 || newToken.empty()) return false;
    if (newHost == cfg_.host && newPort == cfg_.port && newToken == cfg_.token)
        return false;  // identical — nothing to retry with
    cfg_.host = std::move(newHost);
    cfg_.port = newPort;
    cfg_.token = std::move(newToken);
    return true;
}
```

The "identical → don't retry" check is what prevents an infinite
loop when the handshake file is stale relative to the actual
listener state.

## Routing: `BP_READER_BACKEND=live`

Wire up the new backend in `BackendFactory::Create`:

```cpp
// BackendFactory.cpp
if (cfg.backend == "live") {
    LiveBlueprintReader::Config lc;
    lc.host = cfg.liveHost;
    lc.port = cfg.liveProcPort;
    lc.token = cfg.liveToken;
    // Wire the handshake-file path so the reader can self-refresh.
    if (!cfg.uproject.empty()) {
        lc.handshakeFilePath =
            (cfg.uproject.parent_path() / "Saved" / "bp-reader-live.json").string();
    }
    return std::make_unique<LiveBlueprintReader>(std::move(lc));
}
```

Discovery happens earlier in `LoadConfig` — if a handshake file
exists, we pre-populate `cfg.liveHost`, `cfg.liveProcPort`,
`cfg.liveToken` from it so the user doesn't have to set env vars by
hand:

```cpp
if (auto hf = ReadHandshakeFile(cfg.uproject)) {
    if (cfg.liveHost.empty() || cfg.liveHost == "127.0.0.1") {
        cfg.liveHost = hf->host;
    }
    if (cfg.liveProcPort == 0) cfg.liveProcPort = hf->port;
    if (cfg.liveToken.empty()) cfg.liveToken = hf->token;
}
```

## Concurrency

The plugin's listener thread accepts; a per-connection worker thread
authenticates and runs the op loop. The op handler hops to the game
thread via `AsyncTask`. Multiple MCP-server processes connecting
simultaneously each get their own connection thread; all of them
funnel through the same single-threaded game-thread dispatch.

The MCP server side uses a `std::mutex` around `RunOp` so two
threads inside one MCP-server process can't interleave frames on
one socket. The simple correctness model is: one socket, one in-
flight op, lock-step request/response.

## Checkpoint

Open `LyraStarterGame.uproject` in `UnrealEditor.exe` (full editor, not
commandlet). In the editor's Output Log filter for
`LogBlueprintReaderLive` — you should see:

```
LogBlueprintReaderLive: Display: FLiveServer listening on 127.0.0.1:53413
LogBlueprintReaderLive: Display: Wrote live-handshake file: D:/Projects/UE5_MCP/Saved/bp-reader-live.json (port=53413)
```

Confirm the handshake file:

```pwsh
Get-Content D:/Projects/UE5_MCP/Saved/bp-reader-live.json
# {"version":1,"host":"127.0.0.1","port":53413,"token":"a1b2...","pid":17324,...}
```

Launch the MCP server with `BP_READER_BACKEND=live`. The first call
should succeed without spawning `UnrealEditor-Cmd.exe` (`Get-Process
UnrealEditor-Cmd` shows nothing). Open a Blueprint in the editor,
modify a variable's default in the Details panel, and **without
saving**, call `read_blueprint` over MCP. You should see the modified
value — the editor's in-memory state, not the on-disk state.

Close the editor. The handshake file goes away (the plugin's
`ShutdownModule` deletes it). The next MCP call against the live
backend now fails fast with the "is the editor running" diagnostic.
Reopen the editor; the file reappears; the very next call succeeds
(self-refresh re-reads the file and reconnects).

Per-op latency on the live transport is typically 5 – 20 ms — the
TCP round-trip plus the game-thread hop. No daemon cold-start, no
process-launch overhead.

You now have two write-capable backends, and they don't fight. The
question is: which one should the MCP server pick? That's
Chapter 11.

See also:
[design/05-backends.md](../design/05-backends.md),
[design/02-architecture.md](../design/02-architecture.md).
