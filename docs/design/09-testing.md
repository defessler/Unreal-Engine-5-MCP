# 09 — Testing

The test suite has three layers that pay very different costs to run.
Knowing which layer your change touches tells you which subset to run
locally; CI handles the rest.

| Layer                   | Cost     | Needs UE | What it tests |
|-------------------------|----------|----------|---------------|
| Mock fixtures           | < 5s     | no       | server logic, wire JSON, BPIR, codegen |
| Live commandlet         | minutes  | yes      | the real plugin's `RunOneOp` dispatch end-to-end |
| Live in-process (TCP)   | minutes  | yes (editor open) | the live backend against a real editor |

All three are doctest cases in a single executable at
`Plugins/BlueprintReader/mcp-server/build/tests/Release/bp-reader-tests.exe`.
Live cases auto-skip when their env vars aren't set, so a fresh-clone
run drops straight through to mock-only coverage in seconds.

For backend-specific failure modes the tests exercise, see
[08 — Errors & diagnostics](08-error-diagnostics.md).


## doctest framework

The suite uses doctest because it's header-only (vendored under
`mcp-server/third_party/`), supports test suites and runtime skip
predicates, and produces stable per-case output that's easy to grep.

The entire test main is in `tests/test_main.cpp`:

```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
```

That's it. doctest supplies `int main()`. Every other `tests/*.cpp`
file just `#include <doctest/doctest.h>` and writes `TEST_CASE(...)`
blocks; the linker collects them.

There are ~440 test cases across the files in `tests/`. The full suite
runs in well under 5 seconds on a dev box when only mock cases are
active.

### File layout

Each `test_<topic>.cpp` is self-contained — no shared `test_main`,
no header sharing beyond `test_helpers.h` for fixture-loading
helpers. A new test topic gets a new file; CMake's `file(GLOB ...)`
pattern in `tests/CMakeLists.txt` picks it up automatically.

The full list of test files (`tests/test_*.cpp`):

```
test_apply_ops.cpp          test_jsonrpc.cpp                test_single_instance_lock.cpp
test_auto_backend.cpp       test_live_backend.cpp           test_soak.cpp
test_bpir.cpp               test_main.cpp                   test_tools.cpp
test_caching_reader.cpp     test_mcp.cpp                    test_transpile_roundtrip.cpp
test_commandlet_arg_encoding.cpp  test_mock_backend.cpp     test_type_shorthand.cpp
test_commandlet_backend.cpp test_protocol_negotiation.cpp   test_types.cpp
test_cpp_class.cpp          test_read_only.cpp              test_unsupported_treatment.cpp
test_cpp_codegen.cpp        test_diagnostics.cpp
test_cpp_lex.cpp            test_framing_stress.cpp
test_cpp_parse.cpp          test_json_projection.cpp
test_decompile.cpp
```

### Shared helpers

`tests/test_helpers.h` exposes two functions:

```cpp
inline std::filesystem::path FixturesDir() {
    return TestExecutableDir() / "fixtures";
}

inline backends::MockBlueprintReader MakeMockReader() {
    return backends::MockBlueprintReader(FixturesDir());
}
```

The fixtures directory is staged next to the test exe at build time
(CMake copies `mcp-server/fixtures/*.json` to
`build/tests/Release/fixtures/`). Tests don't need to know absolute
paths or env vars; `MakeMockReader()` returns a ready-to-use reader.


## Layer 1: mock fixtures

Source: `mcp-server/tests/test_mock_backend.cpp`,
`test_bpir.cpp`, `test_cpp_codegen.cpp`, `test_decompile.cpp`,
`test_transpile_roundtrip.cpp`, `test_jsonrpc.cpp`, `test_mcp.cpp`,
`test_tools.cpp`, and most of the rest.

These are the fast, no-UE-needed tests. They cover everything that
lives inside the MCP server: wire JSON adapters, backend logic
(against the mock), BPIR validation, codegen, the C++ parser,
JSON-RPC framing, tool registration, and the MCP envelope.

### Mock fixture shape

Each `fixtures/BP_*.json` is one BP described in the same wire
shape the production backends return. A real opener
(`mcp-server/fixtures/BP_Enemy.json:1-15`):

