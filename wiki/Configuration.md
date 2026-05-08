# Configuration

The MCP server reads its configuration from environment variables at
startup. In a Claude config you set them under the server's `env` block.

## Environment variables

| Variable                    | Default                                | Purpose                                                                  |
|-----------------------------|----------------------------------------|--------------------------------------------------------------------------|
| `BP_READER_BACKEND`         | `mock`                                 | `mock` \| `commandlet`                                                   |
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
