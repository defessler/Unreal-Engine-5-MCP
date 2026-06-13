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
	CreateBlueprintResult CreateBlueprint(std::string_view, std::string_view,
										  std::string_view) override;
	CloneGraphResult CloneGraph(std::string_view, std::string_view,
								std::string_view) override;
	void ImplementInterface(std::string_view, std::string_view) override;
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
	// Arbitrary Python has full unreal.* API access and can mutate the
	// project — treated as a write and rejected in read-only mode.
	PythonResult RunPythonScript(std::string_view) override;
	// Structural diff is a read op (no .uasset mutation) — pass through.
	nlohmann::json StructuralDiff(std::string_view a, std::string_view b,
								   const StructuralDiffOptions& opts) override;
	nlohmann::json ReadActorInstance(std::string_view assetPath) override;
	nlohmann::json DescribeK2Node(std::string_view classPath) override;
	nlohmann::json UiListWidgets(int maxDepth, int maxWidgets,
								  std::string_view window,
								  std::string_view type) override;
	nlohmann::json UiClick(std::string_view widgetPath,
							std::string_view expectType,
							std::string_view expectText) override;
	nlohmann::json UiType(std::string_view widgetPath,
						   std::string_view text,
						   std::string_view expectType) override;
	nlohmann::json UiFocusTab(std::string_view tabLabel) override;
	nlohmann::json UiInvokeMenu(std::string_view menu, std::string_view entry) override;

	// ----- Asset-registry queries (reads) -------------------------------
	AssetRegistryListResult ListAssets(std::string_view, bool) override;
	AssetRegistryListResult FindAsset(std::string_view, std::string_view) override;

	// ----- Project + Content Browser ops --------------------------------
	// Reads pass through; writes (save/move/delete/create) throw the
	// read-only error.
	ProjectMetadata GetProjectMetadata() override;
	SaveAllResult SaveAll(bool, std::string_view scope = "touched") override;
	MoveAssetResult MoveAsset(std::string_view, std::string_view) override;
	DeleteAssetResult DeleteAsset(std::string_view, bool) override;
	CreateFolderResult CreateFolder(std::string_view) override;
	std::vector<BPAssetSummary> ListDataTables(std::string_view) override;
	DataTableInfo ReadDataTable(std::string_view) override;
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

	// ----- Phase 8 EA-pull Wave 1 (editor-awareness reads) -------------
	// All read-shaped — no .uasset mutation. open_asset_editor and
	// close_asset_editor manipulate UI window state, not .uasset bytes,
	// so they pass through too.
	OpenAssetsResult ListOpenAssets() override;
	ActiveAssetResult GetActiveAsset() override;
	CompileStatusResult GetCompileStatus(std::string_view) override;
	DirtyPackagesResult GetDirtyPackages() override;
	FocusedWindowResult GetFocusedWindow() override;
	PieStateResult GetPieState() override;
	ModalStateResult GetModalState() override;
	EditorModesResult GetActiveEditorMode() override;
	FocusedWidgetResult GetFocusedWidget() override;
	OpenAssetEditorResult OpenAssetEditor(std::string_view) override;
	CloseAssetEditorResult CloseAssetEditor(std::string_view) override;
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
	// Viewport view-state writes pass through (per-viewport, no dirty);
	// layer/actor visibility reject (level-domain mutation).
	SetViewModeResult SetViewMode(std::string_view mode) override;
	SetGizmoModeResult SetGizmoMode(std::string_view mode) override;
	SetViewportRealtimeResult SetViewportRealtime(bool enabled) override;
	SetActorVisibilityResult SetActorVisibility(std::string_view actorName, bool visible) override;
	HiddenLayersResult GetHiddenLayers() override;
	SetLayerVisibilityResult SetLayerVisibility(std::string_view layer, bool visible) override;
	CameraBookmarksResult GetCameraBookmarks() override;
	GotoBookmarkResult GotoCameraBookmark(int slot) override;  // view-state, allowed
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
	// Writes — reject (mutate runtime game-feature state).
	GameFeatureActionResult ActivateGameFeature(std::string_view plugin) override;
	GameFeatureActionResult DeactivateGameFeature(std::string_view plugin) override;
	RecentAssetsResult GetRecentlyOpenedAssets() override;
	DebugInstanceResult GetDebugInstance(std::string_view assetPath) override;
	BreakpointsResult GetBlueprintBreakpoints(std::string_view assetPath) override;
	WatchedPinsResult GetWatchedPins(std::string_view assetPath) override;
	ActiveStatsResult GetActiveStats() override;
	SetPluginEnabledResult SetPluginEnabled(std::string_view pluginName, bool enabled) override;  // write — reject
	StreamingSourcesResult GetStreamingSources() override;
	RecentSavedPackagesResult GetRecentlySavedPackages() override;
	ProjectSettingsResult ListProjectSettings() override;
	ProjectSettingValuesResult GetProjectSettingValues(std::string_view classPath) override;
	SetProjectSettingResult SetProjectSetting(std::string_view classPath, std::string_view property, std::string_view value) override;  // write — reject
	AutomationTestsResult ListAutomationTests() override;
	EditorEventsResult GetEditorEvents() override;
	CookTargetResult GetActiveCookTarget() override;
	WorkspaceLayoutResult GetWorkspaceLayout() override;
	TraceStateResult GetTraceState() override;
	UiStateStubResult GetUiStateStub(std::string_view feature) override;

	// ----- Material authoring ------------------------------------------
	// Reads (list/read) pass through; writes (add expression, connect,
	// set parameter, compile) throw the read-only error.
	CreateMaterialResult CreateMaterial(std::string_view) override;
	CreateMaterialInstanceResult CreateMaterialInstance(std::string_view,
		std::string_view) override;
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

	// ----- UMG widget authoring ----------------------------------------
	WidgetBlueprintInfo ReadWidgetBlueprint(std::string_view) override;
	AddWidgetResult AddWidget(std::string_view, std::string_view,
		std::string_view, std::string_view) override;
	SetWidgetPropertyResult SetWidgetProperty(std::string_view, std::string_view,
		std::string_view, std::string_view) override;
	BindWidgetEventResult BindWidgetEvent(std::string_view, std::string_view,
		std::string_view, std::string_view) override;
	CompileWidgetBlueprintResult CompileWidgetBlueprint(std::string_view) override;

	// ----- Behavior Tree (Stage 2) -------------------------------------
	std::vector<BPAssetSummary> ListBehaviorTrees(std::string_view) override;
	BehaviorTreeInfo ReadBehaviorTree(std::string_view) override;
	AddBTNodeResult AddBTNode(std::string_view, std::string_view,
		std::string_view, std::string_view) override;
	SetBTNodePropertyResult SetBTNodeProperty(std::string_view, std::string_view,
		std::string_view, std::string_view) override;
	CompileBehaviorTreeResult CompileBehaviorTree(std::string_view) override;

	// ----- DataAsset (Stage 2) -----------------------------------------
	std::vector<BPAssetSummary> ListDataAssets(std::string_view) override;
	DataAssetInfo ReadDataAsset(std::string_view) override;
	CreateDataAssetResult CreateDataAsset(std::string_view, std::string_view) override;
	SetDataAssetPropertyResult SetDataAssetProperty(std::string_view,
		std::string_view, std::string_view) override;

	// ----- StateTree (Stage 2) -----------------------------------------
	std::vector<BPAssetSummary> ListStateTrees(std::string_view) override;
	StateTreeInfo ReadStateTree(std::string_view) override;
	AddStateTreeStateResult AddStateTreeState(std::string_view,
		std::string_view, std::string_view) override;
	SetStateTreeTransitionResult SetStateTreeTransition(std::string_view,
		std::string_view, std::string_view, std::string_view) override;
	CompileStateTreeResult CompileStateTree(std::string_view) override;

	// ----- Stage 3 ----------------------------------------------------
	// Profile + stats + screenshots + viewport reads pass through —
	// they don't mutate .uasset files. Cook / package + camera write +
	// show flag are arguable; treat as read-shaped diagnostics (they
	// produce artifacts, not .uasset edits). Only writes that touch
	// the project tree (none here) would reject.
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

	// ----- Stage 4: reads pass through, writes reject ------------------
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

	// ----- batch sentinels ------------------------------------------
	// BeginBatch / EndBatch are technically not writes themselves, but in
	// read-only mode they're still no-ops because no writes can happen
	// inside a batch. Pass-through to the inner so apply_ops with all-read
	// ops still works correctly (preview-style batches).
	void BeginBatch() override;
	nlohmann::json EndBatch(bool skipCompile = false, bool rollback = false,
							bool saveOnError = false) override;
	// ShutdownDaemon is allowed in read-only mode — releasing the daemon
	// is the whole reason this mode exists ("let me work in the editor
	// without daemon contention").
	nlohmann::json ShutdownDaemon() override;
	// UX-P4a: health is a read — pass through, never Reject().
	HealthResult HealthCheck() override;
	nlohmann::json DiffAsset(std::string_view a, std::string_view b,
	                         std::string_view depth = "structural") override;
	nlohmann::json PrepareMerge(std::string_view base, std::string_view mine,
	                            std::string_view theirs, std::string_view target = "") override;

	// Forward to inner. The read-only decorator doesn't change what the
	// underlying backend can or can't do — it just rejects writes — so
	// the unsupported-tools list is inner's responsibility. Without
	// this forward, a ReadOnly(Mock) chain would advertise the mock's
	// long list of unsupported tools as if they worked.
	// ----- Editor state / asset-graph / config (previously missing) -----
	// Reads pass through; set_config_value rejects (persistent settings
	// write, like set_cvar); build_lighting passes through (build artifact,
	// not a .uasset edit — matches cook/package).
	BPRJson GetEditorState() override;
	BPRJson ListTimelines(std::string_view a) override;
	BPRJson ReadTimeline(std::string_view a, std::string_view n) override;
	BPRJson ListAnimMontages(std::string_view p) override;
	BPRJson ReadAnimMontage(std::string_view a) override;
	AssetGraphResult GetReferencers(std::string_view) override;
	AssetGraphResult GetDependencies(std::string_view) override;
	ConfigReadResult ReadConfigValue(std::string_view, std::string_view, std::string_view) override;
	ConfigWriteResult SetConfigValue(std::string_view, std::string_view, std::string_view, std::string_view) override;
	BuildLightingResult BuildLighting(std::string_view) override;

	std::vector<std::string> UnsupportedTools() const override {
		return inner_->UnsupportedTools();
	}

private:
	std::unique_ptr<IBlueprintReader> inner_;
};

// Factory: wraps `inner` if `readOnly`, otherwise returns `inner` unchanged.
std::unique_ptr<IBlueprintReader> MaybeWrapReadOnly(
	std::unique_ptr<IBlueprintReader> inner, bool readOnly);

}    // namespace bpr::backends