```json
{
  "summary": {
    "asset_path": "/Game/AI/BP_Enemy",
    "name": "BP_Enemy",
    "parent_class": "ACharacter",
    "modified_iso": "2026-04-22T18:14:03Z"
  },
  "metadata": {
    "asset_path": "/Game/AI/BP_Enemy",
    "name": "BP_Enemy",
    "parent_class": "ACharacter",
    "interfaces": [],
    "variables": [ ... ],
    ...
```

`MockBlueprintReader::LoadDir`
(`mcp-server/src/backends/MockBlueprintReader.cpp:59-65`) reads every
`.json` file in the directory, parses each into a `FixtureEntry`
through the nlohmann adapters in `BlueprintReaderTypes.h`, and keys
the result by `summary.asset_path`. Three fixtures ship with the repo:

| File                     | Asset path                          | Use |
|--------------------------|-------------------------------------|-----|
| `BP_Enemy.json`          | `/Game/AI/BP_Enemy`                 | event graph + function topology |
| `BP_PlayerController.json` | `/Game/Player/BP_PlayerController` | function with inputs/outputs/locals |
| `BP_Pickup.json`         | `/Game/Items/BP_Pickup`             | SCS component hierarchy |

The three between them exercise every wire shape: graphs, functions,
variables, components, find-hits, optional fields.

The fixtures aren't snapshots of real `.uasset` files — they're
hand-crafted JSON that matches the wire format. This means we can
test edge cases (a node with no comment, a pin with a `null` default)
without having to set up a corresponding UE asset.

### Sample test

Real shape (`tests/test_mock_backend.cpp:10-13`):

```cpp
TEST_CASE("MockBlueprintReader loads all 3 fixtures") {
    auto reader = bpr::test::MakeMockReader();
    CHECK(reader.FixtureCount() == 3);
}
```

The pin-count assertion at `test_tools.cpp:35` (`CHECK(spec.size() == 119);`)
is a tool-count check — keep this aligned with `BlueprintTools.cpp`
registrations. Bumping it is part of the "Adding a new tool" checklist
in CLAUDE.md.


## Layer 2: live commandlet

Source: `mcp-server/tests/test_commandlet_backend.cpp`.

These tests construct a real `CommandletBlueprintReader` pointed at
the source-built editor and exercise it against fixture BPs the seed
commandlet produced. They take long enough that running the suite is
a noticeable wait — a single one-shot test pays the ~5–7s editor
cold-start cost.

### Auto-skip pattern

Live tests skip when their env vars aren't set
(`test_commandlet_backend.cpp:37-40`):

```cpp
bool LiveBackendAvailable() {
    return !GetEnv("BP_READER_ENGINE_DIR").empty() &&
           !GetEnv("BP_READER_PROJECT").empty();
}
```

Every test case attaches a doctest skip predicate
(`test_commandlet_backend.cpp:53-54`):

```cpp
TEST_CASE("CommandletBlueprintReader: List under /Game/AI returns seeded blueprints"
          * doctest::skip(!LiveBackendAvailable())) {
    ...
}
```

`doctest::skip(predicate)` is a per-case modifier — when the predicate
is true, the case is reported as skipped rather than passed. CI
implicitly skips every live case (CI never sets the env vars); local
runs that set them get full coverage.

### Seed commandlet

Source:
`Plugins/BlueprintReader/Source/BlueprintReaderEditor/Private/BlueprintReaderSeedCommandlet.cpp`.

The plugin ships a separate commandlet —
`UBlueprintReaderSeedCommandlet` (`-run=BlueprintReaderSeed`) — that
synthesizes the two BPs every live test depends on:

- `/Game/AI/BP_TestEnemy` — 5 variables, 2 functions, event-graph
  topology. `SeedBP_TestEnemy` at
  `BlueprintReaderSeedCommandlet.cpp:255`.
- `/Game/AI/BP_TestPickup` — separate fixture exercising other
  pathways. `SeedBP_TestPickup` at
  `BlueprintReaderSeedCommandlet.cpp:302`.

Run with:

```bat
"D:\Projects\Unreal Engine 5\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" ^
  "D:\Projects\UE5_MCP\UE5_MCP.uproject" ^
  -run=BlueprintReaderSeed -nullrhi -nosplash -unattended -nopause
```

