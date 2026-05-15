# Tool Reference

126 tools — 12 read, 22 write, 3 meta, 3 batch, 3 transpile, 13 project /
content-browser, 15 live editor, 1 automation, 7 material, 5 widget,
5 behavior tree, 4 data asset, 5 state tree, 4 profiling, 2 cook,
3 class introspection, 4 viewport, 4 Niagara, 4 Sequencer, 3 GAS, 4
AnimGraph. All use snake_case JSON keys; nullable string fields emit
`null`; `BPNode.meta` is a real nested object (not a string-of-JSON).
Wire shapes are pinned in
`Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/BlueprintReaderTypes.h`.

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

The query matches against the node's class, rendered title, AND
`meta.targetFunction` / `meta.function_name` / `meta.variableName` /
`meta.eventName`. That's important for operator nodes whose underlying
identifier differs from the displayed title — e.g. searching for
`"Greater_FloatFloat"` finds K2 nodes rendered as `"Greater (float)"`,
and `"ReceiveBeginPlay"` finds the event node titled `"Event BeginPlay"`.

Each hit carries `graph_name` and `graph_type` so the caller can
immediately feed the result into `get_node` / `delete_node` /
`wire_pins` (all of which require `graph_name`) — no second round trip
to `read_blueprint` to resolve the graph.

```jsonc
// Response shape (per hit, simplified):
{
  "id":         "<node-guid>",
  "class":      "K2Node_CallFunction",
  "title":      "Greater (float)",
  "graph_name": "EventGraph",
  "graph_type": "EventGraph",
  "meta": { "kind": "CallFunction", "function_name": "Greater_FloatFloat" }
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

Each pin in the response carries its own `linked_to` array — a per-pin
view of incoming/outgoing connections — so you can verify wiring from
a single `get_node` call without a separate `get_graph` for the
graph-level `connections[]` array. Entries are
`{node_id, pin_id, pin_name}`; `pin_id` is the canonical reference for
follow-up ops, `pin_name` is for human-readable assertions.

DynamicCast nodes whose target class is missing (deleted, renamed, or
in an uncompiled module) carry `meta.castBroken: "true"`. Their title
becomes literally `"Bad cast node"` and the output pin's
`sub_category_object` is null, but `castBroken` is the reliable
detection signal — pair with `find_node(kind="DynamicCast")` and
filter on it.

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

When wrapped inside an `apply_ops` batch, write ops **defer** their
compile + save until the batch's `EndBatch` flush — a 10-op generation
collapses to 1 compile + 1 save instead of 10 each. Single-op callers see
no behavior change. See [Batch tools → `apply_ops`](#apply_ops) for
details.

### `create_blueprint`
Create a new BP asset under `/Game/...` extending `parent_class`.
Idempotent — calling with an existing asset returns
`{ok:true, already_existed:true}` instead of erroring. Pair with
`apply_ops` to create + populate a BP in one batch.

```json
{ "asset_path":   "/Game/AI/BP_Boss",
  "parent_class": "Actor" }
```

`parent_class` accepts:
- short names: `"Actor"`, `"ACharacter"` (UE prefix conventions are tried)
- full UClass paths: `"/Script/Engine.Actor"`

### `duplicate_blueprint`
File-level duplicate: source BP at `asset_path` → new BP at
`dest_asset_path`. Both must be under `/Game/`. Idempotent — if the
destination already exists, returns `{ok:true, already_existed:true}`
without overwriting. The new BP starts identical to the source (same
vars, functions, graphs, components) and is registered with the asset
registry so a follow-up `apply_ops` batch can mutate it.

```json
{ "asset_path":      "/Game/AI/BP_Enemy",
  "dest_asset_path": "/Game/AI/BP_Boss" }
```

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

### `retype_variable`
Change a member variable's type **without delete + re-add**. UE rewires
every `VariableGet` / `VariableSet` node that references it in place,
so existing graphs survive. For a brand-new variable, use
`add_variable` instead.

```json
{ "asset_path": "/Game/AI/BP_Enemy",
  "name":       "Item",
  "type":       "object:Actor" }
```

`type` accepts the same shorthand strings (`"float"`, `"object:Actor"`,
`"[]float"`, `"{string:int}"`) and BPPinType objects as `add_variable`.

### `set_variable_category`
Change the My-Blueprint-panel category label on a member variable —
the "Stats" / "Combat" group header in the BP editor's variables list.
Empty `category` clears it back to default. For a brand-new variable,
pass `category` to `add_variable` instead — this tool is for
retroactive edits.

```json
{ "asset_path": "/Game/AI/BP_Enemy",
  "name":       "Health",
  "category":   "Stats" }
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

### `add_component`
Add a component to a Blueprint's `SimpleConstructionScript` tree.
`component_class` is the full UClass path (e.g.
`/Script/Engine.StaticMeshComponent`). Pass `parent` to attach as a
child of an existing node; omit for root attachment. `socket` applies
to `SceneComponent` children only. Idempotent on `name`.

```json
{ "asset_path":      "/Game/AI/BP_Enemy",
  "name":            "SpotLight",
  "component_class": "/Script/Engine.SpotLightComponent",
  "parent":          "RootSceneComp",
  "socket":          "head" }
```

### `remove_component`
Remove a component from the SCS tree by name. Returns
`{removed:false}` when the component isn't found. Children are
promoted to the removed node's parent.

```json
{ "asset_path": "/Game/AI/BP_Enemy", "name": "SpotLight" }
```

### `attach_component`
Re-parent an SCS component. Pass `new_parent` to attach as a child;
empty means root. `socket` applies to `SceneComponent` children only.

```json
{ "asset_path": "/Game/AI/BP_Enemy",
  "name":       "SpotLight",
  "new_parent": "ArmComp",
  "socket":     "hand_r" }
```

### `set_component_property`
Set a property on a component's template (the BP-author default
values — what the BP Details panel shows for that component). Same
string→type coercion as `set_data_row_value`
(`FProperty::ImportText`). Returns pre-set and post-set
ExportText'd values for verification.

