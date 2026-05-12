---
name: bp-reader
description: Use this skill when the user asks to inspect, edit, build, transpile, or operate on Unreal Engine 5 Blueprint assets and the surrounding project through the bp-reader MCP server. Triggers on phrases like "the blueprint", "BP_*", "/Game/...", "add a variable", "wire these pins", "what does this blueprint do", "convert this BP to C++", "compile this pseudocode into a function", "spawn an actor", "move/rename/delete this asset", "save all", "read a DataTable", "run a console command", "set a CVar", "start/stop PIE", "Live Coding compile", "get/set the selection", "read the output log", "run automation tests", or any explicit mention of bp-reader tools (list_blueprints, read_blueprint, add_function, wire_pins, apply_ops, compile_function, transpile_function, parse_cpp_function, get_project_metadata, save_all, move_asset, console_command, get_cvar, pie_start, spawn_actor, run_automation_tests, etc.). Skip for non-blueprint UE topics like raw C++ source unrelated to BPs, build scripts, ini files.
---

# bp-reader — using the Blueprint MCP tools

The `bp-reader` MCP server exposes **119 tools** that read, mutate, and
round-trip-to-C++ `.uasset` Blueprint files in a UE 5.7.4 project.
This skill covers *how to use them well* — the wire format, common
patterns, when to prefer one tool over another. For *building or
maintaining* the server itself, read the project's CLAUDE.md.

When a workflow gets specialized — multi-step writes, BP↔C++, error
triage — there are focused sub-skills:

- **bp-batches** — `apply_ops`, `compile_function`, `preview_ops`,
  named-slot GUID resolution.
- **bp-cpp** — `decompile_function`, `transpile_function`,
  `transpile_blueprint`, `parse_cpp_function`, BPIR pivot.
- **bp-debug** — diagnosing tool-call errors, mock backend pitfalls,
  daemon transport failures.

## The toolbox at a glance

| Category   | Tools |
|------------|-------|
| **Read**   | `list_blueprints`, `summarize_blueprint`, `read_blueprint`, `get_graph`, `get_function`, `list_variables`, `get_components`, `find_node`, `get_node`, `find_overriders` |
| **Write — variables** | `add_variable`, `delete_variable`, `rename_variable`, `set_variable_default`, `retype_variable`, `set_variable_category` |
| **Write — functions** | `add_function`, `delete_function`, `add_function_input`, `add_function_output` |
| **Write — graph topology** | `add_node`, `delete_node`, `set_node_position`, `wire_pins`, `set_pin_default`, `auto_layout_graph` |
| **Write — assets** | `create_blueprint`, `duplicate_blueprint` |
| **Write — components (SCS)** | `add_component`, `remove_component`, `attach_component`, `set_component_property` |
| **Batch / generation** | `apply_ops`, `preview_ops`, `compile_function` |
| **Transpile (BP↔C++)** | `decompile_function`, `decompile_blueprint`, `transpile_function`, `transpile_blueprint`, `write_generated_source`, `parse_cpp_function` |
| **Project + Content Browser** | `get_project_metadata`, `save_all`, `move_asset`, `delete_asset`, `create_folder`, `list_data_tables`, `read_data_table`, `add_data_row`, `set_data_row_value` |
| **Live editor** | `console_command`, `get_cvar`, `set_cvar`, `pie_start`, `pie_stop`, `live_coding_compile`, `get_selected_actors`, `set_selection`, `spawn_actor`, `set_actor_transform`, `delete_actor`, `read_output_log` |
| **Automation** | `run_automation_tests` |
| **Discoverability** | `list_node_kinds`, `list_pin_categories` |
| **Meta** | `shutdown_daemon` |

If a task touches Blueprints, **call `tools/list` first** to see the
exact schemas — the server may have grown since this skill was written.
And when in doubt about valid `add_node` kinds or pin categories, call
the discoverability tools rather than guessing.

## Before doing real work — orient cheaply

Pick the lightest tool that answers the question:

- **"Is this a real BP?"** → `summarize_blueprint` (parent class +
  counts only — tiny response, fast). Use this **before**
  `read_blueprint` when you don't yet know how big the BP is.
