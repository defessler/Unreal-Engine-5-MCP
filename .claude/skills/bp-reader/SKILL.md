---
name: bp-reader
description: Use this skill when the user asks to inspect, edit, or build Unreal Engine 5 Blueprint assets through the bp-reader MCP server (tools like list_blueprints, read_blueprint, add_function, wire_pins, etc.). Triggers on phrases like "the blueprint", "BP_*", "/Game/...", "add a variable", "wire these pins", "what does this blueprint do", or any explicit mention of the bp-reader tools. Skip for non-blueprint UE topics (C++ source, build scripts, ini files).
---

# bp-reader — using the Blueprint MCP tools

The `bp-reader` MCP server exposes 21 tools that read and edit
`.uasset` Blueprint files in a UE 5.7.4 project. This skill covers
*how to use them well* — the wire format, common patterns, when to
prefer one tool over another. For *building or maintaining* the server
itself, read the project's CLAUDE.md.

## The toolbox at a glance

| Category   | Tools |
|------------|-------|
| **Read**   | `list_blueprints`, `read_blueprint`, `get_graph`, `get_function`, `list_variables`, `get_components`, `find_node` |
| **Write — variables** | `add_variable`, `delete_variable`, `rename_variable`, `set_variable_default` |
| **Write — functions** | `add_function`, `delete_function`, `add_function_input`, `add_function_output` |
| **Write — graph topology** | `add_node`, `delete_node`, `set_node_position`, `wire_pins` |
| **Discoverability** | `list_node_kinds`, `list_pin_categories` |

If a user task touches Blueprints, **call `tools/list` first** to see the
exact schemas — the server may have grown since this skill was written.
And when in doubt about valid `add_node` kinds or pin categories, call
the discoverability tools rather than guessing.

## Wire format you'll see and emit

All paths are package paths: `/Game/AI/BP_Foo`, never object paths
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

A `BPNode.meta` is a free-form JSON object, not a string. The plugin
fills in K2-specific extras like `kind`, `eventName`, `targetFunction`,
`isPureCast`, etc. — useful for `find_node` filtering.

Pin IDs are GUIDs (e.g. `8255EA22-4BA1-E744-FABC-E7973E353E57`). Prefer
them over pin names when wiring — they're stable across blueprint
recompiles. Names work too as a fallback.

## Reading a blueprint — pick the right tool

- **`read_blueprint`** is the broad overview: parent class, interfaces,
  variables, function names, graph names. Start here when the user asks
  "what's in this BP?" — gives you enough to plan further calls.