```json
{ "asset_path": "/Game/AI/BP_Enemy",
  "component":  "SpotLight",
  "property":   "Intensity",
  "value":      "5000.0" }
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

**Cascade behavior on failed slots.** When an op that binds a named
slot (`id: "foo"`) fails in a non-atomic batch, every downstream op
referencing `$foo` (or transitively, a slot bound to another failed
op) short-circuits with a richer error linking back to the upstream
cause. Each cascaded result carries `cause: "upstream-slot-failed"`
so the agent can distinguish cascades from native op failures:

```jsonc
{ "ok": false,
  "op_index": 4,
  "error": "field \"from_node\" references slot \"$mintedNode\", which was "
           "supposed to be bound by an earlier op that failed: "
           "op[2] failed: <original failure reason>",
  "cause": "upstream-slot-failed" }
```

The dependent op's call to the backend is skipped entirely — saves a
doomed daemon round-trip per cascaded op.

**Single-recompile batching (A1).** Ops inside a batch *defer* their
compile + save until the trailing `EndBatch` flush — N ops on the same
BP collapse to 1 compile + 1 save. Mid-batch failure semantics are
controlled by `on_failure`:

- `"compile"` (default) — best-effort: compile + save what landed
  before the failure. Matches the equivalent unbatched sequence's
  observable outcome.
- `"skip"` — discard the pending compile + save. Nothing reaches disk.
  The in-memory daemon state stays dirty (subsequent reads in the same
  daemon session can see partial mutations) until restart; documented
  limitation of strict-atomic mode.

**Compile diagnostics (C1).** The result includes a top-level
`diagnostics` array. Each entry carries `severity`, `message`, optional
`node_guid`, optional `asset_path`, and — for nodes minted within
this batch — an `op_index` pointing at which op produced the offending
node. Lets the agent attribute "wire failed type-check on the Branch's
Condition pin" to "the wire_pins op at index 7" without re-reading the
BP. Top-level `compile_errors` and `compile_warnings` counts give a
quick OK/not-OK signal.

**Supported ops:** `create_blueprint`, `add_variable`, `delete_variable`,
`rename_variable`, `set_variable_default`, `add_function`,
`add_function_input`, `add_function_output`, `delete_function`,
`add_node`, `wire_pins`, `set_node_position`, `delete_node`,
`set_pin_default` (used by `compile_function` for literal pin defaults).

### `preview_ops`
Validate an `apply_ops` batch **without mutating anything**. Walks the
op array, parses each op's required fields, resolves named-slot refs
(against placeholder GUIDs so multi-step refs validate), and uses
read-only backend calls to confirm referenced variables and functions
exist. Returns per-op `{ok}` results plus a `would_compile` list of
asset paths the real `apply_ops` would touch.

```jsonc
{ "ops": [...] }   // same shape apply_ops accepts
```

Use cases:
- Agent self-check before running a multi-op generation
- Human-in-the-loop confirmation step ("here's what I'd do; OK?")
- CI/lint pass over a generated batch

### `compile_function`
Compile a tiny pseudocode DSL into a fully-wired BP function. The agent
thinks in pseudocode (its native form); the server materializes nodes +
wires + literals in one call. Wrapped in an `apply_ops` batch internally
so the whole function compiles in a single recompile.

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
          "to":  { "call": "-",   // operator alias → KismetMathLibrary::Subtract_IntInt
                   "args": { "A": { "var": "Health" },
                             "B": { "var": "Amount" } } } },
        { "if":   { "call": "<=", "args": { "A": { "var": "Health" },
                                            "B": { "lit": 0 } } },     // literal pin default
          "then": [{ "call": "OnDeath" }] }
      ]
    }
  ]
}
```

**Statement forms:** `{if, then, [else]}`, `{set, to}`, `{call, args}`, `{comment}`.

**Expression forms:**
- `{var:"name"}` — VariableGet for a member variable.
- `{lit: value}` — literal pin default (string / number / boolean). Uses
  `set_pin_default` under the hood; UE has no first-class literal node.
- `{call:"fn", args:{...}}` — CallFunction node. Operator aliases:
  - Math: `+`, `-`, `*`, `/`, `%`
  - Comparison: `==`, `!=`, `<`, `<=`, `>`, `>=`
  - Boolean: `&&`, `||`, `!`
  - Float-explicit variants: `+f`, `-f`, `*f`, `/f`, `==f`, `<f`, `<=f`
  - Or use `"Owner::Function"` form for any other call.

**Auto-wired entry.** The function's `K2Node_FunctionEntry` is wired
straight into the first statement's exec input — same as what UE does
when you build a function in the editor. No manual `wire_pins` call is
needed to "kick off" the body.

**Exec-tail merging.** After an `if/then/else`, exec from BOTH branches
converges into the next statement (UE's K2 schema accepts multiple
sources on exec input pins — no Sequence/Join node needed).

Pass `dry_run: true` to get the compiled op list without executing —
useful for inspecting what `compile_function` would do before committing.

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

## Transpile tools

The BP↔C++ pipeline. A versioned **Blueprint Intermediate Representation
(BPIR)** sits in the middle — a JSON AST that BPs lower into and that
source languages emit / consume. Today only C++ ships; Lua/Python/JS
are future work that drops in as additional codegen + parser pairs
without touching the IR. Schema docs: [BPIR.md](BPIR.md).

```
                        ┌──────────────┐
       BP graph  ⇄     │     BPIR     │   ⇄  C++ source
                        └──────────────┘
                              ▲
                              │
              compile_function (BPIR → BP, already shipping)
```

### `decompile_function`
Walk a BP function's graph and reconstruct a structured BPIR tree —
the inverse of `compile_function`. No editor work needed; pure
server-side traversal of the JSON `get_function` already returns. Pair
with `transpile_function` (BPIR → C++) for the full pipeline, or feed
the BPIR through your own analysis.

