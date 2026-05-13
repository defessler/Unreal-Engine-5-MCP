# Multi-session MCP server + shared commandlet daemon

**Status:** Approved design, pre-implementation.
**Date:** 2026-05-13.
**Topic:** Let multiple Claude Code / Copilot CLI sessions use bp-reader against the same UE project concurrently. Centralize commandlet-daemon ownership so spawn/lifecycle is robust. Improve cross-project process identifiability as a stretch goal.

## Summary

Today the MCP server holds a process-wide single-instance lock for the
duration of its run. Live mode never needed this lock — the editor's
TCP listener already accepts multiple clients — but commandlet mode
did, because each MCP server spawned its own `UnrealEditor-Cmd
-Daemon` child via stdin/stdout. The lock was placed at the MCP-server
level for simplicity, which has the side effect of blocking concurrent
sessions even in live mode.

This design:

1. Removes the MCP-server-level lock.
2. Converts the commandlet daemon from a per-MCP-server stdin/stdout
   child into a project-scoped localhost TCP server, mirroring the
   live backend's transport.
3. Moves the single-instance constraint to the daemon's lifetime (one
   `UnrealEditor-Cmd -Daemon` process per project, regardless of how
   many MCP servers are running).
4. Renames the MCP server exe at build time to
   `bp-reader-mcp-<ProjectName>.exe` (with a canonical
   `bp-reader-mcp.exe` hard-link for backward compatibility).
5. Project-keys remaining temp-dir files with the project's FNV-1a
   hash for grep-ability.

Result: N concurrent sessions on the same project just work, with no
new process types and no architectural broker layer.

## Goals

- **(a) Same-project concurrency.** Multiple MCP server processes
  against one UE project must coexist without blocking each other
  during normal read/write workflows.
- **Robust daemon lifecycle.** No stray `UnrealEditor-Cmd` processes
  after sessions exit. Crashed daemons recover automatically. No
  duplicate-daemon spawns under concurrent first-connect races.
- **Process identifiability.** A user with N projects open should be
  able to tell which MCP server / daemon belongs to which project at
  a glance in Task Manager / `ps`.
- **Backward compatibility for existing `.mcp.json` configs.**
  Anyone who currently invokes `bp-reader-mcp.exe` by full path
  continues to work without edits.

## Non-goals

- **Cross-project sharing.** Different UE projects keep their own
  daemons, MCP servers, and handshake files. No cross-project
  coordination.
- **Standalone broker process.** We considered a separate
  `bp-reader-broker` that would own all backend logic. Rejected as
  overkill for the actual pain — the commandlet daemon already exists
  and just needs to become multi-client.
- **stdio transport on the daemon.** Dropping this code path; a
  future re-add lives behind a `Transport` interface but is not
  shipped now.
- **Cross-client daemon stderr attribution.** All clients share the
  daemon's stderr tail. Per-connection stderr tagging is deferred.

## Architecture

### Per-project run-time topology

```
Session 1 client (Claude) ──► bp-reader-mcp #1 ──┐
Session 2 client (Claude) ──► bp-reader-mcp #2 ──┤  N sessions = N MCP servers.
Session 3 client (Copilot)──► bp-reader-mcp #3 ──┘  No inter-MCP coordination.

                       Each MCP server's auto backend probes per call
                                          │
                       ┌──────────────────┴──────────────────┐
                       ▼                                     ▼
              UE editor's live TCP listener         bp-reader-cmdlet daemon
              (BlueprintReaderLiveServer)           (UnrealEditor-Cmd -Daemon as
              Already multi-client; each MCP        a localhost TCP server)
              connects independently with auth.     ONE per project. First MCP
                                                    server to need it spawns.
                                                    Others discover via handshake.
```

Process count: same as today. Locks: relocated.

### Process changes

- `bp-reader-mcp` (MCP server): drops its single-instance lock.
  Otherwise behavior unchanged. Built as
  `bp-reader-mcp-<ProjectName>.exe` with a hard-link copy named
  `bp-reader-mcp.exe` for backward compatibility.
- `UnrealEditor-Cmd -Daemon` (commandlet daemon): instead of reading
  arg lines from stdin and writing results to stdout, hosts a
  localhost TCP listener and serves any number of client connections
  with the same JSON frame protocol the live backend uses.

### File primitives

All in `<Project>/Saved/`:

