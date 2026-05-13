# 08 — Errors & diagnostics

A tool error is the agent's only signal that something went wrong, so
the error model has to do two things: classify the failure precisely
enough that the agent can react, and carry enough context that the
agent doesn't need to re-drive the call to figure out what happened.

This document covers:

- The C++ exception hierarchy (`BlueprintReaderError`, `AssetNotFound`).
- The numeric exit codes the plugin commandlet uses.
- The MCP error envelope shape (`_meta.tool`, `_meta.args`,
  `_meta.elapsed_ms`).
- The classified write-failure diagnostics (file-lock probe,
  non-Blueprint misroute, uncompiled parent).
- The `apply_ops` cascade attribution (`cause: "upstream-slot-failed"`).
- Compile diagnostics with `op_index` correlation.
- Live backend self-refresh on connect-refused / auth-fail.

See [05 — Backends](05-backends.md) for which backend produces each
failure mode, [09 — Testing](09-testing.md) for the test cases that
pin each diagnostic.


## Exception model

`backends/IBlueprintReader.h:19-31` defines two exception types:

```cpp
class BlueprintReaderError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class AssetNotFound : public BlueprintReaderError {
public:
    using BlueprintReaderError::BlueprintReaderError;
};
```

`BlueprintReaderError` is the backend-side error type. The MCP tool
dispatcher catches this (and `std::exception`) and wraps the message
into the MCP envelope. Tools should never let a raw exception escape;
the dispatcher catches `std::exception` as a safety net but `what()`
quality varies wildly across STL implementations and shouldn't be
the front-line agent-facing message.

`AssetNotFound` is a subclass so callers can distinguish "you typed
the path wrong" from "I couldn't talk to the engine". The commandlet
backend maps exit code 4 to this; the live backend maps `code=4` from
the op-result frame to it. Tools that take an asset path and want to
return a richer error (e.g. "did you mean X?") should catch
`AssetNotFound` specifically and let other exceptions pass through.


## Commandlet exit codes

The plugin's `RunOneOp` dispatch returns a small set of numeric exit
codes, documented per-op at the return sites in
`Plugins/BlueprintReader/Source/BlueprintReaderEditor/Private/BlueprintReaderCommandlet.cpp`.
The set is intentionally small; the structured detail goes to the
output JSON file, not the exit code.

| Code | Meaning                              | Plugin source |
|------|--------------------------------------|---------------|
| 0    | Success                              | every `EmitOk` path |
| 1    | Generic failure (bad args, parse)    | `return 1` sites at 778, 784, 864, 905, 936, 976, 989 |
| 3    | Crash / fatal in handler             | `return 3` (line 394) |
| 4    | Missing target (asset, graph, node)  | `return 4` sites at 803, 869, 871, 877, 908, 916, 939, 944, 998 |
| 5    | Compile or save failed               | `return 5` sites at 815, 828, 837, 886, 919, 952, 1011 |

`CommandletBlueprintReader::RunOpOneShot` reads the exit code and maps
back to exceptions (`CommandletBlueprintReader.cpp:480-490`):

```cpp
if (r.exitCode != 0) {
    std::string tail = TrimLines(r.stderrTail.empty() ? r.stdoutTail : r.stderrTail, 250);
    cleanup();
    if (r.exitCode == 4) {
        throw AssetNotFound(fmt::format(
            "commandlet reported missing target (exit=4); tail:\n{}", tail));
    }
    throw BlueprintReaderError(fmt::format(
        "commandlet exit={}; tail:\n{}", r.exitCode, tail));
}
```

The tail of stderr (capped at ~250 lines via
`TrimLines`) goes into the error message — this is what gives the
agent the actual UE log output. The cap matters: UE log lines can
contain non-ASCII content, and the tail-cutting in `AppendTail`
(`CommandletBlueprintReader.cpp:122-133`) splits at the next newline
to avoid leaving a half-UTF-8-codepoint in the message.

Codes 1 and 5 both surface as `BlueprintReaderError`; the agent has to
read the message to tell them apart. Splitting them into separate
exception types was considered but rejected — the tail content is what
agents actually use to react, and the numeric distinction adds little.


## MCP error envelope

Source: `Plugins/BlueprintReader/mcp-server/src/jsonrpc/Mcp.cpp`.

Every `tools/call` response — success or failure — carries an MCP
content envelope. On failure, an extra `_meta` block adds telemetry +
the original args. The dispatcher at `Mcp.cpp:131-158`:

```cpp
try {
    nlohmann::json toolResult = (*fn)(arguments);
    nlohmann::json meta = {
        {"elapsed_ms", elapsedMs()},
        {"tool", name},
    };
    return jr::Response::Ok(MakeToolTextContent(toolResult.dump(2),
        /*isError=*/false, std::move(meta)));
} catch (const std::exception& e) {
    nlohmann::json meta = {
        {"elapsed_ms", elapsedMs()},
        {"tool", name},
    };
    if (!arguments.empty()) meta["args"] = arguments;
    return jr::Response::Ok(MakeToolTextContent(
        fmt::format("tool error: {}", e.what()), /*isError=*/true, std::move(meta)));
}
```

A real error envelope:

```json
{
  "content": [{
    "type": "text",
    "text": "tool error: asset not found: /Game/AI/BP_Nope"
  }],
  "isError": true,
  "_meta": {
    "elapsed_ms": 47,
    "tool": "read_blueprint",
    "args": { "asset_path": "/Game/AI/BP_Nope" }
  }
}
```

The `_meta` field is per the MCP 2024-11-05 spec — clients that
recognize it surface the telemetry, clients that don't ignore it
silently. Field choices:

- `tool` — the tool name. Useful when an agent batches multiple
  `tools/call` invocations and needs to attribute failures.
- `elapsed_ms` — wall-clock time inside the handler. Spikes hint at
  daemon-cold-start vs. cached-read; the agent can use this to decide
  whether to retry vs. give up.
- `args` — the verbatim `arguments` payload. Echoing args means
  agents debugging a tool failure don't need to scroll back in the
  transcript to see what they passed. Args are reproduced **without
  filtering**; MCP tool args in this server don't carry credentials.

`MakeToolTextContent` at `Mcp.cpp:15-33` builds the envelope and
attaches `_meta` only when non-empty.


## Write-failure classification

Most write failures in UE are silent — `SavePackage` returns false and
the editor logs a generic warning. This is unhelpful for an agent that
can't see the log. The plugin runs three classified probes on the
write path so failures come back with an actionable cause.

### File-lock probe

When `CompileAndSaveBlueprint` fails its `SavePackage` step (most often
a Windows sharing violation because the editor is open), the plugin
runs a non-destructive Win32 probe to confirm. From
`BlueprintReaderCommandlet.cpp:570-617`:

```cpp
#if PLATFORM_WINDOWS
const FString LockHint =
    TEXT("close the editor before running commandlet writes, or set "
         "BP_READER_BACKEND=auto so writes route through the editor's "
         "in-process TCP server (issue #2)");
HANDLE Probe = ::CreateFileW(*FileName,
    GENERIC_READ | GENERIC_WRITE,
    0,                        // no share — fail if anyone has it open
    nullptr,
    OPEN_EXISTING,            // never create or truncate
    FILE_ATTRIBUTE_NORMAL,
    nullptr);
if (Probe == INVALID_HANDLE_VALUE) {
    const DWORD Err = ::GetLastError();
    if (Err == ERROR_SHARING_VIOLATION) {
        UE_LOG(LogBlueprintReader, Error,
            TEXT("SavePackage failed: %s — file is locked (sharing violation). %s"),
            *FileName, *LockHint);
    } else {
        UE_LOG(LogBlueprintReader, Error,
            TEXT("SavePackage failed: %s — could not open file for diagnostic probe "
                 "(Win32 error %lu). %s"),
            *FileName, Err, *LockHint);
    }
} else {
    ::CloseHandle(Probe);
    UE_LOG(LogBlueprintReader, Error,
        TEXT("SavePackage failed: %s — file opens cleanly, so this isn't a "
             "file-lock issue; check the editor log above for the underlying "
             "compile/serialization error"),
        *FileName);
}
#endif
```

Two careful invariants:

1. **`OPEN_EXISTING` not `CREATE_ALWAYS`.** The probe must never
   modify the file. An earlier version used `IPlatformFile::OpenWrite`
   which truncates on success — turning a non-lock save failure into
   asset corruption. The CLAUDE.md gotcha section flags this; the
   inline comment at `BlueprintReaderCommandlet.cpp:576-579` records
   the regression that drove the fix.
2. **No share flags.** `0` for `dwShareMode` means the probe fails
   with `ERROR_SHARING_VIOLATION` (32) when anyone else has the file
   open. That's the signal we want; a shared open would silently
   succeed and miss the lock.

The hint string names `BP_READER_BACKEND=auto` as the canonical
workaround — that's the path that routes writes to the live editor's
in-process server instead of fighting for the file lock.

### Non-Blueprint asset misroute

