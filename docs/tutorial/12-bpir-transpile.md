# Chapter 12 — BPIR and transpile

You can now read and write Blueprint graphs by issuing
`add_node` / `wire_pins` operations. That's the right granularity
for **automation** — an agent can construct a graph step by step —
but it's the wrong granularity for **understanding**. An LLM looking
at a function with 30 nodes and 80 pins is staring at a wiring
diagram. It can't reason about the function's behavior the way it
can reason about source code.

The shortest path between "graph" and "natural language" runs
through **source code**. So the question is: how do we translate a
Blueprint function into C++ (and back), without baking the C++ type
system into the graph reader?

The answer is an intermediate representation. We call it BPIR —
Blueprint Intermediate Representation. It's a versioned JSON AST
that any source-language frontend lowers to/from, and that maps 1:1
onto BP graph operations.

```
                      BPIR
                       ^|
                       ||
   BP graph <----------+----------> C++ source
   (UEdGraph)          ||           (.h / .cpp)
                       ||
                       v|
                  (future:
                   Lua, Python,
                   JavaScript,
                   Verse, ...)
```

Adding a new target language is a codegen module plus a parser. The
introspector, the graph editor, the wire format, and the rest of
the tool surface are untouched.

## The BPIR schema

BPIR is a JSON document with a top-level `{version, kind, ...}`
envelope:

```jsonc
// Function form:
{ "version": 1, "kind": "function",
  "name": "TakeDamage",
  "metadata": { "asset_path": "/Game/AI/BP_Enemy", "return_type": "bool" },
  "inputs":  [{ "name": "Amount", "type": "float" }],
  "outputs": [{ "name": "Killed", "type": "bool" }],
  "locals":  [{ "name": "WasAlive", "type": "bool" }],
  "body":    [/* statements */] }

// Class form (whole BP):
{ "version": 1, "kind": "class",
  "name": "BP_Enemy",
  "metadata": { "parent_class": "Actor" },
  "variables": [{ "name": "Health", "type": "float", "default": "100" }],
  "functions": [/* function docs */] }
```

### Statement and expression forms

Statements are the K2 node families distilled down — `if/set/call/
return/cast/switch/for_each/while/sequence/break/continue/comment/
unsupported`. Expressions cover the value-producing nodes — `var/
lit/call/cast/member/index/self/new_array/new_struct`. The full
grammar is in [`tools/Bpir.h`](../../Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/tools/Bpir.h).
Two examples:

```
{if: <expr>, then: [s], else: [s]}      // K2Node_IfThenElse
{cast: <expr>, to: "<class>",           // K2Node_DynamicCast
 [as: "<localName>"], success: [s], fail: [s]}
```

The shape is **deliberately small**. It covers every BP construct
that's structurally portable to source-language form. Things that
aren't (timelines, async, latent, animation graph nodes) get an
`{unsupported: {node_class, guid, reason, fields?}}` form so callers
see exactly what couldn't be translated.

### Validation

```cpp
// Bpir.h
void ValidateBpir(const nlohmann::json& doc);
const std::vector<std::string>& StatementForms();
const std::vector<std::string>& ExpressionForms();
std::string DetectStatementForm(const nlohmann::json& stmt);
std::string DetectExpressionForm(const nlohmann::json& expr);
nlohmann::json MigrateToCurrent(const nlohmann::json& doc);
```

`ValidateBpir` walks the document, checks required fields per form,
type-checks expression positions, and throws `std::invalid_argument`
with a path-qualified message (`"body[3].if"`) on any violation.
Codegen and the parser both run the validator on their output as a
sanity check. If validation fails inside codegen, that's a bug we
want to surface immediately — not later, when some downstream
consumer trips on a malformed doc.

## Read direction: decompile

`decompile_function` walks a BP graph and emits BPIR. The pass lives
in `tools/Decompile.{h,cpp}`. Algorithm sketch:

```
1. Find the FunctionEntry node; take its `then` exec output as start.
2. Walk exec edges; classify each node by its Class field.
3. Pattern-match recognized control flow:
   - K2Node_IfThenElse           -> {if, then, else}
   - K2Node_DynamicCast          -> {cast, success, fail}
   - K2Node_Switch*              -> {switch, cases, default}
   - K2Node_ExecutionSequence    -> {sequence}
   - MacroInstance(ForEachLoop)  -> {for_each, in, body}
   - MacroInstance(WhileLoop)    -> {while, body}
   Recurse into the branches; converge at the immediate post-dominator.
4. For value-shaped nodes (VariableGet, CallFunction-as-rvalue,
   MakeArray, MakeStruct, Self, Literal), trace data edges backward
   from each consumer pin to build expressions.
5. Anything that doesn't pattern-match → emit {unsupported} with
   the node's class + guid + relevant meta.
```

