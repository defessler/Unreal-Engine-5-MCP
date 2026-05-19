# BP Roundtrip Capability — Design

**Date:** 2026-05-16
**Driving prompt:** Ralph-loop request to (a) research UE5.7+ BP tooling and (b) prove end-to-end capability by reading a complex BP with the read tools and recreating it with the write tools without copying anything, including a BP↔C++ roundtrip.
**Completion sentinel:** `RALPH-DONE-BP-ROUNDTRIP` (emitted only when every acceptance criterion below is met).
**Sequencing:** Research-first (per user decision; recommended approach was code-first, user overrode).

---

## 1. Goals & non-goals

### Goals (all must be true for completion)

1. Seven research documents committed under `docs/research/`, indexed by `docs/research/README.md`, covering: UE5.7 overview, editor automation, scripting-language hooks, BP syntactic-sugar nodes, ThirdPersonCharacter (TPC) anatomy, bp-reader extensibility audit.
2. UE5 ThirdPerson template imported into `Content/Imported/ThirdPerson/`.
3. **Roundtrip path A — granular writes:** TPC → `ReadToSpec` produces `BPSpec` JSON → `SpecToBP` consumes spec and drives only the existing write tools (`create_blueprint`, `add_variable`, `add_component`, `add_node`, `connect_pins`, ...) to build `/Game/Recreated/BP_TPC_Granular`.
4. **Roundtrip path B — BPIR transpile:** TPC → `decompile_blueprint` → BPIR JSON → C++ emit → UBT compile of an auto-generated module (`BPRoundtripModule`) → `parse_cpp_blueprint` on the emitted source → `transpile_blueprint` → `/Game/Recreated/BP_TPC_BPIR`.
5. **Structural diff:** new MCP tool `bp_structural_diff` shows empty diff (or only whitelisted differences) between TPC and each clone.
6. **Functional tests:** doctest cases in `BlueprintReaderMcpTests` driving both paths against `BP_TestEnemy` (smoke) and TPC (full) pass.
7. Extensibility audit identifies seams in `IBlueprintReader`, `ToolRegistry`, BPIR codegen, and backends; any concrete leaks found that block goals 3-6 are fixed; speculative refactoring is not done.

### Non-goals

- Implementing new scripting backends (Lua/AngelScript/Verse). Research only.
- Refactoring code that already works. Audit documents seams; only blocking leaks get fixed.
- Performance work on the existing tools (separate concern).
- New features beyond `bp_structural_diff` and roundtrip orchestration helpers.

---

## 2. Architecture

```
docs/research/                       Phase-1 deliverables (markdown only)
  README.md                          index
  ue5.7-overview.md
  editor-automation.md
  scripting-languages.md
  syntactic-sugar-nodes.md
  tpc-anatomy.md                     (depends on TPC import; written after Content/Imported/ exists)
  bp-reader-extensibility-audit.md

Content/Imported/ThirdPerson/        UE5 template assets (committed; ~few MB)
  BP_ThirdPersonCharacter.uasset
  ABP_Manny.uasset
  BP_ThirdPersonGameMode.uasset
  BP_ThirdPersonPlayerController.uasset
  Input/IA_*.uasset, IMC_*.uasset

Content/Recreated/                   Roundtrip outputs (gitignored; regenerable)
  BP_TPC_Granular.uasset             (excluded from git via .gitignore in this dir)
  BP_TPC_BPIR.uasset

Plugins/BlueprintReader/
  Source/BlueprintReaderEditor/
    Public/BlueprintStructuralDiff.h            new
    Private/BlueprintStructuralDiff.cpp         new
    Private/BlueprintReaderCommandlet.cpp       += EOp::StructuralDiff
    Private/BPRoundtripSeedCommandlet.cpp       new: imports UE5 ThirdPerson template into
                                                Content/Imported/ ; idempotent

  Tests/BlueprintReaderMcpCore/
    Private/tools/BlueprintTools.cpp            += register "bp_structural_diff"
    Private/backends/IBlueprintReader.h         += pure-virtual StructuralDiff(...)
    Private/backends/{Mock,Commandlet,Live,Auto,ReadOnly,Caching}BlueprintReader.cpp
                                                += implementations
    Private/roundtrip/                          new subdir, library code reused by tests
      BPSpec.{h,cpp}                            spec JSON serializer/deserializer
      ReadToSpec.{h,cpp}                        drives IBlueprintReader read methods
      SpecToBP.{h,cpp}                          drives IBlueprintReader write methods
      BPIRRoundtrip.{h,cpp}                     decompile → C++ → compile → parse → transpile

  Tests/BlueprintReaderMcpTests/
    Private/test_structural_diff.cpp            new
    Private/test_roundtrip_granular.cpp         new
    Private/test_roundtrip_bpir.cpp             new
    fixtures/BP_TPC_spec.json                   new: golden spec for TPC
    fixtures/BP_TPC_bpir.json                   new: golden BPIR for TPC
    fixtures/BP_TestEnemy_spec.json             new: golden spec for the smoke target
```

