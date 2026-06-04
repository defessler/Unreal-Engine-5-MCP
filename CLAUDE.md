# Claude project guidance — UE5_MCP

**Project state.** This git repository tracks the **`BlueprintReader`
plugin + docs only** — not a full game project. Development happens
inside a **local, untracked** Lyra Starter Game build host
(`LyraStarterGame.uproject`, `Source/Lyra*`, `Plugins/LyraGenerated`
and the other Lyra plugins) on UE 5.8 that lives on the maintainer's
disk but is *not* in the repo — a fresh clone gets the plugin only.
Build/test commands below target `LyraEditor` / `LyraStarterGame.uproject`
because that's the maintainer's local host; substitute your own editor
target + `.uproject` to build elsewhere. The Lyra BP→C++ conversion
work (302 companion classes, recipe + lessons-learned) is documented in
[`docs/research/lyra-bp-to-cpp-conversion.md`](docs/research/lyra-bp-to-cpp-conversion.md).
The old `setup.bat` + Lyra asset-restoration flow is no longer in the
repo (kept locally only; see "Asset restoration" below).

A standalone MCP server that exposes a large set of
Blueprint-introspection / mutation / BP↔C++ transpile / editor-control
tools (exact count + full catalog: generated `docs/TOOLS.md`) to MCP
clients (Claude Code, Claude Desktop, Copilot, ChatGPT bridge). Two
halves:

- **`Plugins/BlueprintReader/`** — two-module UE plugin (correct
  isolation: `Type:"Editor"` modules are stripped from non-editor
  targets by UBT automatically; runtime module ships in both):
  - **`BlueprintReaderEditor`** (Type=Editor) — full BP introspection
    + write tools. Loaded only in editor builds.
    - `UBPRCommandlet` (`-run=BPR`) — dispatches
      every read + write op (`-Op=List|Read|Graph|Function|Variables|
      Components|Find|AddVariable|...`). Long-lived `-Daemon` mode reads
      newline-delimited commandlet-arg lines from stdin so one editor
      process serves many tool calls.
    - `BlueprintReaderLiveServer` — TCP listener that lets the MCP server
      talk to a running editor without spawning a second commandlet
      daemon. Auto-publishes port + token via
      `<Project>/Saved/bp-reader-live.json`.
    - `UBPRSeedCommandlet` (`-run=BPRSeed`) —
      synthesizes `Content/AI/BP_TestEnemy.uasset` and
      `BP_TestPickup.uasset` used by the live integration tests.
  - **`BlueprintReaderRuntime`** (Type=Runtime) — read-only BP
    introspection via UClass reflection. Loads in editor AND packaged
    builds. Reads asset-registry entries, parent class, interfaces,
    UPROPERTY-reflected variables (with CDO defaults), UFUNCTION
    signatures, components from SCS / CDO. Cannot read source-level
    K2 graphs (stripped during cook) — `graphs[]` returns empty.
    Wired through two console commands for in-game triage:
    `bp_reader.list <Path>` and `bp_reader.read <AssetPath>`.
- **`Plugins/BlueprintReader/Tests/BlueprintReaderMcp*/`** — C++20
  MCP server as a pair of UE Program targets (BlueprintReaderMcp,
  BlueprintReaderMcpCore, BlueprintReaderMcpTests). JSON-RPC 2.0 over
  stdio. Backends: `mock` (fixtures only, no UE), `commandlet` (drives
  the plugin via `UnrealEditor-Cmd.exe`), `live` (talks to a running
  editor via TCP), and `auto` (probes per call, picks live or
  commandlet). **Auto is the default** when a `.uproject` is
  auto-discovered. Built via UBT (same pipeline + UBA cache as the
  editor target); third-party deps (nlohmann_json, fmt, doctest) are
  vendored under `Tests/ThirdParty/`. The "Tests/" dir name is a UBT
  artifact — only path UBT scans for plugin-hosted Program Target.cs
  files; BlueprintReaderMcp itself is a production exe.

When you need to **use** the MCP tools to read or modify a blueprint,
the `bp-reader` skill in `.claude/skills/bp-reader/` covers patterns,
the wire format, and per-tool guidance. This file covers building,
testing, and maintaining the project itself.

**Source of truth for skills + agents:**
`Plugins/BlueprintReader/Claude/{agents,skills}/` ships with the
plugin. The `.claude/` at the project root is a deployed copy —
maintain by editing the plugin folder and running
`Plugins/BlueprintReader/Scripts/Install-ClaudeAssets.ps1` (or
`install-claude-assets.sh` on Unix). Keeping skills next to the code
means a fresh plugin pull always brings matching docs.

