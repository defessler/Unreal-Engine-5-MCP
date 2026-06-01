// CachingBlueprintReader — decorator around IBlueprintReader that
// memoizes read responses for a configurable TTL and invalidates on
// writes.
//
// Why: AI clients do flurries of reads in a session. A typical "tell me
// about BP_Enemy" question routes through ReadBlueprint -> ListVariables
// -> GetGraph in succession, and the agent often retries with different
// projections. Round-tripping each call to the editor commandlet costs
// 50–500 ms; all of that is duplicate work for the same .uasset.
//
// Cache semantics:
//   - Each read call is keyed by (operation, asset_path, *extras*).
//   - Entries expire after `ttl` (default 30 s).
//   - Any write tool invalidates ALL cached entries for the affected
//     asset_path. ListBlueprints is invalidated by any write because the
//     `modified_iso` summary changes.
//   - Cache is in-memory, per-process, NOT shared across server runs.
//
// Trade-off: TTL is the simplest correct invalidation strategy that
// doesn't require us to know .uasset on-disk paths. For longer cache
// lifetimes you'd want mtime-based invalidation; until users complain
// about staleness, TTL is plenty.
//
// Thread-safety: a single mutex guards the cache. The MCP server
// processes one request at a time, so contention is effectively zero —
// the mutex exists so the decorator stays safe if we ever multiplex.

#pragma once

#include "backends/IBlueprintReader.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>

namespace bpr::backends {

class CachingBlueprintReader : public IBlueprintReader {
public:
	struct Stats {
		std::atomic<std::uint64_t> hits{0};
		std::atomic<std::uint64_t> misses{0};
		std::atomic<std::uint64_t> invalidations{0};
	};

	// TTL is stored in milliseconds internally so tests can use sub-second
	// values without losing precision. Production callers pass `seconds`
	// (from env vars) and rely on the implicit duration conversion.
	//
	// C2: when `projectDir` is non-empty, the cache also stamps each entry
	// with the .uasset file's mtime at insert time and re-checks on lookup.
	// External editor edits to the same asset invalidate the entry even if
	// its TTL hasn't expired. Empty projectDir disables mtime checking.
	CachingBlueprintReader(std::unique_ptr<IBlueprintReader> inner,
						   std::chrono::milliseconds ttl,
						   std::filesystem::path projectDir = {});

	// ----- read tools (cached) -----------------------------------------
	std::vector<BPAssetSummary> ListBlueprints(std::string_view path) override;
	BPMetadata                  ReadBlueprint(std::string_view assetPath) override;
	BPGraph                     GetGraph(std::string_view assetPath, std::string_view graphName) override;
	BPFunction                  GetFunction(std::string_view assetPath, std::string_view fnName) override;
	std::vector<BPVariable>     ListVariables(std::string_view assetPath) override;
	std::vector<BPComponent>    GetComponents(std::string_view assetPath) override;
	std::vector<BPNode>         FindNode(std::string_view assetPath, std::string_view query,
										  std::string_view kind = {}) override;

	// ----- write tools (pass-through + invalidate) ---------------------
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
	// Exec tool — nothing to cache; forward straight to inner.
	PythonResult RunPythonScript(std::string_view code) override;
	// Structural diff is a comparison across two assets — caching the
	// result by a single asset key is awkward and gains little (diff is
	// not a hot loop in practice). Pass through.
	nlohmann::json StructuralDiff(std::string_view a, std::string_view b,
								   const StructuralDiffOptions& opts) override;
	nlohmann::json ReadActorInstance(std::string_view assetPath) override;

	// ----- Asset-registry queries (pass-through) ------------------------
	// Asset registry is hot in the editor; no TTL caching needed.
	AssetRegistryListResult ListAssets(std::string_view, bool) override;
	AssetRegistryListResult FindAsset(std::string_view, std::string_view) override;

