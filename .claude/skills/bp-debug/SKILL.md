---
name: bp-debug
description: Use this skill when a bp-reader MCP tool call has failed and the user wants to know why, when symptoms suggest infrastructure issues (slow calls, daemon crashes, mock backend write rejections), or when planning recovery from a partial multi-step write. Triggers on phrases like "why did that fail", "the call returned an error", "AssetNotFound", "exit code 5", "mock backend is read-only", "daemon timed out", "file is locked", or any reported tool error. Skip for normal BP edit/read flows (use bp-reader / bp-batches / bp-cpp).
---

# bp-debug — diagnosing bp-reader failures

## First-pass triage

Pull these signals from the failure before guessing:

1. **Error class name** — `AssetNotFound`, `BlueprintReaderError`,
   `CppParseError`, `std::invalid_argument`.
2. **Commandlet exit code** (in the tail of commandlet errors):
   - `1` — generic UE failure (bad command line, missing module).
   - `3` — crashed during op (callstack in the tail).
   - `4` — asset / graph / function / node not found.
   - `5` — compile error after a write op.
3. **`_meta.elapsed_ms`** in the MCP envelope — per-call timing.
4. **Backend in use** — `tools/list` + server stderr; or set
   `BP_READER_BACKEND` explicitly while debugging.
5. **Which MCP server / project** — the exe is now
   `bp-reader-mcp-<ProjectName>.exe` (e.g. `bp-reader-mcp-UE5_MCP.exe`)
   alongside the canonical `bp-reader-mcp.exe` hard link. Check
   `tasklist /V` or `Get-Process` to confirm which project the
   server you're talking to belongs to.

## Multi-session triage

Multiple Claude/Copilot sessions can run against the same UE project
concurrently — the MCP server-level single-instance lock is gone. A
few things to know when debugging in that world:

- **One daemon per project, shared across sessions.** The commandlet
  daemon lives at `<Project>/Saved/bp-reader-cmdlet.json` (handshake)
  with `bp-reader-cmdlet.lock` (lifetime lock) and
  `bp-reader-cmdlet-spawn.lock` (held only during a spawn attempt).
  If you see "daemon already running" symptoms, check the handshake
  file's `pid` against `Get-Process UnrealEditor-Cmd`.
- **`shutdown_daemon` is global now.** Calling it terminates the
  daemon used by every session against this project, not just the
  caller's. Other sessions whose next call needs commandlet mode
  will transparently spawn a fresh daemon. Mention this if a user
  asks "why did my other Claude window suddenly slow down?"
- **Identifying your MCP server.** The exe is renamed at build time:
  a project named `UE5_MCP` builds `bp-reader-mcp-UE5_MCP.exe`
  alongside the canonical `bp-reader-mcp.exe` hard link. Existing
  `.mcp.json` configs that reference the canonical name keep working.
  `Get-Process bp-reader-mcp*` shows one per active session.
- **Cross-session batch isolation is partial.** Per-connection state
  (defer flag, pending-compile set) is isolated correctly. The
  per-BP write lock (Task 4.3) and commit-partial-on-disconnect
  policy (Task 4.4) are **deferred**. If you see weird
  interleaved-edit symptoms when two sessions issue `apply_ops`
  batches on the SAME BP concurrently, this is the gap. Workaround:
  serialize batches manually across sessions if it matters.
- **Daemon idle shutdown.** With no clients connected for
  `BP_READER_DAEMON_IDLE_SECONDS` (default 300), the daemon exits
  cleanly and deletes its handshake. Next call cold-starts a fresh
  one. If you see unexpected ~7 s elapsed-ms on a call that should
  be hot, this is probably why.

## Common errors → fixes

### `AssetNotFound`
Asset / graph / function / node doesn't exist. Treat as user error first.
- Use **package paths** (`/Game/AI/BP_Foo`), not object paths.
- Function and graph names are case-sensitive; confirm via
  `read_blueprint`.
- For `get_node`, confirm the GUID via `find_node` — pin GUIDs vs
  node GUIDs are easy to swap.

### Commandlet write failed (three classified messages)
Recent diagnostic work classifies write failures into three actionable
cases. Look at the daemon stderr tail:

- **"file is locked (sharing violation)"** — editor is open holding
  the `.uasset`. Either close the editor, or set
  `BP_READER_BACKEND=auto` so writes route through the editor's
  in-process TCP server.
- **"asset is `<ClassName>`, not a UBlueprint"** — bp-reader handles
  Blueprint assets only. Data Assets, DataTables, Curves, Materials
  aren't supported. Inspect them via the editor or raw asset
  serialization.
- **"parent class `<Path>` could not be resolved"** — the asset
  references a C++ class whose module isn't compiled in this build.
  Rebuild the project (`Build.bat <TargetName> Win64 Development`)
  before retrying.

There's also a fourth case ("file opens cleanly, so this isn't a
file-lock issue") that defers to the editor log for the underlying
compile/serialization error.

### `BlueprintReaderError: ...mock backend is read-only...`
Mock backend serves fixtures only. Set
`BP_READER_BACKEND=auto` / `commandlet` (and the engine + project env
vars), or accept that this session can't write and surface that to the
user. Don't fake the write.

### `commandlet exit=5; tail: <compile errors>`
A write op caused a compile failure. **The save doesn't commit** —
the on-disk `.uasset` is unchanged. But the daemon's in-memory state
may be partial.
- Re-read the BP (`read_blueprint` / `get_graph`) to see what landed.
- For `apply_ops`, inspect `diagnostics[]` — `op_index` attributes
  the failure to a specific op.
