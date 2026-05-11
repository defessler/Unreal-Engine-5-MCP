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
| `BP_READER_STARTUP_TIMEOUT_SECONDS` | `600`                          | How long the server waits for the editor daemon to reach READY on first launch. Bigger UE projects (lots of plugins, large content set, cold DDC) can take 5–10 minutes the first time. |
| `BP_READER_DAEMON`          | `1` (on)                               | `0`/`false`/`no`/`off` to opt out of daemon mode.                        |
| `BP_READER_PREWARM`         | `0` (off)                              | `1`/`true`/`yes`/`on` to spawn the editor daemon on MCP startup in a background thread, hiding the cold-start cost. Requires `BP_READER_DAEMON` enabled (default). |
| `BP_READER_EDITOR_ARGS`     | (empty)                                | Whitespace-separated args appended to `UnrealEditor-Cmd.exe`'s command line. Most useful value: `-EnableAllPlugins` — makes plugin-module load failures non-fatal so the editor starts up even when binary marketplace plugins (DLSS, Wwise, etc.) aren't built. See [Troubleshooting](Troubleshooting). |
| `BP_READER_EDITOR_CONFIG`   | (empty → `Development`)                | Picks which `UnrealEditor-Cmd[-Win64-Config].exe` the daemon launches. Default unsets to `Development` (suffix-less). Set to `DebugGame` / `Debug` / `Test` / `Shipping` if your `BlueprintReaderEditor` module is built in that config — UE only loads plugin DLLs whose suffix matches the running editor process. |
| `BP_READER_CACHE_TTL_SECONDS` | `30`                                 | How long the server memoizes read-tool responses for (per (operation, asset) key). Set to `0` to disable. See [Response caching](#response-caching) below. |
| `BP_READER_READ_ONLY`       | `0` (off)                              | `1`/`true`/`yes`/`on` rejects every write tool with a structured error. Use this when running the MCP server alongside an open UE editor (concurrent writes to the same `.uasset` corrupt state). Reads pass through normally; the cache's mtime invalidation (C2) keeps responses fresh as the editor saves. |
| `BP_READER_LIVE_HOST`       | `127.0.0.1`                            | Hostname for the live backend's TCP connection. Loopback only; non-loopback connections are rejected by the editor-side listener. |
| `BP_READER_LIVE_PORT`       | (unset → live backend disabled)        | TCP port for the live backend. **Set in BOTH** the editor's process env (the listener binds here) AND the MCP server's process env (the client connects here). Pick anything 8400–8500 range that's not in use. |
| `BP_READER_LIVE_TOKEN`      | (unset → live backend refuses to start) | Shared secret for the live backend's auth handshake. **Set in BOTH** processes; values must match. Pick a random string. Treat like a password — anyone with localhost access who can read your env vars can mutate BPs. |

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
for every subsequent call until the MCP server exits. Per-call cost
drops from ~5 s to ~30 ms.

Mechanics:
- Server pipes one newline-terminated commandlet-arg line per call to
  the editor's stdin.
- Plugin emits a `__BPR_DONE <code>__` sentinel on stdout when each call
  finishes; server reads that to know the result file is ready.
- If the daemon dies (crash, OOM, killed), the next call falls back to a
  one-shot subprocess automatically — no error surfaced to the client.

Disable for debugging:

```json
"env": {
  "BP_READER_BACKEND": "commandlet",
  "BP_READER_DAEMON":  "0",
  ...
}
```

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
      "command": "D:\\Projects\\UE5_MCP\\Plugins\\BlueprintReader\\mcp-server\\build\\Release\\bp-reader-mcp.exe",
      "env": {
        "BP_READER_BACKEND":    "commandlet",
        "BP_READER_ENGINE_DIR": "D:\\Projects\\Unreal Engine 5",
        "BP_READER_PROJECT":    "D:\\Projects\\Game1\\Game1.uproject",
        "BP_READER_PREWARM":    "1"
      }
    },
    "bp-game2": {
      "command": "D:\\Projects\\UE5_MCP\\Plugins\\BlueprintReader\\mcp-server\\build\\Release\\bp-reader-mcp.exe",
      "env": {
        "BP_READER_BACKEND":    "commandlet",
        "BP_READER_ENGINE_DIR": "D:\\Projects\\Unreal Engine 5",
        "BP_READER_PROJECT":    "D:\\Projects\\Game2\\Game2.uproject"
      }
    }
  }
}
```

Each server holds its own daemon process and binds to one project. Don't
share a single MCP server entry across projects — the daemon's editor
process has the project's modules loaded and switching projects mid-
flight would require killing it.

(Note: enabling `BP_READER_PREWARM` on multiple registered servers means
each one warms its own editor on session start — multiplied cost. Pick
which projects you want pre-warmed and leave the rest lazy.)

## Timeouts

Two separate timeouts:

| Variable                            | Default | What it covers                                                                                                                                                          |
|-------------------------------------|---------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `BP_READER_STARTUP_TIMEOUT_SECONDS` | 600 s   | Time the server waits for the editor daemon to print `__BPR_READY__` on its first launch. Big projects (many plugins, large content set, cold DDC) can take 5–10 minutes. |
| `BP_READER_TIMEOUT_SECONDS`         | 120 s   | Per-tool-call timeout once the daemon is hot. Tool calls return in ~30 ms typically; the timeout only matters for genuinely slow ops (large blueprint compiles).         |

If you hit the **startup** timeout (`daemon failed to reach READY (waited up
to Ns; bump BP_READER_STARTUP_TIMEOUT_SECONDS for slower projects)`), bump
that env var. The wait is one-time per MCP server lifetime — once the
daemon hits READY, subsequent tool calls run under the per-call timeout.

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
      "command": "D:\\Projects\\UE5_MCP\\Plugins\\BlueprintReader\\mcp-server\\build\\Release\\bp-reader-mcp.exe"
    }
  }
}
```

Manual override (fixed port):

```jsonc
{
  "mcpServers": {
    "bp-reader-live": {
      "command": "D:\\Projects\\UE5_MCP\\Plugins\\BlueprintReader\\mcp-server\\build\\Release\\bp-reader-mcp.exe",
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

All 61 tools route over the same wire: reads, writes, batch ops,
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
`Plugins/BlueprintReader/mcp-server/fixtures/`:

- `BP_Enemy.json` — Actor parent, 4 vars, EventGraph + Damage function.
- `BP_Pickup.json` — Actor parent, 2 vars, EventGraph only.
- `BP_PlayerController.json` — PlayerController parent, 5 vars, multiple
  graphs.

Edit them to test edge cases. Override the dir with
`BP_READER_FIXTURES_DIR` if you want to point at a custom set.

## CI

GitHub Actions workflow at `.github/workflows/mcp-server.yml` builds and
runs the mock-backend tests on every push that touches
`Plugins/BlueprintReader/mcp-server/` or the workflow itself. The 12
commandlet-backed tests skip automatically when `BP_READER_ENGINE_DIR` /
`BP_READER_PROJECT` aren't set, so CI runs in under a minute against the
vendored deps.

The workflow runs on `windows-2022`. Linux/macOS are not currently
supported because the subprocess management is `CreateProcessW`-based.

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

For raw debugging, drive the server directly with the bundled smoke test:

```powershell
pwsh -File Plugins\BlueprintReader\mcp-server\scripts\roundtrip.ps1 `
    -Exe Plugins\BlueprintReader\mcp-server\build\Release\bp-reader-mcp.exe `
    -Asset /Game/AI/BP_TestEnemy
```

The script writes the request/response pairs to the console.
