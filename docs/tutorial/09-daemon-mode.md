# Chapter 9 — Long-lived editor daemon (multi-session-ready)

By the end of Chapter 8 you have a single-process MCP server, a UE
plugin commandlet, write ops, and `apply_ops` batching. Every call
follows the same shape: MCP spawns `UnrealEditor-Cmd.exe`, the
commandlet runs one op, writes its JSON result to a temp file, then
exits. The shell-out is the bottleneck.

## The problem

A cold-start of `UnrealEditor-Cmd.exe` on a modestly-sized project is
not cheap. On the reference machine, time-to-first-byte for a single
`-Op=List` call breaks down roughly like this:

| Phase | Time |
|---|---|
| `CreateProcessW` + DLL loads | ~700 ms |
| UE module bring-up | 1.5 – 3 s |
| Asset registry scan | 1 – 3 s |
| Plugin loading | 500 ms – 1 s |
| Actual op | 5 – 50 ms |
| Process teardown | 200 – 400 ms |
| **Total** | **5 – 10 s** |

5 – 10 seconds per call is hostile to interactive use. An agent that
issues a dozen tool calls to read a graph, add a variable, wire two
pins, and inspect the result is staring at 60 – 120 seconds of wall
time before any of its work happens. We need the editor to stay
resident across calls.

There's a second problem we'd like to solve at the same time. If two
Claude Code sessions run against the same UE project, each spawns its
own MCP server, and a per-MCP-server daemon would double up — two
`UnrealEditor-Cmd.exe` processes loading the same modules, fighting
for `.uasset` write locks. We want one daemon per project, shared
across all the MCP servers that need it.

## The shape of the fix

Keep one editor process alive. Talk to it from N MCP servers
concurrently. The way both halves of the problem fall out together:
host a **localhost TCP listener** inside the daemon, mirroring the
in-editor `BlueprintReaderLiveServer` you'll build in Chapter 10.
Multiple MCP servers connect to the same listener. The first MCP
server to need commandlet mode spawns the daemon; every later
arrival attaches to the existing one through a handshake file.

This is the **daemon mode**:

- Plugin: `-Daemon` flag on the commandlet command line switches
  from `RunOneOp` (one shot, then exit) to `RunDaemon` (host a TCP
  listener, accept connections, serve op frames).
- MCP server: read the daemon's handshake file, open a socket,
  authenticate, send `op` frames, read `result` frames. The exact
  same socket client the live backend uses (Chapter 10) — one
  class, two server lifecycles.

Per-call cost drops to "the op itself" plus a few milliseconds of
TCP round-trip.

## Plugin side: `RunDaemon`

The commandlet's `Main` already calls `RunOneOp(Params)` for one-shot
use. Wrap that in a `Daemon` check before the dispatch:

```cpp
// BlueprintReaderCommandlet.cpp
int32 UBlueprintReaderCommandlet::Main(const FString& Params)
{
    if (FParse::Param(*Params, TEXT("Daemon")))
    {
        return RunDaemon();
    }
    return RunOneOp(Params);
}
```

`RunDaemon` is the new loop. Instead of polling stdin and emitting
stdout sentinels, it stands up a TCP listener, publishes a handshake
file, and lets `FBlueprintReaderCmdletServer` handle the rest. The
heavy lifting — accept threads, frame parsing, op dispatch — is
factored into a runnable that mirrors `BlueprintReaderLiveServer`
(see Chapter 10).

```cpp
// BlueprintReaderCommandlet.cpp — abbreviated
int32 UBlueprintReaderCommandlet::RunDaemon()
{
    // Cold-start work happens once on entry: UE has already loaded
    // its modules, scanned the asset registry, and brought up our
    // plugin. Every subsequent op pays just the op-itself cost.

    FBlueprintReaderCmdletServer Server;
    if (!Server.Start(/*Port=*/0))   // 0 = kernel-picked ephemeral
    {
        UE_LOG(LogBPRDaemon, Error, TEXT("Cmdlet daemon: TCP listener failed to bind"));
        return 1;
    }

    // Server.Start has already written <Project>/Saved/bp-reader-cmdlet.json
    // with { host, port, token, pid, started_at }, atomically.

    // Wait until the server requests shutdown. Two triggers:
    //   1. Idle timeout (no connections for BP_READER_DAEMON_IDLE_SECONDS).
    //   2. SIGINT / Ctrl-C from the OS (wired in InstallCtrlHandler).
    Server.WaitForShutdown();
    Server.Stop();   // deletes the handshake file before tearing down

    return 0;
}
```

