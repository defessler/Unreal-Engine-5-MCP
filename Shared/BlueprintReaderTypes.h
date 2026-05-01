// BlueprintReaderTypes.h
//
// Shared POD types between the standalone MCP server (Phase 0) and the
// in-engine BlueprintReader plugin (Phase 1+). One header, two compile modes:
//
//   * Standalone path: plain C++ structs + nlohmann/json round-trip.
//   * UE path: USTRUCT/UPROPERTY-decorated mirrors for FJsonObjectConverter.
//
// Wire format is identical on both sides: snake_case JSON keys, exact field
// shapes from PLAN.md.
//
// Phase 0 only needs the standalone branch. The UE branch is laid out so the
// plugin task can drop it into a UE module by defining WITH_UE before include.
//
// Notes for the UE branch:
//   * Pin meta is a free-form JSON object in the wire format. The standalone
//     branch holds it as `nlohmann::json`. The UE branch stores it as an FString
//     containing serialized JSON; the plugin's FJsonObjectConverter wrapper is
//     responsible for emitting/parsing that field as an inline object so the
//     two sides agree on the wire.

#pragma once

#ifdef WITH_UE
    #include "CoreMinimal.h"
    #include "BlueprintReaderTypes.generated.h"

    using BPRString = FString;
    template <class T> using BPRArray = TArray<T>;
    using BPROptionalString = FString; // empty FString == null
    using BPRJson = FString;            // serialized JSON object; see header notes
#else
    #include <cstdint>
    #include <optional>
    #include <string>
    #include <vector>
    #include <nlohmann/json.hpp>

    using BPRString = std::string;
    template <class T> using BPRArray = std::vector<T>;
    using BPROptionalString = std::optional<std::string>;
    using BPRJson = nlohmann::json;
#endif

// ----- BPPinType ------------------------------------------------------------

#ifdef WITH_UE
USTRUCT()
struct FBPPinType
{
    GENERATED_BODY()
    UPROPERTY() FString Category;
    UPROPERTY() FString SubCategory;
    UPROPERTY() FString SubCategoryObject;
    UPROPERTY() bool    bIsArray = false;
    UPROPERTY() bool    bIsSet   = false;
    UPROPERTY() bool    bIsMap   = false;
};
#else
struct BPPinType
{
    BPRString Category;
    BPROptionalString SubCategory;
    BPROptionalString SubCategoryObject;
    bool IsArray = false;
    bool IsSet   = false;
    bool IsMap   = false;
};
#endif

// ----- BPPin ----------------------------------------------------------------

#ifdef WITH_UE
USTRUCT()
struct FBPPin
{
    GENERATED_BODY()
    UPROPERTY() FString    Id;
    UPROPERTY() FString    Name;
    UPROPERTY() FString    Direction;
    UPROPERTY() FBPPinType Type;
    UPROPERTY() FString    DefaultValue;
};
#else
struct BPPin
{
    BPRString Id;
    BPRString Name;
    BPRString Direction;       // "Input" | "Output"
    BPPinType Type;
    BPROptionalString DefaultValue;
};
#endif

// ----- BPPosition (inline {x,y} in wire format) -----------------------------

#ifdef WITH_UE
USTRUCT()
struct FBPPosition
{
    GENERATED_BODY()
    UPROPERTY() int32 X = 0;
    UPROPERTY() int32 Y = 0;
};
#else
struct BPPosition
{
    int32_t X = 0;
    int32_t Y = 0;
};
#endif

// ----- BPNode ---------------------------------------------------------------

#ifdef WITH_UE
USTRUCT()
struct FBPNode
{
    GENERATED_BODY()
    UPROPERTY() FString         Id;
    UPROPERTY() FString         Class;
    UPROPERTY() FString         Title;
    UPROPERTY() FBPPosition     Position;
    UPROPERTY() FString         Comment;
    UPROPERTY() TArray<FBPPin>  Pins;
    UPROPERTY() FString         Meta; // serialized JSON object
};
#else
struct BPNode
{
    BPRString Id;
    BPRString Class;
    BPRString Title;
    BPPosition Position;
    BPROptionalString Comment;
    BPRArray<BPPin> Pins;
    BPRJson Meta = BPRJson::object();
};
#endif

// ----- BPConnection ---------------------------------------------------------

#ifdef WITH_UE
USTRUCT()
struct FBPConnection
{
    GENERATED_BODY()
    UPROPERTY() FString FromNode;
    UPROPERTY() FString FromPin;
    UPROPERTY() FString ToNode;
    UPROPERTY() FString ToPin;
};
#else
struct BPConnection
{
    BPRString FromNode;
    BPRString FromPin;
    BPRString ToNode;
    BPRString ToPin;
};
#endif

// ----- BPGraphSummary -------------------------------------------------------

