# Usage

Once `bp-reader` is in your Claude MCP config, you don't invoke tools
directly — you describe what you want and Claude picks the right ones.

## Asset paths

All tools take **package paths**, not object paths:

```
Good:  /Game/AI/BP_TestEnemy
Bad:   /Game/AI/BP_TestEnemy.BP_TestEnemy        ← object path
Bad:   D:\Projects\UE5_MCP\Content\AI\BP_TestEnemy.uasset  ← disk path
```

`/Game/` is the content-root mount, equivalent to your project's
`Content/` folder. Plugin content uses the plugin name as the mount, e.g.
`/MyPlugin/Foo`.

## Reading

```
You: What's on /Game/AI/BP_TestEnemy?
Claude: [calls read_blueprint, summarizes]
        Parent: Actor. Five variables (Health: float = 100.0,
        MaxHealth: float = 100.0, ...). Three graphs: EventGraph,
        UserConstructionScript, ApplyDamage (function).
```

Common patterns:

- **"List all blueprints under `/Game/AI`"** → `list_blueprints`
- **"How big is this BP?"** → `summarize_blueprint` (cheap orientation —
  use this before drilling in)
- **"What variables does it have?"** → `list_variables` or `read_blueprint`
- **"Show me the event graph"** → `get_graph`
- **"Find any `K2Node_VariableSet` nodes"** → `find_node` with `kind: variable_set`
- **"What components are on this actor?"** → `get_components`
- **"What does `ApplyDamage` do?"** → `get_function`

`find_node` is the workhorse for "what touches X" questions — it returns
nodes filtered by class name, title substring, or `kind` (a normalized
classification of K2 node types).

### Keeping responses small

Every read tool accepts an optional `fields` parameter that drops keys
you don't need from the response. AI clients pay tokens for every byte
of a tool result, so projection matters on busy BPs:

```jsonc
// Just the parent class — useful when scanning many BPs
{ "asset_path": "/Game/AI/BP_Enemy", "fields": ["parent_class"] }

// Just variable names + types
{ "asset_path": "/Game/AI/BP_Enemy",
  "fields": ["variables[].name", "variables[].type.category"] }

// Find calls to a function, just IDs and titles
{ "asset_path": "/Game/AI/BP_Enemy", "query": "ApplyDamage",
  "fields": ["id", "title"] }
```

`limit` / `offset` paginate the array-shaped tools (`list_blueprints`,
`list_variables`, `find_node`, `get_components`). On a project with
thousands of blueprints, `list_blueprints` with `limit: 50` is dramatically
cheaper than the full list.