```json
{ "asset_path":    "/Game/AI/BP_TestEnemy",
  "function_name": "TakeDamage" }
```

Returns `{ok, bpir, unsupported_count}`. The `bpir` doc validates
against the BPIR schema; any K2 nodes that don't pattern-match a known
control structure (timelines, latent ability calls, anim graphs, etc.)
appear in the body as `{unsupported: {...}}` statements with the
offending node's class + GUID + reason for triage.

### `decompile_blueprint`
Whole-class extraction: parent class, variables (with replicated /
editable / category metadata), interfaces, every function as a BPIR
function-doc. Top-level `{kind: "class"}` BPIR.

```json
{ "asset_path": "/Game/AI/BP_TestEnemy" }
```

Returns `{ok, bpir, unsupported_count}`. Use as the input to
`transpile_blueprint` for end-to-end BP → C++ class generation.

### `transpile_function`
BP function → C++ source. Composes `decompile_function` (BP → BPIR) +
the C++ codegen (BPIR → source). Phase 1 ships **readable** mode:
syntactically valid blocks with real type names + UE prefix conventions,
but UCLASS/UFUNCTION scaffolding is comments rather than full
decoration — use `transpile_blueprint` for the compilable pair.

```json
{ "asset_path":    "/Game/AI/BP_TestEnemy",
  "function_name": "TakeDamage",
  "target_lang":   "cpp",
  "mode":          "readable",
  "use_operator_aliases": true }
```

Returns:

```jsonc
{ "ok": true,
  "asset_path": "/Game/AI/BP_TestEnemy",
  "function_name": "TakeDamage",
  "target_lang":   "cpp",
  "mode":          "readable",
  "source": "void TakeDamage(float Amount) {\n    if (bIsAlive) {\n        Health = (Health - Amount);\n    }\n    return;\n}\n",
  "notes":  [],          // unsupported / approximation entries
  "unsupported_count": 0
}
```

`use_operator_aliases` (default `true`) inverts `compile_function`'s
operator alias map: `KismetMathLibrary::Add_IntInt` renders as `a + b`,
`EqualEqual_IntInt` as `a == b`, etc. Set `false` to keep canonical
qualified-name calls.

### `transpile_blueprint`
Whole BP class → compilable UE C++ `.h`/`.cpp` pair. Composes
`decompile_blueprint` + class-emit. End-to-end verified against
real BPs + UBT — the generated source compiles cleanly into the
project's editor target.

```json
{ "asset_path":         "/Game/AI/BP_Enemy",
  "target_lang":        "cpp",
  "module_api_macro":   "MYGAME_API",
  "class_name_suffix":  "_Generated",
  "use_operator_aliases": true }
```

Class name follows UE convention: a BP `BP_Enemy` extending `ACharacter`
becomes `ABP_Enemy_Generated`. Pass `class_name_suffix: ""` to drop in
place of the BP entirely.

Returns `{ok, class_name, header_file, impl_file, header_source,
impl_source, notes, sidecar, sidecar_file, unsupported_count}`. The
**sidecar** is JSON describing every unsupported / approximated node
plus `manual_steps` for triage; write it next to the `.h/.cpp` so the
agent can iterate over what's left to port.

**What the generated code does right** (out of the box, no
hand-editing required):

- **`UCLASS(MinimalAPI, Blueprintable)`** when no `module_api_macro`
  is passed — so `Cast<>` still works from other modules without
  exporting every symbol. With a macro, plain `UCLASS(Blueprintable)`
  + the macro on the class line.
- **`TObjectPtr<X>` for UPROPERTY object refs** (UE5 convention since
  5.0). Function arg pointers stay raw `X*`. Soft refs use
  `TSoftObjectPtr<X>` / `TSoftClassPtr<X>` consistently.
- **`VisibleAnywhere, BlueprintReadOnly`** for U*Component UPROPERTYs;
  data fields use `EditAnywhere, BlueprintReadWrite`.
- **Constructor with `bReplicates = true`** when Actor-derived + any
  Replicated UPROPERTY (without this, DOREPLIFETIME registrations are
  a silent no-op at runtime).
- **`DOREPLIFETIME_CONDITION(Class, Var, COND_*)`** when the BP
  variable carries a RepCondition (OwnerOnly, SkipOwner, etc.).
  `ReplicatedUsing=OnRep_X` + `UFUNCTION() void OnRep_X()` when the
  variable has RepNotify.
- **Forward declarations** for UPROPERTY-referenced types before
  `.generated.h` (UE convention: forward-decl in .h, include in .cpp).
- **`UFUNCTION(BlueprintPure)` + `const` member function** inferred
  when the BP function has no exec output on FunctionEntry + has a
  return value.
- **`const FString&` / `const FVector&` / `const TArray<X>&` args**
  per Epic's "anything larger than FVector → const ref" perf
  convention. Primitives + FName + pointers pass by value.
- **Safety defaults for primitives**: `bool = false`, `int32 = 0`,
  `float = 0` even when BP carried `default_value: null`
  (uninitialized primitives are undefined behavior in C++).
