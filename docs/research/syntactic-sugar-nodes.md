# Syntactic-sugar K2 nodes and their idiomatic C++ lowerings

Research note informing the bp-reader server's `transpile_function` and
`decompile_function` pipeline. Audience: contributors extending
`CppEmit.cpp` / `Decompile.cpp` to cover more BP shapes with idiomatic
output. Companion to
[`docs/design/10-bp-to-cpp-node-coverage.md`](../design/10-bp-to-cpp-node-coverage.md),
which tracks what is actually shipped.

---

## 1. Definition

A K2 node is **syntactic sugar** if the Blueprint compiler expands it
into a graph of simpler primitives during the
`UEdGraphSchema_K2::ExpandNode` / `ExpandFunctionCall` pass of compile,
so the running VM (or transpiler) never sees the sugar form — only the
desugared primitives. The opposite is an **opaque** node that holds
runtime behaviour the VM dispatches directly (e.g. `K2Node_Timeline`,
`K2Node_BaseAsyncTask`'s factory call, `K2Node_CallFunction` for a
non-promotable UFUNCTION). Sugar nodes exist purely for authoring
ergonomics: `K2Node_IfThenElse` is sugar for the conditional branch
opcode, `K2Node_MakeArray` is sugar for `Array_Add` calls against a new
TArray, `K2Node_MacroInstance(ForEachLoop)` is sugar for an indexed
`WhileLoop` over `Array.Length()`. For a C++ transpiler the practical
distinction is: sugar nodes have a clean C++ language construct
(`if`/`switch`/`for`/initializer-list); opaque nodes need either a
sentinel-rendered call, an auto-synthesised helper, or a structured
`unsupported` TODO.

The bp-reader pipeline lowers sugar in **two** places:

1. **`Decompile.cpp`** walks the K2 graph and emits a versioned BPIR
   form ({if}, {switch}, {sequence}, {for_each}, {new_array}, ...) for
   each recognised sugar class. Unrecognised classes fall through to
   `{unsupported: {node_class, guid, ...}}`.
2. **`CppEmit.cpp`** dispatches each BPIR form to a C++ render
   (`EmitStatement` / `EmitExpr`). Sentinels like `__bpr_format_text`
   and `__bpr_select_n` cross both halves.

Coverage gaps in either half show up the same way in the generated
output: a `// TODO[bpr-unsupported]: K2Node_<X>` line plus a sidecar
note. Closing a gap is usually a two-side change.

**Why "syntactic" is the right framing.** The post-compile K2 graph
the introspector reads has already had sugar expanded for the BP VM —
a `ForEachLoop` macro instance is, at the bytecode level, a
`WhileLoop` plus an integer counter plus an `Array_Get`. The
transpiler operates against the **source** K2 graph, not the
post-compile form, precisely because the sugar carries the author's
intent. Re-deriving a `for-each` range-for from the expanded
`while`+counter shape is theoretically possible but loses information
(was that an enumerated loop, or a counter the author wrote
manually?). Working at the K2 level preserves the original idiom.

**Two kinds of sugar shape.** Inspecting the dispatcher logic in
`Decompile.cpp` reveals two distinct lowering shapes:

- **Direct BPIR forms** — control-flow and container nodes
  (`K2Node_IfThenElse`, `K2Node_Switch`, `K2Node_MakeArray`,
  `K2Node_ExecutionSequence`, ...) produce native BPIR statement /
  expression forms that CppEmit renders as C++ language constructs.
  Examples: `{if, then, else}` → `if () { } else { }`; `{new_array}`
  → `TArray<T>{...}`; `{switch, cases}` → C++ `switch`.
- **Sentinel calls** — nodes whose lowering needs more than a single
  language construct emit a `__bpr_<name>` sentinel call that CppEmit
  matches by name. Examples: `__bpr_format_text` for the
  `FFormatNamedArguments` boilerplate; `__bpr_select_n` for chained
  ternaries; `__bpr_async_factory` / `__bpr_async_bind` /
  `__bpr_async_activate` for the latent-async triplet. Sentinels are
  cheap to add (one entry in each half) and let CppEmit hide multi-line
  C++ behind a single expression slot.

---

## 2. Inventory table

Every row is **sugar** (lowers cleanly to C++). Status uses the legend
from `docs/design/10-bp-to-cpp-node-coverage.md`:
✅ supported · 🔄 passthrough · ⚠️ approximation · ❌ unsupported.

Line numbers reference
[`Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/tools/codegen/CppEmit.cpp`](../../Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/tools/codegen/CppEmit.cpp)
unless otherwise stated. `Decompile` numbers reference
[`Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/tools/Decompile.cpp`](../../Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/tools/Decompile.cpp).

