// Direct tests against the MockBlueprintReader.

#include <doctest/doctest.h>

#include "backends/MockBlueprintReader.h"
#include "test_helpers.h"

using namespace bpr::backends;

TEST_CASE("MockBlueprintReader loads all 4 fixtures") {
    auto reader = bpr::test::MakeMockReader();
    CHECK(reader.FixtureCount() == 4);
}

TEST_CASE("ListBlueprints filters by path prefix") {
    auto reader = bpr::test::MakeMockReader();

    auto all = reader.ListBlueprints("/Game");
    CHECK(all.size() == 4);

    auto ai = reader.ListBlueprints("/Game/AI");
    REQUIRE(ai.size() == 1);
    CHECK(ai[0].AssetPath == "/Game/AI/BP_Enemy");

    auto items = reader.ListBlueprints("/Game/Items");
    REQUIRE(items.size() == 1);
    CHECK(items[0].Name == "BP_Pickup");
}

TEST_CASE("ReadBlueprint returns full metadata") {
    auto reader = bpr::test::MakeMockReader();
    auto md = reader.ReadBlueprint("/Game/AI/BP_Enemy");
    CHECK(md.ParentClass == "ACharacter");
    CHECK(md.Variables.size() == 3);
    CHECK(md.Functions.size() == 2);
    CHECK(md.Graphs.size() == 2);
}

TEST_CASE("ReadBlueprint throws AssetNotFound for missing path") {
    auto reader = bpr::test::MakeMockReader();
    CHECK_THROWS_AS(reader.ReadBlueprint("/Game/Nope"), AssetNotFound);
}

TEST_CASE("GetGraph returns the requested graph") {
    auto reader = bpr::test::MakeMockReader();
    auto g = reader.GetGraph("/Game/AI/BP_Enemy", "EventGraph");
    CHECK(g.Name == "EventGraph");
    CHECK(g.Type == "EventGraph");
    CHECK(g.Nodes.size() >= 6);
    CHECK(!g.Connections.empty());
}

TEST_CASE("GetGraph throws on unknown graph name") {
    auto reader = bpr::test::MakeMockReader();
    CHECK_THROWS_AS(reader.GetGraph("/Game/AI/BP_Enemy", "NoSuchGraph"),
                    BlueprintReaderError);
}

TEST_CASE("GetFunction returns the function with locals + signature") {
    auto reader = bpr::test::MakeMockReader();
    auto fn = reader.GetFunction("/Game/Player/BP_PlayerController", "AddScore");
    CHECK(fn.Name == "AddScore");
    CHECK(fn.Inputs.size() == 1);
    CHECK(fn.Outputs.size() == 1);
    CHECK(fn.Locals.size() == 2);
    CHECK(fn.Graph.Type == "Function");
}

TEST_CASE("GetComponents returns SCS hierarchy from fixture") {
    auto reader = bpr::test::MakeMockReader();
    auto comps = reader.GetComponents("/Game/Items/BP_Pickup");
    REQUIRE(comps.size() == 3);
    bool sawRoot = false, sawMesh = false, sawHalo = false;
    for (const auto& c : comps) {
        if (c.Name == "DefaultSceneRoot") { sawRoot = true; CHECK(c.IsRoot); }
        if (c.Name == "PickupMesh") {
            sawMesh = true;
            REQUIRE(c.Parent.has_value());
            CHECK(*c.Parent == "DefaultSceneRoot");
        }
        if (c.Name == "Halo") {
            sawHalo = true;
            REQUIRE(c.Parent.has_value());
            CHECK(*c.Parent == "PickupMesh");
        }
    }
    CHECK(sawRoot); CHECK(sawMesh); CHECK(sawHalo);
}

TEST_CASE("GetComponents returns empty array for fixtures with no components") {
    auto reader = bpr::test::MakeMockReader();
    auto comps = reader.GetComponents("/Game/AI/BP_Enemy");
    CHECK(comps.empty());
}

TEST_CASE("ListVariables matches metadata.Variables") {
    auto reader = bpr::test::MakeMockReader();
    auto vars = reader.ListVariables("/Game/AI/BP_Enemy");
    CHECK(vars.size() == 3);
    bool sawReplicated = false;
    for (const auto& v : vars) {
        if (v.Name == "Health") {
            CHECK(v.IsReplicated);
            sawReplicated = true;
        }
    }
    CHECK(sawReplicated);
}