- **Synthesized return** at the end of any non-void function whose
  body walk didn't hit a FunctionResult — uses the output defaults
  (`return false;` from BP's `bool Killed = false` output).

**BP node lowerings beyond the basics** (each K2 node maps to its
canonical C++ idiom):

- `KismetMathLibrary::Add_VectorVector` → `A + B` (and Multiply,
  Subtract, etc. for Vector / Vector2D / Rotator + comparisons)
- `KismetMathLibrary::Add_IntInt`, `EqualEqual_FloatFloat`,
  `BooleanAND`/`OR`/`Not_PreBool` → C++ operators
- `KismetSystemLibrary::IsValid` → bare `IsValid()`
- `KismetSystemLibrary::PrintString` (+ trace / draw debug helpers,
  `GameplayStatics::GetPlayerController` etc.) → injects `this` as
  WorldContextObject so the call compiles
- `KismetArrayLibrary::Array_Add(Arr, Item)` → `Arr.Add(Item)` (+
  `Num()`, `Contains()`, `Remove()`, `IsValidIndex()`, etc.)
- `KismetStringLibrary::ToUpper` / `Len` / `Contains` →
  `Str.ToUpper()` / `Str.Len()` / etc.
- `K2Node_DestroyActor` → `Target->Destroy()` (or `this->Destroy()`)
- `K2Node_ConstructObjectFromClass` → `NewObject<UObject>(Outer, Class)`
- `K2Node_GetClassDefaults` → `Class->GetDefaultObject<UObject>()->Field`
- `K2Node_CallParentFunction` → `Super::Foo(args)`
- `K2Node_FormatText` → `FText::Format(NSLOCTEXT(...), Args)` with
  populated `FFormatNamedArguments`
- `K2Node_ForEachLoop` / `WhileLoop` (BP macros) → range-based for /
  while statements
- `K2Node_Switch*` → C++ switch / case
- `K2Node_DynamicCast` → `Cast<T>(obj)` (expression form) or the
  if-cast-success pattern (statement form)
- `K2Node_Knot` (reroute) → transparent passthrough on both exec +
  data flow

**Things that emit a TODO + sidecar refactor hint** (latent or
stateful — no single-statement C++ equivalent):

- `Delay` / `RetriggerableDelay` / `DelayUntilNextTick` → FTimerManager
  pattern with the canonical SetTimer hint
- `LoadAsset` / `LoadAssetClass` → FStreamableManager async load
- `K2Node_MultiGate` → int32 state + switch refactor
- `K2Node_Timeline` → UTimelineComponent member + PlayFromStart
- `K2Node_AsyncAction` / `K2Node_LatentAbilityCall` → UAsyncAction
  callback pattern
- `K2Node_AnimNode_*` → move to UAnimInstance-derived class

**Name-collision warnings** in the sidecar when BP overrides UE
reserved virtual methods (`TakeDamage`, `Tick`, `BeginPlay`, `Jump`,
etc.) — the generated UFUNCTION shadows the inherited virtual
instead of overriding it (compiles with `-Woverloaded-virtual`, but
the function never gets called via UE's damage / tick / event
pipeline). The sidecar gives a rename hint (`Bp_TakeDamage`) or
"refactor to the real override signature".

### `write_generated_source`
Write a transpiled `.h` / `.cpp` into the project's `Source/` tree.
Path-confined by the plugin to `<ProjectDir>/Source/` — anything else
is rejected. Pair with `transpile_blueprint`: pass back the
`header_source` / `impl_source` strings the transpile returned, plus
the destination paths under your game module.

```json
{ "path":        "D:/Projects/MyGame/Source/MyGame/Generated/BP_Enemy_Generated.h",
  "content":    "<source string>",
  "create_dirs": true }
```

After all files are written, run UBT (or use the editor's Live Coding)
to compile the new class — the BP can then reparent to the C++ class
for hybrid workflows.

### `parse_cpp_function`
C++ → BPIR — the inverse of `transpile_function`. Closes the BP↔C++
loop: source language → BPIR → BP graph (via `compile_function`). The
parser accepts a controlled subset (if/else, range-based for, while,
switch, return, break, continue, calls, member access, `Cast<T>()`,
unary + binary operators with C++ precedence) — enough to round-trip
what `transpile_function` emits, plus reasonable hand-written extensions.

```jsonc
{ "source": "bool TakeDamage(float Damage) { if (bIsAlive) { Health -= Damage; } return true; }" }

// or with an out-of-band signature for a bare body:
{ "source":    "{ if (bIsAlive) { Health -= Damage; } return true; }",
  "signature": { "version": 1, "kind": "function", "name": "TakeDamage",
                 "inputs": [{"name":"Damage","type":"float"}],
                 "outputs": [{"name":"ReturnValue","type":"bool"}] } }
```

Returns `{ok, bpir}`. The BPIR validates against the schema; feed it
straight to `compile_function` to materialize the BP graph.

Out of scope (parser throws with `<line>:<col>: <message>`): the C
preprocessor (`#include` / `#define` / `#ifdef`), templates beyond
`Cast<T>`, lambdas, `decltype`, exceptions, raw pointer arithmetic.
The interface is implementation-decoupled — swapping in libclang for
fuller C++ support stays a future phase that touches no callers.

## Project + Content Browser tools

Project-level introspection + asset-browser operations alongside the
per-Blueprint surface.

### `get_project_metadata`
Read the project's `.uproject` JSON and return parsed metadata.
Returns the normalized fields the agent usually wants plus the raw
JSON for anything else.

```json
{}

// returns
{
  "ok": true,
  "project_name":       "UE5_MCP",
  "project_path":       "D:/Projects/UE5_MCP/UE5_MCP.uproject",
  "engine_association": "5.7",
  "category":           "",
  "description":        "",
  "raw":                { ... full .uproject ... }
}
```

### `save_all`
Save every dirty package the editor has loaded. With `dirty_only=true`
(default), clean packages are skipped — fast no-op when nothing's
changed. Returns the saved count + any failed asset paths.

```json
{ "dirty_only": true }
```

### `move_asset`
Move or rename an asset. `dest_asset_path` is the full destination
package path — pass the same folder with a different leaf for a
rename, or a different folder to move. Both must be under `/Game/`.
Updates the asset registry and fixes references in other assets.

```json
{ "asset_path":      "/Game/AI/BP_Boss",
  "dest_asset_path": "/Game/Bosses/BP_Boss" }
```

### `delete_asset`
Delete an asset. Refuses by default if other assets reference it
(returns the list of referrers); set `force=true` to delete anyway.

```json
{ "asset_path": "/Game/AI/OldBP", "force": false }
```

### `create_folder`
Create a folder under `/Game/`. Idempotent — returns
`{already_existed:true}` when the folder is already present.

```json
{ "folder_path": "/Game/AI/Boss" }
```

### `list_data_tables`
List every `UDataTable` asset under a content path. Mirrors
`list_blueprints` shape but filtered for the DataTable type.

```json
{ "path": "/Game/Data" }
```

### `read_data_table`
Load a DataTable and return the row-struct type, column names, and
every row's field values (serialized via `FJsonObjectConverter`).

```json
{ "asset_path": "/Game/Data/DT_Items" }
```

### `add_data_row`
Add a row to an existing DataTable. `values` is an object mapping
row-struct field names to stringified values; `FProperty::ImportText`
coerces to the property's type (works for scalars, enums, FName /
FString / FText, and structs that round-trip through text).
Idempotent — existing names return `{already_existed:true}` unless
`overwrite:true` is passed. Pair with `read_data_table` to see the
row-struct shape first.

```json
{ "asset_path": "/Game/Data/DT_Items",
  "row_name":   "Sword_Iron",
  "values":     { "DisplayName": "Iron Sword", "Damage": "12", "Tier": "1" } }
```

### `set_data_row_value`
Update a single field on an existing row. Returns the pre-set and
post-set ExportText'd values so the caller can verify the coercion.

```json
{ "asset_path": "/Game/Data/DT_Items",
  "row_name":   "Sword_Iron",
  "field_name": "Damage",
  "value":      "15" }
```

## Live editor tools

Operate on the running editor's in-memory state. Most useful with the
`live` backend (open editor); the commandlet daemon routes them too,
but PIE / Live Coding semantics in a headless editor are limited.

### `console_command`
Execute a UE console command (e.g. `stat unit`, `showflag.bones 1`,
`r.ScreenPercentage 75`). Returns whatever the command echoed.

```json
{ "command": "stat unit" }
```

### `get_cvar` / `set_cvar`
Read or write a console variable. `set_cvar` forces `ECVF_SetByCode`
priority — overrides ini files / scalability defaults.

```json
{ "name": "r.ScreenPercentage" }                       // get
{ "name": "r.ScreenPercentage", "value": "50" }       // set
```

### `pie_start` / `pie_stop`
Start / end a Play-In-Editor session. `mode` accepts
`selected_viewport` (default), `new_editor_window`, `standalone`,
`vr_preview`. `pie_stop` is a no-op when PIE isn't running.

### `live_coding_compile`
Trigger UE's Live Coding compile + patch. Async — progress lands in
the editor log. Pair with `read_output_log` to follow.

### `get_selected_actors`
List the names of currently-selected actors in the level editor.

### `set_selection`
Replace (or extend with `replace:false`) the selection. Returns the
post-call selected names so the caller can verify.

```json
{ "actor_names": ["Cube_0", "PointLight_2"], "replace": true }
```

### `spawn_actor`
Spawn an actor of a given UClass in the current level. `class_path`
is the full path (e.g. `/Script/Engine.StaticMeshActor` or a BP
class). All transform fields are optional; default to identity.

```json
{ "class_path": "/Script/Engine.StaticMeshActor",
  "location": { "x": 0, "y": 0, "z": 100 } }
```

### `set_actor_transform`
Update an actor's world transform. `actor_name` is from
`get_selected_actors` or `spawn_actor`. All transform fields are
absolute (not delta).

### `delete_actor`
Destroy an actor by name. Returns `{deleted:false}` when the actor
wasn't found.

### `read_output_log`
Read recent entries from the editor's output log. The plugin module
installs a 1024-entry ring-buffer log sink at `StartupModule` (size
overridable via `BP_READER_LOG_BUFFER`); this returns up to `limit`
of the most recent entries newest-last, optionally filtered by
minimum severity (`Display` / `Log` / `Warning` / `Error` / `Fatal`).
Each entry: `{severity, category, message, timestamp}`.

```json
{ "limit": 50, "min_severity": "Warning" }
```

## Automation tools

### `run_automation_tests`
Trigger UE's automation test framework with a wildcard pattern
(empty = all tests). The run is async — this tool kicks it off
and returns immediately. Results land in the output log and at
`Saved/Automation/index.json`.

```json
{ "pattern": "BlueprintReader.*" }
```

## Material tools

Author UMaterial expression graphs the same way you'd edit them in
the material editor — add nodes, wire them to each other or to the
master-material slots, override parameters on instances, trigger
shader recompiles. Material expressions live in a separate UObject
tree from Blueprint event graphs; expression `id` is the
expression's UObject name within the material.

### `list_materials`
List all UMaterial / UMaterialInstanceConstant assets under a content
path. Mirrors `list_blueprints` but filters by class. Defaults to
`/Game`. Returns `BPAssetSummary[]`.

```json
{ "path": "/Game/Materials" }
```

### `read_material`
Read a material's expression graph: every `MaterialExpression` node
(id, class, parameter name, x/y) plus every connection. Connections
with empty `to_node` wire to a master-material slot whose name is
in `to_pin` (e.g. `BaseColor`, `Roughness`, `EmissiveColor`,
`Normal`, `Opacity`, `Metallic`, `Specular`, `OpacityMask`).

```json
// request
{ "asset_path": "/Game/Materials/M_Hero" }
// response (abbreviated)
{
  "ok": true,
  "asset_path": "/Game/Materials/M_Hero",
  "expressions": [
    { "id":"MaterialExpressionVectorParameter_0",
      "class":"MaterialExpressionVectorParameter",
      "parameter_name":"BaseColor",
      "x":-300, "y":0 }
  ],
  "connections": [
    { "from_node":"MaterialExpressionVectorParameter_0",
      "from_pin":"Output",
      "to_node":"", "to_pin":"BaseColor" }
  ],
  "parameter_names": ["BaseColor"]
}
```

### `add_material_expression`
Add a `UMaterialExpression` node. `expression_class` is the short
class name (`MaterialExpressionConstant3Vector`,
`MaterialExpressionScalarParameter`,
`MaterialExpressionTextureSampleParameter2D`, etc.). `x`/`y` are
graph coordinates. Returns `expression_id` to use in
`connect_material_expressions`.

```json
{ "asset_path": "/Game/Materials/M_Hero",
  "expression_class": "MaterialExpressionScalarParameter",
  "x": -300, "y": 100 }
```

### `connect_material_expressions`
Wire an expression's output to another expression's input or to a
master-material slot. Empty `to_node` = wire to a master slot
(`to_pin` then names the slot: `BaseColor`, `Metallic`,
`Roughness`, `EmissiveColor`, `Normal`, `Opacity`, etc.).

```json
// expression → expression
{ "asset_path": "/Game/Materials/M_Hero",
  "from_node": "MaterialExpressionMultiply_0", "from_pin": "Output",
  "to_node": "MaterialExpressionAdd_0",        "to_pin": "A" }

// expression → master-material slot
{ "asset_path": "/Game/Materials/M_Hero",
  "from_node": "MaterialExpressionVectorParameter_0", "from_pin": "Output",
  "to_node": "",                                       "to_pin": "BaseColor" }
```

### `set_material_parameter`
Set the default value of a named scalar / vector parameter on a
UMaterial. `value` is the parameter's text representation —
`"0.5"` for a scalar, `"(R=1,G=0,B=0,A=1)"` for a vector. Returns
`{old_value, new_value}`. For overriding on an instance, use
`set_material_instance_parameter`.

```json
{ "asset_path": "/Game/Materials/M_Hero",
  "parameter_name": "BaseColor",
  "value": "(R=0.8,G=0.2,B=0.2,A=1)" }
```

### `set_material_instance_parameter`
Override a parameter on a UMaterialInstanceConstant. `type` is
`scalar`, `vector`, or `texture`; `value` is its text form:

| Type | Example |
|------|---------|
| scalar | `"0.75"` |
| vector | `"(R=1,G=0,B=0,A=1)"` |
| texture | `"/Game/Textures/T_Foo.T_Foo"` (object path of a UTexture) |

```json
{ "asset_path": "/Game/Materials/MI_Hero_Red",
  "parameter_name": "TintColor",
  "type": "vector",
  "value": "(R=0.9,G=0.1,B=0.1,A=1)" }
```

### `compile_material`
Recompile a material's shader code. UE normally compiles
incrementally on edit; call this explicitly to flush pending
recompiles or recover from a stuck shader compile state. Returns
`{compiled: true|false}`.

```json
{ "asset_path": "/Game/Materials/M_Hero" }
```

## UMG widget tools

Author UWidgetBlueprint widget trees. The hierarchy lives in a
UWidgetTree (root + recursive PanelWidget children), separate
from the Blueprint event graph. Properties are set via
`set_widget_property`; events get scaffolded via
`bind_widget_event` (final binding may still need a manual editor
step depending on the delegate).

### `read_widget_blueprint`
Read a UWidgetBlueprint's tree: every `UWidget` node (name, class,
parent) and the root widget's name. Mirrors `get_components` but
for UMG.