### 2.1 New library modules under `BlueprintReaderMcpCore`

**`BPSpec` (header-only schema + JSON glue).** A single JSON document that captures everything the read tools surface about one BP, in a form that drives the write tools without ambiguity:

```jsonc
{
  "package_path": "/Game/Imported/ThirdPerson/BP_ThirdPersonCharacter",
  "parent_class": "/Script/Engine.Character",
  "interfaces": ["/Game/.../BPI_Foo"],
  "variables": [ { "name": "...", "type": {...}, "default": ..., "flags": ["instance_editable", ...] } ],
  "components": [ { "name": "...", "class": "...", "parent": "Root", "transform": {...}, "properties": {...} } ],
  "functions": [
    { "name": "Foo", "inputs": [...], "outputs": [...], "locals": [...],
      "nodes": [ /* existing BPNode wire shape */ ],
      "connections": [ /* {from_node, from_pin, to_node, to_pin} */ ] }
  ],
  "event_graph": { "nodes": [...], "connections": [...] },
  "macros": [...],
  "delegates": [...]
}
```

No new node shape is invented; `nodes` is the existing wire `BPNode` array verbatim (snake_case, `meta` as nested object — see `BlueprintReaderTypes.h`). Stable node IDs are generated by `ReadToSpec` as a content-hash of `(class, signature, position-rank)` so the diff tool can match nodes across the source/clone pair without depending on FGuid.

**`ReadToSpec`.** Pure orchestrator over `IBlueprintReader`. Calls `ReadBlueprint`, then `ListVariables`, then for each function `GetFunctionGraph`, then `GetEventGraph`, then assembles a `BPSpec`. No I/O outside `IBlueprintReader`. Returns `BPSpec` by value or a structured error.

**`SpecToBP`.** Pure consumer of `BPSpec`. Orders the write calls by dependency: (1) `CreateBlueprint` for the package, (2) `AddVariable` for every var (skipping ones already present from parent), (3) `AddComponent` for every SCS node in parent-first order, (4) function skeletons (`AddFunction`) before nodes inside them, (5) `AddNode` per node, (6) `ConnectPins` per connection. Stops at first failure; surfaces the failing op for diagnosis.

**`BPIRRoundtrip`.** One-shot pipeline. Inputs: source `package_path`, output `package_path`. Steps:
1. `DecompileBlueprint(source)` → BPIR JSON.
2. Emit `.cpp` + `.h` under `Plugins/BlueprintReader/Source/BPRoundtripModule/Private/` (new module, see §2.2).
3. Trigger UBT build of `BPRoundtripModule`. Capture full log on failure.
4. `ParseCppBlueprint(emitted_path)` → BPIR JSON' (round-tripped through C++).
5. `TranspileBlueprint(bpir')` to the output package path.
6. Return success or structured error with which step failed and why.

### 2.2 New UBT module: `BPRoundtripModule`

A small **runtime** module under `Plugins/BlueprintReader/Source/BPRoundtripModule/` whose only job is to be the compile target for BPIR-roundtrip's emitted C++. Empty by default; tests generate files into its `Private/` dir and trigger a UBT incremental build of just that module.

