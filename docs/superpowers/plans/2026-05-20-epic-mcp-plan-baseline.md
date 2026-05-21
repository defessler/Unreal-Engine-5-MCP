# Epic MCP Integration Plan — measured baseline (2026-05-20)

Numbers backing the cumulative-test-count / tool-count / capability claims in `2026-05-20-epic-mcp-integration-plan.md`. **Re-measure quarterly or whenever a phase ships and update this file in the same commit as the plan update.** Plan claims that diverge from this file should fail review.

## How these numbers were captured

| Number | Command |
|---|---|
| Tests in source | `Grep '^TEST_CASE\\s*\\(' Plugins/BlueprintReader/Tests/BlueprintReaderMcpTests/Private` count mode |
| Tests run | `Binaries/Win64/BlueprintReaderMcpTests.exe --count` |
| Tools advertised | `BlueprintReaderMcp.exe` → initialize → tools/list → `result.tools.length` |
| Capabilities | `BlueprintReaderMcp.exe` → initialize → `result.capabilities` keys |
| Protocol version | `Grep protocolVersion Plugins/.../jsonrpc` then verify in initialize response |

## Measurements

| Field | Measured (2026-05-21 post-Phase-D) | Prior baseline (2026-05-21 post-Phase-B) | Delta |
|---|---|---|---|
| Test cases in source (`TEST_CASE(` blocks) | **700** across 39 files | 671 | +29 |
| Test cases actually executed | **684** (16 filtered/skipped) | 655 | +29 |
| Tools advertised on `tools/list` (commandlet/live) | **132** | 132 | ✓ (Phase D is shape-only, no new tools) |
| Tools advertising `outputSchema` on `tools/list` | **132 / 132 (100%)** | 9 / 132 | **all tools now declare a shape (Phase D)** |
| Screenshot tools accept `return_inline` | **2** (take_screenshot, take_viewport_screenshot) | 0 | **new in Phase D** |
| Inline-image 1280px cap | enforced via `ImageReader::ReadPngDimensions` + `BuildScreenshotResponse` rejection | n/a | **new in Phase D** |
| `BP_READER_NEVER_INLINE_IMAGES` kill switch | shipped (Env::NeverInlineImages, cached) | n/a | **new in Phase D** |
| Tools advertised on `tools/list` (mock backend) | **47** | 47 | ✓ |
| Server default `protocolVersion` | **2025-06-18** | 2025-06-18 | ✓ |
| Capabilities advertised on initialize | `{tools: {listChanged: true}}` only | same | Phase D doesn't claim new capabilities (Prompts / Resources / Logging still pending) |
| Phase A status | **landed on `origin/main`** as `66954afd` + `3e107c65` | same | ✓ |
| Phase B status | **landed on `origin/main`** as `12373937`, `e6f54125`, `1e542220` | same | ✓ |
| Phase D status | **shipped locally** — push deferred to user | not started | **shipped** |
| Initialize `instructions` field | shipped (DefaultInstructions text, env BP_READER_INSTRUCTIONS gate) | same | ✓ |
| HttpTransport GET | **405 + Allow: POST, DELETE** | same | ✓ |
| ToolRegistry::Add | warn-not-throw on length/char violations | same | ✓ |
| ToolRegistry::HasValidTools() | shipped (main.cpp fail-fast on empty) | same | ✓ |
| `call_tool` recursion guard | rejects 3 meta-tool targets | same | ✓ |
| Dotted alias fallback | `<prefix>.<tool>` resolves to flat name | same | ✓ |
| In-flight call registry | shipped on Server (3 methods) + notifications/cancelled wired | same | ✓ |
| Path-traversal helper | shipped (applied to 3 screenshot tools + write_generated_source) | same | ✓ |
| `sort` opt-in on list_* | shipped on list_blueprints + list_assets | same | ✓ |
| Backwards-compat test matrix | shipped (5 protocol pins + tools/list inventory hash + structural asserts) | same | ✓ |
| tools/list inventory hash anchor | `0x0A155550DA73E1F3` (Phase B baseline, 132 tools) | same | ✓ — Phase D's outputSchema additions are NOT included in the hash (it covers name+desc-prefix+inputSchema.type only) |

## Phase A actual state (2026-05-21, post-merge)

Phase A code shipped as a single commit `66954afd feat(mcp): agent-feedback fixes (round 2) — plugin-side + transport` on 2026-05-20 23:58 PT, then pushed to `origin/main`. **42 files / 2960 insertions / 54 deletions** — bundles Phase A items together with the agent-feedback round-2 fixes (list_assets, find_asset, did-you-mean, UClass validation, get_class_info declared_on).

Why this differs from the v8 plan's "split into 8 reviewable commits" target:
- The work was committed as one atomic commit before the splitting plan was authored.
- Splitting now would require force-pushing a published commit on `main`. Rejected per safety protocol.
- All Phase A deliverables ARE in `main` — the topology is just monolithic instead of stepped.

