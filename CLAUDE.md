# Claude project guidance — UE5_MCP

This repo is a UE 5.7.4 project plus a standalone MCP server that lets
Claude read and edit Blueprint assets through 21 tools. Two halves:

- **`Plugins/BlueprintReader/`** — Editor-only UE plugin with two
  commandlets:
  - `UBlueprintReaderCommandlet` (`-run=BlueprintReader`) — dispatches
    `-Op=List|Read|Graph|Function|Variables|Components|Find` and the
    write ops (`-Op=AddVariable|...`). Also runs in long-lived
    `-Daemon` mode reading newline-delimited commandlet-arg lines from
    stdin.
  - `UBlueprintReaderSeedCommandlet` (`-run=BlueprintReaderSeed`) —
    synthesizes `Content/AI/BP_TestEnemy.uasset` and `BP_TestPickup.uasset`
    used by the live integration tests.
- **`Plugins/BlueprintReader/mcp-server/`** — Standalone C++20 MCP
  server, vendored inside the plugin so the whole thing ships as one
  unit. JSON-RPC 2.0 over stdio. Two backends: `mock` (fixtures only,
  no UE) and `commandlet` (drives the plugin via `UnrealEditor-Cmd.exe`).
  **Daemon mode is the default** for the commandlet backend (~30 ms/call
  after a ~5 s cold start; opt out with `BP_READER_DAEMON=0`). Built
  automatically as a `PreBuildStep` of `BlueprintReader.uplugin` —
  third-party deps (nlohmann_json, fmt, doctest) are vendored under
  `mcp-server/third_party/`, no git/network/vcpkg required.

When you need to **use** the MCP tools to read or modify a blueprint,
the `bp-reader` skill in `.claude/skills/bp-reader/` covers patterns,
the wire format, and per-tool guidance. This file covers building,
testing, and maintaining the project itself.

## Repo layout

```
UE5_MCP/                                      ← project root (this dir)
├── UE5_MCP.uproject
├── Source/                                     project runtime module
├── Plugins/BlueprintReader/                    plugin ships as one unit
│   ├── BlueprintReader.uplugin                 PreBuildSteps run Build-MCPServer.ps1
│   ├── Scripts/Build-MCPServer.ps1             plugin-driven cmake build
│   ├── Source/BlueprintReaderEditor/
│   │   ├── Public/                             BlueprintReaderTypes, Introspector,
│   │   │                                       WireJson, *Commandlet.h
│   │   └── Private/                            impls
│   └── mcp-server/                             standalone C++20 MCP server
│       ├── src/
│       │   ├── BlueprintReaderTypes.h          POD/USTRUCT dual-mode wire types (canonical)
│       │   ├── jsonrpc/                        Server, Mcp (handshake + dispatch)
│       │   ├── tools/                          ToolRegistry, BlueprintTools
│       │   └── backends/                       IBlueprintReader, MockReader, CommandletReader
│       ├── tests/                              doctest cases (mock + live)
│       ├── scripts/roundtrip.ps1               JSON-RPC smoke harness
│       ├── fixtures/                           BP_*.json mock-backend data
│       ├── third_party/                        vendored deps (nlohmann_json, fmt, doctest)
│       ├── CMakeLists.txt
│       └── vcpkg.json                          declared but not consumed by default
├── Content/AI/                                 BP_TestEnemy.uasset, BP_TestPickup.uasset
│                                               (regenerable; see "Reseed test BPs" below)
├── PLAN.md                                     phase status, decisions
├── README.md                                   user-facing setup + tool table
└── .github/workflows/mcp-server.yml            CI (mock-only on windows-2022)
```

The source-built engine is a sibling of this project at
`D:\Projects\Unreal Engine 5\` — outside this repo by design. That
path holds `Engine\`, `UE5.sln`, `Setup.bat`, etc. None of it is
tracked here. The 3 engine `.Build.cs` patches we depend on (see
README.md) stay as documented patches to re-apply, not committed
engine files.

## Two-mode backend, env-var contract

The MCP server picks a backend at startup from env:

| Variable                      | Default                            | Purpose |
|-------------------------------|------------------------------------|---------|
| `BP_READER_BACKEND`           | `mock`                             | `mock` \| `commandlet` (use `commandlet` for real BPs) |
| `BP_READER_FIXTURES_DIR`      | `<exe>/fixtures`                   | Mock backend's fixture dir |
| `BP_READER_ENGINE_DIR`        | (unset → fail-fast for commandlet) | Path to source-built engine root (the dir holding `Engine\Binaries\Win64\UnrealEditor-Cmd.exe`) |
| `BP_READER_PROJECT`           | (unset → fail-fast for commandlet) | Path to the `.uproject` |
| `BP_READER_TIMEOUT_SECONDS`   | `120`                              | Per-call subprocess timeout |
| `BP_READER_DAEMON`            | `1` (on)                           | Set `0`/`false`/`no`/`off` to opt out |

For local dev, the mock backend works against a fresh checkout with no
UE setup — useful for iterating on the MCP server itself.

## Build

### MCP server (mock backend, no UE needed)

```pwsh
cd Plugins/BlueprintReader/mcp-server
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
build\tests\Release\bp-reader-tests.exe   # 60 mock cases run; 12 live + 4 soak cases skip by default
```

### UE plugin (needed for the commandlet backend)

The engine must already be built. **Build flags this machine needs:**

```bat
"D:\Projects\Unreal Engine 5\Engine\Build\BatchFiles\Build.bat" ^
  UE5_MCPEditor Win64 Development ^
  -project="D:\Projects\UE5_MCP\UE5_MCP.uproject" ^
  -NoUba -MaxParallelActions=4 -waitmutex
