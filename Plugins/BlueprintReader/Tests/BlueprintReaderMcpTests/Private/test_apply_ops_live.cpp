// Live: real-editor coverage for the write+save path that apply_ops drives.
// The mock test_apply_ops.cpp verifies dispatch/slot-resolution/idempotency
// against a fake backend; this proves a mutation actually compiles, saves, and
// reads back from a REAL editor — the gap the mock can't close.
//
// To stay safe it works on a DUPLICATE of the seeded BP, never the committed
// asset: duplicate -> add a variable -> read it back -> delete the copy. So a
// failure mid-run can at worst leave a stray /Game/AI/BP_TestEnemy_LiveProbe,
// not a dirtied BP_TestEnemy. Each step is a single -Op that works over the
// daemon or the one-shot fallback (no multi-op batch state required).
//
// Gated on BP_READER_ENGINE_DIR + BP_READER_PROJECT; auto-skips otherwise.

#include <doctest/doctest.h>

#include "backends/CommandletBlueprintReader.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace {

std::string AolEnv(const char* key) {
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

bool AolLiveEnabled() {
	return !AolEnv("BP_READER_ENGINE_DIR").empty() &&
	       !AolEnv("BP_READER_PROJECT").empty();
}

}    // namespace

TEST_CASE("[live] a write op compiles, saves, and reads back from a real editor"
          * doctest::skip(!AolLiveEnabled())) {
	using namespace bpr::backends;
	CommandletBlueprintReader::Config cfg;
	cfg.engineDir = std::filesystem::path(AolEnv("BP_READER_ENGINE_DIR"));
	cfg.uproject  = std::filesystem::path(AolEnv("BP_READER_PROJECT"));
	cfg.timeout   = std::chrono::seconds(780);    // Lyra cold boot is multi-minute
	cfg.useDaemon = true;                          // falls back to one-shot if needed
	CommandletBlueprintReader reader(std::move(cfg));

	const std::string copy = "/Game/AI/BP_TestEnemy_LiveProbe";
	const std::string var  = "BPRLiveProbeVar";

	// Start clean even if a prior run was interrupted.
	reader.DeleteAsset(copy, /*force=*/true);

	const auto dup = reader.DuplicateBlueprint("/Game/AI/BP_TestEnemy", copy);
	REQUIRE_FALSE(dup.alreadyExisted);

	BPPinType boolType;
	boolType.Category = "bool";
	reader.AddVariable(copy, var, boolType, "", "", /*replicated=*/false, /*editable=*/true);

	// Read back from disk: proves CompileAndSaveBlueprint actually persisted it.
	const auto vars = reader.ListVariables(copy);
	const bool present = std::any_of(vars.begin(), vars.end(),
		[&](const BPVariable& v) { return v.Name == var; });
	CHECK(present);

	// Leave the project as found.
	const auto del = reader.DeleteAsset(copy, /*force=*/true);
	CHECK(del.deleted);
}
