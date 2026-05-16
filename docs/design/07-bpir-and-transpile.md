# 07 — BPIR & transpile

BPIR — Blueprint Intermediate Representation — is the pivot for every
BP↔source conversion the server supports. It's a JSON AST, versioned
and validated, that both `decompile_function` (BP → BPIR) and
`compile_function` (BPIR → BP) speak. The codegen side
(`tools/codegen/*`) lowers BPIR to C++; the parser side
(`tools/parse/*`) raises C++ back to BPIR. The two compose to identity
on the language subset the codegen produces.

```
   BP graph  ─decompile→  BPIR  ─CppEmit→  C++
                    ↑       │
                    │       └─compile_function→  BP graph
              CppParse
                    │
                  C++
```

The "BP↔C++ pipeline complete" milestone in the project memory is
exactly this round-trip. Future languages (Lua, Python, JS) are a
codegen + parser pair against the same BPIR — no other piece changes.

See [05 — Backends](05-backends.md) for what produces / consumes the
underlying BP graphs, and [06 — Wire protocol](06-wire-protocol.md)
for the on-wire shape of those graphs.


## The schema

Source: `Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/tools/Bpir.h` (the
schema docs and validator API),
`Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/tools/Bpir.cpp` (the
implementation).

Top-level shape (`Bpir.h:14`):

```json
{ "version": 1, "kind": "function"|"class", ...payload... }
```

`version` is `kBpirSchemaVersion` (`Bpir.h:76`); validators reject any
doc whose version is higher than this build understands and migrate
older docs silently up (today there's only v1 so `MigrateToCurrent` is
a no-op, but the seam exists at `Bpir.cpp:361-365` so a future v2
doesn't require touching every consumer).

### Function payload

`Bpir.h:16-22`:

```json
{
  "name": "TakeDamage",
  "metadata": { "asset_path": "...", "return_type": "...",
                "ufunction_specifiers": [...] },
  "inputs":  [{ "name": "Damage", "type": "float" }],
  "outputs": [{ "name": "Result", "type": "bool" }],
  "locals":  [{ "name": "Temp", "type": "int" }],
  "body":    [ statements... ]
}
```

### Class payload

`Bpir.h:24-29`:

```json
{
  "name": "BP_Enemy",
  "metadata": { "asset_path": "/Game/AI/BP_Enemy",
                "parent_class": "ACharacter",
                "uclass_specifiers": [...] },
  "interfaces": ["IDamageable"],
  "variables":  [{ "name": "Health", "type": "float",
                   "default": "100", "category": "Combat",
                   "replicated": true, "editable": true }],
  "functions":  [ /* BPIR-function-doc objects */ ]
}
```

`variables` here is BPIR's variable-decl form (named `BPVariableDecl`
internally) — `name`, `type`, plus optional `default` / `category` /
`replicated` / `editable`. It's intentionally lighter than the wire
`BPVariable` because BPIR is canonical, not a serialization of UE's
internal layout. The validator at `Bpir.cpp:256-273` rejects bad
shapes here.

### Statement forms

`Bpir.h:32-46` lists the supported forms; `Bpir.cpp:43-50` is the
authoritative list the validator dispatches on:

```cpp
const std::vector<std::string>& StatementFormsImpl() {
    static const std::vector<std::string> forms = {
        "if", "set", "call", "comment",
        "return", "cast", "switch", "for_each", "while", "sequence",
        "break", "continue", "unsupported",
    };
    return forms;
}
```

Form recognition: an object is a statement of form X iff it has key X
as one of its top-level fields. The first match wins (`Bpir.cpp:60-67`,
`FirstMatchingKey`). Order matters only when two forms could overlap;
none do today, but the order is stable.

A real statement from `tests/test_bpir.cpp:77-96`:

```cpp
json{{"if", json{{"var", "bIsAlive"}}}, {"then", json::array()}}
// ↳ { "if": { "var": "bIsAlive" }, "then": [] }
```

Cast statements are richer because UE's `K2Node_DynamicCast` carries
both success and fail exec branches plus an as-typed local
(`Bpir.cpp:198-211`):

```cpp
} else if (form == "cast") {
    ValidateExpression(stmt["cast"], fmt::format("{}.cast", path));
    RequireString(stmt, "to", path);
    if (auto aIt = stmt.find("as"); aIt != stmt.end() && !aIt->is_string()) {
        Bad(path, R"(field "as" must be a string ...)");
    }
    if (!stmt.contains("success")) Bad(path, R"(cast statement requires a "success" branch ...)");
    ValidateStatementList(stmt["success"], fmt::format("{}.success", path));
    if (!stmt.contains("fail")) Bad(path, R"(cast statement requires a "fail" branch ...)");
    ValidateStatementList(stmt["fail"], fmt::format("{}.fail", path));
}
```

