# UE5_MCP — Blueprint Reader MCP

A standalone MCP server that lets Claude (or any MCP client) read Unreal Engine
5 Blueprint assets — variables, graphs, nodes, connections, K2 metadata — via
six tools backed by the bundled `BlueprintReader` UE plugin.

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

| Tool                | Direction | Description                                                                |
|---------------------|-----------|----------------------------------------------------------------------------|
| `summarize_blueprint` | read    | **Tiny** orientation response: parent class + counts of variables / functions / graphs / macros / interfaces. Use this BEFORE `read_blueprint` when you don't yet know how big a BP is. |
| `list_blueprints`   | read      | List all blueprint assets under a content path. Defaults to `/Game`.       |
| `read_blueprint`    | read      | Top-level metadata: parent class, interfaces, variables, graph summaries.  |
| `get_graph`         | read      | Full node + connection graph by name. Defaults to `EventGraph`.            |
| `get_function`      | read      | A function's signature (inputs/outputs/locals) + body graph.               |
| `list_variables`    | read      | Member variables with type, default, category, replication state.          |
| `get_components`    | read      | SCS components — name, class, parent, root flag.                           |
| `find_node`         | read      | Substring search by class/title; optional `kind` filter on K2 extras.      |
| `get_node`          | read      | Fetch a single node by GUID — pins + links + position, no full-graph cost. |
| `find_overriders`   | read      | Structural query: BPs that extend a parent, override a function, or implement an interface. |
| `create_blueprint`  | **write** | Create a new BP under `/Game/...` extending a parent class. Idempotent.    |
| `duplicate_blueprint` | **write** | File-level duplicate: source BP → new BP at a new path. Idempotent.       |
| `retype_variable`   | **write** | Change a member variable's type without delete + re-add. Preserves graph node references. |
| `set_variable_category` | **write** | Change the My-Blueprint-panel category label on a variable.            |
| `add_variable`      | **write** | Add a member variable (BPPinType, default, category, replicated/editable). |
| `delete_variable`   | **write** | Remove a member variable by name.                                          |
| `rename_variable`   | **write** | Rename a member variable; updates references in graphs.                    |
| `add_node`          | **write** | Spawn a Branch / Sequence / VariableGet/Set / CallFunction / CustomEvent. Returns new node GUID. |
| `set_node_position` | **write** | Move a node by GUID inside a graph.                                        |
| `delete_node`       | **write** | Remove a node by GUID; breaks links into/out of it.                        |
| `wire_pins`         | **write** | Connect two pins by GUID (preferred) or name; schema-validated.            |
| `add_function`      | **write** | Create a new BP function graph; returns the echoed name.                   |
| `delete_function`   | **write** | Delete a function graph by name.                                           |
| `add_function_input`  | **write** | Add an input parameter to an existing function (BPPinType).             |
| `add_function_output` | **write** | Add an output parameter; spawns a FunctionResult node if missing.       |
| `set_variable_default` | **write** | Change a member variable's default value (string form).                |
| `list_node_kinds`   | meta      | Enumerate the `kind` values `add_node` accepts + their required extras.    |
| `list_pin_categories` | meta    | Enumerate canonical `BPPinType.category` values + container modifiers.     |
| `shutdown_daemon`   | meta      | Tear down the editor daemon (release project locks). Next read auto-respawns. Pair with `BP_READER_READ_ONLY=1` for editor coexistence. |
| `apply_ops`         | **batch** | Execute a sequence of write ops as one tool call. Named slots (`id` + `$ref`) thread minted GUIDs across ops. Single recompile per affected BP. Surfaces compile diagnostics with per-op attribution. `on_failure: "compile"` (default) or `"skip"` controls partial-state semantics on mid-batch failure. |
| `preview_ops`       | **batch** | Validate an apply_ops batch without mutating anything. Useful for agent self-checks and human-in-the-loop confirmation. |
| `compile_function`  | **batch** | Compile a tiny pseudocode DSL (`if/set/call/var/lit` + math/comparison aliases) into nodes+wires+literals. Lets the agent generate BPs the way it generates code. |
| `auto_layout_graph` | **write** | Reposition every node in a graph using a column-grid layout based on exec connectivity. |

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

