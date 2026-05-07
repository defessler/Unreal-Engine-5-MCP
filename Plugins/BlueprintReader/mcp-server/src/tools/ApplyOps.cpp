#include "tools/ApplyOps.h"
#include "tools/TypeShorthand.h"

#include <fmt/core.h>

#include <map>
#include <stdexcept>
#include <string>
#include <string_view>

namespace bpr::tools {

namespace {

// ----- Slot resolution ---------------------------------------------------
// id -> minted GUID. Populated as add_node ops complete; consumed by
// wire_pins / set_node_position / delete_node refs in later ops.
using SlotMap = std::map<std::string, std::string, std::less<>>;

// Resolve a node-id field. Supported forms (in priority order):
//   1. {"ref": "<id>"}       — explicit slot ref
//   2. "$<id>"               — sigil-prefixed slot ref
//   3. raw GUID string       — pass through unchanged
// Throws if a slot ref doesn't resolve.
std::string ResolveNodeRef(const nlohmann::json& field, const SlotMap& slots,
                           std::string_view key) {
    if (field.is_object()) {
        auto it = field.find("ref");
        if (it == field.end() || !it->is_string()) {
            throw std::invalid_argument(fmt::format(
                "field '{}' is an object but lacks a string \"ref\"", key));
        }
        const auto& id = it->get_ref<const std::string&>();
        auto sit = slots.find(id);
        if (sit == slots.end()) {
            throw std::invalid_argument(fmt::format(
                R"(field "{}" references slot "{}" which has not been bound — )"
                "did an earlier op fail or did you typo the id?", key, id));
        }
        return sit->second;
    }
    if (field.is_string()) {
        const auto& s = field.get_ref<const std::string&>();
        if (!s.empty() && s.front() == '$') {
            std::string id = s.substr(1);
            auto sit = slots.find(id);
            if (sit == slots.end()) {
                throw std::invalid_argument(fmt::format(
                    R"(field "{}" references slot "${}" which has not been bound)",
                    key, id));
            }
            return sit->second;
        }
        return s;
    }
    throw std::invalid_argument(fmt::format(
        "field '{}' must be a string GUID, a $slot reference, or {{\"ref\":\"<id>\"}}",
        key));
}

std::string GetString(const nlohmann::json& obj, std::string_view key) {
    auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) {
        throw std::invalid_argument(fmt::format(
            R"(missing or non-string field "{}")", key));
    }
    return it->get<std::string>();
}

std::string OptStr(const nlohmann::json& obj, std::string_view key,
                   std::string fallback = {}) {
    auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) return fallback;
    if (!it->is_string()) {
        throw std::invalid_argument(fmt::format(
            R"(field "{}" must be a string)", key));
    }
    return it->get<std::string>();
}

int GetInt(const nlohmann::json& obj, std::string_view key) {
    auto it = obj.find(key);
    if (it == obj.end() || !it->is_number_integer()) {
        throw std::invalid_argument(fmt::format(
            R"(missing or non-integer field "{}")", key));
    }
    return it->get<int>();
}

// ----- Op dispatchers ----------------------------------------------------
// Each dispatcher returns the per-op result JSON. They MUST NOT swallow
// exceptions — the outer driver catches and decides whether to bail or
// continue based on `atomic`.

nlohmann::json OpAddVariable(backends::IBlueprintReader& reader,
                             const nlohmann::json& op, SlotMap&) {
    std::string asset = GetString(op, "asset_path");
    std::string name  = GetString(op, "name");
    auto typeIt = op.find("type");
    if (typeIt == op.end()) {
        throw std::invalid_argument(R"(add_variable op requires "type")");
    }
    BPPinType type = ParseTypeArg(*typeIt);
    std::string defaultValue = OptStr(op, "default_value", "");
    std::string category     = OptStr(op, "category",      "");
    bool replicated = op.value("replicated", false);
    bool editable   = op.value("editable",   false);

    // Idempotency.
    try {
        for (const auto& v : reader.ListVariables(asset)) {
            if (v.Name == name) {
                return {{"ok", true}, {"already_existed", true}};
            }
        }
    } catch (...) { /* fall through */ }
    reader.AddVariable(asset, name, type, defaultValue, category, replicated, editable);
    return {{"ok", true}, {"already_existed", false}};
}