	// ----- Project + Content Browser ops (pass-through) -----------------
	// These are project-level rather than per-Blueprint; caching them
	// would add complexity without much value. Save/move/delete also
	// invalidate the global list cache (asset enumeration changes).
	ProjectMetadata GetProjectMetadata() override;
	SaveAllResult SaveAll(bool dirtyOnly) override;
	MoveAssetResult MoveAsset(std::string_view sourcePath,
							  std::string_view destPath) override;
	DeleteAssetResult DeleteAsset(std::string_view assetPath,
								  bool force) override;
	CreateFolderResult CreateFolder(std::string_view folderPath) override;
	std::vector<BPAssetSummary> ListDataTables(std::string_view path) override;
	DataTableInfo ReadDataTable(std::string_view assetPath) override;
	AddDataRowResult AddDataRow(std::string_view, std::string_view,
								const nlohmann::json&, bool) override;
	SetDataRowValueResult SetDataRowValue(std::string_view, std::string_view,
										  std::string_view, std::string_view) override;
	AddComponentResult AddComponent(std::string_view, std::string_view,
									std::string_view, std::string_view,
									std::string_view) override;
	RemoveComponentResult RemoveComponent(std::string_view, std::string_view) override;
	AttachComponentResult AttachComponent(std::string_view, std::string_view,
										  std::string_view, std::string_view) override;
	SetComponentPropertyResult SetComponentProperty(std::string_view, std::string_view,
													std::string_view, std::string_view) override;

	// ----- Live editor ops (pass-through; no caching) -------------------
	// Editor state is too volatile for the TTL cache. CVars change at
	// runtime; selection / actors / log all turn over constantly.
	ConsoleCommandResult ConsoleCommand(std::string_view command) override;
	CVarValue GetCVar(std::string_view name) override;
	CVarValue SetCVar(std::string_view name, std::string_view value) override;
	PieResult PieStart(std::string_view mode) override;
	PieResult PieStop() override;
	LiveCodingResult LiveCodingCompile() override;
	SelectionResult GetSelectedActors() override;
	SelectionResult SetSelection(const std::vector<std::string>& actorNames,
								 bool replace) override;
	SpawnActorResult SpawnActor(std::string_view classPath,
		double locX, double locY, double locZ,
		double rotPitch, double rotYaw, double rotRoll,
		double scaleX, double scaleY, double scaleZ) override;
	void SetActorTransform(std::string_view actorName,
		double locX, double locY, double locZ,
		double rotPitch, double rotYaw, double rotRoll,
		double scaleX, double scaleY, double scaleZ) override;
	DeleteActorResult DeleteActor(std::string_view actorName) override;
	OutputLogResult ReadOutputLog(int limit, std::string_view minSeverity) override;
	AutomationRunResult RunAutomationTests(std::string_view pattern) override;

