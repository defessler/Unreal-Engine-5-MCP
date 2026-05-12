// Round-trip every shape declared in BlueprintReaderTypes.h.

#include <doctest/doctest.h>

#include <BlueprintReaderTypes.h>

#include <nlohmann/json.hpp>

using nlohmann::json;

template <class T>
static T RoundTrip(const T& original) {
    json j = original;
    return j.template get<T>();
}

TEST_CASE("BPPinType round-trips with snake_case keys") {
    BPPinType pt;
    pt.Category = "exec";
    pt.SubCategory = std::nullopt;
    pt.SubCategoryObject = std::nullopt;
    pt.IsArray = false;
    pt.IsSet = true;
    pt.IsMap = false;

    json j = pt;
    CHECK(j["category"] == "exec");
    CHECK(j["sub_category"].is_null());
    CHECK(j["is_set"] == true);

    auto back = j.get<BPPinType>();
    CHECK(back.Category == pt.Category);
    CHECK_FALSE(back.SubCategory.has_value());
    CHECK(back.IsSet);
}

TEST_CASE("BPPin round-trips with optional default_value") {
    BPPin p;
    p.Id = "abc";
    p.Name = "InString";
    p.Direction = "Input";
    p.Type.Category = "string";
    p.DefaultValue = "hello";

    auto back = RoundTrip(p);
    CHECK(back.Id == "abc");
    CHECK(back.Direction == "Input");
    CHECK(back.DefaultValue.has_value());
    CHECK(*back.DefaultValue == "hello");

    p.DefaultValue.reset();
    auto j = json(p);
    CHECK(j["default_value"].is_null());
}

TEST_CASE("BPPin carries inline linked_to array (issue #5)") {
    // Verifies the per-pin connection view round-trips. An empty
    // linked_to is fine — every pin emits the field (possibly empty).
    BPPin p;
    p.Id = "src-pin";
    p.Name = "Then";
    p.Direction = "Output";
    p.Type.Category = "exec";
    BPPinLink l1{"target-node-guid", "target-pin-guid", "execute"};
    BPPinLink l2{"second-node-guid", "second-pin-guid", "ExecutePin"};
    p.LinkedTo.push_back(l1);
    p.LinkedTo.push_back(l2);

    auto j = json(p);
    REQUIRE(j.contains("linked_to"));
    REQUIRE(j["linked_to"].is_array());
    REQUIRE(j["linked_to"].size() == 2);
    CHECK(j["linked_to"][0]["node_id"] == "target-node-guid");
    CHECK(j["linked_to"][0]["pin_id"] == "target-pin-guid");
    // pin_name is part of the wire shape now — agents can verify wiring
    // by name without a follow-up get_node call.
    CHECK(j["linked_to"][0]["pin_name"] == "execute");
    CHECK(j["linked_to"][1]["pin_name"] == "ExecutePin");

    auto back = RoundTrip(p);
    REQUIRE(back.LinkedTo.size() == 2);
    CHECK(back.LinkedTo[0].NodeId == "target-node-guid");
    CHECK(back.LinkedTo[1].PinId == "second-pin-guid");
    CHECK(back.LinkedTo[0].PinName == "execute");

    // Older wire shape (no `linked_to` key) decodes as empty without
    // throwing — backward compat with pre-#5 fixtures.
    nlohmann::json legacy = {
        {"id", "x"}, {"name", "Y"}, {"direction", "Input"},
        {"type", {
            {"category","int"},
            {"sub_category", nullptr},
            {"sub_category_object", nullptr},
            {"is_array", false}, {"is_set", false}, {"is_map", false},
        }},
        {"default_value", nullptr},
    };
    auto legacyDecoded = legacy.get<BPPin>();
    CHECK(legacyDecoded.LinkedTo.empty());

    // Pre-pin_name wire shape (just node_id + pin_id) also decodes — a
    // forward-compat guarantee for any older client/server versions.
    nlohmann::json legacyLink = {
        {"node_id", "n"}, {"pin_id", "p"},
        // pin_name omitted
    };
    auto linkBack = legacyLink.get<BPPinLink>();
    CHECK(linkBack.NodeId == "n");
    CHECK(linkBack.PinId == "p");
    CHECK(linkBack.PinName.empty());
}

