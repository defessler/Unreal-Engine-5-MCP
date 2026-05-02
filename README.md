# UE5_AI_BP — Blueprint Reader MCP

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
  bring-up. Three handcrafted JSON fixtures under `mcp-server/fixtures/`.
- **`commandlet`** — drives a real `UnrealEditor-Cmd.exe -run=BlueprintReader`
  to read live `.uasset` files from a UE project. Defaults to **daemon mode**
  (one editor process reused across calls) so per-call cost is ~30 ms after
  the initial ~5 s editor cold start.

## Tools

| Tool                | Direction | Description                                                                |
|---------------------|-----------|----------------------------------------------------------------------------|
| `list_blueprints`   | read      | List all blueprint assets under a content path. Defaults to `/Game`.       |
| `read_blueprint`    | read      | Top-level metadata: parent class, interfaces, variables, graph summaries.  |
| `get_graph`         | read      | Full node + connection graph by name. Defaults to `EventGraph`.            |
| `get_function`      | read      | A function's signature (inputs/outputs/locals) + body graph.               |
| `list_variables`    | read      | Member variables with type, default, category, replication state.          |
| `find_node`         | read      | Substring search by class/title; optional `kind` filter on K2 extras.      |
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

Wire shapes are pinned in `Shared/BlueprintReaderTypes.h`. Snake_case keys,
nullable string fields emit `null`, `BPNode.meta` is a real nested object.

## Quick start: hooking it up to Claude

### 1. Build the MCP server (no UE required for the mock backend)

```pwsh
cd mcp-server
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
build\tests\Release\bp-reader-tests.exe   # 40+ doctest cases
```

The exe is at `mcp-server/build/Release/bp-reader-mcp.exe`.

### 2. Wire it into your MCP client

**Claude Code** (`~/.claude/mcp.json` or per-project `.mcp.json`):

```json
{
  "mcpServers": {
    "bp-reader": {
      "command": "D:\\Projects\\UE5_AI_BP\\mcp-server\\build\\Release\\bp-reader-mcp.exe",
      "env": {
        "BP_READER_BACKEND":     "commandlet",
        "BP_READER_ENGINE_DIR":  "D:\\Projects\\UE5_AI_BP\\UnrealEngine",
        "BP_READER_PROJECT":     "D:\\Projects\\UE5_AI_BP\\UE5_AI_BP.uproject"
      }
    }
  }
}
```

**Claude Desktop** — same shape under `claude_desktop_config.json`'s
`mcpServers` key (typically `%APPDATA%\Claude\claude_desktop_config.json`).

For local experimentation without UE, drop `BP_READER_BACKEND=commandlet` and
the env block — defaults to the mock backend pointed at the bundled fixtures.

### 3. Try it

In Claude, reference your blueprint by content path:
> "What variables are on `/Game/AI/BP_TestEnemy`? Find any K2Node_VariableGet
> nodes."

Claude calls `read_blueprint` → `find_node`, gets back canonical JSON.

## Configuration (env vars)

| Variable                      | Default                                | Purpose                                                                  |
|-------------------------------|----------------------------------------|--------------------------------------------------------------------------|
| `BP_READER_BACKEND`           | `mock`                                 | `mock` \| `commandlet` \| `live` (Phase 2, not yet implemented)         |
| `BP_READER_FIXTURES_DIR`      | `<exe>/fixtures`                       | Mock backend's fixture dir.                                              |
| `BP_READER_ENGINE_DIR`        | (unset → fail-fast for `commandlet`)   | Path to the source-built engine (`...\UnrealEngine`).                    |
| `BP_READER_PROJECT`           | (unset → fail-fast for `commandlet`)   | Path to the `.uproject`.                                                 |
| `BP_READER_TIMEOUT_SECONDS`   | `120`                                  | Per-call timeout for the editor subprocess.                              |
| `BP_READER_DAEMON`            | `1` (on)                               | `1`/`true`/`yes`/`on` to enable. Set `0` to fall back to one-shot mode.  |

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

