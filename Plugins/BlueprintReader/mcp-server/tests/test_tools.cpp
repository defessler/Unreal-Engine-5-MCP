// Test the BlueprintTools layer directly — bypasses MCP framing, calls the
// tool handlers as plain functions of `arguments`.

#include <doctest/doctest.h>

#include "backends/MockBlueprintReader.h"
#include "tools/BlueprintTools.h"
#include "tools/ToolRegistry.h"

#include "test_helpers.h"

using namespace bpr;
using nlohmann::json;

namespace {

struct Fixture {
    backends::MockBlueprintReader reader;
    tools::ToolRegistry registry;
    Fixture() : reader(test::FixturesDir()) {
        tools::RegisterBlueprintTools(registry, reader);
    }
    json Call(const std::string& name, json args) {
        const auto* fn = registry.Find(name);
        REQUIRE(fn != nullptr);
        return (*fn)(args);
    }
};

} // namespace

TEST_CASE("ToolRegistry exposes 22 tools (8 read + 12 write + 2 meta) with input schemas") {
    Fixture f;
    auto spec = f.registry.ListSpec();
    CHECK(spec.size() == 22);
    for (const auto& t : spec) {
        CHECK(t["inputSchema"]["type"] == "object");
    }
}

TEST_CASE("Discoverability: list_node_kinds returns the dispatch table") {
    Fixture f;
    auto out = f.Call("list_node_kinds", json::object());
    REQUIRE(out.is_array());
    CHECK(out.size() == 12);
    std::vector<std::string> kinds;
    for (auto& k : out) kinds.push_back(k["kind"].get<std::string>());
    auto has = [&](const std::string& s) {
        return std::find(kinds.begin(), kinds.end(), s) != kinds.end();
    };
    for (const char* k : {"Branch","Sequence","VariableGet","VariableSet","CallFunction",
                          "CustomEvent","Cast","Self","MakeArray","MakeStruct",
                          "FormatText","Knot"}) {
        CHECK(has(k));
    }
    // CallFunction declares its required extras.
    for (auto& k : out) {
        if (k["kind"] == "CallFunction") {
            REQUIRE(k["extras"].is_array());
            CHECK(k["extras"].size() == 2);
        }
    }
}

TEST_CASE("Discoverability: list_pin_categories returns categories + containers") {
    Fixture f;
    auto out = f.Call("list_pin_categories", json::object());
    REQUIRE(out.contains("categories"));
    REQUIRE(out["categories"].is_array());
    CHECK(out["categories"].size() >= 14);
    REQUIRE(out.contains("containers"));
    CHECK(out["containers"].size() == 3);
}

TEST_CASE("Write tools throw on the mock backend (read-only by design)") {
    Fixture f;
    CHECK_THROWS_AS(f.Call("add_variable", json{
        {"asset_path", "/Game/AI/BP_Enemy"},
        {"name", "NewVar"},
        {"type", json{{"category","bool"}}}
    }), bpr::backends::BlueprintReaderError);
    CHECK_THROWS_AS(f.Call("set_node_position", json{
        {"asset_path","/Game/AI/BP_Enemy"}, {"graph_name","EventGraph"},
        {"node_id","00000000-0000-0000-0000-000000000000"}, {"x",0}, {"y",0}
    }), bpr::backends::BlueprintReaderError);
    CHECK_THROWS_AS(f.Call("delete_node", json{
        {"asset_path","/Game/AI/BP_Enemy"}, {"graph_name","EventGraph"},
        {"node_id","00000000-0000-0000-0000-000000000000"}
    }), bpr::backends::BlueprintReaderError);
}

TEST_CASE("list_blueprints returns canonical BPAssetSummary array") {
    Fixture f;
    auto out = f.Call("list_blueprints", json{{"path", "/Game"}});
    REQUIRE(out.is_array());
    CHECK(out.size() == 3);
    CHECK(out[0].contains("asset_path"));
    CHECK(out[0].contains("parent_class"));
    CHECK(out[0].contains("modified_iso"));
}

TEST_CASE("read_blueprint returns canonical BPMetadata") {
    Fixture f;
    auto out = f.Call("read_blueprint", json{{"asset_path", "/Game/Items/BP_Pickup"}});
    CHECK(out["name"] == "BP_Pickup");
    CHECK(out["parent_class"] == "AActor");
    REQUIRE(out["interfaces"].is_array());
    CHECK(out["interfaces"][0] == "IInteractable");
}

TEST_CASE("get_graph default name is EventGraph") {
    Fixture f;
    auto out = f.Call("get_graph", json{{"asset_path", "/Game/AI/BP_Enemy"}});
    CHECK(out["name"] == "EventGraph");
    CHECK(out["nodes"].size() >= 6);
}

TEST_CASE("get_function returns full function shape") {
    Fixture f;
    auto out = f.Call("get_function", json{
        {"asset_path", "/Game/Player/BP_PlayerController"},
        {"function_name", "AddScore"}});
    CHECK(out["name"] == "AddScore");
    CHECK(out["locals"].size() == 2);
    CHECK(out["graph"]["type"] == "Function");
}

TEST_CASE("list_variables returns the variables array") {
    Fixture f;
    auto out = f.Call("list_variables", json{{"asset_path", "/Game/AI/BP_Enemy"}});
    REQUIRE(out.is_array());
    CHECK(out.size() == 3);
    CHECK(out[0].contains("is_replicated"));
}

TEST_CASE("find_node returns matching nodes") {
    Fixture f;
    auto out = f.Call("find_node", json{
        {"asset_path", "/Game/AI/BP_Enemy"},
        {"query", "Sequence"}});
    REQUIRE(out.is_array());
    REQUIRE(out.size() == 1);
    CHECK(out[0]["class"] == "K2Node_ExecutionSequence");
}

