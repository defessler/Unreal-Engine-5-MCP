# bp-reader extensibility audit

Phase-1 deliverable for the BP-roundtrip capability work
(`docs/superpowers/specs/2026-05-16-bp-roundtrip-capability-design.md`).
Audience: an engineer about to add a new backend, MCP tool, BPIR node
type, or codegen target. Documents the four extension seams, lists the
concrete leaks found while reading the code, and gives three cookbook
sketches with line-cite anchors.

All cite paths are absolute. Line ranges are inclusive.

---

## 1. The seams

### 1.1 `bpr::backends::IBlueprintReader`

- **File:** `D:/Projects/UE5_MCP/Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/backends/IBlueprintReader.h:33-1298`
- **Shape:** abstract base class. Seven pure-virtual methods on lines
  37-79 (the original read + write surface). Everything from line 187
  onward has a default impl that throws
  `BlueprintReaderError("X not supported by this backend")`. Two
  exceptions on lines 1277-1297 have real no-op defaults
  (`BeginBatch`, `EndBatch`, `ShutdownDaemon`).
- **Concrete subclasses:** `MockBlueprintReader`,
  `CommandletBlueprintReader`, `SocketBlueprintReader`,
  `AutoBlueprintReader`, plus two decorators `CachingBlueprintReader`
  and `ReadOnlyBlueprintReader`.
- **Wire-error contract:** any backend method may throw
  `BlueprintReaderError` (line 21-24) or its subclass `AssetNotFound`
  (line 28-31); the MCP layer translates them to the tool-error
  envelope.
- **To add a new backend method:** add it as a pure virtual in the
  original read/write block (lines 37-169) OR — if it's optional for
  some backends — give it a throwing default like the project / data
  table / material families (e.g. lines 187-189 for
  `GetProjectMetadata`). Pure-virtual forces every subclass to override
  (compile-fails on omission); defaulting lets older backends keep
  compiling but quietly returns the throw-default to callers — which
  is exactly the failure mode the leak section below catalogs.
- **To add a new backend:** subclass, implement methods, register in
  `backends/BackendFactory.cpp:218-288`. The factory's
  `Create(BackendConfig)` switches on `cfg.backend` (mock /
  commandlet / live / auto) and wraps the result in
  `WrapWithCache` + `MaybeWrapReadOnly` before returning.

### 1.2 `bpr::tools::ToolRegistry`

- **Files:**
  `D:/Projects/UE5_MCP/Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/tools/ToolRegistry.h:33-115`
  (descriptor + handler types, public API),
  `D:/Projects/UE5_MCP/Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/tools/ToolRegistry.cpp:9-167`
  (impl).
- **Shape:** `ToolDescriptor { name, description, input_schema }` paired
  with a `ToolFn = std::function<json(const json&)>`. Lookup returns
  the function pointer or `nullptr` if filtered/missing.
- **Two visibility models** documented at
  `ToolRegistry.h:5-20`: a static `ApplyFilter` for env-var trimming
  (`BP_READER_TOOLS` / `BP_READER_TOOLS_EXCLUDE`) and a progressive
  `ActivateToken` widening surface used by the
  `enable_tool_category` meta-tool (`BlueprintTools.cpp:4415-4463`).
- **Replace-in-place** semantics on `Add()`
  (`ToolRegistry.cpp:9-28`) — re-registering the same name overwrites
  the previous descriptor + function, doesn't duplicate.
- **To add a new tool:** append a block to
  `BlueprintTools.cpp::RegisterBlueprintTools` (e.g. the
  `list_blueprints` example at lines 151-180; the `add_node`
  example at lines 896-1010). Each block:
  1. Declares `ToolDescriptor d`,
  2. Sets `d.name`, `d.description`, `d.input_schema`,
  3. Calls `registry.Add(std::move(d), [&reader](const json& args) { ... })`.
