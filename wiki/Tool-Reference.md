# Tool Reference

22 tools — 8 read, 12 write, 2 meta. All use snake_case JSON keys; nullable
string fields emit `null`; `BPNode.meta` is a real nested object (not a
string-of-JSON). Wire shapes are pinned in `Plugins/BlueprintReader/mcp-server/src/BlueprintReaderTypes.h`.

## Response controls (read tools)

Every read tool below accepts the same trio of optional arguments for
shrinking responses — important for AI clients, where every byte of a
tool response is consumed as context tokens:

| Argument | Effect |
|---------|--------|
| `fields`  | Array of dotted paths. Keep only those keys; drop the rest. Use `[]` to apply the path to every element of an array. Example: `["name", "variables[].name"]` returns just the BP name and the names of its variables. |
| `limit`   | Cap the number of items returned (only meaningful when the response is an array — `list_blueprints`, `list_variables`, `find_node`, `get_components`). |
| `offset`  | 0-based index into the result array. Pair with `limit` for paging. |

Omitting these gives the full payload (back-compat with pre-projection
clients).

## Read tools

### `summarize_blueprint`
Tiny orientation response: parent class plus counts of variables,
functions, graphs, macros, and interfaces. Use this **first** when you
don't yet know how big a BP is — saves loading the full payload to find
out it has hundreds of variables.

```json
// args
{ "asset_path": "/Game/AI/BP_TestEnemy" }

// returns
{
  "name":            "BP_TestEnemy",
  "asset_path":      "/Game/AI/BP_TestEnemy",
  "parent_class":    "ACharacter",
  "variable_count":  5,
  "function_count":  2,
  "graph_count":     3,
  "macro_count":     0,
  "interface_count": 1
}
```

### `list_blueprints`
List all blueprint assets under a content path. Defaults to `/Game`.
Uses the asset registry — fast, doesn't load every BP. On big projects
this can return thousands of entries — use `limit`/`offset` to page,
and `fields` to drop columns.

```json
// args
{ "path": "/Game/AI", "limit": 50, "fields": ["asset_path"] }

// returns
[
  { "asset_path": "/Game/AI/BP_TestEnemy" },
  { "asset_path": "/Game/AI/BP_TestPickup" }
]
```

### `read_blueprint`
Top-level metadata: parent class, interfaces, variables, graph
summaries. Pass `fields` to project, e.g. `["parent_class", "variables[].name"]`.

```json
{ "asset_path": "/Game/AI/BP_TestEnemy",
  "fields": ["parent_class", "variables[].name"] }
```

### `get_graph`
Full node + connection graph by name. Defaults to `EventGraph`. Big
graphs are big — `fields=["nodes[].title", "nodes[].kind"]` is a common
projection.

```json
{ "asset_path": "/Game/AI/BP_TestEnemy", "graph_name": "EventGraph" }
```

### `get_function`
A function's signature (inputs/outputs/locals) + body graph. Project
with `fields=["inputs[].name", "outputs[].name"]` for just the signature.

```json
{ "asset_path": "/Game/AI/BP_TestEnemy", "function_name": "ApplyDamage" }
```

### `list_variables`
Member variables with type, default, category, replication state. Big
BPs can have 100+ variables — use `fields`/`limit`/`offset` to keep
responses small.

```json
{ "asset_path": "/Game/AI/BP_TestEnemy",
  "fields": ["name", "type.category"],
  "limit":  20 }
```

### `get_components`
SCS components — name, class, parent, root flag.

```json
{ "asset_path": "/Game/AI/BP_TestEnemy" }
```

### `find_node`
Substring search by class/title; optional `kind` filter on K2 extras.
Pair with `fields=["id", "title", "kind"]` for tiny responses on busy
graphs.

```json
{
  "asset_path": "/Game/AI/BP_TestEnemy",
  "query":      "Health",
  "kind":       "VariableGet",
  "fields":     ["id", "title", "kind"]
}
```

## Write tools

