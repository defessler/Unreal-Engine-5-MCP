# Research notes — UE5.7+ BP tooling

This directory collects research notes informing the BP-reader server's
roadmap. See [`docs/superpowers/specs/2026-05-16-bp-roundtrip-capability-design.md`](../superpowers/specs/2026-05-16-bp-roundtrip-capability-design.md)
for the driving spec and
[`docs/superpowers/plans/2026-05-16-bp-roundtrip-capability.md`](../superpowers/plans/2026-05-16-bp-roundtrip-capability.md)
for the implementation plan that generated these notes.

## Files

- [`ue5.7-overview.md`](ue5.7-overview.md) — what's new in UE5.7 that affects BP tooling.
- [`editor-automation.md`](editor-automation.md) — every lever for headless / scripted editor work.
- [`scripting-languages.md`](scripting-languages.md) — Lua, AngelScript, Verse: how each hooks the editor.
- [`syntactic-sugar-nodes.md`](syntactic-sugar-nodes.md) — K2 nodes that desugar at compile time, with C++ equivalents.
- [`bp-reader-extensibility-audit.md`](bp-reader-extensibility-audit.md) — current seams + how to extend.
- [`tpc-anatomy.md`](tpc-anatomy.md) — full inventory of the UE5 ThirdPerson template (drives the roundtrip tests).
- [`bp-roundtrip-architecture.md`](bp-roundtrip-architecture.md) — what the roundtrip code actually does, lessons-learned, extensibility points.
- [`lyra-bp-to-cpp-conversion.md`](lyra-bp-to-cpp-conversion.md) — case study: converting all 302 Lyra Starter Game BPs to C++, including the LYRAGAME_API export + Private→Public header decisions.
- [`improvement-roadmap.md`](improvement-roadmap.md) — **living, status-tracked backlog** of plugin improvements (Epic-MCP parity, ease-of-use, transpiling, install). Unlike the other notes here, this one is kept current as work lands — see its "Maintaining this roadmap" section.

## How to read

These are research notes, not specifications. They describe what's possible,
what's idiomatic, and where to look — not what the server currently does.
Cross-reference against:

- [`docs/design/`](../design/) — what's actually shipped.
- [`docs/tutorial/`](../tutorial/) — how to use it.
- [`CLAUDE.md`](../../CLAUDE.md) — maintainer-level gotchas.