## Quick start: hooking it up to Claude

### 1. Build the MCP server (no UE required for the mock backend)

```pwsh
cd Plugins\BlueprintReader\mcp-server
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
build\tests\Release\bp-reader-tests.exe   # 45 mock + 12 live (live skip without UE)
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
| `BP_READER_BACKEND`           | `mock`                                 | `mock` \| `commandlet` \| `live` (talks to a running editor over TCP — see [Configuration → Live backend](https://github.com/defessler/Unreal-Engine-5-MCP/wiki/Configuration#live-backend--talk-to-a-running-editor-over-tcp)) |
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
| `BP_READER_LIVE_PORT`         | (unset → live disabled)                | TCP port for the `live` backend. Set in BOTH the editor's process env AND the MCP server's env. |
| `BP_READER_LIVE_TOKEN`        | (unset → live refuses)                 | Shared secret for live-backend auth. Set in both processes; values must match. |

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
│       │   ├── tools\                     ToolRegistry, BlueprintTools
│       │   └── backends\                  IBlueprintReader, MockReader, CommandletReader
│       ├── tests\                         doctest cases (mock + live commandlet)
│       ├── scripts\roundtrip.ps1          JSON-RPC smoke test
│       ├── fixtures\                      BP_Enemy / BP_Pickup / BP_PlayerController
│       ├── third_party\                   vendored deps: nlohmann_json, fmt, doctest
│       ├── CMakeLists.txt
│       └── vcpkg.json                     (declared but not consumed by default)
├── Content\AI\                            BP_TestEnemy.uasset, BP_TestPickup.uasset
                                           (engine source lives outside this repo
                                            at D:\Projects\Unreal Engine 5\)
├── PLAN.md
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

Plugins\BlueprintReader\mcp-server\build\tests\Release\bp-reader-tests.exe   # all 57 cases
pwsh -File Plugins\BlueprintReader\mcp-server\scripts\roundtrip.ps1 `
    -Exe Plugins\BlueprintReader\mcp-server\build\Release\bp-reader-mcp.exe `
    -Asset /Game/AI/BP_TestEnemy
```

(Re)seed the test BPs:

```pwsh
& "D:\Projects\Unreal Engine 5\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
    UE5_MCP.uproject -run=BlueprintReaderSeed `
    -nullrhi -nosplash -unattended -nopause
```

## Plugin-driven build

Once the engine and project are set up (next section), building the editor
target rebuilds the MCP server automatically:

```
UBT
 ├── (PreBuildStep) Plugins/BlueprintReader/Scripts/Build-MCPServer.ps1
 │     ├── skip if bp-reader-mcp.exe is fresher than every src/ file
 │     ├── cmake -S <plugin>/mcp-server -B <plugin>/mcp-server/build -G "VS 17 2022" -A x64
 │     └── cmake --build <plugin>/mcp-server/build --config Release
 └── BlueprintReader.uplugin → BlueprintReaderEditor.dll
```

Wired in `BlueprintReader.uplugin`:

```json
"PreBuildSteps": {
  "Win64": [
    "powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"$(PluginDir)/Scripts/Build-MCPServer.ps1\" -ProjectDir \"$(ProjectDir)\" -PluginDir \"$(PluginDir)\""
  ]
}
```

The script no-ops when used standalone (plugin dropped into a project that
doesn't ship `mcp-server/`), so the plugin remains portable.

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

## Layout / phases

See [PLAN.md](PLAN.md) for the roadmap (Phase 0 mock, Phase 1 commandlet,
Phase 1.5 polish + daemon, Phase 2 live backend).
