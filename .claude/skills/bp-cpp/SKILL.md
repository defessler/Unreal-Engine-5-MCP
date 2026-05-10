---
name: bp-cpp
description: Use this skill when the user wants to convert Blueprints to or from C++ source — for code review, promoting a prototype BP to native, materializing a BP from hand-written C++, or analyzing a BP via its JSON AST (BPIR). Triggers on phrases like "convert this BP to C++", "BP to native", "promote to C++", "transpile this Blueprint", "I wrote this in C++, make it a BP", "decompile the function", "BPIR", or any explicit mention of `decompile_function`, `decompile_blueprint`, `transpile_function`, `transpile_blueprint`, `parse_cpp_function`, `write_generated_source`. Skip for direct BP-only edits (use bp-reader / bp-batches).
---

# bp-cpp — BP ↔ C++ round-trip pipeline

Six tools, one shared pivot. The pivot is **BPIR** — a versioned JSON
AST (`{kind: "function", body: [stmts...], inputs, outputs, ...}`)
that every transpile tool operates on. BP graphs lower into BPIR, BPIR
lowers into source, source parses back into BPIR. Adding Lua / Python
/ JS later is another codegen + parser pair against the same IR.

```
BP graph  ⇄  BPIR (JSON AST)  ⇄  C++ source
              ▲
              │
         compile_function (BPIR → BP, the canonical write surface)
```

## Six tools, mapped to direction

| Direction | Single function | Whole class |
|-----------|-----------------|-------------|
| **BP → BPIR** | `decompile_function` | `decompile_blueprint` |
| **BPIR → C++** | `transpile_function` | `transpile_blueprint` |
| **C++ → BPIR** | `parse_cpp_function` | (not yet) |
| **BPIR → BP** | `compile_function` (see bp-batches) | (composes above) |
| **C++ → disk** | — | `write_generated_source` |

`transpile_function` and `transpile_blueprint` compose decompile +
codegen for you, so the typical "show me this BP as C++" call is one
tool, not two.

## When to use each

### "Show me this BP function as C++"

`transpile_function`. Default `mode=readable` — annotated C++ with
real type names, operator-aliased math (`a + b` instead of
`KismetMathLibrary::Add_IntInt(a, b)`), unsupported nodes as
`// TODO[bpr-unsupported]` comments + a `notes` array.

```jsonc
{ "asset_path":    "/Game/AI/BP_TestEnemy",
  "function_name": "TakeDamage",
  "target_lang":   "cpp",
  "use_operator_aliases": true }
```

Returns:
```jsonc
{ "ok": true,
  "source": "void TakeDamage(float Amount) { ... }",
  "notes":  [],
  "unsupported_count": 0 }
```

Set `use_operator_aliases: false` to keep the canonical
`UKismetMathLibrary::Add_IntInt` calls — useful when the user wants
something they can copy as-is into existing UE codebases that prefer
explicit calls.

### "Promote this BP to compilable C++"

`transpile_blueprint` for the whole class, then `write_generated_source`
×2 to drop `.h` and `.cpp` into `<Project>/Source/<Module>/Generated/`.

```jsonc
{ "asset_path":         "/Game/AI/BP_Enemy",
  "target_lang":        "cpp",
  "module_api_macro":   "MYGAME_API",
  "class_name_suffix":  "_Generated" }
```

The result includes:
- `class_name` — UE-prefix-applied name (`BP_Enemy` extending Actor →
  `ABP_Enemy_Generated`). Pass `class_name_suffix: ""` to drop in
  place of the BP entirely.
- `header_source` / `impl_source` — `.h` and `.cpp` strings with
  UCLASS / UFUNCTION / UPROPERTY decoration, replicated-property
  registration, and operator-aliased function bodies.
- `header_file` / `impl_file` — suggested filenames.
- `sidecar` — JSON listing every unsupported / approximation node
  encountered (timelines, latent actions, anim graphs, ...) plus
  `manual_steps` for triage.
- `sidecar_file` — suggested filename for the sidecar
  (`<Class>.transpile-notes.json`).

Then write each:

