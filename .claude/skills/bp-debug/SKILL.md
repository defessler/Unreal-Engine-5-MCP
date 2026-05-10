---
name: bp-debug
description: Use this skill when a bp-reader MCP tool call has failed and the user wants to know why, when symptoms suggest infrastructure issues (slow calls, daemon crashes, mock backend write rejections), or when planning recovery from a partial multi-step write. Triggers on phrases like "why did that fail", "the call returned an error", "AssetNotFound", "exit code 5", "mock backend is read-only", "daemon timed out", or any reported tool error. Skip for normal BP edit/read flows (use bp-reader / bp-batches / bp-cpp).
---

# bp-debug — diagnosing bp-reader failures

When a tool call fails, the message is usually self-explanatory. This
skill covers the patterns where it isn't, the recovery moves, and
when the failure points at infra rather than user error.

## First-pass triage

Pull these signals from the failure response before guessing:

1. **Error class name** in the message — `AssetNotFound`,
   `BlueprintReaderError`, `CppParseError`, `std::invalid_argument`.
2. **Exit code in the tail** if it's a commandlet failure
   (`exit=N; tail: ...`). UE convention:
   - `1` — generic UE failure (bad command line, missing module).
   - `3` — crashed during op (callstack will be in the tail).
   - `4` — asset / graph / function not found.
   - `5` — compile error after a write op.
3. **`_meta.elapsed_ms`** in the MCP envelope — the per-call timing.
4. **Backend in use** — call `tools/list` and look at server stderr if
   you can't tell whether you're on `auto` / `live` / `commandlet` /
   `mock`.

## Common errors → fixes

### `AssetNotFound`

The asset / graph / function / node doesn't exist. Treat as user
error first.

- Verify the path is a **package path** (`/Game/AI/BP_Foo`), not an
  object path (`/Game/AI/BP_Foo.BP_Foo`) and not a disk path.
- For `get_function` / `get_graph`, confirm the name with
  `read_blueprint` first — function and graph names are case-sensitive.
- For `get_node`, confirm the GUID with `find_node` first — pin GUIDs
  vs node GUIDs are easy to swap.

### `BlueprintReaderError: ...mock backend is read-only...`

The MCP server is running with `BP_READER_BACKEND=mock` and the user
asked for a write tool. The mock backend serves fixtures only. Two
real fixes:

- Set `BP_READER_BACKEND=auto` or `commandlet` (and the engine /
  project env vars).
- Or accept that this session can't write — surface that to the user
  and ask whether they want to switch backends.

Don't try to fake the write. Mock fixtures are read-only by design.

### `commandlet exit=5; tail: <compile errors>`

A write op caused a compile failure. The save **does not commit** —
the on-disk `.uasset` is unchanged. But the daemon's in-memory state
may be partial. Recovery:

- Re-read the BP (`read_blueprint` / `get_graph`) to see what landed.
- For `apply_ops` failures, inspect `diagnostics[]` — `op_index`
  attributes the failure to a specific op.
- If the BP is half-built in memory, either fix forward with another
  `apply_ops` or call `shutdown_daemon` and re-issue (next call cold-
  starts the editor with the on-disk state).

### `commandlet exit=3` with an external-plugin callstack

A plugin's `OnAssetRegistryLoadComplete` handler crashed (Niagara,
Animation, Substance, Wwise — anything that loads on registry-scan
broadcast). Mostly fixed in current builds:

- `list_blueprints` uses `ScanPathsSynchronous({pathFilter})` instead
  of `SearchAllAssets` since a recent build, so the broadcast doesn't
  fire.
- If you're on an older build, narrow the `path` argument to a content
  folder that doesn't reference the offending plugin's assets.
- If the crash persists on a current build, it's a different
  `LoadObject` somewhere in the read path — escalate with the full
  stack and the asset path that reproduces.

### Daemon timeout / "daemon timed out reaching READY"

The editor daemon couldn't finish its initial startup before
`BP_READER_STARTUP_TIMEOUT_SECONDS` (default 600). Big projects (lots
of plugins, large content tree, cold DDC) can take 5–10 minutes the
first time.

