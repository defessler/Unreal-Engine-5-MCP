# Test & Verification Report — 2026-06-02 to 2026-06-04

## Testing methodology overview

Three verification tiers were used depending on the component:

- **[CI]** — Hosted GitHub Actions CI (`mcp-tests.yml`): CMake build on `windows-latest` → 859-case doctest mock suite → `Dump-Tools.ps1 -Check` (catalog drift). Fires on every push touching `Tests/**` or `.github/workflows/mcp-tests.yml`. No UE engine required.
- **[LIVE]** — Local UE 5.8 (installed engine) with the `-nullrhi` commandlet daemon: UBT editor module build → fresh daemon → purpose-written PowerShell probe scripts. Verifies editor-module changes that CI cannot cover.
- **[PS]** — Manual PowerShell script execution against local filesystem/network, covering the update-flow and script infrastructure changes.

---

## Changes and how each was tested

### Output schema backfill (PRs #228–#248) — `[CI]`
~30 commits across 2026-06-02 filling in `output_schema` declarations on all tools. Verified by the existing `test_output_schema_populated.cpp` fixture validator (6 tools asserted shape-by-shape) and `test_tool_modes.cpp` mock matrix (0 schema violations across all 54 advertised tools). CI green on each PR.

---

### Batch 1–12 improvements (PRs #249–#261) — `[CI]` + `[LIVE]`

| Batch | What | How tested |
|---|---|---|
| 1 — structuredContent + protocol 2025-11-25 | spec conformance, did-you-mean | CI mock suite + `test_mcp.cpp` protocol negotiation assertions; live-verified handshake on the running daemon |
| 2 — `add_node` did-you-mean, nested array pagination | UX enrichment | CI + live roundtrip on a real Lyra BP |
| 3 — description trim | docs only | CI catalog check |
| 4 — Gemini/Codex config, install S-tier | scripts | [PS] ran `Generate-ClientConfig.ps1` for all 5 clients |
| 5 — version stamp + doctor staleness | diagnostics | CI doctests in `test_diagnostics.cpp`; [LIVE] `doctor` output on installed UE 5.8 |
| 6 — regex allow/block governance | tool filtering | CI `test_tool_filter.cpp` |
| 7 — granular `apply_ops` progress | progress frames | [LIVE] 3 progress frames verified from a 3-op batch on the real daemon |
| 9 — prebuilt release workflow | CI release pipeline | `release.yml` publish confirmed; `v0.2.0` release asset visible on GitHub |
| 10 — editor-module compile scaffold | CI workflow | scaffold only; awaits self-hosted runner |
| 11 — Marketplace packaging plan | docs only | — |
| 12 — `Install-Plugin.ps1` one-shot installer + CMake fallback | scripts | [PS] full install run on UE 5.8 (installed engine) |

---

### `38ac0346` — granular-recreation tool fixes + 10 add_node kinds + material tools — `[CI]` + `[LIVE]`

**What changed:** 11 tool-layer bug fixes (Event idempotent, FunctionResult dedup, DeleteAsset eviction, 10 new `add_node` kinds, variadic pins, external vars, self-context calls, LiveServer/CmdletServer arg-quoting for space-bearing values), `create_material` / `create_material_instance` promoted to full tools.

**How tested:**
- CI: mock suite (841 cases at that point, 0 failed); tool count assertions bumped to 254; catalog regenerated.
- [LIVE]: full granular BP recreation harness against real Lyra BPs (`B_WeaponFire` 112 nodes, `B_Hero_Default`); `bp_structural_diff` reported diff=0 on all three BPs tested.
- Material tools: end-to-end via the MCP server live path with `BP_READER_BACKEND=live`; `ok:true` + asset saved to disk confirmed.

---

### `1537d4d3` — server exe lives in plugin Binaries — `[CI]`

CMake `CMAKE_RUNTIME_OUTPUT_DIRECTORY` changed; UBT mirror added. CI confirmed the new path (`Plugins/BlueprintReader/Binaries/Win64/`) works. Note: this commit introduced the subsequent `fdb1547c` CI failure (workflows still pointed at the old path), which was fixed immediately.

### `fdb1547c` — fix CI exe paths — `[CI]`

Verified by the CI run itself: `mcp-tests.yml` + `release.yml` both successfully invoked the new path. Run result: **success**.

### `a0628da5` — parent-death watchdog — `[CI]` + manual

Server-side C++ (no UE). Build verified by CI. Manual test: launched `BlueprintReaderMcp.exe` as a child process, killed the parent terminal process, confirmed the server process exited (no orphan).

