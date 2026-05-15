# 05 — Backends

The MCP server doesn't talk to Unreal directly. Every tool handler calls
through `IBlueprintReader`, an abstract interface; a concrete backend
decides whether the call ends up in a fixture file, a child
`UnrealEditor-Cmd.exe` process, or a TCP socket connected to a running
editor.

Four implementations live under
`Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/backends/`:

| Backend     | What it talks to                | When to use                               |
|-------------|---------------------------------|-------------------------------------------|
| `mock`      | JSON files on disk              | tests, no-UE iteration on the server      |
| `commandlet`| spawned `UnrealEditor-Cmd.exe`  | editor is closed; CI / batch jobs         |
| `live`      | TCP listener inside the editor  | editor is open                            |
| `auto`      | one of the above, per call      | default when a `.uproject` is detected    |

Two more files in the same directory wrap any of the above with
cross-cutting behavior: `CachingBlueprintReader` memoizes reads,
`ReadOnlyBlueprintReader` rejects every write. The factory composes them
so the outermost layer is read-only-or-not, then caching, then the raw
backend.

See [06 — Wire protocol](06-wire-protocol.md) for the JSON shapes every
backend returns, and [08 — Errors & diagnostics](08-error-diagnostics.md)
for the failure semantics each backend promises.


## The interface

`IBlueprintReader` (`backends/IBlueprintReader.h:33-1215`) is the entire
contract. Every method is a tool the MCP server exposes; backends
override the ones they support and let the base class default throw
`BlueprintReaderError("... not supported by this backend")` for the
rest.

The split between pure-virtual and defaulted reflects what's required of
every backend versus what's extra. The first block (lines 37-169) is
core Blueprint introspection plus mutation — every backend implements
all of it. Everything below the `// ----- Project + Content Browser ops`
banner (line 171+) is optional; the base class' default throws and
backends override what they support.

Two exception classes are defined alongside (`IBlueprintReader.h:21-31`):

```cpp
class BlueprintReaderError : public std::runtime_error {
    using std::runtime_error::runtime_error;
};
class AssetNotFound : public BlueprintReaderError {
    using BlueprintReaderError::BlueprintReaderError;
};
```

`AssetNotFound` is a subclass so the MCP layer can distinguish "you
typed the path wrong" (HTTP 404 analog) from "I couldn't talk to the
engine" (HTTP 500 analog). The commandlet backend maps exit code 4 to
`AssetNotFound`; the live backend maps a `code=4` op result the same
way.

### Result struct convention

Methods that return more than a single primitive return a nested struct
defined on `IBlueprintReader`. Example
(`IBlueprintReader.h:91-94`):

```cpp
struct AddFunctionResult {
    std::string functionName;
    std::string entryNodeId;  // empty if the plugin couldn't locate it
};
virtual AddFunctionResult AddFunction(std::string_view assetPath,
                                      std::string_view name) = 0;
```

Why a struct rather than `std::pair` or a free shape: the JSON
serialization in `BlueprintTools.cpp` needs stable field names, and
deserializers on the other side (the live backend) need to project the
same names. A named struct keeps the wire JSON and the C++ in lockstep.

### Read vs. write split

Reads (`ListBlueprints`, `ReadBlueprint`, `GetGraph`, `GetFunction`,
`ListVariables`, `GetComponents`, `FindNode`) are idempotent and cheap.
Writes (`AddVariable`, `AddNode`, `WirePins`, `CreateBlueprint`, …)
mutate the `.uasset` and must leave it compilable; the comment on
`IBlueprintReader.h:50-52` makes this a backend invariant. Every write
implementation runs the same plugin-side `CompileAndSaveBlueprint` flow
before returning, except inside a `BeginBatch`/`EndBatch` window — see
the batching section below.


## Mock backend

Source: `MockBlueprintReader.h`, `MockBlueprintReader.cpp`.

The mock backend reads every `*.json` file in a fixtures directory at
construction time and serves the parsed metadata back. It has no
runtime dependency on UE — it's what lets `BlueprintReaderMcpTests.exe` exercise
the server end-to-end in CI without an engine checkout.

### Fixture loading

`MockBlueprintReader::LoadDir` (`MockBlueprintReader.cpp:59-65`) walks
the directory, picks up every `.json` file, and calls `LoadFile` on
each:

```cpp
for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    if (!entry.is_regular_file()) continue;
    if (entry.path().extension() != ".json") continue;
    LoadFile(entry.path());
}
```