Switch requires a `cases` object; the validator at `Bpir.cpp:212-222`
walks every case body. `for_each` mirrors UE's `ForEachLoop` macro;
`while` mirrors `WhileLoop`. Both validate the body recursively.

The `unsupported` form is the safety valve — see below.

### Expression forms

`Bpir.h:48-57`, enumerated at `Bpir.cpp:52-58`:

```cpp
const std::vector<std::string>& ExpressionFormsImpl() {
    static const std::vector<std::string> forms = {
        "var", "lit", "call",
        "cast", "member", "index", "self", "new_array", "new_struct",
    };
    return forms;
}
```

`var` references a named variable, optionally scoped
(`local`/`member`/`input`/`output`). `lit` carries a JSON scalar.
`call` is a function call as an rvalue. `cast` is a pure DynamicCast
(no exec branches — for the statement form with success/fail use the
statement `cast`). `member` is property access. `index` is
array/map subscript. `self` is the K2 self node. `new_array` and
`new_struct` mirror `K2Node_MakeArray` / `K2Node_MakeStruct`.

### Types

BPIR uses the same shorthand grammar as the wire — `"float"`,
`"object:Actor"`, `"[]float"`, `"{string:int}"`. Parsed by
`tools::ParseTypeShorthand` in `tools/TypeShorthand.h`. The choice
to use strings rather than `BPPinType` objects is deliberate: BPIR
is meant to be authored by humans and AI agents directly, and the
shorthand wins on token economy.

### Validation

`ValidateBpir` (`Bpir.cpp:341-359`) is the contract every consumer
relies on. Codegen re-runs the validator at its own entry point
(`CppEmit.cpp` calls it via the `EmitCppFunctionBody` path) so that
even if a tool dispatcher forgot to validate, codegen never sees a
malformed doc. Validation errors are formatted with a structured
path so failures point at the offending node:

```
BPIR at 'body[3].if': expected object
BPIR at 'body[0].cast.success[1]': missing or non-string field "name"
```

The path system uses `fmt::format` to build dotted/bracketed paths
as the recursive validator descends (`Bpir.cpp:147-153`). When the
validator throws, the agent reading the response sees exactly which
sub-tree is the problem.


## Decompile direction (BP → BPIR)

Source: `tools/Decompile.h`, `tools/Decompile.cpp`.

`DecompileFunction` (`Decompile.h:53-56`) takes a `BPFunction` already
fetched via `IBlueprintReader::GetFunction` and reconstructs a BPIR
function doc. The algorithm at `Decompile.h:10-26`:

1. Find the FunctionEntry node; take its `then` exec output as the
   start.
2. Walk exec edges; classify each node by its `Class` field.
3. Pattern-match recognized control flow (Branch, Sequence, Cast,
   Switch, MacroInstance for ForEach/While) into BPIR. Recurse into
   branches; converge at the immediate post-dominator.
4. For value-shaped nodes (VariableGet, CallFunction-as-rvalue,
   MakeArray, MakeStruct, Self, Literal), trace data edges backward
   from each consumer pin to build expressions.
5. Anything that doesn't pattern-match → emit `{unsupported}` with
   the node's class + guid + relevant meta.

Cleanly supported (the comment at `Decompile.h:22-23`):
`if/then/else`, `set/call/return`, `cast`, `sequence`, `var/lit/call`
expressions, `self`.

Emitted as `unsupported`: Timelines, async actions with typed payloads,
AnimGraph / Niagara nodes, the Gate macro variant, legacy
K2Node_InputAction nodes — anything domain-specific or non-portable
that hasn't been given a dedicated auto-lowering pass.

**Auto-lowered** (decompile produces real compilable scaffolds —
either inline statements with synth class members, or synth functions
on the class doc that the codegen treats identically to BP-authored
content):
- Loops: `ForEachLoop` / `ForEachLoopWithBreak` / `ReverseForEachLoop` /
  `WhileLoop` / `IsValid` → native `for` / `while` / `if (IsValid(X))`.
- Stateful macros: `DoOnce` / `FlipFlop` / `DoN` → synth `bool` /
  `int32` member (unique per node GUID, hoisted into class
  `variables[]` by DecompileBlueprint) + guarded `if` block.
