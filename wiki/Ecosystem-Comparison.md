# Ecosystem comparison — bp-reader vs. other UE MCP servers

Quick-read summary. The full gap analysis + staged plan lives in
[Roadmap.md](Roadmap).

## What I found in the ecosystem

11 active UE-MCP servers. The most useful comparables:

- **chongdashu/unreal-mcp** (~1.9k ★) — popular, but narrow (actors
  + minimal BP).
- **flopperam/unreal-engine-mcp** (~900 ★) — **broadest surface**:
  materials, Niagara, IK rig/retarget, MetaSound, PCG, GAS,
  GameplayTags, Landscape, Foliage, Sequencer, PIE-time runtime
  assertion testing.
- **aadeshrao123/Unreal-MCP** — **deepest single-domain coverage**:
  96 Niagara commands, 35 material commands (incl. Substrate), 33
  StateTree, Unreal Insights profiling.
- **GenOrca/unreal-mcp** — cleanest **granular tool design**:
  material expression graph editing, BT + Blackboard CRUD, UMG
  widget BP CRUD.
- **remiphilippe/mcp-unreal** — only one with **headless build /
  cook / Live Coding hot-reload** as MCP tools, plus a bundled local
  Bleve index of UE 5.7 API docs.

Smaller ones (`atomantic/UEMCP`, `runreal/unreal-mcp`,
`runeape-sats/unreal-mcp`, `kvick-games/UnrealMCP`,
`ayeletstudioindia/unreal-analyzer-mcp`) cover narrower surfaces
(viewport ergonomics, Remote-Control passthroughs, static analysis
of UE C++ source).

## Unique to bp-reader

- **BP↔C++ round-trip via BPIR.** Genuinely nobody else has
  bidirectional transpile with a structured AST pivot. Closest is
  Flop's `cpp_source` which is export-only.
- **344-test suite** incl. 12 BPIR↔C++ identity-pinning round-trip
  tests. No other server advertises this.
- **4-backend architecture** (mock / commandlet / live / auto) with
  explicit decorator chain (caching, read-only) and per-call probe.
- **Zero-config live mode** — plugin auto-publishes port + token via
  a handshake file the MCP server reads.
- **Fine-grained Claude Code skills** + a read-only audit subagent.
  No other server ships skills.

## Tier 1 gaps — every other server covers these

- **Material graph authoring** — most-requested missing feature.
- **UMG widget authoring** — universally present except in us.
- **Component / SCS authoring** — already planned as **Cluster D**.
- **DataTable / DataAsset CRUD** — DataTable mutation is
  **Cluster E**; DataAsset is a follow-up.

## Tier 2 gaps

Behavior Tree + Blackboard, Niagara, LevelSequence, StateTree, GAS
+ GameplayTags.

## Tier 3 ergonomics

Unreal Insights profiling, headless cook + package, local UE API
doc search index, viewport helpers (`snap_to_socket`, `line_trace`,
`fit_camera`).

## Tier 4 — out of mainstream scope

World Partition, source control, Chaos / MetaSound / PCG / Landscape
/ Foliage. Nobody covers these well; defer until a real ask.

## The plan (full version in [Roadmap.md](Roadmap))

**Stage 0** — finish what's open:
- Cluster D (components/SCS authoring).
- Cluster E (DataTable row mutation).
- `read_output_log` ring buffer in `StartupModule`.
- **Editor target build verification** — the ~2,500 lines of plugin
  code from PRs #9/#10/#11 haven't been UBT-rebuilt yet; risk of
  unverified UE-API calls.

**Stage 1** — material graph + UMG (the biggest universal gaps).

**Stage 2** — Behavior Tree + DataAsset + StateTree.

**Stage 3** — profiling + headless cook + API doc search + viewport
ergonomics.

**Stage 4** — niche specialties (Niagara, Sequencer, GAS, AnimGraph)
on demand.

Through Stage 2 (~20–30 new tools), bp-reader reaches the surface
area of flopperam (broadest server) while keeping its unique
transpile / architecture / coverage / DX advantages. Each stage is
independent and incrementally shippable.
