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

    // ----- get_components --------------------------------------------------
    {
        ToolDescriptor d;
        d.name = "get_components";
        d.description =
            "List the SCS components (StaticMeshComponent, LightComponent, "
            "child actors, etc.) attached to a blueprint, with parent/child "
            "hierarchy. Each entry: {name, class, parent, is_root}.";
        d.input_schema = AssetPathSchema();
        registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
            const std::string& asset = RequireString(args, "asset_path");
            return nlohmann::json(reader.GetComponents(asset));
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

    // ----- add_node --------------------------------------------------------
    {
        ToolDescriptor d;
        d.name = "add_node";
        d.description =
            "Spawn a new K2 node in a graph. `kind` is one of: "
            "Branch, Sequence, VariableGet, VariableSet, CallFunction, CustomEvent. "
            "Kind-specific args: VariableGet/VariableSet -> `variable`; "
            "CallFunction -> `function` + `function_owner` (UClass path or short name); "
            "CustomEvent -> `event_name`. Returns {ok, node_id}.";
        d.input_schema = {
            {"type", "object"},
            {"properties", {
                {"asset_path",     {{"type","string"}}},
                {"graph_name",     {{"type","string"}}},
                {"kind",           {{"type","string"}}},
                {"x",              {{"type","integer"}}},
                {"y",              {{"type","integer"}}},
                {"variable",       {{"type","string"}}},
                {"function",       {{"type","string"}}},
                {"function_owner", {{"type","string"}}},
                {"event_name",     {{"type","string"}}},
                {"target_class",   {{"type","string"}}},
                {"struct_type",    {{"type","string"}}},
            }},
            {"required", nlohmann::json::array({"asset_path","graph_name","kind","x","y"})},
        };
        registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
            const std::string& asset = RequireString(args, "asset_path");
            const std::string& graph = RequireString(args, "graph_name");
            const std::string& kind  = RequireString(args, "kind");
            int x = args.at("x").get<int>();
            int y = args.at("y").get<int>();
            std::map<std::string, std::string, std::less<>> extras;
            // Map MCP-side names → plugin commandlet flag names.
            auto put = [&](const char* mcpKey, const char* flagKey) {
                std::string v = OptString(args, mcpKey, "");
                if (!v.empty()) extras.emplace(flagKey, std::move(v));
            };
            put("variable",       "Variable");
            put("function",       "Function");
            put("function_owner", "FunctionOwner");
            put("event_name",     "EventName");
            put("target_class",   "TargetClass");
            put("struct_type",    "StructType");
            std::string newId = reader.AddNode(asset, graph, kind, x, y, extras);
            return nlohmann::json{{"ok", true}, {"node_id", newId}};
        });
    }

    // ----- wire_pins -------------------------------------------------------
    {
        ToolDescriptor d;
        d.name = "wire_pins";
        d.description =
            "Connect two pins. `from_pin` and `to_pin` accept either a pin GUID "
            "(preferred — see get_graph) or a pin name. The schema's pin "
            "compatibility rules are enforced.";
        d.input_schema = {
            {"type","object"},
            {"properties", {
                {"asset_path",  {{"type","string"}}},
                {"graph_name",  {{"type","string"}}},
                {"from_node",   {{"type","string"}}},
                {"from_pin",    {{"type","string"}}},
                {"to_node",     {{"type","string"}}},
                {"to_pin",      {{"type","string"}}},
            }},
            {"required", nlohmann::json::array({"asset_path","graph_name","from_node","from_pin","to_node","to_pin"})},
        };
        registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
            reader.WirePins(
                RequireString(args, "asset_path"),
                RequireString(args, "graph_name"),
                RequireString(args, "from_node"),
                RequireString(args, "from_pin"),
                RequireString(args, "to_node"),
                RequireString(args, "to_pin"));
            return nlohmann::json{{"ok", true}};
        });
    }

    // ----- delete_variable -------------------------------------------------
    {
        ToolDescriptor d;
        d.name = "delete_variable";
        d.description = "Remove a member variable by name. Recompiles + saves.";
        d.input_schema = {
            {"type","object"},
            {"properties", {
                {"asset_path", {{"type","string"}}},
                {"name",       {{"type","string"}}},
            }},
            {"required", nlohmann::json::array({"asset_path","name"})},
        };
        registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
            reader.DeleteVariable(
                RequireString(args, "asset_path"),
                RequireString(args, "name"));
            return nlohmann::json{{"ok", true}};
        });
    }

    // ----- rename_variable -------------------------------------------------
    {
        ToolDescriptor d;
        d.name = "rename_variable";
        d.description =
            "Rename a member variable. Updates references in graphs. "
            "Recompiles + saves.";
        d.input_schema = {
            {"type","object"},
            {"properties", {
                {"asset_path", {{"type","string"}}},
                {"old_name",   {{"type","string"}}},
                {"new_name",   {{"type","string"}}},
            }},
            {"required", nlohmann::json::array({"asset_path","old_name","new_name"})},
        };
        registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
            reader.RenameVariable(
                RequireString(args, "asset_path"),
                RequireString(args, "old_name"),
                RequireString(args, "new_name"));
            return nlohmann::json{{"ok", true}};
        });
    }

    // ----- add_function ----------------------------------------------------
    {
        ToolDescriptor d;
        d.name = "add_function";
        d.description =
            "Create a new BP function graph with the given name. Returns "
            "{ok, function_name}. Use add_function_input / add_function_output "
            "to declare its signature.";
        d.input_schema = {
            {"type","object"},
            {"properties", {
                {"asset_path", {{"type","string"}}},
                {"name",       {{"type","string"}}},
            }},
            {"required", nlohmann::json::array({"asset_path","name"})},
        };
        registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
            const std::string& asset = RequireString(args, "asset_path");
            const std::string& name  = RequireString(args, "name");
            std::string echoed = reader.AddFunction(asset, name);
            return nlohmann::json{{"ok", true}, {"function_name", echoed}};
        });
    }

    auto buildBPPinType = [](const nlohmann::json& obj) -> BPPinType {
        BPPinType type;
        type.Category = RequireString(obj, "category");
        if (auto it = obj.find("sub_category"); it != obj.end() && it->is_string()) {
            type.SubCategory = it->get<std::string>();
        }
        if (auto it = obj.find("sub_category_object"); it != obj.end() && it->is_string()) {
            type.SubCategoryObject = it->get<std::string>();
        }
        type.IsArray = obj.value("is_array", false);
        type.IsSet   = obj.value("is_set",   false);
        type.IsMap   = obj.value("is_map",   false);
        return type;
    };

    // ----- add_function_input ----------------------------------------------
    {
        ToolDescriptor d;
        d.name = "add_function_input";
        d.description =
            "Add an input parameter to an existing function. `type` is a BPPinType.";
        d.input_schema = {
            {"type","object"},
            {"properties", {
                {"asset_path",    {{"type","string"}}},
                {"function_name", {{"type","string"}}},
                {"param_name",    {{"type","string"}}},
                {"type",          {{"type","object"}}},
            }},
            {"required", nlohmann::json::array({"asset_path","function_name","param_name","type"})},
        };
        registry.Add(std::move(d), [&reader, buildBPPinType](const nlohmann::json& args) {
            const std::string& asset = RequireString(args, "asset_path");
            const std::string& fn    = RequireString(args, "function_name");
            const std::string& param = RequireString(args, "param_name");
            auto typeIt = args.find("type");
            if (typeIt == args.end() || !typeIt->is_object()) {
                throw std::invalid_argument(R"(missing or non-object argument "type")");
            }
            reader.AddFunctionInput(asset, fn, param, buildBPPinType(*typeIt));
            return nlohmann::json{{"ok", true}};
        });
    }

    // ----- add_function_output ---------------------------------------------
    {
        ToolDescriptor d;
        d.name = "add_function_output";
        d.description =
            "Add an output parameter to an existing function. Spawns a "
            "FunctionResult node if there isn't one yet.";
        d.input_schema = {
            {"type","object"},
            {"properties", {
                {"asset_path",    {{"type","string"}}},
                {"function_name", {{"type","string"}}},
                {"param_name",    {{"type","string"}}},
                {"type",          {{"type","object"}}},
            }},
            {"required", nlohmann::json::array({"asset_path","function_name","param_name","type"})},
        };
        registry.Add(std::move(d), [&reader, buildBPPinType](const nlohmann::json& args) {
            const std::string& asset = RequireString(args, "asset_path");
            const std::string& fn    = RequireString(args, "function_name");
            const std::string& param = RequireString(args, "param_name");
            auto typeIt = args.find("type");
            if (typeIt == args.end() || !typeIt->is_object()) {
                throw std::invalid_argument(R"(missing or non-object argument "type")");
            }
            reader.AddFunctionOutput(asset, fn, param, buildBPPinType(*typeIt));
            return nlohmann::json{{"ok", true}};
        });
    }

    // ----- delete_function -------------------------------------------------
    {
        ToolDescriptor d;
        d.name = "delete_function";
        d.description = "Delete a function graph by name.";
        d.input_schema = {
            {"type","object"},
            {"properties", {
                {"asset_path", {{"type","string"}}},
                {"name",       {{"type","string"}}},
            }},
            {"required", nlohmann::json::array({"asset_path","name"})},
        };
        registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
            reader.DeleteFunction(
                RequireString(args, "asset_path"),
                RequireString(args, "name"));
            return nlohmann::json{{"ok", true}};
        });
    }

    // ----- set_variable_default --------------------------------------------
    {
        ToolDescriptor d;
        d.name = "set_variable_default";
        d.description =
            "Change a member variable's default value (string form, as displayed "
            "in the Details panel — e.g. \"100.0\" for a float, \"true\"/\"false\" "
            "for a bool). Pass empty string to clear.";
        d.input_schema = {
            {"type","object"},
            {"properties", {
                {"asset_path",    {{"type","string"}}},
                {"name",          {{"type","string"}}},
                {"default_value", {{"type","string"}}},
            }},
            {"required", nlohmann::json::array({"asset_path","name","default_value"})},
        };
        registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
            reader.SetVariableDefault(
                RequireString(args, "asset_path"),
                RequireString(args, "name"),
                args.value("default_value", ""));
            return nlohmann::json{{"ok", true}};
        });
    }

    // ===== Discoverability =================================================
    // These two tools return static metadata about the writable surface so a
    // model can ask "what are the valid `kind` values for add_node?" or
    // "what does a BPPinType for a struct ref look like?" without scanning
    // documentation. The lists are baked in to match the plugin's actual
    // dispatch — keep them in sync with BlueprintReaderCommandlet.cpp's
    // RunAddNodeOp + BlueprintReaderWireJson::ParseWirePinType.

    // ----- list_node_kinds -------------------------------------------------
    {
        ToolDescriptor d;
        d.name = "list_node_kinds";
        d.description =
            "List the `kind` values that `add_node` accepts, with required "
            "extras for each. Pure metadata — no backend call.";
        d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
        registry.Add(std::move(d), [](const nlohmann::json&) {
            return nlohmann::json::array({
                nlohmann::json{
                    {"kind", "Branch"},
                    {"class", "K2Node_IfThenElse"},
                    {"description", "Two-way exec branch on a bool condition pin."},
                    {"extras", nlohmann::json::array()},
                },
                nlohmann::json{
                    {"kind", "Sequence"},
                    {"class", "K2Node_ExecutionSequence"},
                    {"description", "Fires Then 0..N in order. Add more outputs by hand in the editor."},
                    {"extras", nlohmann::json::array()},
                },
                nlohmann::json{
                    {"kind", "VariableGet"},
                    {"class", "K2Node_VariableGet"},
                    {"description", "Read a member variable on Self."},
                    {"extras", nlohmann::json::array({
                        nlohmann::json{{"name","variable"}, {"required",true},
                                       {"description","Member variable name."}}
                    })},
                },
                nlohmann::json{
                    {"kind", "VariableSet"},
                    {"class", "K2Node_VariableSet"},
                    {"description", "Write a member variable on Self."},
                    {"extras", nlohmann::json::array({
                        nlohmann::json{{"name","variable"}, {"required",true},
                                       {"description","Member variable name."}}
                    })},
                },
                nlohmann::json{
                    {"kind", "CallFunction"},
                    {"class", "K2Node_CallFunction"},
                    {"description", "Call a UFUNCTION on a class."},
                    {"extras", nlohmann::json::array({
                        nlohmann::json{{"name","function"}, {"required",true},
                                       {"description","Function name as declared on the owning class."}},
                        nlohmann::json{{"name","function_owner"}, {"required",true},
                                       {"description","UClass path (`/Script/Engine.KismetSystemLibrary`) or short name (`KismetSystemLibrary`)."}}
                    })},
                },
                nlohmann::json{
                    {"kind", "CustomEvent"},
                    {"class", "K2Node_CustomEvent"},
                    {"description", "Defines a custom event entry point."},
                    {"extras", nlohmann::json::array({
                        nlohmann::json{{"name","event_name"}, {"required",true},
                                       {"description","FName for the new event."}}
                    })},
                },
                nlohmann::json{
                    {"kind", "Cast"},
                    {"class", "K2Node_DynamicCast"},
                    {"description", "Cast an object to a target class. Provides Cast Failed and As<TargetClass> output pins."},
                    {"extras", nlohmann::json::array({
                        nlohmann::json{{"name","target_class"}, {"required",true},
                                       {"description","UClass path or short name to cast to."}}
                    })},
                },
                nlohmann::json{
                    {"kind", "Self"},
                    {"class", "K2Node_Self"},
                    {"description", "Reference to Self (the owning blueprint instance)."},
                    {"extras", nlohmann::json::array()},
                },
                nlohmann::json{
                    {"kind", "MakeArray"},
                    {"class", "K2Node_MakeArray"},
                    {"description", "Build a TArray literal. Element type is wildcard until connected."},
                    {"extras", nlohmann::json::array()},
                },
                nlohmann::json{
                    {"kind", "MakeStruct"},
                    {"class", "K2Node_MakeStruct"},
                    {"description", "Build a USTRUCT literal. Each member becomes an input pin."},
                    {"extras", nlohmann::json::array({
                        nlohmann::json{{"name","struct_type"}, {"required",true},
                                       {"description","UScriptStruct path, e.g. /Script/CoreUObject.Vector."}}
                    })},
                },
                nlohmann::json{
                    {"kind", "FormatText"},
                    {"class", "K2Node_FormatText"},
                    {"description", "Format an FText with named arguments. Add args by editing the Format string."},
                    {"extras", nlohmann::json::array()},
                },
                nlohmann::json{
                    {"kind", "Knot"},
                    {"class", "K2Node_Knot"},
                    {"description", "Reroute node — pure visual, useful for cleaning up wire crossings."},
                    {"extras", nlohmann::json::array()},
                },
            });
        });
    }

    // ----- list_pin_categories ---------------------------------------------
    {
        ToolDescriptor d;
        d.name = "list_pin_categories";
        d.description =
            "List the canonical BPPinType.category values + container modifiers. "
            "Useful when constructing the `type` argument for add_variable. "
            "Pure metadata — no backend call.";
        d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
        registry.Add(std::move(d), [](const nlohmann::json&) {
            auto cat = [](const char* c, const char* desc, std::vector<std::string> subs = {},
                          const char* objHint = nullptr) {
                nlohmann::json j = {
                    {"category", c},
                    {"description", desc},
                    {"sub_categories", subs},
                };
                if (objHint) j["sub_category_object_hint"] = objHint;
                return j;
            };
            return nlohmann::json{
                {"categories", nlohmann::json::array({
                    cat("exec",      "Execution flow (white wires)."),
                    cat("bool",      "Boolean."),
                    cat("byte",      "8-bit unsigned integer (also used for enums).",
                        {}, "Optional UEnum path if the byte is an enum."),
                    cat("int",       "32-bit signed integer."),
                    cat("int64",     "64-bit signed integer."),
                    cat("real",      "Floating point. `sub_category` selects precision.",
                        {"float","double"}),
                    cat("string",    "FString."),
                    cat("name",      "FName."),
                    cat("text",      "FText."),
                    cat("object",    "UObject reference. `sub_category_object` = the UClass path.",
                        {}, "UClass path, e.g. /Script/Engine.Actor"),
                    cat("class",     "UClass reference. `sub_category_object` = the meta-class.",
                        {}, "UClass path"),
                    cat("interface", "Interface reference.",
                        {}, "UClass path of the interface"),
                    cat("struct",    "USTRUCT.",
                        {}, "UScriptStruct path, e.g. /Script/CoreUObject.Vector"),
                    cat("delegate",  "Single-cast delegate."),
                    cat("wildcard",  "Pin matches any type until connected (used in macros)."),
                })},
                {"containers", nlohmann::json::array({
                    nlohmann::json{{"flag","is_array"}, {"description","TArray<T>"}},
                    nlohmann::json{{"flag","is_set"},   {"description","TSet<T>"}},
                    nlohmann::json{{"flag","is_map"},   {"description","TMap<K,V> — note: only the key type is exposed via BPPinType today."}},
                })},
            };
        });
    }
}

} // namespace bpr::tools
