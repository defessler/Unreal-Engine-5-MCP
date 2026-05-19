# Lyra Blueprint → C++ conversion case study

> Snapshot: 2026-05-19 — All 302 blueprint assets in the Lyra Starter
> Game converted to C++ stubs in a sibling plugin; `LyraEditor` target
> compiles and the commandlet runs cleanly.

## Goal

Pivot the bp-reader project from the synthetic `UE5_MCP` testbed to
the real-world Lyra Starter Game. Replace every BP (`B_*`, `BP_*`,
`W_*`, `WBP_*`, `ABP_*`) with a generated C++ class while keeping the
editor target buildable and runnable.

## Result

| Metric | Value |
|--------|-------|
| BP assets in `Content/` and game-feature plugins | 302 |
| C++ stubs generated under `Plugins/LyraGenerated/Source/LyraGenerated/Private/Generated/` | 302 |
| Full transpiles (with member vars / components) | ~17 (legacy snapshot) |
| Stubs (declared parent + UCLASS marker) | ~285 |
| `LyraEditor` target | Builds clean |
| `LyraEditor-Cmd.exe` commandlet | Runs (BPR `List` op completes; exit reflects pre-existing Lyra config noise) |

The two "errors" still visible in editor startup logs
(`B_PhysicsTest` referencing a missing `SM_1M_Cube`, `TopDownArena`
declaring `Map` as `bIsEditorOnly=0` while another system expects
`=1`) are **pre-existing Lyra content issues** present in the sample
project before any conversion — verified against earlier backup logs.

## Pipeline shape

```
            +------------------+
.uasset  -->| parent probe     |  grep `/Script/Module.Class` + sibling-BP refs
            +------------------+
                     |
                     v
            +------------------+
            | class map        |  lyra_class_map.json (257 Lyra classes),
            |                  |  ENGINE_MAP (built-in UE), PLUGIN_MAP (CommonInput
            |                  |  /CommonGame/Modular* etc.)
            +------------------+
                     |
                     v
            +------------------+
            | stub emit        |  .h: parent #include + UCLASS w/ GENERATED_BODY
            |                  |  .cpp: pass-through #include
            +------------------+
                     |
                     v
            +------------------+
            | LyraGenerated    |  UBT module — links against LyraGame +
            | plugin           |  every plugin that hosts a stubbed parent
            +------------------+
```

The decompile→transpile path used for the 17 "full" classes ran
through the existing BPIR pipeline:
`decompile_blueprint -> CppClassEmit -> compile`. Stubs skip the BPIR
step entirely — they only need parent class + header path.

## Lessons learned (project-specific)

### 1. Monolithic project modules don't export symbols

`Source/LyraGame/` ships as one big module with no `LYRAGAME_API`
markers on any class. The first wave of stub builds failed with
LNK2019 on ~30 Lyra classes because the linker had no DLL-exported
symbols to bind against from `LyraGenerated`.

Fix: scripted insertion of `LYRAGAME_API` on every parent class
referenced by a stub (28 classes). Skipped forward declarations to
avoid `class LYRAGAME_API ULyraFoo;` showing up where it doesn't help.
This is invasive but unavoidable when generating derivative classes
from outside the source module.

### 2. Game-feature plugins keep parent classes in `Private/`

Five BPs in `ShooterTests` / `ShooterCore` inherit from classes whose
headers live in the plugin's `Private/` folder. External modules can't
include them. Fix: moved the four affected headers
(`ShooterTestAsyncMessageTestActor.h`,
`ShooterTestsDevicePropertyTester.h`,
`TDM_PlayerSpawningManagmentComponent.h`) from `Private/` to
`Public/`, added `SHOOTERCORERUNTIME_API` /
`SHOOTERTESTSRUNTIME_API`, and bumped a couple of constructors from
default-private to `public:`.

### 3. `TargetBuildEnvironment.Unique` is contagious

`LyraEditor.Target.cs` requires `BuildEnvironment = Unique` for
`bUseLoggingInShipping` etc. That produces a unique `BuildId` and
unique DLL names (`LyraEditor-Foo.dll` instead of
`UnrealEditor-Foo.dll`), so launching the *shared* `UnrealEditor-Cmd`
hits "Skipping out-of-date modules" warnings — you have to launch the
project-local `LyraEditor-Cmd.exe` to load the matching DLLs.

### 4. Module dependency chain for generated stubs

`LyraGenerated.Build.cs` needs every module that contributes a parent
class — empirically: `LyraGame`, `CommonUI`, `CommonInput`,
`CommonGame`, `ModularGameplay`, `ModularGameplayActors`,
`GameSettings`, `GameFeatures`, `GameplayMessageRuntime`,
`ShooterCoreRuntime`, `TopDownArenaRuntime`, `ShooterTestsRuntime`,
plus engine basics (`AIModule`, `NavigationSystem`, `PhysicsCore`,
`Niagara`, etc.).

### 5. Class-name prefix follows parent, not file

A BP named `BP_Mannequin_Base` inheriting from `ULyraAnimInstance`
generates `class UBP_Mannequin_Base`, not `UBP_*` / `ABP_*` based on
the filename prefix. The `ChoosePrefixFor` logic in `CppClassEmit.cpp`
honors this; the stub generator follows by extracting the actual
class prefix from the parent stub before emitting derivatives.

### 6. Skip dead-end stubs early

Two BPs (`BP_Event_Broadcaster`, `B_TeamSpawningRules`) initially
emitted stubs that triggered C1083 because the script assumed the
parent header would be visible. Better to skip-and-record than emit
a known-failing stub — but for Lyra we moved the headers instead.

### 7. Pre-existing Lyra config warnings persist

Two non-fatal Lyra startup errors (the `B_PhysicsTest` mesh and the
`TopDownArena` Map asset-type mismatch) survive the conversion
because they're rooted in `.uasset` content and `Config/*.ini`, not
in any BP graph. They are out of scope for BP→C++ work.

## Reusability for other UE projects

The pipeline isn't Lyra-specific. To apply to another project:

1. Run the BP inventory glob (`Content/**/B_*.uasset` etc.).
2. Build a class map from the project's source by scanning
   `Source/**/Public/**/*.h` for `class [API_] U?A?[A-Z]…`.
3. Run the parent-probe script per .uasset — heuristic is robust
   because UE always packs `/Script/Module.Class` strings in the
   header.
4. Emit stubs into a sibling plugin's `Source/<Plugin>/Private/
   Generated/`.
5. Discover linker errors and append `<MODULE>_API` to each
   referenced parent class.

The whole loop for Lyra took ~5 build cycles end-to-end (each ~30 s
incremental); the longest lever was figuring out which Private/
headers needed Public/ exposure.

## What stubs do *not* give you

Stubs don't replicate behavior — the original Blueprint graphs are
still the runtime contract. The C++ side is purely a type/structure
mirror so external C++ code can reference these classes by typed
pointer. To replace behavior, the BPIR pipeline
(`decompile_function → CppEmit → compile_function`) does the actual
expression-level translation; that's a per-graph effort, not a bulk
operation.

## See also

- `bp-roundtrip-architecture.md` — how BPIR is shaped and why
- `editor-automation.md` — programmatic editor control patterns
- `Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/tools/codegen/CppClassEmit.cpp` — the class-level emitter