`FBlueprintIntrospector::DiagnoseFailedBlueprintLoad`
(`Plugins/BlueprintReader/Source/BlueprintReaderEditor/Private/BlueprintIntrospector.cpp:352-410`)
runs on every failed `LoadObject<UBlueprint>` to figure out *why* the
cast came back null. The most common cause: the asset on disk isn't a
Blueprint at all (issue #4). A DataAsset, DataTable, Curve, or
Material has the same `.uasset` extension; an agent that types the
wrong path lands on a perfectly-valid asset that nonetheless can't be
read by `bp-reader`.

```cpp
const UClass* AssetClass = Asset.GetClass();
if (AssetClass && !AssetClass->IsChildOf(UBlueprint::StaticClass()))
{
    UE_LOG(LogBlueprintReader, Error,
        TEXT("LoadBlueprint: %s — asset is %s, not a UBlueprint. bp-reader "
             "only handles Blueprint assets (UBlueprint / UWidgetBlueprint). "
             "Data Assets (UPrimaryDataAsset descendants), DataTables, Curves, "
             "Materials, etc. are not supported here — inspect them via the "
             "editor or raw asset serialization (issue #4)."),
        *AssetPath, *AssetClass->GetName());
    return;
}
```

The message names the actual class found (`UDataTable`, `UMaterial`,
…) so the agent knows immediately which other tool to call.

### Uncompiled parent class

`BlueprintIntrospector.cpp:392-404` — if the asset *is* a UBlueprint
but its parent class can't be loaded, that means the C++ module
declaring the parent isn't compiled into this editor build (issue #3).

```cpp
UClass* ParentClass =
    LoadObject<UClass>(nullptr, *ParentRef.ToString(), nullptr, LOAD_Quiet);
if (!ParentClass)
{
    UE_LOG(LogBlueprintReader, Error,
        TEXT("LoadBlueprint: %s — parent class %s could not be resolved. "
             "This typically means the C++ module declaring it is not compiled in "
             "this build (issue #3). Rebuild the project (Build.bat <TargetName> "
             "Win64 Development) before reading or writing this Blueprint."),
        *AssetPath, *ParentClassTag);
    return;
}
```

Again, an action-oriented message: `Build.bat <TargetName> Win64
Development` is the fix.

### Both paths funnel through one diagnostic

`DiagnoseFailedBlueprintLoad` is called from both the read path
(`BlueprintIntrospector.cpp:339-348`) and the write path
(`BlueprintReaderCommandlet.cpp:441`). The original PR added it only
to the write path; Codex review on PR #58 caught the miss, since
`read_blueprint` is the most common entry point for issues #3 and #4.


## `apply_ops` cascade attribution

Source: `mcp-server/src/tools/ApplyOps.cpp`.

`apply_ops` runs a sequence of write ops in one batch. The trouble:
ops can depend on each other through `$slot` references — `wire_pins`
might reference `$branchNode`, which was minted by an earlier
`add_node`. If the earlier op fails, the downstream op was getting a
generic "slot ... has not been bound" error that didn't say *why* the
slot was unbound.

Fix (issue #8) in `ApplyOps.cpp:706-733`: track which slots failed
and propagate the failure reason forward. The `failedSlotReasons` map
is keyed by slot id; when an op references a slot that's in this map,
the dispatcher short-circuits with a richer message
(`ApplyOps.cpp:714-733`):

```cpp
auto findFailedSlotRef = [&](const nlohmann::json& op) -> std::string {
    static constexpr const char* kRefFields[] = {
        "from_node", "to_node", "node_id",
    };
    for (const char* key : kRefFields) {
        auto it = op.find(key);
        if (it == op.end()) continue;
        auto slotId = ExtractSlotRefId(*it);
        if (!slotId.has_value()) continue;
        if (slots.find(*slotId) != slots.end()) continue;
        auto fIt = failedSlotReasons.find(*slotId);
        if (fIt != failedSlotReasons.end()) {
            return fmt::format(
                "field \"{}\" references slot \"${}\", which was supposed to be "
                "bound by an earlier op that failed: {}",
                key, *slotId, fIt->second);
        }
    }
    return {};
};
```

Cascaded ops surface in the per-op result array with a structured
`cause` field (`ApplyOps.cpp:753-759`):

```cpp
results.push_back({
    {"ok", false},
    {"op_index", i},
    {"error", cascadedReason},
    {"cause", "upstream-slot-failed"},
});
```

`cause: "upstream-slot-failed"` is the marker that distinguishes "this
op failed on its own merits" from "this op was poisoned by an earlier
failure". An agent retrying a failed `apply_ops` should:

- Look for the first non-cascade failure.
- Fix that.
- Re-run; cascaded failures downstream typically resolve.

The poison propagates: if a cascaded op itself names a slot via `id`,
that slot's failure reason is the cascade reason (lines 761-765), so
further downstream ops see the full chain rather than the original
root cause repeated.


## Compile diagnostics with `op_index`

Inside an `apply_ops` batch, every write op runs against the same BP
in-memory; the actual `CompileBlueprint` + `SavePackage` is deferred to
`EndBatch`. When the compile produces warnings or errors, they come
back tagged with a node GUID. `apply_ops` tracks which `op_index`
minted each node GUID (`ApplyOps.cpp:694-700`):

```cpp
// For diagnostic attribution: track which op_index minted each
// node_guid. Snapshot the slot map before each op, diff after.
// When EndBatch surfaces compile diagnostics tagged with a node_guid,
// we look up the op that produced that node and tag the diagnostic
// with `op_index` so the caller can attribute warnings to a specific
// batch operation.
std::map<std::string, std::size_t> guidToOpIndex;
```

After the batch flushes, the dispatcher walks the diagnostics and
attaches `op_index` to anything whose node GUID matches an entry in
the map (`ApplyOps.cpp:831-848`). For diagnostics whose subject is a
variable name (e.g. "unused local variable Foo"), no `op_index` is
attached — the comment at `ApplyOps.cpp:835-836` explains: "When the
node GUID isn't surfaced (e.g. compile warnings referenced by var
name), no op_index is attached."

The point of all this: an agent applying a 50-op batch that gets back
"compile warning: ..." can see *which* of its 50 ops introduced the
warning, instead of having to bisect.


## Live backend self-refresh

Source: `LiveBlueprintReader.cpp:176-301`.

When the editor restarts, two things can change: the ephemeral port
(new listener, different number) and the auth token (rotated each
launch). The MCP server caches the values from its initial handshake
read, so a stale cache would make every following call fail.

`EnsureConnected` (`LiveBlueprintReader.cpp:283-301`) handles both:

```cpp
void LiveBlueprintReader::EnsureConnected() {
    if (handshakeOk_) return;

    // Up to two attempts: first with current cfg_, second after re-
    // reading the handshake file if the first failed in a way that
    // a refresh could plausibly fix (connect-refused or auth-failed).
    AttemptResult r = TryConnectAndHandshake();
    if (!r.ok && r.retryWorthwhile && RefreshFromHandshakeFile()) {
        r = TryConnectAndHandshake();
    }
    if (!r.ok) {
        throw BlueprintReaderError(fmt::format(
            "LiveBlueprintReader: {} — is the editor running with "
            "BP_READER_LIVE_PORT/TOKEN published in Saved/bp-reader-live.json?",
            r.error));
    }
}
```

`AttemptResult::retryWorthwhile` (`LiveBlueprintReader.h:299-303`)
distinguishes failure modes:

| Failure                            | retryWorthwhile | Reasoning |
|------------------------------------|-----------------|-----------|
| Connect refused                    | true            | New ephemeral port likely; refresh and retry. |
| Hello-frame read failed            | false           | Whatever we connected to isn't the editor; refresh won't help. |
| Auth send failed                   | true            | Connection state may be off; refresh and retry. |
| `auth_fail` response               | true            | Token rotated on stable-port restart; refresh and retry. |

`RefreshFromHandshakeFile` (`LiveBlueprintReader.cpp:176-198`) returns
`true` only when the on-disk values actually differ from the cached
ones — same values = nothing to retry with. This avoids a spin loop
when the file exists but the editor is wedged.

The fallback error message names the env vars (`BP_READER_LIVE_PORT`,
`BP_READER_LIVE_TOKEN`) and the handshake file
(`Saved/bp-reader-live.json`) so the agent has both the env-override
escape hatch and the auto-discovery file path to inspect.


## Putting it together

A real failure cascade an agent might see:

```
tool error: apply_ops failed at op[7] (op="wire_pins"): field "from_node"
references slot "$branchNode", which was supposed to be bound by an
earlier op that failed: op[3] failed: AddNode: K2Node_IfThenElse:
compile error in graph 'TakeDamage': pin 'Condition' type mismatch
(expected bool, got int)
```

The chain reads bottom-up:

1. Op 3 (`add_node` for a `K2Node_IfThenElse`) failed due to a type
   mismatch — the actionable root cause.
2. Op 7 (`wire_pins`) referenced `$branchNode`, the slot op 3 was
   going to mint.
3. `apply_ops` short-circuited op 7 with `cause: "upstream-slot-failed"`,
   surfacing the root cause inline.
4. The MCP envelope around all of this includes `_meta.tool: "apply_ops"`,
   `_meta.elapsed_ms`, and `_meta.args` with the full op list.

The agent reads this and fixes op 3 (probably casting an int to bool
in the BPIR producer or using the right type literal); no further
investigation needed.

See [09 — Testing](09-testing.md#diagnostics) for which tests pin each
of these diagnostics and how to add new ones.