- **"What's in this BP?"** → `read_blueprint` with `fields` projection
  to drop what you don't need. Big BPs can have 100+ variables — pull
  just `variables[].name` first if you're scanning.
- **"What touches X?"** → `find_node` with `kind` + `query`. Use this
  instead of fetching the whole graph and filtering yourself.

Every read tool accepts `fields`, `limit`, `offset` — AI clients pay
tokens for every byte. Use them.

## Wire format you'll see and emit

All paths are **package paths**: `/Game/AI/BP_Foo`, never object paths
(`/Game/AI/BP_Foo.BP_Foo`) and never disk paths.

A `BPPinType` looks like:

```json
{
  "category": "real",                          // bool, int, real, string, object, struct, ...
  "sub_category": "float",                     // optional; null OK
  "sub_category_object": null,                 // UClass/UScriptStruct/UEnum path when applicable
  "is_array": false,
  "is_set": false,
  "is_map": false
}
```

For an Actor reference: `{"category":"object","sub_category_object":"/Script/Engine.Actor"}`.
For a Vector: `{"category":"struct","sub_category_object":"/Script/CoreUObject.Vector"}`.
For a `TArray<int>`: `{"category":"int","is_array":true}`.

**Type shorthand** (every write tool that takes a `type` accepts both):

| Shorthand | BPPinType equivalent |
|-----------|---------------------|
| `"bool"` / `"int"` / `"float"` / `"string"` | category-only |
| `"object:Actor"` | object + sub_category_object |
| `"struct:FVector"` | struct + sub_category_object |
| `"interface:IDamageable"` | interface + sub_category_object |
| `"enum:EWeaponType"` | enum + sub_category_object |
| `"[]float"` | float, is_array |
| `"{}int"` | int, is_set |
| `"{string:int}"` | map<string,int> |

`BPNode.meta` is a real nested JSON object (not a string-of-JSON). The
plugin fills in K2 extras like `kind`, `eventName`, `targetFunction`,
`isPureCast` — useful for `find_node` filtering.

Pin IDs are GUIDs (e.g. `8255EA22-4BA1-E744-FABC-E7973E353E57`). Prefer
them over names when wiring across multiple calls — they're stable.
For a single wire right after `add_node`, names work too (the response
includes the full pin list so you know the names exist).

## Reading a blueprint — pick the right tool

- **`summarize_blueprint`** — counts only. Always cheap.
- **`read_blueprint`** — overview: parent class, interfaces, variables,
  function names, graph names. Start here for "what's in this BP?".