```jsonc
write_generated_source {
  "path":        "<projectDir>/Source/<Module>/Generated/<header_file>",
  "content":    "<header_source>",
  "create_dirs": true
}
```

Repeat for the `.cpp` and the sidecar JSON. After all three are
written, run UBT (or Live Coding) — the BP can then reparent to the
C++ class for hybrid workflows.

### "I wrote this C++ — make it a BP"

`parse_cpp_function` to get BPIR, then pipe through `compile_function`
to materialize the graph.

```jsonc
parse_cpp_function {
  "source": "bool TakeDamage(float Damage) { if (bIsAlive) { Health -= Damage; } return true; }"
}
```

Returns `{ok, bpir}`. Then:

```jsonc
compile_function {
  "asset_path":    "/Game/AI/BP_FromCpp",
  "function_name": "<bpir.name>",
  "inputs":        "<bpir.inputs>",
  "outputs":       "<bpir.outputs>",
  "locals":        "<bpir.locals>",
  "body":          "<bpir.body>"
}
```

For a bare body (no signature in the C++ text), pass a `signature`
arg to `parse_cpp_function`:

```jsonc
parse_cpp_function {
  "source":    "{ if (bIsAlive) { Health -= Damage; } return true; }",
  "signature": { "version": 1, "kind": "function", "name": "TakeDamage",
                 "inputs": [{"name":"Damage","type":"float"}],
                 "outputs": [{"name":"ReturnValue","type":"bool"}] }
}
```

### "Just give me the JSON AST"

`decompile_function` to inspect or transform BPIR programmatically.
Useful for cross-BP analysis ("does this function call `OnDeath`?")
without rendering source. The BPIR validator runs before return — a
malformed result surfaces as a clear error rather than corrupt
output.

## What the C++ parser accepts

Subset of C++ designed to round-trip what `transpile_function` emits,
plus reasonable hand-written extensions:

**Statements:** `if`/`else`, range-based `for (auto& x : c)`, `while`,
`switch`+`case`+`default`, `return`, `break`, `continue`,
expression-statement (assignment / call / compound-assign), local
declarations (`auto x = ...` or `int32 x = ...`).

**Expressions:** identifiers, qualified names (`Foo::Bar`), literals,
function calls, member access (`.` and `->`), array index, `Cast<T>()`,
`this`, unary (`!`, `-`), binary operators with full C++ precedence.

**Operator → BPIR canonical name:** `+`, `-`, `*`, `/`, `%`, `==`,
`!=`, `<`, `<=`, `>`, `>=`, `&&`, `||`, `!` map to the canonical
`KismetMathLibrary::*` calls — round-trip identity with CppEmit.

**Out of scope (parser throws with `<line>:<col>: <message>`):**
- The C preprocessor (`#include`, `#define`, `#ifdef`).
- UE macros (`UCLASS`, `UPROPERTY`, `UFUNCTION`) — they're decoration
  for the class scaffold, not function body content.
- Templates beyond `Cast<T>`.
- Lambdas, decltype, exception machinery.
- Pointer arithmetic.

If the user's C++ uses out-of-scope syntax, the error message tells
them exactly which line. Don't try to massage their input — just
surface the line:col diagnostic.

## BPIR — the schema

```jsonc
{ "version": 1,
  "kind": "function" | "class",
  "name": "...",
  "metadata": { ... },         // optional
  "inputs":  [ { "name", "type", ... } ],
  "outputs": [ ... ],
  "locals":  [ ... ],
  "body":    [ /* statements */ ]
}
```

**Statement forms:**

| Form | Shape |
|------|-------|
| `if`/then/else | `{if: <expr>, then: [s], [else: [s]]}` |
| `set` | `{set: "<varName>", to: <expr>, [scope: ...]}` |
| `call` | `{call: "<fn>", [args: {pin: <expr>}]}` |
| `comment` | `{comment: "<text>"}` |
| `return` | `{return: <expr> \| [<expr>...] \| null}` |
| `cast` | `{cast: <expr>, to: "<class>", [as: "<localName>"], success: [s], fail: [s]}` |
| `switch` | `{switch: <expr>, cases: {"<value>": [s], ...}, [default: [s]]}` |
| `for_each` | `{for_each: "<elemName>", in: <expr>, body: [s]}` |
| `while` | `{while: <expr>, body: [s]}` |
| `sequence` | `{sequence: [[s1], [s2], ...]}` |
| `break` / `continue` | `{break: null}` / `{continue: null}` |
| `unsupported` | `{unsupported: {node_class, guid, reason, fields}}` |

