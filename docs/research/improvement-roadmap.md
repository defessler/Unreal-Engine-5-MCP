# BlueprintReader — improvement roadmap (living document)

A prioritized, **status-tracked** backlog of improvements for the
BlueprintReader MCP plugin, derived from a four-track research pass on
2026-06-02:

1. Parity vs Epic's official UE 5.8 MCP plugin
2. Effectiveness + ease-of-use for the AI agents that drive the server
3. State of BP↔C++ transpiling + cross-leverage with the rest of the toolset
4. Install / update / distribution

This is **not** a spec or a record of what shipped — it is the *open list*.
For what's actually shipped, see [`docs/design/`](../design/); for the
transpile architecture see [`bp-roundtrip-architecture.md`](bp-roundtrip-architecture.md).

---

## Maintaining this roadmap

**This file is meant to drift toward truth, not rot.** Keep it current as a
normal part of the work — do not let it become a stale wishlist.

When you **complete** an item:
- Flip its `Status:` to `✅ Done (PR #N, YYYY-MM-DD)`.
- Add a one-line entry to the [Revision log](#revision-log).
- Do **not** delete the item — a done item with its PR is a useful record.
  Once a whole section is done, you may collapse its details, but keep the row.

When you **start** an item: set `Status: 🔄 In progress (PR #N)`.

When you **discover** a new improvement (in any session, from any source —
a client-feedback note, a code review, a new Epic release, a bug):
- Add it under the most relevant section with a **new stable ID** (next
  number in that section's prefix) and `Status: ☐ Open`.
- Give it the same shape as the others: Status / Effort (S/M/L) / the
  concrete file or tool to change / why it matters. Cite paths.
- Note any cross-cutting overlap (e.g. "Cross-cuts: PARITY-x").
- Add a Revision-log line.

**IDs are stable and never reused.** If an item is abandoned, set
`Status: ✖ Dropped (reason)` rather than deleting or renumbering.

Effort key: **S** ≈ <½ day · **M** ≈ ½–2 days · **L** ≈ multi-day.
Status key: `☐ Open` · `🔄 In progress` · `✅ Done` · `✖ Dropped`.

> Maintainer hooks that point here: [`CLAUDE.md`](../../CLAUDE.md)
> ("Improvement roadmap") and the research index
> ([`docs/research/README.md`](README.md)). Update those if this file moves.

---

## Start here — the cross-cutting S-tier

Highest leverage-per-hour. Several fix real correctness/conformance gaps;
one (UX-P1a) closes a Q1 *and* a Q2 gap in a single change.

| Item | Effort | Status | One-liner |
|---|---|---|---|
| [UX-P1a](#ux-p1a) | S | ✅ #250 | Emit `structuredContent` on the default dispatch path — spec conformance for ~218 schematized tools; also closes PARITY |
| [INSTALL-1](#install-1) | S | ✅ #253 | Version-stamp the exe + staleness check in `doctor` — fixes the real stale-copy build break |
| [UX-P0a](#ux-p0a) | S | ✅ #250 | `fields` typo currently projects nothing silently → wrong conclusions; warn / did-you-mean |
| [UX-P0b](#ux-p0b) | S | ✅ #250 | `enable_tool_category` with a misspelled category silently no-ops; did-you-mean error |
| [PARITY-2](#parity-2) | S | ✅ #250 | Bump default protocol to `2025-11-25` (we're on `2025-06-18`; Epic ships `2025-11-25`) |
| [TRANS-P1a](#trans-p1a) | S–M | ☐ Open | Implement *or honestly disable* `Mode::Compilable` — currently promised but a no-op |
| [UX-P4b](#ux-p4b) | M | ✅ impl | `bind_widget_event` now emits a real `K2Node_ComponentBoundEvent` (or honest `bound:false`+reason) — false success fixed, live-verified |
| [UX-P4e](#ux-p4e) | S | ✅ impl | Single full payload in `structuredContent` (short text note) — no more doubled JSON in a spilled temp file |
| [TBX-S1](#tbx-s1) | M | ◑ 1a done | Self-update now hard-fails when the digest is absent (no more skippable check); Authenticode signing (1b) still open |
| [TBX-F1](#tbx-f1) | M | ✅ done | Toolbox Settings "Save" now writes the env block into each provider's config (was dead `localStorage`) |

---

## 1. Parity vs Epic's official UE 5.8 MCP plugin

**Verdict (2026-06-02): equal-or-better overall, decisively ahead on the moat.**
We lead on BP↔C++ transpile/BPIR (Epic: none), general K2 graph authoring
(Epic: asset/property writes only — no node-level mutation of arbitrary BPs),
cooked-runtime introspection, out-of-process crash isolation + a no-UE mock
backend, and raw breadth (252 tools vs ~144 C++ `AICallable` UFUNCTIONs).
Epic leads on the items below. None touch our moat; most are S/M increments.

Reference: Epic plugin at
`D:\Games\Epic Games\UE_5.8\Engine\Plugins\Experimental\ModelContextProtocol`
(+ `ToolsetRegistry` + 18 `Toolsets/*`). Our protocol constant:
`…/jsonrpc/Mcp.h:36` and negotiate list `…/jsonrpc/Mcp.cpp:126-128`.

### PARITY-1 — wire progress emission into the long ops
- **Status:** ✅ Done (PR #259, 2026-06-02) · **Effort:** M
- *Finding: the progress PATH was already wired end-to-end — `CallContext::
  EmitProgress`→`notifications/progress` (unit-tested), the dispatcher extracts
  the progressToken + sets a `CallContext::Scope` (`Mcp.cpp`), `main.cpp:384`
  bridges the backend `progressSink`→`EmitProgress`, and the daemon
  auto-captures `FScopedSlowTask`/GWarn progress (`DaemonProgress.h`) → frames →
  the socket reader → the bridge. The roadmap's "zero emit sites in tools/" was
  misleading: progress flows from the daemon's automatic capture, not explicit
  tool calls. Added a concrete granular emit in `apply_ops` (per-op progress for
  batch writes) + a unit test, and **live-verified** against the real daemon: a
  3-op `apply_ops` emitted "apply_ops 1/3 … 3/3" `notifications/progress` frames.
  The async long ops (`cook_content`/`package_project` scaffolded;
  `build_lighting`/`run_automation_tests` fire-and-poll by design to dodge the
  per-call timeout) emit progress for their synchronous portions via the daemon
  capture; converting them to fully-synchronous-with-progress fights the timeout
  model — deferred as a deliberate non-goal.*

### PARITY-2 — bump default protocol to `2025-11-25` {#parity-2}
- **Status:** ✅ Done (PR #250, 2026-06-02) · **Effort:** S
- We default to `2025-06-18` and negotiate down to `2024-11-05`; Epic ships
  `2025-11-25`. Add it to the negotiate list + default in `Mcp.h`/`Mcp.cpp`.
  Confirm no `2025-11-25`-only *required* semantics (the tasks primitive) are
  implied before advertising.
- **Why:** trivial; keeps us level with Epic and current clients.

### PARITY-3 — inline Image result type for screenshots
- **Status:** ✅ Done (already implemented; verified PR #257, 2026-06-02) · **Effort:** S–M
- *Already shipped: `BuildScreenshotResponse` base64-encodes the PNG into an MCP
  image content block (+ `structuredContent`) for all 3 screenshot tools, with a
  1280px cap and the `BP_READER_NEVER_INLINE_IMAGES` kill-switch. `return_inline`
  is **opt-in (default false)** by design — flipping it risks token bloat and the
  headless `-nullrhi` daemon can't render anyway; the tool descriptions document
  it and `test_phase_d` covers both shapes. Verified complete; no code change.*
- (Original note: `take_screenshot` etc. returned only a disk path; Epic returns
  inline base64 Image content. Return MCP image-content blocks.)

### PARITY-4 — regex allow/block-list governance
- **Status:** ✅ Done (PR #255, 2026-06-02) · **Effort:** M
- *Shipped as `BP_READER_TOOL_ALLOW` / `BP_READER_TOOL_BLOCK` (ECMAScript regex
  lists) enforced at DISPATCH by `ToolRegistry::IsGovernanceBlocked` — consulted
  by `Find` AND `FindAny`, so a block can't be bypassed via the `call_tool`
  meta-tool (the gap vs the disclosure-only `BP_READER_TOOLS`). Applies
  uniformly to every tool incl. the transpile family — the single coverable
  governance knob. (Per-class/property block lists deferred — tool-name regex
  covers the multi-agent-safety case.)*
- Epic's `UToolsetRegistrySettings` has regex allow/block lists + per-class /
  per-property `SetObjectProperties` block lists. Ours is coarse env flags
  (`BP_READER_READ_ONLY` / `ALLOW_WRITE` / `ALLOW_TRANSPILE`). Generalize to a
  tool-name + class/property block model.
- **Why:** finer safety for shared-editor / multi-agent setups.

### PARITY-5 — extend client-config writer to Gemini + Codex
- **Status:** ✅ Done (PR #252, 2026-06-02) · **Effort:** S
- `bp-reader-mcp config --client=…` covers claude-code/desktop/copilot/cursor/
  windsurf/vscode; Epic also writes Gemini CLI + Codex (TOML).
- **Why:** cheap parity; widens the addressable client set.

> **Epic advantages we likely can't match cheaply** (recorded for awareness, not
> as backlog items): **in-engine distribution** (checkbox-enable for any 5.8
> user) and a **Python tool-authoring path** (add tools with no C++/recompile).
> These are strategic, not increments — revisit if priorities shift.

---

## 2. Effectiveness + ease-of-use

**Verdict: already in very good shape.** All 8 items from the 2026-05-29 client
feedback are closed; progressive disclosure, the orientation ladder, response
shaping, and did-you-mean are mature; onboarding docs are current. The
2026-06-07 live-session feedback added a fresh batch (UX-P4a–i) — mostly
refinements, but with **two genuine correctness traps**: `bind_widget_event`
reporting false success (UX-P4b) and `apply_ops atomic:true` not rolling back on
a mid-batch op failure (UX-P4c) — plus the older silent-failure `fields`/category
typo traps (UX-P0a/b, now closed).

### UX-P0a — `fields` typo projects nothing, silently {#ux-p0a}
- **Status:** ✅ Done (PR #250, 2026-06-02) · **Effort:** S
- A genuine typo (`asset_paths` plural, or `nodes[].id` on a tool whose body is
  under `graph.`) returns empty objects with no signal → an agent concludes "the
  BP has no X." After applying the filter in `ApplyProjection` /
  `ApplyResponseControls` (`…/tools/JsonProjection.cpp`, `BlueprintToolsDetail.h`),
  collect any requested top-level segment that matched **zero** keys and surface
  it — throw a did-you-mean, or attach `_warnings:["fields: 'x' matched nothing"]`.
- **Why:** highest value-per-minute fix in the audit — the failure is invisible
  today.

### UX-P0b — `enable_tool_category` typo silently no-ops {#ux-p0b}
- **Status:** ✅ Done (PR #250, 2026-06-02) · **Effort:** S
- A misspelled category (`material` vs `materials`) returns
  `{ok:true, added:[], newly_activated_count:0}` — the agent thinks it widened
  the surface, then can't find the tool. In the handler
  (`…/tools/BlueprintTools.cpp:3824`), if the token is not `all`, not a known
  category (`IsKnownCategory`), and not a registered tool name, throw a
  did-you-mean. Keep the genuine already-active no-op returning `ok:true`.
- **Why:** turns a dead end into a one-turn correctable error.

### UX-P1a — emit `structuredContent` on the default path {#ux-p1a}
- **Status:** ✅ Done (PR #250, 2026-06-02) · **Effort:** S · **Cross-cuts:** Q1 output_schema conformance
- ~218 tools advertise `outputSchema` but almost none emit `structuredContent` —
  the default dispatch serializes JSON into a *text* block
  (`…/jsonrpc/Mcp.cpp:329-346`); only the image tools attach structured content.
  Per MCP 2025-06-18 a tool advertising `outputSchema` SHOULD return matching
  `structuredContent`. When the result is a JSON object/array, also set
  `env["structuredContent"]` (keep the text block for back-compat).
- **Why:** one ~6-line change → spec conformance for all schematized tools, and
  structured-content clients stop having to re-parse text. Closes a Q1 gap too.

### UX-P1b — enrich `add_node` unknown-`kind` errors
- **Status:** ✅ Done (PR #251, 2026-06-02) · **Effort:** S
- *Shipped as client-side `kind` pre-validation against `KnownNodeKinds()`
  (did-you-mean + valid set), kept in sync with `list_node_kinds` by a test.*
- The unknown-kind error comes from the plugin verbatim. The `list_node_kinds`
  table lives server-side (`…/tools/BlueprintTools.cpp:2572`), so the server can
  catch the failure and append "valid kinds: …" — mirroring what `wire_pins`
  already does for pin types (`…:2111-2160`).
- **Why:** agent self-corrects in one turn instead of needing a separate
  `list_node_kinds` call.

### UX-P2a — array projection on the big unprojected reads
- **Status:** ✅ Done (PR #251, 2026-06-02) · **Effort:** S–M
- *`get_class_info` + `read_actor_instance` gained `limit`/`offset` (and
  `get_class_info` `fields`) via a `PaginateField` helper. Pagination verified
  live (Mock doesn't back these two reads).*
- `get_class_info` (`…/tools/BlueprintTools_part3.cpp:1182`) has no `fields`/
  `limit` on `properties[]`/`functions[]`; `read_actor_instance` has `fields` but
  no `limit` on `overrides[]`. A wide class or a heavily-overridden actor is a big
  payload. Add `ParseResponseControls`/`ApplyResponseControls` (or at least
  `limit`/`offset` on the dominant array).
- **Why:** token trim on the remaining unbounded reads.

### UX-P2b — trim multi-line tool descriptions
- **Status:** ✅ Done (PR #258, 2026-06-02) · **Effort:** S
- *Trimmed `add_node`'s description — dropped the per-kind-args enumeration
  (authoritative in `list_node_kinds`) + tightened the `add_*`-family
  disambiguation, keeping the common-kinds list inline. `get_class_info` /
  `find_overriders` left as-is: their descriptions carry non-redundant value
  (and `get_class_info` just gained a useful pagination note in UX-P2a), so
  trimming would lose more than the marginal token saving.*
- Several `add_node`/`find_overriders`/`get_class_info` descriptions run 3–6
  lines. Move "when to use X vs Y" prose into SKILL.md/AGENTS.md; keep
  descriptions to 1–2 lines.
- **Why:** low leverage (only costs tokens once an agent widens past `core`), but
  cheap.

### UX-P3a — robust graph/function name resolution + diagnosable NotFound {#ux-p3a}
- **Status:** ✅ Done (eb1c2b50, 2026-06-03) · **Effort:** S
- *Client report: function/graph names containing a space (or a trailing space)
  were unaddressable by `get_function`/`get_graph`/`get_node` + the write ops,
  while `find_node`/`read_blueprint` saw them. Root cause was the missing
  server-side arg-quoting in the reporter's 2026-06-02 build (already fixed in
  38ac0346 — the daemon/live arg join now quotes space-bearing `-Key=Value`
  values on all three backends). Added the report's requested robustness on top,
  live-verified on UE 5.8: `FindGraphByName` (write path) + the WireJson
  graph/function matchers (read path) do exact-then-whitespace-trimmed matching;
  `get_graph`/`get_function` NotFound now emit the available names via
  `EmitError`; the error label strips the `-Op=` prefix (`op=Function`, not the
  `op=-Op=Function` glitch).*
- **Why:** spaces are common in UE function names; a whole BP was un-editable, and
  the generic code-4 gave callers no signal it was a name-resolution miss.

### 2026-06-07 live-session feedback (UX-P4*)

From a real `live`-backend session driving a running editor (widget BPs +
data-asset reads + multi-op `apply_ops` batches). Ordered roughly by impact.

> **Implementation status (2026-06-08, uncommitted).** P4b, P4c, P4d, P4e, P4g,
> P4h, and P4i(a/c) are **implemented + verified** (mock suite 862/0; P4b/P4c/
> P4d/P4h/P4i-a additionally **live-verified** against the real `-nullrhi` Lyra
> daemon via `Saved/uxp4-live.ps1`). **P4f** needed no code (already fixed in
> #210 — the reporter ran a stale build). **Corrections from investigation:**
> P4g was *not* a doc-rename (both diff tools ship; the real gap was category
> membership — fixed in `ToolCategories.cpp`); P4e was *not* a server-side spill
> writer (it was dual-emission in `Mcp.cpp`). **Deferred:** P4a (paused-editor
> health signal — complex live-infra, kept its own item) and P4i-b (`add_widget`
> insert-at-index — lowest value, full-chain cost). P4d's optional server-side
> pre-flight was also deferred since P4c's rollback already prevents the
> partial-commit a bad ref would cause. Known limitation surfaced live: P4b's
> auto-promote of a *freshly-added non-variable* widget can't regenerate the
> generated-class property mid-op (a UMG compile-timing quirk) — it returns an
> honest `bound:false`+reason, never a false success; already-variable widgets
> bind cleanly.

### UX-P4a — distinguish a debugger-paused / halted editor from an unreachable one {#ux-p4a}
- **Status:** ✅ Done (2026-06-09) · **Effort:** M · **Source:** 2026-06-07 live-session feedback
- *Shipped as an ON-DEMAND health channel (a new `health_check` tool — tool
  count 260→261), deliberately NOT an auto-abort on the op path:*
  - **Editor side:** a game-thread heartbeat (`FTSTicker` @0.1s + per-op bump in
    `RunOneOpFromLiveServer`, `FThreadSafeCounter64` ms) + a `{"type":"health"}`
    frame answered INLINE on the connection WORKER thread (no game-thread
    dispatch) by both the LiveServer and the CmdletServer — so it returns even
    while the game thread is wedged, reporting `game_thread_age_ms`.
  - **Server side:** `HealthResult` + `HealthCheck()` through the whole backend
    chain (Mock synthetic-healthy; Socket = own short-lived connection with
    select-gated 5s reads, classifying healthy/paused/unreachable at a 2s age
    threshold; Commandlet probes an already-attached daemon only — never spawns
    one for a probe; Auto forwards; Caching never caches it; ReadOnly passes
    through). Registered as `health_check` (core category) with an output_schema.
  - **What was deliberately dropped:** an op-path recv timeout that auto-threw a
    classified error. A 4-lens adversarial review found it regressed long silent
    live ops (BuildLighting/Cook/Package run >120s with zero frames on the live
    transport, which had an UNBOUNDED recv before) and a busy game thread is
    indistinguishable from a paused one to a heartbeat — so the auto-abort would
    both kill legitimate ops and mislabel them "paused". The on-demand probe
    delivers the roadmap's "distinct error from a timeout": when a call hangs,
    `health_check` answers `paused` vs `unreachable` in milliseconds. The
    bounded-write `{ok:true, recompile:"pending"}` ack is likewise deferred (it
    needs a heap-state AsyncTask detach on the editor side; the current
    stack-capture lifetime contract makes a naive detach dangle).
  - **Verified:** mock suite 863/0 (incl. a new health_check case); 8 tool-count
    asserts + the protocol hash rebaselined; editor module compiled clean on
    UE 5.8 (UBT, all APIs version-neutral — no guards needed); **live-verified**
    against a real `-nullrhi` daemon: raw health frame (idle age=88ms → op →
    age=4ms, Saved/health-smoke.ps1) AND the full `health_check` tool through
    `BlueprintReaderMcp.exe` live backend (`state:"healthy"`, 2ms). The
    debugger-PAUSED branch itself can't be triggered headlessly — mechanism
    proven (a stalled game thread can't advance the ticker), classification
    threshold untested against a real breakpoint.*
- On the `live` backend, when the editor's game thread is halted (a debugger
  breakpoint, a modal dialog, or a long synchronous task), **every** subsequent
  call — reads included — returns a generic `MCP error -32001: Request timed
  out`. There is no health signal to tell "editor paused/halted" from "editor
  hung/unreachable," and no tool-side recovery. A write in flight (a post-
  `add_variable` recompile) half-succeeds: the variable applies but the
  recompile-ack hangs, so the caller gets no usable acknowledgement.
- Add a liveness/health probe the host can answer even when the game thread is
  stopped (e.g. a socket-level ping handled off the game thread, or a status the
  LiveServer reports when a known pause flag is set), surfaced as a *distinct*
  error from a timeout. Pair with a bounded write timeout that returns
  `{ok:true, recompile:"pending"}` instead of blocking indefinitely.
- **Why:** a single game-thread pause silently wedged a whole `live` session with
  no way to diagnose it; the false "timeout" cost ~an hour. Cross-cuts the
  off-game-thread daemon watchdog (v0.2.0 H3) — same "is the editor actually
  alive?" question on the live/socket path.

### UX-P4b — `bind_widget_event` reports success but emits no node {#ux-p4b}
- **Status:** ✅ Implemented (uncommitted, 2026-06-08) · **Effort:** M · **Source:** 2026-06-07 live-session feedback
- *Shipped: `RunBindWidgetEventOp` now spawns a real `K2Node_ComponentBoundEvent`
  via `FKismetEditorUtilities::CreateNewBoundEventForComponent` (+ idempotent
  `FindBoundEventForComponent` reuse) and compiles+saves, OR returns `bound:false`
  with a concrete `reason`; the MCP handler's `ok` now reflects `r.bound` (was
  hardcoded `true`) and surfaces `reason`. Live-verified: honest `bound:false`+
  reason on a missing widget (the false-success bug is gone). Known limit: a
  freshly-`add_widget`'d non-variable widget can't be auto-promoted mid-op (UMG
  compile-timing) — honest `bound:false`, never phantom success. PART B (new
  `add_node` ComponentBoundEvent/AddDelegate/CreateDelegate kinds) deferred as a
  fallback, not needed for the core fix.*
- `bind_widget_event(<listview>, OnItemSelectionChanged → handler)` returned
  `{ok:true, bound:true}`, but no `K2Node_ComponentBoundEvent` and no handler
  function ever materialized — `find_node`, `read_blueprint(functions)`, and a
  full `get_graph(EventGraph)` all showed nothing, before and after an explicit
  `compile_widget_blueprint`. Confirmed twice. This is the worst failure mode: a
  write tool claiming success while doing nothing.
- Make `bind_widget_event` actually create the component-bound event node (and
  its handler graph) or return `bound:false` with a reason. Compounding gap:
  there is **no `add_node` kind** for a component-bound event or for delegate
  binding (`AddDelegate`/`CreateDelegate`), so there is no graph-level fallback —
  add those kinds (and to `list_node_kinds`). File: `BlueprintTools.cpp`
  (`bind_widget_event` handler) + the widget write path in the commandlet.
- **Why:** false success is the most damaging tool behavior — it forces a silent
  hand-off and erodes trust in every other write tool. **Verify on current
  `main` first** — confirm the tool still claims success with no node before
  building the fix (stale-build caveat, see [[live-verify-server-batches]]).

### UX-P4c — `apply_ops atomic:true` does not roll back (stop-and-save) {#ux-p4c}
- **Status:** ✅ Implemented + LIVE-VERIFIED (reload-from-disk rollback; uncommitted, 2026-06-08) · **Effort:** S–M · **Cross-cuts:** v0.2.0 H1 · **Source:** 2026-06-07 live-session feedback
- *Two parts. (1) Default-selection: the `apply_ops` registry handler couples the
  `on_failure` default to `atomic` — unset + `atomic:true` ⇒ `rollback`, else
  `compile`; explicit `on_failure` always wins. Mock-verified. (2) A REAL rollback
  that works headless — see below.*
- **The mechanism: reload-from-disk, not `CancelTransaction`.** First-pass testing
  exposed that `GEditor->CancelTransaction(0)` is a **no-op in the `-nullrhi`
  headless daemon** (no functional undo buffer): a batched mutation survived in
  memory and a later `save_all` persisted it. (My first "live-verified" was a false
  pass — the test read a non-existent `.json.variables` field.) Real fix in
  `RunEndBatchOp`: because the batch DEFERS every save (`MaybeCompileAndSave` only
  marks BPs pending — nothing reaches disk), each touched package on disk still
  holds the exact pre-batch state, so on `-Rollback` we **reload those packages
  from disk** (`UPackageTools::ReloadPackages`, `AssumePositive`) — discarding ALL
  in-memory mutations and clearing the dirty flag. `EndBatch` now returns
  `rolled_back: N`. **Live-verified on the real daemon** (`Saved/uxp4-rollback.ps1`,
  5/5): a batch adding 2 vars + a node rolled back to the exact pre-batch var set
  AND graph node count; `save_all` could not resurrect the changes; a commit batch
  (no `-Rollback`) still persists. CancelTransaction is kept as a best-effort for a
  full GUI editor. Out of scope: rollback of a BP **created** brand-new in the batch
  (no pre-batch disk state — skipped).
- With `atomic:true` (default), a batch aborts at the first failing op — but the
  default `on_failure:"compile"` still compiles **and SAVES** whatever landed
  before the failure. A 30-op splice that failed at op[11] left new nodes added
  and some rewires applied, with the rest unwired + orphan nodes — a broken
  intermediate graph needing manual reconciliation. "Atomic" implies
  all-or-nothing; the actual behavior is "stop-and-keep."
- v0.2.0 **H1 shipped a real `FScopedTransaction` rollback** (live-verified diff=0
  to pre-batch state) — so this may already be fixed on `main`, OR H1's rollback
  doesn't fire on a *mid-batch op failure* when `on_failure:"compile"` runs.
  **Verify against current `main`**: reproduce a mid-batch op failure with the
  defaults; if it still partial-saves, make `atomic:true` truly roll back (wrap
  the whole batch in the transaction, undo on any op failure regardless of
  `on_failure`), or rename / clearly document the partial-commit semantics + the
  default `on_failure` so callers don't assume rollback.
- **Why:** silent partial mutation of a live graph is dangerous and contradicts
  the tool's name; callers reasonably trust "atomic."

### UX-P4d — splice refs require full GUIDs; one bad ref aborts the batch with a generic error {#ux-p4d}
- **Status:** ✅ Implemented (uncommitted, 2026-06-08; pre-flight deferred) · **Effort:** M · **Cross-cuts:** UX-P3a, UX-P4c · **Source:** 2026-06-07 live-session feedback
- *Shipped: `FindNodeByGuid` now accepts an unambiguous hex PREFIX (≥8 chars,
  exact-parse first, never auto-picks on ambiguity), and the four ref-resolving
  ops (`wire_pins`/`set_node_position`/`delete_node`/`set_pin_default`) emit a
  rich `EmitError` naming the offending ref + listing real node GUIDs (op index
  already added by apply_ops). Live-verified: 8-char prefix resolves a node; a
  `...-XXXX` placeholder returns a named error. The optional server-side
  pre-flight (3) was deferred — UX-P4c's rollback now reverts the partial commit
  a bad ref would otherwise cause, removing the urgency.*
- `apply_ops` `$slot` aliases only reference nodes created earlier in the *same*
  batch; wiring into pre-existing nodes needs full 36-char GUIDs. Using 8-char
  prefixes or `-XXXX` placeholders yields `NotFound (code=4)` with a generic
  message ("asset, graph, node, pin, class … missing") that names neither the
  offending ref, nor the op index, nor that the GUID was malformed. Each miss
  costs a round-trip (re-dump the function, brace-scan for full GUIDs, rebuild);
  combined with UX-P4c, a typo'd GUID also half-applies the batch.
- (1) Accept unambiguous short-prefix node refs (resolve a prefix that matches
  exactly one node in scope). (2) Name the offending ref + op index in the error.
  (3) **Pre-flight validate all refs before mutating anything** (a dry resolve
  pass) — this also removes the partial-commit trigger from UX-P4c for the common
  bad-ref case. Cross-cuts `preview_ops` (which should already do (3)). Files:
  the ref-resolution path in `BlueprintTools.cpp` + the commandlet.
- **Why:** the GUID-extraction tax dominates splice workflows; a named, pre-flight
  error turns a multi-round-trip debugging loop into one clear message.

### UX-P4e — large-output temp spill file contains the JSON twice {#ux-p4e}
- **Status:** ✅ Implemented (uncommitted, 2026-06-08) · **Effort:** S · **Source:** 2026-06-07 live-session feedback
- *Root cause corrected: NOT a server-side spill writer (none exists) — it was
  dual emission in `Mcp.cpp` (full payload in BOTH `content[0].text` pretty AND
  `structuredContent` compact); the MCP client concatenated both when spilling.
  Fix (per maintainer's choice): for object results the full payload goes ONCE in
  `structuredContent` and `content[0].text` is a short pointer note → any
  downstream concatenation yields a single valid JSON document. Mock-verified
  (rewrote the structuredContent test + a `PayloadOf` test helper; 862/0).*
- When `get_graph` / `get_function` / `describe_toolset` exceed the inline read
  cap and auto-spill to a temp file, the file contains the payload **twice** — a
  pretty-printed object followed by a compact duplicate — so `ConvertFrom-Json`
  fails with "Additional text encountered after finished reading JSON content."
  Every large read then needs a brace-depth scan to extract the first object.
- Write the spill payload exactly once, as a single valid JSON document.
  (**Distinct from PERF-1**, which removed the *daemon-internal* temp file via the
  `__MEM__` pointer — this is the **client-facing inline-cap spill file** written
  by the MCP server's response-control path that emits the `saved_to` temp path.)
- **Why:** a recurring parse-breaking tax on essentially every whole-graph dump;
  cheap to fix.

### UX-P4f — tool-exposure model contradicts AGENTS.md ("full surface, no gating") {#ux-p4f}
- **Status:** ✅ Already fixed (#210, 2026-06-01; report was a stale build) · **Effort:** S–M · **Cross-cuts:** [[ai-discoverability-token-research]] · **Source:** 2026-06-07 live-session feedback
- *Investigation confirmed: the false "full surface, no gating" claim + the "don't
  invent find_asset" warning were removed in commit `4cef6cf1` (#210); all three
  AI docs now correctly describe progressive disclosure. The reporter ran a
  pre-#210 build. No code/doc change required; optional "Pick the right tool"
  table annotation left as a nice-to-have.*
- AGENTS.md claims the full tool surface is exposed on every `tools/list` with no
  category gating and no `enable_tool_category` step. In practice several tools
  (`find_asset`, `read_widget_blueprint`, `read_data_asset`, `list_functions`,
  `add_widget`, `set_widget_property`, `bind_widget_event`,
  `compile_widget_blueprint`) were **not** directly callable as `<prefix>-<name>`
  and had to be dispatched through `call_tool` or discovered via
  `list_toolsets`/`describe_toolset`. Direct `find_asset` returned "Tool
  'find_asset' does not exist," yet `call_tool name=find_asset` worked — and
  AGENTS.md separately warns "do not invent … find_asset" as if it doesn't exist.
- Reconcile AGENTS.md (and SKILL / copilot-instructions) with the **actual**
  progressive-disclosure exposure model: the default `tools/list` is the curated
  core (~39 tools) + `call_tool`, and the rest are reached via `call_tool` /
  category widening. Document that accurately (the "behind `call_tool`" reality +
  the discovery ladder), or flatten so every tool is directly callable.
- **Why:** the docs describe an exposure model the server doesn't have; agents
  waste calls discovering that a "missing" tool works behind `call_tool`. Ties
  into the known AGENTS.md/SKILL "full surface" false-claim audit item.

### UX-P4g — `bp_structural_diff` is documented but the server ships `diff_asset` {#ux-p4g}
- **Status:** ✅ Implemented (uncommitted, 2026-06-08; premise corrected) · **Effort:** S · **Cross-cuts:** TRANS-leverage · **Source:** 2026-06-07 live-session feedback
- *Premise was wrong: BOTH `bp_structural_diff` AND `diff_asset`+`prepare_merge`
  ship and are fully wired; no AI doc references a wrong name. The REAL gap: all
  three diff tools were in NO progressive-disclosure category, so the default
  `tools/list` hid them (reachable only via `call_tool` by exact name). Fix: added
  them to the `read` (+ `assets`) categories in `ToolCategories.cpp` so they
  surface via `list_toolsets`/`enable_tool_category`. Mock-verified (category
  membership test). Tool count unchanged.*
- AGENTS.md lists `bp_structural_diff` among the read tools, but the running
  server's read toolset has no such tool (verified by enumerating all read-tool
  names) — the diff capability ships as **`diff_asset` + `prepare_merge`**
  (commits `e8c85174`, `6beef8f9`). A client needing a before/after for a
  changelist found no callable `bp_structural_diff` and fell back to current-state
  reads + "inferred" notes.
- Reconcile the docs to the real tool names (rename the AGENTS.md references to
  `diff_asset`, or add a `bp_structural_diff` alias), and confirm `diff_asset`
  covers the documented use (two asset paths and/or depot-rev vs workspace). The
  TRANS-leverage round-trip-oracle note (§3) should also point at `diff_asset`.
- **Why:** a documented tool that isn't callable is a dead end; the capability
  exists under a different name.

### UX-P4h — `VariableGet` on a function param/local fails with a generic NotFound {#ux-p4h}
- **Status:** ✅ Implemented (uncommitted, 2026-06-08) · **Effort:** S · **Cross-cuts:** UX-P3a · **Source:** 2026-06-07 live-session feedback
- *Shipped: `ClassifyNonMemberVarRef` helper detects when a Self-scope
  VariableGet/VariableSet name is actually a function INPUT param or LOCAL (vs a
  member, incl. inherited) and returns a specific actionable message via
  `EmitError` ("'X' is a function INPUT parameter … wire from the entry node's
  output pin"). Live-verified on a real function input; member-var gets still
  succeed.*
- `VariableGet` for a function **input parameter** (or a local) fails with a
  generic `NotFound` — `VariableGet` resolves member variables only. Reasonable,
  but the caller learns it by failure; the workaround is to route the function
  entry node's output pins.
- Detect the case (the name matches a function input/local, not a member) and
  return a specific message: "X is a function input/local — use the entry node's
  output pin, not VariableGet." File: the `add_node`/VariableGet resolution in
  `BlueprintTools.cpp` + the commandlet.
- **Why:** a clear error turns a learn-by-failure round-trip into a one-line fix.

### UX-P4i — minor lossy / confusing outputs {#ux-p4i}
- **Status:** ✅ a+b+c done (b 2026-06-13) · **Effort:** S · **Source:** 2026-06-07 live-session feedback
- *Shipped: (a) `save_all` filters un-saveable packages (`/Script/*`, transient,
  `PKG_CompiledIn`) from `failed_assets` + adds `nothing_to_save` — live-verified
  no `/Script` noise. (c) `list_blueprints` attaches a `_hint` pointing at
  `find_asset` when a path query returns ≤1 rows — mock-verified. (b)
  `add_widget` insert-at-index DONE (2026-06-13): optional 0-based `index` arg
  inserts the new widget at that child position of the parent panel (AddChild
  then ShiftChild, clamped); `-1`/omitted appends (back-compatible). The response
  reports the final `child_index`. Full backend chain (signature + 5 overrides;
  Mock inherits the throwing default) + plugin op + registration; description
  first-40 preserved → no hash drift. **Live-verified** (`Saved/verify-addwidget-
  index.ps1`, real render editor): appends land 0/1/2, `index=1`→1, `index=0`→0,
  `index=99`→5 (clamped to last) — final order [E,A,D,B,C,F]. Mock 879/879.*
- Three small honesty/affordance nits from the same session:
  - **`save_all` reports `saved_count:0` with `failed_assets:[/Script/*]`** that
    looks like a failure but isn't — the assets were already persisted by the
    preceding compile, and the listed "failures" are engine **script packages**
    (`/Script/SlateCore`, …) that can never be saved. Filter `/Script/*` (and
    other unsaveable transient packages) out of `failed_assets`, and make the
    count distinguish "nothing needed saving" from "0 saved due to errors."
  - **`add_widget` only appends as the last child** of the parent — no
    insert-at-index / child-position control. Add an optional `index` (or
    `before`/`after` sibling) arg to the widget-add path.
  - **`list_blueprints` is path-literal, not name-fuzzy** — a wrong subpath
    returns few/zero results with no hint to use `find_asset` for name search. A
    one-line "use find_asset for fuzzy/name search" hint when a list returns 0–1
    results would help.
- **Why:** none are blocking, but each cost a moment of "is this broken?"; the
  `save_all` one in particular reads as a failure on every call.

---

### UX-P5 — client report 2026-06-11 (StateTree condition BP, duplicate pin GUIDs) {#ux-p5}
- **Status:** ◑ a/b/d/e1 done (e1 2026-06-13); c already-shipped (stale build) · **Effort:** S–M · **Source:** 2026-06-11 client issue report (UE 5.7.4 downstream project, single repro asset with two K2Node_FunctionResult nodes sharing pin GUIDs)
- Key cross-cutting insight: **several reported issues were already fixed in
  current `main`** (prefix-GUID resolution UX-P4d; explicit read-only error;
  DLSS denylist doc). The client's exe self-reports `0.1.0` (the version-stamp
  bug, Toolbox §9 / TBX-V5) — so they could NOT tell their build was stale. The
  version-stamp fix is the durable cure; flagged here as the connective tissue.
- **UX-P5a (ISSUE-001, S1→defensive) ✅ Done:** pin GUIDs are not unique within a
  graph (node duplication copies them). The reader's connections were ALREADY
  correct — `from_node`/`to_node` resolve via the real owning node pointer
  (`GetOwningNodeUnchecked`), unchanged since May 1 — so the client's root-cause
  guess ("keys on pin_id alone") doesn't match current code; their symptom was a
  consumer keying link lookups on pin_id alone. Fix: `get_graph`/`get_function`
  now emit `duplicate_pin_guids: [...]` per graph (the client's own suggestion
  #3) so consumers know to key on (node_id, pin_id) together.
- **UX-P5b (ISSUE-002, S2) ✅ Done:** class / object / soft-object / soft-class /
  asset-reference pins store their literal in `DefaultObject` (a UObject*), so
  `default_value` was null for them — blocking headless self-verification of a
  class-pin edit. The introspector already captured `DefaultObjectPath` +
  `DefaultText`; `PinToJson` now emits `default_object_path` + `default_text`
  (matching `describe_k2node`). Covers `get_node`/`get_graph`/`get_function`/
  `find_node` (all route through `PinToJson`).
- **UX-P5c (ISSUE-003, S3) ✅ already shipped (UX-P4d):** `FindNodeByGuid` accepts
  unambiguous ≥8-hex prefixes and `NodeRefError` already emits a detailed
  "need a full GUID or unambiguous prefix" message + sample GUIDs. The client's
  generic NotFound indicates a stale build. (Follow-up e1: verify the detailed
  NodeRefError propagates through `apply_ops` → the MCP error envelope rather
  than collapsing to a bare code-4.)
- **UX-P5d (ISSUE-004, S3) ✅ Done:** `find_node(query:"")` now enumerates every
  node (optionally narrowed by `kind`) instead of erroring/returning zero —
  matching the tool description. Removed the empty-query+empty-kind rejection.
- **UX-P5e (ISSUE-005, S3 DX):** read-only write error is ALREADY explicit
  (names the tool + `BP_READER_ALLOW_WRITE=1`); DLSS `DLSSUtility` startup
  failure is ALREADY documented (`BP_READER_PLUGIN_DENYLIST`). ✅ **e1 done
  (2026-06-13):** `health_check` now reports `write_enabled` — the server-side
  read-only state (`IBlueprintReader::WritesEnabled()`, answered `false` by the
  outermost `ReadOnlyBlueprintReader` decorator, knowable without the editor) —
  so write-gating is discoverable pre-flight, not on the first rejected write.
  Two touch points only (interface default `true` + ReadOnly override `false`);
  mock-tested (`ReadOnly(Mock)`→false, `Mock`→true) and live-verified through
  the shipping exe (`BP_READER_READ_ONLY=1`→false, `BP_READER_ALLOW_WRITE=1`→
  true; `Saved/verify-health-writeenabled.ps1`). Description first-40 preserved →
  no protocol-hash drift. **Follow-ups also DONE (2026-06-13):** (1) `health_check`
  now also reports `daemon_enabled` (BP_READER_DAEMON) + `disabled_plugins`
  (BP_READER_PLUGIN_DENYLIST) — server config discoverable pre-flight. (2) Found
  + fixed a real bug: `CommandletBlueprintReader::RunOp` discarded the op's
  output file on a non-zero exit and threw a generic "exit=4", so a detailed
  `NodeRefError` (did-you-mean GUIDs) written by `EmitError` was LOST through
  `apply_ops`. Now it reads the structured `error` from the output file first
  (the live/socket backend already did this). **Live-verified**
  (`Saved/verify-e1-polish.ps1`): daemon/plugin notes correct; an apply_ops
  bad-node op surfaces "node ref '…' not found in graph 'EventGraph' … Known node
  GUIDs: …" per-op instead of a bare exit=4. Mock 884/884.
- **Why:** UX-P5a/b restore the reader's trustworthiness for automated diagnosis
  (the report's central theme); the rest are friction the version-stamp fix
  largely dissolves by letting clients run a current build.

---

## 3. BP↔C++ transpiling

**Verdict: the most architecturally interesting subsystem — but fidelity is
asymmetric and one advertised feature is a no-op.** BP→BPIR is very mature
(ambitious auto-lowering: `DoOnce`/`FlipFlop`/`Gate`, latent `Delay`→timer+
continuation, async tasks, EnhancedInput). BPIR→C++ is mature for *readable*
output. **BPIR→BP materialization is the weak link** (4 statement forms vs ~15
emitted). Files: `tools/Bpir.{h,cpp}`, `tools/Decompile.cpp`,
`tools/codegen/CppEmit.cpp` + `CppClassEmit.cpp`, `tools/parse/CppParse.cpp`,
`tools/CompileFunction.cpp`. Gaps catalogued in
[`bp-roundtrip-architecture.md`](bp-roundtrip-architecture.md).

### TRANS-P0a — complete BPIR→BP materialization
- **Status:** ☐ Open · **Effort:** L
- `CompileStatement`/`CompileExpr` (`…/tools/CompileFunction.cpp:291-372`,
  `:180-250`) handle only `comment`/`if`/`set`/`call` + `var`/`lit`/`call`.
  `return`, `cast`, `switch`, `for_each`, `while`, `sequence`, `break`/`continue`,
  and all delegate forms **do not materialize** → C++→BPIR→BP silently drops
  most non-trivial bodies into empty graphs. The `clone_graph`/recreation work
  already proves these K2 nodes can be spawned — port that knowledge here (see
  TRANS-P3c on unifying the two materialization paths).
- **Why:** removes the headline round-trip asymmetry; unlocks C++→BPIR→BP and
  round-trip verification.

### TRANS-P0b — consume `components[]` in `CppClassEmit`
- **Status:** ☐ Open · **Effort:** M
- `decompile` emits a `components[]` array but `CppClassEmit` never consumes it
  and Stage-5 never materializes it → no `CreateDefaultSubobject` in generated
  constructors. Actor BPs lose CameraBoom/Mesh/etc.
- **Why:** any real actor BP is unusable without it. Could feed from
  `get_components` (see TRANS-P2b).

### TRANS-P1a — implement *or honestly disable* `Mode::Compilable` {#trans-p1a}
- **Status:** ☐ Open · **Effort:** S (align docs) / M (implement)
- `CppEmitOptions::Mode::Compilable` exists (`…/codegen/CppEmit.h:21`) and
  `transpile_function`'s schema + description advertise "drop-in .h/.cpp pairs"
  (`…/tools/BlueprintTools.cpp:508-510,550`) — but **no code branches on
  `opts.mode`**; both modes emit identical readable output.
- **Why:** the tool currently promises output it doesn't produce — a
  correctness/honesty issue. Minimum: align the docs/schema; better: real
  includes + macros + resolve `/* TODO[bpr-expr] */` to compilable-or-`checkNoEntry()`.

### TRANS-P1b — verify-by-recompile loop ("the killer app")
- **Status:** ☐ Open · **Effort:** M
- Compose `transpile_blueprint` → `write_generated_source` (path-confined to
  `<Project>/Source/`) → Live Coding/UBT compile → parse compiler errors as a
  precise, free fidelity oracle. All pieces exist; nothing composes them.
- **Why:** turns "readable approximation" into "verified compilable" — the
  biggest moat-deepener.

### TRANS-P1c — promote `__bpr_*` sentinels to first-class BPIR forms
- **Status:** ☐ Open · **Effort:** M
- Spawn/timer/format-text/async/select are encoded as
  `{call:"__bpr_spawn_actor_from_class",…}` that only `CppEmit` knows how to
  render — a C++-specific side channel leaking into the supposedly
  language-neutral IR. Make them real BPIR statement forms so every backend
  handles or rejects them explicitly.
- **Why:** prerequisite for any second-language emitter (Lua/Python/JS) and makes
  the validator form-aware.

### TRANS-P2a — harden branch-merge / post-dominator detection
- **Status:** ☐ Open · **Effort:** M
- The merge walk follows only `then` pins (`…/tools/Decompile.cpp:951-975`) → fails
  on nested branches/loops; `cast`/`switch` set `terminatesExec=true` (`:1021`,
  `:2134`) → drop exec flow after the convergence; multi-source data pins do
  "first source wins" (`:751`).
- **Why:** decompile fidelity on nested control flow.

### TRANS-P2b — surface per-delegate param signatures + consume `get_components`
- **Status:** ☐ Open · **Effort:** M (plugin-side introspector change)
- `Decompile.cpp:1336-1357` already *wants* `meta.delegate_params` for
  async/EnhancedInput callback signatures but the introspector doesn't always
  emit them → param-less stubs. Also: decompile consumes only `ReadBlueprint`/
  `GetFunction`/`GetGraph` — it could pull `get_components` (TRANS-P0b) and
  richer `find_node`/`get_graph` topology (TRANS-P2a).
- **Why:** correct callback signatures; better control-flow reconstruction.

### TRANS-P2c — type→header resolution map for compilable includes
- **Status:** ☐ Open · **Effort:** M · **Depends on:** TRANS-P1a
- `MapBpirTypeToCpp` maps shorthand→C++ spelling but has no include/forward-decl
  resolution. Compilable output needs a type→header map (the Lyra
  `lyra_class_map.json` approach is the proven pattern — see
  [`lyra-bp-to-cpp-conversion.md`](lyra-bp-to-cpp-conversion.md)).
- **Why:** prerequisite for compilable output beyond the parent class.

### TRANS-P3a — whole-class C++ parser
- **Status:** ☐ Open · **Effort:** L
- Lift `ParseCppFunction` to parse a UCLASS + all UFUNCTION bodies so Stage 4
  stops being a passthrough (`bpir_after = bpir_before`) and emitted-C++ → BPIR
  re-derivation becomes an automated fidelity check.
- **Why:** closed-loop verification of the C++ side.

### TRANS-P3b — timeline lowering to `UTimelineComponent`
- **Status:** ☐ Open · **Effort:** L
- `…/codegen/UnsupportedTreatment.cpp:33-38` emits a TODO only; no timeline
  codegen.
- **Why:** removes a common unsupported marker (lower frequency than components).

### TRANS-P3c — unify Stage-5 materialization with `clone_graph`/`apply_ops`
- **Status:** ☐ Open · **Effort:** M–L
- `compile_function`/`apply_ops`/`clone_graph` and BPIR Stage-5 materialization
  both solve BPIR/DSL→K2-nodes in two places. Unify on one core (extend
  `CompileFunctionFromBody`) so every new form lands in transpile *and*
  recreation at once. Natural companion to TRANS-P0a.
- **Why:** eliminates divergence; one place to add forms.

### TRANS-leverage (reverse direction) — `bp_structural_diff` as round-trip oracle
- **Status:** ☐ Open · **Effort:** M · **Depends on:** TRANS-P0a
- Once materialization is complete, a `verify_transpile` meta-tool runs
  BP→BPIR→BP→`bp_structural_diff(source, clone)` and reports residuals — the
  proven pattern from the player-char/Lyra recreation work.
- **Why:** automated transpile-fidelity regression signal.

---

## 4. Install / update / distribution

**Verdict: ~7–8 manual install steps and no defined update mechanism.** The root
cause of the stale-copy build break (a pre-5.8 consuming project compiling an old
plugin copy after a fix landed on `main`) is that **nothing detects staleness** —
`doctor` checks only *file presence* (`…/Diagnostics.cpp:97-241`), the `.uplugin`
has no `EngineVersion`, and `VersionName: "0.1.0"` is never read or stamped.

### INSTALL-1 — version-stamp the exe + staleness check in `doctor` {#install-1}
- **Status:** ✅ Done (PR #253, 2026-06-02) · **Effort:** S
- *Shipped via a new `FindPluginDir` (the existing `GuessPluginDirFromCfg`
  yields the project root, not the plugin dir). Both VersionName-mismatch and
  exe-commit-vs-plugin-git-HEAD staleness warn; version surfaced on `--version`,
  `doctor`, and the `initialize` handshake.*
- Embed plugin `VersionName` + git short-hash into `BlueprintReaderMcp.exe` at
  build time (CMake `add_definitions` / UBT `PublicDefinitions`); surface it in
  `doctor` and the MCP `initialize` handshake. Have `doctor` read the on-disk
  `BlueprintReader.uplugin` version (`GuessPluginDirFromCfg` already finds the
  plugin dir — `…/Diagnostics.cpp:31-42`) and **warn when the running exe's
  stamped version ≠ the on-disk plugin source**.
- **Why:** directly catches the stale-copy class of failure that bit a downstream
  consumer.

### INSTALL-2 — de-hardcode the committed `.mcp.json`
- **Status:** ✅ Done (PR #252, 2026-06-02) · **Effort:** S
- *Shipped a portable `.mcp.json.example` + reconciled the README. The tracked
  `.mcp.json` was left as the maintainer's reference (gutting it risks the
  untrack-deletes-worktree trap + breaking the live session); consumers mount
  the plugin into their own project rather than cloning this repo.*
- `.mcp.json:5,9,10` hardcodes maintainer-only `D:\…` paths *and* contradicts the
  README's source-engine paths (`README.md:309`). Replace with a documented
  placeholder + a note to run `config`, or reduce the committed file to a
  mock-only example and let `config`/`Generate-ClientConfig.ps1` be canonical.
- **Why:** the committed config is non-portable and self-contradictory.

### INSTALL-3 — add `EngineVersion` to `.uplugin`
- **Status:** ✅ Done (PR #253, 2026-06-02) · **Effort:** S
- *Shipped as a `doctor` compat-range check (warn outside UE 5.7-5.8, parsed
  from `Engine/Build/Build.version`) — NOT a hard `.uplugin` `EngineVersion`
  pin, which would fight the plugin's intentional multi-version support.*
- No `EngineVersion` field → UE won't warn on a mismatch; combined with the
  multi-engine API guards, a wrong engine just fails deep in the compiler. Add the
  field (or a documented compat range) + a `doctor` check against the engine's
  `Build/Build.version`.
- **Why:** one line turns silent multi-engine compile failures into an upfront
  warning. See [`feedback`/multi-engine-api-guards] and CLAUDE.md's
  "Multi-engine API compatibility" invariant.

### INSTALL-4 — bring `install-claude-assets.sh` to parity with the PS1
- **Status:** ✅ Done (PR #252, 2026-06-02) · **Effort:** S
- The bash version (`Scripts/install-claude-assets.sh:73-110`) deploys only
  skills+agents; it does **not** deploy `AGENTS.md` or
  `.github/copilot-instructions.md` (the PS1 does — `Install-ClaudeAssets.ps1:123-137`).
  Unix/CI users silently lose the cross-AI guidance.
- **Why:** parity gap; Copilot/Codex users on Unix get less.

### INSTALL-5 — fix the stale `Build-MCPServer.ps1` header
- **Status:** ✅ Done (PR #252, 2026-06-02) · **Effort:** S
- Its comment (`Build-MCPServer.ps1:7-8`) claims "PreBuildStep is gone; this
  script is opt-in" and carries a legacy no-op path — but the `.uplugin` **does**
  declare `PreBuildSteps` (`BlueprintReader.uplugin:60`). Update the header to the
  current build model.
- **Why:** misleads maintainers/consumers about how the build works.

### INSTALL-6 — up-to-date gate in `Build-MCPServer.ps1` {#install-6}
- **Status:** ✅ Done (2026-06-09) · **Effort:** S
- *Added a staleness gate at the single build chokepoint (`Build-MCPServer.ps1`,
  which every path — the Toolbox install-with-build → `Install-Plugin.ps1`, the
  `.bat` wrapper, direct calls — flows through). Before invoking UBT/CMake it
  compares the newest `Tests/` source mtime against the built exe (server +
  tests are engine-independent and depend ONLY on `Tests/`), and **skips the
  build entirely when nothing is newer** — narrowing to just the stale target(s)
  when only one changed. `-Force` overrides. When a rebuild **is** needed but the
  exe is locked by the running MCP server, it now **fails fast with an actionable
  message** ("stop the server / kill-MCPs, then rebuild") instead of a cryptic
  `LNK1104` that forced a terminal restart. Verified: up-to-date → clean skip;
  stale+locked → actionable throw.*
- **Why:** a user "choosing to build" against an unchanged tree while the server
  is running hit a needless relink that failed on the locked exe — costing a
  terminal restart for no reason. See [[project_mcp_exe_locked_during_session]].

### INSTALL-M1 — one-shot `Install-Plugin.ps1`
- **Status:** ✅ Done (PR #261, 2026-06-02) · **Effort:** M
- *Shipped `Scripts/Install-Plugin.ps1`: mounts the plugin (copy or `-Symlink`,
  skipping build artifacts; self-copy = no-op when already mounted), optionally
  applies engine patches, builds the server (auto UBT/CMake via INSTALL-M2),
  writes the client config, deploys Claude/AGENTS assets, runs `doctor`. Glue
  over the existing sub-scripts; syntax-validated.*
- Given `-EngineDir` + `-ProjectFile`: copy/symlink the plugin into
  `<Project>/Plugins/`, auto-detect source-vs-installed engine and run the correct
  build path (UBT or the CMake fallback), apply `Patch-Engine.ps1 -Apply` for
  source engines, run `Generate-ClientConfig.ps1` + `Install-ClaudeAssets.ps1`,
  finish with `doctor`. All sub-scripts exist — this is glue.
- **Why:** collapses ~8 steps to one command and removes the "which path am I on?"
  decision.

### INSTALL-M2 — auto-detect source vs installed engine in the build
- **Status:** ✅ Done (PR #261, 2026-06-02) · **Effort:** M
- *`Build-MCPServer.ps1` now detects an installed engine (`InstalledBuild.txt`)
  and transparently routes to an inline CMake/Ninja build (imports vcvars64;
  CMakeLists lands the exes in `Binaries/Win64`) instead of UBT, which rejects
  Program targets there. **Live-verified** on the installed UE 5.8: the script
  routed to the CMake fallback and built to exit 0.*
- Inside `Build-MCPServer.ps1`, transparently fall back to the CMake build when
  UBT rejects Program targets on an installed engine (`Tests/CMakeLists.txt`
  already lands the exe in the right place).
- **Why:** consumer stops having to pick the toolchain by hand.

### INSTALL-M3 — ship a prebuilt `BlueprintReaderMcp.exe` as a Release asset
- **Status:** ✅ Done (PR #256, 2026-06-02) · **Effort:** M
- *Shipped `.github/workflows/release.yml`: on a `v*` tag it CMake-builds the
  server (RelWithDebInfo), smoke-runs the mock suite + `--version`, bundles the
  version-stamped exe + `fixtures/` + `.mcp.json.example` into a zip, and
  attaches it to the Release (also uploads a workflow artifact on manual runs).
  README gained a "prefer not to build?" download note. (Tag-triggered, so not
  exercised on the PR — verified the YAML + that it mirrors the proven
  mcp-tests build.)*
- The server is engine-version-independent pure C++20; CI already builds it via
  CMake (`mcp-tests.yml`). Attach it to a tagged GitHub Release.
- **Why:** removes the single biggest "just try it" barrier (building at all) for
  mock/commandlet use; foundation of a versioned-release update story.

### INSTALL-M4 — compile the editor module in CI on the targeted engines
- **Status:** ✅ Done (scaffold; PR #257, 2026-06-02) · **Effort:** L
- *Shipped `.github/workflows/editor-build.yml`: a self-hosted-runner workflow
  that compile-smokes `-Module=BlueprintReaderEditor` via UBT against a real
  engine (catches the multi-engine API breaks `mcp-tests.yml` can't). NOT
  triggered on PRs (so it never blocks a merge while a runner is unavailable);
  runs on main pushes touching the editor module + on demand. The maintainer
  provisions the runner (label `ue5`) + sets `UE_ENGINE_DIR`/`UE_PROJECT`/
  `UE_EDITOR_TARGET` — one runner per engine version covers the multi-engine
  invariant. Documented in CLAUDE.md.*
- CI builds only the standalone server + mock tests (`.github/workflows/mcp-tests.yml`)
  — the #223→#240 multi-engine regression passed CI silently. A self-hosted
  runner with cached engine(s) doing a compile-only smoke against a pre-5.8 *and*
  a 5.8 engine is the only real guard for the multi-engine invariant.
- **Why:** editor-module breaks currently reach consumers undetected.

### INSTALL-PKG — packaging split (plugin proper + binary server)
- **Status:** ✅ Done (plan + deliverable #2; PR #260, 2026-06-02) · **Effort:** L
- *Shipped: [`packaging-marketplace.md`](packaging-marketplace.md) — the
  two-deliverable design + executable Fab-submission plan. Key correction: the
  `Tests/` Program targets + `ThirdParty/` are **already excluded** from a
  packaged plugin (UAT packages only `Source/Config/Content/Resources/.uplugin`),
  so no tree move is needed. Deliverable #2 (the standalone server) ships as the
  binary release (Batch 9, done). The real remaining Fab blockers — a
  `PreBuildSteps`-free Marketplace `.uplugin` variant, a `Resources/Icon128.png`,
  and verifying the modules build on an unpatched consumer engine — are
  documented as a focused follow-up requiring per-step editor-build verification
  (NOT a hasty in-place restructure that would risk the verified working build).*
- A clean Fab/Marketplace ship is blocked by the `Tests/` `Type=Program` targets,
  vendored third-party (`Tests/ThirdParty/`), the `PreBuildSteps` shell-out, and
  the 3 engine `.Build.cs` patches. Pragmatic path: (a) ship the plugin proper
  (`BlueprintReaderRuntime` + `BlueprintReaderEditor`) as a versioned/Marketplace
  plugin, and (b) ship the MCP server as a **separate prebuilt binary release**
  (INSTALL-M3). The server doesn't belong in the engine plugin tree anyway.
- **Why:** unblocks real distribution and fixes the "Tests/ dir is actually
  production code" awkwardness.

### INSTALL-M5 — no-build update path + self-contained plugin bundle {#install-m5}
- **Status:** ✅ Done (direct to main, 2026-06-03) · **Effort:** M · Cross-cuts INSTALL-M1
- *Added `Setup-Plugin.bat` (plugin root) → `Scripts/Update-Plugin.ps1`: a
  build-free refresh that downloads the plugin ZIP from GitHub over HTTPS (no git),
  self-updates (re-execs the freshly-downloaded updater if it changed), redeploys
  via `Install-Plugin.ps1 -SkipBuild` (robocopy /MIR preserves the built
  Binaries), and reconfigures. `Install-ClaudeAssets.ps1` now reconciles — prunes
  stale `bp-*` skills/agents no longer in the plugin (bounded to the bp-*
  namespace). Made the deployable AI assets self-contained: the outside-plugin
  `docs/`/`wiki/` cross-refs in `AGENTS.md` + `bp-cpp/SKILL.md` became stable
  GitHub URLs (a fresh plugin-only install has no dangling refs), plus a
  plugin-root README. `_Common.ps1` engine inference now falls back to the
  installed engine when a project's EngineAssociation doesn't resolve (empty /
  unregistered GUID / unknown version). Also fixed a CI regression: mcp-tests +
  release workflows now run the exes from the plugin Binaries dir.*
- **Why:** lets a consumer pull the latest plugin + reconfigure without a rebuild,
  and makes the plugin folder a complete, drop-in bundle.

### TEST-1 — exercise the render/interactive surface against a real active editor {#test-1}
- **Status:** ✅ **Done (2026-06-13)** — Track B render tier live; screenshot
  bugs fixed (first sweep); the render/interactive surface is now codified as the
  `BP_READER_SMOKE_RENDER` live smoke (render tier certified: real capture +
  projection). PIE remains honestly headless-limited; heavy build/cook tools are
  registration-checked. · **Effort:** M–L · **Design:** [`live-gui-testing.md`](live-gui-testing.md)
- **Track B is real (via the TEST-2 P1a render harness):** the
  `-RenderOffscreen` editor (`Saved/start-render-editor.ps1`) has a real D3D12
  RHI, so the render tools that the `-nullrhi` daemon could only
  registration-check now actually run. First sweep
  (`Saved/verify-render-tools.ps1`) found + FIXED two real bugs that ONLY
  manifest on a rendering editor (so prior CI/headless never caught them):
  - **`take_screenshot` ignored `dest_path`** — `HighResShot <WxH> <path>` passes
    the path POSITIONALLY, which the engine parser drops (it only honors a NAMED
    `filename=` token, resetting the override otherwise), so the PNG silently
    landed at the engine default (`Saved/Screenshots/.../HighresScreenshotNNNNN.png`)
    while the tool reported `output_file=<dest_path>` that never existed. Fixed to
    `HighResShot WxH filename="<dest>"` (forward-slashed; FParse escape-processes
    the quoted value). Verified: PNG now lands at the requested path.
  - **`take_viewport_screenshot` was a no-op in the editor** — it used the
    game-only `Shot` exec (UGameViewportClient), unhandled outside PIE, so
    `captured:false` on a real editor. Rerouted through HighResShot at native
    resolution (verified: captures real pixels at `dest_path`).
  - Both now force a viewport redraw after the exec (`GEditor->
    RedrawLevelEditingViewports`) so the async capture renders deterministically.
  - **Render-tier limitation found:** the offscreen editor renders ON DEMAND and
    HighResShot uses a GLOBAL config singleton, so only ONE capture renders per
    interaction — back-to-back screenshots race (the 2nd silently doesn't draw).
    A single capture per call is reliable; a real GPU editor (continuous redraw)
    would not hit this. Sequential captures on `-RenderOffscreen` are unreliable.
  - `set_camera_transform` (moved:true) + `set_show_flag` (ok:true) verified
    working on the render tier.
- **Render sweep CODIFIED (2026-06-13): `BP_READER_SMOKE_RENDER` live smoke.**
  `test_tool_smoke_render_live.cpp` drives the render/interactive surface against
  a real `-RenderOffscreen` editor through the full tool-handler → live-socket
  stack and **certifies the render tier**: `take_screenshot` captured=true (real
  D3D12 capture, not the headless `captured:false`), `world_pos_to_screen`
  valid=true (viewport projection), `get_camera_transform` valid=true, plus
  reachability of get_show_flags / get_view_mode / get_selected_actors and the
  safe view-state writes (set_camera_transform / set_show_flag toggle+restore /
  set_view_mode / set_selection clear). 12 tools dispatched, 0 broken, 0 infra
  (`Saved/run-smoke-render.ps1`). Gated on `BP_READER_SMOKE_RENDER` + the render-
  editor handshake → auto-skips in the editor-less hosted CI. No new bugs (the
  first sweep already fixed the two screenshot ones). PIE stays Track-B headless-
  limited (honest `started:false`; real PIE needs an interactive desktop session)
  and `build_lighting`/cook/package are heavy → registration-checked only.
- The `-nullrhi` commandlet daemon can't exercise the ~30 render/interactive
  tools (`take_screenshot`/`take_viewport_screenshot`/`set_camera_transform`/
  `set_show_flag`/`build_lighting`/`pie_start`/selection/`open_asset_editor`) —
  they gate on `FApp::CanEverRender()`==false and honestly report
  `captured/started:false`. **Fidelity bar (maintainer):** the test must match
  how each tool responds when a person is *actively* using the editor. Plan: two
  tracks — **A** render-capable headless daemon (`-AllowCommandletRendering`;
  cheap, CI-safe, but only faithful for non-viewport captures), **B** a real GUI
  editor (`UnrealEditor.exe -unattended -RenderOffScreen`, `live` backend, map
  loaded + active viewport) for the active-editor-state-dependent group + PIE.
  The wedging startup modal was just a missing `-unattended` flag. Code: extend
  `test_tool_smoke_live.cpp` (pick `SocketBlueprintReader` for `live`; gate render
  tools behind `BP_READER_SMOKE_RENDER`/`_PIE`), add `start-render-daemon.ps1`.
  Phase-0 spike (local) settles which tools each track can faithfully cover.
- **Why:** ~30 tools currently have only registration-level coverage; this is the
  only way to validate they respond correctly to real interactive use.

### TEST-2 — editor UI automation driver (Selenium-style) {#test-2}
- **Status:** ✅ **Done (2026-06-13)** — P0 + P1a + P1b (full Selenium-style
  driver: click/type/focus/invoke) + P2 (`BP_READER_SMOKE_UI` live smoke) all
  shipped. AutomationDriver exclusive sessions were conditional ("only if P1
  proves insufficient") and are **not needed** — the gated game-thread Slate
  injection (P1b) covers the driving surface.
  — `ui_list_widgets` + `get_modal_state` buttons (P0); modal side-channel +
  render-tier harness (P1a); `ui_click` (265 tools, slice 1) + `ui_type` (266
  tools, slice 2) + `ui_focus_tab` (267 tools, slice 3) + `ui_invoke_menu` (268
  tools, slice 4); P2 `BP_READER_SMOKE_UI` Track B smoke ✅ ·
  **Effort:** M–L (phased) ·
  **Design:** [`live-gui-testing.md`](live-gui-testing.md) § "Editor UI automation"
- **P1b slice 1 — `ui_click` SHIPPED + END-TO-END VERIFIED:** click an editor
  widget located by its `ui_list_widgets` path. Resolves the path
  (window-index → child-index walk, type-revalidated so a shifted tree errors
  instead of clicking the wrong widget), guards degenerate geometry (w/h=0 →
  refuse, never inject at (0,0)), then injects a synthetic mouse down+up at the
  widget's geometry center via `FSlateApplication::OnMouseDown/OnMouseUp` (the
  same path real OS input takes → Slate hit-tests + routes it). Gated
  `BP_READER_ALLOW_UI=1` (off by default). **Live-verified on the render tier
  (`Saved/verify-ui-click.ps1`)**: clicking a toolbar combo button opened its
  dropdown — menu-widget count went 18 → 61 (+43), an observable UI effect
  proving the click routed, not a no-op. Editor menus open IN-WINDOW (popup
  layer), not as new OS windows — so the observable is menu-widget growth.
  Findings: offscreen widget geometry is MIXED (top-level + toolbar blocks real;
  collapsed/overflow widgets are (0,0) — hence the geometry guard). Full backend
  chain + categories/annotations (action, NOT read-only — passes through like
  console_command). Next P1b: `ui_focus_window/tab`, `ui_invoke_menu`, + the
  BP_READER_SMOKE_UI Track B smoke.
- **P1b slice 2 — `ui_type` SHIPPED + END-TO-END VERIFIED (266 tools):** type
  text into an editor widget located by its `ui_list_widgets` path. Sets
  keyboard focus to the resolved (type-revalidated) widget, then injects one
  synthetic character event per char via `FSlateApplication::SetKeyboardFocus`
  + `OnKeyChar` (the same path real OS input takes → Slate routes per focus).
  Gated `BP_READER_ALLOW_UI=1` (off by default). `ui_list_widgets` now also
  reads back `SEditableText`/`SEditableTextBox` content, giving an observable
  round-trip. **Live-verified on the render tier** both at the raw-op layer
  (`Saved/verify-ui-type.ps1`) and through the **full MCP server stack**
  (`Saved/verify-ui-type-stack.ps1`, `call_tool ui_type` → typed → read back
  the marker). The default editor view has no editable field, so a gated
  `RaiseTestUiWindow` test hook (`BP_READER_TEST_MODAL=1`) raises a non-modal
  window with an `SEditableTextBox` to type into. Full backend chain +
  categories/annotations (action, NOT read-only).
- **P1b slice 3 — `ui_focus_tab` SHIPPED + END-TO-END VERIFIED (267 tools):**
  foreground an editor dock tab by a case-insensitive substring of its label —
  the geometry-independent way to bring a panel forward (no click, no painted
  geometry). Collects every `SDockTab` across the visible windows, matches the
  label, and calls `SDockTab::ActivateInParent(UserClickedOnTab)` (exactly what
  a header click does). Reports `is_foreground` (the tab's post-activation
  foreground state) + `active_tab` (`FGlobalTabmanager::GetActiveTab()` readback)
  as observables; a no-match lists `available_tabs` (candidate-list NotFound).
  Gated `BP_READER_ALLOW_UI=1`. **Live-verified through the full MCP server
  stack** (`Saved/verify-ui-focus-tab.ps1`): discovered 5 tabs, then focusing
  Viewport 1 / Outliner / Details each returned `is_foreground=true` with
  `active_tab` matching the focused label. Finding: focusing a *major* tab (e.g.
  "Home Panel") correctly switches the whole major-tab away, hiding the
  level-editor minor tabs — so a NotFound on a previously-listed minor tab can be
  a real layout change, not a bug. Full backend chain + categories/annotations
  (action, NOT read-only).
- **P1b slice 4 — `ui_invoke_menu` SHIPPED + END-TO-END VERIFIED (268 tools):**
  execute an editor menu command by its registered `UToolMenus` name + entry —
  the most geometry-independent driving primitive (no click, no painted
  geometry, the menu need not be open). `GenerateMenu(name, context)` (NOT
  FindMenu — Generate populates Sections/Blocks), seeded with the level editor's
  GLOBAL command list (`FLevelEditorModule::GetGlobalLevelEditorActions()`),
  then walks Sections→Blocks matching the entry by command **name** (exact, CI)
  or **label** (substring) and executes its bound action via the **public** API
  — `FToolMenuEntry::GetActionForCommand(ctx, outList)` (command-bound entries:
  honor CanExecute, then Execute the returned `FUIAction`) with a
  `TryExecuteToolUIAction(ctx)` fallback for `FToolUIAction` entries.
  **Scope is honest:** `Action`/`Command`/`ConvertUIAction` are all private, so
  only command-bound entries reachable from the global list (File/Edit/Build/
  Select/Play…) + `FToolUIAction` entries execute; an entry whose command lives
  in a module-specific list (e.g. Fab/Bridge in the Window menu) or that uses a
  plain-`FUIAction`/dynamic/script action type returns a clear `ok:false` that
  points the caller at `ui_click` for geometry-based invocation — never a
  phantom success. A no-match lists `available_entries` ([{name,label}]).
  Gated `BP_READER_ALLOW_UI=1`. **Live-verified through the full MCP server
  stack** (`Saved/verify-ui-invoke-menu.ps1`): `Select/SelectNone` then
  `Select/SelectAll` each returned `ok=true invoked=true`, with
  `get_selected_actors` showing the selection actually change **0 → 21 → 0** (an
  observable editor-state effect, not a no-op); `Window/OpenFabTab` correctly
  returned `ok=false` → ui_click. Full backend chain + categories/annotations
  (action, NOT read-only). **TEST-2 P1b complete** — the Selenium-style driver
  now covers click + type + focus + invoke.
- **P2 — `BP_READER_SMOKE_UI` live smoke SHIPPED + VERIFIED (2026-06-13):** a
  gated doctest (`test_tool_smoke_ui_live.cpp`) that drives the whole editor-UI
  tool surface against a REAL render editor through the full tool-handler →
  live-socket stack and asserts every UI tool is reachable (no "not supported"
  / unreachable / crash) — the UI analog of `test_tool_smoke_live`'s
  `BP_READER_SMOKE_ALL`. Connects via `SocketBlueprintReader` reading the render
  editor's `<Project>/Saved/bp-reader-live.json` handshake; **auto-skips** unless
  `BP_READER_SMOKE_UI` is set AND that handshake exists (so the hosted CI, which
  has no editor, skips it). Drives `ui_list_widgets` (asserts `ui_available` +
  `windows` — proves real Slate data), `get_modal_state`, `get_focused_widget`,
  `ui_focus_tab` (Outliner/Details action paths + a NotFound), and
  `ui_invoke_menu` (NotFound + a safe reversible `Select/SelectNone`). A
  gate-rejection or structured NotFound counts as *reachable*, never *broken*,
  matching the existing smoke philosophy. **Live-verified on the render tier**
  (`Saved/run-smoke-ui.ps1`): 8 UI tools dispatched, 0 broken, 0 infra,
  `ui_available=true`. The modal-recovery drill is separately proven by
  `Saved/verify-test2-p1a.ps1` (the gated `RaiseTestModal` hook needs the
  side-channel, not a tool). Mock 879/879 (the smoke is skipped there). No tool/
  hash change — it adds a test, not a tool.
- **Render tier proven (2026-06-11):** a full editor launched
  `UnrealEditor.exe -RenderOffscreen -unattended` comes up on this box with a
  **real D3D12 RHI and Slate INITIALIZED** (`Saved/start-render-editor.ps1`,
  reuse-if-alive). `ui_list_widgets` returns the actual editor widget tree
  (real `SDockingArea`/`SLevelEditor`/`SButton` paths), which also **retro-
  verified P0's populated-tree paths**: a `type=SButton` walk over the real
  (huge) tree returned in ~3 ms with `truncated=true` — the emit-vs-visit budget
  split bounds traversal cost as intended. This is the TEST-1 Track B render
  tier, now real; it unblocks P1b + the render/screenshot/PIE surface.
- **P0 shipped:** `ui_list_widgets` walks the live editor's Slate tree
  (per-widget path/type/tag/text/visible/enabled/rect, per-window + global
  `truncated`, `ui_available` headless-honesty bool, independent emit-vs-visit
  budgets so a `type` filter can't stall the game thread); `get_modal_state`
  now also returns `buttons[]` (`{path, label?}`, nested-button-safe labels) +
  `buttons_truncated`. Read-only, no gate. Built (editor + server), mock
  870/870, and **live-verified against a real `-nullrhi` daemon both directly
  over TCP and through the full `call_tool`→Socket-backend MCP stack** (honest
  headless contract; populated-tree paths need a GUI editor — TEST-1 Track B).
  An 8-finding adversarial review (3 lenses) was applied before commit: the
  visit/emit budget split, depth-cutoff truncation flag, `ui_available`,
  nested-button label boundary, `buttons_truncated`, and the response-local
  path / global-budget doc clarifications all came from that pass.
- **P1a shipped (modal unblocker):** a worker-thread `modal` TCP frame
  (mirroring the UX-P4a health frame) enqueues a command answered ON THE GAME
  THREAD by a drainer hooked into TWO contexts — the idle heartbeat `FTSTicker`
  and the lazily-registered `FSlateApplication::OnModalLoopTickEvent` delegate
  (the only game-thread context that runs INSIDE `AddModalWindow`'s blocking
  loop). So `report`/`dismiss` work even while a hard modal wedges the normal
  `AsyncTask(GameThread)` op dispatch. `BuildActiveModalReport` is shared with
  `get_modal_state`; commands use shared-ownership + a per-command event so the
  worker/drainer are race-safe across a timeout. Plus the opt-in
  `BP_READER_GUI_AUTOMATION=1` → persistent `GIsRunningUnattendedScript`
  prevention gate (`AddModalWindow` self-cancels non-slow-task modals), and a
  test-only `RaiseTestModal` hook (gated `BP_READER_TEST_MODAL=1`, not a tool —
  264 unchanged). **Live-verified on the render tier** (`Saved/verify-test2-
  p1a.ps1`): with a real modal blocking op dispatch (a concurrent normal op
  WEDGED / timed out), the side-channel reported the modal's title + OK/Cancel
  buttons and dismissed it, recovering BOTH the blocked test op and the wedged
  normal op. **Headless regression PASS** (`verify-test2-p1a-headless.ps1`):
  the daemon's health/op are intact and the `modal` frame degrades gracefully
  (`serviced=true, is_open=false` via the ticker drain). Finding: the heartbeat
  DOES go stale inside the modal loop (FTSTicker isn't pumped there), so
  `health_check` can flag a long modal but can't distinguish it from any
  game-thread stall — the modal channel is the modal-specific answer. Editor-
  module only (no MCP server/tool/hash change).
- Programmatic interaction with the real GUI editor — click buttons, drive
  menus, dismiss modals, inspect widgets. A 3-lens research pass (engine source
  on disk + ecosystem + integration design) settled the approach:
  - **Epic ships the Selenium analog in-engine**: `AutomationDriver`
    (Developer module; full source + DLL + import lib verified present in the
    installed 5.8 engine; linkable from this plugin). `By::Id`/`By::Path`/
    `By::WidgetLambda` locators over ALL visible windows (modals + popups
    included); input injected at the Slate message-handler level (no OS focus,
    immune to "UIA can't see Slate"). Caveat: `Enable()` suppresses real user
    input — exclusive sessions only, P2.
  - **The modal wedge is root-caused**: `AddModalWindow`'s nested loop never
    ticks FTSTicker/AsyncTask. Cure = a startup-registered
    `OnModalLoopTickEvent` side-channel (report/dismiss/click inside the modal
    pump); prevention = opt-in `GIsRunningUnattendedScript`.
  - **Phases**: P0 `ui_list_widgets` + real `get_modal_state` (read-only, no
    gate); P1a modal unblocker (worker-thread frame + modal-tick delegate);
    P1b gated (`BP_READER_ALLOW_UI=1`) `ui_click`/`ui_type`/`ui_invoke_menu`
    via game-thread Slate injection + the TEST-1 Track B smoke
    (`BP_READER_SMOKE_UI`) with a modal-recovery drill; P2 AutomationDriver
    sessions (drag/hover/chords) only if P1 proves insufficient.
  - Also: `Automation RunTests <filter>` is a console command — Epic's own
    automation/screenshot tests are triggerable in a live GUI editor through
    the EXISTING `run_console_command` tool (only result harvesting needs
    building). And the prior "UIAutomation can't see Slate" finding was the
    default-off `Accessibility.Enable` CVar, not a hard limit — a 1-hour spike
    is a worthwhile fallback probe.
- **Why:** unblocks TEST-1 Track B (drive + assert the interactive surface in a
  real GUI editor), turns the historical modal wedge into a tested recovery
  path, and gives AI clients a controlled way to operate editor UI that has no
  tool coverage today.

---

## 5. MCP spec parity (2025-11-25 features not yet used)

*Research source: [research-2026-06-04-mcp-ue5-gaps.md](research-2026-06-04-mcp-ue5-gaps.md)*

### MCP-1 — `title` field on all tools {#mcp-1}
- **Status:** ✅ Done (eb99a2a0, 2026-06-04) · **Effort:** S
- Add `title` (human-readable display name) as a top-level field on every Tool descriptor, distinct from the programmatic `name`. Both Claude Desktop and ChatGPT display `title` in the UI. Purely additive metadata — zero behavioral change. Example: `name="get_graph"` → `title="Get Blueprint Graph"`.
- **Why:** Zero-risk quality-of-life improvement for all client UIs; included in 2025-06-18 spec.

### MCP-2 — Complete tool annotations (readOnlyHint, destructiveHint, idempotentHint) {#mcp-2}
- **Status:** ✅ Done (167b97ad, 2026-06-04) · **Effort:** S
- `ToolAnnotations.cpp` has the framework but not all four hints filled in for all tools. Correct policy: read tools in read-only mode → `{readOnlyHint:true, destructiveHint:false, idempotentHint:true, openWorldHint:false}`; write tools → `{readOnlyHint:false, destructiveHint:true}`. Reflect the runtime read/write mode in the annotations so Claude Code's auto-approval logic sees the correct hints.
- **Why:** Claude Code uses `readOnlyHint:true` for auto-approval in `--auto-approve` mode; ChatGPT store requires correct hints for app submission. Both are already spec-defined since 2024-11-05.

### MCP-3 — Input validation errors as `isError:true` tool results {#mcp-3}
- **Status:** ✅ Done (96ad5f1c, 2026-06-04) · **Effort:** S
- Currently, bad/missing args return JSON-RPC error code `-32602`. The 2025-11-25 spec says this MUST be a valid `CallToolResult` with `isError:true` and actionable message text so the model can self-correct without treating it as a transport error. Catch `std::invalid_argument` in the tool dispatch wrapper and re-emit as a tool error result.
- **Why:** Spec MUST compliance; better model self-correction on bad args.

### MCP-4 — `structuredContent` alongside `content[].text` {#mcp-4}
- **Status:** ✅ Done (96ad5f1c, 2026-06-04) · **Effort:** S
- We declare `outputSchema` on ~220 tools but do NOT emit `structuredContent`. Gemini CLI **errors** when `outputSchema` is declared without `structuredContent` ("Tool has an output schema but did not return structured content"). Fix: when `outputSchema` is non-empty and the result is a JSON object, populate `structuredContent` with the same JSON object alongside the existing `content[0].text` serialization. Claude Code ignores `structuredContent` but Gemini requires it. Cross-cuts with UX-P1a (we already emit it for the default dispatch path — extend to all tools uniformly).
- **Why:** Gemini CLI compatibility; programmatic clients; spec compliance.

### MCP-5 — `description` on serverInfo + `listChanged:true` in capabilities {#mcp-5}
- **Status:** ✅ Done (96ad5f1c, 2026-06-04) · **Effort:** S
- Add a `description` string field to `serverInfo` in `InitializeResult` (e.g. "UE5 Blueprint introspection + mutation + transpile"). Declare `"tools": {"listChanged": true}` in server capabilities so clients know to re-fetch `tools/list` when `BP_READER_ALLOW_WRITE` or `BP_READER_ALLOW_TRANSPILE` toggles at runtime.
- **Why:** Best practice; clients can show the server description in their UIs; mode-toggle without reconnect.

### MCP-6 — Streamable HTTP transport (replace deprecated SSE) {#mcp-6}
- **Status:** ✅ Done (b4ed004a, 2026-06-04) · **Effort:** M
- `HttpTransport.h` implements the deprecated 2024-11-05 two-endpoint pattern (`/sse` + `/message`). The 2025-03-26 spec replaced it with a single `/mcp` endpoint supporting POST (client→server) and GET (SSE stream). Newer clients probe by POSTing `InitializeRequest` to `/mcp`; if they get 405 they fall back. Also required: `MCP-Protocol-Version` header on all HTTP requests after initialize (2025-06-18). Keep old endpoints for backward compat.
- **Why:** Unblocks clients that have moved past the 2024-11-05 transport; old pattern deprecated.

### MCP-7 — Tool description quality pass {#mcp-7}
- **Status:** ✅ Done (05675788, 2026-06-04) · **Effort:** M
- 2025 arXiv study: 97.1% of MCP tool descriptions had quality defects ("unclear purpose" in 56%). Improvements yielded +5.85% task success rate, +15.12% evaluator accuracy. For each tool: add (a) explicit purpose statement, (b) activation criteria — *when* to use this vs. similar tools, (c) key parameter constraints and failure modes. Focus on the ~50 highest-traffic tools first. Claude Code uses BM25 over `name + description + parameter names` for tool selection.
- **Why:** Direct path to better AI task success without any server code changes.

### MCP-8 — Tasks primitive for long-running ops {#mcp-8}
- **Status:** ✅ Done (2026-06-13) · **Effort:** L
- **Full implementation (maintainer chose "full now" over wait-for-GA).** A
  `tools/call` can carry a `task` augmentation (`{"task":{"ttl":60000}}` in the
  params); the call then runs on a BACKGROUND THREAD and returns
  `{"task":{"taskId","status":"working","ttl"}}` immediately instead of blocking
  the request until the op finishes. `tasks/get` polls (returns the finished
  CallToolResult envelope under `result`), `tasks/cancel` flips the cooperative
  cancel flag, `tasks/list` enumerates. `capabilities.tasks` is advertised on
  initialize, and the long-running tools (`build_lighting`, `cook_content`,
  `package_project`, `run_automation_tests`, `compile_blueprint`, `apply_ops`,
  …) carry `execution.taskSupport:"optional"` in `tools/list`.
  **Single-task model:** the editor backend (one socket / one commandlet
  subprocess) is exclusive, so at most one task runs at a time and the server
  rejects other `tools/call`s with a clear busy error while one is active —
  keeping the read loop responsive (tasks/get + tasks/cancel never touch the
  backend). Cancellation reuses the existing `CallContext` + Server in-flight
  registry (the worker registers its context under the taskId). New file
  `tools/TaskManager.h` (detach-safe background execution: workers capture only
  shared_ptrs, never `this`). **Tested:** `test_tasks.cpp` — TaskManager unit
  tests (latch-gated busy/lifecycle/throw-→failed) + the MCP protocol flow
  (capability, async dispatch, poll-to-completion, cancel, list); mock 884/884.
  **Live-verified** (`Saved/verify-tasks-live.ps1`) end-to-end through the
  shipping exe against a real editor: a `task`-augmented `list_blueprints` ran on
  a background thread + `tasks/get` polled it to `completed` with the real
  result envelope. **Caveat:** the 2025-11-25 tasks spec is experimental and is
  redesigned in the 2026-07-28 GA — the wire shape here tracks 2025-11-25 and may
  need a small update at GA; the infrastructure (registry/async/cancel) is
  spec-version-independent.
- **Why:** Long-running ops currently time out in some clients; async tasks fix this correctly.

### MCP-9 — Elicitation for destructive write confirmation {#mcp-9}
- **Status:** ✅ Done (b4ed004a, 2026-06-04) · **Effort:** M
- When the client declares `elicitation` capability (2025-06-18+), pause destructive ops (`delete_variable`, `delete_function`, `delete_node`, `delete_asset`, `build_lighting`) and call `elicitation/create` to request `{confirm: boolean}` from the user. Fall back to immediate execution when the client doesn't declare elicitation.
- **Why:** Better UX for irreversible operations; prevents accidental deletions from AI-generated call sequences.

---

## 6. UE5 editor customization gaps

*Research source: [research-2026-06-04-mcp-ue5-gaps.md](research-2026-06-04-mcp-ue5-gaps.md)*

### EDIT-1 — AnimBlueprint state machine read + write {#edit-1}
- **Status:** ✅ Done (05675788, 2026-06-04) · **Effort:** L
- `add_anim_state` always returns `{added:false}` (explicit stub); `read_anim_blueprint` returns parent class only. The `AnimGraph` module is not in `BlueprintReaderEditor.Build.cs` — state machine walks require `UAnimStateMachineGraph`. Fix: add `AnimGraph` private dep; implement walk of `UAnimBlueprint::AnimationGraphs` + `UAnimStateNode`/`UAnimStateTransitionNode`; write ops via `FBlueprintEditorUtils::AddStateNode`. Key headers: `AnimGraph/Classes/AnimGraphNode_StateMachine.h`, `AnimGraph/Classes/AnimStateNode.h`, `AnimGraph/Classes/AnimStateTransitionNode.h`.
- **Why:** Every character game with locomotion/combat needs AnimBPs. Current stubs mislead AI into thinking AnimGraph is writable when it isn't.

### EDIT-2 — Timeline read + write (UTimelineTemplate + UCurveFloat) {#edit-2}
- **Status:** ✅ Done (96ad5f1c, 2026-06-04) · **Effort:** M
- `K2Node_Timeline` appears in graph reads with `kind=Timeline` + `timelineName`, but `UBlueprint::Timelines` is never walked. Zero tools for track data, key frames, or timeline properties. New tools needed: `read_timeline(asset, name)` → `{tracks: [{name, type, keys: [{time, value, interp}]}], length, loop, auto_play}`; `add_timeline_track(asset, timeline_name, type, track_name)`; `set_curve_key(asset, timeline_name, track_name, time, value, interp_mode)`. No special module needed — `UBlueprint::Timelines` is directly accessible. BPIR transpiler emits `// TODO[bpr-unsupported]` — this would fix it too.
- **Why:** Very high frequency (door animations, weapon recoil, UI transitions, etc.). UTimelineTemplate is on UBlueprint directly — easiest new read/write surface.

### EDIT-3 — UPROPERTY metadata specifiers in class introspection {#edit-3}
- **Status:** ✅ Done (167b97ad, 2026-06-04) · **Effort:** M
- `get_class_info` returns `{name, typeName, category, declaredOn}` per property — no metadata specifiers. Fix: use `FField::GetMetaDataMap()` to add a `metadata` map per property; decode `PropertyFlags` as named booleans (`{blueprint_read_write, replicated, transient, edit_anywhere, ...}`) instead of raw hex; surface `RepNotifyFunc`, `GetCPPType()` per property. New tool: `get_registered_customizations()` — lists registered `IDetailCustomization` and `IPropertyTypeCustomization` from `FPropertyEditorModule`.
- **Why:** AI can't reason about access semantics, EditConditions, or Details panel behavior without this. Needed for Details customization generation and accurate `UPROPERTY()` declaration in transpiled code.

### EDIT-4 — AnimMontage read + write {#edit-4}
- **Status:** ✅ Done (167b97ad, 2026-06-04) · **Effort:** M
- Zero tools for AnimMontage assets. `UAnimMontage` has `TArray<FCompositeSection>`, `TArray<FAnimNotifyEvent>`, and slot tracks — all UPROPERTY arrays readable via standard reflection. New tools: `read_anim_montage(asset)` → sections + notifies + slots; `add_montage_section(asset, name, start_time)`; `add_montage_notify(asset, notify_class, trigger_time)`; `set_montage_slot(asset, slot_name, anim_sequence_path, start_time, length)`.
- **Why:** All GAS/action game projects drive character actions through Montages. Notify names in ABP event graphs (`AnimNotify_<Name>`) are only visible from the montage side.

### EDIT-5 — Custom K2Node: describe + generate skeleton {#edit-5}
- **Status:** ✅ Done (2026-06-09) · **Effort:** L
- *Shipped both tools (261→263; protocol hash rebaselined; catalog regenerated):*
  - **`describe_k2node`** (plugin-side `RunDescribeK2NodeOp` + the full 7-site
    backend chain; mock deny-filtered): resolves a class (short name → FindObject
    → LoadObject), spawns a TRANSIENT instance in a sandbox UBlueprint/UEdGraph
    (engine spawner order: AllocateDefaultPins BEFORE PostPlacedNewNode — the
    reverse order crashes pin-touching PostPlaced overrides like
    SpawnActorFromClass), and reports pins (formatted + structured type),
    purity, title, tooltip, menu category. Rejects non-K2 classes,
    abstract/deprecated, and the AnimGraph family (their PostPlacedNewNode
    CastChecks an AnimBlueprint host — fatal in a plain sandbox). Honesty note
    in the payload: pins reflect an UNCONFIGURED instance; ExpandNode output
    isn't statically introspectable (that part of the original spec was
    dropped as infeasible).
  - **`generate_k2node_skeleton`** (pure server-side codegen, new
    `tools/codegen/K2NodeSkeletonEmit.{h,cpp}`; works on every backend incl.
    mock; NOT transpile-gated — writes no files, parses no untrusted C++):
    emits compilable .h/.cpp text — AllocateDefaultPins from a pin spec,
    GetMenuActions registrar idiom, titles, IsNodePure, canonical ExpandNode
    lowering to a CallFunction. Review-hardened: all free text escaped into
    string literals (injection/compile-break), module_api identifier-sanitized,
    duplicate + reserved ("execute"/"then") pin names rejected, known-class
    spelling table (Actor→AActor …), heuristic spellings TODO-marked, /Script/
    class-only target_function rejected, pin-name-matching contract surfaced
    in `notes`.
  - **Verified:** mock 869/0 (7 new cases incl. escaping/validation); 2-lens
    adversarial review (caught a CRITICAL spawn-order bug — confirmed against
    the engine's BlueprintNodeSpawner.cpp — + injection + AnimGraph crash);
    editor module compiles on UE 5.8; **live** against a real `-nullrhi`
    daemon: FormatText (pure, real title), SpawnActorFromClass (full real pin
    set — the class that crashed pre-fix), IfThenElse, AnimGraph node cleanly
    rejected, daemon survived all calls.*
- `transpile_blueprint` emits `// TODO[bpr-unsupported]` for every non-built-in K2 node class. Two new tools: (a) `describe_k2node(class_path)` — given a custom UK2Node class path, reads its `AllocateDefaultPins` output, `GetMenuActions` category/tooltip, `IsNodePure`, and what `ExpandNode` produced at last compile; (b) `generate_k2node_skeleton(pin_spec, target_function)` — emit compilable `.h`/`.cpp` implementing `AllocateDefaultPins`, `GetMenuActions`, and a canonical `ExpandNode`. Builds on the C++ emit infrastructure in the transpiler.
- **Why:** Plugin authors and framework teams building custom BP extension nodes. Lower frequency than EDIT-1/2 but high value for those teams.

---

## 7. Reflection enrichment

*Research source: [research-2026-06-04-mcp-ue5-gaps.md](research-2026-06-04-mcp-ue5-gaps.md)*

### REFLECT-1 — Decode PropertyFlags as named booleans {#reflect-1}
- **Status:** ✅ Done (eb99a2a0, 2026-06-04) · **Effort:** S
- The current wire format emits `PropertyFlags` as a raw hex integer. Replace with (or add alongside) a named-boolean breakdown: `{blueprint_read_write: bool, blueprint_read_only: bool, replicated: bool, rep_notify: bool, transient: bool, save_game: bool, edit_anywhere: bool, edit_defaults_only: bool, edit_instance_only: bool, asset_registry_searchable: bool, ...}`. Decode from `EPropertyFlags` bit constants already defined in `ObjectMacros.h`.
- **Why:** AI can reason about access semantics without knowing bit positions; eliminates a common source of incorrect UPROPERTY() declaration in generated code.

### REFLECT-2 — Surface full property reflection in class introspection {#reflect-2}
- **Status:** ✅ Done (eb99a2a0, 2026-06-04) · **Effort:** S
- Extend `get_class_info` / `IntrospectClass` per-property to include: (a) `metadata` map from `FField::GetMetaDataMap()` (Category, EditCondition, DisplayName, ClampMin/Max, etc.), (b) `rep_notify_func` from `FProperty::RepNotifyFunc`, (c) `cpp_type` from `FProperty::GetCPPType()`. Also add parameter-level metadata for functions: `HidePin`, `DefaultToSelf`, `AutoCreateRefTerm`, `ExpandEnumAsExecs` — readable via `Param->GetMetaData(...)` on `TFieldIterator<FProperty>(Function)`.
- **Why:** Required for accurate `UPROPERTY()` + `UFUNCTION()` declaration in transpiled code; needed for Details customization generation.

### REFLECT-3 — CDO complex-type defaults as parsed JSON {#reflect-3}
- **Status:** ✅ Done (96ad5f1c, 2026-06-04) · **Effort:** S
- `Property->ExportTextItem_InContainer(...)` on a `FVector` CDO returns `"(X=0.000000,Y=0.000000,Z=0.000000)"` — a string the AI has to parse. Post-process those text defaults through a small set of type-aware parsers (FVector, FRotator, FLinearColor, FTransform) and emit `{"X":0,"Y":0,"Z":0}` directly. For unknown struct types, keep the raw text string as a fallback.
- **Why:** AI-friendly property defaults; eliminates a common source of vector/rotator parsing errors in generated code.

### REFLECT-4 — Parameter metadata in function introspection {#reflect-4}
- **Status:** ✅ Done (96ad5f1c, 2026-06-04) · **Effort:** S
- `get_function` / `GetFunction` returns parameter names and types but not their metadata specifiers (`HidePin`, `DefaultToSelf`, `AutoCreateRefTerm`, `ExpandEnumAsExecs`, `ExpandBoolAsExecs`, `ArrayParm`, `DeterminesOutputType`). These drive how the BP call node renders. Add `param_meta` map per parameter in the function's wire shape.
- **Why:** Required for `generate_k2node_skeleton` (EDIT-5) and for AI-generated `UFUNCTION()` declarations to correctly specify pin behavior.

---

## 8. Performance improvements

*Research source: [research-2026-06-04-mcp-ue5-gaps.md](research-2026-06-04-mcp-ue5-gaps.md)*

### PERF-1 — Eliminate per-call temp-file I/O in daemon (result over TCP) {#perf-1}
- **Status:** ✅ Done (167b97ad, 2026-06-04) · **Effort:** M
- Every daemon call writes JSON to `<Intermediate>/bpr-cmdlet-<guid>.json`, the connection thread reads it back, then deletes it — adding 5–20 ms of filesystem I/O per call (2 syscalls). Fix: pass the result directly over the TCP connection instead of via temp file. `EmitJson` needs to accept a write target (file path OR socket buffer); the CmdletServer connection thread reads from the buffer instead of reading a file. This is the single highest-value latency improvement for the warm-daemon path. File: `BlueprintReaderCmdletServer.cpp:197-251`.
- **Why:** Eliminates the largest controllable per-call overhead on the daemon path.

### PERF-2 — Reduce daemon poll interval 50ms → 5ms {#perf-2}
- **Status:** ✅ Done (eb99a2a0, 2026-06-04) · **Effort:** S
- `BlueprintReaderCmdletServer.cpp:241`: `while (!DoneEvent->Wait(50))` polls the game-thread dispatch at 50 ms intervals. This creates a 0–50 ms wait per call even for sub-millisecond ops. Reducing to 5 ms gives 10× throughput improvement with negligible CPU cost (the thread stays asleep most of the time). The `FEvent::Wait(5)` overload is directly available.
- **Why:** 20 calls/sec maximum → 200 calls/sec maximum with a one-line change. High leverage for interactive AI sessions that make many rapid reads.

### PERF-3 — Cache `GetReferencers`, `GetDependencies`, `ListAssets`, `FindAsset` {#perf-3}
- **Status:** ✅ Done (eb99a2a0, 2026-06-04) · **Effort:** S
- These are asset-registry graph queries that never change between write ops. `CachingBlueprintReader.cpp:1250-1252` explicitly passes them through uncached. Add the same TTL+mtime cache pattern already used for `ListBlueprints`/`ReadBlueprint`. For `ListAssets`/`FindAsset`: these are pure registry queries with no filesystem artifact to mtime-check — use TTL-only (60 s is safe; a write op calls `InvalidateAsset` anyway).
- **Why:** `GetReferencers` and `FindAsset` are called frequently in large-project sessions (every time an AI wants to understand what uses a BP). Currently pay a full daemon round-trip every time.

### PERF-4 — Cache the remaining asset-type read tools {#perf-4}
- **Status:** ✅ Done (eb99a2a0, 2026-06-04) · **Effort:** S
- `ReadDataTable`, `ReadDataAsset`, `ReadMaterial`, `ReadWidgetBlueprint`, `ReadBehaviorTree`, `ReadStateTree`, `ReadNiagaraSystem`, `ReadLevelSequence`, `ReadAnimBlueprint` are all pass-through (confirmed in `CachingBlueprintReader.cpp`). All are `.uasset` files under `/Game/` — the same TTL+mtime cache pattern used for BPs applies directly. Add to `CachingBlueprintReader` with per-type cache keys.
- **Why:** These tools are called on assets that change rarely; each currently pays a full commandlet round-trip. Caching drops subsequent reads to sub-millisecond.

### PERF-5 — Replace per-BP filesystem stats in `list_blueprints` {#perf-5}
- **Status:** ✅ Done (3efb0920, 2026-06-04) · **Effort:** M
- `BlueprintReaderCommandlet.cpp:11837`: `IsoDateForFile(FileOnDisk)` is called per BP inside `RunListOp`. On a 1000-BP project this is 1000 `IFileManager::GetTimeStamp` syscalls in a loop on the game thread — a hidden O(N) cost. Fix option A: derive `modified_iso` from `FAssetPackageData` (available from the registry, no syscall). Fix option B: batch the stat calls asynchronously before returning. The `FAssetRegistryModule::Get().GetAssetPackageData()` API provides `DiskSize` and hash — enough to detect changes without a stat.
- **Why:** Removing O(N) syscalls from `list_blueprints` matters for any session that lists a large project's BPs more than once (the caching backend invalidates this on every write op).

---

## 9. Toolbox (Electron install / configure / test GUI)

**Verdict (2026-06-08 three-auditor audit): feature-complete at v0.6.0, but
carries ~31 distinct improvements** — the most serious cluster is the install/
self-update path, which downloads and *executes* binaries. The Electron main
process (`Toolbox/electron/main.ts`, 757 lines) treats the renderer as fully
trusted and exposes broad IPC primitives; today the renderer only loads local
bundled content, so most "XSS→RCE" chains are defense-in-depth — **except** the
ones reachable through a user-editable `.uproject` or the network, which are
genuinely exploitable and ranked P0. Below P0, the highest-value items are the
*half-built* features that look finished but do nothing (Settings "Save", Tester
array-arg input). Files live under `Plugins/BlueprintReader/Toolbox/`. None of
these are in code yet — this section is the tracked backlog. **A research-driven
flesh-out pass (2026-06-08) added recommended approaches + best-practice grounding —
see [Toolbox research notes](#toolbox-research-notes-2026-06-08) at the end of this
section; treat the per-item file:line refs as audit pointers and the research notes
as the recommended designs.**

Maps to the standalone audit memo [[project_toolbox_audit]]. ID prefixes:
**S**ecurity (P0) · **F**unctional/half-built (P1) · **R**obustness (P2) ·
**P**olish/a11y/maintainability (P3).

### Security (P0 — the app downloads and executes binaries)

### TBX-S1 — self-update runs an unsigned exe with an optional, skippable integrity check {#tbx-s1}
- **Status:** ◑ 1a shipped (uncommitted, 2026-06-08); 1b (signing) open · **Effort:** M
- *1a: self-update + plugin-install now **hard-fail when the release asset has no
  `digest`** (the "no integrity check at all" hole is closed). 1b — Authenticode
  signing (Azure Trusted Signing) + `Get-AuthenticodeSignature` verify-before-
  execute — remains open (needs a cert/CI infra). The sha256 still shares the
  GitHub-API trust channel (by design, documented), so 1b is the real authenticity
  fix.*
- `main.ts:523-604` (swap+relaunch), digest at `399,418-424`. The downloaded
  `-toolbox-win64.exe` is swapped in and **executed** with only a SHA256 `digest`
  that comes from the *same* GitHub API response as the URL, and `digest` is
  `optional` — if the release asset has no digest, integrity is **never checked**
  (only a size check). A tampered release or a MITM on the API/CDN auto-executes
  an arbitrary exe. Fix: verify Authenticode/publisher before the swap (or ship a
  detached signature against a pinned key); at minimum **refuse to self-update
  when `digest` is absent**. Pair with `electron-builder` code-signing config.
- **Why:** the Toolbox ships to other users; auto-executing an unverified binary
  is the single highest-severity issue in the app.

### TBX-S2 — shell injection via `.uproject` EngineAssociation in a `reg query` {#tbx-s2}
- **Status:** ✅ Done (uncommitted, 2026-06-08) · **Effort:** S
- *`execFileSync('reg', ['query', key, '/v', name], {shell:false})` (no cmd.exe) +
  a strict `^[0-9]+\.[0-9]+(\.[0-9]+)?$` regex on `EngineAssociation` before it's
  used. typecheck-clean; review-confirmed the injection is closed.*
- `main.ts:116-132` (`getEngineDir`). The non-GUID branch interpolates
  `EngineAssociation` (read from a user-editable / possibly-untrusted `.uproject`)
  straight into `reg query "HKLM\…\${assoc}"` via `execSync` (a `cmd.exe` shell).
  A value like `" & calc & "` breaks out and runs arbitrary commands. The GUID
  branch already regex-validates; the version branch does not. Fix: validate
  `assoc` against a strict version/GUID pattern, and prefer spawning `reg.exe`
  with an argv array (no shell) or a native registry module.
- **Why:** opening a crafted project should never be able to execute code.

### TBX-S3 — PowerShell argument injection in install / extract {#tbx-s3}
- **Status:** ✅ Done (uncommitted, 2026-06-08) · **Effort:** S/M
- *`Expand-Archive` is now a STATIC `-Command` with the zip/dest passed via env
  vars (no path interpolation); `-Client` is validated against `ALLOWED_CLIENTS`
  ({All,ClaudeCode,Cursor,VSCode,Rider,Gemini,Codex} — matched to the caller +
  the script's ValidateSet after the review caught a casing regression). Avoided
  the extract-zip dep by using the env-var approach.*
- `main.ts:491-500` (Expand-Archive `-Command` string-building), `615,629-636`
  (install-script args). `opts.client`/`opts.uproject`/`opts.engineDir` flow from
  the renderer into `pwsh -File <script> -Client <…> -ProjectFile <…>` unvalidated;
  a `client` value starting with `-` or containing metacharacters injects extra
  parameters, and the `Expand-Archive -Command` form only escapes single quotes.
  Fix: validate `client` against the known enum, reject args beginning with `-`,
  confirm paths exist, and prefer `-File` invocation (or a Node unzip lib) over
  `-Command` string-building.
- **Why:** turns a malformed input into parameter/command injection on the host.

### TBX-S4 — `run-script` / `read-file` / `write-file` are unconstrained primitives {#tbx-s4}
- **Status:** ✅ Done (uncommitted, 2026-06-08) · **Effort:** M
- *`run-script` constrained to `.ps1` under the plugin `Scripts/` dir;
  `read-file`/`write-file` to allowed roots (home/appData/temp/project) minus a
  sensitive denylist (~/.ssh, ~/.aws, ~/.gnupg, ~/.config/gh, Startup). All checks
  go through a **realpath-aware** `isPathUnder` (the review's symlink/junction
  bypass), and `save-project` now validates a real `.uproject` (closes the
  renderer-relocates-root chain). Defense-in-depth — renderer loads only local
  bundled content today.*
- `main.ts:677-699` (`run-script` spawns `pwsh -File <any path> <any args>`),
  `302-313` (arbitrary-path read/write). The renderer can run any `.ps1` from any
  path and read/write anywhere on disk. Fix: constrain `run-script` to an
  allowlist of known script names resolved under the plugin's `Scripts/` dir
  (reject `..`, absolute paths outside the subtree, non-`.ps1`); scope
  `read-file`/`write-file` to project/plugin subtrees; treat the renderer as a
  semi-trusted boundary and validate every IPC input.
- **Why:** these primitives grant the renderer arbitrary local code/file power,
  defeating `contextIsolation`.

### TBX-S5 — navigation/window lockdown, `openExternal` scheme check, CSP, signing {#tbx-s5}
- **Status:** ✅ Done except signing (uncommitted, 2026-06-08) · **Effort:** M
- *`setWindowOpenHandler(deny)` (http(s)→OS browser) + `will-navigate`/`will-redirect`
  guard; `openExternal` restricted to http(s) (file:// "Open config file" preserved
  via a constrained `shell.openPath`, per the review regression); prod-only CSP
  (`default-src 'self'` + loopback `connect-src` for the Tester). asar + code-signing
  fold into TBX-S1 1b.*
- `main.ts:193-236` (no `setWindowOpenHandler`/`will-navigate` guard), `331-333`
  (`shell.openExternal(url)` with no scheme check — `file:`/UNC/`javascript:` all
  pass), no CSP on the main window, and `electron-builder.config.json` has no
  `asar`/code-signing. Fix: `setWindowOpenHandler(() => ({action:'deny'}))` +
  a `will-navigate`/`will-redirect` guard, whitelist `http(s):` in `openExternal`,
  set a strict CSP, enable asar + signing.
- **Why:** defense-in-depth so a future remote-content or supply-chain slip can't
  reach the privileged preload bridge.

### Functional / half-built (P1 — look finished, do nothing)

### TBX-F1 — Settings "Save" writes to dead `localStorage` that nothing reads back {#tbx-f1}
- **Status:** ✅ Done (uncommitted, 2026-06-08) · **Effort:** M
- *Shared `src/lib/settings.ts` store (guarded parse — also closes R9); `paths.ts`
  replaced per-provider `serverEntry()` with `serverType`/`baseEnv` + a
  `buildServerEntry` helper; `Providers.tsx` now writes EVERY JSON provider via
  the sibling-preserving merge, injecting the Settings env (+ a pinned
  `BP_READER_PROJECT`, restoring the old script's behavior — review catch).
  Settings footer explains it applies on (re)configure. Codex (TOML) stays on the
  script (env-injection there is a noted follow-up). Review-verified the per-client
  shapes (configKey/type/args) all match the prior script output.*
- `Settings.tsx:115-149`. The page edits ~30 env flags and "Save," but Save only
  writes `localStorage('bpr-env-overrides')` — **nothing reads that key back** into
  any provider config or `.mcp.json`. The only way to use the values is to "Copy
  env block" and paste manually. Fix: wire Save to write the env block into the
  configured provider configs (Providers already knows how to merge), or relabel
  it a local draft and lead with Copy.
- **Why:** the most likely user-confusion item — the page looks fully functional
  but applies nothing.

### TBX-F2 — Tester cannot input array/object arguments → `apply_ops` uncallable {#tbx-f2}
- **Status:** ✅ Done (uncommitted, 2026-06-08) · **Effort:** M
- *`ArgForm` renders a JSON `<textarea>` (red border on invalid) for array/object
  props; `coerceArgs` JSON-parses them — so `apply_ops`/`compile_function` are
  callable from the Tester. Hand-rolled (no rjsf), per the research.*
- `Tester.tsx:561-626` (`ArgForm` renders only text/enum/boolean), `76-93`
  (`coerceArgs`). The schema models `items`, but there's no UI for `array`/`object`
  args, so `apply_ops` and the batch-write tools — *the project's core* — can't be
  exercised from the Tester. Fix: render a JSON/array editor for `array`/`object`
  properties (or a structured row-builder for `ops[]`), parse + validate before
  send.
- **Why:** the single biggest functional gap — the flagship tools are untestable
  in the app built to test tools.

### TBX-F3 — Tester sends with no argument validation {#tbx-f3}
- **Status:** ✅ Done (uncommitted, 2026-06-08) · **Effort:** S–M
- *`validateArgs` (required/number/enum/valid-JSON) gates both single `sendCall`
  AND the batch runner (review catch — batch was initially unguarded); blocks
  with a clear message instead of a confusing round-trip error. Hand-rolled (no
  ajv) — schemas are simple.*
- `Tester.tsx:265-278`. `required` fields aren't enforced (only visually starred),
  and `Number(resolved)` silently yields `NaN`. Fix: validate against the tool's
  input schema (required/number/enum) and block Send with inline errors.
- **Why:** turns confusing server-side failures into upfront, local guidance.

### TBX-F4 — Tester search silently truncates at 50 results {#tbx-f4}
- **Status:** ✅ Done (uncommitted, 2026-06-08) · **Effort:** S
- *Removed the `.slice(0,50)` (renders all matches) + a "N matches" count.
  Virtualization (`react-virtual`) deferred as unneeded at ~250 filtered rows.*
- `Tester.tsx:400` (`filteredTools.slice(0, 50)`). With ~250 tools, matches beyond
  50 vanish with no "showing 50 of N." Fix: show the count and/or virtualize the
  list.
- **Why:** a search that hides matches sends users hunting for tools that are there.

### TBX-F5 — no `.uproject` validation anywhere {#tbx-f5}
- **Status:** ✅ Done (uncommitted, 2026-06-08) · **Effort:** S
- *Install validates extension + on-disk existence (via a dedicated
  `uproject-exists` boolean IPC — NOT the allowlist-gated `read-file`, which the
  review caught would wrongly fail a first off-home-drive pick), gates the button,
  shows an inline error.*
- `Install.tsx:97-102` (button enabled on any non-empty string), `Update.tsx:148`.
  Failures surface deep in IPC. Fix: validate extension + existence (needs an
  `existsSync`-style IPC the bridge lacks today), surface "Not a .uproject" / "File
  not found" inline, and gate install when the project can't be confirmed.
- **Why:** install/update happily run against a path that can't work.

### TBX-F6 — MCP client has no timeout/cancel, no reconnect; shared id counter; init race {#tbx-f6}
- **Status:** ✅ Done (uncommitted, 2026-06-09) · **Effort:** M
- *`mcp-client.ts` rewrite: per-request `AbortController` with a default 30 s
  timeout + external cancel signal + `cancelAll()`; `initialize` de-raced via a
  cached in-flight promise (cleared on failure to allow retry); `nextId` moved to
  an instance field; a `request()` wrapper transparently `reset()`s + re-inits
  once on a lost/expired session (incl. HTTP 404). Tester wires a Cancel button
  (replaces Send while in-flight) and `cancelAll()` on stop/kill. tsc clean.*
- `mcp-client.ts`: `fetch` with no `AbortController` (`50-54`) so a wedged daemon
  hangs the spinner forever; `reset()` defined but never called + no re-handshake
  on session loss (`29,100-103`); module-global `nextId` shared across instances
  (`4`); `initialize` race when two calls fire before the first resolves (`70-78`).
  Fix: per-request timeout + a cancel button, cache the in-flight init promise,
  move `nextId` to an instance field, re-init on session-not-found.
- **Why:** the daemon is known-slow/flaky; the UI must time out and recover rather
  than hang.

### TBX-F7 — `ensureClient` can't reconnect and leaks orphan mock servers {#tbx-f7}
- **Status:** ✅ Done (uncommitted, 2026-06-09) · **Effort:** M
- *`ensureClient` reconnects to the known `serverPort` when `serverPid` is set
  (no second server, no forced re-Start); the mock auto-spawn is guarded on
  `serverPid === null`; the liveness poll now requires 3 consecutive misses
  before nulling the ref, so a single transient probe can't trigger an orphan
  spawn. tsc clean.*
- `Tester.tsx:177-188`. Non-mock backends throw "click Start" instead of rebuilding
  the client for the known port; the mock path spawns a **new** server every time
  the liveness poll (`160-167`) nulls the ref (which a single transient probe can
  trigger). Fix: reconnect to the known `serverPort` when `serverPid` is set; guard
  mock auto-start on `serverPid`; add a failure-count threshold to the poll.
- **Why:** orphan mock servers are exactly what `kill-mcp` exists to clean up.

### TBX-F8 — `cmpVersion` ignores pre-release/build metadata → can hide updates {#tbx-f8}
- **Status:** ✅ Done (uncommitted, 2026-06-08) · **Effort:** S
- *Semver-correct comparator (pre-release < release, numeric-not-lexical, build
  metadata ignored) in shared `src/lib/semver.ts` (Update.tsx) + a matching copy
  in `main.ts` (self-update guard). Review-verified the precedence cases.*
- `Update.tsx:11-19` and `main.ts:356-364`. Both strip the `-` pre-release suffix,
  so `v0.6.0-rc2` compares equal to `v0.6.0` — a real update is hidden or a
  downgrade offered; non-semver tags collapse to `0.0.0`. Fix: a proper semver
  comparator (pre-release < release); treat unparseable tags as "don't update."
- **Why:** the update mechanism can silently fail to offer a newer build.

### TBX-F9 — Providers "Configure All" clobbers per-provider logs {#tbx-f9}
- **Status:** ✅ Done (uncommitted, 2026-06-08) · **Effort:** S
- *Extracted `configureOne` (appends only); Configure All clears the log once,
  prints a per-provider header, and ends with a `Configured N/M` tally.*
- `Providers.tsx:93-131`. Each iteration calls `setLogs([])`, so only the last
  provider's output survives; no per-provider pass/fail tally; `refreshStatuses`
  runs N times. Also the script-branch (`118`) ignores the return code entirely.
  Fix: clear logs once, append per-provider headers, capture each exit code, show
  a tally, refresh once at the end.
- **Why:** batch config gives the user no idea what happened to each provider.

### Robustness (P2)

### TBX-R1 — spawned child processes are untracked and orphan on quit {#tbx-r1}
- **Status:** ✅ Done (uncommitted, 2026-06-09) · **Effort:** M
- *`trackChild` registers every transient spawn (install/extract/script/kill); the
  window-close handler + a new `cancel-operation` IPC tear down the whole tree via
  `taskkill /T /F`. Cancel also aborts the in-flight download fetch (tracked
  AbortControllers), and Install shows a Cancel button while running.*
- `main.ts:432-440` (`runPwsh`), `661-674` (`kill-mcp`), `677-699` (`run-script`).
  Only `start-server` PIDs are killed on window close; install/extract/script
  children (and any UE build they spawn) orphan — and can hold locks on the very
  plugin dir being swapped. No timeout on `runPwsh`/`run-script` and no cancel
  path. Fix: track all children, kill the tree (`taskkill /T`) on quit; add
  timeouts + a cancel IPC.
- **Why:** orphaned PowerShell/build processes wedge the next operation.

### TBX-R2 — downloads aren't atomic; partial files leak; no retries {#tbx-r2}
- **Status:** ✅ Done (uncommitted, 2026-06-09) · **Effort:** M
- *Each attempt streams to `${dest}.part`, verified (size + sha256) before an
  atomic rename into place; the `.part` is always unlinked on failure. Bounded
  backoff retries honor `Retry-After`; 5xx/429/stall/short-read are retriable, a
  digest mismatch is not. (Range-resume left as a future optimization.)*
- `main.ts:385-430`. `downloadFile` streams straight to the final path; the
  stall-watchdog abort leaves a partial file (cleanup only runs on size/hash
  mismatch), contradicting the "partials removed" comment. Single-shot — no retry
  on transient 5xx/rate-limit. Fix: download to `.part`, rename after all checks;
  always unlink leftovers in `catch`/`finally`; add bounded backoff retries
  honoring `Retry-After` + Range-resume for the large exe/zip.
- **Why:** a flaky network leaves junk or fails a multi-MB install with no recovery.

### TBX-R3 — `start-server` leaks the full env and force-enables write mode {#tbx-r3}
- **Status:** ✅ Done (uncommitted, 2026-06-09) · **Effort:** S
- *Dropped the hardcoded `BP_READER_READ_ONLY:'0'` (restores read-only-by-default;
  write mode is now an explicit `BP_READER_ALLOW_WRITE=1` via Settings). Env is
  forwarded as a denylist (drops token/secret/credential keys) rather than an
  allowlist — an allowlist would starve the commandlet-spawned editor of the
  system env it inherits (review H1). Resolve only if the process is still alive
  after 500 ms; reject on spawn error.*
- `main.ts:704-738`. Blanket-spreads `process.env` + renderer `opts.env` into the
  child, and hardcodes `BP_READER_READ_ONLY:'0'` — silently defeating the project's
  read-only-by-default invariant. The 500 ms warmup resolves the PID even if the
  process already crashed. Fix: whitelist forwarded env keys; make write-mode a
  surfaced choice; check `exitCode === null` before resolving.
- **Why:** a server the Toolbox started shouldn't silently be in write mode.

### TBX-R4 — `kill-mcp-servers` has a machine-wide blast radius {#tbx-r4}
- **Status:** ✅ Done (uncommitted, 2026-06-09) · **Effort:** S
- *Scoped by default to this project's server exe (`ExecutablePath -ieq`) + a BPR
  daemon whose command line `.Contains()` this project dir; `{global:true}` opts
  into the machine-wide sweep. Tracked PIDs are always dropped first.*
- `main.ts:650-674`. Force-kills **every** `BlueprintReaderMcp.exe` on the machine
  (matches the known friction note in [[project_client_feedback_2026_05_29]]),
  including other projects/users or a CI run. Fix: default to tracked PIDs; make
  the global sweep explicit opt-in; optionally match on working-dir/command-line.
- **Why:** one project's Toolbox shouldn't kill another's server.

### TBX-R5 — self-update `.bak` restore can permanently break the app {#tbx-r5}
- **Status:** ✅ Done (uncommitted, 2026-06-09) · **Effort:** S
- *swap.ps1 deletes the `.bak` only after a verified swap **and** relaunch — the
  new exe is launched `-PassThru`, slept 2 s, and checked `-not HasExited` before
  the backup is discarded; the restore path keeps the backup.*
- `main.ts:563-576`. The `.bak` is deleted unconditionally after the relaunch
  attempt — including on the restore path — and a failed restore (still-locked exe)
  can leave a partially-overwritten exe with no usable backup. Fix: only delete
  `.bak` after a verified-successful relaunch (confirm the new process/version);
  keep it if the restore path was taken.
- **Why:** a bad self-update should be recoverable, not bricking.

### TBX-R6 — synchronous fs in IPC handlers blocks the main process {#tbx-r6}
- **Status:** ✅ Done (uncommitted, 2026-06-09) · **Effort:** S
- *read-file/write-file use `fs.promises`; write-file is atomic (sibling temp →
  rename, unique per write). The stale-temp sweep is async + deferred off the
  launch critical path (runs after `createWindow`).*
- `main.ts:302-313` (read/write), `242-251` (startup temp-sweep runs a recursive
  `rmSync` before the window is created). Sync I/O freezes all IPC + window
  responsiveness on slow drives or large dirs. Fix: use `fs.promises`; defer the
  temp-sweep off the launch critical path; make config writes atomic (temp+rename).
- **Why:** the UI shouldn't stutter or delay launch on disk I/O.

### TBX-R7 — large-payload rendering janks (JsonViewer / raw / LogStream) {#tbx-r7}
- **Status:** ✅ Done (uncommitted, 2026-06-09) · **Effort:** M
- *JsonViewer caps children per level ("show more") + truncates long strings +
  lazy-renders deep/collapsed nodes; LogStream caps the rendered tail (500 lines)
  and only auto-scrolls (instant, the container not the page) when pinned to the
  bottom; Tester stringifies `resultRaw` lazily — only when the Raw view is open.*
- `JsonViewer.tsx:8-98` (a component+`useState` per node, no cap/virtualization),
  `Tester.tsx:139` (`resultRaw` eagerly stringifies every result), `LogStream.tsx:
  11-39` (renders unbounded lines, smooth-scrolls on every append, hijacking
  scroll). A big tool result or a chatty build freezes the renderer. Fix: per-level
  item caps + "show more," string truncation, lazy child render; stringify raw
  lazily; cap LogStream lines + instant scroll only when pinned to bottom.
- **Why:** large BP/graph results and verbose build logs are the common case.

### TBX-R8 — provider status detection has false positives {#tbx-r8}
- **Status:** ✅ Done (uncommitted, 2026-06-09) · **Effort:** S–M
- *`getTomlProviderStatus` now slices the bp-reader table and compares its
  `command` to the exe (→ 'stale' on mismatch, slash-normalized for TOML's
  escaped backslashes) instead of a bare header match; the assets check probes
  AGENTS.md + copilot-instructions + a Claude skill (all must be present). The
  JetBrains dup/dead-assignment was already resolved (single entry, distinct path).*
- `Providers.tsx:78-91`, `paths.ts:179-185`. TOML (Codex) reports "Configured" on a
  bare section-header match even when pointing at a stale exe (JSON correctly
  detects "stale"); the assets check only probes `AGENTS.md`; JetBrains has a dead
  `'missing'` assignment + two overlapping entries. Fix: compare the configured
  command against `exePath` for TOML too; check all asset targets; disambiguate the
  JetBrains paths.
- **Why:** a green "Configured" badge that points at a stale/missing exe misleads.

### TBX-R9 — setState-after-unmount, unguarded `JSON.parse`, SSE not cleaned up {#tbx-r9}
- **Status:** ✅ Done (uncommitted, 2026-06-09) · **Effort:** S
- *Install/Providers/Update gained a `mountedRef` guard on `appendLog` + an
  unmount effect that unsubscribes the `onScriptLog` listener; Tester closes the
  SSE `EventSource` + aborts in-flight requests on unmount. (The corrupt-storage
  `JSON.parse` white-screen was already covered by Batch 2's guarded
  `loadEnvOverrides`; Update/Tester parses are already try/caught.)*
- `Install/Providers/Update` (`onScriptLog` unsub not tied to a cleanup → setState
  on an unmounted component when navigating mid-op), `Settings.tsx:116-118`
  (unguarded `JSON.parse(localStorage)` white-screens the page on corrupt storage),
  `Tester.tsx:228` (no unmount cleanup for the SSE `EventSource`). Fix: tie unsub to
  effect cleanup / mounted-ref, try/catch the parse, close the EventSource on
  unmount.
- **Why:** mid-operation navigation and corrupt storage shouldn't crash or leak.

### Polish / a11y / maintainability (P3)

### TBX-P1 — accessibility gaps across the app {#tbx-p1}
- **Status:** ✅ Done (core, uncommitted, 2026-06-09) · **Effort:** M
- *Added an `ErrorBoundary` (per-page `key` remount) so a render throw no longer
  white-screens the app, and `aria-label`/`title` on the icon-only window
  controls. Residual (deferred): full `role=tablist`/arrow-key nav on the
  sidebar + badge-vocabulary stabilization — lower-value a11y polish.*
- No error boundary (`App.tsx` — any render throw white-screens the app),
  icon-only window controls with no `aria-label`/`title` (`App.tsx:29-46`), nav +
  group switchers lack `role=tablist`/`aria-current`/arrow-key nav (`Sidebar.tsx`,
  `Settings.tsx:167-176`), `StatusBadge` glyphs read literally and mean different
  things per page (`StatusBadge.tsx` — "Configured" vs "Up to date" on the same
  green). Fix: add an error boundary, accessible names, tablist semantics, and
  stabilize the badge vocabulary.
- **Why:** keyboard/SR users and crash-resilience; the badge ambiguity confuses
  everyone.

### TBX-P2 — `Tester.tsx` is a 781-line god-component {#tbx-p2}
- **Status:** ✅ Done (core, uncommitted, 2026-06-09) · **Effort:** M
- *Extracted the pure arg helpers (`resolveTemplate`/`coerceArgs`/`validateArgs`/
  `isValidJsonOfType` + their schema types) into a React-free, unit-testable
  `src/lib/tool-args.ts`. Residual (deferred): the full component split into
  `useMcpServer`/`useToolCall` hooks + `BatchPanel`/`ArgForm`/`ToolRow` files —
  a large mechanical refactor with real regression risk and no runtime test
  harness; the testable-helpers extraction captured most of the value.*
- Server lifecycle, SSE, single-call, batch, history, and three sub-components in
  one file. Fix: extract `useMcpServer`/`useToolCall` hooks, split
  `BatchPanel`/`ArgForm`/`ToolRow`, move `resolveTemplate`/`coerceArgs` to a
  unit-testable `tool-args.ts`, dedupe `makeErrorResult`/`randomPort`/clipboard
  helpers, memoize the per-render `readSettingsEnv()` (`340`).
- **Why:** the helpers become testable and the file maintainable.

### TBX-P3 — Tester UX depth: history, export, batch editing {#tbx-p3}
- **Status:** ✅ Done (core, uncommitted, 2026-06-09) · **Effort:** M
- *History now persists across sessions (localStorage, cap 50), the keep-20/show-8
  mismatch is fixed (shows all up to the cap with a count), and an Export (JSON
  download) + Clear were added. Residual (deferred): batch step reorder/insert with
  index-stable `{{N}}` refs — a larger batch-editor feature.*
- `Tester.tsx:271,289-333,428`. History cap mismatch (keeps 20, shows 8) and not
  persisted; no result/pipeline export; batch steps can only append/remove (no
  reorder/insert/re-run), and `{{N}}` refs break silently when an earlier step is
  removed. Fix: persist history, add export, support batch reorder/insert with
  index-stable refs.
- **Why:** quality-of-life for the core testing loop.

### TBX-P4 — engine-dir "select below" points at a control that doesn't exist {#tbx-p4}
- **Status:** ✅ Done (uncommitted, 2026-06-09) · **Effort:** S
- *Fixed the copy: the "missing" engine note now reads "only needed if you tick
  'Rebuild MCP server from source' below" — accurate, since the default
  precompiled install needs no engine. No phantom affordance.*
- `Install.tsx:81,150-165`. When the engine isn't detected the copy says "select it
  below," but there's no engine-dir input/Browse unless "Rebuild from source" is
  ticked. Fix: add a Browse field shown when `engineStatus==='missing'`, or fix the
  copy.
- **Why:** the UI references an affordance that isn't there.

### TBX-P5 — fetch-error states not surfaced on Update/Providers refresh {#tbx-p5}
- **Status:** ✅ Done (uncommitted, 2026-06-09) · **Effort:** S
- *Update's `refresh` now try/catches the release check and surfaces a "Couldn't
  check for updates: …" banner (network/rate-limit reason) instead of silently
  nulling the tag. (Providers' refresh is pure local-fs reads — no silent network
  failure to surface there.)*
- `Update.tsx:42-48` (a `getLatestRelease` failure silently nulls the tag and
  disables both buttons with no "why"), Providers refresh similarly. Fix: surface
  the network/rate-limit error.
- **Why:** "why can't I update?" with no message is a dead end.

### TBX-P6 — `electron/tsconfig.json` lacks `noEmitOnError`/`sourceMap` {#tbx-p6}
- **Status:** ✅ Done (uncommitted, 2026-06-09) · **Effort:** S
- *Added `noEmitOnError:true` + `sourceMap:true` and bumped `target`/`lib` to
  ES2022 (matches the runtime features used). A type error now blocks the main.js
  emit, so a broken main process can't be packaged. Full build verified.*
- A type error still emits `main.js`, so a broken main process can be packaged; no
  source maps for packaged-crash diagnosis. Fix: add `noEmitOnError:true` +
  `sourceMap:true` (consider `lib: ES2022` to match the runtime features used).
- **Why:** stops shipping a main process that didn't type-check.

### TBX-P7 — `tools.json` force-cast with no runtime drift check {#tbx-p7}
- **Status:** ✅ Done (uncommitted, 2026-06-09) · **Effort:** S
- *A light runtime guard at the cast site logs a loud `console.error` if the
  generated catalog isn't an array of `{name, input_schema}` — so a `Dump-Tools.ps1`
  schema drift fails visibly instead of silently corrupting the Tester.*
- `Tester.tsx:98` (`as unknown as ToolDescriptor[]`). The catalog is regenerated by
  `Dump-Tools.ps1`; if its schema drifts the UI breaks silently. Fix: a light
  runtime validation or a generated `.d.ts`.
- **Why:** catalog drift should fail loudly, not corrupt the Tester.

### TBX-P8 — misc correctness nits {#tbx-p8}
- **Status:** ✅ Done (uncommitted, 2026-06-09) · **Effort:** S
- *`getExePath` prefers the newer of the plugin/legacy exe by mtime (no stale
  launch); `getProjectDir` caches the dir-walk (invalidated on project re-pick);
  `resolveEngine` is debounced (350 ms) + sequence-tagged so keystrokes don't
  storm IPC or land out-of-order.*
- `getExePath` (`main.ts:83-89`) can pick a stale binary when both plugin + legacy
  exist (no mtime/version check — ties to the stale-exe class in
  [[project_mcp_exe_locked_during_session]]); `getProjectDir` (`44-70`) re-walks 6
  dir levels synchronously on every `get-paths` call (no caching); `resolveEngine`
  fires on every keystroke (`Install.tsx:100`, no debounce/sequence-guard → IPC
  storm + stale results). Fix: prefer-newer/warn on dual exe, cache the project
  dir, debounce + sequence-tag the engine resolve.
- **Why:** small correctness/perf cleanups that each cost a moment of confusion.

### Toolbox research notes (2026-06-08) — recommended approaches

A 6-cluster research pass grounded these items in current (2025–2026) best
practice + our actual catalog. **Recurring verdict: harden the lean in-house
code; don't pull a heavy dependency** into a `package.json` that deliberately has
zero runtime deps beyond react.

- **Self-update (S1/R2/R5).** Do **not** adopt `electron-updater` — it doesn't
  support the *portable* target at all (electron-builder #5378); switching to NSIS
  would forfeit the double-click-no-install UX and still ship unsigned. Two tracks
  instead: (A) **authenticity = code-signing** — a same-response SHA256 defends
  against corruption, not tampering; the real fix is an Authenticode signature
  verified against a thumbprint baked into the app. Cheapest publisher identity is
  **Azure Trusted Signing** (~$10/mo, individual sign-up GA 2025) via
  electron-builder `win.azureSignOptions` in CI. (B) **verify-before-execute** —
  `Get-AuthenticodeSignature` gate in the swap helper. **Ship now (S, ~30 min):
  refuse to self-update when `digest` is absent** — closes the "no integrity at
  all" hole immediately. Atomic `.part`+rename and keep-`.bak`-until-verified are
  hand-rolled (don't add `write-file-atomic` — it's for small config writes).
- **IPC/process security (S2–S5).** No new runtime deps (one optional unzip lib).
  S2: replace `execSync(string)` with `execFileSync('reg', [...], {shell:false})`
  + an `assoc` allowlist regex. S3: drop PowerShell `Expand-Archive -Command`
  string-building for **`extract-zip`** (already transitively present) + validate
  every script arg. S4: `path.resolve` + allowlist under the plugin dir. S5:
  `setWindowOpenHandler(deny)` + `will-navigate` guard + CSP via
  `session.webRequest.onHeadersReceived` + electron-builder signing/asar.
- **Tester arg form (F2/F3/F4).** **Extend the hand-rolled `ArgForm`; do not adopt
  rjsf** — across 260 tools only 12 array/object props are under-specified (and
  they're the high-value ones: `apply_ops.ops`, `compile_function.body/inputs`,
  `set_actor_transform.*`), so a JSON/Monaco editor for `array`/`object` + **`ajv@8`**
  client-validation beats rjsf's auto-render value prop. F4 search: delete
  `.slice(0,50)` (one line) + **`@tanstack/react-virtual`** for the 260-row list.
- **MCP client (F6/F7).** **Harden, don't adopt `@modelcontextprotocol/sdk`** — it
  pulls `zod` + unused surface (resumability/OAuth/elicitation) and still doesn't
  auto-reconnect on a 404. Add `AbortSignal.timeout(ms)` + Cancel, per-instance id
  counter, cached in-flight init promise, and 404→clear-session-and-reinit
  (~80–120 LOC, no deps).
- **Settings apply (F1/R8).** Merge the env block into **each provider's server
  entry `env` object / `[mcp_servers.bp-reader.env]` table** (where every MCP
  client actually reads it) — do not invent a central env file the server can't
  load. Persist the cleaned block to `<userData>/bpr-settings.json` via IPC (keep
  localStorage as the editing draft). Plumbs through existing provider-write code.
- **Rendering (R7) + Tester refactor (P2/P3).** **Harden `JsonViewer`/`LogStream`**
  (item-cap + string-truncation + lazy-raw-stringify + pinned-scroll/line-cap, no
  dep) — `@textea/json-viewer` needs Material-UI (multi-MB), and `react-json-view`
  is unmaintained; only `react-json-view-lite` (zero-dep) is a clean fallback if
  ever needed, but it forfeits the Tailwind theming and still renders expanded
  children. Refactor (P2) + history/export/batch-reorder (P3) are independent.

Effort revisions from the pass: TBX-S1 split (the digest-guard stopgap is **S**,
the signing track is M); TBX-S3 revised **M→S/M** (extract-zip collapses the
injection surface); the rest hold. None of the recommended fixes require adopting
a framework-scale dependency.

### Toolbox implementation plan (sequenced batches)

Ordered by severity-and-leverage; each batch is independently shippable.
**Verification bar:** the Electron app can't be run headlessly here, so each
batch is verified by `npm run build` (vite + `tsc` typecheck, with
`noEmitOnError` added in B4) plus targeted review — weaker than the C++ suite,
stated honestly. No framework-scale deps; only the small, justified ones the
research named (`extract-zip`, `ajv`, `@tanstack/react-virtual`).

- **Batch 1 — Security (TBX-S1–S5), split:**
  - **1a (ship now, no infra):** refuse self-update when `digest` absent (TBX-S1
    stopgap); `execFileSync('reg',…,{shell:false})` + `assoc` allowlist (S2);
    replace PowerShell `Expand-Archive -Command` with `extract-zip` + validate
    every script arg / `client` enum (S3); `path.resolve` allowlist under the
    plugin dir for `run-script`/`read-file`/`write-file` (S4);
    `setWindowOpenHandler(deny)` + `will-navigate` guard + `openExternal` scheme
    allowlist + CSP (S5). **Start here.**
  - **1b (needs infra, deferred):** Azure Trusted Signing in CI + Authenticode
    `Get-AuthenticodeSignature` verify-before-execute (TBX-S1 full). Tracked,
    not blocking — needs an Azure account + signing cert.
- **Batch 2 — Half-built functional (TBX-F1–F9):** Settings "Save" actually
  writes the env block into each provider's server-entry `env` (F1/R8); Tester
  array/object arg editor + `ajv` validation + un-truncate search w/
  `react-virtual` (F2/F3/F4); `.uproject` existence/extension validation (F5);
  MCP client `AbortSignal.timeout`+cancel+reconnect+per-instance id (F6/F7);
  Providers "Configure All" per-item logs/tally (F9); semver-correct `cmpVersion`
  (F8). These are what users notice most (pages that look done but aren't).
- **Batch 3 — Robustness (TBX-R1–R9):** track+kill child procs on quit (R1);
  atomic `.part`+rename downloads + backoff retries (R2); env-whitelist +
  surfaced write-mode in `start-server` (R3); scope `kill-mcp` to tracked PIDs
  (R4); keep `.bak` until verified relaunch (R5); async fs / defer temp-sweep
  (R6); JsonViewer/LogStream caps + lazy raw (R7); provider-status exe match +
  all-asset check (R8); unmount cleanup + guarded `JSON.parse` (R9).
- **Batch 4 — Polish / a11y / maintainability (TBX-P1–P8):** error boundary +
  aria + tablist/badge semantics (P1); Tester `useMcpServer`/`useToolCall`
  extraction (P2); history persistence/export/batch-reorder (P3); engine-dir
  field (P4); fetch-error surfacing (P5); `noEmitOnError`+sourceMap (P6);
  `tools.json` runtime guard (P7); misc (P8).

As each batch ships: flip the TBX `Status:` rows to `✅` with a revision-log line.

---

## 10. Reliability & data-integrity (REL-*) {#reliability}

From the 2026-06-12 deep audit (external best-practices research + inline
verification + a 25-agent fleet with an adversarial refute pass — 5 false
claims excluded). Full findings record with file:line evidence and failure
scenarios: [`reliability-plan.md`](reliability-plan.md). Severity: P0 = data
loss/corruption possible · P1 = reliability failure / unrecoverable change ·
P2 = robustness/trust · P3 = hygiene. Items marked *(fleet)* survived the
refute pass.

### Phase A — P0 closure + doctor trust
- **REL-1** (P0) config writer destroyed user config on JSON parse failure
  (`Generate-ClientConfig.ps1`) · **Status:** ✅ Done (2026-06-12) — abort that
  client's write + `.bak` + atomic temp+rename publish; live-verified.
- **REL-2** (P0) broken BPs saved without consent (`CompileAndSaveBlueprint`
  never checked `NumErrors`) · **Status:** ✅ Done (2026-06-12) — EndBatch
  refuses error saves by default (`save_skipped[]` in the ack, lifted into
  apply_ops + flips ok:false), `save_on_error`/`-SaveOnError` escape hatch,
  `BP_READER_STRICT_COMPILE=1` extends to single ops; live-verified (on-disk
  .uasset byte-identical after a refused save).
- **REL-9** (P2) doctor false-FAILed the editor-DLL check (wrong path helper)
  · **Status:** ✅ Done (2026-06-12) — uses `FindPluginDir`; live-verified.

### Phase B — write-path durability
- **REL-14** (P1, fleet) mark-dirty-but-never-save ops · **Status:** ✅ Done
  (2026-06-12) — SIX sites fixed (the fleet's five + `AddBTNode`, same gap):
  WBP/AnimBP ops route through `MaybeCompileAndSave`; BT/DataAsset ops through
  a new shared `SaveAssetPackage` helper. Live-verified across a FRESH editor
  process for the SaveAssetPackage family (`set_data_asset_property` persisted
  after restart, `Saved/verify-rel-b1.ps1`); widget/anim-family readback not
  exercisable on this host (no UMG WidgetBlueprints in its content — CommonUI
  C++-backed) — their new save call is the same `MaybeCompileAndSave` every
  long-proven op uses.
- **REL-15** (P1, fleet) component ops + ImplementInterface bypass batch
  deferral · **Status:** ✅ Done (2026-06-12) — `AddComponent`/`RemoveComponent`
  /`ReparentComponent`/`SetComponentProperty`/`ImplementInterface`/
  `BindWidgetEvent` now defer via `MaybeCompileAndSave` (clone_graph keeps its
  direct compile ON PURPOSE — its post-compile self-context rebind needs the
  skeleton refreshed mid-op). Bonus REL-2 hardening: the disconnect-mid-batch
  salvage flush now refuses compile-error saves (nobody is present to consent).
  Live-verified: `AddComponent` inside a raw BeginBatch left the disk untouched
  MID-batch (pre-fix it saved immediately), `EndBatch -Rollback` restored, and
  a fresh editor process confirmed no component leak + byte-identical .uasset.
- **REL-5** (P1) single-op live writes bypass the undo stack · **Status:**
  ✅ Done (2026-06-12) — a per-op `FScopedTransaction` opens LAZILY from
  `LoadMutableBlueprint` (the common mutator entry — write ops get exactly one
  undo entry, read ops none, no hand-maintained op list to drift) and closes
  via an `FOpUndoScope` guard on every RunOneOp return path. Live editor only
  (commandlets have no functional buffer; batch ops use H1's transaction).
  Live-verified: add_variable → `TRANSACTION UNDO` → variable gone, editor log
  shows `Undo bp-reader: AddVariable` applied. Finding: `UndoTransaction`
  silently no-ops during package save / GC — callers should retry.
- **REL-4** (P1) no pre-write .uasset backup · **Status:** ✅ Done (2026-06-12)
  — before the FIRST save of each asset per session, the on-disk `.uasset` is
  copied to `Saved/BPReaderBackups/<Package>-<UTC>.uasset` (newest 5 kept per
  asset; `BP_READER_BACKUP=0` opt-out; wired into both CompileAndSaveBlueprint
  and SaveAssetPackage). Live-verified: backup file created on first save of a
  fresh session and byte-matches the pre-mutation asset hash. Residual: the
  material/material-instance ops' inline SavePackage calls aren't covered yet.
- **REL-3** (P1) non-atomic truncate writes of user-owned files · **Status:**
  ✅ Done (2026-06-12) — Generate-ClientConfig (with REL-1), Check-Update.ps1
  cache, and Toolbox `saveProject` (mirrors its own write-file IPC pattern)
  all publish via temp+rename. Live-verified: cache valid + zero tmp residue.
- **REL-21** (P2, fleet) write_generated_source overwrites source files
  non-atomically, no backup · **Status:** ✅ Done (2026-06-12) — pre-existing
  dest is copied to `.bak`, content publishes temp+rename, response gains
  `previous_backed_up`. Live-verified: overwrite left `.bak` holding the OLD
  content and the dest holding the new, no residue.
- **REL-8** (P2) handshake/status files written non-atomically · **Status:**
  ✅ Done (2026-06-12) — all four sites (LiveServer + CmdletServer handshake +
  port-cache) publish via temp+rename. Live-verified: fresh editor handshakes
  through the new path, no `.tmp` residue.

### Phase C — transport & process lifecycle
- **REL-16** (P1, fleet) Auto backend write ops could double-execute on
  socket→commandlet fallback after dispatch · **Status:** ✅ Done (2026-06-12) —
  `SocketTransportError` now carries `requestDispatched` (true once the op frame
  is fully sent); Auto's `GuardWriteFallback` + the commandlet daemon→one-shot
  fallback refuse to re-run a WRITE method after dispatch (surface the error +
  "verify with a read" instead). Write classification is a normalized name set
  shared with the ReadOnly Reject list (unit-tested: `IsWriteMethod`).
- **REL-17** (P1, fleet) LiveServer/CmdletServer ReadFrame unbounded buffer
  growth · **Status:** ✅ Done (2026-06-12) — 10 MB per-frame cap → log +
  disconnect, in both servers. Live-verified: an ~11 MB newline-less frame
  dropped the connection, the daemon stayed alive and served a fresh client.
- **REL-6** (P1) no Job Object on spawned editors · **Status:** ✅ Done
  (2026-06-12) — every spawned editor (one-shot + daemon) is assigned to a
  process-lifetime Job Object with `KILL_ON_JOB_CLOSE`; the idle/grace/lifetime
  timers stay as fallback. Live-verified: killing the MCP server reaped its
  daemon within 10s (not the 300s idle timer).
- **REL-7** (P1) daemon shutdown is hard TerminateProcess + spawn-retry storms ·
  **Status:** ✅ Done (2026-06-12) — `shutdown_daemon`/`TerminateDaemon` now send
  a TCP `Quit` op (acked on the daemon's WORKER thread, so it works even while a
  long op holds the game thread); `TerminateProcess` only as a 5s-timeout
  fallback. Plus a spawn-failure cooldown (`BP_READER_DAEMON_SPAWN_COOLDOWN_SECONDS`,
  default 60) so a persistently-failing spawn can't boot an editor per tool call.
  Live-verified: Quit reached the handler + `RequestShutdown`; daemon exited
  near-instantly (not the 5s hammer), handshake file cleaned up, flushed
  "clean shutdown" line present.
- **REL-12** (P2) updater Stop-Process kills live MCP sessions of every project ·
  **Status:** ✅ Done (2026-06-12) — the swap now stops only servers whose exe
  path matches the target project's install dir.
- **REL-24** (P2, fleet) transport robustness batch · **Status:** ✅ Done
  (2026-06-12) — stdio loop exits on a failed stdout write (dead client pipe);
  HTTP Content-Length parsed with `from_chars` + bounds (no silent
  overflow/negative); constant-time token compare in both servers;
  non-numeric `BP_READER_*` ints warn to stderr.

### Phase D — policy & install integrity
- **REL-19** (P2, fleet) read-only-mode bypass via console_command · **Status:**
  ✅ Done (2026-06-12) — `console_command` now rejects in read-only mode
  (`BP_READER_RO_ALLOW_CONSOLE=1` escape hatch); `run_python_script` was already
  rejected. Live-verified: rejected by default, allowed with the env override.
- **REL-20** (P2, fleet) save_all persists the user's own half-done manual
  edits · **Status:** ✅ Done (2026-06-12) — `save_all` defaults to
  `scope:"touched"` (only packages this session loaded-for-write or saved, via a
  SessionTouchedPackages set fed by LoadMutableBlueprint + SaveAssetPackage);
  `scope:"all"` restores the editor-wide sweep. Live-verified: a
  python-dirtied (untouched) BP was skipped by default and saved under
  scope:all. Wired through all six backends.
- **REL-22** (P2, fleet) BPRSeed silently overwrites user assets · **Status:**
  ✅ Done (2026-06-12) — the seeder refuses to overwrite an existing asset that
  lacks the seed's `BPRSeedMarker` variable (a user asset at the seed path)
  unless `-Force`; seeds now stamp the marker so re-seeding stays idempotent.
- **REL-23** (P2, fleet) runtime console commands live in SHIPPING builds ·
  **Status:** ✅ Done (2026-06-12) — `bp_reader.list/read` registration is
  `#if !UE_BUILD_SHIPPING` (compiled out of packaged games; no BP-internals leak
  to players).
- **REL-10** (P2) robocopy /MIR purges a plain git checkout in Plugins/ ·
  **Status:** ✅ Done (2026-06-12) — Install-Plugin refuses to mirror over a
  dest that has `.git` and isn't a junction ("git pull instead").
- **REL-11** (P2) release artifacts unverified · **Status:** ✅ Done
  (2026-06-12) — release.yml emits a `SHA256SUMS` asset; Update-Plugin verifies
  the downloaded zip/exe against it before extraction (older releases without it
  skip with a log line).

### Phase E — caching correctness
- **REL-18** (P1/P2, fleet) invalidation completeness · **Status:** ✅ Done
  (2026-06-12) — `ImplementInterface` now also invalidates the interface asset
  (its referencer queries); `Activate/DeactivateGameFeature` invalidate the
  whole cache (a feature toggle mounts/unmounts many assets). (MoveAsset's
  global-list + the TTL-vs-external-Ctrl+S staleness were already covered by the
  existing global-key eviction + mtime check.)

### Phase F — small robustness + verification debt
- **REL-25** (P3, fleet) commandlet-backend polish · **Status:** ✅ Done
  (2026-06-12) — `ifstream.is_open()` checked before parse; temp-file names
  carry thread id + a process counter (no RNG-collision races); JSON parse
  failures report file size + the last 200 bytes.
- **REL-13** verification debt — every phase shipped with mock doctests +
  real-editor live verification (`Saved/verify-rel-{a,b1,b2,b3,c,c7,d}.ps1`) ·
  **Status:** ◑ ongoing discipline

---

## 11. Hardening sweep — server never-stuck / Toolbox / cleanup (2026-06-14)

A four-area ultracode audit (server hang-detection + editor/commandlet detection;
Toolbox e2e / no-overwrite / versioning / updates; test validity with real-editor
priority; conventions + Lyra-version check) drove this sweep. Lyra-on-UE-5.8 was
re-confirmed valid (EngineAssociation "5.8"; host migrated to the installed
`D:\Games\Epic Games\UE_5.8`).

### Shipped

- **HARD-1 (server, P1)** — the live/daemon op recv, TCP connect, and handshake
  reads were unbounded blocking calls, so a paused/wedged editor hung the whole
  `tools/call` forever (and wedged single-threaded dispatch + the MCP-8 task slot).
  `RecvLine` is now deadline-bounded (re-armed per frame, so progress-emitting long
  ops never trip) + cancel-aware; `ConnectOnce` is a bounded non-blocking connect;
  known-long ops get a larger floor. The general hang-detection the maintainer
  asked for. MockServer timeout/cancel doctests added. · **Status:** ✅ Done
  (287f22fc, 2026-06-14)
- **HARD-2 (daemon, P1)** — the cmdlet wedge-watchdog force-killed the editor on a
  legit >120s game-thread op (heartbeat only bumped by the blocked main pump). It
  now tracks ops in-flight and applies the tight timeout only when idle; the live
  server's flat `DoneEvent->Wait()` became a shutdown-responsive poll with a
  heap-owned result. Compiled against UE 5.8. · **Status:** ✅ Done (37d61b9c)
- **HARD-3 (Toolbox, P1)** — plugin update could hang forever (robocopy 1M-retry on
  a locked exe); now `/R:2 /W:2` + a path-scoped pre-kill of this project's server.
  A release fails fast unless tag == .uplugin VersionName == package.json. An update
  now prompts to restart the MCP client. Tester mock auto-spawn opens SSE; port
  range widened; `$tag` log-prefix un-clobbered. · **Status:** ✅ Done (e59a1aeb)
- **HARD-4 (tests/docs/conventions)** — doctor git-HEAD nag → Info; tagged comment
  blocks collapsed (TaskManager.h / CommandletErrorParse.h / Mcp.cpp); tautological
  analytics tests trimmed; dead code removed (SecondsFromEnv, SpawnLock::IsHeld,
  StripClassPrefix, AudioBase64/ResourceLink, unused includes) + C4100 fixes;
  GuidTag8 de-dup; CLAUDE.md build/test commands repointed at the real
  installed-engine host (the documented live-test path was absent → the live suite
  silently auto-skipped); a gated live write-roundtrip test now covers the real
  apply_ops save path. · **Status:** ✅ Done (0542ba65 / 07b7aa43 / 666324c6 /
  cd869451 / 75f1cd62)

### Deferred (confirmed-safe, fully specified)

- **HARD-D1 (cleanup)** — **de-dups shipped** across HARD-4 and follow-up passes
  (3c285eb6 `ParseAssetRegistryRows`→shared `CommandletResultParse.h`; 6187d5ab
  `env::IsTruthy` unifies the 5 truthiness sites + fixes the `ParseResponseControls`
  `!= "0"` inconsistency + `AssetPathProperty()` for 8 asset_path literals; 42994084
  merged the duplicate `is_object` branch in `executeAndWrap` +
  `find_node`→`PaginatedSchema()`; 2f4cb2db shared `InFlightGuard` for the two
  Mcp.cpp in-flight RAII structs + `Emitter::UnwrapLit` for the 3 CppEmit
  literal-unwrap lambdas; 58d62149 extracted `ToPackagePath`→
  `BlueprintReaderPathUtils.h` (editor cross-TU, replacing two anon-namespace copies)
  + dropped 7 duplicate editor-TU `#include`s). **Dropped as NOT safe** (on
  inspection, not the clean de-dups the audit assumed): "move 5 result-struct
  parsers" — backends differ (Socket's CloneGraph has an `is_object()` guard the
  commandlet lacks); the `-Op=` label-strip — `std::string` vs `std::wstring` across
  backends; the `toPackage` lambda — only a subset of `NormalizeAssetPath` (no
  trim/backslash), so swapping changes did-you-mean behavior; `WriteGeneratedSource`
  temp-marshalling RAII — string/wstring arg divergence + RAII cleanup would change
  the throw-path temp-leak behavior. **Remaining (single, marginal, deferred):**
  `EmitCallStatement` extraction (codegen surface — output is doctest-pinned, low
  value). · **Status:** ✅ Done (de-dups through 58d62149; one marginal codegen
  extraction deferred)
- **HARD-D2 (test, M)** — **ROOT-CAUSED + made routine.** The "daemon never
  handshakes for Lyra even at 600s" was the ONE-TIME cold shader/DDC compile on the
  first 5.8 open of 5.6 content, not a handshake bug: measured warm = handshake 15s,
  MCP-server attach <1s, full gated `[live]` doctest pass 1s. Fix is operational —
  keep one warm daemon and ATTACH instead of auto-spawning per process, via
  `Scripts/Run-LiveTests.ps1` (85217f9e). · **Status:** ✅ Done (85217f9e, 2026-06-14)
- **HARD-D3 (editor, P2)** — `DeleteAsset` inside a live daemon session purged the
  `.uasset` and returned `deleted=true`, yet a same-session recreate to that path
  still reported `already_existed=true`: the in-memory `UBlueprint` (+ its
  `UPackage`/`FLinkerLoad`) survived, so the create/duplicate idempotency probe
  (`LoadObject`) resurrected the corpse from the lingering linker even though the
  file was gone from disk. Root cause: `GetAssetByObjectPath()` on a package-only
  path returns a degenerate `FAssetData` (`GetAsset()` null, `AssetName` = the full
  path), so `ForceDeleteObjects`/manual eviction never ran (`IsValid(Obj)` false)
  and the re-probe built a malformed object path that never matched (a false
  `deleted=true`). Fix (`RunDeleteAssetOp`): derive the canonical object path from
  `PackageName` (leaf repeated); fall back to `StaticFindObject` when `GetAsset()`
  is null so a resident corpse is still found+evicted; path-independent package
  eviction (`ResetLoaders` + rename-aside + `MarkAsGarbage`, for both the
  ForceDeleteObjects>0 and headless manual paths) so `LoadObject` can't
  re-materialize the export; notify `AssetRegistry` while the object is still valid.
  Live-verified on a warm `-nullrhi` daemon (`Saved/verify-hardd3.ps1`):
  dup→del→dup→del = `already_existed` false×2, `deleted` true×2, file absent on disk
  after each delete. · **Status:** ✅ Done (58d62149, 2026-06-14)
- **HARD-D4 (editor, P2)** — create-type tools can **hard-crash the editor** on a
  cross-class path collision. If an object of a different class already exists at
  the target path, the create proceeds into UE's `StaticAllocateObject`, which
  `appError`s (`UObjectGlobals.cpp` "Cannot replace existing object of a different
  class") — a fatal that kills the whole daemon, not a recoverable tool error.
  Reproduced live: `create_material` then `create_material_instance` at the same
  path fatals (surfaced by the §11 every-tool smoke, which used to share one path).
  The smoke harness was fixed to isolate each tool (198c2f22). Fix: a shared
  `DestClassConflict` guard runs after each create op's typed idempotency probe —
  if a resident OR on-disk object of an incompatible class occupies the canonical
  destination path, it writes a descriptive error body and returns op code 6
  (`DestClassConflict`), so the backend's existing non-zero-code path
  (`Socket::RunOp`/`RunOpOneShot`) throws a clean `BlueprintReaderError` instead
  of the create method parsing the body as a misleading success. Wired into
  `create_blueprint`, `create_material`, `create_material_instance`,
  `create_data_asset`, `duplicate_blueprint` (niagara create is a no-op stub).
  Live-verified (`Saved/verify-hardd4.ps1`): a create over a resident Blueprint
  returns `isError` "DestClassConflict (code=6): a Blueprint already exists
  (loaded in the editor) at …" and the daemon SURVIVES; a fresh path still
  creates (no false positive). · **Status:** ✅ Done (89e048c3, 2026-06-14)

---

## Revision log

Newest first. One line per change to this file.

- **2026-06-14** — **HARD-D4 ✅ Done.** Create/duplicate ops now refuse a cross-class
  destination collision (shared `DestClassConflict` guard → op code 6 → backend
  throws a clean error) instead of letting UE's `StaticAllocateObject` fatally
  crash the editor. Wired into the 5 asset-creating ops + error code 6 in the
  backend table. Live-verified: conflict → `isError` + daemon survives; fresh path
  still creates. Mock 893/0. (89e048c3)

- **2026-06-14** — **§11 verified by a live smoke; new HARD-D4 found.** Ran the full
  gated `[live]` doctests + every-tool sweep on a warm Lyra daemon: HARD-1 (bounded
  recv, incl. crash-recovery respawn), HARD-2 (no false watchdog kill — daemon
  survived 236-tool sweep), HARD-D2 (warm attach), HARD-D3 (delete eviction), and
  the doctor's REL-9 DLL check all green. The sweep surfaced HARD-D4 (create tools
  `appError` on cross-class path collision); fixed the smoke harness to isolate
  per-tool paths (198c2f22) and logged the tool gap. Mock 893/0.

- **2026-06-14** — **HARD-D3 ✅ Done; HARD-D1 ✅ Done (§11 hardening complete).**
  `delete_asset` same-session residue root-caused: a package-only
  `GetAssetByObjectPath` returns a degenerate `FAssetData` (`GetAsset()` null,
  `AssetName` = full path), so the resident in-memory corpse was never evicted and
  `LoadObject` resurrected it on recreate. Fixed with a canonical object path +
  `StaticFindObject` fallback + path-independent package eviction
  (`ResetLoaders`+rename+garbage). Live-verified dup→del×2 (already_existed false×2,
  deleted true×2, file absent). Folded in the last HARD-D1 editor cleanup
  (`ToPackagePath`→`BlueprintReaderPathUtils.h`, −7 dup includes); the in-flight RAII
  + CppEmit helpers had landed in 2f4cb2db. Mock 893/0. (58d62149)
- **2026-06-14** — **Daemon-handshake root-caused (HARD-D2 ✅ Done).** Measured: the
  "never handshakes for Lyra" was the one-time cold shader compile, not a bug (warm
  = 15s handshake / <1s attach / 1s live pass). Added `Scripts/Run-LiveTests.ps1`
  (pre-warm + attach) so live tests are routine. New HARD-D3: `DeleteAsset` doesn't
  evict from the in-session registry. (85217f9e)
- **2026-06-14** — **Hardening sweep (§11).** Four-area ultracode audit →
  HARD-1..4 shipped (server bounded-recv hang-detection 287f22fc; daemon
  watchdog/live-shutdown 37d61b9c; Toolbox hang/version/restart e59a1aeb;
  conventions/doctor/cleanup/docs/live-test 0542ba65/07b7aa43/666324c6/cd869451/
  75f1cd62). HARD-D1 (14 remaining de-dups) + HARD-D2 (live-run blocked by daemon
  flakiness) deferred. Mock 891/0; UE 5.8 editor compile clean; real-data
  read_blueprint smoke passed.

- **2026-06-13** — **Ultracode completeness audit → 10 gaps found + ALL fixed.** A
  7-dimension adversarial audit (roadmap / recipe / test-coverage / docs-sync /
  build-invariants / 2× correctness) of the recently-shipped work, each finding
  independently verified (1 refuted). Fixes across 3 commits: **docs-sync**
  (b0c02373) — regenerated stale `tools.json` (health_check daemon/plugin fields;
  the staleness was breaking CI's drift gate), made serverInfo count-agnostic,
  fixed a `267` comment + ~6 wiki counts. **MCP-8 concurrency** (dd441c64) — 3
  REAL bugs the mock tests missed: shutdown use-after-free (detached workers →
  now join deadlock-safely + Server member reorder), tasks/cancel registration
  TOCTOU + contradictory cancelled-with-success status (markReady latch + status
  reconciliation), and a ToolRegistry data-race on the task path (registry-
  mutators refused task mode). **Test-coverage + reaper** (259a3d74) — engine-
  free CI guards for add_widget index (schema + socket arg-frame) and the
  NodeRefError-through-apply_ops parse (extracted CommandletErrorParse.h helper +
  unit test), plus an MCP-8 ttl reaper. Mock 891/891. The audit's value: it
  caught 2 genuine concurrency bugs in MCP-8 + a CI-breaking catalog drift that
  manual review had missed.
- **2026-06-13** — **UX-P5 e1 follow-ups SHIPPED.** (1) `health_check` reports
  `daemon_enabled` + `disabled_plugins` (server config pre-flight). (2) Fixed a
  real bug: the commandlet backend discarded an op's structured error file on a
  non-zero exit, collapsing a detailed `NodeRefError` (did-you-mean GUIDs) to a
  bare "exit=4" through `apply_ops`; now it reads the output file's `error` first
  (the live backend already did). Live-verified (`Saved/verify-e1-polish.ps1`).
  Mock 884/884; health_check first-40 preserved → no hash drift.
- **2026-06-13** — **MCP-8 SHIPPED: async Tasks primitive (full impl).** A
  `tools/call` with a `task` augmentation runs on a background thread + returns a
  taskId immediately; `tasks/get`/`tasks/cancel`/`tasks/list` + `capabilities.tasks`
  + `execution.taskSupport` on the long-running tools. Single-task model (the
  editor backend is exclusive → busy-rejects concurrent calls, keeping the read
  loop responsive); cancel reuses CallContext + the in-flight registry. New
  `tools/TaskManager.h` (detach-safe). Mock 884/884 (`test_tasks.cpp`);
  live-verified end-to-end (`Saved/verify-tasks-live.ps1`: task-augmented
  list_blueprints polled to completion with the real result). Maintainer chose
  full-now over wait-for-GA; wire shape tracks the experimental 2025-11-25 spec
  (may need a small GA update). No tool/hash change (adds methods, not tools).
- **2026-06-13** — **UX-P4i b SHIPPED: `add_widget` insert-at-index.** Optional
  0-based `index` inserts the new widget at that child position of the parent
  panel (AddChild → ShiftChild, clamped); `-1`/omitted appends (back-compatible).
  Response reports final `child_index`. Full backend chain (interface signature +
  5 overrides; Mock keeps the throwing default) + plugin op (ShiftChild) +
  registration; description first-40 preserved → no hash drift. Live-verified on
  a real render editor (`Saved/verify-addwidget-index.ps1`): appends 0/1/2,
  index=1→1, index=0→0, index=99→5 (clamp). Mock 879/879. Gotcha: the run_python
  setup needs BP_READER_ALLOW_PYTHON on the EDITOR (gate is editor-side) — the
  render editor must be launched with it inherited.
- **2026-06-13** — **TEST-1 render sweep CODIFIED: `BP_READER_SMOKE_RENDER` live
  smoke — TEST-1 COMPLETE.** `test_tool_smoke_render_live.cpp` drives the
  render/interactive surface against a real `-RenderOffscreen` editor and
  certifies the render tier (take_screenshot captured=true, world_pos_to_screen
  valid=true, get_camera_transform valid=true) + reachability of the show-flag/
  view-mode/selection/camera tools. 12 dispatched, 0 broken
  (`Saved/run-smoke-render.ps1`). Auto-skips with no editor. Found a test-side
  field-name bug (cam uses `valid` not `ok`), no tool bug. Mock 879/879 (skipped
  there). PIE stays honest-headless; build/cook registration-only.
- **2026-06-13** — **TEST-2 P2 SHIPPED: `BP_READER_SMOKE_UI` live smoke — TEST-2
  COMPLETE.** A gated doctest (`test_tool_smoke_ui_live.cpp`) drives the whole
  editor-UI tool surface against a real render editor via the live-socket stack
  (reads the `bp-reader-live.json` handshake; auto-skips with no editor/env).
  Live-verified (`Saved/run-smoke-ui.ps1`): 8 UI tools dispatched, 0 broken,
  `ui_available=true`. AutomationDriver sessions were conditional and aren't
  needed (P1b game-thread Slate injection suffices) → TEST-2 done. Mock 879/879
  (skipped there); no tool/hash change (adds a test, not a tool).
- **2026-06-13** — **UX-P5 e1 SHIPPED: `health_check.write_enabled`.** Surfaces
  server-side write-gating pre-flight so a client sees read-only mode without
  attempting a write. Implemented as `IBlueprintReader::WritesEnabled()` (default
  `true`; `ReadOnlyBlueprintReader` — the outermost decorator — overrides `false`),
  read at the health_check handler (backend-independent, no editor needed). Two
  touch points; description first-40 preserved → no hash drift. Mock 879/879
  (`ReadOnly(Mock)`→false, `Mock`→true); live-verified through the shipping exe
  (RO=false, allow-write=true). Catalog 268.
- **2026-06-12** — **TEST-2 P1b slice 4: `ui_invoke_menu` SHIPPED — P1b COMPLETE**
  (268 tools). Execute an editor menu command by registered UToolMenus name +
  entry — the most geometry-independent driving primitive. GenerateMenu seeded
  with the level editor's GLOBAL command list, walk Sections→Blocks (match by
  command name exact-CI or label substring), execute via the **public**
  `FToolMenuEntry::GetActionForCommand` (CanExecute→Execute the `FUIAction`) with
  a `TryExecuteToolUIAction` fallback — `Action`/`Command`/`ConvertUIAction` are
  private, so scope is honestly command-bound (File/Edit/Build/Select/Play) +
  FToolUIAction entries; module-specific (Fab/Bridge) / non-command entries get a
  clear ok:false → ui_click, never a phantom success. Gated BP_READER_ALLOW_UI=1.
  Full backend chain + categories/annotations (action). Description first-40
  preserved → protocol hash unchanged. **Live-verified through the full MCP
  stack** (Saved/verify-ui-invoke-menu.ps1): Select/SelectNone then SelectAll
  each ok+invoked=true with get_selected_actors observing selection change
  **0→21→0**; Window/OpenFabTab → ok:false → ui_click. Mock 878/878. The
  Selenium-style driver now covers click + type + focus + invoke.
- **2026-06-12** — **REL Phases C+D+E+F SHIPPED — §10 reliability plan COMPLETE
  (REL-1..25 all done).** C (transport/process): REL-16 no-double-execute write
  fallback (SocketTransportError.requestDispatched + IsWriteMethod guard, unit-
  tested), REL-17 10MB frame cap (both servers), REL-6 Job Object
  KILL_ON_JOB_CLOSE on every spawned editor, REL-7 graceful TCP-Quit daemon
  shutdown + spawn cooldown, REL-12 project-scoped updater Stop-Process, REL-24
  transport robustness (stdout-fail exit, Content-Length from_chars bounds,
  constant-time token compare, env-parse warnings). D (policy/install): REL-19
  read-only console gate, REL-20 save_all scope:"touched" default, REL-22 seed
  collision marker guard, REL-23 !UE_BUILD_SHIPPING runtime-console gate, REL-10
  git-checkout /MIR guard, REL-11 SHA256SUMS emit+verify. E: REL-18 interface +
  game-feature cache invalidation. F: REL-25 commandlet-backend polish.
  Live-verified: C via verify-rel-c.ps1 (frame cap, job-object reap) +
  verify-rel-c7.ps1 (graceful quit: handshake deleted + flushed clean-shutdown
  line); D via verify-rel-d.ps1. Mock 877/877.
- **2026-06-12** — **REL Phase B slice 3 SHIPPED: REL-3 + REL-21 + REL-8 —
  Phase B complete.** Every user-owned/status file now publishes atomically
  (temp+rename): Check-Update cache, Toolbox project.json, all four
  handshake/port-cache sites; write_generated_source additionally `.bak`s any
  pre-existing source file and reports `previous_backed_up`. Live-verified
  (Saved/verify-rel-b3.ps1): fresh-editor handshake through the new path with
  zero residue; generated-source overwrite left .bak=old + dest=new; update
  cache valid JSON. Toolbox typechecks.
- **2026-06-12** — **REL Phase B slice 2 SHIPPED: REL-4 + REL-5.** Pre-write
  .uasset backup ring (Saved/BPReaderBackups, first save per asset per session,
  newest 5 kept, BP_READER_BACKUP=0 opt-out; wired into CompileAndSaveBlueprint
  + SaveAssetPackage) and per-op undo transactions (lazy FScopedTransaction from
  LoadMutableBlueprint, closed by a RunOneOp scope guard; live editor only).
  Live-verified (Saved/verify-rel-b2.ps1): backup byte-matches the pre-mutation
  asset; add_variable + TRANSACTION UNDO → variable gone with the applied-undo
  line in the editor log. Engine finding: UndoTransaction silently no-ops
  during save/GC — verify retries until the observable flips.
- **2026-06-12** — **REL Phase B slice 1 SHIPPED: REL-14 + REL-15.** Six
  mark-dirty-but-never-save ops now persist (WBP/AnimBP via MaybeCompileAndSave;
  BT/DataAsset via new SaveAssetPackage helper — incl. AddBTNode, a sixth site
  the fleet's list missed); component ops + ImplementInterface + BindWidgetEvent
  now DEFER inside batches (mid-batch disk writes broke H1 rollback's
  nothing-on-disk invariant; clone_graph deliberately keeps its direct compile
  for the skeleton-dependent rebind); the disconnect-mid-batch salvage flush
  refuses compile-error saves. Live-verified (Saved/verify-rel-b1.ps1, fresh
  editor-process read-backs): data-asset property persisted after restart;
  AddComponent in a raw batch left disk untouched mid-batch, rollback left no
  trace. Widget-family readback skipped on this host (no UMG WBPs in content).
- **2026-06-12** — **NEW §10 Reliability (REL-1..25) + Phase A SHIPPED.** Deep
  audit (best-practices research + inline verification + 25-agent fleet with
  adversarial refute pass; findings record in reliability-plan.md). Phase A:
  REL-1 (config writer no longer destroys user config on parse failure — abort
  + .bak + atomic publish), REL-2 (EndBatch refuses to save BPs whose final
  compile has ERRORS — save_skipped[] reporting, apply_ops save_on_error escape,
  BP_READER_STRICT_COMPILE for single ops), REL-9 (doctor DLL check uses
  FindPluginDir — false FAIL gone). Live-verified via Saved/verify-rel-a.ps1
  (corrupt config untouched; refused save left the .uasset byte-identical;
  save_on_error persisted; doctor [ OK ]). Mock 875/875.
- **2026-06-12** — **TEST-2 P1b slice 3: `ui_focus_tab` SHIPPED** (267 tools).
  Foreground an editor dock tab by a label substring — geometry-independent (no
  click, no painted geometry): collect every SDockTab, match, ActivateInParent.
  Reports is_foreground + active_tab (GetActiveTab readback); no-match lists
  available_tabs. Gated BP_READER_ALLOW_UI=1. Full backend chain +
  categories/annotations. Live-verified through the full MCP server stack
  (discovered 5 tabs; Viewport 1 / Outliner / Details each foregrounded, active_tab
  matched). Mock 873/873, hash rebaselined (0x9DA75072AF669656), catalog regen.
  Next P1b: ui_invoke_menu (UToolMenus) + BP_READER_SMOKE_UI Track B smoke.
- **2026-06-12** — **TEST-2 P1b slice 2: `ui_type` SHIPPED** (266 tools). Type text
  into an editor widget by its ui_list_widgets path — focus the type-revalidated
  target, inject one OnKeyChar per char. ui_list_widgets now reads editable-text
  content for an observable round-trip; gated RaiseTestUiWindow test hook raises a
  non-modal SEditableTextBox to type into (default editor view has none). Gated
  BP_READER_ALLOW_UI=1. Full backend chain + categories/annotations. Live-verified
  on the render tier at the raw-op layer AND through the full MCP server stack
  (call_tool ui_type → typed → marker read back). Mock 872/872, hash rebaselined
  (0x5ABBBE5BA4BFC9FE), catalog regen. Next P1b: ui_focus / ui_invoke_menu.
- **2026-06-11** — **TEST-2 P1b slice 1: `ui_click` SHIPPED** (265 tools). Click an
  editor widget by its ui_list_widgets path — path resolver (window→child walk,
  type-revalidated), degenerate-geometry guard (refuse w/h=0), synthetic mouse
  down+up at the geometry center via FSlateApplication::OnMouseDown/OnMouseUp.
  Gated BP_READER_ALLOW_UI=1. END-TO-END VERIFIED on the render tier: clicking a
  toolbar combo opened its dropdown (menu widgets 18→61). Full backend chain +
  action annotation (passes through read-only like console_command). Hash
  rebaselined; mock 871/871. The original "Selenium for the editor" capability,
  working. Next: ui_type / ui_focus / ui_invoke_menu + the Track B UI smoke.
- **2026-06-11** — **TEST-1 Track B render tier live + render-tool fixes**: using
  the P1a `-RenderOffscreen` harness, exercised the render/interactive tools that
  the `-nullrhi` daemon could only registration-check, and fixed two real
  rendering-only bugs: `take_screenshot` ignored `dest_path` (HighResShot needs a
  NAMED `filename=`, not a positional path → silently wrote to the engine
  default), and `take_viewport_screenshot` used the game-only `Shot` command
  (no-op in the editor) → both rerouted through HighResShot `filename=` + forced
  redraw; verified pixels land at `dest_path`. Found the offscreen render-on-
  demand + global-HighResShot-config limitation (only one capture per
  interaction). camera/show-flag verified. Also: v0.9.0 released (version-stamp
  fix → exe reports its real version, confirmed `0.9.0+853c5e7a` in CI).
- **2026-06-11** — **TEST-2 P1a SHIPPED + render tier proven**: the modal side-
  channel (worker-thread `modal` frame → game-thread drainer via the heartbeat
  ticker AND the OnModalLoopTickEvent delegate; report/dismiss; shared
  `BuildActiveModalReport`) + the `BP_READER_GUI_AUTOMATION` prevention gate +
  a gated `RaiseTestModal` hook. Editor-module only (264 tools unchanged).
  Keystone: a full editor `-RenderOffscreen -unattended` runs on this box with
  real D3D12 RHI + Slate (the TEST-1 Track B render tier), which both verified
  the P1a modal-recovery drill (normal op WEDGED while the side-channel reported
  + dismissed the modal) AND retro-verified P0's populated-tree paths against
  the real editor widget tree. Headless daemon regression PASS. Finding:
  FTSTicker is NOT pumped in AddModalWindow's loop (heartbeat goes stale), so
  health_check can't distinguish a modal from any game-thread stall.
- **2026-06-11** — **TEST-2 P0 SHIPPED**: `ui_list_widgets` (264 tools — new
  editor-category read tool walking the live Slate tree) + `get_modal_state`
  enriched with `buttons[]`/`buttons_truncated`. Plugin op + full backend chain
  (6 overrides) + categories/annotations. No hash drift (description changes are
  beyond the 40-char hashed prefix; output-schema not hashed). Live-verified
  against a real `-nullrhi` daemon both directly over TCP and through the full
  `call_tool`→Socket MCP stack. An 8-finding / 3-lens adversarial review landed
  pre-commit: emit-vs-visit budget split (game-thread cost guard for `type`
  filters), depth-cutoff `truncated` flag, `ui_available` headless bool,
  nested-button label boundary, `buttons_truncated`, per-window `truncated`,
  and response-local-path / global-budget doc clarifications. P1a/P1b/P2 remain
  Open.
- **2026-06-09** — **TEST-2 added** (editor UI automation, Selenium-style):
  3-lens research (engine source/ecosystem/integration) found Epic's in-engine
  `AutomationDriver` ships complete in installed 5.8 and is linkable; modal
  wedge root-caused (`AddModalWindow` never ticks FTSTicker) with an
  `OnModalLoopTickEvent` cure; phased P0–P2 plan (widget tree → modal
  unblocker → gated ui_click/type/menu → driver sessions). Full findings in
  live-gui-testing.md. Also: v0.8.0 released (health_check + EDIT-5 tools).
- **2026-06-09** — **EDIT-5 SHIPPED**: `describe_k2node` (transient-sandbox K2
  node introspection, plugin-side + full backend chain) + `generate_k2node_
  skeleton` (pure server-side .h/.cpp codegen, new K2NodeSkeletonEmit) — 261→263
  tools, hash rebaselined. 2-lens review caught a CRITICAL spawn-order bug
  (Allocate must precede PostPlaced — verified against the engine's
  BlueprintNodeSpawner.cpp), string-literal injection, and the AnimGraph-node
  crash family; all fixed + regression-tested. Mock 869/0; live-verified incl.
  SpawnActorFromClass (the pre-fix crasher) + AnimGraph rejection.
- **2026-06-09** — **UX-P4a SHIPPED** (post-v0.7.0): on-demand health channel —
  game-thread heartbeat (FTSTicker + per-op bump) + worker-thread `health` frame
  in BOTH the LiveServer and CmdletServer + `HealthCheck()` through the full
  backend chain + a new `health_check` tool (260→261, hash rebaselined). A 4-lens
  adversarial review killed the op-path auto-abort variant (false-stall on long
  silent live ops; busy≈paused to a heartbeat) — the probe is on-demand only.
  Mock 863/0; editor module compiles on UE 5.8; live-verified (raw frame +
  full tool through the live backend, `Saved/health-smoke.ps1`). Bounded-write
  `recompile:"pending"` ack deferred (needs heap-state AsyncTask detach).
- **2026-06-08** — **Toolbox Batch 2 SHIPPED** (uncommitted): TBX-F1/F2/F3/F4/F5/
  F8/F9 done; F6/F7 (MCP-client robustness) split out to **Batch 2b**. F1 = Settings
  Save actually applies (shared store + `serverType`/`baseEnv` refactor + all JSON
  providers through the merge with env injected). F2/F3 = Tester array/object JSON
  args + schema validation (single + batch). F4 = un-truncated search. F5 =
  `.uproject` validation. F8 = semver `cmpVersion`. F9 = Configure All per-provider
  logs. Renderer + electron `tsc` clean. **2-lens review caught + fixed: a HIGH
  cross-batch regression (F5's existence-via-`read-file` hit the Batch-1a allowlist
  → dedicated `uproject-exists` IPC), F3 not covering the batch path, and F1 losing
  the `BP_READER_PROJECT` pin.** Review confirmed all 8 JSON provider config shapes
  match the prior script output + F8 precedence correct.
- **2026-06-08** — **Toolbox Batch 1a SHIPPED** (uncommitted): TBX-S2/S3/S4/S5
  done + TBX-S1 1a (digest-guard). `electron/main.ts`: execFileSync(reg)+assoc
  regex, env-var Expand-Archive + client allowlist, realpath-aware path allowlists
  for run-script/read-file/write-file, nav/window lockdown + http(s) openExternal
  + prod CSP, refuse-no-digest self-update. `tsc -p electron` clean. **Adversarial
  review (2-lens workflow) caught + fixed: a critical regression (ALLOWED_CLIENTS
  casing broke per-client install), a symlink/junction bypass (→ realpath), and a
  file:// "Open config file" regression (→ shell.openPath).** 1b (Authenticode
  signing) deferred (needs cert/CI). Verification bar: typecheck + review (the
  Electron app can't run headlessly here).
- **2026-06-08** — Added the Toolbox **implementation plan** (sequenced batches
  1a/1b/2/3/4) to §9; starting Batch 1a (security hardening).
- **2026-06-08** — **Server-side gap CLOSED + catalog regenerated.** After the
  session's MCP server disconnected (unlocking the exe), rebuilt
  `BlueprintReaderMcp.exe` (0.1.0+ae135040) and: (1) regenerated the tool catalog
  via `Dump-Tools.ps1` — `-Check` now passes (my earlier hand-sync of
  `tools.json` was 1 line off; now canonical); (2) drove the server-side fixes
  through the REAL server → commandlet daemon on real data (`Saved/verify-server-
  side.ps1`, 5/5): P4e single payload (`structuredContent` full + short text
  note), P4g diff tools hidden by default (40) then discoverable after
  `enable_tool_category(read)`, P4i-c `list_blueprints` find_asset hint, and A's
  apply_ops progress reaching 3/3. These were mock-only before.
- **2026-06-08** — Research: how to test the render/interactive surface against a
  real **active** editor → [`live-gui-testing.md`](live-gui-testing.md) (new TEST-1
  item). Key findings: the GUI "wedging modal" was a missing `-unattended` flag
  (`FMessageDialog` auto-defaults under it); `-AllowCommandletRendering` flips
  `CanEverRender()` true headless. **Fidelity caveat (per maintainer):** tests
  must match how tools respond when a person is *actively* using the editor — so
  viewport/selection/camera/PIE/open-editor tools need a real GUI editor (Track B,
  `live` backend, map loaded + active viewport), NOT just a render-capable
  commandlet (Track A, which lacks an active viewport/selection and is only a
  partial measure).
- **2026-06-08** — **UX-P4c FIXED for real (reload-from-disk).** Replaced the
  headless-broken `CancelTransaction` rollback with reloading each touched
  package from disk on `-Rollback` (`UPackageTools::ReloadPackages`,
  `AssumePositive`) — valid because the batch defers all saves, so disk holds the
  pre-batch state. `EndBatch` returns `rolled_back:N`. **Live-verified 5/5**
  (`Saved/uxp4-rollback.ps1`): a batch of 2 vars + a node rolled back to the exact
  pre-batch var set + graph node count, `save_all` couldn't resurrect it, and a
  commit batch still persists. Editor module rebuilt exit 0; test exe unchanged
  (862/0). UX-P4c now ✅.
- **2026-06-08** — **CORRECTION: UX-P4c rollback is NOT live-verified.** A
  re-run against the final build (per request) exposed that the earlier "P4c
  rollback live-verified" claim was a false pass (test read a non-existent
  `.json.variables` field). Corrected testing proved `EndBatch -Rollback` /
  `CancelTransaction` is a no-op in the `-nullrhi` headless daemon — a batched
  `AddVariable` survived in memory and `save_all` persisted it. P4c's default-
  selection is mock-verified and correct; the underlying H1 rollback MECHANISM is
  broken headless and needs a real fix (see UX-P4c). The OTHER live claims hold:
  P4h/P4d/P4i-a/P4b-honesty used direct error-string/exit-code/field checks
  (re-confirmed PASS on the final build), not the buggy field access.
- **2026-06-08** — Post-review fixes (5-agent review of the UX-P4 diff). Real
  findings addressed: PARITY-1 `EmitProgress` off-by-one (#259 comment — numeric
  progress was 0-based `i`, never reached `total`; now `i+1`, test strengthened);
  `bind_widget_event` returns `ok:false` when compile/save fails (not durable);
  `bp_structural_diff` added to the `assets` category for parity with
  `diff_asset`/`prepare_merge`; `RunOps` "unknown on_failure" error string dropped
  the now-misleading `(default)`; `PayloadOf` test helper no longer throws on a
  non-JSON error envelope. Dismissed as false-positive/intentional: the "const
  pointer compile error" (it's `const T*`, reassignable — builds clean), the
  `structuredContent` text-only tradeoff (UX-P4e, maintainer-chosen), and the
  `BatchGuard` edge path (unreachable — the mid-batch catch applies rollback +
  releases the guard). Multi-engine: the new UMG bind APIs are UE4-era stable +
  use the project-wide FProperty system, so no version guard. Mock 862/0.
- **2026-06-08** — Toolbox research pass: added [Toolbox research notes](#toolbox-research-notes-2026-06-08)
  to §9 (6 clusters — self-update/signing, IPC security, Tester arg-form, MCP
  client, Settings apply, rendering). Recurring verdict: harden the lean in-house
  code, don't adopt framework-scale deps; concrete libs where worth it (Azure
  Trusted Signing, extract-zip, ajv, @tanstack/react-virtual). TBX-S1 split (digest
  stopgap = S); TBX-S3 M→S/M. See [[project_toolbox_audit]].
- **2026-06-08** — **Implemented UX-P4b/c/d/e/g/h/i(a,c) (uncommitted).** Server-side
  (mock 862/0): P4c (atomic⇒rollback default in `ApplyOps.cpp` + tests),
  P4e (single payload via `structuredContent`, short text note, in `Mcp.cpp` +
  `PayloadOf` test helper), P4g (3 diff tools added to read/assets categories),
  P4i-c (`list_blueprints` find_asset `_hint`), P4b handler honesty (`ok`=`bound`
  + `reason` field through the chain). Plugin-side (editor module rebuilt vs
  installed UE 5.8; **live-verified** via `Saved/uxp4-live.ps1` + `uxp4-p4b*.ps1`):
  P4b real `K2Node_ComponentBoundEvent`, P4d prefix refs + named errors in 4 ops,
  P4h param/local guidance, P4i-a `save_all` `/Script` filter + `nothing_to_save`.
  P4f closed as already-fixed (#210, stale-build report). Docs: bp-batches SKILL +
  AGENTS atomicity; `docs/tools.json` on_failure description hand-synced (server
  exe locked this session, can't run Dump-Tools — **regen the catalog on the next
  clean server build**). Deferred: P4a (paused-editor health), P4d server pre-flight
  (P4c rollback covers it), P4b PART-B node kinds, P4i-b add_widget index. Build
  fix: stale UBT cache referenced a removed `PreBuild-1.bat` → dropped a no-op
  there (gitignored intermediate). See [[project_client_feedback_2026_06_07]].
- **2026-06-08** — Added **Section 9 (Toolbox)**: 31 items from the 2026-06-08
  three-auditor audit of the Electron app (`Toolbox/`), prefixed TBX-S/F/R/P
  (security/functional/robustness/polish). P0 security cluster (unsigned
  self-update TBX-S1, `.uproject` shell/arg injection S2/S3, unconstrained IPC
  primitives S4, nav/CSP/signing S5); P1 half-built (Settings Save dead F1, Tester
  array-args F2). TBX-S1 + TBX-F1 promoted to the S-tier table. A research-driven
  flesh-out pass is queued. See [[project_toolbox_audit]].
- **2026-06-08** — Added UX-P4a–i from the 2026-06-07 live-session client
  feedback (Section 2): debugger-paused-vs-unreachable health signal (P4a),
  `bind_widget_event` false success + missing component-bound-event node kind
  (P4b), `apply_ops atomic:true` non-rollback to verify against H1 (P4c),
  short-ref + pre-flight validation + named-ref errors for splice batches (P4d),
  doubled-JSON in the client-facing spill file (P4e), AGENTS.md exposure-model
  mismatch (P4f), `bp_structural_diff`→`diff_asset` doc reconcile (P4g),
  `VariableGet`-on-param error (P4h), and minor `save_all`/`add_widget`/
  `list_blueprints` output nits (P4i). P4b + P4e promoted to the S-tier table.
  Downstream-project specifics genericized to neutral UE placeholders.
- **2026-06-02** — Initial roadmap created from the four-track research pass
  (Epic 5.8 parity, effectiveness/ease-of-use, transpiling, install/update).
  All items `☐ Open`.
- **2026-06-02** — Batch 1 (PR #250): UX-P1a, PARITY-2, UX-P0a, UX-P0b shipped
  (structuredContent on the default dispatch path; protocol → 2025-11-25;
  `fields`-typo `_warnings`; `enable_tool_category` did-you-mean). Mock suite
  833/0. Pending live-editor verification.
- **2026-06-02** — Batch 2 (PR #251): UX-P1b (add_node `kind` did-you-mean) +
  UX-P2a (`get_class_info`/`read_actor_instance` nested-array pagination).
  Mock suite 835/0. UX-P2a pagination pending live-editor verification.
- **2026-06-02** — Batch 4 (PR #252): PARITY-5 (Gemini/Codex config clients),
  INSTALL-2 (`.mcp.json.example` + README), INSTALL-4 (`.sh` parity),
  INSTALL-5 (Build-MCPServer header). Verified by running `config`.
- **2026-06-02** — Batch 5 (PR #253): INSTALL-1 (version stamp + doctor
  staleness) + INSTALL-3 (engine compat-range in doctor). Mock suite 837/0;
  `--version`/`doctor` verified by running the exe.
- **2026-06-02** — **LIVE-EDITOR VERIFICATION (Batches 1/2/4/5).** Drove the
  rebuilt server against the real Lyra `-nullrhi` commandlet daemon
  (`Saved/live-verify.ps1`). Confirmed on real `IntrospectClass` data: protocol
  `2025-11-25`; `serverInfo.version` stamp in the handshake; `structuredContent`
  on object results; `get_class_info` pagination (`functions_total:160`, `limit`
  trimmed); `fields`-typo `_warnings` (+ available-keys hint); `add_node`
  unknown-kind error (valid-kinds list). The loop's live-verification gate is
  satisfied for the merged server-side work. (Gotcha: had to rebuild the server
  exe from `main` first — a stale exe gave false results, exactly what INSTALL-1
  now catches. See [[live-verify-server-batches]].)
- **2026-06-02** — Batch 6 (PR #255): PARITY-4 governance — dispatch-time,
  bypass-resistant `BP_READER_TOOL_ALLOW`/`BP_READER_TOOL_BLOCK` regex
  (Find + FindAny). Mock suite 840/0; live smoke confirmed `^delete_` hides
  `delete_*` from tools/list while keeping the rest.
- **2026-06-02** — Batch 9 (PR #256): INSTALL-M3 prebuilt-binary release
  workflow (`release.yml`, tag-triggered) + README download note.
- **2026-06-02** — Batch 10 (PR #257): INSTALL-M4 self-hosted editor-compile
  scaffold (`editor-build.yml`) + PARITY-3 verified already-complete (inline
  screenshot images; opt-in by design).
- **2026-06-02** — Batch 3 (PR #258): UX-P2b — trimmed `add_node`'s description
  (dropped the per-kind-args enumeration, in `list_node_kinds`). Mock suite 840/0.
- **2026-06-02** — Batch 7 (PR #259): PARITY-1 progress — confirmed the
  daemon→bridge→`notifications/progress` path is wired + unit-tested; added a
  granular per-op emit in `apply_ops` (+ test). Mock suite 841/0; **live-verified**
  3 progress frames from a 3-op `apply_ops` against the real daemon.
- **2026-06-02** — Batch 11 (PR #260): INSTALL-PKG — two-deliverable
  Marketplace packaging plan (`packaging-marketplace.md`); the server is already
  package-excluded, so deliverable #2 = the binary release.
- **2026-06-02** — Batch 12 (PR #261): INSTALL-M1 (`Install-Plugin.ps1`
  one-shot installer) + INSTALL-M2 (`Build-MCPServer.ps1` auto-routes to the
  CMake fallback on an installed engine; live-verified on UE 5.8). **This
  completes EVERY Q1 (parity) + Q2 (ease-of-use) + Q4 (install) roadmap item.
  Q3 (transpiling, TRANS-*) is intentionally deferred — its cross-leverage
  seams (progress bridge, governance gate, structuredContent, capability flag)
  were kept clean for a future focused effort.**
- **2026-06-03** — UX-P3a (eb1c2b50): trim-tolerant graph/function resolution +
  candidate-list NotFound + `op=` label fix, from a client space-named-function
  report (root cause was a stale pre-38ac0346 build). Live-verified on UE 5.8:
  embedded-space + trailing-space names resolve, trimmed input hits the
  trailing-space graph, and a missing name returns the candidate list end-to-end.
- **2026-06-03** — INSTALL-M5: `Setup-Plugin.bat`/`Update-Plugin.ps1` no-build
  ZIP update (self-updating) + `Install-ClaudeAssets` reconcile-prune +
  self-contained AI-asset URLs + plugin-root README + engine-inference fallback.
  CI exe path fixed (mcp-tests + release run from the plugin Binaries). Mock
  suite 841/0.
- **2026-06-04** — Hardening + conciseness + updates batch (v0.2.0):
  A1 (inbound asset_path normalisation, 96+29 call sites, 849→849 mock);
  C1+C2 (lean default empty-array prune + dedup connections/linked_to);
  C3 (node cap 300 + paginated find_node envelope);
  C4+A2 (blanket 200-item default + clamp on all list tools + schema backfill, 859/0);
  U1 (Check-Update.ps1 GitHub releases API + cache); U2 (doctor reads cache);
  U4 (CHANGELOG + staying-current guidance); U3 (Update-Plugin prebuilt exe + accurate rebuild detection);
  U5 (v0.2.0 tag + release publish);
  H3 (off-game-thread daemon watchdog FRunnable, live-verified 8s max-lifetime → alive=0);
  H1 (real FScopedTransaction rollback live-verified diff=0 pre-batch state);
  H2 (single-op write lock env-gated, live-verified code=6);
  A3 (package + object path both resolve, live-verified). 859 mock/0 final.
- **2026-06-04** — Fourth batch (05675788): EDIT-1 (real AnimBP state machine read
  via AnimGraph module + UAnimStateNode/UAnimStateTransitionNode; add_anim_state now creates
  real node); MCP-7 partial (get_graph, get_function, find_node descriptions with activation
  criteria). 859 mock/0; editor clean.
- **2026-06-04** — Third research batch (167b97ad): MCP-2 (all 258 tools now classified);
  EDIT-3 (EPropertyFlags + metadata map in IntrospectClass); EDIT-4 (list_anim_montages +
  read_anim_montage; 258 tools); PERF-1 (no temp files in daemon -- JsonBody via __MEM__ pointer).
  859 mock/0; editor clean.
- **2026-06-04** — Second research batch (96ad5f1c): MCP-3/4/5 (all were/became done);
  REFLECT-3 (CDO FVector/FRotator defaults as JSON); REFLECT-4 (is_pure/callable/const +
  func_meta in GraphInfoToJson); PERF-5 (conditional stat guard in list_blueprints);
  EDIT-2 (list_timelines + read_timeline; full backend stack; 256 tools). 859 mock/0.
- **2026-06-04** — Implemented MCP-1, PERF-2/3/4, REFLECT-1/2 from research (eb99a2a0):
  title field on all 254 tools (curated table + snake_case auto-fallback);
  daemon poll 5ms (10x throughput, live-verified);
  12 uncached tools now cached (GetReferencers/FindAsset/ReadMaterial/etc.);
  EPropertyFlags decoded as named booleans + CppType + RepNotifyFunc + MetaDataMap in VariableToJson.
  859 mock/0; editor build clean on UE 5.8.
- **2026-06-04** — Research pass: MCP 2025-11-25 spec gaps, UE5 editor customization gaps,
  UPROPERTY/UFUNCTION/reflection architecture, performance bottlenecks. Added 5 new
  sections (MCP-1–9, EDIT-1–5, REFLECT-1–4, PERF-1–5) — 23 new `☐ Open` items.
  Source: `docs/research/research-2026-06-04-mcp-ue5-gaps.md`.
