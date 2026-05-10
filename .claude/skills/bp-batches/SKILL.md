---
name: bp-batches
description: Use this skill when the user wants to make a multi-step Blueprint mutation (more than one write op against the same BP), generate a function from pseudocode, dry-run a batch before committing, or attribute compile errors back to specific ops in a multi-step write. Triggers on phrases like "build a function that...", "in one transaction", "preview / dry run / before I commit", "compile this pseudocode into a Blueprint", "if any step fails, roll back", or explicit mention of `apply_ops`, `preview_ops`, or `compile_function`. Skip for single-step writes (use the individual tool from bp-reader).
---

# bp-batches — multi-step BP writes done right

For more than one mutation against the same BP, the right surface is
one of:

- **`compile_function`** — pseudocode DSL → fully-wired BP function.
- **`apply_ops`** — explicit op list with named-slot GUID resolution.
- **`preview_ops`** — dry-run an `apply_ops` batch without mutating.

All three collapse N×compile/save into 1 (the batch flushes once at
`EndBatch`). For single-op edits, just call the individual tool from
the master `bp-reader` skill — wrapping one op in a batch buys nothing.

## When each tool fits

| Goal | Tool |
|------|------|
| Function body the agent can describe in pseudocode | `compile_function` |
| Spawn nodes + wire them, named-slot refs across ops | `apply_ops` |
| Validate a batch shape before running it | `preview_ops` |
| Mixed writes across variables + functions + nodes | `apply_ops` |
| Idempotent setup ("ensure these vars exist") | `apply_ops` (each op idempotent) |

If you can express the work as pseudocode, **prefer** `compile_function`
— it generates the right operator-aliased calls (`+`, `==`, `&&`, …),
auto-wires FunctionEntry → first statement, and merges exec-tails after
if/else without you writing a Sequence node. Only drop to `apply_ops`
when you need fine-grained control or you're mixing function-creation
with variable / structural changes.

## `compile_function` — pseudocode → BP

```jsonc
{
  "asset_path":    "/Game/AI/BP_Enemy",
  "function_name": "TakeDamage",
  "inputs":  [{ "name": "Amount", "type": "float" }],
  "body": [
    { "if":   { "var": "bIsInvulnerable" },
      "then": [],
      "else": [
        { "set": "Health",
          "to":  { "call": "-",
                   "args": { "A": { "var": "Health" },
                             "B": { "var": "Amount" } } } },
        { "if":   { "call": "<=", "args": { "A": { "var": "Health" },
                                             "B": { "lit": 0 } } },
          "then": [{ "call": "OnDeath" }] }
      ]
    }
  ]
}
```

**Statement forms:** `{if, then, [else]}`, `{set, to}`, `{call, args}`,
`{comment}`. Plus the BPIR extensions (`return`, `cast`, `switch`,
`for_each`, `while`, `sequence`, `break`, `continue`) when invoking
through the same shape — see the bp-cpp skill for the BPIR schema in
full.

**Expression forms:**
- `{var: "name"}` — VariableGet.
- `{lit: <value>}` — literal pin default (string / number / boolean /
  null). Uses `set_pin_default` under the hood.
- `{call: "fn", args: {...}}` — CallFunction. Operator aliases:
  - Math: `+`, `-`, `*`, `/`, `%`
  - Comparison: `==`, `!=`, `<`, `<=`, `>`, `>=`
  - Boolean: `&&`, `||`, `!`
  - Float-explicit: `+f`, `-f`, `*f`, `/f`, `==f`, `<f`, `<=f`
  - Or full `"Owner::Function"` for any other call.

The function's `K2Node_FunctionEntry` is auto-wired into the first
statement's exec input. Exec from BOTH branches of an if/else converges
into the next statement (UE's K2 schema accepts multiple sources on
exec input pins — no Sequence/Join node needed).

Pass `dry_run: true` to get the compiled op list **without** executing.
Useful for inspection or pasting into an `apply_ops` batch.

## `apply_ops` — arbitrary multi-step

```jsonc
{
  "ops": [
    { "op": "create_blueprint", "asset_path": "/Game/AI/BP_Boss", "parent_class": "Actor" },
    { "op": "add_variable", "asset_path": "/Game/AI/BP_Boss", "name": "Health", "type": "float" },
    { "op": "add_function", "asset_path": "/Game/AI/BP_Boss", "function_name": "TakeDamage" },
    { "op": "add_function_input", "asset_path": "/Game/AI/BP_Boss",
      "function_name": "TakeDamage", "param_name": "Amount", "type": "float" },
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
  "atomic": true,
  "on_failure": "compile"
}
```

