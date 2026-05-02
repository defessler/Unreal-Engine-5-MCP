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

TEST_CASE("ToolRegistry exposes 20 tools (6 read + 12 write + 2 meta) with input schemas") {
    Fixture f;
    auto spec = f.registry.ListSpec();
    CHECK(spec.size() == 20);
    for (const auto& t : spec) {
        CHECK(t["inputSchema"]["type"] == "object");
    }
}

TEST_CASE("Discoverability: list_node_kinds returns the dispatch table") {
    Fixture f;
    auto out = f.Call("list_node_kinds", json::object());
    REQUIRE(out.is_array());
    CHECK(out.size() == 6);
    std::vector<std::string> kinds;
    for (auto& k : out) kinds.push_back(k["kind"].get<std::string>());
    auto has = [&](const std::string& s) {
        return std::find(kinds.begin(), kinds.end(), s) != kinds.end();
    };
    for (const char* k : {"Branch","Sequence","VariableGet","VariableSet","CallFunction","CustomEvent"}) {
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