	// ----- Phase 8 EA-pull Wave 1 (live editor state; uncacheable) -------
	// Volatile snapshots — caching would lie. Pass straight through.
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
	CinematicCameraResult GetCinematicCamera() override;
	SequencerStateResult GetSequencerState(std::string_view assetPath) override;
	AnimEditorStateResult GetAnimEditorState(std::string_view assetPath) override;
	NiagaraModuleSelectionResult GetNiagaraModuleSelection(std::string_view assetPath) override;
	CurveEditorSelectionResult GetCurveEditorSelection(std::string_view assetPath) override;
	BufferVizModeResult GetBufferVisualizationMode() override;
	GizmoStateResult GetGizmoState() override;
	ViewportRealtimeResult GetViewportRealtime() override;
	ViewportCameraSettingsResult GetViewportCameraSettings() override;
	SnappingSettingsResult GetSnappingSettings() override;
	ActiveViewportResult GetActiveViewport() override;
	HiddenActorsResult GetHiddenActors() override;
	VisibleActorsResult GetVisibleActors(std::string_view classFilter, double maxDistanceCm) override;
	SetViewModeResult SetViewMode(std::string_view mode) override;
	SetGizmoModeResult SetGizmoMode(std::string_view mode) override;
	SetViewportRealtimeResult SetViewportRealtime(bool enabled) override;
	SetActorVisibilityResult SetActorVisibility(std::string_view actorName, bool visible) override;
	HiddenLayersResult GetHiddenLayers() override;
	SetLayerVisibilityResult SetLayerVisibility(std::string_view layer, bool visible) override;
	CameraBookmarksResult GetCameraBookmarks() override;
	GotoBookmarkResult GotoCameraBookmark(int slot) override;
	HoverTargetResult GetHoverTarget() override;
	IsolateModeResult GetIsolateMode() override;
	AsyncCompileStateResult GetAsyncCompileState() override;
	ShaderCompileStateResult GetShaderCompileState() override;
	CurrentLevelResult GetCurrentLevel() override;
	LoadedLevelsResult ListLoadedLevels() override;
	SourceControlProviderResult GetSourceControlProvider() override;
	AssetRegistryStateResult GetAssetRegistryState() override;
	DataLayerStatesResult GetDataLayerStates() override;
	AutosaveStatusResult GetAutosaveStatus() override;
	RecoveryStateResult GetRecoveryState() override;
	SourceControlStatusResult GetSourceControlStatus(std::string_view assetPath) override;
	FileLockStatusResult GetFileLockStatus(std::string_view assetPath) override;
	ActiveCultureResult GetActiveCulture() override;
	EditorThemeResult GetEditorTheme() override;
	MonitorInfoResult GetMonitors() override;
	LiveCodingStateResult GetLiveCodingState() override;
	GameFeatureActionResult ActivateGameFeature(std::string_view plugin) override;
	GameFeatureActionResult DeactivateGameFeature(std::string_view plugin) override;
	RecentAssetsResult GetRecentlyOpenedAssets() override;
	DebugInstanceResult GetDebugInstance(std::string_view assetPath) override;
	BreakpointsResult GetBlueprintBreakpoints(std::string_view assetPath) override;
	WatchedPinsResult GetWatchedPins(std::string_view assetPath) override;
	ActiveStatsResult GetActiveStats() override;
	SetPluginEnabledResult SetPluginEnabled(std::string_view pluginName, bool enabled) override;
	StreamingSourcesResult GetStreamingSources() override;
	RecentSavedPackagesResult GetRecentlySavedPackages() override;
	ProjectSettingsResult ListProjectSettings() override;
	ProjectSettingValuesResult GetProjectSettingValues(std::string_view classPath) override;
	SetProjectSettingResult SetProjectSetting(std::string_view classPath, std::string_view property, std::string_view value) override;
	AutomationTestsResult ListAutomationTests() override;
	EditorEventsResult GetEditorEvents() override;
	CookTargetResult GetActiveCookTarget() override;
	WorkspaceLayoutResult GetWorkspaceLayout() override;
	TraceStateResult GetTraceState() override;
	UiStateStubResult GetUiStateStub(std::string_view feature) override;

	// ----- Material authoring (pass-through; ReadMaterial cacheable but
	// not yet wired — punt until staleness is a problem) -----------------
	std::vector<BPAssetSummary> ListMaterials(std::string_view) override;
	MaterialInfo ReadMaterial(std::string_view) override;
	AddMaterialExpressionResult AddMaterialExpression(std::string_view,
		std::string_view, int, int) override;
	ConnectMaterialResult ConnectMaterialExpressions(std::string_view,
		std::string_view, std::string_view,
		std::string_view, std::string_view) override;
	SetMaterialParameterResult SetMaterialParameter(std::string_view,
		std::string_view, std::string_view) override;
	SetMIParameterResult SetMaterialInstanceParameter(std::string_view,
		std::string_view, std::string_view, std::string_view) override;
	CompileMaterialResult CompileMaterial(std::string_view) override;

	// ----- UMG widget authoring (pass-through) --------------------------
	WidgetBlueprintInfo ReadWidgetBlueprint(std::string_view) override;
	AddWidgetResult AddWidget(std::string_view, std::string_view,
		std::string_view, std::string_view) override;
	SetWidgetPropertyResult SetWidgetProperty(std::string_view, std::string_view,
		std::string_view, std::string_view) override;
	BindWidgetEventResult BindWidgetEvent(std::string_view, std::string_view,
		std::string_view, std::string_view) override;
	CompileWidgetBlueprintResult CompileWidgetBlueprint(std::string_view) override;

