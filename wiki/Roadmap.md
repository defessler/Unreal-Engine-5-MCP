# Roadmap — bp-reader vs. the UE-MCP ecosystem

State of `bp-reader` (defessler/Unreal-Engine-5-MCP) as of mid-2026,
compared against the ~12 other UE MCP servers active on GitHub, and
the plan to close meaningful gaps.

## Where bp-reader stands

**59 tools** across 8 categories (12 read, 18 write, 3 batch, 6
transpile, 7 project + content browser, 12 live editor, 1
automation, 3 meta). Architecturally:

- 4 backends: mock / commandlet / live / auto (auto probes per call).
- Auto-discovery of `.uproject`, engine root, live port + token.
- 344 automation tests + 12 round-trip tests pinning BPIR ↔ C++
  identity for the patterns CppEmit produces.
- Sub-skill library for Claude Code: `bp-batches`, `bp-cpp`,
  `bp-debug`, plus a read-only audit subagent.

## Ecosystem survey (top 6 worth comparing)

| Server | Stars | Strengths |
|--------|------:|-----------|
| **chongdashu/unreal-mcp** | ~1.9k | The popular one. Actors + minimal BP graph authoring. Narrow surface. |
| **flopperam/unreal-engine-mcp** | ~900 | Broadest by a wide margin: materials, Niagara, Chaos, IK rig/retarget, MetaSound, PCG, GAS, GameplayTags, Landscape, Foliage, Sequencer, PIE-time runtime assertion testing. |
| **aadeshrao123/Unreal-MCP** | n/a | Deepest single-domain coverage: 96 Niagara commands, 35 material commands (incl. Substrate), 33 StateTree, Unreal Insights profiling (`performance_start_trace` + `analyze_insight` with diagnose / spikes / flame / hotpath modes). |
| **GenOrca/unreal-mcp** | ~90 | Cleanest granular design (68 named tools). Best-shaped material expression graph authoring (`create_expression`, `connect_expressions`, `recompile`), Behavior Tree + Blackboard CRUD, UMG widget BP CRUD. |
| **ChiR24/Unreal_mcp** | ~600 | Fat-dispatch design (22 multi-action tools). Gameplay-feature verbs: `manage_combat`, `manage_inventory`, `manage_ai`, `manage_networking`. |
| **remiphilippe/mcp-unreal** | ~22 | Only one with headless build / cook / automation + Live Coding hot-reload as first-class MCP tools. Bundles a local Bleve index of the UE 5.7 API docs. |

A few smaller ones (`atomantic/UEMCP`, `runreal/unreal-mcp`,
`runeape-sats/unreal-mcp`, `kvick-games/UnrealMCP`) cover narrower
surfaces (viewport ergonomics, Python remote-execution wrappers,
Remote-Control passthroughs). `ayeletstudioindia/unreal-analyzer-mcp`
is a different shape entirely — static analysis of UE C++ source via
tree-sitter.

## What bp-reader has that they don't

- **BP↔C++ round-trip via BPIR.** Genuinely unique. Flop has a
  `cpp_source` tool that exports BP→C++ but no bidirectional
  transpile + structured AST pivot.
- **344-test suite** around the MCP itself, including 12 BPIR ↔ C++
  identity-pinning round-trip tests. Other servers cite tests
  occasionally; none advertise coverage at this level.
- **Multi-backend architecture** with explicit decorator chain
  (mock → commandlet/live → caching → read-only) and an auto-probing
  router. Other servers pick one path and stick with it.
- **Zero-config live mode** — plugin auto-publishes port + token via
  a handshake file the MCP server reads. No two-process env-var
  setup.
- **Fine-grained Claude Code skills.** A master `bp-reader` skill
  plus sub-skills for batches, BP↔C++, and error triage, plus a
  read-only audit subagent. No other server ships skills.

## What they have that bp-reader doesn't

Ranked by **impact** for the audience that wants AI-driven UE
authoring, with rough effort estimates:

### Tier 1 — high-leverage gameplay surface

