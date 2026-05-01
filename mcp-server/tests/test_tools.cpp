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

TEST_CASE("ToolRegistry exposes 6 tools with input schemas") {
    Fixture f;
    auto spec = f.registry.ListSpec();
    CHECK(spec.size() == 6);
    for (const auto& t : spec) {
        CHECK(t["inputSchema"]["type"] == "object");
    }
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
