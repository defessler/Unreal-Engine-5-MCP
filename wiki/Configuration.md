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
| `BP_READER_TIMEOUT_SECONDS` | `120`                                  | Per-call subprocess timeout.                                             |
| `BP_READER_DAEMON`          | `1` (on)                               | `0`/`false`/`no`/`off` to opt out of daemon mode.                        |
| `BP_READER_PREWARM`         | `0` (off)                              | `1`/`true`/`yes`/`on` to spawn the editor daemon on MCP startup in a background thread, hiding the cold-start cost. Requires `BP_READER_DAEMON` enabled (default). |

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
      "command": "D:\\Projects\\UE5_MCP\\mcp-server\\build\\Release\\bp-reader-mcp.exe",
      "env": {
        "BP_READER_BACKEND":    "commandlet",
        "BP_READER_ENGINE_DIR": "D:\\Projects\\Unreal Engine 5",
        "BP_READER_PROJECT":    "D:\\Projects\\Game1\\Game1.uproject",
        "BP_READER_PREWARM":    "1"
      }
    },
    "bp-game2": {
      "command": "D:\\Projects\\UE5_MCP\\mcp-server\\build\\Release\\bp-reader-mcp.exe",
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

`BP_READER_TIMEOUT_SECONDS` applies per call. Default 120 s is generous
for everything except the very first cold-start call (which can hit
shader compile if the DDC is empty). If you regularly hit the timeout,
warm the DDC by opening the project in the full editor once.

## Mock backend fixtures

The mock backend reads three handcrafted JSON files from
`mcp-server/fixtures/`:

- `BP_Enemy.json` — Actor parent, 4 vars, EventGraph + Damage function.
- `BP_Pickup.json` — Actor parent, 2 vars, EventGraph only.
- `BP_PlayerController.json` — PlayerController parent, 5 vars, multiple
  graphs.

Edit them to test edge cases. Override the dir with
`BP_READER_FIXTURES_DIR` if you want to point at a custom set.

## CI

GitHub Actions workflow at `.github/workflows/mcp-server.yml` builds and
runs the mock-backend tests on every push that touches `mcp-server/`,
`Shared/`, or the workflow itself. The 12 commandlet-backed tests skip
automatically when `BP_READER_ENGINE_DIR` / `BP_READER_PROJECT` aren't
set, so CI runs in under a minute once the FetchContent cache is warm.

The workflow runs on `windows-2022`. Linux/macOS are not currently
supported because the subprocess management is `CreateProcessW`-based.

## Logs

The MCP server writes nothing to stdout by design — stdout is the
JSON-RPC channel. Anything diagnostic goes to stderr, which Claude
captures and surfaces in its tool-call debug panel.

For raw debugging, drive the server directly with the bundled smoke test:

```powershell
pwsh -File mcp-server\scripts\roundtrip.ps1 `
    -Exe mcp-server\build\Release\bp-reader-mcp.exe `
    -Asset /Game/AI/BP_TestEnemy
```

The script writes the request/response pairs to the console.
