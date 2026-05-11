# UE5_MCP — Blueprint Reader MCP

A standalone MCP server that lets Claude (or any MCP client) read **and edit**
Unreal Engine 5 Blueprint assets — variables, graphs, nodes, connections, K2
metadata — and **round-trip BPs to/from C++** via 61 tools backed by the
bundled `BlueprintReader` UE plugin.

```
┌─────────────────┐  JSON-RPC/stdio  ┌─────────────────┐  CreateProcessW  ┌──────────────────┐
│  Claude / any   │ ──────────────► │  bp-reader-mcp  │ ───────────────► │ UnrealEditor-Cmd │
│   MCP client    │ ◄────────────── │      .exe       │ ◄─────────────── │  + plugin DLL    │
└─────────────────┘                 └─────────────────┘  result JSON     └──────────────────┘
```

Two backends:
- **`mock`** — fixture-backed; no UE required. Used for unit tests and
  bring-up. Three handcrafted JSON fixtures under `Plugins/BlueprintReader/mcp-server/fixtures/`.
- **`commandlet`** — drives a real `UnrealEditor-Cmd.exe -run=BlueprintReader`
  to read live `.uasset` files from a UE project. Defaults to **daemon mode**
  (one editor process reused across calls) so per-call cost is ~30 ms after
  the initial ~5 s editor cold start.

## Tools

