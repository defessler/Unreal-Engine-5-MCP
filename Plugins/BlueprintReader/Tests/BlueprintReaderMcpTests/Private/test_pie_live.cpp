// Live: pie_start must report HONESTLY in a headless (-nullrhi) daemon —
// started=false plus an explanatory note — rather than the misleading
// started=true it used to return. RequestPlaySession silently queues a play
// session that can never sustain without a rendering viewport, so callers
// would otherwise see started=true yet pie_running=false forever. The fix
// gates on FApp::CanEverRender() in RunPieStartOp.
//
// Gated on BP_READER_ENGINE_DIR + BP_READER_PROJECT; auto-skips otherwise
// (it spawns a real -nullrhi editor/daemon and issues one PIE call).

#include <doctest/doctest.h>

#include "backends/CommandletBlueprintReader.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace {

std::string PieEnv(const char* key) {
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

bool PieLiveEnabled() {
	return !PieEnv("BP_READER_ENGINE_DIR").empty() &&
	       !PieEnv("BP_READER_PROJECT").empty();
}

}    // namespace

TEST_CASE("[live] pie_start reports honestly in a headless (-nullrhi) daemon"
          * doctest::skip(!PieLiveEnabled())) {
	bpr::backends::CommandletBlueprintReader::Config cfg;
	cfg.engineDir = std::filesystem::path(PieEnv("BP_READER_ENGINE_DIR"));
	cfg.uproject  = std::filesystem::path(PieEnv("BP_READER_PROJECT"));
	cfg.timeout   = std::chrono::seconds(180);
	cfg.useDaemon = true;
	bpr::backends::CommandletBlueprintReader reader(std::move(cfg));

	const auto r = reader.PieStart("selected_viewport");

	// The daemon runs -nullrhi → FApp::CanEverRender() is false → PIE can't
	// start. The tool must say so, not claim success.
	CHECK_FALSE(r.started);
	CHECK_FALSE(r.note.empty());
	CHECK(r.mode == "selected_viewport");
}