- **`get_graph`** for the full node + connection topology. Defaults to
  `EventGraph`; pass `graph_name` for `ConstructionScript`,
  `UserConstructionScript` (UE 5's name for it), function names, or
  macro names.
- **`get_function`** when you want the signature *and* body of one
  function. Returns `inputs[]`, `outputs[]`, `locals[]`, plus the body
  graph. Don't use `get_graph` for this — `get_function` parses the
  FunctionEntry/FunctionResult pins to build the typed signature for you.
- **`list_variables`** for just the member-variable table. Cheaper than
  `read_blueprint` when that's all you need.
- **`get_components`** for the SCS hierarchy
  (StaticMeshComponent / LightComponent / etc., with parent/child
  relationships).
- **`find_node`** for "where is X used?". Substring match on class+title;
  the optional `kind` filter narrows by K2 extras (`VariableGet`,
  `CallFunction`, `Event`, `CustomEvent`, `Cast`, ...). Pass empty
  `query` + a `kind` to enumerate all nodes of one kind.

## Editing — patterns that work

### Add a variable

```json
add_variable {
  "asset_path": "/Game/AI/BP_TestEnemy",
  "name": "Health",
  "type": {"category":"real", "sub_category":"float"},
  "default_value": "100.0",
  "category": "Combat",
  "replicated": true,
  "editable": true
}
```

`default_value` is the string form as displayed in the Details panel
(`"100.0"` for a float, `"true"`/`"false"` for a bool). Use
`set_variable_default` later to change it. `set_variable_type` doesn't
exist (it'd have to update referencing nodes); use
`delete_variable` + `add_variable` if you really need a different type.

### Compose a function from scratch

```
add_function(asset_path, name="MyFunc")               → {function_name}
add_function_input(asset_path, "MyFunc", "Damage", floatType)
add_function_output(asset_path, "MyFunc", "Killed",  boolType)
```

That gives you the empty signature. Body is filled in via `add_node` +
`wire_pins` against the new graph (which is named after the function).

### Build graph topology

The repeating pattern is: spawn nodes → wire them.

```
id1 = add_node(asset, "EventGraph", "VariableGet", x, y, {"variable":"bIsAlive"})
id2 = add_node(asset, "EventGraph", "Branch",      x+200, y)
id3 = add_node(asset, "EventGraph", "CallFunction", x+450, y,
               {"function":"PrintString",
                "function_owner":"/Script/Engine.KismetSystemLibrary"})
wire_pins(asset, "EventGraph", id1, "bIsAlive", id2, "Condition")
wire_pins(asset, "EventGraph", id2, "then",      id3, "execute")
```

`add_node` returns `{ok, node_id}` — pin GUIDs aren't predictable, so
use pin **names** for the immediate `wire_pins` calls. If the wire
ever fails the schema's compatibility check (incompatible types, etc.),
the call throws; catch and propagate to the user with a useful message.

### Refactor: rename and the rest

`rename_variable` updates references in graphs automatically — safe to
use without re-reading. `delete_variable` does *not* delete referencing
nodes; you may want to `find_node` for `VariableGet`/`VariableSet`
matching the old name first and decide whether to delete those too.

## When to look up rather than hardcode

The set of `add_node` `kind` values has grown — currently 12, but new
ones land regularly. **Don't hardcode the kind list from memory** —
call `list_node_kinds` to get the current set + each kind's required
extras. Same for `list_pin_categories` when constructing an unfamiliar
`BPPinType` (especially `struct` / `class` / `interface` where
`sub_category_object` matters).

The current 12 `add_node` kinds:

| Kind | Required extras |
|------|------------------|
| `Branch`, `Sequence`, `Self`, `MakeArray`, `FormatText`, `Knot` | (none) |
| `VariableGet`, `VariableSet` | `variable` |
| `CallFunction` | `function`, `function_owner` |
| `CustomEvent` | `event_name` |
| `Cast` | `target_class` |
| `MakeStruct` | `struct_type` |

`function_owner`/`target_class` accept either a UClass path
(`/Script/Engine.KismetSystemLibrary`) or a short name
(`KismetSystemLibrary`).

## Performance — assume daemon mode

The commandlet backend defaults to daemon mode: ~5 s editor cold start
on the first call, then **15–30 ms per subsequent call** in the same
MCP session. So:

- Don't worry about call count. A 20-step refactor (read + edit +
  re-read + wire) costs ~1 s after the first call lands. Compose
  workflows naturally rather than batching.
- If a single call is taking >1 s, something's off — the editor may
  have crashed and respawned, or daemon transport fell back to
  one-shot. Inspect `_meta.elapsed_ms` on the response (carried in the
  MCP envelope) and the server's stderr if available.

## Errors and edge cases

- **`AssetNotFound`** is thrown for "asset/graph/function/node missing"
  — distinguishable from generic `BlueprintReaderError` only by class
  name in some clients. Treat it as user-error (typo in path) rather
  than infrastructure failure.
- **Mock backend is read-only.** All write tools throw on the mock
  backend with a clear message pointing at
  `BP_READER_BACKEND=commandlet`. If a test or local dev session is
  running against fixtures, surface that to the user rather than
  silently failing.
- **Daemon transport failure auto-falls-back** to one-shot for that
  call. The MCP server logs this to stderr but the call still succeeds
  — Claude won't see a difference. So don't pin behavior on
  daemon-mode timing alone.
- **Compile errors during a write op** are surfaced as
  `BlueprintReaderError` with `exit=5` in the message tail. The save
  doesn't commit, so the on-disk `.uasset` is unchanged. The state in
  memory may still be partial — on a session-level error the user
  should reseed (see CLAUDE.md).

## What the MCP doesn't expose (yet)

If a user asks for any of these, say so explicitly rather than
attempting a workaround that fails:

- **Setting a node pin's default value.** No `set_pin_default` tool;
  the `default_value` on `BPPin` is read-only via this surface.
- **Spawning unusual node kinds** (Switch, GetClassDefaults,
  SpawnActorFromClass, AddDelegate, ...). Only the kinds in
  `list_node_kinds` are supported via `add_node` — anything else
  has to be added to the plugin's `RunAddNodeOp` first. The user can
  still create those in the editor UI.
- **Variable type changes.** `set_variable_type` doesn't exist;
  `delete_variable` + `add_variable` works but loses references.
- **Cross-asset analysis** like "find every blueprint that calls
  `MyFunc`". Single-blueprint search only.
- **Editor-only operations** — undo/redo, hot-reload, content browser
  drags. The MCP only sees compiled `.uasset` reads/writes.

## When a user asks something like…

- *"What does BP_TestEnemy do?"* → `read_blueprint` first, then
  `get_graph EventGraph` and `get_function` for each function listed.
  Summarize structure before diving into per-node detail.
- *"Add a Health variable to the enemy."* → `add_variable` with
  `category: "real"`, `sub_category: "float"`. Confirm the result with
  `list_variables` if the user wants verification.
- *"Wire BeginPlay to a Branch with bIsAlive condition."* → 
  `find_node` for the existing BeginPlay (`kind="Event"`); spawn the
  Branch and the VariableGet via `add_node`; `wire_pins` three times
  (BeginPlay→Branch.exec, bIsAlive→Branch.Condition, Branch.then→…).
- *"Refactor MaxHealth to MaxHP everywhere."* → `rename_variable`. It
  updates all references automatically; no need to walk graphs.
- *"What components does this BP have?"* → `get_components`.
- *"Find every PrintString call in this BP."* →
  `find_node` with `query="PrintString"` and `kind="CallFunction"` for
  precision.

If a request hits a tool that doesn't exist (e.g. setting pin
defaults), say so directly and tell the user what is supported. Don't
fabricate a workaround that quietly fails.
