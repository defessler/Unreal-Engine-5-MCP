// Selects an IBlueprintReader implementation based on the BP_READER_BACKEND
// env var. Centralizes the backend-name → implementation mapping so tests
// can mock the env without re-implementing the parsing.
#pragma once

#include "backends/IBlueprintReader.h"

#include <filesystem>
#include <memory>
#include <string>

namespace bpr::backends {

struct BackendConfig {
    std::string backend = "mock";    // mock | commandlet | live
    std::filesystem::path fixturesDir;
    // commandlet-only:
    std::filesystem::path engineDir;
    std::filesystem::path uproject;
    int timeoutSeconds = 120;
};

// Read BP_READER_BACKEND and BP_READER_FIXTURES_DIR from the environment.
// fixturesDir defaults to <executableDir>/fixtures.
BackendConfig ConfigFromEnv(const std::filesystem::path& executableDir);

// May throw BlueprintReaderError for unsupported backends or bad fixture
// directories.
std::unique_ptr<IBlueprintReader> Create(const BackendConfig& cfg);

} // namespace bpr::backends
