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

// Not `final`: tests subclass this to override a single method (e.g.
// FindAsset) and drive a tool handler without stubbing the whole
// IBlueprintReader surface.
class MockBlueprintReader : public IBlueprintReader {
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
	AddFunctionResult AddFunction(std::string_view assetPath, std::string_view name) override;
	void AddFunctionInput(std::string_view assetPath, std::string_view functionName,
						  std::string_view paramName, const BPPinType& type) override;
	void AddFunctionOutput(std::string_view assetPath, std::string_view functionName,
						   std::string_view paramName, const BPPinType& type) override;
	void DeleteFunction(std::string_view assetPath, std::string_view name) override;
	void SetVariableDefault(std::string_view assetPath, std::string_view name,
							std::string_view newDefault) override;
	CreateBlueprintResult CreateBlueprint(std::string_view assetPath,
										  std::string_view parentClass,
										  std::string_view blueprintType) override;
	CreateMaterialResult CreateMaterial(std::string_view assetPath) override;
	CreateMaterialInstanceResult CreateMaterialInstance(std::string_view assetPath,
														std::string_view parentPath) override;
	CloneGraphResult CloneGraph(std::string_view sourcePath,
								std::string_view targetPath,
								std::string_view graphName) override;
	void ImplementInterface(std::string_view assetPath,
							std::string_view interfacePath) override;
	void SetPinDefault(std::string_view assetPath, std::string_view graphName,
					   std::string_view nodeId, std::string_view pinSpec,
					   std::string_view value) override;
	void RetypeVariable(std::string_view assetPath, std::string_view name,
						const BPPinType& newType) override;
	void SetVariableCategory(std::string_view assetPath, std::string_view name,
							 std::string_view category) override;
	DuplicateBlueprintResult DuplicateBlueprint(std::string_view sourceAssetPath,
												std::string_view destAssetPath) override;
	WriteGeneratedSourceResult WriteGeneratedSource(std::string_view destPath,
													std::string_view content,
													bool createDirs) override;

	// Structural diff isn't supported by the mock backend — the fixture
	// JSON doesn't carry the UBlueprint reflection needed to compute it.
	nlohmann::json StructuralDiff(std::string_view a, std::string_view b,
								   const StructuralDiffOptions& opts) override;

	// Not supported on the mock backend (no UObject world); throws. Listed
	// in UnsupportedTools() so the catalog hides it under the mock.
	nlohmann::json ReadActorInstance(std::string_view assetPath) override;

	// Not supported on the mock backend (no UClass registry); throws. Listed
	// in UnsupportedTools() so the catalog hides it under the mock.
	nlohmann::json DescribeK2Node(std::string_view classPath) override;

	// Not supported on the mock backend (no Slate UI); throws. Listed in
	// UnsupportedTools() so the catalog hides it under the mock.
	nlohmann::json UiListWidgets(int maxDepth, int maxWidgets,
								  std::string_view window,
								  std::string_view type) override;
	nlohmann::json UiClick(std::string_view widgetPath,
							std::string_view expectType,
							std::string_view expectText) override;
	nlohmann::json UiType(std::string_view widgetPath,
						  std::string_view text,
						  std::string_view expectType) override;

	// UX-P4a: the mock backend has no editor game thread, so health is always a
	// synthetic-healthy result (age 0). It IS supported (returns, never throws) —
	// do NOT add it to UnsupportedTools().
	HealthResult HealthCheck() override;

	// Capability advertisement — the mock backend has no editor and no
	// asset registry, so a long tail of tools the catalog otherwise
	// advertises just throws "not supported by this backend" when
	// called. Main filters these out at startup so agents don't burn
	// turns discovering capability gaps.
	std::vector<std::string> UnsupportedTools() const override;

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

	// EDIT-2: timeline tools — throw on mock (no fixture support yet)
	BPRJson ListTimelines(std::string_view a) override;
	BPRJson ReadTimeline(std::string_view a, std::string_view n) override;
	BPRJson ListAnimMontages(std::string_view p) override;
	BPRJson ReadAnimMontage(std::string_view a) override;

	// asset_path -> entry.
	std::map<std::string, FixtureEntry, std::less<>> assets_;
};

}    // namespace bpr::backends