```json
// request
{ "asset_path": "/Game/UI/WBP_HUD" }
// response
{ "ok": true,
  "asset_path": "/Game/UI/WBP_HUD",
  "root_name": "RootVerticalBox",
  "nodes": [
    { "name": "RootVerticalBox", "class": "VerticalBox", "parent": "" },
    { "name": "HealthBar",       "class": "ProgressBar", "parent": "RootVerticalBox" }
  ]
}
```

### `add_widget`
Add a UWidget node. `widget_class` is the short class name
(`Button`, `TextBlock`, `Image`, `VerticalBox`, etc.). Empty
`parent_name` = becomes the new root (only if the tree was empty);
otherwise appends as a child of `parent_name`. The parent must be a
PanelWidget. Idempotent on name (returns `already_existed:true`).

```json
{ "asset_path": "/Game/UI/WBP_HUD",
  "parent_name": "RootVerticalBox",
  "widget_class": "ProgressBar",
  "name": "HealthBar" }
```

### `set_widget_property`
Set a UPROPERTY on a widget. `property_name` is the property's
name as authored in C++ (`Text`, `ColorAndOpacity`, `Visibility`,
`Percent`). `value` is the property's text form — same encoding
UE's property system uses (`(R=...,G=...,B=...,A=...)` for
FLinearColor, plain text for FText).

