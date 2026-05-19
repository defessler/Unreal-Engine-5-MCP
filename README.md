# UE5_MCP — Blueprint Reader MCP

A standalone MCP server that lets Claude (or any MCP client) read **and edit**
Unreal Engine 5 Blueprint assets — variables, graphs, nodes, connections, K2
metadata — and **round-trip BPs to/from C++** via 126 tools backed by the
bundled `BlueprintReader` UE plugin.

```
┌─────────────────┐  JSON-RPC/stdio  ┌─────────────────┐  CreateProcessW  ┌──────────────────┐
│  Claude / any   │ ──────────────► │  bp-reader-mcp  │ ───────────────► │ UnrealEditor-Cmd │
│   MCP client    │ ◄────────────── │      .exe       │ ◄─────────────── │  + plugin DLL    │
└─────────────────┘                 └─────────────────┘  result JSON     └──────────────────┘
```

Two backends:
- **`mock`** — fixture-backed; no UE required. Used for unit tests and
  bring-up. Three handcrafted JSON fixtures under `Plugins/BlueprintReader/Tests/BlueprintReaderMcpTests/fixtures/`.
- **`commandlet`** — drives a real `UnrealEditor-Cmd.exe -run=BPR`
  to read live `.uasset` files from a UE project. Defaults to **daemon mode**
  (one editor process reused across calls) so per-call cost is ~30 ms after
  the initial ~5 s editor cold start.

The plugin ships as two modules with proper isolation:
- **`BlueprintReaderEditor`** (`Type:"Editor"`) — full BP introspection
  + write tools. Loaded only in editor builds; UBT excludes it from
  packaged Game / Server / Client targets automatically.
- **`BlueprintReaderRuntime`** (`Type:"Runtime"`) — read-only BP
  introspection via UClass reflection. Loads in both editor and
  packaged builds, so you can introspect a shipped game's blueprints
  (asset-registry list, class hierarchy, interfaces, UPROPERTY
  variables with CDO defaults, UFUNCTION signatures, components from
  SCS / CDO). Source-level K2 graphs aren't available in cooked
  builds (stripped during cook), so `graphs[]` comes back empty —
  everything else round-trips. Two console commands wired:
  `bp_reader.list <Path>` and `bp_reader.read <AssetPath>` so you can
  triage cooked-game BP shapes without leaving the in-game console.

## Tools