- `Type: Runtime` so it loads in editor and works headlessly.
- No dependencies beyond `Core, CoreUObject, Engine, GameplayTags` (matches what TPC transpile typically needs).
- `Private/` is gitignored except for `.gitkeep`; emitted files are ephemeral.
- Listed in `BlueprintReader.uplugin` as a module entry.

Rationale for a dedicated module: keeps test artifacts out of `BlueprintReaderEditor` (which would force the editor module to rebuild on every roundtrip), avoids polluting any production module with auto-generated code, and gives us a clean module-scoped UBT build target.

### 2.3 New plugin op: `EOp::StructuralDiff`

In `BlueprintReaderCommandlet`. Args: `-A=/Game/...`, `-B=/Game/...`, `-OutputPath=...`, `-Pretty=0|1`. Loads both as `UBlueprint*` (read-only), runs `BlueprintStructuralDiff::Compare`, emits result JSON.

`BlueprintStructuralDiff::Compare(const UBlueprint* A, const UBlueprint* B, const FCompareOptions&)` returns `FStructuralDiffResult { bool bEqual; TArray<FDifference> Differences; }`. Differences carry `(Path, Kind, ValueA, ValueB)`. Match strategy:

- **Variables:** matched by name; compare type, default-value, UPROPERTY flags.
- **Components:** matched by SCS node name; compare `UClass*`, parent name, transform, and CDO property overrides.
- **Functions/macros:** matched by name; for each, build a position-independent node-set match using `(NodeClass, Signature, InputPinCount, OutputPinCount)` as a multi-key, then within each matched group, line up by connection topology. Flag missing/extra nodes and pin-level connection mismatches.
- **Event graph:** same node-signature matching.
- **Whitelist:** options struct lets tests exempt expected differences (e.g. node-position pixel coords, asset-name change from `BP_TPC` to `BP_TPC_Granular`).

Lives in plugin not server because it needs full `UBlueprint`/`USCS_Node`/`UEdGraphNode` reflection — server-side just shells out and surfaces results.

### 2.4 New MCP tool: `bp_structural_diff`

Schema: `{ source: string, candidate: string, options?: {ignore_node_positions?: bool, allowed_name_substitutions?: [[from, to]]} }`. Wraps `EOp::StructuralDiff` via `IBlueprintReader::StructuralDiff`. Implemented across all four backends; mock returns an error explaining "diff requires live or commandlet backend".

### 2.5 New plugin seed commandlet: `BPRoundtripSeed`

