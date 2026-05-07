# Tool Reference

27 tools — 10 read, 13 write, 2 meta, 2 batch. All use snake_case JSON keys;
nullable string fields emit `null`; `BPNode.meta` is a real nested object
(not a string-of-JSON). Wire shapes are pinned in
`Plugins/BlueprintReader/mcp-server/src/BlueprintReaderTypes.h`.

## Type shorthand (write tools)

Every write tool that takes a `type` argument accepts both the canonical
BPPinType object form and a compact string shorthand:

```jsonc
"type": "float"               // → {category:"real", sub_category:"float"}
"type": "int"
"type": "bool"
"type": "string"
"type": "object:Actor"        // object reference; long path also accepted
"type": "struct:FVector"
"type": "interface:IDamageable"
"type": "enum:EWeaponType"
"type": "[]float"             // array<float>
"type": "{}int"               // set<int>
"type": "{string:int}"        // map<string,int>

// canonical form still works:
"type": { "category": "real", "sub_category": "float" }
```

Bad shorthand (`"type": "garbage"`) throws `invalid_argument` before the
backend is touched, with a hint listing the accepted forms.

## Idempotent writes

`add_variable` and `add_function` are idempotent — calling them with a
name that already exists returns `{ok: true, already_existed: true}`
instead of throwing. The agent can retry blindly without first checking
whether the name's taken.

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

### `get_node`
Fetch a single node by GUID. Same shape as one entry from `get_graph`'s
nodes array. Cheaper than re-fetching the whole graph after a mutation
when all you need is "did the wire I just made stick?" Pairs with
`find_node` (which gives you the GUID).

```json
{ "asset_path":"/Game/AI/BP_TestEnemy",
  "graph_name":"EventGraph", "node_id":"<guid>" }
```

### `find_overriders`
Structural query across BPs under `path` (defaults to `/Game`). Filter by
`parent_class` / `function_name` / `interface` (any combination, at
least one required). Returns matching `{asset_path, parent_class, matched: [...]}`
entries. Replaces the manual `list_blueprints` + N×`read_blueprint` loop.

```jsonc
// Find every BP that overrides BeginPlay
{ "function_name": "BeginPlay" }

// All AI characters that implement IDamageable
{ "path":"/Game/AI", "parent_class":"ACharacter", "interface":"IDamageable" }
```

`parent_class` accepts both short names (`ACharacter`) and full paths
(`/Script/Engine.Character`).

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
Spawn a node. Returns its new GUID **plus the full pin list** so you can
wire it without a follow-up `get_graph` call.

```json
{
  "asset_path":   "/Game/AI/BP_TestEnemy",
  "graph_name":   "EventGraph",
  "kind":         "branch",            // see list_node_kinds
  "x": 0, "y": 0,
  // optional, kind-specific:
  "variable":      "Health",            // for variable_get/set
  "function_owner":"/Script/Engine.GameplayStatics", // for call_function
  "function":      "GetPlayerCharacter",             // for call_function
  "target_class":  "/Script/Engine.Actor"            // for cast nodes
}
```

Response shape:

```jsonc
{ "ok": true,
  "node_id": "<guid>",
  "title": "Branch",
  "class": "K2Node_IfThenElse",
  "pins": [
    { "name": "exec",      "guid": "...", "direction": "input",
      "type": {"category":"exec"} },
    { "name": "Condition", "guid": "...", "direction": "input",
      "type": {"category":"bool"} },
    { "name": "True",      "guid": "...", "direction": "output",
      "type": {"category":"exec"} },
    { "name": "False",     "guid": "...", "direction": "output",
      "type": {"category":"exec"} }
  ]
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
On failure, the error message **includes both pin types** so the agent
can self-correct in one turn:

```
WirePins: schema rejected the connection [from_pin type=object(Actor),
to_pin type=bool]
```

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

`auto_layout_graph` is a follow-up companion: spawn nodes anywhere, then
call it once to lay them out cleanly.

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

## Batch tools

### `apply_ops`
Run a sequence of write operations as a single tool call. Reduces N
round-trips and N agent reasoning steps to one. Each op is `{op:"<name>", ...args}`
matching the corresponding individual tool's args.

**Named slots**: any `add_node` op may carry an `id` field. Subsequent
ops can reference that node's GUID with `"$<id>"` or `{"ref":"<id>"}`
in any node-id field. Eliminates the need to thread minted GUIDs through
the agent's reasoning.

```jsonc
{
  "ops": [
    { "op": "add_function", "asset_path": "/Game/AI/BP_Enemy", "name": "TakeDamage" },
    { "op": "add_function_input", "asset_path": "/Game/AI/BP_Enemy",
      "function_name": "TakeDamage", "param_name": "Amount", "type": "float" },
    { "op": "add_node", "id": "branch",
      "asset_path": "/Game/AI/BP_Enemy", "graph_name": "TakeDamage",
      "kind": "Branch", "x": 200, "y": 0 },
    { "op": "add_node", "id": "getHealth",
      "asset_path": "/Game/AI/BP_Enemy", "graph_name": "TakeDamage",
      "kind": "VariableGet", "variable": "Health", "x": 0, "y": 100 },
    { "op": "wire_pins",
      "asset_path": "/Game/AI/BP_Enemy", "graph_name": "TakeDamage",
      "from_node": "$getHealth", "from_pin": "Health",
      "to_node":   "$branch",    "to_pin":   "Condition" }
  ],
  "atomic": true
}
```

Returns:

```jsonc
{ "ok": true,
  "succeeded": 5, "failed": 0,
  "slots":   { "branch": "<guid1>", "getHealth": "<guid2>" },
  "results": [ { "ok": true, "function_name": "TakeDamage", "already_existed": false },
               { "ok": true },
               { "ok": true, "node_id": "<guid1>", "pins": [...] },
               { "ok": true, "node_id": "<guid2>", "pins": [...] },
               { "ok": true } ] }
```

`atomic: false` continues on errors; failed ops appear as `{ok:false, error:"..."}`
in the per-op results array.

**Limitation (v1):** each underlying op still saves+recompiles individually.
True single-recompile batching needs plugin work — tracked separately.
The agent reasoning win is already large because all the GUID threading
and round-trips collapse into one call.

### `compile_function`
Compile a tiny pseudocode DSL into a fully-wired BP function. The agent
thinks in pseudocode (its native form); the server materializes nodes +
wires + layout in one call.

```jsonc
{
  "asset_path": "/Game/AI/BP_Enemy",
  "function_name": "TakeDamage",
  "inputs":  [{ "name": "Amount", "type": "float" }],
  "body": [
    { "if":   { "var": "bIsInvulnerable" },
      "then": [],
      "else": [
        { "set": "Health",
          "to":  { "call": "Subtract::Float",
                   "args": { "A": { "var": "Health" }, "B": { "var": "Amount" } } } },
        { "if":   { "call": "LessEqual::Float",
                    "args": { "A": { "var": "Health" }, "B": { "var": "Amount" } } },
          "then": [{ "call": "OnDeath" }] }
      ]
    }
  ]
}
```

Statement forms (v1): `{if, then, [else]}`, `{set, to}`, `{call, args}`,
`{comment}`.
Expression forms (v1): `{var:"name"}`, `{call:"fn", args:{...}}`.

Pass `dry_run: true` to get the compiled op list without executing —
useful for the agent to inspect / confirm before committing.

**Limitations (v1):**
- `{lit:value}` literal expressions not yet supported. Model literals as
  const variables and reference them with `{var:"name"}`, or drop to
  `apply_ops` with an explicit literal-node spawn.
- After `if/then/else`, exec continues from the `then` branch's tail; the
  `else` tail is left dangling. Use `apply_ops` + explicit Sequence/Join
  wiring if you need both branches to merge.
- Per-op save+recompile applies (same as `apply_ops`).

On unrecognized statement/expression forms, the response says exactly
which form was invalid so the agent can fall back to `apply_ops` for
that statement only.

### `auto_layout_graph`
Reposition every node in a graph using a column-grid layout based on
exec connectivity. Calls `set_node_position` on each node. Readable
output, but not as tidy as UE's built-in graph-tidy command. Plugin-side
integration with `KismetGraphSchema`'s real tidy pass is tracked
separately — until then this keeps generated graphs from overlapping.

```json
{ "asset_path":"/Game/AI/BP_Enemy", "graph_name":"EventGraph",
  "col_width": 400, "row_height": 200 }
```

Returns `{ok, placed, strategy: "grid"}`.

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