| File | Purpose | Lifetime |
|---|---|---|
| `bp-reader-live.json` | Live editor handshake (unchanged) | Created by live server on `StartupModule`; deleted on `ShutdownModule` |
| `bp-reader-cmdlet.json` | Commandlet daemon handshake | Created by daemon on TCP listen; deleted on graceful exit |
| `bp-reader-cmdlet.lock` | OS exclusive lock, held by daemon for its lifetime | Auto-released on process exit |
| `bp-reader-cmdlet-spawn.lock` | OS lock held by an MCP server during its spawn-attempt window only | Released after spawn confirmation (or on MCP server crash) |
| Stable-port cache (PR #49) | Optional persistent port for live mode | Unchanged |

Temp-dir files (`%TEMP%`) — one-shot commandlet result blobs and any
future scratch — get an FNV-1a project hash prefix:
`bp-reader-<fnv1a>-<random>.json`. Grep-friendly when multiple
projects' daemons are active.

## Daemon transport + protocol

Localhost TCP, ephemeral port, `SO_REUSEADDR` (same pattern as
`BlueprintReaderLiveServer`).

### Handshake file shape

```json
{ "version": 1,
  "host": "127.0.0.1",
  "port": <ephemeral>,
  "token": "<32 hex chars>",
  "pid": <daemon-pid>,
  "started_at": "2026-05-13T..." }
```

Atomic write (tmp + rename). Deleted on graceful exit. Stale-detection
key is `pid`: if no such process exists, the handshake is ignored.

### Wire frames

Same four frames as the live backend, byte-for-byte:

```
server → client:  {"type":"hello", "version":"1"}
client → server:  {"type":"auth",  "token":"<shared>"}
server → client:  {"type":"auth_ok"}  |  {"type":"auth_fail"}
client → server:  {"type":"op", "id":N, "args":["-Op=Read","-Asset=/Game/AI/BP_Foo"]}
server → client:  {"type":"result", "id":N, "code":0, "json":{...}}
                  {"type":"error",  "id":N, "error":"asset path is required"}
```

The `args` array is the exact shape the daemon already accepts on
stdin (`-Op=Read -Asset=...`). The plugin's `RunOneOp(const FString&
Params)` dispatcher is untouched — only the transport layer changes.

### Daemon-side concurrency

- One reader thread per accepted socket.
- Reader threads enqueue ops on a single game-thread dispatch queue
  via `AsyncTask(ENamedThreads::GameThread, ...)`.
- The dispatcher processes ops serially on the game thread. UE's
  "UObject mutation on game thread" rule is naturally honored.
- Per-BP write locks (see Per-session isolation, below) layer on top
  of this; concurrent batches against different BPs run end-to-end
  before either's `EndBatch`, but their internal ops still serialize
  on the game thread.

### Backend client consolidation

MCP-side, the existing `LiveBlueprintReader` becomes
`SocketBlueprintReader` (rename). One client class targets two
handshake files: `bp-reader-live.json` and `bp-reader-cmdlet.json`.
The auto backend probes both. `BackendFactory`,
`AutoBlueprintReader::TryBuildLive`, and the existing
`test_live_backend.cpp` cases adapt to the rename. Roughly 150 lines
of duplicated socket+frame handling are eliminated.

## Daemon lifecycle

### MCP-server "ensure commandlet connection" flow

```
1. Read bp-reader-cmdlet.json.
   If present, pid still exists, and TCP probe succeeds → attach. Done.

2. Try-acquire bp-reader-cmdlet-spawn.lock (separate from the daemon's
   alive lock; held only during this spawn-attempt window).

   ├─ Got it:
   │     Re-read handshake — someone may have spawned in the gap.
   │     If still not connectable:
   │       spawn UnrealEditor-Cmd -Daemon -DaemonSocket=auto
   │       poll for handshake file (timeout: BP_READER_DAEMON_STARTUP_SECONDS, default 600)
   │     Release spawn lock. Attach.
   │
   └─ Didn't get it (another MCP server is mid-spawn):
         Poll handshake every 250ms (timeout: same).
         Attach when present + TCP probe succeeds.

3. If both attach and spawn fail → surface a clear error with the
   step that failed and the timeout window used.
