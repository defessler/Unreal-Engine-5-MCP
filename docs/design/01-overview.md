# 01 — Overview

bp-reader is a Blueprint introspection and mutation surface for UE 5.7,
exposed to MCP-aware coding agents (Claude Code, Claude Desktop, VS Code
Copilot, ChatGPT bridges) as a JSON-RPC 2.0 stdio server. This document
sketches what's in the repo and how the parts fit. The other three
design docs zoom in:

- [02-architecture.md](02-architecture.md) — component diagram, process
  model, request lifecycle.
- [03-plugin-internals.md](03-plugin-internals.md) — the UE editor side
  (commandlet dispatch, live TCP server, introspector).
- [04-mcp-server.md](04-mcp-server.md) — the standalone C++20 MCP
  server (JSON-RPC, registry, telemetry).

## Two halves of one shipping unit

```
Plugins/BlueprintReader/
├── BlueprintReader.uplugin
├── Source/
│   ├── BlueprintReaderEditor/           UE 5.7 editor plugin (UnrealEd)
│   └── BlueprintReaderRuntime/          opt-in cooked-runtime introspection
└── Tests/                               UE Program targets (UBT-built)
    ├── BlueprintReaderMcp/              main exe → BlueprintReaderMcp.exe
    ├── BlueprintReaderMcpCore/          shared static-lib module (impl)
    ├── BlueprintReaderMcpTests/         doctest suite → BlueprintReaderMcpTests.exe
    └── ThirdParty/                      vendored: nlohmann_json, fmt, doctest
```

Both halves ship together as one UE plugin. The MCP server lives
under `Tests/` (only path UBT auto-scans for plugin Program
`Target.cs` files) and builds with the rest of the plugin via the
same UBT pipeline — no separate CMake step. The plugin's `.uplugin`
declares a `PreBuildSteps` hook that invokes UBT for
`BlueprintReaderMcp` before any consuming target's own build runs, so
`Build.bat LyraEditor …` builds the editor module *and* the MCP
server exe in one invocation. `Build.bat BlueprintReaderMcp …` builds
the server in isolation when iterating on server-only changes; the
convenience wrapper `Plugins/BlueprintReader/Scripts/Build-MCPServer.ps1`
covers both Program targets (server + doctest exe) in one shot.

### Editor plugin — `BlueprintReaderEditor`

UnrealEd-only module. Hosts the entry points that actually touch
`UBlueprint` objects:

- `UBPRCommandlet` (`-run=BPR`) — the workhorse.
  Dispatches every read and write op via a single `RunOneOp(Params)`
  function. Long-lived `-Daemon` mode reads newline-delimited args from
  stdin so one `UnrealEditor-Cmd.exe` process serves many tool calls
  cheaply.
- `BlueprintReaderLiveServer` — TCP listener that lets the MCP server
  talk to a running editor without spawning a second commandlet daemon.
  Auto-publishes port + token via `<Project>/Saved/bp-reader-live.json`.
- `UBPRSeedCommandlet` (`-run=BPRSeed`) —
  synthesizes `Content/AI/BP_TestEnemy.uasset` and
  `BP_TestPickup.uasset` for live integration tests.

### Runtime plugin — `BlueprintReaderRuntime`

Optional, packaged-build-safe sibling. Reads what UE preserves through
cook (UClass reflection, CDO defaults, UFunction signatures, asset
registry) and exposes the same TCP protocol as the editor live server.
Off by default; opt-in via the `bp.reader.listen` CVar. See
`Source/BlueprintReaderRuntime/BlueprintReaderRuntime.Build.cs`.

### Standalone MCP server (`Plugins/BlueprintReader/Tests/`)

A C++20 stdio MCP server. JSON-RPC 2.0 frames in (newline-delimited or
LSP-style `Content-Length`), tool result envelopes out. No UE
dependency in this tree — it links against vendored
`Tests/ThirdParty/nlohmann_json`, `Tests/ThirdParty/fmt`, and
`Tests/ThirdParty/doctest` (referenced via `PrivateIncludePaths` in
`BlueprintReaderMcpCore.Build.cs`). The tool handlers call into a
`IBlueprintReader` interface; the concrete reader is chosen at
startup based on environment.

## The four backends

The MCP server abstracts "how do I actually read or mutate this
blueprint?" behind an `IBlueprintReader` interface
(`src/backends/IBlueprintReader.h`). Four concrete implementations:

