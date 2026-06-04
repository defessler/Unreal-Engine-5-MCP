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
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace bpr::tools {

// Per MCP 2025-03-26 §tools/annotations. Hints, not guarantees — a
// well-behaved client uses these to filter the tool surface (Copilot's
// "read-only tools only" toggle, Claude Code's permission UI) but
// MUST NOT rely on them for security decisions if the server is
// untrusted.
//
// All four fields use std::optional<bool> so we can distinguish
// "not set" from "explicitly false". An emitted annotations object
// omits any field still in the nullopt state — clients then fall
// back to the spec defaults (readOnlyHint=false, destructiveHint=true,
// idempotentHint=false, openWorldHint=true). Because the destructive
// + openWorld defaults are TRUE, we explicitly set =false on tools
// that are non-destructive or closed-world so a Copilot user filtering
// by destructive doesn't see every non-delete write tool flagged.
struct ToolAnnotations {
	std::optional<bool> read_only_hint;
	std::optional<bool> destructive_hint;
	std::optional<bool> idempotent_hint;
	std::optional<bool> open_world_hint;

	// True when at least one hint has been assigned (used by Add() to
	// decide whether to apply the auto-classification table).
	bool IsSet() const {
		return read_only_hint.has_value() || destructive_hint.has_value() ||
			   idempotent_hint.has_value() || open_world_hint.has_value();
	}

	// Render to the JSON shape advertised on tools/list. Returns an
	// empty object when IsSet() is false; the caller can drop those
	// from the descriptor entirely.
	nlohmann::json ToJson() const;
};

struct ToolDescriptor {
	std::string name;
	std::string description;
	nlohmann::json input_schema;   // JSON Schema object
	// Optional. When set, advertised on tools/list per MCP 2025-06-18
	// `outputSchema` field. Lets clients reason about the response shape
	// before they parse it. Omit (null/empty) if the tool emits free-form
	// JSON or its response shape is not yet schematized.
	nlohmann::json output_schema;
	// Optional. If left in the default (all-nullopt) state at Add()
	// time, ToolRegistry consults the canonical AnnotationsFor() table
	// in ToolAnnotations.cpp and populates this — so the 100+ existing
	// registration sites don't need touching. Explicit values on the
	// descriptor take precedence (no override).
	ToolAnnotations annotations;
	// Optional. Human-readable display name per MCP 2025-06-18 `title`
	// field. Separate from the programmatic `name` identifier. When
	// non-empty, emitted as "title" on tools/list; clients (Claude
	// Desktop, ChatGPT) display it in their UI instead of the raw name.
	// When empty, `name` is used as-is by clients.
	std::string title;
};

// Tool name validation per MCP 2025-11-25 spec:
//   * 1 to 128 characters
//   * only `[A-Za-z0-9_.\-]`
// Returns a non-empty error string on rejection, empty string on success.
// Pure function — call from anywhere, no I/O.
std::string ValidateToolName(const std::string& name);

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

	// Lookup that ignores the active set. Used by lazy-discovery's
	// `call_tool` meta-tool: when tools/list is restricted to the 3
	// meta-tools, agents reach the underlying tool table via this
	// path instead. Returns nullptr only if the name was never
	// registered (or didn't pass ValidateToolName, so was rejected).
	const ToolFn* FindAny(const std::string& name) const;

	// All registered descriptors regardless of filter state. Used by
	// describe_toolset to look up tools by name even when filtered.
	const std::vector<ToolDescriptor>& AllDescriptors() const { return descriptors_; }

	// How many tools are currently active.
	size_t Size() const;

	// Total registered (active + inactive). Useful for the post-filter
	// log line + the progressive-disclosure meta-tool's response.
	size_t TotalRegistered() const { return descriptors_.size(); }

	// Pre-flight check: returns true when the registry has at least one
	// tool active (so `tools/list` won't return an empty array). Used by
	// main.cpp to fail-fast when an over-aggressive BP_READER_TOOLS filter
	// or a never-called-RegisterBlueprintTools bug leaves the server with
	// nothing to advertise. Cheaper than waiting for a tools/list call to
	// surface the misconfiguration.
	bool HasValidTools() const { return Size() > 0; }

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

	// True when `token` is a recognized ActivateToken/ApplyFilter token:
	// `all`, a `/regex/` form, a known category / workflow preset, or a
	// registered tool name. A token failing this is almost always a typo;
	// the enable_tool_category handler turns it into a did-you-mean error
	// rather than the silent no-op ActivateToken returns for unknown tokens.
	bool IsKnownToken(const std::string& token) const;

	// Best-effort closest match for an unknown token, drawn from the known
	// category names + registered tool names (case-insensitive Levenshtein,
	// small threshold). Empty when nothing is close enough to suggest.
	std::string SuggestToken(const std::string& token) const;

	// After a tools/call, the JSON-RPC dispatcher consults this. Returns
	// true at most once per state change — taking the flag clears it.
	bool TakeListChangedFlag();

	// Dispatch-time GOVERNANCE (PARITY-4), distinct from the disclosure
	// filter (ApplyFilter) above. Compiles `allowRegexes` / `blockRegexes`
	// (ECMAScript) and applies them to EVERY tool — including ones reached
	// via the lazy-discovery `call_tool` meta-tool, because both Find AND
	// FindAny consult the result (so a block can't be bypassed). A tool is
	// blocked when it matches any block pattern, or (when the allow list is
	// non-empty) matches NO allow pattern. Malformed regex throws
	// std::regex_error — let it fail loud at startup. Applies uniformly to
	// the transpile family too, so it's the single coverable governance knob.
	void ApplyGovernance(const std::vector<std::string>& allowRegexes,
						 const std::vector<std::string>& blockRegexes);

	// True when `name` is governance-blocked per ApplyGovernance. Consulted
	// by Find / FindAny / ListSpec. No governance configured → always false.
	bool IsGovernanceBlocked(const std::string& name) const;

private:
	std::vector<ToolDescriptor> descriptors_;
	std::map<std::string, ToolFn> fns_;
	// Governance allow/block patterns (PARITY-4). Empty → no governance.
	std::vector<std::regex> allowPatterns_;
	std::vector<std::regex> blockPatterns_;
	// Names currently visible to clients. Empty AND filterApplied_=false
	// means "show all" (default state — Add() doesn't have to maintain
	// a parallel set). After ApplyFilter or ActivateToken runs, the set
	// is authoritative.
	std::set<std::string> active_;
	bool filterApplied_ = false;
	bool listChanged_ = false;
};

}    // namespace bpr::tools