| Capability | Why it matters | Closest reference | Effort |
|-----------|----------------|-------------------|--------|
| **Material graph authoring** | Most-requested feature missing from bp-reader. UE materials are everywhere; agents need to set parameters + edit expressions for any rendering-aware work. | GenOrca's `create_expression` / `connect_expressions` / `recompile` + MI scalar/vector/texture/static-switch param tools (11 total). Aadeshrao's 35-command module covers Substrate too. | M-L |
| **UMG widget authoring** | UI is the second-most-frequent BP author target after gameplay code. | GenOrca's 6 widget-BP CRUD tools; Flop's `widget_inspect`/`widget_edit`; aadeshrao's 11. | M |
| **Component / SCS authoring** | Already planned as Cluster D. Foundational for spawning real BP actors. | Flop, GenOrca, ChiR24 all cover. | M |
| **DataTable / DataAsset CRUD** | Cluster E covers DataTable row mutation. Full DataAsset CRUD (generic UPrimaryDataAsset) is the missing piece. | Aadeshrao (8 DataTable + 12 DataAsset commands). | S-M |

### Tier 2 — impactful, more specialized

| Capability | Why it matters | Closest reference | Effort |
|-----------|----------------|-------------------|--------|
| **Behavior Tree + Blackboard CRUD** | AI behaviour is a common ask; BT/BB are well-defined assets that fit our existing pattern. | GenOrca's 12-tool module — full BB key CRUD + BT node build. | M |
| **Niagara / VFX editing** | Important for content authoring; deep API surface but the read path is tractable. | Aadeshrao goes deepest (96 commands incl. scratch pad). Flop has `niagara_inspect/edit/script_edit`. | L |
| **LevelSequence / Sequencer authoring** | Cinematic authoring; tracks + keyframes are the API. | Flop's `sequencer_edit`, ChiR24's `manage_sequence`. | M-L |
| **StateTree authoring** | Modern UE5 control-flow asset; underserved everywhere except aadeshrao. | Aadeshrao (33 commands). | M |
| **GAS + GameplayTag registry editing** | Gameplay Ability System is widely adopted in modern UE5 projects. | Flop's `gas_edit` + `tag_registry_edit`; ChiR24's `manage_gas`. | M |

### Tier 3 — developer ergonomics

| Capability | Why it matters | Closest reference | Effort |
|-----------|----------------|-------------------|--------|
| **Unreal Insights profiling** | Performance auditing closes the loop for AI-driven optimization workflows. | Aadeshrao's `performance_start_trace / stop / analyze_insight` (diagnose / spikes / flame / hotpath / histogram). Flop's `performance_audit`. | M |
| **Headless build / cook / Live Coding** | Already partially in our commandlet daemon; just need explicit MCP tools. Live Coding compile already lives in Cluster B; cook + package are next. | remiphilippe (the only one with explicit tooling here). | S |
| **Local UE API doc search index** | Surfaces the right UE class / function by natural-language query without leaving the MCP. | remiphilippe (Bleve full-text index of UE 5.7 docs). | M |
| **Viewport ergonomics** | High-leverage scene composition: `snap_to_socket`, `line_trace`, `spawn_on_surface_raycast`, `viewport_fit / look_at / bounds`. | atomantic + GenOrca cover these. | S |
| **Animation / IK rig / IK retarget / AnimGraph** | Lower priority than BT but important for character work. | Flop has the broadest set; remiphilippe has AnimBP state machines. | L |

### Tier 4 — out of mainstream scope

| Capability | Notes |
|-----------|-------|
| World Partition / HLOD / DataLayers | Nobody covers this well. Out of scope for now. |
| Source control (Perforce/Git) | Nobody exposes it. Out of scope. |
| Chaos destruction / MetaSound / PCG / Landscape / Foliage | Niche specialty surfaces. Defer unless a user asks. |

## Plan — outstanding work + bridge to the ecosystem

Sequenced for incremental shipping. Each row is roughly one PR.

### Stage 0 — finish what's open (no new feature scope)

