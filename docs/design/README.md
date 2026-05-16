# bp-reader — technical design document

This is the as-built reference for how bp-reader works today.

| # | Topic | What it covers |
|---|---|---|
| [01](01-overview.md) | System overview | Two halves (plugin + MCP server), 126 tools, what they do |
| [02](02-architecture.md) | Architecture | Component boundaries, process model, request flow |
| [03](03-plugin-internals.md) | UE plugin internals | Editor module structure, commandlet dispatch, K2 schema usage |
| [04](04-mcp-server.md) | MCP server | JSON-RPC, tool registry, response controls, telemetry |
| [05](05-backends.md) | Backends | mock / commandlet / live / auto, backend probe, daemon transport |
| [06](06-wire-protocol.md) | Wire protocol | Types, JSON shapes, conventions, evolution policy |
| [07](07-bpir-and-transpile.md) | BPIR + transpile | The IR, codegen, C++ parser |
| [08](08-error-diagnostics.md) | Errors + diagnostics | Exit codes, classified failures, recovery paths |
| [09](09-testing.md) | Testing strategy | Mock fixtures, doctest, live tests, CI |
| [10](10-bp-to-cpp-node-coverage.md) | BP → C++ node coverage | Per-K2Node mapping table, sentinel reference, supported / approximation / unsupported breakdown |

Read in order if you're new to the codebase. Skip around if you're
chasing a specific question — each file is self-contained.

## Scope

Covers the codebase at `Plugins/BlueprintReader/` and how it
integrates into the parent UE project. Does **not** cover:

- General MCP protocol design (see modelcontextprotocol.io).
- Unreal Engine fundamentals (UCLASS, UObject, asset registry — UE
  docs cover these better).
- How LLM clients consume MCP tools (out of scope; depends on the
  client).
