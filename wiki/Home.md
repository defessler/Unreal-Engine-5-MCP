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

- **[Installation](Installation)** — clone, build, hook into Claude.
- **[Usage](Usage)** — what Claude can do once it's wired up.
- **[Tool Reference](Tool-Reference)** — every tool, its inputs, what it returns.
- **[Configuration](Configuration)** — env vars, daemon mode, timeouts.
- **[Troubleshooting](Troubleshooting)** — common failure modes + fixes.

## TL;DR

If you just want Claude to read your Blueprints:

1. [Build the MCP server](Installation#1-build-the-mcp-server) (5 min, no UE
   needed — the `mock` backend uses bundled fixtures).
2. [Build the UE plugin](Installation#3-build-the-ue-plugin) into your project
   (1–3 hours first time — engine source build).
3. [Add `bp-reader` to your Claude MCP config](Installation#4-wire-it-into-claude).
4. Ask Claude: *"What variables are on `/Game/AI/BP_TestEnemy`?"*

## Backends

| Backend       | Needs UE?    | Use case                                |
|---------------|--------------|------------------------------------------|
| `mock`        | No           | MCP server dev, smoke tests, demos.     |
| `commandlet`  | Yes (built)  | Reading + editing real `.uasset` files. |

Default is `mock`. Set `BP_READER_BACKEND=commandlet` plus engine/project
paths to drive a real editor. See [Configuration](Configuration).

## Performance

| Mode                          | Cold call | Subsequent calls   |
|-------------------------------|-----------|--------------------|
| `commandlet` one-shot         | 5–7 s     | 5–7 s each         |
| `commandlet` daemon (default) | 5 s       | **15–30 ms each**  |
| `mock`                        | <5 ms     | <5 ms              |

Daemon mode keeps one `UnrealEditor-Cmd.exe` alive across calls and pipes
commandlet-arg lines to its stdin. Falls back to one-shot automatically if
the child crashes.

## Where to file issues

[github.com/defessler/Unreal-Engine-5-MCP/issues](https://github.com/defessler/Unreal-Engine-5-MCP/issues)
