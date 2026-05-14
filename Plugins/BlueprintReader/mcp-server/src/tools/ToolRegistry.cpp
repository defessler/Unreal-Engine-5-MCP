#include "tools/ToolRegistry.h"

#include "tools/ToolCategories.h"

#include <algorithm>
#include <set>

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
}

nlohmann::json ToolRegistry::ListSpec() const {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& d : descriptors_) {
        arr.push_back({
            {"name", d.name},
            {"description", d.description},
            {"inputSchema", d.input_schema},
        });
    }
    return arr;
}

const ToolFn* ToolRegistry::Find(const std::string& name) const {
    auto it = fns_.find(name);
    return (it == fns_.end()) ? nullptr : &it->second;
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
        // Literal tool name. Insert even if it doesn't currently match a
        // registered tool — ApplyFilter is a set operation, not a
        // validation. A typo just doesn't keep anything, which is the
        // safe failure mode.
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

    // Remove from descriptor vector + dispatch map in one pass. Erase
    // dispatch entries first while we still have the descriptor list to
    // iterate; descriptor erase is the second step.
    for (auto it = fns_.begin(); it != fns_.end();) {
        if (keep.count(it->first) == 0) {
            it = fns_.erase(it);
        } else {
            ++it;
        }
    }
    descriptors_.erase(
        std::remove_if(descriptors_.begin(), descriptors_.end(),
            [&](const ToolDescriptor& d) { return keep.count(d.name) == 0; }),
        descriptors_.end());
}

} // namespace bpr::tools
