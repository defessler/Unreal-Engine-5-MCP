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
shaping, and did-you-mean are mature; onboarding docs are current. Remaining
items are refinements, with two genuine silent-failure traps (P0).

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
- **Status:** ☐ Open · **Effort:** M
- `HttpTransport.h` implements the deprecated 2024-11-05 two-endpoint pattern (`/sse` + `/message`). The 2025-03-26 spec replaced it with a single `/mcp` endpoint supporting POST (client→server) and GET (SSE stream). Newer clients probe by POSTing `InitializeRequest` to `/mcp`; if they get 405 they fall back. Also required: `MCP-Protocol-Version` header on all HTTP requests after initialize (2025-06-18). Keep old endpoints for backward compat.
- **Why:** Unblocks clients that have moved past the 2024-11-05 transport; old pattern deprecated.

### MCP-7 — Tool description quality pass {#mcp-7}
- **Status:** ✅ Done (05675788, 2026-06-04) · **Effort:** M
- 2025 arXiv study: 97.1% of MCP tool descriptions had quality defects ("unclear purpose" in 56%). Improvements yielded +5.85% task success rate, +15.12% evaluator accuracy. For each tool: add (a) explicit purpose statement, (b) activation criteria — *when* to use this vs. similar tools, (c) key parameter constraints and failure modes. Focus on the ~50 highest-traffic tools first. Claude Code uses BM25 over `name + description + parameter names` for tool selection.
- **Why:** Direct path to better AI task success without any server code changes.

### MCP-8 — Tasks primitive for long-running ops {#mcp-8}
- **Status:** ☐ Open · **Effort:** L
- Mark `compile_blueprint`, `build_lighting`, `run_automation_tests`, `cook_content`, `package_project`, and large `apply_ops` batches with `execution: {taskSupport: "optional"}`. Implement basic `tasks/get` and `tasks/cancel` methods. Clients can call with `{"task":{"ttl":60000}}` to get a taskId and poll instead of blocking. **Note:** Tasks are experimental in 2025-11-25 and the 2026-07-28 RC redesigns them — implement conservatively or wait for 2026-07-28 stable GA.
- **Why:** Long-running ops currently time out in some clients; async tasks fix this correctly.

### MCP-9 — Elicitation for destructive write confirmation {#mcp-9}
- **Status:** ☐ Open · **Effort:** M
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
- **Status:** ☐ Open · **Effort:** L
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
- **Status:** ☐ Open · **Effort:** M
- `BlueprintReaderCommandlet.cpp:11837`: `IsoDateForFile(FileOnDisk)` is called per BP inside `RunListOp`. On a 1000-BP project this is 1000 `IFileManager::GetTimeStamp` syscalls in a loop on the game thread — a hidden O(N) cost. Fix option A: derive `modified_iso` from `FAssetPackageData` (available from the registry, no syscall). Fix option B: batch the stat calls asynchronously before returning. The `FAssetRegistryModule::Get().GetAssetPackageData()` API provides `DiskSize` and hash — enough to detect changes without a stat.
- **Why:** Removing O(N) syscalls from `list_blueprints` matters for any session that lists a large project's BPs more than once (the caching backend invalidates this on every write op).

---

## Revision log

Newest first. One line per change to this file.

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
