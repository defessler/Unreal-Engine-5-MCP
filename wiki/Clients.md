# Clients & launching

How to start `BlueprintReaderMcp.exe` and how to wire it into Claude, GitHub
Copilot, and ChatGPT. The server is a Windows-only stdio JSON-RPC
process — every MCP client below understands stdio except ChatGPT,
which needs a bridge (covered last).

## Starting the server

You almost never need to start it yourself — every supported client
launches the exe with the env vars it needs and pipes JSON-RPC over
the child's stdio. Three exceptions where manual launch matters:

1. **Debugging** — pipe a hand-crafted frame at it, watch stderr.
2. **Validating a config change** — confirm env paths resolve before
   restarting Claude / Copilot.
3. **Driving from a script** — your own MCP client or a smoke test.

Two equivalent launchers ship under `Plugins\BlueprintReader\Scripts\`:

```
Start-MCPServer.ps1   ← PowerShell — direct
Start-MCPServer.bat   ← double-click-friendly wrapper around the .ps1
```

Both auto-load env from `<ProjectDir>\.mcp.json` so the launch matches
what Claude Code would spawn (commandlet mode by default for this
project).

```powershell
# PowerShell
pwsh -File Plugins\BlueprintReader\Scripts\Start-MCPServer.ps1

# cmd / Explorer double-click
Plugins\BlueprintReader\Scripts\Start-MCPServer.bat
```

Override per-call (works with either):

```
-Backend mock              force mock backend
-Prewarm 0                 skip editor pre-warm (fast startup)
-EngineDir "D:\Other"      different engine
-UProject  "D:\Foo.uproject"
-Exe       "...\Debug\BlueprintReaderMcp.exe"
```

For building the MCP server (it's a UBT `Program` target — no separate
CMake step), there's a wrapper pair:

```
Build-MCPServer.ps1   ← PowerShell
Build-MCPServer.bat   ← cmd wrapper
```

Both invoke `Build.bat BlueprintReaderMcp Win64 Development` against
your engine + `.uproject`, producing
`<Project>\Binaries\Win64\BlueprintReaderMcp.exe`. Pair it with
`Verify-Build.ps1` to confirm both halves of the plugin (server exe +
editor DLL) are present.

Server runs in the foreground; stderr goes to the console; stdin reads
JSON-RPC frames. Ctrl-C or close stdin to stop.

To send a request manually (PowerShell):

```powershell
$body = '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"manual","version":"0"}}}'
"Content-Length: $($body.Length)`r`n`r`n$body" |
    pwsh -File Plugins\BlueprintReader\Scripts\Start-MCPServer.ps1
```

For a full handshake + tool-call smoke, the doctest binary covers it
end-to-end:

```powershell
.\Binaries\Win64\BlueprintReaderMcpTests.exe
```

## Claude Code (recommended)

Project scope — the repo ships `.mcp.json` at the root. Cloning + launching
Claude Code from the project dir wires bp-reader automatically:

```json
{
  "mcpServers": {
    "bp-reader": {
      "command": "D:\\Projects\\UE5_MCP\\Binaries\\Win64\\BlueprintReaderMcp.exe",
      "env": {
        "BP_READER_BACKEND":    "commandlet",
        "BP_READER_ENGINE_DIR": "D:\\Projects\\Unreal Engine 5",
        "BP_READER_PROJECT":    "D:\\Projects\\UE5_MCP\\UE5_MCP.uproject",
        "BP_READER_PREWARM":    "1"
      }
    }
  }
}
```

Edit if your local layout differs. Claude reads `.mcp.json` once at
session startup; changes need an `/exit` + reopen to take effect.

User scope (available in any directory, not just the project):

```powershell
claude mcp add bp-reader --scope user `
    --env BP_READER_BACKEND=commandlet `
    --env "BP_READER_ENGINE_DIR=D:\Projects\Unreal Engine 5" `
    --env "BP_READER_PROJECT=D:\Projects\UE5_MCP\UE5_MCP.uproject" `
    --env BP_READER_PREWARM=1 `
    -- "D:\Projects\UE5_MCP\Binaries\Win64\BlueprintReaderMcp.exe"
```

