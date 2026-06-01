# BlueprintReader MCP Server — Requirements & Decision Rules

> The contract used to make design/implementation decisions about the server.
> Sources: [CLAUDE.md](../CLAUDE.md) (maintaining), the bp-reader SKILL.md (using),
> the backend/test/registry audits, and recorded decisions. Tagged **[established]**
> (already true/enforced) vs **[session]** (decided in the 2026-05 work cycle, being
> implemented).

## 1. Purpose & shape
- Standalone MCP server: JSON-RPC 2.0 over stdio, exposing a large set of Blueprint
  introspection / mutation / BP↔C++ transpile / editor-control tools (exact count +
  catalog: generated [`docs/TOOLS.md`](TOOLS.md)). [established]
- Two-module UE plugin — `BlueprintReaderEditor` (Type=Editor, full read+write via
  `UBPRCommandlet -run=BPR` + `BlueprintReaderLiveServer`) and
  `BlueprintReaderRuntime` (Type=Runtime, read-only reflection, loads in cooked
  builds). The MCP server is a UBT Program target. [established]

## 2. Backends & selection
- Modes: `mock` (fixtures, no UE), `commandlet` (headless editor subprocess),
  `live` (TCP to a running editor), `auto` (probes per call → live if editor open,
  else commandlet). **auto** is default when a `.uproject` is discovered, else
  `mock`. [established]
- Decorator chain (every call flows through all layers):
  `ReadOnly → Caching → Auto → (Socket | Commandlet | Mock)`. [established]
- Live and daemon servers share the **same** op dispatch (`RunOneOp`); the live
  server is not a subset. [established]
- Key env vars: `BP_READER_BACKEND`, `BP_READER_READ_ONLY` (**default on**),
  `BP_READER_ALLOW_WRITE`, `BP_READER_ALLOW_TRANSPILE` (off), `BP_READER_DAEMON`,
  `BP_READER_DAEMON_IDLE_SECONDS`, `BP_READER_AUTO_CHECKOUT`,
  `BP_READER_PLUGIN_DENYLIST`, `BP_READER_TIMEOUT_SECONDS`, `BP_READER_LIVE_PORT/TOKEN`.
  [established]

## 3. Tool-availability contract (core requirement)
- **Every tool is registered and dispatches to a DEFINED response in every mode —
  never a `"not supported by this backend"` fallthrough.** [established as goal;
  audit shows 0 fallthroughs today across the full surface]
- **Every `IBlueprintReader` virtual is overridden in every production backend**
  (Commandlet, Socket/live, Auto, Caching, ReadOnly). The `AutoBlueprintReader`
  `FORWARD` is the critical one — a missing forward breaks the *default* backend even
  if commandlet/live implement it. There is **no compile-time guard**; each override
  is hand-written. [established]
- A tool must be reachable from **all** real backends, not just the commandlet
  daemon. Ops that exist only as commandlet EOps (`clone_graph`, `implement_interface`)
  are **not** "available" until wired as MCP tools through all backends. [session]
- **Mock is intentionally minimal**: read-only, BP-fixture-only. Editor/registry/
  non-BP-asset tools (~71) are declared in `UnsupportedTools()` and filtered at
  startup — a *defined* "unsupported," not a fallthrough. Mock is **not** expanded
  with fixtures to reach parity. [session]
- **Read-only by default**: write tools are rejected with a clear, actionable error
  unless `BP_READER_ALLOW_WRITE=1` / `BP_READER_READ_ONLY=0`. [established]
- **Transpile gated**: the 6 BP↔C++ tools return a structured
  `{ok:false, error:"transpile_disabled", hint:…}` unless `BP_READER_ALLOW_TRANSPILE=1`.
  [established]

## 4. Tool categories & per-mode expected behavior
Categories: `read`, `write` (asset-mutating), `live_only` (PIE/viewport/editor-state),
`transpile`, `meta`. Expected outcome per category × mode:

| category | mock | commandlet | live |
|---|---|---|---|
| read | success vs schema (or defined "unsupported" if non-BP) | success vs schema | success vs schema |
| write (asset) | mock read-only error | success vs schema | success vs schema |
| live_only | mock "unsupported" | defined error (no PIE world) or success | success vs schema |
| transpile (off) | `transpile_disabled` | `transpile_disabled` | `transpile_disabled` |
| any write + read-only guard on | reject | reject | reject |
[session]

## 5. Verification requirements
- **Always test against real data** — commandlet/live backend + real `.uassets`. Not
  compile + mock-suite + a gated test that auto-skips. Run the real path, report real
  results. [established — standing rule]
- **Every tool is tested AND full-shape-verified in every applicable mode** (mock,
  commandlet, live). Success results validate against the tool's declared
  `output_schema`; error cases assert the intended error shape. [session]
- **Regression guard**: a test that fails if any tool yields the fallthrough error on
  the `auto` backend. [session]
- Existing harness: `test_tools.cpp` (mock cases + tool-count assertion),
  `test_tool_smoke_live.cpp` (`BP_READER_SMOKE_ALL`, all tools, commandlet,
  reachability), `test_mcp.cpp` (handshake + count). Live-only cases auto-skip without
  env. [established]

## 6. Wire format & protocol invariants
- snake_case JSON keys; `BPNode.meta` is a real nested object; `null` for empty
  optional strings; **package paths** (`/Game/AI/BP_Foo`), never object paths.
  [established]
- Each `tools/call` envelope carries `_meta:{elapsed_ms, tool}`. [established]
- **BPIR** (versioned JSON AST) is the BP↔source pivot for decompile/transpile/parse.
  [established]

## 7. Recreation-fidelity requirement
- The read/write tools must recreate any gameplay BP **structurally identical**:
  `bp_structural_diff == 0`. The diff compares node/graph **names, types, topology** —
  NOT variable defaults, component property values, or interfaces. [established]
- Target: **all 76 gameplay-focused BPs → diff 0** (residuals: interface-BP type, cast
  input-pin wildcard, local macro graphs + composites, template input events). [session]
- Recreate into a **fresh** path; the headless `-nullrhi` daemon is the backend for
  bulk work (GPU live editor TDR-crashes under sustained batch; same-path re-clone hits
  GC-timing flake). [established this cycle]

## 8. Build/test invariants
- `LyraEditor.Target.cs`: `BuildSettingsVersion.V6`, `TargetBuildEnvironment.Unique`;
  launch `LyraEditor-Cmd.exe` (not `UnrealEditor-Cmd.exe`). [established]
- Three engine `.Build.cs` patches re-applied after a fresh engine clone. [established]
- On this machine: `-NoUba -MaxParallelActions=4`. `BP_READER_SKIP_PREBUILD=1` when the
  running MCP exe locks its relink. [established]
- **Adding/extending a tool** — the checklist: commandlet (`EOp` + `ParseOp` + dispatch
  + `RunFooOp`); `IBlueprintReader` virtual; `MockBlueprintReader`; **every** backend +
  decorator override (spot-check `grep -c`); `BlueprintTools.cpp` registration with
  input **and** output schema; mock + live tests; tool-count assertions in
  `test_tools.cpp` + `test_mcp.cpp`; `list_node_kinds` entry for node-spawning ops.
  [established]

## 9. Operational constraints
- Daemon is **TCP-only**; handshake files publish host/port/token; the auth frame must
  be **BOM-free UTF-8**. [established this cycle]
- **No proprietary downstream project names** in code/comments/messages — neutral UE
  placeholders only. [established — standing rule]
- Outward/irreversible actions (cook/package/delete/PIE in a shared editor) confirmed
  or guarded; destructive tools registration-checked, not blindly dispatched in tests.
  [established]

## 10. Source-of-truth precedence
1. `CLAUDE.md` — maintaining the project.
2. `Plugins/BlueprintReader/Claude/skills/bp-reader/SKILL.md` — using the tools.
3. This requirements doc — the decision contract.
4. The active plan file — current implementation steps.
5. Memory files — durable cross-session facts/preferences.