## Repo layout

```
UE5_MCP/                                      ← repo root (tracks plugin + docs only)
├── LyraStarterGame.uproject                     (untracked — local build host)
├── Source/LyraGame, LyraEditor                  (untracked — local build host)
├── Plugins/LyraGenerated/                       (untracked — local build host: 302 BP→C++ companions)
├── Plugins/BlueprintReader/                    plugin ships as one unit (tracked)
│   ├── BlueprintReader.uplugin
│   ├── Scripts/Build-MCPServer.ps1             UBT-wrapper convenience script
│   ├── Source/BlueprintReaderRuntime/          runtime BP introspection (loads in cooked builds)
│   │   ├── Public/                             BlueprintRuntimeIntrospector, JSON output
│   │   └── Private/                            impls + bp_reader.{list,read} console cmds
│   ├── Source/BlueprintReaderEditor/
│   │   ├── Public/                             BlueprintReaderTypes, Introspector,
│   │   │                                       WireJson, LiveServer, *Commandlet.h
│   │   └── Private/                            impls
│   └── Tests/                                  UE Program targets (UBT-built)
│       ├── BlueprintReaderMcp/                 main exe → BlueprintReaderMcp.exe
│       │   ├── BlueprintReaderMcp.{Target,Build}.cs
│       │   └── Private/main.cpp
│       ├── BlueprintReaderMcpCore/             shared static-lib module (impl)
│       │   ├── BlueprintReaderMcpCore.Build.cs
│       │   └── Private/
│       │       ├── BlueprintReaderTypes.h      wire types (snake_case JSON)
│       │       ├── jsonrpc/                    Server, Mcp (handshake + dispatch)
│       │       ├── tools/                      ToolRegistry, BlueprintTools, Bpir, Decompile
│       │       │   ├── codegen/                CppEmit, CppClassEmit, UnsupportedTreatment
│       │       │   └── parse/                  CppLex, CppParse (C++ → BPIR)
│       │       └── backends/                   Mock, Commandlet, Live, Auto, Caching, ReadOnly
│       ├── BlueprintReaderMcpTests/            doctest suite → BlueprintReaderMcpTests.exe
│       │   ├── BlueprintReaderMcpTests.{Target,Build}.cs
│       │   ├── Private/                        859+ cases (mock + live)
│       │   └── fixtures/                       BP_*.json mock-backend data
│       └── ThirdParty/                         vendored: nlohmann_json, fmt, doctest
├── Content/AI/                                 BP_TestEnemy.uasset, BP_TestPickup.uasset (tracked)
│                                               (regenerable; see "Reseed test BPs" below)
│                                               Other Lyra Content/ is NOT tracked — it lives
│                                               in the local build host only (the legacy
│                                               setup.bat restoration flow is no longer in the repo).
├── README.md                                   user-facing setup + tool table
└── wiki/                                       source of truth for the GitHub Wiki
                                                (manually pushed to <repo>.wiki.git)
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
| `BP_READER_DAEMON_IDLE_SECONDS` | `300`                            | Daemon self-exits after this many seconds with zero active connections (orphan cleanup). Min 5s. |
| `BP_READER_DAEMON_MAX_LIFETIME_SECONDS` | `0` (off)                | Hard max wall-clock lifetime backstop — daemon self-exits after this long regardless of activity. Guards against a wedged connection that keeps idle-shutdown from ever firing. Off by default. |
| `BP_READER_READ_ONLY`         | `1` (on, **default**)              | Read-only by default — every write tool is rejected with a clear error. Guards against two processes mutating the same `.uasset` concurrently. Set `0` (or `BP_READER_ALLOW_WRITE=1`) to enable writes. |
| `BP_READER_ALLOW_WRITE`       | `0` (off)                          | Discoverable inverse of `BP_READER_READ_ONLY` — set `1` to enable write tools. An explicit `BP_READER_READ_ONLY` wins if both are set. |
| `BP_READER_ALLOW_TRANSPILE`  | `0` (off)                          | Set `1` to enable the 6 BP↔C++ transpile tools (off by default). |
| `BP_READER_AUTO_CHECKOUT`    | `1` (on)                           | Before a write op mutates an asset, auto-check-it-out of source control (Perforce/Git) non-interactively. Stops a live editor from popping a blocking "Check Out?" modal mid-call. Set `0` to manage checkouts yourself. |
| `BP_READER_PLUGIN_DENYLIST`  | (empty)                            | Comma-separated plugin names; each becomes a `-DisablePlugin=<name>` arg on the headless editor spawn. Use to skip a plugin that crashes in `StartupModule` (e.g. DLSS) and would otherwise kill the commandlet before handshake. Caveat: `-DisablePlugin` does **not** override `.uproject`-force-enabled plugins. |

For local dev, the mock backend works against a fresh checkout with no
UE setup — useful for iterating on the MCP server itself.

## Build

The MCP server is now a UE Program target — it builds via UBT
alongside the rest of the plugin. The plugin's `.uplugin` carries a
`PreBuildSteps` hook (see `Plugins/BlueprintReader/Scripts/PreBuildHook.ps1`)
that invokes UBT for `BlueprintReaderMcp` before any consuming target's
own build runs, so the first command below builds both the editor
module *and* the server in one invocation. The doctest binary remains
explicit (heavier, less often needed):

```bat
:: Editor (plugin DLLs) + MCP server exe (via plugin PreBuildStep):
"D:\Projects\Unreal Engine 5\Engine\Build\BatchFiles\Build.bat" ^
  LyraEditor Win64 Development ^
  -project="D:\Projects\UE5_MCP\LyraStarterGame.uproject" ^
  -NoUba -MaxParallelActions=4 -waitmutex