**Expression forms:**

| Form | Shape |
|------|-------|
| `var` | `{var: "<name>", [scope: ...]}` |
| `lit` | `{lit: <value>}` (string/number/bool/null) |
| `call` | `{call: "<fn>", [args: {pin: <expr>}]}` |
| `cast` | `{cast: <expr>, to: "<class>"}` (pure, no success/fail) |
| `member` | `{member: <expr>, name: "<field>"}` |
| `index` | `{index: <arr>, idx: <expr>}` |
| `self` | `{self: null}` |
| `new_array` | `{new_array: [<expr>...]}` |
| `new_struct` | `{new_struct: "<type>", fields: {name: <expr>}}` |

Type strings reuse the BP type shorthand (`"float"`, `"object:Actor"`,
`"[]float"`, `"{string:int}"`, ...).

## What lands as TODO + sidecar entries

When a BP construct doesn't map cleanly to compilable C++, the
codegen emits a TODO comment and the sidecar records what manual
work remains. Treatment table (the most common):

| K2 node | Treatment | Sidecar guidance |
|---------|-----------|------------------|
| `K2Node_Timeline` | `UTimelineComponent*` member stub + `UCurveFloat*` UPROPERTY per track + constructor stub | "Manually configure the timeline's curve assets in the editor; auto-binding TBD." |
| `K2Node_LatentAbilityCall` | Best-effort `UAbilityTask_*::Create*` call | "Latent action's exec output (Completed, Cancelled, ...) needs manual binding to OnX delegate." |
| `K2Node_AsyncAction` | Async-action `Create*` call | "Async action's named exec outputs need manual delegate-binding code." |
| `K2Node_SpawnActorFromClass` | `GetWorld()->SpawnActor<>()` | "Pin defaults pre-spawn aren't directly portable; verify SpawnParameters." |
| `K2Node_AnimNode_*` | `// TODO[bpr-anim]` placeholder | "Anim graphs require AnimInstance subclassing; not auto-portable." |
| `K2Node_NiagaraXxx` | Placeholder | "Niagara module-graph; manual port required." |

Surface the sidecar to the user when they're working on a BP that
includes any of these — it's the triage list for "what do I still
need to do by hand?"

## Round-trip identity

For the patterns `transpile_function` emits, parsing back via
`parse_cpp_function` recovers an equivalent BPIR. 12 round-trip cases
in the test suite pin this — covering if/else, cast, switch, for_each,
while, arithmetic, member, index, and nested combinations.

This means: `decompile_function` → `transpile_function` → user edits
the C++ → `parse_cpp_function` → `compile_function` is a real
round-trip workflow. The BP graph after the round-trip is structurally
equivalent (modulo node GUIDs) to the input. Useful when a user wants
to refactor a BP function with code-style edits.

## Common asks → tool sequences

- *"Show me ApplyDamage as C++."* → `transpile_function`.
- *"Generate compilable C++ for this whole BP class."* →
  `transpile_blueprint` → `write_generated_source` (×2 or ×3 with
  sidecar).
- *"I wrote this Tick function in C++, make it a BP."* →
  `parse_cpp_function` → `compile_function`.
- *"Round-trip BP_Foo's TakeDamage through C++ — I want to clean it
  up via code edits."* → `transpile_function` → user edits source →
  `parse_cpp_function` → `compile_function`.
- *"What does this BP function look like as JSON?"* →
  `decompile_function`.
- *"What can't be ported automatically?"* → `transpile_blueprint`,
  inspect `sidecar.unsupported_nodes` and `sidecar.approximations`.