The decompile pass runs **server-side**, in the MCP server, using
the `BPGraph` already returned by `IBlueprintReader::GetFunction`.
The introspector built that representation in Chapter 4; the
decompile pass just consumes it. No commandlet round-trip needed
for decompilation — once you have the graph JSON, you have
everything.

`{unsupported}` is the load-bearing safety valve. Without it,
decompile would either lie (silently dropping nodes) or fail
catastrophically on the first oddity. With it, decompile is
lossless: every node either becomes a structured BPIR form or
becomes a sentinel that records exactly what couldn't be
represented.

## Write direction: compile

`compile_function` is the inverse. It takes a BPIR function doc
and materializes the BP graph:

```
1. Add the function (idempotent) + its inputs/outputs/locals.
2. Lay nodes out in a simple top-down stack so they don't overlap.
3. For each statement, materialize the necessary nodes and wire
   exec + data pins. Track the "current exec tail" — the pin we'd
   hook the next sequential statement to.
4. Run everything through apply_ops semantics so a single tool call
   covers it.
```

Statement forms map almost directly to add_node + wire_pins
sequences:

```
{if: <expr>, then: [s], else: [s]}
  -> add_node K2Node_IfThenElse + wire condition pin
  -> recursively compile then-block, append to "true" exec pin
  -> recursively compile else-block, append to "false" exec pin
  -> rejoin to outer exec tail at the post-dominator
```

`compile_function` returns the entry node GUID; subsequent tool
calls can append to the function by referring to it.

The structural reason this works at all is that BPIR's statement
shapes were chosen to **be** the K2 node families. We didn't
invent a new control-flow primitive; we just gave names to the
ones BP already has. Round-tripping decompile → compile is therefore
not a translation exercise; it's a serialize/deserialize exercise.

## C++ codegen: `CppEmit`

`CppEmit` lowers BPIR statements to C++ source. The pass lives in
`tools/codegen/CppEmit.{h,cpp}`.

```cpp
struct CppEmitOptions {
    enum class Mode { Readable, Compilable };
    Mode mode = Mode::Readable;
    int  indentSpaces = 4;
    // When true, every BPIR `{call: "+"}` etc. is rendered with the
    // operator alias (e.g. `a + b`) instead of the canonical
    // `UKismetMathLibrary::Add_IntInt(a, b)` call. Default true.
    bool useOperatorAliases = true;
};

CppEmitResult EmitCppFunctionBody(const nlohmann::json& bpirFunctionDoc,
                                  CppEmitOptions opts = {});
```

The interesting choices are:

### Operator aliases

A BPIR `{call: "Add_IntInt"}` with two args could render as either
`UKismetMathLibrary::Add_IntInt(a, b)` (canonical, what the BP
compiler ultimately emits) or `a + b` (what a human writes). The
"readable" mode picks the operator alias when one exists.

The alias table covers arithmetic, comparison, logical, and bitwise
ops on the primitive types BP exposes. It's a fixed list — anything
not on it falls through to the canonical call.

### Type mapping

BPIR types use a shorthand grammar (in `TypeShorthand.cpp`):
`float`, `int`, `bool`, `string`, `object:Actor`, `[]float`,
`{string:int}`. `MapBpirTypeToCpp` lowers these to C++:

```
float            -> float
int              -> int32
bool             -> bool
string           -> FString
object:Actor     -> AActor*
[]float          -> TArray<float>
{string:int}     -> TMap<FString, int32>
```

There are two context variants for UE5-specific conventions:

