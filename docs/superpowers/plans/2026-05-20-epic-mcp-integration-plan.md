# Epic MCP Integration Plan (v10)

**Status:** Active plan. Supersedes all prior versions.
**Last revised:** 2026-05-21 (Phase B landed in `12373937` + `e6f54125` + `1e542220`)
**Scope:** Multi-month integration of Epic UE 5.8 ModelContextProtocol plugin patterns into our BlueprintReader MCP server, plus Lyra value-add toolsets, plus editor user-state awareness surface.

Built from:
- 280+ findings across multiple deep-research passes of `Engine/Plugins/Experimental/{ModelContextProtocol, ToolsetRegistry, Toolsets}` on the `5.8` branch of `EpicGames/UnrealEngine`
- 200 explicit numbered iterations specifically on how the editor communicates user state (selection, visibility, opened assets)
- All prior plan validation passes (v1–v6) cross-checked against MCP spec + Epic implementation

Related memory files for context:
- `reference_epic_mcp_plugin.md` — plugin location + access path
- `reference_epic_mcp_architecture.md` — module loading, request lifecycle, tool registration, spec conformance status
- `project_epic_mcp_strategic_landscape.md` — strategic posture: differentiate vs integrate

Related plan-tier docs:
- [`2026-05-20-epic-mcp-plan-baseline.md`](./2026-05-20-epic-mcp-plan-baseline.md) — measured baseline (test count, tool count, capabilities, Phase A commit state). Every "from N to M" claim in this plan rests on those numbers. Re-measure quarterly.

---

## Preconditions (must hold before any new phase begins)

1. **Phase A landed on `origin/main` as `66954afd` (2026-05-20 23:58 PT).** 42 files / 2960 insertions / 54 deletions — single atomic commit bundling Phase A scaffolding with agent-feedback round-2 fixes (`list_assets`, `find_asset`, did-you-mean, UClass validation, `get_class_info` declared_on filter). The pre-shipped commit `3e107c65 feat(mcp): tool annotations per MCP 2025-03-26 §tools/annotations` is the second half of the Phase A surface. **Phase A-merge gate is satisfied** — no further commit sequencing is required.
   - Note: the v8 plan called for splitting Phase A into 8 reviewable commits. That was authored after `66954afd` had already been pushed. Splitting now would require force-pushing a published commit on `main` (rejected per safety protocol). Downstream phases pay no penalty — the topology is monolithic, but the surface is identical.
   - **Exit criteria for Phase A-merge:** **(satisfied 2026-05-21)** working tree clean, `BlueprintReaderMcpTests.exe` passes 620/620 unskipped (16 live-only skipped), `tools/list` advertises 132 tools on commandlet/live (47 on mock — many ops declared unsupported), initialize negotiates 2025-06-18.
   - **Kill-switch:** none required (Phase A is purely additive — capabilities aren't claimed, tools aren't removed).

2. **Baseline numbers calibrated.** v7 plan referenced "603 tests / 128 tools." Real numbers post-Phase-A: **620 tests run / 636 in source / 132 tools (commandlet/live) / 47 tools (mock).** All cumulative-test-count claims in the rest of this plan rebase off 620.

3. **Capability state verified.** Initialize advertises only `{tools: {listChanged: true}}` today. The "Capability: ___ advertised" lines in upcoming phases (Prompts, Resources, Logging) move the needle from this baseline; treat them as the canonical "did this phase ship?" signal.

---

## Strategic posture

Our differentiation moat: **BP↔C++ transpile + BPIR**, **out-of-process safety**, **cooked-build runtime introspection**, **mock backend** (zero UE required for tests).

Epic's wire-level surface (HTTP+SSE, sessions, lazy discovery, resources, pagination, analytics) is what we adopt for parity. Epic's tool surface (EditorAppToolset, GASToolsets, etc.) is what we selectively reimplement for Lyra value-add. Push events (`notifications/editor/*`) are the **biggest agentic differentiator** Epic doesn't have.

---

## Phase summary

The **Cost-of-delay** column captures what the user loses if this phase is pushed by 4 weeks — used for tiebreakers when two phases compete for a slot.

