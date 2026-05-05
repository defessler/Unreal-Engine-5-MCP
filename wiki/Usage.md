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
- **"What variables does it have?"** → `list_variables` or `read_blueprint`
- **"Show me the event graph"** → `get_graph`
- **"Find any `K2Node_VariableSet` nodes"** → `find_node` with `kind: variable_set`
- **"What components are on this actor?"** → `get_components`
- **"What does `ApplyDamage` do?"** → `get_function`

`find_node` is the workhorse for "what touches X" questions — it returns
nodes filtered by class name, title substring, or `kind` (a normalized
classification of K2 node types).

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
| Add a member var with type X         | `add_variable`                                                             |
| Add an editable replicated var       | `add_variable` (with `editable: true`, `replicated: true`)                 |
| Spawn a Branch + wire exec           | `add_node` (kind=`branch`) → `wire_pins`                                   |
| Add a function that reads a var      | `add_function` → `add_function_input` (if needed) → `add_node` (`variable_get`) → `wire_pins` |
| Refactor: rename + update call sites | `rename_variable` (graphs are auto-updated)                                |
| Audit: where is X read?              | `find_node` with `class_filter: K2Node_VariableGet`, then `query: VarName` |

### Discovering valid node kinds and pin types

Two meta tools tell you what the schema accepts:

- `list_node_kinds` → all `kind` values `add_node` understands plus their
  required extras (`variable`, `function_owner`, `target_class`, ...).
- `list_pin_categories` → canonical `BPPinType.category` values
  (`bool`, `int`, `float`, `string`, `name`, `text`, `object`, `class`,
  `struct`, `enum`, ...) plus container modifiers (`array`, `set`, `map`).

Claude calls these on its own when it's unsure — you don't need to.

## Things to know

### Daemon mode

The first call to a real-UE backend takes 5 s (editor cold start). Every
subsequent call within the same MCP session takes ~30 ms. Background:
the server keeps one `UnrealEditor-Cmd.exe` alive across calls and pipes
commandlet-arg lines to its stdin. If the daemon dies (crash, OOM,
killed), the next call falls back to a one-shot subprocess automatically
— no error surfaced to you.

Disable daemon mode for debugging with `BP_READER_DAEMON=0`.

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
