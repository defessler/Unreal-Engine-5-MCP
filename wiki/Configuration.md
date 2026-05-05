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

## Per-project configuration

To work on multiple UE projects from the same Claude session, register
each as its own MCP server:

```json
{
  "mcpServers": {
    "bp-game1": {
      "command": "D:\\Projects\\UE5_MCP\\mcp-server\\build\\Release\\bp-reader-mcp.exe",
      "env": {
        "BP_READER_BACKEND":    "commandlet",
        "BP_READER_ENGINE_DIR": "D:\\Projects\\Unreal Engine 5",
        "BP_READER_PROJECT":    "D:\\Projects\\Game1\\Game1.uproject"
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
