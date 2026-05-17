# BP roundtrip architecture — what the code actually does

This doc captures the architecture and lessons-learned for the BP
roundtrip implementation (the granular `BPSpec` path and the BPIR
`decompile → emit C++ → compile → re-materialize` path). It is the
synthesis of what the code is doing today and the corners that bit us
along the way — read this before extending the pipeline.

For the goal and original design, see
[`docs/superpowers/specs/2026-05-16-bp-roundtrip-capability-design.md`](../superpowers/specs/2026-05-16-bp-roundtrip-capability-design.md);
for what's in the test fixture, see [`tpc-anatomy.md`](tpc-anatomy.md);
for the surrounding tooling, see [`bp-reader-extensibility-audit.md`](bp-reader-extensibility-audit.md).

## Two roundtrip paths, one diff oracle

### Path A — granular: BPSpec pivot

```
BP_TestEnemy           ReadToSpec          SpecToBP             clone
(/Game/AI/...)  →  BPSpec (struct)  →  write tool calls  →  /Game/Recreated/...
                                       (CreateBlueprint,
                                        AddVariable,
                                        AddFunction,
                                        AddNode,
                                        WirePins, ...)
                                                       ↓
                                              bp_structural_diff
                                                  (source vs clone)
```

Code: [`Tests/BlueprintReaderMcpCore/Private/roundtrip/BPSpec.{h,cpp}`](../../Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/roundtrip/),
`ReadToSpec.{h,cpp}`, `SpecToBP.{h,cpp}`. Test:
`Tests/BlueprintReaderMcpTests/Private/test_roundtrip_granular.cpp`.

**Status:** BP_TestEnemy roundtrips cleanly with `differences: []`,
`ok: true`. TPC roundtrips with documented gaps (mostly K2 nodes whose
`AddNode` mapping isn't wired yet — see `SpecToBP.cpp`'s `kind` switch).

### Path B — BPIR: decompile → C++ → compile → re-materialize

```
                                                              UBT
BP                  Decompile          EmitCppClass       BPRoundtripModule
(source)        →  BPIR class doc  →  .h / .cpp files  →    .dll built       
   │                                                              ↓ (stage 4)
   │                                                       bpir_after
   │                                                       (passthrough today)
   │                                                              ↓ (stage 5)
   ↓                                                      CreateBlueprint
diff (bpir_before                                        + AddVariable per var
 vs bpir_after)                                          + AddFunction per fn
                                                         + CompileFunctionFromBody
                                                                  ↓
                                                              clone BP
                                                       (/Game/Recreated/BPIR_...)
```

Code: `Tests/BlueprintReaderMcpCore/Private/roundtrip/BPIRRoundtrip.{h,cpp}`,
backed by `tools/Decompile.cpp` (stage 1) + `tools/codegen/CppClassEmit.cpp`
(stage 2) + UBT (stage 3) + `tools/CompileFunction.cpp::CompileFunctionFromBody`
(stage 5). Test: `Tests/BlueprintReaderMcpTests/Private/test_roundtrip_bpir.cpp`.

**Status:** Stages 1–3 + 5 work end-to-end for BP_TestEnemy AND TPC. Stage
4 (parse emitted C++ back to BPIR) is a documented passthrough —
`bpir_after = bpir_before` — because a whole-class C++ parser hasn't been
lifted out of the per-function `tools::ParseCppFunction` helper yet.

### The shared diff oracle: `bp_structural_diff`

Both paths terminate by calling `bp_structural_diff(source, clone, {})`
through `IBlueprintReader::StructuralDiff`. The plugin-side comparator
([`Plugins/BlueprintReader/Source/BlueprintReaderEditor/Private/BlueprintStructuralDiff.cpp`](../../Plugins/BlueprintReader/Source/BlueprintReaderEditor/Private/BlueprintStructuralDiff.cpp))
returns `{ok, differences: [{path, kind, a, b}]}` where `kind` is one
of `missing` / `extra` / `value_mismatch` / `type_mismatch`. Empty
`differences` means structurally identical (modulo self-class
normalization — `<SELF>` substitution for the BP's own GeneratedClass
on `self` pin signatures).

## Layered architecture

```
   Test code (doctest)        ── test_roundtrip_*.cpp
            │
            ↓
   roundtrip/  (in MCP core)  ── BPSpec, ReadToSpec, SpecToBP,
                                 BPIRRoundtrip, CompileFunctionFromBody
            │
            ↓
   IBlueprintReader            ── pure-virtual interface
            │
   ┌────────┼────────┬─────────┬───────────┐
   ↓        ↓        ↓         ↓           ↓
 Mock  Commandlet  Socket  Caching   Auto / ReadOnly
            │
            ↓
   UnrealEditor-Cmd.exe -run=BPR -Op=...
   (BlueprintReaderCommandlet.cpp, in the plugin's Editor module)
            │
            ↓
   UE5 reflection + K2 graph APIs
```