Writes to `~/.claude.json`. Bp-reader will spawn in *every* Claude Code
session you start; the editor daemon is still lazy unless `PREWARM=1`.

## Claude Desktop

Same JSON shape as Claude Code, in
`%APPDATA%\Claude\claude_desktop_config.json` under `mcpServers`:

```json
{
  "mcpServers": {
    "bp-reader": {
      "command": "D:\\Projects\\UE5_MCP\\Binaries\\Win64\\BlueprintReaderMcp.exe",
      "env": {
        "BP_READER_BACKEND":    "commandlet",
        "BP_READER_ENGINE_DIR": "D:\\Projects\\Unreal Engine 5",
        "BP_READER_PROJECT":    "D:\\Projects\\UE5_MCP\\UE5_MCP.uproject",
        "BP_READER_PREWARM":    "1"
      }
    }
  }
}
```

Restart Claude Desktop after editing.

## GitHub Copilot (VS Code)

MCP support is GA since VS Code 1.102 (July 2025). Tools only surface
in **Agent mode** — switch the Copilot Chat mode picker from Ask/Edit
to Agent.

Workspace scope (recommended, travels with the repo): create
`.vscode/mcp.json` next to your `.code-workspace` or workspace root:

```json
{
  "servers": {
    "bp-reader": {
      "type": "stdio",
      "command": "D:\\Projects\\UE5_MCP\\Binaries\\Win64\\BlueprintReaderMcp.exe",
      "env": {
        "BP_READER_BACKEND":    "commandlet",
        "BP_READER_ENGINE_DIR": "D:\\Projects\\Unreal Engine 5",
        "BP_READER_PROJECT":    "D:\\Projects\\UE5_MCP\\UE5_MCP.uproject",
        "BP_READER_PREWARM":    "1"
      }
    }
  }
}
```

Note: top-level key is `"servers"` (not `"mcpServers"` like Claude),
each entry has an explicit `"type": "stdio"`. Otherwise the shape is
identical.

User scope (cross-workspace): open the Command Palette →
**MCP: Open User Configuration**. Same JSON shape.

VS Code prompts to trust the server the first time it sees a new
entry. If a server gets stuck after edits, run **MCP: Reset Cached
Tools** or reload the window.

## GitHub Copilot (JetBrains Rider / IDEA / etc.)

JetBrains Copilot supports MCP, but its env-block handling has a quirk
worth knowing about: env vars past the first ~4 entries can silently
fail to reach the spawned server. If you set everything in the
config's `env` block and the server's banner shows defaults for the
later entries, this is what's happening.

**Workaround: route the launch through `Start-MCPServer.bat`**, which
re-loads the env from `.mcp.json` itself before exec'ing the exe. The
launcher reads every `BP_READER_*` key from the JSON's env block and
sets it in the child process — so even if Copilot only forwards 2 of
your 6 vars, the launcher fills in the rest.

```json
{
  "bp-reader": {
    "type": "stdio",
    "command": "D:\\YourGame\\MyProject\\Plugins\\BlueprintReader\\Scripts\\Start-MCPServer.bat",
    "env": {
      "BP_READER_BACKEND":    "commandlet",
      "BP_READER_ENGINE_DIR": "D:\\YourGame",
      "BP_READER_PROJECT":    "D:\\YourGame\\MyProject\\MyProject.uproject"
    }
  }
}
```

Then put the **complete** env block (including any vars beyond the
first 4) in `<projectRoot>/.mcp.json`:

```json
{
  "mcpServers": {
    "bp-reader": {
      "command": "...BlueprintReaderMcp.exe",
      "env": {
        "BP_READER_BACKEND":    "commandlet",
        "BP_READER_ENGINE_DIR": "D:\\YourGame",
        "BP_READER_PROJECT":    "D:\\YourGame\\MyProject\\MyProject.uproject",
        "BP_READER_PREWARM":    "1",
        "BP_READER_EDITOR_ARGS": "-EnableAllPlugins"
      }
    }
  }
}
```

