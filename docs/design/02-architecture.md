# 02 — Architecture

How a `tools/call` from Claude reaches a `UBlueprint*` and how the
response returns. Refer to [01-overview.md](01-overview.md) for what
the pieces are; this file covers how they fit together. Per-side
deep-dives are in [03-plugin-internals.md](03-plugin-internals.md)
(UE) and [04-mcp-server.md](04-mcp-server.md) (server).

## Component diagram

```
+--------------------+       JSON-RPC 2.0 stdio frames
|  MCP client        |     (newline-delimited or Content-Length)
|  (Claude, Copilot) |<--------------------------------+
+--------------------+                                 |
                                                       v
                                          +-------------------------+
                                          |  BlueprintReaderMcp.exe      |
                                          |  ----------------------- |
                                          |  jsonrpc::Server         |
                                          |     ReadFrame / WriteFrame
                                          |  mcp::Register…           |
                                          |     initialize / tools/*   |
                                          |  tools::ToolRegistry       |
                                          |     268 descriptors        |
                                          |  IBlueprintReader (iface)  |
                                          +-------------------------+
                                                       |
                          +----------------------------+----------------------------+
                          |                            |                            |
                          v                            v                            v
              +----------------------+    +-----------------------+    +-------------------+
              |  CommandletBackend   |    |  LiveBackend          |    |  MockBackend       |
              |  ------------------- |    |  -------------------- |    |  -------------     |
              |  CreateProcessW      |    |  TCP socket           |    |  reads fixtures/   |
              |  stdin/stdout pipes  |    |  127.0.0.1:<port>     |    |  no UE needed      |
              +----------------------+    +-----------------------+    +-------------------+
                          |                            |
                          v                            v
              +----------------------+    +-----------------------+
              |  UnrealEditor-Cmd    |    |  UnrealEditor.exe     |
              |  -run=BPR|    |  (editor open)        |
              |  -Daemon             |    |  BlueprintReaderLive  |
              |  newline arg lines   |    |  Server FTcpListener  |
              |  RunOneOp dispatch   |    |  game-thread RunOneOp |
              +----------------------+    +-----------------------+

`AutoBlueprintReader` probes both per call (live first, commandlet
fallback) — see backend dispatch in [03-plugin-internals.md] and the
factory wiring in `Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/backends/BackendFactory.cpp:196-266`.
```

## Process model

In production there are typically two processes:

