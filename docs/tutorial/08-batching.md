# Chapter 8 — Batching ops into one tool call

The graph you built at the end of Chapter 7 took five MCP round
trips: three `add_node` calls and two `wire_pins`. Every one of
those is a child editor process launch, a load, a mutation, a
compile, and a save. Even ignoring the editor-startup cost (which
Chapter 9 fixes), you still pay five compiles and five saves for
work that's conceptually a single edit.

This chapter introduces `apply_ops`: one MCP call carrying a
sequence of write ops, executed under a single compile + save. Along
the way you'll build named-slot resolution (so ops can reference
GUIDs minted by earlier ops in the same batch), failure-cascade
diagnostics, and a dry-run sibling `preview_ops`.

See [../design/04-mcp-server.md](../design/04-mcp-server.md) for the
tool-registry / response-control mechanics this builds on, and
[../design/08-error-diagnostics.md](../design/08-error-diagnostics.md)
for the cascade-diagnostic design.

## What we're solving

A multi-step refactor — "add a Health variable, then add a function
that reads it, then add an event that calls the function" — falls
naturally into:

```
add_variable Health float
add_function GetHealth
add_node     in GetHealth: VariableGet for Health
wire_pins    from VariableGet.Health to FunctionResult.ReturnValue
```

Naively that's four tool calls. Each one:

1. Loads BP_TestEnemy from disk into the editor.
2. Mutates.
3. Calls `CompileBlueprint` (100ms - 2s depending on BP size).
4. Calls `SavePackage` (more disk I/O).

