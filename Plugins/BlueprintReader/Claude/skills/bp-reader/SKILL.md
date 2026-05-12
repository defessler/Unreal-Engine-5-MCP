---
name: bp-reader
description: Use this skill when the user asks to inspect, edit, transpile, or operate on Unreal Engine 5 Blueprint assets and the surrounding project through the bp-reader MCP server. Triggers on phrases like "the blueprint", "BP_*", "/Game/...", "add a variable", "wire these pins", "what does this blueprint do", "convert this BP to C++", "compile this pseudocode", "spawn an actor", "move/rename/delete this asset", "save all", "read a DataTable", "run a console command", "start/stop PIE", "Live Coding compile", "run automation tests", or any explicit mention of bp-reader tools. Skip for non-blueprint UE topics like raw C++ source unrelated to BPs, build scripts, or .ini files.
---

# bp-reader — using the Blueprint MCP tools

The `bp-reader` MCP server reads, mutates, and round-trips
`.uasset` Blueprint files in a UE 5.7.4 project. This skill covers
*how to use them well*. For *building or maintaining* the server,
see the project's CLAUDE.md.

Specialized sub-skills handle:
- **bp-batches** — `apply_ops`, `compile_function`, `preview_ops`.
- **bp-cpp** — BP↔C++ transpile (`decompile_*`, `transpile_*`, `parse_cpp_function`).
- **bp-debug** — triaging tool-call errors, backend transport failures.

## Discovering tools

The MCP server has grown past what's worth enumerating in this file.
**Call `tools/list` first** for current schemas, and use the
discoverability tools for option sets:

- `list_node_kinds` — valid `kind` values for `add_node` (with extras).
- `list_pin_categories` — valid `BPPinType.category` values.

If a request needs an `add_node` kind not in `list_node_kinds`, that
kind has to be added to the plugin's `RunAddNodeOp` first. Say so
directly rather than guessing.

## Wire format basics

**Paths**: always package paths (`/Game/AI/BP_Foo`), never object
paths (`/Game/AI/BP_Foo.BP_Foo`) and never disk paths.

**`BPPinType`** — every typed surface:
```json
{ "category":            "real",          // bool | int | real | string | object | struct | class | interface | enum | ...
  "sub_category":        "float",         // optional
  "sub_category_object": null,            // class / struct / enum path when applicable
  "is_array": false, "is_set": false, "is_map": false }
```

**Type shorthand** (every write tool that takes a `type` accepts both):

| Shorthand              | Expands to |
|------------------------|------------|
| `"bool"` / `"int"` / `"string"`   | `{category:"<that>"}` |
| `"float"` / `"double"`            | `{category:"real", sub_category:"float|double"}` |
| `"object:Actor"`                  | `{category:"object", sub_category_object:"<UClass path>"}` |
| `"struct:FVector"`                | `{category:"struct", sub_category_object:"<UScriptStruct path>"}` |
| `"interface:IDamageable"`         | `{category:"interface", sub_category_object:"<UInterface path>"}` |
| `"enum:EWeaponType"`              | `{category:"enum", sub_category_object:"<UEnum path>"}` |
| `"[]float"` / `"{}int"` / `"{string:int}"` | array / set / map of the inner type |

**Pin IDs are GUIDs** (e.g. `8255EA22-4BA1-...`). Prefer them over
names when wiring across multiple calls — they're stable. For a wire
right after `add_node`, names work too (the response includes pin GUIDs).

**`BPNode.meta`** is a real nested JSON object. Useful keys:
`kind`, `targetFunction`, `variableName`, `eventName`, `isPureCast`,
`castBroken` ("true" for DynamicCast with a missing target class).

## Orient cheaply, then commit

Pick the lightest tool that answers the question:

- *"Is this a real BP?"* → `summarize_blueprint` (parent + counts only).
- *"What's in this BP?"* → `read_blueprint` with a `fields` projection.
- *"What touches X?"* → `find_node` with `kind` + `query`.

Every read tool accepts `fields`, `limit`, `offset` — AI clients pay
tokens per byte. Use them.

## Reading a blueprint — which tool

- **`summarize_blueprint`** — counts only. Always cheap.
- **`read_blueprint`** — overview: parent, interfaces, variables,
  function names, graph names.