- **Tool count assertions** at
  `D:/Projects/UE5_MCP/Plugins/BlueprintReader/Tests/BlueprintReaderMcpTests/Private/test_tools.cpp:36`
  and `test_mcp.cpp:94` both pin `spec.size() == 127`. Bump both when
  adding a tool. (CLAUDE.md's "tools/test count" note covers this.)
- **Category membership** at
  `D:/Projects/UE5_MCP/Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/tools/ToolCategories.cpp`
  groups tools for the filter/disclosure plumbing; new tools should be
  added to the relevant category list (e.g. `assets`, `cpp`,
  `materials`) so the env-var filter and the `enable_tool_category`
  meta-tool can address them.
- **Per-tool dispatch lives in two helper files** for the larger DSL
  surfaces:
  `D:/Projects/UE5_MCP/Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/tools/ApplyOps.cpp:963-1062`
  registers `apply_ops` + `preview_ops`;
  `D:/Projects/UE5_MCP/Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/tools/CompileFunction.cpp:388-573`
  registers `compile_function`. Both are invoked from
  `BlueprintTools.cpp:4408-4413` at the bottom of
  `RegisterBlueprintTools`.

### 1.3 `bpr::tools::Bpir`

- **File:** `D:/Projects/UE5_MCP/Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/tools/Bpir.h:1-118`,
  impl `Bpir.cpp:44-452`.
- **Shape:** JSON AST. Two top-level kinds: `function` and `class`
  (`Bpir.h:13-29`). A `function` doc has `inputs/outputs/locals/body`
  arrays; `body` is a sequence of statement objects. A `class` doc has
  `variables/functions/interfaces`.
- **Statement form vocabulary** (the `StatementForms()` accessor
  returns this list, used by every consumer): lines `Bpir.cpp:49-60`:
  `if, set, call, comment, return, cast, switch, for_each, while,
  sequence, break, continue, broadcast, bind_delegate,
  unbind_delegate, clear_delegate, unsupported`.
- **Expression form vocabulary** (`Bpir.cpp:62-69`):
  `var, lit, call, cast, member, index, self, new_array, new_struct,
  new_set, new_map`.
- **Schema versioning** at `Bpir.h:80-85`: `kBpirSchemaVersion = 1`.
  `MigrateToCurrent` (`Bpir.h:115`) is the seam for future v2 → v1
  back-translation; today it's an identity.
- **To add a new statement form:** four touch points:
  1. Add the key to the static vector in
     `Bpir.cpp:StatementFormsImpl()` lines 49-60.
  2. Add a `ValidateStatement` arm handling its required fields
     (`Bpir.cpp` near the other arms, starting around line 217).
  3. Add a recognizer in `Decompile.cpp` that pattern-matches the
     underlying `K2Node_*` class and emits the new form (e.g. the
     `K2Node_IfThenElse` arm at lines 939-985 is the prototype).
  4. Add a lowerer in `CppEmit.cpp::EmitStatement`
     (`CppEmit.cpp:1157-1541` — the existing arms each handle one
     form).
- **To add a new expression form:** same four points but in
  `ValidateExpression` (`Bpir.cpp:85-220`) and
  `CppEmit.cpp::EmitExpr` (around `CppEmit.cpp:520-720`).
- **Pivot character:** `Decompile.cpp` is the BP→BPIR pass,
  `CompileFunction.cpp` is the BPIR→BP pass, `CppEmit.cpp` is the
  BPIR→C++ pass, `CppParse.cpp` is the C++→BPIR pass. Each form needs
  coverage in three of the four (Decompile + CppEmit + CompileFunction
  for a graph round-trip; CppParse only when the form is something
  someone would write in C++).

### 1.4 `bpr::tools::codegen::CppEmit` / `CppClassEmit`

- **Files:**
  `D:/Projects/UE5_MCP/Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/tools/codegen/CppEmit.h:1-89`
  (function-body emitter),
  `D:/Projects/UE5_MCP/Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/tools/codegen/CppEmit.cpp`
  (impl),
  `D:/Projects/UE5_MCP/Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/tools/codegen/CppClassEmit.h:1-124`
  (whole-class emitter),
  `D:/Projects/UE5_MCP/Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/tools/codegen/UnsupportedTreatment.h:1-82`
  (per-node fallback table).
- **Shape:** `EmitCppFunction(bpirDoc, opts) -> {source, notes}` and
  `EmitCppClass(bpirClassDoc, opts) -> {headerSource, implSource,
  headerFileName, implFileName, className, notes}`. The
  `notes` array carries unsupported-node entries that get rolled into
  the `*.transpile-notes.json` sidecar built by `BuildSidecar`
  (`UnsupportedTreatment.h:78-80`, invocation
  `BlueprintTools.cpp:427`).
- **Dispatch:** `CppEmit.cpp:EmitStatement`
  (lines 1157-1541) is a long `if/else` over `form` —
  one arm per statement form. `EmitExpr` (around line 510) does the
  same for expressions. New target languages would mirror this
  pattern.
- **Style options** at `CppEmit.h:20-28` and
  `CppClassEmit.h:44-89` — readable vs compilable mode, operator
  aliasing, UPROPERTY/UFUNCTION category remap, module API macro.
- **Type mapping helpers** at `CppEmit.h:60-87`:
  `MapBpirTypeToCpp`, `MapBpirTypeToCppMember` (TObjectPtr<>),
  `MapBpirTypeToCppArg` (const-ref-by-default for heavy types).
- **To add a new target language:** write `<Lang>Emit.h/.cpp` next to
  `CppEmit.cpp`, mirror the dispatch pattern. The MCP tool
  `transpile_function` (`BlueprintTools.cpp:276-333`) already accepts
  a `target_lang` arg but currently throws for anything other than
  `"cpp"` (line 309-313) — the new emit module would be wired by
  adding an enum/lookup at that point.

### 1.5 `bpr::tools::parse::CppParse` / `CppLex`

- **Files:**
  `D:/Projects/UE5_MCP/Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/tools/parse/CppParse.h:1-78`,
  `D:/Projects/UE5_MCP/Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/tools/parse/CppParse.cpp`,
  `D:/Projects/UE5_MCP/Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/tools/parse/CppLex.cpp`.
- **Shape:** `ParseCppFunction(source) -> bpirDoc` and
  `ParseCppFunction(source, signatureShell) -> bpirDoc` (`CppParse.h:65-77`).
  Recursive descent over a hand-rolled token stream. The lexer is
  separate so a future language could replace just the parser layer.
- **Supported subset:** documented at `CppParse.h:13-26`. Out of
  scope: preprocessor, templates beyond `Cast<T>`, lambdas, decltype,
  exception machinery, pointer arithmetic. Unrecognized constructs
  throw `CppParseError` (`CppParse.h:45-48`) with
  `<line>:<col>: message` prefix.
- **Tool exposure:** `parse_cpp_function`
  (`BlueprintTools.cpp:513-563`). Today there is NO class-level
  counterpart (`parse_cpp_blueprint`), even though `transpile_blueprint`
  emits a class — see leak 2.4.
- **To add a new source language:** new `<Lang>Parse.h/.cpp` pair next
  to `CppParse.cpp`, expose via a new tool (`parse_<lang>_function`)
  in `BlueprintTools.cpp`. The BPIR output is the only contract — as
  long as `ValidateBpir` accepts the result, every downstream
  consumer works unchanged.

### 1.6 Plugin-side `BlueprintReaderCommandlet::EOp`

- **File:** `D:/Projects/UE5_MCP/Plugins/BlueprintReader/Source/BlueprintReaderEditor/Private/BlueprintReaderCommandlet.cpp:132-271`
  (enum), `:273-382+` (string→enum parser), `:6391-6459` (dispatch
  switch for the legacy read ops), with each `Run<X>Op` defined
  elsewhere in the file.
- **Shape:** the op enum has one entry per plugin operation
  (~120 entries; the server's 127 tools map to a subset because some
  server-side tools are pure orchestrations of existing ops, e.g.
  `apply_ops`, `compile_function`, `summarize_blueprint`,
  `decompile_function`/`transpile_*`, `find_overriders`,
  `auto_layout_graph`, `list_node_kinds`, `list_pin_categories`,
  `parse_cpp_function`).
- **To add a new op:**
  1. New enum value in `EOp` (e.g. lines 200-209 for the most recent
     additions in the Stage 1 component-authoring block).
  2. Matching `ParseOp` arm
     (`BlueprintReaderCommandlet.cpp:281+`, in the
     long if-chain — note the gotcha: FParse path-conversion issues
     covered in CLAUDE.md's `FParse::Value` and "Git Bash" sections).
  3. Dispatch arm in `RunOneOp` (the legacy read-op switch is at
     :6391-6459; write ops follow a dispatch table elsewhere in the
     file — search for `case EOp::AddVariable` etc.).
  4. `Run<X>Op(Params, OutputPath, bPretty)` implementation that uses
     the shared helpers (`LoadMutableBlueprint`, `FindGraphByName`,
     `FindNodeByGuid`, `CompileAndSaveBlueprint`, `EmitOk`).
- **MCP-side mirror:** every new op needs a matching
  `IBlueprintReader` method (see seam 1.1), at least a
  mock-throws-or-stubs impl, and a `CommandletBlueprintReader` /
  `SocketBlueprintReader` impl that serializes args + calls
  `RunOp(...)`.
- **AutoBlueprintReader forwarder** for the new method must also be
  added (see leak 2.1 — the existing macro-driven forwarders
  (`AutoBlueprintReader.cpp:341-451`) cover only the original read +
  write block, NOT later additions).

---

## 2. Concrete leak audit

### 2.1 `[blocking-Phase-2]` AutoBlueprintReader does not forward Stage 2-4 / project-ops / live-editor methods

- **Files:** `D:/Projects/UE5_MCP/Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/backends/AutoBlueprintReader.h:86-145`
  (only original read + write + batch + shutdown declared) versus
  `D:/Projects/UE5_MCP/Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/backends/CachingBlueprintReader.h:60-258`
  (108 overrides total) and
  `D:/Projects/UE5_MCP/Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/backends/CommandletBlueprintReader.h:79-290`
  (116 overrides).
- **Count:** AutoBlueprintReader declares 32 `override` methods
  (counted via Grep). Commandlet declares 116, Caching declares 108.
  Auto is missing forwarders for ~76 methods including: every project
  op (`GetProjectMetadata`, `SaveAll`, `MoveAsset`, `DeleteAsset`,
  `CreateFolder` — `IBlueprintReader.h:187-244`), every data-table /
  data-asset / component / material / widget / behavior-tree /
  state-tree / niagara / sequencer / gameplay-tags / anim-bp method
  (`IBlueprintReader.h:248-1018`), every live-editor op
  (`ConsoleCommand`, `GetCVar`, `SetCVar`, `PieStart`, `PieStop`,
  `RunPythonScript`, `GetEditorState`, `GetSelectedActors`,
  `SpawnActor`, etc. — `IBlueprintReader.h:1029-1237`), automation
  tests (`RunAutomationTests`, line 1249), and several misc methods
  (`BuildLighting` line 1167, `ReadOutputLog` line 1233,
  `GetReferencers` / `GetDependencies` lines 1122/1126, config R/W
  lines 1144/1150).
- **Impact:** `auto` is the default backend per CLAUDE.md
  ("backend default — if we found a uproject, default to `auto`" at
  `BackendFactory.cpp:211-213`). Every Stage 2-4 tool, every project
  op, every live-editor op falls through the `IBlueprintReader`
  default impl and throws e.g. `"GetProjectMetadata not supported by
  this backend"` even when a real commandlet or live editor is sitting
  underneath. Phase 2's roundtrip tooling depends on
  `SaveAll`, `MoveAsset`, `DeleteAsset` (regenerating
  `BP_TPC_Granular`/`BP_TPC_BPIR`), and on live-editor introspection.
- **Why it's structural, not a one-off bug:** the forwarders are
  macro-driven (`AutoBlueprintReader.cpp:341-345` defines `FORWARD`
  and `FORWARD_VOID`), but the macros only get invoked for the
  methods explicitly declared in the header. The pattern requires
  manually adding both a declaration AND a forwarder for every new
  method. The 76 missing forwarders are methods added AFTER the
  initial Auto design.
- **Mitigation today:** users with Stage 2+ tools need
  `BP_READER_BACKEND=commandlet` or `BP_READER_BACKEND=live` set
  explicitly. The default `auto` value silently breaks those tools.
- **Fix shape:** add the missing method declarations (override) to
  `AutoBlueprintReader.h` and forwarder bodies to
  `AutoBlueprintReader.cpp`, mirroring
  `CachingBlueprintReader.cpp`'s pattern. Mechanical change; ~150
  lines.

### 2.2 `[blocking-Phase-2]` `compile_function` BPIR coverage is a tiny subset (4/18 statement forms)

- **File:** `D:/Projects/UE5_MCP/Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/tools/CompileFunction.cpp:282-372`
- **Symptom:** `CompileStatement` only recognizes `comment` (line
  291), `if` (line 295), `set` (line 320), and `call` (line 337). The
  catch-all on line 369-371 throws
  `"unrecognized statement form. Supported: {if,then,[else]},
  {set,to}, {call,args}, {comment}. Got: ..."`.
- **What's missing:** every other BPIR statement form documented at
  `Bpir.h:32-52` and surfaced by `StatementForms()`
  (`Bpir.cpp:49-60`): `return, cast, switch, for_each, while,
  sequence, break, continue, broadcast, bind_delegate,
  unbind_delegate, clear_delegate, unsupported`.
- **Impact:** Phase 2 path A (`BPIR → BP`) re-uses `compile_function`
  to materialize a function in `BP_TPC_Granular`. A TPC function body
  containing a `switch`, `cast`, `for_each`, `while`, or any delegate
  op (the TPC has multicast delegates) will fail at the first
  unsupported statement. The roundtrip can't complete.
- **Asymmetry:** Decompile (BP→BPIR) covers all forms;
  `Decompile.cpp:939-985` for `if`, lines 986-1024 for `cast`,
  2080-2131 for `switch`, 1475+ for macro instances (`for_each`,
  `while`), etc. CppEmit covers all forms
  (`CppEmit.cpp:1157-1541`). CppParse generates 12 forms
  (`CppParse.cpp` includes `break/continue/sequence/if/cast/for_each/
  while/switch/return/comment/set/call`). Only `compile_function`
  lags.
- **Fix shape:** extend the `if/else` chain at
  `CompileFunction.cpp:285-371` with arms for each missing form, each
  emitting the appropriate `add_node` + `wire_pins` sequence into
  `c.ops`. Non-trivial: e.g. lowering `for_each` to a `ForEachLoop`
  macro instance requires the macro-instance plugin op + correct
  exec/data pin wiring. Phase-2-blocking because at least `return`,
  `cast`, and `for_each` are common in TPC functions.

### 2.3 `[blocking-Phase-2]` `apply_ops` op vocabulary doesn't cover Stage 2-4 mutations

- **File:** `D:/Projects/UE5_MCP/Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/tools/ApplyOps.cpp:439-498`
  (the `RunOp` dispatch table).
- **Symptom:** the dispatch covers only `add_variable, delete_variable,
  rename_variable, retype_variable, set_variable_default,
  set_variable_category, add_function, add_function_input,
  add_function_output, delete_function, add_node, wire_pins,
  set_node_position, delete_node` (per the error message at line
  488-498).
- **Missing:** every Stage 1+ write op that exists as a top-level MCP
  tool. Cannot batch `add_component`, `attach_component`,
  `remove_component`, `set_component_property`, `add_widget`,
  `set_widget_property`, `bind_widget_event`, etc. inside an
  `apply_ops` block.
- **Impact:** Phase 2 path A's `SpecToBP` orchestrator
  (spec §2.1) is designed to order `CreateBlueprint → AddVariable →
  AddComponent (parent-first) → AddFunction → AddNode → ConnectPins`.
  Batching the AddComponent passes saves one editor round-trip per
  component. Without it, every component on TPC's SCS tree is its own
  commandlet call (~12 components in TPC). The roundtrip still
  *works* — SpecToBP can call each tool individually — but its
  effective throughput drops by ~10x and any rollback semantics
  (`atomic: true`) are no longer cross-op.
- **Severity:** soft-blocking. Tagged blocking because spec §3.2's
  iteration loop becomes prohibitively slow without it on the full
  TPC roundtrip; soft because there's a textbook workaround.
- **Fix shape:** add new arms to `RunOp` (line 439+) and
  `ValidateOpShape` (line 531+, the preview validator) for each new
  op kind. Mechanical; each arm is ~10 lines.

### 2.4 `[blocking-Phase-2]` Missing `parse_cpp_blueprint` for class-level C++ → BPIR

- **Reference:** spec line 17 names the tool `parse_cpp_blueprint` in
  the Phase 2 path B description; spec line 65 mentions the
  `parse_cpp_blueprint` tool as part of the BPIR roundtrip flow.
  Grep over `D:/Projects/UE5_MCP/Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/tools`
  shows no occurrences of `parse_cpp_blueprint` or `ParseCppClass`.
- **What exists:** only `parse_cpp_function`
  (`BlueprintTools.cpp:513-563`), which calls
  `ParseCppFunction(source)` (`CppParse.h:68`). The result is a
  `kind:"function"` BPIR doc.
- **Impact:** Phase 2 path B's plan is `decompile_blueprint` →
  CppEmit → UBT build → `parse_cpp_blueprint` → `transpile_blueprint`.
  Without `parse_cpp_blueprint`, the agent can't round-trip a whole
  class; it would have to call `parse_cpp_function` per-function +
  hand-assemble a `kind:"class"` doc (variables, interfaces, parent
  class). Doable but brittle.
- **Fix shape:** add `ParseCppClass` to `CppParse.h/.cpp` that walks a
  full `.h/.cpp` pair (or a concatenation), identifies the UCLASS
  block, parses each UPROPERTY decl into BPIR `variables`, parses
  each UFUNCTION body via the existing `ParseCppFunction`, assembles
  a `{kind:"class", ...}` doc that `ValidateBpir`'s class arm
  (`Bpir.cpp:474-482`) already accepts. Register a `parse_cpp_blueprint`
  tool in `BlueprintTools.cpp`. Bump tool count assertions.

### 2.5 `[blocking-Phase-2]` Missing `bp_structural_diff` tool + `IBlueprintReader::StructuralDiff`

- **Reference:** spec §2.3 (`EOp::StructuralDiff`) and §2.4
  (`bp_structural_diff` MCP tool). Grep confirms neither exists today.
- **Impact:** Goal #5 of the spec ("Structural diff: new MCP tool
  `bp_structural_diff` shows empty diff…") and goal #3/#4 (roundtrip
  acceptance criterion is "diff is empty") cannot be measured.
- **Fix shape:** the spec itself documents the implementation in §2.3
  (`BlueprintStructuralDiff::Compare` lives plugin-side because it
  needs `UBlueprint*` / `USCS_Node` / `UEdGraphNode` reflection) and
  §2.4 (MCP tool wires `IBlueprintReader::StructuralDiff` across
  Mock/Commandlet/Live/Auto/ReadOnly/Caching). This is a planned
  Phase-2 add, not a leak — flagging here so the Phase-2 builder
  knows the seam touches all four backend implementations + the
  plugin-side EOp.

### 2.6 `[non-blocking]` Decompile-emitted BPIR sub-forms use synthetic `__bpr_*` call markers that CppEmit doesn't normalize

- **Files:** `D:/Projects/UE5_MCP/Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/tools/Decompile.cpp:463`
  (`__bpr_select_ternary`), :478 (`__bpr_select_n`), :528
  (`__bpr_get_class_defaults`), :563 (`__bpr_format_text`), :1058
  (`__bpr_spawn_actor_from_class`), :1106
  (`__bpr_get_data_table_row`), :1136
  (`__bpr_construct_object_from_class`), :1298
  (`__bpr_async_factory`), :1417 (`__bpr_async_bind`), :1430
  (`__bpr_async_activate`), :2061 (`__bpr_destroy_actor`).
- **Symptom:** Decompile pattern-matches K2 nodes like
  `K2Node_SpawnActorFromClass` and emits a synthetic
  `{call: "__bpr_spawn_actor_from_class", args: {...}}` instead of
  the canonical `UGameplayStatics::BeginDeferredActorSpawnFromClass`
  pair. CppEmit's `EmitCallExpr` (around `CppEmit.cpp:652+`) doesn't
  know these markers — it'll emit them verbatim as a C++ identifier,
  producing a function call to a non-existent symbol.
- **Impact:** the BPIR pivot is supposed to be language-neutral, but
  these `__bpr_*` names are an undocumented coupling between
  Decompile and (would-be) future lowerers. CppEmit silently emits
  uncompilable code; CompileFunction (which only knows
  `if/set/call/comment`) would emit a CallFunction node with the
  marker name as the function name and fail at compile-time inside
  the editor.
- **Why non-blocking:** the spec's Phase-2 acceptance is roundtrip
  identity. A TPC that decompiles to `__bpr_spawn_actor_from_class`
  and re-compiles via the BPIR pipeline would produce a broken BP,
  but the goal-#4 acceptance criterion `differences = []` would
  catch it. We can document this as "the BPIR roundtrip currently
  handles only the subset of K2 patterns that don't decompile to a
  `__bpr_*` marker"; Phase-2 work would either skip TPC sections
  using these nodes or expand `UnsupportedTreatment.h` to give them
  a real classification.
- **Fix shape:** populate `UnsupportedTreatment.cpp` (the
  `ClassifyUnsupported` switch — file referenced at
  `UnsupportedTreatment.h:71`) with a real C++ snippet for each
  `__bpr_*` form, OR refactor Decompile to emit the canonical
  GameplayStatics / KismetSystemLibrary call directly. The latter is
  more invasive; the former is local to `UnsupportedTreatment.cpp`.

### 2.7 `[non-blocking]` `summarize_blueprint` and `decompile_blueprint` registered without mock-distinguishing tests

- **Files:** `BlueprintTools.cpp:565-597` (`summarize_blueprint`,
  reads via `ReadBlueprint` which the mock backend implements) and
  `BlueprintTools.cpp:247-273` (`decompile_blueprint`, reads via
  `ReadBlueprint` + `GetFunction` per function — also mock-friendly).
- **Symptom:** these tools work against the mock backend because they
  only call read methods that Mock implements. No leak per se; flagged
  to confirm there's no hidden dependency on commandlet state.

### 2.8 `[non-blocking]` Many `IBlueprintReader` methods have throwing defaults but no mock override

The mock backend (`MockBlueprintReader.cpp`) overrides 7 read methods
+ 21 write methods + `FindNode` = ~29 of the IBlueprintReader
methods. Every other method (76+ from the Stage 2-4 family) inherits
the IBlueprintReader default that throws
`"X not supported by this backend"`. This is the documented
"mock-as-read-only" pattern (mock backend rejects writes; rejection
extends to anything not in the original BP-write surface). Listed for
completeness:

- Stage 2-4 methods (project, data-table, data-asset, material,
  widget, behavior-tree, state-tree, niagara, sequencer, gameplay-tag,
  anim-bp, profile, cook, class-info, viewport, live-editor): the
  mock raises "not supported by this backend" via the base-class
  default (e.g. `IBlueprintReader.h:187-189` for
  `GetProjectMetadata`).
- A mock-driven test of any of these tools surfaces the throw as the
  MCP tool error envelope.
- **Why non-blocking:** mock is intentionally a tiny surface; the
  CLAUDE.md "Adding a new tool" recipe explicitly says
  "MockBlueprintReader: throw `BlueprintReaderError('...mock backend
  is read-only...')`" — relying on the throw-default has the same
  user-facing effect. Phase 2 doesn't run mock-against-Stage-2+ paths;
  the granular-write tests use live/commandlet.
- **Possible cleanup:** override `IBlueprintReader::GetProjectMetadata`
  in `MockBlueprintReader.cpp` to return a synthetic
  `ProjectMetadata` (or to throw with a distinct error message
  pointing at "mock backend") so the failure surface is consistent
  with the write-tool rejection convention. Same for the other Stage
  2-4 read methods. Mechanical; ~50 lines.

### 2.9 `[non-blocking]` `BeginBatch`/`EndBatch` semantics undefined for Mock/Live

- **Files:** `IBlueprintReader.h:1277-1280` (defaults are no-op +
  `{}`), `CommandletBlueprintReader.cpp` has real impls,
  `MockBlueprintReader.cpp` does NOT override (uses defaults),
  `SocketBlueprintReader.cpp:2439-2451` has real impls,
  `CachingBlueprintReader.cpp` overrides (forwards + tracks depth).
- **Symptom:** calling `apply_ops` against the mock backend with
  write ops will throw at the first write (mock rejects), but the
  caller's `BeginBatch` was a no-op so there's no batch state to
  tear down — silently fine. In live mode, `BeginBatch`/`EndBatch`
  send the batch sentinel ops to the editor over TCP; failure modes
  documented at `SocketBlueprintReader.cpp:2439+`.
- **Why non-blocking:** the actual contract (CLAUDE.md "Common
  gotchas" section) is "Best-effort failure semantics" — the caller
  knows batches can be partial. Worth documenting in Phase-2 SpecToBP
  to explicitly call `EndBatch(skipCompile=true)` on the catch path.

### 2.10 `[non-blocking]` `WriteGeneratedSource` is a backend method but mock rejects — limits Phase 2 path B fixture generation

- **Files:** `IBlueprintReader.h:163-169`, mock impl
  `MockBlueprintReader.cpp:293-297` (throws).
- **Symptom:** Phase 2 path B's BPIRRoundtrip step #2 ("emit `.cpp`
  + `.h` under `…/BPRoundtripModule/Private/`") calls
  `WriteGeneratedSource`. The mock backend rejects, so mock-mode
  tests can't exercise the C++-emit-then-disk-write step. They can
  exercise CppEmit's in-memory output (the tool `transpile_blueprint`
  returns header/impl source strings directly — see
  `BlueprintTools.cpp:436-449`); only the disk-write step is gated to
  commandlet/live.
- **Why non-blocking:** path B's spec already routes through UBT,
  which requires the real `Source/` tree anyway. Mock-mode tests can
  verify `transpile_blueprint`'s output shape without ever touching
  disk.

### Leak count summary

- **Blocking Phase-2:** 5 (2.1, 2.2, 2.3, 2.4, 2.5)
- **Non-blocking:** 5 (2.6, 2.7, 2.8, 2.9, 2.10)

---

## 3. Extension cookbooks

Each is a sketch only — file locations + line ranges for the
prototype, not full code.

### 3.1 Add a new Lua backend

Suppose the new backend talks to a Lua-script-based introspector
over a Unix socket or in-process. The shape is identical to the
existing SocketBlueprintReader.

1. **Subclass `IBlueprintReader`** in a new
   `D:/Projects/UE5_MCP/Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/backends/LuaBlueprintReader.{h,cpp}`.
   Use `SocketBlueprintReader.h:44-330` as the template — same
   structure: `Config` struct, ctor, destructor, lazy connect,
   `RunOp` helper, per-method override.
2. **Decide which methods to override.** The minimum is the seven
   pure-virtuals on `IBlueprintReader.h:37-79` (compile-failure
   otherwise). For everything else, decide per-method whether the
   Lua side supports it; methods you don't override inherit the
   throw-default — see leak 2.1 for why that's a silent UX failure.
3. **Register in BackendFactory.** Add an `if (cfg.backend == "lua")`
   arm to `BackendFactory.cpp:218-275`, mirroring the
   `cfg.backend == "live"` arm (lines 238-252). Extend `BackendConfig`
   in `BackendFactory.h:15-64` with any Lua-specific knobs
   (interpreter path, script directory). Document new
   `BP_READER_LUA_*` env vars in `ConfigFromEnv`.
4. **Tests.** Add a mock-shaped test that constructs the backend
   with a fake interpreter (no real Lua process) and exercises the
   error paths.
5. **End-to-end test gating.** New file under
   `D:/Projects/UE5_MCP/Plugins/BlueprintReader/Tests/BlueprintReaderMcpTests/Private/`,
   guarded by an env var (`BP_READER_LUA_INTERPRETER`) like
   the existing 12 live-gated cases that auto-skip when env vars
   aren't set (see `test_tools.cpp:33-40` for the count and the
   CLAUDE.md "Live" test section for the env-gating pattern).
