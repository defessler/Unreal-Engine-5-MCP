# Chapter 13 — Production polish

You have a working MCP server with four backends, write ops, batching,
a long-lived editor daemon, a live TCP transport, auto-routing, and
a BP-to-source pipeline. The architecture is done. This chapter is
the polish — the work that turns "demonstrably running" into "you
can hand it to a teammate and they don't bounce off rough edges
within five minutes."

## Testing

The mock fixtures from Chapter 2 are the backbone. They let us run
the entire server logic end-to-end without UE installed, on every
push, in CI, in under five seconds.

### doctest

The test framework is doctest — one vendored header
(`third_party/doctest/doctest.h`), zero build system integration,
written in C++ that looks like ordinary code:

```cpp
TEST_CASE("list_blueprints returns fixture summaries") {
    auto reader = MakeMockReader();
    auto bps = reader->ListBlueprints("/Game/AI");
    REQUIRE(bps.size() == 2);
    CHECK(bps[0].asset_path == "/Game/AI/BP_TestEnemy");
}
```

The test binary is `bp-reader-tests.exe`. Roughly 440 cases. They
all pass on a clean checkout with no environment setup; the live
tests detect missing env vars and skip cleanly:

```cpp
TEST_CASE("live backend round-trips ListBlueprints" * doctest::skip(
    std::getenv("BP_READER_PROJECT") == nullptr))
{
    // ... requires a running editor or commandlet
}
```

`doctest::skip(condition)` is the right tool for environment-gated
tests. The case still appears in the output but its body never
runs, so we don't get false greens from tests that silently no-op
on missing env vars.

### Test layout

The tests live in `mcp-server/tests/` next to the server source:

```
tests/
├── test_main.cpp                    # doctest harness entry
├── test_helpers.h                   # MakeMockReader, fixture helpers
├── test_jsonrpc.cpp                 # protocol layer
├── test_mcp.cpp                     # tools/list, tools/call envelopes
├── test_tools.cpp                   # per-tool input schema + handlers
├── test_apply_ops.cpp               # batch operation semantics
├── test_caching_reader.cpp          # cache hit/miss, TTL, invalidation
├── test_auto_backend.cpp            # probe state machine
├── test_commandlet_backend.cpp      # arg encoding, daemon transport
├── test_live_backend.cpp            # framing, handshake, refresh
├── test_decompile.cpp               # graph → BPIR walker
├── test_bpir.cpp                    # schema validation
├── test_cpp_codegen.cpp             # BPIR → C++ readable mode
├── test_cpp_class.cpp               # whole-class generation
├── test_cpp_lex.cpp                 # token stream
├── test_cpp_parse.cpp               # C++ → BPIR
├── test_diagnostics.cpp             # classified errors
├── test_framing_stress.cpp          # partial reads, multi-frame packs
└── ...
```

One file per concern. Tests are self-contained: a test that needs a
fixture builds it inline, asserts on it, and lets the destructor
clean up.

### Tool count assertion

The test for `tools/list` pins the exact tool count:

```cpp
TEST_CASE("MCP server registers expected tool count") {
    auto spec = BuildToolSpec();
    REQUIRE(spec.size() == 119);  // bump when adding a new tool
}
```

This catches the "forgot to register the new tool" failure mode at
test time, not at first-use time. The CLAUDE.md "Adding a new tool"
checklist explicitly calls out bumping the assertion.

## Error handling polish

The diagnostics chapter belongs in the design docs
([design/08-error-diagnostics.md](../design/08-error-diagnostics.md)),
but a few patterns deserve a tutorial pass.

### Classified write failures

When a write op fails, the user wants to know **why** more than
they want the raw error code. A handful of common failure modes
each have specific diagnostic paths:

**File-lock probe.** Before reporting an opaque "save failed" the
commandlet probes the on-disk `.uasset` with raw `CreateFileW`:

```cpp
HANDLE h = CreateFileW(WidePath.c_str(),
    GENERIC_READ | GENERIC_WRITE,
    0,                       // no share -> reveals exclusive locks
    nullptr, OPEN_EXISTING,
    FILE_ATTRIBUTE_NORMAL, nullptr);
if (h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION) {
    // The asset is currently held open by another process — most
    // commonly the editor the user has the project loaded in. Surface
    // this distinct from "the asset doesn't exist" or "permissions."
    return ClassifyFailure::FileLocked;
}
CloseHandle(h);
```