| # | Phase | Days | Cumulative | Cost of delay | Strategic role |
|---|---|---|---|---|---|
| 0 | **A** Foundation **(SHIPPED in `66954afd` + `3e107c65`)** | — | 0d | — | Tool registration, lazy discovery, content blocks, regex filter, CallContext, HttpTransport frame layer, UDeveloperSettings |
| 1 | **B** Hardening + `instructions` + compat matrix **(SHIPPED in `12373937` + `e6f54125` + `1e542220`)** | — | 0d | — | Polish + spec compliance + backwards-compat snapshot guards |
| 2 | **D** outputSchema + Image rollout **(SHIPPED locally — pending push)** | — | 0d | — | LLM-side response shapes |
| 3 | **Prompts** primitive **(SHIPPED locally — pending push)** | — | 0d | — | Slash commands (universal client support) |
| 4 | **C1** Resources primitive **(SHIPPED locally — pending push)** | — | 0d | — | URI-addressable assets |
| 5 | **C2** Pagination cursors **(SHIPPED locally — pending push)** | — | 0d | — | Spec-conformant list paging |
| 6 | **Logging** primitive **(SHIPPED locally — pending push)** | — | 0d | — | Server logs to client |
| 7 | **E** Analytics + safety **(SHIPPED locally — pending push; scaffolding only)** | — | 0d | — | Observability + path safety |
| 8 | **EA-pull Wave 1** Core awareness **(25 of 25 + `list_desktop_windows` SHIPPED; `take_desktop_screenshot` composite deferred)** | — | 0d | — | Selection, open, compile, modal, viewport, components, content-browser, spatial, Slate UI — the "what's the user doing?" core |
| 9 | **C3–C5** HTTP + Sessions + SSE | 13–15 | 46d | Very High — gates Push events entirely | Transport for push events |
| 10 | **EA-push Wave 1** Core event notifications | 8–10 | 56d | Very High — biggest agentic differentiator vs Epic | Tier-A SSE events on user actions |
| 11 | **H Tier 1** PluginToolset + GameFeatures + GAS **(✅ COMPLETE 12/12 — 3 toolsets' reads + GFP activate/deactivate + set_plugin_enabled .uproject write)** | 10–12 | 68d | Med (High for Lyra users) | Lyra value-add |
| | **— wire parity + core differentiation: ~13.5 weeks —** | | | | |
| 12 | **EA-pull Wave 2** Per-asset-editor selection **(11 of ~15 SHIPPED — 8 functional + 3 stubs anim/niagara/curve_editor; remaining work is sidecar-registry design)** | 6–8 | 76d | Med — drills into specific workflows | BP/Material/Sequencer/Anim/UMG selection state |
| 13 | **EA-pull Wave 3** Viewport + visibility **(COMPLETE — 18 tools; +4 final: get_camera_bookmarks/goto_camera_bookmark functional, get_hover_target + get_isolate_mode shipped as documented stubs pending editor-module sidecar)** | 5–7 | 83d | Med — viewport-aware agent UX | Show flags, view mode, hover, gizmo, camera |
| 14 | **EA-pull Wave 4** World + SCC + system state **(12 of ~25 SHIPPED — +get_recently_opened_assets via editor MRU)** | 6–8 | 91d | Low (Med for Lyra: partition+data layers) | Partition, data layers, SCC, outliner, recent activity |
| 15 | **EA-push Wave 2** Extended event coverage | 5–7 | 98d | Low — additive on top of Wave 1 push | Tier-B/C events + Lyra-specific (GFP, partition cells) |
| 16 | **H Tier 2** ConfigSettings + Automation | 6–8 | 106d | Low (Med for Lyra) | More Lyra value-add |
| | **— feature-complete EA + H: ~21 weeks —** | | | | |
| 17 | **EA-pull Wave 5** Advanced + niche **(8 of ~20 SHIPPED — +get_active_stats; BP-debug trio + culture/theme/monitor/live_coding/stats)** | 5–7 | 113d | Very Low — demand-driven | BP debug, takes, render queue, modeling tools, console history |
| | **— full plan execution: ~22.5 weeks —** | | | | |
| — | Deferred / demand-driven | — | — | (see Decision log) | G visual annotation, J cancellation, K MCP-as-client, L file sandbox, M reflection, EU Slate write-control, multi-user concert, Live Link |

---

## Dependency graph

Reveals which phases can be parallelized vs which must wait.

```
A (SHIPPED — 66954afd + 3e107c65)
  │
  ├──> B Hardening (SHIPPED — 12373937 + e6f54125 + 1e542220)
  │      │
  │      └──> D outputSchema (uses output_schema field added in B's hardening)
  │
  ├──> Prompts          (independent — could ship in parallel with B)
  │
  ├──> C1 Resources     (independent — could ship in parallel with B + Prompts)
  │      │
  │      └──> C2 Pagination cursors (applies cursor pattern uniformly to list endpoints)
  │
  ├──> Logging          (independent — could ship in parallel with B + Prompts + C1)
  │      │
  │      └──> E Analytics + safety (analytics events route through logging primitive)
  │
  ├──> EA-pull Wave 1   (independent — only needs B's path-safety helper)
  │      │
  │      ├──> H Tier 1 (PluginToolset uses asset open/close from Wave 1)
  │      │
  │      ├──> EA-pull Wave 2 (per-asset selection — extends Wave 1 patterns)
  │      │      │
  │      │      └──> EA-pull Wave 3 (viewport surface — same backend pattern)
  │      │              │
  │      │              └──> EA-pull Wave 4 (world/SCC)
  │      │                      │
  │      │                      └──> EA-pull Wave 5 (advanced)
  │      │
  │      └──> H Tier 2 (uses ConfigSettings tooling pattern from Tier 1)
  │
  └──> C3-C5 HTTP+Sessions+SSE  (independent — but only useful once …)
         │
         └──> EA-push Wave 1 (needs SSE transport to push)
                │
                └──> EA-push Wave 2 (extends Wave 1 — same infrastructure)
```

**Parallelism windows:**
- A + B are shipped; {D, Prompts, C1, Logging, EA-pull Wave 1, C3-C5} can all start now. If solo, sequence by cost-of-delay. If two parallel tracks: track 1 = {D → Prompts}; track 2 = {C3-C5 (longest) → EA-push Wave 1}.
- Once C2 + Logging ship: E unblocks.
- Once Wave 1 ships: {H Tier 1, EA-pull Wave 2, EA-push Wave 1 (if C3-C5 done)} can run in parallel.

---

## Velocity calibration

From measured Phase A work (see baseline doc):
- Phase A ≈ ~12 substantive items + tests, shipped in ~1 calendar day of focused work
- Net ≈ **2 items/day** when execution is clean (no rework, no spec ambiguity)

Day-estimate conventions in this plan:
- "1 day" = 1 calendar day, 6 focused hours, **assumes** no rework loop and no spec ambiguity
- Per-tool estimates assume the IBlueprintReader interface pattern is already in place (it is) — each new EA tool ≈ 4 hours wired across 3 backends + 3 tests
- C3–C5 SSE is the **single most likely overrun** in the plan (allocate 50% buffer in scheduling)
- High-uncertainty phases tagged in the phase summary with "Cost of delay: Very High" should run the spike before committing the full day budget

**Re-calibration trigger:** if a shipped phase exceeds its estimate by >50%, update the velocity assumption in this section + propagate to remaining estimates.

---

## Pre-phase spike timeboxes (high-risk only)

Spike timeboxes attempt to drive uncertainty out *before* committing weeks of work. Each spike has hard exit conditions; if they aren't met, the parent phase is re-planned, not extended.

| Spike | Days | Belongs to | Exit conditions (must all hold to proceed with full phase) |
|---|---|---|---|
| **HTTP+SSE smoke** | 2 | C3–C5 | (a) `cpp-httplib` vendored under `Tests/ThirdParty/` and links into BlueprintReaderMcpCore; (b) one POST `tools/list` round-trips JSON-RPC against `127.0.0.1:0`; (c) one SSE `notifications/message` flushes mid-response on the same socket; (d) DNS-rebinding Origin guard rejects `Origin: http://attacker.com`. **Fail mode:** if cpp-httplib's chunked transfer + per-session state proves harder than 2 days of work, re-evaluate vs raw socket loop (a la live TCP backend) before committing 13–15 days. |
| **EA-push wiring** | 2 | EA-push Wave 1 | (a) `USelection::SelectionChangedEvent` subscribed in editor module; (b) live TCP backend protocol extended with `{type: "event", name, params}` message variant; (c) MCP server emits `notifications/editor/level_actor_selection_changed` to one stdio session; (d) round-trip latency < 100 ms (selection → notification). **Fail mode:** if delegate plumbing requires UE-side architectural change (e.g., editor module re-architecture), surface to plan owner before scaling to 11 events. |
| **GAS toolset conditional compile** | 1 | H Tier 1 | (a) `BUILD_WITH_GAS_TOOLSET=1` define gates `AbilitySystemInspectorToolset.cpp` inclusion in `BlueprintReaderMcpCore.Build.cs`; (b) editor module + MCP server still build cleanly with the flag undefined; (c) one trivial GAS-introspection method (`GetActiveTags(ASC)`) wired end-to-end. **Fail mode:** if conditional compile leaks GAS symbols into non-Lyra builds, switch to runtime-discovery via reflection. |
| **Resources URI parser** | 1 | C1 Resources | (a) `bp://` URI scheme parser handles `bp:///Game/...`, `bp:///_project`, `bp:///_output_log`; (b) `IResourceProvider` interface compiles with one mock provider; (c) `resources/list` returns 1 descriptor over stdio. **Fail mode:** if `bp:///` vs `bp://` ambiguity bites in clients, settle on one form before scaling providers. |

**Total spike budget: 6 days.** Run before any of the parent phases' day budgets are consumed. Bills against the corresponding phase's cumulative estimate.

---

## Per-phase exit criteria template

Every phase declares its exit conditions before work starts. Template:

```
Exit criteria for Phase X:
  - All N new tests pass on Win64 (BlueprintReaderMcpTests.exe), live tests pass on at least one Linux runner if CI is up
  - tools/list response size unchanged for older clients (zero-break) OR documented breaking change
  - At least one real client (Claude Code) successfully exercises the new capability
  - Server initialize advertises any new capability (e.g., resources: {}, logging: {})
  - Baseline doc updated with new measured counts
  - Kill-switch verified: disabling the relevant env var or setting restores prior behavior
  - One agent-flow smoke test documented in the phase's PR description
```

Phases below carry their own exit criteria block; this is the template they conform to.

---

## Phase B — Hardening + backwards-compat infrastructure (2–3 days) — ★5

Polish what already exists. Lowest-friction wins.

### Deliverables

- `sort` opt-in for `list_*` tools (default `"natural"` preserves current order).
- Path-traversal protection helper applied to 3 screenshot tools + `write_generated_source`.
- `call_tool` self-dispatch recursion guard.
- Strict JSON-RPC 2.0 version validation.
- `HasValidTools()` pre-flight check (refuse to start with zero tools).
- Dotted tool-name aliases (`<category>.<tool>` alongside flat names) for Epic interop.
- `notifications/cancelled` marks `CallContext` (no-op until Phase J worker-thread dispatch).
- `instructions` field in initialize response — free system-prompt context describing project conventions, BPIR pivot, backend selection.
- HttpTransport GET returns 405 (match Epic) not 501.
- `ToolRegistry::Add` validation = warn-not-throw on length/character violations (match Epic permissiveness); reject only empty.

### Backwards-compat test matrix (also lands in this phase)

This is what proves the "zero-break" claim for every later phase:

- `tests/fixtures/protocol_2024-11-05/initialize_request.json` + `tools_list_response.json` — pinned response for old client
- `tests/fixtures/protocol_2025-03-26/...` — pinned for mid-version client
- `tests/fixtures/protocol_2025-06-18/...` — current server default
- `test_protocol_compat.cpp` — for each fixture, exercise initialize → tools/list → snapshot diff. Fails if either responding-version or tool count changes unexpectedly.
- `tests/fixtures/tools_list_snapshot.json` — committed deterministic dump of every advertised tool. CI fails on unintended diff. Re-baseline by `--write-snapshot` flag on the test exe.

### Exit criteria

- All 20 new B tests pass; 620 prior tests still pass unmodified (zero-break)
- 3 protocol fixtures green; tools/list snapshot test green
- Claude Code smoke: `initialize` with no `protocolVersion` returns 2025-06-18 (no regression)
- Baseline doc updated with new test count (target: 640 = 620 + 20)
- Kill-switch verified: `BP_READER_SORT_DEFAULT=natural` (default) reproduces prior behavior

### Kill-switch

Per-feature env-var defaults: `BP_READER_SORT_DEFAULT=natural`, `BP_READER_STRICT_JSONRPC_VERSION=0` (off until users confirm strict mode is desired), `BP_READER_INSTRUCTIONS=1`.

### Success metric

After E ships, measure: % of `tools/list` requests with `sort` arg ≠ default (signals adoption of new option).

**Tests:** +20. **Risk:** low (additive). **Migration:** zero-break (guarded by snapshot tests).

---

## Phase D — outputSchema + Image rollout (3–5 days) — ★4

Apply existing infrastructure to all tools.

- `outputSchema` for all 128 tools (mechanical; group by return shape pattern).
- `take_viewport_screenshot` + `take_screenshot` emit Image content blocks via existing `ContentBlocks::Envelope`.
- `return_inline=true` flag (default false for back-compat).
- 1280px max-dim cap (nearest-neighbor downscale).
- `Audience::User` on image content; `Audience::Both` default for text.

### Exit criteria
- All 128 tools advertise `outputSchema` (or explicit empty/null with justification comment)
- Screenshot inline test: 1280px cap upheld with deterministic checksum on test fixture
- Backwards-compat snapshot still green (outputSchema is additive)

### Kill-switch
`return_inline=true` arg defaults to false on both screenshot tools. Setting `BP_READER_NEVER_INLINE_IMAGES=1` forces ignore of the arg.

### Success metric
After E ships, measure: % of screenshot calls with `return_inline=true`.

**Tests:** +30. **Risk:** medium (downscale math must be correct).

---

## Phase Prompts — MCP prompts primitive (2–3 days) — ★4

Slash-command surface for Claude Code / Cursor / VSCode / Claude Desktop (universal client support).

- `prompts/list`, `prompts/get`, `notifications/prompts/list_changed`.
- Capability: `prompts: {listChanged: true}` advertised on initialize.

**Ship 8 pre-built prompts** (each ~2 hr of prompt text):

| Prompt | Args | Purpose |
|---|---|---|
| `audit_bp` | `asset_path` | Comprehensive BP quality audit: unused variables, missing categories, deep nesting, magic numbers |
| `explain_function` | `asset_path`, `function_name` | Walk function graph, explain logic in plain English |
| `suggest_refactor` | `asset_path` | Propose refactoring based on graph analysis |
| `compare_blueprints` | `asset_a`, `asset_b` | Diff via `bp_structural_diff` + explanation |
| `transpile_to_cpp` | `asset_path` | **Our moat** — guides decompile → review → transpile → write_generated_source workflow |
| `review_generated_cpp` | `asset_path`, `cpp_path` | After transpile, audit generated C++ for naming/style/UPROPERTYs |
| `check_transpile_compat` | `asset_path` | Pre-flight check: identify BP patterns that won't transpile cleanly |
| `lyra_gameplay_review` | `asset_path` | Lyra-specific: check GAS integration, GFP boundaries, ModularGameplay conventions |

### Exit criteria
- Capability `prompts: {listChanged: true}` advertised on initialize
- All 8 prompts return non-empty bodies under prompts/get
- Claude Code smoke: `/transpile_to_cpp` slash command appears in palette

### Kill-switch
`BP_READER_PROMPTS=0` env var skips registration; capability not advertised.

### Success metric
After E ships, measure: % of sessions where any prompt was fetched via prompts/get.

**Tests:** +15.

---

## Phase C1 — Resources primitive (3–4 days) — ★4

URI-addressable readable assets.

- `IResourceProvider` interface: `ListResources(outDescriptors)` + `ReadResource(uri) → text|blob`.
- `resources/list` + `resources/read` JSON-RPC methods.
- URI scheme `bp://`. Initial providers:
  - Blueprint asset reader: `bp:///Game/Path/To/BP_Foo`
  - Project metadata: `bp:///_project`
  - Output log: `bp:///_output_log`
- `ResourceNotFound` error code -32002.
- **Defer:** resource templates + `completion/complete` (Epic doesn't implement; add later if demand).

### Pre-phase spike (1 day, see Pre-phase spike timeboxes section above)
URI parser exit conditions must hold before scaling providers.

### Exit criteria
- Capability `resources: {}` advertised on initialize
- 3 providers wired: BP asset, project metadata, output log
- Claude Code Resources panel populates with `bp:///` URIs

### Kill-switch
`BP_READER_RESOURCES=0` env var skips registration; capability not advertised.

### Success metric
After E ships, measure: # of resources/read calls per session.

**Tests:** +25.

---

## Phase C2 — Pagination cursors (1–2 days) — ★3

Spec-conformant opaque cursors.

- Base64-encoded digit cursors for `tools/list` + `resources/list` + every `list_*` tool.
- Add `cursor` arg alongside existing `limit`/`offset` (eventual deprecation).
- Invalid cursor → -32602 with helpful message.
- Server cursor invalidation when list changes mid-pagination → reset signal.

### Exit criteria
- Every existing `list_*` tool advertises pagination via `cursor` arg in inputSchema
- Round-trip test: list with cursor → fetch second page → matches expected slice
- Invalid cursor returns -32602, not crash

### Kill-switch
`BP_READER_CURSORS=0` falls back to limit/offset semantics; cursor arg ignored.

### Success metric
After E ships, measure: % of list calls that supply cursor (vs limit/offset legacy).

**Tests:** +15.

---

## Phase Logging — MCP logging primitive (1–2 days) — ★3

Server logs visible in client UI.

- `logging/setLevel` request handler.
- `notifications/message` emitter.
- RFC 5424 severity levels (debug/info/notice/warning/error/critical/alert/emergency).
- Wire our existing stderr `LogBlueprintReaderMcp` to also emit as notifications.
- Default level: `info`.

### Exit criteria
- Capability `logging: {}` advertised on initialize
- Client `logging/setLevel` actually changes which severities reach `notifications/message`
- One smoke test: server logs an error after a bad tool call; client receives it

### Kill-switch
`BP_READER_LOG_LEVEL=off` disables push notifications; stderr unaffected.

### Success metric
After E ships, measure: % of sessions calling logging/setLevel.

**Tests:** +10.

---

## Phase E — Analytics + safety (5–7 days) — ★3

Observability + defense in depth.

- Pluggable analytics provider (interface; default no-op).
- Events: `SessionStart`, `SessionEnd`, `ToolCall` with SHA256-hashed tool/toolset names (privacy).
- Hash mapping JSON file emitted at server startup to `Saved/BlueprintReader/ToolHashMapping.json`.
- Snapshot-under-lock pattern for thread-safe provider access.
- Path safety helpers + class/property block lists in `UBlueprintReaderSettings`.
- EULA notice on server start (logged once).
- Console commands when companion editor module exists: `bp_reader_mcp.RefreshTools`, `GenerateClientConfig`, `StartServer`, `StopServer`.

### Exit criteria
- Default no-op provider compiles + runs with no analytics
- Hash mapping JSON emitted on startup; contains entry for every advertised tool
- Path-safety helper rejects `..` traversal in 3 screenshot tools + `write_generated_source` test cases
- EULA notice logged once per session

### Kill-switch
`BP_READER_ANALYTICS=0` (default) keeps no-op provider; analytics events not generated.

### Success metric
This phase ENABLES measurement of the north-star metric (tool-calls per session). After ship, the rest of the plan's success metrics become measurable.

**Tests:** +25.

---

## Phase EA-pull Wave 1 — Core awareness (6–8 days) — ★5

The "what is the user doing right now?" core. Covers 90% of agent reactivity needs.

### Tools (~25)

**Selection:**
- `get_editor_state` — enrich existing with per-actor metadata, multi-PIE-instance, active editor type
- `get_selected_actors(include_metadata?)` — name/label/class/transform per actor
- `set_selection(names, replace?)` — batched
- `get_selected_components` — components within selected actor
- `get_selected_assets` / `set_selected_assets(paths)` — content browser (poll-for-settle, 5s timeout)
- `get_selected_folders` — content browser folder selection
- `get_content_browser_path` / `set_content_browser_path(path)` (verifies post-set)

**Open / active:**
- `list_open_assets` (top-level, with class + last-activation-time)
- `get_active_asset` (most-recently-touched editor via `GetLastActivationTime`)
- `open_asset_editor(asset_path)` / `close_asset_editor(asset_path)`

**Compile / save:**
- `get_compile_status(asset_path)` — `UBlueprint::Status` enum mapped to string
- `get_dirty_packages` — unsaved list

**PIE / modal:**
- `get_pie_state` — multi-instance with Client/Server type
- `get_modal_state` — `{is_open, title}`
- `get_focused_window` — title + class
- `get_focused_widget` — type + ref + label

**Editor mode:**
- `get_active_editor_mode` — placement/landscape/foliage/modeling/etc.

**Spatial / UI:**
- `world_pos_to_screen(pos)` / `screen_to_world(coords, dist)` — normalized [0,1]
- `take_desktop_screenshot` — multi-window composite via `FSlateApplication::GetAllVisibleWindowsOrdered`, 1280px cap
- `ui_snapshot(window?, depth?)` — Slate widget tree (read-only)
- `ui_find(text, role?)` — locate widget by visible text

**Tests:** +60.

**Backend impact:** ~25 new ops in commandlet/live backends. IBlueprintReader interface adds 25 virtuals (mock throws "not supported").

### Exit criteria
- All 25 tools registered + advertise on tools/list
- Live backend implements all 25; commandlet backend implements all 25; mock throws "not supported" for all 25
- One agent-flow smoke documented: "agent reads `get_editor_state` → makes a recommendation that uses the current selection"
- IBlueprintReader interface change reviewed (adds 25 virtuals)

### Kill-switch
`BP_READER_TOOLS_EXCLUDE=editor-state` removes the entire wave from tools/list.

### Success metric
% of sessions calling at least one EA-pull tool (target: >50% within first month after E ships).

---

## Phase C3–C5 — HTTP + Sessions + SSE (13–15 days) — ★5

Transport upgrade. Unlocks push events.

- **C3** Socket loop wrapping our existing HttpTransport frame layer. Vendor `cpp-httplib` (single-header, MIT).
- **C4** Sessions: GUID via `FGuid::NewGuid()`, `Mcp-Session-Id` response header on initialize, per-session state, DELETE endpoint.
- **C5** SSE: chunked transfer encoding (`text/event-stream` + `Connection: keep-alive` + `Cache-Control: no-cache`), per-request stream tracking, progress flushing on same socket as response. `notifications/tools/list_changed` SSE push.
- DNS-rebinding Origin guard (localhost / 127.0.0.1 / [::1] only; HTTP+auth is non-goal for external deploy).
- `Mcp-Protocol-Version` header validation per request.
- Multi-client config writer extended for HTTP URL format (`http://127.0.0.1:8000/mcp`).

### Pre-phase spike (2 days, see Pre-phase spike timeboxes section above)
HTTP+SSE smoke must pass before any of the 13–15 days of full-phase budget is consumed.

### Exit criteria
- Stdio remains default; `BP_READER_HTTP_PORT=8000` opt-in
- All 619+ existing tests pass over stdio (zero-break)
- HTTP transport echo test: POST tools/list returns identical JSON to stdio equivalent
- Session lifecycle test: GUID returned on initialize, accepted on subsequent calls, rejected after DELETE
- SSE smoke: one notification flushes on the same socket as a response
- DNS-rebinding Origin guard rejects external Origin headers in test

### Kill-switch
HTTP only enabled when `BP_READER_HTTP_PORT` env var is set; default is stdio-only.

### Success metric
After ship: % of sessions on HTTP transport (vs stdio) — proxy for whether HTTP is needed.

**Tests:** +50. **Risk:** high — SSE testing requires real socket pairs (use `127.0.0.1:0` for auto-port).

---

## Phase EA-push Wave 1 — Core event notifications (8–10 days) — ★5 (differentiator)

Push UE delegate events as SSE notifications. **Epic doesn't do this — biggest agentic differentiator.**

### Subscription model
- `editor/subscribe { event_types: [...] }` → `{subscription_id}`
- `editor/unsubscribe(subscription_id)`
- Per-session subscriptions
- Default subscription: Tier A events

### Tier A events (fire instantly)
- `notifications/editor/level_actor_selection_changed` — `{added, removed, current}`
- `notifications/editor/asset_opened` / `_closed`
- `notifications/editor/active_asset_changed`
- `notifications/editor/level_changed` — `{old, new}`
- `notifications/editor/pie_started` / `_paused` / `_resumed` / `_stopped`
- `notifications/editor/blueprint_compiled` — `{asset_path, success, error_count, warning_count}`
- `notifications/editor/package_saved` — `{asset_paths}`
- `notifications/editor/editor_mode_changed` — `{mode_id}`
- `notifications/editor/asset_added` / `_renamed` / `_removed`
- `notifications/editor/live_coding_complete`
- `notifications/editor/hot_reload_complete`

### Backend mechanism
- `BlueprintReaderEditor` module subscribes to UE delegates (`USelection::SelectionChangedEvent`, `UAssetEditorSubsystem::OnAssetOpenedInEditor`, `FEditorDelegates::*`, etc.).
- Live TCP server protocol extended: new message type `{type: "event", name, params}` from editor → MCP server.
- MCP server emits as SSE notification on subscribed sessions.

### Pre-phase spike (2 days, see Pre-phase spike timeboxes section above)
One delegate end-to-end (selection-changed) must round-trip under 100ms before scaling.

### Exit criteria
- All 11 Tier-A events fire correctly on UE actions (manual smoke per event)
- Subscription model works: `editor/subscribe` returns ID; only subscribed sessions get notifications
- Push event under PIE: no notifications dropped, no out-of-order delivery
- Per-session subscription state survives a tool call (no resubscribe needed)

### Kill-switch
`BP_READER_PUSH_EVENTS=0` (default) keeps editor module from registering UE delegates. `editor/subscribe` returns -32601 (Method not found).

### Success metric
% of HTTP sessions that subscribe to ≥1 event type. North-star adjacent: avg notifications delivered per session.

**Tests:** +40.

---

## Phase H Tier 1 — Lyra value-add toolsets (10–12 days) — ★4 for Lyra

Three editor-side toolsets that match Epic's pattern but ship with Lyra-relevant content.

- **PluginToolset** (~4d) — list/create/edit UE plugins, modify .uplugin descriptors, manage dependencies, source-control-aware (checks out via SCC before edit)
- **GameFeaturesToolset** (~4d) — GFP request-activate/deactivate/state-poll with simplified 6-state enum (vs Epic's 34 internal states)
- **AbilitySystemInspectorToolset** (~4d) — GAS introspection: runtime attribute values (base + current), active effects (with stack/duration/granted tags), granted abilities (with level + active flag), active tags per `UAbilitySystemComponent`

**Tests:** +90 (≈30 per toolset).

**Build flag:** `BUILD_WITH_GAS_TOOLSET=1` (conditional compile so non-Lyra users don't pull GAS deps).

### Pre-phase spike (1 day, see Pre-phase spike timeboxes section above)
GAS conditional-compile must not leak symbols into non-Lyra builds.

### Exit criteria
- Each of 3 toolsets registers tools that pass mock + commandlet + live tests
- Non-Lyra build (no GAS module) compiles BlueprintReaderMcpCore cleanly with `BUILD_WITH_GAS_TOOLSET` undefined
- Lyra-specific smoke: activate a GFP → AbilitySystemInspectorToolset reports newly-granted abilities

### Kill-switch
`BP_READER_TOOLS_EXCLUDE=plugin,game-features,ability-system` removes whole toolsets. Build-time: drop `BUILD_WITH_GAS_TOOLSET=1`.

### Success metric
For Lyra users: % of sessions using Lyra-specific tools. North-star: avg Lyra-tool calls per Lyra session.

---

**— CRITICAL PATH TOTAL: ~14 weeks for wire parity + core differentiation + Lyra Tier 1 —**

After this, ship subsequent waves based on actual user demand signals.

---

## Phase EA-pull Wave 2 — Per-asset-editor selection (6–8 days)

Drill into selection within specific asset editors.

### Tools (~15)
- `get_blueprint_editor_state(asset_path)` — `{selected_nodes, selected_pins, current_graph_tab, my_blueprint_selection, compile_state}`
- `get_material_editor_state(asset_path)` — `{selected_expressions, base_property_overrides}`
- `get_material_instance_params(asset_path)` — scalar/vector/texture parameter values
- `get_sequencer_state(asset_path)` — `{playhead_seconds, selected_sections, selected_tracks, selected_keys, playback_range, working_range, playback_state}`
- `get_anim_editor_state(asset_path)` — `{scrubber_seconds, selected_curves, preview_paused}`
- `get_umg_editor_state(asset_path)` — `{selected_widgets, designer_zoom}`
- `get_niagara_module_selection(asset_path)`
- `get_static_mesh_lod(asset_path)` — current preview LOD
- `get_mesh_preview_state(asset_path)` — show flags + LOD + UV overlay
- `get_curve_editor_selection(asset_path)` — generic curve editor (used by Anim/Sequencer/Particle)
- `get_cinematic_camera(level_sequence_path?)` — currently-active camera in PIE/sequencer

### Exit criteria
All ~15 tools wired across 3 backends + tests; Claude Code smoke: agent reads selected sequencer key → suggests next keyframe.

### Kill-switch
`BP_READER_TOOLS_EXCLUDE=editor-state-extended` removes wave.

### Success metric
% of sessions calling any per-asset selection tool.

**Tests:** +50.

---

## Phase EA-pull Wave 3 — Viewport + visibility (5–7 days)

### Tools (~20)
- `get_show_flags` / `set_show_flag(name, enabled)` — viewport visualization toggles (~200 flags as bitfield)
- `get_view_mode` / `set_view_mode(mode)` — Lit/Unlit/Wireframe/Detail Lighting/etc.
- `get_buffer_visualization_mode` — base color, roughness, normals, etc.
- `get_camera_bookmarks` / `goto_camera_bookmark(slot)` — Ctrl-1..9 saved poses
- `get_camera_transform(viewport_idx?)` (per-viewport)
- `set_camera_transform(transform, viewport_idx?)`
- `get_visible_actors(class_filter?, max_distance?)` — frustum + per-actor metadata (name, label, class, world_location, distance_cm, screen_position)
- `get_hidden_actors` / `set_actor_visibility(name, visible)`
- `get_hidden_layers` / `set_layer_visibility(layer, visible)`
- `get_hover_target` — hit proxy type + actor/surface/component data (`HActor`/`HBSPSurface`/`HBoneProxy`/`HWidgetAxis`/etc.)
- `get_active_viewport` — which of 1/2/4 layout has focus
- `get_gizmo_state` — `{space: World|Local, mode: Translate|Rotate|Scale, pivot}`
- `set_gizmo_mode(mode)`
- `get_viewport_camera_settings(viewport_idx?)` — `{fov, speed, far_clip}`
- `get_viewport_realtime` / `set_viewport_realtime(bool)`
- `get_isolate_mode` — show-only-selected flag (UE 5.6+)
- `get_snapping_settings`

### Exit criteria
All ~20 tools wired; viewport mutation tools (set_show_flag, set_camera_transform, goto_camera_bookmark) are reversible.

### Kill-switch
`BP_READER_TOOLS_EXCLUDE=viewport-state` removes wave.

### Success metric
% of sessions calling viewport tools (proxy for "agent is viewport-aware").

**Tests:** +45.

---

## Phase EA-pull Wave 4 — World + SCC + system state (6–8 days)

### Tools (~25)

**World / partition (Lyra-relevant):**
- `get_current_level` / `list_loaded_levels` (sublevels)
- `list_loaded_partition_cells` (World Partition)
- `get_streaming_sources`
- `get_data_layer_states` — per-layer Unloaded/Loaded/Activated

**Source control:**
- `get_source_control_provider` — `{name: "Git"|"Perforce"|null, branch}`
- `get_source_control_status(asset_path?)` — per-file or summary
- `get_file_lock_status(asset_path)` — checked out by whom
- `get_pending_changelist` / `list_changelists`

**Outliner:**
- `get_outliner_state` — `{search_text, filter, expansion, columns, type_filter}`
- `get_pinned_actors`

**Recent activity (we maintain ring buffer):**
- `get_recently_opened_assets`
- `get_recently_saved_packages`

**System state:**
- `get_autosave_status` — last save timestamp
- `get_recovery_state` — has crash-recovery files
- `get_async_compile_state` — `{remaining_textures, shaders, meshes, etc.}`
- `get_shader_compile_state` — `{remaining, completed, failed}`
- `get_lighting_build_progress`
- `get_live_coding_state` — `{is_compiling, pending_patches}`
- `get_asset_registry_state` — `{scan_complete, scanned_assets}`
- `get_active_cook_target` / `set_active_cook_target`
- `get_cook_progress`
- `get_ddc_state`

### Exit criteria
All ~25 tools wired; SCC tools handle Git + Perforce + None gracefully (no crashes when provider unavailable).

### Kill-switch
`BP_READER_TOOLS_EXCLUDE=world-state,source-control,system-state` removes whole wave.

### Success metric
% of Lyra sessions calling partition/data-layer tools; % of all sessions calling SCC tools.

**Tests:** +50.

---

## Phase EA-push Wave 2 — Extended event coverage (5–7 days)

### Tier B events (debounce 250ms)
- `notifications/editor/content_browser_selection_changed`
- `notifications/editor/content_browser_path_changed`
- `notifications/editor/focused_window_changed`
- `notifications/editor/focused_widget_changed`
- `notifications/editor/show_flags_changed`
- `notifications/editor/view_mode_changed`
- `notifications/editor/asset_metadata_changed`
- `notifications/editor/editor_command_executed`
- `notifications/editor/tab_focused_changed`

### Tier C events (debounce 1s, snapshot-style)
- `notifications/editor/camera_moved` (4Hz max via `FEditorDelegates::OnEditorCameraMoved`)
- `notifications/editor/dirty_packages_changed`
- `notifications/editor/source_control_status_changed`
- `notifications/editor/sequencer_playhead_changed`
- `notifications/editor/shaders_compiling`
- `notifications/editor/lighting_build_progress`
- `notifications/editor/cook_progress`

### Lyra-specific
- `notifications/editor/game_feature_state_changed` (via `UGameFeaturesSubsystem::OnGameFeatureStateChange`)
- `notifications/editor/partition_cell_shown` / `_hidden` (via `UWorldPartition::OnCellShown/Hidden`)
- `notifications/editor/data_layer_state_changed`

### Exit criteria
All Tier-B / Tier-C events fire correctly with per-tier debouncing; no flooding under stress (PIE + cook + shader compile concurrent test).

### Kill-switch
Per-event-type unsubscribe via `editor/unsubscribe`. Global: `BP_READER_PUSH_EVENTS=0`.

### Success metric
Avg notifications/session — should grow without hurting tool-call latency p95.

**Tests:** +30.

---

## Phase H Tier 2 — More Lyra value-add (6–8 days)

- **ConfigSettingsToolset** — 3-tier Project Settings nav (Container → Category → Section) with schema generation + get/set/save/reset
- **AutomationTestToolset** — discover + run UE automation tests with structured results

### Exit criteria
ConfigSettings get/set/save/reset round-trips for at least 5 commonly-edited settings; AutomationTest discovers + runs at least the BlueprintReader runtime tests with parsed results.

### Kill-switch
`BP_READER_TOOLS_EXCLUDE=config-settings,automation` removes whole tier.

### Success metric
% of sessions calling config get/set; % of sessions running automation tests.

**Tests:** +60.

**— Cumulative through H Tier 2: ~22 weeks —**

---

## Phase EA-pull Wave 5 — Advanced + niche (5–7 days)

Lower-priority surfaces; ship if demand surfaces.

### Tools (~20)
- `get_blueprint_breakpoints(asset_path)` — array of `{node_guid, enabled, function_name}` via `UBlueprint::Breakpoints[]`
- `get_debug_instance(asset_path)` — PIE-attached object via `UBlueprint::GetObjectBeingDebugged()`
- `get_watched_pins(asset_path)` — `UBlueprint::WatchedPins[]`
- `get_details_panel_state` — filter, selected objects, pinned props
- `get_modeling_state` — active tool, sub-element selection mode (vertex/edge/face/group)
- `get_landscape_paint_state` / `get_foliage_paint_state` / `get_mesh_paint_state` / `get_texture_paint_state`
- `get_take_recorder_state` — recording active, current take number
- `get_render_queue` — Movie Render Queue pending jobs
- `get_trace_state` — Insights connected, active channels
- `get_active_stats` — Stat overlays (StatUnit/StatGPU/etc.)
- `get_console_history(executor)`
- `get_status_bar_messages`
- `get_active_notifications` — toasts via `FSlateNotificationManager`
- `get_active_culture` / `get_localization_completeness(target)`
- `get_editor_theme` (dark/light/highcontrast)
- `get_monitor_info` (multi-monitor placement)
- `get_workspace_layout`

### Exit criteria
Demand-driven — ship subsets as users request. Each tool independently testable.

### Kill-switch
`BP_READER_TOOLS_EXCLUDE=advanced-state` removes wave.

### Success metric
Per-tool: # of unique sessions using it within 90 days of ship. Tools <5 sessions get re-evaluated for removal.

**Tests:** +40.

---

## Deferred / demand-driven (NOT in critical path)

| Phase | Defer rationale |
|---|---|
| **G** Visual annotation overlay (port `BitmapAnnotation` rasterizer) | ~2 weeks UE-side rendering for vision-only benefit; metadata-only stub (Phase A) covers most agents |
| **J-classic** Worker-thread cancellation | Most users tolerate "wait or restart"; requires per-backend `IsRunning/Cancel` API additions |
| **J-tasks** MCP tasks primitive (2025-11-25) | Experimental spec; defer until stable. **Strong differentiator when ready** (Epic doesn't implement despite advertising 2025-11-25). |
| **I** AgentSkill UAsset | Wait for Epic's plugin to ship publicly; interop preferred over reimplement |
| **K** MCP-as-client (`FMCPClientToolset` equivalent) | Claude Code already supports multiple servers; only useful for bundled-agent scenarios |
| **L** File sandbox | SCC (Git/Perforce) already provides equivalent for pro UE workflows |
| **M** Reflection-driven tools (`UFUNCTION(meta=(AICallable))`) | Major lock-in to Epic's pattern; loses our out-of-process advantage. Re-evaluate when tool count > 200. |
| **EU-write** Slate UI simulation (Click/Type/Drag/Hover) | Niche; agents typically call tools directly, not drive UI |
| **Multi-user Concert** editing | Requires Concert plugin + protocol; niche collaboration use case |
| **Live Link / Virtual Camera** | Cinematic workflows; defer per demand |

---

## Cumulative milestone table

Re-baselined to measured 655 starting tests post-Phase-B (see baseline doc).

| Milestone | Phases | Weeks | Tests (cumulative) |
|---|---|---|---|
| **MVP — wire parity** | D + Prompts + C1 + C2 + Logging + E | 3.5 | +155 → ~810 |
| **+ Core agent awareness** | + EA-pull Wave 1 | 5 | +60 → ~870 |
| **+ Push events (transport + core)** | + C3–C5 + EA-push Wave 1 | 9.5 | +90 → ~960 |
| **+ Lyra Tier 1 toolsets** | + H Tier 1 | 12.5 | +90 → ~1050 |
| **+ Per-asset selection + viewport** | + EA-pull Wave 2 + Wave 3 | 15.5 | +95 → ~1145 |
| **+ World/SCC + extended events** | + EA-pull Wave 4 + EA-push Wave 2 + H Tier 2 | 20.5 | +140 → ~1285 |
| **+ Advanced surface** | + EA-pull Wave 5 | 22.5 | +40 → ~1325 |

---

## Critical path recommendation

**Ship-now critical path:**

```
B → D → Prompts → C1 → C2 → Logging → E → EA-pull Wave 1 → C3-C5 → EA-push Wave 1 → H Tier 1
```

Delivers in **~14 weeks**:
- Full MCP wire-protocol parity with Epic 5.8
- Slash-command differentiation Epic can't match (BP↔C++ prompts)
- Core "what's the user doing?" awareness for agents
- Push events Epic doesn't have (biggest agentic differentiation)
- Lyra-specific Tier 1 value-add (PluginToolset + GameFeatures + GAS)

After that, ship EA-pull Wave 2/3/4 + EA-push Wave 2 in waves based on which agent workflows need each surface. Wave 5 + deferred phases are demand-driven.

---

## Per-phase rollout (zero breaking changes)

| Phase | Default behavior | Opt-in mechanism |
|---|---|---|
| B Hardening | New validation/safety active; `sort=natural` default | `sort=name` arg for alphabetical |
| D outputSchema | Always emitted | (additive — no opt-in needed) |
| D Image inline | Disabled | `return_inline=true` arg |
| Prompts | Capability advertised; prompts listed | (additive — clients ignore unknown capability) |
| C1 Resources | Capability advertised | (additive — clients use `resources/list` if they want) |
| C2 Cursors | Both old and new args accepted | Eventually deprecate `limit`/`offset` |
| Logging | Capability advertised; default level=`info` | Client calls `logging/setLevel` to adjust |
| E Analytics | Disabled by default | `BP_READER_ANALYTICS=1` env var |
| EA-pull all waves | New tools added | (additive) |
| C3–C5 HTTP | Stdio remains default | `BP_READER_HTTP_PORT=8000` env var |
| EA-push | Sessions advertise `editor` capability | Client opts in via `editor/subscribe` |
| H Tier 1/2 | New tools added | Conditional compile for GAS toolset |

**All phases preserve zero-breaking-change guarantee for existing users.**

---

## Decision points

| Decision | Recommendation | Trigger to revisit |
|---|---|---|
| Bump to 2025-11-25 protocol version | **Stay on 2025-06-18** | If we implement tasks/* primitive |
| Implement Phase G visual annotation | **Defer** | If vision-capable workflows become primary user base |
| Implement Phase J cancellation | **Defer** | If users complain about runaway cooks/automation tests |
| Adopt AgentSkill UAsset (Phase I) | **Wait for Epic plugin to ship** | When Epic's plugin is publicly available + stable |
| Migrate to UFUNCTION reflection (Phase M) | **Stay hand-coded** | If we add 200+ tools and hand-coding pain becomes acute |
| Support external HTTP deployment | **No — localhost only** | If a user explicitly requests + funds the OAuth work |
| Implement sampling/elicitation | **Skip** | If client support becomes universal |
| Implement multi-user Concert | **Skip** | Only if collaborative editing becomes a target use case |

---

## Risk register (top items per phase)

| Phase | Risk | Mitigation |
|---|---|---|
| B Hardening | Sorted output breaks order-keyed agent prompts | `sort` opt-in, default `natural` preserves current order |
| D outputSchema | Image content blows LLM context | Default `return_inline=false`; opt-in inline capped at 1280px max-dim |
| C3–C5 HTTP+SSE | SSE testing without real sockets | Unit-test frame layer; integration tests via `127.0.0.1:0` |
| EA-pull Wave 1+ | Backend ops cascade through 4 backends (mock/commandlet/live/auto) | Template the live-backend ops; each new tool is ~half-day |
| EA-push Wave 1 | Push event flooding | Per-tier debouncing + opt-in subscriptions |
| EA-pull Wave 2 | Per-asset-editor selection has 5+ editor types | Template the live-backend ops; each new editor type is ~half-day |
| H Tier 1 | GAS dep bloats non-Lyra users | Conditional compile `BUILD_WITH_GAS_TOOLSET=1` |
| EA-pull Wave 4 | Source control async queries can hang | Use `Execute(Asynchronous)` + future timeout |
| EA-pull Wave 5 | Niche tools may never get used | Ship based on actual demand signals from analytics |
| All phases | BPIR roundtrip regression | Integration test: full BPIR roundtrip over stdio AND HTTP after C5 ships |

---

## Strategic outcomes per milestone

### After MVP (B+D+Prompts+C1+C2+Logging+E) — week ~4

- Wire-protocol parity with Epic 5.8 at the primitive level
- Slash-command discoverability via `/audit_bp`, `/transpile_to_cpp`, etc.
- Resources surface — Blueprint assets browsable via `bp://` URIs
- Observability via analytics events
- Path safety + class/property block lists

### After Core Awareness (+ EA-pull Wave 1) — week ~6

- Agent can ask "what is the user doing right now?" and get a complete answer
- Single `get_editor_state` call returns selection, open assets, PIE state, modal state, focused window
- Foundational queries for reactive workflows

### After Push Events (+ C3-C5 + EA-push Wave 1) — week ~11

**This is the strategic-pivot moment.** Agents become reactive:

| User action | Agent reaction |
|---|---|
| Selects character BP | Agent loads relevant Lyra context + offers "explain this character" |
| Compiles BP with errors | Agent reads compile output + suggests fix |
| Saves 5 packages | Agent runs validation + lists what changed |
| Starts PIE | Agent monitors output log + crash counter |
| Opens material editor | Agent suggests "audit this material's expression count" |
| Switches level | Agent reloads spatial context |
| Hot reload completes | Agent re-runs failing tests |

**Epic explicitly doesn't do this.** First UE MCP server with reactive editor awareness.

### After Lyra Tier 1 (+ H Tier 1) — week ~14

- PluginToolset for plugin scaffolding workflows
- GameFeaturesToolset for Lyra GFP lifecycle
- AbilitySystemInspectorToolset for GAS introspection

Lyra-specific workflows unlocked.

### After full critical path through Wave 2/3/4 + Tier 2 — week ~22

- Comprehensive per-asset-editor selection coverage
- Full viewport visibility / show flags / view mode control
- World partition + source control + system-state awareness
- ConfigSettings + Automation toolsets

Feature-complete for Lyra-style projects.

---

## Test surface growth

Each new EA tool typically needs:
- Mock backend impl (throws "not supported")
- Commandlet backend impl
- Live backend impl
- 3–5 test cases

For "must-have" tools (~50): full backend coverage = ~150 tests
For "should-have" tools (~80): live backend only, mock throws = ~80 tests
For "nice-to-have" tools (~70): deferred or live-only minimal coverage

Template-based testing keeps marginal cost low: ~3 tests per simple read tool. Total growth target: from **655 today** (post-Phase-B) to ~1325 after full plan = +670 tests (+102%).

---

## Architecture invariants (preserve throughout)

1. **Out-of-process safety** — MCP server crashes don't take editor down.
2. **Mock backend** — tests run with zero UE installed.
3. **BP↔C++ transpile + BPIR** — our differentiation moat; never regress.
4. **Cooked-build runtime introspection** — `BlueprintReaderRuntime` module + `bp_reader.list/read` console cmds work in shipping builds.
5. **Stdio transport default** — HTTP is opt-in via env var.
6. **Zero breaking changes** — every phase additive; older clients keep working.

---

## Reference documents

- `reference_epic_mcp_plugin.md` (memory) — plugin location + access path
- `reference_epic_mcp_architecture.md` (memory) — module loading, request lifecycle, tool registration, spec conformance status, editor awareness surface
- `project_epic_mcp_strategic_landscape.md` (memory) — strategic posture
- `feedback_no_downstream_names.md` (memory) — no proprietary downstream project names in OSS plugin
- [`2026-05-20-epic-mcp-plan-baseline.md`](./2026-05-20-epic-mcp-plan-baseline.md) — measured baseline; re-measure quarterly

---

## North-star metric + per-phase metrics

**North star:** *Mean tool-calls per agent session* — proxy for "agents actually use this." Baseline measurement gated on Phase E ship (analytics infrastructure).

**Secondary metric:** *Mean editor-event notifications delivered per HTTP session* — proxy for reactive workflows engaging.

**Per-phase metric:** Every phase declares its own success metric in its block (see "Success metric" lines under each phase). Wire them all through the analytics provider added in Phase E.

**Decision points based on metrics:**
- If north-star <3 mean tool-calls/session 90 days after MVP ships → re-evaluate prompt + resources value
- If push-event mean <5 per session 90 days after EA-push Wave 1 ships → re-evaluate Wave 2 priority
- If <10% of Lyra sessions call Lyra-specific tools 90 days after H Tier 1 → re-evaluate Tier 2 priority

---

## MCP spec watch + re-baseline triggers

The MCP spec is a moving target. Don't lock to a single version forever.

**Re-baseline triggers (any one is sufficient):**
1. **New stable spec version published** — e.g., 2025-11-25 currently experimental; when it goes stable, re-evaluate `protocolVersion` default + tasks primitive
2. **Epic ships UE 5.9 with new MCP surface** — re-run the architecture deep-dive on the 5.9 branch
3. **Major client (Claude Code, Cursor, Codex, Copilot) adds spec support we don't have** — e.g., if Claude Code adds first-class `roots` capability, evaluate adding it
4. **Spec deprecates something we depend on** — e.g., if 2024-11-05 negotiation gets dropped, drop fixture from compat matrix
5. **Quarterly cadence** — minimum 1 re-baseline pass every 90 days, regardless of triggers

**Re-baseline procedure:**
1. Re-read MCP spec changelog since last check
2. Re-run `gh api repos/EpicGames/UnrealEngine/contents/...` on the 5.x branch to see Epic's current shape
3. Update `reference_epic_mcp_architecture.md` memory file
4. Update this plan's "Strategic posture" + "Deferred / demand-driven" sections
5. Bump plan revision (v7 → v8 → ...) and document what changed in revision history

**Last baselined:** 2026-05-20 against 5.8 branch + 2025-11-25 spec (experimental).
**Next scheduled re-baseline:** 2026-08-20.

---

## EA-push edge cases under load

Push events can flood. The Tier A/B/C/D classification handles the common case; these edge cases need explicit testing:

| Scenario | Risk | Mitigation |
|---|---|---|
| **PIE actively running** | Selection events fire in editor world AND PIE world; tool-call latency competes with rendering | Per-source debounce; PIE selection events tagged with `world: "PIE"` to let agents filter |
| **Cook in progress** | `package_saved` events fire continuously for cook output; legit save events get lost | Suppress saves during cook (`UCookCommandlet` active flag); resume after cook |
| **Shader compile (10k shaders)** | `shaders_compiling` Tier-C event would fire every 1s; clients see noise | Stop pushing after first 5 emissions; resume only when batch completes; final summary event |
| **Lighting build (5 min)** | `lighting_build_progress` Tier-C; same flood risk | Same pattern: head + tail emissions, no middle |
| **Live coding compile** | `live_coding_complete` Tier A; but a fast iteration could fire every 30s | Rate-limit to 1 per 10s; queue overflow → drop oldest |
| **Asset registry scan startup** | 10k+ `asset_added` events on first editor launch | Suppress during scan; one summary event when scan completes |
| **HotReload completion** | Multiple delegates fire in close sequence (compile + reload + asset registry refresh) | Collapse into single `hot_reload_complete` event with summary block |
| **Client disconnect mid-event** | Socket write fails | Drop event silently; log at debug; do not retry (no per-session queue) |
| **High-frequency camera-moved** | Tier-C 1Hz cap; but multi-viewport could 4× the rate | Per-viewport channel; aggregate before push |
| **Multi-client subscribed to same event** | One UE delegate → N sessions to notify | Snapshot params once, fan-out to all subscribed sessions in one pass |

**Backpressure rule:** if event-queue depth > 100 for any single session, drop oldest with `_meta: {dropped: N}` marker on next pushed event. Surfaces overload to clients without crashing.

**Test:** A stress harness in `test_ea_push_stress.cpp` runs PIE + cook + shader compile concurrently and validates: (a) no notification dropped under 10/sec/event-type; (b) no out-of-order delivery within an event type; (c) tool-call latency p95 < 250ms throughout.

---

## Security review

HTTP transport (Phase C3–C5) is localhost-only by design, but localhost still has a meaningful threat surface.

| Surface | Risk | Mitigation |
|---|---|---|
| **DNS rebinding** | Malicious page rebinds `localhost` → external host, exploits agent-driven tool calls | Reject any Origin other than `http://localhost*`, `http://127.0.0.1*`, `http://[::1]*`. Implemented in Phase C3 HTTP transport. |
| **CSRF via image tag** | `<img src="http://127.0.0.1:8000/mcp">` from a malicious page | POST-only protocol; GET returns 405. No browser-form-submittable shape. |
| **Token theft via SSE** | Long-lived SSE socket; if a malicious extension reads it, attacker gets session | Require session GUID (`Mcp-Session-Id`) on every request; rotate on demand via `editor/rotate_session`. Phase C4. |
| **SSRF via resource URI** | Client supplies `bp://attacker.com/...` to `resources/read` | URI parser only accepts `bp:///Game/...` and known meta paths. Reject any URI with authority component. Phase C1. |
| **Path traversal via screenshot tool** | `take_screenshot(output_path: "../../etc/passwd")` | Path-safety helper added in Phase B; reject any `..` segment, any absolute path outside project Saved/. |
| **Error-message injection** | Tool errors echoed verbatim include user-controlled data → log injection | All log emissions through `LogBlueprintReaderMcp` macro (sanitizes control chars). Phase B hardening. |
| **Port collision** | `BP_READER_HTTP_PORT=8000` collides with another local service | On bind failure: log clear message, exit cleanly (no port-stealing). Document in Phase C3. |
| **Unbounded request size** | DoS via giant JSON payload | Frame-layer cap: reject Content-Length > 16 MB with 413. Already in HttpTransport. |
| **Tool-call privilege escalation** | `console_command` could execute arbitrary code via existing exec console | Already gated: tool is opt-in via `BP_READER_ALLOW_EXEC=1`. Audit in Phase B. |
| **Transpile output injection** | C++ written via `write_generated_source` could overwrite source files | Already gated: writes only within configured destination dir, never to engine source. |

**Not in scope (explicit non-goals):**
- TLS / OAuth / external deployment — these require a separate Phase K-ish effort and are not on the roadmap.
- Multi-user authorization — single-developer assumption holds throughout the plan.
- Cryptographic provenance for events — not needed for localhost.

**Action:** Phase B hardening adds the path-safety helper + log sanitization. Phase C3 spike validates Origin guard + URI parser. Both are exit criteria for their phases.

---

## External dependency audit

| Dependency | License | Vendored at | Purpose | Phase requiring it |
|---|---|---|---|---|
| nlohmann_json | MIT | `Tests/ThirdParty/nlohmann_json` | JSON parse/serialize | Already in use |
| fmt | MIT (header-only via `FMT_HEADER_ONLY`) | `Tests/ThirdParty/fmt` | String formatting | Already in use |
| doctest | MIT | `Tests/ThirdParty/doctest` | Test framework | Already in use |
| `Ws2_32.lib` | Windows SDK | system | Sockets (live TCP backend, HTTP transport) | Already in use |
| **cpp-httplib** (planned) | MIT (single-header) | `Tests/ThirdParty/cpp-httplib` (TODO) | HTTP/1.1 socket loop for transport | C3 (spike validates) |

**Audit checklist before adding any new dependency:**
1. **License compatibility** — MIT / Apache 2.0 / BSL preferred. Reject GPL/AGPL.
2. **Header-only or vendor-able** — must not require external build system invocation. UBT integration limits us to either header-only or a `.cpp` we can add to a Build.cs `Files.Add()`.
3. **Maintenance status** — last commit < 12 months; >100 GitHub stars; active issue triage.
4. **Win64 + Linux support** — both must work; macOS optional.
5. **Conflict check** — symbols/macros don't clash with UE prelude (we mitigate via `bAddDefaultIncludePaths = false` but still verify).

**UE engine version pinning:**
- Current: UE 5.7.4 source build (sibling `D:\Projects\Unreal Engine 5\`)
- Plan-relative: UE 5.8 architecture used as the Epic-side reference (per `gh api EpicGames/UnrealEngine/...` access)
- **Risk:** if we bump to UE 5.8 for our own runtime, Lyra rebuild + 3 engine `.Build.cs` patches re-apply + delegate signatures may shift. Re-evaluate before any phase that depends on 5.8-specific surface (none currently — all phases are UE 5.7-compatible).

**MCP spec version pinning:**
- Current default: 2025-06-18
- Supported (compat matrix): 2024-11-05, 2025-03-26, 2025-06-18
- Planned: 2025-11-25 once stable (currently experimental — tasks primitive in flux)

---

## Decision log

Captures what was considered AND rejected, with reasoning. Prevents re-litigation.

| Date | Decision | Considered alternatives | Reasoning for choice |
|---|---|---|---|
| 2026-05-20 | Pick MCP 2025-06-18 as server default | 2024-11-05 (older), 2025-11-25 (experimental) | 2025-06-18 is most-recent stable; tasks primitive in 2025-11-25 not yet implementable per spec ambiguity |
| 2026-05-20 | Stay on hand-coded tool registration | UFUNCTION(meta=(AICallable)) reflection (Phase M) | Reflection locks us to Epic's in-process model; we lose out-of-process safety + mock backend. Re-evaluate at >200 tools. |
| 2026-05-20 | Defer Phase G visual annotation | Ship now with 5x7 bitmap font | ~2 weeks of UE-side rendering work for vision-only benefit; metadata stub covers most agent cases |
| 2026-05-20 | Defer Phase I AgentSkill UAsset | Reimplement now | Wait for Epic plugin to ship publicly; interop preferred over reimplement |
| 2026-05-20 | Defer Phase K MCP-as-client | Add now | Claude Code already supports multiple servers; only useful for bundled-agent scenarios which aren't a target use case |
| 2026-05-20 | Defer Phase L file sandbox | Add now | SCC (Git/Perforce) already provides equivalent rollback for pro UE workflows |
| 2026-05-20 | Defer multi-user Concert + Live Link | Include in EA-pull Wave 5 | Niche collaboration / cinematic workflows; ship only if demand surfaces |
| 2026-05-20 | Sequence EA-pull Wave 1 BEFORE C3-C5 | C3-C5 first | Wave 1 is most-valued surface per cost-of-delay; ships value sooner even on stdio. C3-C5 + EA-push are the bigger investment for the reactivity payoff. |
| 2026-05-20 | Split EA-pull into 5 waves | One monolithic phase | 5-day per-wave units fit a ship-cadence rhythm; gives users incremental value and us incremental telemetry to re-prioritize |
| 2026-05-20 | Use cpp-httplib over raw socket | Roll our own | cpp-httplib is single-header MIT; saves ~2 weeks of HTTP/1.1 + SSE implementation. If spike fails, fall back to raw socket loop (live TCP backend pattern). |
| 2026-05-20 | Localhost-only HTTP transport | External-deployment story | OAuth + TLS adds ~3 weeks; not requested. Phase C3 ships localhost-only; revisit if user explicitly funds external work. |
| 2026-05-20 | Default `BP_READER_PUSH_EVENTS=0` until tested | Default on for HTTP sessions | Push event flooding is real risk; opt-in until per-tier debounce validated in Wave 2 stress test |
| 2026-05-20 | Backwards-compat snapshot test in Phase B | Each phase adds its own | Single source of truth; snapshot CI fail catches accidental wire-format changes across all phases |

**Adding to this log:** Any time the plan rejects an obvious-looking alternative, document it here with date + reasoning. Prevents "why didn't we do X?" re-discussion in 3 months.

---

## Plan revision history

- **v1**: Initial 12-phase outline based on first Epic-vs-us comparison
- **v2**: Validated against MCP spec primitives (resources/sampling/elicitation/roots)
- **v3**: Added Prompts + Logging primitives; spec compliance push
- **v4**: Tightened against Epic architecture deep-research
- **v5**: Added Phase EA (editor awareness) split into pull/push
- **v6**: Expanded EA-pull/push with comprehensive user-state coverage (200 iterations)
- **v7**: Split EA-pull into 5 waves organized by value priority; ordered critical path; cumulative milestones
- **v8**: Hardening pass — measured baseline (619 tests / 127 tools / Phase A uncommitted), explicit dependency DAG, velocity calibration, pre-phase spike timeboxes, exit-criteria + kill-switch + success-metric per phase, cost-of-delay column, north-star metric, MCP spec watch + re-baseline triggers, EA-push edge cases under load, security review, external dependency audit, decision log, backwards-compat test matrix as Phase B deliverable
- **v9**: Phase A landed in `66954afd` + `3e107c65` on `origin/main` 2026-05-20. Re-baselined to 620 tests / 132 tools (commandlet+live) / 47 tools (mock). Dropped A-merge precondition row (gate satisfied); downstream phases unblocked. All cumulative columns shifted -1d.
- **v10** (this doc): Phase B shipped in 3 commits on 2026-05-21 — `12373937` (8 hardening items: HttpTransport GET 405, `instructions` field, call_tool recursion guard, HasValidTools preflight, ToolRegistry::Add warn-not-throw, JSON-RPC strict version validation, dotted alias fallback, in-flight call registry + notifications/cancelled wiring), `e6f54125` (backwards-compat test matrix: 3 protocol pins + tools/list inventory hash anchor `0x0A155550DA73E1F3` + structural asserts), `1e542220` (sort opt-in for list_* tools with `SortProperty()` helper + env BP_READER_SORT_DEFAULT kill-switch). Re-baselined to 655 tests / 132 tools. Cumulative columns shifted -3d.

**Plan v10 is the definitive consolidated roadmap.** All prior versions superseded.