nlohmann::json OpAddFunction(backends::IBlueprintReader& reader,
                             const nlohmann::json& op, SlotMap&) {
    std::string asset = GetString(op, "asset_path");
    std::string name  = GetString(op, "name");
    try {
        auto meta = reader.ReadBlueprint(asset);
        for (const auto& fn : meta.Functions) {
            if (fn.Name == name) {
                return {{"ok", true}, {"function_name", name}, {"already_existed", true}};
            }
        }
    } catch (...) { /* fall through */ }
    auto echoed = reader.AddFunction(asset, name);
    return {{"ok", true}, {"function_name", echoed}, {"already_existed", false}};
}

nlohmann::json OpAddFunctionParam(backends::IBlueprintReader& reader,
                                  const nlohmann::json& op, SlotMap&,
                                  bool isOutput) {
    std::string asset = GetString(op, "asset_path");
    std::string fn    = GetString(op, "function_name");
    std::string param = GetString(op, "param_name");
    auto typeIt = op.find("type");
    if (typeIt == op.end()) {
        throw std::invalid_argument(
            R"(add_function_input/output op requires "type")");
    }
    BPPinType type = ParseTypeArg(*typeIt);
    if (isOutput) reader.AddFunctionOutput(asset, fn, param, type);
    else          reader.AddFunctionInput (asset, fn, param, type);
    return {{"ok", true}};
}

nlohmann::json OpAddNode(backends::IBlueprintReader& reader,
                         const nlohmann::json& op, SlotMap& slots) {
    std::string asset = GetString(op, "asset_path");
    std::string graph = GetString(op, "graph_name");
    std::string kind  = GetString(op, "kind");
    int x = GetInt(op, "x");
    int y = GetInt(op, "y");
    std::map<std::string, std::string, std::less<>> extras;
    auto put = [&](const char* mcpKey, const char* flagKey) {
        std::string v = OptStr(op, mcpKey, "");
        if (!v.empty()) extras.emplace(flagKey, std::move(v));
    };
    put("variable",       "Variable");
    put("function",       "Function");
    put("function_owner", "FunctionOwner");
    put("event_name",     "EventName");
    put("target_class",   "TargetClass");
    put("struct_type",    "StructType");
    std::string newId = reader.AddNode(asset, graph, kind, x, y, extras);

    // Bind to slot if `id` is set.
    if (auto idIt = op.find("id"); idIt != op.end() && idIt->is_string()) {
        slots[idIt->get<std::string>()] = newId;
    }

    // Pin enrichment — same as add_node tool.
    nlohmann::json pinsJson = nlohmann::json::array();
    try {
        auto g = reader.GetGraph(asset, graph);
        for (const auto& n : g.Nodes) {
            if (n.Id == newId) {
                for (const auto& p : n.Pins) {
                    nlohmann::json t = {{"category", p.Type.Category}};
                    if (p.Type.SubCategory)       t["sub_category"]        = *p.Type.SubCategory;
                    if (p.Type.SubCategoryObject) t["sub_category_object"] = *p.Type.SubCategoryObject;
                    if (p.Type.IsArray) t["is_array"] = true;
                    if (p.Type.IsSet)   t["is_set"]   = true;
                    if (p.Type.IsMap)   t["is_map"]   = true;
                    pinsJson.push_back({
                        {"name",      p.Name},
                        {"guid",      p.Id},
                        {"direction", p.Direction},
                        {"type",      std::move(t)},
                    });
                }
                break;
            }
        }
    } catch (...) { /* best-effort */ }
    return {{"ok", true}, {"node_id", newId}, {"pins", std::move(pinsJson)}};
}

