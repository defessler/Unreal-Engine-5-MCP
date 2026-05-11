# Copilot instructions — UE5_MCP

This repo is a UE 5.7.4 project plus a standalone MCP server that
exposes 39 Blueprint tools to AI assistants. If you're a Copilot-class
agent (GitHub Copilot, Cursor, JetBrains AI Assistant, etc.) working
in this codebase, here's what you need to know.

## Two halves

- **`Plugins/BlueprintReader/`** — Editor-only UE plugin (commandlet
  + TCP live server) that talks to `.uasset` Blueprint files.
- **`Plugins/BlueprintReader/mcp-server/`** — Standalone C++20 MCP
  server (JSON-RPC over stdio) that drives the plugin.

The MCP server is the cross-platform interface — every assistant uses
the same tool surface regardless of client.

## The tool surface

77 tools across 10 categories. Don't memorize the list — call
`tools/list` first to see the current schemas. Categories:

- **Read** (10): `list_blueprints`, `summarize_blueprint`,
  `read_blueprint`, `get_graph`, `get_function`, `list_variables`,
  `get_components`, `find_node`, `get_node`, `find_overriders`.
- **Write** (18): variables, functions, graph topology, asset
  creation. All idempotent where idempotency makes sense.
- **Batch / generation** (3): `apply_ops`, `preview_ops`,
  `compile_function`. Use these for multi-step work — they collapse
  N×compile to 1 and offer named-slot GUID resolution + dry-run.
- **Transpile (BP↔C++)** (6): `decompile_function`,
  `decompile_blueprint`, `transpile_function`, `transpile_blueprint`,
  `write_generated_source`, `parse_cpp_function`. Pivot on a JSON
  AST called BPIR.
- **Project + Content Browser** (7): `get_project_metadata`,
  `save_all`, `move_asset`, `delete_asset`, `create_folder`,
  `list_data_tables`, `read_data_table`. Project-level introspection
  + asset-browser ops alongside the per-Blueprint surface.
- **Live editor** (12): `console_command`, `get_cvar`, `set_cvar`,
  `pie_start`, `pie_stop`, `live_coding_compile`, `get_selected_actors`,
  `set_selection`, `spawn_actor`, `set_actor_transform`, `delete_actor`,
  `read_output_log`. Operate on the running editor's in-memory state;
  most useful with the `live` backend (open editor).
- **Automation** (1): `run_automation_tests`. Kick off UE's
  automation suites with a wildcard pattern.
- **Material authoring** (7): `list_materials`, `read_material`,
  `add_material_expression`, `connect_material_expressions`,
  `set_material_parameter`, `set_material_instance_parameter`,
  `compile_material`. Walk + edit the UMaterial expression graph
  and override MIC parameters.
- **UMG widgets** (5): `read_widget_blueprint`, `add_widget`,
  `set_widget_property`, `bind_widget_event`,
  `compile_widget_blueprint`. Author UWidgetBlueprint trees.
- **Discoverability + meta** (3): `list_node_kinds`,
  `list_pin_categories`, `shutdown_daemon`.

The wiki page **Tool Reference** has every tool's input/output shape
with examples:
https://github.com/defessler/Unreal-Engine-5-MCP/wiki/Tool-Reference

## Wire format quickstart

- **Asset paths are package paths.** `/Game/AI/BP_Foo`. Never object
  paths (`/Game/AI/BP_Foo.BP_Foo`), never disk paths.
- **Type shorthand** for any `type` arg: `"float"`, `"int"`,
  `"object:Actor"`, `"struct:FVector"`, `"interface:IDamageable"`,
  `"enum:EWeaponType"`, `"[]float"`, `"{}int"`, `"{string:int}"`. The
  full BPPinType object form also works.
- **Pin IDs are GUIDs.** Prefer them over names when wiring across
  multiple calls; names work for the immediate `wire_pins` after an
  `add_node` (the response includes the full pin list).
- **`BPNode.meta` is a real nested JSON object**, not a
  string-of-JSON.

## Tool selection cheat sheet

