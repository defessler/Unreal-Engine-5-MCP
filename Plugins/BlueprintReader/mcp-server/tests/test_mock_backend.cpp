// Direct tests against the MockBlueprintReader.

#include <doctest/doctest.h>

#include "backends/MockBlueprintReader.h"
#include "test_helpers.h"

using namespace bpr::backends;

TEST_CASE("MockBlueprintReader loads all 3 fixtures") {
    MockBlueprintReader reader(bpr::test::FixturesDir());
    CHECK(reader.FixtureCount() == 3);
}

TEST_CASE("ListBlueprints filters by path prefix") {
    MockBlueprintReader reader(bpr::test::FixturesDir());

    auto all = reader.ListBlueprints("/Game");
    CHECK(all.size() == 3);

    auto ai = reader.ListBlueprints("/Game/AI");
    REQUIRE(ai.size() == 1);
    CHECK(ai[0].AssetPath == "/Game/AI/BP_Enemy");

    auto items = reader.ListBlueprints("/Game/Items");
    REQUIRE(items.size() == 1);
    CHECK(items[0].Name == "BP_Pickup");
}

TEST_CASE("ReadBlueprint returns full metadata") {
    MockBlueprintReader reader(bpr::test::FixturesDir());
    auto md = reader.ReadBlueprint("/Game/AI/BP_Enemy");
    CHECK(md.ParentClass == "ACharacter");
    CHECK(md.Variables.size() == 3);
    CHECK(md.Functions.size() == 2);
    CHECK(md.Graphs.size() == 2);
}

TEST_CASE("ReadBlueprint throws AssetNotFound for missing path") {
    MockBlueprintReader reader(bpr::test::FixturesDir());
    CHECK_THROWS_AS(reader.ReadBlueprint("/Game/Nope"), AssetNotFound);
}

TEST_CASE("GetGraph returns the requested graph") {
    MockBlueprintReader reader(bpr::test::FixturesDir());
    auto g = reader.GetGraph("/Game/AI/BP_Enemy", "EventGraph");
    CHECK(g.Name == "EventGraph");
    CHECK(g.Type == "EventGraph");
    CHECK(g.Nodes.size() >= 6);
    CHECK(!g.Connections.empty());
}

TEST_CASE("GetGraph throws on unknown graph name") {
    MockBlueprintReader reader(bpr::test::FixturesDir());
    CHECK_THROWS_AS(reader.GetGraph("/Game/AI/BP_Enemy", "NoSuchGraph"),
                    BlueprintReaderError);
}

TEST_CASE("GetFunction returns the function with locals + signature") {
    MockBlueprintReader reader(bpr::test::FixturesDir());
    auto fn = reader.GetFunction("/Game/Player/BP_PlayerController", "AddScore");
    CHECK(fn.Name == "AddScore");
    CHECK(fn.Inputs.size() == 1);
    CHECK(fn.Outputs.size() == 1);
    CHECK(fn.Locals.size() == 2);
    CHECK(fn.Graph.Type == "Function");
}

TEST_CASE("GetComponents returns SCS hierarchy from fixture") {
    MockBlueprintReader reader(bpr::test::FixturesDir());
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
    MockBlueprintReader reader(bpr::test::FixturesDir());
    auto comps = reader.GetComponents("/Game/AI/BP_Enemy");
    CHECK(comps.empty());
}

TEST_CASE("ListVariables matches metadata.Variables") {
    MockBlueprintReader reader(bpr::test::FixturesDir());
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
    MockBlueprintReader reader(bpr::test::FixturesDir());
    auto branches = reader.FindNode("/Game/AI/BP_Enemy", "Branch");
    CHECK(branches.size() == 1);
    CHECK(branches[0].Class == "K2Node_IfThenElse");

    auto callFns = reader.FindNode("/Game/AI/BP_Enemy", "K2Node_CallFunction");
    CHECK(callFns.size() >= 2);

    auto print = reader.FindNode("/Game/AI/BP_Enemy", "print string");
    CHECK(print.size() == 1);
    CHECK(print[0].Title == "Print String");
}

TEST_CASE("FindNode kind filter narrows results") {
    MockBlueprintReader reader(bpr::test::FixturesDir());
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
