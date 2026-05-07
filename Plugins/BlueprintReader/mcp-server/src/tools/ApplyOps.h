// ApplyOps — execute a batch of write operations in one tool call,
// resolving named slots so later ops can reference GUIDs minted by
// earlier ops without an extra round-trip.
//
// Ops are executed sequentially against the underlying IBlueprintReader.
// Each op is a JSON object whose `op` field selects the dispatch and
// whose remaining fields are forwarded as the op's arguments.
//
// Named slots: an op may carry an optional `id` field (caller-chosen
// string, scoped to this batch). For node-spawning ops (currently
// add_node), the new node's GUID is stored under that id. Subsequent
// ops that reference a node — wire_pins, set_node_position, delete_node
// — can substitute `{ "ref": "<id>" }` (or just the bare string) for a
// node-id field. The string form `"$<id>"` is also accepted for
// compactness:
//
//   { "op": "add_node", "id": "branch", "graph_name": "EventGraph",
//     "kind": "Branch", "x": 0, "y": 0 }
//   { "op": "wire_pins", "graph_name": "EventGraph",
//     "from_node": "$branch", "from_pin": "True",
//     "to_node":   "$call",   "to_pin":   "exec" }
//
// Atomicity: when `atomic: true`, a failure mid-batch raises and the
// caller knows the partial state may be inconsistent. The plugin-side
// commandlet currently saves+recompiles per-op, so true rollback would
// require plugin work; for now `atomic` just means "stop on first
// error" rather than "all-or-nothing." Documented as a v1 limitation.
//
// Returns: per-op results in an array, mirroring the input ops order.
// Each entry is `{ ok, ...op-specific }` or `{ ok: false, error: "..." }`
// when `atomic: false` (continue-on-error mode).

#pragma once

#include "tools/ToolRegistry.h"
#include "backends/IBlueprintReader.h"

#include <nlohmann/json.hpp>

namespace bpr::tools {

// Register the `apply_ops` tool against `registry`. Borrows `reader` for
// the lifetime of the registry (same convention as RegisterBlueprintTools).
void RegisterApplyOps(ToolRegistry& registry, backends::IBlueprintReader& reader);

// Execute a sequence of ops directly (no MCP framing). Same dispatcher
// + slot semantics as apply_ops, exposed so other tools (e.g.
// compile_function) can compose batches without re-invoking the registry.
//
// `ops` is the same array shape apply_ops accepts. `atomic` mirrors the
// tool's argument: when true, throws on first failure; when false,
// continues and reports per-op `{ok:false, error}` entries.
//
// Returns the same shape as the tool: { ok, succeeded, failed, slots, results }.
nlohmann::json RunOps(backends::IBlueprintReader& reader,
                      const nlohmann::json& ops, bool atomic);

} // namespace bpr::tools