All write tools recompile and save the blueprint. They return the new
state (variable list, node GUID, etc.) on success. Successful writes
also drop the [server-side cache](Configuration#response-caching) for
the affected asset, so a follow-up read sees the new state.

### `add_variable`
Add a member variable with full BPPinType + default + category + flags.

```json
{
  "asset_path":   "/Game/AI/BP_TestEnemy",
  "name":         "Stamina",
  "type":         { "category": "float" },
  "default":      "75.0",
  "category":     "Stats",
  "replicated":   false,
  "editable":     true,
  "blueprint_read_only": false
}
```

### `delete_variable`
Remove a member variable by name.

```json
{ "asset_path": "/Game/AI/BP_TestEnemy", "name": "Stamina" }
```

### `rename_variable`
Rename + update references in graphs.

```json
{ "asset_path": "/Game/AI/BP_TestEnemy", "old_name": "Stamina", "new_name": "Energy" }
```

### `set_variable_default`
Change a member variable's default value (string form).

```json
{ "asset_path": "/Game/AI/BP_TestEnemy", "name": "Health", "default": "200.0" }
```

### `add_node`
Spawn a node. Returns its new GUID.

```json
{
  "asset_path":   "/Game/AI/BP_TestEnemy",
  "graph_name":   "EventGraph",
  "kind":         "branch",            // see list_node_kinds
  "position":     { "x": 0, "y": 0 },
  // optional, kind-specific:
  "variable":      "Health",            // for variable_get/set
  "function_owner":"/Script/Engine.GameplayStatics", // for call_function
  "function_name": "GetPlayerCharacter",             // for call_function
  "target_class":  "/Script/Engine.Actor"            // for cast nodes
}
```

### `set_node_position`
Move a node by GUID.

```json
{ "asset_path": "...", "graph_name": "EventGraph", "node_guid": "...", "position": { "x": 100, "y": 200 } }
```

### `delete_node`
Remove a node by GUID; breaks links into/out of it.

```json
{ "asset_path": "...", "graph_name": "EventGraph", "node_guid": "..." }
```

### `wire_pins`
Connect two pins, schema-validated. Prefer GUIDs; names work as fallback.

```json
{
  "asset_path":   "/Game/AI/BP_TestEnemy",
  "graph_name":   "EventGraph",
  "from_node":    "<guid>",
  "from_pin":     "Then",              // pin name or GUID
  "to_node":      "<guid>",
  "to_pin":       "Exec"
}
```

### `add_function`
Create a new BP function graph. Returns the echoed function name.

```json
{ "asset_path": "/Game/AI/BP_TestEnemy", "function_name": "ApplyDamage" }
```

### `delete_function`
Delete a function graph by name.

```json
{ "asset_path": "/Game/AI/BP_TestEnemy", "function_name": "ApplyDamage" }
```

### `add_function_input` / `add_function_output`
Add an input or output parameter to an existing function. The output
variant spawns a `K2Node_FunctionResult` if the function doesn't have
one yet.

```json
{
  "asset_path":   "/Game/AI/BP_TestEnemy",
  "function_name":"ApplyDamage",
  "name":         "DamageAmount",
  "type":         { "category": "float" }
}
```

## Meta tools

### `list_node_kinds`
Enumerate the `kind` values `add_node` accepts plus their required extras.
Use this when you're not sure how to spawn a particular node type.

```json
// returns
{ "kinds": [
    { "kind": "branch",        "extras": [] },
    { "kind": "sequence",      "extras": [] },
    { "kind": "variable_get",  "extras": ["variable"] },
    { "kind": "variable_set",  "extras": ["variable"] },
    { "kind": "call_function", "extras": ["function_owner", "function_name"] },
    { "kind": "custom_event",  "extras": ["event_name"] }
] }
```

### `list_pin_categories`
Enumerate canonical `BPPinType.category` values + container modifiers.

```json
// returns
{
  "categories": ["bool", "int", "float", "string", "name", "text", "object", "class", "struct", "enum"],
  "containers": ["none", "array", "set", "map"]
}
```

## Telemetry

Every `tools/call` envelope carries `_meta: { elapsed_ms, tool }` per the
MCP 2024-11-05 spec extension point. Doesn't change content; clients that
surface `_meta` (Claude Desktop's debug panel, custom MCP clients) see it.