```

The two-lock model avoids the simultaneous-spawn race: only one MCP
server can be in the spawn-attempt window at a time. The other waits
on the handshake instead of spawning a competing daemon.

### Daemon-side idle shutdown

- Daemon tracks active TCP connections.
- On `connections == 0`, start a `BP_READER_DAEMON_IDLE_SECONDS` timer
  (default 300 = 5 min).
- If the timer fires with `connections == 0` still, exit gracefully:
  1. Stop accepting new connections.
  2. Delete handshake file.
  3. Process `exit()` (OS releases the lifetime lock automatically).
- A new connection during the countdown cancels the timer.

### Crash recovery

- Daemon dies without graceful exit → handshake file may remain on
  disk, but the lifetime lock is released by the OS and the pid is no
  longer running.
- Next MCP server's step 1 above sees the dead pid → ignores
  handshake → step 2 acquires the spawn lock → spawns a fresh daemon.
- An MCP server that crashes mid-spawn releases the spawn lock the
  same way (OS auto-release on process exit).

### Live-editor opens mid-session

- `AutoBlueprintReader`'s per-call probe (2s cache) detects the new
  live listener and routes the next call to live.
- The now-unused commandlet daemon idles out after
  `BP_READER_DAEMON_IDLE_SECONDS`. No active cross-process
  coordination needed.

### Forced shutdown

The existing `shutdown_daemon` MCP tool keeps its name. Documented
semantic update: in shared-daemon mode, calling it terminates the
daemon used by every session against this project. Sessions whose
next call needs commandlet mode just spawn a fresh daemon
transparently. The original use case (free file locks, force a fresh
spawn) still works as before.

## Per-session state isolation

### Connection-scoped state

| State | Today | After |
|---|---|---|
| Request id sequence | Per-MCP-server counter | Per-socket counter (already correct in client logic) |
| `BeginBatch` / `EndBatch` defer flag | Function-local `static bool` in `BlueprintReaderCommandlet.cpp` | `Map<ConnectionId → BatchContext>` in the batch module |
| Pending-compile blueprint set | Function-local `static TArray<TWeakObjectPtr<UBlueprint>>` | Per-`BatchContext` |
| Op `id` echo, `_meta.elapsed_ms` | Per-request | Unchanged |
| `read_output_log` ring buffer | Global `FOutputDevice` | Unchanged — log is editor-global anyway |
| Auth token | Env var | Per-connection auth from handshake (unchanged) |

### Batch isolation: per-BP write lock

- A batch entering `BeginBatch` for a specific BP acquires a per-BP
  write lock owned by the batch's `BatchContext`.
- Released at `EndBatch`.
- Reads bypass the lock entirely.
- Effects:
  - Two batches on the same BP serialize: client B's `BeginBatch` on
    `BP_Enemy` blocks until client A's `EndBatch` on `BP_Enemy`.
  - Two batches on different BPs run concurrently.
  - Read ops always proceed; they see in-memory state up to the last
    completed op in any active batch on that BP.

### Disconnect mid-batch

If a socket closes after `BeginBatch` but before `EndBatch`:

- Default policy: **commit-partial.** The daemon calls
  `EndBatch(skipCompile=false)` on the orphaned context, then
  discards it. Anything that landed is saved; anything queued but
  not started is dropped. This mirrors the existing
  `on_failure: "compile"` default in `apply_ops`.
- Override via `BP_READER_BATCH_ON_DISCONNECT=discard` env to use
  `EndBatch(skipCompile=true)` semantics instead.

### Cross-client error attribution

Each op frame's response is sent on the socket that issued the op.
`op_index` and compile diagnostics scope to that op only. No
cross-client leakage. Daemon stderr is shared across clients;
per-connection tagging is deferred.

## Build-time exe rename

`Plugins/BlueprintReader/Scripts/Build-MCPServer.ps1` (already runs
per-plugin-install) gets a small addition:

1. Detect project name by walking up from the plugin to the nearest
   `*.uproject`. Already does this in spirit; just exposes the
   project name as a build-time variable.
2. Pass to CMake as `-DBP_READER_PROJECT_NAME=<ProjectName>`.
3. CMake configures the output name as
   `bp-reader-mcp-${BP_READER_PROJECT_NAME}` when the variable is
   set, falling back to `bp-reader-mcp` otherwise.
4. Post-build step creates a hard-link (Windows: `mklink /H`; Unix:
   `ln`) named `bp-reader-mcp.exe` next to the renamed binary, so
   any `.mcp.json` referencing the canonical name still resolves.

Same source, same content, two filenames per build. Process tools
see whichever name was used to launch — `tasklist /V` and `ps -ef`
show the project at a glance.

## Migration

1. **Existing `.mcp.json`** configs reference `bp-reader-mcp.exe`
   absolute paths. After upgrade, the canonical name is still
   present (as a hard link); no edits required.
2. **MCP-server lock removal** is a behavior change: the second
   session that previously failed to start now succeeds. No upgrade
   ritual.
3. **Daemon transport change** is internal — both halves ship from
   the same `PreBuildSteps` build, so version skew between MCP
   server and daemon is impossible in normal use. The `Transport`
   interface abstraction means a future stdio re-add is a one-class
   addition.
4. **Existing tests** stay green: the changed code paths (lock,
   transport, batch state) have new tests; the unchanged paths
   (mock backend, live backend, BPIR / transpile) keep their
   coverage.

## Testing strategy

### Unit tests (mock backend, no UE)

Add to `test_live_backend.cpp` (rename to
`test_socket_backend.cpp`) or a new `test_commandlet_daemon.cpp`,
using the existing multi-script `MockServer`:

- Spawn race: two clients race; only one ends up calling spawn,
  the other waits and attaches.
- Discovery: pre-existing valid handshake; second client attaches
  without invoking spawn.
- Stale handshake: handshake with dead pid; client ignores it and
  spawns fresh.
- Idle shutdown timer: simulated connect → disconnect → assert
  exit after timer.
- Per-BP write lock: two `apply_ops` batches on the same BP
  serialize end-to-end; two on different BPs run concurrently.
- Commit-partial on disconnect: socket close mid-batch → daemon
  flushes pending state via `EndBatch(skipCompile=false)`.
- Discard-partial via env override: same scenario with
  `BP_READER_BATCH_ON_DISCONNECT=discard` → no flush.

### Integration tests (live UE, env-gated)

Existing `BP_READER_ENGINE_DIR` / `BP_READER_PROJECT` gates apply.
CI keeps skipping if unset. New cases:

- Two MCP-server processes against the same project: both read/write
  the same BP, both see consistent state.
- Edit-from-A, read-from-B: write through one MCP server, read
  through another, observe within the same daemon tick.

### Soak

Extend `test_soak.cpp` with a multi-client variant: 4 concurrent
MCP-server processes, 5000 mixed ops, predominantly reads with
write spikes. Assert no daemon duplication, no stray processes,
final state consistent.

### Test-count expectation

Today: 441 cases / 29134 assertions / 0 failed. After this design:
roughly +15–25 cases for the multi-client coverage. Existing tests
unchanged in behavior.

## Rollout phases

The writing-plans skill will turn these into a real plan with
verification checkpoints. Provisional order:

1. **Daemon TCP transport + handshake (plugin side).** MCP server
   keeps using the current stdin/stdout path. Manual probe
   validates the new TCP path; existing tests stay green.
2. **MCP-side `SocketBlueprintReader` consolidation.** Rename
   `LiveBlueprintReader`. Add cmdlet-handshake awareness. Update
   `BackendFactory`, `AutoBlueprintReader`. Adjust
   `test_live_backend.cpp` (which becomes
   `test_socket_backend.cpp`).
3. **Drop the MCP-server single-instance lock.** Add multi-client
   tests (mock + integration). Verify live AND commandlet
   multi-session paths.
4. **Daemon lifecycle hardening.** Idle shutdown, crash recovery,
   spawn-race lock. Per-BP write lock for batches.
5. **Build-time exe rename + FNV-1a temp-file hashing.**
6. **Docs + skill updates.** `docs/design/05-backends.md`,
   `docs/design/06-wire-protocol.md`, `docs/tutorial/09-daemon-mode.md`,
   `Plugins/BlueprintReader/Claude/skills/bp-debug/SKILL.md`,
   wiki `Configuration.md`. Skill manifests re-deployed via the
   install script.

## Risks and open questions

- **Editor TCP listener multi-client support.** The live editor's
  `BlueprintReaderLiveServer` accepts multiple sequential
  connections in tests, but I should verify the real listener
  threads each accepted connection independently rather than
  serializing them. If it serializes, multi-session live mode
  works but with queued semantics — not the end of the world,
  but worth measuring. Phase 1 verification step.
- **Per-BP write lock correctness.** Two batches on the same BP
  serialize cleanly, but if one batch deletes a BP and a second
  batch was about to start on it, the second sees an
  `AssetNotFound` — desired or surprising? Probably desired.
  Worth a test case.
- **Daemon idle shutdown during quiet stretches.** A user with the
  editor closed and no recent MCP activity sees the daemon die
  after 5 min. Next call then pays the ~5-second cold-start cost
  again. This already happens today between sessions; the only
  change is that it now happens within one session if the user
  stops issuing calls for 5 minutes. Idle timeout is env-tunable,
  but the default needs to be empirically OK. Worth tracking.

## Out-of-scope follow-ups

- **Table-driven `RunOneOp` dispatch** (the if-else simplification
  from the earlier audit). Independent change; can be done before
  or after this one.
- **`BPNode.meta` typed values** (every meta value is currently a
  string). Multi-file wire-format change; not part of this design.
- **Cross-project broker.** If future use cases want one process
  to serve multiple projects, a broker layer becomes attractive.
  Not needed for the requested workflow.

## File-level changes (preview, not authoritative)

- `Plugins/BlueprintReader/Source/BlueprintReaderEditor/Private/BlueprintReaderCommandlet.cpp` — daemon transport switch from stdin/stdout to TCP. `RunDaemon` becomes a listener loop.
- `Plugins/BlueprintReader/Source/BlueprintReaderEditor/Private/BlueprintReaderLiveServer.cpp` — likely no change (already does what we want).
- `Plugins/BlueprintReader/mcp-server/src/backends/LiveBlueprintReader.{h,cpp}` — rename to `SocketBlueprintReader`, accept either handshake-file path.
- `Plugins/BlueprintReader/mcp-server/src/backends/CommandletBlueprintReader.{h,cpp}` — delete the stdin/stdout-daemon path. Thin wrapper over `SocketBlueprintReader` targeting `bp-reader-cmdlet.json`.
- `Plugins/BlueprintReader/mcp-server/src/backends/AutoBlueprintReader.{h,cpp}` — probe both handshakes; build whichever connects.
- `Plugins/BlueprintReader/mcp-server/src/backends/BackendFactory.cpp` — wire the new transport choices.
- `Plugins/BlueprintReader/mcp-server/src/SingleInstanceLock.{h,cpp}` — remove (or keep as a library for daemon-side use).
- `Plugins/BlueprintReader/Scripts/Build-MCPServer.ps1` — pass `-DBP_READER_PROJECT_NAME=...` to CMake; post-build hard-link.
- `Plugins/BlueprintReader/mcp-server/CMakeLists.txt` — honor `BP_READER_PROJECT_NAME` for `OUTPUT_NAME`.
- `Plugins/BlueprintReader/mcp-server/tests/test_live_backend.cpp` — rename to `test_socket_backend.cpp`, add multi-client cases.
- `Plugins/BlueprintReader/mcp-server/tests/test_commandlet_backend.cpp` — adapt to socket transport.
- `Plugins/BlueprintReader/mcp-server/tests/test_soak.cpp` — extend with multi-client variant.
- `docs/design/05-backends.md`, `docs/design/06-wire-protocol.md`, `docs/tutorial/09-daemon-mode.md`, `wiki/Configuration.md`, `Plugins/BlueprintReader/Claude/skills/bp-debug/SKILL.md` — documentation updates.

## Self-review log

- **Placeholders:** none. Every TODO/TBD slot was resolved during
  the brainstorming dialogue.
- **Internal consistency:** the lock model in "Daemon lifecycle"
  matches the architecture diagram and the spawn-flow pseudocode.
  Per-session isolation references match the daemon dispatcher
  design.
- **Scope check:** focused enough for a single implementation plan.
  The exe rename is grouped because it's mechanically tied to the
  multi-session story (process identifiability is part of the
  multi-session UX). Larger items (broker, BPNode meta typing) are
  explicitly listed as out of scope.
- **Ambiguity check:**
  - "Daemon stderr tagging" defer is explicit.
  - "`BP_READER_BATCH_ON_DISCONNECT`" default is explicit.
  - "Editor listener multi-client" is flagged as a Phase 1
    verification step rather than left as a guess.