Cold-start work happens once on entry: UE has already loaded its
modules, scanned the asset registry, and brought up our plugin.
Every subsequent op pays just the op-itself cost.

### Why TCP and not stdin/stdout

An earlier draft of this chapter walked you through a stdin/stdout
daemon: feed newline-delimited arg strings to the child's stdin,
read JSON paths back on stdout, with `__BPR_READY__` and
`__BPR_DONE <code>__` sentinels for framing. That design works for
one MCP server. It doesn't generalize.

The issues:

1. **One stdin per child.** Two MCP servers can't pipe to the same
   stdin handle. Each would need to spawn its own daemon, and now
   you've got two editors racing for `.uasset` locks.
2. **UE redirects C stdio.** UE's runtime can route `fputs(stdout)`,
   `printf`, even `FPlatformMisc::LocalPrint` through its own log
   device. Raw byte fidelity requires going straight to
   `GetStdHandle(STD_OUTPUT_HANDLE)` + `WriteFile`. Manageable but
   fragile.
3. **Sentinel-based framing is brittle.** The MCP-side scanner finds
   the first `__BPR_DONE` occurrence regardless of source — so a
   log line that mentions the marker verbatim eats the next call.

TCP solves all three: connections are first-class multi-client, the
wire is bytes-on-a-socket nobody redirects, and frames are
structurally distinct JSON objects with a `type` discriminator. The
trade-off is a tiny bit of listener-setup code, which we already
have for the live backend — and which Chapter 10 introduces in
detail. Reuse pays for itself.

### Bind, publish, accept

The listener-side pattern is the same as the in-editor live server
you'll see in Chapter 10. `FBlueprintReaderCmdletServer` is a
side-by-side copy with one substantive difference: it lives in the
commandlet process, not the editor module, and the handshake file
it publishes is `bp-reader-cmdlet.json` rather than
`bp-reader-live.json`.

```cpp
// FBlueprintReaderCmdletServer::Start
OutSocket = Sub->CreateSocket(NAME_Stream, TEXT("BPRCmdlet"),
                              FNetworkProtocolTypes::IPv4);

// SO_REUSEADDR before bind: lets us reclaim the cached port even
// when a previous daemon's socket is still in TIME_WAIT.
OutSocket->SetReuseAddr(true);

TSharedRef<FInternetAddr> BindAddr = Sub->CreateInternetAddr();
BindAddr->SetIp(0x7F000001);   // 127.0.0.1
BindAddr->SetPort(0);          // kernel-picked ephemeral
OutSocket->Bind(*BindAddr);
OutSocket->Listen(8);

ExpectedToken = FGuid::NewGuid().ToString(EGuidFormats::Digits)
              + FGuid::NewGuid().ToString(EGuidFormats::Digits);

WriteHandshakeFile();
```

Loopback-only is non-negotiable. Anything that can reach
`127.0.0.1:<port>` runs as our user and has the same filesystem
access already; binding to `0.0.0.0` would expose an
op-execution surface to the LAN.

The handshake file:

```jsonc
// <Project>/Saved/bp-reader-cmdlet.json
{ "version":   1,
  "host":      "127.0.0.1",
  "port":      53413,
  "token":     "a1b2c3d4...",
  "pid":       18424,
  "started_at": "2026-05-13T18:42:11Z" }
```

Atomic write (tmp + rename). Deleted on graceful exit. Stale
handshakes (daemon crashed without cleanup) are detected by the
MCP-server side via the `pid` field — if that process no longer
exists, the handshake is ignored and the next MCP server respawns.

### Per-connection threads + game-thread dispatch

```cpp
// FBlueprintReaderCmdletServer::AcceptLoop
while (!bStopping)
{
    FSocket* IncomingSocket = nullptr;
    if (Listener->WaitForPendingConnection(bHasPending, /*timeout=*/0.5)
        && bHasPending)
    {
        IncomingSocket = Listener->Accept(TEXT("BPRCmdletClient"));
        if (IncomingSocket)
        {
            // Per-connection runnable handles hello/auth then loops on op frames.
            ActiveConnections.Add(MakeShared<FCmdletConnectionRunnable>(IncomingSocket, this));
        }
    }
}
```

`FCmdletConnectionRunnable` is a `FRunnable` spawned per incoming
connection. It handles the four-frame handshake then enters the op
loop:

```cpp
// 1. Send hello.
SendFrame(TEXT("{\"type\":\"hello\",\"version\":\"1\"}"));

// 2. Read auth frame; compare token; reply auth_ok or auth_fail.
FString AuthRaw;
if (!ReadFrame(AuthRaw)) return 0;
TSharedPtr<FJsonObject> AuthMsg = ParseJson(AuthRaw);
FString PresentedToken;
if (!AuthMsg.IsValid() ||
    !AuthMsg->TryGetStringField(TEXT("token"), PresentedToken) ||
    PresentedToken != Owner->ExpectedToken)
{
    SendFrame(TEXT("{\"type\":\"auth_fail\"}"));
    return 0;
}
SendFrame(TEXT("{\"type\":\"auth_ok\"}"));

// 3. Op loop: read op frame; dispatch on game thread; write result.
while (!bStopping)
{
    FString OpRaw;
    if (!ReadFrame(OpRaw)) break;   // client disconnected
    DispatchOp(OpRaw);
}
```

The game-thread hop is the load-bearing detail — mutating
`UBlueprint` objects from a worker thread crashes UE. We hand off
via `AsyncTask`:

```cpp
FEvent* DoneEvent = FPlatformProcess::GetSynchEventFromPool(false);
int32 Code = -1;
AsyncTask(ENamedThreads::GameThread, [&Code, Params, DoneEvent]()
{
    Code = RunOneOp(Params);
    DoneEvent->Trigger();
});
DoneEvent->Wait();
FPlatformProcess::ReturnSynchEventToPool(DoneEvent);
```

`RunOneOp` is the same dispatcher one-shot mode used in Chapter 5.
One implementation, two transports (and a third for the in-editor
live server in Chapter 10). Adding a new op to the commandlet
automatically makes it available to every transport.

### The dispatch trick: temp file plus splice

`RunOneOp` writes JSON to whatever `-Out=` path you give it. The
runnable assigns each op a unique temp path, runs the op, reads the
file back, and **splices** the JSON straight into the result frame
without re-parsing:

```cpp
const FString OutPath = FPaths::Combine(
    FPaths::ProjectIntermediateDir(),
    FString::Printf(TEXT("bpr-cmdlet-%s.json"),
                    *FGuid::NewGuid().ToString(EGuidFormats::DigitsLower)));
Params.Append(FString::Printf(TEXT(" -Out=\"%s\" -Compact"), *OutPath));

// ... dispatch on game thread ...

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

Splicing means the daemon side never has to parse the JSON it
already serialized. The MCP-server side will parse exactly once.

### Idle shutdown

The daemon tracks its active connection count. When the last client
disconnects, it starts a `BP_READER_DAEMON_IDLE_SECONDS` timer
(default 300 = 5 min). If the timer fires with the count still at
zero, the daemon stops accepting new connections, deletes its
handshake file, and exits cleanly. A new connection during the
countdown cancels the timer.

```cpp
void FBlueprintReaderCmdletServer::OnConnectionClosed()
{
    if (--ActiveConnectionCount == 0)
    {
        FCoreDelegates::OnEndFrame.AddRaw(this,
            &FBlueprintReaderCmdletServer::TickIdleTimer);
        IdleTimerStartedAt = FPlatformTime::Seconds();
    }
}

