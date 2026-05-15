// BlueprintReaderTypes.h
//
// Shared POD types between the standalone MCP server and the in-engine
// BlueprintReader plugin. One header, two compile modes:
//
//   * Standalone path: plain C++ structs + nlohmann/json round-trip.
//   * UE path: USTRUCT/UPROPERTY-decorated mirrors for FJsonObjectConverter,
//     activated by `#define WITH_UE` before include.
//
// Wire format is identical on both sides: snake_case JSON keys.
//
// Pin meta is a free-form JSON object in the wire format. The standalone
// branch holds it as `nlohmann::json`. The UE branch stores it as an
// FString containing serialized JSON; the plugin's FJsonObjectConverter
// wrapper emits/parses that field as an inline object so the two sides
// agree on the wire.

#pragma once

    #include <cstdint>
    #include <optional>
    #include <string>
    #include <vector>
    #include <nlohmann/json.hpp>

    using BPRString = std::string;
    template <class T> using BPRArray = std::vector<T>;
    using BPROptionalString = std::optional<std::string>;
    using BPRJson = nlohmann::json;

// ----- BPPinType ------------------------------------------------------------

struct BPPinType
{
    BPRString Category;
    BPROptionalString SubCategory;
    BPROptionalString SubCategoryObject;
    bool IsArray = false;
    bool IsSet   = false;
    bool IsMap   = false;
};

// ----- BPPin ----------------------------------------------------------------

struct BPPinLink
{
    BPRString NodeId;
    BPRString PinId;
    BPRString PinName;  // additive; older fixtures decode it as empty
};
struct BPPin
{
    BPRString Id;
    BPRString Name;
    BPRString Direction;       // "Input" | "Output"
    BPPinType Type;
    BPROptionalString DefaultValue;
    // Inline view of this pin's connections — same data as the graph-
    // level connections[] array, but reachable from a single get_node
    // call rather than requiring a separate get_graph (issue #5).
    BPRArray<BPPinLink> LinkedTo;
};

// ----- BPPosition (inline {x,y} in wire format) -----------------------------

struct BPPosition
{
    int32_t X = 0;
    int32_t Y = 0;
};

// ----- BPNode ---------------------------------------------------------------

struct BPNode
{
    BPRString Id;
    BPRString Class;
    BPRString Title;
    BPPosition Position;
    BPROptionalString Comment;
    BPRArray<BPPin> Pins;
    BPRJson Meta = BPRJson::object();
    // Populated only by `find_node` results (which span every graph in the
    // BP). Empty in get_node / graph payloads, where the graph is implicit
    // from the caller's request. Fixes issue #6 — agents previously had no
    // way to call get_node/delete_node/wire_pins on a find_node hit because
    // those ops all require -Graph= and the find result didn't include it.
    BPROptionalString GraphName;
    BPROptionalString GraphType;
};

// ----- BPConnection ---------------------------------------------------------

struct BPConnection
{
    BPRString FromNode;
    BPRString FromPin;
    BPRString ToNode;
    BPRString ToPin;
};

// ----- BPGraphSummary -------------------------------------------------------

struct BPGraphSummary
{
    BPRString Name;
    BPRString Type;
};

// ----- BPGraph --------------------------------------------------------------

struct BPGraph
{
    BPRString Name;
    BPRString Type;
    BPRArray<BPNode> Nodes;
    BPRArray<BPConnection> Connections;
};

// ----- BPVariable -----------------------------------------------------------

struct BPVariable
{
    BPRString Name;
    BPPinType Type;
    BPROptionalString DefaultValue;
    BPROptionalString Category;
    bool IsReplicated = false;
    bool IsEditable   = false;
    BPROptionalString RepCondition;
    bool ExposeOnSpawn = false;
    BPROptionalString RepNotifyFunc;
};

// ----- BPFunctionSummary ----------------------------------------------------

struct BPFunctionSummary
{
    BPRString Name;
};

// ----- BPFunction -----------------------------------------------------------