- Latent: `Delay` / `RetriggerableDelay` / `DelayUntilNextTick` →
  `__bpr_set_timer` sentinel statement + synth `FTimerHandle` member +
  generated `UFUNCTION()` continuation method whose body is the
  post-delay exec. Continuations are pushed into the walker's
  `autoSynthFunctions` collector and hoisted onto the class
  `functions[]` array, so they go through the same codegen pipeline
  as BP-authored functions.
- EnhancedInput: `K2Node_EnhancedInputAction` event nodes →
  `DecompileBlueprint`'s `ProcessEnhancedInputBindings` pass scans
  event graphs, generates per-trigger `UFUNCTION()` callbacks +
  `TObjectPtr<UInputAction>` UPROPERTY + a synthesized
  `SetupPlayerInputComponent` override that wires everything in a
  `Cast<UEnhancedInputComponent>` block.

Each auto-lowering shares the same `Walker.autoSynthVars` /
`Walker.autoSynthFunctions` channel so a Delay inside an EnhancedInput
callback works correctly — the continuation participates in the same
hoist pass.

The validator runs on the way out (`Decompile.cpp` calls `ValidateBpir`
on its result before returning) so a bug in the decompile pass
surfaces as a clear schema-violation error rather than as garbage
downstream.

`DecompileBlueprint` (`Decompile.h:59-61`) is the whole-class wrapper:
list every function on the BP, decompile each, plus the variable
list, package as a `{kind: "class", ...}` doc.


## Codegen direction (BPIR → C++)

Source:
`Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/tools/codegen/CppEmit.h`,
`CppEmit.cpp`, `CppClassEmit.h`, `CppClassEmit.cpp`.

### CppEmit — function bodies

`EmitCppFunctionBody` (`CppEmit.h:44-45`) takes a BPIR function doc
and renders its `body[]` as a C++ block. Surrounding scaffold
(`<class>::<func>(args) { ... }`) is the caller's responsibility; this
function emits only the contents.

`CppEmitOptions` (`CppEmit.h:20-28`) controls:

- `mode = Readable | Compilable` — Readable renders human-friendly
  output with comments and operator aliases; Compilable renders
  strictly-valid C++ a UBT build would accept.
- `indentSpaces` — defaults to 4.
- `useOperatorAliases` — when true, `{call: "KismetMathLibrary::Add_IntInt", ...}`
  renders as `a + b` instead of `UKismetMathLibrary::Add_IntInt(a, b)`.
  Default true.

### Type mapping

`MapBpirTypeToCpp` (`CppEmit.h:57`) is the inverse of
`TypeShorthand::Parse`. The implementation at `CppEmit.cpp:29-139`
walks a recursive descent through container prefixes (`[]` → `TArray`,
`{}` → `TSet`, `{K:V}` → `TMap`) before resolving inner names.

There are three variants for three contexts (`CppEmit.h:57-78`):

- `MapBpirTypeToCpp` — bare form. `object:Actor` → `AActor*`.
- `MapBpirTypeToCppMember` — UPROPERTY-context. Wraps object refs
  in `TObjectPtr<>` per UE5 convention. `object:Actor` → `TObjectPtr<AActor>`.
- `MapBpirTypeToCppArg` — function-argument context. Wraps heavy
  types (`FString`, `FText`, struct types, containers) in `const X&`.

The split reflects Epic's actual conventions: `TObjectPtr` is for
class-member fields (the editor uses it for pointer reflection),
`const X&` is for arguments (avoid copies). Function locals and pin
defaults use the bare form.

### Aliases and world-context

Four lookup tables in `CppEmit.cpp` rewrite the canonical
BP-via-Kismet-library form into idiomatic C++:

- **`OpReverseMap`** (`CppEmit.cpp:151-205`) maps
  `KismetMathLibrary::Add_IntInt` etc. to operator syntax (`a + b`).
  Covers int / float / vector / vector2d / rotator / string / name
  overloads. Cosmetic — `Compilable` mode still produces correct C++
  either way — but the result reads much better.
- **`MethodCallAliases`** (`CppEmit.cpp:222-255`) maps
  `KismetArrayLibrary::Array_Add(TargetArray, Item)` to
  `Array.Add(Item)`. The alias entry names the method and which arg
  slot holds the receiver — by name, not position, because BPIR's
  `args` is a JSON object and alphabetical iteration would otherwise
  shuffle the receiver off the front.