Each file becomes one `FixtureEntry`, keyed in `assets_` by the
`summary.asset_path` field. A duplicate `asset_path` throws — the
fixtures directory is meant to define a flat set of canonical BPs.

### Fixture shape

Each fixture is a single JSON object with four required keys and one
optional. Real example —
`Plugins/BlueprintReader/Tests/BlueprintReaderMcpTests/fixtures/BP_Enemy.json:1-69`:

```json
{
  "summary":   { "asset_path": "/Game/AI/BP_Enemy", "name": "BP_Enemy",
                 "parent_class": "ACharacter", "modified_iso": "..." },
  "metadata":  { /* BPMetadata */ },
  "graphs":    [ /* BPGraph[] */ ],
  "functions": [ /* BPFunction[] */ ],
  "components":[ /* BPComponent[] — optional */ ]
}
```

`MockBlueprintReader::LoadFile` (`MockBlueprintReader.cpp:67-109`) uses
nlohmann/json's `to_json` / `from_json` adapters declared in
`BlueprintReaderTypes.h:395+` — the same code path the live backend uses
to decode its TCP responses. Whatever the wire format accepts, the mock
fixture accepts.

### Read-only

Every write method on `MockBlueprintReader` throws with a uniform
message — see `MockBlueprintReader.cpp:166-272`. Example:

```cpp
void MockBlueprintReader::AddVariable(...) {
    throw BlueprintReaderError(
        "AddVariable: mock backend is read-only; set BP_READER_BACKEND=commandlet");
}
```

The message names the backend env var so an AI agent that hits the
error gets a single hop to the fix. This is the only place in the code
where a fully-qualified env var appears in error text; everywhere else
errors reference the cause structurally.

### Filtering and finding

