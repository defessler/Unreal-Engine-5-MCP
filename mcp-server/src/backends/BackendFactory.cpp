#include "backends/BackendFactory.h"
#include "backends/MockBlueprintReader.h"

#include <cstdlib>

#include <fmt/core.h>

namespace bpr::backends {

namespace {

std::string EnvOrDefault(const char* key, std::string fallback) {
#ifdef _MSC_VER
    char* buf = nullptr;
    std::size_t len = 0;
    if (_dupenv_s(&buf, &len, key) == 0 && buf != nullptr) {
        std::string out(buf);
        std::free(buf);
        return out;
    }
    return fallback;
#else
    if (const char* v = std::getenv(key); v != nullptr && *v != '\0') {
        return std::string(v);
    }
    return fallback;
#endif
}

} // namespace

BackendConfig ConfigFromEnv(const std::filesystem::path& executableDir) {
    BackendConfig cfg;
    cfg.backend = EnvOrDefault("BP_READER_BACKEND", "mock");
    auto fix = EnvOrDefault("BP_READER_FIXTURES_DIR", "");
    if (fix.empty()) {
        cfg.fixturesDir = executableDir / "fixtures";
    } else {
        cfg.fixturesDir = std::filesystem::path(fix);
    }
    return cfg;
}

std::unique_ptr<IBlueprintReader> Create(const BackendConfig& cfg) {
    if (cfg.backend == "mock") {
        return std::make_unique<MockBlueprintReader>(cfg.fixturesDir);
    }
    if (cfg.backend == "commandlet" || cfg.backend == "live") {
        throw BlueprintReaderError(fmt::format(
            "backend '{}' is not implemented in Phase 0 (mock-only). "
            "Set BP_READER_BACKEND=mock for now.", cfg.backend));
    }
    throw BlueprintReaderError(fmt::format(
        "unknown backend '{}': expected one of mock|commandlet|live", cfg.backend));
}

} // namespace bpr::backends
