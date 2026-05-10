#include "backends/BackendFactory.h"
#include "backends/AutoBlueprintReader.h"
#include "backends/CachingBlueprintReader.h"
#include "backends/CommandletBlueprintReader.h"
#include "backends/LiveBlueprintReader.h"
#include "backends/MockBlueprintReader.h"
#include "backends/ReadOnlyBlueprintReader.h"
#include "Env.h"

#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>

#include <fmt/core.h>
#include <nlohmann/json.hpp>

namespace bpr::backends {

namespace {

// Look for `<projectDir>/Saved/bp-reader-live.json`, the file the
// editor's BlueprintReaderLiveServer drops on StartupModule. Returns
// an empty optional if missing / malformed; caller falls back to env
// vars or commandlet.
//
// Schema: { "version": 1, "host": "127.0.0.1", "port": <int>,
//           "token": "<hex>", "pid": <int>, "started_at": "..." }
struct HandshakeFile {
    std::string host;
    int         port = 0;
    std::string token;
    int         pid  = 0;
};
std::optional<HandshakeFile> ReadHandshakeFile(
    const std::filesystem::path& uproject, std::ostream& log) {
    if (uproject.empty()) return std::nullopt;
    std::filesystem::path path =
        uproject.parent_path() / "Saved" / "bp-reader-live.json";
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return std::nullopt;
    std::ifstream f(path);
    if (!f) {
        log << "[bp-reader-mcp] live handshake file exists but is unreadable: "
            << path.string() << "\n";
        return std::nullopt;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(ss.str());
    } catch (const std::exception& e) {
        log << "[bp-reader-mcp] live handshake file is malformed JSON ("
            << e.what() << "); ignoring\n";
        return std::nullopt;
    }
    HandshakeFile hf;
    hf.host  = j.value("host",  std::string("127.0.0.1"));
    hf.port  = j.value("port",  0);
    hf.token = j.value("token", std::string());
    hf.pid   = j.value("pid",   0);
    if (hf.port <= 0 || hf.token.empty()) {
        log << "[bp-reader-mcp] live handshake file present but missing "
               "port/token; ignoring\n";
        return std::nullopt;
    }
    return hf;
}

} // namespace

BackendConfig ConfigFromEnv(const std::filesystem::path& executableDir,
                            std::ostream& log) {
    BackendConfig cfg;

    // ----- explicit env vars (with defaults) ---------------------------
    cfg.backend = env::GetOrDefault("BP_READER_BACKEND", "");

    auto fix = env::GetOrDefault("BP_READER_FIXTURES_DIR", "");
    if (fix.empty()) {
        cfg.fixturesDir = executableDir / "fixtures";
    } else {
        cfg.fixturesDir = std::filesystem::path(fix);
    }

    auto engineDir = env::GetOrDefault("BP_READER_ENGINE_DIR", "");
    if (!engineDir.empty()) cfg.engineDir = std::filesystem::path(engineDir);

    auto uproj = env::GetOrDefault("BP_READER_PROJECT", "");
    if (!uproj.empty()) cfg.uproject = std::filesystem::path(uproj);

    cfg.timeoutSeconds        = env::IntOrDefault("BP_READER_TIMEOUT_SECONDS", 120);
    cfg.startupTimeoutSeconds = env::IntOrDefault("BP_READER_STARTUP_TIMEOUT_SECONDS", 600);
    cfg.editorConfig          = env::GetOrDefault("BP_READER_EDITOR_CONFIG", "");
    cfg.editorExtraArgs       = env::GetOrDefault("BP_READER_EDITOR_ARGS", "");
    cfg.useDaemon             = env::BoolOrDefault("BP_READER_DAEMON", true, log);
    cfg.prewarm               = env::BoolOrDefault("BP_READER_PREWARM", false, log);
    cfg.cacheTtlSeconds       = env::IntOrDefault("BP_READER_CACHE_TTL_SECONDS", 30);
    cfg.readOnly              = env::BoolOrDefault("BP_READER_READ_ONLY", false, log);
    cfg.liveHost              = env::GetOrDefault("BP_READER_LIVE_HOST", "127.0.0.1");
    cfg.liveProcPort          = env::IntOrDefault("BP_READER_LIVE_PORT", 0);
    cfg.liveToken             = env::GetOrDefault("BP_READER_LIVE_TOKEN", "");

    // ----- auto-discovery (Tier 1 UX) ---------------------------------
    //
    // The exe normally lives at:
    //   <projectRoot>/Plugins/BlueprintReader/mcp-server/build/Release/bp-reader-mcp.exe
    //                              ^pluginDir^
    //                              ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
    //                              ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
    //                              \-------- 5 levels up to project root --/
    //
    // From there we can read .uproject -> EngineAssociation, and resolve the
    // engine root from HKCU. Eliminates BP_READER_PROJECT and
    // BP_READER_ENGINE_DIR for users on the standard layout.

    // Plugin dir is 3 levels above exeDir:
    //   <plugin>/mcp-server/build/Release/bp-reader-mcp.exe
    //                ^1         ^2     ^3 (== exeDir)
    //   ^plugin = up 3 from exeDir
    std::filesystem::path pluginDir;
    {
        auto p = executableDir;  // ...\Release
        for (int i = 0; i < 3 && !p.empty(); ++i) p = p.parent_path();
        pluginDir = p;
    }

    if (cfg.uproject.empty()) {
        // Search up from pluginDir's parent (Plugins/) for a .uproject.
        std::filesystem::path searchStart = pluginDir.parent_path();  // Plugins/
        if (auto found = env::FindUprojectAbove(searchStart)) {
            cfg.uproject = *found;
            log << "[bp-reader-mcp] auto-discovered project: "
                << cfg.uproject.string() << "\n";
        }
    }

    if (cfg.engineDir.empty() && !cfg.uproject.empty()) {
        if (auto assoc = env::ReadEngineAssociation(cfg.uproject)) {
            if (auto root = env::ResolveEngineFromRegistry(*assoc)) {
                cfg.engineDir = *root;
                log << "[bp-reader-mcp] auto-discovered engine: "
                    << cfg.engineDir.string() << " (from EngineAssociation="
                    << *assoc << ")\n";
            } else {
                log << "[bp-reader-mcp] warning: .uproject EngineAssociation='"
                    << *assoc << "' not found in HKCU\\SOFTWARE\\Epic Games\\"
                    << "Unreal Engine\\Builds — set BP_READER_ENGINE_DIR "
                    << "explicitly, or right-click the .uproject and run "
                    << "'Switch Unreal Engine version'\n";
            }
        }
    }

    if (cfg.editorConfig.empty()) {
        if (auto detected = env::DetectEditorConfig(pluginDir)) {
            cfg.editorConfig = *detected;
            // Only emit a log line if it's a non-default config — Development
            // is the implicit default and worth less log noise.
            if (cfg.editorConfig != "Development") {
                log << "[bp-reader-mcp] auto-detected editor config: "
                    << cfg.editorConfig
                    << " (matched plugin DLL suffix; override with "
                    << "BP_READER_EDITOR_CONFIG)\n";
            }
        }
    }

    // Auto-discover live host/port/token from the editor's handshake
    // file when env vars haven't supplied them. The plugin's
    // BlueprintReaderLiveServer drops this file on StartupModule, so
    // an open editor "publishes" its credentials for the MCP server.
    if (auto hf = ReadHandshakeFile(cfg.uproject, log)) {
        if (cfg.liveHost.empty() || cfg.liveHost == "127.0.0.1") {
            cfg.liveHost = hf->host;
        }
        if (cfg.liveProcPort == 0) cfg.liveProcPort = hf->port;
        if (cfg.liveToken.empty()) cfg.liveToken = hf->token;
        log << "[bp-reader-mcp] discovered live editor on "
            << cfg.liveHost << ":" << cfg.liveProcPort
            << " (pid=" << hf->pid << ")\n";
    }

    // Backend default — if we found a uproject, default to `auto` so
    // we transparently pick live (when an editor is up) or commandlet
    // (when not). Explicit BP_READER_BACKEND=commandlet|live still wins
    // for users who want the old, deterministic behavior.
    if (cfg.backend.empty()) {
        cfg.backend = cfg.uproject.empty() ? "mock" : "auto";
    }

    return cfg;
}

std::unique_ptr<IBlueprintReader> Create(const BackendConfig& cfg) {
    auto buildInner = [&]() -> std::unique_ptr<IBlueprintReader> {
        if (cfg.backend == "mock") {
            return std::make_unique<MockBlueprintReader>(cfg.fixturesDir);
        }
        if (cfg.backend == "commandlet") {
            CommandletBlueprintReader::Config cc;
            cc.engineDir       = cfg.engineDir;
            cc.uproject        = cfg.uproject;
            cc.timeout         = std::chrono::seconds(cfg.timeoutSeconds);
            cc.startupTimeout  = std::chrono::seconds(cfg.startupTimeoutSeconds);
            cc.useDaemon       = cfg.useDaemon;
            cc.editorConfig    = cfg.editorConfig;
            cc.editorExtraArgs = cfg.editorExtraArgs;
            auto r = std::make_unique<CommandletBlueprintReader>(std::move(cc));
            if (cfg.prewarm && cfg.useDaemon) {
                r->Prewarm();
            }
            return r;
        }
        if (cfg.backend == "live") {
            LiveBlueprintReader::Config lc;
            lc.host = cfg.liveHost;
            lc.port = cfg.liveProcPort;
            lc.token = cfg.liveToken;
            return std::make_unique<LiveBlueprintReader>(std::move(lc));
        }
        if (cfg.backend == "auto") {
            // Auto-routes per call: probe the live handshake; if the
            // editor responds, use Live, else Commandlet. Live config
            // may be empty (no editor running yet) — the wrapper
            // re-reads the handshake file on each probe so it picks
            // up an editor that launched mid-session.
            AutoBlueprintReader::Config ac;
            ac.uproject = cfg.uproject;
            ac.liveHost = cfg.liveHost.empty() ? "127.0.0.1" : cfg.liveHost;
            ac.livePort = cfg.liveProcPort;
            ac.liveToken = cfg.liveToken;
            CommandletBlueprintReader::Config cc;
            cc.engineDir       = cfg.engineDir;
            cc.uproject        = cfg.uproject;
            cc.timeout         = std::chrono::seconds(cfg.timeoutSeconds);
            cc.startupTimeout  = std::chrono::seconds(cfg.startupTimeoutSeconds);
            cc.useDaemon       = cfg.useDaemon;
            cc.editorConfig    = cfg.editorConfig;
            cc.editorExtraArgs = cfg.editorExtraArgs;
            ac.commandletConfig = std::move(cc);
            ac.prewarmCommandlet = cfg.prewarm && cfg.useDaemon;
            return std::make_unique<AutoBlueprintReader>(std::move(ac));
        }
        throw BlueprintReaderError(fmt::format(
            "unknown backend '{}': expected one of mock|commandlet|live|auto", cfg.backend));
    };
    // C2: pass the .uproject path so the cache can resolve /Game/X to the
    // on-disk .uasset and add mtime-based invalidation on top of TTL. The
    // cache itself is no-op when projectDir is empty (mock backend, etc.).
    auto cached = WrapWithCache(buildInner(),
                                std::chrono::seconds(cfg.cacheTtlSeconds),
                                cfg.uproject);
    // ReadOnly wraps outermost so writes are rejected before any caching
    // or commandlet round-trip happens — fast-fail with a clear error.
    return MaybeWrapReadOnly(std::move(cached), cfg.readOnly);
}

} // namespace bpr::backends
