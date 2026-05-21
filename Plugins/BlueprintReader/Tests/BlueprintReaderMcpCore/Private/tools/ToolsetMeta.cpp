#include "tools/ToolsetMeta.h"

#include "tools/ContentBlocks.h"
#include "tools/ToolCategories.h"

#include <stdexcept>

namespace bpr::tools {

namespace toolset_meta_detail {

// Find the descriptor by name without going through the (possibly
// filter-trimmed) Find(). describe_toolset needs to inspect tools that
// were filtered out of the active set when tool search is enabled.
const ToolDescriptor* FindDescriptor(const ToolRegistry& registry, const std::string& name) {
	for (const auto& d : registry.AllDescriptors()) {
		if (d.name == name) return &d;
	}
	return nullptr;
}

nlohmann::json BuildToolEntry(const ToolDescriptor& d) {
	nlohmann::json entry = {
		{"name", d.name},
		{"description", d.description},
		{"inputSchema", d.input_schema},
	};
	if (!d.output_schema.is_null() && !d.output_schema.empty()) {
		entry["outputSchema"] = d.output_schema;
	}
	return entry;
}

}  // namespace toolset_meta_detail
using namespace toolset_meta_detail;

void RegisterToolsetMetaTools(ToolRegistry& registry) {
	// ---- list_toolsets -------------------------------------------------------
	{
		ToolDescriptor d;
		d.name = "list_toolsets";
		d.description =
			"[meta] List all available toolsets with names, one-line descriptions, and tool counts. "
			"Use this first to discover what's available, then `describe_toolset(name)` to see the tools "
			"inside one, then `call_tool(name, arguments)` to invoke a specific tool. "
			"Lazy-discovery flow per MCP 2025-06-18.";
		d.input_schema = {
			{"type", "object"},
			{"properties", nlohmann::json::object()},
		};
		d.output_schema = {
			{"type", "object"},
			{"properties", {
				{"toolsets", {
					{"type", "array"},
					{"items", {
						{"type", "object"},
						{"required", nlohmann::json::array({"name","description","tool_count"})},
						{"properties", {
							{"name",        {{"type","string"}}},
							{"description", {{"type","string"}}},
							{"tool_count",  {{"type","integer"}, {"minimum", 0}}},
						}},
					}},
				}},
			}},
			{"required", nlohmann::json::array({"toolsets"})},
		};
		registry.Add(std::move(d), [&registry](const nlohmann::json& /*args*/) {
			nlohmann::json arr = nlohmann::json::array();
			for (const auto& name : AllCategoryNames()) {
				arr.push_back({
					{"name", name},
					{"description", CategoryDescription(name)},
					{"tool_count", static_cast<int>(ExpandCategory(name).size())},
				});
			}
			return nlohmann::json{{"toolsets", std::move(arr)}};
		});
	}

	// ---- describe_toolset ----------------------------------------------------
	{
		ToolDescriptor d;
		d.name = "describe_toolset";
		d.description =
			"[meta] Return the full tool list for a toolset — each tool's name, description, "
			"inputSchema, and (when set) outputSchema. Call this after `list_toolsets` to expand "
			"the toolset you want to use. Then call `call_tool(toolset_name=..., name=..., arguments=...)`.";
		d.input_schema = {
			{"type", "object"},
			{"properties", {
				{"name", {{"type","string"}, {"description", "Toolset name as returned by list_toolsets."}}},
			}},
			{"required", nlohmann::json::array({"name"})},
		};
		d.output_schema = {
			{"type", "object"},
			{"properties", {
				{"name",        {{"type","string"}}},
				{"description", {{"type","string"}}},
				{"tools",       {{"type","array"}}},
			}},
			{"required", nlohmann::json::array({"name","tools"})},
		};
		registry.Add(std::move(d), [&registry](const nlohmann::json& args) {
			auto nameIt = args.find("name");
			if (nameIt == args.end() || !nameIt->is_string()) {
				throw std::invalid_argument("describe_toolset requires a string `name`");
			}
			const std::string toolsetName = nameIt->get<std::string>();
			if (!IsKnownCategory(toolsetName)) {
				throw std::invalid_argument("unknown toolset: " + toolsetName);
			}
			nlohmann::json toolsArr = nlohmann::json::array();
			for (const auto& toolName : ExpandCategory(toolsetName)) {
				if (const auto* desc = FindDescriptor(registry, toolName)) {
					toolsArr.push_back(BuildToolEntry(*desc));
				}
			}
			return nlohmann::json{
				{"name", toolsetName},
				{"description", CategoryDescription(toolsetName)},
				{"tools", std::move(toolsArr)},
			};
		});
	}

	// ---- call_tool ----------------------------------------------------------
	{
		ToolDescriptor d;
		d.name = "call_tool";
		d.description =
			"[meta] Dispatch a tool by name with the given arguments, bypassing the lazy-discovery "
			"filter. Use after `describe_toolset` to invoke any tool by its name. The optional "
			"`toolset_name` is currently informational (validation only); names are global across "
			"toolsets in this server.";
		d.input_schema = {
			{"type", "object"},
			{"properties", {
				{"name",         {{"type","string"}, {"description","Tool name (e.g. read_blueprint)."}}},
				{"toolset_name", {{"type","string"}, {"description","Optional toolset for validation (e.g. core, cpp). Names are global, so this is informational."}}},
				{"arguments",    {{"type","object"}, {"description","Arguments for the target tool. Matches the tool's inputSchema."}}},
			}},
			{"required", nlohmann::json::array({"name"})},
		};
		// No outputSchema — pass-through to the target tool's own shape.
		registry.Add(std::move(d), [&registry](const nlohmann::json& args) {
			auto nameIt = args.find("name");
			if (nameIt == args.end() || !nameIt->is_string()) {
				throw std::invalid_argument("call_tool requires a string `name`");
			}
			const std::string targetName = nameIt->get<std::string>();
			// Self-dispatch recursion guard: an agent (or a buggy client)
			// can issue `call_tool({name: "call_tool", arguments: {...}})`,
			// which would otherwise loop until stack overflow. Reject the
			// three meta-tool names — they're already top-level for the
			// agent to invoke directly; tunneling them through call_tool
			// is never necessary and is always indistinguishable from a
			// mistake.
			if (targetName == "call_tool" ||
				targetName == "list_toolsets" ||
				targetName == "describe_toolset") {
				throw std::invalid_argument(
					"call_tool cannot dispatch the meta-tool '" + targetName +
					"' — invoke it directly instead.");
			}
			nlohmann::json subArgs = nlohmann::json::object();
			if (auto argsIt = args.find("arguments"); argsIt != args.end()) {
				if (!argsIt->is_object()) {
					throw std::invalid_argument("call_tool `arguments` must be an object");
				}
				subArgs = *argsIt;
			}
			if (auto tsIt = args.find("toolset_name"); tsIt != args.end() && tsIt->is_string()) {
				const std::string ts = tsIt->get<std::string>();
				if (!ts.empty() && !IsKnownCategory(ts)) {
					throw std::invalid_argument("call_tool unknown toolset_name: " + ts);
				}
			}
			const ToolFn* fn = registry.FindAny(targetName);
			if (fn == nullptr) {
				throw std::invalid_argument("call_tool unknown tool: " + targetName);
			}
			// Note: invoking the underlying fn here drops it through the
			// MCP layer's standard envelope path (which already handles
			// _mcp content blocks and structuredContent). We return its
			// result verbatim.
			return (*fn)(subArgs);
		});
	}
}

void EnableToolSearchMode(ToolRegistry& registry) {
	// Tools that stay visible even in tool-search mode. Keep the meta-tools
	// plus shutdown_daemon (an agent finishing its session shouldn't have
	// to dig through list_toolsets to find the shutdown).
	const std::vector<std::string> allowList = {
		"list_toolsets",
		"describe_toolset",
		"call_tool",
		"shutdown_daemon",
	};
	registry.ApplyFilter(allowList, /*denySpec*/ {});
}

}  // namespace bpr::tools
