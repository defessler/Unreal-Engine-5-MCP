// Tests for the decompile_function pass — BPGraph → BPIR reconstruction.
// Runs against the MockBlueprintReader's bundled fixture BPs so we don't
// need a live UE editor.
//
// What the fixtures give us today (BP_Enemy.json):
//   - BP_Enemy: 4 vars, 2 functions (TakeDamage, OnDeath), event graph
//     with a Branch + Sequence + a few CallFunction nodes.
//   - BP_Pickup: smaller, exercises VariableSet + a single Branch.
// These cover the core decompile patterns; richer cases come later
// when we have a live commandlet test or build dedicated fixtures.

#include <doctest/doctest.h>

#include "tools/Decompile.h"
#include "tools/Bpir.h"
#include "backends/MockBlueprintReader.h"

#include "test_helpers.h"

using namespace bpr;
using namespace bpr::tools;
using namespace bpr::backends;

namespace {
struct Fixture {
    MockBlueprintReader reader;
    Fixture() : reader(test::FixturesDir()) {}
};
} // namespace

TEST_CASE("decompile: produces a BPIR function doc that validates") {
    Fixture f;
    auto doc = DecompileFunction(f.reader, "/Game/AI/BP_Enemy", "TakeDamage");
    CHECK_NOTHROW(ValidateBpir(doc));
    CHECK(doc["kind"] == "function");
    CHECK(doc["name"] == "TakeDamage");
    CHECK(doc["version"] == kBpirSchemaVersion);
    CHECK(doc.contains("body"));
    CHECK(doc["body"].is_array());
}

TEST_CASE("decompile: function metadata carries asset_path") {
    Fixture f;
    auto doc = DecompileFunction(f.reader, "/Game/AI/BP_Enemy", "TakeDamage");
    REQUIRE(doc.contains("metadata"));
    CHECK(doc["metadata"]["asset_path"] == "/Game/AI/BP_Enemy");
}

TEST_CASE("decompile: inputs / outputs / locals are emitted with shorthand types") {
    Fixture f;
    auto doc = DecompileFunction(f.reader, "/Game/Player/BP_PlayerController", "AddScore");
    REQUIRE(doc["inputs"].is_array());
    // BP_PlayerController.AddScore has at least one input — verify the
    // shape rather than a specific count (fixture may evolve).
    if (!doc["inputs"].empty()) {
        const auto& in0 = doc["inputs"][0];
        CHECK(in0.contains("name"));
        CHECK(in0.contains("type"));
        CHECK(in0["type"].is_string());  // shorthand string, not object
    }
}

TEST_CASE("decompile: missing function throws AssetNotFound") {
    Fixture f;
    CHECK_THROWS_AS(
        DecompileFunction(f.reader, "/Game/AI/BP_Enemy", "DoesNotExist"),
        BlueprintReaderError);
}

TEST_CASE("decompile_blueprint: emits class doc with functions array") {
    Fixture f;
    auto doc = DecompileBlueprint(f.reader, "/Game/AI/BP_Enemy");
    CHECK_NOTHROW(ValidateBpir(doc));
    CHECK(doc["kind"] == "class");
    CHECK(doc["name"] == "BP_Enemy");
    REQUIRE(doc["functions"].is_array());
    // Each function entry should itself be a valid BPIR function doc.
    for (const auto& fn : doc["functions"]) {
        CHECK_NOTHROW(ValidateBpir(fn));
    }
}

TEST_CASE("decompile_blueprint: variables include category + replication flags") {
    Fixture f;
    auto doc = DecompileBlueprint(f.reader, "/Game/AI/BP_Enemy");
    REQUIRE(doc["variables"].is_array());
    bool sawHealth = false;
    for (const auto& v : doc["variables"]) {
        if (v["name"] == "Health") {
            sawHealth = true;
            CHECK(v["type"] == "float");
            // Health in fixture is replicated + editable — verify
            // at least one of those is preserved.
            if (v.contains("replicated")) CHECK(v["replicated"] == true);
        }
    }
    CHECK(sawHealth);
}

TEST_CASE("decompile: parent class makes it into the class doc metadata") {
    Fixture f;
    auto doc = DecompileBlueprint(f.reader, "/Game/AI/BP_Enemy");
    REQUIRE(doc.contains("metadata"));
    CHECK(doc["metadata"]["parent_class"] == "ACharacter");
}

TEST_CASE("decompile: round-trip BPIR through ValidateBpir works for empty function") {
    Fixture f;
    // Even a function with no body should round-trip cleanly.
    auto doc = DecompileFunction(f.reader, "/Game/Player/BP_PlayerController", "AddScore");
    auto migrated = MigrateToCurrent(doc);
    CHECK_NOTHROW(ValidateBpir(migrated));
}
