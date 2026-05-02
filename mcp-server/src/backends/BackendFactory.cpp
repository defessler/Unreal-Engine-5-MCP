#include "backends/BackendFactory.h"
#include "backends/CommandletBlueprintReader.h"
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
        return out.empty() ? fallback : out;
    }
    return fallback;
#else
    if (const char* v = std::getenv(key); v != nullptr && *v != '\0') {
        return std::string(v);
    }
    return fallback;
#endif
}

int IntFromEnvOrDefault(const char* key, int fallback) {
    auto s = EnvOrDefault(key, "");
    if (s.empty()) return fallback;
    try {
        return std::stoi(s);
    } catch (...) {
        return fallback;
    }
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

    auto engineDir = EnvOrDefault("BP_READER_ENGINE_DIR", "");
    if (!engineDir.empty()) {
        cfg.engineDir = std::filesystem::path(engineDir);
    }
    auto uproj = EnvOrDefault("BP_READER_PROJECT", "");
    if (!uproj.empty()) {
        cfg.uproject = std::filesystem::path(uproj);
    }
    cfg.timeoutSeconds = IntFromEnvOrDefault("BP_READER_TIMEOUT_SECONDS", 120);
    return cfg;
}

std::unique_ptr<IBlueprintReader> Create(const BackendConfig& cfg) {
    if (cfg.backend == "mock") {
        return std::make_unique<MockBlueprintReader>(cfg.fixturesDir);
    }
    if (cfg.backend == "commandlet") {
        CommandletBlueprintReader::Config cc;
        cc.engineDir = cfg.engineDir;
        cc.uproject  = cfg.uproject;
        cc.timeout   = std::chrono::seconds(cfg.timeoutSeconds);
        return std::make_unique<CommandletBlueprintReader>(std::move(cc));
    }
    if (cfg.backend == "live") {
        throw BlueprintReaderError(fmt::format(
            "backend '{}' is not implemented yet (Phase 2). "
            "Set BP_READER_BACKEND to 'mock' or 'commandlet'.", cfg.backend));
    }
    throw BlueprintReaderError(fmt::format(
        "unknown backend '{}': expected one of mock|commandlet|live", cfg.backend));
}

} // namespace bpr::backends
