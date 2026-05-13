# Build bp-reader from scratch

A 13-chapter tutorial for a mid-level engineer to construct bp-reader
end-to-end. Each chapter is a milestone: at the end, you have
something demonstrably working before moving on.

## Audience + assumptions

You should be comfortable with:

- C++17 or later. We use C++20 in the MCP server.
- JSON-RPC 2.0 at a conceptual level (request → response, with `id`).
- Unreal Engine 5: writing a UCLASS, building a plugin, the basics of
  the editor module pattern. We won't re-teach UE fundamentals.
- CMake and `Build.bat` enough to compile.

You do NOT need prior experience with:

- The MCP protocol specifically — Chapter 1 covers it.
- The UCommandlet pattern — Chapter 3 introduces it.
- The K2 schema (BP graph editing API) — Chapter 7 covers it.
- Writing a transpiler — Chapter 12 walks through the IR pivot.

## What you'll build

A C++20 MCP server + a UE5 editor plugin that together let an LLM
inspect, edit, and round-trip Blueprint assets. By the end of
Chapter 13 you'll have:

- 119+ MCP tools across read, write, batch, transpile, project,
  content browser, and live-editor categories.
- Four backends: mock (fixture-only), commandlet (child UE process),
  live (in-process TCP), and auto (routes per call).
- A test suite (~440 cases) covering the wire shape, error paths,
  and end-to-end batch operations.

## Chapter map

| # | Title | Milestone |
|---|---|---|
| [01](01-foundation.md) | Foundation: MCP basics and project layout | Empty repo → JSON-RPC echo server |
| [02](02-mock-backend.md) | Mock backend + first read tool | `list_blueprints` returns fixture data |
| [03](03-ue-plugin.md) | UE plugin scaffolding | Editor plugin loads in UE; commandlet dispatches |
| [04](04-introspector.md) | Reading a UBlueprint | `read_blueprint` returns real wire JSON |
| [05](05-commandlet-bridge.md) | MCP → child UE process | mcp-server spawns commandlet, reads response |
| [06](06-first-write.md) | First write op | `add_variable` round-trips |
| [07](07-graph-writes.md) | Graph mutations | `add_node` + `wire_pins` with K2 schema |
| [08](08-batching.md) | apply_ops + named slots | Multi-op batch with slot resolution |
| [09](09-daemon-mode.md) | Long-lived editor daemon | Per-call latency drops from seconds to ms |
| [10](10-live-tcp-backend.md) | In-process TCP server | Talk to a running editor instead of a child process |
| [11](11-auto-routing.md) | Auto backend | Route per call: live if editor is open, commandlet otherwise |
| [12](12-bpir-transpile.md) | BPIR + transpile | BP → C++ → BP round-trip via an IR |
| [13](13-production.md) | Production polish | Tests, CI, error handling, distribution |

## How to use this tutorial

1. Read the chapter intro to know the milestone.
2. Work through the code samples in order; they're cumulative.
3. At each "checkpoint" callout, verify your code does what's claimed
   before moving on.
4. Cross-link to the [design docs](../design/) when you want the
   "why" behind a choice — the tutorial focuses on the "how".

## A note on completeness

This tutorial constructs the architecture and the foundational tool
set. It does NOT walk through every one of the 119 tools — once you
understand the patterns (Chapter 6 + 7), adding more is mechanical.
The pattern is documented at the end of Chapter 7 and in the project's
`CLAUDE.md`.