```
UE5_AI_BP\
├── UE5_AI_BP.uproject
├── Source\                              project runtime module
├── Plugins\BlueprintReader\             plugin (Editor-only)
│   └── Source\BlueprintReaderEditor\
│       ├── BlueprintIntrospector       FBlueprintInfo from FBlueprintGeneratedClass
│       ├── BlueprintReaderJson         legacy/rich plugin shape (camelCase, K2 extras)
│       ├── BlueprintReaderWireJson     canonical MCP wire shape (snake_case)
│       ├── BlueprintReaderCommandlet   -run=BlueprintReader; -Op + -Daemon dispatch
│       └── BlueprintReaderSeedCmdlet   -run=BlueprintReaderSeed; synthesize test BPs
├── Content\AI\                          BP_TestEnemy.uasset, BP_TestPickup.uasset
├── Shared\BlueprintReaderTypes.h        WITH_UE-gated POD/USTRUCT pair
├── mcp-server\                          standalone C++20 MCP server
│   ├── src\
│   │   ├── jsonrpc\                    Server, Mcp (initialize/tools/call/...)
│   │   ├── tools\                       ToolRegistry, BlueprintTools
│   │   └── backends\                    IBlueprintReader, MockReader, CommandletReader
│   ├── tests\                           doctest cases (mock + live commandlet)
│   ├── scripts\roundtrip.ps1            JSON-RPC smoke test
│   ├── fixtures\                        BP_Enemy / BP_Pickup / BP_PlayerController
│   ├── CMakeLists.txt
│   └── vcpkg.json
├── UnrealEngine\                        source-built engine, .gitignored
├── PLAN.md
└── README.md
```

## Live verification

If you have the editor built and the test BPs seeded:

```pwsh
$env:BP_READER_BACKEND     = "commandlet"
$env:BP_READER_ENGINE_DIR  = "D:\Projects\UE5_AI_BP\UnrealEngine"
$env:BP_READER_PROJECT     = "D:\Projects\UE5_AI_BP\UE5_AI_BP.uproject"

mcp-server\build\tests\Release\bp-reader-tests.exe                       # all 47 cases
pwsh -File mcp-server\scripts\roundtrip.ps1 `
    -Exe mcp-server\build\Release\bp-reader-mcp.exe `
    -Asset /Game/AI/BP_TestEnemy
```

(Re)seed the test BPs:

```pwsh
UnrealEngine\Engine\Binaries\Win64\UnrealEditor-Cmd.exe `
    UE5_AI_BP.uproject -run=BlueprintReaderSeed `
    -nullrhi -nosplash -unattended -nopause
```

## Engine setup (only needed for the `commandlet` backend)

The mock backend works against a fresh clone with no UE setup. To run the
commandlet backend you need a source-built UE 5.7.4 + this project's editor
target compiled.

### Engine build

```bat
cd UnrealEngine
Setup.bat
GenerateProjectFiles.bat
```

Open `UnrealEngine\UE5.sln` in Visual Studio 2022 (Game Development with C++
workload + Windows 10/11 SDK) and build the **`UnrealEditor`** target in
*Development Editor / Win64*. First build is 1–3 hours; `Setup.bat` pulls
~70–80 GB of binary dependencies.

### Engine association

```bat
cd UnrealEngine\Engine\Binaries\Win64
UnrealVersionSelector.exe -register
```

Then either:
- Right-click `UE5_AI_BP.uproject` → **Switch Unreal Engine version…** and pick
  the source build, *or*
- Edit `EngineAssociation` in `UE5_AI_BP.uproject` to that GUID directly.

Generate project files:

```bat
"UnrealEngine\Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.exe" ^
  -projectfiles -project="D:\Projects\UE5_AI_BP\UE5_AI_BP.uproject" ^
  -game -rocket -progress
```

Build the editor target:

```bat
"UnrealEngine\Engine\Build\BatchFiles\Build.bat" ^
  UE5_AI_BPEditor Win64 Development ^
  -project="D:\Projects\UE5_AI_BP\UE5_AI_BP.uproject" ^
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
gitignored `UnrealEngine/` and need re-applying after a fresh engine clone.

### Required project target settings

`Source/UE5_AI_BPEditor.Target.cs` must declare:

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
UnrealEngine\Engine\Binaries\Win64\UnrealEditor-Cmd.exe ^
  UE5_AI_BP.uproject ^
  -ExecCmds="Automation RunTests BlueprintReader.Editor; Quit" ^
  -TestExit="Automation Test Queue Empty" ^
  -ReportExportPath="Saved\Automation" ^
  -log -unattended -nopause -nullrhi -nosplash
```

Report lands at `Saved/Automation/index.json` (`succeeded` / `failed` counts)
plus `index.html`.

## CI

GitHub Actions workflow at `.github/workflows/mcp-server.yml` builds + runs
the mock-backend tests on every push that touches `mcp-server/`, `Shared/`,
or the workflow itself. The live commandlet tests skip automatically when
`BP_READER_ENGINE_DIR` / `BP_READER_PROJECT` aren't set, so CI runs in under
a minute once the FetchContent cache is warm.

## Layout / phases

See [PLAN.md](PLAN.md) for the roadmap (Phase 0 mock, Phase 1 commandlet,
Phase 1.5 polish + daemon, Phase 2 live backend).
