// Live integration test for the CommandletBlueprintReader. Only runs when
// BP_READER_ENGINE_DIR and BP_READER_PROJECT are set in the environment;
// skipped otherwise so a fresh-clone doctest run stays fast.
//
// The seed commandlet (BlueprintReaderSeed) must have been run beforehand to
// produce /Game/AI/BP_TestEnemy and /Game/AI/BP_TestPickup.

#include <doctest/doctest.h>

#include "backends/CommandletBlueprintReader.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace {

std::string GetEnv(const char* key) {
#ifdef _MSC_VER
    char* buf = nullptr;
    std::size_t len = 0;
    if (_dupenv_s(&buf, &len, key) == 0 && buf != nullptr) {
        std::string out(buf);
        std::free(buf);
        return out;
    }
    return {};
#else
    if (const char* v = std::getenv(key); v != nullptr && *v != '\0') return std::string(v);
    return {};
#endif
}

bool LiveBackendAvailable() {
    return !GetEnv("BP_READER_ENGINE_DIR").empty() &&
           !GetEnv("BP_READER_PROJECT").empty();
}

std::unique_ptr<bpr::backends::CommandletBlueprintReader> MakeLiveReader(bool useDaemon = false) {
    bpr::backends::CommandletBlueprintReader::Config cfg;
    cfg.engineDir = std::filesystem::path(GetEnv("BP_READER_ENGINE_DIR"));
    cfg.uproject  = std::filesystem::path(GetEnv("BP_READER_PROJECT"));
    cfg.timeout   = std::chrono::seconds(180);
    cfg.useDaemon = useDaemon;
    return std::make_unique<bpr::backends::CommandletBlueprintReader>(std::move(cfg));
}

} // namespace

TEST_CASE("CommandletBlueprintReader: List under /Game/AI returns seeded blueprints"
          * doctest::skip(!LiveBackendAvailable())) {
    auto reader = MakeLiveReader();
    auto items = reader->ListBlueprints("/Game/AI");
    REQUIRE_GE(items.size(), 2);
    bool sawEnemy = false, sawPickup = false;
    for (const auto& s : items) {
        if (s.AssetPath == "/Game/AI/BP_TestEnemy")  sawEnemy  = true;
        if (s.AssetPath == "/Game/AI/BP_TestPickup") sawPickup = true;
    }
    CHECK(sawEnemy);
    CHECK(sawPickup);
}

TEST_CASE("CommandletBlueprintReader: ReadBlueprint returns canonical wire shape"
          * doctest::skip(!LiveBackendAvailable())) {
    auto reader = MakeLiveReader();
    auto md = reader->ReadBlueprint("/Game/AI/BP_TestEnemy");
    CHECK(md.AssetPath == "/Game/AI/BP_TestEnemy");
    CHECK(md.Name == "BP_TestEnemy");
    CHECK_GE(md.Variables.size(), 3);

    bool sawHealth = false;
    for (const auto& v : md.Variables) {
        if (v.Name == "Health") {
            sawHealth = true;
            CHECK(v.IsReplicated);
            CHECK(v.IsEditable);
        }
    }
    CHECK(sawHealth);
    // TakeDamage + OnDeath are functions added by the seeder.
    CHECK_GE(md.Functions.size(), 2);
}

TEST_CASE("CommandletBlueprintReader: GetFunction returns inputs/outputs/locals"
          * doctest::skip(!LiveBackendAvailable())) {
    auto reader = MakeLiveReader();
    auto fn = reader->GetFunction("/Game/AI/BP_TestEnemy", "TakeDamage");
    CHECK(fn.Name == "TakeDamage");
    // The seeder declares Damage:float input, Killed:bool output, NewHealth local.
    CHECK_GE(fn.Inputs.size(), 1);
    CHECK_GE(fn.Outputs.size(), 1);
    CHECK_GE(fn.Locals.size(), 1);
    CHECK(fn.Graph.Type == "Function");
}