6. **Wrapper compatibility.** Confirm `CachingBlueprintReader` and
   `ReadOnlyBlueprintReader` still work when wrapping the new
   backend — they walk the interface contract, so they should be
   indifferent to the inner. No code changes needed.

Concrete line anchors for the pattern:

- IBlueprintReader subclass: `SocketBlueprintReader.h:44-66` is the
  ctor shape; lines 71-330 are the per-method override block.
- Factory registration: `BackendFactory.cpp:238-252` (the live arm).
- BackendConfig extension: `BackendFactory.h:57-64` (the live
  fields).

### 3.2 Add a new BPIR statement kind (`assert`)

Suppose you want to add a `{assert: <expr>, [message: "..."]}`
statement form. End-to-end ask: it should be producible from
`Decompile` (when matching `K2Node_DevelopmentOnlyAssert`), consumable
by `CppEmit` (lowering to `check(x);`), validatable by `ValidateBpir`,
and runnable through `CompileFunction` (when the agent writes one
in DSL).

1. **Add the form to the vocabulary.**
   `D:/Projects/UE5_MCP/Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/tools/Bpir.cpp:49-60`
   — append `"assert"` to the `StatementForms()` vector. Pattern:
   one-line append.
2. **Add a validator arm.** `Bpir.cpp` near line 217 (the
   `ValidateStatement` switch is a sequence of `if (form == ...)` arms
   like the existing `if (form == "return")` arm — search the file).
   The arm checks: `assert` field must be an object (re-using
   `ValidateExpression`), optional `message` must be a string.