```json
{ "asset_path": "/Game/UI/WBP_HUD",
  "widget_name": "HealthBar",
  "property_name": "Percent",
  "value": "0.75" }
```

### `bind_widget_event`
Bind a widget's event (e.g. `OnClicked` on a Button) to a named
handler function. Scaffolds the binding so the event graph knows
about the handler — depending on the delegate shape the final
runtime bind may still need a manual step in the editor (UMG's
"Bind Event to ..." workflow). Pairs with `add_function` if you
want to author the handler explicitly first.

```json
{ "asset_path": "/Game/UI/WBP_HUD",
  "widget_name": "ContinueButton",
  "event_name": "OnClicked",
  "handler_function": "OnContinueClicked" }
```

### `compile_widget_blueprint`
Compile a UWidgetBlueprint. Equivalent to the Compile button in
the UMG designer. Returns `{compiled: true|false}` — `false`
means compile failed; check `read_output_log` for errors.

```json
{ "asset_path": "/Game/UI/WBP_HUD" }
```

## Behavior Tree tools

UBehaviorTree assets host a root `UBTCompositeNode` plus
descendant tasks / decorators / services. Node ids are the
runtime UObject names — stable within the tree. Full editor-side
attach for new nodes still uses the BT editor (EdGraph wiring);
the tools below scaffold the runtime objects + properties.

### `list_behavior_trees`
List UBehaviorTree assets under a content path (default `/Game`).

```json
{ "path": "/Game/AI" }
```

### `read_behavior_tree`
Walk a tree. Returns every node (id, class, kind, parent) and the
root node id. `node_kind` is `composite` / `task` / `decorator` /
`service` / `unknown`.

