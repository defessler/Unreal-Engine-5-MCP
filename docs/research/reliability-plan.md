# Reliability & data-integrity plan — MCP server, UE plugin, Toolbox

**Date:** 2026-06-12 · **Status:** approved + in execution; items mirrored in
the [improvement roadmap](improvement-roadmap.md) §10 (REL-*) — flip statuses
THERE as fixes ship; this doc is the stable findings record.

Sources: (1) external best-practices research — MCP spec 2025-06-18/2025-11-25
+ GitHub's production MCP server conventions, Epic's asset-save/transaction/
checkout conventions, Electron updater security (Doyensec 2026), Windows
crash-safe-write patterns (temp+flush+rename, ReplaceFile, Job Objects);
(2) inline code verification of every cited file:line; (3) a 25-agent deep-audit
fleet (7 read-only area auditors + an adversarial refute pass on every P0/P1
claim). **5 fleet claims were refuted** and excluded — e.g. "a game-thread
exception hangs the LiveServer worker" died because ops return error codes
rather than throw, and "the one-shot output file may be read before it's
flushed" died on Windows process-exit flush semantics. Items below marked
*(fleet)* survived the refute pass with file:line evidence.

## What the audit found is already strong

Worth recording so future audits don't re-litigate it:

- **MCP protocol surface**: full version negotiation (2024-11-05 → 2025-11-25,
  `Mcp.cpp:121-130`), all four tool annotations emitted (`ToolRegistry.cpp:18-21`),
  `structuredContent` + text-block dual emission (verified live), progress/
  cancellation plumbing (`CallContext.h`), logging via `notifications/message`.
  This matches or exceeds the GitHub-server reference bar.
- **Batch rollback (H1/UX-P4c)** is genuinely crash-aware: every save defers to
  EndBatch, so the on-disk state stays pre-batch until the whole batch compiles;
  rollback is a reload-from-disk pass that works headless
  (`BlueprintReaderCommandlet.cpp:1473-1503`), plus write-ownership release on
  re-entry (`:1420-1425`).
