#include "tools/BlueprintTools.h"

#include <stdexcept>
#include <string>

#include <fmt/core.h>
#include <nlohmann/json.hpp>

namespace bpr::tools {

namespace {

const std::string& RequireString(const nlohmann::json& obj, std::string_view key) {
    auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) {
        throw std::invalid_argument(fmt::format(R"(missing or non-string argument "{}")", key));
    }
    return it->get_ref<const std::string&>();
}

std::string OptString(const nlohmann::json& obj, std::string_view key,
                      std::string fallback) {
    auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) return fallback;
    if (!it->is_string()) {
        throw std::invalid_argument(fmt::format(R"(argument "{}" must be a string)", key));
    }
    return it->get<std::string>();
}

nlohmann::json AssetPathSchema() {
    return nlohmann::json{
        {"type", "object"},
        {"properties", {
            {"asset_path", {
                {"type", "string"},
                {"description", "UE asset path, e.g. /Game/AI/BP_Enemy"},
            }},
        }},
        {"required", nlohmann::json::array({"asset_path"})},
    };
}

} // namespace

void RegisterBlueprintTools(ToolRegistry& registry, backends::IBlueprintReader& reader) {
    // ----- list_blueprints -------------------------------------------------
    {
        ToolDescriptor d;
        d.name = "list_blueprints";
        d.description =
            "List blueprint assets under a content path. Defaults to /Game.";
        d.input_schema = {
            {"type", "object"},
            {"properties", {
                {"path", {
                    {"type", "string"},
                    {"description", "Content path filter, e.g. /Game/AI. Defaults to /Game."},
                }},
            }},
        };
        registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
            std::string path = OptString(args, "path", "/Game");
            auto items = reader.ListBlueprints(path);
            return nlohmann::json(items);
        });
    }

    // ----- read_blueprint --------------------------------------------------
    {
        ToolDescriptor d;
        d.name = "read_blueprint";
        d.description =
            "Read top-level metadata for a blueprint: parent class, interfaces, "
            "variables, function/graph summaries, macros.";
        d.input_schema = AssetPathSchema();
        registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
            const std::string& asset = RequireString(args, "asset_path");
            return nlohmann::json(reader.ReadBlueprint(asset));
        });
    }

    // ----- get_graph -------------------------------------------------------
    {
        ToolDescriptor d;
        d.name = "get_graph";
        d.description =
            "Fetch a graph (nodes + connections) by name. Defaults to EventGraph.";
        d.input_schema = {
            {"type", "object"},
            {"properties", {
                {"asset_path", {
                    {"type", "string"},
                    {"description", "UE asset path, e.g. /Game/AI/BP_Enemy"},
                }},
                {"graph_name", {
                    {"type", "string"},
                    {"description", "Graph name. Defaults to \"EventGraph\"."},
                }},
            }},
            {"required", nlohmann::json::array({"asset_path"})},
        };
        registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
            const std::string& asset = RequireString(args, "asset_path");
            std::string graph = OptString(args, "graph_name", "EventGraph");
            return nlohmann::json(reader.GetGraph(asset, graph));
        });
    }

    // ----- get_function ----------------------------------------------------
    {
        ToolDescriptor d;
        d.name = "get_function";
        d.description =
            "Fetch a blueprint function: signature (inputs/outputs), locals, "
            "and body graph.";
        d.input_schema = {
            {"type", "object"},
            {"properties", {
                {"asset_path", {
                    {"type", "string"},
                    {"description", "UE asset path, e.g. /Game/AI/BP_Enemy"},
                }},
                {"function_name", {
                    {"type", "string"},
                    {"description", "Function name as it appears in the blueprint."},
                }},
            }},
            {"required", nlohmann::json::array({"asset_path", "function_name"})},
        };
        registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
            const std::string& asset = RequireString(args, "asset_path");
            const std::string& fn = RequireString(args, "function_name");
            return nlohmann::json(reader.GetFunction(asset, fn));
        });
    }

    // ----- list_variables --------------------------------------------------
    {
        ToolDescriptor d;
        d.name = "list_variables";
        d.description =
            "List all member variables on a blueprint, with type, default, "
            "category, and replication state.";
        d.input_schema = AssetPathSchema();
        registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
            const std::string& asset = RequireString(args, "asset_path");
            return nlohmann::json(reader.ListVariables(asset));
        });
    }

    // ----- find_node -------------------------------------------------------
    {
        ToolDescriptor d;
        d.name = "find_node";
        d.description =
            "Search nodes within a blueprint by class or title (case-insensitive substring). "
            "Optional `kind` further filters by the K2 extras kind, e.g. CallFunction, "
            "VariableGet, Event, CustomEvent, DynamicCast, MacroInstance, "
            "FunctionEntry, FunctionResult.";
        d.input_schema = {
            {"type", "object"},
            {"properties", {
                {"asset_path", {
                    {"type", "string"},
                    {"description", "UE asset path, e.g. /Game/AI/BP_Enemy"},
                }},
                {"query", {
                    {"type", "string"},
                    {"description", "Substring matched against node class or title. "
                                    "Pass an empty string to match any node when filtering by `kind` only."},
                }},
                {"kind", {
                    {"type", "string"},
                    {"description", "Optional. K2 extras `kind` to match exactly (case-insensitive)."},
                }},
            }},
            {"required", nlohmann::json::array({"asset_path", "query"})},
        };
        registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
            const std::string& asset = RequireString(args, "asset_path");
            const std::string& q = RequireString(args, "query");
            std::string kind = OptString(args, "kind", "");
            return nlohmann::json(reader.FindNode(asset, q, kind));
        });
    }

    // ===== Write tools (Phase 1.5) =========================================

    // ----- add_variable ----------------------------------------------------
    {
        ToolDescriptor d;
        d.name = "add_variable";
        d.description =
            "Add a member variable to a blueprint. `type` is a BPPinType: "
            "{category, sub_category, sub_category_object, is_array, is_set, is_map}.";
        d.input_schema = {
            {"type", "object"},
            {"properties", {
                {"asset_path",    {{"type","string"}}},
                {"name",          {{"type","string"}}},
                {"type",          {{"type","object"},
                                   {"description","BPPinType wire shape, e.g. {\"category\":\"real\",\"sub_category\":\"float\"}"}}},
                {"default_value", {{"type","string"}}},
                {"category",      {{"type","string"}}},
                {"replicated",    {{"type","boolean"}}},
                {"editable",      {{"type","boolean"}}},
            }},
            {"required", nlohmann::json::array({"asset_path","name","type"})},
        };
        registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
            const std::string& asset = RequireString(args, "asset_path");
            const std::string& name  = RequireString(args, "name");
            auto typeIt = args.find("type");
            if (typeIt == args.end() || !typeIt->is_object()) {
                throw std::invalid_argument(R"(missing or non-object argument "type")");
            }
            // Build BPPinType tolerantly: only `category` is required.
            BPPinType type;
            type.Category = RequireString(*typeIt, "category");
            if (auto it = typeIt->find("sub_category"); it != typeIt->end() && it->is_string()) {
                type.SubCategory = it->get<std::string>();
            }
            if (auto it = typeIt->find("sub_category_object"); it != typeIt->end() && it->is_string()) {
                type.SubCategoryObject = it->get<std::string>();
            }
            type.IsArray = typeIt->value("is_array", false);
            type.IsSet   = typeIt->value("is_set",   false);
            type.IsMap   = typeIt->value("is_map",   false);

            std::string defaultValue = OptString(args, "default_value", "");
            std::string category     = OptString(args, "category",      "");
            bool replicated = args.value("replicated", false);
            bool editable   = args.value("editable",   false);
            reader.AddVariable(asset, name, type, defaultValue, category, replicated, editable);
            return nlohmann::json{{"ok", true}};
        });
    }

    // ----- set_node_position -----------------------------------------------
    {
        ToolDescriptor d;
        d.name = "set_node_position";
        d.description = "Move a node (by GUID) to (x, y) inside a graph. Recompiles + saves the BP.";
        d.input_schema = {
            {"type", "object"},
            {"properties", {
                {"asset_path", {{"type","string"}}},
                {"graph_name", {{"type","string"}}},
                {"node_id",    {{"type","string"}, {"description","Node GUID"}}},
                {"x",          {{"type","integer"}}},
                {"y",          {{"type","integer"}}},
            }},
            {"required", nlohmann::json::array({"asset_path","graph_name","node_id","x","y"})},
        };
        registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
            const std::string& asset = RequireString(args, "asset_path");
            const std::string& graph = RequireString(args, "graph_name");
            const std::string& node  = RequireString(args, "node_id");
            int x = args.at("x").get<int>();
            int y = args.at("y").get<int>();
            reader.SetNodePosition(asset, graph, node, x, y);
            return nlohmann::json{{"ok", true}};
        });
    }

    // ----- delete_node -----------------------------------------------------
    {
        ToolDescriptor d;
        d.name = "delete_node";
        d.description = "Delete a node by GUID. Breaks any links into/out of it. Recompiles + saves.";
        d.input_schema = {
            {"type", "object"},
            {"properties", {
                {"asset_path", {{"type","string"}}},
                {"graph_name", {{"type","string"}}},
                {"node_id",    {{"type","string"}, {"description","Node GUID"}}},
            }},
            {"required", nlohmann::json::array({"asset_path","graph_name","node_id"})},
        };
        registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
            const std::string& asset = RequireString(args, "asset_path");
            const std::string& graph = RequireString(args, "graph_name");
            const std::string& node  = RequireString(args, "node_id");
            reader.DeleteNode(asset, graph, node);
            return nlohmann::json{{"ok", true}};
        });
    }
}

} // namespace bpr::tools