struct BPFunction
{
    BPRString Name;
    BPRArray<BPVariable> Inputs;
    BPRArray<BPVariable> Outputs;
    BPRArray<BPVariable> Locals;
    BPGraph Graph;
};

// ----- BPAssetSummary -------------------------------------------------------

struct BPAssetSummary
{
    BPRString AssetPath;
    BPRString Name;
    BPRString ParentClass;
    BPRString ModifiedIso;
};

// ----- BPComponent ----------------------------------------------------------
// Returned by `get_components`. Mirrors the SCS hierarchy.

struct BPComponent
{
    BPRString Name;
    BPRString Class;
    BPROptionalString Parent;
    bool IsRoot = false;
};

// ----- BPMetadata -----------------------------------------------------------

struct BPMetadata
{
    BPRString AssetPath;
    BPRString Name;
    BPRString ParentClass;
    BPRArray<BPRString> Interfaces;
    BPRArray<BPVariable> Variables;
    BPRArray<BPFunctionSummary> Functions;
    BPRArray<BPRString> Macros;
    BPRArray<BPGraphSummary> Graphs;
};

// ============================================================================
// Standalone-only nlohmann/json adapters.
// Wire keys are snake_case (canonical wire shape).
// Optional<string> serializes to null when empty.
// ============================================================================

#ifndef WITH_UE
namespace nlohmann
{
    template <>
    struct adl_serializer<std::optional<std::string>>
    {
        static void to_json(json& j, const std::optional<std::string>& opt)
        {
            if (opt.has_value()) j = *opt;
            else                 j = nullptr;
        }
        static void from_json(const json& j, std::optional<std::string>& opt)
        {
            if (j.is_null()) opt.reset();
            else             opt = j.get<std::string>();
        }
    };
}

inline void to_json(nlohmann::json& j, const BPPinType& v)
{
    j = nlohmann::json{
        {"category",            v.Category},
        {"sub_category",        v.SubCategory},
        {"sub_category_object", v.SubCategoryObject},
        {"is_array",            v.IsArray},
        {"is_set",              v.IsSet},
        {"is_map",              v.IsMap},
    };
}
inline void from_json(const nlohmann::json& j, BPPinType& v)
{
    j.at("category").get_to(v.Category);
    j.at("sub_category").get_to(v.SubCategory);
    j.at("sub_category_object").get_to(v.SubCategoryObject);
    j.at("is_array").get_to(v.IsArray);
    j.at("is_set").get_to(v.IsSet);
    j.at("is_map").get_to(v.IsMap);
}

inline void to_json(nlohmann::json& j, const BPPinLink& v)
{
    j = nlohmann::json{
        {"node_id", v.NodeId},
        {"pin_id",  v.PinId},
        {"pin_name", v.PinName},
    };
}
inline void from_json(const nlohmann::json& j, BPPinLink& v)
{
    j.at("node_id").get_to(v.NodeId);
    j.at("pin_id").get_to(v.PinId);
    // pin_name was added later — older wire shapes / fixtures may omit it.
    if (j.contains("pin_name") && j["pin_name"].is_string()) {
        j["pin_name"].get_to(v.PinName);
    } else {
        v.PinName.clear();
    }
}

inline void to_json(nlohmann::json& j, const BPPin& v)
{
    j = nlohmann::json{
        {"id",            v.Id},
        {"name",          v.Name},
        {"direction",     v.Direction},
        {"type",          v.Type},
        {"default_value", v.DefaultValue},
        {"linked_to",     v.LinkedTo},
    };
}
inline void from_json(const nlohmann::json& j, BPPin& v)
{
    j.at("id").get_to(v.Id);
    j.at("name").get_to(v.Name);
    j.at("direction").get_to(v.Direction);
    j.at("type").get_to(v.Type);
    j.at("default_value").get_to(v.DefaultValue);
    // linked_to was added later (issue #5); older fixtures + older
    // plugin builds won't include it. Default to empty array.
    if (j.contains("linked_to") && j["linked_to"].is_array()) {
        j["linked_to"].get_to(v.LinkedTo);
    } else {
        v.LinkedTo.clear();
    }
}

