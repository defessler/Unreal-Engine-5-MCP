# BlueprintReader

A self-contained Unreal Engine plugin + standalone MCP server that exposes
Blueprint introspection / mutation / BP<->C++ transpile / editor-control tools to
MCP clients (Claude Code, Claude Desktop, Cursor, Copilot, Codex, Gemini).

Everything the plugin needs travels in this folder: the two UE modules
(`Source/`), the standalone C++ MCP server + tests (`Tests/`), the AI-assistant
assets (`Claude/` skills + agents, `AGENTS.md`), and the setup scripts
(`Scripts/`). Drop this folder into `<YourProject>/Plugins/BlueprintReader` and
run one of the entry points below.

## Quick start

From `<YourProject>/Plugins/BlueprintReader/` (paths are inferred; no args needed):

| Command | What it does |
|---------|--------------|
| `Setup-Plugin.bat` | **No-build refresh + configure.** Pulls the latest plugin from GitHub (ZIP, no git), redeploys it, writes the MCP client config, deploys the Claude/AGENTS assets, runs `doctor`. Self-updating. |
| `Scripts\Install-Plugin.bat` | **Full install incl. build.** Everything `Setup-Plugin` does, plus builds the MCP server (UBT on a source engine, CMake fallback on an installed one). |
| `Scripts\Build-MCPServer.bat` | Build just the MCP server exe. |
| `Scripts\Generate-ClientConfig.bat` | (Re)write the MCP client config only. |
| `Scripts\Install-ClaudeAssets.bat` | (Re)deploy + reconcile the Claude/AGENTS assets only. |
| `Scripts\Patch-Engine.bat` | Apply the source-engine `.Build.cs` patches (source engines only). |
| `Scripts\Verify-Build.bat` | Check both halves of the plugin are present; print fix steps. |

All `.bat` wrappers run with `-ExecutionPolicy Bypass` and require PowerShell 7
(`pwsh`). The MCP server exe is built into `Binaries/Win64/` here, so it ships
with the plugin.

## What gets deployed into the project

`Setup-Plugin` / `Install-Plugin` reconcile these into `<YourProject>`:

- `.claude/skills/bp-*`, `.claude/agents/bp-audit.md` <- from `Claude/` (Claude Code)
- `AGENTS.md`, `.github/copilot-instructions.md` <- from `AGENTS.md` (Codex / Cursor / Copilot / Gemini)
- `.mcp.json` (or the per-client config) pointing at the server exe

Reconcile is bounded to the `bp-*` namespace and never touches consumer-authored
`.claude/` content; assets removed upstream are pruned on the next run.

## Full documentation

Build / test / maintain notes, the generated tool catalog, per-tool I/O shapes,
the BPIR schema, and configuration live in the repository and wiki:
<https://github.com/defessler/Unreal-Engine-5-MCP>
