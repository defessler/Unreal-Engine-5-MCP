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
| **Tier 1 — high-value, tractable (reliability · safety · fast checks)** |
| 1.1 | **Daemon handshake fix** | session | **H** | M | editor-startup debug | **Do first** (unblocks fast checks + live verification) |
| 1.2 | bpir↔LyraGenerated header collision | session/bpir | M | S–M | — | Do (prefix RT_ headers) |
| 1.3 | set/map + remaining special node kinds in `add_node` | session | M | S–M | — | Do (mirror UE node-class pick) |
| 1.4 | **Bad-plugin resilience in commandlet** (DLSS/Wwise-style) | new | **H** | M | — | Do (default `-EnableAllPlugins` + denylist + graceful degrade) |
| 1.5 | **Server / commandlet lifetime** (no zombies, no never-ending commandlets) | new | **H** | M | gated by 1.1 to fully test | Do (harden idle-shutdown + process-tree reap + watchdog) |
| 1.6 | **Default to read-only** (writes opt-in via env) | new | **H** | S–M | behavior change | Do (flip default via existing ReadOnly decorator) |
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
| **Tier D — docs & onboarding (do after the new defaults settle)** |
| D.1 | **Audit all docs for accuracy** (README, `wiki/`, `docs/*`, CLAUDE.md, SKILL.md) | new | M–H | M–L | — | Do (reconcile to current reality + new defaults) |
| D.2 | **Dead-simple quick start** (add plugin + keep up to date) | new | **H** | S–M | — | Do (top of README + wiki landing) |
| **Tier 5 — keep deferred (honest: don't build unless trigger fires)** |
| 5.1 | **I** AgentSkill UAsset | Epic | — | — | **blocked: Epic plugin not public** | Hold until Epic ships publicly |
| 5.2 | **G** visual annotation overlay | Epic | L | XL | UE rasterizer | Defer unless vision-primary users |
| 5.3 | **M** reflection-driven tools (`UFUNCTION(AICallable)`) | Epic | — | XL | **loses out-of-process moat** | Keep hand-coded (we're at 249>200 trigger, but lock-in cost stands) |
| 5.4 | **K** MCP-as-client, **L** file sandbox, **EU-write** Slate control, Concert, Live Link | Epic | L | varies | niche / covered by SCC / multi-server | Skip unless explicit demand |

---

## Recommended execution sequence (the build items)

```
Tier 0  (clear the deck; you do the restart/rebuild)
  └─> 1.1 Daemon fix ──┬─> (enables fast live verification for everything after)
       1.4 plugin     │     reliability cluster — do together; all are
       resilience  ───┤     "make the commandlet/server robust on real projects"
       1.5 lifetime ──┘
  └─> 1.6 read-only default   (safety posture; small; lands the final default set)
  └─> 1.2 bpir collision ──> un-gate bpir tests
       1.3 node kinds
  └─> Tier D docs + quick start   (AFTER 1.6 so docs capture the final defaults:
                                   progressive-disclosure + lean-reads + read-only)
  └─> 2.2 J cancellation/progress   (spec parity; worker-thread dispatch)
  └─> 2.1 bp-roundtrip stages 4–5   (largest; the moat — its own mini-plan)
  Tier 3 (3.1/3.2) only if you'll patch the engine / add an editor sidecar
  Tier 4 as convenience demands;  Tier 5 parked behind its triggers
```

Rationale: **1.1 + 1.4 + 1.5 form a reliability cluster** — a daemon that comes up, survives bad plugins, and reaps itself is what makes the tool usable on real projects *and* makes live-verifying everything else cheap (today every live check is a slow one-shot). **1.6 read-only** is small and lands the final default posture, so **Tier D docs/quick-start come right after** and can document the finished defaults. **2.1** is the highest *strategic* value but XL — scope it as its own plan once Tier 1 is clean.

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

### 1.4 Bad-plugin resilience in commandlet mode
- **Problem:** projects with binary marketplace plugins whose modules aren't built (DLSS, Wwise, etc.) can fail to start — or crash — under `-Cmd` headless launch, taking the daemon/one-shot down on spawn.
- **Approach:** make the commandlet/daemon editor spawn default to **`-EnableAllPlugins`** (UE's switch that makes plugin-module load failures non-fatal; the client-config writer already emits it — make it the backend's spawn default, overridable via `BP_READER_EDITOR_ARGS`). For plugins that crash *harder* than a load-fail (native init), support a **known-bad denylist → `-DisablePlugin=`**, and detect repeated spawn crashes (daemon child exits before handshake) to auto-disable recently-loaded suspects on retry. Always surface the editor-log tail (`bp-reader-mcp-daemon-failure.log`) with an actionable hint.
- **Exit:** a project carrying an unbuilt marketplace plugin starts the commandlet/daemon (or degrades with a clear message) instead of crash-on-spawn; a regression note documents the tested plugin set.

### 1.5 Server / commandlet lifetime
- **Problem:** ensure the server doesn't linger and commandlets never run forever (this session's locked-exe pain is adjacent).
- **Approach:** audit + harden the *existing* mechanisms — daemon idle-shutdown (`BP_READER_DAEMON_IDLE_SECONDS`=300 via `FCmdletServer::WantsShutdown` on `ActiveConnections==0`), per-call subprocess timeout (`BP_READER_TIMEOUT_SECONDS`), MCP-server exit on stdin EOF. Verify each actually fires: (a) the idle daemon reaps itself (couldn't be exercised this session — daemon never came up; do after 1.1); (b) **no orphaned `*Editor-Cmd.exe` outlives the MCP server** — parent the children to a Win32 **Job Object** (kill-on-close) or kill the process tree on exit; (c) a hard **max-lifetime watchdog** backstop so nothing runs unbounded.
- **Exit:** closing/killing the MCP client leaves zero surviving editor/daemon processes; an idle daemon exits within its window; a runaway commandlet is bounded by the per-call timeout. A test (or documented manual smoke) covers each.

### 1.6 Default to read-only
- **Approach:** flip the safety default — **read-only ON by default**, writes opt-in (`BP_READER_ALLOW_WRITE=1`, or invert `BP_READER_READ_ONLY`'s default → on). Route through the existing `ReadOnlyBlueprintReader` decorator (already rejects writes with a clear message) — wire it in `main.cpp`/`BackendFactory` so it wraps the chain by default. Tag write tools in annotations; the generated client config + quick-start show how to enable writes.
- **Exit:** out of the box, every mutation tool is rejected with "writes disabled — set `BP_READER_ALLOW_WRITE=1`"; the env restores writes; read/mock tests unaffected; a test asserts default-rejects-write + env-enables-write. **Behavior change** — call out in the changelog + quick-start.

### D.1 Audit all docs for accuracy
- **Approach:** go file-by-file through `README.md`, `wiki/*` (source-of-truth, manually pushed to the wiki remote), `docs/{research,tutorial,design,superpowers}`, `CLAUDE.md`, and `Plugins/.../Claude/skills/bp-reader/SKILL.md`. Reconcile each to current reality: tool count (249), the **new defaults** (progressive disclosure default-on → ~37 advertised; lean graph reads; read-only-by-default once 1.6 lands), the env-var tables, build commands (`LyraEditor` target + engine patches), the project being **Lyra** (not the historical `UE5_MCP.uproject`), backend selection, and the `call_tool`/`enable_tool_category` discovery flow. Fix drift; flag contradictions.
- **Exit:** a checklist of every doc with pass/fix notes; corrections committed; `wiki/` updated and pushed to the wiki remote per convention.

### D.2 Dead-simple quick start
- **Approach:** one short, copy-pasteable **Quick Start** at the top of README (and the wiki landing): (1) drop `Plugins/BlueprintReader/` into your project's `Plugins/`; (2) build the editor once; (3) paste the MCP client-config snippet (Claude Code / Cursor / …) with the **safe defaults** (read-only + progressive disclosure) and the one line to enable writes; (4) **keep it up to date** — git submodule/subtree (or copy) + `Install-ClaudeAssets` script. No UE internals on the happy path.
- **Exit:** a new user adds the plugin and drives one read tool from their MCP client in <10 min using only the quick start; the "keep up to date" path is verified.

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
