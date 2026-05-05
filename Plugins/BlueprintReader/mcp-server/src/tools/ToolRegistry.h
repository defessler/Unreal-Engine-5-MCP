// Tool registry — descriptors for tools/list and a dispatch table for
// tools/call. The MCP layer does not interpret tool semantics; it only knows
// the registry.
#pragma once

#include <functional>
#include <map>
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

    // For tools/list. Returns array of {name, description, inputSchema}.
    nlohmann::json ListSpec() const;

    // Lookup. Returns nullptr if missing.
    const ToolFn* Find(const std::string& name) const;

private:
    std::vector<ToolDescriptor> descriptors_;
    std::map<std::string, ToolFn> fns_;
};

} // namespace bpr::tools
