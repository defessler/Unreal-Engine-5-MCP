# UE5_MCP Wiki

> **Status update — Lyra integration complete.** The repo root ships
> as a Lyra Starter Game project (`LyraStarterGame.uproject`) on
> UE 5.7.4. **All 302 Lyra blueprints have C++ companion classes**
> under `Plugins/LyraGenerated/Source/LyraGenerated/Private/Generated/`
> (17 full transpiles + 285 stubs); `LyraEditor` builds clean and
> `LyraEditor-Cmd.exe` commandlet runs successfully. The plugin is
> project-agnostic; set `BP_READER_PROJECT` and `BP_READER_EDITOR_TARGET`
> when invoking against any project (Lyra = `LyraEditor`). The
> conversion recipe + lessons-learned live in
> [`docs/research/lyra-bp-to-cpp-conversion.md`](https://github.com/defessler/Unreal-Engine-5-MCP/blob/main/docs/research/lyra-bp-to-cpp-conversion.md).


A standalone MCP server + UE 5.7 plugin that lets Claude (or any MCP
client) read, edit, **and round-trip BPs to/from C++** — variables,
graphs, nodes, connections, K2 metadata — through 249 tools.

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
- **[Architecture](Architecture)** — UML of the whole system: server core,
  the `IBlueprintReader` backend decorator stack, the BPIR pivot, both UE
  plugin modules, and end-to-end call + event sequence diagrams.
- **[Tool Reference](Tool-Reference)** — every tool, its inputs, what it returns.
- **[BPIR](BPIR)** — the Blueprint Intermediate Representation schema
  that powers `decompile_function` / `transpile_function` /
  `parse_cpp_function`. Versioned JSON AST; pivot for BP ↔ C++ (and
  future Lua / Python / JS).
- **[Configuration](Configuration)** — env vars, daemon mode, timeouts.
- **[Coding-Conventions](Coding-Conventions)** — what hand-written + generated code in this repo looks like.
- **[Troubleshooting](Troubleshooting)** — common failure modes + fixes.

## TL;DR

If you just want Claude to read your Blueprints:

1. [Set up + build the engine](Installation#2-build-the-engine) — 1–3 hours
   first time (UE source build).
2. [Build the editor target + MCP server](Installation#3-build-the-ue-plugin)
   — both halves are UBT targets in the same plugin; the
   `Build-MCPServer.ps1` wrapper builds them in one shot.
3. Launch your client from the project directory — the shipped
   [`.mcp.json`](Clients#claude-code-recommended) wires Claude Code
   automatically. For Copilot, drop a `.vscode/mcp.json`. For ChatGPT,
   bridge via HTTPS — see [Clients](Clients).
4. Ask the AI: *"What variables are on `/Game/AI/BP_TestEnemy`?"*

Want to skip UE and just kick the tires? [Build the MCP server
standalone](Installation#1-build-the-mcp-server) and use the mock backend
(3 fixture BPs, 5 min, no UE needed).

## BP ↔ C++

A versioned JSON AST ([BPIR](BPIR)) sits between BPs and source
languages. C++ ships today; Lua / Python / JS drop in as additional
codegen + parser pairs without touching the IR.

- **BP → C++** — `transpile_function` / `transpile_blueprint`
  (UCLASS scaffolding + UPROPERTY/UFUNCTION decoration +
  `GetLifetimeReplicatedProps`); pair with `write_generated_source` to
  drop `.h`/`.cpp` into `<Project>/Source/<Module>/Generated/`.
- **C++ → BP** — `parse_cpp_function` produces BPIR; pipe through
  `compile_function` to materialize the BP graph.

Round-trip identity (BPIR → C++ → BPIR) is pinned by tests for the
patterns the codegen emits. See
[Tool Reference → Transpile tools](Tool-Reference#transpile-tools).

## Backends

| Backend       | Needs UE?    | Use case                                |
|---------------|--------------|------------------------------------------|
| `mock`        | No           | MCP server dev, smoke tests, demos.     |
| `commandlet`  | Yes (built)  | Reading + editing `.uasset` files with the editor **closed**. Spawns a `UnrealEditor-Cmd.exe` child process. |
| `live`        | Yes, running | Reading + editing with the editor **open**. Connects to a TCP listener inside the running editor; no concurrent .uasset writes, sees unsaved edits. |
| `auto`        | Either       | Probes per call. Routes to `live` when the editor is up, `commandlet` when it isn't. **This is the default** when a `.uproject` is auto-discovered. |

Default is `auto` (or `mock` if no `.uproject` is auto-discovered).
Set `BP_READER_BACKEND=<name>` to opt out of probing. See
[Configuration](Configuration).

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
