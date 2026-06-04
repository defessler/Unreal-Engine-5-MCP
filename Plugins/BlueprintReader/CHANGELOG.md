# Changelog

All notable changes to the BlueprintReader MCP plugin.

Format: [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).
Versioning: bump `VersionName` in `BlueprintReader.uplugin`, tag `vX.Y.Z` →
`release.yml` publishes the prebuilt server bundle automatically. The version
flows into `bp-reader-mcp --version`, the `doctor` report, and the MCP
`initialize` `serverInfo.version`.

---

## [0.4.2] — 2026-06-04

### Fixed
- Release CI: `package-lock.json` now committed so `npm ci` works on GitHub Actions runners
- Release CI: switched from `npm ci` to `npm install` for robustness; fixed `cache-dependency-path` glob
- Release CI: removed server-only zip — only plugin bundle and Toolbox installer are published

## [0.4.1] — 2026-06-04

### Added
- **Toolbox Settings page**: 28 environment flags organized into Backend, Permissions, Tools, Performance, and HTTP transport groups. Each flag has inline descriptions, a matching input control, and a "Copy env block" button that generates the ready-to-paste `"env": { ... }` JSON for any MCP client config.

### Fixed
- `wiki/Configuration.md`: added 11 previously undocumented flags (`BP_READER_REQUIRE_CONFIRM`, `BP_READER_VERBOSE`, `BP_READER_HTTP_PORT/PATH`, `BP_READER_DAEMON_WEDGE_SECONDS`, `BP_READER_TOOL_ALLOW/BLOCK`, `BP_READER_AUTO_CHECKOUT`, `BP_READER_PLUGIN_DENYLIST`, etc.)
- Stale tool-count references updated from 252/254 → 258 across CLAUDE.md, wiki, Toolbox, and source comments
- Toolbox sidebar version corrected from `v1.0.0` to `v0.4.0`
- `[Unreleased]` CHANGELOG section removed (all items were already shipped)
- `.mcp.json` untracked from git (contained hardcoded local paths); added to `.gitignore`

## [0.4.0] — 2026-06-04

First full public release. Consolidates all work from v0.1–v0.3:
- 258 MCP tools across read, write, editor-control, and transpile categories
- BlueprintReader Toolbox — Electron GUI installer, provider configurator, and MCP tester
- Full install release bundle: plugin ZIP + Toolbox NSIS installer + server-only ZIP
- All features from v0.2.0 and v0.3.x (see prior changelog entries for details)

## [0.3.2] — 2026-06-04

### Added
- **Full install release bundle**: release now publishes three assets per tag — complete plugin ZIP (entire `Plugins/BlueprintReader/` with prebuilt server exe), Electron Toolbox NSIS installer, and the server-only ZIP (backward compat).

## [0.3.1] — 2026-06-04

### Added
- **BlueprintReader Toolbox** — standalone Electron + React + TypeScript GUI app at `Plugins/BlueprintReader/Toolbox/`:
  - **Install page**: one-click `Install-Plugin.ps1` with streaming log output, auto-detected project path and engine dir
  - **Providers page**: live status cards (configured / stale / missing) for all 5 AI clients (Claude Code, Cursor, VSCode, Gemini, Codex) + Skills/Agents deploy button
  - **Tester page**: fuzzy-search across all 258 tools (catalog bundled from `docs/tools.json`), auto-generated argument forms from `inputSchema`, `POST /mcp` calls + SSE EventSource for live notifications, 20-entry call history. Supports all backends: Mock, Commandlet, Live, Auto.
  - **Update page**: `Check-Update.ps1` cache display + one-click `Update-Plugin.ps1` with streaming log
  - Launch via `Toolbox.bat` at the plugin root
  - Build: `cd Plugins/BlueprintReader/Toolbox && npm install && npm run dev` (dev) or `npm run dist` (NSIS installer → `Binaries/Toolbox/`)

## [0.3.0] — 2026-06-04