The bottom three layers were already in place before the roundtrip
work; the top two are new. The interface is what lets the same
roundtrip code run against the mock backend (for serialization checks)
AND the commandlet backend (for actual BP materialization).

## Self-healing patterns

Several real bugs were found and fixed during the roundtrip work that
matter for any future commandlet-driven test loop:

### `DeleteAsset` disk-purge (`BlueprintReaderCommandlet.cpp`)

`ObjectTools::ForceDeleteObjects` in commandlet mode removes the
in-memory `UObject` but does NOT persist the deletion to disk. The
`.uasset` file lingers forever. Next-run `CreateBlueprint` then hits
its idempotency probe, "loads" the stale clone, and downstream
`AddVariable` returns exit=1 with "variable already exists".

**Fix** (commit `a2e746f`):
- `AR.ScanPathsSynchronous` on the containing folder before the
  registry lookup — picks up `.uasset` files saved by prior commandlet
  processes (their cache write didn't propagate to ours).
- After `ForceDeleteObjects`, always call `IFileManager::Get().Delete`
  on the `.uasset` path.
- When the registry doesn't know the asset but `-Force` is set + the
  file exists on disk, raw-delete the file. Returns `ok=true,
  already_absent=false, deleted=true`.
- When `-Force` is set + asset truly absent, soft-success with
  `already_absent=true`. Makes the test pattern
  `DeleteAsset(clone, force=true); CreateBlueprint(clone, ...)`
  idempotent.

### `UseCommandletResultAsExitCode = true` (`UBPRCommandlet`)

Without this flag, UE forces commandlet exit to 1 whenever ANY
Error-level log fires, even when our op explicitly returned 0. Several
expected no-op paths log at Error level (most notoriously the
`CreateBlueprint` idempotency probe's `LoadBlueprint: ... asset not in
registry`). Without the flag, callers see spurious failures on
successful ops.

### `BlueprintReaderCommandlet::RunCommand` — log capture

`std::system` with `>` redirect doesn't reliably populate `build.log`
on Windows in commandlet mode. Use `CreateProcessW` with redirected
handles (same pattern `CommandletBlueprintReader` uses for one-shot
ops). This bit us when Stage 5 first failed at "AddVariable:Health
exit=1, tail empty" — the empty tail was an artifact of the broken
log redirect, not a missing log.

## BPIR Stage 5 — body materialization

`CompileFunctionFromBody(reader, args)` is the public callable hoisted
out of `compile_function`'s MCP tool lambda. Same input shape, runs
directly against the reader. Used by BPIRRoundtrip Stage 5 to wire up
function bodies.

**Supported BPIR statement forms** (the compile_function DSL subset):

| BPIR form | K2 node |
|---|---|
| `{if, then, [else]}` | `K2Node_IfThenElse` (Branch) |
| `{set, to}` | `K2Node_VariableSet` |
| `{call, args}` | `K2Node_CallFunction` |
| `{comment}` | no-op (exec pass-through) |

**Unsupported forms** (recorded in `BPIRRoundtripResult::body_compile_failures`):

| BPIR form | K2 node | Why deferred |
|---|---|---|
| `{return}` | `K2Node_FunctionResult` | Need to find auto-spawned result node + wire to output pin defaults; no `find_node` op exists in ApplyOps yet |
| `{cast, to, as, success, fail}` | `K2Node_DynamicCast` | Branching + local var binding |
| `{switch, cases, default}` | `K2Node_Switch*` | Per-case routing |
| `{for_each, in, body}` | `ForEachLoop` macro | Macro expansion |
| `{while, body}` | `WhileLoop` macro | Same |
| `{sequence}` | `K2Node_ExecutionSequence` | Fan-out exec (could collapse to sequential) |
| `{break}` / `{continue}` | loop control nodes | Depend on for_each / while |
| `{broadcast}` / `{bind_delegate}` / `{unbind_delegate}` / `{clear_delegate}` | delegate K2 nodes | Each needs a target lookup |
| `{unsupported}` | safety valve | Per design — never round-trips |

For each unsupported form, the BPIR doc still serializes faithfully —
only the K2-graph materialization is missing. Adding a form means:
1. Extend `CompileStatement` in `tools/CompileFunction.cpp` (the
   `if (stmt.contains("X"))` chain at lines ~285–365).
2. Spawn the right K2 node via `Compiler.AddNode(...)`.
3. Wire exec + data pins.
4. Update the "Supported" line in `CompileFunction.h` description.

### Why dedup function names

Decompile synthesizes function-per-node handlers for unresolved
`EnhancedInputAction` nodes (when the `IA_*` asset can't be found,
each instance produces a placeholder `OnUnknownTriggered` /
`OnUnknownStarted` / `OnUnknownCompleted` function). TPC has three
unresolved EIA nodes → three duplicate `OnUnknownTriggered` entries
in `functions[]`. Stage 5 dedups by name and records the elided
entries in `body_compile_failures`.

## Extensibility points

| Want to | Touch |
|---|---|
| Add a new BPIR statement form | `tools/CompileFunction.cpp::CompileStatement` (+ list the form in `CompileFunction.h` description) |
| Add a new BPIR expression form | `tools/CompileFunction.cpp::CompileExpr` |
| Add a new K2 node kind to `AddNode` | `tools/BlueprintTools.cpp::list_node_kinds` + `BlueprintReaderCommandlet.cpp::RunAddNodeOp` + matching SpecToBP entry |
| Add a new commandlet op | `BlueprintReaderCommandlet.cpp` (EOp enum + ParseOp + dispatch table + Run<Op>Op) + `IBlueprintReader.h` (virtual) + all backend impls (`Mock`/`Commandlet`/`Socket`/`Caching`/`ReadOnly`/`Auto`) + register in `tools/BlueprintTools.cpp` |
| Add a new BP→source language | new codegen under `tools/codegen/` (alongside `CppEmit`/`CppClassEmit`) + register MCP tool that composes Decompile + new codegen |
| Add a new source→BP language | new parser under `tools/parse/` (alongside `CppLex`/`CppParse`) + Stage 4 of `BPIRRoundtrip` could use it for the round-trip-back direction |
| Make a new test BP | `BlueprintReaderSeedCommandlet.cpp` (synthesize the BP via UE's reflection APIs at seed time) |
| Import an editor template | `BPRoundtripSeedCommandlet.cpp` (uses `FPackageName::RegisterMountPoint` + `UEditorAssetLibrary::DuplicateAsset`) |

## Known limitations / open work

- **BPIR Stage 4 is passthrough.** Real C++ → BPIR parsing for whole
  classes isn't wired yet. The per-function `tools::ParseCppFunction`
  exists; lifting it to handle a UCLASS-and-friends declaration plus
  every UFUNCTION body is straightforward but not done. Once landed,
  `bpir_after` would be derived from the emitted C++ instead of
  passed through from `bpir_before`.

- **Body-form coverage.** `CompileFunctionFromBody` handles 4 of ~15
  BPIR statement forms. Functions whose bodies use unsupported forms
  end up with empty graphs in the clone, with the gap recorded in
  `body_compile_failures`.

- **Components.** SpecToBP handles components (CreateComponent +
  SetComponentProperty + parent wiring); BPIRRoundtrip Stage 5 does
  NOT — BPIR doesn't model components today (`Decompile` emits a
  `components[]` array but the codegen doesn't consume it and Stage 5
  doesn't loop over it). TPC's CameraBoom + FollowCamera don't make
  it into the BPIR clone.

- **Pin defaults on auto-spawned skeletons.** SpecToBP looks up the
  auto-spawned FunctionEntry / FunctionResult / lifecycle-event nodes
  via `FindNode` and routes connections through them. BPIRRoundtrip's
  Stage 5 doesn't — bodies wire from the entry's `then` correctly
  (because compile_function emits the right tail), but output pin
  defaults on the FunctionResult are not set, so a `{return: {lit:
  false}}` statement is currently soft-failed (no `__result` slot in
  the compile_function DSL yet).

- **Worktree build cost.** Building the editor target from a
  `.claude/worktrees/<name>/` worktree forces UBT to rebuild ~3000
  modules including the engine, because UBT generates intermediates
  per project location. The workflow is: edit in worktree (for git
  isolation), `cp` the edited source files into the parent project's
  matching path, build the parent. Painful but unavoidable until UE's
  intermediate cache learns to follow git worktrees.

## See also

- `CLAUDE.md` (project root) — build commands, env vars, common gotchas
- [`bp-reader-extensibility-audit.md`](bp-reader-extensibility-audit.md) — what's pluggable, what isn't
- [`syntactic-sugar-nodes.md`](syntactic-sugar-nodes.md) — K2 nodes that desugar at compile time + their C++ equivalents (informs the BPIR statement set)
- [`editor-automation.md`](editor-automation.md) — commandlets vs Python vs RemoteControl vs ITF
- The plan: `docs/superpowers/plans/2026-05-16-bp-roundtrip-capability.md`