- **UE's own SavePackage is temp+move** — a crash mid-save never torn-writes the
  .uasset. We never bypass SavePackage with raw file writes (one exception:
  REL-21's generated-source writes).
- **SavePackage failure diagnosis** distinguishes sharing-violations via a
  non-destructive Win32 probe (a previous truncating probe was itself a
  corruption bug, caught in PR #59 review).
- **Caching invalidation** is extensive (67 invalidation touchpoints) and write
  ops invalidate per-asset (gaps: REL-18).

## Findings & plan items

> **STATUS: COMPLETE — all 25 items (REL-1..25) shipped + tested 2026-06-12**
> (commits `a3186862` / `9f26e607` / `10e938de` / `35b1b9eb` / `df5aba61`, all
> CI-green). The per-phase `(☐ open)` headers below are the original plan
> shape; the authoritative live status tracker is
> [roadmap §10](improvement-roadmap.md#reliability) (every row ✅ Done).
> Verification: mock 877/877 + live `Saved/verify-rel-{a,b1,b2,b3,c,c7,d}.ps1`.

Severity: **P0** = data loss / corruption possible · **P1** = reliability
failure or unrecoverable-change hazard · **P2** = robustness / trust gap ·
**P3** = hygiene.

### Phase A — P0 data-loss closure + doctor trust ✅ shipped 2026-06-12

**REL-1 (P0) ✅ — config writer destroyed user config on parse failure.**
`Scripts/Generate-ClientConfig.ps1` `Read-JsonFileOrEmpty`: a corrupt existing
`.mcp.json`/client config returned `@{}` and the save replaced the whole file,
deleting every other MCP server entry. *Fixed:* parse failure now **aborts that
client's write** (script exits 1, other clients still written); every
successful write takes a `.bak` first and publishes atomically (temp+rename) —
covering this script's share of REL-3 too. Live-verified: corrupt config →
exit 1 + file byte-identical; valid config → sibling entries preserved + .bak.

**REL-2 (P0) ✅ — broken Blueprints were saved without consent.**
`BlueprintReaderCommandlet.cpp` `CompileAndSaveBlueprint` never checked
`Results.NumErrors` — a mutation that broke the graph persisted a
compile-failing BP over the last good on-disk state (and into source control
via auto-checkout). *Fixed:* the **batch flush (EndBatch) now refuses** to save
a BP whose final compile has errors (the batch is the atomic-construction
unit); refused assets are reported in the ack's `save_skipped[]`
(`{asset_path, error_count}`), lifted into the `apply_ops` response, and flip
`ok:false`. Escape hatch: `apply_ops save_on_error:true` / commandlet
`-SaveOnError`. Single (non-batch) ops keep saving by default — incremental
construction legitimately passes through error states between calls and
one-shot mode needs persistence to carry state — but `BP_READER_STRICT_COMPILE=1`
extends the refusal to them. Live-verified: error batch → `ok:false`,
`save_skipped` listed, on-disk .uasset byte-identical; `save_on_error:true` →
persisted with errors reported.

**REL-9 (P2) ✅ — doctor false-FAILed the editor-DLL check.**
`Diagnostics.cpp` used a walk-3-levels-up helper (project root) instead of the
sibling `FindPluginDir`, probing a bogus `Plugins/Binaries/Win64` path with the
plugin-shipped exe layout. *Fixed:* uses `FindPluginDir`; the dead helper is
removed. Live-verified: doctor now prints `[ OK ] … DLL found` at the real path.

### Phase B — write-path durability (☐ open)

**REL-14 (P1, fleet)** Five ops **mark dirty but never save** — client gets
`ok:true`, change silently lost on session end (and in batch mode never enters
`BatchPending`): `AddWidget` (:4577), `SetWidgetProperty` (:4625),
`AddAnimState` (:6614), `SetBTNodeProperty` (:5016), `SetDataAssetProperty`
(:5355). *Fix:* route each through `MaybeCompileAndSave` (or the appropriate
save for non-BP assets), matching `AddVariable`/`AddNode`.

**REL-15 (P1, fleet)** Batch-deferral bypass: component ops (:3816, :3854,
:3924, :4013) + `ImplementInterface` (:11815) call `CompileAndSaveBlueprint`
directly — inside a batch they write to disk mid-batch, breaking H1 rollback's
"nothing reached disk until EndBatch" invariant. *Fix:* `MaybeCompileAndSave`.

**REL-5 (P1)** Single-op live writes bypass the undo stack (only `BeginBatch`
opens a transaction). *Fix:* `FScopedTransaction` per non-batch write op when
`GEditor && !IsRunningCommandlet()`.

**REL-4 (P1)** No pre-write .uasset backup (UE autosave never runs under
commandlets). *Fix:* first save per asset per connection copies the on-disk
`.uasset` to `Saved/BPReaderBackups/<Package>-<UTC>.uasset` (ring N=5);
`BP_READER_BACKUP=0` opt-out.

**REL-3 (P1)** Non-atomic truncate writes of user-owned files. ✅ for
`Generate-ClientConfig.ps1` (shipped with REL-1); ☐ remaining:
`Check-Update.ps1:134-142` (Saved/ cache), Toolbox `main.ts` `saveProject`
(reuse its own temp+rename write-file IPC helper at `:503-510`).

**REL-21 (P2, fleet)** `write_generated_source` (:2089-2095) overwrites
existing source files via plain `SaveStringToFile` — no temp+rename, no backup.
*Fix:* atomic write + `.bak` of any pre-existing file.

**REL-8 (P2)** Handshake/status files non-atomic
(`BlueprintReaderLiveServer.cpp:652,725`, `BlueprintReaderCmdletServer.cpp:992,
1054`) — partial-JSON reads are a plausible contributor to known handshake
flakiness. *Fix:* `<file>.tmp` + `IFileManager::Move`.

### Phase C — transport & process lifecycle (☐ open)

**REL-16 (P1, fleet)** `AutoBlueprintReader.cpp:354-381` — **write ops can
double-execute**: a socket attempt lands the mutation but the response read
fails → Auto falls back to commandlet and re-runs the write. *Fix:* for writes,
fall back only when failure provably preceded dispatch (connect/send failed);
after the op frame was sent, surface the transport error instead of retrying.

**REL-17 (P1, fleet)** `BlueprintReaderLiveServer.cpp:345-370` — `ReadFrame`
appends to `PendingBuffer` with no size limit; one connection sending a
newline-less stream OOMs the editor. *Fix:* 10 MB frame cap → log + disconnect
(same in CmdletServer's reader).

**REL-6 (P1)** No Job Object on spawned editors
(`CommandletBlueprintReader.cpp:289,1221`) — MCP-server death orphans a
multi-GB editor until idle timers fire. *Fix:* `CreateJobObject` +
`KILL_ON_JOB_CLOSE` at spawn; timers stay as fallback.

**REL-7 (P1)** Daemon shutdown is hard `TerminateProcess` (:1138-1141, "TCP
shutdown not wired yet"). *Fix:* send the shutdown op over TCP, bounded wait,
then terminate. Plus spawn-retry backoff/limit (:1056-1072) against
editor-per-tool-call spawn storms.

**REL-12 (P2)** `Update-Plugin.ps1:227` — unconditional, **unscoped**
`Stop-Process BlueprintReaderMcp` kills live sessions of every project.
*Fix:* match the process's exe path to the target project; require `-Force`;
print "restart your MCP client".

**REL-24 (P2, fleet)** Transport robustness batch: stdio loop never detects
stdout write failure (`Server.cpp:375-462`); Content-Length parsed without
overflow checks (`HttpServerMain.cpp:64-112`); token comparison not
constant-time + handshake-file ACL hardening
(`BlueprintReaderLiveServer.cpp:118-122,509-518`); non-numeric `BP_READER_*`
env values fall back silently (`BackendFactory.cpp:106-107`).

### Phase D — policy & install integrity (☐ open)

**REL-19 (P2, fleet)** Read-only-mode bypass: `console_command` /
`run_python_script` pass through ReadOnly (`ReadOnlyBlueprintReader.cpp:
295-297`) — `obj savepackage` or arbitrary Python mutates from "read-only"
mode. *Fix:* reject both in read-only mode (`BP_READER_RO_ALLOW_CONSOLE=1`
override); document.

**REL-20 (P2, fleet)** `save_all` (:2112-2193) persists **every** dirty
package, including the user's own half-done manual edits in a live editor.
*Fix:* default scope = assets this connection touched; `scope:"all"` opt-in.

**REL-22 (P2, fleet)** `BPRSeedCommandlet.cpp:272,322` — seeds at fixed
`/Game/AI/BP_TestEnemy` paths and silently overwrites an existing user asset
with that name. *Fix:* refuse unless the existing asset came from a prior seed
(tag check); `-Force` override.

**REL-23 (P2, fleet)** `BlueprintReaderRuntimeConsole.cpp:19-46` —
`bp_reader.list/read` console commands live in SHIPPING builds (leaks BP
internals to players). *Fix:* `!UE_BUILD_SHIPPING` gate or default-off CVar.

**REL-10 (P2)** `Install-Plugin.ps1:87-91` — robocopy `/MIR` purges a plain
(non-junction) git checkout in `Plugins/`. *Fix:* `.git` present + not a
junction → refuse with "git pull instead".

**REL-11 (P2)** `Update-Plugin.ps1:214` + `release.yml` — artifacts verified
only by `--version`; no checksums. *Fix:* release.yml emits `SHA256SUMS`;
updater verifies before extraction. (Authenticode rides the Toolbox signing
item — one cert.)

### Phase E — caching correctness (☐ open)

**REL-18 (P1/P2, fleet)** Invalidation completeness in
`CachingBlueprintReader.cpp`: `ImplementInterface` doesn't stale the interface
BP / referencer queries (:424-428); `Activate/DeactivateGameFeature` invalidate
nothing (:957-964); `MoveAsset` misses the global list cache (:551-560);
TTL-only entries serve stale reads after an external editor Ctrl+S when mtime
checks are disabled (:76-131). *Fix:* broaden per-op invalidation; always
mtime-check when projectDir is known.

### Phase F — small robustness + verification debt (☐ open)

**REL-25 (P3, fleet)** `CommandletBlueprintReader.cpp` polish: check
`ifstream.is_open()` before parse (:829); thread-unique temp names (:467);
output-file size/stability validation + tail logging on parse failure
(:822-836).

**REL-13** Verification debt: every phase lands with engine-free doctests in CI
where possible + live verification on the real editor (standing bar: real
assets, no gated-skip-only coverage). Phase A's harness:
`Saved/verify-rel-a.ps1`.

## Execution order

A ✅ → B (REL-14+15, then 5+4, then 3-remainder+21+8) → C (16+17 first) →
D → E → F. Each increment: implement → build (editor UBT + server CMake) →
mock suite green → live verify → roadmap §10 status flip → commit → CI green.

## Out of scope here (tracked elsewhere)

Toolbox E2E harness / PS7 / async registry / project single-source /
self-update signing → roadmap §9 (TBX-V1…V7). Editor-module CI on real
engines → INSTALL-M4. Daemon handshake latency → known issue (REL-8 may help).
