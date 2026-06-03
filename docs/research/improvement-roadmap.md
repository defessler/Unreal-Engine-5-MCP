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
| [INSTALL-1](#install-1) | S | ☐ Open | Version-stamp the exe + staleness check in `doctor` — fixes the real stale-copy build break |
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
- **Status:** ☐ Open · **Effort:** M
- Infra exists (`CallContext::EmitProgress`, `notifications/progress`) but there
  are **zero emit sites** in `tools/` — `cook_content`, `package_project`,
  `run_automation_tests`, `build_lighting` run silent. Epic emits a passive
  heartbeat per tick. Parse progress markers from the commandlet/daemon op output
  and forward them.
- **Why:** only remaining true spec gap vs Epic on progress; biggest UX win for
  the slowest tools.

### PARITY-2 — bump default protocol to `2025-11-25` {#parity-2}
- **Status:** ✅ Done (PR #250, 2026-06-02) · **Effort:** S
- We default to `2025-06-18` and negotiate down to `2024-11-05`; Epic ships
  `2025-11-25`. Add it to the negotiate list + default in `Mcp.h`/`Mcp.cpp`.
  Confirm no `2025-11-25`-only *required* semantics (the tasks primitive) are
  implied before advertising.
- **Why:** trivial; keeps us level with Epic and current clients.

### PARITY-3 — inline Image result type for screenshots
- **Status:** ☐ Open · **Effort:** S–M
- `take_screenshot` / `take_viewport_screenshot` / `take_annotated_screenshot`
  return only a disk path; Epic returns inline base64 Image content (auto-detects
  `UTexture2D`→Image, `USoundWave`→Audio). Return MCP image-content blocks.
- **Why:** lets vision models see captures without a filesystem round-trip.

### PARITY-4 — regex allow/block-list governance
- **Status:** ☐ Open · **Effort:** M
- Epic's `UToolsetRegistrySettings` has regex allow/block lists + per-class /
  per-property `SetObjectProperties` block lists. Ours is coarse env flags
  (`BP_READER_READ_ONLY` / `ALLOW_WRITE` / `ALLOW_TRANSPILE`). Generalize to a
  tool-name + class/property block model.
- **Why:** finer safety for shared-editor / multi-agent setups.

### PARITY-5 — extend client-config writer to Gemini + Codex
- **Status:** ☐ Open · **Effort:** S
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
- **Status:** ☐ Open · **Effort:** S
- The unknown-kind error comes from the plugin verbatim. The `list_node_kinds`
  table lives server-side (`…/tools/BlueprintTools.cpp:2572`), so the server can
  catch the failure and append "valid kinds: …" — mirroring what `wire_pins`
  already does for pin types (`…:2111-2160`).
- **Why:** agent self-corrects in one turn instead of needing a separate
  `list_node_kinds` call.

### UX-P2a — array projection on the big unprojected reads
- **Status:** ☐ Open · **Effort:** S–M
- `get_class_info` (`…/tools/BlueprintTools_part3.cpp:1182`) has no `fields`/
  `limit` on `properties[]`/`functions[]`; `read_actor_instance` has `fields` but
  no `limit` on `overrides[]`. A wide class or a heavily-overridden actor is a big
  payload. Add `ParseResponseControls`/`ApplyResponseControls` (or at least
  `limit`/`offset` on the dominant array).
- **Why:** token trim on the remaining unbounded reads.

### UX-P2b — trim multi-line tool descriptions
- **Status:** ☐ Open · **Effort:** S
- Several `add_node`/`find_overriders`/`get_class_info` descriptions run 3–6
  lines. Move "when to use X vs Y" prose into SKILL.md/AGENTS.md; keep
  descriptions to 1–2 lines.
- **Why:** low leverage (only costs tokens once an agent widens past `core`), but
  cheap.

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
- **Status:** ☐ Open · **Effort:** S
- Embed plugin `VersionName` + git short-hash into `BlueprintReaderMcp.exe` at
  build time (CMake `add_definitions` / UBT `PublicDefinitions`); surface it in
  `doctor` and the MCP `initialize` handshake. Have `doctor` read the on-disk
  `BlueprintReader.uplugin` version (`GuessPluginDirFromCfg` already finds the
  plugin dir — `…/Diagnostics.cpp:31-42`) and **warn when the running exe's
  stamped version ≠ the on-disk plugin source**.
- **Why:** directly catches the stale-copy class of failure that bit a downstream
  consumer.

### INSTALL-2 — de-hardcode the committed `.mcp.json`
- **Status:** ☐ Open · **Effort:** S
- `.mcp.json:5,9,10` hardcodes maintainer-only `D:\…` paths *and* contradicts the
  README's source-engine paths (`README.md:309`). Replace with a documented
  placeholder + a note to run `config`, or reduce the committed file to a
  mock-only example and let `config`/`Generate-ClientConfig.ps1` be canonical.
- **Why:** the committed config is non-portable and self-contradictory.

### INSTALL-3 — add `EngineVersion` to `.uplugin`
- **Status:** ☐ Open · **Effort:** S
- No `EngineVersion` field → UE won't warn on a mismatch; combined with the
  multi-engine API guards, a wrong engine just fails deep in the compiler. Add the
  field (or a documented compat range) + a `doctor` check against the engine's
  `Build/Build.version`.
- **Why:** one line turns silent multi-engine compile failures into an upfront
  warning. See [`feedback`/multi-engine-api-guards] and CLAUDE.md's
  "Multi-engine API compatibility" invariant.

### INSTALL-4 — bring `install-claude-assets.sh` to parity with the PS1
- **Status:** ☐ Open · **Effort:** S
- The bash version (`Scripts/install-claude-assets.sh:73-110`) deploys only
  skills+agents; it does **not** deploy `AGENTS.md` or
  `.github/copilot-instructions.md` (the PS1 does — `Install-ClaudeAssets.ps1:123-137`).
  Unix/CI users silently lose the cross-AI guidance.
- **Why:** parity gap; Copilot/Codex users on Unix get less.

### INSTALL-5 — fix the stale `Build-MCPServer.ps1` header
- **Status:** ☐ Open · **Effort:** S
- Its comment (`Build-MCPServer.ps1:7-8`) claims "PreBuildStep is gone; this
  script is opt-in" and carries a legacy no-op path — but the `.uplugin` **does**
  declare `PreBuildSteps` (`BlueprintReader.uplugin:60`). Update the header to the
  current build model.
- **Why:** misleads maintainers/consumers about how the build works.

### INSTALL-M1 — one-shot `Install-Plugin.ps1`
- **Status:** ☐ Open · **Effort:** M
- Given `-EngineDir` + `-ProjectFile`: copy/symlink the plugin into
  `<Project>/Plugins/`, auto-detect source-vs-installed engine and run the correct
  build path (UBT or the CMake fallback), apply `Patch-Engine.ps1 -Apply` for
  source engines, run `Generate-ClientConfig.ps1` + `Install-ClaudeAssets.ps1`,
  finish with `doctor`. All sub-scripts exist — this is glue.
- **Why:** collapses ~8 steps to one command and removes the "which path am I on?"
  decision.

### INSTALL-M2 — auto-detect source vs installed engine in the build
- **Status:** ☐ Open · **Effort:** M
- Inside `Build-MCPServer.ps1`, transparently fall back to the CMake build when
  UBT rejects Program targets on an installed engine (`Tests/CMakeLists.txt`
  already lands the exe in the right place).
- **Why:** consumer stops having to pick the toolchain by hand.

### INSTALL-M3 — ship a prebuilt `BlueprintReaderMcp.exe` as a Release asset
- **Status:** ☐ Open · **Effort:** M
- The server is engine-version-independent pure C++20; CI already builds it via
  CMake (`mcp-tests.yml`). Attach it to a tagged GitHub Release.
- **Why:** removes the single biggest "just try it" barrier (building at all) for
  mock/commandlet use; foundation of a versioned-release update story.

### INSTALL-M4 — compile the editor module in CI on the targeted engines
- **Status:** ☐ Open · **Effort:** L
- CI builds only the standalone server + mock tests (`.github/workflows/mcp-tests.yml`)
  — the #223→#240 multi-engine regression passed CI silently. A self-hosted
  runner with cached engine(s) doing a compile-only smoke against a pre-5.8 *and*
  a 5.8 engine is the only real guard for the multi-engine invariant.
- **Why:** editor-module breaks currently reach consumers undetected.

### INSTALL-PKG — packaging split (plugin proper + binary server)
- **Status:** ☐ Open · **Effort:** L
- A clean Fab/Marketplace ship is blocked by the `Tests/` `Type=Program` targets,
  vendored third-party (`Tests/ThirdParty/`), the `PreBuildSteps` shell-out, and
  the 3 engine `.Build.cs` patches. Pragmatic path: (a) ship the plugin proper
  (`BlueprintReaderRuntime` + `BlueprintReaderEditor`) as a versioned/Marketplace
  plugin, and (b) ship the MCP server as a **separate prebuilt binary release**
  (INSTALL-M3). The server doesn't belong in the engine plugin tree anyway.
- **Why:** unblocks real distribution and fixes the "Tests/ dir is actually
  production code" awkwardness.

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