If the probe reveals an exclusive lock by another process, the
error message names the likely holder ("Editor is open — close it
or use BP_READER_BACKEND=live") rather than just "save failed."

**Non-Blueprint asset.** When the user passes `/Game/AI/Foo` and
`Foo` is a Material, not a Blueprint, we detect this at load time
and emit a hint:

```
"Asset /Game/AI/Foo exists but is a UMaterial, not a UBlueprint.
 Did you mean to call list_materials or read_material instead?"
```

**Uncompiled parent class.** A Blueprint whose parent class hasn't
been compiled (or whose parent module isn't loaded) fails to load
with an opaque inner UE error. We catch that case and add the hint
about rebuilding the parent.

### Loading hygiene

When the commandlet loads a Blueprint for a read op, two flags suppress
expected warnings:

```cpp
UPackage* Package = LoadPackage(
    nullptr, *PackagePath, LOAD_NoWarn | LOAD_Quiet);
```

`LOAD_NoWarn | LOAD_Quiet` keeps "missing import" / "missing
referenced asset" warnings from polluting the daemon's stdout. The
daemon's stdout is the sentinel channel; every line UE writes adds
work to the MCP-side scanner and a real warning would otherwise
interleave with our `__BPR_DONE` sentinels.

### GC anchor pattern

Mutating a `UBlueprint` from a commandlet involves multiple steps
(`AddMemberVariable`, recompile, save). Between those steps, UE can
run GC if anything triggers it. Without a GC anchor, our local
pointer to the BP gets invalidated:

```cpp
UBlueprint* BP = Cast<UBlueprint>(LoadObject<UObject>(/*...*/));

// Anchor against GC for the duration of the write. Without this, an
// AddMemberVariable that internally creates and discards transient
// UObjects can trigger a collection that invalidates our BP pointer.
TStrongObjectPtr<UBlueprint> Anchor(BP);

FBlueprintEditorUtils::AddMemberVariable(BP, /*...*/);
FKismetEditorUtilities::CompileBlueprint(BP);
SavePackage(BP->GetPackage(), /*...*/);
```

`TStrongObjectPtr` is the GC root for the duration. Pattern lives
in every write op that does more than one mutation.

## Single-instance MCP server lock

Two MCP-server instances against the same project means two daemons
fighting over the same `.uasset` files. `SingleInstanceLock` enforces
"one bp-reader-mcp.exe per .uproject":

```cpp
// SingleInstanceLock.h
class SingleInstanceLock {
public:
    explicit SingleInstanceLock(const std::filesystem::path& projectPath);
    ~SingleInstanceLock();
    bool IsHeld() const { return held_; }
    const std::filesystem::path& LockPath() const { return lockPath_; }
    std::optional<int> OwnerPid() const;
};
```

Implementation:

- Lock file at `<system-temp>/bp-reader-mcp-<hash>.lock`. The
  `<hash>` is an **FNV-1a** of the absolute project path —
  different projects get different locks; the same project always
  gets the same lock.
- Windows: `CreateFileW` with `dwShareMode = 0` (exclusive open).
- POSIX: `open()` + `flock(LOCK_EX | LOCK_NB)`.
- Crash-safe: when the process exits (clean or via OS termination)
  the kernel releases the handle. No stale-lock cleanup needed; the
  next instance just picks it up.

Used at `main.cpp` startup:

```cpp
util::SingleInstanceLock lock(uprojectPath);
if (!lock.IsHeld()) {
    std::cerr << "another bp-reader-mcp is using " << lock.LockPath();
    if (auto pid = lock.OwnerPid()) {
        std::cerr << " (pid=" << *pid << ")";
    }
    std::cerr << "\n";
    return 1;
}
```

## Stable editor port across relaunches

Chapter 10 mentioned the persistent port cache and `SO_REUSEADDR` in
passing. The point: an MCP client that learned port 53413 last time
shouldn't have to re-read the handshake file every time the user
restarts the editor.

```cpp
// FLiveServer::Start
// SO_REUSEADDR before bind: lets us reclaim the same port immediately
// after a previous editor instance exits, even while its socket is
// still in TIME_WAIT (~30-60 s window).
OutSocket->SetReuseAddr(true);
```

Plus a sidecar file `<Project>/Saved/bp-reader-live-port.json` that
records the last successfully-bound port:

```cpp
const FString Json = FString::Printf(TEXT("{\"port\":%d}\n"), Port);
FFileHelper::SaveStringToFile(Json, *Path, /* ... */);
```

On the next launch, `Start` tries the cached port first; if it
binds, we keep the same port across the entire editor relaunch.
Token is still freshly generated, but the inner-layer self-refresh
on `LiveBlueprintReader` (Chapter 10) handles that transparently.

## Strict warnings as errors

The CMakeLists.txt promotes the four "you forgot to use a thing"
warnings to errors on MSVC:

```cmake
if(MSVC)
    add_compile_options(/W4 /permissive- /Zc:__cplusplus /utf-8 /EHsc)
    # Promote unused-variable / unused-parameter warnings to errors so
    # they can't slip through into downstream consumers' build logs.
    #   C4100: unreferenced formal parameter
    #   C4101: unreferenced local variable
    #   C4189: local variable initialized but not used
    #   C4505: unreferenced local function has been removed
    add_compile_options(/we4100 /we4101 /we4189 /we4505)
    add_definitions(-DNOMINMAX -D_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-Wall -Wextra
        -Werror=unused-parameter
        -Werror=unused-variable
        -Werror=unused-function)
endif()
```

The motivating case: `DecompileStatement` compiled clean here at
`/W4` but surfaced as a warning in a downstream consumer's build
(different warning configuration). Now `/we4100` makes the local
build fail, forcing the fix on this side rather than letting it
slip through to consumers' build logs.

## Telemetry

Every `tools/call` envelope carries a `_meta` block on response.
The Mcp dispatcher wraps the result:

```cpp
nlohmann::json env = MakeToolResponse(toolName, std::move(result));
nlohmann::json meta = {
    {"tool", toolName},
    {"elapsed_ms", elapsedMs()},
};
// _meta is the MCP 2024-11-05 spec extension point on the
// result envelope; clients that surface it see telemetry, others
// ignore it without erroring.
env["_meta"] = std::move(meta);
```

On error paths the same `_meta` carries the original args
(redacted of tokens) so a failing call has enough context for a
human to reproduce:

```jsonc
{
  "jsonrpc": "2.0",
  "id": 17,
  "error": { "code": -32000, "message": "blueprint not found" },
  "_meta": {
    "tool": "read_blueprint",
    "elapsed_ms": 42,
    "args": { "asset_path": "/Game/AI/BP_DoesNotExist" }
  }
}
```

The MCP spec's extension point makes `_meta` legal everywhere — old
clients ignore it, new clients can surface it as a tooltip / log
line. We pay no compatibility cost for shipping it on every call.

## Distribution

The whole thing ships as one unit: the UE plugin's directory tree
includes the MCP server sources, and the plugin's
`PreBuildSteps.Win64` builds the server as part of the UE build:

```json
// BlueprintReader.uplugin (excerpt)
"PreBuildSteps": {
  "Win64": [
    "powershell.exe -ExecutionPolicy Bypass -File \"$(PluginDir)/Scripts/Build-MCPServer.ps1\" -ProjectDir \"$(ProjectDir)\" -PluginDir \"$(PluginDir)\""
  ]
}
```

`Build-MCPServer.ps1`:

- Looks for `mcp-server/` next to the plugin or under the project.
- Skips the build if `bp-reader-mcp.exe` is newer than every source
  file under `src/` plus `CMakeLists.txt` — so incremental UE
  builds don't pay for a no-op MCP rebuild.
- Configures + builds with CMake. Plain CMake — no vcpkg, no git,
  no network.

The "no network" part matters. Third-party deps are vendored under
`third_party/`:

```
third_party/
├── nlohmann_json/  (header-only)
├── fmt/            (header-only)
├── doctest/        (single header)
└── README.md       (versions and provenance)
```

CMake exports the same target names FetchContent produces, so the
target-link lines (`nlohmann_json::nlohmann_json`, `fmt::fmt`,
`doctest::doctest`) work whether the deps are vendored or pulled at
configure time. Switching back to FetchContent is a one-line
CMakeLists change if you ever need it.

A fresh clone of the repo on Windows 2022 with `Visual Studio 17`
and `cmake.exe` on PATH builds and tests in under five minutes,
zero environment setup. CI runs that exact sequence on every push
that touches `mcp-server/**`.

## Skill manifests

The MCP server defines tools; the Claude client needs a "skill" or
"recipe" to discover them. Skill manifests live at
`Plugins/BlueprintReader/Claude/`:

```
Plugins/BlueprintReader/Claude/
├── skills/
│   ├── bp-reader/
│   │   ├── SKILL.md           # human-readable description
│   │   └── manifest.json      # tool wiring
│   └── ...
├── Install-Skills.ps1         # copy into ~/.claude/skills/
└── README.md
```

`Install-Skills.ps1` copies the skill directories into
`~/.claude/skills/` so Claude Code picks them up. Re-running it
overwrites the destination with the source-of-truth versions from
the plugin tree. The sync workflow is "edit in the plugin tree,
re-run install" — there's no two-way sync to maintain.

## Hooks for future tools

The "Add a new tool" pattern from CLAUDE.md formalizes the surface
area you have to touch:

1. **Plugin** (`BlueprintReaderCommandlet.cpp`): add an `EOp` value,
   a `ParseOp` entry, a dispatch line in `RunOneOp`, and a
   `RunFooOp(Params, OutputPath, bPretty)` implementation.
2. **MCP interface** (`IBlueprintReader.h`): one pure virtual.
3. **Mock backend**: hard-code from fixtures (read) or throw "read-
   only" (write).
4. **Commandlet + live backends**: serialize args, call `RunOp`.
   Auto's `FORWARD` macro picks it up automatically.
5. **`BlueprintTools.cpp`**: register the tool with its input schema
   and a handler that pulls args from the JSON.
6. **Tests**: a mock case (shape assertion) and a live case if the
   op needs a real BP.
7. **Tool count assertions** in `test_tools.cpp` and `test_mcp.cpp`.

If the new tool is a node-spawning op, also add it to
`list_node_kinds` in `BlueprintTools.cpp` — keep the dispatch table
and the discoverability list in lockstep.

The pattern is table-driven on purpose. Every tool the project has
shipped has followed exactly this checklist; the friction for
adding the 120th tool is the same as the friction for adding the
20th. That's the point of the layering — concerns are stable, only
the per-tool data moves.

## Checkpoint

A clean clone on Windows 2022 (CI runner) should pass all tests
without any env setup:

```pwsh
git clone <repo>
cd <repo>/Plugins/BlueprintReader/mcp-server
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
.\build\tests\Release\bp-reader-tests.exe --reporters=console
```

The output should end with something like:

```
===============================================================================
[doctest] test cases: 444 | 444 passed | 0 failed | 0 skipped
[doctest] assertions: 12537 | 12537 passed | 0 failed |
[doctest] Status: SUCCESS!
```

Live-only cases auto-skip cleanly when their env vars (`BP_READER_PROJECT`,
`BP_READER_ENGINE_DIR`) aren't set — the runner doesn't have a UE
install and we don't need one for the mock-only pass.

Set the env vars on a developer box with UE installed and re-run.
The previously-skipped live cases now execute, and the total count
goes up. Same binary, same source, no rebuild — the only change
is the environment.

CI on every push covers the mock path. The live path is verified
by hand (or by developer pre-merge runs) before a PR goes in.
`.github/workflows/mcp-server.yml` is intentionally minimal —
checkout, cmake configure, cmake build, run the test exe — no
matrix, no parallel jobs, no dep-cache state (because there is
none). Runs in under 10 minutes from green check-in to green pass.

You now have the production package: a tested, packaged, distributed
MCP server that's safe to hand to a teammate. The README points
them at the install steps; CLAUDE.md gives them the maintenance
patterns; the design docs answer the "why" questions. This is the
end of the tutorial.

See also:
- [design/09-testing.md](../design/09-testing.md) for the full test
  taxonomy.
- [design/08-error-diagnostics.md](../design/08-error-diagnostics.md)
  for the classified-failure rules and exit-code conventions.
- [../README.md](../README.md) for user-facing install + client
  config snippets.
- `CLAUDE.md` for the maintenance guide and the "Add a new tool"
  checklist.
