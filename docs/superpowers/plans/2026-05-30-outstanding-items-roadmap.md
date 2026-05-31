# Outstanding-Items Roadmap (consolidated)

**Status:** Proposed (2026-05-30). One place for *everything* still open.
**Consolidates:**
- This session's streamlining remainder (daemon, `inspect_asset`, trims) + latent/hygiene items.
- The **Epic MCP Integration Plan v10** remainders — documented stubs + the full deferred/demand-driven list.
- The **bp-roundtrip** plan's deferred stages 4–5.

**What's already shipped (context):** Epic plan Phases A–17 are largely on `main` (HTTP+SSE, push events, Lyra toolsets, awareness waves → the 249-tool surface), plus this session's streamlining (default-on progressive disclosure + lean graph reads) and fixes (pin-sort, SCC auto-checkout, array-node, 4 friction-log items, 249-tool live smoke).

---

## Cross-cutting constraints (read first — they shape sequencing)

1. **Locked production exe.** Any *server-side* change relinks `BlueprintReaderMcp.exe`, which is held open (LNK1104) while it serves a live MCP client. Workflow for every server change: force-close the bp-reader server → relink → restart the client. (See `[[mcp-exe-locked-during-session]]`.)
2. **Daemon is unreliable right now** — its handshake never appears, so calls fall back to slow one-shot. This is both an outstanding item *and* a prerequisite for cheaply **live-verifying** most other items. Fix it early. (`[[commandlet-daemon-handshake-flaky]]`)
3. **Engine-RTTI wall.** The Phase-12 selection stubs can't return real data in-process (no-RTTI engine + module-private toolkit types). They need an editor-module sidecar or an engine-build change. (`[[mcp-plan-terminal-boundary]]`)
4. **Staleness.** Epic-plan "X of Y" counts are ~9 days old; verify exact per-phase state against the live tool surface + tests during execution.

---

## Tiered inventory

