// MockBlueprintReader — fixture-backed implementation. Loads each
// BP_*.json file under the configured fixture directory at construction
// time. Each fixture file holds a fully-realized BPMetadata + a list of
// graphs + functions, plus a BPAssetSummary. See fixtures/README.md for
// the on-disk shape.
#pragma once

#include "backends/IBlueprintReader.h"

#include <filesystem>
#include <map>

namespace bpr::backends {

class MockBlueprintReader final : public IBlueprintReader {
public:
    explicit MockBlueprintReader(const std::filesystem::path& fixturesDir);

    std::vector<BPAssetSummary> ListBlueprints(std::string_view path) override;
    BPMetadata                  ReadBlueprint(std::string_view assetPath) override;
    BPGraph                     GetGraph(std::string_view assetPath, std::string_view graphName) override;
    BPFunction                  GetFunction(std::string_view assetPath, std::string_view fnName) override;
    std::vector<BPVariable>     ListVariables(std::string_view assetPath) override;
    std::vector<BPComponent>    GetComponents(std::string_view assetPath) override;
    std::vector<BPNode>         FindNode(std::string_view assetPath, std::string_view query,
                                          std::string_view kind = {}) override;

    // Write tools — mock fixtures are read-only; these throw.
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

    // Number of loaded fixtures — for diagnostics + tests.
    std::size_t FixtureCount() const { return assets_.size(); }

private:
    struct FixtureEntry {
        BPAssetSummary summary;
        BPMetadata metadata;
        std::vector<BPGraph> graphs;
        std::vector<BPFunction> functions;
        std::vector<BPComponent> components;  // optional `components` array in fixture JSON
    };

    void LoadDir(const std::filesystem::path& dir);
    void LoadFile(const std::filesystem::path& file);

    const FixtureEntry& Require(std::string_view assetPath) const;

    // asset_path -> entry.
    std::map<std::string, FixtureEntry, std::less<>> assets_;
};

} // namespace bpr::backends