| Backend | When | How |
|---------|------|-----|
| `mock` | no UE setup needed (CI, server development) | reads canned JSON from `Plugins/BlueprintReader/Tests/BlueprintReaderMcpTests/fixtures/` |
| `commandlet` | editor closed | spawns `UnrealEditor-Cmd.exe -run=BPR -Daemon`, talks to it over stdin/stdout |
| `live` | editor open | connects to the editor's TCP listener (port + token from handshake file) |
| `auto` | default when a `.uproject` is auto-discovered | probes per call: live if the editor's listener accepts, commandlet otherwise |

Auto's per-call probe (with a 2s cache) is what makes the "open the
editor in the middle of a session" workflow work without restarting
the MCP server. See `src/backends/AutoBlueprintReader.cpp`. Backend
selection logic lives in
`src/backends/BackendFactory.cpp:196-266` — the same code also wraps
the chosen reader with a TTL-keyed cache
(`CachingBlueprintReader`) and an optional read-only guard
(`ReadOnlyBlueprintReader`).

## 126 tools across 22 categories

The MCP server registers 126 tools at startup. The tests pin the
expected count
(`tests/test_tools.cpp:35`, `tests/test_mcp.cpp:90`). The categories,
from the test breakdown:

> 12 read + 22 write + 3 meta + 3 batch + 3 transpile + 13 project/asset
> + 15 live editor + 1 automation + 7 material + 5 widget + 5 BT
> + 4 DataAsset + 5 StateTree + 4 profile + 2 cook + 3 class info
> + 4 viewport + 4 Niagara + 4 Sequencer + 3 GAS + 4 AnimGraph

Tool names map 1:1 to `EOp` enum entries in
`BlueprintReaderCommandlet.cpp` lines 115-244. The MCP server's tool
descriptors live in `Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/tools/BlueprintTools.cpp`
(one `ToolDescriptor` block per tool, ~123 of them) with the rest in
`ApplyOps.cpp` (`apply_ops`, `preview_ops`) and
`CompileFunction.cpp` (`compile_function`).

Don't enumerate them here — call `tools/list` against the running
server, or read `wiki/Tool-Reference.md`. Adding a new tool means
plumbing through both halves; the checklist is in
[CLAUDE.md → "Adding a new tool"](../../CLAUDE.md).

## Wire format

JSON keys are snake_case throughout. `BPNode.meta` is a real nested
object (not a string-of-JSON). Empty optional strings serialize as
JSON `null`, not `""`. The canonical types live in
`Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/BlueprintReaderTypes.h` — previously the same header had a `#ifdef WITH_UE` block with
USTRUCT mirrors for UE-side JSON conversion; that path was removed
in the PR #75 UBT migration (UHT rejected USTRUCT inside conditional
blocks and the WITH_UE branch was never actually consumed). See
[02-architecture.md → "Key invariants"](02-architecture.md#key-invariants)
for why this matters and where each invariant is enforced.

Asset paths on the wire are always **package paths**
(`/Game/AI/BP_Enemy`), never UE's internal object paths
(`/Game/AI/BP_Enemy.BP_Enemy`). The plugin's
`FBlueprintReaderWireJson::ToPackagePath` strips the trailing object suffix consistently
(see `BlueprintReaderWireJson.cpp`).

## Where the docs live

| Location | Purpose | Audience |
|----------|---------|----------|
| [`wiki/`](../../wiki) | source-of-truth for the GitHub Wiki; manually pushed to `<repo>.wiki.git` | end users (setup, tool reference, troubleshooting) |
| [`docs/design/`](.) | technical design documents (you're reading one) | engineers reading or extending the codebase |
| [`docs/tutorial/`](../tutorial) | step-by-step build-from-scratch | engineers learning how the system is constructed |
| [`.claude/skills/bp-reader/`](../../.claude/skills/bp-reader) | Claude skill manifests describing *how to use* the tool surface | Claude agents (and humans reading them) |
| [`CLAUDE.md`](../../CLAUDE.md) | maintainer cheat sheet (build flags, gotchas, "adding a new tool") | anyone modifying the project |
| [`README.md`](../../README.md) | user-facing setup, MCP client config snippets, tool table | end users picking up the project |

## See also

- [02-architecture.md](02-architecture.md) — how the pieces talk.
- [03-plugin-internals.md](03-plugin-internals.md) — UE-side guts.
- [04-mcp-server.md](04-mcp-server.md) — server-side guts.
- [CLAUDE.md](../../CLAUDE.md) — maintainer cheat sheet (the build
  invariants and "common gotchas" are mandatory reading).
- [README.md](../../README.md) — user-facing setup + tool table.