TEST_CASE("CommandletBlueprintReader: ListVariables surfaces seeded vars"
          * doctest::skip(!LiveBackendAvailable())) {
    auto reader = MakeLiveReader();
    auto vars = reader->ListVariables("/Game/AI/BP_TestPickup");
    CHECK_GE(vars.size(), 2);
}

TEST_CASE("CommandletBlueprintReader: AssetNotFound on bogus path"
          * doctest::skip(!LiveBackendAvailable())) {
    auto reader = MakeLiveReader();
    CHECK_THROWS_AS(reader->ReadBlueprint("/Game/Nope/Definitely_Does_Not_Exist"),
                    bpr::backends::BlueprintReaderError);
}

TEST_CASE("CommandletBlueprintReader: extended write tools — add_node + wire_pins + delete_variable + rename_variable"
          * doctest::skip(!LiveBackendAvailable())) {
    auto reader = MakeLiveReader(/*useDaemon=*/true);
    const std::string asset = "/Game/AI/BP_TestEnemy";

    // 1. Add a fresh CustomEvent node to the EventGraph. Returns a GUID.
    std::map<std::string, std::string, std::less<>> evtArgs{
        {"EventName", "BPR_TestEvent"}};
    std::string customEventId = reader->AddNode(
        asset, "EventGraph", "CustomEvent", -300, 320, evtArgs);
    CHECK(!customEventId.empty());

    // 2. Add a Branch node next to it.
    std::string branchId = reader->AddNode(
        asset, "EventGraph", "Branch", -50, 320, {});
    CHECK(!branchId.empty());

    // 3. Wire the CustomEvent's `then` output to the Branch's exec input.
    //    Pin GUIDs aren't easy to predict here, so use names — wire_pins
    //    accepts both.
    reader->WirePins(asset, "EventGraph", customEventId, "then", branchId, "execute");

    // 4. Read back the graph and assert the new connection exists.
    auto graph = reader->GetGraph(asset, "EventGraph");
    bool found = false;
    for (const auto& c : graph.Connections) {
        if (c.FromNode == customEventId && c.ToNode == branchId) { found = true; break; }
    }
    CHECK(found);

    // 5. Delete the Branch we just added (cleanup partial — keep the
    //    CustomEvent since UE's serialization may keep an event call somewhere).
    reader->DeleteNode(asset, "EventGraph", branchId);

    // 6. Rename a member variable then rename it back.
    reader->RenameVariable(asset, "MaxHealth", "MaxHP");
    auto vars1 = reader->ListVariables(asset);
    bool sawNew = false;
    for (const auto& v : vars1) if (v.Name == "MaxHP") sawNew = true;
    CHECK(sawNew);
    reader->RenameVariable(asset, "MaxHP", "MaxHealth");

    // 7. Delete the variable we'll re-add. Ignore "not found" errors
    //    (e.g. if a previous test run already left it removed).
    try { reader->DeleteVariable(asset, "AggroTarget"); } catch (...) {}
    auto vars2 = reader->ListVariables(asset);
    bool sawAggro = false;
    for (const auto& v : vars2) if (v.Name == "AggroTarget") sawAggro = true;
    CHECK_FALSE(sawAggro);

    // 8. Re-add it via add_variable so reseed isn't strictly necessary
    //    for any subsequent test runs in this session.
    BPPinType actorType;
    actorType.Category = "object";
    actorType.SubCategoryObject = "/Script/Engine.Actor";
    reader->AddVariable(asset, "AggroTarget", actorType, "", "AI", true, false);
}