void FBlueprintReaderCmdletServer::OnConnectionAccepted()
{
    if (++ActiveConnectionCount == 1)
    {
        FCoreDelegates::OnEndFrame.RemoveAll(this);
        IdleTimerStartedAt = 0;
    }
}
```

The reason for an idle exit at all: a Claude session that spawned a
daemon and then ended (or was Ctrl-C'd) leaves the daemon up. Without
idle shutdown, you accumulate dead-end `UnrealEditor-Cmd.exe`
processes — one per project per "I tried that out once". 5 minutes
is long enough that an active session doesn't trigger it; short
enough that an abandoned daemon doesn't outlive the user's interest.

## MCP server side: attach, don't spawn

The MCP server's commandlet backend used to *spawn* the daemon and
own it for its own lifetime. Today's model: **attach to an existing
daemon if one's listening; spawn only if necessary; either way, talk
to it through `SocketBlueprintReader`** — the same TCP client the
live backend (Chapter 10) uses.

### State on `CommandletBlueprintReader`

```cpp
// CommandletBlueprintReader.cpp
std::unique_ptr<SocketBlueprintReader> socket_;  // null until first attach
std::mutex daemonMutex_;
```

`socket_` is the entire transport state today. No process handle, no
pipe handles, no accumulator buffer. The daemon process is owned by
its own pid; we just open a TCP connection to it.

### Two-lock spawn coordination

`EnsureDaemonAttached` is the entry point for first-attach. It runs a
two-lock dance to coordinate with other MCP servers that might also
be trying to spawn:

```cpp
SocketBlueprintReader& CommandletBlueprintReader::EnsureDaemonAttached()
{
    std::lock_guard<std::mutex> lock(daemonMutex_);
    if (socket_) return *socket_;

    // Step 1: try attaching to an existing daemon.
    socket_ = TryAttachExistingDaemon();   // reads handshake, pid-checks, TCP-probes
    if (socket_) return *socket_;

    // Step 2: race for the spawn-attempt lock.
    auto spawnLockPath = cfg_.uproject.parent_path() / "Saved" /
        "bp-reader-cmdlet-spawn.lock";
    SpawnLock spawnLock(spawnLockPath);
    const bool acquired = spawnLock.TryAcquire(cfg_.startupTimeout);

    if (acquired) {
        // Re-check inside the lock — someone else may have finished
        // their spawn during our wait.
        socket_ = TryAttachExistingDaemon();
        if (!socket_) {
            SpawnDaemon();                      // CreateProcessW UnrealEditor-Cmd -Daemon
            PollForHandshake(cfg_.startupTimeout);
            socket_ = TryAttachExistingDaemon();
        }
    } else {
        // Someone else is (or was) mid-spawn. Wait for the handshake
        // they're about to publish.
        PollForHandshake(cfg_.startupTimeout);
        socket_ = TryAttachExistingDaemon();
    }

    if (!socket_) {
        throw BlueprintReaderError(/* clear error with timeout used */);
    }
    return *socket_;
}
```

Two locks at play:

- **`bp-reader-cmdlet.lock`** — exclusive lifetime lock held by the
  daemon for its entire run. OS auto-releases on process exit (clean
  or crash). Answers "is a daemon currently alive?"
- **`bp-reader-cmdlet-spawn.lock`** — held by one MCP server during
  its spawn-attempt window only. Lets a second arriver wait on the
  handshake rather than double-spawning.

Decoupling "alive" from "starting up" is what closes the
simultaneous-spawn race. Only one MCP server can be inside the spawn
window at a time. A late arriver either finds the handshake already
published (and just attaches) or, in the narrow case where it raced
the publication, waits on the spawn lock and re-checks.

### Per-op call

Once attached, the call path is identical to live's:

```cpp
nlohmann::json CommandletBlueprintReader::RunOpDaemon(
    const std::vector<std::string>& args)
{
    auto& sock = EnsureDaemonAttached();
    return sock.RunOp(args);   // sends op frame, reads result frame
}
```

`SocketBlueprintReader::RunOp`
(`mcp-server/src/backends/SocketBlueprintReader.cpp:RunOp`) frames a
request, calls `SendAll`, reads one line back with `RecvLine`, and
either returns the embedded `json` field or throws. Same shape on
the wire as live. Same C++ code paths after the wire.

### Fallback to one-shot

A transport failure (broken pipe, daemon crashed mid-call) doesn't
fail the user's request. `RunOp` catches the transport-level error,
invalidates `socket_`, and falls through to `RunOpOneShot` for that
call:

```cpp
if (cfg_.useDaemon) {
    try {
        return RunOpDaemon(args);
    } catch (const BlueprintReaderError& e) {
        std::fprintf(stderr,
            "[bp-reader-mcp][commandlet][daemon] transport error, "
            "falling back to one-shot: %s\n", e.what());
        socket_.reset();
    }
}
return RunOpOneShot(args);
```

The next call's `EnsureDaemonAttached` sees a stale handshake (dead
pid) and starts the two-lock dance from scratch — the user's call
shouldn't fail because the daemon's transport is degraded.

## Multi-session: N MCP servers, one daemon

The structural payoff. Two Claude Code sessions running against
`UE5_MCP.uproject` each launch their own MCP server process. Both
MCP servers, on first commandlet-mode call, run
`EnsureDaemonAttached`:

- **First arriver:** handshake absent → spawn lock acquired →
  `CreateProcessW UnrealEditor-Cmd -Daemon` → poll for handshake →
  attach. Daemon publishes its `bp-reader-cmdlet.json`.
- **Second arriver (anywhere from milliseconds to minutes later):**
  reads the handshake → pid is alive → TCP probe succeeds →
  attaches. No spawn, no contention.

Both MCP servers now hold their own socket to the same daemon. The
daemon's per-connection runnables service them independently; ops
serialize on the daemon's game thread, but that's the same
constraint that already held for one client. The connection count
goes from 1 to 2, then back down as sessions end. When the last
session disconnects, the idle timer starts.

### Per-connection state

The daemon keeps a small amount of per-connection state inside each
`FCmdletConnectionRunnable`. The most important piece is the
`BeginBatch`/`EndBatch` defer flag, which used to be a function-local
`static bool` in `BlueprintReaderCommandlet.cpp` — fine in the
single-client world, a correctness bug in the multi-client one:
Session A starts a batch, Session B sees Session A's defer flag and
silently skips its own compile.

In the new model the defer flag lives in a per-connection
`FBatchContext`, and ops carry their `op_index` and compile
diagnostics back on the same socket that issued them. No
cross-client leakage.

(There are two follow-ups the implementation defers: a per-BP write
lock so two batches on the **same** BP serialize end-to-end, and a
commit-or-discard policy for sockets that close mid-batch. They're
documented in the spec and tracked as known limitations. The basic
multi-session story works without them; concurrent batches on the
same BP across sessions just aren't yet isolated.)

## Telemetry

Every `tools/call` envelope already carries
`_meta: {elapsed_ms, tool}` from Chapter 4. The daemon doesn't change
the shape — `elapsed_ms` measures the same wall-clock window
(MCP-server-side, from "request received" to "result emitted"). What
changes is the distribution: cold-start happens only on the first
arriver's first call; every subsequent MCP server attaches to a
warm daemon and pays only TCP-round-trip overhead.

## Checkpoint

Start a single MCP server. The first call cold-starts the daemon, so
expect 5 – 10 seconds. Subsequent calls in the same session should
drop to tens of milliseconds. Verify by reading `_meta.elapsed_ms`
from the JSON-RPC response:

```jsonc
// First call after MCP startup (daemon cold-starts):
{ "result": {...}, "_meta": { "tool": "list_blueprints", "elapsed_ms": 7843 } }