What landed (verified via `git show --stat 66954afd`):
- `tools/ContentBlocks.{cpp,h}` (123 + 75 lines, new)
- `tools/ToolsetMeta.{cpp,h}` (193 + 45 lines, new)
- `tools/ToolCategories.{cpp,h}` (58 + 9 lines, new — added beyond the plan's Phase A scope as part of the same drop)
- `jsonrpc/CallContext.{cpp,h}` (66 + 105 lines, new)
- `jsonrpc/HttpTransport.{cpp,h}` (249 + 95 lines, new)
- `tools/ToolAnnotations.cpp` (2 lines, augmenting the pre-shipped header)
- `BlueprintReaderEditor/Public/BlueprintReaderSettings.h` + `.../Private/BlueprintReaderSettings.cpp` (89 + 22 lines, new UDeveloperSettings)
- `Scripts/Generate-ClientConfig.ps1` (214 lines, new)
- `Tests/.../Private/test_http_transport.cpp` (150 lines, new)
- `Tests/.../Private/test_tool_filter.cpp` (36 lines, new)
- Modifications across `BlueprintTools.cpp`, `BlueprintReaderCommandlet.cpp`, `ToolRegistry.{cpp,h}`, `Mcp.{cpp,h}`, all 6 backends, `main.cpp`, 3 test files

Verification (2026-05-21 morning):
- `git status`: clean working tree (modulo plan docs + untracked scratch dirs)
- `BlueprintReaderMcpTests.exe`: **620 / 620 passed, 0 failed, 16 skipped** (30,316 assertions)
- Initialize handshake: `protocolVersion=2025-06-18`, `capabilities={tools:{listChanged:true}}` (no new caps advertised yet — consistent with Phase A being pure scaffolding for later phases)
- Server name: `bp-reader-mcp`, version `0.1.0`

**Implication for the v8 plan:** the Preconditions section's "Phase A is implemented but uncommitted" + "Phase A-merge gate (~1 day)" is stale. Update to "Phase A landed in `66954afd`; baseline is 620 tests / 132 tools." Downstream phases can start immediately.

## External dependencies (current)

Vendored, header-only, under `Plugins/BlueprintReader/Tests/ThirdParty/`:
- `nlohmann_json` (MIT) — JSON parse/serialize
- `fmt` (MIT, `FMT_HEADER_ONLY=1`) — formatting
- `doctest` (MIT) — test framework

System libraries (Win64):
- `Ws2_32.lib` — sockets (live TCP backend)

**Not yet vendored** but needed by upcoming phases:
- `cpp-httplib` (MIT, single-header) — required by Phase C3 (HTTP transport socket loop)

There is **no** `vcpkg.json` in the tree. The earlier plan referenced "listed in vcpkg.json but not consumed" for `cpp-subprocess` — that lineage is stale; UBT pulls everything from `Tests/ThirdParty/`.

## Velocity calibration data

Recent commit cadence on `main` (2026-05-15 → 2026-05-20):
- 2026-05-15: 8 commits (transpile staging)
- 2026-05-16: 25 commits (transpile + style passes + node coverage)
- 2026-05-17: 0 commits
- 2026-05-18: 0 commits
- 2026-05-19: 2 commits (setup script refinements)
- 2026-05-20: 3 commits (LyraEditor naming, Texture.h include fix, tool annotations as commit)

Phase A merged in two commits: `3e107c65 feat(mcp): tool annotations per MCP 2025-03-26 §tools/annotations` (the foundation header) + `66954afd feat(mcp): agent-feedback fixes (round 2) — plugin-side + transport` (all remaining Phase A items bundled with round-2 agent-feedback fixes).

For per-phase day estimates in the plan, assume:
- "1 day" = 1 calendar day with focused work
- Bursty cadence is normal (25-commit day followed by 2-day pause)
- Phase A wall-clock ≈ several hours of focused work for ~12 substantive items + tests, so velocity calibration suggests **~2 items/day** when running clean

The plan's per-phase day estimates (Phase B = 2–3d, Phase D = 3–5d) are consistent with this rate.

## Re-measure trigger

Re-run the measurement commands and update this file when:
1. Any phase ships (e.g., Phase B exit criteria met)
2. Test count drifts >5% from this baseline
3. Tool count changes by ≥1
4. A new capability is advertised on initialize
5. A new protocol version becomes the server default
6. Quarterly minimum (next: 2026-08-21)

## Revision log

- **2026-05-20** — initial baseline; Phase A measured as uncommitted in working tree (25 modified + 12 new files).
- **2026-05-21 morning** — Phase A landed as commit `66954afd` on `origin/main`. Re-measured: 620 tests, 132 tools (commandlet/live), 47 tools (mock). Capabilities unchanged. Phase A-merge gate satisfied — downstream phases can start.
- **2026-05-21 mid-day** — Phase B shipped across 3 commits: `12373937` (8 hardening items + 23 tests), `e6f54125` (backwards-compat matrix + 8 tests), `1e542220` (sort opt-in + 4 tests). Re-measured: 655 tests, 132 tools (same — Phase B is purely hardening), capabilities still `{tools:{listChanged:true}}`. Phase B success metric (kill-switch + zero-break) verified.