```json
// request
{ "asset_path": "/Game/AI/BT_Enemy" }
// response (abbreviated)
{
  "ok": true,
  "asset_path": "/Game/AI/BT_Enemy",
  "root_node_id": "BTComposite_Selector_0",
  "nodes": [
    { "node_id": "BTComposite_Selector_0", "class": "BTComposite_Selector",
      "node_kind": "composite", "parent": "" },
    { "node_id": "BTTask_MoveTo_0", "class": "BTTask_MoveTo",
      "node_kind": "task", "parent": "BTComposite_Selector_0" }
  ]
}
```

### `add_bt_node`
Scaffold a new node. `node_kind` is `composite` / `decorator` /
`service` / `task`; `node_class` is the short class name (e.g.
`BTComposite_Selector`, `BTTask_MoveTo`,
`BTDecorator_Blackboard`). Empty `parent_node_id` becomes the
root composite (only allowed on an empty tree).

```json
{ "asset_path": "/Game/AI/BT_Enemy",
  "parent_node_id": "BTComposite_Selector_0",
  "node_kind": "task",
  "node_class": "BTTask_MoveTo" }
```

### `set_bt_node_property`
Set a UPROPERTY on a node (e.g. MoveTo's `AcceptableRadius`,
Blackboard decorator's `KeyName`). `value` is the property's text
form.

```json
{ "asset_path": "/Game/AI/BT_Enemy",
  "node_id": "BTTask_MoveTo_0",
  "property_name": "AcceptableRadius",
  "value": "50.0" }
```

### `compile_behavior_tree`
Mark the asset dirty so the BT editor re-initializes on next
open. Returns `{compiled: true}`.

```json
{ "asset_path": "/Game/AI/BT_Enemy" }
```

## DataAsset tools

`UDataAsset` and its subclasses are pure data containers. We
expose them as `{class, properties}` pairs where `properties`
is the JSON projection of every UPROPERTY on the asset.

### `list_data_assets`
List every UDataAsset subclass instance under a content path
(default `/Game`).

```json
{ "path": "/Game/Configs" }
```

### `read_data_asset`
Read every UPROPERTY on a UDataAsset.

```json
// request
{ "asset_path": "/Game/Configs/DA_EnemyConfig" }
// response
{
  "ok": true,
  "asset_path": "/Game/Configs/DA_EnemyConfig",
  "class": "EnemyConfig",
  "properties": {
    "Health": "100.0",
    "DamagePerHit": "10.0",
    "Description": "Spear-wielding goblin"
  }
}
```

### `create_data_asset`
Create a new UDataAsset instance from a UDataAsset subclass
class name. Idempotent on `asset_path` (returns
`already_existed:true`).

```json
{ "asset_path": "/Game/Configs/DA_NewEnemy",
  "class_name": "EnemyConfig" }
```

### `set_data_asset_property`
Set a UPROPERTY on a UDataAsset. `value` is the property's text
form (UE's standard property serializer).

```json
{ "asset_path": "/Game/Configs/DA_EnemyConfig",
  "property_name": "Health",
  "value": "150.0" }
```

## StateTree tools

UStateTree is experimental in UE 5.x. We expose discovery via
the Asset Registry; state authoring scaffolds and returns
`hint: "Finish state authoring in StateTreeEditor"` because the
full state graph lives behind editor-only API
(`FStateTreeEditorData`) that requires the StateTreeEditor
module.

### `list_state_trees`
List UStateTree assets under a content path (default `/Game`).

```json
{ "path": "/Game/AI/States" }
```

### `read_state_tree`
Return the asset shape (path + empty states/transitions arrays +
hint). Inspect richer state graphs in StateTreeEditor for now.

```json
{ "asset_path": "/Game/AI/States/ST_Player" }
```

### `add_state_tree_state`
Scaffold a state. Returns `{state_id, name, hint}`.

```json
{ "asset_path": "/Game/AI/States/ST_Player",
  "parent_state_id": "",
  "name": "Idle" }
```

### `set_state_tree_transition`
Define a transition between two states. `trigger` names an event
class or tick condition (e.g. `OnTick`, `OnEvent.Damage`).

```json
{ "asset_path": "/Game/AI/States/ST_Player",
  "from_state_id": "Idle",
  "to_state_id": "Combat",
  "trigger": "OnEvent.Damage" }
```

### `compile_state_tree`
Returns `{compiled: false, hint}` — full compile requires
StateTreeEditor.

```json
{ "asset_path": "/Game/AI/States/ST_Player" }
```

## Profiling tools

Drive UE's profiling backends from the agent — start a capture,
run your scenario, stop and read the output file. `start_profile`
selects between `stats` (UE's built-in `stat startfile` flow),
`csv` (CSVProfiler), and `insights` (UnrealInsights trace).

### `start_profile`
Start a capture.

```json
{ "mode": "stats" }
// or: { "mode": "csv" } / { "mode": "insights" }
```

### `stop_profile`
Stop the active capture; returns the output file path.

```json
{}
```

### `get_stats`
Toggle a stat group on (`Unit`, `Game`, `GPU`, `Memory`, `Engine`).
The text snapshot is captured asynchronously by the editor — use
`read_output_log` after to read it back.

```json
{ "group": "Unit" }
```

### `take_screenshot`
High-res screenshot via `HighResShot`. Width/height optional.

```json
{ "dest_path": "D:/screenshots/hero.png",
  "width": 3840, "height": 2160 }
```

## Headless cook / package tools

These return the `RunUAT.bat` command line you should execute
manually. The MCP server doesn't shell out inline to avoid editor-
already-running reentrancy issues with the daemon.

### `cook_content`
```json
{ "platform": "Windows" }
```

### `package_project`
```json
{ "platform": "Windows", "output_dir": "D:/Builds/MyGame" }
```

## Class introspection tools

Reflection over the live UClass registry. Cheap; useful for
agent orientation against a class hierarchy the user just
mentioned.

### `get_class_info`
```json
// request
{ "class_name": "Actor" }
// response (abbreviated)
{ "ok": true,
  "class": "Actor",
  "parent": "Object",
  "ancestors": ["Object"],
  "properties": [
    { "name": "RootComponent", "type": "TObjectPtr<USceneComponent>", "category": "Actor" }
  ],
  "functions": [
    { "name": "BeginPlay",   "flags": "BlueprintEvent" },
    { "name": "K2_DestroyActor", "flags": "BlueprintCallable" }
  ]
}
```

### `find_class`
Substring search across the UClass registry. Case-insensitive,
capped at 200 results.

```json
{ "query": "Controller" }
// → { "ok": true, "classes": ["PlayerController", "AIController", ...] }
```

### `list_functions`
Cheaper projection than `get_class_info` when you only need the
call surface.

```json
{ "class_name": "PlayerController" }
```

## Viewport tools

Editor viewport ergonomics — frame an actor, move the camera,
capture, toggle show flags.

### `focus_actor`
Equivalent to selecting the actor and pressing F.

```json
{ "actor_name": "BP_TestEnemy_0" }
```

### `set_camera_transform`
Move the active viewport camera explicitly.

```json
{ "loc_x": 0, "loc_y": 0, "loc_z": 500,
  "rot_pitch": -30, "rot_yaw": 90, "rot_roll": 0 }
```

### `take_viewport_screenshot`
Quick viewport capture (vs `take_screenshot` which uses HighResShot
for offline-quality output).

```json
{ "dest_path": "D:/screenshots/viewport.png" }
```

### `set_show_flag`
Toggle a viewport show flag (`Bones`, `Bounds`, `Collision`,
`Wireframe`, `Lighting`).

```json
{ "flag_name": "Collision", "enabled": true }
```

## Niagara tools

UNiagaraSystem lives in the Niagara plugin (`/Script/Niagara`).
Discovery via Asset Registry works without linking the editor
module; deeper authoring (emitter handle CRUD, parameter graph
edits) returns a scaffolded shape with a hint pointing at
NiagaraEditor.

### `list_niagara_systems`
```json
{ "path": "/Game/VFX" }
```

### `read_niagara_system`
Returns `{emitters[], parameter_names[], hint}`. Asset is
discovered; deeper introspection needs NiagaraEditor.

```json
{ "asset_path": "/Game/VFX/NS_Hero" }
```

### `create_niagara_system`
Idempotent — returns `{created, already_existed, hint}`. Asset
creation routes through NiagaraEditor's New System wizard.

```json
{ "asset_path": "/Game/VFX/NS_NewEffect" }
```

### `set_niagara_parameter`
Scaffold — full parameter override needs NiagaraEditor or a
runtime UNiagaraComponent instance.

```json
{ "asset_path": "/Game/VFX/NS_Hero",
  "parameter_name": "User.HitColor",
  "value": "(R=1,G=0,B=0,A=1)" }
```

## Sequencer tools

ULevelSequence assets discoverable via Asset Registry; track /
range mutations are scaffolded (`hint`) — full MovieScene API
manipulation lives behind editor modules we don't link.

### `list_level_sequences`
```json
{ "path": "/Game/Cinematics" }
```

### `read_level_sequence`
Returns the playback range + top-level tracks. Currently scaffolded
(empty arrays + hint).

```json
{ "asset_path": "/Game/Cinematics/LS_Intro" }
```

### `add_sequence_track`
```json
{ "asset_path": "/Game/Cinematics/LS_Intro",
  "track_class": "MovieSceneAudioTrack",
  "track_name": "Dialog" }
```

### `set_sequence_playback_range`
```json
{ "asset_path": "/Game/Cinematics/LS_Intro",
  "start_seconds": 0,
  "end_seconds": 12.5 }
```

## GAS / GameplayTag tools

### `list_gameplay_tags`
Query the GameplayTagsManager. `filter` is an optional substring.

```json
{ "filter": "Damage" }
```

### `add_gameplay_tag`
Scaffold — the project's tag dictionary lives in
`Config/Tags/DefaultGameplayTags.ini`. The tool returns a hint
pointing at the file; the actual mutation needs an .ini edit.

```json
{ "name": "Status.Damage.Fire",
  "comment": "Burn DoT applied by fire weapons." }
```

### `read_ability_set`
Best-effort reader for any UDataAsset that holds an array of
`{class, level}`-shaped struct entries (common pattern for ability
sets in GAS projects). Returns each entry's class + level.

```json
{ "asset_path": "/Game/GAS/AS_HeroBase" }
```

## AnimGraph tools

### `list_anim_blueprints`
```json
{ "path": "/Game/Characters" }
```

### `read_anim_blueprint`
Returns parent class + state machines. State-machine walk requires
the AnimGraph editor module (currently scaffolded; agent gets the
parent class + an empty state_machines array + hint).

```json
{ "asset_path": "/Game/Characters/ABP_Hero" }
```

### `add_anim_state`
Scaffold — full state authoring requires AnimGraph module.

```json
{ "asset_path": "/Game/Characters/ABP_Hero",
  "state_machine": "Locomotion",
  "state_name": "Crouching" }
```

### `compile_anim_blueprint`
Full compile via `FKismetEditorUtilities::CompileBlueprint` (works
because UAnimBlueprint is a regular UBlueprint subclass).

```json
{ "asset_path": "/Game/Characters/ABP_Hero" }
```

## Meta tools

### `shutdown_daemon`
Tear down the editor daemon process so the project's locks (DDC, asset
registry, `.uasset` handles) release. Use when you want to launch the
full UE editor without daemon contention. The next read tool call
auto-respawns the daemon (cold-start cost on first call after).

```json
{}
```

Returns `{ok, was_running, hint}`. Idempotent: calling when no daemon
is alive returns `was_running:false` without erroring.

Pair with `BP_READER_READ_ONLY=1` if you want the MCP server to keep
serving queries while you work in the editor —
[Configuration → Read-only coexistence](Configuration#read-only-coexistence-with-the-open-editor)
covers the workflow.

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