- `MapBpirTypeToCppMember` wraps UObject pointers in `TObjectPtr<>`
  for UPROPERTY fields (Epic's recommendation since 5.0).
- `MapBpirTypeToCppArg` wraps heavy types (`FString`, `FVector`,
  `TArray`, etc.) in `const X&` for function arguments. Primitives
  pass by value unchanged.

### Unsupported handling

When CppEmit hits an `{unsupported}` statement, it calls
`ClassifyUnsupported`:

```cpp
struct UnsupportedClassification {
    enum class Kind {
        TodoComment,    // no good substitution; emit `// TODO[bpr-unsupported]`
        Approximation,  // best-effort C++ stub generated; user verifies
    };
    Kind kind = Kind::TodoComment;
    std::string snippet;   // for Approximation, the C++ snippet to emit
    std::string note;      // for sidecar JSON
};
UnsupportedClassification ClassifyUnsupported(const nlohmann::json& unsupportedField);
```

The treatment table is in `UnsupportedTreatment.cpp`. It maps known
K2 node classes to either a TODO (with a manual-steps hint in the
sidecar) or an Approximation (with a best-effort C++ stub). Examples:

- `K2Node_Timeline` → `TodoComment`, sidecar lists "configure the
  curve asset" as the manual step.
- `K2Node_SpawnActorFromClass` → `Approximation`, emits a
  `GetWorld()->SpawnActor<>()` call with a note about
  `bNoCollisionFail` (deprecated in 5.x).

Both inline snippets and sidecar entries get carried in
`CppEmitResult::notes`. The whole-class pipeline lifts those into a
top-level sidecar JSON so the agent can iterate over "what manual
work remains" without re-parsing the source.

## Class-level codegen: `CppClassEmit`

A whole BP needs more than function bodies. `CppClassEmit` layers
on top of `CppEmit` to produce a complete `.h`/`.cpp` pair:

```cpp
struct CppClassEmitResult {
    std::string headerSource;
    std::string implSource;
    std::string headerFileName;  // e.g. "BP_Enemy_Generated.h"
    std::string implFileName;
    std::string className;       // e.g. "ABP_Enemy_Generated"
    nlohmann::json notes;        // unsupported / approximation entries
};

CppClassEmitResult EmitCppClass(const nlohmann::json& bpirClassDoc,
                                CppClassEmitOptions opts = {});
```

The pass adds the UCLASS scaffolding UBT requires:

- `#pragma once` + matching `.generated.h` include
- `UCLASS()` macro with inferred specifiers
- `GENERATED_BODY()`
- `UPROPERTY()` decls for every BP variable, with `Replicated` /
  `EditAnywhere` / `BlueprintReadWrite` / `Category` specifiers
  inferred from the BP's variable metadata
- `UFUNCTION()` decls for every BP function, with `BlueprintCallable`
  and `Category` specifiers
- `.cpp` file with function bodies plus `GetLifetimeReplicatedProps()`
  when any variable is `Replicated`

### Class naming

`<Prefix><Name><Suffix> : public <ParentClass>`. The default
suffix is `_Generated` so the file pair sits **next to** the BP
without trying to replace it: `BP_Enemy` → `ABP_Enemy_Generated`,
parent `AActor`. `PrefixClassName` applies the UE prefix
convention (`A` for actors, `U` for non-actor UObjects) based on
the inferred parent.

Parent header is looked up from a small table for well-known UE
base classes (`Actor` → `GameFramework/Actor.h`, etc.). Unknowns
emit a TODO include — the agent fills in the right header.

### Deliberate gaps

CppClassEmit doesn't try to be a full BP → C++ compiler. It
ignores:

- SCS component initialization (timelines, `CreateDefaultSubobject`
  calls in the constructor).
- Complex per-variable CDO defaults set in the editor's Class
  Defaults panel.
- Native event signature rewrites — BP overrides of
  `ReceiveBeginPlay` don't auto-rewrite into `virtual void
  BeginPlay() override`.

Each of these surfaces as a TODO + sidecar entry. The agent or the
user closes them by hand; the generated file is a starting point,
not a finished port.

## C++ parser: `CppLex` + `CppParse`

The reverse direction (C++ source → BPIR) lives in
`tools/parse/CppLex.{h,cpp}` and `tools/parse/CppParse.{h,cpp}`.

### CppLex

A tiny lexer that produces a token stream from a bare C++ snippet.
Recognized token classes: identifiers, numeric literals (`int`,
`float`, hex, scientific notation), string literals (with the
common escape sequences), char literals, punctuation, the small set
of keywords we care about (`if`, `else`, `for`, `while`, `switch`,
`case`, `default`, `return`, `break`, `continue`, `auto`, `this`,
`true`, `false`, `nullptr`, `Cast`).

Out of scope: the C preprocessor. The caller passes a function body
or a function definition — no `#include`, `#define`, `#if`. Macros
are decorations that wrap the BP-generated scaffold, not part of
the function we're parsing.

### CppParse

A recursive-descent parser that turns the token stream into a BPIR
function doc. Accepted subset:

```
Statements:    if/else, for(auto& x : c), while, switch+case+default,
               return, break, continue, expression-statement,
               variable-declaration (auto/typed locals via Cast).
Expressions:   identifiers, qualified names, literals, function
               calls, member access (. and ->), array index,
               Cast<T>(), this, unary (!, -), binary
               (arithmetic / comparison / logical / assign).
Operator precedence: standard C++ subset.

Out of scope (parser throws with a clear error):
  - The C preprocessor.
  - Templates beyond Cast<T>.
  - Lambdas, decltype, exception machinery.
  - Pointer arithmetic.
```