The seed is idempotent — running it twice is safe. The output
`.uasset` files are committed to `Content/AI/` so a fresh clone has
them without running the seed, but if the seed changes their shape
you commit the regenerated `.uasset`s.

### Running live commandlet tests

```pwsh
$env:BP_READER_BACKEND     = "commandlet"
$env:BP_READER_ENGINE_DIR  = "D:\Projects\Unreal Engine 5"
$env:BP_READER_PROJECT     = "D:\Projects\UE5_MCP\UE5_MCP.uproject"
.\Plugins\BlueprintReader\mcp-server\build\tests\Release\bp-reader-tests.exe
```

The mock cases still run first; the live cases get unskipped when
the env vars are set.

### Daemon-mode tests

A subset of `test_commandlet_backend.cpp` constructs the reader with
`useDaemon=true` to exercise the daemon path. These are appreciably
faster than one-shot (per-call cost ~1s instead of ~5–7s), so they
amortize across multiple test cases sharing one daemon process.


## Layer 3: live in-process (TCP)

Source: `tests/test_live_backend.cpp`,
`tests/test_auto_backend.cpp`.

`LiveBlueprintReader` talks to an editor over TCP. The tests don't
require a real editor — they spin up a mock TCP server in-process
that speaks the same wire protocol, which is enough to exercise the
backend's handshake, framing, refresh, and retry logic.

The mock at `test_live_backend.cpp:45+` (`MockServer`) accepts one
connection per script:

```cpp
class MockServer {
public:
    using Script = std::function<void(SOCKET clientSock)>;

    // Single-connection convenience ctor.
    explicit MockServer(Script script)
        : MockServer(std::vector<Script>{std::move(script)}) {}
    ...
};
```

A script is a callback that runs against the accepted client socket
— it can send `hello`, expect `auth`, respond `auth_ok` or
`auth_fail`, handle op frames, or close the socket abruptly. This
gives tests fine-grained control over server-side behavior without
needing a real editor.

The multi-connection variant tests the auth-fail-then-refresh path
documented in [08 — Errors & diagnostics](08-error-diagnostics.md#live-backend-self-refresh):
first connection scripts `auth_fail`, the handshake file is rewritten
between connections, the second connection scripts `auth_ok` —
asserts the backend successfully recovers without a process restart.

Real editor integration is covered by the same env-var-gated
auto-skip pattern; tests construct an `AutoBlueprintReader` and call
`SelectBackendForTesting` (`AutoBlueprintReader.h:137`) to confirm
the probe correctly routes to live.


## Continuous integration

Source: `.github/workflows/mcp-server.yml`.

CI is mock-only by design. The workflow is short
(`.github/workflows/mcp-server.yml:1-33`):

```yaml
name: mcp-server

on:
  push:
    branches: [main]
    paths:
      - 'Plugins/BlueprintReader/mcp-server/**'
      - '.github/workflows/mcp-server.yml'
  pull_request:
    paths:
      - 'Plugins/BlueprintReader/mcp-server/**'
      - '.github/workflows/mcp-server.yml'
  workflow_dispatch:

jobs:
  build-and-test:
    runs-on: windows-2022
    timeout-minutes: 20
    steps:
      - uses: actions/checkout@v4
      - name: Configure
        run: cmake -S Plugins/BlueprintReader/mcp-server -B Plugins/BlueprintReader/mcp-server/build -G "Visual Studio 17 2022" -A x64
      - name: Build
        run: cmake --build Plugins/BlueprintReader/mcp-server/build --config Release --parallel
      - name: Run tests (mock-only — live commandlet tests skip without env)
        run: ./Plugins/BlueprintReader/mcp-server/build/tests/Release/bp-reader-tests.exe --reporters=console
```

Why mock-only:

- GitHub-hosted Windows runners don't have UE installed and can't
  realistically pull down a ~70GB source-built engine.
- Mock coverage is already substantial — the entire server logic,
  every wire shape, every BPIR validator path, every codegen test.
  The live commandlet tests are a thin layer over what mocks already
  verify.

`paths:` constrains the workflow to fire only when something under
`mcp-server/` or the workflow file itself changes. PR diffs that
only touch the plugin C++ don't trigger the build (the plugin can
only be tested against UE locally).

No vcpkg / FetchContent caching needed: deps are vendored under
`mcp-server/third_party/`, so the build is deterministic and
network-free.