3. **Add a CppEmit lowerer.**
   `D:/Projects/UE5_MCP/Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/tools/codegen/CppEmit.cpp:1157-1541`
   (the `EmitStatement` body). Add an arm:
   ```cpp
   if (form == "assert") {
       std::string cond = EmitExpr(s["assert"]);
       Line(fmt::format("check({});", cond));
       return;
   }
   ```
   Use the existing arm for `if` (lines 1163-1180) as the prototype.
4. **Add a Decompile recognizer.**
   `D:/Projects/UE5_MCP/Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/tools/Decompile.cpp`
   near line 939 (the `K2Node_IfThenElse` arm is the prototype).
   Match on `n.Class.find("K2Node_DevelopmentOnlyAssert") !=
   std::string::npos`, build the BPIR statement, set
   `r.statement = {{"assert", condExpr}}`, return.
5. **Add a CompileFunction (BPIR → BP) handler.**
   `D:/Projects/UE5_MCP/Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/tools/CompileFunction.cpp:282-371`.
   Add a new arm before the catch-all throw on line 369:
   ```cpp
   if (stmt.contains("assert")) {
       std::string condSlot = CompileExpr(c, stmt["assert"]);
       std::string assertSlot = c.AddNode("CallFunction", "assertfn",
           {{"function", "Assert"},
            {"function_owner", "KismetSystemLibrary"}});
       EmitValueConnect(c, condSlot, "ReturnValue", assertSlot, "Condition");
       WireTailsTo(c, prevs, assertSlot, "execute");
       return {{assertSlot, "then"}};
   }
   ```
   (Use the `set` arm at lines 320-336 as the prototype for the
   AddNode + EmitValueConnect + WireTailsTo pattern.)