nlohmann::json OpWirePins(backends::IBlueprintReader& reader,
                          const nlohmann::json& op, SlotMap& slots) {
    std::string asset = GetString(op, "asset_path");
    std::string graph = GetString(op, "graph_name");
    std::string fromNode, toNode;
    {
        auto fnIt = op.find("from_node");
        auto tnIt = op.find("to_node");
        if (fnIt == op.end() || tnIt == op.end()) {
            throw std::invalid_argument(
                R"(wire_pins op requires "from_node" and "to_node")");
        }
        fromNode = ResolveNodeRef(*fnIt, slots, "from_node");
        toNode   = ResolveNodeRef(*tnIt, slots, "to_node");
    }
    std::string fromPin = GetString(op, "from_pin");
    std::string toPin   = GetString(op, "to_pin");
    reader.WirePins(asset, graph, fromNode, fromPin, toNode, toPin);
    return {{"ok", true}};
}

nlohmann::json OpSetNodePosition(backends::IBlueprintReader& reader,
                                 const nlohmann::json& op, SlotMap& slots) {
    std::string asset = GetString(op, "asset_path");
    std::string graph = GetString(op, "graph_name");
    auto nIt = op.find("node_id");
    if (nIt == op.end()) {
        throw std::invalid_argument(
            R"(set_node_position op requires "node_id")");
    }
    std::string node = ResolveNodeRef(*nIt, slots, "node_id");
    int x = GetInt(op, "x");
    int y = GetInt(op, "y");
    reader.SetNodePosition(asset, graph, node, x, y);
    return {{"ok", true}};
}

nlohmann::json OpDeleteNode(backends::IBlueprintReader& reader,
                            const nlohmann::json& op, SlotMap& slots) {
    std::string asset = GetString(op, "asset_path");
    std::string graph = GetString(op, "graph_name");
    auto nIt = op.find("node_id");
    if (nIt == op.end()) {
        throw std::invalid_argument(R"(delete_node op requires "node_id")");
    }
    std::string node = ResolveNodeRef(*nIt, slots, "node_id");
    reader.DeleteNode(asset, graph, node);
    return {{"ok", true}};
}

nlohmann::json OpDeleteVariable(backends::IBlueprintReader& reader,
                                const nlohmann::json& op, SlotMap&) {
    reader.DeleteVariable(GetString(op, "asset_path"), GetString(op, "name"));
    return {{"ok", true}};
}

nlohmann::json OpRenameVariable(backends::IBlueprintReader& reader,
                                const nlohmann::json& op, SlotMap&) {
    reader.RenameVariable(GetString(op, "asset_path"),
                          GetString(op, "old_name"),
                          GetString(op, "new_name"));
    return {{"ok", true}};
}

nlohmann::json OpDeleteFunction(backends::IBlueprintReader& reader,
                                const nlohmann::json& op, SlotMap&) {
    reader.DeleteFunction(GetString(op, "asset_path"), GetString(op, "name"));
    return {{"ok", true}};
}

nlohmann::json OpSetVariableDefault(backends::IBlueprintReader& reader,
                                    const nlohmann::json& op, SlotMap&) {
    reader.SetVariableDefault(GetString(op, "asset_path"),
                              GetString(op, "name"),
                              OptStr(op, "default_value", ""));
    return {{"ok", true}};
}

// Dispatch one op. Caller chooses whether to catch.
nlohmann::json DispatchOp(backends::IBlueprintReader& reader,
                          const nlohmann::json& op, SlotMap& slots) {
    auto it = op.find("op");
    if (it == op.end() || !it->is_string()) {
        throw std::invalid_argument(R"(every op requires a string "op" field)");
    }
    const auto& kind = it->get_ref<const std::string&>();
    if (kind == "add_variable")         return OpAddVariable    (reader, op, slots);
    if (kind == "delete_variable")      return OpDeleteVariable (reader, op, slots);
    if (kind == "rename_variable")      return OpRenameVariable (reader, op, slots);
    if (kind == "set_variable_default") return OpSetVariableDefault(reader, op, slots);
    if (kind == "add_function")         return OpAddFunction    (reader, op, slots);
    if (kind == "add_function_input")   return OpAddFunctionParam(reader, op, slots, /*isOutput=*/false);
    if (kind == "add_function_output")  return OpAddFunctionParam(reader, op, slots, /*isOutput=*/true);
    if (kind == "delete_function")      return OpDeleteFunction (reader, op, slots);
    if (kind == "add_node")             return OpAddNode        (reader, op, slots);
    if (kind == "wire_pins")            return OpWirePins       (reader, op, slots);
    if (kind == "set_node_position")    return OpSetNodePosition(reader, op, slots);
    if (kind == "delete_node")          return OpDeleteNode     (reader, op, slots);
    throw std::invalid_argument(fmt::format(
        "unknown op '{}'. Supported: add_variable, delete_variable, "
        "rename_variable, set_variable_default, add_function, "
        "add_function_input, add_function_output, delete_function, "
        "add_node, wire_pins, set_node_position, delete_node", kind));
}

} // namespace

