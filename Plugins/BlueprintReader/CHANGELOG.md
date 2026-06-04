# Changelog

All notable changes to the BlueprintReader MCP plugin.

Format: [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).
Versioning: bump `VersionName` in `BlueprintReader.uplugin`, tag `vX.Y.Z` →
`release.yml` publishes the prebuilt server bundle automatically. The version
flows into `bp-reader-mcp --version`, the `doctor` report, and the MCP
`initialize` `serverInfo.version`.

---

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

## [Unreleased]

### Added
- **Self-notifying updates** (`Check-Update.ps1`, U1/U2): polls GitHub releases API
  and caches the result so `doctor` can surface "vX.Y.Z available — run
  Setup-Plugin.bat" without a network call on every startup. Hourly throttle;
  silent on network failure.
- **`Setup-Plugin.bat`** (plugin root): one-click no-build refresh from GitHub
  (ZIP over HTTPS, no git required), self-updating, redeploys via `Install-Plugin
  -SkipBuild` (preserves the built `Binaries/`), reconfigures.
- **`Update-Plugin.ps1`** (U3): `Setup-Plugin.bat` wrapper that auto-downloads the
  prebuilt exe from the GitHub release when a matching asset exists; falls back to
  source-only when not. Detects and prints "rebuild needed" only when sources
  actually changed.
- **Lean-by-default responses** (C1/C2): tool results no longer emit empty arrays
  for known noise keys (`linked_to`, `connections`, `delegate_params`); `links` data
  is de-duplicated between per-pin `linked_to[]` and the graph-level `connections[]`.
  Kill-switch: `BP_READER_VERBOSE=1`.
- **Paginated envelopes on all list tools** (C4): every `list_*` / `find_*` tool
  returns `{total, count, has_more, next_cursor, results:[]}` by default (page size
  200). Absolute clamp: explicit `limit=` capped at 1000.
- **Node cap on graph reads** (C3): `get_graph` and `get_function` accept
  `max_nodes` (default 300); results exceeding the cap include `nodes_total` /
  `nodes_truncated` / `next_cursor`. `find_node` also returns the paginated envelope.
- **Inbound `asset_path` normalisation** (A1): both the package path form
  (`/Game/AI/BP_Foo`) and the object path form (`/Game/AI/BP_Foo.BP_Foo`) are now
  accepted at every tool boundary; the object suffix is stripped before dispatch.
- **Self-contained plugin bundle**: the `Claude/` skills / agents, `AGENTS.md`,
  and `CHANGELOG.md` all live inside the plugin folder. External `docs/` /`wiki/`
  links in the deployed assets were replaced with stable GitHub URLs so a
  plugin-only install has no dangling references.
- **Install reconciliation** (`Install-ClaudeAssets`): prunes stale `bp-*` skills /
  agents that no longer exist in the plugin (bounded to the `bp-*` namespace).
- **Engine-inference fallback** (`_Common.ps1`): the launcher scripts now fall back
  to the installed engine when the `.uproject` `EngineAssociation` doesn't resolve
  (empty / unregistered GUID / unknown version), with a warning naming the engine
  they picked.
- **Parent-death watchdog** (`main.cpp`): the MCP server process now exits when its
  parent process dies, preventing orphaned processes that lock the server exe during
  builds.
- **Space-tolerant graph / function name resolution**: `FindGraphByName` and the
  WireJson matchers do an exact-then-whitespace-trimmed match so a trailing-space
  mismatch between the caller's name and the stored FName still resolves. NotFound
  errors now list the available names.
- **Graph/function `op=` label in errors**: the error label is now `op=Function`
  instead of the previous `op=-Op=Function` formatting glitch.

### Fixed
- `Install-Plugin.bat` engine-inference failure on projects with an unregistered
  GUID or missing `EngineAssociation` in the `.uproject`.
- GitHub CI exe path (`mcp-tests.yml`, `release.yml`) updated to run the test exe
  from `Plugins/BlueprintReader/Binaries/Win64/` (moved there in a prior release).
- `Build-MCPServer.ps1` / `.bat` files were encoding-broken on Windows PowerShell 5.1
  (em-dashes read as ANSI); converted to pure ASCII.

---

## [0.1.0] — Initial release

- 254 MCP tools: Blueprint introspection + mutation + BP↔C++ transpile + editor
  control.
- Two backends: live (running editor via TCP) + commandlet (headless
  `UnrealEditor-Cmd.exe`), auto-selected. Mock backend for development.
- Daemon mode: long-lived editor process reused across calls.
- Read-only by default (`BP_READER_ALLOW_WRITE=1` to enable writes).
- Progressive disclosure: `tools/list` surfaces ~38 core tools by default; all
  254 accessible via `enable_tool_category`.
- Full BP→C++ transpile / C++→BPIR parse pipeline (opt-in via
  `BP_READER_ALLOW_TRANSPILE=1`).
