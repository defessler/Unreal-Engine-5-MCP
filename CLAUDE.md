# Claude project guidance — UE5_MCP

UE 5.7.4 project plus a standalone MCP server that exposes 39
Blueprint-introspection / mutation / BP↔C++ transpile tools to MCP
clients (Claude Code, Claude Desktop, Copilot, ChatGPT bridge). Two
halves:

- **`Plugins/BlueprintReader/`** — Editor-only UE plugin:
  - `UBlueprintReaderCommandlet` (`-run=BlueprintReader`) — dispatches
    every read + write op (`-Op=List|Read|Graph|Function|Variables|
    Components|Find|AddVariable|...`). Long-lived `-Daemon` mode reads
    newline-delimited commandlet-arg lines from stdin so one editor
    process serves many tool calls.
  - `BlueprintReaderLiveServer` — TCP listener that lets the MCP server
    talk to a running editor without spawning a second commandlet
    daemon. Auto-publishes port + token via
    `<Project>/Saved/bp-reader-live.json`.
  - `UBlueprintReaderSeedCommandlet` (`-run=BlueprintReaderSeed`) —
    synthesizes `Content/AI/BP_TestEnemy.uasset` and `BP_TestPickup.uasset`
    used by the live integration tests.
- **`Plugins/BlueprintReader/mcp-server/`** — Standalone C++20 MCP
  server, vendored inside the plugin so the whole thing ships as one
  unit. JSON-RPC 2.0 over stdio. Backends: `mock` (fixtures only, no
  UE), `commandlet` (drives the plugin via `UnrealEditor-Cmd.exe`),
  `live` (talks to a running editor via TCP), and `auto` (probes per
  call, picks live or commandlet). **Auto is the default** when a
  `.uproject` is auto-discovered. Built automatically as a
  `PreBuildStep` of `BlueprintReader.uplugin` — third-party deps
  (nlohmann_json, fmt, doctest) are vendored under
  `mcp-server/third_party/`, no git/network/vcpkg required.

When you need to **use** the MCP tools to read or modify a blueprint,
the `bp-reader` skill in `.claude/skills/bp-reader/` covers patterns,
the wire format, and per-tool guidance. This file covers building,
testing, and maintaining the project itself.

## Repo layout

```
UE5_MCP/                                      ← project root
├── UE5_MCP.uproject
├── Source/                                     project runtime module
├── Plugins/BlueprintReader/                    plugin ships as one unit
│   ├── BlueprintReader.uplugin                 PreBuildSteps run Build-MCPServer.ps1
│   ├── Scripts/Build-MCPServer.ps1             plugin-driven cmake build
│   ├── Source/BlueprintReaderEditor/
│   │   ├── Public/                             BlueprintReaderTypes, Introspector,
│   │   │                                       WireJson, LiveServer, *Commandlet.h
│   │   └── Private/                            impls
│   └── mcp-server/                             standalone C++20 MCP server
│       ├── src/
│       │   ├── BlueprintReaderTypes.h          wire types (snake_case JSON)
│       │   ├── jsonrpc/                        Server, Mcp (handshake + dispatch)
│       │   ├── tools/                          ToolRegistry, BlueprintTools, Bpir, Decompile
│       │   │   ├── codegen/                    CppEmit, CppClassEmit, UnsupportedTreatment
│       │   │   └── parse/                      CppLex, CppParse (C++ → BPIR)
│       │   └── backends/                       Mock, Commandlet, Live, Auto, Caching, ReadOnly
│       ├── tests/                              doctest cases (mock + live)
│       ├── scripts/                            JSON-RPC + smoke harnesses
│       ├── fixtures/                           BP_*.json mock-backend data
│       ├── third_party/                        vendored deps
│       ├── CMakeLists.txt
│       └── vcpkg.json                          declared but not consumed by default
├── Content/AI/                                 BP_TestEnemy.uasset, BP_TestPickup.uasset
│                                               (regenerable; see "Reseed test BPs" below)
├── README.md                                   user-facing setup + tool table
├── wiki/                                        source of truth for the GitHub Wiki
│                                               (manually pushed to <repo>.wiki.git)
└── .github/workflows/mcp-server.yml            CI (mock-only on windows-2022)
```

The source-built engine is a sibling of this project at
`D:\Projects\Unreal Engine 5\` — outside this repo by design. Three
engine `.Build.cs` patches we depend on stay as documented patches to
re-apply, not committed engine files (see README.md).

## Backend selection

The MCP server picks a backend at startup. Default is `auto` when a
`.uproject` is auto-discovered, `mock` otherwise. Auto re-probes on
every tool call (with a 2s cache) and routes to live (editor open) or
commandlet (editor closed).

| Variable                      | Default                            | Purpose |
|-------------------------------|------------------------------------|---------|
| `BP_READER_BACKEND`           | `auto` (or `mock`)                 | `mock` \| `commandlet` \| `live` \| `auto` |
| `BP_READER_FIXTURES_DIR`      | `<exe>/fixtures`                   | Mock backend's fixture dir |
| `BP_READER_ENGINE_DIR`        | auto-discovered from `.uproject`   | Path to source-built engine root |
| `BP_READER_PROJECT`           | auto-discovered above the exe      | Path to the `.uproject` |
| `BP_READER_LIVE_PORT/TOKEN`   | auto from handshake file           | Editor publishes; MCP server reads |
| `BP_READER_TIMEOUT_SECONDS`   | `120`                              | Per-call subprocess timeout |
| `BP_READER_DAEMON`            | `1` (on)                           | Set `0` to opt out of daemon mode |

For local dev, the mock backend works against a fresh checkout with no
UE setup — useful for iterating on the MCP server itself.

## Build

### MCP server (mock backend, no UE needed)

```pwsh
cd Plugins/BlueprintReader/mcp-server
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
build\tests\Release\bp-reader-tests.exe   # ~350 cases, <5 s
```

### UE plugin (needed for the commandlet / live backends)

The engine must already be built. **Build flags this machine needs:**

```bat
"D:\Projects\Unreal Engine 5\Engine\Build\BatchFiles\Build.bat" ^
  UE5_MCPEditor Win64 Development ^
  -project="D:\Projects\UE5_MCP\UE5_MCP.uproject" ^
  -NoUba -MaxParallelActions=4 -waitmutex