Four loads, four compiles, four saves — when one of each would do.
And the agent has to thread node GUIDs through its own reasoning
(remember the variable's new ID, remember the function's entry node
ID, remember the VariableGet node's output pin GUID), which is
exactly the kind of state-keeping LLMs are bad at.

## Plugin-side: BeginBatch / EndBatch

We extend the `IBlueprintReader` interface with two methods:

```cpp
virtual void BeginBatch() = 0;
virtual nlohmann::json EndBatch(bool skipCompile = false) = 0;
```

The contract: between `BeginBatch` and `EndBatch`, every write op
defers its `CompileAndSaveBlueprint` call. `EndBatch` flushes —
compiles and saves each touched BP exactly once.

The plugin-side implementation is a single flag and a set:

```cpp
bool& BatchDeferFlag()       { static bool b = false;        return b; }
TSet<UBlueprint*>& Pending() { static TSet<UBlueprint*> s;   return s; }

bool MaybeCompileAndSave(UBlueprint* BP)
{
    if (BatchDeferFlag()) { Pending().Add(BP); return true; }
    return CompileAndSaveBlueprint(BP);
}
```

Every write op from Chapter 6 onward already calls
`MaybeCompileAndSave`. Outside a batch the flag is false and the
call compiles + saves inline. Inside a batch the flag is true and
the BP is queued. `EndBatch` walks the pending set, calls
`CompileAndSaveBlueprint` per BP collecting diagnostics, and emits
`{ recompiled, error_count, warning_count, diagnostics }`.

This is daemon-scoped (Chapter 9 handles the daemon lifecycle).
One-shot mode never opens a batch — that's a property of the MCP
server's `apply_ops` driver, not of the plugin.

## MCP-side: the apply_ops driver

The driver in `Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/tools/ApplyOps.cpp` owns:

- A RAII guard that calls `BeginBatch` on entry and `EndBatch` on
  scope exit.
- A `SlotMap` that maps user-defined slot ids to the GUIDs minted
  by each op.
- An op dispatcher (`DispatchOp`) and a per-op error policy.
- Diagnostic attribution: which op_index minted which GUID.
- The cascading-failure detector.

The skeleton:

```cpp
nlohmann::json RunOps(backends::IBlueprintReader& reader,
                      const nlohmann::json& ops, bool atomic,
                      std::string_view onFailure)
{
    // Resolve on_failure once. Unknown values fall back with a clear
    // error so misspellings don't silently change behavior.
    bool skipOnFailure = false;
    if (onFailure == "compile" || onFailure.empty()) skipOnFailure = false;
    else if (onFailure == "skip")                    skipOnFailure = true;
    else throw std::invalid_argument(fmt::format(
        R"(unknown on_failure value "{}" - supported: "compile" | "skip")",
        onFailure));

    SlotMap slots;
    nlohmann::json results = nlohmann::json::array();
    int succeeded = 0, failed = 0;

    struct BatchGuard {
        backends::IBlueprintReader& r;
        bool active = true;
        bool skipOnEarlyExit = false;
        BatchGuard(backends::IBlueprintReader& r_) : r(r_) { r.BeginBatch(); }
        ~BatchGuard() {
            if (active) {
                try { (void)r.EndBatch(skipOnEarlyExit); } catch (...) {}
            }
        }
        void release() { active = false; }
    };
    BatchGuard guard(reader);

    // ... dispatch loop ...

    nlohmann::json flushAck = reader.EndBatch(/*skipCompile=*/false);
    guard.release();
    // ... assemble output ...
}
```

The guard's destructor handles every early-exit path: dispatcher
throws under `atomic: true`, bug in a per-op handler, even a panic
that triggers stack unwinding. `EndBatch` always runs so the
plugin's `BatchPending` state is cleared and we don't leave the
daemon in batch mode forever.

## Named slots: minting and resolving GUIDs

The agent doesn't get to see GUIDs between ops in a batch — there's
only one tool call, no intermediate responses. So we let each
node-spawning op carry an `id` field:

```json
{
  "ops": [
    {"op": "add_node", "id": "branch", "asset_path":"/Game/X", "graph_name":"EventGraph",
     "kind":"Branch", "x":300, "y":0},
    {"op": "add_node", "id": "print",  "asset_path":"/Game/X", "graph_name":"EventGraph",
     "kind":"CallFunction", "function":"PrintString",
     "function_owner":"/Script/Engine.KismetSystemLibrary",
     "x":600, "y":0},
    {"op": "wire_pins", "asset_path":"/Game/X", "graph_name":"EventGraph",
     "from_node":"$branch", "from_pin":"True",
     "to_node":"$print", "to_pin":"execute"}
  ]
}
```

Subsequent ops use either `"$<id>"` (the sigil form) or
`{"ref":"<id>"}` (the explicit form) anywhere a node GUID is
expected. Both inflate to the GUID minted earlier.

### ExtractSlotRefId

Inspection-only. Returns the slot id, the input is not a ref, or
nullopt for malformed shapes. We split it out from resolution so
the failed-slot detector below can peek without throwing:

```cpp
std::optional<std::string> ExtractSlotRefId(const nlohmann::json& field) {
    if (field.is_object()) {
        auto it = field.find("ref");
        if (it != field.end() && it->is_string()) {
            return it->get<std::string>();
        }
        return std::nullopt;
    }
    if (field.is_string()) {
        const auto& s = field.get_ref<const std::string&>();
        if (!s.empty() && s.front() == '$') {
            return s.substr(1);
        }
    }
    return std::nullopt;
}
```

### ResolveNodeRef

Does the actual lookup. Three shapes are accepted, in priority order:

1. `{"ref": "<id>"}` — explicit slot ref
2. `"$<id>"` — sigil-prefixed slot ref
3. raw GUID string — passes through unchanged

```cpp
std::string ResolveNodeRef(const nlohmann::json& field, const SlotMap& slots,
                           std::string_view key) {
    if (auto slotId = ExtractSlotRefId(field); slotId.has_value()) {
        auto sit = slots.find(*slotId);
        if (sit == slots.end()) {
            if (field.is_string()) {
                throw std::invalid_argument(fmt::format(
                    R"(field "{}" references slot "${}" which has not been bound)",
                    key, *slotId));
            }
            throw std::invalid_argument(fmt::format(
                R"(field "{}" references slot "{}" which has not been bound - )"
                "did an earlier op fail or did you typo the id?", key, *slotId));
        }
        return sit->second;
    }
    if (field.is_object()) {
        throw std::invalid_argument(fmt::format(
            "field '{}' is an object but lacks a string \"ref\"", key));
    }
    if (field.is_string()) {
        return field.get_ref<const std::string&>();  // raw GUID passthrough
    }
    throw std::invalid_argument(fmt::format(
        "field '{}' must be a string GUID, a $slot reference, or {{\"ref\":\"<id>\"}}",
        key));
}
```

Binding the other direction is one line at the end of each
node-minting op:

```cpp
if (auto idIt = op.find("id"); idIt != op.end() && idIt->is_string()) {
    slots[idIt->get<std::string>()] = newId;
}
```

`add_function` also binds a slot — to its entry node's GUID — so a
later op can wire from `FunctionEntry.then` without a follow-up
`get_function` read.

## Per-op error policy

`atomic: true` (default) bails on the first failure. `atomic: false`
continues, marking each failed op in the results array. The
dispatch loop:

```cpp
for (std::size_t i = 0; i < ops.size(); ++i) {
    const auto& op = ops[i];
    std::size_t prevSlotCount = slots.size();

    // (cascade check goes here — see below)

    try {
        results.push_back(DispatchOp(reader, op, slots));
        ++succeeded;
        if (slots.size() > prevSlotCount) {
            for (const auto& [slotId, guid] : slots) {
                if (!guid.empty() && guidToOpIndex.find(guid) == guidToOpIndex.end()) {
                    guidToOpIndex[guid] = i;
                }
            }
        }
    } catch (const std::exception& e) {
        ++failed;
        if (auto idIt = op.find("id"); idIt != op.end() && idIt->is_string()) {
            failedSlotReasons[idIt->get<std::string>()] = fmt::format(
                "op[{}] failed: {}", i, e.what());
        }
        if (atomic) {
            throw bpr::backends::BlueprintReaderError(fmt::format(
                "apply_ops failed at op[{}] (op=\"{}\"): {}",
                i,
                op.contains("op") && op["op"].is_string()
                    ? op["op"].get<std::string>() : "<missing>",
                e.what()));
        }
        results.push_back({
            {"ok", false},
            {"op_index", i},
            {"error", e.what()},
        });
    }
}
```

Three things to notice here.

First, `guidToOpIndex` is the diagnostic attribution table.
Whenever `slots` grows, we record which `op_index` minted each new
GUID. When `EndBatch` returns compile diagnostics tagged with a
`node_guid`, we look up the minting op and tag the diagnostic with
its `op_index` — so callers can correlate "warning X on node Y" to
"the wire_pins op at index 7".

Second, `failedSlotReasons` is the cascade table. When an op that
mints a slot fails, we record *why* so downstream ops referencing
that slot get a richer error than "slot ... has not been bound".

Third, the `op_index` field is what makes per-op results
debuggable. Without it, an `atomic: false` batch that fails three
ops in the middle gives you no way to pair the per-op errors back
to the input ops.

## Cascade detection

The naive failure case: op 3 spawns a node with `id: "branch"`, op
3 fails, op 5 tries to wire from `$branch`. Op 5's
`ResolveNodeRef` throws "slot branch has not been bound" — true but
unhelpful, because the real cause is two ops earlier.

The fix is a pre-check before each dispatch:

```cpp
auto findFailedSlotRef = [&](const nlohmann::json& op) -> std::string {
    static constexpr const char* kRefFields[] = {
        "from_node", "to_node", "node_id",
    };
    for (const char* key : kRefFields) {
        auto it = op.find(key);
        if (it == op.end()) continue;
        auto slotId = ExtractSlotRefId(*it);
        if (!slotId.has_value()) continue;     // raw GUID or malformed
        if (slots.find(*slotId) != slots.end()) continue;  // already bound
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

If the pre-check fires, we short-circuit with a `cause:
"upstream-slot-failed"` tag so downstream tooling can group the
cascade:

```cpp
std::string cascadedReason = findFailedSlotRef(op);
if (!cascadedReason.empty()) {
    ++failed;
    if (atomic) throw bpr::backends::BlueprintReaderError(/* ... */);
    results.push_back({
        {"ok", false},
        {"op_index", i},
        {"error", cascadedReason},
        {"cause", "upstream-slot-failed"},
    });
    // Propagate forward if this op itself names a slot.
    if (auto idIt = op.find("id"); idIt != op.end() && idIt->is_string()) {
        failedSlotReasons[idIt->get<std::string>()] = cascadedReason;
    }
    continue;
}
```

The pre-check uses the shared `ExtractSlotRefId` helper rather than
duplicating its parsing rules, so future ref shapes get cascade
detection automatically.

## EndBatch failure semantics

`on_failure` controls what `EndBatch` does after a mid-batch failure:

- `"compile"` (default): best-effort. The plugin compiles and saves
  whatever ops landed before the failure. The agent sees a partial
  success and can decide what to do next.
- `"skip"`: don't compile, don't save. Nothing reaches disk. The
  in-memory daemon state stays dirty until restart, so subsequent
  reads in the same session can see the partial mutations — that's
  a documented limitation of strict-atomic mode in v1.

`"rollback"` is reserved but not implemented; doing it properly
would need plugin-side `FScopedTransaction` support so we can
revert via the editor's undo stack.

## Lifting compile diagnostics to the response

`EndBatch` returns an ack object with `diagnostics`, `error_count`,
`warning_count`, and `recompiled`. After the dispatch loop completes
we lift those to the top-level response. For each diagnostic that
carries a `node_guid`, look up the minting op in `guidToOpIndex` and
tag the diagnostic with that `op_index` — so callers see "warning X
on node minted by op[7]" instead of an orphan node ID.

The `recompiled` field is the batch's receipt: the list of BP paths
that actually compiled and saved. For the agent it's confirmation;
for tests it's an assertion target ("apply_ops should have
recompiled exactly this one BP").

## preview_ops: dry run

The `preview_ops` tool walks the same op array but never calls a
write method. `ValidateOps` mirrors `RunOps`: each op gets
validated for required fields, slot refs resolve against placeholder
GUIDs (`00000000-0000-0000-0000-NNNNNNNNNNNN`), and read-only
backend calls confirm referenced variables and functions exist.
The response carries per-op `{ok}` rows, the slot map, and a
`would_compile` array listing every BP path the real `apply_ops`
would touch.

Two consumers in mind:

1. **Agent self-checks** — "is this batch syntactically valid before
   I run it?"
2. **Human-in-the-loop confirmation** — show the user
   `would_compile` and per-op intent before committing.

## Registering apply_ops

`BlueprintTools.cpp` gets one more entry, registered against the
`apply_ops` name with `ops` (required), `atomic` (default true),
and `on_failure` (`"compile"` or `"skip"`) in the input schema. The
handler validates that `ops` is an array, pulls `atomic` and
`on_failure` from the args, and dispatches to `RunOps`. A sibling
entry for `preview_ops` dispatches to `ValidateOps` the same way.

## Checkpoint

Build, then drive `apply_ops` end to end:

```pwsh
<removed in the UBT migration; use BlueprintReaderMcpTests.exe instead> -Tool apply_ops -Args @'
{
  "ops": [
    {"op": "create_blueprint",
     "asset_path": "/Game/AI/BP_BatchDemo",
     "parent_class": "/Script/Engine.Actor"},

    {"op": "add_variable",
     "asset_path": "/Game/AI/BP_BatchDemo",
     "name": "Score", "type": "int"},

    {"op": "add_function", "id": "scoreFn",
     "asset_path": "/Game/AI/BP_BatchDemo",
     "name": "GetScore"},

    {"op": "add_function_output",
     "asset_path": "/Game/AI/BP_BatchDemo",
     "function_name": "GetScore",
     "param_name": "Value", "type": "int"},

    {"op": "add_node", "id": "getScore",
     "asset_path": "/Game/AI/BP_BatchDemo",
     "graph_name": "GetScore",
     "kind": "VariableGet", "variable": "Score",
     "x": 200, "y": 0}
  ]
}
'@
```

Expect a response like:

```json
{
  "ok": true,
  "succeeded": 5,
  "failed": 0,
  "slots": { "scoreFn": "...", "getScore": "..." },
  "recompiled": ["/Game/AI/BP_BatchDemo.BP_BatchDemo"],
  "compile_errors": 0,
  "compile_warnings": 0,
  "results": [ /* per-op results */ ]
}
```

Now confirm it landed:

```pwsh
<removed in the UBT migration; use BlueprintReaderMcpTests.exe instead> `
    -Tool read_blueprint -Args '{"asset_path":"/Game/AI/BP_BatchDemo"}'
```

The response should show one variable (`Score`), one function
(`GetScore`) with one output (`Value`), and the graph containing a
`VariableGet` node bound to `Score`. Five writes, one compile, one
save — and the agent never had to remember a GUID.

That's the basic apply_ops surface. The next chapter cuts the
per-call latency from seconds to milliseconds by reusing one
long-lived editor process instead of spawning a new one per call.
