# BPIR — Blueprint Intermediate Representation

A versioned JSON AST that any source-language frontend (C++, future
Lua/Python/JS) lowers to/from, and that maps 1:1 onto BP graph
operations.

BPIR is the **pivot** for blueprint ↔ source conversions:

```
                  ┌──────────────┐
   BP graph  ⇄   │     BPIR     │   ⇄  source language
                  └──────────────┘
                        ▲
                        │
   compile_function (already does BPIR → BP via apply_ops)
```

## Why BPIR exists

Before BPIR, the project's `compile_function` tool accepted an implicit
DSL — a JSON shape that materialized BP nodes + wires. That DSL works
well as a write-only API ("compile this function in this BP") but isn't
suitable as a read format or as a pivot for source-language conversion:
no version, no schema docs, no validator that catches malformed input
before it reaches the dispatcher.

BPIR formalizes that DSL into a versioned schema with its own validator,
and extends it with the constructs source languages need (return, cast,
loops, switch, member access, etc.).

## Top-level shape

```jsonc
{
  "version": 1,
  "kind":    "function" | "class",
  ...payload...
}
```

`version` is the schema version (currently `1`). Validators reject docs
whose version is higher than they understand. Migration paths exist for
older docs.

`kind` selects between the two top-level payload types.

## Function payload

```jsonc
{
  "version": 1,
  "kind":    "function",
  "name":    "TakeDamage",
  "metadata": {
    "asset_path":   "/Game/AI/BP_Enemy",      // optional
    "return_type":  "void",                   // optional
    "ufunction_specifiers": ["BlueprintCallable", "Category=Combat"]
  },
  "inputs":  [{ "name": "Amount",  "type": "float" }],
  "outputs": [{ "name": "Killed",  "type": "bool"  }],
  "locals":  [{ "name": "Buffer",  "type": "int32" }],
  "body":    [ /* statements */ ]
}
```

- **`name`** (required) — function name in the BP / generated source.
- **`metadata`** (optional) — annotations consumed by codegen but not by
  `compile_function`. Includes asset path (for context), explicit
  return type if differentiable from the outputs[0] convention, and
  UFUNCTION specifier list (for compilable C++ output).