### Named slots

Any `add_node` op may carry an `id` field. Subsequent ops reference
that node's GUID via `"$<id>"` or `{"ref":"<id>"}` in any node-id
field. Eliminates the need to thread minted GUIDs through the agent's
reasoning.

### atomic + on_failure semantics

- `atomic: true` (default in most use cases) — first failure stops the
  batch.
- `atomic: false` — continue past failures; failed ops appear as
  `{ok:false, error:"..."}` in the per-op `results`.
- `on_failure: "compile"` (default) — best-effort: compile + save what
  landed before the failure. Matches the equivalent unbatched
  sequence's observable outcome.
- `on_failure: "skip"` — discard the pending compile + save. Nothing
  reaches disk. The in-memory daemon state stays dirty (subsequent
  reads in the same daemon session can see partial mutations) until
  restart; documented limitation of strict-atomic mode.

### Result shape

```jsonc
{ "ok": true,
  "succeeded": 5, "failed": 0,
  "slots":   { "branch": "<guid1>", "getHealth": "<guid2>" },
  "results": [ /* per-op results */ ],
  "diagnostics": [
    { "severity": "Warning", "message": "...", "node_guid": "...",
      "asset_path": "...", "op_index": 4 }
  ],
  "compile_errors": 0,
  "compile_warnings": 1 }
```

**`diagnostics[].op_index`** points at which op produced the offending
node — lets you say "the wire_pins op at index 7 failed type-check on
the Branch's Condition pin" without re-reading the BP.

### Supported ops

`create_blueprint`, `add_variable`, `delete_variable`,
`rename_variable`, `set_variable_default`, `add_function`,
`add_function_input`, `add_function_output`, `delete_function`,
`add_node`, `wire_pins`, `set_node_position`, `delete_node`,
`set_pin_default`. (Not every individual write tool is batchable yet —
if a user asks for one not on this list, fall back to a sequence of
single-tool calls.)

## `preview_ops` — dry-run before committing

Same args as `apply_ops`. Returns per-op `{ok}` plus a
`would_compile` array of asset paths the real run would touch. Uses
read-only backend calls to confirm referenced vars/functions exist.

Use cases:
- Agent self-check before running a multi-op generation.
- Human-in-the-loop confirmation step ("here's what I'd do; OK?").
- CI/lint pass over a generated batch.

If `preview_ops` reports an error and the user wants to proceed
anyway, surface the specific failure rather than blindly retrying with
`apply_ops`.

## Patterns that work

### "Ensure this BP has these variables"

`apply_ops` with idempotent `add_variable` ops. Each returns
`already_existed:true` instead of failing if the var is already there.
Atomic isn't load-bearing here — repeated runs converge to the same
state.

### "Build a small function with one branch"

`compile_function`. One call, one recompile, automatic exec wiring.

### "Build a function that calls into another BP's interface"

`compile_function` with `{"call": "Owner::Function", "args": {...}}` —
the operator aliases only cover KismetMathLibrary; full qualified
names work for any other call.

### "Generate, then verify"

`apply_ops` or `compile_function` followed by `read_blueprint` /
`get_graph` to confirm. If `apply_ops` returned `slots`, you already
know the GUIDs — `get_node` is cheaper than re-fetching the whole
graph.

### "Try it in dry-run, then commit"

`preview_ops` → human confirmation → `apply_ops` with the same
arguments. The two share an op-list shape exactly.

## Failure handling

When `apply_ops` returns `failed > 0`:

1. Look at the first `results[i]` with `ok:false`. Its `error` string
   is the actual failure.
2. Cross-reference with `diagnostics[]` if the failure was a compile
   error — `op_index` points at the offending op.
3. Common patterns:
   - "Pin type mismatch" — `wire_pins` between incompatible types.
     The error includes both pin types so you can fix in one turn.
   - "Variable not found" — a `VariableGet`/`VariableSet` referenced
     a name that doesn't exist (typo, or the `add_variable` op was
     ordered after the consumer).
   - "Asset not found" — usually a path typo. Distinguishable from
     other errors via `AssetNotFound` class name.

If a partial batch landed (`atomic: false` or `on_failure: "compile"`
with mid-batch failure), the BP may be in a half-built state. Either
finish with a follow-up `apply_ops`, or `delete_function` /
`delete_variable` to roll back manually.
