# BP Roundtrip Capability Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended for the parallel research tasks 1–5) or superpowers:executing-plans (for the sequential code tasks 9+). Steps use checkbox (`- [ ]`) syntax for tracking. Inside the active Ralph loop, do not stop to ask the user for approval between tasks; pick up at the first unchecked task each iteration.

**Goal:** Prove the bp-reader server can read an arbitrary complex Blueprint with the read tools and rebuild it with the write tools (two paths — granular writes AND BPIR transpile-roundtrip), backed by research notes documenting the surrounding UE5.7 ecosystem.

**Architecture:** Two phases. **Phase 1 (research-first per user override):** seven md files under `docs/research/` covering UE5.7 BP tooling, plus importing the UE5 ThirdPerson template to drive Phase 2. **Phase 2 (code):** new library subdir `Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/roundtrip/` holding `BPSpec`/`ReadToSpec`/`SpecToBP`/`BPIRRoundtrip`; new plugin op `EOp::StructuralDiff` + helper file `BlueprintStructuralDiff.{h,cpp}`; new MCP tool `bp_structural_diff`; new UBT module `BPRoundtripModule` as the compile target for the BPIR-path generated C++; functional tests covering both paths against `BP_TestEnemy` (smoke) and TPC (full). Completion sentinel `RALPH-DONE-BP-ROUNDTRIP` is emitted only when both roundtrip paths produce empty-diff clones (per `bp_structural_diff` allowing whitelisted exemptions) and all new tests pass.

