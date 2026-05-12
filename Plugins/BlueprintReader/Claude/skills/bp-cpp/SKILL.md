---
name: bp-cpp
description: Use this skill when the user wants to convert Blueprints to or from C++ source — for code review, promoting a prototype BP to native, materializing a BP from hand-written C++, or analyzing a BP via its JSON AST (BPIR). Triggers on phrases like "convert this BP to C++", "BP to native", "promote to C++", "transpile this Blueprint", "I wrote this in C++, make it a BP", "decompile the function", "BPIR", or any explicit mention of `decompile_function`, `decompile_blueprint`, `transpile_function`, `transpile_blueprint`, `parse_cpp_function`, `write_generated_source`. Skip for direct BP-only edits (use bp-reader / bp-batches).
---

# bp-cpp — BP ↔ C++ round-trip pipeline

Six tools, one shared pivot: **BPIR**, a versioned JSON AST
(`{kind, body: [stmts...], inputs, outputs, ...}`). BP graphs lower
into BPIR, BPIR lowers into source, source parses back into BPIR.
Adding another target language is just another codegen + parser pair
against the same IR.

```
BP graph  ⇄  BPIR (JSON AST)  ⇄  C++ source
              ▲
              │
         compile_function (BPIR → BP — see bp-batches)
```

| Direction | Single function | Whole class |
|-----------|-----------------|-------------|
| **BP → BPIR**  | `decompile_function` | `decompile_blueprint` |
| **BPIR → C++** | `transpile_function` | `transpile_blueprint` |
| **C++ → BPIR** | `parse_cpp_function` | (not yet) |
| **BPIR → BP**  | `compile_function` (in bp-batches) | (composes above) |
| **C++ → disk** | — | `write_generated_source` |

`transpile_function` / `transpile_blueprint` compose decompile + codegen
internally, so the typical "show me this BP as C++" is one call.

## When to use each

### "Show me this BP function as C++"
`transpile_function`. Default `mode=readable` — annotated C++ with
real type names, operator-aliased math (`a + b` not
`KismetMathLibrary::Add_IntInt(a, b)`), unsupported nodes as
`// TODO[bpr-unsupported]` comments + a `notes[]` array.

```jsonc
{ "asset_path":    "/Game/AI/BP_TestEnemy",
  "function_name": "TakeDamage",
  "target_lang":   "cpp",
  "use_operator_aliases": true }
```

Set `use_operator_aliases: false` for the canonical
`UKismetMathLibrary::Add_IntInt` style — useful when pasting into a
codebase that prefers explicit calls.

### "Promote this BP to compilable C++"
`transpile_blueprint` for the whole class, then `write_generated_source`
twice (`.h`, `.cpp`) + once for the sidecar.

```jsonc
{ "asset_path":         "/Game/AI/BP_Enemy",
  "target_lang":        "cpp",
  "module_api_macro":   "MYGAME_API",
  "class_name_suffix":  "_Generated" }
```

Returns:
- `class_name` — UE-prefix-applied (e.g. `ABP_Enemy_Generated`). Pass
  empty `class_name_suffix` to drop in place of the BP entirely.
- `header_source` / `impl_source` — `.h` / `.cpp` strings with
  UCLASS / UFUNCTION / UPROPERTY decoration, replicated-property
  registration, operator-aliased function bodies.
- `header_file` / `impl_file` — suggested filenames.
- `sidecar` — JSON listing every unsupported/approximation node
  (timelines, latent actions, anim graphs, …) plus `manual_steps`.
- `sidecar_file` — `<Class>.transpile-notes.json`.

Then `write_generated_source` each file under
`<Project>/Source/<Module>/Generated/`, run UBT or Live Coding, and
reparent the BP to the C++ class for hybrid workflows.

### "I wrote this C++ — make it a BP"
`parse_cpp_function` → `compile_function`.

```jsonc
parse_cpp_function {
  "source": "bool TakeDamage(float Damage) { if (bIsAlive) { Health -= Damage; } return true; }"
}
```