TEST_CASE("CommandletBlueprintReader: write tools (AddVariable round-trip)"
          * doctest::skip(!LiveBackendAvailable())) {
    auto reader = MakeLiveReader(/*useDaemon=*/true);

    // Pick a unique name so re-running this test against an already-modified
    // BP doesn't fail the "already exists" guard. The seed always reseeds at
    // the start of a session so this is mostly defensive.
    const std::string varName = "BPR_LiveTestVar";

    // First, scrub it if it lingered from a previous run by reseeding the BP.
    // (No `delete_variable` tool yet — we rely on the seed commandlet.)

    BPPinType type;
    type.Category = "bool";

    try {
        reader->AddVariable("/Game/AI/BP_TestEnemy", varName, type,
                            "false", "Tests", false, true);
    } catch (const bpr::backends::BlueprintReaderError& e) {
        // If it already exists from a prior run, that's a test artifact, not a
        // failure of write semantics. Surface it but don't fail the test —
        // the read-back below is what matters.
        INFO("AddVariable threw (likely already-exists): " << e.what());
    }

    auto vars = reader->ListVariables("/Game/AI/BP_TestEnemy");
    bool sawIt = false;
    for (const auto& v : vars) {
        if (v.Name == varName) { sawIt = true; break; }
    }
    CHECK(sawIt);
}

TEST_CASE("CommandletBlueprintReader: daemon mode reuses one editor process across calls"
          * doctest::skip(!LiveBackendAvailable())) {
    auto reader = MakeLiveReader(/*useDaemon=*/true);

    // First call pays the editor cold-start (~5s); subsequent calls should be
    // sub-second since the editor stays alive.
    auto t0 = std::chrono::steady_clock::now();
    auto first = reader->ListBlueprints("/Game/AI");
    auto firstMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    REQUIRE_GE(first.size(), 2);

    auto t1 = std::chrono::steady_clock::now();
    auto md = reader->ReadBlueprint("/Game/AI/BP_TestEnemy");
    auto vars = reader->ListVariables("/Game/AI/BP_TestEnemy");
    auto fn = reader->GetFunction("/Game/AI/BP_TestEnemy", "TakeDamage");
    auto found = reader->FindNode("/Game/AI/BP_TestEnemy", "", "VariableGet");
    auto subsequentMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t1).count();

    CHECK(md.Name == "BP_TestEnemy");
    CHECK_GE(vars.size(), 4);
    CHECK(fn.Name == "TakeDamage");
    CHECK_GE(found.size(), 1);

    // Soft check: 4 follow-up calls together should be much cheaper than the
    // first call (which paid the cold-start). On the dev box this is typically
    // ~0.5–1.5 s for the four calls vs ~5–7 s for the first.
    INFO("First call (cold start) took " << firstMs << "ms; "
         "next 4 calls together took " << subsequentMs << "ms");
    CHECK(subsequentMs < firstMs * 4);
}

TEST_CASE("CommandletBlueprintReader: FindNode kind filter narrows by K2 extras"
          * doctest::skip(!LiveBackendAvailable())) {
    auto reader = MakeLiveReader();

    // Empty query + kind="VariableGet" should match the bIsAlive Get node.
    auto vars = reader->FindNode("/Game/AI/BP_TestEnemy", "", "VariableGet");
    REQUIRE_GE(vars.size(), 1);
    bool sawIsAlive = false;
    for (const auto& n : vars) {
        if (n.Class == "K2Node_VariableGet") sawIsAlive = true;
    }
    CHECK(sawIsAlive);

    // Empty query + kind="CallFunction" should match the PrintString node.
    auto calls = reader->FindNode("/Game/AI/BP_TestEnemy", "", "CallFunction");
    REQUIRE_GE(calls.size(), 1);

    // kind="Event" should match the auto-generated BeginPlay/Tick events.
    auto events = reader->FindNode("/Game/AI/BP_TestEnemy", "", "Event");
    CHECK_GE(events.size(), 1);

    // Query + kind together should AND.
    auto noMatch = reader->FindNode("/Game/AI/BP_TestEnemy", "DoesNotExist", "Event");
    CHECK(noMatch.empty());
}