The server also caches read results for 30 s by default — repeated
queries about the same BP within a turn are essentially free. See
[Configuration → Response caching](Configuration#response-caching).

## Editing

The write tools mutate the asset on disk and recompile the blueprint.
Each operation is its own commandlet call (one save per op), so changes
land immediately and are picked up by the editor on next load.

```
You: Add a Stamina float variable to BP_TestEnemy with default 75
Claude: [calls add_variable]
        Added Stamina (float, default 75) to BP_TestEnemy.

You: Wire OnHit's exec output to a Branch, then to ApplyDamage
Claude: [calls add_node twice (Branch, CallFunction:ApplyDamage),
         then wire_pins twice]
        Spawned Branch (GUID …) and CallFunction:ApplyDamage (GUID …);
        wired the True branch into ApplyDamage's exec input.
```

Patterns that compose well:

| Goal                                | Tool sequence                                                              |
|--------------------------------------|----------------------------------------------------------------------------|
| Add a member var with type X         | `add_variable` (use shorthand: `"type":"float"`, `"object:Actor"`, etc.)   |
| Add an editable replicated var       | `add_variable` (with `editable: true`, `replicated: true`)                 |
| Spawn a Branch + wire exec           | `add_node` (kind=`branch`) → `wire_pins` (no `get_graph` round-trip — pins come back from `add_node`) |
| Add a function that reads a var      | `add_function` → `add_function_input` (if needed) → `add_node` (`variable_get`) → `wire_pins` |
| Refactor: rename + update call sites | `rename_variable` (graphs are auto-updated)                                |
| Audit: where is X read?              | `find_node` with `kind: "VariableGet"` and `query: "VarName"` |
| Audit: who overrides BeginPlay?      | `find_overriders` with `function_name: BeginPlay`                          |
| Inspect a single node by GUID        | `get_node` (cheaper than re-fetching the whole `get_graph`)                |

### Generating whole functions in one call

For multi-step work, compose with **`apply_ops`** (run a batch of writes
with named-slot GUID resolution) or **`compile_function`** (compile a
pseudocode DSL into nodes+wires):

```jsonc
// apply_ops — every node spawn + wire in one tool call
{ "ops": [
  { "op": "add_function", "asset_path": "/Game/AI/BP_Enemy", "name": "TakeDamage" },
  { "op": "add_function_input",  "asset_path": "/Game/AI/BP_Enemy",
    "function_name": "TakeDamage", "param_name": "Amount", "type": "float" },
  { "op": "add_node", "id": "branch",  "asset_path": "/Game/AI/BP_Enemy",
    "graph_name": "TakeDamage", "kind": "Branch", "x": 200, "y": 0 },
  { "op": "add_node", "id": "getH",    "asset_path": "/Game/AI/BP_Enemy",
    "graph_name": "TakeDamage", "kind": "VariableGet",
    "variable": "Health", "x": 0, "y": 100 },
  { "op": "wire_pins", "asset_path": "/Game/AI/BP_Enemy",
    "graph_name": "TakeDamage",
    "from_node": "$getH",   "from_pin": "Health",
    "to_node":   "$branch", "to_pin":   "Condition" }
] }
```

```jsonc
// compile_function — pseudocode → graph
{ "asset_path": "/Game/AI/BP_Enemy",
  "function_name": "Heal",
  "inputs": [{ "name": "Amount", "type": "float" }],
  "body": [
    { "set": "Health",
      "to":  { "call": "Add::Float",
               "args": { "A": { "var": "Health" }, "B": { "var": "Amount" } } } }
  ] }
```

Run **`auto_layout_graph`** afterwards to lay nodes out cleanly without
having to invent coordinates yourself.

### Idempotency and error self-correction

- `add_variable` / `add_function` with an existing name return
  `{ok:true, already_existed:true}` instead of throwing — retry blindly
  without checking first.
- `wire_pins` errors include both pin types
  (`[from_pin type=object(Actor), to_pin type=bool]`), so the agent can
  fix a type mismatch in one turn instead of needing to re-inspect the
  graph.

### Discovering valid node kinds and pin types

Two meta tools tell you what the schema accepts:

- `list_node_kinds` → all `kind` values `add_node` understands plus their
  required extras (`variable`, `function_owner`, `target_class`, ...).
- `list_pin_categories` → canonical `BPPinType.category` values
  (`bool`, `int`, `float`, `string`, `name`, `text`, `object`, `class`,
  `struct`, `enum`, ...) plus container modifiers (`array`, `set`, `map`).

Claude calls these on its own when it's unsure — you don't need to.

## BP ↔ C++

Convert blueprints to or from C++. Useful for code review (a 50-node
graph as a 30-line function diffs cleanly), promoting a prototype BP
to native for perf, or writing logic in your editor of choice and
materializing the BP graph from source.

```
You: Show me ApplyDamage on BP_TestEnemy as C++.
Claude: [calls transpile_function]
        bool ApplyDamage(float Amount) {
            if (bIsAlive) {
                Health = (Health - Amount);
                if (Health <= 0) {
                    OnDeath();
                    return true;
                }
            }
            return false;
        }
```

```
You: I wrote a Tick() function in pseudocode — turn it into a BP.
You: void Tick(float DeltaSeconds) { Energy = Energy + (DeltaSeconds * 10); }
Claude: [calls parse_cpp_function → compile_function]
        Created Tick on /Game/AI/BP_TestEnemy with the body wired up.
```

Common patterns:

| Goal                                          | Tool sequence                                          |
|-----------------------------------------------|--------------------------------------------------------|
| BP function as readable C++                   | `transpile_function`                                   |
| Whole BP class as compilable `.h`/`.cpp`      | `transpile_blueprint` → `write_generated_source` (×2)  |
| Hand-written C++ → BP graph                   | `parse_cpp_function` → `compile_function`              |
| BP as analyzable JSON AST (BPIR)              | `decompile_function`                                   |
| Round-trip a BP through C++ for review        | `transpile_function`, edit the source, `parse_cpp_function` → `compile_function` |

The transpile pipeline pivots on a versioned JSON AST called BPIR (see
the [BPIR](BPIR) page). Every transpile tool is just BP ↔ BPIR ↔ C++,
so adding Lua / Python / JS support is "just" another codegen + parser
pair — same IR, no plumbing changes elsewhere.

### Where the C++ won't compile cleanly

A shrinking list. Most "structural" patterns that BP used to require
manual porting for are now auto-lowered. The constructs that still
need TODO + sidecar attention:

- **Timelines** (`K2Node_Timeline`) — emitted as a stub `UTimelineComponent*`
  member with a TODO comment. Manually configure the timeline's curve
  assets in the editor.
- **Async tasks with typed payload pins** (e.g. `LoadAsset`'s
  `OnSuccess(Payload)`) — the Delay lowering handles only the
  `Duration → Completed` shape; tasks whose continuation needs typed
  parameters fall through to a TODO with the offending node class.
- **Gate macro** (multi-input stateful: `Enter` / `Open` / `Close` /
  `Toggle`) — the walker doesn't yet thread entry-pin info, so Gate
  emits a TODO. Other stateful macros (DoOnce, FlipFlop, DoN) ARE
  auto-lowered.
- **Legacy input nodes** (`K2Node_InputAction`, `K2Node_InputKey`,
  `K2Node_InputAxis`) — pre-EnhancedInput. Migrate the BP to
  EnhancedInput (which IS auto-lowered) or wire
  `InputComponent->BindAction(FName, ...)` manually.
- **Anim graph / Niagara module-graph nodes** — domain-specific; not
  portable to actor-side C++. Move the logic to a `UAnimInstance`
  subclass / Niagara emitter as appropriate.

What *does* port automatically:

- Standard control flow (if/else, for, while, switch).
- Variable get/set, function call, cast, return.
- `K2Node_SpawnActorFromClass` → `GetWorld()->SpawnActor<T>(...)`
  with full `FActorSpawnParameters` wiring.
- `K2Node_AddComponent` → `NewObject + SetRelativeTransform +
  RegisterComponent` block.
- **Stateful macros**: `DoOnce` / `FlipFlop` / `DoN` → synth class
  member (per-instance, derived from node GUID) + guarded `if` block.
- **Latent actions**: `Delay` / `RetriggerableDelay` /
  `DelayUntilNextTick` → `GetWorld()->GetTimerManager().SetTimer(...)`
  + generated `UFUNCTION()` continuation method + `FTimerHandle`
  member. Nested delays chain naturally.
- **EnhancedInput**: `K2Node_EnhancedInputAction` → per-trigger
  `UFUNCTION()` callback + `TObjectPtr<UInputAction>` UPROPERTY +
  auto-generated `SetupPlayerInputComponent` override wrapping
  `EIC->BindAction(...)` in a `Cast<UEnhancedInputComponent>` guard.
- **ConstructionScript / UserConstructionScript** →
  `virtual void OnConstruction(const FTransform&) override`.

`transpile_blueprint` writes a `<Class>_Generated.transpile-notes.json`
sidecar listing every unsupported node + manual step. Use it as a
triage list when you take the generated source past the "compiles"
line.

## Things to know

### Daemon mode

The first call to a real-UE backend takes 5–30 s (editor cold start;
varies with DDC warmth). Every subsequent call within the same MCP
session takes ~30 ms. Background: the server keeps one
`UnrealEditor-Cmd.exe` alive across calls and pipes commandlet-arg
lines to its stdin. If the daemon dies (crash, OOM, killed), the next
call falls back to a one-shot subprocess automatically — no error
surfaced to you.

Disable daemon mode for debugging with `BP_READER_DAEMON=0`.

### Pre-warm (seamless first call)

Set `BP_READER_PREWARM=1` to have the MCP server spawn the editor
daemon on a background thread the moment it starts. Claude Code spawns
MCP servers eagerly at session start, so the editor warms while you
type — by the time you ask Claude about a Blueprint, the daemon is
ready and your "first" call returns in ~30 ms.

The repo's shipped `.mcp.json` has this on. Costs ~600 MB of RAM for an
editor process that sits idle even if you never invoke a BP tool that
session. See [Configuration](Configuration#pre-warm).

### Hot-reload caveats

If you have the editor open in another window and the MCP server
modifies a blueprint, the editor doesn't always pick up the change
visually until you click off the asset and back. The change is on disk
and compiled — the UI is just stale.

### Reading vs editing safety

Read tools (`list_blueprints`, `read_blueprint`, `get_graph`,
`get_function`, `list_variables`, `get_components`, `find_node`) never
mutate state. Write tools always recompile the blueprint and save it.
There's no transaction or undo across MCP calls — if you ask Claude to
do something destructive, it commits immediately. Source control is
your safety net.

### Multiple projects

The MCP server is bound to one project per Claude session via
`BP_READER_PROJECT`. To work on multiple projects, configure each one
as its own MCP server entry with a different name:

```json
{
  "mcpServers": {
    "bp-reader-game1":  { "command": "...", "env": { "BP_READER_PROJECT": "...Game1.uproject", ... } },
    "bp-reader-game2":  { "command": "...", "env": { "BP_READER_PROJECT": "...Game2.uproject", ... } }
  }
}
```

Claude will pick the right one based on context.
