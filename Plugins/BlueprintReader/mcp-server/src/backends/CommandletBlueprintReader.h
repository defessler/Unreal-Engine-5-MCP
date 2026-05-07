// CommandletBlueprintReader — drives the BlueprintReader plugin's UE
// commandlet to read blueprint data.
//
// Two modes:
//   * One-shot (default): spawn `UnrealEditor-Cmd.exe -run=BlueprintReader
//     -Op=<...>` per tool call. Each call pays the editor cold-start cost
//     (~5–7 s on a Dev box).
//   * Daemon (`Config::useDaemon = true`): spawn the editor once with
//     `-run=BlueprintReader -Daemon` and reuse the same process across all
//     calls. The plugin's daemon loop reads commandlet-arg lines from stdin,
//     prints `__BPR_READY__` once at startup and `__BPR_DONE <code>__` after
//     each command. Subsequent calls cost only the per-call work (~1 s).
//
// Both modes write JSON payloads to a temp file under %TEMP% so noisy
// editor log output on stdout doesn't pollute it.
//
// Configuration (env, defaults set by ConfigFromEnv):
//   * BP_READER_ENGINE_DIR       — path to the source-built engine
//   * BP_READER_PROJECT          — path to the .uproject
//   * BP_READER_TIMEOUT_SECONDS  — per-call timeout (default 120)
//   * BP_READER_DAEMON           — set to 1/true/yes to enable daemon mode
#pragma once

#include "backends/IBlueprintReader.h"

#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
#endif

namespace bpr::backends {

class CommandletBlueprintReader final : public IBlueprintReader {
public:
    struct Config {
        std::filesystem::path engineDir;
        std::filesystem::path uproject;
        std::chrono::seconds timeout{120};         // per-tool-call timeout
        std::chrono::seconds startupTimeout{600};  // initial daemon READY wait;
                                                   // big projects need minutes to
                                                   // load modules + scan asset registry
        std::string editorExtraArgs;               // appended to commandlet command
                                                   // line, whitespace-separated
        bool useDaemon = false;
    };

    explicit CommandletBlueprintReader(Config cfg);
    ~CommandletBlueprintReader() override;

    CommandletBlueprintReader(const CommandletBlueprintReader&) = delete;
    CommandletBlueprintReader& operator=(const CommandletBlueprintReader&) = delete;

    std::vector<BPAssetSummary> ListBlueprints(std::string_view path) override;
    BPMetadata                  ReadBlueprint(std::string_view assetPath) override;
    BPGraph                     GetGraph(std::string_view assetPath, std::string_view graphName) override;
    BPFunction                  GetFunction(std::string_view assetPath, std::string_view fnName) override;
    std::vector<BPVariable>     ListVariables(std::string_view assetPath) override;
    std::vector<BPComponent>    GetComponents(std::string_view assetPath) override;
    std::vector<BPNode>         FindNode(std::string_view assetPath, std::string_view query,
                                         std::string_view kind = {}) override;

    void AddVariable(std::string_view assetPath, std::string_view name,
                     const BPPinType& type, std::string_view defaultValue,
                     std::string_view category, bool replicated, bool editable) override;
    void SetNodePosition(std::string_view assetPath, std::string_view graphName,
                         std::string_view nodeId, int x, int y) override;
    void DeleteNode(std::string_view assetPath, std::string_view graphName,
                    std::string_view nodeId) override;
    std::string AddNode(std::string_view assetPath, std::string_view graphName,
                        std::string_view kind, int x, int y,
                        const std::map<std::string, std::string, std::less<>>& extras) override;
    void WirePins(std::string_view assetPath, std::string_view graphName,
                  std::string_view fromNodeId, std::string_view fromPinSpec,
                  std::string_view toNodeId,   std::string_view toPinSpec) override;
    void DeleteVariable(std::string_view assetPath, std::string_view name) override;
    void RenameVariable(std::string_view assetPath, std::string_view oldName,
                        std::string_view newName) override;
    std::string AddFunction(std::string_view assetPath, std::string_view name) override;
    void AddFunctionInput(std::string_view assetPath, std::string_view functionName,
                          std::string_view paramName, const BPPinType& type) override;
    void AddFunctionOutput(std::string_view assetPath, std::string_view functionName,
                           std::string_view paramName, const BPPinType& type) override;
    void DeleteFunction(std::string_view assetPath, std::string_view name) override;
    void SetVariableDefault(std::string_view assetPath, std::string_view name,
                            std::string_view newDefault) override;

    // Spin up the editor daemon now in a background thread. Tool calls that
    // arrive before the daemon is READY block on the same daemonMutex_ used
    // by RunOpDaemon, so this is racy-safe: a real call either completes the
    // prewarm work itself (if it lost the race) or finds a hot daemon. No-op
    // if useDaemon is false or the platform doesn't support daemon mode.
    void Prewarm();

private:
    // Dispatches to RunOpOneShot or RunOpDaemon. Always writes its JSON
    // payload to a temp file under %TEMP% and returns the parsed JSON.
    nlohmann::json RunOp(const std::vector<std::wstring>& opArgs);

    nlohmann::json RunOpOneShot(const std::vector<std::wstring>& opArgs);
    nlohmann::json RunOpDaemon(const std::vector<std::wstring>& opArgs);

#if defined(_WIN32)
    void EnsureDaemon();
    void TerminateDaemon();
    // Drain daemonStdout_ into accumulator_ until `marker` appears (or the
    // deadline hits). On success, consumes everything up to and including
    // the marker. Returns the bytes consumed (excluding the marker).
    // EnsureDaemon passes startupTimeout (longer); RunOpDaemon passes the
    // per-call timeout.
    std::string ReadUntilMarker(const std::string& marker, std::chrono::seconds timeout);

    HANDLE daemonProcess_ = nullptr;
    HANDLE daemonStdin_   = nullptr;
    HANDLE daemonStdout_  = nullptr;
    std::string accumulator_;
#endif

    std::mutex daemonMutex_;  // serializes RunOpDaemon (one in-flight call max)
    std::thread prewarmThread_;  // joined in destructor; running EnsureDaemon under daemonMutex_
    Config cfg_;
    std::filesystem::path editorCmdExe_;
};

} // namespace bpr::backends
