#include "tools/ToolRegistry.h"

#include "tools/ToolAnnotations.h"
#include "tools/ToolCategories.h"

#include <algorithm>
#include <regex>
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

void ToolRegistry::Add(ToolDescriptor desc, ToolFn fn) {
	if (const std::string err = ValidateToolName(desc.name); !err.empty()) {
		// Fail loud at registration so we never advertise a name an
		// MCP client would reject. Per spec the rule is SHOULD, but
		// strict clients enforce it — better to find typos at startup.
		throw std::invalid_argument(err);
	}
	// Auto-classify: if the caller didn't set annotations explicitly,
	// look up the canonical hints by tool name. Keeps the 100+ existing
	// registration sites untouched while still emitting readOnlyHint /
	// destructiveHint etc. on tools/list. Unknown names get nothing
	// (the lookup returns an all-nullopt ToolAnnotations).
	if (!desc.annotations.IsSet()) {
		desc.annotations = AnnotationsFor(desc.name);
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

nlohmann::json ToolRegistry::ListSpec() const {
	nlohmann::json arr = nlohmann::json::array();
	for (const auto& d : descriptors_) {
		if (filterApplied_ && active_.count(d.name) == 0)
		{
			continue;
		}
		nlohmann::json entry = {
			{"name", d.name},
			{"description", d.description},
			{"inputSchema", d.input_schema},
		};
		// MCP 2025-06-18 adds `outputSchema` as an optional advertised
		// field; surface ours when set. Older clients ignore the extra key.
		if (!d.output_schema.is_null() && !d.output_schema.empty()) {
			entry["outputSchema"] = d.output_schema;
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

const ToolFn* ToolRegistry::Find(const std::string& name) const {
	if (filterApplied_ && active_.count(name) == 0)
	{
		return nullptr;
	}
	auto it = fns_.find(name);
	return (it == fns_.end()) ? nullptr : &it->second;
}

const ToolFn* ToolRegistry::FindAny(const std::string& name) const {
	auto it = fns_.find(name);
	return (it == fns_.end()) ? nullptr : &it->second;
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

bool ToolRegistry::TakeListChangedFlag() {
	if (!listChanged_)
	{
		return false;
	}
	listChanged_ = false;
	return true;
}

}    // namespace bpr::tools