Returns `{ok, bpir}`. Hand `bpir` to `compile_function` (see
bp-batches for the call shape).

For a bare body (no C++ signature), pass `signature`:

```jsonc
parse_cpp_function {
  "source":    "{ if (bIsAlive) { Health -= Damage; } return true; }",
  "signature": { "version": 1, "kind": "function", "name": "TakeDamage",
                 "inputs":  [{"name":"Damage","type":"float"}],
                 "outputs": [{"name":"ReturnValue","type":"bool"}] }
}
```

### "Just give me the JSON AST"
`decompile_function` — inspect or transform BPIR programmatically.
Useful for cross-BP analysis ("does this function call `OnDeath`?")
without rendering source. BPIR validator runs before return; malformed
output surfaces as a clear error.

## BPIR schema

For the canonical shape (statement + expression forms, type strings,
metadata fields), see `wiki/BPIR.md` — the wiki is the authoritative
schema reference. Quick reminders:

- Type strings reuse BP type shorthand: `"float"`, `"object:Actor"`,
  `"[]float"`, `"{string:int}"`, …
- `unsupported` statements carry the offending K2 node class + reason;
  CppEmit lowers them to `// TODO[bpr-unsupported]` + a sidecar entry.

## C++ parser — what it accepts

Subset of C++ designed to round-trip what `transpile_function` emits
plus reasonable hand-written extensions:

- **Statements:** `if`/`else`, range-`for`, `while`, `switch`/`case`/`default`,
  `return`, `break`, `continue`, expression-statement, local declarations
  (`auto x = ...`, `int32 x = ...`).
- **Expressions:** identifiers, `Foo::Bar`, literals, calls,
  `.` / `->`, array index, `Cast<T>()`, `this`, unary `!`/`-`,
  binary operators with full C++ precedence.
- **Operator → BPIR canonical name:** `+ - * / %`, `== != < <= > >=`,
  `&& || !` map to canonical `KismetMathLibrary::*` calls — round-trip
  identity with CppEmit.

Out of scope (parser throws `<line>:<col>: <message>`):
preprocessor, UE macros (`UCLASS`/`UPROPERTY`/`UFUNCTION` — they're
class-scaffold decoration, not body content), templates beyond
`Cast<T>`, lambdas, decltype, exceptions, pointer arithmetic.

Don't try to massage the user's input — surface the line:col
diagnostic directly.

## Unsupported nodes — TODO + sidecar

When a BP construct doesn't map cleanly to compilable C++, codegen
emits a TODO and the sidecar records what's left to do by hand.
Common patterns:

| K2 node | Treatment | Sidecar |
|---------|-----------|---------|
| `K2Node_Timeline` | `UTimelineComponent*` member stub | Configure curve assets in editor |
| `K2Node_LatentAbilityCall` / `K2Node_AsyncAction` | TODO comment | Bind named exec outputs to delegates manually |
| `K2Node_AnimNode_*` / `K2Node_NiagaraXxx` | TODO comment | Manual port required (AnimInstance / Niagara) |
| Anything else | TODO comment with offending K2 class | Manual port; add a treatment-table entry if it's common |

**Not** on this list: `K2Node_SpawnActorFromClass`, `K2Node_AddComponent`.
Decompile recognizes them as structured BPIR calls; CppEmit lowers to
real `GetWorld()->SpawnActor<T>(...)` / `NewObject + RegisterComponent`
blocks. No TODO needed.

Surface the sidecar to the user when their BP has any unsupported
nodes — it's the triage list for "what do I still need to do by hand?"

## Round-trip identity

For the patterns `transpile_function` emits, parsing back via
`parse_cpp_function` recovers equivalent BPIR. The test suite pins
this with round-trip cases covering if/else, cast, switch, for_each,
while, arithmetic, member, index, and nested combinations.

That makes this workflow real:
`decompile_function` → `transpile_function` → user edits the C++ →
`parse_cpp_function` → `compile_function`. The BP after the round-trip
is structurally equivalent (modulo node GUIDs) to the input — useful
for refactoring BP via code-style edits.