## Test conventions

Three patterns the suite uses consistently:

### One file per topic

Each `test_<topic>.cpp` covers one slice of the surface. The slice
boundaries follow source-code boundaries (`backends/`, `tools/`, `jsonrpc/`,
`tools/codegen/`, `tools/parse/`). A change to `tools/Bpir.cpp`
should be reflected in `test_bpir.cpp` only.

### No shared test main

doctest's `DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN` lives in `test_main.cpp`;
every other file just `#include <doctest/doctest.h>`. The linker
collects `TEST_CASE` registrations from all object files. There's
nothing test-suite-wide to set up — `MakeMockReader()` is the heaviest
fixture.

### Property tests for round-trips

The transpile pipeline has a strong invariant: `BPIR → C++ → BPIR`
should be identity on the language subset CppEmit produces. The
roundtrip tests at `test_transpile_roundtrip.cpp` pin this directly.
Example (`test_transpile_roundtrip.cpp:53-58`):

```cpp
TEST_CASE("Roundtrip: simple set with int literal") {
    json original = MakeFn(json::array({
        json{{"set", "Health"}, {"to", json{{"lit", 100}}}}
    }));
    CheckBodiesEqual(original["body"], Roundtrip(original));
}
```

Property tests catch regressions in either direction simultaneously
— if CppEmit changes how it renders `set`, parsing the result back
to BPIR may not match; if CppParse loses fidelity on some form,
likewise. Either side breaks, the test fails fast.


## Specialty test cases

### Diagnostics

`test_diagnostics.cpp` builds fake project layouts under temp dirs
and asserts that `Diagnostics` produces the expected
auto-discovery / failure messages — see
[08 — Errors & diagnostics](08-error-diagnostics.md#write-failure-classification).
The fakes use real-on-disk file structures (`.uproject` JSON, fake
binary placeholders) so the tested code path is identical to the
production one.

### Framing stress

`test_framing_stress.cpp` exercises the JSON-RPC framing layer with
adversarial inputs: large payloads, mixed CRLF, short reads, batched
requests. The file header (`test_framing_stress.cpp:1-7`) records
the regression it prevents:

> The transport had a year-long latent bug where it only spoke
> Content-Length framing while the MCP spec mandates newline-delimited
> JSON. JetBrains Copilot couldn't talk to us at all. These tests are
> the regression net so we don't ship that again.

### Soak

`test_soak.cpp` is in a separate doctest test suite (opt-in,
`bp-reader-tests --test-suite=soak`) that runs ~10k MCP tool calls
against the mock backend and asserts nothing degrades. The default
suite stays under a second; soak takes a few seconds to a minute. It
exists for shaking out transport-layer changes — most changes don't
need it but it's available when one does.

### Single-instance lock

`test_single_instance_lock.cpp` tests the project-singleton mutex
behavior — ensures two MCP-server processes against the same project
don't fight for resources. This pins behavior the wiki / README
documents for users running both Claude Code and Claude Desktop
against the same project.

### Read-only wrapper

`test_read_only.cpp` exercises `ReadOnlyBlueprintReader` — every
write tool throws a clear error pointing at `BP_READER_READ_ONLY`,
every read tool passes through unchanged. This is the wrapper that
coexists with an open UE editor without trying to compete for write
locks.


## Adding a new test case

When adding a tool:

1. **Mock test** (always) — assert the shape or that
   `MockBlueprintReader` throws the read-only error. Goes in the
   appropriate `test_<topic>.cpp` based on what the tool tests
   (e.g. backend logic → `test_mock_backend.cpp`, MCP envelope →
   `test_mcp.cpp`).

2. **Live commandlet test** (if the op needs a real BP) — add a
   case with `doctest::skip(!LiveBackendAvailable())` to
   `test_commandlet_backend.cpp`. Use the daemon (`useDaemon=true`)
   so the cost amortizes with neighboring tests.

3. **Tool count assertions** in `test_tools.cpp:35` and
   `test_mcp.cpp` need to be bumped (`spec.size() == N`). This is
   listed in the "Adding a new tool" checklist in CLAUDE.md.

The mock test pins what the dispatcher promises agents; the live
test confirms the plugin actually delivers it. Don't ship one
without the other — the contract surface between them is exactly
where regressions live.