- **`NameAliases`** (`CppEmit.cpp:262-277`) handles cases where BP
  routes through a Kismet library but the C++ form is unqualified
  (`IsValid`, `GetClass`).
- **`WorldContextFunctions`** (`CppEmit.cpp:286-299+`) lists Kismet
  functions whose first arg is a hidden `WorldContextObject` pin in
  BP but a real first arg in C++. The emitter injects `this` —
  without that, `PrintString` etc. wouldn't compile.

### CppClassEmit — whole .h/.cpp pairs

`CppClassEmit.h:70-72`:

```cpp
CppClassEmitResult EmitCppClass(const nlohmann::json& bpirClassDoc,
                                CppClassEmitOptions opts = {});
```

This is what `transpile_blueprint` calls. It layers on top of CppEmit
by adding UCLASS scaffolding (`CppClassEmit.h:7-15`):

- `#pragma once` plus matching `.generated.h` include
- `UCLASS()` macro with inferred specifiers
- `GENERATED_BODY()`
- `UPROPERTY()` decls for every BP variable, with
  `Replicated` / `EditAnywhere` / `BlueprintReadWrite` / `Category`
  specifiers inferred from BP variable metadata
- `UFUNCTION()` decls for every BP function
- Per-function bodies in the .cpp via `CppEmit`
- `GetLifetimeReplicatedProps()` when any variable is `Replicated`

Class naming follows UE convention: `BP_Enemy` with parent `AActor` →
`ABP_Enemy_Generated`; with parent `UObject` → `UBP_Enemy_Generated`.
The `_Generated` suffix is configurable via `CppClassEmitOptions::classNameSuffix`
(default; matches the plan's "companion file" convention).

`ParentClassToHeader` (`CppClassEmit.h:78`) maps a parent short name
to its UE include path (`"Actor"` → `"GameFramework/Actor.h"`). Unknown
parents emit a TODO include the agent can resolve.


## The `unsupported` safety valve

Source: `tools/codegen/UnsupportedTreatment.h`,
`tools/codegen/UnsupportedTreatment.cpp`.

Some BP constructs don't have a clean compilable-C++ equivalent.
Timelines, latent actions, AnimGraph nodes — translating them naively
would produce code that doesn't compile or doesn't run. BPIR's
`unsupported` statement form is the escape hatch
(`Bpir.h:45`):

```json
{"unsupported": {"node_class": "K2Node_Timeline", "guid": "...",
                 "reason": "...", "fields": {...}}}
```

The validator (`Bpir.cpp:243-251`) requires a string `node_class`
field; everything else is optional context.

`UnsupportedTreatment` classifies each unsupported entry into one of
two kinds (`UnsupportedTreatment.h:53-66`):

```cpp
struct UnsupportedClassification {
    enum class Kind {
        TodoComment,    // no good substitution; emit `// TODO[bpr-unsupported]`
        Approximation,  // best-effort C++ stub generated; user verifies
    };
    Kind kind = Kind::TodoComment;
    std::string snippet;  // pre-built C++ for inline emission (Approximation only)
    std::string note;     // sidecar message
};
```

The treatment table at `UnsupportedTreatment.cpp:26-80+` maps K2 node
classes to recipes. Real entries:

- `K2Node_Timeline` → TodoComment + a note explaining manual
  `UTimelineComponent` setup.
- `K2Node_AsyncAction` → TodoComment + note about
  `UAsyncAction*::CreateXxx` + delegate binding.
- `K2Node_LatentAbilityCall` / async tasks with typed payload pins →
  TodoComment + note about `UAsyncAction*::CreateXxx` + delegate
  binding.

Note: `Delay` / `RetriggerableDelay` / `DelayUntilNextTick` treatment
entries in `UnsupportedTreatment.cpp` are now defensive — the
decompile-side handler emits a `__bpr_set_timer` sentinel +
auto-generated continuation method, so the classifier rarely sees an
`unsupported` Delay. The table entries stay as a safety net for cases
where introspection meta is missing (older plugin builds, malformed
nodes); when they fire, they still produce the same SetTimer-style
TodoComment hint.

A few node kinds get `Approximation` treatment instead — the
`K2Node_SpawnActorFromClass` case mentioned in the header comment
(`UnsupportedTreatment.h:11-13`) becomes a `GetWorld()->SpawnActor<>()`
call with a `// verify` comment.

### Sidecar JSON

Every unsupported node hit by codegen — whether `TodoComment` or
`Approximation` — adds an entry to a sidecar JSON document the caller
can write next to the generated `.cpp`. The agent uses this as a
checklist of remaining manual work without re-parsing the source.