TEST_CASE("FindNode searches both event graph and function graphs") {
    auto reader = bpr::test::MakeMockReader();
    auto branches = reader.FindNode("/Game/AI/BP_Enemy", "Branch");
    CHECK(branches.size() == 1);
    CHECK(branches[0].Class == "K2Node_IfThenElse");

    auto callFns = reader.FindNode("/Game/AI/BP_Enemy", "K2Node_CallFunction");
    CHECK(callFns.size() >= 2);

    auto print = reader.FindNode("/Game/AI/BP_Enemy", "print string");
    CHECK(print.size() == 1);
    CHECK(print[0].Title == "Print String");
}

TEST_CASE("FindNode tags each hit with graph_name + graph_type (issue #6)") {
    // Each find_node hit must carry the graph it came from. get_node,
    // delete_node, and wire_pins all require -Graph= — the agent had
    // no other way to find that out from a find_node result.
    auto reader = bpr::test::MakeMockReader();
    auto hits = reader.FindNode("/Game/AI/BP_Enemy", "");
    REQUIRE(!hits.empty());
    for (const auto& n : hits) {
        CHECK(n.GraphName.has_value());
        CHECK_FALSE(n.GraphName->empty());
        CHECK(n.GraphType.has_value());
        CHECK_FALSE(n.GraphType->empty());
    }
    // The graph_name field is wire-emitted only when populated — a
    // BPNode coming back from get_node (where the caller already knows
    // the graph) must serialize without the key, so existing wire
    // consumers stay unaffected.
    BPNode noGraph;
    noGraph.Id = "abc";
    noGraph.Class = "K2Node_X";
    noGraph.Title = "X";
    nlohmann::json j = noGraph;
    CHECK_FALSE(j.contains("graph_name"));
    CHECK_FALSE(j.contains("graph_type"));
}

TEST_CASE("FindNode query also matches meta.event_name (issue #12 extension)") {
    // K2Node_Event titles are rendered with an "Event " prefix and
    // sometimes humanized (e.g. "Event BeginPlay" for ReceiveBeginPlay).
    // An agent searching for the underlying function name would miss
    // the hit without the eventName fallback.
    auto reader = bpr::test::MakeMockReader();
    // BP_Enemy fixture has a K2Node_Event with title "Event BeginPlay"
    // and meta.event_name "ReceiveBeginPlay".
    auto hits = reader.FindNode("/Game/AI/BP_Enemy", "ReceiveBegin");
    REQUIRE(hits.size() >= 1);
    bool sawIt = false;
    for (const auto& n : hits) {
        if (n.Class == "K2Node_Event") { sawIt = true; break; }
    }
    CHECK(sawIt);
}

TEST_CASE("FindNode query also matches meta.function_name (issue #12)") {
    // Operator nodes (Greater_IntInt, Less_FloatFloat, EqualEqual_*)
    // render their title as the operator alias ("integer > integer",
    // "Greater (float)") so a query for the function name they were
    // spawned with — "Greater_FloatFloat" — wouldn't find them
    // without the meta.function_name / meta.targetFunction fallback.
    auto reader = bpr::test::MakeMockReader();
    // BP_Enemy fixture has a node with title "Greater (float)" and
    // meta.function_name "Greater_FloatFloat".
    auto byFnName = reader.FindNode("/Game/AI/BP_Enemy", "Greater_Float");
    REQUIRE(byFnName.size() >= 1);
    // The hit must be the right node — its meta carries the function.
    bool sawIt = false;
    for (const auto& n : byFnName) {
        if (n.Meta.is_object()) {
            auto it = n.Meta.find("function_name");
            if (it != n.Meta.end() && it->is_string() &&
                it->get<std::string>() == "Greater_FloatFloat") {
                sawIt = true; break;
            }
        }
    }
    CHECK(sawIt);
    // Title-only match still works (query that only appears in title).
    auto byTitle = reader.FindNode("/Game/AI/BP_Enemy", "(float)");
    CHECK(byTitle.size() >= 1);
}

TEST_CASE("FindNode kind filter narrows results") {
    auto reader = bpr::test::MakeMockReader();
    // Empty query + kind-only should still match.
    auto allCalls = reader.FindNode("/Game/AI/BP_Enemy", "", "CallFunction");
    // BP_Enemy fixture has function_name in meta but its mock fixtures don't
    // have a "kind" key, so the default-shaped fixtures return zero matches —
    // that's the contract: kind filter is exact match against meta["kind"].
    CHECK(allCalls.empty());

    // Combined: query + kind, neither matches => empty.
    auto none = reader.FindNode("/Game/AI/BP_Enemy", "ThisDoesNotExist", "Event");
    CHECK(none.empty());

    // Query-only path is unchanged from prior behavior.
    auto print = reader.FindNode("/Game/AI/BP_Enemy", "print string", "");
    CHECK(print.size() == 1);
}
