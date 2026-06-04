#include "tools/ToolRegistry.h"

#include "tools/ToolAnnotations.h"
#include "tools/ToolCategories.h"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <limits>
#include <regex>
#include <set>
#include <stdexcept>

namespace bpr::tools {

nlohmann::json ToolAnnotations::ToJson() const {
	nlohmann::json obj = nlohmann::json::object();
	if (read_only_hint.has_value())   obj["readOnlyHint"]   = *read_only_hint;
	if (destructive_hint.has_value()) obj["destructiveHint"] = *destructive_hint;
	if (idempotent_hint.has_value())  obj["idempotentHint"]  = *idempotent_hint;
	if (open_world_hint.has_value())  obj["openWorldHint"]   = *open_world_hint;
	return obj;
}

std::string ValidateToolName(const std::string& name) {
	if (name.empty()) {
		return "tool name is empty";
	}
	if (name.size() > 128) {
		return "tool name exceeds 128 chars: " + name.substr(0, 32) + "...";
	}
	for (char c : name) {
		const bool ok =
			(c >= 'A' && c <= 'Z') ||
			(c >= 'a' && c <= 'z') ||
			(c >= '0' && c <= '9') ||
			c == '_' || c == '-' || c == '.';
		if (!ok) {
			return "tool name contains invalid character: '" + name +
				"' (allowed: [A-Za-z0-9_.-])";
		}
	}
	return {};
}

bool IsHardRejection(const std::string& name) {
	// Empty name is the only hard rejection. Everything else (length,
	// invalid chars) is a warn-not-throw soft violation per Epic 5.8's
	// permissive registration model — strict-mode clients will reject
	// the tool on their end, but a single tool's name typo shouldn't
	// kill startup for all of them.
	return name.empty();
}

void ToolRegistry::Add(ToolDescriptor desc, ToolFn fn) {
	if (IsHardRejection(desc.name)) {
		// Empty name is unrecoverable: there's no key to dispatch on,
		// and no human-meaningful identity for the diagnostic. Hard fail.
		throw std::invalid_argument("ToolRegistry::Add: tool name is empty");
	}
	if (const std::string err = ValidateToolName(desc.name); !err.empty()) {
		// Soft violation — log to stderr but accept the registration.
		// Matches Epic 5.8's permissive model: strict MCP clients will
		// reject the tool on their end if it's malformed, but other
		// tools in the registry stay healthy. The MCP spec's character
		// + length rules are MUST-respect for clients, SHOULD-respect
		// for servers.
		std::cerr << "[bp-reader-mcp] ToolRegistry::Add: warning: " << err
				  << " — registration permitted; strict MCP clients may reject this tool.\n";
	}
	// Auto-classify: if the caller didn't set annotations explicitly,
	// look up the canonical hints by tool name. Keeps the 100+ existing
	// registration sites untouched while still emitting readOnlyHint /
	// destructiveHint etc. on tools/list. Unknown names get nothing
	// (the lookup returns an all-nullopt ToolAnnotations).
	if (!desc.annotations.IsSet()) {
		desc.annotations = AnnotationsFor(desc.name);
	}
	// Auto-title (MCP-1): when the caller didn't set an explicit title,
	// first check the curated TitleFor() table, then derive one from the
	// snake_case name as a fallback. Callers can override via desc.title.
	if (desc.title.empty() && !desc.name.empty()) {
		desc.title = TitleFor(desc.name);
		if (desc.title.empty()) {
			// Fallback: snake_case → Title Case (replace _ with space).
			std::string t;
			t.reserve(desc.name.size() + 4);
			bool nextUpper = true;
			for (char c : desc.name) {
				if (c == '_' || c == '-' || c == '.') {
					t.push_back(' ');
					nextUpper = true;
				} else {
					t.push_back(nextUpper ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c);
					nextUpper = false;
				}
			}
			desc.title = std::move(t);
		}
	}
	// Replace-in-place semantics: if a tool with this name was registered
	// before, overwrite both the descriptor and the function. Without this,
	// re-registering the same tool name appended a duplicate descriptor and
	// tools/list would advertise the same tool twice.
	fns_[desc.name] = std::move(fn);
	auto it = std::find_if(descriptors_.begin(), descriptors_.end(),
		[&](const ToolDescriptor& d) { return d.name == desc.name; });
	if (it != descriptors_.end()) {
		*it = std::move(desc);
	} else {
		descriptors_.push_back(std::move(desc));
	}
	// If we're already in filtered mode, leave new tools INACTIVE so
	// callers that activated a specific subset don't suddenly see new
	// surface added later. ApplyFilter / ActivateToken are the only ways
	// to bring tools into the active set after filter is applied.
	// (In the default unfiltered case, ListSpec walks descriptors_
	// directly — no need to maintain active_ then.)
}

namespace default_output_schema_detail {

// Tools whose return shape is a top-level JSON array (vs object). The
// default fallback in OutputSchemaFor uses this set to emit
// `{type:"array"}` instead of the generic `{type:"object"}`.
//
// This is the curated list — anything not here defaults to object.
// Sourced by inspection of each tool's handler return value.
bool ReturnsArray(const std::string& name) {
	static const std::set<std::string> kArr = {
		// Generic + typed asset listings
		"list_assets", "find_asset",
		"list_blueprints", "list_data_tables", "list_materials",
		"list_widgets", "list_behavior_trees", "list_data_assets",
		"list_state_trees", "list_niagara_systems", "list_level_sequences",
		"list_anim_blueprints", "list_gameplay_tags",
		// BP introspection arrays
		"list_variables", "list_functions",
		"get_components",
		"find_node", "find_overriders", "find_dangling_references",
		// Discoverability
		"list_node_kinds",
		// Class introspection — find returns object {classNames:[]}; not here
	};
	return kArr.count(name) > 0;
}

// Permissive defaults — when a tool doesn't set output_schema
// explicitly, advertise the broadest valid JSON-Schema shape so older
// clients have *something* to validate against. Tools that want a
// tighter contract set d.output_schema themselves.
nlohmann::json OutputSchemaFor(const std::string& name) {
	if (ReturnsArray(name)) {
		return nlohmann::json{
			{"type", "array"},
			{"items", {{"type", "object"}}},
		};
	}
	return nlohmann::json{{"type", "object"}};
}

}    // namespace default_output_schema_detail

nlohmann::json ToolRegistry::ListSpec() const {
	using default_output_schema_detail::OutputSchemaFor;
	nlohmann::json arr = nlohmann::json::array();
	for (const auto& d : descriptors_) {
		if (filterApplied_ && active_.count(d.name) == 0)
		{
			continue;
		}
		if (IsGovernanceBlocked(d.name))
		{
			continue;  // PARITY-4: never advertise a governance-blocked tool
		}
		nlohmann::json entry = {
			{"name", d.name},
			{"description", d.description},
			{"inputSchema", d.input_schema},
		};
		// MCP 2025-06-18 §title — human-readable display name separate from
		// the programmatic `name`. Clients like Claude Desktop and ChatGPT
		// display this in their UIs. Only emit when explicitly set.
		if (!d.title.empty()) {
			entry["title"] = d.title;
		}
		// MCP 2025-06-18 §outputSchema. We advertise one for every tool —
		// explicit when the registration site declared a tight shape, else
		// a permissive default keyed by tool name (array vs object). Older
		// clients ignore the extra key; newer ones can use it to validate
		// structured returns.
		if (!d.output_schema.is_null() && !d.output_schema.empty()) {
			entry["outputSchema"] = d.output_schema;
		} else {
			entry["outputSchema"] = OutputSchemaFor(d.name);
		}
		// MCP 2025-03-26 §tools/annotations. Only emit when at least
		// one hint is set so clients on older specs don't see noise.
		if (d.annotations.IsSet()) {
			entry["annotations"] = d.annotations.ToJson();
		}
		arr.push_back(std::move(entry));
	}
	return arr;
}

namespace dotted_alias_detail {
// Epic 5.8 MCP clients can address tools by `<toolset>.<tool>` (e.g.
// `blueprint.read_blueprint`). Our internal registration uses flat names
// only, so the dispatcher needs a fallback: when a lookup misses, strip
// everything up to and including the last '.' and retry. The dotted form
// is NOT advertised on tools/list — only the flat name appears — so we
// don't double the surface, but inbound calls from Epic-style clients
// still resolve. Pure last-segment strip: anything before the last dot
// is treated as an opaque prefix, not validated as a category name.
std::string StripDottedPrefix(const std::string& name) {
	const auto dot = name.find_last_of('.');
	if (dot == std::string::npos || dot + 1 >= name.size()) {
		return name;
	}
	return name.substr(dot + 1);
}
}  // namespace dotted_alias_detail
using namespace dotted_alias_detail;

const ToolFn* ToolRegistry::Find(const std::string& name) const {
	auto it = fns_.find(name);
	if (it == fns_.end()) {
		// Epic-interop fallback: try the un-prefixed last segment.
		if (name.find('.') != std::string::npos) {
			const auto bare = StripDottedPrefix(name);
			if (bare != name) {
				return Find(bare);
			}
		}
		return nullptr;
	}
	if (filterApplied_ && active_.count(name) == 0)
	{
		return nullptr;
	}
	if (IsGovernanceBlocked(name))
	{
		return nullptr;  // PARITY-4: dispatch-time block
	}
	return &it->second;
}

const ToolFn* ToolRegistry::FindAny(const std::string& name) const {
	auto it = fns_.find(name);
	if (it == fns_.end()) {
		if (name.find('.') != std::string::npos) {
			const auto bare = StripDottedPrefix(name);
			if (bare != name) {
				return FindAny(bare);
			}
		}
		return nullptr;
	}
	// PARITY-4: governance is bypass-resistant — enforced even on the
	// call_tool path (which uses FindAny to reach the unfiltered table).
	if (IsGovernanceBlocked(name))
	{
		return nullptr;
	}
	return &it->second;
}

void ToolRegistry::ApplyGovernance(const std::vector<std::string>& allowRegexes,
								   const std::vector<std::string>& blockRegexes) {
	allowPatterns_.clear();
	blockPatterns_.clear();
	for (const auto& s : allowRegexes) {
		if (!s.empty()) { allowPatterns_.emplace_back(s, std::regex::ECMAScript); }
	}
	for (const auto& s : blockRegexes) {
		if (!s.empty()) { blockPatterns_.emplace_back(s, std::regex::ECMAScript); }
	}
}

bool ToolRegistry::IsGovernanceBlocked(const std::string& name) const {
	// Block list wins: any match → blocked.
	for (const auto& re : blockPatterns_) {
		if (std::regex_search(name, re)) { return true; }
	}
	// A non-empty allow list is restrictive: no match → blocked.
	if (!allowPatterns_.empty()) {
		for (const auto& re : allowPatterns_) {
			if (std::regex_search(name, re)) { return false; }
		}
		return true;
	}
	return false;
}

size_t ToolRegistry::Size() const {
	if (!filterApplied_)
	{
		return descriptors_.size();
	}
	return active_.size();
}

namespace tool_registry_detail {

// A token wrapped in /.../ delimiters is treated as an ECMAScript regex
// matched against every registered tool name. Tokens without the
// delimiters fall back to the legacy meaning: "all", category name, or
// literal tool name. Per Epic 5.8's FToolset::SetNameFilters precedent.
bool IsRegexToken(const std::string& tok) {
	return tok.size() >= 2 && tok.front() == '/' && tok.back() == '/';
}

// Expand a list of tokens (tool names + category names + "all" + regex)
// into a set of concrete tool names. Categories are looked up via
// ToolCategories.cpp; anything else is treated as a literal tool name
// (no validation here — a typo in the env var silently does nothing,
// which we surface to the user via the post-filter log line in main).
void ExpandTokens(const std::vector<std::string>& tokens,
				  const std::vector<ToolDescriptor>& all,
				  std::set<std::string>& out) {
	for (const auto& tok : tokens) {
		if (tok.empty())
		{
			continue;
		}
		if (tok == "all") {
			for (const auto& d : all)
			{
				out.insert(d.name);
			}
			continue;
		}
		if (IsRegexToken(tok)) {
			// Strip the /.../ delimiters and compile. Malformed patterns
			// throw std::regex_error — let it propagate so a typo in the
			// env var fails loud at startup rather than silently turning
			// into "no tools matched".
			const std::string body = tok.substr(1, tok.size() - 2);
			const std::regex re(body, std::regex::ECMAScript);
			for (const auto& d : all) {
				if (std::regex_search(d.name, re)) {
					out.insert(d.name);
				}
			}
			continue;
		}
		if (IsKnownCategory(tok)) {
			for (const auto& name : ExpandCategory(tok)) {
				out.insert(name);
			}
			continue;
		}
		out.insert(tok);
	}
}
}    // namespace tool_registry_detail
using namespace tool_registry_detail;

void ToolRegistry::ApplyFilter(const std::vector<std::string>& allowSpec,
							   const std::vector<std::string>& denySpec) {
	if (allowSpec.empty() && denySpec.empty())
	{
		return;  // nothing to do
	}

	std::set<std::string> keep;
	if (allowSpec.empty()) {
		// No allow-list → start from every registered tool, then subtract.
		for (const auto& d : descriptors_)
		{
			keep.insert(d.name);
		}
	} else {
		ExpandTokens(allowSpec, descriptors_, keep);
	}
	if (!denySpec.empty()) {
		std::set<std::string> deny;
		ExpandTokens(denySpec, descriptors_, deny);
		for (const auto& d : deny)
		{
			keep.erase(d);
		}
	}
	// Drop names that don't correspond to any registered tool — keeps
	// the active set's invariant: every entry maps to a real dispatch fn.
	for (auto it = keep.begin(); it != keep.end();) {
		if (fns_.find(*it) == fns_.end()) {
			it = keep.erase(it);
		} else {
			++it;
		}
	}
	active_ = std::move(keep);
	filterApplied_ = true;
	// Deliberately do NOT set listChanged_ here. ApplyFilter runs at
	// startup (main.cpp) before any MCP client is connected. Setting
	// the flag would queue a spurious notifications/tools/list_changed
	// for the first tools/call. ActivateToken (the runtime widening
	// path) IS the one that sets the flag — that call only happens via
	// the meta-tool from a connected client.
}

std::vector<std::string> ToolRegistry::ActivateToken(const std::string& token) {
	if (token.empty()) return {};

	// If no filter has been applied yet, ALL tools are already active —
	// activating more is a no-op. Caller probably didn't mean to use
	// progressive disclosure in this case, but the API is permissive.
	if (!filterApplied_) return {};

	std::set<std::string> requested;
	ExpandTokens({token}, descriptors_, requested);

	std::vector<std::string> newlyActive;
	for (const auto& name : requested) {
		// Skip unknown names (silently — same as ApplyFilter).
		if (fns_.find(name) == fns_.end())
		{
			continue;
		}
		if (active_.insert(name).second) {
			newlyActive.push_back(name);
		}
	}
	if (!newlyActive.empty())
	{
		listChanged_ = true;
	}
	return newlyActive;
}

namespace {
// Case-insensitive Levenshtein distance — used for did-you-mean suggestions
// over the (short) category + tool-name lists. Not perf-critical.
std::size_t EditDistanceCI(const std::string& a, const std::string& b) {
	const std::size_t n = a.size();
	const std::size_t m = b.size();
	std::vector<std::size_t> prev(m + 1);
	std::vector<std::size_t> cur(m + 1);
	for (std::size_t j = 0; j <= m; ++j) { prev[j] = j; }
	auto lc = [](char c) {
		return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	};
	for (std::size_t i = 1; i <= n; ++i) {
		cur[0] = i;
		for (std::size_t j = 1; j <= m; ++j) {
			const std::size_t cost = (lc(a[i - 1]) == lc(b[j - 1])) ? 0 : 1;
			cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
		}
		std::swap(prev, cur);
	}
	return prev[m];
}
}    // namespace

bool ToolRegistry::IsKnownToken(const std::string& token) const {
	if (token.empty()) { return false; }
	if (token == "all") { return true; }
	if (IsRegexToken(token)) { return true; }  // explicit /regex/ — valid syntactically
	if (IsKnownCategory(token)) { return true; }
	return fns_.find(token) != fns_.end();
}

std::string ToolRegistry::SuggestToken(const std::string& token) const {
	if (token.empty()) { return {}; }
	std::string best;
	std::size_t bestDist = std::numeric_limits<std::size_t>::max();
	auto consider = [&](const std::string& cand) {
		const std::size_t d = EditDistanceCI(token, cand);
		if (d < bestDist) { bestDist = d; best = cand; }
	};
	for (const auto& c : AllCategoryNames()) { consider(c); }
	for (const auto& d : descriptors_) { consider(d.name); }
	// Only suggest when reasonably close: edit distance <= 2, or <= a third of
	// the token length (so longer names like "material-tuning" still match a
	// near-miss). Avoids suggesting an unrelated name for a wild typo.
	const std::size_t threshold = std::max<std::size_t>(2, token.size() / 3);
	if (bestDist <= threshold) { return best; }
	return {};
}

bool ToolRegistry::TakeListChangedFlag() {
	if (!listChanged_)
	{
		return false;
	}
	listChanged_ = false;
	return true;
}

}    // namespace bpr::tools