TEST_CASE("Tool handlers throw on missing required arg") {
    Fixture f;
    CHECK_THROWS_AS(f.Call("read_blueprint", json::object()), std::invalid_argument);
    CHECK_THROWS_AS(f.Call("get_function", json{{"asset_path", "/Game/AI/BP_Enemy"}}),
                    std::invalid_argument);
}

// ===== Response controls (fields / limit / offset) =========================

TEST_CASE("summarize_blueprint returns counts plus parent_class") {
    Fixture f;
    auto out = f.Call("summarize_blueprint", json{{"asset_path", "/Game/AI/BP_Enemy"}});
    CHECK(out["asset_path"] == "/Game/AI/BP_Enemy");
    CHECK(out["parent_class"] == "ACharacter");
    CHECK(out["variable_count"].is_number());
    CHECK(out["function_count"].is_number());
    CHECK(out["graph_count"].is_number());
    CHECK(out["macro_count"].is_number());
    CHECK(out["interface_count"].is_number());
    // Sanity: counts must agree with what list_variables / read_blueprint say.
    auto vars = f.Call("list_variables", json{{"asset_path", "/Game/AI/BP_Enemy"}});
    CHECK(out["variable_count"] == static_cast<int>(vars.size()));
}

TEST_CASE("summarize_blueprint payload is small (~few hundred bytes)") {
    Fixture f;
    auto out = f.Call("summarize_blueprint", json{{"asset_path", "/Game/AI/BP_Enemy"}});
    auto full = f.Call("read_blueprint", json{{"asset_path", "/Game/AI/BP_Enemy"}});
    CHECK(out.dump().size() < full.dump().size() / 2);
}

TEST_CASE("read_blueprint honors fields projection") {
    Fixture f;
    auto out = f.Call("read_blueprint", json{
        {"asset_path", "/Game/AI/BP_Enemy"},
        {"fields", json::array({"parent_class"})}});
    CHECK(out.size() == 1);
    CHECK(out["parent_class"] == "ACharacter");
    CHECK_FALSE(out.contains("variables"));
}

TEST_CASE("read_blueprint with array projection on variables[].name") {
    Fixture f;
    auto out = f.Call("read_blueprint", json{
        {"asset_path", "/Game/AI/BP_Enemy"},
        {"fields", json::array({"variables[].name"})}});
    CHECK(out.size() == 1);
    REQUIRE(out["variables"].is_array());
    for (auto& v : out["variables"]) {
        CHECK(v.size() == 1);
        CHECK(v.contains("name"));
    }
}

TEST_CASE("list_blueprints honors limit/offset pagination") {
    Fixture f;
    auto all = f.Call("list_blueprints", json{{"path","/Game"}});
    REQUIRE(all.is_array());
    REQUIRE(all.size() >= 2);

    auto first = f.Call("list_blueprints", json{{"path","/Game"},{"limit",1}});
    REQUIRE(first.is_array());
    CHECK(first.size() == 1);
    CHECK(first[0]["asset_path"] == all[0]["asset_path"]);

    auto second = f.Call("list_blueprints", json{{"path","/Game"},{"limit",1},{"offset",1}});
    REQUIRE(second.is_array());
    CHECK(second.size() == 1);
    CHECK(second[0]["asset_path"] == all[1]["asset_path"]);
}

TEST_CASE("list_blueprints with fields returns just the requested keys per element") {
    Fixture f;
    auto out = f.Call("list_blueprints", json{
        {"path","/Game"},
        {"fields", json::array({"asset_path"})}});
    REQUIRE(out.is_array());
    CHECK(out.size() >= 1);
    for (auto& el : out) {
        CHECK(el.size() == 1);
        CHECK(el.contains("asset_path"));
    }
}

TEST_CASE("list_variables honors limit/offset and fields together") {
    Fixture f;
    auto all = f.Call("list_variables", json{{"asset_path","/Game/AI/BP_Enemy"}});
    REQUIRE(all.is_array());

    auto sliced = f.Call("list_variables", json{
        {"asset_path","/Game/AI/BP_Enemy"},
        {"limit", 2},
        {"fields", json::array({"name"})}});
    REQUIRE(sliced.is_array());
    CHECK(sliced.size() == std::min<std::size_t>(2, all.size()));
    for (auto& v : sliced) {
        CHECK(v.size() == 1);
        CHECK(v.contains("name"));
    }
}

TEST_CASE("Negative limit/offset throw") {
    Fixture f;
    CHECK_THROWS_AS(f.Call("list_blueprints",
                           json{{"path","/Game"},{"offset",-1}}),
                    std::invalid_argument);
    // limit < -1 disallowed; -1 is the sentinel (no cap) so it must be allowed.
    CHECK_THROWS_AS(f.Call("list_blueprints",
                           json{{"path","/Game"},{"limit",-2}}),
                    std::invalid_argument);
    CHECK_NOTHROW(f.Call("list_blueprints",
                         json{{"path","/Game"},{"limit",-1}}));
}

TEST_CASE("offset past end yields empty array, not error") {
    Fixture f;
    auto out = f.Call("list_blueprints", json{
        {"path","/Game"}, {"offset", 9999}});
    REQUIRE(out.is_array());
    CHECK(out.empty());
}

TEST_CASE("fields with non-string element throws invalid_argument") {
    Fixture f;
    CHECK_THROWS_AS(f.Call("read_blueprint", json{
        {"asset_path","/Game/AI/BP_Enemy"},
        {"fields", json::array({"name", 42})}}),
        std::invalid_argument);
}
