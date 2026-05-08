#include "backends/BackendFactory.h"
#include "backends/CachingBlueprintReader.h"
#include "backends/CommandletBlueprintReader.h"
#include "backends/MockBlueprintReader.h"
#include "Env.h"

#include <iostream>

#include <fmt/core.h>

namespace bpr::backends {

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

    // Backend default — if we found a uproject, switch to commandlet.
    // Mock is only the right default when there's nothing project-shaped
    // around the exe.
    if (cfg.backend.empty()) {
        cfg.backend = cfg.uproject.empty() ? "mock" : "commandlet";
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
            throw BlueprintReaderError(fmt::format(
                "backend '{}' is not implemented yet (Phase 2). "
                "Set BP_READER_BACKEND to 'mock' or 'commandlet'.", cfg.backend));
        }
        throw BlueprintReaderError(fmt::format(
            "unknown backend '{}': expected one of mock|commandlet|live", cfg.backend));
    };
    // C2: pass the .uproject path so the cache can resolve /Game/X to the
    // on-disk .uasset and add mtime-based invalidation on top of TTL. The
    // cache itself is no-op when projectDir is empty (mock backend, etc.).
    return WrapWithCache(buildInner(),
                         std::chrono::seconds(cfg.cacheTtlSeconds),
                         cfg.uproject);
}

} // namespace bpr::backends