### `9f3f9afc` — pure ASCII plugin scripts — `[CI]`

Build verified by CI (em-dash was causing PowerShell 5.1 parse errors for a user). Validated all 8 `.ps1` files parse with 0 errors under `pwsh` and `powershell.exe -Version 5.1`.

---

### Path inference (`e98b62f0`) + engine fallback (`7886a056`) — `[PS]`

**`_Common.ps1` inference:** Ran `Patch-Engine.bat` with zero args → confirmed resolution of `ProjectDir=D:\Projects\UE5_MCP`, `ProjectFile=LyraStarterGame.uproject`, `EngineDir=D:\Games\Epic Games\UE_5.8`.

**Engine fallback for unregistered GUID:** Simulated a `.uproject` with a GUID not in HKCU Builds → confirmed fallback to the installed `UE_5.8` with a warning.

**Install failure fix (`7886a056`):** The user reported `Install-Plugin.bat` throwing on a project with a GUID `EngineAssociation`. Fix re-tested with three synthetic `.uproject` files (GUID, empty, clean version-string); all resolved correctly.

---

### `Setup-Plugin.bat` / `Update-Plugin.ps1` (`5b448aef` → `03829c23`) — `[PS]`

**Full clone→deploy→configure path:**
- Ran `Update-Plugin.ps1 -PostUpdate` against a throwaway temp project → verified: plugin deployed, `.mcp.json` written, `.claude` assets created, `AGENTS.md` created, `Binaries/` untouched.
- Full real ZIP-download run: downloaded `main.zip` from GitHub, extracted, self-update re-exec triggered (hashes differed due to CRLF/LF), deployed, configured, temp dir cleaned up (0 dirs left).

**Normalized line-ending hash (`f20d6217`):** Verified LF vs CRLF copies of the same file hash identically via `SHA256([text].Replace("\r\n","\n"))`.

---

### `eb1c2b50` — space-named graph/function resolution — `[CI]` + `[LIVE]`

**What:** Trim-tolerant `FindGraphByName` in both write and read paths; candidate-list `NotFound` errors; `op=` label strip.

**CI:** Mock suite 849/0.

**[LIVE] via `Saved/verify-spaces.ps1`:**
- Created BP with functions `"Spaced Fn Name"` (embedded space) and `"Trailing Space "` (trailing space)
- `get_function "Spaced Fn Name"` → `code=0` ✓
- `get_graph "Spaced Fn Name"` → `code=0` ✓
- `get_function "Trailing Space "` (exact) → `code=0` ✓
- `get_function "Trailing Space"` (trimmed) → `code=0` (trim fallback) ✓
- `get_function "DoesNotExist"` → `code=4` + candidate list `[UserConstructionScript, Spaced Fn Name, Trailing Space ]` ✓

**End-to-end client-facing format:** Drove `BlueprintReaderMcp.exe` as a `live` client; confirmed `op=Function` (not `op=-Op=Function`), candidate list present.

---

### A1 — `NormalizeAssetPath` (`fe5be67b`) — `[CI]`

8 new doctests in `test_normalize_asset_path.cpp` covering: pass-through, object-suffix strip, whitespace trim, backslash normalisation, dot-in-directory non-strip, idempotency, `RequireAssetPath`/`OptAssetPath` wrappers. All in the CI suite (849 total).

---

### C1+C2 — Lean default (`d8c6ea80`) — `[CI]` + end-to-end

**CI:** `BP_READER_VERBOSE=1` disables pruning; `PruneEmpty` doesn't strip schema-required keys (caught by `test_output_schema_populated.cpp` during development, refined to bloat-keys-only list).

**End-to-end:** Drove a `get_graph` call through the mock server; confirmed `lean=true` drops `linked_to[]` from pins while `connections[]` preserved at graph level; `verbose=1` restores raw shape.

---

### C3 — Node cap + paginated `find_node` (`82d883b2`) — `[CI]` + end-to-end

**CI:** `test_tools.cpp` updated to assert the new `{total,count,has_more,results}` envelope; `test_output_schema_populated.cpp` row updated.

**End-to-end via mock server:**
- `find_node query="Event"` → `{total:1, count:1, has_more:false, results:[...]}` ✓
- `get_graph max_nodes=2` on a 7-node fixture → `{nodes.count:2, nodes_truncated:true, nodes_total:7}` ✓

---

### C4+A2 — Blanket page size + schema backfill (`ad552604`) — `[CI]`

**Test churn:** `test_tools.cpp`, `test_cursor.cpp`, `test_mcp.cpp`, `test_output_schema_populated.cpp`, `test_phase_d.cpp` all updated. Added `AsResults()` helper to `test_helpers.h`.

