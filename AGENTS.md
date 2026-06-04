# AGENTS.md — BlueprintReader MCP

Guidance for any AI agent (Claude, Copilot, Codex/GPT, Cursor, Aider, …)
working with the **BlueprintReader** MCP server — a standalone tool surface
that lets an assistant inspect, edit, transpile, and drive **Unreal Engine 5
Blueprints** and the live editor.

> This file is the portable source of truth (it ships inside
> `Plugins/BlueprintReader/`). The repo root `AGENTS.md` and
> `.github/copilot-instructions.md` are deployed copies — edit *this* file and
> run `Plugins/BlueprintReader/Scripts/Install-ClaudeAssets.ps1` to refresh
> them. Claude users get the deeper, auto-discovered skills under `.claude/`.

## The tool surface — discover it, don't memorize it

The server exposes a large tool surface (BP read/write, batch generation,
BP↔C++ transpile, materials, UMG, behavior trees, data assets, live editor,
profiling, class reflection, …). **Counts and names rotate — never hardcode
them.** Two rules:

1. **Call `tools/list` first.** It is the authoritative, current catalog.
2. **Progressive disclosure is on by default.** `tools/list` advertises only a
   small `core` set plus meta-tools — *not* the whole surface — to keep token
   cost down. To reach the rest:
   - `enable_tool_category("<category>")` widens the advertised set (then a
     `notifications/tools/list_changed` fires), **or**
   - `call_tool("<name>", { …args… })` invokes *any* tool directly without
     widening — cheapest when you already know the tool name.
   - `list_toolsets` / `describe_toolset` enumerate what's available.

For an offline catalog (without running the server) see the generated tool
reference at
<https://github.com/defessler/Unreal-Engine-5-MCP/blob/main/docs/TOOLS.md>.

## Wire format (read before your first call)

- **Asset paths are package paths**: `/Game/AI/BP_Foo` — never object paths
  (`/Game/AI/BP_Foo.BP_Foo`) or disk paths.
- **`type` arg accepts a shorthand**: `"float"`, `"int"`, `"bool"`,
  `"object:Actor"`, `"struct:FVector"`, `"interface:IDamageable"`,
  `"enum:EWeaponType"`, `"[]float"` (array), `"{string:int}"` (map). The full
  `BPPinType` object form also works.
- **Pin IDs are GUIDs** — prefer them over names when wiring across calls.
- **`BPNode.meta` is a real nested JSON object**, not a string-of-JSON. Keys
  are snake_case; absent optional strings are `null`.
- **`fields` projection**: every read tool takes a `fields` array (dotted
  paths, e.g. `["parent_class","variables[].name"]`). Use it — you pay tokens
  for every byte returned. List tools take `limit`/`offset`/`cursor`.

## Pick the right tool

| Want to… | Tool |
|----------|------|
| Orient cheaply on an unknown BP | `summarize_blueprint` |
| Inventory under a content path | `list_blueprints` (typed) / `list_assets` (any class) |
| Find an asset by substring | `find_asset` (paginated) |
| Read a Blueprint class's structure | `read_blueprint` (+ `fields`) |
| Read a level-placed / OFPA actor instance + its overrides | `read_actor_instance` |
| Read one function's signature + body | `get_function` (not `get_graph`) |
| Read one graph's node topology | `get_graph` |
| "Where is X used / who overrides Y?" | `find_node` / `find_overriders` |
| Build a function from pseudocode | `compile_function` |
| Multi-step write (one compile+save, dry-run) | `apply_ops` / `preview_ops` |
| BP ↔ C++ | `transpile_function` / `decompile_function` / `parse_cpp_function` |
| Drive the running editor | `console_command`, `pie_start`/`pie_stop`, `spawn_actor`, … |

`read_blueprint` is `UBlueprint`-shaped — for non-Blueprint assets use the
typed reader (`read_material`, `read_data_asset`, …) or `read_actor_instance`.

## Performance — assume daemon mode

Default backend is `auto`: each call routes to a live editor (~ms) or a
commandlet **daemon** (~seconds cold-start, then ms). So **don't minimize call
count** — a 20-step refactor is sub-second after the first call. Batch with
`apply_ops` for *atomicity* (one compile + one save), not for latency.

## Don't

1. **Don't fabricate node kinds.** Call `list_node_kinds` for valid `add_node`
   kinds; say so if what you need isn't supported.
2. **Don't dump whole graphs into context** — project with `fields`, paginate
   lists.
3. **Don't treat the mock backend as writable** — write tools throw a clear
   message; surface it (set `BP_READER_ALLOW_WRITE=1` / use a real backend).
4. **Don't hardcode tool counts or fixture asset names** — both rotate; read
   `tools/list` (or the generated catalog at
   <https://github.com/defessler/Unreal-Engine-5-MCP/blob/main/docs/TOOLS.md>).
5. **Don't invent a multi-tool workaround for an unsupported op** — state
   what *is* supported.

## Staying current

Run `bp-reader-mcp doctor` to check for staleness and update availability. If an
update is available, run `Setup-Plugin.bat` (or `Scripts/Update-Plugin.ps1`) from
the plugin root — it downloads the latest plugin from GitHub over HTTPS (no git
required), redeploys it (preserving the built server binary), and reconfigures the
MCP client config + Claude assets.

You can also check manually:
```
Scripts\Check-Update.bat            # prints "vX.Y.Z available" or "up to date"
Scripts\Update-Plugin.bat           # no-build refresh + reconfigure
Scripts\Build-MCPServer.bat         # rebuild the server exe (after a source update)
```

After installing an update, restart Claude Code / your MCP client to pick up any
new skills or changed tool descriptions.

## Deeper docs

- **Claude skills** — auto-discovered per-task playbooks (bp-reader, bp-batches,
  bp-cpp, bp-debug) + the bp-audit agent. Deployed to `.claude/skills/` in this
  project; source in `Plugins/BlueprintReader/Claude/skills/`.
- **Changelog** — what changed in each release: `Plugins/BlueprintReader/CHANGELOG.md`.
- **Repo docs** — build / test / maintain (`CLAUDE.md`), full setup, per-tool
  I/O shapes, env vars (README), and the wiki (Tool-Reference, BPIR,
  Configuration, Usage, Troubleshooting):
  <https://github.com/defessler/Unreal-Engine-5-MCP>.
