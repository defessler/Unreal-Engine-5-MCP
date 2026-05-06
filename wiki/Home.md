# UE5_MCP Wiki

A standalone MCP server + UE 5.7 plugin that lets Claude (or any MCP
client) read and edit Blueprint assets — variables, graphs, nodes,
connections, K2 metadata — through 21 tools.

```
┌─────────────────┐  JSON-RPC/stdio  ┌─────────────────┐  CreateProcessW  ┌──────────────────┐
│  Claude / any   │ ──────────────►  │  bp-reader-mcp  │ ───────────────► │ UnrealEditor-Cmd │
│   MCP client    │ ◄──────────────  │      .exe       │ ◄─────────────── │  + plugin DLL    │
└─────────────────┘                  └─────────────────┘  result JSON     └──────────────────┘
```

## Pages

- **[Installation](Installation)** — clone, build the engine + plugin.
- **[Clients](Clients)** — how to start the MCP server and wire it
  into Claude Code, Claude Desktop, GitHub Copilot (VS Code), and
  ChatGPT (with the bridge required for ChatGPT).
- **[Usage](Usage)** — what your AI of choice can do once it's wired up.
- **[Tool Reference](Tool-Reference)** — every tool, its inputs, what it returns.
- **[Configuration](Configuration)** — env vars, daemon mode, timeouts.
- **[Troubleshooting](Troubleshooting)** — common failure modes + fixes.

## TL;DR

If you just want Claude to read your Blueprints:

1. [Set up + build the engine](Installation#2-build-the-engine) — 1–3 hours
   first time (UE source build).
2. [Build the editor target](Installation#3-build-the-ue-plugin) — the
   plugin's `PreBuildStep` builds the MCP server too, no separate cmake.
3. Launch your client from the project directory — the shipped
   [`.mcp.json`](Clients#claude-code-recommended) wires Claude Code
   automatically. For Copilot, drop a `.vscode/mcp.json`. For ChatGPT,
   bridge via HTTPS — see [Clients](Clients).
4. Ask the AI: *"What variables are on `/Game/AI/BP_TestEnemy`?"*

Want to skip UE and just kick the tires? [Build the MCP server
standalone](Installation#1-build-the-mcp-server) and use the mock backend
(3 fixture BPs, 5 min, no UE needed).

## Backends

| Backend       | Needs UE?    | Use case                                |
|---------------|--------------|------------------------------------------|
| `mock`        | No           | MCP server dev, smoke tests, demos.     |
| `commandlet`  | Yes (built)  | Reading + editing real `.uasset` files. |

Default is `mock`. Set `BP_READER_BACKEND=commandlet` plus engine/project
paths to drive a real editor. See [Configuration](Configuration).

## Performance

| Mode                                 | Cold call         | Subsequent calls   |
|--------------------------------------|-------------------|--------------------|
| `commandlet` one-shot                | 5–7 s             | 5–7 s each         |
| `commandlet` daemon (default)        | 5 s               | **15–30 ms each**  |
| `commandlet` daemon + `PREWARM=1`    | **~30 ms** *      | **15–30 ms each**  |
| `mock`                               | <5 ms             | <5 ms              |

\* Pre-warm spawns the editor on MCP startup in a background thread. If
your first tool call lands before the daemon hits READY (~5–30 s
depending on DDC warmth), it blocks on the same mutex and inherits the
warm daemon — no double-spawn. See [Configuration](Configuration#pre-warm).

Daemon mode keeps one `UnrealEditor-Cmd.exe` alive across calls and pipes
commandlet-arg lines to its stdin. Falls back to one-shot automatically if
the child crashes.

## Where to file issues

[github.com/defessler/Unreal-Engine-5-MCP/issues](https://github.com/defessler/Unreal-Engine-5-MCP/issues)