#ifdef WITH_UE
USTRUCT()
struct FBPGraphSummary
{
    GENERATED_BODY()
    UPROPERTY() FString Name;
    UPROPERTY() FString Type; // "EventGraph" | "Function" | "Macro" | "Construction"
};
#else
struct BPGraphSummary
{
    BPRString Name;
    BPRString Type;
};
#endif

// ----- BPGraph --------------------------------------------------------------

#ifdef WITH_UE
USTRUCT()
struct FBPGraph
{
    GENERATED_BODY()
    UPROPERTY() FString               Name;
    UPROPERTY() FString               Type;
    UPROPERTY() TArray<FBPNode>       Nodes;
    UPROPERTY() TArray<FBPConnection> Connections;
};
#else
struct BPGraph
{
    BPRString Name;
    BPRString Type;
    BPRArray<BPNode> Nodes;
    BPRArray<BPConnection> Connections;
};
#endif

// ----- BPVariable -----------------------------------------------------------

#ifdef WITH_UE
USTRUCT()
struct FBPVariable
{
    GENERATED_BODY()
    UPROPERTY() FString    Name;
    UPROPERTY() FBPPinType Type;
    UPROPERTY() FString    DefaultValue;
    UPROPERTY() FString    Category;
    UPROPERTY() bool       bIsReplicated = false;
    UPROPERTY() bool       bIsEditable   = false;
};
#else
struct BPVariable
{
    BPRString Name;
    BPPinType Type;
    BPROptionalString DefaultValue;
    BPROptionalString Category;
    bool IsReplicated = false;
    bool IsEditable   = false;
};
#endif

// ----- BPFunctionSummary ----------------------------------------------------

#ifdef WITH_UE
USTRUCT()
struct FBPFunctionSummary
{
    GENERATED_BODY()
    UPROPERTY() FString Name;
};
#else
struct BPFunctionSummary
{
    BPRString Name;
};
#endif

// ----- BPFunction -----------------------------------------------------------

#ifdef WITH_UE
USTRUCT()
struct FBPFunction
{
    GENERATED_BODY()
    UPROPERTY() FString             Name;
    UPROPERTY() TArray<FBPVariable> Inputs;
    UPROPERTY() TArray<FBPVariable> Outputs;
    UPROPERTY() TArray<FBPVariable> Locals;
    UPROPERTY() FBPGraph            Graph;
};
#else
struct BPFunction
{
    BPRString Name;
    BPRArray<BPVariable> Inputs;
    BPRArray<BPVariable> Outputs;
    BPRArray<BPVariable> Locals;
    BPGraph Graph;
};
#endif

// ----- BPAssetSummary -------------------------------------------------------

#ifdef WITH_UE
USTRUCT()
struct FBPAssetSummary
{
    GENERATED_BODY()
    UPROPERTY() FString AssetPath;
    UPROPERTY() FString Name;
    UPROPERTY() FString ParentClass;
    UPROPERTY() FString ModifiedIso;
};
#else
struct BPAssetSummary
{
    BPRString AssetPath;
    BPRString Name;
    BPRString ParentClass;
    BPRString ModifiedIso;
};
#endif

// ----- BPMetadata -----------------------------------------------------------

#ifdef WITH_UE
USTRUCT()
struct FBPMetadata
{
    GENERATED_BODY()
    UPROPERTY() FString                    AssetPath;
    UPROPERTY() FString                    Name;
    UPROPERTY() FString                    ParentClass;
    UPROPERTY() TArray<FString>            Interfaces;
    UPROPERTY() TArray<FBPVariable>        Variables;
    UPROPERTY() TArray<FBPFunctionSummary> Functions;
    UPROPERTY() TArray<FString>            Macros;
    UPROPERTY() TArray<FBPGraphSummary>    Graphs;
};
#else
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
#endif

// ============================================================================
// Standalone-only nlohmann/json adapters.
// Wire keys are snake_case to match PLAN.md's canonical shape.
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

inline void to_json(nlohmann::json& j, const BPPin& v)
{
    j = nlohmann::json{
        {"id",            v.Id},
        {"name",          v.Name},
        {"direction",     v.Direction},
        {"type",          v.Type},
        {"default_value", v.DefaultValue},
    };
}
inline void from_json(const nlohmann::json& j, BPPin& v)
{
    j.at("id").get_to(v.Id);
    j.at("name").get_to(v.Name);
    j.at("direction").get_to(v.Direction);
    j.at("type").get_to(v.Type);
    j.at("default_value").get_to(v.DefaultValue);
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
}
inline void from_json(const nlohmann::json& j, BPVariable& v)
{
    j.at("name").get_to(v.Name);
    j.at("type").get_to(v.Type);
    j.at("default_value").get_to(v.DefaultValue);
    j.at("category").get_to(v.Category);
    j.at("is_replicated").get_to(v.IsReplicated);
    j.at("is_editable").get_to(v.IsEditable);
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
#endif // !WITH_UE