	// ----- Behavior Tree authoring (Stage 2) ----------------------------
	std::vector<BPAssetSummary> ListBehaviorTrees(std::string_view) override;
	BehaviorTreeInfo ReadBehaviorTree(std::string_view) override;
	AddBTNodeResult AddBTNode(std::string_view, std::string_view,
		std::string_view, std::string_view) override;
	SetBTNodePropertyResult SetBTNodeProperty(std::string_view, std::string_view,
		std::string_view, std::string_view) override;
	CompileBehaviorTreeResult CompileBehaviorTree(std::string_view) override;

	// ----- DataAsset CRUD (Stage 2) -------------------------------------
	std::vector<BPAssetSummary> ListDataAssets(std::string_view) override;
	DataAssetInfo ReadDataAsset(std::string_view) override;
	CreateDataAssetResult CreateDataAsset(std::string_view, std::string_view) override;
	SetDataAssetPropertyResult SetDataAssetProperty(std::string_view,
		std::string_view, std::string_view) override;

	// ----- StateTree authoring (Stage 2) --------------------------------
	std::vector<BPAssetSummary> ListStateTrees(std::string_view) override;
	StateTreeInfo ReadStateTree(std::string_view) override;
	AddStateTreeStateResult AddStateTreeState(std::string_view,
		std::string_view, std::string_view) override;
	SetStateTreeTransitionResult SetStateTreeTransition(std::string_view,
		std::string_view, std::string_view, std::string_view) override;
	CompileStateTreeResult CompileStateTree(std::string_view) override;

	// ----- Stage 3: pass-through (transient editor state, no caching) ---
	StartProfileResult StartProfile(std::string_view) override;
	StopProfileResult StopProfile() override;
	StatGroupResult GetStats(std::string_view) override;
	ScreenshotResult TakeScreenshot(std::string_view, int, int) override;
	CookResult CookContent(std::string_view) override;
	CookResult PackageProject(std::string_view, std::string_view) override;
	ClassInfo IntrospectClass(std::string_view) override;
	FindClassResult FindClass(std::string_view) override;
	std::vector<ClassFunctionInfo> ListFunctions(std::string_view) override;
	FocusActorResult FocusActor(std::string_view) override;
	SetCameraResult SetCameraTransform(double, double, double,
									   double, double, double) override;
	ViewportScreenshotResult TakeViewportScreenshot(std::string_view) override;
	SetShowFlagResult SetShowFlag(std::string_view, bool) override;

	// ----- Stage 4 (pass-through; writes invalidate) -------------------
	std::vector<BPAssetSummary> ListNiagaraSystems(std::string_view) override;
	NiagaraSystemInfo ReadNiagaraSystem(std::string_view) override;
	CreateNiagaraSystemResult CreateNiagaraSystem(std::string_view) override;
	SetNiagaraParameterResult SetNiagaraParameter(std::string_view,
		std::string_view, std::string_view) override;
	std::vector<BPAssetSummary> ListLevelSequences(std::string_view) override;
	LevelSequenceInfo ReadLevelSequence(std::string_view) override;
	AddSequenceTrackResult AddSequenceTrack(std::string_view,
		std::string_view, std::string_view) override;
	SetSequencePlaybackRangeResult SetSequencePlaybackRange(std::string_view,
		double, double) override;
	GameplayTagListResult ListGameplayTags(std::string_view) override;
	AddGameplayTagResult AddGameplayTag(std::string_view, std::string_view) override;
	AbilitySetInfo ReadAbilitySet(std::string_view) override;
	std::vector<BPAssetSummary> ListAnimBlueprints(std::string_view) override;
	AnimBlueprintInfo ReadAnimBlueprint(std::string_view) override;
	AddAnimStateResult AddAnimState(std::string_view, std::string_view,
		std::string_view) override;
	CompileAnimBlueprintResult CompileAnimBlueprint(std::string_view) override;