61 tools across 8 categories — see the
[Tool Reference](https://github.com/defessler/Unreal-Engine-5-MCP/wiki/Tool-Reference)
for every input/output shape with examples.

| Category | Tools | What they do |
|----------|-------|--------------|
| **Read** (10) | `list_blueprints`, `summarize_blueprint`, `read_blueprint`, `get_graph`, `get_function`, `list_variables`, `get_components`, `find_node`, `get_node`, `find_overriders` | Inventory + structural queries. `summarize_blueprint` is the cheap orientation tool; `find_overriders` does cross-BP queries in one call. |
| **Write** (18) | `add_variable` / `retype_variable` / `rename_variable` / etc.; `add_function` / `add_function_input/output`; `add_node` / `wire_pins` / `set_pin_default` / `auto_layout_graph`; `create_blueprint` / `duplicate_blueprint` | Single-step mutations. All idempotent where idempotency makes sense; `wire_pins` errors include both pin types so the agent can self-correct. |
| **Batch / generation** (3) | `apply_ops`, `preview_ops`, `compile_function` | Multi-step writes: named-slot GUID resolution, dry-run, pseudocode → BP graph. Collapse N×compile to 1. |
| **Transpile (BP↔C++)** (6) | `decompile_function`, `decompile_blueprint`, `transpile_function`, `transpile_blueprint`, `write_generated_source`, `parse_cpp_function` | Round-trip BPs to and from C++ via the BPIR JSON AST. See [BP↔C++ round-trip](#bp--c-round-trip) below. |
| **Project + Content Browser** (9) | `get_project_metadata`, `save_all`, `move_asset`, `delete_asset`, `create_folder`, `list_data_tables`, `read_data_table`, `add_data_row`, `set_data_row_value` | Project-level introspection + asset-browser ops complementing the per-Blueprint surface. |
| **Live editor** (12) | `console_command`, `get_cvar`, `set_cvar`, `pie_start`, `pie_stop`, `live_coding_compile`, `get_selected_actors`, `set_selection`, `spawn_actor`, `set_actor_transform`, `delete_actor`, `read_output_log` | Operate on the running editor's in-memory state. Work best with the `live` backend (open editor); commandlet daemon routes them too. |
| **Automation** (1) | `run_automation_tests` | Kick off UE's automation test framework with a wildcard pattern; results land in the output log + `Saved/Automation/`. |
| **Discoverability + meta** (3) | `list_node_kinds`, `list_pin_categories`, `shutdown_daemon` | Self-describing surface so the agent can ask "what's a valid `add_node` kind?" or "what does a struct-ref BPPinType look like?" without scanning docs. |

Wire shapes are pinned in `Plugins/BlueprintReader/mcp-server/src/BlueprintReaderTypes.h`. Snake_case
keys, nullable string fields emit `null`, `BPNode.meta` is a real nested object.

Every read tool also accepts `fields` (response projection — drop keys you
don't need), plus `limit` / `offset` for tools that return arrays. AI clients
pay tokens for every byte of a tool response, so projecting and paginating
matters. See [Tool Reference](https://github.com/defessler/Unreal-Engine-5-MCP/wiki/Tool-Reference#response-controls-read-tools)
for examples.

Every write tool that takes a `type` accepts a string shorthand —
`"float"`, `"object:Actor"`, `"struct:FVector"`, `"[]float"`,
`"{string:int}"` — alongside the canonical BPPinType object form.
`add_variable` and `add_function` are idempotent (return
`already_existed:true` instead of erroring on duplicates), and `wire_pins`
errors include both pin types so the agent can self-correct.

For multi-step generation, `apply_ops` runs a whole batch in one call with
named-slot GUID resolution **and a single recompile + save at the batch
end** (collapses N×compile to 1). `compile_function` accepts a pseudocode
DSL (`if`/`set`/`call`/`var`/`lit` plus math+comparison operator aliases)
and materializes the graph. `preview_ops` validates a batch without
mutating, and `create_blueprint` generates whole new BP assets from
scratch. See [Tool Reference → Batch tools](https://github.com/defessler/Unreal-Engine-5-MCP/wiki/Tool-Reference#batch-tools).

## BP ↔ C++ round-trip

A versioned **Blueprint Intermediate Representation (BPIR)** sits between
the BP graph and any source language. C++ ships today; Lua / Python / JS
drop in as additional codegen + parser pairs without touching the IR.

```
                        ┌──────────────┐
       BP graph  ⇄     │     BPIR     │   ⇄  C++ source
                        └──────────────┘
                              ▲
                              │
              compile_function (BPIR → BP, already shipping)
```

- **BP → C++** — `transpile_function` (single function) or
  `transpile_blueprint` (whole class with UCLASS scaffolding +
  UPROPERTY/UFUNCTION decoration + `GetLifetimeReplicatedProps`). Pair
  with `write_generated_source` to drop `.h`/`.cpp` into
  `<Project>/Source/<Module>/Generated/`.
- **C++ → BP** — `parse_cpp_function` produces BPIR; pipe through
  `compile_function` to materialize the BP graph.
- **Round-trip identity** — for the patterns `transpile_function` emits,
  `parse_cpp_function` recovers an equivalent BPIR (pinned by 12
  round-trip test cases).

Subset accepted by the parser: if/else, range-based for, while, switch,
return, break, continue, calls, member access, `Cast<T>()`, unary +
binary operators with full C++ precedence. Anything outside the subset
(timelines, latent actions, anim graphs, raw pointer arithmetic) is
flagged in a sidecar JSON for manual port. See
[Tool Reference → Transpile tools](https://github.com/defessler/Unreal-Engine-5-MCP/wiki/Tool-Reference#transpile-tools)
and [BPIR schema](https://github.com/defessler/Unreal-Engine-5-MCP/wiki/BPIR).

## Quick start: hooking it up to Claude

### 1. Build the MCP server (no UE required for the mock backend)

```pwsh
cd Plugins\BlueprintReader\mcp-server
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
build\tests\Release\bp-reader-tests.exe   # 344 cases (live skip without UE)
```

The exe is at `Plugins/BlueprintReader/mcp-server/build/Release/bp-reader-mcp.exe`.

Third-party deps (nlohmann_json, fmt, doctest) are vendored under
`Plugins/BlueprintReader/mcp-server/third_party/`, so this works **with no network access and no
git** — CMake itself is the only external tool required.

> **If you'll use the `commandlet` backend, you can skip this step** —
> building the editor target also builds the MCP server via the plugin's
> `PreBuildSteps`. See [Plugin-driven build](#plugin-driven-build) below.

### 2. Wire it into your MCP client

The repo ships a project-scope **`.mcp.json`** at the root, so cloning +
launching Claude Code from the project directory wires bp-reader
automatically. The contents:

```json
{
  "mcpServers": {
    "bp-reader": {
      "command": "D:\\Projects\\UE5_MCP\\Plugins\\BlueprintReader\\mcp-server\\build\\Release\\bp-reader-mcp.exe",
      "env": {
        "BP_READER_BACKEND":  "commandlet",
        "BP_READER_ENGINE_DIR": "D:\\Projects\\Unreal Engine 5",
        "BP_READER_PROJECT":  "D:\\Projects\\UE5_MCP\\UE5_MCP.uproject",
        "BP_READER_PREWARM":  "1"
      }
    }
  }
}
```

Adjust paths if your layout differs. With `BP_READER_PREWARM=1` the editor
daemon spawns in a background thread on MCP startup, so the first BP
question lands in ~30 ms instead of paying the ~5–30 s editor cold start.

For other configs:
- **User-scope** (any directory): `claude mcp add bp-reader --scope user ...`
  — see Wiki *Configuration*.
- **Claude Desktop** — same JSON shape under
  `%APPDATA%\Claude\claude_desktop_config.json`'s `mcpServers`.
- **Mock-only** (no UE) — drop the entire `env` block; the server defaults
  to the bundled fixtures and exposes the read tools as a demo.

### 3. Try it

In Claude, reference your blueprint by content path:
> "What variables are on `/Game/AI/BP_TestEnemy`? Find any K2Node_VariableGet
> nodes."

Claude calls `read_blueprint` → `find_node`, gets back canonical JSON.

## Configuration (env vars)

| Variable                      | Default                                | Purpose                                                                  |
|-------------------------------|----------------------------------------|--------------------------------------------------------------------------|
| `BP_READER_BACKEND`           | `auto` (when uproject found), else `mock` | `mock` \| `commandlet` \| `live` \| `auto`. Auto probes the editor's handshake file each call and routes to live (editor open) or commandlet (editor closed) — no manual flipping when the editor opens / closes mid-session. See [Configuration → Live backend](https://github.com/defessler/Unreal-Engine-5-MCP/wiki/Configuration#live-backend--talk-to-a-running-editor-over-tcp). |
| `BP_READER_FIXTURES_DIR`      | `<exe>/fixtures`                       | Mock backend's fixture dir.                                              |
| `BP_READER_ENGINE_DIR`        | (unset → fail-fast for `commandlet`)   | Path to the source-built engine (`...\UnrealEngine`).                    |
| `BP_READER_PROJECT`           | (unset → fail-fast for `commandlet`)   | Path to the `.uproject`.                                                 |
| `BP_READER_TIMEOUT_SECONDS`   | `120`                                  | Per-tool-call subprocess timeout (once the daemon is hot).               |
| `BP_READER_STARTUP_TIMEOUT_SECONDS` | `600`                            | How long to wait for the editor daemon's first READY signal. Big UE projects (lots of plugins, large content, cold DDC) can take 5–10 min the first time — bump this if you see "daemon timed out reaching READY". |
| `BP_READER_EDITOR_ARGS`       | (empty)                                | Whitespace-separated args appended to `UnrealEditor-Cmd.exe`'s command line. Set to `-EnableAllPlugins` if your `.uproject` enables binary marketplace plugins (DLSS, Wwise, etc.) whose modules aren't built in your engine — that switch makes plugin-module load failures non-fatal so the editor starts up. UE's `-DisablePlugin=` does *not* override `.uproject`-enabled plugins. |
| `BP_READER_EDITOR_CONFIG`     | (empty → `Development`)                | Picks `UnrealEditor-Cmd[-Win64-Config].exe`. Set to `DebugGame` / `Debug` / `Test` / `Shipping` if your editor target was built in that config — UE only loads plugin DLLs whose suffix matches the editor exe. |
| `BP_READER_DAEMON`            | `1` (on)                               | `1`/`true`/`yes`/`on` to enable. Set `0` to fall back to one-shot mode.  |
| `BP_READER_PREWARM`           | `0` (off)                              | `1`/`true`/`yes`/`on` to spawn the editor daemon on MCP startup in a background thread, hiding the cold-start cost behind whatever Claude is doing. |
| `BP_READER_CACHE_TTL_SECONDS` | `30`                                   | How long the server memoizes read-tool responses for. AI clients flurry repeated reads on the same BP — caching short-circuits the duplicates. `0` disables. Writes invalidate the affected asset's entries. |
| `BP_READER_READ_ONLY`         | `0` (off)                              | `1`/`true`/`yes`/`on` to reject every write tool. Use when running the daemon alongside an open editor — concurrent writes to the same `.uasset` corrupt state. |
| `BP_READER_LIVE_PORT`         | auto (kernel-picked ephemeral)         | TCP port for the `live` backend. **Plugin defaults to picking an ephemeral port and publishing it via `<Project>/Saved/bp-reader-live.json`**, which the MCP server auto-discovers — no manual env-var plumbing in the typical case. Set explicitly in BOTH processes only if you need a fixed port. |
| `BP_READER_LIVE_TOKEN`        | auto (random GUID per editor session)  | Shared secret for live-backend auth. **Plugin defaults to a random 256-bit token written to the handshake file**; MCP server reads it from there. Set explicitly only when the env-var route is preferred (e.g. CI). |
| `BP_READER_LIVE_DISABLED`     | `0` (off)                              | `1` to skip starting the editor's TCP listener entirely (opt-out for users who want the daemon-only flow). |

## Performance

Per-call wall clock on a Dev box (no shader compile, with NTFS journal cache):

| Mode                        | Cold call | Subsequent calls |
|-----------------------------|-----------|------------------|
| `commandlet` one-shot       | 5–7 s     | 5–7 s each       |
| `commandlet` daemon (default)| 5 s      | **15–30 ms each**|
| `mock`                      | <5 ms     | <5 ms            |

Daemon mode keeps one `UnrealEditor-Cmd.exe` alive and pipes commandlet-arg
lines to its stdin; the plugin emits a sentinel after each command so the
backend knows the result file is ready. If the daemon transport fails for any
reason (child crashes, marker doesn't appear), the backend falls back to a
fresh one-shot for that call rather than failing the user-visible op.

## Project layout

The plugin is fully self-contained — drop `Plugins\BlueprintReader\` into
any UE project's `Plugins\` folder and it builds (UE plugin module + MCP
server) as one unit.

```
UE5_MCP\
├── UE5_MCP.uproject
├── Source\                                project runtime module
├── Plugins\BlueprintReader\               plugin (everything ships together)
│   ├── BlueprintReader.uplugin            PreBuildSteps run Build-MCPServer.ps1
│   ├── Scripts\Build-MCPServer.ps1        plugin-driven cmake build
│   ├── Source\BlueprintReaderEditor\
│   │   ├── BlueprintIntrospector         FBlueprintInfo from FBlueprintGeneratedClass
│   │   ├── BlueprintReaderJson           legacy/rich plugin shape (camelCase, K2 extras)
│   │   ├── BlueprintReaderWireJson       canonical MCP wire shape (snake_case)
│   │   ├── BlueprintReaderCommandlet     -run=BlueprintReader; -Op + -Daemon dispatch
│   │   └── BlueprintReaderSeedCmdlet     -run=BlueprintReaderSeed; synthesize test BPs
│   └── mcp-server\                        standalone C++20 MCP server
│       ├── src\
│       │   ├── BlueprintReaderTypes.h    POD/USTRUCT dual-mode wire types
│       │   ├── jsonrpc\                  Server, Mcp (initialize/tools/call/...)
│       │   ├── tools\                     ToolRegistry, BlueprintTools, Bpir, Decompile
│       │   │   ├── codegen\               CppEmit, CppClassEmit, UnsupportedTreatment
│       │   │   └── parse\                 CppLex, CppParse (C++ → BPIR)
│       │   └── backends\                  IBlueprintReader, MockReader, CommandletReader
│       ├── tests\                         doctest cases (mock + live commandlet, 344 total)
│       ├── scripts\
│       │   ├── roundtrip.ps1              JSON-RPC handshake + read smoke
│       │   ├── smoke-batch-ops.ps1        apply_ops / preview_ops smoke
│       │   ├── smoke-decompile.ps1        BP → BPIR → readable C++ smoke
│       │   ├── smoke-transpile-cpp.ps1    BP → compilable .h/.cpp (-RunUbt)
│       │   └── smoke-cpp-roundtrip.ps1    C++ → BPIR → compile_function → BP
│       ├── fixtures\                      BP_Enemy / BP_Pickup / BP_PlayerController
│       ├── third_party\                   vendored deps: nlohmann_json, fmt, doctest
│       ├── CMakeLists.txt
│       └── vcpkg.json                     (declared but not consumed by default)
├── Content\AI\                            BP_TestEnemy.uasset, BP_TestPickup.uasset
                                           (engine source lives outside this repo
                                            at D:\Projects\Unreal Engine 5\)
└── README.md
```

## Subcommands

```pwsh
bp-reader-mcp.exe doctor                    # check setup, exit non-zero if broken
bp-reader-mcp.exe config                    # print .mcp.json snippet (Claude Code)
bp-reader-mcp.exe config --client=copilot   # same, for VS Code Copilot
bp-reader-mcp.exe config --client=claude-desktop
bp-reader-mcp.exe --help
```

`doctor` walks the install — engine path, .uproject location + plugin
entry, editor exe, plugin DLL config-suffix match — and prints actionable
fix hints with build commands. Replaces the `Verify-Build.bat` flow.

`config` auto-discovers the engine + project paths and outputs a
ready-to-paste config block for the chosen client.

## Manual launch

To run the server outside Claude — debugging, scripted JSON-RPC, smoke
tests — use the bundled launcher. It auto-loads env from `.mcp.json`:

```pwsh
pwsh -File Plugins\BlueprintReader\Scripts\Start-MCPServer.ps1
```

Or, for a double-click-friendly wrapper:

```
Plugins\BlueprintReader\Scripts\Start-MCPServer.bat
```

Both pass through `-Backend`, `-Prewarm`, `-EngineDir`, `-UProject`.
Server runs in foreground, stderr to console, stdin reads JSON-RPC
frames.

A parallel pair (`Build-MCPServer.{ps1,bat}`) runs the same logic UBT
uses as a PreBuildStep — useful for building the server standalone.

## Live verification

If you have the editor built and the test BPs seeded:

```pwsh
$env:BP_READER_BACKEND     = "commandlet"
$env:BP_READER_ENGINE_DIR  = "D:\Projects\Unreal Engine 5"
$env:BP_READER_PROJECT     = "D:\Projects\UE5_MCP\UE5_MCP.uproject"

Plugins\BlueprintReader\mcp-server\build\tests\Release\bp-reader-tests.exe   # all 344 cases
pwsh -File Plugins\BlueprintReader\mcp-server\scripts\roundtrip.ps1 `
    -Exe Plugins\BlueprintReader\mcp-server\build\Release\bp-reader-mcp.exe `
    -Asset /Game/AI/BP_TestEnemy
```

End-to-end smoke for the BP↔C++ pipeline (Phases 1–3):

```pwsh
$exe = "Plugins\BlueprintReader\mcp-server\build\Release\bp-reader-mcp.exe"
pwsh -File Plugins\BlueprintReader\mcp-server\scripts\smoke-decompile.ps1     -Exe $exe
pwsh -File Plugins\BlueprintReader\mcp-server\scripts\smoke-transpile-cpp.ps1 -Exe $exe
pwsh -File Plugins\BlueprintReader\mcp-server\scripts\smoke-cpp-roundtrip.ps1 -Exe $exe
```

Add `-RunUbt` to `smoke-transpile-cpp.ps1` to rebuild the editor target
with the freshly-generated `.h`/`.cpp` and confirm it links.

(Re)seed the test BPs:

```pwsh
& "D:\Projects\Unreal Engine 5\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
    UE5_MCP.uproject -run=BlueprintReaderSeed `
    -nullrhi -nosplash -unattended -nopause
```

## Plugin-driven build

Building the editor target rebuilds the MCP server automatically — the
plugin's `PreBuildStep` runs `Scripts/Build-MCPServer.ps1`, which
no-ops if the exe is already fresher than every `src/` file:

```
UBT
 ├── (PreBuildStep) Plugins/BlueprintReader/Scripts/Build-MCPServer.ps1
 │     ├── skip if bp-reader-mcp.exe is fresher than every src/ file
 │     ├── cmake -S <plugin>/mcp-server -B <plugin>/mcp-server/build -G "VS 17 2022" -A x64
 │     └── cmake --build <plugin>/mcp-server/build --config Release
 └── BlueprintReader.uplugin → BlueprintReaderEditor.dll
```

## Engine setup (only needed for the `commandlet` backend)

The mock backend works against a fresh clone with no UE setup. To run the
commandlet backend you need a source-built UE 5.7.4 + this project's editor
target compiled.

### Engine build

The engine source is checked out at `D:\Projects\Unreal Engine 5\` —
a *sibling* of the project, intentionally outside this repo so the
~100 GB of engine source never lands in the project's git history.

```bat
cd "D:\Projects\Unreal Engine 5"
Setup.bat
GenerateProjectFiles.bat
```

Open `D:\Projects\Unreal Engine 5\UE5.sln` in Visual Studio 2022
(Game Development with C++ workload + Windows 10/11 SDK) and build the
**`UnrealEditor`** target in *Development Editor / Win64*. First build
is 1–3 hours; `Setup.bat` pulls ~70–80 GB of binary dependencies.

### Engine association

```pwsh
& "D:\Projects\Unreal Engine 5\Engine\Binaries\Win64\UnrealVersionSelector-Win64-Shipping.exe" `
    /switchversionsilent `
    "D:\Projects\UE5_MCP\UE5_MCP.uproject" `
    "D:\Projects\Unreal Engine 5"
```

This writes a stable GUID-style entry under
`HKCU\SOFTWARE\Epic Games\Unreal Engine\Builds` and updates the project's
`EngineAssociation` field to match. Hand-editing the registry is brittle —
Rider / VS / UBT all want the canonical format.

Generate project files:

```bat
"D:\Projects\Unreal Engine 5\Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.exe" ^
  -projectfiles -project="D:\Projects\UE5_MCP\UE5_MCP.uproject" ^
  -game -rocket -progress
```

Build the editor target:

```bat
"D:\Projects\Unreal Engine 5\Engine\Build\BatchFiles\Build.bat" ^
  UE5_MCPEditor Win64 Development ^
  -project="D:\Projects\UE5_MCP\UE5_MCP.uproject" ^
  -NoUba -MaxParallelActions=4
```

### Engine source patches required

The 5.7.4 GitHub source has three modules whose `Build.cs` declares
`PrivateIncludePaths` relative to `Engine/Source/` instead of the module dir.
That breaks project-target builds with `fatal error C1083`. Patch each:

- `Engine/Source/Developer/Windows/LiveCoding/LiveCoding.Build.cs`
- `Engine/Source/Developer/IOS/TVOSTargetPlatformSettings/TVOSTargetPlatformSettings.Build.cs`
- `Engine/Platforms/VisionOS/Source/Developer/VisionOSTargetPlatformSettings/VisionOSTargetPlatformSettings.Build.cs`

Add `using System.IO;` and replace the relative `PrivateIncludePaths` entry
with `Path.Combine(ModuleDirectory, ...)`. The patches live inside the
sibling engine checkout at `D:\Projects\Unreal Engine 5\`, which isn't
tracked by this repo — re-apply them after a fresh engine clone.

### Required project target settings

`Source/UE5_MCPEditor.Target.cs` must declare:

```csharp
DefaultBuildSettings = BuildSettingsVersion.V6;
BuildEnvironment = TargetBuildEnvironment.Shared;
```

### Build flags for small page files

`-NoUba -MaxParallelActions=4` if the system page file is small (Unreal Build
Accelerator allocates large VAS chunks per worker; default parallelism + UBA
can saturate a 20 GB page file even on 64 GB RAM machines).

## Automation tests (UE side)

```bat
"D:\Projects\Unreal Engine 5\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" ^
  UE5_MCP.uproject ^
  -ExecCmds="Automation RunTests BlueprintReader.Editor; Quit" ^
  -TestExit="Automation Test Queue Empty" ^
  -ReportExportPath="Saved\Automation" ^
  -log -unattended -nopause -nullrhi -nosplash
```

Report lands at `Saved/Automation/index.json` (`succeeded` / `failed` counts)
plus `index.html`.

## CI

GitHub Actions workflow at `.github/workflows/mcp-server.yml` builds + runs
the mock-backend tests on every push that touches
`Plugins/BlueprintReader/mcp-server/` or the workflow itself. The live
commandlet tests skip automatically when `BP_READER_ENGINE_DIR` /
`BP_READER_PROJECT` aren't set, so CI runs in under a minute against the
vendored deps.