| K2 class | Desugars to | Recommended C++ | bp-reader status | Anti-pattern to avoid |
|---|---|---|---|---|
| `K2Node_IfThenElse` | Two-target conditional branch on the `Condition` bool pin. | `if (<cond>) { <then> } else { <else> }` | ✅ Decompile `Decompile.cpp:939` emits `{if, then, else}`; CppEmit `CppEmit.cpp:1163` renders `if (..) { .. } else { .. }`. See [10-bp-to-cpp-node-coverage.md §Flow control](../design/10-bp-to-cpp-node-coverage.md#flow-control). | Do not generate `UKismetSystemLibrary::SwitchHasAuthority`-style runtime helpers; the language `if` is the canonical form. |
| `K2Node_ExecutionSequence` | Ordered exec dispatch — n exec outputs run in sequence in the same stack frame. | Inlined ordered C++ statements; no marker comments. | ✅ Decompile `Decompile.cpp:1026` emits `{sequence: [[..], [..]]}`; CppEmit `CppEmit.cpp:1427` inlines each branch. Empty branches dropped. | Do not emit `// sequence branch N` marker comments — they group statements visually without semantic value and were removed in #103. Do not lower to nested lambdas; flat statements match BP semantics exactly. |
| `K2Node_MultiGate` | Increments a hidden int32 cursor, dispatches to the next output exec; supports random / loop / wraparound modes via meta. | `int32 Idx = MultiGate_<tag>_Cursor++ % N; switch (Idx) { case 0: ... }` plus a synth `int32` member, plus an optional reset hook. | ❌ Decompile `Decompile.cpp:1182` emits a structured `{unsupported}` carrying `gate_count` + `fields` so CppEmit's `ClassifyUnsupported` renders a sidecar TODO. No auto-lowering yet. Closing this is a near-clone of the DoOnce/FlipFlop/DoN pattern in #150's lineage. | Do not lower to a single-shot `if (Cursor == 0)` — Multi-Gate's whole point is sequencing across calls. Do not call `UKismetSystemLibrary::SetTimer` per gate; the dispatch is synchronous. |
| `K2Node_Switch` (abstract base) | Selector pin + per-case exec outputs + optional `Default` exec output. Subclasses below specialise the selector type. | `switch (<sel>) { case X: ...; break; default: ...; break; }` for integral/enum/name; chained `if` for string. | ✅ Decompile `Decompile.cpp:2083` emits `{switch, cases, default}`; CppEmit `CppEmit.cpp:1380` renders C++ `switch`. | Do not emit a switch over a `FString` — the C++ language doesn't allow it. SwitchString must lower to chained `if (Sel == TEXT("X"))` (currently rendered as C++ `switch` which compiles only because case labels are quoted FStrings — see "Coverage gaps" below). |
| `K2Node_SwitchInteger` | Integer selector; cases are integer pin labels. | `switch (<i>) { case 0: ...; break; }` | ✅ Same dispatch as base. Case labels are the BP output-pin names, which are already numeric strings. Renders cleanly. | Do not emit a chained `if/else` chain for integral switches — `switch` is the canonical form and compiles to a jump table when dense. |
| `K2Node_SwitchString` | Per-case string label, no compile-time table. | Chained `if (S == TEXT("A")) { .. } else if (S == TEXT("B")) { .. } else { .. }`. | ⚠️ Renders as C++ `switch` with quoted FString case labels, which **does not compile** in C++ (case labels must be integral constants). Need a custom `string_switch` BPIR form. | Do not emit `switch (S)` over an FString — the C++ language rejects it. Chained `if` (or a `TMap<FString, std::function<void()>>` for dense N) is the only legal form. |
| `K2Node_SwitchEnum` | Enum selector; cases are enum value pin labels. | `switch (<e>) { case EEnumType::A: ...; break; }` | ✅ Same dispatch as base. Case labels are the enum value names; CppEmit emits them verbatim. The enum's `EEnumType::` qualifier comes from the selector expression's resolved type. | Do not lower to `static_cast<uint8>(e)` + integer switch — loses readability and the enum's strong-typing. |
| `K2Node_SwitchName` | FName selector, FName case labels. | `switch (<n>)` is legal because FName has `operator==` + a constexpr GetTypeHash, but C++ idiom is chained `if (N == TEXT("X"))`. | ⚠️ Renders as C++ `switch` over FName, which compiles only if the toolchain treats FName as integral. Safer to render as chained `if`. | Same as SwitchString. Switches over `FName` work in some compilers but are not portable; chained `if` is the canonical form. |
| `K2Node_SwitchClass` (theoretical) | Per-case `UClass*` label. | Chained `Cast<X>(Obj)` checks. | n/a — not present in UE 5.7 BlueprintGraph. The closest user-visible analogue is K2Node_DynamicCast in a sequence, or a `UKismetSystemLibrary::SwitchOnClass` macro pattern. If a project ships a custom subclass of `K2Node_Switch` keyed on UClass, the dispatcher at `Decompile.cpp:2083` will substring-match it and produce a `{switch}` that CppEmit cannot legally render. | Do not lower a class switch to `switch (Class)` — UClass* is a pointer; `switch` over pointer is illegal. Use `if (Class == X::StaticClass())` or a sequence of `Cast<X>`. |
| `K2Node_DynamicCast` (statement form) | Tries `Cast<T>(Obj)`; routes to `then` if non-null, `else` (CastFailed) otherwise. | `if (auto* As<T> = Cast<T>(Obj)) { <then> } else { <fail> }` | ✅ Decompile `Decompile.cpp:986` emits `{cast, to, as, success, fail}`; CppEmit `CppEmit.cpp:1358` renders the typed `if (auto* AsX = Cast<X>(..))` block. Path-strip on `to` from #103/#104 prevents `/Game/...` asset paths leaking into the template argument. | Do not call `Object->IsA(X::StaticClass())` then `static_cast<X*>(Object)` — `Cast<>` does both in one operation and is the canonical UE idiom. |
| `K2Node_DynamicCast` (expression form) | Same as statement form but used in a data-pin slot. | `Cast<T>(Obj)` | ✅ CppEmit `CppEmit.cpp:534` renders `Cast<X>(inner)`. | Do not unwrap and re-wrap with `IsValid` for cast-success checks; the `auto*` + truth-check pattern handles both. |
| `K2Node_ClassDynamicCast` | Cast on a UClass* (not an instance). | `Cast<TSubclassOf<T>>(SomeClass)` or `SomeClass->IsChildOf<T>()`. | ⚠️ Treated as a DynamicCast variant by the same dispatcher; renders as `Cast<X>(..)` which compiles for class refs but loses the `TSubclassOf` typing. Manual fixup needed when the result feeds a `UClass*`-typed UFUNCTION. | Do not emit a runtime `static_cast<UClass*>` — `Cast<>` over a class reference uses UE's reflection-aware cast machinery. |
| `K2Node_MacroInstance` (`StandardMacros:ForEachLoop`) | Hidden int32 counter + `while (i < Array.Num())` + per-iteration `ArrayElement = Array[i]`. | `for (auto& Element : <Array>) { <body> }` | ✅ Decompile `Decompile.cpp:1562` recognises the macro, emits `{for_each, in, body}`; CppEmit `CppEmit.cpp:1405` renders the range-for. See [10-bp-to-cpp-node-coverage.md §Loops](../design/10-bp-to-cpp-node-coverage.md#loops-via-macros). | Do not emit `for (int32 i = 0; i < Array.Num(); ++i)` + `Array[i]` — the range-for is the modern UE5 idiom. Do not lower to `UKismetArrayLibrary::Array_Get` per element. |
| `K2Node_MacroInstance` (`ForEachLoopWithBreak`) | Same as ForEachLoop plus a `Break` input exec. | Range-for with `break;` honoured inside the body. | ✅ Same dispatcher; `{break}` BPIR form renders as `break;` (CppEmit `CppEmit.cpp:1441`). | Do not lower the break input exec to `return;` — it terminates only the loop, not the function. |
| `K2Node_MacroInstance` (`ReverseForEachLoop`) | int32 counter walking from `Array.Num()-1` down. | `for (int32 i = Array.Num() - 1; i >= 0; --i) { auto& Element = Array[i]; ... }` | ⚠️ Currently lowered to the same forward range-for shape; the `reverse_flag` is stashed on the BPIR but the emitter doesn't yet honour it. Renders working but **semantically wrong** code (iterates forward). | Do not emit `for (auto& E : ReverseRange(Array))` — no such helper exists in UE5; use an index-based reverse loop. |
| `K2Node_MacroInstance` (`WhileLoop`) | `Condition` pin + body exec; per-iteration re-evaluation. | `while (<cond>) { <body> }` | ✅ Decompile emits `{while, body}`; CppEmit `CppEmit.cpp:1419` renders the loop. | Do not lower to `for (;;) { if (!cond) break; ... }` — `while` is the canonical form. |
| `K2Node_MacroInstance` (`Gate`) | Stateful router: Enter exec only fires Exit when the gate is "open"; Open/Close/Toggle inputs flip a hidden bool. | `if (bGate_<tag>_IsOpen) { <body> }` at the Enter call site, plus `bIsOpen = true/false/!bIsOpen;` at the Open/Close/Toggle call sites, plus a synth `bool bGate_<tag>_IsOpen` member. | ❌ Walker visits the macro once per upstream exec without knowing which input pin the visit came from. The dispatcher emits a structured sidecar with the recipe + per-pin code patterns, leaving the agent to apply at each call site. Full auto-lowering requires `DecompileStatementsFrom` to thread entry-pin info. See `f0a412e refactor(walker): thread destination pin into ExecTarget; auto-lower Gate` for the most recent attempt. | Do not lower to a `UKismetSystemLibrary::Delay` chain — Gate is purely synchronous. Do not synthesise a `FCriticalSection`; the BP runtime is single-threaded. |
| `K2Node_MacroInstance` (`DoOnce`) | Hidden bool, body fires on first call until `Reset` flips it back. | `if (!bDoOnce_<tag>_HasFired) { bDoOnce_<tag>_HasFired = true; <body> }` plus synth member. | ✅ Auto-lowered via the `auto-synth member` channel. Sidecar entry for upstream Reset wiring. | Do not lower to `static bool bFired = false` inside the function — that survives across instances of the class (wrong scope). Member variable derived from node GUID is the canonical form. |
| `K2Node_MacroInstance` (`DoN`) | int counter, body fires N times then stops; `Reset` zeroes the counter. | `if (DoN_<tag>_Counter < N) { ++DoN_<tag>_Counter; <body> }` plus synth `int32` member. | ✅ Same auto-synth channel as DoOnce. | Same anti-pattern: `static int32` is class-scope only by accident, not by design. |
| `K2Node_MacroInstance` (`FlipFlop`) | Hidden bool, alternates A / B exec outputs on each call. | `if (bFlipFlop_<tag>_IsA) { bFlipFlop_<tag>_IsA = false; <A> } else { bFlipFlop_<tag>_IsA = true; <B> }` plus synth bool member initialised to `true` so first call routes to A. | ✅ Auto-lowered. | Do not use bitwise XOR (`bFlag ^= 1`) — works but obscures intent; explicit `if / else` toggle reads better and matches BP semantics. |
| `K2Node_MacroInstance` (`IsValid`) | Calls `UKismetSystemLibrary::IsValid` on InputObject and routes to IsValid / IsNotValid exec outputs. | `if (IsValid(X)) { <then> } else { <else> }` | ✅ Decompile `Decompile.cpp:1518` emits `{if, then, else}` with a `{call: "IsValid"}` condition; CppEmit's NameAliases (`CppEmit.cpp:405`) ensures `KismetSystemLibrary::IsValid` is rendered unqualified as `IsValid(..)`. | Do not emit `if (X != nullptr)` — that misses UE's PendingKill check. `IsValid()` is the only correct null-and-pending-kill test. |
| `K2Node_MacroInstance` (`IsValidClass`) | Same as IsValid but for UClass* input. | `if (IsValid(<Class>)) { ... }` | ❌ Falls through to the generic "unrecognised macro" path; ends up as an `{unsupported}` sidecar entry. Should be a one-line addition next to the `IsValid` handler. | Same as IsValid. |
| `K2Node_MacroInstance` (`Select`, when implemented as macro) | See `K2Node_Select` below — the standalone node form is more common. | Ternary / chained ternary. | n/a — most Select usage in shipping BPs is via the standalone `K2Node_Select` node, not the macro variant. |
| `K2Node_MacroInstance` (`ForEachElementInEnum`) | Iterates `0..EnumType::MAX-1`; emits each enum element + its display name. | `for (int32 I = 0; I < (int32)EEnumType::EEnumType_MAX; ++I) { EEnumType Element = (EEnumType)I; ... }` | ❌ Not handled — falls through to unsupported. A near-zero-cost addition next to ForEachLoop. | Do not call `StaticEnum<EEnumType>()->NumEnums()` per iteration — hoist outside the loop. |
| `K2Node_Select` (standalone) | Picks one of N value pins based on `Index` (bool, int, or enum). | Ternary `(<Index> ? <True> : <False>)` for N=2; chained ternaries `(<Index> == 0 ? <O_0> : (<Index> == 1 ? <O_1> : <O_N>))` for N>2. | ✅ Decompile `Decompile.cpp:418` emits `__bpr_select_ternary` or `__bpr_select_n` sentinel; CppEmit `CppEmit.cpp:744` / `:755` renders the ternary form. See [10-bp-to-cpp-node-coverage.md §Flow control](../design/10-bp-to-cpp-node-coverage.md#flow-control). | Do not lower to `switch` for the 2-arm case — ternary reads cleaner and produces equivalent code. Do not call `UKismetMathLibrary::SelectFloat`/`SelectInt` runtime helpers — those exist only because BP doesn't have a ternary; C++ does. |
| `K2Node_Knot` (reroute) | Pure passthrough — exec or data flows from input to output unchanged. | No output; the consumer / successor sees the upstream value or exec target directly. | ✅🔄 Decompile handles both data form (`Decompile.cpp:339`, recurses into the source) and exec form (`Decompile.cpp:2074`, skips and follows `then`). CppEmit never sees a knot. | Do not preserve knots in BPIR as a no-op node — they'd inflate the AST and confuse downstream code. Always passthrough at decompile time. |
| `K2Node_Composite` (collapsed graph) | Internal grouping; the contained graph is expanded in place at compile. | Expanded into the surrounding function body. | ➖ Out of scope — composites are an authoring artifact only and are gone by the time `transpile_function` runs against the compiled-form graph (intermediate Kismet bytecode pass). If a composite appears in a graph snapshot (i.e. against the source K2 graph rather than the post-compile graph), Decompile.cpp does **not** handle it — falls through to `{unsupported}`. | Do not emit a C++ lambda or nested function for a composite — the BP semantics is in-place expansion, not lexical scoping. |
| `K2Node_Tunnel` | Entry/exit nodes of a collapsed or macro graph. | Passthrough — the consumer sees the upstream value via the matching tunnel pin. | 🔄 Decompile `Decompile.cpp:404` recurses into the tunnel's input pin (data form); exec form is not currently handled (relies on the walker resolving the tunnel boundary during expand). | Do not emit a `goto` for a tunnel — exec flow is structured, not arbitrary. |
| `K2Node_FormatText` | Walks the format string, builds an `FFormatNamedArguments` map from named pins, calls `FText::Format`. | `FText::Format(NSLOCTEXT("BPR", "FormatText", "..."), [&]{ FFormatNamedArguments Args; Args.Add(TEXT("Name"), Value); return Args; }())` rendered as an immediately-invoked lambda. | ✅ Decompile `Decompile.cpp:541` emits `__bpr_format_text`; CppEmit `CppEmit.cpp:1070` renders the lambda. See [10-bp-to-cpp-node-coverage.md §Special / utility](../design/10-bp-to-cpp-node-coverage.md#special--utility). | Do not lower to `FString::Printf(TEXT("..."), ...)` — `FormatText` is for **localised** text; Printf loses the LOCTEXT key, breaks localisation, and converts FText → FString. Use `FString::Format` only when the BP source explicitly used `BuildString` (different node class). |
| `K2Node_MakeArray` | Heterogeneously-typed brace expansion: creates a `TArray<T>` and appends each input pin. | `TArray<T>{ A, B, C }` braced init — the LHS type drives `T`. | ✅ Decompile `Decompile.cpp:575` emits `{new_array: [...]}`; CppEmit `CppEmit.cpp:553` renders the brace init. See [10-bp-to-cpp-node-coverage.md §Containers](../design/10-bp-to-cpp-node-coverage.md#containers). | **Do not** emit `TArray<T> A; A.Add(B); A.Add(C);` — verbose, breaks one-shot RAII, and loses the brace-init reservation. **Do not** call `UKismetArrayLibrary::Array_Add` in a loop — that's the BP runtime form, not C++. Initialiser-list is canonical. |
| `K2Node_MakeMap` | Pairs of (Key_N, Value_N) input pins; creates a `TMap<K,V>`. | `TMap<K,V>{ {K1, V1}, {K2, V2} }` | ✅ Decompile `Decompile.cpp:601` emits `{new_map: [{key, value}, ...]}`; CppEmit `CppEmit.cpp:585` renders `{{k, v}, {k, v}}`. | **Do not** emit `Map.Add(Key, Value);` per pair — TMap supports brace init in UE 5.x. **Do not** call `UKismetMapLibrary::Map_Add` in a loop. |
| `K2Node_MakeSet` | Like MakeArray but produces a TSet. | `TSet<T>{ A, B, C }` | ✅ Decompile `Decompile.cpp:586` emits `{new_set: [...]}`; CppEmit `CppEmit.cpp:567` renders the brace init. | **Do not** emit `Set.Add(X)` per element. |
| `K2Node_MakeStruct` | Per-field input pins; constructs the struct with field-wise assignment. | `FFoo{ /*Field=*/<expr>, ... }` braced init with comment-tagged designated initialisers (since UE C++ doesn't enable C99 designated initialiser syntax until C++20 in some toolchains). | ✅ Decompile `Decompile.cpp:684` emits `{new_struct, fields}`; CppEmit `CppEmit.cpp:603` renders the brace init with `/*FieldName=*/` comments. | **Do not** emit `FFoo F; F.Field1 = X; F.Field2 = Y;` — loses the const-correctness of brace init. **Do not** call `UKismetMathLibrary::MakeVector` style helpers for trivially-constructible struct primitives — brace init compiles to the same code. |
| `K2Node_MakeContainer` (abstract base) | Polymorphic across MakeArray / MakeSet / MakeMap — picks the right container kind from meta. | Same as the container-specific entries above. | ✅ The container-specific subclasses' handlers cover this; bare `K2Node_MakeContainer` instances are rare in user graphs. | Same as the subclasses. |
| `K2Node_GetArrayItem` | Array indexer (`Array.GetItem(Index)`). | `<Array>[<Index>]` | ✅ Decompile produces `{index: <Array>, idx: <Index>}` via BuildExpression; CppEmit `CppEmit.cpp:547` renders `Array[Index]`. | **Do not** call `UKismetArrayLibrary::Array_Get(Array, Index)` — `operator[]` is the canonical form. **Do not** emit a bounds check unless the BP wires the `bSuccess` output — TArray asserts in debug builds. |
| `K2Node_BreakStruct` | Per-field output pins. | `<Struct>.<Field>` | ✅ Decompile `Decompile.cpp:658` emits `{member, name}`; CppEmit `CppEmit.cpp:544` renders `Struct.Field`. | **Do not** emit `UKismetSystemLibrary::BreakStruct` runtime helper — direct field access compiles to the same offset load. |
| `K2Node_AssignmentStatement` | BP's `<Lhs> = <Rhs>` node. | `<Lhs> = <Rhs>;` | ✅ Decompile `Decompile.cpp:891` emits `{set, to, scope}`; CppEmit `CppEmit.cpp:1181` renders `Lhs = Rhs;` with `this->` shadow handling. | Do not emit `Lhs.SetValue(Rhs)` — direct assignment compiles to the same store. |
| `K2Node_EnumLiteral` | A constant enum value. | `EEnumType::ValueName` | ✅ Decompile `Decompile.cpp:367` emits a `{lit}` carrying the qualified enum value; CppEmit `EmitLit` renders it verbatim. | **Do not** emit `(EEnumType)1` — loses readability and breaks if the enum changes ordering. The qualified-name form is the only stable C++ representation. |
| `K2Node_Literal` | Asset/object reference held in node meta. | Object-ref literal (BP only emits this for object references; primitive literals come from pin defaults). | ✅ Decompile `Decompile.cpp:480` emits `{lit}`. CppEmit renders as `TEXT("...")` for the object-ref string. The agent then wraps with `Cast<X>(StaticLoadObject(...))` or `FSoftObjectPath` as appropriate. | **Do not** call `StaticLoadObject` per use — soft references should be resolved once at construction or via `FSoftObjectPath::TryLoad()`. |
| `K2Node_Self` | The Self pin. | `this` | ✅ CppEmit `CppEmit.cpp:551` renders `this`. | **Do not** emit `Self` as a bare identifier — it's not a C++ keyword. |

---

## 3. Anti-pattern note: language constructs beat library calls

For every container / control-flow row above, the canonical C++ form is
the **language construct**, not a runtime helper call. The BP runtime
needs `UKismetArrayLibrary::Array_Add` because the BP language has no
brace-init syntax; C++ has both. Emitting the helper-call form is a
correctness regression on three axes:

1. **Performance** — `TArray<int32>{1, 2, 3}` allocates once with the
   right reservation; `Array.Add(1); Array.Add(2); Array.Add(3);`
   reallocates twice.
2. **Readability** — engineers reviewing transpiled C++ expect the
   modern UE5 idiom, not the BP-runtime trampoline.
3. **Const-correctness** — brace init can produce a `const TArray<T>`;
   `Add()` calls can't.

The full anti-pattern catalogue:

| Sugar node | Anti-pattern | Why it's wrong |
|---|---|---|
| `K2Node_MakeArray` | `TArray<T> A; A.Add(B); A.Add(C);` | Two reallocations vs one; loses const-correctness. |
| `K2Node_MakeMap` | `TMap<K,V> M; M.Add(K1, V1); M.Add(K2, V2);` | Same. |
| `K2Node_MakeSet` | `TSet<T> S; S.Add(A); S.Add(B);` | Same. |
| `K2Node_MakeStruct` | `FFoo F; F.A = X; F.B = Y;` | Loses const-correctness; can't be used in a `constexpr` slot. |
| `K2Node_GetArrayItem` | `UKismetArrayLibrary::Array_Get(Array, Idx)` | One extra function call per access; loses inlining. |
| `K2Node_IfThenElse` | `UKismetSystemLibrary::SwitchHasAuthority` style runtime branch | The language `if` is the canonical form. |
| `K2Node_ExecutionSequence` | Nested lambdas, `// branch N` markers | Inflates output, adds zero semantic information. |
| `K2Node_MacroInstance(ForEachLoop)` | Index-loop + `Array_Get` per element | Range-for is canonical in modern UE5. |
| `K2Node_FormatText` | `FString::Printf(TEXT(".."), ..)` | Drops LOCTEXT key, breaks localisation, FText → FString downgrade. |
| `K2Node_Select` (2-arm) | `switch (i)` with `case 0` / `case 1` | Ternary reads cleaner and codegens to the same conditional move. |
| `K2Node_DynamicCast` | `IsA(X::StaticClass())` then `static_cast<X*>` | `Cast<>` does both atomically and is reflection-aware. |
| `K2Node_MacroInstance(IsValid)` | `if (X != nullptr)` | Misses UE's PendingKill check. |

---

## 4. Why this matters for bp-reader

`transpile_function` is judged on whether the generated C++ reads like
hand-written UE5 source. Every row where the emitter falls back to a
runtime-helper call or to `{unsupported}` makes the output less
readable, less reviewable, and more work for the agent to clean up.
The `bp-reader` skill (`Plugins/BlueprintReader/Claude/skills/bp-reader/`)
prompts the model toward idiomatic patterns, but the codegen has to
emit them in the first place.

### What "idiomatic" looks like in practice

Concrete examples of the same BP source rendered idiomatically (the
current emitter) vs. the anti-pattern (what a naïve transpiler might
produce):

**BP**: `MakeArray` (3 int literals) → `Print String (Length)`

```cpp
// Idiomatic (current bp-reader output)
TArray<int32> Items{1, 2, 3};
UKismetSystemLibrary::PrintString(this, FString::FromInt(Items.Num()));

// Anti-pattern (avoided)
TArray<int32> Items;
Items.Add(1);
Items.Add(2);
Items.Add(3);
UKismetSystemLibrary::PrintString(this,
    FString::FromInt(UKismetArrayLibrary::Array_Length(Items)));
```

**BP**: `ForEachLoop` over `MyActors` array, calls `Destroy` on each.

```cpp
// Idiomatic
for (auto& Element : MyActors) {
    Element->Destroy();
}

// Anti-pattern
for (int32 i = 0; i < UKismetArrayLibrary::Array_Length(MyActors); ++i) {
    AActor* Element;
    UKismetArrayLibrary::Array_Get(MyActors, i, Element);
    Element->Destroy();
}
```

**BP**: `Select` between two ints based on a bool.

```cpp
// Idiomatic
const int32 V = (bUseHigh ? HighValue : LowValue);

// Anti-pattern
int32 V;
UKismetMathLibrary::SelectInt(HighValue, LowValue, bUseHigh, V);
```

**BP**: `Branch` on `IsValid` macro.

```cpp
// Idiomatic
if (IsValid(Target)) {
    Target->Destroy();
}

// Anti-pattern
bool bIsValid;
UKismetSystemLibrary::IsValid(Target, bIsValid);
if (bIsValid) {
    Target->Destroy();
}
```

The pattern is consistent: prefer language constructs over runtime
helper functions, prefer member-function syntax over free-function
trampolines, and never break a one-shot expression into a
declare-then-call sequence unless BP semantics genuinely require it.

Cross-link table — where each row of the inventory above is tracked in
the design doc:

| Inventory row | Design-doc section |
|---|---|
| `K2Node_IfThenElse` | [§Flow control](../design/10-bp-to-cpp-node-coverage.md#flow-control) |
| `K2Node_ExecutionSequence` | Same |
| `K2Node_MultiGate` | Same — listed under "Remaining gaps". |
| `K2Node_Switch*` | Same |
| `K2Node_DynamicCast` | [§Casting / type checks](../design/10-bp-to-cpp-node-coverage.md#casting--type-checks) |
| `K2Node_ClassDynamicCast` | Same |
| `K2Node_MacroInstance(*)` | [§Loops](../design/10-bp-to-cpp-node-coverage.md#loops-via-macros) for loops; [§Remaining gaps](../design/10-bp-to-cpp-node-coverage.md#remaining-gaps) for Gate / DoOnce / FlipFlop / DoN. |
| `K2Node_Knot` / `K2Node_Tunnel` / `K2Node_Composite` | [§Macros / composites / passthrough](../design/10-bp-to-cpp-node-coverage.md#macros--composites--passthrough) |
| `K2Node_FormatText` | [§Special / utility](../design/10-bp-to-cpp-node-coverage.md#special--utility) |
| `K2Node_MakeArray` / `MakeMap` / `MakeSet` / `MakeStruct` | [§Containers](../design/10-bp-to-cpp-node-coverage.md#containers) and [§Structs](../design/10-bp-to-cpp-node-coverage.md#structs) |
| `K2Node_Select` | [§Flow control](../design/10-bp-to-cpp-node-coverage.md#flow-control) |
| `K2Node_AssignmentStatement` | [§Variable nodes](../design/10-bp-to-cpp-node-coverage.md#variable-nodes) |
| `K2Node_EnumLiteral` | [§Constants / literals / self](../design/10-bp-to-cpp-node-coverage.md#constants--literals--self) |

---

## 5. Coverage gaps

Inspection of `CppEmit.cpp` (1721 lines) plus `Decompile.cpp`'s K2
class dispatch shows the following sugar nodes that are **not** handled
idiomatically:

### 5a. Unhandled at decompile (falls straight through to `{unsupported}`)

These have no K2-class match in `Decompile.cpp`; the dispatcher walks
past them and emits a generic unsupported entry.

| K2 class | Gap | Closing it |
|---|---|---|
| `K2Node_Composite` | Not seen post-compile, but appears in raw K2 graph snapshots. Handler missing entirely. | Add a recursion into the composite's contained graph at decompile time; emit the inlined statements directly. |
| `K2Node_SetFieldsInStruct` | Useful when an actor writes 2+ fields of a struct member at once. Missing. | Lower to a sequence of `{set, to}` statements, one per field. |
| `K2Node_StructMemberGet` / `K2Node_StructMemberSet` | Specialised get/set on a named struct field. Missing. | StructMemberGet → `{member, name}` (already exists for BreakStruct); StructMemberSet → `{set, to}` with a `member`-form LHS. |
| `K2Node_CastByteToEnum` | Lowers `uint8 → EEnumType`. Missing. | `static_cast<EEnumType>(<byte>)`. One-liner addition to `BuildExpression`. |
| `K2Node_BitmaskLiteral` | Bitmask flag literal — used heavily for `EObjectFlags`-style enums. Missing. | Lower to a `{lit}` carrying the bitwise-OR'd integer or to an OR expression `(1 << A) | (1 << B)`. |
| `K2Node_DoOnceMultiInput` | The multi-input variant of DoOnce — fires once across N inputs. Missing. | Synth `bool` member + per-input-pin guard. Near-clone of single-input DoOnce. |
| `K2Node_MacroInstance(IsValidClass)` | One-line addition next to `IsValid` (which is already handled at `Decompile.cpp:1518`). | Recognise `bare == "IsValidClass"` and emit the same `{if, {call: "IsValid"}, then, else}` shape. |
| `K2Node_MacroInstance(ForEachElementInEnum)` | Iterates enum values. Missing. | Lower to `for (int32 I = 0; I < (int32)EEnumType::EEnumType_MAX; ++I)` — sidecar-tagged because the `_MAX` sentinel naming convention is not universal across UE enums. |
| `K2Node_Copy` | Specialised cross-graph value copy. Missing. | Lower to `{set, to}`. |

### 5b. Approximation gaps (emits compiling but wrong C++)

| K2 class | What's wrong | Fix |
|---|---|---|
| `K2Node_SwitchString` | Renders as C++ `switch` over FString — illegal. | Introduce a `{string_switch}` BPIR form that CppEmit renders as chained `if (S == TEXT("X"))`. |
| `K2Node_SwitchName` | Renders as C++ `switch` over FName — non-portable. | Same as above with FName comparisons. |
| `K2Node_MacroInstance(ReverseForEachLoop)` | Renders as forward range-for — **iterates in the wrong direction**. The BPIR carries a `reverse_flag` but CppEmit ignores it. | Honour the flag in `EmitStatement(for_each)` — emit `for (int32 I = <In>.Num() - 1; I >= 0; --I) { auto& <Elem> = <In>[I]; ... }`. |
| `K2Node_ClassDynamicCast` | Renders as `Cast<X>(..)` — works for class refs but loses `TSubclassOf<X>` typing. | Detect the class-ref form in BuildExpression and emit either `Cast<UClass>(..)` with a hint, or use `TSubclassOf<X>(SomeClass)` directly. |

### 5c. Stateful sugar that needs walker / synth-member infrastructure

| K2 class | Why deferred | Hint |
|---|---|---|
| `K2Node_MultiGate` | Same auto-synth pattern as DoOnce / FlipFlop / DoN but with an integer cursor. | Reuse the synth-member channel; the GUID-derived name pattern works directly. |
| `K2Node_MacroInstance(Gate)` | Needs entry-pin awareness in the walker (currently visits a node from a predecessor without tracking the destination pin). | The recent `f0a412e` commit threaded destination pin into ExecTarget — close the remaining gap by routing per-input behaviour in the macro-instance dispatcher. |
| `K2Node_DoOnceMultiInput` | Same as DoOnce but with N inputs sharing one flag. | Near-clone of the existing handler. |

### 5d. Out-of-scope / not sugar

For completeness — the dispatcher correctly classifies these as **not**
sugar:

- `K2Node_Timeline` — opaque, stateful, ships a `UTimelineComponent`.
- `K2Node_BaseAsyncTask` / `K2Node_AsyncAction` / `K2Node_LatentAbilityCall`
  / `K2Node_LatentGameplayTaskCall` — opaque; auto-lowered via the
  3-sentinel async pattern (`__bpr_async_factory` /
  `__bpr_async_bind` / `__bpr_async_activate`).
- `K2Node_CallFunction(Delay)` — sentinel-lowered into
  `__bpr_set_timer` + a synth continuation method.
- `K2Node_EnhancedInputAction` — event-graph aggregation into
  `SetupPlayerInputComponent`.
- `K2Node_MatineeController` — legacy, replaced by Sequencer.
- `K2Node_AnimNode_*`, `K2Node_NiagaraXxx` — out-of-scope graph
  domains.

---

## See also

- [`docs/design/10-bp-to-cpp-node-coverage.md`](../design/10-bp-to-cpp-node-coverage.md)
  — what's shipped + the status legend used here.
- [`Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/tools/codegen/CppEmit.cpp`](../../Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/tools/codegen/CppEmit.cpp)
  — 1721 lines of BPIR-to-C++ rendering, the place to extend.
- [`Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/tools/Decompile.cpp`](../../Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/tools/Decompile.cpp)
  — the K2-class dispatcher that produces BPIR; most gap closures
  start here.
- [`Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/tools/codegen/UnsupportedTreatment.cpp`](../../Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/tools/codegen/UnsupportedTreatment.cpp)
  — where `{unsupported}` entries get classified into TODO comments vs.
  inline approximations.
- `Engine/Source/Editor/BlueprintGraph/Classes/K2Node_*.h` (sibling
  engine checkout at `D:/Projects/Unreal Engine 5/`) — authoritative
  class hierarchy; UE 5.7 ships ~120 K2Node_* headers, of which the
  ~25 above are syntactic sugar.
- `Engine/Content/EditorBlueprintResources/StandardMacros.uasset` —
  the BP macro library that backs `ForEachLoop`, `WhileLoop`,
  `IsValid`, `IsValidClass`, `Gate`, `DoOnce`, `DoN`, `FlipFlop`,
  `Select`, `ForEachLoopWithBreak`, `ReverseForEachLoop`,
  `ForEachElementInEnum`. Lives as a .uasset; introspect via the
  BP macro browser in-editor or via the bp-reader server's `read`
  tool against `/Engine/EditorBlueprintResources/StandardMacros`.
- `Engine/Content/EditorBlueprintResources/ActorMacros.uasset` and
  `ActorComponentMacros.uasset` — actor / component lifecycle macros,
  outside this note's scope but mentioned for completeness.