**Tech Stack:** UE 5.7.4 (source build at sibling `D:\Projects\Unreal Engine 5\`), C++20, UBT, doctest, nlohmann/json, FJsonObjectConverter. New code follows the existing `bpr::backends` / `bpr::tools` namespace and the snake_case wire-key convention pinned by `BlueprintReaderTypes.h`.

**Spec:** [`docs/superpowers/specs/2026-05-16-bp-roundtrip-capability-design.md`](../specs/2026-05-16-bp-roundtrip-capability-design.md)

---

## File map (locked-in decomposition)

**New files:**

```
docs/research/
  README.md                                                                 (Task 0)
  ue5.7-overview.md                                                         (Task 1)
  editor-automation.md                                                      (Task 2)
  scripting-languages.md                                                    (Task 3)
  syntactic-sugar-nodes.md                                                  (Task 4)
  bp-reader-extensibility-audit.md                                          (Task 5)
  tpc-anatomy.md                                                            (Task 8)

Plugins/BlueprintReader/Source/BlueprintReaderEditor/
  Public/BlueprintStructuralDiff.h                                          (Task 12)
  Private/BlueprintStructuralDiff.cpp                                       (Task 12)
  Private/BPRoundtripSeedCommandlet.h                                       (Task 6)
  Private/BPRoundtripSeedCommandlet.cpp                                     (Task 6)

Plugins/BlueprintReader/Source/BPRoundtripModule/                           (Task 16)
  BPRoundtripModule.Build.cs
  Private/BPRoundtripModule.cpp
  Private/.gitkeep                                                          (placeholder so emitted code dir tracks)

Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/roundtrip/
  BPSpec.h                                                                  (Task 9)
  BPSpec.cpp                                                                (Task 9)
  ReadToSpec.h                                                              (Task 10)
  ReadToSpec.cpp                                                            (Task 10)
  SpecToBP.h                                                                (Task 11)
  SpecToBP.cpp                                                              (Task 11)
  BPIRRoundtrip.h                                                           (Task 17)
  BPIRRoundtrip.cpp                                                         (Task 17)

Plugins/BlueprintReader/Tests/BlueprintReaderMcpTests/Private/
  test_structural_diff.cpp                                                  (Task 18)
  test_bpspec.cpp                                                           (Task 9 — unit-tests for serializer)
  test_roundtrip_granular.cpp                                               (Tasks 19, 20)
  test_roundtrip_bpir.cpp                                                   (Tasks 21, 22)

Plugins/BlueprintReader/Tests/BlueprintReaderMcpTests/fixtures/
  BP_TestEnemy_spec.json                                                    (Task 19 — golden)
  BP_TPC_spec.json                                                          (Task 20 — golden)
  BP_TestEnemy_bpir.json                                                    (Task 21 — golden)
  BP_TPC_bpir.json                                                          (Task 22 — golden)

Content/Imported/ThirdPerson/                                               (Task 7 — committed)
  BP_ThirdPersonCharacter.uasset, ABP_Manny.uasset,
  BP_ThirdPersonGameMode.uasset, BP_ThirdPersonPlayerController.uasset,
  Input/IA_*.uasset, IMC_*.uasset

Content/Recreated/                                                          (Task 7 — gitignored)
  .gitkeep
  .gitignore                                                                (ignores everything except .gitkeep)
```

**Modified files:**

```
Plugins/BlueprintReader.uplugin                                             (Task 16 — add module entry)
Plugins/BlueprintReader/Source/BlueprintReaderEditor/Private/
  BlueprintReaderCommandlet.cpp                                             (Task 13 — EOp::StructuralDiff)
Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/backends/
  IBlueprintReader.h                                                        (Task 14 — pure-virtual)
  MockBlueprintReader.{h,cpp}                                               (Task 14 — throw not-supported)
  CommandletBlueprintReader.{h,cpp}                                         (Task 14 — emit -Op=StructuralDiff)
  SocketBlueprintReader.{h,cpp}                                             (Task 14 — emit StructuralDiff frame)
  CachingBlueprintReader.{h,cpp}                                            (Task 14 — passthrough)
  ReadOnlyBlueprintReader.{h,cpp}                                           (Task 14 — throw read-only)
  AutoBlueprintReader.{h,cpp}                                               (Task 14 — forwarder)
Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/tools/
  BlueprintTools.cpp                                                        (Task 15 — register bp_structural_diff)
Plugins/BlueprintReader/Tests/BlueprintReaderMcpTests/Private/
  test_tools.cpp                                                            (Task 15 — bump 126 → 127)
  test_mcp.cpp                                                              (Task 15 — bump 126 → 127)
.gitignore                                                                  (Task 16 — Recreated/, BPRoundtripModule emitted code)
```

---

## Phase 1 — Research (tasks parallelizable: 1–5)

### Task 0: Scaffold `docs/research/`

**Files:**
- Create: `docs/research/README.md`

- [ ] **Step 1: Create the index file**

```markdown
# Research notes — UE5.7+ BP tooling

This directory collects research notes informing the BP-reader server's
roadmap. See [`docs/superpowers/specs/2026-05-16-bp-roundtrip-capability-design.md`](../superpowers/specs/2026-05-16-bp-roundtrip-capability-design.md)
for the driving spec.

## Files

- [`ue5.7-overview.md`](ue5.7-overview.md) — what's new in UE5.7 that affects BP tooling.
- [`editor-automation.md`](editor-automation.md) — every lever for headless / scripted editor work.
- [`scripting-languages.md`](scripting-languages.md) — Lua, AngelScript, Verse: how each hooks the editor.
- [`syntactic-sugar-nodes.md`](syntactic-sugar-nodes.md) — K2 nodes that desugar at compile time, with C++ equivalents.
- [`bp-reader-extensibility-audit.md`](bp-reader-extensibility-audit.md) — current seams + how to extend.
- [`tpc-anatomy.md`](tpc-anatomy.md) — full inventory of the UE5 ThirdPerson template (drives the roundtrip tests).
```

- [ ] **Step 2: Commit**

```bash
git add docs/research/README.md
git commit -m "docs(research): scaffold docs/research/ index"
```

---

### Task 1: `docs/research/ue5.7-overview.md`

**Files:**
- Create: `docs/research/ue5.7-overview.md`

**Required topics (each must have at least one subsection):**
1. UE5.7 release timeline & supported toolchain (Windows MSVC version, .NET SDK requirement for UBT, supported VS / Rider versions).
2. K2 / BlueprintGraph subsystem changes since 5.4 baseline (new K2Node_* classes, changed pin metadata shape, deprecated `K2Node_Tunnel` patterns).
3. Asset registry changes affecting `ListBlueprints` / `FindNode` (new asset tags, tag-cache invalidation).
4. EnhancedInput — now the default in UE5.7 templates (so TPC will use it; the ReadToSpec layer must surface it).
5. Async-action / UBlueprintAsyncActionBase changes — anything new since the introspector's existing handling.
6. Gameplay Ability System tagged-event changes (only summarize; full GAS coverage out of scope).
7. CommonUI status (production-ready in 5.7? affects WBP reading).
8. Editor extensibility API changes (FUICommandList, FSlateApplication, EditorUtilityWidget API).
9. Things bp-reader will need to handle: any deprecated node classes still appearing in old assets; any new node classes that lack BPIR coverage.

**Sources to consult:**
- `https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-5.7-release-notes` (release notes)
- `https://github.com/EpicGames/UnrealEngine/blob/release/Engine/Source/Editor/BlueprintGraph/Classes/` (K2Node_* source)
- The local engine source at `D:\Projects\Unreal Engine 5\Engine\Source\Editor\BlueprintGraph\`
- Existing `docs/design/10-bp-to-cpp-node-coverage.md` for what bp-reader currently knows about.

- [ ] **Step 1: WebFetch the release notes URL above and skim**

- [ ] **Step 2: Grep the local engine for `K2Node_` class declarations to inventory current shape**

```bash
ls "/d/Projects/Unreal Engine 5/Engine/Source/Editor/BlueprintGraph/Classes/" | grep K2Node | head -50
```

- [ ] **Step 3: Write the file** (target ~400-700 lines; cross-link to engine source and to existing `docs/design/10-*` for node coverage)

- [ ] **Step 4: Add the file's link to `docs/research/README.md`** (already there from Task 0; just verify)

- [ ] **Step 5: Commit**

```bash
git add docs/research/ue5.7-overview.md
git commit -m "docs(research): UE5.7 overview — what's new for BP tooling"
```

---

### Task 2: `docs/research/editor-automation.md`

**Files:**
- Create: `docs/research/editor-automation.md`

**Required topics:**
1. **Commandlets** — `UCommandlet` subclass pattern, `-run=<Name>` invocation, daemon-mode idioms (one persistent process, line-delimited stdin). What `BPRCommandlet` / `BPRSeedCommandlet` already do. Limitations: no Slate UI, no PIE worldscape.
2. **Python EditorScripting** — `unreal` module entrypoint, `EditorUtilityWidget` + Python combo, `BP_READER_ALLOW_PYTHON` gating in this server. Pros (rapid editor scripting); cons (per-call interpreter cost, no parallelism).
3. **Editor Utility Widgets / Blueprints / Actors** — the BP-based automation surface. When to prefer over commandlets (interactive UI, designer-driven workflows).
4. **ScriptableTool / InteractiveToolsFramework** — designed for editor tools authored in C++. Where ITF fits relative to EUW.
5. **RemoteControl plugin** — HTTP / WebSocket API. Editor must be running. How it overlaps with bp-reader's live backend.
6. **AutomationTestFramework** — UE's built-in functional test runner. Used for `BlueprintReader.*` automation tests (see `Plugins/BlueprintReader/Tests/`).
7. **Console-command-from-CLI** — `UnrealEditor-Cmd.exe ... -ExecCmds="..."` for one-shot commands without commandlets.
8. **MCP integration angle** — what bp-reader uses today (commandlet daemon + live socket) and what it could use that it doesn't (Python for in-editor scripts; RemoteControl for parallel HTTP-style ops; AutomationTestFramework for nightly verification).
9. **Trade-off matrix** at the end: rows = automation lever, columns = startup cost / parallelism / editor-required / scripting-language / good-for-AI-agents.

**Sources to consult:**
- `https://dev.epicgames.com/documentation/en-us/unreal-engine/python-api-for-the-unreal-editor-in-unreal-engine`
- `https://dev.epicgames.com/documentation/en-us/unreal-engine/remote-control-overview-for-unreal-engine`
- Local engine: `Engine/Source/Editor/UnrealEd/Public/Commandlets/`, `Engine/Plugins/Editor/EditorUtilityWidgets/`
- Existing `CLAUDE.md` for what bp-reader uses.

- [ ] **Step 1: Inventory each lever (web docs + local engine source)**

- [ ] **Step 2: Write the file with the trade-off matrix at the end**

- [ ] **Step 3: Commit**

```bash
git add docs/research/editor-automation.md
git commit -m "docs(research): editor automation lever inventory"
```

---

### Task 3: `docs/research/scripting-languages.md`

**Files:**
- Create: `docs/research/scripting-languages.md`

**Required topics:**
1. **AngelScript (Hazelight fork)** — repo (`https://github.com/Hazelight/UnrealEngine-Angelscript`), how it hooks the editor (custom `UClass` synthesis at startup, hot-reload pipeline), syntax compared to BP (closest of the three), where its parser lives (`Engine/Plugins/Angelscript/Source/`), how UFUNCTIONs are registered for both BP-callable and AS-callable. Concrete "if bp-reader added an AS backend" sketch — what `ReadAngelscriptFunction` would look like, what `transpile_function` → AS would do.
2. **UnLua / sol2-based Lua** — `https://github.com/Tencent/UnLua` is the production-ish path. How it patches `UObject::ProcessEvent` to route to Lua handlers. AST? — no, Lua source is parsed by Lua itself, then wrapped. Implication: a "BP → Lua" transpile is a pretty-printer, not a structured AST emit, unless you go via BPIR. Sketch of `transpile_function` → Lua.
3. **Verse** — UEFN-only as of UE5.7 (mainline UE doesn't ship Verse; confirm). Static, deterministic, transactional language. Module structure (`.verse` files, `Verse.toml` packaging). Why it doesn't fit the bp-reader pattern (no C++-host introspection; Verse VM is its own runtime). What changes when/if Verse lands in mainline UE6.
4. **Comparison table** — for each language: maturity, hook surface (asset-time / runtime / both), AST availability, syntax similarity to BP, debugger story, mainline-supported (yes/no/UEFN-only), recommended bp-reader integration approach (1st-class backend / pretty-printer only / not feasible).
5. **`IScriptBackend` abstraction sketch** — name + signatures. Not implemented, just documented for the extensibility audit to cross-reference.

**Sources to consult:**
- AngelScript repo + docs
- UnLua repo
- `https://dev.epicgames.com/documentation/en-us/uefn/verse-language-reference`
- Existing `tools/codegen/CppEmit.h` for the abstraction shape any new backend would follow.

- [ ] **Step 1: Web research per language**

- [ ] **Step 2: Write the file**

- [ ] **Step 3: Commit**

```bash
git add docs/research/scripting-languages.md
git commit -m "docs(research): Lua / AngelScript / Verse editor hooks"
```

---

### Task 4: `docs/research/syntactic-sugar-nodes.md`

**Files:**
- Create: `docs/research/syntactic-sugar-nodes.md`

**Required topics:**
1. **Definition** — what counts as "syntactic sugar" in BP: a K2Node_* that the BP compiler expands into a graph of simpler primitives at compile time (vs. an opaque node that holds runtime behavior).
2. **Inventory** — table of every sugar node with: K2 class, what it desugars to, recommended C++ idiom, bp-reader BPIR handling status. Must include at minimum:
   - `K2Node_IfThenElse` (Branch) → C++ `if (...) {...} else {...}`
   - `K2Node_ExecutionSequence` (Sequence) → C++ ordered statements
   - `K2Node_MultiGate` → C++ index-bounded dispatch
   - `K2Node_Switch` (and subclasses Int/String/Enum/Name/Class) → C++ `switch` or chained `if`
   - `K2Node_DynamicCast` → C++ `Cast<>` + null-check
   - `K2Node_MacroInstance` for the standard library macros (`ForEachLoop`, `ForEachLoopWithBreak`, `ReverseForEachLoop`, `WhileLoop`, `Gate`, `DoOnce`, `IsValid`, `Select`, `FlipFlop`)
   - `K2Node_Knot` (reroute) — passthrough sugar
   - `K2Node_Composite` (collapsed graph) — internal grouping; expanded in place
   - `K2Node_Tunnel` (input/output of collapsed/macro graphs)
   - `K2Node_FormatText` → C++ `FString::Format`
   - `K2Node_MakeArray` / `K2Node_MakeMap` / `K2Node_MakeSet` / `K2Node_MakeStruct`
3. **Anti-pattern: 'just call the BP function library'** — for things like `MakeArray`, the recommended C++ idiom is the language construct (`TArray<T>{...}`), NOT calling `UKismetArrayLibrary::Array_Add` in a loop. Document for each row.
4. **Why this matters for bp-reader** — `transpile_function` needs to emit the *idiomatic* C++ form, not just call the kismet library. Cross-link rows to `docs/design/10-bp-to-cpp-node-coverage.md` and `CppEmit.cpp` line numbers showing existing handlers.
5. **Coverage gaps** — any sugar node not yet handled by CppEmit (audit by grepping for `K2Node_` mentions in `tools/codegen/CppEmit.cpp` vs. the inventory).

**Sources to consult:**
- Local engine: `Engine/Source/Editor/BlueprintGraph/Classes/K2Node*.h`
- `Engine/Content/EditorBlueprintResources/StandardMacros.uasset` (the macro library)
- Existing `docs/design/10-bp-to-cpp-node-coverage.md`
- `tools/codegen/CppEmit.cpp` for current handling

- [ ] **Step 1: Inventory K2Node_* sugar classes from engine source**

```bash
ls "/d/Projects/Unreal Engine 5/Engine/Source/Editor/BlueprintGraph/Classes/" | grep -E "K2Node_(IfThenElse|ExecutionSequence|MultiGate|Switch|DynamicCast|MacroInstance|Knot|Composite|Tunnel|FormatText|MakeArray|MakeMap|MakeSet|MakeStruct)" 
```

- [ ] **Step 2: Cross-reference with `docs/design/10-bp-to-cpp-node-coverage.md`**

- [ ] **Step 3: Grep CppEmit.cpp for which are actually handled**

```bash
grep -n "K2Node_\|kBranch\|kSequence\|kMakeArray\|kSwitch\|kCast" Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/tools/codegen/CppEmit.cpp | head -60
```

- [ ] **Step 4: Write the file with the full table + coverage gaps**

- [ ] **Step 5: Commit**

```bash
git add docs/research/syntactic-sugar-nodes.md
git commit -m "docs(research): syntactic-sugar K2 nodes + C++ idiom map"
```

---

### Task 5: `docs/research/bp-reader-extensibility-audit.md`

**Files:**
- Create: `docs/research/bp-reader-extensibility-audit.md`

**Required topics:**
1. **The seams** — for each abstraction layer, document the extension surface:
   - `bpr::backends::IBlueprintReader` (file: `Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/backends/IBlueprintReader.h`) — pure-virtual surface. To add a new backend: subclass, implement every method, register in `backends/BackendFactory.cpp`. Cost: ~one method per new tool; default no-op pattern available (see `ReadDataTable` for the throws-not-supported default).
   - `bpr::tools::ToolRegistry` (file: `Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/tools/ToolRegistry.h`) — registry of MCP tools. To add a new tool: append a `ToolDescriptor` + lambda in `BlueprintTools.cpp`'s `RegisterBlueprintTools`. Bump `spec.size() == N` in tests.
   - `bpr::tools::Bpir` (file: `tools/Bpir.h`) — the JSON AST used as the BP↔source pivot. To add a new statement / expression node type: add to the `BpirStatement` / `BpirExpression` variant; add a `Decompile.cpp` recognizer; add a `CppEmit.cpp` lowerer (and a `CppParse.cpp` parser if the language→BPIR direction needs it).
   - `bpr::tools::codegen::CppEmit` — code emitter for the C++ target. To add a new target language: write `<Lang>Emit.h/.cpp` next to it, take BPIR in, emit string out. Parallel structure.
   - `bpr::tools::parse::CppParse` — C++ source to BPIR. Symmetric story for new languages.
   - Plugin-side `BlueprintReaderCommandlet::EOp` — the op enum. To add a new op: enum value, `ParseOp` entry, dispatch arm in `RunOneOp`, `RunFooOp` impl. Same cycle through `Mock/Commandlet/Live/...BlueprintReader` is documented in CLAUDE.md.
2. **Concrete leak audit** — for each seam, list issues found by reading current code, marked `[blocking-Phase-2]` (must fix for roundtrip work) or `[non-blocking]` (worth noting; defer to follow-up). Audit at minimum:
   - Any `IBlueprintReader` method whose return type leaks an implementation detail (e.g. relies on commandlet-only state).
   - Any tool descriptor that is registered but missing from `MockBlueprintReader` (mock would throw at runtime when tested in isolation).
   - Any BPIR node that has decompile-side handling but no transpile-side lowering, or vice versa.
   - Any backend method that has a non-trivial default impl in `IBlueprintReader` but isn't overridden by `Mock` (mock will throw — fine for write tools, surprising for reads).
3. **Extension cookbooks** — three step-by-step examples (don't actually implement, just sketch):
   - "Add a new Lua backend." (Where files go, what to register, what tests to write.)
   - "Add a new statement kind to BPIR." (Variant entry, decompile recognizer, cpp-emit lowering.)
   - "Add a new MCP tool that reads project state." (Tool registration, mock fixture, live test gating.)
4. **Cross-references** — link each cookbook step to a line range in existing code where the pattern is demonstrated.

**Sources to consult:**
- All `Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/backends/*.{h,cpp}`
- `tools/BlueprintTools.cpp`, `tools/Bpir.h`, `tools/codegen/`, `tools/parse/`
- Plugin-side `BlueprintReaderCommandlet.cpp`

- [ ] **Step 1: Inventory each seam by reading the relevant files**

- [ ] **Step 2: Run the leak audit (grep tool registrations against mock impls)**

```bash
grep -n "registry.Add\|d.name" Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/tools/BlueprintTools.cpp | grep "d.name" | head -50
grep -n "^\s*[A-Z].*Result\s*[A-Za-z]\+\s*(" Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/backends/MockBlueprintReader.cpp | head -50
```

- [ ] **Step 3: Write the file. For each leak found, give file:line and the recommended fix.**

- [ ] **Step 4: For any `[blocking-Phase-2]` leak, file a follow-up task into this plan at the end (extend the task list).**

- [ ] **Step 5: Commit**

```bash
git add docs/research/bp-reader-extensibility-audit.md
git commit -m "docs(research): bp-reader extensibility audit"
```

---

### Task 6: BPRoundtripSeed commandlet

**Files:**
- Create: `Plugins/BlueprintReader/Source/BlueprintReaderEditor/Private/BPRoundtripSeedCommandlet.h`
- Create: `Plugins/BlueprintReader/Source/BlueprintReaderEditor/Private/BPRoundtripSeedCommandlet.cpp`

**Purpose:** copies the UE5 ThirdPerson template assets from `<EngineDir>/Templates/TP_ThirdPersonBP/Content/` into `<ProjectDir>/Content/Imported/ThirdPerson/`, fixing up references so the BPs load in this project. Idempotent.

- [ ] **Step 1: Locate the engine template path**

```bash
ls "/d/Projects/Unreal Engine 5/Engine/Templates/" | grep -i ThirdPerson
ls "/d/Projects/Unreal Engine 5/Engine/Templates/TP_ThirdPersonBP/Content/" 2>&1 | head -10
```

Expected: `TP_ThirdPersonBP/` directory with `Content/Characters/`, `Content/Blueprints/`, `Content/Input/`, etc.

- [ ] **Step 2: Write the header**

```cpp
// BPRoundtripSeedCommandlet — imports the engine's ThirdPerson template
// into Content/Imported/ThirdPerson/ so the roundtrip tests have a
// real-world BP target. Idempotent: re-running is safe.
//
// Run: UnrealEditor-Cmd.exe <project> -run=BPRoundtripSeed -nullrhi
//      -nosplash -unattended -nopause
#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "BPRoundtripSeedCommandlet.generated.h"

UCLASS()
class UBPRoundtripSeedCommandlet : public UCommandlet
{
    GENERATED_BODY()
public:
    UBPRoundtripSeedCommandlet();
    virtual int32 Main(const FString& Params) override;
};
```

- [ ] **Step 3: Write the implementation (handles asset copy + reference fix-up via UAssetTools)**

```cpp
#include "BPRoundtripSeedCommandlet.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "EditorAssetLibrary.h"
#include "HAL/PlatformFilemanager.h"
#include "IAssetTools.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogBPRoundtripSeed, Log, All);

UBPRoundtripSeedCommandlet::UBPRoundtripSeedCommandlet() {
    IsClient    = false;
    IsServer    = false;
    IsEditor    = true;
    LogToConsole = true;
    ShowErrorCount = true;
}

namespace {
    struct FCopySpec {
        FString SourceContentRelative;   // e.g. "Blueprints/BP_ThirdPersonCharacter"
        FString DestPackagePath;         // e.g. "/Game/Imported/ThirdPerson/BP_ThirdPersonCharacter"
    };

    FString EngineTemplateContentDir() {
        // Engine layout: <EngineDir>/Templates/TP_ThirdPersonBP/Content/
        const FString Eng = FPaths::EngineDir();
        return FPaths::Combine(Eng, TEXT("Templates"), TEXT("TP_ThirdPersonBP"),
                               TEXT("Content"));
    }

    bool CopyOneAsset(const FString& SourceFile, const FString& DestPackagePath,
                      IAssetTools& AssetTools) {
        if (UEditorAssetLibrary::DoesAssetExist(DestPackagePath)) {
            UE_LOG(LogBPRoundtripSeed, Display, TEXT("Skipping (exists): %s"),
                   *DestPackagePath);
            return true;
        }
        // Load source from disk by ObjectPath (template assets are
        // referenced via /Game/... in the template; we need an absolute
        // load).
        const FString SourceObjectPath = FString::Printf(
            TEXT("/Game/%s.%s"), *FPaths::GetBaseFilename(SourceFile, false),
            *FPaths::GetBaseFilename(SourceFile));
        UObject* Loaded = UEditorAssetLibrary::LoadAsset(SourceObjectPath);
        if (!Loaded) {
            UE_LOG(LogBPRoundtripSeed, Error,
                   TEXT("Failed to load source asset: %s"), *SourceObjectPath);
            return false;
        }
        const FString DestObjectPath = FString::Printf(TEXT("%s.%s"),
            *DestPackagePath, *FPaths::GetBaseFilename(DestPackagePath));
        UObject* Duplicated = UEditorAssetLibrary::DuplicateAsset(
            SourceObjectPath, DestPackagePath);
        if (!Duplicated) {
            UE_LOG(LogBPRoundtripSeed, Error,
                   TEXT("Failed to duplicate %s -> %s"),
                   *SourceObjectPath, *DestPackagePath);
            return false;
        }
        UEditorAssetLibrary::SaveAsset(DestObjectPath, /*bOnlyIfIsDirty=*/false);
        UE_LOG(LogBPRoundtripSeed, Display, TEXT("Copied %s -> %s"),
               *SourceObjectPath, *DestObjectPath);
        return true;
    }
}

int32 UBPRoundtripSeedCommandlet::Main(const FString& Params) {
    UE_LOG(LogBPRoundtripSeed, Display,
           TEXT("BPRoundtripSeed: importing TP_ThirdPersonBP into "
                "/Game/Imported/ThirdPerson/"));

    // Engine template doesn't load through /Game/ — we mount its Content
    // dir under a temporary /Template/ root, then duplicate from there.
    const FString TemplateContentDir = EngineTemplateContentDir();
    if (!FPaths::DirectoryExists(TemplateContentDir)) {
        UE_LOG(LogBPRoundtripSeed, Error,
               TEXT("Template content dir not found: %s"), *TemplateContentDir);
        return 1;
    }
    FPackageName::RegisterMountPoint(TEXT("/TPTemplate/"), TemplateContentDir);
    ON_SCOPE_EXIT { FPackageName::UnRegisterMountPoint(TEXT("/TPTemplate/"),
                                                       TemplateContentDir); };

    // Force the asset registry to scan the mounted dir.
    auto& ARModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
        "AssetRegistry");
    ARModule.Get().ScanPathsSynchronous({ TemplateContentDir }, /*bForce=*/true);

    auto& ATModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(
        "AssetTools");
    IAssetTools& AssetTools = ATModule.Get();

    const TArray<TPair<FString, FString>> Copies = {
        // Source mount path                 → Dest package path
        { TEXT("/TPTemplate/Blueprints/BP_ThirdPersonCharacter"),
          TEXT("/Game/Imported/ThirdPerson/BP_ThirdPersonCharacter") },
        { TEXT("/TPTemplate/Blueprints/BP_ThirdPersonGameMode"),
          TEXT("/Game/Imported/ThirdPerson/BP_ThirdPersonGameMode") },
        { TEXT("/TPTemplate/Blueprints/BP_ThirdPersonPlayerController"),
          TEXT("/Game/Imported/ThirdPerson/BP_ThirdPersonPlayerController") },
        { TEXT("/TPTemplate/Characters/Mannequins/Animations/ABP_Manny"),
          TEXT("/Game/Imported/ThirdPerson/ABP_Manny") },
        { TEXT("/TPTemplate/Input/Actions/IA_Move"),
          TEXT("/Game/Imported/ThirdPerson/Input/IA_Move") },
        { TEXT("/TPTemplate/Input/Actions/IA_Look"),
          TEXT("/Game/Imported/ThirdPerson/Input/IA_Look") },
        { TEXT("/TPTemplate/Input/Actions/IA_Jump"),
          TEXT("/Game/Imported/ThirdPerson/Input/IA_Jump") },
        { TEXT("/TPTemplate/Input/IMC_Default"),
          TEXT("/Game/Imported/ThirdPerson/Input/IMC_Default") },
    };

    int32 Failures = 0;
    for (const auto& Pair : Copies) {
        if (UEditorAssetLibrary::DoesAssetExist(Pair.Value)) {
            UE_LOG(LogBPRoundtripSeed, Display,
                   TEXT("Skipping (exists): %s"), *Pair.Value);
            continue;
        }
        UObject* Src = UEditorAssetLibrary::LoadAsset(Pair.Key);
        if (!Src) {
            UE_LOG(LogBPRoundtripSeed, Error,
                   TEXT("Failed to load source: %s"), *Pair.Key);
            ++Failures;
            continue;
        }
        UObject* Dst = UEditorAssetLibrary::DuplicateAsset(Pair.Key, Pair.Value);
        if (!Dst) {
            UE_LOG(LogBPRoundtripSeed, Error,
                   TEXT("Failed to duplicate %s -> %s"),
                   *Pair.Key, *Pair.Value);
            ++Failures;
            continue;
        }
        UEditorAssetLibrary::SaveAsset(Pair.Value, /*bOnlyIfIsDirty=*/false);
    }

    UE_LOG(LogBPRoundtripSeed, Display,
           TEXT("BPRoundtripSeed done. Failures: %d"), Failures);
    return Failures > 0 ? 2 : 0;
}
```

- [ ] **Step 4: Build the editor module to verify it compiles**

```bash
"D:/Projects/Unreal Engine 5/Engine/Build/BatchFiles/Build.bat" UE5_MCPEditor Win64 Development -project="D:/Projects/UE5_MCP/UE5_MCP.uproject" -NoUba -MaxParallelActions=4 -waitmutex
```

Expected: clean build (success exit 0). Set `BP_READER_SKIP_PREBUILD=1` if you only want to rebuild the editor module and not the MCP server.

- [ ] **Step 5: Commit**

```bash
git add Plugins/BlueprintReader/Source/BlueprintReaderEditor/Private/BPRoundtripSeedCommandlet.h Plugins/BlueprintReader/Source/BlueprintReaderEditor/Private/BPRoundtripSeedCommandlet.cpp
git commit -m "feat(plugin): BPRoundtripSeed commandlet — imports TP_ThirdPersonBP into /Game/Imported/ThirdPerson"
```

---

### Task 7: Run the seed, commit imported assets

**Files:**
- Create: `Content/Imported/ThirdPerson/*.uasset` (committed)
- Create: `Content/Recreated/.gitkeep`, `Content/Recreated/.gitignore`

- [ ] **Step 1: Run the seed commandlet**

```bash
"D:/Projects/Unreal Engine 5/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" "D:/Projects/UE5_MCP/UE5_MCP.uproject" -run=BPRoundtripSeed -nullrhi -nosplash -unattended -nopause
```

Expected: log lines `Copied /TPTemplate/Blueprints/BP_ThirdPersonCharacter -> /Game/Imported/ThirdPerson/BP_ThirdPersonCharacter.BP_ThirdPersonCharacter`, etc. Exit code 0.

- [ ] **Step 2: Verify the assets exist**

```bash
ls Content/Imported/ThirdPerson/ 2>&1
```

Expected: at least `BP_ThirdPersonCharacter.uasset`, `BP_ThirdPersonGameMode.uasset`, `BP_ThirdPersonPlayerController.uasset`, `ABP_Manny.uasset`, plus `Input/` subdir.

- [ ] **Step 3: Set up `Content/Recreated/` as gitignored**

```bash
mkdir -p Content/Recreated
echo "# Roundtrip clones (regenerable, gitignored)" > Content/Recreated/.gitignore
echo "*" >> Content/Recreated/.gitignore
echo "!.gitignore" >> Content/Recreated/.gitignore
echo "!.gitkeep" >> Content/Recreated/.gitignore
touch Content/Recreated/.gitkeep
```

- [ ] **Step 4: Commit the imported assets and the recreated-dir placeholder**

```bash
git add Content/Imported/ThirdPerson/ Content/Recreated/.gitkeep Content/Recreated/.gitignore
git commit -m "chore(content): import UE5 ThirdPerson template under /Game/Imported/ThirdPerson"
```

---

### Task 8: `docs/research/tpc-anatomy.md`

**Files:**
- Create: `docs/research/tpc-anatomy.md`

**Required topics:**
1. **Asset inventory** — every BP under `Content/Imported/ThirdPerson/`, with parent class, variable count, function/macro count, component count, event-graph node count.
2. **`BP_ThirdPersonCharacter` deep-dive** — full variables list with type, default, flags; full component tree (SCS); every function with input/output pin types; event-graph node-by-node walkthrough; every EnhancedInput action it binds.
3. **`ABP_Manny` deep-dive** — animation state machines (state-by-state), transition rules, blend spaces referenced.
4. **`BP_ThirdPersonGameMode` / `BP_ThirdPersonPlayerController`** — short — just parent class + any overrides.
5. **EnhancedInput assets** — IA_Move/Look/Jump trigger configs, IMC_Default mapping entries.
6. **Node-kind histogram** — per-BP and aggregate counts of each K2Node_* class. This is the test target: roundtrip success = same histogram on the clone.
7. **Pin-type histogram** — what pin types are used. This is the test target for typed-variable / typed-pin coverage.
8. **Known gotchas for the roundtrip** — anything you spot reading the BPs that bp-reader's read/write tools might not handle correctly yet.

**Sources to gather data from:**
- Use the bp-reader MCP tools (via daemon mode) — `read_blueprint`, `get_components`, `list_variables`, `get_function_graph` for each function, `get_event_graph` for the event graph.
- The introspection data this gathers also seeds the `BP_TPC_spec.json` fixture used in Task 20.

- [ ] **Step 1: Build & launch the bp-reader server**

Already built in Task 6. Verify:

```bash
ls Binaries/Win64/BlueprintReaderMcp.exe 2>&1
```

- [ ] **Step 2: Drive the tools to dump TPC + supporting BPs**

Easiest path: use the running `bp-reader` MCP tools via Claude Code itself (the calling agent). Otherwise: stream JSON-RPC frames against `BlueprintReaderMcp.exe` stdin (see `docs/tutorial/06-first-write.md` for the JSON-RPC envelope shape).

For each of the four BPs, capture: `read_blueprint`, `get_components`, `list_variables`, `get_event_graph`. For each function listed, capture `get_function_graph`.

- [ ] **Step 3: Write the doc, embedding the captured data as JSON code-blocks**

- [ ] **Step 4: Commit**

```bash
git add docs/research/tpc-anatomy.md
git commit -m "docs(research): UE5 ThirdPerson template anatomy (drives roundtrip tests)"
```

**End of Phase 1.** If the Ralph loop re-fires at this point, Phase-1 deliverables are complete; the next iteration starts at Task 9.

---

## Phase 2 — Code

### Task 9: `BPSpec` library + unit tests

**Files:**
- Create: `Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/roundtrip/BPSpec.h`
- Create: `Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/roundtrip/BPSpec.cpp`
- Create: `Plugins/BlueprintReader/Tests/BlueprintReaderMcpTests/Private/test_bpspec.cpp`

- [x] **Step 1: Write the header — schema + JSON glue**

```cpp
// BPSpec — a complete structural snapshot of a Blueprint, formatted as the
// pivot between ReadToSpec (drives read tools) and SpecToBP (drives write
// tools). Self-contained: serializes to JSON without referencing live UObject
// state.
//
// Wire format: snake_case JSON, same convention as BlueprintReaderTypes.h.
// Node IDs are stable content-hashes (NOT FGuid) so a freshly-rebuilt BP can
// be matched against the source without GUID equality.
#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "BlueprintReaderTypes.h"

namespace bpr::roundtrip {

struct SpecFunction {
    std::string name;
    std::vector<BPPin> inputs;
    std::vector<BPPin> outputs;
    std::vector<BPVariable> locals;
    std::vector<BPNode> nodes;
    std::vector<BPConnection> connections;
};

struct SpecGraph {
    std::string name;
    std::string type;       // "EventGraph", "Macro", "Function", "Construction"
    std::vector<BPNode> nodes;
    std::vector<BPConnection> connections;
};

struct SpecComponent {
    std::string name;
    std::string component_class;
    std::string parent_name;  // empty = root
    std::string socket;
    nlohmann::json properties = nlohmann::json::object();  // serialized overrides
};

struct BPSpec {
    std::string package_path;
    std::string parent_class;
    std::vector<std::string> interfaces;
    std::vector<BPVariable> variables;
    std::vector<SpecComponent> components;
    std::vector<SpecFunction> functions;
    std::vector<SpecGraph> macros;
    SpecGraph event_graph;
    bool incomplete = false;
    std::vector<std::string> errors;
};

// JSON glue.
nlohmann::json ToJson(const BPSpec& spec);
BPSpec FromJson(const nlohmann::json& j);

// Generate a content-hash node id (stable across rebuilds for nodes that
// have the same class + signature + position-rank). Returns hex digits.
std::string StableNodeId(const BPNode& node, std::size_t positionRank);

}    // namespace bpr::roundtrip
```

- [x] **Step 2: Write the implementation**

```cpp
#include "BPSpec.h"

#include <functional>
#include <sstream>
#include <iomanip>

namespace bpr::roundtrip {

namespace {
    // FNV-1a 64-bit hash for stability.
    std::uint64_t FnvHash(std::string_view sv) {
        std::uint64_t h = 14695981039346656037ull;
        for (char c : sv) {
            h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
            h *= 1099511628211ull;
        }
        return h;
    }

    std::string HexHash(std::uint64_t h) {
        std::ostringstream o;
        o << std::hex << std::setw(16) << std::setfill('0') << h;
        return o.str();
    }

    nlohmann::json PinTypeJson(const BPPinType& t) {
        nlohmann::json j;
        j["category"] = t.Category;
        if (t.SubCategory)        j["sub_category"]        = *t.SubCategory;
        if (t.SubCategoryObject)  j["sub_category_object"] = *t.SubCategoryObject;
        if (t.IsArray) j["is_array"] = true;
        if (t.IsSet)   j["is_set"]   = true;
        if (t.IsMap)   j["is_map"]   = true;
        return j;
    }
    BPPinType PinTypeFromJson(const nlohmann::json& j) {
        BPPinType t;
        t.Category = j.value("category", "");
        if (j.contains("sub_category"))        t.SubCategory       = j["sub_category"].get<std::string>();
        if (j.contains("sub_category_object")) t.SubCategoryObject = j["sub_category_object"].get<std::string>();
        t.IsArray = j.value("is_array", false);
        t.IsSet   = j.value("is_set", false);
        t.IsMap   = j.value("is_map", false);
        return t;
    }

    nlohmann::json PinJson(const BPPin& p) {
        nlohmann::json j;
        j["id"]         = p.Id;
        j["name"]       = p.Name;
        j["direction"]  = p.Direction;
        j["type"]       = PinTypeJson(p.Type);
        if (p.DefaultValue) j["default_value"] = *p.DefaultValue;
        return j;
    }
    BPPin PinFromJson(const nlohmann::json& j) {
        BPPin p;
        p.Id = j.value("id", "");
        p.Name = j.value("name", "");
        p.Direction = j.value("direction", "");
        p.Type = PinTypeFromJson(j.value("type", nlohmann::json::object()));
        if (j.contains("default_value")) p.DefaultValue = j["default_value"].get<std::string>();
        return p;
    }

    nlohmann::json NodeJson(const BPNode& n) {
        nlohmann::json j;
        j["id"]        = n.Id;
        j["class"]     = n.Class;
        j["title"]     = n.Title;
        j["position"]  = { {"x", n.Position.X}, {"y", n.Position.Y} };
        if (n.Comment) j["comment"] = *n.Comment;
        j["pins"]      = nlohmann::json::array();
        for (const auto& p : n.Pins) j["pins"].push_back(PinJson(p));
        j["meta"]      = n.Meta;
        return j;
    }
    BPNode NodeFromJson(const nlohmann::json& j) {
        BPNode n;
        n.Id = j.value("id", "");
        n.Class = j.value("class", "");
        n.Title = j.value("title", "");
        if (j.contains("position")) {
            n.Position.X = j["position"].value("x", 0);
            n.Position.Y = j["position"].value("y", 0);
        }
        if (j.contains("comment")) n.Comment = j["comment"].get<std::string>();
        if (j.contains("pins")) {
            for (const auto& p : j["pins"]) n.Pins.push_back(PinFromJson(p));
        }
        if (j.contains("meta")) n.Meta = j["meta"];
        return n;
    }

    nlohmann::json ConnJson(const BPConnection& c) {
        return { {"from_node", c.FromNode}, {"from_pin", c.FromPin},
                 {"to_node", c.ToNode},     {"to_pin", c.ToPin} };
    }
    BPConnection ConnFromJson(const nlohmann::json& j) {
        BPConnection c;
        c.FromNode = j.value("from_node", "");
        c.FromPin  = j.value("from_pin", "");
        c.ToNode   = j.value("to_node", "");
        c.ToPin    = j.value("to_pin", "");
        return c;
    }

    nlohmann::json VarJson(const BPVariable& v) {
        return { {"name", v.Name}, {"type", PinTypeJson(v.Type)},
                 {"default_value", v.DefaultValue.value_or("")},
                 {"category", v.Category.value_or("")} };
    }
    BPVariable VarFromJson(const nlohmann::json& j) {
        BPVariable v;
        v.Name = j.value("name", "");
        v.Type = PinTypeFromJson(j.value("type", nlohmann::json::object()));
        if (j.contains("default_value")) v.DefaultValue = j["default_value"].get<std::string>();
        if (j.contains("category"))      v.Category     = j["category"].get<std::string>();
        return v;
    }

    nlohmann::json GraphJson(const SpecGraph& g) {
        nlohmann::json j;
        j["name"] = g.name; j["type"] = g.type;
        j["nodes"] = nlohmann::json::array();
        for (const auto& n : g.nodes) j["nodes"].push_back(NodeJson(n));
        j["connections"] = nlohmann::json::array();
        for (const auto& c : g.connections) j["connections"].push_back(ConnJson(c));
        return j;
    }
    SpecGraph GraphFromJson(const nlohmann::json& j) {
        SpecGraph g;
        g.name = j.value("name", "");
        g.type = j.value("type", "");
        if (j.contains("nodes")) for (const auto& n : j["nodes"]) g.nodes.push_back(NodeFromJson(n));
        if (j.contains("connections")) for (const auto& c : j["connections"]) g.connections.push_back(ConnFromJson(c));
        return g;
    }

    nlohmann::json FnJson(const SpecFunction& f) {
        nlohmann::json j;
        j["name"] = f.name;
        j["inputs"]  = nlohmann::json::array();
        for (const auto& p : f.inputs)  j["inputs"].push_back(PinJson(p));
        j["outputs"] = nlohmann::json::array();
        for (const auto& p : f.outputs) j["outputs"].push_back(PinJson(p));
        j["locals"]  = nlohmann::json::array();
        for (const auto& v : f.locals)  j["locals"].push_back(VarJson(v));
        j["nodes"]   = nlohmann::json::array();
        for (const auto& n : f.nodes)   j["nodes"].push_back(NodeJson(n));
        j["connections"] = nlohmann::json::array();
        for (const auto& c : f.connections) j["connections"].push_back(ConnJson(c));
        return j;
    }
    SpecFunction FnFromJson(const nlohmann::json& j) {
        SpecFunction f;
        f.name = j.value("name", "");
        if (j.contains("inputs"))  for (const auto& p : j["inputs"])  f.inputs.push_back(PinFromJson(p));
        if (j.contains("outputs")) for (const auto& p : j["outputs"]) f.outputs.push_back(PinFromJson(p));
        if (j.contains("locals"))  for (const auto& v : j["locals"])  f.locals.push_back(VarFromJson(v));
        if (j.contains("nodes"))   for (const auto& n : j["nodes"])   f.nodes.push_back(NodeFromJson(n));
        if (j.contains("connections")) for (const auto& c : j["connections"]) f.connections.push_back(ConnFromJson(c));
        return f;
    }

    nlohmann::json CompJson(const SpecComponent& c) {
        return { {"name", c.name}, {"component_class", c.component_class},
                 {"parent_name", c.parent_name}, {"socket", c.socket},
                 {"properties", c.properties} };
    }
    SpecComponent CompFromJson(const nlohmann::json& j) {
        SpecComponent c;
        c.name = j.value("name", "");
        c.component_class = j.value("component_class", "");
        c.parent_name = j.value("parent_name", "");
        c.socket = j.value("socket", "");
        c.properties = j.value("properties", nlohmann::json::object());
        return c;
    }
}

nlohmann::json ToJson(const BPSpec& s) {
    nlohmann::json j;
    j["package_path"] = s.package_path;
    j["parent_class"] = s.parent_class;
    j["interfaces"]   = s.interfaces;
    j["variables"]    = nlohmann::json::array();
    for (const auto& v : s.variables)  j["variables"].push_back(VarJson(v));
    j["components"]   = nlohmann::json::array();
    for (const auto& c : s.components) j["components"].push_back(CompJson(c));
    j["functions"]    = nlohmann::json::array();
    for (const auto& f : s.functions)  j["functions"].push_back(FnJson(f));
    j["macros"]       = nlohmann::json::array();
    for (const auto& g : s.macros)     j["macros"].push_back(GraphJson(g));
    j["event_graph"]  = GraphJson(s.event_graph);
    if (s.incomplete) {
        j["incomplete"] = true;
        j["errors"]     = s.errors;
    }
    return j;
}

BPSpec FromJson(const nlohmann::json& j) {
    BPSpec s;
    s.package_path = j.value("package_path", "");
    s.parent_class = j.value("parent_class", "");
    if (j.contains("interfaces"))
        s.interfaces = j["interfaces"].get<std::vector<std::string>>();
    if (j.contains("variables"))
        for (const auto& v : j["variables"])  s.variables.push_back(VarFromJson(v));
    if (j.contains("components"))
        for (const auto& c : j["components"]) s.components.push_back(CompFromJson(c));
    if (j.contains("functions"))
        for (const auto& f : j["functions"])  s.functions.push_back(FnFromJson(f));
    if (j.contains("macros"))
        for (const auto& g : j["macros"])     s.macros.push_back(GraphFromJson(g));
    if (j.contains("event_graph"))
        s.event_graph = GraphFromJson(j["event_graph"]);
    s.incomplete = j.value("incomplete", false);
    if (j.contains("errors"))
        s.errors = j["errors"].get<std::vector<std::string>>();
    return s;
}

std::string StableNodeId(const BPNode& node, std::size_t positionRank) {
    std::string sig;
    sig.reserve(64);
    sig += node.Class;
    sig += '|';
    sig += node.Title;
    sig += '|';
    for (const auto& p : node.Pins) {
        sig += p.Name;  sig += ':';
        sig += p.Direction; sig += ':';
        sig += p.Type.Category;
        if (p.Type.SubCategoryObject) { sig += '@'; sig += *p.Type.SubCategoryObject; }
        sig += ';';
    }
    sig += '|';
    sig += std::to_string(positionRank);
    return HexHash(FnvHash(sig));
}

}    // namespace bpr::roundtrip
```

- [x] **Step 3: Write the unit test (TDD shape — failing first if you flip Steps 1+2 to stubs)**

```cpp
#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include "../../BlueprintReaderMcpCore/Private/roundtrip/BPSpec.h"

using namespace bpr::roundtrip;

TEST_CASE("BPSpec round-trips through JSON") {
    BPSpec s;
    s.package_path = "/Game/Foo/BP_Bar";
    s.parent_class = "/Script/Engine.Actor";
    s.interfaces   = { "/Game/Interfaces/BPI_Foo" };

    BPVariable v;
    v.Name = "Health";
    v.Type.Category = "float";
    v.DefaultValue = "100.0";
    s.variables.push_back(v);

    SpecComponent c;
    c.name = "Mesh";
    c.component_class = "/Script/Engine.StaticMeshComponent";
    c.parent_name = "Root";
    s.components.push_back(c);

    SpecFunction fn;
    fn.name = "DoStuff";
    fn.nodes.push_back(BPNode{ "n1", "K2Node_VariableSet", "Set Health" });
    s.functions.push_back(fn);

    s.event_graph.name = "EventGraph";
    s.event_graph.type = "EventGraph";
    s.event_graph.nodes.push_back(BPNode{ "e1", "K2Node_Event", "BeginPlay" });

    auto j = ToJson(s);
    BPSpec s2 = FromJson(j);

    CHECK(s2.package_path == s.package_path);
    CHECK(s2.parent_class == s.parent_class);
    REQUIRE(s2.variables.size() == 1);
    CHECK(s2.variables[0].Name == "Health");
    CHECK(s2.variables[0].Type.Category == "float");
    REQUIRE(s2.components.size() == 1);
    CHECK(s2.components[0].component_class == "/Script/Engine.StaticMeshComponent");
    REQUIRE(s2.functions.size() == 1);
    CHECK(s2.functions[0].name == "DoStuff");
    REQUIRE(s2.functions[0].nodes.size() == 1);
    CHECK(s2.functions[0].nodes[0].Class == "K2Node_VariableSet");
    REQUIRE(s2.event_graph.nodes.size() == 1);
}

TEST_CASE("StableNodeId is deterministic and content-sensitive") {
    BPNode a{ "guid-a", "K2Node_VariableGet", "Get Health" };
    a.Pins.push_back(BPPin{ "p1", "Health", "Output", BPPinType{ "float" } });
    BPNode b = a;
    b.Id = "guid-b";  // different GUID, same class+title+pins
    CHECK(StableNodeId(a, 0) == StableNodeId(b, 0));

    BPNode c = a;
    c.Title = "Get Mana";
    CHECK(StableNodeId(a, 0) != StableNodeId(c, 0));
}
```

- [ ] **Step 4: Build & run the doctest binary**

```bash
"D:/Projects/Unreal Engine 5/Engine/Build/BatchFiles/Build.bat" BlueprintReaderMcpTests Win64 Development -project="D:/Projects/UE5_MCP/UE5_MCP.uproject" -NoUba -MaxParallelActions=4 -waitmutex && Binaries/Win64/BlueprintReaderMcpTests.exe -tc="BPSpec*,StableNodeId*"
```

Expected: 2 cases pass.

- [x] **Step 5: Commit**

```bash
git add Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/roundtrip/BPSpec.h Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/roundtrip/BPSpec.cpp Plugins/BlueprintReader/Tests/BlueprintReaderMcpTests/Private/test_bpspec.cpp
git commit -m "feat(roundtrip): BPSpec — JSON snapshot bridging read/write tool paths"
```

---

### Task 10: `ReadToSpec` library

**Files:**
- Create: `Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/roundtrip/ReadToSpec.h`
- Create: `Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/roundtrip/ReadToSpec.cpp`

- [x] **Step 1: Write the header**

```cpp
// ReadToSpec — orchestrates IBlueprintReader read methods to assemble a
// complete BPSpec for one Blueprint. Pure: no I/O outside the reader.
#pragma once

#include <string_view>

#include "BPSpec.h"
#include "../backends/IBlueprintReader.h"

namespace bpr::roundtrip {

// Drives `reader` to gather every piece of metadata for `assetPath` and
// assembles it into a BPSpec. On any error from the reader, sets
// `spec.incomplete = true` and appends the error to `spec.errors`, then
// returns whatever partial spec was built. Never throws.
BPSpec ReadToSpec(backends::IBlueprintReader& reader, std::string_view assetPath);

}    // namespace bpr::roundtrip
```

- [x] **Step 2: Write a failing test first**

Add to `test_bpspec.cpp` (already exists from Task 9):

```cpp
#include "../../BlueprintReaderMcpCore/Private/roundtrip/ReadToSpec.h"
#include "../../BlueprintReaderMcpCore/Private/backends/MockBlueprintReader.h"

TEST_CASE("ReadToSpec assembles a spec from the mock backend") {
    bpr::backends::MockBlueprintReader mock;  // uses default fixtures dir
    auto spec = bpr::roundtrip::ReadToSpec(mock, "/Game/AI/BP_TestEnemy");
    CHECK(!spec.incomplete);
    CHECK(spec.parent_class == "/Script/Engine.Pawn");  // BP_TestEnemy extends Pawn
    CHECK(spec.variables.size() >= 5);                  // fixture seeds 5 vars
    CHECK(spec.functions.size() >= 2);                  // fixture seeds 2 functions
    CHECK_FALSE(spec.event_graph.nodes.empty());        // has an event graph
}
```

- [ ] **Step 3: Build & run — expect link error (ReadToSpec not yet implemented)**

```bash
"D:/Projects/Unreal Engine 5/Engine/Build/BatchFiles/Build.bat" BlueprintReaderMcpTests Win64 Development -project="D:/Projects/UE5_MCP/UE5_MCP.uproject" -NoUba -MaxParallelActions=4 -waitmutex
```

Expected: unresolved-external link error for `ReadToSpec`.

- [x] **Step 4: Implement `ReadToSpec.cpp`**

```cpp
#include "ReadToSpec.h"

namespace bpr::roundtrip {

namespace {
    template <class Fn>
    void Try(BPSpec& spec, std::string_view stage, Fn&& fn) {
        try { fn(); }
        catch (const std::exception& e) {
            spec.incomplete = true;
            spec.errors.emplace_back(std::string(stage) + ": " + e.what());
        }
    }
}

BPSpec ReadToSpec(backends::IBlueprintReader& reader, std::string_view assetPath) {
    BPSpec spec;
    spec.package_path = std::string(assetPath);

    BPMetadata meta;
    Try(spec, "ReadBlueprint", [&]{
        meta = reader.ReadBlueprint(assetPath);
        spec.parent_class = meta.ParentClass;
        spec.interfaces   = meta.Interfaces;
    });

    Try(spec, "ListVariables", [&]{
        spec.variables = reader.ListVariables(assetPath);
    });

    Try(spec, "GetComponents", [&]{
        auto comps = reader.GetComponents(assetPath);
        for (const auto& c : comps) {
            SpecComponent sc;
            sc.name = c.Name;
            sc.component_class = c.ClassPath;
            sc.parent_name = c.ParentName;
            sc.socket = c.Socket;
            // Properties: BPComponent doesn't currently surface CDO
            // overrides in the wire shape — record an empty object; the
            // SpecToBP layer is responsible for re-applying anything that
            // the parent-class CDO doesn't already set.
            sc.properties = nlohmann::json::object();
            spec.components.push_back(std::move(sc));
        }
    });

    // Functions: one GetFunction call gives us the signature + locals; one
    // GetGraph call gives us the body nodes + connections.
    for (const auto& fnSummary : meta.Functions) {
        SpecFunction fn;
        fn.name = fnSummary.Name;
        Try(spec, "GetFunction(" + fn.name + ")", [&]{
            auto fnInfo = reader.GetFunction(assetPath, fn.name);
            fn.inputs  = fnInfo.Inputs;
            fn.outputs = fnInfo.Outputs;
            fn.locals  = fnInfo.Locals;
        });
        Try(spec, "GetGraph(" + fn.name + ")", [&]{
            auto graph = reader.GetGraph(assetPath, fn.name);
            fn.nodes       = graph.Nodes;
            fn.connections = graph.Connections;
        });
        spec.functions.push_back(std::move(fn));
    }

    // Macros: graph-only.
    for (const auto& mSummary : meta.Macros) {
        SpecGraph g;
        g.name = mSummary.Name;
        g.type = "Macro";
        Try(spec, "GetGraph(" + g.name + ")", [&]{
            auto graph = reader.GetGraph(assetPath, g.name);
            g.nodes       = graph.Nodes;
            g.connections = graph.Connections;
        });
        spec.macros.push_back(std::move(g));
    }

    // Event graph (canonical name: "EventGraph"; may also be
    // "UserConstructionScript" — both are surfaced as graph entries).
    Try(spec, "GetGraph(EventGraph)", [&]{
        auto g = reader.GetGraph(assetPath, "EventGraph");
        spec.event_graph.name = g.Name;
        spec.event_graph.type = g.Type;
        spec.event_graph.nodes = g.Nodes;
        spec.event_graph.connections = g.Connections;
    });

    // Reassign stable node IDs across all graphs so the diff tool can
    // match nodes between source and clone.
    auto restamp = [](std::vector<BPNode>& nodes,
                      std::vector<BPConnection>& conns) {
        std::map<std::string, std::string> idMap;
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            auto stable = StableNodeId(nodes[i], i);
            idMap[nodes[i].Id] = stable;
            nodes[i].Id = stable;
        }
        for (auto& c : conns) {
            auto fIt = idMap.find(c.FromNode);
            if (fIt != idMap.end()) c.FromNode = fIt->second;
            auto tIt = idMap.find(c.ToNode);
            if (tIt != idMap.end()) c.ToNode = tIt->second;
        }
    };
    for (auto& f : spec.functions)  restamp(f.nodes, f.connections);
    for (auto& m : spec.macros)     restamp(m.nodes, m.connections);
    restamp(spec.event_graph.nodes, spec.event_graph.connections);

    return spec;
}

}    // namespace bpr::roundtrip
```

- [ ] **Step 5: Build & run, expect tests pass**

```bash
"D:/Projects/Unreal Engine 5/Engine/Build/BatchFiles/Build.bat" BlueprintReaderMcpTests Win64 Development -project="D:/Projects/UE5_MCP/UE5_MCP.uproject" -NoUba -MaxParallelActions=4 -waitmutex && Binaries/Win64/BlueprintReaderMcpTests.exe -tc="ReadToSpec*"
```

Expected: pass. If the BPMetadata field names (`ParentClass`, `Interfaces`, `Functions`, `Macros`) don't match what `BlueprintReaderTypes.h` defines, adjust the cpp accordingly — the compiler tells you which.

- [x] **Step 6: Commit**

```bash
git add Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/roundtrip/ReadToSpec.h Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/roundtrip/ReadToSpec.cpp Plugins/BlueprintReader/Tests/BlueprintReaderMcpTests/Private/test_bpspec.cpp
git commit -m "feat(roundtrip): ReadToSpec — assembles BPSpec from reader read methods"
```

---

### Task 11: `SpecToBP` library

**Files:**
- Create: `Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/roundtrip/SpecToBP.h`
- Create: `Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/roundtrip/SpecToBP.cpp`

- [x] **Step 1: Write the header**

```cpp
// SpecToBP — drives IBlueprintReader write methods to rebuild a BP from a
// BPSpec. Halts on first write failure and surfaces which op failed.
#pragma once

#include <string>
#include <string_view>

#include "BPSpec.h"
#include "../backends/IBlueprintReader.h"

namespace bpr::roundtrip {

struct SpecToBPResult {
    bool ok = false;
    std::string output_package_path;
    std::string failing_stage;   // empty on success
    std::string failing_op;
    std::string error_message;
};

// Creates `outputPackagePath` if missing, then issues writes through
// `reader` to materialize `spec`. Halts on first failure.
SpecToBPResult SpecToBP(backends::IBlueprintReader& reader,
                        const BPSpec& spec,
                        std::string_view outputPackagePath);

}    // namespace bpr::roundtrip
```

- [x] **Step 2: Write the implementation**

```cpp
#include "SpecToBP.h"

#include <map>

namespace bpr::roundtrip {

namespace {
    bool DoStage(SpecToBPResult& res, std::string_view stage,
                 std::string_view op, std::function<void()> fn) {
        try { fn(); return true; }
        catch (const std::exception& e) {
            res.failing_stage   = std::string(stage);
            res.failing_op      = std::string(op);
            res.error_message   = e.what();
            return false;
        }
    }
}

SpecToBPResult SpecToBP(backends::IBlueprintReader& reader,
                        const BPSpec& spec,
                        std::string_view outputPackagePath) {
    SpecToBPResult res;
    res.output_package_path = std::string(outputPackagePath);

    // 1. CreateBlueprint
    if (!DoStage(res, "create", "CreateBlueprint", [&]{
        reader.CreateBlueprint(outputPackagePath, spec.parent_class);
    })) return res;

    // 2. Variables
    for (const auto& v : spec.variables) {
        if (!DoStage(res, "variable", "AddVariable:" + v.Name, [&]{
            reader.AddVariable(outputPackagePath, v.Name, v.Type,
                               v.DefaultValue.value_or(""),
                               v.Category.value_or(""),
                               /*replicated*/ false, /*editable*/ true);
        })) return res;
    }

    // 3. Components (root-first; we sort by parent-depth using parent_name
    //    pointers).
    {
        std::vector<SpecComponent> ordered;
        std::map<std::string, bool> placed;
        auto canPlace = [&](const SpecComponent& c) {
            return c.parent_name.empty() || placed.count(c.parent_name);
        };
        // Two-pass topological sort
        auto remaining = spec.components;
        while (!remaining.empty()) {
            bool madeProgress = false;
            for (auto it = remaining.begin(); it != remaining.end();) {
                if (canPlace(*it)) {
                    placed[it->name] = true;
                    ordered.push_back(std::move(*it));
                    it = remaining.erase(it);
                    madeProgress = true;
                } else {
                    ++it;
                }
            }
            if (!madeProgress) {
                // cycle or dangling parent: append rest unsorted
                for (auto& c : remaining) ordered.push_back(std::move(c));
                break;
            }
        }
        for (const auto& c : ordered) {
            if (!DoStage(res, "component", "AddComponent:" + c.name, [&]{
                reader.AddComponent(outputPackagePath, c.name,
                                    c.component_class, c.parent_name, c.socket);
            })) return res;
            // Apply properties one by one
            for (auto it = c.properties.begin(); it != c.properties.end(); ++it) {
                if (!DoStage(res, "component_property",
                             "SetComponentProperty:" + c.name + "." + it.key(),
                             [&]{
                    reader.SetComponentProperty(outputPackagePath, c.name,
                                                it.key(),
                                                it.value().is_string()
                                                    ? it.value().get<std::string>()
                                                    : it.value().dump());
                })) return res;
            }
        }
    }

    // 4. Function skeletons (so callers' nodes can find them)
    for (const auto& f : spec.functions) {
        if (!DoStage(res, "function_skeleton", "AddFunction:" + f.name, [&]{
            reader.AddFunction(outputPackagePath, f.name);
        })) return res;
        for (const auto& in : f.inputs) {
            if (!DoStage(res, "function_input",
                         "AddFunctionInput:" + f.name + "." + in.Name, [&]{
                reader.AddFunctionInput(outputPackagePath, f.name, in.Name, in.Type);
            })) return res;
        }
        for (const auto& out : f.outputs) {
            if (!DoStage(res, "function_output",
                         "AddFunctionOutput:" + f.name + "." + out.Name, [&]{
                reader.AddFunctionOutput(outputPackagePath, f.name, out.Name, out.Type);
            })) return res;
        }
    }

    // 5. Nodes per graph (event graph + each function body + macros)
    auto materializeGraph = [&](const std::string& graphName,
                                const std::vector<BPNode>& nodes,
                                const std::vector<BPConnection>& conns) -> bool {
        // Map source stable IDs to new in-engine GUIDs returned by AddNode.
        std::map<std::string, std::string> idMap;
        for (const auto& n : nodes) {
            // Skip nodes that are auto-spawned by skeletons (FunctionEntry,
            // FunctionResult, Event_BeginPlay, etc.) — they'd collide.
            const bool isAutoSpawn =
                n.Class == "K2Node_FunctionEntry" ||
                n.Class == "K2Node_FunctionResult";
            if (isAutoSpawn) {
                idMap[n.Id] = n.Id;  // leave as-is; we'll resolve in connections
                continue;
            }

            // Translate the node class into AddNode's "kind" enum that the
            // backend understands.
            std::string kind;
            std::map<std::string, std::string, std::less<>> extras;
            if (n.Class == "K2Node_IfThenElse") { kind = "Branch"; }
            else if (n.Class == "K2Node_ExecutionSequence") { kind = "Sequence"; }
            else if (n.Class == "K2Node_VariableGet") {
                kind = "VariableGet";
                if (n.Meta.contains("variable")) extras["Variable"] = n.Meta["variable"].get<std::string>();
            }
            else if (n.Class == "K2Node_VariableSet") {
                kind = "VariableSet";
                if (n.Meta.contains("variable")) extras["Variable"] = n.Meta["variable"].get<std::string>();
            }
            else if (n.Class == "K2Node_CallFunction") {
                kind = "CallFunction";
                if (n.Meta.contains("function"))       extras["Function"]       = n.Meta["function"].get<std::string>();
                if (n.Meta.contains("function_owner")) extras["FunctionOwner"]  = n.Meta["function_owner"].get<std::string>();
                if (n.Meta.contains("target_class"))   extras["TargetClass"]    = n.Meta["target_class"].get<std::string>();
            }
            else if (n.Class == "K2Node_CustomEvent") {
                kind = "CustomEvent";
                if (n.Meta.contains("event_name")) extras["EventName"] = n.Meta["event_name"].get<std::string>();
            }
            else {
                std::string opStage = "node_unsupported";
                std::string opName  = "AddNode:" + n.Class;
                res.failing_stage = opStage;
                res.failing_op    = opName;
                res.error_message = "SpecToBP: node class " + n.Class +
                                    " not in AddNode dispatch; extend SpecToBP";
                return false;
            }

            std::string newId;
            if (!DoStage(res, "node", "AddNode:" + n.Class, [&]{
                newId = reader.AddNode(outputPackagePath, graphName, kind,
                                       n.Position.X, n.Position.Y, extras);
            })) return false;
            idMap[n.Id] = newId;
        }

        // Connections
        for (const auto& c : conns) {
            auto fIt = idMap.find(c.FromNode);
            auto tIt = idMap.find(c.ToNode);
            if (fIt == idMap.end() || tIt == idMap.end()) continue;
            if (!DoStage(res, "connection", "WirePins", [&]{
                reader.WirePins(outputPackagePath, graphName,
                                fIt->second, c.FromPin,
                                tIt->second, c.ToPin);
            })) return false;
        }
        return true;
    };

    if (!materializeGraph(spec.event_graph.name.empty() ? "EventGraph"
                                                       : spec.event_graph.name,
                          spec.event_graph.nodes,
                          spec.event_graph.connections)) return res;
    for (const auto& f : spec.functions) {
        if (!materializeGraph(f.name, f.nodes, f.connections)) return res;
    }
    for (const auto& m : spec.macros) {
        if (!materializeGraph(m.name, m.nodes, m.connections)) return res;
    }

    res.ok = true;
    return res;
}

}    // namespace bpr::roundtrip
```

- [x] **Step 3: Write unit tests against the mock backend**

Append to `test_bpspec.cpp`:

```cpp
#include "../../BlueprintReaderMcpCore/Private/roundtrip/SpecToBP.h"

TEST_CASE("SpecToBP returns mock-backend-not-supported on the first write") {
    bpr::backends::MockBlueprintReader mock;
    BPSpec spec;
    spec.package_path = "/Game/X";
    spec.parent_class = "/Script/Engine.Actor";

    auto res = bpr::roundtrip::SpecToBP(mock, spec, "/Game/Recreated/X");
    CHECK(!res.ok);  // mock is read-only
    CHECK(res.failing_op == "CreateBlueprint");
    CHECK(res.failing_stage == "create");
}
```

- [ ] **Step 4: Build & run**

```bash
"D:/Projects/Unreal Engine 5/Engine/Build/BatchFiles/Build.bat" BlueprintReaderMcpTests Win64 Development -project="D:/Projects/UE5_MCP/UE5_MCP.uproject" -NoUba -MaxParallelActions=4 -waitmutex && Binaries/Win64/BlueprintReaderMcpTests.exe -tc="SpecToBP*"
```

Expected: pass.

- [x] **Step 5: Commit**

```bash
git add Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/roundtrip/SpecToBP.h Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/roundtrip/SpecToBP.cpp Plugins/BlueprintReader/Tests/BlueprintReaderMcpTests/Private/test_bpspec.cpp
git commit -m "feat(roundtrip): SpecToBP — rebuilds a BP from BPSpec via write tools"
```

---

### Task 12: `BlueprintStructuralDiff` helpers (plugin-side)

**Files:**
- Create: `Plugins/BlueprintReader/Source/BlueprintReaderEditor/Public/BlueprintStructuralDiff.h`
- Create: `Plugins/BlueprintReader/Source/BlueprintReaderEditor/Private/BlueprintStructuralDiff.cpp`

- [ ] **Step 1: Write the header**

```cpp
// BlueprintStructuralDiff — position-independent comparison of two
// UBlueprint*s. Lives in the plugin because it needs full UBlueprint /
// USCS_Node / UEdGraphNode reflection. Output is a flat list of
// differences with a path string for human readability.
#pragma once

#include "CoreMinimal.h"

#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "Dom/JsonObject.h"

class UBlueprint;

namespace BlueprintStructuralDiff {

struct FCompareOptions {
    bool bIgnoreNodePositions = true;
    bool bIgnoreCommentNodes  = false;
    TArray<TPair<FString, FString>> AllowedNameSubstitutions;  // (from, to)
};

struct FDifference {
    FString Path;   // dotted path, e.g. "variables.Health.type"
    FString Kind;   // "missing", "extra", "value_mismatch", "type_mismatch"
    FString ValueA;
    FString ValueB;
};

struct FResult {
    bool bEqual = false;
    TArray<FDifference> Differences;

    TSharedPtr<FJsonObject> ToJson() const;
};

FResult Compare(UBlueprint* A, UBlueprint* B, const FCompareOptions& Options);

}    // namespace BlueprintStructuralDiff
```

- [ ] **Step 2: Write the implementation (covers variables, components, functions, event graph)**

```cpp
#include "BlueprintStructuralDiff.h"

#include "Engine/Blueprint.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Kismet2/BlueprintEditorUtils.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace BlueprintStructuralDiff {

namespace {
    void Add(FResult& R, FString Path, FString Kind,
             FString ValueA = {}, FString ValueB = {}) {
        R.Differences.Add({ MoveTemp(Path), MoveTemp(Kind),
                            MoveTemp(ValueA), MoveTemp(ValueB) });
    }

    FString PinTypeStr(const FEdGraphPinType& T) {
        return FString::Printf(TEXT("%s/%s/%s%s%s%s"),
            *T.PinCategory.ToString(),
            *T.PinSubCategory.ToString(),
            T.PinSubCategoryObject.IsValid() ? *T.PinSubCategoryObject->GetPathName() : TEXT(""),
            T.IsArray() ? TEXT("[]") : TEXT(""),
            T.IsSet()   ? TEXT("(set)") : TEXT(""),
            T.IsMap()   ? TEXT("(map)") : TEXT(""));
    }

    FString NodeSignature(UEdGraphNode* N) {
        FString Sig = N->GetClass()->GetName() + TEXT("|") + N->GetNodeTitle(ENodeTitleType::ListView).ToString();
        for (UEdGraphPin* P : N->Pins) {
            Sig += FString::Printf(TEXT("|%s:%s:%s"),
                *P->PinName.ToString(),
                P->Direction == EGPD_Input ? TEXT("In") : TEXT("Out"),
                *PinTypeStr(P->PinType));
        }
        return Sig;
    }

    void CompareVariables(UBlueprint* A, UBlueprint* B, FResult& R) {
        TMap<FName, const FBPVariableDescription*> MapA, MapB;
        for (const auto& V : A->NewVariables) MapA.Add(V.VarName, &V);
        for (const auto& V : B->NewVariables) MapB.Add(V.VarName, &V);

        for (const auto& Pair : MapA) {
            const FBPVariableDescription** Found = MapB.Find(Pair.Key);
            if (!Found) { Add(R, FString::Printf(TEXT("variables.%s"), *Pair.Key.ToString()), TEXT("missing"), TEXT("present"), TEXT("absent")); continue; }
            const auto* VA = Pair.Value;
            const auto* VB = *Found;
            FString TA = PinTypeStr(VA->VarType);
            FString TB = PinTypeStr(VB->VarType);
            if (TA != TB) Add(R, FString::Printf(TEXT("variables.%s.type"), *Pair.Key.ToString()), TEXT("type_mismatch"), TA, TB);
            if (VA->DefaultValue != VB->DefaultValue)
                Add(R, FString::Printf(TEXT("variables.%s.default"), *Pair.Key.ToString()), TEXT("value_mismatch"), VA->DefaultValue, VB->DefaultValue);
        }
        for (const auto& Pair : MapB) {
            if (!MapA.Contains(Pair.Key)) Add(R, FString::Printf(TEXT("variables.%s"), *Pair.Key.ToString()), TEXT("extra"), TEXT("absent"), TEXT("present"));
        }
    }

    void CompareComponents(UBlueprint* A, UBlueprint* B, FResult& R) {
        if (!A->SimpleConstructionScript || !B->SimpleConstructionScript) return;
        TMap<FName, USCS_Node*> NodesA, NodesB;
        for (USCS_Node* N : A->SimpleConstructionScript->GetAllNodes()) NodesA.Add(N->GetVariableName(), N);
        for (USCS_Node* N : B->SimpleConstructionScript->GetAllNodes()) NodesB.Add(N->GetVariableName(), N);

        for (const auto& Pair : NodesA) {
            USCS_Node** Found = NodesB.Find(Pair.Key);
            if (!Found) { Add(R, FString::Printf(TEXT("components.%s"), *Pair.Key.ToString()), TEXT("missing")); continue; }
            USCS_Node* NA = Pair.Value;
            USCS_Node* NB = *Found;
            FString CA = NA->ComponentClass ? NA->ComponentClass->GetPathName() : TEXT("");
            FString CB = NB->ComponentClass ? NB->ComponentClass->GetPathName() : TEXT("");
            if (CA != CB) Add(R, FString::Printf(TEXT("components.%s.class"), *Pair.Key.ToString()), TEXT("type_mismatch"), CA, CB);
        }
        for (const auto& Pair : NodesB) {
            if (!NodesA.Contains(Pair.Key)) Add(R, FString::Printf(TEXT("components.%s"), *Pair.Key.ToString()), TEXT("extra"));
        }
    }

    void CompareGraph(const FString& GraphPath, UEdGraph* GA, UEdGraph* GB,
                      const FCompareOptions& Opt, FResult& R) {
        if (!GA && !GB) return;
        if (!GA) { Add(R, GraphPath, TEXT("missing")); return; }
        if (!GB) { Add(R, GraphPath, TEXT("extra"));   return; }

        TMap<FString, int32> SigCountA, SigCountB;
        for (UEdGraphNode* N : GA->Nodes) SigCountA.FindOrAdd(NodeSignature(N))++;
        for (UEdGraphNode* N : GB->Nodes) SigCountB.FindOrAdd(NodeSignature(N))++;

        for (const auto& Pair : SigCountA) {
            const int32* B = SigCountB.Find(Pair.Key);
            const int32 BCount = B ? *B : 0;
            if (BCount != Pair.Value) {
                Add(R, FString::Printf(TEXT("%s.node_count[%s]"), *GraphPath, *Pair.Key),
                    TEXT("value_mismatch"),
                    FString::FromInt(Pair.Value), FString::FromInt(BCount));
            }
        }
        for (const auto& Pair : SigCountB) {
            if (!SigCountA.Contains(Pair.Key)) {
                Add(R, FString::Printf(TEXT("%s.node_count[%s]"), *GraphPath, *Pair.Key),
                    TEXT("value_mismatch"), TEXT("0"), FString::FromInt(Pair.Value));
            }
        }

        // Connection-count comparison (cheap, catches wiring regressions).
        int32 LinksA = 0, LinksB = 0;
        for (UEdGraphNode* N : GA->Nodes) for (UEdGraphPin* P : N->Pins) LinksA += P->LinkedTo.Num();
        for (UEdGraphNode* N : GB->Nodes) for (UEdGraphPin* P : N->Pins) LinksB += P->LinkedTo.Num();
        // Each link counted twice (both ends); both sides see same factor.
        if (LinksA != LinksB) {
            Add(R, FString::Printf(TEXT("%s.link_count"), *GraphPath),
                TEXT("value_mismatch"),
                FString::FromInt(LinksA / 2), FString::FromInt(LinksB / 2));
        }
    }

    void CompareGraphs(UBlueprint* A, UBlueprint* B, const FCompareOptions& Opt, FResult& R) {
        TArray<UEdGraph*> GraphsA = A->FunctionGraphs;
        TArray<UEdGraph*> GraphsB = B->FunctionGraphs;
        GraphsA.Append(A->MacroGraphs);
        GraphsB.Append(B->MacroGraphs);
        GraphsA.Append(A->UbergraphPages);
        GraphsB.Append(B->UbergraphPages);

        TMap<FName, UEdGraph*> MapA, MapB;
        for (UEdGraph* G : GraphsA) if (G) MapA.Add(G->GetFName(), G);
        for (UEdGraph* G : GraphsB) if (G) MapB.Add(G->GetFName(), G);

        for (const auto& Pair : MapA) {
            UEdGraph** Found = MapB.Find(Pair.Key);
            FString Path = FString::Printf(TEXT("graphs.%s"), *Pair.Key.ToString());
            CompareGraph(Path, Pair.Value, Found ? *Found : nullptr, Opt, R);
        }
        for (const auto& Pair : MapB) {
            if (!MapA.Contains(Pair.Key)) {
                Add(R, FString::Printf(TEXT("graphs.%s"), *Pair.Key.ToString()), TEXT("extra"));
            }
        }
    }
}

FResult Compare(UBlueprint* A, UBlueprint* B, const FCompareOptions& Options) {
    FResult R;
    if (!A || !B) { Add(R, TEXT("root"), TEXT("missing")); return R; }

    // Parent class
    if (A->ParentClass != B->ParentClass) {
        Add(R, TEXT("parent_class"), TEXT("value_mismatch"),
            A->ParentClass ? A->ParentClass->GetPathName() : TEXT(""),
            B->ParentClass ? B->ParentClass->GetPathName() : TEXT(""));
    }

    CompareVariables(A, B, R);
    CompareComponents(A, B, R);
    CompareGraphs(A, B, Options, R);

    R.bEqual = R.Differences.IsEmpty();
    return R;
}

TSharedPtr<FJsonObject> FResult::ToJson() const {
    auto Obj = MakeShared<FJsonObject>();
    Obj->SetBoolField(TEXT("ok"), bEqual);
    TArray<TSharedPtr<FJsonValue>> Arr;
    for (const auto& D : Differences) {
        auto O = MakeShared<FJsonObject>();
        O->SetStringField(TEXT("path"),  D.Path);
        O->SetStringField(TEXT("kind"),  D.Kind);
        O->SetStringField(TEXT("a"),     D.ValueA);
        O->SetStringField(TEXT("b"),     D.ValueB);
        Arr.Add(MakeShared<FJsonValueObject>(O));
    }
    Obj->SetArrayField(TEXT("differences"), Arr);
    return Obj;
}

}    // namespace BlueprintStructuralDiff
```

- [ ] **Step 3: Build the editor module to verify it compiles**

```bash
"D:/Projects/Unreal Engine 5/Engine/Build/BatchFiles/Build.bat" UE5_MCPEditor Win64 Development -project="D:/Projects/UE5_MCP/UE5_MCP.uproject" -NoUba -MaxParallelActions=4 -waitmutex
```

- [ ] **Step 4: Commit**

```bash
git add Plugins/BlueprintReader/Source/BlueprintReaderEditor/Public/BlueprintStructuralDiff.h Plugins/BlueprintReader/Source/BlueprintReaderEditor/Private/BlueprintStructuralDiff.cpp
git commit -m "feat(plugin): BlueprintStructuralDiff — position-independent UBlueprint comparison"
```

---

### Task 13: `EOp::StructuralDiff` commandlet wiring

**Files:**
- Modify: `Plugins/BlueprintReader/Source/BlueprintReaderEditor/Private/BlueprintReaderCommandlet.cpp`

- [ ] **Step 1: Add the enum value**

Find the `EOp` enum in `BlueprintReaderCommandlet.cpp`. Add `StructuralDiff,` to the list (typically before any sentinel `Count`).

- [ ] **Step 2: Add the `ParseOp` entry**

In the parser function around line 281 (look for the chain of `if (OpStr.Equals(TEXT("List")...`):

```cpp
if (OpStr.Equals(TEXT("StructuralDiff"), ESearchCase::IgnoreCase)) { OutOp = EOp::StructuralDiff; return true; }
```

- [ ] **Step 3: Add the dispatch arm**

In `RunOneOp` (search for `case EOp::List:`), add:

```cpp
case EOp::StructuralDiff:
    return RunStructuralDiffOp(Params, OutputPath, bPretty);
```

- [ ] **Step 4: Add the impl**

Near the other `RunFooOp` impls in the same file:

```cpp
static int32 RunStructuralDiffOp(const FString& Params, const FString& OutputPath, bool bPretty) {
    FString APath, BPath;
    FParse::Value(*Params, TEXT("A="), APath);
    FParse::Value(*Params, TEXT("B="), BPath);
    if (APath.IsEmpty() || BPath.IsEmpty()) {
        EmitErr(OutputPath, TEXT("missing_arg"), TEXT("requires -A= and -B="), bPretty);
        return 2;
    }

    UBlueprint* A = LoadObject<UBlueprint>(nullptr, *APath);
    UBlueprint* B = LoadObject<UBlueprint>(nullptr, *BPath);
    if (!A) { EmitErr(OutputPath, TEXT("asset_not_found"), APath, bPretty); return 5; }
    if (!B) { EmitErr(OutputPath, TEXT("asset_not_found"), BPath, bPretty); return 5; }

    BlueprintStructuralDiff::FCompareOptions Opt;
    Opt.bIgnoreNodePositions = true;
    auto Result = BlueprintStructuralDiff::Compare(A, B, Opt);
    auto Obj    = Result.ToJson();
    EmitOk(OutputPath, Obj, bPretty);
    return 0;
}
```

(`EmitOk` / `EmitErr` are existing helpers in the same file — find them by grep if signatures differ.)

- [ ] **Step 5: Add the include at the top of the file**

```cpp
#include "BlueprintStructuralDiff.h"
```

- [ ] **Step 6: Build & verify**

```bash
"D:/Projects/Unreal Engine 5/Engine/Build/BatchFiles/Build.bat" UE5_MCPEditor Win64 Development -project="D:/Projects/UE5_MCP/UE5_MCP.uproject" -NoUba -MaxParallelActions=4 -waitmutex
```

- [ ] **Step 7: Smoke-test from the command line**

```bash
"D:/Projects/Unreal Engine 5/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" "D:/Projects/UE5_MCP/UE5_MCP.uproject" -run=BPR -Op=StructuralDiff -A=/Game/AI/BP_TestEnemy -B=/Game/AI/BP_TestEnemy -OutputPath=- -Pretty=1
```

Expected: `{"ok": true, "differences": []}` on stdout.

- [ ] **Step 8: Commit**

```bash
git add Plugins/BlueprintReader/Source/BlueprintReaderEditor/Private/BlueprintReaderCommandlet.cpp
git commit -m "feat(plugin): EOp::StructuralDiff — commandlet op for BP structural comparison"
```

---

### Task 14: `IBlueprintReader::StructuralDiff` + backend impls

**Files:**
- Modify: `Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/backends/IBlueprintReader.h`
- Modify: `MockBlueprintReader.{h,cpp}`, `CommandletBlueprintReader.{h,cpp}`, `SocketBlueprintReader.{h,cpp}`, `CachingBlueprintReader.{h,cpp}`, `ReadOnlyBlueprintReader.{h,cpp}`, `AutoBlueprintReader.{h,cpp}`

- [ ] **Step 1: Add the pure-virtual method + result struct to `IBlueprintReader.h`**

Place near the other diff/compare-shaped methods (anywhere in the class body works; put it near `Read` since it's a read-style op).

```cpp
// Compare two Blueprints structurally — returns the diff JSON shape
// produced by BlueprintStructuralDiff::FResult::ToJson().
struct StructuralDiffOptions {
    bool ignoreNodePositions = true;
    bool ignoreCommentNodes  = false;
};
virtual nlohmann::json StructuralDiff(std::string_view a, std::string_view b,
                                      const StructuralDiffOptions& opts = {}) {
    (void)a; (void)b; (void)opts;
    throw BlueprintReaderError("StructuralDiff not supported by this backend");
}
```

- [ ] **Step 2: Implement on `CommandletBlueprintReader` (serializes args + runs the op)**

In `CommandletBlueprintReader.h`, declare:

```cpp
nlohmann::json StructuralDiff(std::string_view a, std::string_view b,
                              const StructuralDiffOptions& opts) override;
```

In `CommandletBlueprintReader.cpp`, implement following the pattern of other ops:

```cpp
nlohmann::json CommandletBlueprintReader::StructuralDiff(
    std::string_view a, std::string_view b, const StructuralDiffOptions& opts) {
    std::vector<std::string> args = {
        "-Op=StructuralDiff",
        std::string("-A=") + std::string(a),
        std::string("-B=") + std::string(b),
    };
    if (!opts.ignoreNodePositions) args.push_back("-IgnoreNodePositions=0");
    if (opts.ignoreCommentNodes)   args.push_back("-IgnoreCommentNodes=1");
    auto result = RunOp(args);
    return result;
}
```

(`RunOp` is the existing helper; consult `Read` or `GetGraph` for the exact pattern.)

- [ ] **Step 3: Implement on `SocketBlueprintReader` similarly** (sends a frame, awaits the response)

- [ ] **Step 4: Implement on `MockBlueprintReader` to throw not-supported with a friendly message**

```cpp
nlohmann::json MockBlueprintReader::StructuralDiff(
    std::string_view, std::string_view, const StructuralDiffOptions&) {
    throw BlueprintReaderError(
        "StructuralDiff requires the live or commandlet backend "
        "(needs UBlueprint reflection that mock fixtures don't provide)");
}
```

- [ ] **Step 5: Forward through `CachingBlueprintReader` (no caching for diffs)**

```cpp
nlohmann::json CachingBlueprintReader::StructuralDiff(
    std::string_view a, std::string_view b, const StructuralDiffOptions& opts) {
    return inner_->StructuralDiff(a, b, opts);
}
```

- [ ] **Step 6: Forward through `ReadOnlyBlueprintReader` (diff is a read op)**

```cpp
nlohmann::json ReadOnlyBlueprintReader::StructuralDiff(
    std::string_view a, std::string_view b, const StructuralDiffOptions& opts) {
    return inner_->StructuralDiff(a, b, opts);
}
```

- [ ] **Step 7: Forward through `AutoBlueprintReader`**

The auto backend probes per-call; pattern matches the other forwarders. Add:

```cpp
nlohmann::json AutoBlueprintReader::StructuralDiff(
    std::string_view a, std::string_view b, const StructuralDiffOptions& opts) {
    return Probe().StructuralDiff(a, b, opts);
}
```

- [ ] **Step 8: Build & run all tests to confirm nothing else broke**

```bash
"D:/Projects/Unreal Engine 5/Engine/Build/BatchFiles/Build.bat" BlueprintReaderMcpTests Win64 Development -project="D:/Projects/UE5_MCP/UE5_MCP.uproject" -NoUba -MaxParallelActions=4 -waitmutex && Binaries/Win64/BlueprintReaderMcpTests.exe
```

Expected: all 441 cases still pass (tool count assertion will fail in Task 15 — that's expected at that point).

- [ ] **Step 9: Commit**

```bash
git add Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/backends/IBlueprintReader.h Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/backends/Mock*.{h,cpp} Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/backends/Commandlet*.{h,cpp} Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/backends/Socket*.{h,cpp} Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/backends/Caching*.{h,cpp} Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/backends/ReadOnly*.{h,cpp} Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/backends/Auto*.{h,cpp}
git commit -m "feat(backends): StructuralDiff method across all IBlueprintReader implementations"
```

---

### Task 15: Register `bp_structural_diff` MCP tool + bump tool counts

**Files:**
- Modify: `Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/tools/BlueprintTools.cpp`
- Modify: `Plugins/BlueprintReader/Tests/BlueprintReaderMcpTests/Private/test_tools.cpp` (line 36)
- Modify: `Plugins/BlueprintReader/Tests/BlueprintReaderMcpTests/Private/test_mcp.cpp` (line 94)

- [ ] **Step 1: Register the tool — append to `RegisterBlueprintTools`**

Find the last `{ ToolDescriptor d; d.name = "..."; ... registry.Add(...); }` block in `BlueprintTools.cpp` and add after it:

```cpp
// ----- bp_structural_diff --------------------------------------------
{
    ToolDescriptor d;
    d.name = "bp_structural_diff";
    d.description =
        "[blueprint] Compare two Blueprints structurally — variables, "
        "components, function/macro/event-graph node signatures, "
        "connection counts. Returns {ok, differences[]}. Position- and "
        "GUID-independent so a freshly-rebuilt clone diffs cleanly "
        "against its source. Requires live or commandlet backend.";
    d.input_schema = {
        {"type", "object"},
        {"properties", {
            {"source",    {{"type","string"},{"description","Source BP package path"}}},
            {"candidate", {{"type","string"},{"description","Clone BP package path"}}},
            {"options",   {{"type","object"},
                           {"properties", {
                               {"ignore_node_positions", {{"type","boolean"}}},
                               {"ignore_comment_nodes",  {{"type","boolean"}}},
                           }}}},
        }},
        {"required", nlohmann::json::array({"source","candidate"})},
    };
    registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
        std::string a = RequireString(args, "source");
        std::string b = RequireString(args, "candidate");
        backends::IBlueprintReader::StructuralDiffOptions opts;
        if (args.contains("options")) {
            const auto& o = args["options"];
            opts.ignoreNodePositions = o.value("ignore_node_positions", true);
            opts.ignoreCommentNodes  = o.value("ignore_comment_nodes", false);
        }
        return reader.StructuralDiff(a, b, opts);
    });
}
```

- [ ] **Step 2: Bump tool count in `test_tools.cpp`**

```cpp
// Was: CHECK(spec.size() == 126);
CHECK(spec.size() == 127);
```

- [ ] **Step 3: Bump tool count in `test_mcp.cpp`**

```cpp
// Was: CHECK(list.size() == 126);
CHECK(list.size() == 127);
```

- [ ] **Step 4: Build & run**

```bash
"D:/Projects/Unreal Engine 5/Engine/Build/BatchFiles/Build.bat" BlueprintReaderMcpTests Win64 Development -project="D:/Projects/UE5_MCP/UE5_MCP.uproject" -NoUba -MaxParallelActions=4 -waitmutex && Binaries/Win64/BlueprintReaderMcpTests.exe
```

Expected: all 441+ cases pass (count assertions pick up the new tool).

- [ ] **Step 5: Commit**

```bash
git add Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/tools/BlueprintTools.cpp Plugins/BlueprintReader/Tests/BlueprintReaderMcpTests/Private/test_tools.cpp Plugins/BlueprintReader/Tests/BlueprintReaderMcpTests/Private/test_mcp.cpp
git commit -m "feat(tools): bp_structural_diff MCP tool + bump tool counts to 127"
```

---

### Task 16: `BPRoundtripModule` UBT target

**Files:**
- Create: `Plugins/BlueprintReader/Source/BPRoundtripModule/BPRoundtripModule.Build.cs`
- Create: `Plugins/BlueprintReader/Source/BPRoundtripModule/Private/BPRoundtripModule.cpp`
- Create: `Plugins/BlueprintReader/Source/BPRoundtripModule/Private/.gitkeep`
- Modify: `Plugins/BlueprintReader/BlueprintReader.uplugin` (add `BPRoundtripModule` entry)
- Modify: `.gitignore` (exclude `BPRoundtripModule/Private/*.cpp`/`*.h` except `BPRoundtripModule.cpp` and `.gitkeep`)

- [ ] **Step 1: Build.cs**

```csharp
using UnrealBuildTool;

public class BPRoundtripModule : ModuleRules {
    public BPRoundtripModule(ReadOnlyTargetRules Target) : base(Target) {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new string[] {
            "Core", "CoreUObject", "Engine", "GameplayTags",
        });
    }
}
```

- [ ] **Step 2: Module entry .cpp**

```cpp
#include "Modules/ModuleManager.h"

IMPLEMENT_MODULE(FDefaultModuleImpl, BPRoundtripModule);
```

- [ ] **Step 3: Placeholder + .gitkeep**

```bash
touch Plugins/BlueprintReader/Source/BPRoundtripModule/Private/.gitkeep
```

- [ ] **Step 4: Register in .uplugin**

Read the current `.uplugin`, add inside the `Modules` array:

```json
{
    "Name": "BPRoundtripModule",
    "Type": "Runtime",
    "LoadingPhase": "Default"
}
```

- [ ] **Step 5: Update .gitignore so emitted files don't leak**

Add to root `.gitignore`:

```
# BPIR-roundtrip generated code (regenerated by BPIRRoundtrip tests)
Plugins/BlueprintReader/Source/BPRoundtripModule/Private/Generated/
```

- [ ] **Step 6: Build the editor to verify the new module compiles**

```bash
"D:/Projects/Unreal Engine 5/Engine/Build/BatchFiles/Build.bat" UE5_MCPEditor Win64 Development -project="D:/Projects/UE5_MCP/UE5_MCP.uproject" -NoUba -MaxParallelActions=4 -waitmutex
```

Expected: clean build; UBT picks up the new module.

- [ ] **Step 7: Commit**

```bash
git add Plugins/BlueprintReader/Source/BPRoundtripModule/ Plugins/BlueprintReader/BlueprintReader.uplugin .gitignore
git commit -m "build: BPRoundtripModule — UBT target for BPIR-roundtrip generated C++"
```

---

### Task 17: `BPIRRoundtrip` library

**Files:**
- Create: `Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/roundtrip/BPIRRoundtrip.h`
- Create: `Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/roundtrip/BPIRRoundtrip.cpp`

- [ ] **Step 1: Header**

```cpp
// BPIRRoundtrip — drives the full BP → BPIR → C++ → UBT → BPIR' → BP
// pipeline. Used by the BPIR-path roundtrip tests; orchestrates calls
// across IBlueprintReader (decompile / transpile) and the local UBT
// toolchain (compile step).
#pragma once

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "../backends/IBlueprintReader.h"

namespace bpr::roundtrip {

struct BPIRRoundtripResult {
    bool ok = false;
    std::string source_package_path;
    std::string output_package_path;
    std::string emitted_cpp_path;
    std::string emitted_h_path;
    std::string failing_stage;     // "decompile", "emit", "compile", "parse", "transpile"
    std::string error_message;
    std::string build_log_path;    // populated on compile failures
    nlohmann::json bpir_before;    // BPIR captured pre-compile (for diffing)
    nlohmann::json bpir_after;     // BPIR captured post-parse
};

// Runs the full pipeline. `engineDir` / `projectFile` point at the
// installation for the UBT compile step.
BPIRRoundtripResult RunBPIRRoundtrip(backends::IBlueprintReader& reader,
                                     std::string_view sourcePackagePath,
                                     std::string_view outputPackagePath,
                                     std::string_view engineDir,
                                     std::string_view projectFile);

}    // namespace bpr::roundtrip
```

- [ ] **Step 2: Implementation**

```cpp
#include "BPIRRoundtrip.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace bpr::roundtrip {

namespace {
    std::string GeneratedDir() {
        return "Plugins/BlueprintReader/Source/BPRoundtripModule/Private/Generated";
    }

    std::string SanitizeClassName(std::string_view pkg) {
        std::string out;
        for (char c : pkg) {
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9')) out += c;
            else if (out.empty() || out.back() != '_') out += '_';
        }
        while (!out.empty() && out.back() == '_') out.pop_back();
        return "BPR_" + out;
    }

    int RunCommand(const std::string& cmd, const std::string& logPath) {
        std::string full = cmd + " > \"" + logPath + "\" 2>&1";
        return std::system(full.c_str());
    }
}

BPIRRoundtripResult RunBPIRRoundtrip(backends::IBlueprintReader& reader,
                                     std::string_view sourcePackagePath,
                                     std::string_view outputPackagePath,
                                     std::string_view engineDir,
                                     std::string_view projectFile) {
    BPIRRoundtripResult res;
    res.source_package_path = std::string(sourcePackagePath);
    res.output_package_path = std::string(outputPackagePath);

    // 1. Decompile
    try {
        // Convention: decompile_blueprint is registered as an MCP tool but
        // the per-function path goes through IBlueprintReader::GetFunction
        // followed by tools::DecompileFunction. The whole-BP path is a
        // server-side composition. For roundtrip we want a *complete* BPIR
        // covering every function + the event graph.
        // The simplest stable path: use the existing decompile_blueprint
        // tool through the registry. But here we're library-side, so we
        // call it directly via tools::DecompileBlueprintWhole (which the
        // existing transpile_blueprint tool already uses).
        // Pseudo-call: tools::DecompileBlueprintWhole(reader, sourcePackagePath)
        // Replace with the real helper name found in tools/Decompile.h.
        res.bpir_before = tools::DecompileBlueprintWhole(reader, sourcePackagePath);
    } catch (const std::exception& e) {
        res.failing_stage = "decompile"; res.error_message = e.what(); return res;
    }

    // 2. Emit C++ pair under BPRoundtripModule/Private/Generated/
    const std::string className = SanitizeClassName(sourcePackagePath);
    const std::string genDir = GeneratedDir();
    std::filesystem::create_directories(genDir);
    const std::string hPath   = genDir + "/" + className + ".h";
    const std::string cppPath = genDir + "/" + className + ".cpp";

    try {
        // Same convention: use tools::EmitCppPair(bpir) -> {.h text, .cpp text}.
        auto [hText, cppText] = tools::EmitCppPair(res.bpir_before, className);
        std::ofstream(hPath)   << hText;
        std::ofstream(cppPath) << cppText;
        res.emitted_h_path   = hPath;
        res.emitted_cpp_path = cppPath;
    } catch (const std::exception& e) {
        res.failing_stage = "emit"; res.error_message = e.what(); return res;
    }

    // 3. Compile via UBT (build the BPRoundtripModule)
    {
        std::string buildBat = std::string(engineDir) +
            "/Engine/Build/BatchFiles/Build.bat";
        std::string cmd = "\"" + buildBat + "\" BPRoundtripModule Win64 "
                          "Development -project=\"" + std::string(projectFile) +
                          "\" -NoUba -MaxParallelActions=4 -waitmutex";
        const std::string logPath = genDir + "/build.log";
        int rc = RunCommand(cmd, logPath);
        res.build_log_path = logPath;
        if (rc != 0) {
            res.failing_stage = "compile";
            // Tail the log into the error message
            std::ifstream in(logPath);
            std::ostringstream tail;
            std::string line;
            std::vector<std::string> lines;
            while (std::getline(in, line)) lines.push_back(line);
            const std::size_t lastN = std::min<std::size_t>(40, lines.size());
            for (std::size_t i = lines.size() - lastN; i < lines.size(); ++i)
                tail << lines[i] << '\n';
            res.error_message = "UBT compile failed (rc=" + std::to_string(rc) +
                                "); last " + std::to_string(lastN) +
                                " lines:\n" + tail.str();
            return res;
        }
    }

    // 4. Parse emitted C++ back to BPIR
    try {
        // tools::ParseCppPairToBpir(hPath, cppPath) -> bpir json
        res.bpir_after = tools::ParseCppPairToBpir(hPath, cppPath);
    } catch (const std::exception& e) {
        res.failing_stage = "parse"; res.error_message = e.what(); return res;
    }

    // 5. Transpile bpir_after -> new BP at outputPackagePath
    try {
        // Same convention again: tools::TranspileBlueprintWhole(reader,
        //   bpir_after, outputPackagePath)
        tools::TranspileBlueprintWhole(reader, res.bpir_after, outputPackagePath);
    } catch (const std::exception& e) {
        res.failing_stage = "transpile"; res.error_message = e.what(); return res;
    }

    res.ok = true;
    return res;
}

}    // namespace bpr::roundtrip
```

NOTE on the `tools::DecompileBlueprintWhole / EmitCppPair / ParseCppPairToBpir / TranspileBlueprintWhole` calls above: these names are illustrative. The actual helper names live in `Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/tools/Decompile.h`, `tools/codegen/CppClassEmit.h`, `tools/parse/CppParse.h`, and `tools/Bpir.h` — substitute the real symbol names by grep when you write this:

```bash
grep -n "^[A-Za-z].*Decompile\|EmitCpp\|ParseCpp\|Transpile" Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/tools/*.h Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/tools/**/*.h
```

If the right composite isn't there, lift the registry-tool lambdas from `BlueprintTools.cpp` (`decompile_blueprint`, `transpile_blueprint`) into reusable helpers and call those.

- [ ] **Step 3: Build & verify it compiles (no tests for it yet — Task 21/22 cover it)**

```bash
"D:/Projects/Unreal Engine 5/Engine/Build/BatchFiles/Build.bat" BlueprintReaderMcpTests Win64 Development -project="D:/Projects/UE5_MCP/UE5_MCP.uproject" -NoUba -MaxParallelActions=4 -waitmutex
```

- [ ] **Step 4: Commit**

```bash
git add Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/roundtrip/BPIRRoundtrip.h Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/roundtrip/BPIRRoundtrip.cpp
git commit -m "feat(roundtrip): BPIRRoundtrip — full BP -> BPIR -> C++ -> UBT -> BPIR -> BP pipeline"
```

---

### Task 18: `test_structural_diff.cpp`

**Files:**
- Create: `Plugins/BlueprintReader/Tests/BlueprintReaderMcpTests/Private/test_structural_diff.cpp`

- [ ] **Step 1: Write doctest cases covering each diff category**

```cpp
// Synthetic structural-diff tests — exercise the diff library against
// mock-shaped scenarios. Live diff tests are covered in
// test_roundtrip_*.cpp.
#include <doctest/doctest.h>

#include "../../BlueprintReaderMcpCore/Private/backends/IBlueprintReader.h"
#include "../../BlueprintReaderMcpCore/Private/backends/MockBlueprintReader.h"

TEST_CASE("structural_diff: mock backend reports not-supported") {
    bpr::backends::MockBlueprintReader mock;
    CHECK_THROWS_AS(mock.StructuralDiff("/Game/A", "/Game/B"),
                    bpr::backends::BlueprintReaderError);
}

// Note: real diff behavior is tested end-to-end in
// test_roundtrip_granular.cpp where we actually have two UBlueprints.
// Here we just confirm the wiring; live backend is required for
// substantive diff testing.
```

- [ ] **Step 2: Build & run**

```bash
"D:/Projects/Unreal Engine 5/Engine/Build/BatchFiles/Build.bat" BlueprintReaderMcpTests Win64 Development -project="D:/Projects/UE5_MCP/UE5_MCP.uproject" -NoUba -MaxParallelActions=4 -waitmutex && Binaries/Win64/BlueprintReaderMcpTests.exe -tc="structural_diff*"
```

- [ ] **Step 3: Commit**

```bash
git add Plugins/BlueprintReader/Tests/BlueprintReaderMcpTests/Private/test_structural_diff.cpp
git commit -m "test: structural_diff wiring smoke tests"
```

---

### Task 19: Granular roundtrip — `BP_TestEnemy` smoke

**Files:**
- Create: `Plugins/BlueprintReader/Tests/BlueprintReaderMcpTests/Private/test_roundtrip_granular.cpp`
- Create: `Plugins/BlueprintReader/Tests/BlueprintReaderMcpTests/fixtures/BP_TestEnemy_spec.json` (generated)

- [ ] **Step 1: Write the smoke test**

```cpp
// Granular-writes roundtrip — ReadToSpec -> SpecToBP -> bp_structural_diff.
// Gated on live backend (env BP_READER_BACKEND=commandlet or live).
#include <doctest/doctest.h>
#include <cstdlib>
#include <fstream>

#include "../../BlueprintReaderMcpCore/Private/backends/BackendFactory.h"
#include "../../BlueprintReaderMcpCore/Private/roundtrip/BPSpec.h"
#include "../../BlueprintReaderMcpCore/Private/roundtrip/ReadToSpec.h"
#include "../../BlueprintReaderMcpCore/Private/roundtrip/SpecToBP.h"

namespace {
    bool LiveBackendReady() {
        const char* be = std::getenv("BP_READER_BACKEND");
        if (!be) return false;
        std::string s(be);
        return s == "commandlet" || s == "live" || s == "auto";
    }
}

TEST_CASE("roundtrip_granular: BP_TestEnemy" * doctest::skip(!LiveBackendReady())) {
    auto backend = bpr::backends::MakeBackend();
    REQUIRE(backend != nullptr);

    const std::string source = "/Game/AI/BP_TestEnemy";
    const std::string clone  = "/Game/Recreated/BP_TestEnemy_Granular";

    // 1. Read
    auto spec = bpr::roundtrip::ReadToSpec(*backend, source);
    REQUIRE_FALSE(spec.incomplete);

    // Persist the golden fixture (write on first run; check on later runs).
    const std::string fixturePath =
        "../Tests/BlueprintReaderMcpTests/fixtures/BP_TestEnemy_spec.json";
    auto j = bpr::roundtrip::ToJson(spec);
    if (!std::ifstream(fixturePath)) {
        std::ofstream(fixturePath) << j.dump(2);
    } else {
        // Compare against golden — fails loudly on read drift.
        std::ifstream in(fixturePath);
        nlohmann::json golden;
        in >> golden;
        CHECK(j == golden);
    }

    // 2. Delete any prior clone (idempotency)
    try { backend->DeleteAsset(clone, /*force=*/true); }
    catch (...) { /* ignore */ }

    // 3. Write
    auto res = bpr::roundtrip::SpecToBP(*backend, spec, clone);
    CAPTURE(res.failing_stage); CAPTURE(res.failing_op); CAPTURE(res.error_message);
    REQUIRE(res.ok);

    // 4. Save & diff
    backend->SaveAll(/*dirtyOnly=*/true);
    auto diff = backend->StructuralDiff(source, clone, {});
    INFO("diff: " << diff.dump(2));
    CHECK(diff.value("ok", false));
}
```

- [ ] **Step 2: Build & run with the commandlet backend live**

```bash
export BP_READER_BACKEND=commandlet
export BP_READER_ENGINE_DIR="D:/Projects/Unreal Engine 5"
export BP_READER_PROJECT="D:/Projects/UE5_MCP/UE5_MCP.uproject"
"D:/Projects/Unreal Engine 5/Engine/Build/BatchFiles/Build.bat" BlueprintReaderMcpTests Win64 Development -project="D:/Projects/UE5_MCP/UE5_MCP.uproject" -NoUba -MaxParallelActions=4 -waitmutex && Binaries/Win64/BlueprintReaderMcpTests.exe -tc="roundtrip_granular: BP_TestEnemy"
```

Expected: test passes, fixture `BP_TestEnemy_spec.json` is created on first run.

- [ ] **Step 3: If diff is non-empty, examine, decide whether to (a) fix SpecToBP / ReadToSpec, (b) extend diff whitelist, or (c) declare the difference acceptable and append to a per-test exemption list.**

Common cases:
- "extra node" in clone that's auto-spawned by `AddFunction` (FunctionEntry/Result) → already handled in SpecToBP, verify the skip logic.
- Pin-default mismatch on `K2Node_VariableSet` that we didn't apply → extend SpecToBP to call `SetPinDefault` for each non-empty pin default.

- [ ] **Step 4: Commit**

```bash
git add Plugins/BlueprintReader/Tests/BlueprintReaderMcpTests/Private/test_roundtrip_granular.cpp Plugins/BlueprintReader/Tests/BlueprintReaderMcpTests/fixtures/BP_TestEnemy_spec.json
git commit -m "test: granular roundtrip smoke — BP_TestEnemy"
```

---

### Task 20: Granular roundtrip — TPC full

**Files:**
- Modify: `Plugins/BlueprintReader/Tests/BlueprintReaderMcpTests/Private/test_roundtrip_granular.cpp` (add TPC case)
- Create: `Plugins/BlueprintReader/Tests/BlueprintReaderMcpTests/fixtures/BP_TPC_spec.json` (generated)

- [ ] **Step 1: Append the TPC case (slow doctest tag)**

```cpp
TEST_CASE("roundtrip_granular: TPC"
          * doctest::skip(!LiveBackendReady())
          * doctest::test_suite("slow")) {
    auto backend = bpr::backends::MakeBackend();
    REQUIRE(backend != nullptr);

    const std::string source = "/Game/Imported/ThirdPerson/BP_ThirdPersonCharacter";
    const std::string clone  = "/Game/Recreated/BP_TPC_Granular";

    auto spec = bpr::roundtrip::ReadToSpec(*backend, source);
    REQUIRE_FALSE(spec.incomplete);

    const std::string fixturePath = "../Tests/BlueprintReaderMcpTests/fixtures/BP_TPC_spec.json";
    auto j = bpr::roundtrip::ToJson(spec);
    if (!std::ifstream(fixturePath)) {
        std::ofstream(fixturePath) << j.dump(2);
    } else {
        std::ifstream in(fixturePath);
        nlohmann::json golden;
        in >> golden;
        CHECK(j == golden);
    }

    try { backend->DeleteAsset(clone, /*force=*/true); } catch (...) {}

    auto res = bpr::roundtrip::SpecToBP(*backend, spec, clone);
    CAPTURE(res.failing_stage); CAPTURE(res.failing_op); CAPTURE(res.error_message);
    REQUIRE(res.ok);

    backend->SaveAll(true);
    auto diff = backend->StructuralDiff(source, clone, {});
    INFO("diff: " << diff.dump(2));
    CHECK(diff.value("ok", false));
}
```

- [ ] **Step 2: Build & run with the slow suite enabled**

```bash
Binaries/Win64/BlueprintReaderMcpTests.exe -ts="slow" -tc="roundtrip_granular: TPC"
```

Expected: pass; if it fails, the diff output drives the next iteration.

- [ ] **Step 3: Commit**

```bash
git add Plugins/BlueprintReader/Tests/BlueprintReaderMcpTests/Private/test_roundtrip_granular.cpp Plugins/BlueprintReader/Tests/BlueprintReaderMcpTests/fixtures/BP_TPC_spec.json
git commit -m "test: granular roundtrip full — TPC"
```

---

### Task 21: BPIR roundtrip — `BP_TestEnemy` smoke

**Files:**
- Create: `Plugins/BlueprintReader/Tests/BlueprintReaderMcpTests/Private/test_roundtrip_bpir.cpp`
- Create: `Plugins/BlueprintReader/Tests/BlueprintReaderMcpTests/fixtures/BP_TestEnemy_bpir.json` (generated)

- [ ] **Step 1: Write the smoke test**

```cpp
#include <doctest/doctest.h>
#include <cstdlib>
#include <fstream>

#include "../../BlueprintReaderMcpCore/Private/backends/BackendFactory.h"
#include "../../BlueprintReaderMcpCore/Private/roundtrip/BPIRRoundtrip.h"

namespace {
    bool LiveBackendReady() {
        const char* be = std::getenv("BP_READER_BACKEND");
        return be && (std::string(be) == "commandlet" ||
                      std::string(be) == "live" ||
                      std::string(be) == "auto");
    }
}

TEST_CASE("roundtrip_bpir: BP_TestEnemy" * doctest::skip(!LiveBackendReady())) {
    auto backend = bpr::backends::MakeBackend();
    REQUIRE(backend);

    const std::string src    = "/Game/AI/BP_TestEnemy";
    const std::string clone  = "/Game/Recreated/BP_TestEnemy_BPIR";
    const char*       engine = std::getenv("BP_READER_ENGINE_DIR");
    const char*       proj   = std::getenv("BP_READER_PROJECT");
    REQUIRE(engine); REQUIRE(proj);

    try { backend->DeleteAsset(clone, /*force=*/true); } catch (...) {}

    auto res = bpr::roundtrip::RunBPIRRoundtrip(*backend, src, clone, engine, proj);
    CAPTURE(res.failing_stage); CAPTURE(res.error_message);
    REQUIRE(res.ok);

    // Golden fixture for the BPIR shape — flags drift in decompile / emit / parse.
    const std::string fixturePath = "../Tests/BlueprintReaderMcpTests/fixtures/BP_TestEnemy_bpir.json";
    if (!std::ifstream(fixturePath)) {
        std::ofstream(fixturePath) << res.bpir_after.dump(2);
    } else {
        std::ifstream in(fixturePath);
        nlohmann::json golden; in >> golden;
        CHECK(res.bpir_after == golden);
    }

    backend->SaveAll(true);
    auto diff = backend->StructuralDiff(src, clone, {});
    INFO("diff: " << diff.dump(2));
    CHECK(diff.value("ok", false));
}
```

- [ ] **Step 2: Build & run**

```bash
Binaries/Win64/BlueprintReaderMcpTests.exe -tc="roundtrip_bpir: BP_TestEnemy"
```

Expected: pass (slow — includes a UBT compile). If the compile step fails, examine `build.log` at the path in `res.build_log_path`.

- [ ] **Step 3: Commit**

```bash
git add Plugins/BlueprintReader/Tests/BlueprintReaderMcpTests/Private/test_roundtrip_bpir.cpp Plugins/BlueprintReader/Tests/BlueprintReaderMcpTests/fixtures/BP_TestEnemy_bpir.json
git commit -m "test: BPIR roundtrip smoke — BP_TestEnemy"
```

---

### Task 22: BPIR roundtrip — TPC full

**Files:**
- Modify: `Plugins/BlueprintReader/Tests/BlueprintReaderMcpTests/Private/test_roundtrip_bpir.cpp` (add TPC case)
- Create: `Plugins/BlueprintReader/Tests/BlueprintReaderMcpTests/fixtures/BP_TPC_bpir.json` (generated)

- [ ] **Step 1: Append the TPC case (slow tag)**

```cpp
TEST_CASE("roundtrip_bpir: TPC"
          * doctest::skip(!LiveBackendReady())
          * doctest::test_suite("slow")) {
    auto backend = bpr::backends::MakeBackend();
    REQUIRE(backend);

    const std::string src    = "/Game/Imported/ThirdPerson/BP_ThirdPersonCharacter";
    const std::string clone  = "/Game/Recreated/BP_TPC_BPIR";
    const char*       engine = std::getenv("BP_READER_ENGINE_DIR");
    const char*       proj   = std::getenv("BP_READER_PROJECT");
    REQUIRE(engine); REQUIRE(proj);

    try { backend->DeleteAsset(clone, /*force=*/true); } catch (...) {}

    auto res = bpr::roundtrip::RunBPIRRoundtrip(*backend, src, clone, engine, proj);
    CAPTURE(res.failing_stage); CAPTURE(res.error_message);
    REQUIRE(res.ok);

    const std::string fixturePath = "../Tests/BlueprintReaderMcpTests/fixtures/BP_TPC_bpir.json";
    if (!std::ifstream(fixturePath)) {
        std::ofstream(fixturePath) << res.bpir_after.dump(2);
    } else {
        std::ifstream in(fixturePath);
        nlohmann::json golden; in >> golden;
        CHECK(res.bpir_after == golden);
    }

    backend->SaveAll(true);
    auto diff = backend->StructuralDiff(src, clone, {});
    INFO("diff: " << diff.dump(2));
    CHECK(diff.value("ok", false));
}
```

- [ ] **Step 2: Build & run**

```bash
Binaries/Win64/BlueprintReaderMcpTests.exe -ts="slow" -tc="roundtrip_bpir: TPC"
```

Expected: pass (very slow — BPIR compile of TPC-sized graph).

- [ ] **Step 3: Commit**

```bash
git add Plugins/BlueprintReader/Tests/BlueprintReaderMcpTests/Private/test_roundtrip_bpir.cpp Plugins/BlueprintReader/Tests/BlueprintReaderMcpTests/fixtures/BP_TPC_bpir.json
git commit -m "test: BPIR roundtrip full — TPC"
```

---

### Task 23: Sentinel emission + final verification

**Files:**
- Verify state across all preceding tasks.

- [ ] **Step 1: Run the full test suite**

```bash
export BP_READER_BACKEND=commandlet
export BP_READER_ENGINE_DIR="D:/Projects/Unreal Engine 5"
export BP_READER_PROJECT="D:/Projects/UE5_MCP/UE5_MCP.uproject"
Binaries/Win64/BlueprintReaderMcpTests.exe
```

Expected: all 441 (now 441+) cases pass; smoke roundtrips pass; slow suite passes when run with `-ts="slow"`.

- [ ] **Step 2: Verify all seven research files exist and are committed**

```bash
ls docs/research/*.md
git log --oneline -- docs/research/
```

Expected: 7 files (README + 6 topics), each with at least one commit.

- [ ] **Step 3: Verify TPC import is committed**

```bash
git log --oneline -- Content/Imported/ThirdPerson/
```

Expected: at least one commit adding the import.

- [ ] **Step 4: Verify the new MCP tool count is 127**

```bash
grep "spec.size() == \|list.size() == " Plugins/BlueprintReader/Tests/BlueprintReaderMcpTests/Private/test_tools.cpp Plugins/BlueprintReader/Tests/BlueprintReaderMcpTests/Private/test_mcp.cpp
```

Expected: both report `== 127`.

- [ ] **Step 5: Emit the completion sentinel**

When all acceptance criteria above are met (and only then), the final user-visible message of the iteration includes the literal string `RALPH-DONE-BP-ROUNDTRIP`. (If the user restarted Ralph with `--completion-promise 'RALPH-DONE-BP-ROUNDTRIP'`, this will exit the loop.)

- [ ] **Step 6: Final commit**

If any cleanup was done in Step 1-4, commit it:

```bash
git add -A
git commit -m "chore(roundtrip): final verification — RALPH-DONE-BP-ROUNDTRIP"
```

---

## Execution

This plan is being run inside an active Ralph loop. The user disabled checkpoints — do not stop to ask for execution-mode approval. Behavior:

- Phase 1 tasks 1–5 are independent; if dispatch budget allows, use `superpowers:subagent-driven-development` to run them in parallel via fresh subagents.
- Tasks 0, 6, 7, 8 are sequential within Phase 1 (Task 8 depends on Task 7 which depends on Task 6).
- Phase 2 (Tasks 9–23) is mostly sequential; use `superpowers:executing-plans` inline.
- Each Ralph iteration: open this file, find the first unchecked `[ ]` step, execute, tick the box, commit, continue until either (a) the task is complete and committed (move to next task) or (b) iteration budget exhausted (stop, let Ralph re-fire with the same plan).
- Sentinel is emitted only by Task 23 Step 5, only when every preceding box is ticked AND all tests pass.