- Bump `BP_READER_STARTUP_TIMEOUT_SECONDS=1200` in the MCP server's
  env if you're on a cold project.
- Check the server's stderr — startup timeouts usually have a
  diagnostic line about which plugin / module is taking the time.
- The daemon falls back to one-shot mode for that call. The next call
  retries the daemon spawn from scratch.

### Live backend "no editor running" / connection refused

`BP_READER_BACKEND=live` is set explicitly but no editor is listening.
- If the user expects an editor to be open, confirm
  `<ProjectDir>/Saved/bp-reader-live.json` exists and the listed pid
  matches a running editor.
- If the user wants automatic fallback to commandlet when no editor
  is open, switch to `BP_READER_BACKEND=auto` (or unset
  `BP_READER_BACKEND` — auto is the default when a `.uproject` is
  found).

### `wire_pins` schema rejection

The error includes both pin types so the agent can self-correct in
one turn:

```
WirePins: schema rejected the connection [from_pin type=object(Actor),
to_pin type=bool]
```

- Most fixes are inserting a Cast or a conversion node.
- For object → object cross-class, `add_node kind=Cast target_class=...`
  and wire through it.
- For pin-name vs pin-GUID confusion: pin names are case-sensitive and
  match the editor display. Use the GUIDs from `add_node`'s response
  instead if names aren't matching.

### `compile_function`: "unrecognized statement form"

The pseudocode body had a key the dispatcher doesn't recognize. The
error names the form. Two common causes:

- Typo in a key (`{ifs: ...}` instead of `{if: ...}`).
- Using a BPIR-only form that `compile_function` doesn't lower
  directly (`for_each`, `while`, `switch`, `cast` statement form).
  If you need those, run via `decompile`/`transpile` round-trip
  through C++ instead — the BPIR support is for the read direction
  today.

### `parse_cpp_function`: `<line>:<col>: <message>`

The parser rejected the C++ source. Format is exact: line + column +
message. Common causes:

- Unsupported syntax (lambdas, templates beyond `Cast<T>`, raw pointer
  arithmetic). The error message names the construct.
- Preprocessor directives (`#include`, `#define`) — pass a bare body,
  not a translation unit.

If the user is round-tripping via `transpile_function` →
`parse_cpp_function`, identity is pinned for the patterns CppEmit
emits — a parse failure on round-tripped output is a bug, surface it.

### `_meta.elapsed_ms` is unexpectedly high (>1 s on a hot session)

Daemon transport probably fell back to one-shot. The MCP server
recovers automatically (next call respawns the daemon), so this is
almost always self-healing — but if it persists, inspect the server's
stderr.

## Recovery patterns

### Partial multi-step write landed, want to roll back

`apply_ops` with `atomic: false` or `on_failure: "compile"` may leave
a half-built BP. Options:

1. **Fix forward** — another `apply_ops` to add the missing pieces.
   Often easier than rollback because individual ops are idempotent.
2. **Targeted rollback** — `delete_function` / `delete_variable` /
   `delete_node` for the specific things that landed. Re-read the BP
   first to confirm what's actually there.
3. **Asset-level rollback** — if the BP was created in this batch
   (`create_blueprint` op), and it's now broken, the simplest path
   is to delete the asset (manually in the editor — no
   `delete_blueprint` MCP tool).

### "It worked once and now fails"

- Daemon may have crashed → next call respawns; one transient failure
  is usually a non-issue.
- If the error is "asset not found" but the asset exists, the daemon's
  cached asset registry may be stale → call `shutdown_daemon` to
  release the editor and let the next call re-spawn fresh.

### "Tool call works but returns nothing useful"

- Check `fields` projection — over-aggressive `fields` may have
  dropped what you care about.
- Some empty-result returns are correct (e.g. `find_node` with no
  matches). Ask the user to confirm before assuming a bug.

## When to escalate

If you've ruled out user error, mock-backend confusion, and the
common patterns above, and the failure persists across `shutdown_daemon`
+ retry:

- Capture the request, the response (with `_meta`), and the server's
  stderr tail.
- Suggest the user open an issue at
  github.com/defessler/Unreal-Engine-5-MCP/issues with that bundle.