| # | Item | Source | Value | Effort | Blocker/Risk | Recommendation |
|---|------|--------|-------|--------|--------------|----------------|
| **Tier 0 — hygiene / activation (hours)** |
| 0.1 | `.gitignore` `Temp/ tmp/ tpc-data/` | session | L | S | — | Do |
| 0.2 | Activate shipped streamlining (restart client + `LyraEditor` rebuild) | session | H | — | user action | Do (you) |
| **Tier 1 — high-value, tractable** |
| 1.1 | **Daemon handshake fix** | session | **H** | M | editor-startup debug | **Do first** (unblocks fast checks + live verification) |
| 1.2 | bpir↔LyraGenerated header collision | session/bpir | M | S–M | — | Do (prefix RT_ headers) |
| 1.3 | set/map + remaining special node kinds in `add_node` | session | M | S–M | — | Do (mirror UE node-class pick) |
| **Tier 2 — moat / spec parity (larger, high strategic value)** |
| 2.1 | **bp-roundtrip stages 4–5** (parse C++→BPIR whole-class; transpile BPIR→whole-BP) | bp-roundtrip | **H (moat)** | XL | — | Do (the differentiator) |
| 2.2 | **J — cancellation + progress** for long ops (cook/package/automation/build) | Epic (adopt-now leftover) | M–H | M | worker-thread dispatch | Do (last unshipped "adopt-now" item) |
| **Tier 3 — Epic stubs → real data (engine-build dependent)** |
| 3.1 | Phase-12 selection (`get_anim_editor_state`, `get_niagara_module_selection`, `get_curve_editor_selection`) | Epic | M | L | **engine RTTI** | Do *iff* willing to patch engine/add sidecar |
| 3.2 | Wave-3/14/16/17 v1 stubs (`get_hover_target`, `get_isolate_mode`, `reset_project_setting`, …) → real | Epic | L–M | M | editor-module sidecar | Do alongside 3.1 |
| **Tier 4 — convenience / demand-driven (do if wanted)** |
| 4.1 | `inspect_asset` unified quick-look | session | L–M | M | new tool plumbing | Optional (lean reads + `summarize_blueprint` cover most) |
| 4.2 | Tool-description trims | session | L | M (tedious) | — | Optional |
| 4.3 | `take_desktop_screenshot` composite; EA-push Wave-2 Lyra events (GFP/partition) | Epic | L | M | live-editor-only | Demand-driven |
| **Tier 5 — keep deferred (honest: don't build unless trigger fires)** |
| 5.1 | **I** AgentSkill UAsset | Epic | — | — | **blocked: Epic plugin not public** | Hold until Epic ships publicly |
| 5.2 | **G** visual annotation overlay | Epic | L | XL | UE rasterizer | Defer unless vision-primary users |
| 5.3 | **M** reflection-driven tools (`UFUNCTION(AICallable)`) | Epic | — | XL | **loses out-of-process moat** | Keep hand-coded (we're at 249>200 trigger, but lock-in cost stands) |
| 5.4 | **K** MCP-as-client, **L** file sandbox, **EU-write** Slate control, Concert, Live Link | Epic | L | varies | niche / covered by SCC / multi-server | Skip unless explicit demand |

---

## Recommended execution sequence (the build items)

```
Tier 0  (clear the deck; you do the restart/rebuild)
  └─> 1.1 Daemon fix  ──┬─> (enables fast live verification for everything after)
                        ├─> 1.2 bpir collision   ──> un-gate bpir compile tests
                        └─> 1.3 node kinds
              └─> 2.2 J cancellation/progress   (spec parity; needs worker-thread dispatch)
              └─> 2.1 bp-roundtrip stages 4–5   (largest; the moat — schedule as its own mini-plan)
  Tier 3 (3.1/3.2) only if you decide to patch the engine / add an editor sidecar
  Tier 4 as convenience demands;  Tier 5 stays parked behind its triggers
```

Rationale: 1.1 first because a working daemon makes live-verifying 1.2/1.3/2.x cheap (today every live check is a slow one-shot). 2.1 is the highest *strategic* value but XL — treat it as its own scoped plan once Tier 1 is clean.

---

## Per-item approach + exit criteria (actionable tiers)

### 1.1 Daemon handshake fix
- **Diagnose empirically:** clear the stale `Saved/bp-reader-cmdlet*` files, spawn `LyraEditor-Cmd … -run=BPR -Daemon` manually, tail its engine log to find where it stalls (candidates: the `UAssetManager::ScanPathsForPrimaryAssets 'Map'/TopDownArena` ensure seen during startup; module load; the `FCmdletServer::WriteHandshakeFile` path).
- **Likely fixes:** plumb `startupTimeout` through the directly-constructed reader (env only reaches it via `BackendFactory`); make stale-handshake detection robust; ensure `Server.Start()` → handshake write actually runs before the blocking pump.
- **Exit:** daemon publishes `bp-reader-cmdlet.json` within timeout; the 249-tool live smoke runs daemon-backed (fast, not one-shot); 0 broken.

### 1.2 bpir ↔ LyraGenerated collision
- **Approach:** prefix `BPRoundtripModule`'s emitted headers (e.g. `RT_BP_TestEnemy.h`) so they can't collide with LyraGenerated companions; update the roundtrip path assertions.
- **Exit:** `BP_READER_RUN_BPIR_COMPILE=1` runs the two gated `[roundtrip][bpir]` cases green; remove the gate (or keep it as opt-in for cost).

### 1.3 set/map + special node kinds
- **Approach:** in `RunAddNodeOp`, mirror UE's `BlueprintFunctionNodeSpawner` node-class selection: set/map wildcard funcs (`SetParam`/`MapParam` metadata) + remaining special K2 nodes (`Select`, `GetArrayItem`, …). (`[[add-node-node-class-gotcha]]`)
- **Exit:** a live `add_node` + `wire_pins` + `compile_function` test for a set/map function compiles with no "undetermined type"; `list_node_kinds` updated; count assertion bumped.

### 2.2 J — cancellation + progress
- **Approach:** worker-thread tool dispatch + per-backend `IsRunning/Cancel`; `notifications/progress` during long ops; honor `notifications/cancelled` (the `CallContext` marker scaffolded in Phase B). Target the 4 long ops (cook/package/automation/build_lighting).
- **Exit:** a long op streams progress + is cancellable; kill-switch leaves synchronous behavior intact.

### 2.1 bp-roundtrip stages 4–5
- **Scope as its own plan** (XL). Stage 4: whole-class C++→BPIR (`ParseCppPair…`); Stage 5: BPIR→new BP (`TranspileBlueprintWhole`). Completes the BP↔C++ roundtrip moat.
- **Exit:** the `[roundtrip][bpir]` tests assert stages 4–5 *succeed* (currently they assert deterministic failure); a real BP round-trips BP→C++→BP with bounded structural drift.

### 3.1/3.2 Engine-RTTI stubs → real data
- **Approach:** a small in-editor "sidecar" module that *can* cross-cast `IAssetEditorInstance*` to the concrete toolkit (compiled in-editor where the headers exist), exposing selection/hover/isolate data to the live backend. Or patch the engine to export the toolkit headers.
- **Exit:** the stubbed tools return real selection/hover/isolate data on the live backend; mock + commandlet behavior unchanged.

---

## Verification (cross-cutting)
- Mock suite (`BlueprintReaderMcpTests.exe`) green after every change.
- Live verification via the **daemon** (after 1.1) — re-run the 249-tool smoke (`BP_READER_SMOKE_ALL`) + targeted per-item live cases.
- Each server change: force-close server → relink → restart client → confirm behavior in a fresh exe (as done for default-on this session).
- Tool-count + protocol-compat snapshot assertions bumped for any surface change.

---

## Decision log (carried from Epic plan v10 + this consolidation)
- Stay on MCP **2025-06-18** unless we implement tasks/* (then 2025-11-25).
- **M reflection** / Epic `UToolsetDefinition` infra: stay hand-coded (out-of-process moat) despite passing the 200-tool trigger.
- **I AgentSkill:** integrate (don't reimplement) once Epic's plugin is public.
- External HTTP deploy: localhost-only unless funded.