// Second call, same session:
{ "result": {...}, "_meta": { "tool": "read_blueprint",  "elapsed_ms": 38 } }

// Tenth call:
{ "result": {...}, "_meta": { "tool": "add_variable",    "elapsed_ms": 24 } }
```

Confirm the daemon is one process and that its handshake file
exists:

```pwsh
Get-Process UnrealEditor-Cmd
# exactly one process

Get-Content D:/Projects/UE5_MCP/Saved/bp-reader-cmdlet.json
# {"version":1,"host":"127.0.0.1","port":53413,"token":"...","pid":18424,...}
```

Kill it and the next call should respawn it (and take cold-start
time again) — that's the crash-recovery path.

### Multi-session check

The big-deal milestone: spawn **two MCP server processes** against
the same project simultaneously. Use two separate terminals or two
Claude Code sessions:

```pwsh
# Terminal A
$env:BP_READER_PROJECT = "D:\Projects\UE5_MCP\UE5_MCP.uproject"
$env:BP_READER_BACKEND = "commandlet"
& "D:/Projects/UE5_MCP/Plugins/BlueprintReader/mcp-server/build/Release/bp-reader-mcp.exe"

# Terminal B (simultaneously)
$env:BP_READER_PROJECT = "D:\Projects\UE5_MCP\UE5_MCP.uproject"
$env:BP_READER_BACKEND = "commandlet"
& "D:/Projects/UE5_MCP/Plugins/BlueprintReader/mcp-server/build/Release/bp-reader-mcp.exe"
```

Send a `tools/call list_blueprints` to each. Both should succeed.
Check process counts in a third terminal:

```pwsh
Get-Process UnrealEditor-Cmd
# Count: 1 — the shared daemon

Get-Process bp-reader-mcp
# Count: 2 — one MCP server per session
```

The first session's call cold-starts the daemon (~7 s). The second
session's call attaches to the already-warm daemon (~50 ms). Kill
either MCP server; the other keeps working. Kill both MCP servers;
the daemon's connection count drops to 0 and the idle timer starts —
after `BP_READER_DAEMON_IDLE_SECONDS` (default 300) it exits cleanly
and the handshake file disappears.

### Opt-out

Set `BP_READER_DAEMON=0` to fall back to one-shot mode (one
`UnrealEditor-Cmd.exe` per call, ~5–10 s each). Useful for CI scripts
that expect deterministic per-call isolation. Default is on.

You now have an editor process that lives across calls AND across
sessions. The transport is a localhost TCP socket — bytes-on-the-wire
that nothing in UE's stdio redirection touches. Chapter 10
introduces the in-editor live backend with the same wire protocol:
instead of spawning an editor we don't see, we talk to the editor
the developer already has open.

See also:
[design/05-backends.md](../design/05-backends.md) for the backend
taxonomy and the two-lock daemon lifecycle in detail,
[design/06-wire-protocol.md](../design/06-wire-protocol.md) for the
op-frame contract shared between cmdlet and live.
