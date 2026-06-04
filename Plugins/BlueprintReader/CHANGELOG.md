# Changelog

All notable changes to the BlueprintReader MCP plugin.

Format: [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).
Versioning: bump `VersionName` in `BlueprintReader.uplugin`, tag `vX.Y.Z` →
`release.yml` publishes the prebuilt server bundle automatically. The version
flows into `bp-reader-mcp --version`, the `doctor` report, and the MCP
`initialize` `serverInfo.version`.

---

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