TEST_CASE("BPPosition uses x/y keys") {
    BPPosition pos{42, -7};
    json j = pos;
    CHECK(j["x"] == 42);
    CHECK(j["y"] == -7);
    auto back = j.get<BPPosition>();
    CHECK(back.X == 42);
    CHECK(back.Y == -7);
}

TEST_CASE("BPNode preserves meta as free-form object") {
    BPNode n;
    n.Id = "n0";
    n.Class = "K2Node_CallFunction";
    n.Title = "Print String";
    n.Position = {10, 20};
    n.Comment = std::nullopt;
    n.Meta = json{{"function_name", "PrintString"}, {"function_owner", "K"}};

    auto back = RoundTrip(n);
    CHECK(back.Class == n.Class);
    CHECK(back.Meta["function_name"] == "PrintString");
    CHECK(back.Meta["function_owner"] == "K");
}

TEST_CASE("BPConnection wire keys match canonical shape") {
    BPConnection c{"a", "b", "c", "d"};
    json j = c;
    CHECK(j["from_node"] == "a");
    CHECK(j["from_pin"] == "b");
    CHECK(j["to_node"] == "c");
    CHECK(j["to_pin"] == "d");
}

TEST_CASE("BPGraph round-trips with embedded nodes and connections") {
    BPGraph g;
    g.Name = "EventGraph";
    g.Type = "EventGraph";
    BPNode n;
    n.Id = "x";
    n.Class = "K2Node_Event";
    n.Title = "BeginPlay";
    g.Nodes.push_back(n);
    g.Connections.push_back({"x", "p0", "y", "p1"});
    auto back = RoundTrip(g);
    CHECK(back.Nodes.size() == 1);
    CHECK(back.Connections.size() == 1);
    CHECK(back.Connections[0].FromPin == "p0");
}

TEST_CASE("BPVariable round-trips") {
    BPVariable v;
    v.Name = "Health";
    v.Type.Category = "real";
    v.Type.SubCategory = "float";
    v.DefaultValue = "100.0";
    v.Category = "Combat";
    v.IsReplicated = true;
    v.IsEditable = true;
    auto back = RoundTrip(v);
    CHECK(back.Name == "Health");
    CHECK(back.IsReplicated);
    CHECK(back.Type.SubCategory.has_value());
    CHECK(*back.Type.SubCategory == "float");
}

TEST_CASE("BPFunction round-trips with inputs/outputs/locals") {
    BPFunction f;
    f.Name = "Foo";
    f.Inputs.push_back({});
    f.Inputs.back().Name = "in0";
    f.Inputs.back().Type.Category = "int";
    f.Outputs.push_back({});
    f.Outputs.back().Name = "out0";
    f.Outputs.back().Type.Category = "bool";
    f.Locals.push_back({});
    f.Locals.back().Name = "loc0";
    f.Locals.back().Type.Category = "real";
    f.Locals.back().Type.SubCategory = "float";
    f.Graph.Name = "Foo";
    f.Graph.Type = "Function";
    auto back = RoundTrip(f);
    CHECK(back.Inputs.size() == 1);
    CHECK(back.Outputs.size() == 1);
    CHECK(back.Locals.size() == 1);
    CHECK(back.Graph.Type == "Function");
}

TEST_CASE("BPAssetSummary uses snake_case wire keys") {
    BPAssetSummary s;
    s.AssetPath = "/Game/AI/BP_Foo";
    s.Name = "BP_Foo";
    s.ParentClass = "AActor";
    s.ModifiedIso = "2026-01-02T03:04:05Z";
    json j = s;
    CHECK(j["asset_path"] == "/Game/AI/BP_Foo");
    CHECK(j["parent_class"] == "AActor");
    CHECK(j["modified_iso"] == "2026-01-02T03:04:05Z");
    auto back = j.get<BPAssetSummary>();
    CHECK(back.Name == "BP_Foo");
}

TEST_CASE("BPMetadata round-trips with arrays") {
    BPMetadata m;
    m.AssetPath = "/Game/X";
    m.Name = "X";
    m.ParentClass = "AActor";
    m.Interfaces = {"IA", "IB"};
    m.Macros = {"M1"};
    m.Functions.push_back({"Fn"});
    m.Graphs.push_back({"EventGraph", "EventGraph"});
    auto back = RoundTrip(m);
    CHECK(back.Interfaces.size() == 2);
    CHECK(back.Functions.size() == 1);
    CHECK(back.Graphs[0].Type == "EventGraph");
}