`FindNode` (`MockBlueprintReader.cpp:274-333`) is the only read that
does more than table lookup. It scans every graph in the fixture, does
case-insensitive substring matches against `class`, `title`, and a
handful of `meta` fields, and tags each hit with its `graph_name` /
`graph_type`. That tagging is part of the wire contract — see
[06 — Wire protocol](06-wire-protocol.md#find_node-hits).


## Commandlet backend

Source: `CommandletBlueprintReader.h`, `CommandletBlueprintReader.cpp`.

The commandlet backend drives a real `UnrealEditor-Cmd.exe -run=BlueprintReader`.
The plugin's `UBlueprintReaderCommandlet` dispatches per `-Op=...` to
the same `RunOneOp` logic the live backend executes; the only
difference is the transport.

Two modes, controlled by `Config::useDaemon`
(`CommandletBlueprintReader.h:41-55`):

- **One-shot** — spawn `UnrealEditor-Cmd.exe -run=BlueprintReader -Op=...`
  per tool call. Each call pays the editor cold-start cost (~5–7 s on a
  dev box).
- **Daemon** — spawn once with `-Daemon`. The daemon now hosts a
  **localhost TCP listener** (same wire frames as the live editor's
  `BlueprintReaderLiveServer`) and publishes its port + token to
  `<Project>/Saved/bp-reader-cmdlet.json`. The MCP server attaches
  through `SocketBlueprintReader` — the same socket client that talks
  to the live editor — so the per-call cost drops to ~1 s. Default-on;
  opt out with `BP_READER_DAEMON=0`.

### Topology: one daemon, many MCP servers

Each Claude Code / Copilot CLI session has its own MCP server process.
The MCP-server-level single-instance lock that used to gate this was
removed in the multi-session work; the lifetime lock now sits on the
daemon. The result: **N MCP servers share ONE commandlet daemon per
project**. The first MCP server to need commandlet mode spawns the
daemon; every later arrival attaches to the same listener through the
published handshake.

```
Session 1 client (Claude)  ──► bp-reader-mcp #1 ──┐
Session 2 client (Claude)  ──► bp-reader-mcp #2 ──┤
Session 3 client (Copilot) ──► bp-reader-mcp #3 ──┘
                                                  │
                       ┌──────────────────────────┴────────────────────┐
                       ▼                                               ▼
              UE editor's live TCP listener                  bp-reader-cmdlet daemon
              (BlueprintReaderLiveServer)                    (UnrealEditor-Cmd -Daemon
              Multi-client. Each MCP server                  hosting a TCP listener)
              connects independently with auth.              ONE per project; shared
                                                             across all sessions.
```

### Handshake files in `<Project>/Saved/`

| File | Purpose | Lifetime |
|---|---|---|
| `bp-reader-live.json` | Live editor handshake (port + token + pid) | Created by live server on `StartupModule`; deleted on `ShutdownModule` |
| `bp-reader-cmdlet.json` | Commandlet daemon handshake — mirrors the live one's shape | Created by daemon on TCP listen; deleted on graceful exit |
| `bp-reader-cmdlet.lock` | Daemon's exclusive lifetime lock | Held by the daemon process; OS auto-releases on process exit (clean or crash) |
| `bp-reader-cmdlet-spawn.lock` | MCP-server-held lock during a spawn-attempt window | Released after spawn confirmation or on MCP-server crash |

The MCP-server-level single-instance lock that previously existed is
**gone**. Concurrent MCP servers no longer block each other at all.

### Spawn flow: two-lock coordination

`CommandletBlueprintReader::EnsureDaemonAttached`
(`CommandletBlueprintReader.cpp:792-875`) handles first-attach for
each MCP server with a two-lock dance:

1. **Try to attach.** Read `bp-reader-cmdlet.json`. If the pid is
   live and a TCP probe succeeds → attach, done.
2. **Race for `bp-reader-cmdlet-spawn.lock`** (separate from the
   daemon's `bp-reader-cmdlet.lock` lifetime lock).
   - **Got it:** re-check the handshake file (someone may have
     spawned in the gap). If still no daemon, `CreateProcessW` a
     fresh `UnrealEditor-Cmd -Daemon`, poll for the handshake until
     `BP_READER_STARTUP_TIMEOUT_SECONDS`, then attach. Release the
     spawn lock.
   - **Didn't get it** (another MCP server is mid-spawn): poll the
     handshake file every 250 ms until either it appears or the
     timeout fires. Attach if it appeared.
3. **Still nothing** → throw a clear error naming the failed step
   and the timeout window used.

Decoupling "daemon alive" from "daemon starting" is the lock model
that closes the simultaneous-spawn race: only one MCP server can be
inside the spawn window at a time, and any second arriver waits on
the handshake instead of spawning a competing daemon. If the spawner
itself crashes, the OS releases its spawn lock and a later arriver
takes over.

### Crash recovery

A daemon that dies without graceful exit leaves the handshake file on
disk but releases the lifetime lock. The next MCP server's step 1
notices the dead pid (`bp-reader-cmdlet.json` carries it), ignores the
handshake, and falls through to step 2 to spawn a fresh daemon. No
manual cleanup needed.

### Idle shutdown

The daemon tracks active connections. When the last client disconnects
it starts a `BP_READER_DAEMON_IDLE_SECONDS` timer (default 300). If no
new connection arrives before it fires, the daemon stops accepting,
deletes its handshake, and exits cleanly. A connection that arrives
during the countdown cancels it.

### One-shot dispatch

`RunOpOneShot` (`CommandletBlueprintReader.cpp:435+`) builds an argv,
launches via `CreateProcessW`, drains stdout/stderr into bounded
tail buffers, waits up to `cfg_.timeout`, and parses the JSON the
commandlet writes to a temp file under `%TEMP%`. The temp file is the
data channel; stdout is just for diagnostics. UE's log noise can drown
out anything written to its `stdout`, so the plugin writes structured
output to a path passed via `-Out=...`.

A real argv:

```
<uproject>
  -run=BlueprintReader -Op=Graph
  -Asset=/Game/AI/BP_Enemy -Graph=EventGraph
  -Out=C:\Users\...\Temp\bp-reader-deadbeef.json
  -Compact -nullrhi -nosplash -unattended -nopause -stdout
```

`-NullRHI`, `-nosplash`, `-unattended`, `-nopause` are the standard
commandlet flags that suppress windowing / dialogs. `-stdout` ensures
the log tail comes back on the pipe.

### Exit codes

The plugin uses small, numbered exit codes the backend maps back to
exception types
(`CommandletBlueprintReader.cpp:480-490`):

```cpp
if (r.exitCode == 4) {
    throw AssetNotFound(fmt::format(
        "commandlet reported missing target (exit=4); tail:\n{}", tail));
}
throw BlueprintReaderError(fmt::format(
    "commandlet exit={}; tail:\n{}", r.exitCode, tail));
```

The codes are documented in
[08 — Errors & diagnostics](08-error-diagnostics.md#commandlet-exit-codes).

### Arg encoding for FParse

`-Asset=`-style flags need careful quoting because UE's `FParse::Value`
has its own quoted-string parsing on top of `CommandLineToArgvW`. The
encoder lives in `CommandletArgEncoding.h` so it's unit-testable
without spinning up an editor. The dispatch (`EncodeArg`,
`CommandletArgEncoding.h:114-122`) splits args into two classes: option
args (`-`-prefixed with `=`) get `EncodeArgForFParse`, everything else
gets `QuoteWindowsArg`. The two encoders use different quoting because
the consumers are different: positional args go through
`CommandLineToArgvW`'s parsing only, while options also go through
`FParse::Value`, which triggers its quoted path only if the char
immediately after `=` is `"`. The history comment at
`CommandletArgEncoding.h:109-113` records two earlier regressions.

### Daemon mode (TCP transport)

The daemon used to be a stdin/stdout child of each MCP server: one
editor process per MCP server, fed newline-delimited
commandlet-arg strings on stdin, scanned for `__BPR_READY__` and
`__BPR_DONE <code>__` sentinels on stdout. That model is gone.

Today's daemon is a TCP server. `RunDaemon` on the plugin side spins
up the same listener pattern as `BlueprintReaderLiveServer`:
loopback bind, ephemeral port, `SO_REUSEADDR`, per-connection
runnable that handles auth then loops on op frames. The MCP-server
side talks to it through `SocketBlueprintReader` — see [06 — Wire
protocol](06-wire-protocol.md#op-frame-live--commandlet) for the
frame catalog.

`EnsureDaemonAttached`
(`CommandletBlueprintReader.cpp:792-875`) is the entry point. Per
the two-lock flow above, it either attaches an existing daemon or
spawns one and attaches. The returned `SocketBlueprintReader&` is
reused for every subsequent op on this MCP server.

### Daemon failure recovery and prewarming

If a socket call throws transport-level (the daemon died, the
listener got hung up on, etc.), `RunOp` invalidates the cached
`socket_` and falls through to `RunOpOneShot` for that call.
`AssetNotFound` passes through as-is; everything else is treated as
transport. A dead daemon doesn't break the next call — the next
`EnsureDaemonAttached` re-runs the two-lock flow, sees the stale
handshake (dead pid), and respawns.

`Prewarm()` spins up the daemon on a background thread so the first
real tool call doesn't pay cold-start latency. Opt in with
`BP_READER_PREWARM=1`. The prewarm thread and the live-call path
share `daemonMutex_`, so calls arriving before the spawn completes
block on the same lock and inherit the now-warm socket.


## Socket backend (live + cmdlet)

Source: `SocketBlueprintReader.h`, `SocketBlueprintReader.cpp` —
historically `LiveBlueprintReader`, renamed when the commandlet
daemon switched to the same wire protocol. One client class targets
two handshake files: `bp-reader-live.json` (in-editor listener) and
`bp-reader-cmdlet.json` (commandlet daemon's listener). The frames
on the wire are identical; the difference is which process is
listening and how the MCP server discovered it.

`LiveBlueprintReader` is now a thin alias kept for source-level
compatibility; new code should reference `SocketBlueprintReader`
directly.

The contract: newline-delimited JSON, four message types,
shared-token auth.

### Why this exists

UE will not let two editor-shaped processes own the same project
simultaneously — they fight over the DDC, the asset registry, and the
`.uasset` file locks. If the user has the full editor open, the
commandlet backend can't write (file-lock probe in
`BlueprintReaderCommandlet.cpp:580-617` will fire). The live backend
sidesteps this: there's only ever one editor process, and we send it
op-args over a socket instead of via stdin to a child.

### Protocol

Four message types
(`LiveBlueprintReader.h:11-17`):

```
server → client  { "type": "hello", "version": "1" }
client → server  { "type": "auth", "token": "<shared>" }
server → client  { "type": "auth_ok" } | { "type": "auth_fail" }
client → server  { "type": "op", "id": N, "args": ["-Op=...", ...] }
server → client  { "type": "result", "id": N, "code": K, "json": {...} }
```

`args` is the same shape the commandlet daemon accepts —
`-Op=Read -Asset=/Game/AI/BP_Enemy`. Server-side dispatch calls the same
`RunOneOp` the commandlet does, just on the editor's game thread instead
of in a child process.

`RunOp` itself (`LiveBlueprintReader.cpp:304-352`) just frames a request,
calls `SendAll`, reads one line back with `RecvLine`, and either returns
the embedded `json` field or throws.

### Handshake file

The editor publishes `<Project>/Saved/bp-reader-live.json` on
`StartupModule`. Schema (read by `ReadHandshakeFile`,
`BackendFactory.cpp:29-69`):

```json
{ "version": 1, "host": "127.0.0.1", "port": <int>,
  "token": "<hex>", "pid": <int>, "started_at": "..." }
```

This is how the MCP server discovers the editor's ephemeral port and
shared token without anyone setting env vars. The editor's
`BlueprintReaderLiveServer::ShutdownModule` deletes the file, so its
absence is a strong "no editor" signal — auto-discovery in
`BackendFactory.cpp:174-183` treats the file as authoritative when env
vars haven't already supplied the same fields.

### Self-refresh

The editor's port + token rotate on every restart, so a cached
`cfg_` would point at a dead socket forever without recovery.
`LiveBlueprintReader::EnsureConnected`
(`LiveBlueprintReader.cpp:283-301`) handles two failure modes —
connect refused (likely a new ephemeral port) and `auth_fail`
(likely a stable port with a rotated token) — both by re-reading
the handshake file and retrying once. `AttemptResult::retryWorthwhile`
distinguishes "the editor is gone" from "the server we connected to
isn't speaking our protocol", and `RefreshFromHandshakeFile`
(`LiveBlueprintReader.cpp:176-198`) returns true only when the on-disk
values differ. Full diagnostic in
[08 — Errors & diagnostics](08-error-diagnostics.md#live-backend-self-refresh).

### Per-socket framing state

`RecvLine` buffers extra bytes (the next frame may already be in the
recv buffer) into a per-socket `PendingBuf`. The buffer map is
`thread_local`. `Disconnect()` erases the entry
(`LiveBlueprintReader.cpp:157-168`) so a reused socket fd — Windows
can hand back the same number after a close — doesn't pick up stale
bytes from the dead session.


## Auto backend

Source: `AutoBlueprintReader.h`, `AutoBlueprintReader.cpp`.

Auto is the default backend when a `.uproject` is auto-discovered
(`BackendFactory.cpp:189-191`). On every tool call it probes both
handshake files in priority order — `bp-reader-live.json` first, then
`bp-reader-cmdlet.json` — and attaches to the first one that's actually
listening. Live wins when an editor is open; cmdlet wins when only the
shared daemon is up. The decision is cached for `probeTtl` (default 2s)
so back-to-back calls don't each pay two TCP connect attempts.

### Probe sequence

`AutoBlueprintReader::Probe` (`AutoBlueprintReader.cpp:179-227`):

1. Call `TryBuildLive` — re-reads the handshake file and merges with
   any env-var overrides. Returns null if nothing is discoverable, in
   which case `useLive_=false` and we're done.
2. Otherwise do a one-shot non-blocking TCP `connect()` to the published
   host:port (`TcpProbe`, `AutoBlueprintReader.cpp:64-120`). A 300ms
   timeout (`Config::probeConnectTimeout`) is enough to tell "editor is
   listening" from "stale handshake file from a crashed editor".
3. On success, construct the `LiveBlueprintReader` if we don't have one,
   set `useLive_=true`, and stamp `lastProbe_`.

`Pick()` (`AutoBlueprintReader.cpp:229-236`) is the per-call entry:
it re-probes if the TTL has expired, otherwise reuses `useLive_`. The
TCP probe is intentionally separate from the auth handshake — the
cheaper "is anyone listening" check is enough to decide routing, and
`LiveBlueprintReader` does its own auth handshake on the first real
RunOp.

### `TryBuildLive` precedence

`TryBuildLive` (`AutoBlueprintReader.cpp:147-177`) merges three sources
of live-config in priority order: env-var values
(`BP_READER_LIVE_HOST/PORT/TOKEN`, baked into `cfg_`), handshake file
fields filling anything missing, otherwise null. The handshake-file
path is also wired into the resulting
`LiveBlueprintReader::Config::handshakeFilePath` so the inner reader
can self-refresh independently of Auto's outer probe — covers the rare
case where the editor restarts between the probe and the next op.

### Lifecycle inside Auto

Both inner readers are lazy. The commandlet's constructor validates
the editor exe path on disk (expensive on cold systems); a session that
lives entirely on Live should never pay that cost. Live is rebuilt on
probe transitions — `TryBuildLive` runs on first probe-success and
`live_.reset()` clears it on probe-failure.

`ShutdownDaemon` (`AutoBlueprintReader.cpp:372-388`) always routes to
the commandlet — Live has no daemon to shut down. If the commandlet
was never spawned this session, the response is
`{ok:true, was_running:false}` with a hint.


## Configuration cascade

`ConfigFromEnv` (`BackendFactory.cpp:73-194`) builds a `BackendConfig`
from three sources, in priority order:

1. **Environment variables** — explicit user override. Read first.
2. **Auto-discovery** — walks up from the exe to find a `.uproject`,
   reads `EngineAssociation`, resolves the engine via `HKCU\SOFTWARE\
   Epic Games\Unreal Engine\Builds`. Fills any field still empty.
3. **Defaults** — hardcoded fallbacks for everything else (`timeout=120s`,
   `daemon=true`, `cacheTtl=30s`, `host=127.0.0.1`).

Both handshake files (when present) feed into step 2: `bp-reader-live.json`
seeds live-mode host/port/token, `bp-reader-cmdlet.json` seeds
commandlet-mode socket attachment. The auto backend's per-call probe
re-reads both before each call (subject to its 2s cache).

### Env-var surface

The complete set, documented on `BackendConfig`
(`BackendFactory.h:15-64`):

| Variable                            | Default       | Effect |
|-------------------------------------|---------------|--------|
| `BP_READER_BACKEND`                 | `auto` / `mock` | `mock` \| `commandlet` \| `live` \| `auto` |
| `BP_READER_FIXTURES_DIR`            | `<exe>/fixtures` | mock fixture dir |
| `BP_READER_ENGINE_DIR`              | auto-discovered | source-built engine root |
| `BP_READER_PROJECT`                 | auto-discovered | `.uproject` path |
| `BP_READER_TIMEOUT_SECONDS`         | 120           | per-call subprocess timeout |
| `BP_READER_STARTUP_TIMEOUT_SECONDS` | 600           | daemon initial handshake-publish wait |
| `BP_READER_DAEMON`                  | true          | daemon mode (commandlet) |
| `BP_READER_DAEMON_IDLE_SECONDS`     | 300           | daemon exits after N seconds idle (no active clients) |
| `BP_READER_PREWARM`                 | false         | spawn daemon on startup |
| `BP_READER_EDITOR_CONFIG`           | auto-detected | `Development` / `DebugGame` / ... |
| `BP_READER_EDITOR_ARGS`             | empty         | extra commandlet args |
| `BP_READER_CACHE_TTL_SECONDS`       | 30            | read-tool memoization window |
| `BP_READER_READ_ONLY`               | false         | reject every write tool |
| `BP_READER_LIVE_HOST`               | 127.0.0.1     | live host override |
| `BP_READER_LIVE_PORT`               | 0             | live port override |
| `BP_READER_LIVE_TOKEN`              | empty         | live auth token override |


## Backend factory

`backends::Create(cfg)` (`BackendFactory.cpp:196-266`) is the single
construction entry point. `buildInner` picks the inner backend by
`cfg.backend` (mock / commandlet / live / auto, throws on anything
else), then `Create` composes wrappers:

```cpp
auto cached = WrapWithCache(buildInner(),
                            std::chrono::seconds(cfg.cacheTtlSeconds),
                            cfg.uproject);
return MaybeWrapReadOnly(std::move(cached), cfg.readOnly);
```

ReadOnly wraps outermost so writes fail fast — no caching round-trip,
no commandlet spawn. Caching wraps the raw backend so an explicit
`BP_READER_READ_ONLY=1` denies writes without even consulting the
cache.


## What's next

- The wire JSON every backend produces is specified in
  [06 — Wire protocol](06-wire-protocol.md).
- The BPIR that powers `decompile_function` and `transpile_function`
  rides on top of `IBlueprintReader::GetFunction` — see
  [07 — BPIR & transpile](07-bpir-and-transpile.md).
- Backend-specific failure modes (file locks, auth refresh, exit
  codes) are in [08 — Errors & diagnostics](08-error-diagnostics.md).
- How the test suite exercises each backend is in
  [09 — Testing](09-testing.md).