1. **Cluster D: Components / SCS authoring** (4 tools) — was deferred from the auto-mode push that delivered Clusters A/B/C. Needs careful `USimpleConstructionScript` + `USCS_Node` tree manipulation + Blueprint recompile.
2. **Cluster E: DataTable row mutation** (2 tools) — `add_data_row` / `set_data_row_value`. Needs `FStructProperty` reflection for setting struct fields by name. Pairs with the existing `list_data_tables` / `read_data_table` (Cluster A).
3. **`read_output_log` ring buffer** — register a custom `FOutputDevice` in `StartupModule` with a ring buffer; the existing `read_output_log` tool starts returning real entries. Currently a stub.
4. **Editor target build verification** — UBT-rebuild the editor with the merged Cluster A/B/C plugin code and fix any UE-API compile errors. Risk-mitigation for the ~2,500 lines of unverified plugin code that landed in PRs #9/#10/#11.

### Stage 1 — Tier 1 gameplay surface (highest ecosystem-parity payoff)

5. **Material graph authoring cluster.** ~8–10 tools modeled on GenOrca's API:
   - `list_materials(path)`, `read_material(asset_path)` — enumerate + dump material expression graph.
   - `add_material_expression(material, class, x, y)`, `connect_material_expressions(material, from_node, from_pin, to_node, to_pin)`, `set_material_parameter(material, name, value)`.
   - `compile_material(asset_path)`.
   - `list_material_instances(path)`, `set_mi_scalar / vector / texture / static_switch_param`.
6. **UMG widget authoring cluster.** ~6 tools:
   - `read_widget_blueprint(asset_path)`, `add_widget(parent_id, widget_class)`, `set_widget_property(widget_id, prop, value)`, `bind_widget_event(widget_id, event, handler_fn)`, `compile_widget_blueprint(asset_path)`.

### Stage 2 — Tier 2 gameplay feature surface

7. **Behavior Tree + Blackboard cluster.** ~8 tools: read/write BT nodes (composite, task, decorator, service), Blackboard key CRUD, BT compile.
8. **DataAsset CRUD extension** to Cluster E's DataTable work. Generic `UPrimaryDataAsset` get/set property + create-from-class.
9. **StateTree authoring cluster.** ~6 tools — state + transition + evaluator CRUD.

### Stage 3 — Tier 3 ergonomics

10. **Unreal Insights profiling.** ~3 tools: `start_trace`, `stop_trace`, `analyze_insight(mode: diagnose|spikes|flame|hotpath|histogram)`. Wraps the `Unreal.Trace.*` CVars and `UnrealInsights.exe -analyze` path.
11. **Headless cook + package + smoke-test.** ~3 tools. Most plumbing already exists in our commandlet daemon; these surface it explicitly.
12. **Local UE API doc search index.** Bundle a pre-indexed corpus of UE 5.7 headers + docs. One tool: `search_unreal_api(query, limit)`.
13. **Viewport ergonomics cluster.** ~6 small tools: `snap_actor_to_socket`, `line_trace`, `spawn_on_surface_raycast`, `viewport_fit`, `viewport_look_at`, `set_view_mode`.

### Stage 4 — niche specialties (only if users ask)

14. Niagara editing (deep — defer unless asked).
15. LevelSequence / Sequencer.
16. GAS + GameplayTag registry.
17. AnimGraph / IK rig / retarget.

## How the gap shapes priorities

Stage 0 closes work the codebase already opened. Stages 1–2 close
ecosystem-parity gaps that almost every other server covers. Stage 3
is about developer ergonomics — the small tools that punch above
their weight. Stage 4 is on demand.

If we add **everything through Stage 2** (items 1–9), bp-reader
covers approximately the same surface as flopperam (the
broadest-by-a-wide-margin server), plus retains its unique BP↔C++
transpile + multi-backend architecture + test coverage advantages.
Estimated effort: ~6–8 PRs depending on cluster size, each shippable
incrementally.

## Where the ceiling is

bp-reader's narrow-tool design (one MCP tool per concrete op) is the
right call for AI clients: agents pick from a known set, each tool's
schema is self-describing, errors attribute cleanly. The cost is
that adding new domain coverage requires real tool design — we can't
do what ChiR24 did ("one fat `manage_combat` dispatcher") because
that pattern produces worse agent UX. So Tier-1 + Tier-2 expansion
will add roughly **20–30 new tools** to reach ecosystem parity, not
5–10.