1. **`BlueprintReaderMcp.exe`** — the MCP server. One per project. Started
   by the MCP client. Lives for the client's session. The MCP-server-level
   single-instance lock was removed in the multi-session work (PR #68);
   the lifetime lock now sits on the daemon side (see
   `Source/BlueprintReaderEditor/Private/BlueprintReaderCmdletServer.cpp:25-60`
   and [05-backends.md](05-backends.md)). Override with
   `BP_READER_ALLOW_MULTI=1`.

2. **Backend process** depends on the backend:
   - `commandlet`: a child `UnrealEditor-Cmd.exe -run=BPR
     -Daemon` process spawned via `CreateProcessW`, fed
     newline-delimited arg strings on its stdin. Long-lived, reused
     across many tool calls.
   - `live`: no child of ours — the user's already-running
     `UnrealEditor.exe` is the target. We connect over TCP.
   - `mock`: no second process; the MCP server reads
     fixtures off disk directly.
   - `auto`: holds *both* a live and a commandlet sub-reader, probes
     the live handshake on each call, falls back to commandlet when
     the editor isn't reachable. The commandlet daemon is started
     lazily on first commandlet-routed call.

`BlueprintReaderMcp.exe` does not host a UI, does not have a Unreal engine
instance in-process, and never loads `.uasset` files itself. All
asset work happens in the editor / commandlet process.

## Request lifecycle

Tracing one `tools/call read_blueprint /Game/AI/BP_Enemy`:

```
1. Client writes JSON-RPC frame on stdin of BlueprintReaderMcp.exe.
   jsonrpc::ReadFrame auto-detects framing on the first request
   (`Server.cpp:117-159`); subsequent frames use the same format.

2. jsonrpc::Server::Run parses the frame as JSON, validates the
   envelope (`Server.cpp:202-261`), dispatches to the "tools/call"
   handler registered in mcp::RegisterHandlers (`Mcp.cpp:95-159`).

3. tools/call handler:
   - looks up the tool by name in ToolRegistry (`Mcp.cpp:117`)
   - starts a steady_clock timer
   - calls the registered handler closure with `arguments`
   - on success: dumps result JSON as a text-content envelope with
     `_meta: {elapsed_ms, tool}` (`Mcp.cpp:136-141`)
   - on exception: catches, wraps in an MCP tool error envelope with
     `_meta: {elapsed_ms, tool, args}` (`Mcp.cpp:142-158`)

4. The tool's handler closure (registered in
   `tools/BlueprintTools.cpp`) extracts args, calls into
   `IBlueprintReader` (e.g. `reader.ReadBlueprint(asset)`), applies
   response controls (`ApplyResponseControls` —
   `tools/BlueprintTools.cpp:119-130`), and returns the projected JSON.

5. The concrete IBlueprintReader implementation does the I/O.
   For the commandlet backend that means:
   - format the call as a commandlet-arg string
     (e.g. `-Op=Read -Asset=/Game/AI/BP_Enemy -Out=<tempfile>`)
   - write it as one line on the daemon's stdin
   - scan the daemon's stdout for `__BPR_DONE <code>__`
   - read the JSON the daemon wrote to `<tempfile>`
   - parse and return

6. jsonrpc::Server::Run writes the response frame on stdout using
   the same framing format the client used.
```

The live backend's path is structurally the same — frame goes over TCP
instead of stdin, but the temp-file dance is unchanged (the live
server uses an `IFileManager`-backed path under `ProjectIntermediateDir`
keyed by a fresh `FGuid` per call —
`BlueprintReaderLiveServer.cpp:141-144`).

## Threading model

### MCP server side (`BlueprintReaderMcp.exe`)

- Single thread for the whole JSON-RPC loop. `jsonrpc::Server::Run`
  is a blocking read-dispatch-write loop on `std::cin` / `std::cout`
  (`Server.cpp:264-333`). No background workers, no async dispatch.
- Tool handlers run inline on that thread. Long-running calls
  block subsequent requests; this is intentional — MCP clients
  serialize their tool calls per server.
- The backend may spawn a child process (commandlet daemon) or
  hold a TCP connection (live), but the MCP server's I/O thread
  waits synchronously for the backend's response.

### Commandlet daemon side (`UnrealEditor-Cmd.exe -run=BPR -Daemon`)

- Runs entirely on the main thread (the commandlet thread, which UE
  treats as the game thread). UE's normal task graph and asset
  registry are available.
- Reads stdin via raw Win32 `GetStdHandle(STD_INPUT_HANDLE)` +
  `ReadFile` to bypass UE's stdio redirection
  (`BlueprintReaderCommandlet.cpp:5334-5369`). Same for stdout via
  `WriteFile` so the `__BPR_DONE` sentinel hits the actual pipe and
  not UE's log device.
- One line in = one op = one synchronous dispatch through
  `RunOneOp`. No concurrency within a daemon process.

### Editor live-server side (`UnrealEditor.exe` with `FLiveServer`)

- `FLiveServer` owns one `FTcpListener` on a background thread.
- Each accepted TCP connection gets its own `FRunnable` (one
  thread per client) — `FLiveConnectionRunnable`
  (`BlueprintReaderLiveServer.cpp:53-258`). That thread does the
  protocol I/O (hello → auth → op loop).
- The actual op dispatch is *bounced to the game thread* via
  `AsyncTask(ENamedThreads::GameThread, …)` and the connection
  thread blocks on an `FEvent` until the game thread completes
  (`BlueprintReaderLiveServer.cpp:146-155`). This is non-negotiable:
  `UBlueprint` mutation, the asset registry, the compiler — all of
  it must run on the game thread.
- In practice one MCP server has exactly one live connection, so
  there's one I/O thread + one game-thread bounce per call.

## Key invariants

These hold across the whole codebase. Violating any of them breaks
the protocol or breaks `.uasset` integrity.

### snake_case wire JSON

The MCP wire format uses snake_case keys (`asset_path`, `default_value`,
`is_replicated`). UE side wraps this through
`FJsonObjectConverter` + the `FBPVariableInfo` / `FBPNodeInfo` USTRUCTs
which are explicitly tagged for the snake_case shape. The plugin's
hand-rolled emit path goes through
`BlueprintReaderWireJson.cpp`, which is the single source of truth
for serialized field names. Don't bypass it.

### Package paths everywhere

`Blueprint->GetPathName()` returns the UE object path
(`/Game/AI/BP_Foo.BP_Foo`). The wire shape always uses the package
path (`/Game/AI/BP_Foo`). The plugin's
`FBlueprintReaderWireJson::ToPackagePath` strips the trailing
object suffix
(`BlueprintReaderWireJson.cpp:14-24`). Any new wire shape that
references an asset path must go through that helper.

### Empty optional strings → JSON null

The wire shape distinguishes "field is absent / not applicable" from
"field is the empty string". The plugin emits `null` for the former
via `BlueprintReaderWireJson.cpp:26-42` (`StringOrNull` /
`SetStringOrNull`). On the MCP-server side this maps to
`std::optional<std::string>` in `BlueprintReaderTypes.h`.

### `meta` is an inline object, not a string of JSON

Per-node `BPNode.meta` is a nested JSON object on the wire, not a
serialized JSON string. The plugin emits it via
`NodeMetaToJson`
(`BlueprintReaderWireJson.cpp:101-113`). The standalone MCP server
holds it as `nlohmann::json` directly; the UE-side USTRUCT mirror
stores it as a serialized FString because `FJsonObjectConverter`
doesn't preserve arbitrary JSON values through a UPROPERTY, and the
emit path unwraps it back to a nested object before sending. See
the header comment in
[`BlueprintReaderTypes.h:12-16`](../../Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/BlueprintReaderTypes.h).

### Error envelope with `_meta`

Every `tools/call` response carries `_meta`. On success: `_meta:
{elapsed_ms, tool}`. On error: `_meta: {elapsed_ms, tool, args}` —
the args are echoed verbatim so an agent debugging a failure can see
exactly what triggered it without re-driving the call
(`Mcp.cpp:142-158`). MCP spec 2024-11-05 reserves `_meta` as the
extension point on tool result envelopes; clients that surface it
get telemetry, others ignore it.

### `__BPR_DONE <code>__` sentinel is sacred

The commandlet daemon emits `__BPR_DONE <exit-code>__\n` after each
op. The MCP-side `CommandletBlueprintReader` scans daemon stdout for
the *first* occurrence of that literal. Putting that string in any
log message — even a `Display`-level help line — breaks the next
call's scan. The corollary: the daemon's stdout uses raw Win32
`WriteFile`, not UE log devices, so unrelated `UE_LOG` chatter
doesn't end up interleaved with the sentinel
(`BlueprintReaderCommandlet.cpp:5334-5391`).

### Frame format auto-detection, locked on first read

`jsonrpc::Server::Run` doesn't preconfigure the framing format. It
auto-detects on the first incoming frame: `{` or `[` → newline-
delimited (MCP spec), anything else → LSP-style `Content-Length`
headers. Both client and server use the detected format for the rest
of the session (`Server.cpp:117-159`, `Server.cpp:285-292`).

### Single-instance lock per project (superseded — see 05-backends.md)

The original MCP-server-level single-instance lock (exclusive
`CreateFileW` on `%TEMP%/bp-reader-mcp-<fnv1a64>.lock`) was removed in
the multi-session work. The concern it guarded against — two processes
competing for the same `.uasset` files and DDC — is now handled by the
daemon-side lifetime lock and idle/grace timers. See
[05-backends.md](05-backends.md) for the current behavior. Different
projects still get independent daemons and run in parallel fine.

## See also

- [01-overview.md](01-overview.md) — what the pieces are.
- [03-plugin-internals.md](03-plugin-internals.md) — op dispatch,
  K2 schema usage, daemon mode.
- [04-mcp-server.md](04-mcp-server.md) — JSON-RPC server, tool
  registry, response controls.
- [CLAUDE.md → "Common gotchas"](../../CLAUDE.md) — the lived
  history of each invariant.
