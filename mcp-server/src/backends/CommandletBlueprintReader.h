// CommandletBlueprintReader — drives the BlueprintReader plugin's UE
// commandlet over a one-shot subprocess for each tool call. Cold start is
// 10–30s; that's acceptable for Phase 1 (a daemon mode is a future phase).
//
// The reader spawns `UnrealEditor-Cmd.exe <uproject> -run=BlueprintReader
// -Op=<...> ... -Out=<temp.json> -nullrhi -nosplash -unattended -nopause
// -stdout`, waits for it to exit, then reads the temp file and parses the
// canonical wire-format JSON into the Shared types.
//
// Configuration (env, with sensible defaults set by ConfigFromEnv):
//   * BP_READER_ENGINE_DIR      — path to the source-built engine
//   * BP_READER_PROJECT          — path to the .uproject
//   * BP_READER_TIMEOUT_SECONDS — per-call timeout (default 120)
#pragma once

#include "backends/IBlueprintReader.h"

#include <chrono>
#include <filesystem>

namespace bpr::backends {

class CommandletBlueprintReader final : public IBlueprintReader {
public:
    struct Config {
        std::filesystem::path engineDir;
        std::filesystem::path uproject;
        std::chrono::seconds timeout{120};
    };

    explicit CommandletBlueprintReader(Config cfg);

    std::vector<BPAssetSummary> ListBlueprints(std::string_view path) override;
    BPMetadata                  ReadBlueprint(std::string_view assetPath) override;
    BPGraph                     GetGraph(std::string_view assetPath, std::string_view graphName) override;
    BPFunction                  GetFunction(std::string_view assetPath, std::string_view fnName) override;
    std::vector<BPVariable>     ListVariables(std::string_view assetPath) override;
    std::vector<BPNode>         FindNode(std::string_view assetPath, std::string_view query) override;

private:
    // Run a single `-run=BlueprintReader` invocation with the given argv. Writes
    // its JSON payload into a temp file and returns the parsed JSON.
    nlohmann::json RunOp(const std::vector<std::wstring>& opArgs);

    Config cfg_;
    std::filesystem::path editorCmdExe_;
};

} // namespace bpr::backends
