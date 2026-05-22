// AutoBlueprintReader — routes each call to live or commandlet based
// on whether an editor is currently running.
//
// Why this exists: users would otherwise flip BP_READER_BACKEND between
// `commandlet` (no editor open) and `live` (editor open) by hand. Auto
// mode probes the editor's BlueprintReaderLiveServer handshake file +
// TCP listener on each call (with a short cache) and picks the right
// backend transparently. Editor opens mid-session → calls start
// landing on Live; editor closes → calls fall back to commandlet.
//
// Probe strategy:
//   1. Re-read `<ProjectDir>/Saved/bp-reader-live.json` (cheap; the
//      file exists iff an editor module is loaded). The plugin deletes
//      the file at ShutdownModule, so absence is a strong "no editor"
//      signal.
//   2. If file exists, attempt a one-shot TCP connect to the published
//      host:port. Success → use Live; failure (refused / timeout) → fall
//      through to the cmdlet check (the file is stale from a crashed
//      editor).
//   3. If no live editor, re-read `<ProjectDir>/Saved/bp-reader-cmdlet.json`
//      (written by a commandlet daemon attached via TCP — Task 2.4).
//      Success → use a SocketBlueprintReader pointing at that daemon,
//      avoiding a second commandlet spawn; failure → fall through.
//   4. No daemons available → fall back to spawning a commandlet via
//      `CommandletBlueprintReader` (its own attach-or-spawn logic).
//   5. Probe result is cached for `probeTtl` to amortize the
//      file-read + connect cost across rapid back-to-back tool calls.
//
// Lifecycle:
//   - Live reader is created lazily on first probe-success and reused
//     until a probe fails (then dropped + re-created next probe-success).
//   - Commandlet reader is created up-front (so `prewarm` semantics
//     work the same as plain commandlet mode); shut down only on
//     destruction. Probe failures don't tear down the commandlet, just
//     route to it.
//
// Edge cases:
//   - User has BOTH BP_READER_LIVE_PORT/TOKEN env vars AND an editor
//     handshake file: env wins (caller asked for explicit values).
//   - Live probe succeeds but RunOp throws mid-call (editor dying):
//     the call surfaces the error; the next call re-probes and likely
//     falls back to commandlet.
//   - Concurrent commandlet daemon + live editor on the same project:
//     auto mode prefers live and the commandlet daemon stays parked;
//     no concurrent writes because every call routes to live until
//     editor closes.

#pragma once

#include "backends/CommandletBlueprintReader.h"
#include "backends/IBlueprintReader.h"
#include "backends/SocketBlueprintReader.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

namespace bpr::backends {

class AutoBlueprintReader : public IBlueprintReader {
public:
	struct Config {
		std::filesystem::path uproject;          // for handshake-file lookup
		std::string liveHost = "127.0.0.1";      // env-override fallback
		int         livePort = 0;                // env-override fallback
		std::string liveToken;                   // env-override fallback
		CommandletBlueprintReader::Config commandletConfig;
		bool prewarmCommandlet = false;
		// Cache the probe decision for this long. Tradeoff: longer →
		// less per-call cost, slower to react to editor open/close;
		// shorter → reactivity at the cost of a TCP connect attempt
		// per call. 2s is a reasonable middle ground for a
		// human-driven workflow.
		std::chrono::milliseconds probeTtl{2000};
		std::chrono::milliseconds probeConnectTimeout{300};
	};

	explicit AutoBlueprintReader(Config cfg);
	~AutoBlueprintReader() override;

	AutoBlueprintReader(const AutoBlueprintReader&) = delete;
	AutoBlueprintReader& operator=(const AutoBlueprintReader&) = delete;

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
	void RetypeVariable(std::string_view assetPath, std::string_view name,
						const BPPinType& newType) override;
	void SetVariableCategory(std::string_view assetPath, std::string_view name,
							 std::string_view category) override;
	DuplicateBlueprintResult DuplicateBlueprint(std::string_view sourceAssetPath,
												std::string_view destAssetPath) override;
	WriteGeneratedSourceResult WriteGeneratedSource(std::string_view destPath,
													std::string_view content,
													bool createDirs) override;
	nlohmann::json StructuralDiff(std::string_view a, std::string_view b,
								   const StructuralDiffOptions& opts) override;

	// ----- batch + meta ---------------------------------------------
	void BeginBatch() override;
	nlohmann::json EndBatch(bool skipCompile = false) override;
	nlohmann::json ShutdownDaemon() override;

	// ----- Project + Asset-registry queries -------------------------
	// Without these explicit overrides the calls would dispatch to
	// IBlueprintReader's throwing defaults — vtable for unoverridden
	// virtuals points at the base class. Forward to Pick() so the
	// active backend (Socket or Commandlet) handles them.
	ProjectMetadata GetProjectMetadata() override;
	AssetRegistryListResult ListAssets(std::string_view path, bool recursive) override;
	AssetRegistryListResult FindAsset(std::string_view query, std::string_view path) override;