126 tools across 22 categories — see the
[Tool Reference](https://github.com/defessler/Unreal-Engine-5-MCP/wiki/Tool-Reference)
for every input/output shape with examples.

| Category | Tools | What they do |
|----------|-------|--------------|
| **Read** (10) | `list_blueprints`, `summarize_blueprint`, `read_blueprint`, `get_graph`, `get_function`, `list_variables`, `get_components`, `find_node`, `get_node`, `find_overriders` | Inventory + structural queries. `summarize_blueprint` is the cheap orientation tool; `find_overriders` does cross-BP queries in one call. |
| **Write** (22) | `add_variable` / `retype_variable` / `rename_variable` / etc.; `add_function` / `add_function_input/output`; `add_node` / `wire_pins` / `set_pin_default` / `auto_layout_graph`; `create_blueprint` / `duplicate_blueprint`; `add_component` / `remove_component` / `attach_component` / `set_component_property` | Single-step mutations. All idempotent where idempotency makes sense; `wire_pins` errors include both pin types so the agent can self-correct. |
| **Batch / generation** (3) | `apply_ops`, `preview_ops`, `compile_function` | Multi-step writes: named-slot GUID resolution, dry-run, pseudocode → BP graph. Collapse N×compile to 1. |
| **Transpile (BP↔C++)** (6) | `decompile_function`, `decompile_blueprint`, `transpile_function`, `transpile_blueprint`, `write_generated_source`, `parse_cpp_function` | Round-trip BPs to and from C++ via the BPIR JSON AST. See [BP↔C++ round-trip](#bp--c-round-trip) below. |
| **Project + Content Browser** (13) | `get_project_metadata`, `save_all`, `move_asset`, `delete_asset`, `create_folder`, `list_data_tables`, `read_data_table`, `add_data_row`, `set_data_row_value`, `get_referencers`, `get_dependencies`, `read_config_value`, `set_config_value` | Project-level introspection + asset-browser ops complementing the per-Blueprint surface. Asset-graph queries (`get_referencers` / `get_dependencies`) source from the in-memory Asset Registry. Config tools edit `Default*.ini` files via GConfig. |
| **Live editor** (15) | `console_command`, `get_cvar`, `set_cvar`, `pie_start`, `pie_stop`, `live_coding_compile`, `get_selected_actors`, `set_selection`, `spawn_actor`, `set_actor_transform`, `delete_actor`, `read_output_log`, `get_editor_state`, `run_python_script`, `build_lighting` | Operate on the running editor's in-memory state. `get_editor_state` is a one-call situational-awareness bundle (open assets, active asset, current level, viewport camera, selection, PIE state) inspired by Epic AIAssistant's Slate querier. `run_python_script` is gated by `BP_READER_ALLOW_PYTHON=1` — full `unreal.*` Python API access wrapped in a transaction. Work best with the `live` backend (open editor); commandlet daemon routes them too. |
| **Automation** (1) | `run_automation_tests` | Kick off UE's automation test framework with a wildcard pattern; results land in the output log + `Saved/Automation/`. |
| **Material authoring** (7) | `list_materials`, `read_material`, `add_material_expression`, `connect_material_expressions`, `set_material_parameter`, `set_material_instance_parameter`, `compile_material` | Walk the UMaterial expression graph; add nodes; wire them to other expressions or to the master-material slots (BaseColor / Roughness / Normal / …); override scalar/vector/texture parameters on a UMaterialInstanceConstant; trigger shader recompiles. |
| **UMG widgets** (5) | `read_widget_blueprint`, `add_widget`, `set_widget_property`, `bind_widget_event`, `compile_widget_blueprint` | Inspect UWidgetBlueprint's UWidgetTree; add widgets under a PanelWidget parent; set widget properties; scaffold event handlers; compile. |
| **Behavior Trees** (5) | `list_behavior_trees`, `read_behavior_tree`, `add_bt_node`, `set_bt_node_property`, `compile_behavior_tree` | Walk a UBehaviorTree's runtime node graph (composite / decorator / service / task); scaffold new nodes; set node properties; mark dirty. Final attach for new nodes still uses the BT editor for graph wiring. |
| **DataAssets** (4) | `list_data_assets`, `read_data_asset`, `create_data_asset`, `set_data_asset_property` | Create / inspect / mutate any UDataAsset subclass. Properties round-trip via UE's text property serializer (same encoding `set_component_property` uses). |
| **StateTree** (5) | `list_state_trees`, `read_state_tree`, `add_state_tree_state`, `set_state_tree_transition`, `compile_state_tree` | Discover UStateTree assets via Asset Registry; state/transition authoring scaffolds and returns a hint pointing at StateTreeEditor (full authoring still needs the editor module). |
| **Profiling** (4) | `start_profile`, `stop_profile`, `get_stats`, `take_screenshot` | Drive UE's profiling backends (stats / CSV / UnrealInsights), snapshot stat groups, capture high-res screenshots. |
| **Headless cook** (2) | `cook_content`, `package_project` | Returns the RunUAT command line to run for a target platform. We don't shell out inline (avoids editor-already-running reentrancy); agent runs the printed command. |
| **Class introspection** (3) | `introspect_class` (tool name `get_class_info`), `find_class`, `list_functions` | Reflection over the live UClass registry: parent chain, UPROPERTYs, UFUNCTIONs with their BP flags. Substring class-name search across the registry. |
| **Viewport ergonomics** (4) | `focus_actor`, `set_camera_transform`, `take_viewport_screenshot`, `set_show_flag` | Frame an actor, move the editor camera, capture the active viewport, toggle showflags (Bones, Collision, Wireframe, etc.). |
| **Niagara** (4) | `list_niagara_systems`, `read_niagara_system`, `create_niagara_system`, `set_niagara_parameter` | Discover UNiagaraSystem assets + scaffold; full authoring still needs NiagaraEditor module. |
| **Sequencer** (4) | `list_level_sequences`, `read_level_sequence`, `add_sequence_track`, `set_sequence_playback_range` | Discover ULevelSequence assets + scaffolded track/range writes (full authoring needs LevelSequenceEditor + MovieScene). |
| **GAS / GameplayTags** (3) | `list_gameplay_tags`, `add_gameplay_tag`, `read_ability_set` | Query the GameplayTagsManager, scaffold tag additions (writes Config/Tags/DefaultGameplayTags.ini), and read any DataAsset that holds an array of `{class, level}`-shaped ability entries. |
| **AnimGraph** (4) | `list_anim_blueprints`, `read_anim_blueprint`, `add_anim_state`, `compile_anim_blueprint` | Discover UAnimBlueprint + read parent class + compile via FKismetEditorUtilities. State-machine authoring scaffolded; full graph needs AnimGraph module. |
| **Discoverability + meta** (3) | `list_node_kinds`, `list_pin_categories`, `shutdown_daemon` | Self-describing surface so the agent can ask "what's a valid `add_node` kind?" or "what does a struct-ref BPPinType look like?" without scanning docs. |

Wire shapes are pinned in `Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/BlueprintReaderTypes.h`. Snake_case
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
  `transpile_blueprint` (whole class). End-to-end verified — generated
  source compiles cleanly via UBT into the project's editor target.
  Output picks up Epic's UE5 conventions: `UCLASS(MinimalAPI,
  Blueprintable)` when no API macro, `TObjectPtr<X>` for UPROPERTY
  object refs, `VisibleAnywhere/BlueprintReadOnly` for component
  fields, constructor with `bReplicates = true` for replicated actors,
  forward declarations before `.generated.h`, `UFUNCTION(BlueprintPure)
  const` inference from FunctionEntry shape, `const FString& /
  FVector&` args for heavy types, safety defaults on primitives
  (`bool = false`, `int = 0`), synthetic returns when the BP body
  doesn't reach a FunctionResult, `DOREPLIFETIME_CONDITION` from
  RepCondition, `ReplicatedUsing=` + UFUNCTION OnRep_X callbacks
  for RepNotify, sidecar warnings for UE-reserved name collisions
  (`TakeDamage` shadowing AActor::TakeDamage, etc.). Pair with
  `write_generated_source` to drop `.h`/`.cpp` into
  `<Project>/Source/<Module>/Generated/`.
- **Auto-lowered structural patterns** — what used to require manual
  porting now lands as real compilable scaffolds. **Stateful macros**
  (`DoOnce`, `FlipFlop`, `DoN`) get synth `bool` / `int32` member
  variables plus guarded `if` blocks. **Latent actions**
  (`Delay`, `RetriggerableDelay`, `DelayUntilNextTick`) split the
  parent function at the delay boundary: pre-delay code ends with
  `GetWorld()->GetTimerManager().SetTimer(...)`, post-delay code
  lives as a generated `UFUNCTION()` continuation method, and an
  `FTimerHandle` member tracks the timer for cancel/state.
  **EnhancedInput** (`K2Node_EnhancedInputAction`) generates one
  `UFUNCTION()` callback per wired trigger pin, a
  `TObjectPtr<UInputAction>` UPROPERTY per action, and a synthesized
  `SetupPlayerInputComponent` override that wraps `EIC->BindAction(...)`
  calls in a `Cast<UEnhancedInputComponent>` guard. Nested patterns
  compose (a `Delay` inside an EnhancedInput callback still lowers
  correctly).
- **C++ → BP** — `parse_cpp_function` produces BPIR; pipe through
  `compile_function` to materialize the BP graph.
- **Round-trip identity** — for the patterns `transpile_function` emits,
  `parse_cpp_function` recovers an equivalent BPIR (pinned by 12
  round-trip test cases).

Subset accepted by the parser: if/else, range-based for, while, switch,
return, break, continue, calls, member access, `Cast<T>()`, unary +
binary operators with full C++ precedence. Patterns still landing as
TODO + sidecar (no auto-lowering yet): timelines, async tasks with
typed payload pins, anim graphs, Gate macro (multi-input stateful),
legacy non-EnhancedInput nodes, raw pointer arithmetic. See
[Tool Reference → Transpile tools](https://github.com/defessler/Unreal-Engine-5-MCP/wiki/Tool-Reference#transpile-tools)
and [BPIR schema](https://github.com/defessler/Unreal-Engine-5-MCP/wiki/BPIR).

## Quick start: hooking it up to Claude

### 1. Build the MCP server

The MCP server is now a UE Program target built via UBT (same pipeline
as the rest of the plugin — UBA-friendly, no separate CMake toolchain).
You need an Unreal Engine source build to compile it.

```pwsh
# From a project that has BlueprintReader in its Plugins/:
"<Engine>\Engine\Build\BatchFiles\Build.bat" `
  BlueprintReaderMcp Win64 Development `
  -project="<Absolute>\MyGame.uproject"
```

The exe lands at `<ProjectDir>/Binaries/Win64/BlueprintReaderMcp.exe`.

Both binaries (`BlueprintReaderMcp` + `BlueprintReaderMcpTests`) can be
built in one shot via `Plugins/BlueprintReader/Scripts/Build-MCPServer.ps1`:

```pwsh
.\Plugins\BlueprintReader\Scripts\Build-MCPServer.ps1 `
  -EngineDir "D:\Path\To\UnrealEngine" `
  -ProjectFile "$PWD\MyGame.uproject"
```

Third-party deps (nlohmann_json, fmt, doctest) are vendored under
`Plugins/BlueprintReader/Tests/ThirdParty/`, so this works with no
network access and no extra package manager — UBT is the only build
tool required.

> **Tests:** `Build.bat BlueprintReaderMcpTests Win64 Development -project=...`
> produces `<ProjectDir>/Binaries/Win64/BlueprintReaderMcpTests.exe` —
> 441 doctest cases, ~5 s to run; the 12 live-only cases auto-skip when
> the UE editor env vars aren't set.

### 2. Install the Claude skills (optional but recommended)

The plugin ships Claude skills + an agent that teach Claude (and
compatible clients) how to drive `bp-reader` well. Install them with:

```pwsh
pwsh Plugins/BlueprintReader/Scripts/Install-ClaudeAssets.ps1
```

```bash
./Plugins/BlueprintReader/Scripts/install-claude-assets.sh
```

The script copies `Plugins/BlueprintReader/Claude/{agents,skills}/`
into `<project-root>/.claude/`, where Claude Code discovers them.
Idempotent — re-run it after every plugin update so the skills stay
in lockstep with the wire format. See
`Plugins/BlueprintReader/Claude/README.md` for the layout.

### 3. Wire it into your MCP client

The repo ships a project-scope **`.mcp.json`** at the root, so cloning +
launching Claude Code from the project directory wires bp-reader
automatically. The contents:

```json
{
  "mcpServers": {
    "bp-reader": {
      "command": "D:\\Projects\\UE5_MCP\\Binaries\\Win64\\BlueprintReaderMcp.exe",
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

### 4. Try it

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
│   ├── BlueprintReader.uplugin
│   ├── Scripts\Build-MCPServer.ps1        UBT-wrapper convenience script
│   ├── Source\BlueprintReaderEditor\
│   │   ├── BlueprintIntrospector         FBlueprintInfo from FBlueprintGeneratedClass
│   │   ├── BlueprintReaderJson           legacy/rich plugin shape (camelCase, K2 extras)
│   │   ├── BlueprintReaderWireJson       canonical MCP wire shape (snake_case)
│   │   ├── BlueprintReaderCommandlet     -run=BPR; -Op + -Daemon dispatch
│   │   └── BlueprintReaderSeedCmdlet     -run=BPRSeed; synthesize test BPs
│   ├── Source\BlueprintReaderRuntime\    runtime-safe BP introspection
│   └── Tests\                             UE Program targets (UBT-built)
│       ├── BlueprintReaderMcp\           main exe → BlueprintReaderMcp.exe
│       │   ├── BlueprintReaderMcp.Target.cs
│       │   ├── BlueprintReaderMcp.Build.cs
│       │   └── Private\main.cpp
│       ├── BlueprintReaderMcpCore\       shared static-lib module (impl)
│       │   ├── BlueprintReaderMcpCore.Build.cs
│       │   └── Private\
│       │       ├── BlueprintReaderTypes.h   POD wire types
│       │       ├── jsonrpc\                 Server, Mcp (initialize/tools/call/…)
│       │       ├── tools\                   ToolRegistry, BlueprintTools, Bpir, Decompile
│       │       │   ├── codegen\             CppEmit, CppClassEmit, UnsupportedTreatment
│       │       │   └── parse\               CppLex, CppParse (C++ → BPIR)
│       │       └── backends\                IBlueprintReader, MockReader, CommandletReader
│       ├── BlueprintReaderMcpTests\      doctest suite → BlueprintReaderMcpTests.exe
│       │   └── Private\                   441 cases (mock + live commandlet)
│       └── ThirdParty\                    vendored: nlohmann_json, fmt, doctest
├── Content\AI\                            BP_TestEnemy.uasset, BP_TestPickup.uasset
                                           (engine source lives outside this repo
                                            at D:\Projects\Unreal Engine 5\)
└── README.md
```

## Subcommands

```pwsh
BlueprintReaderMcp.exe doctor                    # check setup, exit non-zero if broken
BlueprintReaderMcp.exe config                    # print .mcp.json snippet (Claude Code)
BlueprintReaderMcp.exe config --client=copilot   # same, for VS Code Copilot
BlueprintReaderMcp.exe config --client=claude-desktop
BlueprintReaderMcp.exe config --mock             # mock-only snippet (no UE required)
BlueprintReaderMcp.exe --help
```

`doctor` walks the install — engine path, .uproject location + plugin
entry, editor exe, plugin DLL config-suffix match — and prints actionable
fix hints with build commands. Replaces the `Verify-Build.bat` flow.

`config` auto-discovers the engine + project paths and outputs a
ready-to-paste config block for the chosen client. `--mock` skips
discovery and emits a fixture-backed snippet (useful for testing
your MCP client wiring without a UE project).

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

`Plugins/BlueprintReader/Scripts/Build-MCPServer.ps1` is a thin UBT
wrapper that builds both Program targets in one shot — useful for
building the server standalone when you just want fresh binaries.

## Live verification

If you have the editor built and the test BPs seeded:

```pwsh
$env:BP_READER_BACKEND     = "commandlet"
$env:BP_READER_ENGINE_DIR  = "D:\Projects\Unreal Engine 5"
$env:BP_READER_PROJECT     = "D:\Projects\UE5_MCP\UE5_MCP.uproject"

Binaries\Win64\BlueprintReaderMcpTests.exe   # all 441 cases (12 live-only auto-skip)
```

The legacy smoke scripts that lived under `mcp-server/scripts/` were
removed alongside the CMake build. The doctest suite covers the same
ground (and more); for an interactive smoke run, build with
`-Config Development` and stream stdin/stdout against the exe directly.

(Re)seed the test BPs:

```pwsh
& "D:\Projects\Unreal Engine 5\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
    UE5_MCP.uproject -run=BPRSeed `
    -nullrhi -nosplash -unattended -nopause
```

## Build pipeline

The MCP server is a pair of UE Program targets that live under
`Plugins/BlueprintReader/Tests/`. UBT builds them with the same
pipeline (UBA, ninja, shared compile cache) as the editor target:

```
Build.bat BlueprintReaderMcp     Win64 Development -project=…  →  Binaries/Win64/BlueprintReaderMcp.exe
Build.bat BlueprintReaderMcpTests Win64 Development -project=…  →  Binaries/Win64/BlueprintReaderMcpTests.exe
Build.bat UE5_MCPEditor          Win64 Development -project=…  →  the editor (independent target)
```

The plugin's `BlueprintReader.uplugin` declares a `PreBuildSteps` hook
that invokes UBT for `BlueprintReaderMcp` before every editor build,
so `Build.bat UE5_MCPEditor ...` pulls the server along automatically
— drop the plugin into any project and the convenience just works
without touching the project's `Target.cs`. The test exe is *not*
pulled in (heavier, less often needed); use
`Plugins/BlueprintReader/Scripts/Build-MCPServer.ps1 -Targets Tests`
to build it. Set `BP_READER_SKIP_PREBUILD=1` in the build environment
to disable the auto-build entirely.

## Working with non-UE5_MCP projects (Lyra)

The plugin is now project-agnostic. Drop `Plugins/BlueprintReader/` into
any UE 5.7 project and set these env vars when invoking the MCP server
or running the live tests:

| Variable | Purpose |
|---|---|
| `BP_READER_PROJECT` | Full path to the `.uproject` (replaces the auto-discovered UE5_MCP.uproject). |
| `BP_READER_EDITOR_TARGET` | Editor-target name for projects that use `TargetBuildEnvironment.Unique` (e.g. `LyraEditor`). The plugin looks for `<Project>/Binaries/Win64/<TargetName>-Cmd.exe` before falling back to the engine's `UnrealEditor-Cmd.exe`. |
| `BP_READER_EDITOR_CMD` | Full path to a `-Cmd.exe` binary that overrides both. |

The `LyraStarterGame.uproject` checked in at the repo root demonstrates
the Lyra Starter Game integration — see `GeneratedFromBP/` for 270+
.h/.cpp pairs auto-emitted from Lyra's BP-style assets via
`transpile_blueprint`.

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
That breaks project-target builds with `fatal error C1083`.

**Easy path:** the plugin ships an idempotent script that applies all
three patches:

```pwsh
# Dry-run first to see what would change:
.\Plugins\BlueprintReader\Scripts\Patch-Engine.ps1 `
    -EngineDir "D:\Projects\Unreal Engine 5"

# Apply:
.\Plugins\BlueprintReader\Scripts\Patch-Engine.ps1 `
    -EngineDir "D:\Projects\Unreal Engine 5" -Apply
```

The script adds `using System.IO;` (if missing) and rewrites each
relative `PrivateIncludePaths` entry to `Path.Combine(ModuleDirectory,
...)` form. Patches affect:

- `Engine/Source/Developer/Windows/LiveCoding/LiveCoding.Build.cs`
- `Engine/Source/Developer/IOS/TVOSTargetPlatformSettings/TVOSTargetPlatformSettings.Build.cs`
- `Engine/Platforms/VisionOS/Source/Developer/VisionOSTargetPlatformSettings/VisionOSTargetPlatformSettings.Build.cs`

These patches live inside the sibling engine checkout (e.g.
`D:\Projects\Unreal Engine 5\`), which isn't tracked by this repo — re-run
the script after a fresh engine clone.

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

The prior `.github/workflows/mcp-server.yml` (CMake-on-Windows-2022) was
removed when the MCP server moved into the UE build pipeline. UBT-based
CI needs a runner with the source-built engine available, which is
heavier than the prior mock-only CI was — not yet set up.

For local pre-push verification, run the test target:

```pwsh
"<Engine>\Engine\Build\BatchFiles\Build.bat" `
  BlueprintReaderMcpTests Win64 Development `
  -project="$PWD\UE5_MCP.uproject"
Binaries\Win64\BlueprintReaderMcpTests.exe
```

441 cases including the mock-backend coverage; the 12 live-only cases
auto-skip when the UE editor env vars aren't set.

