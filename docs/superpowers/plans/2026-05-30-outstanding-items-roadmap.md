# Outstanding-Items Roadmap (consolidated)

**Status:** Proposed (2026-05-30). **Feasibility-audited 2026-05-30** — every item fact-checked against the repo + UE 5.7 engine source + the MCP spec by a 23-agent verification workflow with adversarial refutation on the high-risk claims. Corrections are folded in below; the verdict table is at the end.

**Consolidates:** this session's streamlining remainder; the **Epic MCP Integration Plan v10** remainders (documented stubs + deferred list); the **bp-roundtrip** stages 4–5 deferral; plus two new items (player-character recreate-roundtrip test; repo cleanup).

**Already shipped (context):** Epic Phases A–17 are largely on `main` (HTTP+SSE, push events, Lyra toolsets, awareness waves → the 249-tool surface), plus this session's streamlining (default-on progressive disclosure + lean graph reads) and fixes (pin-sort, SCC auto-checkout, array-node, 4 friction-log items, 249-tool live smoke).

---

## Progress — 2026-05-30 (autonomous PR-per-step run)

Landed on `main` this run (9 PRs + a history purge):
- **0.1 `.gitignore`** — `Temp/ tmp/ tpc-data/` ignored (folded into the cleanup PR). ✅
- **D.3 repo cleanup** — repo scoped to plugin + docs (PR #174, untrack 2092 files), then a **`git filter-repo` history purge**: `.git` **1.41 GiB → 3.41 MiB** (~400×). The Lyra build host stays on disk locally (untracked); `origin/backup/pre-purge-main` retains the full old history as a recovery point — **delete it to finalize the size reduction**. ✅
- **1.6 default to read-only** — `BP_READER_ALLOW_WRITE` opt-in; verified the ReadOnly decorator covers all 217 interface methods (audit's "2 gaps" did not hold); mock suite 801/801 green (PR #175). ✅
- **D.1 docs audit + D.2 quick start** — README + CLAUDE + 6 wiki pages reconciled to the plugin-only repo + read-only default; new top-of-README Quick start; tool counts 127 → ~249 (PR #176). ✅
- **1.4 plugin-crash resilience** — `BP_READER_PLUGIN_DENYLIST` (→ `-DisablePlugin=<name>`) so a plugin that crashes in `StartupModule` (e.g. DLSS) is skipped non-interactively; actionable child-exit-before-handshake errors. Compiles, mock 801/801 (PR #178). ✅ *(runtime needs a live editor)*
- **2.1 bp-roundtrip Stage 4 (increment)** — replaced the pass-through cheat with a real per-function `EmitCppFunction`→`ParseCppFunction` re-parse + graceful fallback (PR #179). Whole-class header re-parse is the remaining piece. ✅ *(partial)*
- **1.7 player-char recreate-roundtrip test** — env-gated live test: read `B_Hero_Default` → recreate skeleton via write tools (not duplicate) → `bp_structural_diff` (PR #180). Compiles + auto-skips; end-to-end live run pending a working backend. ✅ *(test compiled, not run live)*
- **1.5 lifetime** — opt-in `BP_READER_DAEMON_MAX_LIFETIME_SECONDS` hard backstop (daemon self-exits regardless of activity); documented IDLE_SECONDS. Editor builds clean (PR #181). ✅ *(runtime needs a live daemon; deliberately NOT a Job Object — would orphan a shared daemon)*
- **1.3 set/map node kinds** — confirmed a non-issue (no work), per the feasibility audit. ✅ (dropped)
- **1.2 bpir↔LyraGenerated header collision** — **mooted by the cleanup**: a consumer project that mounts the plugin has no `LyraGenerated`, so the UHT basename collision can't occur. Now only affects the maintainer's local build host's gated `[roundtrip][bpir]` tests. Deprioritized.

**Remaining — require live-editor development + verification (a blind merge would be unverified or fake-progress):**
- **1.1 daemon handshake fix** — interactive debugging of the UE engine-init stall in a running daemon (needs a live editor + log inspection). Note: tools already work via the slow one-shot fallback, so this is a reliability/perf task, not a breakage. My 1.4 work (`-EnableAllPlugins`/denylist guidance + retry-worthy errors) may help projects whose stall is plugin-load-related.
- **2.2 cancellation + progress** — prerequisite: un-stub cook/package (they return `started=false` today); live cancellation is cooperative-only and needs a long op + runtime to verify.
- **3.1 anim/persona selection → real** — confirmed: needs `bUseRTTI=true` **plus** intricate, version-specific Persona preview-scene access (selected bone/socket) developed against a *running* anim editor. A compile-only version would return `valid:true` with garbage — worse than the honest `valid:false` stub. The `bUseRTTI` enabler alone is inert. Real work, live-only.

Each of these three is gated on a running editor (which item 1.1 itself addresses, and which is documented-flaky). They warrant focused, live-verified work — not a blind merge to `main` that ships unverified process-control, an unfinished parser, or garbage editor-state data.

---

## Cross-cutting constraints (read first — they shape sequencing)

1. **Locked production exe.** Any *server-side* change relinks `BlueprintReaderMcp.exe`, held open (LNK1104) while it serves a live MCP client. Workflow per server change: force-close the bp-reader server → relink → restart the client. (`[[mcp-exe-locked-during-session]]`)
2. **Daemon unreliable right now** — handshake never appears, calls fall back to slow one-shot. It's both an outstanding item *and* a prerequisite for cheaply **live-verifying** most others. **Audit-refined root cause:** the handshake *is* published correctly (`FCmdletServer::Start()` writes it before the blocking pump); the stall is upstream in **UE engine initialization** (asset-registry/Lyra module load) which runs *before* `RunDaemon` even executes. (`[[commandlet-daemon-handshake-flaky]]`)
3. **Engine RTTI is a build-config gap, not a wall** *(audit-corrected)*. The per-asset-editor selection stubs ARE reachable from an editor module via `static_cast` to **public** toolkit interfaces (`IHasPersonaToolkit`→`IAnimationBlueprintEditor`), gated only on adding `bUseRTTI=true` to `BlueprintReaderEditor.Build.cs` (1 line; pattern already in `BlueprintReaderMcpCore.Build.cs:37`). Exception: the **Niagara** toolkit is module-private (no public interface) → stays stubbed.
4. **Staleness.** Epic-plan "X of Y" counts predate continued work; verify per-phase state against the live tool surface + tests during execution.

---

## Tiered inventory

| # | Item | Source | Value | Effort | Blocker/Risk | Recommendation |
|---|------|--------|-------|--------|--------------|----------------|
| **Tier 0 — hygiene / activation (hours)** |
| 0.1 | `.gitignore` `Temp/ tmp/ tpc-data/` | session | L | S | — | Do |
| 0.2 | Activate shipped streamlining (restart client + `LyraEditor` rebuild) | session | H | — | user action | Do (you) |
| **Tier 1 — reliability · safety · fast checks (do first)** |
| 1.1 | **Daemon handshake fix** | session | **H** | M | stall is in UE *engine init* (pre-`RunDaemon`) | **Do first** (unblocks live verification) |
| 1.2 | bpir↔LyraGenerated header collision | bpir | M | S–M | — | Do (prefix `RT_`; UHT collides on basename) |
| 1.3 | ~~set/map + special node kinds~~ → **RESOLVED (false premise)** | session | — | — | — | **No work** — set/map already work via base node; only arrays needed the fix (done) |
| 1.4 | **Bad-plugin resilience** (DLSS/Wwise) | new | **H** | M | crash-on-init can't be caught in-process | Do (split: `-EnableAllPlugins` load-fail tier + crash-detect/auto-disable tier) |
| 1.5 | **Server / commandlet lifetime** | new | **H** | M | gated by 1.1 to test | Do (harden real `HANDLE`+`TerminateProcess`+lock+idle-shutdown; +optional Job Object) |
| 1.6 | **Default to read-only** | new | **H** | S | behavior change | Do (1-line flip; decorator covers 216/218 — close 2 gaps) |
| 1.7 | **Player-character recreate-roundtrip test** | new | **H** | M | gated by 1.1 (live) | Do (read `B_Hero_Default` → rebuild via write tools, *no* duplicate → `bp_structural_diff`) |
| **Tier 2 — moat / spec parity** |
| 2.1 | **bp-roundtrip Stage 4** (whole-class C++→BPIR) | bp-roundtrip | **H (moat)** | **M** | — | Do (Stage 5 already ~done; Stage 4 is the gap, ~500–1000 LOC) |
| 2.2 | **J — cancellation + progress** | Epic | M–H | M | cook/package are stubs; live can't preempt | Do (prereq: dispatch work; per-backend semantics) |
| **Tier 3 — per-asset-editor selection → real data** |
| 3.1 | Anim/Persona selection (`get_anim_editor_state`, curve editor) → real | Epic | M | M | **`bUseRTTI=true` (1 line)** | Do (feasible via public Persona interfaces) |
| 3.2 | Niagara selection + misc v1 stubs | Epic | L–M | varies | Niagara toolkit module-private | Niagara stays stubbed; others case-by-case |
| **Tier 4 — convenience / demand-driven** |
| 4.1 | `inspect_asset` unified quick-look | session | L–M | M | new tool plumbing | Optional (lean reads + `summarize_blueprint` cover most) |
| 4.2 | Tool-description trims | session | L | M (tedious) | — | Optional |
| 4.3 | `take_desktop_screenshot` composite; EA-push Wave-2 Lyra events | Epic | L | M | live-editor-only | Demand-driven |
| **Tier D — docs & onboarding (after defaults settle)** |
| D.1 | Audit all docs for accuracy | new | M–H | M–L | — | Do |
| D.2 | Dead-simple quick start | new | **H** | S–M | — | Do |
| D.3 | **Repo cleanup** (plugin + docs only; clone into local Lyra) | new | M | M | Tests must stay under `Plugins/` | Do (extract; submodule/symlink binding) |
| **Tier 5 — keep deferred (with revisit triggers)** |
| 5.1 | **I** AgentSkill UAsset | Epic | — | — | **Epic plugin not public** | Hold until Epic ships publicly |
| 5.2 | **G** visual annotation overlay | Epic | L | XL | UE rasterizer | Defer unless vision-primary |
| 5.3 | **M** reflection-driven tools | Epic | — | XL | loses out-of-process moat | Keep hand-coded |
| 5.4 | **K** MCP-as-client, **L** file sandbox, **EU-write**, Concert, Live Link | Epic | L | varies | niche / covered by SCC | Skip unless demand |

---

## Recommended execution sequence

```
Tier 0  (clear the deck; you do the restart/rebuild)
  └─> 1.1 Daemon fix ──┬─> enables fast live verification for everything after
       1.4 plugin      │   reliability cluster — do together
       resilience  ────┤
       1.5 lifetime ───┘
  └─> 1.6 read-only default        (small; lands the final default posture)
  └─> 1.2 bpir collision           ──> un-gate bpir tests
  └─> 1.7 player-char recreate-roundtrip test   (after 1.1: needs a working live backend)
  └─> Tier D docs + quick start + D.3 repo cleanup  (after defaults settle)
  └─> 2.2 J cancel/progress  ;  2.1 bp-roundtrip Stage 4 (its own mini-plan)
  └─> 3.1 anim/persona (after the 1-line bUseRTTI fix) ; 3.2 Niagara stays stubbed
  Tier 4 as demand;  Tier 5 parked behind triggers
```

(1.3 dropped — no work.) Rationale: **1.1+1.4+1.5** make the commandlet/server robust on real projects *and* make live-verifying everything cheap. **1.6** is a 1-liner that finalizes the default posture, so **Tier D docs** can document it. **1.7** is the strongest write-surface test and needs a working live backend (1.1).

---

## Per-item approach + exit criteria

### 1.1 Daemon handshake fix
- **Audit-refined root cause:** the handshake publish is correct — `FCmdletServer::Start()` writes `bp-reader-cmdlet.json` (`BlueprintReaderCmdletServer.cpp:614`) *before* the blocking pump (`BlueprintReaderCommandlet.cpp:10970`), and `BP_READER_STARTUP_TIMEOUT_SECONDS` *does* reach the daemon via `BackendFactory` → `Config.startupTimeout`. The 600s is consumed during **UE engine init** (asset-registry scan / Lyra module load — e.g. the `UAssetManager::ScanPathsForPrimaryAssets 'Map'/TopDownArena` ensure) *before* `UBPRCommandlet::Main`/`RunDaemon` runs.
- **Approach:** tail the daemon engine log to confirm the init-phase stall; fix via (a) larger startup timeout, (b) `-EnableAllPlugins` / disable heavy plugins (ties to 1.4), (c) async/deferred asset load at daemon start.
- **Exit:** daemon publishes its handshake within timeout; the 249-tool live smoke runs **daemon-backed** (fast, not one-shot); 0 broken.

### 1.2 bpir ↔ LyraGenerated header collision
- **Approach:** prefix `BPRoundtripModule`'s emitted headers `RT_<Name>.h` (`CppClassEmit.cpp:1045-1050`). UHT collides on the **basename** only (directory-agnostic), so a prefix is sufficient. The generated class name is independent of the filename, and the `[roundtrip][bpir]` tests assert file *contents*, not filenames — safe.
- **Exit:** `BP_READER_RUN_BPIR_COMPILE=1` runs the two gated cases green.

### 1.3 set/map node kinds — RESOLVED (no work)
- **Audit finding (verdict overturned to infeasible/non-issue):** the array fix this session was the *only* special-case needed. Set/Map library functions already resolve wildcards via the base `UK2Node_CallFunction::ConformContainerPins()` (`K2Node_CallFunction.cpp:3022-3216`, preserves `ContainerType`); no `K2Node_CallSet/MapFunction` exists; `BlueprintFunctionNodeSpawner` only special-cases `MD_ArrayParam`. The current `add_node` (`BlueprintReaderCommandlet.cpp:9543-9545`) is already correct.
- **Action:** **none.** Re-open only if a *specific* other special node is reported broken.

### 1.4 Bad-plugin resilience — two tiers
- **Tier-1 (missing module / unbuilt DLL):** default the editor spawn to `-EnableAllPlugins` — makes module-LOAD failures non-fatal (`PluginManager.cpp:1564-1567`). Overridable via `BP_READER_EDITOR_ARGS`.
- **Tier-2 (crash-on-init):** `-EnableAllPlugins` does **NOT** survive a plugin crashing in `StartupModule()` (`ModuleManager.cpp:1087,1236` — no try-catch; a hard crash like DLSS native init kills the process). Mitigation: detect the editor child exiting before handshake (`CommandletBlueprintReader.cpp:849-889`), append the suspect to a per-project denylist, retry with `-DisablePlugin=`. **Caveat:** `-DisablePlugin=` does NOT override `.uproject`-enabled plugins (`PluginManager.cpp:1577-1590`; README:319) — those need a derived target/uproject or the live backend.
- **Exit:** unbuilt-plugin project starts (Tier-1) or degrades with a clear message + auto-disable retry (Tier-2); the crash-on-init `.uproject` limitation is documented (stop claiming `-EnableAllPlugins` covers it).

### 1.5 Server / commandlet lifetime — harden the real mechanism
- **Existing (verified):** idle-shutdown `BP_READER_DAEMON_IDLE_SECONDS` (300s, min 5s) via `FCmdletServer::WantsShutdown` on `ActiveConnections==0` (`BlueprintReaderCmdletServer.cpp:346-371`); child reap via explicit `HANDLE` + `TerminateProcess` (`CommandletBlueprintReader.cpp:1070-1077`, `984-997`); daemon singleton via exclusive lock `bp-reader-cmdlet.lock` (`:820-854`); per-call timeout `BP_READER_TIMEOUT_SECONDS` (120s); MCP exit on stdin EOF (`main.cpp:600-604`). **No Win32 Job Object exists today** — the earlier "Job-Object reap" wording was a proposal, not current code.
- **Approach:** verify each fires (idle-shutdown couldn't be exercised — daemon never came up; do after 1.1). Orphan-safety improvement: ensure `TerminateProcess` runs on *all* MCP exit paths, **or** add a Win32 Job Object with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` so a hard parent death still reaps children. Add a max-lifetime watchdog backstop.
- **Exit:** closing the client leaves zero surviving editor/daemon processes; idle daemon exits within its window; runaway op bounded by the per-call timeout.

### 1.6 Default to read-only
- **Approach:** flip `BP_READER_READ_ONLY` default on (or add `BP_READER_ALLOW_WRITE`); wire the existing `ReadOnlyBlueprintReader` decorator into the chain by default. **Close the gap:** the decorator overrides 216/218 methods — find + cover the 2 missing mutation methods so nothing slips through. Tag write tools in annotations.
- **Exit:** every mutation rejected with a clear "writes disabled — set `BP_READER_ALLOW_WRITE=1`"; env restores writes; a test asserts both. **Behavior change → changelog + quick-start.**

### 1.7 Player-character recreate-roundtrip test (NEW)
- **What:** the strongest end-to-end test of the write surface. **Read** `B_Hero_Default` (`/Game/Characters/Heroes/B_Hero_Default`, 215 KB; on disk; in `Scripts/lyra-assets-manifest.json`; restored by `setup.bat`; currently used by *zero* tests) → **manually reconstruct** it via the mutation tools (`create_blueprint` + `add_variable`/`add_function`/`add_component`/`add_node`/`wire_pins`/`apply_ops`) into a fresh `/Game/Recreated/...` asset — **NOT** `duplicate_blueprint` — → **`bp_structural_diff(original, recreated)`** and assert minimal/known drift (leverages the position-independent diff fixed this session).
- **Scope honestly:** assert **structural** equivalence over what the tools cover (variables, functions, components, event-graph node topology + wiring). Engine-subsystem specifics the tools can't reproduce (GAS grants, input mappings, anim wiring) show as diff — a **coverage signal**, not a failure; assert the reproducible skeleton matches + record the known-gap diff.
- **Layer (pick one — the suite separates them):** *mock/CI* — craft a `B_Hero_Default.json` fixture (precedent `BP_ExampleCharacter.json`) + a `test_mock_backend.cpp` shape assertion; OR *live* — env-gated case in `test_commandlet_backend.cpp` doing the real read→recreate→diff (auto-skips without engine/project env). The **live** layer is the real test; gated by 1.1.
- **Fallback:** `B_SimpleHeroPawn` (41 KB, inherits `B_Hero_Default`) if `B_Hero_Default` is too heavy; if `Content/` is stripped, `setup.bat` restores the asset (document the dependency in the test comment).
- **Exit:** read→manual-recreate→diff yields only the documented known-gap drift.

### 2.1 bp-roundtrip — Stage 4 (Stage 5 already ~done)
- **Audit finding:** **Stage 5 (BPIR→whole-BP) is substantially COMPLETE** — `CreateBlueprint`/`AddVariable`/`AddComponent`/`AddFunction` + per-function body via `compile_function` (`BPIRRoundtrip.cpp:219-461`); full class schema exists (`Decompile.cpp:3156-3168`, `CppClassEmit.cpp:1001-1495`). **Stage 4 (whole-class C++→BPIR) is the deferred gap** — currently `res.bpir_after = res.bpir_before` pass-through (`BPIRRoundtrip.cpp:209-217`); `ParseCppFunction` is per-function only (`CppParse.h:50-76`). Stage 4 = UCLASS/UPROPERTY/UFUNCTION header parsing (~500–1000 LOC, **M**, not XL).
- **Approach:** build whole-class C++→BPIR parse; lift the inline Stage-5 materialization into a reusable `TranspileBlueprintWhole`.
- **Exit:** the `[roundtrip][bpir]` tests assert stages 4–5 *succeed*; a real BP round-trips BP→C++→BP with bounded drift.

### 2.2 J — cancellation + progress
- **Prerequisite:** `cook_content`/`package_project` currently return `started=false` with a manual-RunUAT hint (`BlueprintReaderCommandlet.cpp:4783-4823`) — they must actually dispatch work first, or the item is aspirational.
- **Per-backend:** *commandlet* — cancellable via subprocess kill (`TerminateProcess`). *Live* — dispatches `AsyncTask(GameThread)` then blocks on `DoneEvent->Wait()` (`BlueprintReaderLiveServer.cpp:187-192`); the MCP layer **cannot preempt** — cancellation is **cooperative only** (op must poll `CallContext::IsCancelled()`, `CallContext.h:39-42`; spec permits ignoring). No tool wires the scaffold today (`IsCancelled`/`EmitProgress` appear only in tests).
- **Approach:** wire progress + cooperative-cancel into the long ops; mark live long-ops "best-effort cancellation" in annotations.
- **Exit:** a long op streams progress; commandlet ops cancel via kill; live ops cancel cooperatively; synchronous default preserved.

### 3.1 Anim/Persona selection → real data
- **Audit finding (reclassified from "needs engine change"):** feasible via an editor sidecar — subscribe `OnAssetOpenedInEditor` (`AssetEditorSubsystem.h:189-190`), `static_cast` `IAssetEditorInstance*` → public toolkit interfaces (`IHasPersonaToolkit`→`IAnimationBlueprintEditor`, `IHasPersonaToolkit.h:7-13`). **Gated only on a 1-line build fix:** add `bUseRTTI=true` to `BlueprintReaderEditor.Build.cs` (pattern in `BlueprintReaderMcpCore.Build.cs:37`).
- **Exit:** anim/persona/curve selection tools return real data on the live backend; mock/commandlet unchanged.

### 3.2 Niagara + misc stubs
- **Niagara stays stubbed:** `NiagaraSystemToolkit` is module-private with no public interface (`NiagaraSystemToolkit.h:1-50`) — no in-editor cross-cast possible without an engine change. Ship as a documented stub. Other Wave-3/14/16/17 v1 stubs (`get_hover_target`, `get_isolate_mode`, …) are documented-stub-by-design; promote case-by-case as public APIs allow.

### D.1 Audit all docs for accuracy
- **Approach:** file-by-file through `README.md`, `wiki/*` (manually pushed), `docs/{research,tutorial,design,superpowers}`, `CLAUDE.md`, `Plugins/.../Claude/skills/bp-reader/SKILL.md`. Reconcile to current reality: tool count, the **new defaults** (progressive disclosure default-on → ~37 advertised; lean graph reads; read-only-by-default once 1.6 lands), env-var tables, build commands (`LyraEditor` + engine patches), the project being **Lyra**, backend selection, `call_tool`/`enable_tool_category` flow. Fix drift; flag contradictions.
- **Exit:** a checklist of every doc with pass/fix notes; corrections committed; `wiki/` pushed to its remote.

### D.2 Dead-simple quick start
- **Approach:** one short copy-pasteable **Quick Start** at the top of README + wiki landing: (1) drop `Plugins/BlueprintReader/` into your project's `Plugins/` (or submodule it — see D.3); (2) build the editor once; (3) paste the MCP client-config snippet with the safe defaults (read-only + progressive disclosure) + the one line to enable writes; (4) keep up to date (submodule/subtree + `Install-ClaudeAssets`). No UE internals on the happy path.
- **Exit:** a new user adds the plugin + drives one read tool in <10 min from the quick start alone.

### D.3 Repo cleanup — plugin + docs only (NEW)
- **Keep:** `Plugins/BlueprintReader/` (entire subtree — `Tests/` MUST stay under `Plugins/`: UBT's RulesCompiler only scans `<Plugin>/Tests/` for `.Target.cs`, so moving it breaks discovery silently), `docs/`, `wiki/`, `README.md`, `CLAUDE.md`.
- **Remove:** `LyraStarterGame.uproject`; `Source/LyraGame`+`LyraEditor`+`Lyra*.Target.cs`; `Plugins/LyraGenerated` + the 12 other plugins; `Config/`; all `Content/` except `Content/AI/BP_TestEnemy.uasset`+`BP_TestPickup.uasset`; build artifacts (`Binaries/`,`Intermediate/`,`Saved/`,`DerivedDataCache/`,`Platforms/`).
- **Binding (build continuity):** the Tests are UE Program targets that need a host `.uproject` with the plugin in `Plugins/`. After extraction, mount into a **local Lyra checkout** via git **submodule** (recommended) or **junction/symlink** (Windows; `PreBuildHook.ps1` resolves symlinks via `Resolve-Path`). The MCP server walks up from `Binaries/Win64/` to find the `.uproject` (`BackendFactory.cpp:145-154`) — submodule/symlink resolves; a bare clone with no host `.uproject` needs `BP_READER_PROJECT` set (or a stub `.uproject`).
- **Approach:** manual curation (clone → delete Lyra scaffold → push to a `blueprint-reader-plugin` repo → deprecate the original) over `git filter-repo` (short history + prior squashes). ~240 plugin files kept, ~1530 removed. The three engine `.Build.cs` patches stay as external re-applications regardless.
- **Defer (per user):** turning `setup.bat` into a general "overall setup" script — Lyra is already local, so no sync script is needed now.
- **Exit:** `git clone` of the plugin repo + submodule/symlink into a local Lyra builds the editor + MCP server + runs the suite; the repo holds only plugin + docs.

---

## Verification (cross-cutting)
- Mock suite (`BlueprintReaderMcpTests.exe`) green after every change; tool-count + protocol-compat snapshots bumped for any surface change.
- Live verification via the **daemon** (after 1.1): re-run the 249-tool smoke (`BP_READER_SMOKE_ALL`) + the 1.7 recreate-roundtrip + targeted per-item live cases.
- Each server change: force-close server → relink → restart client → confirm in a fresh exe (as done for default-on this session).
- 1.6 read-only: a test asserting default-rejects-write + `BP_READER_ALLOW_WRITE` enables it.

---

## Decision log
- Stay on MCP **2025-06-18**. The **2025-11-25 `tasks/*` spec is published + implementable** (full task lifecycle, `tasks/cancel`, capability negotiation, SDKs) — so J's deferral is on **priority + impending-breaking-change risk** (the 2026-07-28 RC changed it), *not* "not implementable." Revisit if we adopt `tasks/*`.
- **M reflection** / Epic `UToolsetDefinition`: stay hand-coded (out-of-process moat) despite passing the 200-tool trigger.
- **I AgentSkill:** integrate (don't reimplement) once Epic's plugin is public.
- External HTTP deploy: localhost-only unless funded.

---

## Feasibility audit (2026-05-30) — verdict table

23-agent workflow vs repo + UE 5.7 engine source + MCP spec; adversarial refutation on high-risk items.

| Item | Verdict | Note |
|------|---------|------|
| 1.1 daemon fix | ✅ accurate | refine wording — stall is in UE *engine init* before `RunDaemon`, not the handshake write |
| 1.2 RT_ header prefix | ✅ accurate | UHT collides on basename; prefix is safe |
| 1.3 set/map node kinds | ⛔ false premise | dropped — set/map already work via base `UK2Node_CallFunction::ConformContainerPins` |
| 1.4 plugin resilience | ✏️ revised | `-EnableAllPlugins` = load-fail only; crash-on-init needs Tier-2 detect/auto-disable |
| 1.5 lifetime | ✏️ revised | no Job Object exists; real mech = `HANDLE`+`TerminateProcess`+lock+idle-shutdown |
| 1.6 read-only default | ✅ accurate | 1-line flip; decorator covers 216/218 — close 2 gaps |
| 2.1 bp-roundtrip | ✏️ revised | Stage 5 ~done; only Stage 4 deferred (M, not XL) |
| 2.2 J cancel/progress | ✏️ revised | cook/package are stubs; LIVE = cooperative-cancel only |
| 3.1 anim/persona stubs | ✏️ upgraded | feasible after `bUseRTTI=true` + public Persona interfaces |
| 3.2 Niagara stub | ✅ accurate | toolkit module-private → stays stubbed |
| Tier-5 J-spec rationale | ✏️ revised | 2025-11-25 tasks IS published; deferral is priority/risk-based |
| 1.7 player-char test | ➕ new | `B_Hero_Default` exists; specify mock vs live layer |
| D.3 repo cleanup | ➕ new | feasible; `Tests/` must stay under `Plugins/`; submodule/symlink binding |

**Bottom line:** nothing is outright impossible. One item was a false premise (1.3 → dropped); five needed mechanism/scope corrections (1.4, 1.5, 2.1, 2.2, Tier-5 rationale); one got *more* feasible (3.1); the rest are accurate as written. All corrections are folded into the items above.