	// ----- Phase 8 EA-pull Wave 1 (editor-awareness reads) -----------
	// Same pattern as above: explicit overrides because IBlueprintReader's
	// defaults throw. Live editor exposes them; commandlet returns sane
	// empties for the read-only awareness queries that don't need a UI.
	OpenAssetsResult ListOpenAssets() override;
	ActiveAssetResult GetActiveAsset() override;
	CompileStatusResult GetCompileStatus(std::string_view assetPath) override;
	DirtyPackagesResult GetDirtyPackages() override;
	FocusedWindowResult GetFocusedWindow() override;
	PieStateResult GetPieState() override;
	ModalStateResult GetModalState() override;
	EditorModesResult GetActiveEditorMode() override;
	FocusedWidgetResult GetFocusedWidget() override;
	OpenAssetEditorResult OpenAssetEditor(std::string_view assetPath) override;
	CloseAssetEditorResult CloseAssetEditor(std::string_view assetPath) override;
	CameraTransformResult GetCameraTransform() override;
	ViewModeResult GetViewMode() override;
	ShowFlagsResult GetShowFlags() override;
	SelectedComponentsResult GetSelectedComponents() override;
	ContentBrowserSelectionResult GetSelectedAssets() override;
	ContentBrowserSelectionResult SetSelectedAssets(const std::vector<std::string>& assetPaths) override;
	ContentBrowserFoldersResult GetSelectedFolders() override;
	ContentBrowserPathResult GetContentBrowserPath() override;
	ContentBrowserPathResult SetContentBrowserPath(std::string_view folderPath) override;
	WorldToScreenResult WorldToScreen(double x, double y, double z) override;
	ScreenToWorldResult ScreenToWorld(double x, double y, double maxDist) override;
	UiSnapshotResult UiSnapshot(std::string_view windowFilter, int maxDepth) override;
	UiSnapshotResult UiFind(std::string_view text, std::string_view roleFilter) override;
	DesktopWindowsResult ListDesktopWindows() override;
	GameFeaturesListResult ListGameFeatures() override;
	GameFeatureStateResult GetGameFeatureState(std::string_view pluginName) override;
	PluginListResult ListPlugins() override;
	PluginDescriptorResult GetPluginDescriptor(std::string_view pluginName) override;
	PluginDependenciesResult GetPluginDependencies(std::string_view pluginName) override;
	ActorAbilitiesResult ListActorAbilities(std::string_view actorName) override;
	ActorTagsResult ListActorGameplayTags(std::string_view actorName) override;
	ActorAttributesResult ListActorAttributes(std::string_view actorName) override;
	ActorEffectsResult ListActorGameplayEffects(std::string_view actorName) override;
	BlueprintEditorStateResult GetBlueprintEditorState(std::string_view assetPath) override;
	MaterialInstanceParamsResult GetMaterialInstanceParams(std::string_view assetPath) override;
	StaticMeshInfoResult GetStaticMeshInfo(std::string_view assetPath) override;
	UmgEditorStateResult GetUmgEditorState(std::string_view assetPath) override;
	MaterialEditorStateResult GetMaterialEditorState(std::string_view assetPath) override;
	MeshPreviewStateResult GetMeshPreviewState(std::string_view assetPath) override;

	// Test/diagnostic accessor: which backend would the next call use?
	// Returns "live" or "commandlet". Forces a fresh probe if the
	// cache has expired.
	std::string SelectBackendForTesting();

	// Auto wraps live + commandlet; both implement the full op surface,
	// so neither has unsupported tools to report. Return empty rather
	// than probing — the call should not depend on a live editor.
	std::vector<std::string> UnsupportedTools() const override {
		return {};
	}

private:
	// Probe → returns the reader to route this call to. Caller holds
	// mu_; we may block briefly on a TCP connect.
	IBlueprintReader& Pick();

	// Recompute the routing decision (file read + TCP probe). Caller
	// holds mu_.
	void Probe();

	// Construct a SocketBlueprintReader from current cfg + handshake (or
	// env-var fallbacks). Returns nullptr if nothing's discoverable.
	std::unique_ptr<SocketBlueprintReader> TryBuildLive();

	// Same shape as TryBuildLive but targeting `bp-reader-cmdlet.json`
	// — the handshake file the commandlet daemon publishes when running
	// in TCP mode (Task 2.4). Returns nullptr when the file is absent,
	// malformed, or the TCP listener isn't actually reachable. Lets
	// Auto reuse an already-running daemon instead of spawning a
	// second one through CommandletBlueprintReader.
	std::unique_ptr<SocketBlueprintReader> TryBuildCmdlet();

	// Lazily build the commandlet on first need. Validation is
	// expensive (checks for the editor exe on disk); deferring means a
	// session that lives entirely on Live never pays it.
	CommandletBlueprintReader& EnsureCommandlet();

	// Routing decision: which transport the most recent probe picked.
	enum class Route { Commandlet, Live, CmdletSocket };

	Config cfg_;
	std::mutex mu_;

	// Commandlet reader exists for the lifetime of this object. The
	// ReadOnly / Caching wrappers in BackendFactory wrap *this*; the
	// commandlet inside is the raw reader.
	std::unique_ptr<CommandletBlueprintReader> commandlet_;
	// Live reader is recycled on probe transitions.
	std::unique_ptr<SocketBlueprintReader> live_;
	// Socket reader pointing at the commandlet daemon's TCP listener
	// (bp-reader-cmdlet.json). Same wire shape as live_; semantically
	// routes to the commandlet path. Recycled on probe transitions.
	std::unique_ptr<SocketBlueprintReader> cmdletSocket_;

	// Last successful probe. When (now - lastProbe_) < probeTtl, we
	// reuse `route_` without re-checking.
	std::chrono::steady_clock::time_point lastProbe_{};
	Route route_ = Route::Commandlet;
};

}    // namespace bpr::backends