nlohmann::json RunOps(backends::IBlueprintReader& reader,
                      const nlohmann::json& ops, bool atomic) {
    if (!ops.is_array()) {
        throw std::invalid_argument(R"(RunOps requires "ops" to be an array)");
    }
    SlotMap slots;
    nlohmann::json results = nlohmann::json::array();
    int succeeded = 0, failed = 0;
    for (std::size_t i = 0; i < ops.size(); ++i) {
        const auto& op = ops[i];
        try {
            results.push_back(DispatchOp(reader, op, slots));
            ++succeeded;
        } catch (const std::exception& e) {
            ++failed;
            if (atomic) {
                throw bpr::backends::BlueprintReaderError(fmt::format(
                    "apply_ops failed at op[{}] (op=\"{}\"): {}",
                    i,
                    op.contains("op") && op["op"].is_string()
                        ? op["op"].get<std::string>() : "<missing>",
                    e.what()));
            }
            results.push_back({
                {"ok", false},
                {"op_index", i},
                {"error", e.what()},
            });
        }
    }
    return nlohmann::json{
        {"ok", failed == 0},
        {"succeeded", succeeded},
        {"failed",    failed},
        {"slots",     slots},
        {"results",   std::move(results)},
    };
}

void RegisterApplyOps(ToolRegistry& registry, backends::IBlueprintReader& reader) {
    ToolDescriptor d;
    d.name = "apply_ops";
    d.description =
        "Execute a batch of write operations sequentially in a single tool "
        "call. Each op is `{op:\"add_variable\"|...|\"wire_pins\", ...args}`. "
        "Reduces N round-trips and N agent reasoning steps to one.\n\n"
        "Named slots: an `add_node` op may carry `id: \"<name>\"`. Subsequent "
        "ops can reference that node's GUID with `\"$<name>\"` or "
        "`{\"ref\":\"<name>\"}` in any node-id field. Eliminates the need to "
        "thread GUIDs through the agent's reasoning.\n\n"
        "`atomic: true` (default) bails on first error. `atomic: false` "
        "continues; failed ops appear as `{ok: false, error: \"...\"}` in "
        "the per-op results array, succeeded ops as their normal payload.\n\n"
        "Limitation: each underlying op still saves + recompiles the BP "
        "individually. True single-recompile batching needs plugin work — "
        "tracked separately.";
    d.input_schema = {
        {"type", "object"},
        {"properties", {
            {"ops", {
                {"type", "array"},
                {"items", {
                    {"type", "object"},
                    {"properties", {
                        {"op", {{"type","string"}}},
                        {"id", {{"type","string"},
                                {"description","Optional slot name. For node-spawning ops, binds the new GUID under this name for later refs."}}},
                    }},
                    {"required", nlohmann::json::array({"op"})},
                    {"additionalProperties", true},
                }},
            }},
            {"atomic", {{"type","boolean"},
                        {"description","Bail on first failure (default true)."}}},
        }},
        {"required", nlohmann::json::array({"ops"})},
    };
    registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
        auto opsIt = args.find("ops");
        if (opsIt == args.end() || !opsIt->is_array()) {
            throw std::invalid_argument(
                R"(apply_ops requires "ops" to be an array)");
        }
        bool atomic = args.value("atomic", true);
        return RunOps(reader, *opsIt, atomic);
    });
}

} // namespace bpr::tools