6. **Optional: CppParse arm.** If users will write `assert(x)` in C++
   source for `parse_cpp_function`, add a recognizer to
   `CppParse.cpp` — but C++ doesn't have a first-class `assert`
   statement form (it's a macro), so this is reasonable to skip; the
   parser would naturally lex it as a call expression and the
   `set/call` path would handle it as a void call.
7. **Tests.** Add a doctest case under
   `D:/Projects/UE5_MCP/Plugins/BlueprintReader/Tests/BlueprintReaderMcpTests/Private/`
   exercising the round-trip: validate an `assert` doc through
   `ValidateBpir`, emit C++ via `EmitCppFunction`, check the output
   contains `"check(...)"`. Mock-only.
8. **Tool-count assertions** at `test_tools.cpp:36` and
   `test_mcp.cpp:94` do NOT change — no new tool was added, only a
   new statement form inside existing tools.

Concrete line anchors:

- Vocabulary: `Bpir.cpp:49-60`.
- Validator arm prototype: `Bpir.cpp:217+` (search for `if (form == "return")`).
- CppEmit arm prototype: `CppEmit.cpp:1163-1180` (the `if` arm).
- Decompile recognizer prototype: `Decompile.cpp:939-985` (the
  `K2Node_IfThenElse` arm).
- CompileFunction arm prototype: `CompileFunction.cpp:320-336` (the
  `set` arm — uses `AddNode` + `EmitValueConnect` + `WireTailsTo`).

### 3.3 Add a new MCP tool that reads project state (`get_engine_version`)

Suppose you want a tool that returns the project's engine version
(today's `get_project_metadata` returns `engineAssociation` but you
want a richer normalized view including the resolved engine major /
minor / patch). The tool reads project state, doesn't mutate
anything.

1. **Add the backend method.**
   `D:/Projects/UE5_MCP/Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/backends/IBlueprintReader.h`.
   The `ProjectMetadata` family is at lines 179-189 — that's the
   pattern. Decide: pure-virtual or throwing-default? For a read
   method that not every backend can satisfy (mock can't), use a
   throwing default like `GetProjectMetadata` does at 187-189:
   ```cpp
   struct EngineVersion {
       std::string association;   // raw from .uproject
       int major = 0, minor = 0, patch = 0;
       std::string changelist;
   };
   virtual EngineVersion GetEngineVersion() {
       throw BlueprintReaderError("GetEngineVersion not supported by this backend");
   }
   ```
2. **Implement on CommandletBlueprintReader.**
   `D:/Projects/UE5_MCP/Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/backends/CommandletBlueprintReader.cpp:1521-1553`
   (the existing `GetProjectMetadata` impl) is the prototype.
   Serialize args to `-Op=GetEngineVersion`, parse the JSON result.
3. **Implement on SocketBlueprintReader.** Same pattern; the
   `RunOp(args)` helper handles transport. The existing
   `GetProjectMetadata` arm in `SocketBlueprintReader.cpp` is the
   prototype.
4. **Add AutoBlueprintReader forwarder.** **CRITICAL** — see leak
   2.1. `AutoBlueprintReader.h` and `.cpp` both need new entries.
   Without this, even with the commandlet impl, calling the tool
   from the default `auto` backend will throw the inherited
   IBlueprintReader default. Use the existing
   `FORWARD(ReadBlueprint, a)` pattern at
   `AutoBlueprintReader.cpp:356-358` as the template.
5. **Add CachingBlueprintReader forwarder.** Same pattern;
   `CachingBlueprintReader.cpp` has an existing pass-through arm for
   `GetProjectMetadata` (search file). Likely needs no caching (per
   the design note at `CachingBlueprintReader.h:115-119`).
6. **Add ReadOnlyBlueprintReader forwarder.** `ReadOnlyBlueprintReader.cpp`
   pattern: read methods just delegate to `inner_->GetEngineVersion()`
   (see lines 28-52 for the prototype with `ListBlueprints` etc.).
7. **Add MockBlueprintReader override.** Optional. Without an
   override, mock-backend tests on the new tool will surface the
   throw-default — see leak 2.8. For consistency with the other
   read methods, add a stub returning a fixture value or throwing
   with a mock-distinct message.
8. **Add the plugin-side EOp.**
   `D:/Projects/UE5_MCP/Plugins/BlueprintReader/Source/BlueprintReaderEditor/Private/BlueprintReaderCommandlet.cpp`.
   Enum entry near line 175 (in the project-ops block), `ParseOp`
   arm near line 308 (`if (OpStr.Equals(TEXT("GetEngineVersion"), …))`),
   dispatch arm, and a `RunGetEngineVersionOp(Params, OutputPath,
   bPretty)` impl that returns a JSON string. See the existing
   `RunGetProjectMetadataOp` in the same file as the prototype.
9. **Register the MCP tool.**
   `D:/Projects/UE5_MCP/Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/tools/BlueprintTools.cpp`.
   Pattern: see `get_project_metadata` at lines 1910-1933. Push a new
   block at an appropriate spot in the file. The handler:
   ```cpp
   ToolDescriptor d;
   d.name = "get_engine_version";
   d.description = "[discover] Return resolved engine version "
       "components for the current project.";
   d.input_schema = {{"type","object"}, {"properties", json::object()}};
   registry.Add(std::move(d), [&reader](const json& args) {
       (void)args;
       auto v = reader.GetEngineVersion();
       return json{
           {"ok", true},
           {"association", v.association},
           {"major", v.major}, {"minor", v.minor}, {"patch", v.patch},
           {"changelist", v.changelist}};
   });
   ```
10. **Add to a category.**
    `D:/Projects/UE5_MCP/Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/tools/ToolCategories.cpp:60+`
    — add `"get_engine_version"` to the `discover` or `assets`
    category so it shows up under the env-var filter and the
    progressive-disclosure meta-tool.
11. **Add a mock fixture.** Optional; the mock fixture loader at
    `MockBlueprintReader.cpp:86-128` reads `BP_*.json` files. A
    project-level fixture would need a new fixture-loader path; for
    a one-method read tool, easier to override
    `MockBlueprintReader::GetEngineVersion` to return a hard-coded
    `EngineVersion` value.
12. **Bump tool-count assertions.**
    `D:/Projects/UE5_MCP/Plugins/BlueprintReader/Tests/BlueprintReaderMcpTests/Private/test_tools.cpp:36`
    and `test_mcp.cpp:94`: both `CHECK(spec.size() == 127)` become
    `== 127`.
13. **Add tests.**
    - Mock case: `Fixture f; auto out = f.Call("get_engine_version", json::object());` — assert the throw or the stub return.
    - Live-gated case under the env-var pattern (see
      `test_tools.cpp:33-40` and the live env section in CLAUDE.md).

Concrete line anchors:

- Backend method shape (throwing-default): `IBlueprintReader.h:187-189`
  (`GetProjectMetadata`).
- Commandlet impl: `CommandletBlueprintReader.cpp:1521-1553`.
- Auto forwarder: `AutoBlueprintReader.cpp:356-358` and
  `AutoBlueprintReader.h:88` for the declaration. **NOTE:** for the
  full 76-method gap in leak 2.1, the analogous existing forwarder
  is the `GetProjectMetadata` line you'd add right above the
  `BeginBatch` declaration at `AutoBlueprintReader.h:137`.
- ReadOnly delegation: `ReadOnlyBlueprintReader.cpp:28-52`.
- Caching pass-through: search `CachingBlueprintReader.cpp` for the
  existing `GetProjectMetadata` arm.
- Plugin EOp prototype: `BlueprintReaderCommandlet.cpp:158-160` for
  enum, :300-301 for parser, search the file for the dispatch arm
  pattern.
- MCP tool registration prototype: `BlueprintTools.cpp:1910-1933`
  (`get_project_metadata`).
- Tool category prototype: `ToolCategories.cpp:60` (`apply_ops`
  appears in the `core` category — same pattern).
- Test count locations: `test_tools.cpp:36`, `test_mcp.cpp:94`.

---

## 4. Cross-references quick index

| Pattern | File:line |
|---|---|
| IBlueprintReader pure-virtual block | `IBlueprintReader.h:37-79` |
| IBlueprintReader throwing-default prototype | `IBlueprintReader.h:187-189` |
| BackendFactory dispatch | `BackendFactory.cpp:218-288` |
| Mock backend write-rejection prototype | `MockBlueprintReader.cpp:191-196` |
| Commandlet method prototype | `CommandletBlueprintReader.cpp:1521-1553` (GetProjectMetadata) |
| Socket method prototype | `SocketBlueprintReader.h:71-330` |
| Auto forwarder macros | `AutoBlueprintReader.cpp:341-345` |
| Auto FORWARD arm prototype | `AutoBlueprintReader.cpp:356-358` |
| ReadOnly delegation prototype | `ReadOnlyBlueprintReader.cpp:28-52` |
| ReadOnly rejection prototype | `ReadOnlyBlueprintReader.cpp:12-19, 55-77` |
| Caching pass-through prototype | `CachingBlueprintReader.h:115-258` |
| ToolRegistry Add semantics | `ToolRegistry.cpp:9-28` |
| MCP tool registration prototype (read) | `BlueprintTools.cpp:151-180` (list_blueprints) |
| MCP tool registration prototype (write) | `BlueprintTools.cpp:786-844` (add_variable) |
| MCP tool with kind-extras | `BlueprintTools.cpp:896-1010` (add_node) |
| Tool count assertions | `test_tools.cpp:36`, `test_mcp.cpp:94` |
| BPIR vocab additions | `Bpir.cpp:49-60` (statements), :62-69 (expressions) |
| Validator arm prototype | `Bpir.cpp:215+` (search "ValidateStatement") |
| CppEmit statement dispatch | `CppEmit.cpp:1157-1541` |
| CppEmit expression dispatch | `CppEmit.cpp:520-720` |
| Decompile statement recognizer prototype | `Decompile.cpp:939-985` (K2Node_IfThenElse) |
| Decompile expression recognizer prototype | `Decompile.cpp:351-475` |
| CompileFunction statement dispatch | `CompileFunction.cpp:282-372` |
| CppParse statement parser prototype | `CppParse.cpp:513-640` (if/switch/while/for_each) |
| ApplyOps dispatch table | `ApplyOps.cpp:439-498` |
| Plugin EOp enum | `BlueprintReaderCommandlet.cpp:132-271` |
| Plugin ParseOp arm | `BlueprintReaderCommandlet.cpp:273-382` |
| Plugin RunOneOp legacy dispatch | `BlueprintReaderCommandlet.cpp:6391-6459` |
| ToolCategories list | `ToolCategories.cpp:60+` |
| Tool-filter API | `ToolRegistry.cpp:96-167` |
| Progressive-disclosure meta-tool | `BlueprintTools.cpp:4415-4463` |
| Unsupported-treatment classifier | `UnsupportedTreatment.h:67-81` |
| Sidecar builder | `UnsupportedTreatment.h:78-80` |
| Async-task BPIR markers (leak 2.6) | `Decompile.cpp:1298, 1417, 1430` (`__bpr_async_*`) |