:: MCP server exe in isolation (e.g. when iterating on server-only changes):
"D:\Projects\Unreal Engine 5\Engine\Build\BatchFiles\Build.bat" ^
  BlueprintReaderMcp Win64 Development ^
  -project="D:\Projects\UE5_MCP\LyraStarterGame.uproject" ^
  -NoUba -MaxParallelActions=4 -waitmutex

:: doctest suite (859+ cases) — not pulled in automatically:
"D:\Projects\Unreal Engine 5\Engine\Build\BatchFiles\Build.bat" ^
  BlueprintReaderMcpTests Win64 Development ^
  -project="D:\Projects\UE5_MCP\LyraStarterGame.uproject" ^
  -NoUba -MaxParallelActions=4 -waitmutex
```

Set `BP_READER_SKIP_PREBUILD=1` in the build environment to skip the
MCP server side and rebuild just the editor module.

Or both Program targets at once via the wrapper script:

```pwsh
Plugins\BlueprintReader\Scripts\Build-MCPServer.ps1 `
  -EngineDir "D:\Projects\Unreal Engine 5" `
  -ProjectFile "D:\Projects\UE5_MCP\LyraStarterGame.uproject" `
  -ExtraArgs "-NoUba -MaxParallelActions=4"
```

`-NoUba -MaxParallelActions=4` is required on this machine to fit the
build inside a small page file (UBA allocates large VAS chunks per
worker). Drop those flags on machines with normal page files to get
UBA acceleration. After the first full build, incremental rebuilds are
fast (5–10 s for plugin-only changes; ~10 s for an MCP-server-only
incremental rebuild).

### Installed-engine fallback (CMake) for the MCP Program targets

An **installed / Launcher engine** (e.g. a `D:\…\Epic Games\UE_5.8`
binary install) **refuses to build UE Program targets at all** — UBT
fails with `Program targets are not currently supported from this
engine distribution`. That blocks `BlueprintReaderMcp` and
`BlueprintReaderMcpTests` (both `Type=Program`) via UBT, even though
the editor plugin modules build fine. The server + tests are pure
standalone C++20 (`bCompileAgainstEngine=false`, vendored
`json`/`fmt`/`doctest`) and need no engine, so on installed-engine
hosts build them directly with CMake/Ninja + the system MSVC toolchain,
bypassing UBT:

```pwsh
Plugins\BlueprintReader\Scripts\..\..\..\Saved\build-mcp-cmake.ps1   # local helper, or:
# cmake -S Plugins/BlueprintReader/Tests -B Saved/mcp-build -G Ninja
# cmake --build Saved/mcp-build
```