| Want to... | Tool |
|------------|------|
| Orient cheaply on an unknown BP | `summarize_blueprint` |
| Inventory under a content path | `list_blueprints` |
| Get parent class + variable / function names | `read_blueprint` (with `fields`) |
| Read one function's signature + body | `get_function` (NOT `get_graph`) |
| Read one graph's topology | `get_graph` |
| "Where is X used?" within a BP | `find_node` (+ `kind` filter) |
| Cross-BP structural query | `find_overriders` |
| Build a function from pseudocode | `compile_function` |
| Multi-step write with rollback semantics | `apply_ops` (+ `preview_ops`) |
| Convert BP to C++ for review | `transpile_function` |
| Convert BP class to compilable C++ | `transpile_blueprint` + `write_generated_source` |
| Convert C++ → BP graph | `parse_cpp_function` → `compile_function` |
| Read `.uproject` metadata | `get_project_metadata` |
| Save all dirty packages | `save_all` |
| Move / rename / delete an asset | `move_asset` / `delete_asset` |
| Create a content-browser folder | `create_folder` |
| Inventory + read DataTables | `list_data_tables` / `read_data_table` |
| Run a console command in the editor | `console_command` |
| Read / write a console variable | `get_cvar` / `set_cvar` |
| Start / stop Play-In-Editor | `pie_start` / `pie_stop` |
| Trigger Live Coding rebuild | `live_coding_compile` |
| Inspect / change the editor's selection | `get_selected_actors` / `set_selection` |
| Spawn / move / delete actors in the level | `spawn_actor` / `set_actor_transform` / `delete_actor` |
| Read recent editor log entries | `read_output_log` |
| Run UE automation tests | `run_automation_tests` |

## Performance — assume daemon mode

Default backend is `auto`. Each call probes whether an editor is open;
routes to live (~5–30 ms) or commandlet daemon (~5 s cold start, then
~15–30 ms thereafter). So:

- **Don't worry about call count.** A 20-step refactor is sub-second
  after the first call.
- **Batch for atomicity, not latency.** `apply_ops` collapses a
  multi-op sequence to one compile + one save — that's the value, not
  network round-trip reduction.

## Idempotency + error self-correction

- `add_variable`, `add_function`, `create_blueprint`,
  `duplicate_blueprint` — all idempotent. Calling with a duplicate
  returns `{ok:true, already_existed:true}` instead of throwing.
- `wire_pins` errors include both pin types so you can fix in one
  turn without re-reading the graph.
- `apply_ops` results carry a `diagnostics[]` array with `op_index`
  for compile failures — you can attribute errors to specific ops.

## The five things to NOT do

1. **Don't fabricate node kinds.** If you need an unfamiliar kind,
   call `list_node_kinds` first. Currently supported kinds are
   limited; spawning Switch / GetClassDefaults / SpawnActorFromClass
   etc. requires plugin work first.
2. **Don't treat the mock backend as writable.** All write tools
   throw with a clear message pointing at
   `BP_READER_BACKEND=commandlet`. Surface that to the user — don't
   silently fail.
3. **Don't dump whole graphs into context.** Use `fields` projection
   on every read. AI clients pay tokens for every byte.
4. **Don't hardcode tool counts or fixture asset names** in
   generated code or docs — both rotate.
5. **Don't manually walk N BPs when `find_overriders` covers the
   predicate.** It does the structural query in one call.

## Where to dig deeper

- **README.md** — top-level project overview + tool table + Claude /
  Copilot config snippets.
- **CLAUDE.md** — building / testing / maintaining the project
  itself. Read this if you're modifying the MCP server or plugin.
- **`wiki/`** — the source of truth for the GitHub Wiki:
  - **Tool-Reference** — every tool's input/output shape.
  - **BPIR** — the JSON AST that BP↔C++ pivots on.
  - **Configuration** — env vars, daemon mode, live backend setup.
  - **Usage** — conversational examples.
  - **Troubleshooting** — common failure modes + fixes.
- **`Plugins/BlueprintReader/mcp-server/scripts/`** — smoke harnesses
  that drive the MCP server end-to-end. Read these to see real
  request/response patterns.

## Adding tools (if you're modifying the MCP server itself)

CLAUDE.md has the full checklist. Short version: every tool needs an
entry in the plugin's `EOp` + dispatch table, the
`IBlueprintReader` interface, the relevant backends, the
`BlueprintTools.cpp` registry, and tests. Tool count assertions in
`test_tools.cpp` and `test_mcp.cpp` need bumping.

## Don't fabricate workarounds

If a request hits a tool that doesn't exist (or a node kind not in
`list_node_kinds`), say so directly and tell the user what *is*
supported. Don't fabricate a multi-tool dance that quietly fails on
the unsupported case.