inline void to_json(nlohmann::json& j, const BPPosition& v)
{
    j = nlohmann::json{ {"x", v.X}, {"y", v.Y} };
}
inline void from_json(const nlohmann::json& j, BPPosition& v)
{
    j.at("x").get_to(v.X);
    j.at("y").get_to(v.Y);
}

inline void to_json(nlohmann::json& j, const BPNode& v)
{
    j = nlohmann::json{
        {"id",       v.Id},
        {"class",    v.Class},
        {"title",    v.Title},
        {"position", v.Position},
        {"comment",  v.Comment},
        {"pins",     v.Pins},
        {"meta",     v.Meta},
    };
    // GraphName/GraphType only appear on find_node hits — emit only when
    // populated so get_node / graph payloads stay unchanged.
    if (v.GraphName.has_value()) j["graph_name"] = *v.GraphName;
    if (v.GraphType.has_value()) j["graph_type"] = *v.GraphType;
}
inline void from_json(const nlohmann::json& j, BPNode& v)
{
    j.at("id").get_to(v.Id);
    j.at("class").get_to(v.Class);
    j.at("title").get_to(v.Title);
    j.at("position").get_to(v.Position);
    j.at("comment").get_to(v.Comment);
    j.at("pins").get_to(v.Pins);
    if (j.contains("meta")) v.Meta = j.at("meta");
    else                    v.Meta = nlohmann::json::object();
    if (j.contains("graph_name") && j["graph_name"].is_string()) {
        v.GraphName = j["graph_name"].get<std::string>();
    }
    if (j.contains("graph_type") && j["graph_type"].is_string()) {
        v.GraphType = j["graph_type"].get<std::string>();
    }
}

inline void to_json(nlohmann::json& j, const BPConnection& v)
{
    j = nlohmann::json{
        {"from_node", v.FromNode},
        {"from_pin",  v.FromPin},
        {"to_node",   v.ToNode},
        {"to_pin",    v.ToPin},
    };
}
inline void from_json(const nlohmann::json& j, BPConnection& v)
{
    j.at("from_node").get_to(v.FromNode);
    j.at("from_pin").get_to(v.FromPin);
    j.at("to_node").get_to(v.ToNode);
    j.at("to_pin").get_to(v.ToPin);
}

inline void to_json(nlohmann::json& j, const BPGraphSummary& v)
{
    j = nlohmann::json{ {"name", v.Name}, {"type", v.Type} };
}
inline void from_json(const nlohmann::json& j, BPGraphSummary& v)
{
    j.at("name").get_to(v.Name);
    j.at("type").get_to(v.Type);
}

inline void to_json(nlohmann::json& j, const BPGraph& v)
{
    j = nlohmann::json{
        {"name",        v.Name},
        {"type",        v.Type},
        {"nodes",       v.Nodes},
        {"connections", v.Connections},
    };
}
inline void from_json(const nlohmann::json& j, BPGraph& v)
{
    j.at("name").get_to(v.Name);
    j.at("type").get_to(v.Type);
    j.at("nodes").get_to(v.Nodes);
    j.at("connections").get_to(v.Connections);
}