The launcher's banner prints `forwarded from .mcp.json: …` so you can
verify what got injected. Restart the IDE after switching the
`command` field.

## ChatGPT (requires a bridge)

> ⚠ **ChatGPT does not support local stdio MCP servers.** It only
> connects to remote MCP servers over HTTPS. Adding `BlueprintReaderMcp.exe`
> directly is not possible — you must wrap it with a bridge that
> exposes the stdio process as an HTTPS endpoint, then register the
> public URL via ChatGPT's Connectors UI.

Plan availability: MCP support landed for ChatGPT Plus / Pro / Business
/ Enterprise / Edu in **September 2025** as a beta called **Developer
mode**. Free accounts get pre-built connectors but not the custom-MCP
"add by URL" path.

### 1. Bridge the stdio server to HTTPS

The simplest path uses `mcp-remote` (a small Node bridge maintained by
the MCP community) plus any tunnel that gives you a public HTTPS URL —
ngrok, Cloudflare Tunnel, Tailscale Funnel, or your own VPS.

Rough recipe:

```powershell
# 1. Install mcp-remote (one-time)
npm i -g mcp-remote

# 2. Wrap BlueprintReaderMcp as an HTTP MCP server on localhost:8080
mcp-remote `
    --command "D:\Projects\UE5_MCP\Binaries\Win64\BlueprintReaderMcp.exe" `
    --port 8080

# 3. Tunnel it out (separate shell). Free ngrok works for dev:
ngrok http 8080
# → https://abc123.ngrok-free.app
```

Set `BP_READER_*` env vars in the shell where you launch `mcp-remote` so
they propagate to the spawned exe.

> Production note: ngrok URLs rotate on each restart, which means
> re-registering the connector in ChatGPT every time. For sustained
> use, a stable URL (Cloudflare Tunnel + a domain you own, or a small
> VPS) is much less friction.

### 2. Register the connector in ChatGPT

1. Profile → **Settings** → **Apps & Connectors**.
2. Scroll to **Advanced** at the bottom; toggle **Developer mode** on.
3. Back up to **Connectors** → **Create** (or **Add custom connector**).
4. Fill in:
   - **Name**: bp-reader (or whatever)
   - **MCP Server URL**: the HTTPS endpoint from step 1, ending in
     `/mcp` or `/sse` (whichever transport `mcp-remote` exposes — usually
     `/mcp`)
   - **Authentication**: none (your tunnel is the only access control)
   - Acknowledge the unverified-server warning
5. In a new chat, open the tools picker and enable the connector
   for that conversation.

ChatGPT shows a confirmation dialog before each write tool call by
default — there's no way to silently accept all calls (which is a
sensible default given the "Developer mode" framing).

### Caveats

- **Tunnel = trust boundary.** Anyone who knows your ngrok/tunnel URL
  can call your tools, including write tools that mutate `.uasset`
  files. Use auth on the tunnel (ngrok basic auth, Cloudflare Access)
  for anything beyond local dev.
- **Latency.** Round-tripping through a tunnel adds 100–500 ms vs.
  Claude/Copilot's direct stdio. The 28 s editor cold start is still
  the dominant cost; subsequent calls are 30 ms + tunnel.
- **No guarantee of feature parity.** ChatGPT's connector model
  evolves; Developer mode is still beta. Verify behavior on your plan.

## Summary

| Client          | Local stdio? | Config location                              | Key       |
|-----------------|--------------|----------------------------------------------|-----------|
| Claude Code     | ✓            | `<project>/.mcp.json` or `~/.claude.json`    | `mcpServers` |
| Claude Desktop  | ✓            | `%APPDATA%\Claude\claude_desktop_config.json` | `mcpServers` |
| Copilot (VS Code) | ✓          | `.vscode/mcp.json` (workspace) or User cfg   | `servers`    |
| ChatGPT         | ✗ (HTTPS only) | Settings → Connectors (UI, no file)        | n/a       |

Claude and Copilot are symmetric and trivial. ChatGPT works but adds
operational overhead (bridge + tunnel). For day-to-day Blueprint
work, Claude Code or Copilot is the path of least resistance.
