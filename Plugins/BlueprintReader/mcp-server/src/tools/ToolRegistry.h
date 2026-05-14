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

    // How many tools survived ApplyFilter (or were registered total if
    // no filter ran).
    size_t Size() const { return descriptors_.size(); }

    // Trim the registry to a subset.
    //
    // `allowSpec`: if non-empty, only tools matching at least one of these
    //              tokens survive. Empty → start from "all tools".
    // `denySpec`:  any tool matching one of these tokens is removed AFTER
    //              the allow step. Empty → no removals.
    //
    // Each token is either:
    //   * A tool name (`read_blueprint`, `add_node`, …)
    //   * A category name (`core`, `read`, `write`, `cpp`, `editor`,
    //     `assets`, `materials`, `widgets`, `behavior-trees`,
    //     `data-tables`, `data-assets`, `state-trees`, `niagara`,
    //     `sequencer`, `gameplay-tags`, `anim-bp`, `profiling`, `cook`,
    //     `tests`, `class-info`, `discover`) — expands to that
    //     category's tool list. The full mapping lives in
    //     ToolCategories.cpp.
    //   * `all`: shorthand for every registered tool.
    //
    // Used by main.cpp to honor `BP_READER_TOOLS` /
    // `BP_READER_TOOLS_EXCLUDE` env vars so MCP clients with tool-count
    // caps (Copilot caps at 128 total tools across all servers + its
    // built-ins) can pare the surface down to what they need.
    //
    // Idempotent — re-applying with the same args is a no-op. Tools
    // removed are also un-registered from the dispatch table, so
    // tools/call against a filtered-out tool returns "tool not found"
    // the same way an unknown tool would.
    void ApplyFilter(const std::vector<std::string>& allowSpec,
                     const std::vector<std::string>& denySpec);

private:
    std::vector<ToolDescriptor> descriptors_;
    std::map<std::string, ToolFn> fns_;
};

} // namespace bpr::tools