- **`get_graph`** — full node + connection topology of one graph.
  Defaults to `EventGraph`; pass `graph_name` for function/macro
  graphs or `UserConstructionScript` (UE 5's name for ConstructionScript).
- **`get_function`** — one function's signature *and* body. Don't
  use `get_graph` for this; `get_function` parses the entry/result
  pins into a typed signature for you.
- **`list_variables`** — just the member-variable table.
- **`get_components`** — SCS component hierarchy.
- **`find_node`** — "where is X used?". Matches on class, title, AND
  meta identifier fields (`targetFunction`, `function_name`,
  `variableName`, `eventName`) — so searching `"Greater_FloatFloat"`
  finds operator nodes rendered as `"Greater (float)"`, and
  `"ReceiveBeginPlay"` finds the `K2Node_Event` titled `"Event
  BeginPlay"`. Optional `kind` filter narrows by K2 extras. **Each
  hit carries `graph_name` + `graph_type`**, so you can pipe results
  straight into `get_node` / `delete_node` / `wire_pins` without
  re-reading the BP.
- **`get_node`** — one node by GUID. Each pin in the response carries
  an inline `linked_to: [{node_id, pin_id, pin_name}, ...]` so you
  can verify wiring without a follow-up `get_graph`. DynamicCast
  zombies (target class deleted / not compiled) carry
  `meta.castBroken: "true"` — use that, not title-matching on
  "Bad cast node".
- **`find_overriders`** — cross-BP structural query ("every BP that
  overrides BeginPlay"). Replaces the manual `list_blueprints` +
  N×`read_blueprint` loop.

## Writing — pick the right surface

For a single mutation, call the individual write tool. Beyond that:

- **`compile_function`** if the function body fits BPIR pseudocode
  (`if`/`set`/`call`/`var`/`lit` + math/comparison aliases).
- **`apply_ops`** for arbitrary multi-step writes. Named-slot GUID
  resolution (`id` + `$ref`) threads minted GUIDs across ops. Single
  recompile + save at batch end.
- **`preview_ops`** dry-runs the same batch shape against read-only
  calls.

Detailed patterns + on_failure + cascade diagnostics → **bp-batches**.

### Single mutation: add a variable
```json
add_variable {
  "asset_path": "/Game/AI/BP_TestEnemy",
  "name": "Health",
  "type": "float",
  "default": "100.0",
  "category": "Combat",
  "replicated": true,
  "editable": true
}
```
Idempotent — calling with an existing name returns
`{ok:true, already_existed:true}`. Retry blindly.

### Refactoring helpers
- `rename_variable` — updates references in graphs automatically.
- `retype_variable` — changes a member's type **without** delete+re-add;
  UE rewires every Get/Set in place.
- `delete_variable` — does *not* delete referencing nodes; `find_node`
  for `VariableGet`/`VariableSet` matching the old name first if you
  want to clean those up.
- `duplicate_blueprint` — file-level copy; idempotent on the destination.

## BP ↔ C++ — round-trip pipeline

```
BP graph  ⇄  BPIR (JSON AST)  ⇄  C++ source
              ▲
              │
         compile_function  (BPIR → BP — the canonical write surface)
```

- BP function → readable C++: `transpile_function`.
- BP class → compilable `.h`/`.cpp` with UCLASS scaffolding:
  `transpile_blueprint` → `write_generated_source` ×2.
- C++ → BP: `parse_cpp_function` → `compile_function`.
- BP function as JSON AST: `decompile_function`.

Detailed usage (which tool when, sidecar JSON, the C++ parser subset)
→ **bp-cpp**.

## Performance — auto / live / commandlet

Default backend is `auto` when a `.uproject` is auto-discovered. Auto
probes per call:
- Editor open → routes to **live** (~5–30 ms / call).
- Editor closed → routes to **commandlet daemon** (~5 s cold start,
  then ~15–30 ms / call within the same MCP session).

Don't batch for latency — batch for **atomicity + single recompile**.
If a single call takes >1 s, something's off (editor crashed, daemon
transport fell back to one-shot). Inspect `_meta.elapsed_ms` on the
response envelope.

## Errors + edge cases

- **`AssetNotFound`** — asset/graph/function/node missing. Treat as
  user error (typo) rather than infra failure.
- **Mock backend is read-only.** Write tools throw with a message
  pointing at `BP_READER_BACKEND=commandlet`. Surface that — don't
  silently fail.
- **Compile failure during a write** surfaces as `BlueprintReaderError`
  with `exit=5`; the save doesn't commit.
- **Commandlet write fail** is classified into three actionable
  cases: file-lock, non-Blueprint asset, uncompiled parent class.
  Triage details in **bp-debug**.

## What the MCP doesn't expose

Say so directly rather than attempting a workaround that fails:

- **Node kinds outside `list_node_kinds`** — `add_node` only supports
  what's enumerated there. User can still create those in the editor UI.
- **Per-call-site cross-asset search** — `find_overriders` covers
  parent/interface/function-name structural queries, but not "every
  BP that *calls* MyFunc".
- **Editor-only ops** — undo/redo, hot-reload, content browser
  drags. The MCP only sees compiled `.uasset` reads/writes.

## Patterns worth memorizing

- *"What does BP_X do?"* — usually fastest as `transpile_function` on
  each function (most human-skimmable). Fall back to `read_blueprint`
  + `get_graph` if you need the literal node-level shape.
- *"Find every PrintString call"* — `find_node` `query:"PrintString"`
  + `kind:"CallFunction"`. The meta-field match means you can also
  search by underlying function name even when the title is humanized.
- *"Make BP_Boss from BP_Enemy"* — `duplicate_blueprint`, then mutate.
- *"Wire X to Y"* — for 1–2 wires, `add_node` + `wire_pins` is fine.
  For 3+ nodes, switch to `compile_function` or `apply_ops`.
