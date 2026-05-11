// ReadOnlyBlueprintReader — decorator that lets reads pass through and
// rejects every write with a structured error.
//
// Use case: coexistence with an open UE editor. Two processes mutating
// the same .uasset on disk corrupts state (see Troubleshooting wiki —
// "running daemon alongside open editor"). With BP_READER_READ_ONLY=1,
// the server still serves reads (list_blueprints, read_blueprint, etc.)
// against the on-disk state — fresh as of the last editor save thanks
// to the C2 mtime cache invalidation — but every write op throws a
// clear error pointing the caller at the right escape hatch:
//
//   "this MCP server is in read-only mode (BP_READER_READ_ONLY=1) —
//    open the editor and edit there, or unset the env var and restart
//    the server."
//
// The decorator wraps the inner reader (mock, commandlet, future live)
// at the same seam as CachingBlueprintReader. Stacks naturally:
//   commandlet → caching → read-only  (the order BackendFactory uses).

#pragma once

#include "backends/IBlueprintReader.h"

#include <memory>

namespace bpr::backends {

class ReadOnlyBlueprintReader : public IBlueprintReader {
public:
    explicit ReadOnlyBlueprintReader(std::unique_ptr<IBlueprintReader> inner);

    // ----- read tools (pass-through) --------------------------------
    std::vector<BPAssetSummary> ListBlueprints(std::string_view path) override;
    BPMetadata                  ReadBlueprint(std::string_view assetPath) override;
    BPGraph                     GetGraph(std::string_view assetPath, std::string_view graphName) override;
    BPFunction                  GetFunction(std::string_view assetPath, std::string_view fnName) override;
    std::vector<BPVariable>     ListVariables(std::string_view assetPath) override;
    std::vector<BPComponent>    GetComponents(std::string_view assetPath) override;
    std::vector<BPNode>         FindNode(std::string_view assetPath, std::string_view query,
                                          std::string_view kind = {}) override;

    // ----- write tools (all throw) ----------------------------------
    void AddVariable(std::string_view, std::string_view, const BPPinType&,
                     std::string_view, std::string_view, bool, bool) override;
    void SetNodePosition(std::string_view, std::string_view, std::string_view,
                         int, int) override;
    void DeleteNode(std::string_view, std::string_view, std::string_view) override;
    std::string AddNode(std::string_view, std::string_view, std::string_view,
                        int, int,
                        const std::map<std::string, std::string, std::less<>>&) override;
    void WirePins(std::string_view, std::string_view, std::string_view,
                  std::string_view, std::string_view, std::string_view) override;
    void DeleteVariable(std::string_view, std::string_view) override;
    void RenameVariable(std::string_view, std::string_view, std::string_view) override;
    AddFunctionResult AddFunction(std::string_view, std::string_view) override;
    void AddFunctionInput(std::string_view, std::string_view, std::string_view,
                          const BPPinType&) override;
    void AddFunctionOutput(std::string_view, std::string_view, std::string_view,
                           const BPPinType&) override;
    void DeleteFunction(std::string_view, std::string_view) override;
    void SetVariableDefault(std::string_view, std::string_view, std::string_view) override;
    CreateBlueprintResult CreateBlueprint(std::string_view, std::string_view) override;
    void SetPinDefault(std::string_view, std::string_view, std::string_view,
                       std::string_view, std::string_view) override;
    void RetypeVariable(std::string_view, std::string_view,
                        const BPPinType&) override;
    void SetVariableCategory(std::string_view, std::string_view,
                             std::string_view) override;
    DuplicateBlueprintResult DuplicateBlueprint(std::string_view,
                                                std::string_view) override;
    WriteGeneratedSourceResult WriteGeneratedSource(std::string_view,
                                                    std::string_view,
                                                    bool) override;

    // ----- Project + Content Browser ops --------------------------------
    // Reads pass through; writes (save/move/delete/create) throw the
    // read-only error.
    ProjectMetadata GetProjectMetadata() override;
    SaveAllResult SaveAll(bool) override;
    MoveAssetResult MoveAsset(std::string_view, std::string_view) override;
    DeleteAssetResult DeleteAsset(std::string_view, bool) override;
    CreateFolderResult CreateFolder(std::string_view) override;
    std::vector<BPAssetSummary> ListDataTables(std::string_view) override;
    DataTableInfo ReadDataTable(std::string_view) override;
    AddDataRowResult AddDataRow(std::string_view, std::string_view,
                                const nlohmann::json&, bool) override;
    SetDataRowValueResult SetDataRowValue(std::string_view, std::string_view,
                                          std::string_view, std::string_view) override;

    // ----- Live editor ops ----------------------------------------------
    // Reads (GetCVar, GetSelectedActors, ReadOutputLog, ConsoleCommand,
    // PieStart, LiveCodingCompile) pass through — they don't mutate
    // .uasset files which is what read-only mode is protecting. Real
    // mutations (SetCVar, SetSelection, SpawnActor, SetActorTransform,
    // DeleteActor, PieStop) reject. PieStart/Stop are debatable; we
    // gate Stop only since Start is "trigger an editor mode" not a write.
    ConsoleCommandResult ConsoleCommand(std::string_view) override;
    CVarValue GetCVar(std::string_view) override;
    CVarValue SetCVar(std::string_view, std::string_view) override;
    PieResult PieStart(std::string_view) override;
    PieResult PieStop() override;
    LiveCodingResult LiveCodingCompile() override;
    SelectionResult GetSelectedActors() override;
    SelectionResult SetSelection(const std::vector<std::string>&, bool) override;
    SpawnActorResult SpawnActor(std::string_view,
        double, double, double, double, double, double,
        double, double, double) override;
    void SetActorTransform(std::string_view,
        double, double, double, double, double, double,
        double, double, double) override;
    DeleteActorResult DeleteActor(std::string_view) override;
    OutputLogResult ReadOutputLog(int, std::string_view) override;
    // Automation tests pass through — they're read-shaped (trigger a run,
    // return what happened); they don't mutate .uasset files directly.
    AutomationRunResult RunAutomationTests(std::string_view) override;

    // ----- batch sentinels ------------------------------------------
    // BeginBatch / EndBatch are technically not writes themselves, but in
    // read-only mode they're still no-ops because no writes can happen
    // inside a batch. Pass-through to the inner so apply_ops with all-read
    // ops still works correctly (preview-style batches).
    void BeginBatch() override;
    nlohmann::json EndBatch(bool skipCompile = false) override;
    // ShutdownDaemon is allowed in read-only mode — releasing the daemon
    // is the whole reason this mode exists ("let me work in the editor
    // without daemon contention").
    nlohmann::json ShutdownDaemon() override;

private:
    std::unique_ptr<IBlueprintReader> inner_;
};

// Factory: wraps `inner` if `readOnly`, otherwise returns `inner` unchanged.
std::unique_ptr<IBlueprintReader> MaybeWrapReadOnly(
    std::unique_ptr<IBlueprintReader> inner, bool readOnly);

} // namespace bpr::backends
