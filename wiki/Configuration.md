# Configuration

The MCP server reads its configuration from environment variables at
startup. In a Claude config you set them under the server's `env` block.

## Environment variables

| Variable                    | Default                                | Purpose                                                                  |
|-----------------------------|----------------------------------------|--------------------------------------------------------------------------|
| `BP_READER_BACKEND`         | `mock`                                 | `mock` \| `commandlet` \| `live` (talks to a running editor over TCP — see below) |
| `BP_READER_FIXTURES_DIR`    | `<exe>/fixtures`                       | Mock backend fixture dir.                                                |
| `BP_READER_ENGINE_DIR`      | (unset → fail-fast for `commandlet`)   | Source-built engine root (the dir holding `Engine\Binaries\Win64\`).     |
| `BP_READER_PROJECT`         | (unset → fail-fast for `commandlet`)   | Path to `.uproject`.                                                     |
| `BP_READER_TIMEOUT_SECONDS` | `120`                                  | Per-tool-call subprocess timeout.                                        |
| `BP_READER_STARTUP_TIMEOUT_SECONDS` | `600`                          | How long the server waits for the commandlet daemon to publish its handshake file on first launch. Bigger UE projects (lots of plugins, large content set, cold DDC) can take 5–10 minutes the first time. |
| `BP_READER_DAEMON`          | `1` (on)                               | `0`/`false`/`no`/`off` to opt out of daemon mode.                        |
| `BP_READER_DAEMON_IDLE_SECONDS` | `300`                              | Daemon exits cleanly after this many seconds with no connected MCP-server clients. Set higher if a slow workflow disconnects between calls and you want to avoid cold-restart cost; set lower to free editor memory faster after sessions end. |
| `BP_READER_BATCH_ON_DISCONNECT` | `commit`                           | When an MCP-server socket closes mid-batch, `commit` (default) flushes the connection's pending BPs via game-thread `CompileAndSaveBlueprint`; `discard` drops them without saving. Use `discard` for fail-closed semantics (no half-applied state on disk) at the cost of losing in-flight work; `commit` keeps best-effort progress. Cross-session BP write lock is now also enforced — concurrent writes to the same BP across sessions get a `blueprint_locked_by_other_session` error (exit code 6) instead of silently corrupting the holding session's pending state. See [Multi-session](#multi-session) for the full picture. |
| `BP_READER_PREWARM`         | `0` (off)                              | `1`/`true`/`yes`/`on` to spawn the editor daemon on MCP startup in a background thread, hiding the cold-start cost. Requires `BP_READER_DAEMON` enabled (default). |
| `BP_READER_EDITOR_ARGS`     | (empty)                                | Whitespace-separated args appended to `UnrealEditor-Cmd.exe`'s command line. Most useful value: `-EnableAllPlugins` — makes plugin-module load failures non-fatal so the editor starts up even when binary marketplace plugins (DLSS, Wwise, etc.) aren't built. See [Troubleshooting](Troubleshooting). |
| `BP_READER_EDITOR_CONFIG`   | (empty → `Development`)                | Picks which `UnrealEditor-Cmd[-Win64-Config].exe` the daemon launches. Default unsets to `Development` (suffix-less). Set to `DebugGame` / `Debug` / `Test` / `Shipping` if your `BlueprintReaderEditor` module is built in that config — UE only loads plugin DLLs whose suffix matches the running editor process. |
| `BP_READER_CACHE_TTL_SECONDS` | `30`                                 | How long the server memoizes read-tool responses for (per (operation, asset) key). Set to `0` to disable. See [Response caching](#response-caching) below. |
| `BP_READER_READ_ONLY`       | `0` (off)                              | `1`/`true`/`yes`/`on` rejects every write tool with a structured error. Use this when running the MCP server alongside an open UE editor (concurrent writes to the same `.uasset` corrupt state). Reads pass through normally; the cache's mtime invalidation (C2) keeps responses fresh as the editor saves. |
| `BP_READER_LIVE_HOST`       | `127.0.0.1`                            | Hostname for the live backend's TCP connection. Loopback only; non-loopback connections are rejected by the editor-side listener. |
| `BP_READER_LIVE_PORT`       | (unset → live backend disabled)        | TCP port for the live backend. **Set in BOTH** the editor's process env (the listener binds here) AND the MCP server's process env (the client connects here). Pick anything 8400–8500 range that's not in use. |
| `BP_READER_LIVE_TOKEN`      | (unset → live backend refuses to start) | Shared secret for the live backend's auth handshake. **Set in BOTH** processes; values must match. Pick a random string. Treat like a password — anyone with localhost access who can read your env vars can mutate BPs. |
| `BP_READER_TOOLS`           | (unset → all 126 tools)                | Comma-separated allow-list of tool names AND/OR category names. When set, `tools/list` advertises only the matching subset. Use to fit under MCP clients' tool-count caps — GitHub Copilot caps at **128 tools total** across all servers + its built-ins, leaving razor-thin headroom for the full bp-reader surface. See [Tool filtering](#tool-filtering) below for category names + recommended presets. |
| `BP_READER_TOOLS_EXCLUDE`   | (unset → no removals)                  | Comma-separated deny-list of tool names AND/OR category names. Applied AFTER the allow step (or against the full tool set when `BP_READER_TOOLS` is unset). Useful when you want most-of-everything minus specific asset types: `BP_READER_TOOLS_EXCLUDE=materials,widgets,niagara` keeps ~89 of 126 tools. |
| `BP_READER_ALLOW_PYTHON`    | `0` (off)                              | `1`/`true`/`yes`/`on` enables the `run_python_script` tool. Off by default — arbitrary Python in the editor has full `unreal.*` API access and bypasses every safety convention the curated tool surface establishes. When disabled, calling `run_python_script` returns `{ok: false, error: "python_disabled", hint: ...}` rather than executing. |

## Tool filtering

The full bp-reader surface is 126 tools. Some MCP clients cap the total
number of tools they're willing to advertise — most notably **GitHub
Copilot caps at 128 tools** across all servers + its built-ins, which
fails fast with `"You may not include more than 128 tools in your
request"` when the full surface is exposed. Two env vars let you pare
the surface down without giving up the workflow you actually need:

- `BP_READER_TOOLS` — allow-list. Comma-separated list of tool names
  and/or category names. When set, ONLY these are advertised.
- `BP_READER_TOOLS_EXCLUDE` — deny-list. Subtracted after the allow
  step (or from the full set when no allow-list is set).

Both vars take **category names** as a shorthand for groups of tools:

| Category         | Tools | What's in it |
|------------------|-------|--------------|
| `core`           | ~35   | Minimum viable BP authoring: list/read/get/find, var/function/node CRUD, batches, save_all, discovery |
| `read`           | ~40   | Every read-only tool (covers BPs, materials, widgets, BTs, data assets, anim, etc.) |
| `write`          | ~25   | Every BP write tool (vars, functions, nodes, components, batches) |
| `cpp`            | 7     | Decompile + transpile + parse_cpp_function pipeline |
| `editor`         | ~15   | Viewport, PIE, console, cvars, log, screenshots, selection |
| `assets`         | 6     | list_blueprints, move/delete/folder, project metadata, save_all |
| `materials`      | 7     | Material graph CRUD + compile |
| `widgets`        | 5     | UMG widget authoring + compile |
| `behavior-trees` | 5     | BT graph CRUD + compile |
| `data-tables`    | 4     | DataTable read + row ops |
| `data-assets`    | 4     | DataAsset read + property ops |
| `state-trees`    | 5     | StateTree CRUD + compile |
| `niagara`        | 4     | Niagara system CRUD + parameters |
| `sequencer`      | 4     | LevelSequence read + track/playback ops |
| `gameplay-tags`  | 3     | Tag listing + add + ability set read |
| `anim-bp`        | 4     | Anim BP read + state + compile |
| `profiling`      | 3     | start/stop_profile + get_stats |
| `cook`           | 2     | cook_content + package_project |
| `tests`          | 1     | run_automation_tests |
| `class-info`     | 3     | find_class + get_class_info + list_functions |
| `discover`       | 3     | list_node_kinds + list_pin_categories + shutdown_daemon |
| `all`            | 119   | Everything (the implicit default if `BP_READER_TOOLS` is unset) |

The canonical mapping lives in
[`tools/ToolCategories.cpp`](https://github.com/defessler/Unreal-Engine-5-MCP/blob/main/Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/tools/ToolCategories.cpp).

### Workflow presets

Curated cross-category sets tuned for a kind of session. Same token
namespace as the per-domain categories — just use the workflow name
directly as the `BP_READER_TOOLS` value.

| Preset            | Tools | Shape of session |
|-------------------|-------|------------------|
| `bp-authoring`    | 35    | BP CRUD: variable/function/node/component, batches. Same as `core`. |
| `material-tuning` | 11    | Find a BP's mesh, read the material on it, tweak parameters, recompile. |
| `cpp-roundtrip`   | 13    | BP ↔ source: decompile, transpile, parse C++ back, compile to BP. |
| `editor-control`  | 19    | Viewport + PIE + console + log. Drive the editor like a remote control. No BP authoring. |
| `widget-design`   | 8     | UMG: read a widget BP, add nodes, set properties, wire events, compile. |
| `gameplay-tuning` | 16    | Read BPs, tweak variable defaults + component props, batch-apply, PIE-test. The "tweak and try" loop. |

The mapping lives in [`tools/ToolCategories.cpp`](https://github.com/defessler/Unreal-Engine-5-MCP/blob/main/Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/tools/ToolCategories.cpp).

### Recommended presets

**Minimal BP-authoring (35 tools):**

```json
"env": { "BP_READER_TOOLS": "bp-authoring" }
```

**Pure material tuning session (11 tools):**

```json
"env": { "BP_READER_TOOLS": "material-tuning" }
```

**BP-authoring + transpile (~42 tools):**

```json
"env": { "BP_READER_TOOLS": "core,cpp" }
```

**Most-of-everything except specialty asset graphs (82 tools):**

```json
"env": {
  "BP_READER_TOOLS_EXCLUDE": "materials,widgets,niagara,sequencer,state-trees,anim-bp,gameplay-tags,behavior-trees"
}
```

**A mix:** allow categories + add or remove specific tools:

```json
"env": {
  "BP_READER_TOOLS":         "core,cpp,read_material",
  "BP_READER_TOOLS_EXCLUDE": "shutdown_daemon"
}
```

The MCP server logs the result on stderr at startup:

```
[bp-reader-mcp] tool filter: kept 35 of 119 tools (allow=core)
```

Unknown tokens (typos in tool/category names) silently drop nothing —
the log line will show fewer tools than expected, which is the signal
to check spelling.

### Progressive disclosure (BP_READER_PROGRESSIVE=1)

The static filter above is fixed for the whole session — you pick a
preset, you live with it. **Progressive disclosure** is the dynamic
variant: start with a narrow initial set, and let the agent widen the
surface mid-session by calling a meta-tool. Useful when you don't
know up front which tools you'll need, or want to keep the visible
surface small for selection-accuracy reasons (smaller list ≈ fewer
wrong-tool calls).

| Env var | Default | Effect |
|---|---|---|
| `BP_READER_PROGRESSIVE` | `0` (off) | `1` to enable progressive mode. Adds the `enable_tool_category` meta-tool to the active set; client sees `capabilities.tools.listChanged: true` and the server emits `notifications/tools/list_changed` when the active set grows. |
| `BP_READER_TOOLS_INITIAL` | `core` | Initial active subset when progressive mode is on. Same token vocabulary as `BP_READER_TOOLS` — tool names, category names, workflow presets, or `all`. |

### How the agent uses it

After `initialize`, the client sees `capabilities.tools.listChanged:
true` and the initial `tools/list` includes `enable_tool_category`.
The agent calls it when it needs more:

```json
{
  "jsonrpc": "2.0", "id": 42,
  "method": "tools/call",
  "params": {
    "name": "enable_tool_category",
    "arguments": { "category": "materials" }
  }
}
```

Response:

```json
{
  "ok": true,
  "token": "materials",
  "added": ["list_materials", "read_material", "add_material_expression", ...],
  "newly_activated_count": 7,
  "total_active": 43,
  "total_registered": 119
}
```

The server immediately follows the response with:

```json
{ "jsonrpc": "2.0", "method": "notifications/tools/list_changed" }
```

The client re-fetches `tools/list` and now sees the wider surface.
Subsequent `enable_tool_category` calls can keep widening — the
active set only grows, never shrinks.

### When to choose progressive vs. static

| Use static filter | Use progressive disclosure |
|---|---|
| You know what surface you need up front. | The agent's session might pivot — material work then UMG work, etc. |
| Smaller is better (every byte of schema costs context). | Keep the initial schema tiny but let the agent earn more tools as the session evolves. |
| Your client doesn't support `notifications/tools/list_changed`. | Your client does (Claude Code, recent Copilot). |
| You want full reproducibility — same env = same tool surface across calls. | You're willing to let the agent dynamically reshape its toolbox per task. |

### Sample config

```json
"env": {
  "BP_READER_PROGRESSIVE":    "1",
  "BP_READER_TOOLS_INITIAL":  "core"
}
```

The server logs both decisions on stderr at startup:

```
[bp-reader-mcp] tool filter: kept 35 of 119 tools (allow=core)
[bp-reader-mcp] progressive disclosure: enabled. Initial active set is 36 tools (of 119 registered). Agent can widen via `enable_tool_category(<name>)`.
```

(36 = the 35 core tools + the `enable_tool_category` meta-tool.)

## Pre-warm

With `BP_READER_PREWARM=1`, the MCP server spawns the editor daemon on a
background thread the moment it starts up — *before* the first tool call.
Claude Code spawns MCP servers eagerly at session start, so the editor
warms up while you type your first prompt. By the time you ask Claude
about a Blueprint, the daemon is already READY and the call returns in
~30 ms instead of paying the ~5–30 s cold start.

Mechanics:
- Background thread acquires the same `daemonMutex_` that protects real
  tool calls; `EnsureDaemon()` runs under the lock.
- Tool calls that arrive mid-prewarm block on the mutex and inherit the
  now-warm daemon — no double-launch.
- If prewarm fails (engine not built, project mis-set), the error is
  logged to stderr and swallowed. The next real tool call retries
  `EnsureDaemon()` under its own lock and surfaces any error to Claude.
- The destructor `join()`s the prewarm thread before terminating the
  daemon, so MCP shutdown is always clean.

Trade-off: the editor sits at ~600 MB RAM for the lifetime of the MCP
server, even if you never touch a Blueprint tool that session. For dev
workstations this is negligible; for laptops or shared CI runners,
leave prewarm off and pay the cold start on first call.

## Daemon mode (default)

The commandlet backend defaults to **daemon mode**: one
`UnrealEditor-Cmd.exe` process is launched on the first call and reused
for every subsequent call. Per-call cost drops from ~5 s to ~30 ms.

The daemon hosts a localhost TCP listener on an ephemeral port and
publishes its port + auth token to
`<Project>/Saved/bp-reader-cmdlet.json`. MCP servers attach to this
listener through the same socket client that talks to the live editor.

Mechanics:
- First MCP server to need commandlet mode spawns the daemon (under a
  spawn-coordination lock — see below) and attaches via TCP.
- Subsequent MCP servers, against the same project, read the
  handshake file and attach to the same daemon. **One daemon per
  project, shared across all sessions.**
- If the daemon dies (crash, OOM, killed), the next call's
  attach-attempt reads a stale handshake (dead pid), spawns fresh,
  and the user's call falls back to a one-shot subprocess for that
  one call — no error surfaced to the client.
- With no clients connected for `BP_READER_DAEMON_IDLE_SECONDS`
  (default 300), the daemon exits cleanly and deletes its
  handshake file. Next call cold-restarts a fresh daemon.

Disable for debugging:

```json
"env": {
  "BP_READER_BACKEND": "commandlet",
  "BP_READER_DAEMON":  "0",
  ...
}
```

### Cmdlet handshake files

All in `<Project>/Saved/`, alongside `bp-reader-live.json`:

| File | Purpose | Lifetime |
|---|---|---|
| `bp-reader-cmdlet.json` | Daemon handshake — same shape as the live one (`version`, `host`, `port`, `token`, `pid`, `started_at`). | Published when the daemon binds its TCP listener; deleted on graceful exit. |
| `bp-reader-cmdlet.lock` | OS exclusive lifetime lock held by the daemon. | Auto-released by the OS on daemon process exit (clean or crash). |
| `bp-reader-cmdlet-spawn.lock` | OS lock held by an MCP server during its spawn-attempt window only. | Released after spawn confirmation, or on MCP-server crash. |

The two-lock model means concurrent first-time MCP-server arrivals
don't race to spawn duplicate daemons. The spawn-lock winner calls
`CreateProcessW`; any second arriver polls the handshake file.

## Multi-session

The MCP server is **no longer single-instance**. Two Claude Code or
Copilot CLI sessions can both run `bp-reader-mcp.exe` against the
same UE project at the same time — they each get their own MCP
server process and share the one commandlet daemon for that project.

Implications:
- No need to close one Claude session before opening another against
  the same project.
- `Get-Process bp-reader-mcp*` shows one process per active session.
  Only one `UnrealEditor-Cmd.exe` (the shared daemon).
- Live mode is also multi-client — both sessions can connect to the
  same open editor.
- **Cross-session batch isolation is full.** Per-connection state
  (defer flag, pending-compile set) is isolated. Cross-session BP
  write lock is enforced — if your session is in an open batch and
  another session already holds the lock on the same BP, your write
  op gets `blueprint_locked_by_other_session` (exit code 6) with the
  holding connection id + retry hint. The in-memory BP is untouched,
  so the holding session's pending state stays clean. Disconnect
  releases ownership so a crashed client can't keep a BP locked.
  Commit-partial-on-disconnect is on by default; set
  `BP_READER_BATCH_ON_DISCONNECT=discard` to drop pending edits
  when a session drops mid-batch instead of flushing them.

### Exe rename

A project named `UE5_MCP` builds **two** binaries under
`Binaries/Win64/`:

- `bp-reader-mcp-UE5_MCP.exe` — the real binary, with the project
  name in its filename so `tasklist /V` / `Get-Process` shows which
  project the running MCP server belongs to.
- `bp-reader-mcp.exe` — a hard link to the same file, for backward
  compatibility.

Existing `.mcp.json` configs that reference `bp-reader-mcp.exe` by
full path keep working unchanged — the hard link resolves to the
same on-disk bytes. New configs can opt into the project-suffixed
name for clearer process attribution; both names launch the same
program.

`shutdown_daemon`, when invoked, terminates the daemon **for every
session** against this project. The original use case (free file
locks, force a fresh spawn) still works as before; just be aware
that other sessions will pay the cold-restart cost on their next
commandlet-mode call.

## Project scope vs user scope

Two ways to register bp-reader with Claude Code:

| Scope            | File                              | Loads when                                |
|------------------|-----------------------------------|--------------------------------------------|
| **Project**      | `<project>/.mcp.json`             | Claude Code launched from project dir.    |
| **User**         | `~/.claude.json`                  | Every Claude Code session, any directory. |

The repo ships a `.mcp.json` at root, so cloning + launching from the
project dir wires bp-reader automatically. Other devs cloning the repo
get the config for free (paths still need to match their layout).

User scope is right when you want bp-reader available everywhere, e.g.
asking Claude about UE5_MCP from a chat happening in another project.
The cost: the MCP server spawns in every Claude session (lightweight
process; the editor daemon is still lazy unless `BP_READER_PREWARM=1`).

## Per-project configuration

To work on multiple UE projects from the same Claude session, register
each as its own MCP server (user scope):

```json
{
  "mcpServers": {
    "bp-game1": {
      "command": "D:\\Projects\\UE5_MCP\\Binaries\\Win64\\BlueprintReaderMcp.exe",
      "env": {
        "BP_READER_BACKEND":    "commandlet",
        "BP_READER_ENGINE_DIR": "D:\\Projects\\Unreal Engine 5",
        "BP_READER_PROJECT":    "D:\\Projects\\Game1\\Game1.uproject",
        "BP_READER_PREWARM":    "1"
      }
    },
    "bp-game2": {
      "command": "D:\\Projects\\UE5_MCP\\Binaries\\Win64\\BlueprintReaderMcp.exe",
      "env": {
        "BP_READER_BACKEND":    "commandlet",
        "BP_READER_ENGINE_DIR": "D:\\Projects\\Unreal Engine 5",
        "BP_READER_PROJECT":    "D:\\Projects\\Game2\\Game2.uproject"
      }
    }
  }
}
```

Each server entry binds to exactly one `.uproject`. Don't share a
single MCP server entry across projects — the daemon's editor process
has the project's modules loaded and switching projects mid-flight
would require killing it.

Within one project, multiple Claude/Copilot sessions launching MCP
servers from the same entry share **one** daemon — the per-project
shared-daemon model. Across projects, daemons are separate.

(Note: enabling `BP_READER_PREWARM` on multiple registered servers means
each one warms its own editor on session start — multiplied cost. Pick
which projects you want pre-warmed and leave the rest lazy.)

## Timeouts

Two separate timeouts:

| Variable                            | Default | What it covers                                                                                                                                                          |
|-------------------------------------|---------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `BP_READER_STARTUP_TIMEOUT_SECONDS` | 600 s   | Time the server waits for the commandlet daemon to publish its handshake file on first launch. Big projects (many plugins, large content set, cold DDC) can take 5–10 minutes. |
| `BP_READER_TIMEOUT_SECONDS`         | 120 s   | Per-tool-call timeout once the daemon is hot. Tool calls return in ~30 ms typically; the timeout only matters for genuinely slow ops (large blueprint compiles).         |

If you hit the **startup** timeout (`commandlet daemon: ... handshake
never appeared within Ns (bump BP_READER_STARTUP_TIMEOUT_SECONDS for
slower projects)`), bump that env var. The wait is one-time per daemon
lifetime — once the daemon publishes its handshake, every MCP server
that attaches to it pays only the per-call timeout.

If you hit the **per-call** timeout regularly, warm the DDC by opening the
project in the full editor once; commandlet-mode shader compiles run
serially against an empty DDC and dominate cold-start cost.

## Response caching

The server memoizes read-tool results for a short TTL (default 30 s). This
matters because AI clients tend to issue **flurries** of related reads:
"tell me about BP_Enemy" → `summarize_blueprint` → `read_blueprint` →
`list_variables` → `get_graph` is a typical pattern, and the agent often
retries with different `fields` projections. Without caching every one of
those round-trips the editor commandlet (~50–500 ms each, all duplicate
work for the same `.uasset`).

Cache semantics:
- Each read call is keyed by `(operation, asset_path, *extras*)` — for
  example `("graph", "/Game/AI/BP_Enemy", "EventGraph")`.
- Entries expire after `BP_READER_CACHE_TTL_SECONDS`.
- Any **write tool** invalidates ALL cached entries for the affected
  `asset_path`. `list_blueprints` is also invalidated by any write
  (because the `modified_iso` summary changes).
- Cache is in-memory, per-process, NOT shared across MCP sessions.

When to tune the TTL:
- **Default (`30`)** — right for AI-client workloads where you query the
  same BP repeatedly within a turn or two.
- **Longer (`120`+)** — fine if you almost never edit BPs in the editor
  while the MCP server is running. You'll see slightly stale results if
  someone manually edits a `.uasset` with the editor open.
- **`0` (disable)** — set this when you're benchmarking the underlying
  backend, or you have an external mutator that the cache wouldn't see
  (rare).

**mtime-based invalidation (C2).** When the server knows the project
directory (auto-discovered or via `BP_READER_PROJECT`), the cache also
stamps each entry with the `.uasset` file's mtime at insert time and
re-checks on lookup. External edits in the open editor — e.g. you save a
BP manually — invalidate the cached entry on the next read, even if the
TTL hasn't expired. The check is one `stat` per cache hit (microseconds);
disabled automatically when the project dir isn't resolved.

Trade-off: TTL alone is the conservative-but-stale strategy; mtime adds
a freshness check on top. The two combined catch both AI-induced edits
(via the cache's own write-invalidation) and human-induced editor edits
(via mtime).

## Auto backend — picks live vs commandlet per call

`BP_READER_BACKEND=auto` (the default when a `.uproject` is found)
probes the editor's TCP listener on each call and routes to **live**
when an editor is open or **commandlet** when not. No flipping env
vars when you launch / close the editor — auto handles the transition
mid-session, with a 2-second probe cache so per-call cost stays
negligible.

How it knows whether the editor is up:

1. The plugin's `BlueprintReaderEditor` module writes
   `<ProjectDir>/Saved/bp-reader-live.json` on `StartupModule` with
   the port + token + pid. Deletes it on `ShutdownModule`.
2. The auto backend re-reads that file before each call (subject to
   the cache) and does a one-shot TCP connect to confirm the listener
   actually accepts. File present + connect OK → live. Anything else →
   commandlet.
3. A stale handshake file from a crashed editor is harmless: the TCP
   probe fails fast (~5ms) and routing falls through to commandlet.

Set `BP_READER_BACKEND=commandlet` (or `live`) to opt out — explicit
backend selection still works as before.

## Live backend — talk to a running editor over TCP

`BP_READER_BACKEND=live` connects the MCP server to a TCP listener
inside an already-running UE editor's `BlueprintReaderEditor` module,
instead of spawning a second `UnrealEditor-Cmd.exe` daemon. This is the
**only** way to do reads + writes coexisting with the open editor — no
concurrent `.uasset` writes, no DDC contention, no asset-registry
forks, because there's only one editor process. Reads see the editor's
live in-memory state (including unsaved edits); writes go through the
editor's normal mutation pipeline so the content browser refreshes
immediately.

### Zero-config (recommended)

The plugin's editor module **automatically** picks an ephemeral TCP
port + a random 256-bit token at editor startup, and writes them to
`<ProjectDir>/Saved/bp-reader-live.json` (auto-gitignored — `Saved/`
is in UE's default ignore list). The MCP server reads that file when
it starts (and the auto backend re-reads it on every probe), so the
typical user just launches the editor and runs the MCP client — no
env vars needed.

The handshake file shape:

```jsonc
{ "version":   1,
  "host":      "127.0.0.1",
  "port":      <ephemeral>,
  "token":     "<32+ hex chars>",
  "pid":       <editor pid>,
  "started_at": "2026-05-09T..." }
```

The file inherits the project directory's NTFS / POSIX permissions —
treat it like the project itself. Anyone who can read your project
files can read the token.

### Editor restart recovery

The live backend re-reads the handshake file on connect failure and on
auth failure, then retries once before surfacing an error to the agent.
That means restarting the editor while the MCP server is running just
works — no need to bounce the client. The two restart cases are both
handled:
- **Editor binds a new ephemeral port** → first connect refuses; the
  reader re-reads `bp-reader-live.json` (new port + token) and retries.
- **Editor reuses the same port but rotates the token** (common when
  the port cache survives across runs) → first connect succeeds with
  the stale token, auth fails; the reader re-reads the file and
  retries with the fresh token.

Pathological cases (bad hello frame, mid-handshake disconnect, etc.)
skip the retry — refreshing wouldn't change anything — and the error
bubbles immediately.

### Manual override (for fixed-port scenarios)

Pick a port and a token, and set both in **two** places — the editor's
launching process and the MCP server's process:

```pwsh
$env:BP_READER_LIVE_PORT  = "8421"
$env:BP_READER_LIVE_TOKEN = "use-a-real-random-secret-here"
```

Useful when:
- CI / scripted runs that need a deterministic port.
- The handshake file location isn't writable (locked-down `Saved/`).
- Your MCP client doesn't share a project directory with the editor
  process (multi-host setups).

### `.mcp.json` for the live backend

Auto-discovery (no env block needed if the editor's already running
or will be):

```jsonc
{
  "mcpServers": {
    "bp-reader": {
      "command": "D:\\Projects\\UE5_MCP\\Binaries\\Win64\\BlueprintReaderMcp.exe"
    }
  }
}
```

Manual override (fixed port):

```jsonc
{
  "mcpServers": {
    "bp-reader-live": {
      "command": "D:\\Projects\\UE5_MCP\\Binaries\\Win64\\BlueprintReaderMcp.exe",
      "env": {
        "BP_READER_BACKEND":    "live",
        "BP_READER_LIVE_PORT":  "8421",
        "BP_READER_LIVE_TOKEN": "<same secret the editor was launched with>"
      }
    }
  }
}
```

The MCP server connects lazily — it doesn't fail to start if the editor
isn't running yet. The first tool call returns a clear error pointing at
"is the editor running?" (auto mode falls through to commandlet
silently, so this only fires under explicit `BP_READER_BACKEND=live`).

### Opt-out

Set `BP_READER_LIVE_DISABLED=1` in the editor's environment to skip
starting the TCP listener entirely (no handshake file written, no
listener bound). Auto mode then always picks commandlet.

### Operational notes

- **Loopback only.** The editor's listener binds `127.0.0.1`; non-loopback
  connections are dropped. There's no remote-access mode in v1.
- **Auth is required.** Token is auto-generated by default. Set
  `BP_READER_LIVE_TOKEN` explicitly if you want a fixed value (CI
  scenarios, etc.). Wrong token → server closes the connection after
  `auth_fail`.
- **One connection at a time** is the design assumption. The listener
  supports multiple concurrent connections, but ops are dispatched on the
  game thread serially — so practical throughput is one in-flight op
  regardless.
- **Editor stop = server still works.** When you close the editor, the
  handshake file is deleted; auto mode's next call falls through to
  commandlet. Re-launch the editor and the next call lands on live again.
- **No daemon, no `shutdown_daemon` semantics for live mode.** When auto
  mode currently routes through commandlet, `shutdown_daemon` works
  normally; when routing through live, it returns
  `was_running:false`.

### What works in v1

All 126 tools route over the same wire: reads, writes, batch ops,
`compile_function` / `preview_ops`, plus the BP↔C++ transpile group
(`decompile_function`, `decompile_blueprint`, `transpile_function`,
`transpile_blueprint`, `write_generated_source`, `parse_cpp_function`).
The editor's existing `RunOneOp` dispatcher handles them on the game
thread — same code as the commandlet daemon.

### What's deferred

- Auto-discovery of the listener (you set the port explicitly).
- Cross-platform — listener is Windows-tested only in v1.
- Multiple editors at once — one MCP server, one editor.
- Reconnection backoff — currently throws on first failed op; caller
  retries explicitly.

## Read-only coexistence with the open editor

Running the MCP daemon at the same time you have the full UE editor
open is *fragile by default*. Both processes hold the same `.uasset`
files, the same DDC, and the same asset-registry caches. Concurrent
writes can:

- Drop edits (last-write-wins on `.uasset` saves).
- Diverge in-memory state from disk (editor's loaded BP doesn't see
  daemon's changes; daemon's reads don't see editor's unsaved buffer).
- Corrupt the DDC (rare, but possible under shader/material thrash).

**`BP_READER_READ_ONLY=1` is the safe coexistence mode.** With it:

- Read tools (`list_blueprints`, `read_blueprint`, `get_graph`, etc.)
  work normally and return the on-disk state. The C2 mtime cache
  evicts stale entries on the next read after the editor saves —
  responses stay fresh within seconds of the editor's save.
- Every write tool fails fast with a structured error pointing at
  this env var. `apply_ops` / `compile_function` fail too because
  their first mutation op throws.

```jsonc
// .mcp.json snippet for "editor open, agent reads only"
"env": {
  "BP_READER_BACKEND":   "commandlet",
  "BP_READER_READ_ONLY": "1",
  "BP_READER_PREWARM":   "0"
}
```

To switch to read-write mode: close the editor, unset
`BP_READER_READ_ONLY` (or set to `0`), restart the MCP server.

## Mock backend fixtures

The mock backend reads three handcrafted JSON files from
`Plugins/BlueprintReader/Tests/BlueprintReaderMcpTests/fixtures/`:

- `BP_Enemy.json` — Actor parent, 4 vars, EventGraph + Damage function.
- `BP_Pickup.json` — Actor parent, 2 vars, EventGraph only.
- `BP_PlayerController.json` — PlayerController parent, 5 vars, multiple
  graphs.

Edit them to test edge cases. Override the dir with
`BP_READER_FIXTURES_DIR` if you want to point at a custom set.

## CI

The prior `.github/workflows/mcp-server.yml` (CMake-on-Windows-2022,
mock-backend tests only) was removed during the migration of the MCP
server to a UE Program target. UBT-based CI needs a runner with the
source-built engine available, which is heavier than the prior
mock-only CI was — not yet set up. For local pre-push verification,
build the tests target via UBT (see the [Installation](Installation)
page) and run `Binaries\Win64\BlueprintReaderMcpTests.exe`.

Linux/macOS are not currently supported because the subprocess
management is `CreateProcessW`-based.

## Manual launch

For debugging, scripted JSON-RPC, or smoke-testing a config change
before opening a session, use `Plugins\BlueprintReader\Scripts\Start-MCPServer.ps1`.
It auto-loads env from the project's `.mcp.json` and runs the server
in the foreground. See [Clients → Starting the server](Clients#starting-the-server)
for the full invocation reference (param overrides, manual JSON-RPC
piping, the bundled smoke test).

## Logs

The MCP server writes nothing to stdout by design — stdout is the
JSON-RPC channel. Anything diagnostic goes to stderr, which Claude
captures and surfaces in its tool-call debug panel.

For raw debugging, run the doctest suite — it exercises both the wire
shapes and the backends with rich failure diagnostics:

```powershell
Binaries\Win64\BlueprintReaderMcpTests.exe
```

Or drive `BlueprintReaderMcp.exe` directly by piping JSON-RPC frames
to its stdin (the `roundtrip.ps1` smoke harness that previously
filled this slot was removed alongside the CMake build).

The script writes the request/response pairs to the console.
