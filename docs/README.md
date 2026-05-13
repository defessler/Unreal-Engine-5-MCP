# bp-reader documentation

Two parallel sets of long-form docs:

## [`design/`](design/) — Technical Design Document

How the system works today. Reference material for understanding the
architecture, the wire format, the backends, and the data flow. Each
file is self-contained on one topic; cross-references where helpful.

Audience: anyone reading the codebase trying to understand "why is
this here?" or "how does X reach Y?"

## [`tutorial/`](tutorial/) — Build it from scratch

Step-by-step construction of bp-reader from an empty directory. Each
chapter is a milestone where you have something working. Code samples
throughout; expected to be followed in order.

Audience: a mid-level engineer who has used Unreal Engine 5 and knows
C++ and JSON-RPC at a working level, but hasn't built an MCP server
or an in-process editor automation surface before.

## Related references

- [`../README.md`](../README.md) — user-facing setup + tool table.
- [`../CLAUDE.md`](../CLAUDE.md) — maintainer guidance.
- [`../wiki/`](../wiki/) — GitHub Wiki source (manually pushed).
- [`../Plugins/BlueprintReader/Claude/skills/`](../Plugins/BlueprintReader/Claude/skills/)
  — Claude skill manifests describing the tool surface.