- **`get_graph`** — full node + connection topology of one graph.
  Defaults to `EventGraph`; pass `graph_name` for `ConstructionScript`,
  `UserConstructionScript` (UE 5's name), function names, or macro
  names.
- **`get_function`** — one function's signature *and* body. Returns
  `inputs[]`, `outputs[]`, `locals[]`, plus the body graph. Don't
  `get_graph` for this — `get_function` parses the FunctionEntry/Result
  pins to build the typed signature for you.
- **`list_variables`** — just the member-variable table. Cheaper than
  `read_blueprint` when that's all you need.
- **`get_components`** — SCS component hierarchy with parent/child.
- **`find_node`** — "where is X used?". Substring match on class+title,
  and also against `meta.targetFunction` / `meta.function_name` /
  `meta.variableName` / `meta.eventName` — so a search for
  `"Greater_FloatFloat"` finds operator nodes rendered as
  `"Greater (float)"`, and `"ReceiveBeginPlay"` finds the event node
  rendered as `"Event BeginPlay"`. Optional `kind` filter narrows by
  K2 extras (`VariableGet`, `CallFunction`, `Event`, `CustomEvent`,
  `Cast`, ...). Pass empty `query` + a `kind` to enumerate all nodes
  of one kind. **Each hit carries `graph_name` + `graph_type`** so you
  can feed it directly into `get_node` / `delete_node` / `wire_pins`
  without a separate `read_blueprint` to enumerate graphs.
- **`get_node`** — one node by GUID. Cheaper than re-fetching
  `get_graph` after a mutation when all you want is "did the wire
  stick?". Each pin in the response carries an inline `linked_to`
  array (`[{node_id, pin_id, pin_name}, ...]`) so you can verify
  wiring without a separate `get_graph` for the graph-level
  `connections[]` view. DynamicCast nodes with a missing target class
  carry `meta.castBroken: "true"` — that's the reliable detection
  signal for "Bad cast node" zombies (don't title-match).
- **`find_overriders`** — cross-BP structural query. "Every BP that
  overrides BeginPlay" or "every ACharacter implementing IDamageable".
  Replaces the manual `list_blueprints` + N×`read_blueprint` loop.

## Writing — pick the right surface

For a single mutation, call the individual write tool. For anything
multi-step:

- **`compile_function`** if you can express the function body as
  pseudocode (`if`/`set`/`call`/`var`/`lit` + math/comparison aliases).
  Materializes nodes + wires in one call. Don't hand-build graph
  topology if you can write the pseudocode instead.
- **`apply_ops`** for arbitrary multi-step writes. Named-slot GUID
  resolution (`id` + `$ref`) threads minted node GUIDs across ops.
  Single recompile + save at batch end.
- **`preview_ops`** dry-runs the same batch shape against read-only
  backend calls. Surface to the user when they ask "what would that
  do?" before committing.

Detailed patterns + on_failure semantics + diagnostics shape live in
the **bp-batches** skill.

### Single-mutation example: add a variable

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
`{ok:true, already_existed:true}` instead of throwing. Retry blindly.

### Quick graph topology

```
id1 = add_node(asset, "EventGraph", "VariableGet", x, y, {"variable":"bIsAlive"})
id2 = add_node(asset, "EventGraph", "Branch",      x+200, y)
wire_pins(asset, "EventGraph", id1, "bIsAlive", id2, "Condition")
```

`add_node` returns `{ok, node_id, pins}` — pins includes every pin's
GUID so the immediate `wire_pins` can use either names or GUIDs. If
the wire fails the schema's compatibility check, the error includes
both pin types so you can self-correct in one turn.

For more than ~3 nodes, switch to `compile_function` or `apply_ops` —
named slots and single-recompile semantics make those cheaper and
clearer.

### Refactoring

- `rename_variable` updates references in graphs automatically — safe
  to use without re-reading.
- `retype_variable` changes a member variable's type **without**
  delete + re-add. UE rewires every `VariableGet` / `VariableSet` in
  place, so existing graphs survive.
- `delete_variable` does *not* delete referencing nodes — `find_node`
  for `VariableGet`/`VariableSet` matching the old name first if you
  want to clean those up too.
- `duplicate_blueprint` is file-level: source BP → new BP at a new
  path. Idempotent on the destination.

## When to look up rather than hardcode

The set of `add_node` `kind` values has grown, and node kinds + their
required extras are versioned with the plugin. **Don't hardcode the
list from memory** — call `list_node_kinds` to get the current set.
Same for `list_pin_categories` when constructing an unfamiliar
`BPPinType` (especially `struct` / `class` / `interface` where
`sub_category_object` matters).

## BP ↔ C++ — the round-trip pipeline

A versioned **BPIR** (Blueprint Intermediate Representation) JSON AST
sits between BP graphs and source code. The transpile tools all pivot
on it:

```
BP graph  ⇄  BPIR (JSON AST)  ⇄  C++ source
              ▲
              │
         compile_function (BPIR → BP, the canonical write surface)
```

Quick mapping:
- BP function → readable C++: `transpile_function`.
- BP class → compilable `.h`/`.cpp` with UCLASS scaffolding:
  `transpile_blueprint` → `write_generated_source` ×2.
- C++ → BP: `parse_cpp_function` → `compile_function`.
- BP function as analyzable JSON AST: `decompile_function`.

The detailed pattern (which tool to use when, how the sidecar JSON
flags unsupported nodes, what the parser subset accepts) is in the
**bp-cpp** sub-skill.

## Performance — auto / live / commandlet

The MCP server picks a backend at startup; default is `auto` when a
`.uproject` is found. Auto probes per call:
- Editor open → routes to **live** (~5–30 ms per call).
- Editor closed → routes to **commandlet daemon** (~5 s cold start,
  then ~15–30 ms per call within the same MCP session).

So:
- Don't worry about call count. A 20-step refactor (read + edit +
  re-read + wire) costs sub-second after the first call lands. Compose
  workflows naturally rather than batching for performance — batch
  for **atomicity + single recompile**, not for latency.
- If a single call is taking >1 s, something's off — the editor may
  have crashed, or daemon transport fell back to one-shot. Inspect
  `_meta.elapsed_ms` on the response (carried in the MCP envelope).

## Errors + edge cases (high-level)

- **`AssetNotFound`** — asset/graph/function/node missing. Treat as
  user-error (typo in path) rather than infrastructure failure.
- **Mock backend is read-only.** Write tools throw with a message
  pointing at `BP_READER_BACKEND=commandlet`. If the user is running
  against fixtures, surface that — don't silently fail.
- **Compile errors during a write op** surface as
  `BlueprintReaderError` with `exit=5`. The save doesn't commit.

Detailed triage flows are in the **bp-debug** sub-skill.

## What the MCP doesn't expose (yet)

If a user asks for any of these, say so directly rather than
attempting a workaround that fails:

- **Spawning unusual node kinds** (Switch, GetClassDefaults,
  SpawnActorFromClass, AddDelegate, ...). Only kinds in
  `list_node_kinds` are supported via `add_node` — anything else has
  to be added to the plugin's `RunAddNodeOp` first. The user can
  still create those in the editor UI.
- **Cross-asset *graph* analysis** like "find every BP that calls
  `MyFunc`". `find_overriders` is the closest — structural queries
  on parent/interface/function-name. Per-call-site cross-asset search
  isn't there.
- **Editor-only operations** — undo/redo, hot-reload, content browser
  drags. The MCP only sees compiled `.uasset` reads/writes.

## Common asks → tool sequences

- *"What does BP_TestEnemy do?"* → `summarize_blueprint` →
  `read_blueprint` → `get_graph EventGraph` → `get_function` for each
  function listed. Or jump to `transpile_function` for the most
  human-skimmable summary if the BP is mostly functions.
- *"Add a Health variable to the enemy."* → `add_variable` with
  `type:"real:float"`. Confirm with `list_variables` if the user wants
  verification.
- *"Wire BeginPlay to a Branch with bIsAlive condition."* → most
  efficient is `compile_function` if you can express the body in
  pseudocode; otherwise `find_node` for BeginPlay (`kind="Event"`),
  spawn nodes via `add_node`, `wire_pins`, optionally
  `auto_layout_graph` to tidy.
- *"Refactor MaxHealth to MaxHP everywhere."* → `rename_variable`.
- *"Change Health from int to float."* → `retype_variable`.
- *"What components does this BP have?"* → `get_components`.
- *"Find every PrintString call in this BP."* → `find_node` with
  `query="PrintString"` and `kind="CallFunction"`.
- *"Find every BP that overrides BeginPlay."* → `find_overriders`
  with `function_name:"BeginPlay"`.
- *"Make BP_Boss from BP_Enemy."* → `duplicate_blueprint` then mutate.
- *"Convert ApplyDamage to C++ for code review."* → `transpile_function`.
- *"What project is this and which engine version?"* → `get_project_metadata`.
- *"Save everything."* → `save_all`.
- *"Move /Game/AI/BP_Boss to /Game/Bosses/BP_Boss."* → `move_asset`.
- *"List the DataTables in /Game/Data."* → `list_data_tables`.
- *"Spawn a StaticMeshActor at (0, 0, 100)."* → `spawn_actor` with
  `class_path:"/Script/Engine.StaticMeshActor"` + a `location` object.
- *"What's selected right now?"* → `get_selected_actors`.
- *"Run `stat unit` in the editor."* → `console_command`.
- *"Bump `r.ScreenPercentage` to 50."* → `set_cvar` with `name:"r.ScreenPercentage"` `value:"50"`.
- *"Start PIE."* → `pie_start`. (Stop it later with `pie_stop`.)
- *"Recompile my C++ via Live Coding."* → `live_coding_compile`, then
  `read_output_log` to watch progress.
- *"Run the BlueprintReader.* tests."* → `run_automation_tests` with
  `pattern:"BlueprintReader.*"`; check `read_output_log` or
  `Saved/Automation/index.json` for results.

If a request hits a tool that doesn't exist (or a node kind not in
`list_node_kinds`), say so directly and tell the user what is
supported. Don't fabricate a workaround that quietly fails.
