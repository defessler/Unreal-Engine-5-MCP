// Live: read_actor_instance (#208) loads an arbitrary package and finds a
// UObject inside it via ForEachObjectWithPackage. This is the only live
// coverage for the tool, and it guards the engine-API deprecation migration
// (the ForEachObjectWithPackage `bIncludeNestedObjects` bool overload ->
// EGetObjectsFlags) against silent breakage.
//
// Gated on BP_READER_ENGINE_DIR + BP_READER_PROJECT; auto-skips otherwise
// (it drives a real -nullrhi editor/daemon). Targets the seeded test BP
// Content/AI/BP_TestEnemy (regenerable via -run=BPRSeed).

#include <doctest/doctest.h>

#include "backends/CommandletBlueprintReader.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

namespace {

std::string RaiEnv(const char* key) {
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
	if (const char* v = std::getenv(key); v != nullptr && *v != '\0') {
		return std::string(v);
	}
	return {};
#endif
}

bool RaiLiveEnabled() {
	return !RaiEnv("BP_READER_ENGINE_DIR").empty() &&
	       !RaiEnv("BP_READER_PROJECT").empty();
}

}    // namespace

TEST_CASE("[live] read_actor_instance finds a UObject in a real package"
          * doctest::skip(!RaiLiveEnabled())) {
	bpr::backends::CommandletBlueprintReader::Config cfg;
	cfg.engineDir = std::filesystem::path(RaiEnv("BP_READER_ENGINE_DIR"));
	cfg.uproject  = std::filesystem::path(RaiEnv("BP_READER_PROJECT"));
	cfg.timeout   = std::chrono::seconds(180);
	cfg.useDaemon = true;
	bpr::backends::CommandletBlueprintReader reader(std::move(cfg));

	const nlohmann::json j = reader.ReadActorInstance("/Game/AI/BP_TestEnemy");

	// LoadPackage + ForEachObjectWithPackage must locate an object and report
	// its class/name — proving the deprecation-migrated iteration still works.
	REQUIRE(j.is_object());
	CHECK(j.value("ok", false));
	CHECK_FALSE(j.value("object_class", std::string{}).empty());
	CHECK_FALSE(j.value("object_name", std::string{}).empty());
}
