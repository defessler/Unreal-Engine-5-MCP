---
name: bp-batches
description: Use this skill when the user wants to make a multi-step Blueprint mutation (more than one write op against the same BP), generate a function from pseudocode, dry-run a batch before committing, or attribute compile errors back to specific ops in a multi-step write. Triggers on phrases like "build a function that...", "in one transaction", "preview / dry run / before I commit", "compile this pseudocode into a Blueprint", "if any step fails, roll back", or explicit mention of `apply_ops`, `preview_ops`, or `compile_function`. Skip for single-step writes (use the individual tool from bp-reader).
---

# bp-batches — multi-step BP writes

For more than one mutation against the same BP, pick:

- **`compile_function`** — pseudocode DSL → fully-wired BP function.
- **`apply_ops`** — explicit op list with named-slot GUID resolution.
- **`preview_ops`** — dry-run an `apply_ops` batch without mutating.

All three collapse N×compile/save into 1 (the batch flushes once at
`EndBatch`). For single-op edits, just call the individual tool — a
batch around one op buys nothing.

| Goal | Tool |
|------|------|
| Function body the agent can describe in pseudocode | `compile_function` |
| Mixed writes: vars + functions + nodes + wires      | `apply_ops` |
| Validate a batch shape before running it           | `preview_ops` |
| Idempotent setup ("ensure these vars exist")        | `apply_ops` |

If you can express the work as pseudocode, **prefer**
`compile_function` — it generates operator-aliased calls (`+`, `==`,
`&&`, …), auto-wires FunctionEntry → first statement, and merges
exec-tails after if/else without a Sequence node.

## `compile_function` — pseudocode → BP

```jsonc
{
  "asset_path":    "/Game/AI/BP_Enemy",
  "function_name": "TakeDamage",
  "inputs":  [{ "name": "Amount", "type": "float" }],
  "body": [
    { "set": "Health",
      "to":  { "call": "-",
               "args": { "A": { "var": "Health" },
                         "B": { "var": "Amount" } } } },
    { "if":   { "call": "<=", "args": { "A": { "var": "Health" },
                                         "B": { "lit": 0 } } },
      "then": [{ "call": "OnDeath" }] }
  ]
}
```

**Statement forms:** `{if, then, [else]}`, `{set, to}`, `{call, args}`,
`{comment}`, plus the BPIR shapes (`return`, `cast`, etc.) that
`compile_function` currently understands — see `compile_function`'s
schema (`tools/list`) for the authoritative list. The BPIR pseudocode
DSL is a *subset* of full BPIR; loops (`for_each` / `while`) and
`switch` lower via `transpile_function` but not yet via
`compile_function` (see bp-debug if a request hits this gap).

**Expression forms:**
- `{var: "name"}` — VariableGet.
- `{lit: <value>}` — literal pin default.
- `{call: "fn", args: {...}}` — CallFunction. Operator aliases for
  KismetMathLibrary: `+ - * / %`, `== != < <= > >=`, `&& || !`, plus
  `+f -f *f /f ==f <f <=f` for float-explicit. For anything else use
  `"Owner::Function"`.

Pass `dry_run: true` to get the compiled op list without executing —
useful for inspection or pasting into an `apply_ops` batch.

## `apply_ops` — arbitrary multi-step

```jsonc
{
  "ops": [
    { "op": "create_blueprint", "asset_path": "/Game/AI/BP_Boss",
      "parent_class": "Actor" },
    { "op": "add_variable", "asset_path": "/Game/AI/BP_Boss",
      "name": "Health", "type": "float" },
    { "op": "add_function", "asset_path": "/Game/AI/BP_Boss",
      "function_name": "TakeDamage" },
    { "op": "add_node", "id": "branch",
      "asset_path": "/Game/AI/BP_Boss", "graph_name": "TakeDamage",
      "kind": "Branch", "x": 200, "y": 0 },
    { "op": "add_node", "id": "getHealth",
      "asset_path": "/Game/AI/BP_Boss", "graph_name": "TakeDamage",
      "kind": "VariableGet", "variable": "Health", "x": 0, "y": 100 },
    { "op": "wire_pins",
      "asset_path": "/Game/AI/BP_Boss", "graph_name": "TakeDamage",
      "from_node": "$getHealth", "from_pin": "Health",
      "to_node":   "$branch",    "to_pin":   "Condition" }
  ],
  "atomic": true
}
```
*(With `atomic: true` and no explicit `on_failure`, this batch is all-or-nothing:
any failing op reverts the whole batch to the pre-call state.)*

