// LiveBlueprintReader — talks to a running UE editor over TCP instead
// of spawning a UnrealEditor-Cmd commandlet daemon as a child process.
//
// Use case: you have the full editor open. Instead of running a second
// editor-shaped process (which would fight over the same DDC / asset
// registry / .uasset files), the agent connects to the editor itself.
// Reads see live in-memory state including unsaved edits; writes go
// through the editor's normal mutation pipeline (content browser
// refreshes, asset becomes dirty, save-all picks it up).
//
// Wire protocol (newline-delimited JSON over a localhost TCP socket):
//   server → client  { "type": "hello", "version": "1" }
//   client → server  { "type": "auth", "token": "<shared>" }
//   server → client  { "type": "auth_ok" } | { "type": "auth_fail" }
//   client → server  { "type": "op", "id": N, "args": ["-Op=...", ...] }
//   server → client  { "type": "result", "id": N, "code": K, "json": {...} }
//
// The `args` array is the same `-Op=Read -Asset=...` format the
// commandlet daemon accepts — implemented by an editor module-internal
// dispatcher that calls into the same RunOneOp logic the commandlet
// uses, just on the editor's game thread instead of in a child process.
//
// Auth: BP_READER_LIVE_TOKEN env var must be set in BOTH the editor's
// process AND the MCP server's process. Editor refuses to start the
// listener without it; client refuses to connect without it.
//
// Failure modes:
//  - Editor not running with the live module → connect fails, throws.
//  - Token mismatch → server closes connection after auth_fail; client
//    throws on the next op call.
//  - Mid-batch socket close → in-flight op throws; subsequent calls
//    throw too. Caller must construct a new LiveBlueprintReader.

#pragma once

#include "backends/IBlueprintReader.h"

#include <chrono>
#include <mutex>
#include <string>

namespace bpr::backends {

class LiveBlueprintReader : public IBlueprintReader {
public:
    struct Config {
        std::string host = "127.0.0.1";
        int         port = 0;            // 0 → throw at construct time
        std::string token;               // required (auth)
        std::chrono::seconds connectTimeout{5};
        std::chrono::seconds opTimeout{60};
    };

    explicit LiveBlueprintReader(Config cfg);
    ~LiveBlueprintReader() override;

    LiveBlueprintReader(const LiveBlueprintReader&) = delete;
    LiveBlueprintReader& operator=(const LiveBlueprintReader&) = delete;

    // ----- read tools -----------------------------------------------
    std::vector<BPAssetSummary> ListBlueprints(std::string_view path) override;
    BPMetadata                  ReadBlueprint(std::string_view assetPath) override;
    BPGraph                     GetGraph(std::string_view assetPath, std::string_view graphName) override;
    BPFunction                  GetFunction(std::string_view assetPath, std::string_view fnName) override;
    std::vector<BPVariable>     ListVariables(std::string_view assetPath) override;
    std::vector<BPComponent>    GetComponents(std::string_view assetPath) override;
    std::vector<BPNode>         FindNode(std::string_view assetPath, std::string_view query,
                                          std::string_view kind = {}) override;

    // ----- write tools ----------------------------------------------
    // All writes route through the editor's RunOneOp dispatch on the
    // game thread — same code as the commandlet, just no child process.
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
    AddFunctionResult AddFunction(std::string_view assetPath, std::string_view name) override;
    void AddFunctionInput(std::string_view assetPath, std::string_view functionName,
                          std::string_view paramName, const BPPinType& type) override;
    void AddFunctionOutput(std::string_view assetPath, std::string_view functionName,
                           std::string_view paramName, const BPPinType& type) override;
    void DeleteFunction(std::string_view assetPath, std::string_view name) override;
    void SetVariableDefault(std::string_view assetPath, std::string_view name,
                            std::string_view newDefault) override;
    CreateBlueprintResult CreateBlueprint(std::string_view assetPath,
                                          std::string_view parentClass) override;
    void SetPinDefault(std::string_view assetPath, std::string_view graphName,
                       std::string_view nodeId, std::string_view pinSpec,
                       std::string_view value) override;

    // ----- batch sentinels ------------------------------------------
    void BeginBatch() override;
    nlohmann::json EndBatch(bool skipCompile = false) override;

    // ShutdownDaemon doesn't apply to live mode (the editor runs
    // independently of the MCP server's lifetime). Returns
    // {ok:true, was_running:false}.
    nlohmann::json ShutdownDaemon() override;

private:
    // Send op-args, read result, return parsed `json` field. Throws
    // BlueprintReaderError on any wire/protocol/handler failure.
    nlohmann::json RunOp(const std::vector<std::string>& args);

    // Connect + handshake. Sets handshakeOk_ on success. Called lazily
    // from the first RunOp so construction can complete cleanly even
    // when the editor isn't running yet.
    void EnsureConnected();
    void Disconnect();

    Config cfg_;
    std::mutex mu_;
    intptr_t   socket_ = -1;  // typed as intptr_t to avoid winsock2.h in header
    bool       handshakeOk_ = false;
    int        nextRequestId_ = 1;
};

} // namespace bpr::backends
