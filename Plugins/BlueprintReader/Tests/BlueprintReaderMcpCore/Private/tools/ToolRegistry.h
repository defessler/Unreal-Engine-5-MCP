// Tool registry — descriptors for tools/list and a dispatch table for
// tools/call. The MCP layer does not interpret tool semantics; it only knows
// the registry.
//
// Two visibility models (composable, both default to "all visible"):
//   * STATIC filter: ApplyFilter shrinks the active subset based on
//     BP_READER_TOOLS / BP_READER_TOOLS_EXCLUDE env vars at startup. Once
//     trimmed it stays trimmed for the session. This is the default when
//     a user fits-under a client's tool-count cap by hand-picking categories.
//   * PROGRESSIVE disclosure: start with a small initial subset, and let the
//     agent widen the surface mid-session via the `enable_tool_category`
//     meta-tool. ToolRegistry tracks a `listChanged_` flag the JSON-RPC
//     dispatcher consults after every `tools/call` to emit
//     `notifications/tools/list_changed` per MCP 2025-03-26.
//
// Both models share the same underlying mechanism: an `active_` set that
// gates ListSpec()/Find(). The full descriptors + dispatch fns are always
// kept — the static filter just trims `active_` once and walks away; the
// progressive flow mutates `active_` at runtime in response to meta-tool
// calls.
#pragma once

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace bpr::tools {

struct ToolDescriptor {
	std::string name;
	std::string description;
	nlohmann::json input_schema;   // JSON Schema object
};

// Tool handlers receive the `arguments` map from the tools/call request and
// return arbitrary JSON. They may throw std::exception subclasses; the MCP
// layer turns those into MCP tool errors (isError: true) — NOT JSON-RPC errors.
using ToolFn = std::function<nlohmann::json(const nlohmann::json& arguments)>;

class ToolRegistry {
public:
	void Add(ToolDescriptor desc, ToolFn fn);

	// For tools/list. Returns array of {name, description, inputSchema}
	// for ACTIVE tools only (subject to the static filter and/or the
	// progressive-disclosure active set).
	nlohmann::json ListSpec() const;

	// Lookup. Returns nullptr if missing OR if filtered out of the
	// active set. The dispatcher treats both as "no such tool".
	const ToolFn* Find(const std::string& name) const;

	// How many tools are currently active.
	size_t Size() const;

	// Total registered (active + inactive). Useful for the post-filter
	// log line + the progressive-disclosure meta-tool's response.
	size_t TotalRegistered() const { return descriptors_.size(); }

	// Trim the active subset.
	//
	// `allowSpec`: if non-empty, only tools matching at least one of these
	//              tokens become active. Empty → start from "all tools".
	// `denySpec`:  any tool matching one of these tokens is deactivated
	//              AFTER the allow step. Empty → no removals.
	//
	// Each token is either:
	//   * A tool name (`read_blueprint`, `add_node`, …)
	//   * A category name (`core`, `read`, `write`, `cpp`, `editor`,
	//     `assets`, `materials`, `widgets`, `behavior-trees`,
	//     `data-tables`, `data-assets`, `state-trees`, `niagara`,
	//     `sequencer`, `gameplay-tags`, `anim-bp`, `profiling`, `cook`,
	//     `tests`, `class-info`, `discover`, or a workflow preset like
	//     `material-tuning` / `editor-control`) — expands to that
	//     category's tool list. The full mapping lives in
	//     ToolCategories.cpp.
	//   * `all`: shorthand for every registered tool.
	//
	// Used by main.cpp to honor BP_READER_TOOLS / BP_READER_TOOLS_EXCLUDE
	// env vars + by progressive-disclosure init.
	//
	// Idempotent — re-applying with the same args is a no-op. The
	// descriptor + dispatch tables are untouched; only the active set
	// changes.
	void ApplyFilter(const std::vector<std::string>& allowSpec,
					 const std::vector<std::string>& denySpec);

	// Progressive disclosure: activate a category (or single tool name).
	// Returns the names that were newly activated (already-active names
	// are not in the return value). Sets the listChanged_ flag if the
	// active set actually changed.
	//
	// Accepts the same token vocabulary as ApplyFilter: tool names,
	// category names, workflow presets, or `all`.
	std::vector<std::string> ActivateToken(const std::string& token);

	// After a tools/call, the JSON-RPC dispatcher consults this. Returns
	// true at most once per state change — taking the flag clears it.
	bool TakeListChangedFlag();

private:
	std::vector<ToolDescriptor> descriptors_;
	std::map<std::string, ToolFn> fns_;
	// Names currently visible to clients. Empty AND filterApplied_=false
	// means "show all" (default state — Add() doesn't have to maintain
	// a parallel set). After ApplyFilter or ActivateToken runs, the set
	// is authoritative.
	std::set<std::string> active_;
	bool filterApplied_ = false;
	bool listChanged_ = false;
};

} // namespace bpr::tools