- `test_phase_d.cpp`: asserts `list_blueprints`/`list_variables`/`list_assets` → `outputSchema.type == "object"` (envelope); `list_node_kinds`/`list_data_tables` → `"array"` (unchanged).
- `test_tool_modes.cpp`: mock matrix — 0 schema violations across all 54 tools.
- CI: **success** on `ad552604`.

---

### U1+U2+U4 — Update notifications (`c3f7a803`) — `[CI]` + `[PS]`

**CI (3 new doctests in `test_diagnostics.cpp`):**
- `doctor` shows "Update available: v0.2.0" when cache has `update_available:true` ✓
- No warning when `update_available:false` ✓
- No warning when cache absent ✓

**[PS]:** `Check-Update.bat` run online → "Plugin is up to date (0.1.0)"; offline → silent exit 0 ✓.

---

### U3 — Prebuilt exe download (`b73ff409`) — `[PS]`

Tested after `v0.2.0` tag went live:
- With matching release asset: download + `--version` verify + exe swap + "no rebuild needed" ✓
- With no matching asset: source-only fallback ✓

---

### H3 — Daemon watchdog (`3104c0af`) — `[CI]` + `[LIVE]`

**CI (7 new doctests in `test_daemon_watchdog.cpp`):** Pure function `ShouldForceExit()` tested for: inactive (never fires), max-lifetime (fires exactly at threshold), max-lifetime disabled (`startedAtUnix=0`), wedge-detection (fires at threshold), no-wedge when `lastTick=0` (brand-new daemon), max-lifetime overrides healthy heartbeat, both disabled.

**[LIVE]:**
- Started daemon with `BP_READER_DAEMON_MAX_LIFETIME_SECONDS=8`
- After 30s total: `alive=0` ✓
- Daemon log confirmed `"clean shutdown"` 16s after process start ✓

---

### H1 — FScopedTransaction rollback (`013161b1`) — `[CI]` + `[LIVE]`

**CI:** 859/0 after propagating `EndBatch(bool, bool)` through all 6 backend implementations; `test_apply_ops.cpp` mock signature updated.

**[LIVE] via `Saved/verify-rollback.ps1`:**
- Created BP with `PreBatchVar`
- `apply_ops` with `on_failure="rollback"`, `atomic=true`:  Op1 `add_variable BatchVar` succeeded; Op2 `add_variable FAIL type=INVALID_TYPE_XYZ` forced failure
- After rollback: `list_variables` → `[PreBatchVar]` only; `BatchVar` absent ✓
- Diff=0 with pre-batch state confirmed ✓

---

### H2 — Single-op write lock (`013161b1`) — `[LIVE]`

Via `Saved/verify-writelock.ps1` with `BP_READER_LOCK_SINGLE_OP=1`:
- Session 1: opened batch on `_LockTest` BP
- Session 2: single-op `add_variable` on same BP → `code=6, error="blueprint_locked_by_other_session"` ✓
- Session 1 ended batch → released lock ✓

---

### A3 — Path parity confirm (`013161b1`) — `[LIVE]`

Via `BlueprintReaderMcp.exe` live path:
- `summarize_blueprint "/Game/AI/BP_TestEnemy"` → `name="BP_TestEnemy"` ✓
- `summarize_blueprint "/Game/AI/BP_TestEnemy.BP_TestEnemy"` → `name="BP_TestEnemy"` ✓

---

### CI fix commits — `[CI]`

| Commit | Fix | Result |
|---|---|---|
| `eeb5899e` | `ReadOnlyBlueprintReader::EndBatch` missing second param | Compile error fixed |
| `2e575eda` | Catalog stale after H1 enum expansion | `Dump-Tools -Check` fixed |
| `38af42cc` | `docs/tools.json` added to CI trigger paths | Future catalog-only commits auto-trigger CI |

Final run `38af42cc` → **CI success**.

---

## Summary

| Metric | Value |
|---|---|
| Commits shipped (2 days) | ~55 |
| Mock doctest cases at end | 859 (was 841 at session start) |
| New doctests added | 18 (`NormalizeAssetPath`×8, `DaemonWatchdog`×7, `UpdateNotice`×3) |
| Live-verified probes run | 6 (`verify-spaces`, `verify-rollback`, `verify-writelock`, path-parity, watchdog timing, material tools) |
| CI runs triggered | ~20 |
| Final CI state | **green** (`38af42cc`) |
| v0.2.0 release asset | 1 prebuilt `BlueprintReaderMcp-v0.2.0-win64.zip` on GitHub |