- **`inputs` / `outputs` / `locals`** (optional arrays) — each entry is
  `{name, type, [default], [category], [replicated], [editable]}`.
  `type` uses the [type shorthand](#type-strings) grammar.
- **`body`** (required) — array of [statements](#statement-forms).

## Class payload

```jsonc
{
  "version": 1,
  "kind":    "class",
  "name":    "BP_Enemy",
  "metadata": {
    "asset_path":   "/Game/AI/BP_Enemy",
    "parent_class": "ACharacter",
    "uclass_specifiers": ["Blueprintable", "ClassGroup=AI"]
  },
  "interfaces": ["IDamageable"],
  "variables": [
    { "name": "Health", "type": "float",
      "default": "100.0", "category": "Stats",
      "replicated": true, "editable": true }
  ],
  "functions": [
    { "version": 1, "kind": "function", "name": "TakeDamage", "body": [...] }
  ]
}
```

The class doc embeds full function docs (with their own `version` /
`kind` headers) under `functions`. Each is independently validatable.

## Statement forms

Statements are JSON objects; the first key matching one of the known
form names selects the dispatch. Order in the validator is fixed but
shouldn't matter in practice — no two forms share a key.

### Existing (already in `compile_function`)

```jsonc
{ "if": <expr>, "then": [stmts], "else": [stmts] }
```
`then` / `else` arrays are optional (empty branches are allowed).
Materializes a `K2Node_IfThenElse`.

```jsonc
{ "set": "<varName>", "to": <expr>,
  "scope": "local" | "member" | "input" | "output" }
```
Materializes a `K2Node_VariableSet`. `scope` is optional but
recommended — codegen needs it to disambiguate same-named variables.

```jsonc
{ "call": "<fn>", "args": { "<pinName>": <expr>, ... } }
```
Statement form of a `K2Node_CallFunction` (return value, if any, is
discarded). `<fn>` can be qualified (`"Owner::Func"`) or use an
[operator alias](#operator-aliases).

```jsonc
{ "comment": "<text>" }
```
A node comment / region marker. No exec node spawned; codegen emits
the comment in the source.

### New in BPIR (extending compile_function)

```jsonc
{ "return": <expr> | [<expr>, ...] | null }
```
Connects to a `K2Node_FunctionResult`. Single-value, multi-value, or
bare-return forms.

```jsonc
{ "cast": <expr>, "to": "<class>",
  "as": "<localName>",
  "success": [stmts], "fail": [stmts] }
```
Materializes a `K2Node_DynamicCast` with both exec branches. `as` names
the local variable bound to the cast result inside the success branch.
**`success` and `fail` are required** (use `[]` for empty).

```jsonc
{ "switch": <expr>,
  "cases": { "<value>": [stmts], ... },
  "default": [stmts] }
```
Materializes a `K2Node_Switch*` (subclass picked by the expression's
type). `cases` keys are stringified value literals.

```jsonc
{ "for_each": "<elemName>", "in": <expr>, "body": [stmts] }
```
ForEachLoop macro. `<elemName>` is the loop-element local variable.
`in` should evaluate to an array.

```jsonc
{ "while": <expr>, "body": [stmts] }
```
WhileLoop macro. `<expr>` evaluates each iteration; loop exits when
false.

```jsonc
{ "sequence": [ [stmts], [stmts], ... ] }
```
Explicit `K2Node_ExecutionSequence`. Each inner array is one branch
(`Then 0`, `Then 1`, ...).

```jsonc
{ "break": null }
{ "continue": null }
```
Loop-control statements. The `null` value is conventional and ignored.

```jsonc
{ "unsupported": {
    "node_class": "K2Node_Timeline",
    "guid": "...",
    "reason": "no C++ equivalent",
    "fields": { ... }
} }
```
Safety valve for BP nodes that BPIR can't represent cleanly. Decompile
emits these so the agent can see what was skipped; codegen emits a
`TODO[bpr-unsupported]` comment plus a sidecar JSON entry. **Required
field**: `node_class` (string). All others optional.

## Expression forms

```jsonc
{ "var": "<name>",
  "scope": "local" | "member" | "input" | "output" }
```
`K2Node_VariableGet`. `scope` is optional but recommended.

```jsonc
{ "lit": <value> }
```
A pin literal default. `<value>` must be a JSON scalar (string, number,
boolean, null). Materialized via `set_pin_default` against the consumer
pin — no node spawn.

```jsonc
{ "call": "<fn>", "args": { ... } }
```
Expression form of `K2Node_CallFunction` — the return value is the
expression's value. Same `args` shape as the statement form.

```jsonc
{ "cast": <expr>, "to": "<class>" }
```
Pure (expression-only) cast. Result is the cast value or null.

```jsonc
{ "member": <expr>, "name": "<field>" }
```
Struct member access or property access. Materialized via
`K2Node_BreakStruct` or a property-getter call depending on the
expression's type.

```jsonc
{ "index": <arr>, "idx": <expr> }
```
Array element or map lookup. Materialized via `Array_Get` /
`Map_Find`.

```jsonc
{ "self": null }
```
`K2Node_Self` — `this` reference.

```jsonc
{ "new_array": [<expr>, ...] }
```
`K2Node_MakeArray`.

```jsonc
{ "new_struct": "<type>", "fields": { "<name>": <expr>, ... } }
```
`K2Node_MakeStruct`.

## Type strings

BPIR types use the existing shorthand grammar from
`tools/TypeShorthand.cpp`:

| Shorthand | C++ |
|-----------|-----|
| `"bool"` | `bool` |
| `"int"` | `int32` |
| `"int64"` | `int64` |
| `"byte"` | `uint8` |
| `"float"` | `float` |
| `"double"` | `double` |
| `"string"` | `FString` |
| `"name"` | `FName` |
| `"text"` | `FText` |
| `"object:T"` | `T*` (with proper U/A prefix) |
| `"class:T"` | `TSubclassOf<T>` |
| `"interface:T"` | `TScriptInterface<IT>` |
| `"struct:T"` | `T` (value type) |
| `"enum:T"` | `T` |
| `"[]T"` | `TArray<T>` |
| `"{}T"` | `TSet<T>` |
| `"{K:V}"` | `TMap<K, V>` |

The canonical BPPinType object form (`{category, sub_category, ...}`)
is also accepted everywhere a type string is — the parser
auto-detects.

## Operator aliases

`{call: "<op>"}` — both statement and expression form — accepts
operator shorthand that maps to `UKismetMathLibrary::*`:

```
+, -, *, /, %             (default int variants;
                           use `+f`, `-f` etc. for float)
==, !=, <, <=, >, >=      (`==f`, `<f`, `<=f` for float)
&&, ||, !
```

Or the canonical `"Owner::Function"` form for any other CallFunction.

## Validation

The server-side validator (`tools::ValidateBpir(json)`) is called
before any consumer (codegen, decompile, compile_function). On failure
it throws `std::invalid_argument` with a path-prefixed message:

```
BPIR at 'body[3].cast': missing "success" branch (use {} for empty)
BPIR at 'body[0].to.args.A.var': field "var" must be a string
BPIR at '': BPIR doc requires integer "version" field
```

The path uses `[i]` for array indices and `.field` for object
properties. Errors at the root use empty path.

## Schema versioning

When a breaking change happens (form added with new required field,
field renamed), bump `kBpirSchemaVersion` and add a migration step in
`MigrateToCurrent()` that brings older docs forward. v1 is the
inaugural schema; v2+ is future work.

## Reference

- Schema validator: `Plugins/BlueprintReader/mcp-server/src/tools/Bpir.{h,cpp}`
- Tests: `Plugins/BlueprintReader/mcp-server/tests/test_bpir.cpp`
- The compile_function tool consumes BPIR `body[]` arrays:
  `tools/CompileFunction.cpp`
- Type shorthand grammar: `tools/TypeShorthand.cpp`