inline void to_json(nlohmann::json& j, const BPVariable& v)
{
    j = nlohmann::json{
        {"name",          v.Name},
        {"type",          v.Type},
        {"default_value", v.DefaultValue},
        {"category",      v.Category},
        {"is_replicated", v.IsReplicated},
        {"is_editable",   v.IsEditable},
    };
    // Optional fields — only emit when non-default so existing wire
    // consumers stay compatible. Old clients reading this don't see
    // anything unfamiliar; new clients reading old payloads default.
    if (v.RepCondition.has_value() && !v.RepCondition->empty()) {
        j["rep_condition"] = *v.RepCondition;
    }
    if (v.ExposeOnSpawn) {
        j["expose_on_spawn"] = true;
    }
    if (v.RepNotifyFunc.has_value() && !v.RepNotifyFunc->empty()) {
        j["rep_notify_func"] = *v.RepNotifyFunc;
    }
}
inline void from_json(const nlohmann::json& j, BPVariable& v)
{
    j.at("name").get_to(v.Name);
    j.at("type").get_to(v.Type);
    j.at("default_value").get_to(v.DefaultValue);
    j.at("category").get_to(v.Category);
    j.at("is_replicated").get_to(v.IsReplicated);
    j.at("is_editable").get_to(v.IsEditable);
    // Optional — older payloads omit these.
    if (auto it = j.find("rep_condition"); it != j.end() && it->is_string()) {
        v.RepCondition = it->get<std::string>();
    }
    if (auto it = j.find("expose_on_spawn"); it != j.end() && it->is_boolean()) {
        v.ExposeOnSpawn = it->get<bool>();
    }
    if (auto it = j.find("rep_notify_func"); it != j.end() && it->is_string()) {
        v.RepNotifyFunc = it->get<std::string>();
    }
}

inline void to_json(nlohmann::json& j, const BPFunctionSummary& v)
{
    j = nlohmann::json{ {"name", v.Name} };
}
inline void from_json(const nlohmann::json& j, BPFunctionSummary& v)
{
    j.at("name").get_to(v.Name);
}

inline void to_json(nlohmann::json& j, const BPFunction& v)
{
    j = nlohmann::json{
        {"name",    v.Name},
        {"inputs",  v.Inputs},
        {"outputs", v.Outputs},
        {"locals",  v.Locals},
        {"graph",   v.Graph},
    };
}
inline void from_json(const nlohmann::json& j, BPFunction& v)
{
    j.at("name").get_to(v.Name);
    j.at("inputs").get_to(v.Inputs);
    j.at("outputs").get_to(v.Outputs);
    j.at("locals").get_to(v.Locals);
    j.at("graph").get_to(v.Graph);
}

inline void to_json(nlohmann::json& j, const BPAssetSummary& v)
{
    j = nlohmann::json{
        {"asset_path",   v.AssetPath},
        {"name",         v.Name},
        {"parent_class", v.ParentClass},
        {"modified_iso", v.ModifiedIso},
    };
}
inline void from_json(const nlohmann::json& j, BPAssetSummary& v)
{
    j.at("asset_path").get_to(v.AssetPath);
    j.at("name").get_to(v.Name);
    j.at("parent_class").get_to(v.ParentClass);
    j.at("modified_iso").get_to(v.ModifiedIso);
}

inline void to_json(nlohmann::json& j, const BPMetadata& v)
{
    j = nlohmann::json{
        {"asset_path",   v.AssetPath},
        {"name",         v.Name},
        {"parent_class", v.ParentClass},
        {"interfaces",   v.Interfaces},
        {"variables",    v.Variables},
        {"functions",    v.Functions},
        {"macros",       v.Macros},
        {"graphs",       v.Graphs},
    };
}
inline void from_json(const nlohmann::json& j, BPMetadata& v)
{
    j.at("asset_path").get_to(v.AssetPath);
    j.at("name").get_to(v.Name);
    j.at("parent_class").get_to(v.ParentClass);
    j.at("interfaces").get_to(v.Interfaces);
    j.at("variables").get_to(v.Variables);
    j.at("functions").get_to(v.Functions);
    j.at("macros").get_to(v.Macros);
    j.at("graphs").get_to(v.Graphs);
}

inline void to_json(nlohmann::json& j, const BPComponent& v)
{
    j = nlohmann::json{
        {"name",    v.Name},
        {"class",   v.Class},
        {"parent",  v.Parent},
        {"is_root", v.IsRoot},
    };
}
inline void from_json(const nlohmann::json& j, BPComponent& v)
{
    j.at("name").get_to(v.Name);
    j.at("class").get_to(v.Class);
    j.at("parent").get_to(v.Parent);
    j.at("is_root").get_to(v.IsRoot);
}
#endif // !WITH_UE