### Added
- **258 tools** (was 254): `list_timelines`, `read_timeline` (UTimelineTemplate + FRichCurve keys); `list_anim_montages`, `read_anim_montage` (sections, notifies, slot tracks for GAS/action game workflows).
- **AnimBlueprint state machine read** (`read_anim_blueprint`): real walk of `UAnimationStateMachineGraph` via the AnimGraph module — states, transitions, and their connections. `add_anim_state` now creates real `UAnimStateNode` objects (was a stub).
- **Full UPROPERTY metadata in `list_variables`**: decoded `EPropertyFlags` as named booleans (`blueprint_read_write`, `replicated`, `transient`, `save_game`, etc.), `rep_notify_func`, `cpp_type` (exact C++ typename), and the full `meta=()` key-value map per property.
- **UFUNCTION flags + metadata in `get_function`**: `is_pure`, `is_callable`, `is_const`, `is_static`, and `func_meta` (HidePin, DefaultToSelf, AutoCreateRefTerm, Keywords, etc.) per function graph.
- **UPROPERTY metadata in `get_class_info`**: `blueprint_read_write`, `blueprint_read_only`, `edit_anywhere`, `replicated`, `transient`, `save_game` flags + `rep_notify_func` + full metadata map per property.
- **CDO struct defaults as JSON**: `FVector`/`FRotator`/`FLinearColor`/`FColor`/`FQuat`/etc. defaults emitted as `{"X":0,"Y":0,"Z":0}` instead of UE text format `"(X=0,Y=0,Z=0)"`.
- **Complete tool annotations** (MCP 2025-03-26): all 258 tools now have `readOnlyHint`, `destructiveHint`, `idempotentHint` correctly set — enables Claude Code auto-approval and ChatGPT store compliance.
- **`serverInfo.description`** in MCP initialize response (MCP 2025-11-25).
- **`MCP-Protocol-Version` header** on all HTTP POST responses (MCP 2025-06-18 §transports).
- **Destructive-op confirmation guard**: set `BP_READER_REQUIRE_CONFIRM=1` to require `_confirm:true` on any destructive tool before it executes.
- **Title field on all 258 tools** (MCP 2025-06-18): every tool now has a human-readable display name shown in client UIs.

### Performance
- **No temp-file I/O per daemon call** (PERF-1): result JSON written directly to a stack-allocated `FString` via `__MEM__:<ptr>` sentinel — eliminates two filesystem operations per call.
- **5ms daemon poll** (PERF-2): was 50ms — 10× throughput improvement on rapid-fire read sessions.
- **12 more cached tools** (PERF-3/4): `GetReferencers`, `GetDependencies`, `ListAssets`, `FindAsset`, `ReadDataTable`, `ReadMaterial`, `ReadWidgetBlueprint`, `ReadBehaviorTree`, `ReadStateTree`, `ReadNiagaraSystem`, `ReadLevelSequence`, `ReadAnimBlueprint` now use the TTL+mtime cache.
- **No O(N) filesystem stats in `list_blueprints`** (PERF-5): per-BP `GetTimeStamp` syscall skipped by default (opt-in with `-IncludeMtime`).

### Fixed
- C4996 `std::getenv` MSVC warnings eliminated across all 15 callsites — replaced with `bpr::env::Get()` / `bpr::env::GetOrDefault()`.

### Changed
- Tool descriptions improved for `get_graph`, `get_function`, `find_node` with explicit "when to use vs alternative" guidance.

## [0.1.0] — Initial release

- 258 MCP tools: Blueprint introspection + mutation + BP↔C++ transpile + editor
  control.
- Two backends: live (running editor via TCP) + commandlet (headless
  `UnrealEditor-Cmd.exe`), auto-selected. Mock backend for development.
- Daemon mode: long-lived editor process reused across calls.
- Read-only by default (`BP_READER_ALLOW_WRITE=1` to enable writes).
- Progressive disclosure: `tools/list` surfaces ~38 core tools by default; all
  254 accessible via `enable_tool_category`.
- Full BP→C++ transpile / C++→BPIR parse pipeline (opt-in via
  `BP_READER_ALLOW_TRANSPILE=1`).