### Named slots
Any `add_node` op may carry an `id`. Subsequent ops reference the
minted GUID via `"$<id>"` or `{"ref":"<id>"}` in any node-id field.

### atomic + on_failure
- `atomic: true` (default) — first failure stops the batch.
- `atomic: false` — continue past failures; failed ops appear as
  `{ok:false, error:"..."}` in `results`.
- **`on_failure` defaults to match `atomic`** (so `atomic:true` is truly
  all-or-nothing):
  - `atomic: true` ⇒ default **`rollback`** — the whole batch is reverted to
    the exact pre-batch state on any failure; nothing partial reaches disk.
  - `atomic: false` ⇒ default `compile` — save what landed (you opted into a
    partial result).
- Set `on_failure` explicitly to override the default:
  - `"compile"` — best-effort: compile + save what landed before the failure
    (a *partial* mutation).
  - `"skip"` — discard pending compile + save; nothing reaches disk (in-memory
    daemon state stays dirty until restart).
  - `"rollback"` — cancel the transaction; revert all in-memory mutations to
    the pre-batch state.

### Cascade on failed slots
When a slot-binding op fails in a non-atomic batch, every downstream
op referencing `$<that-slot>` (transitively, if the dependent also
has an `id`) short-circuits with a richer error and a
`cause: "upstream-slot-failed"` tag — so you can distinguish cascades
from native op failures:

```jsonc
{ "ok": false, "op_index": 4,
  "error": "field \"from_node\" references slot \"$mintedNode\", "
           "which was supposed to be bound by an earlier op that "
           "failed: op[2] failed: <original reason>",
  "cause": "upstream-slot-failed" }
```

The dependent op's backend call is skipped entirely — no doomed
round-trip per cascaded op.

### Result shape

```jsonc
{ "ok": true,
  "succeeded": 5, "failed": 0,
  "slots":   { "branch": "<guid1>", "getHealth": "<guid2>" },
  "results": [ /* per-op results, with `cause` on cascades */ ],
  "diagnostics": [
    { "severity": "Warning", "message": "...", "node_guid": "...",
      "asset_path": "...", "op_index": 4 }
  ],
  "compile_errors": 0, "compile_warnings": 1 }
```

`diagnostics[].op_index` attributes compile messages back to specific
ops — "the wire_pins op at index 7 failed type-check on the Branch's
Condition pin" without re-reading the BP.

### Supported ops

The dispatcher accepts: `create_blueprint`, `duplicate_blueprint`,
`add_variable`, `delete_variable`, `rename_variable`,
`retype_variable`, `set_variable_default`, `set_variable_category`,
`add_function`, `add_function_input`, `add_function_output`,
`delete_function`, `add_node`, `wire_pins`, `set_node_position`,
`delete_node`, `set_pin_default`. If a request needs an op not in
this list, fall back to single-tool calls (or extend `ApplyOps.cpp`).

## `preview_ops` — dry-run

Same args as `apply_ops`. Returns per-op `{ok}` plus a
`would_compile` array of asset paths the real run would touch. Uses
read-only backend calls to confirm referenced vars/functions exist.

Use cases: agent self-check before generation, human-in-the-loop
confirmation, CI/lint over a generated batch.

## Failure handling

When `apply_ops` returns `failed > 0`:

1. Look at the first `results[i]` with `ok:false`. The `error`
   string is the actual failure.
2. If `cause: "upstream-slot-failed"`, the root failure is in an
   earlier op — find the first non-cascaded failure first.
3. Cross-reference `diagnostics[]` for compile errors — `op_index`
   points at the offending op.

Common patterns:
- **"Pin type mismatch"** — `wire_pins` between incompatible types.
  Error includes both pin types so you can fix in one turn.
- **"Variable not found"** — name typo, or `add_variable` ordered
  after the consumer.
- **"Asset not found"** — usually a path typo. Surfaces as the
  `AssetNotFound` class.

A partial batch only lands now if you opted into it — `atomic: false`, or an
explicit `on_failure: "compile"`/`"skip"`. The default `atomic: true` path
rolls back automatically (nothing partial reaches disk), so no manual cleanup
is needed. If you did opt into a partial result, either finish with a follow-up
`apply_ops`, or `delete_function`/`delete_variable` to undo manually.
