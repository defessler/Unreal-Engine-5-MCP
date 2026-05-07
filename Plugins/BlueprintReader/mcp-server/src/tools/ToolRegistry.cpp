#include "tools/ToolRegistry.h"

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

} // namespace bpr::tools