Sidecar shape (`UnsupportedTreatment.h:23-39`):

```json
{
  "version": 1,
  "generated_at": "2026-05-12T...",
  "source_bp": "/Game/AI/BP_Enemy",
  "generated_files": ["BP_Enemy_Generated.h", "BP_Enemy_Generated.cpp"],
  "unsupported_nodes": [
    { "guid": "...", "class": "K2Node_Timeline", "function": "TakeDamage",
      "treatment": "todo_comment",
      "manual_steps": ["Configure 'Fade' curve asset"] }
  ],
  "approximations": [
    { "guid": "...", "class": "K2Node_SpawnActorFromClass",
      "function": "TakeDamage",
      "treatment": "spawn_actor_call",
      "verify": "SpawnParameters' bNoCollisionFail equivalent (deprecated in 5.x)" }
  ]
}
```

`BuildSidecar` (`UnsupportedTreatment.h:78-80`) assembles the document
from CppEmit's notes array.

The pattern (header comment at `UnsupportedTreatment.h:18-21`)
explicitly acknowledges Hazelight's UnrealEngine-Angelscript repo as
inspiration for the class→treatment table.


## C++ parser direction (C++ → BPIR)

Source: `tools/parse/CppLex.h`, `CppLex.cpp`,
`tools/parse/CppParse.h`, `CppParse.cpp`.

The parser is hand-rolled. libclang would be the obvious choice for a
full C++ frontend, but vendoring LLVM (~30–50MB of binaries) clashes
with the project's no-network/no-fetch third_party policy. The
hand-rolled lexer + recursive-descent parser are scoped to the subset
CppEmit produces, plus reasonable extensions a human might write.

`CppLex.h:1-26` documents the lexer's subset:

- Keywords: `if`, `else`, `for`, `while`, `switch`, `case`, `default`,
  `return`, `break`, `continue`, `true`, `false`, `nullptr`, `this`,
  `auto`, `const`
- Identifiers, qualified names (`Foo::Bar` is one token —
  `CppTokenKind::QualifiedName`)
- Number literals (with optional `f` suffix), string literals
  (including `TEXT("...")` wrapper)
- Operators: `+ - * / % == != < <= > >= && || ! = -> . :: , ; ( ) { } [ ] &`
- `Cast<T>(expr)` — special token

Out of scope (parser throws with a clear error):

- The C preprocessor — caller passes a bare function body, no
  `#include` / `#define` / `#if`
- UE macros — they're decoration for the class scaffold, not function
  body content
- Templates beyond `Cast<T>`
- Lambdas, decltype, exception machinery
- Pointer arithmetic

`ParseCppFunction` (`CppParse.h:68`) accepts either a full function
definition or a bare body block. Errors come back as `CppParseError`
with `<line>:<col>: <message>` prefix. The returned BPIR doc is
validated before return — a validation failure surfaces as a
`CppParseError` so callers don't need a separate exception handler.


## Round-trip identity

Source: `tests/test_transpile_roundtrip.cpp`.

For the patterns CppEmit produces, the composed pipeline
`BPIR → CppEmit → CppParse` should yield the original BPIR (modulo
cosmetic fields like metadata that the parser can't recover from
source). The roundtrip tests pin this property. A real case
(`test_transpile_roundtrip.cpp:53-58`) starts from a BPIR with one
`set Health to 100`, runs `EmitCppFunction` then `ParseCppFunction`,
and asserts deep JSON equality of the bodies. Coverage spans control
flow (if/then/else, range-based for, switch), expressions (literals,
calls, casts), and side-effecting forms (set, return, break). Each
test fails fast on either an emit regression or a parse regression
— the property pins both ends simultaneously. If you change either
codegen or parsing, run the roundtrip tests first.


## Bigger picture

Adding Lua / Python / JS later is another codegen + parser pair against
the same BPIR. The MCP tool layer, the wire protocol, and the backends
don't change — only `tools/codegen/` and `tools/parse/` grow. The
`unsupported` form is what makes this practical: a new target-language
codegen doesn't need to handle every BP construct on day one. Anything
it doesn't know how to emit becomes a TODO comment plus a sidecar
entry, and the agent owns the remaining manual work.

For how each piece is tested, see [09 — Testing](09-testing.md). For
how compile errors and validation failures bubble up to the MCP
client, see [08 — Errors & diagnostics](08-error-diagnostics.md).