	// Batch sentinels (A1) — forwards to inner and tracks depth so
	// invalidations triggered by writes during a batch don't drop entries
	// that subsequent ops in the same batch would re-fetch. Flushed by
	// EndBatch's trailing call to InvalidateAsset for each pending entry.
	void BeginBatch() override;
	nlohmann::json EndBatch(bool skipCompile = false) override;
	nlohmann::json ShutdownDaemon() override;

	// Drop everything for `assetPath`, plus the global ListBlueprints
	// cache. Public so callers / tests can force-clear.
	void InvalidateAsset(std::string_view assetPath);
	void InvalidateAll();

	const Stats& GetStats() const { return stats_; }

	// Forward to inner. Caching doesn't change which tools are
	// reachable, only how fast they respond.
	// ----- Editor state / asset-graph / config (previously missing) -----
	// Pass-through forwarders that were absent, so get_editor_state /
	// get_referencers / get_dependencies / read+set_config_value /
	// build_lighting threw "not supported by this backend" on any chain
	// including this decorator.
	BPRJson GetEditorState() override;
	AssetGraphResult GetReferencers(std::string_view) override;
	AssetGraphResult GetDependencies(std::string_view) override;
	ConfigReadResult ReadConfigValue(std::string_view, std::string_view, std::string_view) override;
	ConfigWriteResult SetConfigValue(std::string_view, std::string_view, std::string_view, std::string_view) override;
	BuildLightingResult BuildLighting(std::string_view) override;

	std::vector<std::string> UnsupportedTools() const override {
		return inner_->UnsupportedTools();
	}

private:
	struct Entry {
		std::chrono::steady_clock::time_point inserted;
		// Each key uniquely identifies the operation+args, so the void*
		// payload always has the same concrete type per key. We use
		// shared_ptr<const T> at call sites and static_pointer_cast back
		// out — type-safety is enforced by *call site discipline*, not
		// by the cache itself.
		std::shared_ptr<const void> value;
		// C2: mtime stamp captured at insert. Zero-time means "no source
		// file resolved" (e.g. ListBlueprints across a directory) — those
		// entries skip mtime checks.
		std::filesystem::file_time_type sourceMtime{};
		bool hasMtime = false;
	};

	// Look up (or compute) the entry for `key`. Pass the asset path so
	// the reverse index can be maintained for fast invalidation.
	// `compute` runs OUTSIDE the mutex to keep the editor commandlet's
	// round-trip off the critical section.
	std::shared_ptr<const void> LookupOrCompute(
		const std::string& key, std::string_view assetPath,
		const std::function<std::shared_ptr<const void>()>& compute);

	std::unique_ptr<IBlueprintReader> inner_;
	std::chrono::milliseconds ttl_;
	// C2: project root for resolving /Game/X → <root>/Content/X.uasset.
	// Empty disables mtime checks.
	std::filesystem::path projectDir_;

	mutable std::mutex mu_;
	std::map<std::string, Entry> entries_;
	// assetPath -> set of keys that should be dropped when the asset is
	// mutated. The empty key "" gathers global keys (e.g. ListBlueprints).
	std::map<std::string, std::set<std::string>> byAsset_;

	// Batch state (A1): depth counter so nested batches behave; pending
	// asset invalidations recorded during the batch and flushed at the
	// outermost EndBatch.
	int batchDepth_ = 0;
	std::set<std::string> pendingInvalidations_;
	bool pendingGlobalInvalidation_ = false;

	Stats stats_;
};

// Convenience factory: matches the BackendFactory style. Takes ownership
// of `inner`. If ttl <= 0 returns `inner` unwrapped (caching disabled).
// `projectDir` enables mtime-based cache invalidation (C2) when set.
std::unique_ptr<IBlueprintReader> WrapWithCache(
	std::unique_ptr<IBlueprintReader> inner, std::chrono::seconds ttl,
	std::filesystem::path projectDir = {});

}    // namespace bpr::backends
