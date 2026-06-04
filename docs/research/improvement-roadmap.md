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
