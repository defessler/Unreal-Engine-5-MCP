// Selects an IBlueprintReader implementation based on the BP_READER_BACKEND
// env var. Centralizes the backend-name → implementation mapping so tests
// can mock the env without re-implementing the parsing.
#pragma once

#include "backends/IBlueprintReader.h"

#include <filesystem>
#include <iosfwd>
#include <memory>
#include <string>

namespace bpr::backends {

struct BackendConfig {
    std::string backend = "mock";    // mock | commandlet | live
    std::filesystem::path fixturesDir;
    // commandlet-only:
    std::filesystem::path engineDir;
    std::filesystem::path uproject;
    int timeoutSeconds        = 120;   // per-tool-call subprocess timeout
    int startupTimeoutSeconds = 600;   // daemon's initial READY wait. Big UE projects
                                       // can take minutes to load modules + scan the
                                       // asset registry + warm shaders the first time;
                                       // 120s isn't enough for them. Tune via
                                       // BP_READER_STARTUP_TIMEOUT_SECONDS.
    std::string editorConfig;     // "Development" (default) | "DebugGame" |
                                  // "Debug" | "Test" | "Shipping". Picks
                                  // which UnrealEditor-Cmd[-Win64-Config].exe
                                  // the daemon launches — must match the
                                  // config your BlueprintReaderEditor module
                                  // was compiled in. Set via
                                  // BP_READER_EDITOR_CONFIG.
    std::string editorExtraArgs;  // appended to UnrealEditor-Cmd's command line
                                  // (whitespace-separated). Useful for
                                  // -EnableAllPlugins when a project enables
                                  // plugins whose binaries aren't built. Set
                                  // via BP_READER_EDITOR_ARGS.
    bool useDaemon = true;        // commandlet-only; opt out via BP_READER_DAEMON=0
    bool prewarm    = false;      // commandlet+daemon-only; opt in via BP_READER_PREWARM=1.
                                  // Spawns the editor daemon on startup in a background
                                  // thread so the first tool call doesn't pay the ~5–30 s
                                  // editor cold-start cost.
    int  cacheTtlSeconds = 30;    // Memoize read-tool results for this many
                                  // seconds. 0 disables caching. Default is
                                  // 30 — long enough to absorb back-to-back
                                  // queries from an AI client, short enough
                                  // that user edits in the editor flush within
                                  // a turn or two. Set via
                                  // BP_READER_CACHE_TTL_SECONDS.
    bool readOnly = false;        // Reject every write tool with a clear
                                  // error pointing at this env var. Intended
                                  // for coexistence with an open UE editor —
                                  // see Troubleshooting wiki. Set via
                                  // BP_READER_READ_ONLY.

    // Live backend (BP_READER_BACKEND=live): connect to a running UE
    // editor's BlueprintReaderEditor module's TCP listener instead of
    // spawning a commandlet child. Editor and MCP server must share the
    // same token. See Configuration → "Live backend" in the wiki.
    std::string liveHost;         // BP_READER_LIVE_HOST (default 127.0.0.1)
    int liveProcPort = 0;         // BP_READER_LIVE_PORT (required for live)
    std::string liveToken;        // BP_READER_LIVE_TOKEN (required)
};

// Read BP_READER_* from the environment, then auto-discover anything still
// unset by walking up from the exe to find a .uproject and resolving its
// engine via the registry. `log` receives any informational/warning messages
// (auto-discovery findings, env-var parse warnings).
//
// fixturesDir defaults to <executableDir>/fixtures.
BackendConfig ConfigFromEnv(const std::filesystem::path& executableDir,
                            std::ostream& log);

// May throw BlueprintReaderError for unsupported backends or bad fixture
// directories.
std::unique_ptr<IBlueprintReader> Create(const BackendConfig& cfg);

} // namespace bpr::backends