- For cascaded failures, look for the first `results[i]` with
  `ok:false` whose `cause` is NOT `"upstream-slot-failed"` — that's
  the root.
- If the in-memory state is bad, `shutdown_daemon` and the next call
  cold-starts the editor with on-disk state. (Note: shared daemon —
  this affects every session against this project, not just yours.)

### `commandlet exit=3` with a plugin callstack
A plugin's `OnAssetRegistryLoadComplete` handler crashed (Niagara,
Animation, Substance, Wwise — anything that loads on registry-scan
broadcast).
- Current builds use `ScanPathsSynchronous({pathFilter})` so the
  broadcast doesn't fire — should be fixed.
- On older builds, narrow the `path` argument to a content folder
  that doesn't reference the offending plugin.
- If it still crashes on current builds, capture the stack + asset
  path and escalate.

### Daemon timeout / "daemon timed out reaching READY"
Editor couldn't finish startup before `BP_READER_STARTUP_TIMEOUT_SECONDS`
(default 600). Big projects can take 5–10 min cold.
- Bump to `1200` for cold projects.
- Check stderr — startup timeouts usually name which plugin / module
  is slow.
- The daemon falls back to one-shot for that call; next call retries
  the daemon spawn from scratch.

### Live backend "connection refused" / "auth failed"
The live backend now **self-refreshes** the handshake file
(`<ProjectDir>/Saved/bp-reader-live.json`) on both connect-refused and
auth-fail, then retries once. So:
- If you see a one-off failure followed by success, the recovery
  worked — editor probably restarted between calls.
- If failures persist, the editor isn't running OR the handshake
  file is stale (e.g. crashed editor). Check the file's `pid` vs
  running processes.
- For automatic fallback to commandlet when no editor is open, use
  `BP_READER_BACKEND=auto` (the default when a `.uproject` is
  auto-discovered).

### `wire_pins` schema rejection
Error includes both pin types so you can self-correct in one turn:

```
WirePins: schema rejected the connection [from_pin type=object(Actor),
to_pin type=bool]
```

- Most fixes: insert a Cast or conversion node.
- For object → object cross-class, `add_node kind=Cast target_class=...`
  and wire through it.
- Pin names are case-sensitive and match the editor display. If
  names aren't matching, use pin GUIDs from `add_node`'s response.

Note: `wire_pins` now routes through the K2 schema's
`TryCreateConnection`, which propagates wildcard pin types via
`NotifyPinConnectionListChanged`. Array library nodes (Array_Length,
Array_Random, Array_Get, …) concretize their wildcard pins on
connect — if a wire succeeded but the function won't compile with
"undetermined type", that's a different bug, not a `wire_pins`
schema rejection.

### `compile_function`: "unrecognized statement form"
Pseudocode body has a key the dispatcher doesn't recognize. Two
common causes:
- Typo in a key (`{ifs: ...}` for `{if: ...}`).
- BPIR-only form that `compile_function` doesn't lower today
  (`for_each`, `while`, `switch`, `cast`-as-statement). Round-trip
  via `transpile_function` / `parse_cpp_function` if you need those —
  BPIR support there is for the read direction.

### `parse_cpp_function`: `<line>:<col>: <message>`
Parser rejected the C++ source. Common causes:
- Unsupported syntax (lambdas, templates beyond `Cast<T>`, raw
  pointer arithmetic). The error names the construct.
- Preprocessor directives (`#include`, `#define`) — pass a bare body,
  not a translation unit.

If round-tripping `transpile_function` → `parse_cpp_function`, parse
failures on round-tripped output are a bug; surface them.

### `_meta.elapsed_ms` is unexpectedly high (>1 s on a hot session)
Daemon transport probably fell back to one-shot. Self-healing — next
call respawns the daemon. If it persists, inspect server stderr.

## `find_node` returns nothing for nodes I can see
`find_node` matches on class, rendered title, AND meta identifier
fields (`targetFunction` / `function_name` / `variableName` /
`eventName`). Pin labels are NOT searched. For "every place I read
Health", use `query:"Health"` + `kind:"VariableGet"`. There's no
`class_filter` argument — use `kind`.

## Recovery patterns

### Partial multi-step write landed — roll back
`apply_ops` with `atomic: false` or `on_failure: "compile"` may
leave a half-built BP. Options:

1. **Fix forward** — another `apply_ops` to finish. Individual ops
   are idempotent, so re-running the full batch usually converges.
2. **Targeted rollback** — `delete_function` / `delete_variable` /
   `delete_node` for the things that landed. Re-read first to
   confirm what's there.
3. **Asset-level rollback** — if `create_blueprint` was in the batch
   and the BP is broken, `delete_asset` removes it.

### "It worked once and now fails"
- Daemon may have crashed → next call respawns; one transient
  failure is usually a non-issue.
- If "asset not found" persists for an existing asset, the daemon's
  cached registry may be stale → `shutdown_daemon` and let the next
  call cold-start.

### "Tool returned but no useful data"
- Check `fields` — over-aggressive projection may have dropped what
  you wanted.
- Empty results can be correct (`find_node` with no matches). Confirm
  with the user before assuming a bug.

## When to escalate

If you've ruled out user error, mock-backend confusion, and the
common patterns above, and the failure persists across
`shutdown_daemon` + retry:

- Capture the request, the response with `_meta`, and the server's
  stderr tail.
- Suggest filing an issue at
  github.com/defessler/Unreal-Engine-5-MCP/issues with that bundle.