```

`-NoUba -MaxParallelActions=4` is required to fit the build inside a
small page file (UBA allocates large VAS chunks per worker).

After the first full build, **incremental rebuilds are fast** (5–10 s
for plugin-only changes) — UBT's adaptive build excludes the touched
files from the unity cpp and only recompiles what changed.

#### Build invariants

- `Source/UE5_MCPEditor.Target.cs` must declare:
  ```csharp
  DefaultBuildSettings = BuildSettingsVersion.V6;
  BuildEnvironment = TargetBuildEnvironment.Shared;
  ```
- Three engine `.Build.cs` patches (see README.md) for project-target
  builds to resolve their `PrivateIncludePaths` correctly. These live
  in the sibling engine checkout (`D:\Projects\Unreal Engine 5\`),
  which is not tracked by this repo, and must be re-applied after a
  fresh engine clone.
- `BlueprintReaderEditor.Build.cs` private deps:
  `UnrealEd, BlueprintGraph, Json, JsonUtilities, AssetRegistry`.
  `FBlueprintEditorUtils` and `FKismetEditorUtilities` come from
  `UnrealEd` — don't add `Kismet` / `KismetCompiler` (they're
  unrelated modules despite the `Kismet2/` include path).

## Test

### Mock-only (CI-equivalent, fast)

```pwsh
Plugins\BlueprintReader\mcp-server\build\tests\Release\bp-reader-tests.exe
```

60 cases pass in <1 s; 16 auto-skip (12 commandlet-backed without UE
env vars set + 4 soak cases tagged into a separate suite). CI runs
this on every push to `main` that touches
`Plugins/BlueprintReader/mcp-server/**` or the workflow file.
Workflow at `.github/workflows/mcp-server.yml`.

### Soak / large-project simulation (opt-in)

```pwsh
Plugins\BlueprintReader\mcp-server\build\tests\Release\bp-reader-tests.exe --test-suite=soak
```

4 cases, ~22k assertions: 5000 mixed tool calls (read tool surface,
randomized), 1000 each through both framings, large-response handling
via `find_node`, "big project" path diversity simulation. Takes a
few seconds. Run after transport-layer or tool-registry changes.

### Live (drives real `UnrealEditor-Cmd.exe`)

```pwsh
$env:BP_READER_BACKEND     = "commandlet"
$env:BP_READER_ENGINE_DIR  = "D:\Projects\Unreal Engine 5"
$env:BP_READER_PROJECT     = "D:\Projects\UE5_MCP\UE5_MCP.uproject"
Plugins\BlueprintReader\mcp-server\build\tests\Release\bp-reader-tests.exe   # 57 cases, ~80 s
```

JSON-RPC roundtrip smoke test:

```pwsh
pwsh -File Plugins\BlueprintReader\mcp-server\scripts\roundtrip.ps1 `
    -Exe Plugins\BlueprintReader\mcp-server\build\Release\bp-reader-mcp.exe `
    -Asset /Game/AI/BP_TestEnemy
```

### Reseed test BPs

```bat
"D:\Projects\Unreal Engine 5\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" ^
  "D:\Projects\UE5_MCP\UE5_MCP.uproject" ^
  -run=BlueprintReaderSeed -nullrhi -nosplash -unattended -nopause
```

Recreates `Content/AI/BP_TestEnemy.uasset` (5 vars + 2 functions +
event-graph topology) and `BP_TestPickup.uasset`. Required for the
live tests; safe to re-run any time. Commit them after if the seed
output changed.

## Common gotchas (learned the hard way)

- **`FParse::Value` and JSON values.** Anything passed as `-Foo=<json>`
  on a UE commandlet command line gets mangled because the inner
  double-quotes terminate FParse's quoted-string parsing. Pass
  structured args as individual flags instead (`-TypeCategory=`,
  `-TypeSubCategory=`, etc.). Same with empty values — bare `-Query=`
  followed by another `-Flag=value` makes FParse swallow the next token.
  CommandletBlueprintReader skips `-Query=` and `-Default=` etc. when
  the value is empty.

- **UE stdio in commandlet mode.** `FPlatformMisc::LocalPrint` and even
  `fputs(stdout)` can hit UE's redirected log device instead of the
  real stdout pipe in some configs. The daemon's stdio uses
  `GetStdHandle(STD_OUTPUT_HANDLE)` + `WriteFile` directly to bypass
  that. If something writes to the daemon and the MCP-side scanner
  never sees it, this is probably why.

- **Sentinel format.** The daemon emits `__BPR_DONE <code>__\n` after
  each command. **Don't put that string in any log message** — the
  MCP-side marker scanner finds the first occurrence regardless of
  source, so a help line mentioning `__BPR_DONE <code>__` will be
  parsed as `<code>__)` and break the next call.

- **Path resolution.** `Blueprint->GetPathName()` returns object path
  (`/Game/AI/BP_Foo.BP_Foo`). The wire format always uses package paths
  (`/Game/AI/BP_Foo`). `BlueprintReaderWireJson::ToPackagePath` strips
  the suffix; honor that consistently if you add new wire shapes.

- **`UserConstructionScript` vs `ConstructionScript`.** UE 5.7's actual
  graph name for the construction script is `UserConstructionScript`.
  The introspector classifies both names as `WireType="Construction"`
  and the metadata serializer surfaces it as a graph entry rather than
  a function.

- **Function output pins.** `FBlueprintEditorUtils::AddMemberVariable`
  works for variables, but `AddFunctionOutput` had a silent failure
  when no `K2Node_FunctionResult` existed yet (functions without
  outputs don't auto-create one). The plugin spawns one on first
  `add_function_output` call. Same pattern in the seed commandlet.

- **`AddNode` extras tunneling.** The MCP `add_node` schema names the
  extras with the JSON convention (`variable`, `function_owner`,
  `target_class`); the plugin commandlet expects flag-cased names
  (`Variable`, `FunctionOwner`, `TargetClass`). The mapping happens
  in `BlueprintTools.cpp`'s `add_node` handler. Add new kinds to
  both ends.

- **Worktree builds eat hours.** Building the editor target inside a
  fresh git worktree forces UBT to rebuild ~3000 modules including
  the engine, because it can't reuse the parent project's
  intermediates. If you need to test plugin changes from a worktree,
  copy the changed files into the parent project's `Plugins/` and
  build there — incremental rebuilds run in seconds.

## Adding a new tool

The pattern is consistent across all 21 existing tools:

1. **Plugin** (`BlueprintReaderCommandlet.cpp`): add an `EOp` value,
   a `ParseOp` entry, a dispatch line in `RunOneOp`, and a
   `RunFooOp(Params, OutputPath, bPretty)` implementation. Use the
   shared helpers (`LoadMutableBlueprint`, `FindGraphByName`,
   `FindNodeByGuid`, `CompileAndSaveBlueprint`, `EmitOk`).
2. **MCP interface** (`IBlueprintReader.h`): pure virtual.
3. **MockBlueprintReader**: throw `BlueprintReaderError("...mock backend
   is read-only...")` for write tools, or hard-code from fixtures for
   read tools.
4. **CommandletBlueprintReader**: serialize args + call `RunOp`.
   Skip empty optional flags (FParse caveat above).
5. **`BlueprintTools.cpp`**: register the tool with its input schema
   and a handler that pulls args from the JSON and calls the
   `IBlueprintReader` method.
6. **Tests**: a mock case (asserts shape or throws-as-expected) and
   a live case if the op needs a real BP. Use the daemon for live
   tests so they're cheap.
7. **Tool count assertions** in `test_tools.cpp` and `test_mcp.cpp`
   need to be bumped (`spec.size() == N`).

If the new tool is a node-spawning op, also add an entry to
`list_node_kinds` in `BlueprintTools.cpp` — keep the dispatch table and
the discoverability list in lockstep.

## Decisions worth knowing

- **Wire format:** snake_case JSON keys, `BPNode.meta` is a real nested
  object (not a string-of-JSON), `null` for empty optional strings.
  Pinned in `Plugins/BlueprintReader/mcp-server/src/BlueprintReaderTypes.h`.
- **Subprocess management:** `CreateProcessW` directly, no
  `cpp-subprocess` dependency. `cpp-subprocess` is in `vcpkg.json` but
  not consumed.
- **List op:** uses asset registry, not a disk walk. Faster + gets
  `parent_class` from asset tags without loading every BP.
- **Telemetry:** every `tools/call` envelope carries
  `_meta: {elapsed_ms, tool}` per the MCP 2024-11-05 spec extension
  point. Doesn't change content; clients that surface `_meta` see it.

## See also

- [PLAN.md](PLAN.md) — phase history + decisions log.
- [README.md](README.md) — user-facing tool table + Claude Code/Desktop
  config snippet.
- `.claude/skills/bp-reader/SKILL.md` — guidance for *using* the MCP
  tools (this CLAUDE.md is for *maintaining* them).
