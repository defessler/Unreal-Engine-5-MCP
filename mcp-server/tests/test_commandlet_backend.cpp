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

std::unique_ptr<bpr::backends::CommandletBlueprintReader> MakeLiveReader() {
    bpr::backends::CommandletBlueprintReader::Config cfg;
    cfg.engineDir = std::filesystem::path(GetEnv("BP_READER_ENGINE_DIR"));
    cfg.uproject  = std::filesystem::path(GetEnv("BP_READER_PROJECT"));
    cfg.timeout   = std::chrono::seconds(180);
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
