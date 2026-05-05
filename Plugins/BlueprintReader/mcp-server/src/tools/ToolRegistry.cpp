#include "tools/ToolRegistry.h"

namespace bpr::tools {

void ToolRegistry::Add(ToolDescriptor desc, ToolFn fn) {
    fns_[desc.name] = std::move(fn);
    descriptors_.push_back(std::move(desc));
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