```

`-NoUba -MaxParallelActions=4` is required to fit the build inside a
small page file (UBA allocates large VAS chunks per worker). After the
first full build, incremental rebuilds are fast (5–10 s for plugin-only
changes).

### Build invariants

- `Source/UE5_MCPEditor.Target.cs` must declare:
  ```csharp
  DefaultBuildSettings = BuildSettingsVersion.V6;
  BuildEnvironment = TargetBuildEnvironment.Shared;
  ```
- Three engine `.Build.cs` patches (see README.md) for project-target
  builds to resolve `PrivateIncludePaths` correctly. These live in the
  sibling engine checkout, are not tracked here, and must be re-applied
  after a fresh engine clone.
- `BlueprintReaderEditor.Build.cs` private deps:
  `UnrealEd, BlueprintGraph, Json, JsonUtilities, AssetRegistry,
   Networking, Sockets`. `FBlueprintEditorUtils` and
  `FKismetEditorUtilities` come from `UnrealEd` — don't add `Kismet` /
  `KismetCompiler` (unrelated modules despite the `Kismet2/` include
  path).

## Test

### Mock-only (CI-equivalent, fast)

```pwsh
Plugins\BlueprintReader\mcp-server\build\tests\Release\bp-reader-tests.exe
```

~350 cases pass in <5 s; live cases auto-skip when env vars aren't set.
CI runs this on every push that touches the mcp-server tree.

### Live (drives real `UnrealEditor-Cmd.exe`)

```pwsh
$env:BP_READER_BACKEND     = "commandlet"
$env:BP_READER_ENGINE_DIR  = "D:\Projects\Unreal Engine 5"
$env:BP_READER_PROJECT     = "D:\Projects\UE5_MCP\UE5_MCP.uproject"
Plugins\BlueprintReader\mcp-server\build\tests\Release\bp-reader-tests.exe
```

JSON-RPC + smoke harnesses live under `mcp-server/scripts/`:
`roundtrip.ps1`, `smoke-batch-ops.ps1`, `smoke-decompile.ps1`,
`smoke-transpile-cpp.ps1`, `smoke-cpp-roundtrip.ps1`.

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
  followed by another `-Flag=value` makes FParse swallow the next
  token. The commandlet client skips `-Query=` and `-Default=` etc.
  when the value is empty.

- **UE stdio in commandlet mode.** `FPlatformMisc::LocalPrint` and
  even `fputs(stdout)` can hit UE's redirected log device instead of
  the real stdout pipe in some configs. The daemon's stdio uses
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

The pattern is consistent across all 58 tools:

1. **Plugin** (`BlueprintReaderCommandlet.cpp`): add an `EOp` value,
   a `ParseOp` entry, a dispatch line in `RunOneOp`, and a
   `RunFooOp(Params, OutputPath, bPretty)` implementation. Use the
   shared helpers (`LoadMutableBlueprint`, `FindGraphByName`,
   `FindNodeByGuid`, `CompileAndSaveBlueprint`, `EmitOk`).
2. **MCP interface** (`IBlueprintReader.h`): pure virtual.
3. **MockBlueprintReader**: throw `BlueprintReaderError("...mock backend
   is read-only...")` for write tools, or hard-code from fixtures for
   read tools.
4. **CommandletBlueprintReader / LiveBlueprintReader**: serialize args
   + call `RunOp`. AutoBlueprintReader's forwarders are macro-driven
   and pick this up automatically. Skip empty optional flags (FParse
   caveat above).
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
- **BPIR is the BP↔source pivot.** A versioned JSON AST (`tools/Bpir.h`)
  is what `decompile_function`, `transpile_function`, and
  `parse_cpp_function` operate on. Adding Lua / Python / JS later is
  another codegen + parser pair — same IR.
- **Auto backend probes per call.** Editor open → routes to live;
  editor closed → commandlet. Handshake file at
  `<Project>/Saved/bp-reader-live.json` distributes port + token.
- **Subprocess management:** `CreateProcessW` directly, no
  `cpp-subprocess` dependency. Listed in `vcpkg.json` but not consumed.
- **List op:** uses asset registry, not a disk walk. Faster + gets
  `parent_class` from asset tags without loading every BP.
- **Telemetry:** every `tools/call` envelope carries
  `_meta: {elapsed_ms, tool}` per the MCP 2024-11-05 spec extension
  point.

## See also

- [README.md](README.md) — user-facing setup + tool table + Claude /
  Copilot config snippets.
- [wiki/](wiki/) — source of truth for the GitHub Wiki (manually
  pushed to `<repo>.wiki.git` on doc changes).
- `.claude/skills/bp-reader/SKILL.md` — guidance for *using* the MCP
  tools (this CLAUDE.md is for *maintaining* them).
