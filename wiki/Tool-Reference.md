# Tool Reference

59 tools — 12 read, 18 write, 3 meta, 3 batch, 3 transpile, 7 project /
content-browser, 12 live editor, 1 automation. All use
snake_case JSON keys; nullable string fields emit `null`; `BPNode.meta`
is a real nested object (not a string-of-JSON). Wire shapes are pinned
in `Plugins/BlueprintReader/mcp-server/src/BlueprintReaderTypes.h`.

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
`decompile_blueprint` + class-emit: emits a `UCLASS()` decl with
`UPROPERTY()` decoration (Replicated / EditAnywhere / Category specifiers
inferred from BP variable metadata), `UFUNCTION()` decls, function
bodies, and `GetLifetimeReplicatedProps()` registration when any
variable is Replicated.

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
(timelines, latent actions, async actions, anim/Niagara graphs) plus
`manual_steps` for triage; write it next to the `.h/.cpp` so the agent
can iterate over what's left to port.

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
