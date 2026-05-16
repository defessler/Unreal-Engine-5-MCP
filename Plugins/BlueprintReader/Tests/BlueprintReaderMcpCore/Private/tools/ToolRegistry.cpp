#include "tools/ToolRegistry.h"

#include "tools/ToolCategories.h"

#include <algorithm>

namespace bpr::tools {

void ToolRegistry::Add(ToolDescriptor desc, ToolFn fn) {
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
		if (filterApplied_ && active_.count(d.name) == 0) continue;
		arr.push_back({
			{"name", d.name},
			{"description", d.description},
			{"inputSchema", d.input_schema},
		});
	}
	return arr;
}

const ToolFn* ToolRegistry::Find(const std::string& name) const {
	if (filterApplied_ && active_.count(name) == 0) return nullptr;
	auto it = fns_.find(name);
	return (it == fns_.end()) ? nullptr : &it->second;
}

size_t ToolRegistry::Size() const {
	if (!filterApplied_) return descriptors_.size();
	return active_.size();
}

namespace {
// Expand a list of tokens (tool names + category names + "all") into a
// set of concrete tool names. Categories are looked up via
// ToolCategories.cpp; anything else is treated as a literal tool name
// (no validation here — a typo in the env var silently does nothing,
// which we surface to the user via the post-filter log line in main).
void ExpandTokens(const std::vector<std::string>& tokens,
				  const std::vector<ToolDescriptor>& all,
				  std::set<std::string>& out) {
	for (const auto& tok : tokens) {
		if (tok.empty()) continue;
		if (tok == "all") {
			for (const auto& d : all) out.insert(d.name);
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
} // namespace

void ToolRegistry::ApplyFilter(const std::vector<std::string>& allowSpec,
							   const std::vector<std::string>& denySpec) {
	if (allowSpec.empty() && denySpec.empty()) return;  // nothing to do

	std::set<std::string> keep;
	if (allowSpec.empty()) {
		// No allow-list → start from every registered tool, then subtract.
		for (const auto& d : descriptors_) keep.insert(d.name);
	} else {
		ExpandTokens(allowSpec, descriptors_, keep);
	}
	if (!denySpec.empty()) {
		std::set<std::string> deny;
		ExpandTokens(denySpec, descriptors_, deny);
		for (const auto& d : deny) keep.erase(d);
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
		if (fns_.find(name) == fns_.end()) continue;
		if (active_.insert(name).second) {
			newlyActive.push_back(name);
		}
	}
	if (!newlyActive.empty()) listChanged_ = true;
	return newlyActive;
}

bool ToolRegistry::TakeListChangedFlag() {
	if (!listChanged_) return false;
	listChanged_ = false;
	return true;
}

} // namespace bpr::tools