Run `-run=BPRoundtripSeed` once to populate `Content/Imported/ThirdPerson/`. Imports the UE5 ThirdPerson FeaturePack from `<EngineDir>/Templates/TP_ThirdPersonBP/` (it's in the engine source) into `Content/Imported/ThirdPerson/`. Idempotent: if the assets exist, no-op. Replaces the need to do this through the editor's "Add Content" dialog.

---

## 3. Data flow

### 3.1 Phase 1 — Research

Linear, no dependencies between docs except TPC-anatomy depends on `Content/Imported/ThirdPerson/` existing. Order:

1. Write `docs/research/README.md` (index, placeholders for remaining docs).
2. Run `BPRoundtripSeed` commandlet to populate `Content/Imported/ThirdPerson/`. Commit those assets.
3. Write `ue5.7-overview.md`, `editor-automation.md`, `scripting-languages.md`, `syntactic-sugar-nodes.md`, `bp-reader-extensibility-audit.md` in parallel (no order dependencies among these five).
4. Write `tpc-anatomy.md` after running `ReadToSpec` against `BP_ThirdPersonCharacter` (and the other 3 imported BPs) — the doc literally embeds the spec summary as ground truth.

Phase-1 success criterion: all seven md files exist, are non-empty, are referenced from `docs/research/README.md`, and are committed.

### 3.2 Phase 2 path A — Granular writes roundtrip

```
TPC asset
  └─ ReadToSpec  ──────────►  BPSpec JSON  ─┐
                                            │
                              SpecToBP  ◄───┘  ──►  BP_TPC_Granular asset
                                            │
                              bp_structural_diff(TPC, BP_TPC_Granular)
                                            │
                                            ▼
                                differences=[]   ──►  pass
                                differences=[…]  ──►  fix in ReadToSpec/SpecToBP/write tool, re-run
```

The BPSpec JSON is committed as a fixture (`fixtures/BP_TPC_spec.json`) on first successful run, then used in subsequent runs as a regression-catch (if `ReadToSpec` output drifts, the test loudly fails).

### 3.3 Phase 2 path B — BPIR transpile roundtrip

```
TPC asset
  └─ decompile_blueprint  ──►  BPIR JSON
                                  │
                            CppEmit  ──►  .cpp/.h under BPRoundtripModule/Private/
                                  │
                            UBT build BPRoundtripModule  ──►  module DLL (proves C++ compiles)
                                  │
                            parse_cpp_blueprint  ──►  BPIR JSON'
                                  │
                            transpile_blueprint  ──►  BP_TPC_BPIR asset
                                  │
                            bp_structural_diff(TPC, BP_TPC_BPIR)
                                  │
                                  ▼
                            differences=[]   ──►  pass
```

`BPIR JSON` and `BPIR JSON'` are *also* diffed (BPIR-level) as a sanity check that the C++ round-trip is lossless at the IR level even when the final-asset diff has expected differences (e.g. comment nodes that don't survive C++ → BPIR).

### 3.4 Iteration loop (Ralph iterations)

Each Ralph iteration runs the tests; on failure, reads the diff output and the most recent commit history; identifies which component (ReadToSpec, SpecToBP, BPIR codegen, a specific tool) is the root cause; makes a targeted fix; re-runs. Sentinel emitted only when both paths produce empty diffs (modulo whitelist) for both TPC and BP_TestEnemy.

---

## 4. Error handling

| Failure | Behavior | Surface |
|---|---|---|
| `ReadToSpec` partial read (a tool returns error mid-assembly) | Stop, return `BPSpec` with `incomplete: true` and an `errors[]` array | JSON; test asserts complete; CI fails loudly |
| `SpecToBP` write op fails | Stop, leave partial BP in place for inspection, return `{step, op, args, error}` | JSON; functional test surfaces failing op |
| BPIR-roundtrip C++ compile fails | Dump full UBT log to `Intermediate/BPRoundtrip/<timestamp>.log`, attach last 200 lines to error | JSON; test surfaces log path |
| `bp_structural_diff` finds differences | Return them; test asserts `differences = []` (modulo whitelist) | JSON; readable table for human review |
| Source asset doesn't exist | Hard-fail at `ReadToSpec` entry | JSON; test fails with clear message |
| `EOp::StructuralDiff` can't load one of the BPs | Hard-fail with `AssetNotFound` | JSON; test fails |

No retry logic in roundtrip code itself — failures bubble up and Ralph iterations are the retry mechanism (with diffs to drive the next fix).

`Content/Recreated/` is gitignored so failed runs don't pollute the working tree. Tests clean up `BP_TPC_Granular` / `BP_TPC_BPIR` between runs by deleting and recreating.

---

## 5. Testing

### 5.1 Test tiers

| Tier | Runs | Where | Notes |
|---|---|---|---|
| Unit | `BlueprintReaderMcpTests` mock-only | every build | BPSpec serialize/deserialize roundtrip on synthetic data; SpecToBP-op-ordering checks without a real backend |
| Mock | `BlueprintReaderMcpTests` mock-backend | every build | ReadToSpec assembly from mock fixtures; structural-diff returns errors-as-expected on mock |
| Smoke live | `BlueprintReaderMcpTests` live, target=`BP_TestEnemy` | dev-machine + nightly | Both roundtrip paths; fast (~30s incl. compile) |
| Full live | `BlueprintReaderMcpTests` live, target=TPC + supporting BPs | dev-machine + nightly | Both roundtrip paths; slow (~5min incl. compile); marked `[.slow]` doctest tag |

Smoke and Full live are gated on `BP_READER_BACKEND=commandlet` / `live` and `BP_READER_PROJECT` env vars per existing convention.

### 5.2 New test files

- `test_structural_diff.cpp` — synthetic two-BP pairs proving diff catches every category of difference (missing var, wrong type, extra node, swapped connection, etc.).
- `test_roundtrip_granular.cpp` — drives ReadToSpec → SpecToBP → diff against `BP_TestEnemy` (smoke) and TPC (full). Persists the spec to the golden fixture on first run; checks against the fixture on subsequent runs.
- `test_roundtrip_bpir.cpp` — same matrix for the BPIR path, including the BPIR-vs-BPIR' sanity diff.

### 5.3 Golden fixtures committed

- `fixtures/BP_TPC_spec.json`
- `fixtures/BP_TPC_bpir.json`
- `fixtures/BP_TestEnemy_spec.json`

Regenerable: `BlueprintReaderMcpTests.exe --regen-fixtures` mode rebuilds them from live. Catches regressions in `ReadToSpec` or `decompile_blueprint` output shape without requiring a full live run.

### 5.4 Tool-count assertions bumped

Per CLAUDE.md gotcha: `test_tools.cpp` and `test_mcp.cpp` have `spec.size() == N` assertions that need to be bumped by 1 (for `bp_structural_diff`).

---

## 6. Unilateral decisions (documented since user said don't ask)

These would have been Section-2 checkpoint questions; locking them in here:

1. **`Content/Recreated/` is gitignored** except for a `.gitkeep`. Rationale: clones are large, regenerable, and the asset metadata changes every regeneration (date stamps, etc.) causing noisy git churn. Golden specs in `fixtures/` are the durable artifact.
2. **`BPRoundtripModule` is a new dedicated module** (rather than dumping emitted code into `BlueprintReaderEditor` or a `Generated/` subdir of an existing module). Rationale: isolates compile-time churn, keeps the editor module rebuild-stable, gives UBT a clean target boundary for incremental builds.
3. **Structural diff is position-independent** — node graphs are compared by signature + topology, not by FGuid or node-array index. Rationale: any BP rebuilt from scratch will have different GUIDs and possibly different node ordering even if semantically identical.
4. **BPIR roundtrip requires C++ compile success**, not just "C++ parses". Rationale: the user prompt said "transpile … into C++, compile, text [sic], convert C++ back to BP" — compile is part of the proof. "Text" is interpreted as "test", i.e. compile-then-test, which is also covered.
5. **No Verse/Lua/AngelScript backend implementations**, even as prototypes. Research-only. Rationale: scope hygiene; one new scripting backend is a multi-week project per the answered scope question.
6. **Extensibility audit is documentation + targeted bug-fixes only**, not a broad refactor. Rationale: per the answered scope question. If the audit discovers a leak that blocks roundtrip work, the fix is in scope; speculative cleanups are not.
7. **The seed commandlet imports the engine's ThirdPerson FeaturePack** rather than re-synthesizing TPC ourselves. Rationale: tests against the actual UE5 template (real complexity, real edge cases) not a strawman; idempotent so safe to re-run.
8. **Roundtrip code lives under `BlueprintReaderMcpCore/Private/roundtrip/`** (server-side library), not in a separate Program target. Rationale: tests need to call it directly, MCP tools may need to expose it later; one location.

---

## 7. Out of scope (explicitly)

- Multi-BP graph dependencies beyond TPC's immediate set (no recursive import of every referenced asset).
- Importing animation data, materials, meshes, textures into the spec — we represent them as path references and trust that the assets exist on disk.
- BP-to-BP "is it semantically equivalent at runtime" testing (no PIE-based functional verification). Structural diff is the proof.
- Live Coding integration for the BPIR compile step (UBT only — Live Coding is editor-bound and flaky for headless flows).
- Performance optimization of the existing 127 tools.
- Verse support (UEFN-only, not in mainline UE5.7 — research will note this).
- Implementing the `IScriptBackend` abstraction. Research notes will describe it; implementation is a follow-up project.