`Plugins/BlueprintReader/Tests/CMakeLists.txt` (recovered + remapped
from the pre-UBT CMake build, removed in commit `74ac475f`) GLOBs the
sources and emits `BlueprintReaderMcp.exe` + `BlueprintReaderMcpTests.exe`
into the plugin's own `Plugins/BlueprintReader/Binaries/Win64` (so the server
ships with the plugin; the UBT path mirrors its `<Project>/Binaries/Win64`
output there too). The one non-obvious flag is **`/Zc:preprocessor`**
(the conforming preprocessor, which UBT enables by default): the legacy
MSVC preprocessor mis-tokenizes the raw-string literals
(`R"(...\"...)"`) that `test_cpp_class.cpp` passes as doctest `CHECK`
macro arguments, so without it the test build fails with
`C2017/C3688/C2661`. The resulting exes are engine-version-independent —
a binary built this way drives a UE 5.8 editor/daemon exactly as a
UBT-built one would. On a **source engine**, prefer the documented UBT
path above (the CMake build is the installed-engine fallback, not a
replacement).

### Build invariants

- `Source/LyraEditor.Target.cs` must declare:
  ```csharp
  DefaultBuildSettings = BuildSettingsVersion.V6;
  BuildEnvironment = TargetBuildEnvironment.Unique;
  // Lyra requires Unique because of its warning-override settings.
  // When using Unique env, launch LyraEditor-Cmd.exe (not UnrealEditor-Cmd.exe)
  // so the matching LyraEditor-*.dll plugin modules load.
  ```
  **Installed-engine variant.** A binary/Launcher engine forbids a
  Unique build environment (`Targets with a unique build environment
  cannot be built with an installed engine`). On an installed engine,
  drop `BuildEnvironment = Unique` and instead add
  `bOverrideBuildEnvironment = true;` *after*
  `ApplySharedLyraTargetSettings(this)` (Lyra's warning-level overrides
  otherwise trip `modifies shared settings … not allowed`). With the
  Shared env the project module DLLs are built as
  `UnrealEditor-LyraGame.dll` / `UnrealEditor-LyraEditor.dll` and loaded
  by the **engine's** `UnrealEditor-Cmd.exe` — there is no
  `LyraEditor-Cmd.exe`, so launch `<Engine>/Binaries/Win64/UnrealEditor-Cmd.exe`
  with the `.uproject`. (`LyraEditor.Target.cs` is part of the untracked
  local build host, so this is host config, not a tracked change.)
  The MCP server auto-build is handled inside the plugin via
  `BlueprintReader.uplugin`'s `PreBuildSteps` block (which invokes
  `Plugins/BlueprintReader/Scripts/PreBuildHook.ps1`). No project-side
  wiring needed — drop the plugin into another `.uproject` and the
  editor build pulls the server along.
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
- **Multi-engine API compatibility.** The plugin is built against more than
  one UE version (a pre-5.8 engine *and* UE 5.8), so an engine API present in
  5.8 may not exist on the older one. Guard version-specific engine APIs with
  `UE_VERSION_OLDER_THAN(major, minor, patch)` / `UE_VERSION_NEWER_THAN`
  (`#include "Misc/EngineVersionComparison.h"`). In particular, do **not**
  "fix" a 5.8 deprecation *warning* by switching to the new-only replacement
  API — that turns a harmless warning into a hard build *error* on the older
  engine (e.g. `EGetObjectsFlags` and `FViewMatrices::GetWorldToClip` are
  5.8+; this regressed a downstream build in #223 and was fixed with version
  guards in #240). **The hosted CI (`mcp-tests.yml`) does not compile the
  editor module** — it builds only the standalone MCP server + mock suite via
  CMake with no engine, so editor-module breaks pass *that* CI silently.
  `.github/workflows/editor-build.yml` is a **self-hosted** scaffold that
  compile-smokes the editor module against a real engine (register a runner
  labelled `ue5` + set the `UE_ENGINE_DIR`/`UE_PROJECT`/`UE_EDITOR_TARGET`
  vars; one runner per targeted engine version covers the multi-engine
  invariant). Until a runner is provisioned, a local editor build on *each*
  targeted engine version remains the only guard.

## Test

### Mock-only (fast)

```pwsh
Plugins\BlueprintReader\Binaries\Win64\BlueprintReaderMcpTests.exe
```

859+ cases / 34000+ assertions pass in <5 s; the live-only cases
auto-skip when env vars aren't set.

CI (`.github/workflows/mcp-tests.yml`) builds the MCP server + doctest
suite via the engine-free CMake fallback on `windows-latest` and runs the
mock suite + the tool-catalog drift check (`Dump-Tools.ps1 -Check`) on any
`Tests/**` change. It needs no engine (the server + tests are standalone
C++20). A UBT-based CI for the editor module would need a runner with the
engine source — not set up.

### Live (drives real `UnrealEditor-Cmd.exe`)

```pwsh
$env:BP_READER_BACKEND     = "commandlet"
$env:BP_READER_ENGINE_DIR  = "D:\Projects\Unreal Engine 5"
$env:BP_READER_PROJECT     = "D:\Projects\UE5_MCP\LyraStarterGame.uproject"
Plugins\BlueprintReader\Binaries\Win64\BlueprintReaderMcpTests.exe
```

The legacy smoke scripts (`roundtrip.ps1`, `smoke-batch-ops.ps1`, etc.)
were removed alongside the CMake build — the doctest suite covers their
ground. For an interactive smoke, stream JSON-RPC frames against
`BlueprintReaderMcp.exe`'s stdin/stdout directly.

### Reseed test BPs

```bat
"D:\Projects\Unreal Engine 5\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" ^
  "D:\Projects\UE5_MCP\LyraStarterGame.uproject" ^
  -run=BPRSeed -nullrhi -nosplash -unattended -nopause
```

Recreates `Content/AI/BP_TestEnemy.uasset` (5 vars + 2 functions +
event-graph topology) and `BP_TestPickup.uasset`. Required for the
live tests; safe to re-run any time. Commit them after if the seed
output changed.

## Common gotchas (learned the hard way)

- **Git Bash / MSYS path translation.** Invoking `UnrealEditor-Cmd.exe`
  directly from Git Bash with `-Asset=/Game/AI/BP_Foo` gets the
  path rewritten to `C:/Program Files/Git/Game/AI/BP_Foo` by MSYS
  (treats `/Game/...` as a Unix absolute path). UE then rejects the
  package name. Set `MSYS_NO_PATHCONV=1` for the call, or run from
  PowerShell / cmd.exe. The MCP server uses `CreateProcessW`
  directly and isn't affected — this only bites when invoking the
  commandlet by hand.

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

- **`UserConstructionScript` vs `ConstructionScript`.** UE 5.8's actual
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

The pattern is consistent across every tool (current count + catalog:
generated `docs/TOOLS.md`, regenerated by `Scripts/Dump-Tools.ps1`):

1. **Plugin** (`BlueprintReaderCommandlet.cpp`): add an `EOp` value,
   a `ParseOp` entry, a dispatch line in `RunOneOp`, and a
   `RunFooOp(Params, OutputPath, bPretty)` implementation. Use the
   shared helpers (`LoadMutableBlueprint`, `FindGraphByName`,
   `FindNodeByGuid`, `CompileAndSaveBlueprint`, `EmitOk`).
2. **MCP interface** (`IBlueprintReader.h`): pure virtual.
3. **MockBlueprintReader**: throw `BlueprintReaderError("...mock backend
   is read-only...")` for write tools, or hard-code from fixtures for
   read tools.
4. **Backends + decorators** — every layer of the
   `ReadOnly → Caching → Auto → (Socket | Commandlet | Mock)` chain must
   handle the new method, or the **default `auto` backend** dispatches to
   `IBlueprintReader`'s *throwing default* (`"X not supported by this
   backend"`) even though a real backend implements it. There is **no
   compile-time guard** and **no auto-generation** — each is a
   hand-written `override`:
   - **CommandletBlueprintReader + SocketBlueprintReader** (the
     `commandlet` / `live` backends): serialize args + call `RunOp`.
     Skip empty optional flags (FParse caveat above).
   - **AutoBlueprintReader**: one `override` whose body is
     `FORWARD(Method, args)` (or `FORWARD_VOID` for `void` returns).
   - **CachingBlueprintReader**: pass-through `return inner_->Method(...)`
     for reads/live ops; forward + `InvalidateAsset(path)` for `.uasset`
     writes.
   - **ReadOnlyBlueprintReader**: pass-through for reads; `Reject("tool_
     name")` for `.uasset` / persistent-settings writes.

   Spot-check after adding (each must be ≥ 1):
   `grep -c "AutoBlueprintReader::<Method>" AutoBlueprintReader.cpp`
   (likewise `Caching`/`ReadOnly`). Skipping this is what left material /
   data-table / widget / actor / console / etc. throwing "not supported"
   on the default backend despite working on raw commandlet/live.
5. **`BlueprintTools.cpp`**: register the tool with its input schema
   and a handler that pulls args from the JSON and calls the
   `IBlueprintReader` method.
6. **Tests**: a mock case (asserts shape or throws-as-expected) and
   a live case if the op needs a real BP. Use the daemon for live
   tests so they're cheap.
7. **Tool count assertions** in `test_tools.cpp` and `test_mcp.cpp`
   need to be bumped (`spec.size() == N`).
8. **Regenerate the tool catalog**: run
   `Plugins/BlueprintReader/Scripts/Dump-Tools.ps1` to refresh
   `docs/TOOLS.md` + `docs/tools.json`, and commit them — CI's
   `Dump-Tools.ps1 -Check` step fails the build otherwise. (The catalog,
   not any hand-typed number, is the single source of truth for the tool
   count — docs/README/AGENTS defer to it.)

If the new tool is a node-spawning op, also add an entry to
`list_node_kinds` in `BlueprintTools.cpp` — keep the dispatch table and
the discoverability list in lockstep.

## Decisions worth knowing

- **Wire format:** snake_case JSON keys, `BPNode.meta` is a real nested
  object (not a string-of-JSON), `null` for empty optional strings.
  Pinned in `Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/BlueprintReaderTypes.h`.
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

## Improvement roadmap

[`docs/research/improvement-roadmap.md`](docs/research/improvement-roadmap.md)
is the **live, status-tracked backlog** of plugin improvements (Epic-MCP
parity, ease-of-use, transpiling, install/distribution). It is a living
document, not a wishlist — keep it current as part of the work:

- **When you ship an improvement**, flip that item's `Status:` to
  `✅ Done (PR #N, YYYY-MM-DD)` and add a line to its revision log. Don't
  delete the item — a done row with its PR is the record.
- **When you discover a new improvement** (from client feedback, a review, a
  new Epic release, a bug), add it under the right section with a **new stable
  ID** and `Status: ☐ Open`, matching the existing Status/Effort/file/why shape.
- IDs are stable and never reused; abandoned items become `✖ Dropped (reason)`.

The doc's own "Maintaining this roadmap" section has the full protocol.

## Asset restoration (legacy — no longer in the repo)

The Lyra asset-restoration flow — `setup.bat` at the old project root,
`Scripts/lyra-assets-manifest.json`, `Scripts/Publish-LyraAssetsRelease.ps1`,
the `-Source` / `-Repair` / `-Clean` / `-VerifyOnly` flags, and the
`lyra-assets-vN` GitHub Release bundle — is **no longer tracked in this
repo**. It lives only in the maintainer's local build host. Now that the
repo tracks the plugin + docs (not a Lyra project), a fresh clone has no
Lyra content to restore: you bring your own UE 5.8 host project and mount
`Plugins/BlueprintReader/` into its `Plugins/`. The only tracked content
is the two test BPs under `Content/AI/` (regenerable via `-run=BPRSeed`).

Any historical references to `setup.bat` in older docs/plans under
`docs/` are kept for provenance and do not apply to a fresh clone.

## Publishing the wiki

`wiki/` is the in-repo source of truth, but the live GitHub Wiki is a
**separate git repo** (`<repo>.wiki.git`) that nothing auto-syncs — push it
by hand after wiki changes land on `main`. Page files map 1:1 by name
(`wiki/Home.md` → the **Home** page, `wiki/Tool-Reference.md` →
**Tool-Reference**, …); the wiki's default branch is `master`.

```pwsh
$tmp = "$env:TEMP\bpr-wiki"
Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
git clone https://github.com/defessler/Unreal-Engine-5-MCP.wiki.git $tmp
Copy-Item wiki\*.md $tmp -Force
git -C $tmp add -A
git -C $tmp commit -m "Sync from repo: <what changed>"
git -C $tmp push origin master
```

There is no CI on the wiki repo, so **verify rendering manually** afterward —
open a couple of changed pages on GitHub (tables, code fences, and
`(#anchor)` links are the usual breakage). `docs/TOOLS.md` is generated and
ships in the repo, not the wiki.

## See also

- [README.md](README.md) — user-facing setup + tool table + Claude /
  Copilot config snippets.
- [wiki/](wiki/) — source of truth for the GitHub Wiki (manually
  pushed to `<repo>.wiki.git` on doc changes — see [Publishing the
  wiki](#publishing-the-wiki) for the push recipe).
- `Plugins/BlueprintReader/Claude/` — skills + agents that ship with
  the plugin (this is the source-of-truth for those files; `.claude/`
  at the project root is a deployed copy synced via the install
  script).
- `Plugins/BlueprintReader/Claude/skills/bp-reader/SKILL.md` —
  guidance for *using* the MCP tools (this CLAUDE.md is for
  *maintaining* them).