```cpp
class CppParseError : public std::runtime_error { ... };
nlohmann::json ParseCppFunction(std::string_view source);
nlohmann::json ParseCppFunction(std::string_view source,
                                const nlohmann::json& signature);
```

Errors come with `<line>:<col>: <message>` positions; the agent can
self-correct. The output is run through `ValidateBpir` before
return, so the parser never emits a malformed doc — validation
failures surface as `CppParseError` with the path of the offending
form.

The deliberate "no libclang" stance keeps the dep surface tiny.
Vendoring libclang would balloon the MCP server binary and pull in
LLVM tooling. The cost is that we accept a smaller language; the
benefit is that the parser builds in seconds and ships in a 5 MB
exe. If clang awareness becomes necessary later (full UE-header
awareness, full C++ resolution), `ParseCppFunction` is the seam to
swap.

## Round-trip identity

The three passes compose:

```
decompile_function(bp)               -> BPIR
transpile_function(BPIR)             -> C++
parse_cpp_function(C++)              -> BPIR'
compile_function(BPIR')              -> bp'
```

The interesting equality is `BPIR == BPIR'` for any input BP that
uses only the supported constructs. We pin this with tests:
`smoke-cpp-roundtrip.ps1` runs a fixture BP through the full
pipeline and diffs the resulting BPIR docs. Operator aliases,
type-shorthand normalization, and statement-form canonicalization
all happen before the diff — equality is structural, not textual.

Where the round-trip breaks: anything that decompile emits as
`{unsupported}`. `parse_cpp_function` reads the
`// TODO[bpr-unsupported]` comment back as a comment, not as an
`{unsupported}` form. That's by design — once the user has rewritten
the unsupported region in C++, the round-trip is meaningless. The
sidecar JSON is the audit trail.

## Tool surface

Five tools expose the pipeline:

| Tool | Direction | Input | Output |
|---|---|---|---|
| `decompile_function` | BP → BPIR | asset, function name | BPIR function doc |
| `decompile_blueprint` | BP → BPIR | asset | BPIR class doc |
| `transpile_function` | BPIR → C++ | BPIR function doc | C++ source string |
| `transpile_blueprint` | BPIR → C++ | BPIR class doc | header + impl + sidecar |
| `parse_cpp_function` | C++ → BPIR | C++ source | BPIR function doc |
| `compile_function` | BPIR → BP | BPIR function doc | entry-node GUID |

All five are server-side computations on top of the introspector and
the K2 schema work — no new commandlet ops, no new wire protocol.
Adding a target language (say, Lua) means writing `LuaEmit.cpp` +
`LuaParse.cpp` and registering two more tools. The BPIR pivot has
already done the structural work.

## Checkpoint

Pick any function on `BP_TestEnemy` (the seed BP from Chapter 3):

```jsonc
// transpile_function: BP → C++
{ "tool": "transpile_function",
  "args": {
    "asset_path": "/Game/AI/BP_TestEnemy",
    "function_name": "TakeDamage"
  }
}
```

The response includes a `source` field with the C++ rendering plus
a `notes` array (empty if every node was clean). The source should
compile as a `.cpp` snippet in a UE project that has the right
includes — type names use UE conventions (`int32`, `FString`,
`AActor*`), operator aliases are rendered (`Health -= Damage` not
`UKismetMathLibrary::Subtract_FloatFloat(Health, Damage)`),
indentation is consistent.

Now run `parse_cpp_function` on the returned source:

```jsonc
{ "tool": "parse_cpp_function",
  "args": { "source": "<the C++ from above>" }
}
```

You get a BPIR doc back. Compare it to the BPIR you'd get from
`decompile_function` against the same function:

```jsonc
{ "tool": "decompile_function",
  "args": {
    "asset_path": "/Game/AI/BP_TestEnemy",
    "function_name": "TakeDamage"
  }
}
```

The two docs should be **structurally identical** for any function
that uses only supported constructs. If they differ, the diff
points at exactly where the round-trip lost information — almost
always either an `{unsupported}` form (BP construct without a clean
C++ map) or an operator-alias normalization (an arithmetic op the
table doesn't yet cover).

For the whole-class flow: `transpile_blueprint` produces a `.h` and
`.cpp` pair plus a `transpile-notes.json` sidecar listing
unsupported nodes and approximations. Write the three files to disk
and try to `Build.bat` them as part of a real module — the
generated headers compile against a clean UE project.

You now have an end-to-end BP-to-source pipeline pivoting through a
versioned IR. Chapter 13 is the polish: tests, error diagnostics,
distribution.

See also:
[design/07-bpir-and-transpile.md](../design/07-bpir-and-transpile.md),
[design/06-wire-protocol.md](../design/06-wire-protocol.md).
