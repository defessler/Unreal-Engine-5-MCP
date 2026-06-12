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

	// ----- batch + meta ---------------------------------------------
	void BeginBatch() override;
	nlohmann::json EndBatch(bool skipCompile = false, bool rollback = false) override;
	nlohmann::json ShutdownDaemon() override;
	HealthResult HealthCheck() override;  // UX-P4a — forwards to live/commandlet
	nlohmann::json DiffAsset(std::string_view a, std::string_view b,
	                         std::string_view depth = "structural") override;
	nlohmann::json PrepareMerge(std::string_view base, std::string_view mine,
	                            std::string_view theirs, std::string_view target = "") override;
	// Exec tool — forward to the active backend. Without this override the
	// call would hit IBlueprintReader's throwing default ("not supported")
	// even though commandlet/live implement it.
	PythonResult RunPythonScript(std::string_view code) override;

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

	// ----- Full editor-action + per-asset-type op surface --------------
	// AutoBlueprintReader is the default backend; an unoverridden virtual
	// dispatches to IBlueprintReader's throwing default ("X not supported
	// by this backend") instead of routing to live/commandlet. These were
	// missing, so material / data-table / widget / actor / console / etc.
	// tools failed on the default backend. Forward them like every other op.
	AddAnimStateResult AddAnimState(std::string_view, std::string_view, std::string_view) override;
	AddBTNodeResult AddBTNode(std::string_view, std::string_view, std::string_view, std::string_view) override;
	AddComponentResult AddComponent(std::string_view assetPath, std::string_view name, std::string_view componentClass, std::string_view parentName, std::string_view socket) override;
	AddDataRowResult AddDataRow(std::string_view assetPath, std::string_view rowName, const nlohmann::json& values, bool overwrite) override;
	AddGameplayTagResult AddGameplayTag(std::string_view, std::string_view) override;
	CreateMaterialResult CreateMaterial(std::string_view) override;
	CreateMaterialInstanceResult CreateMaterialInstance(std::string_view, std::string_view) override;
	AddMaterialExpressionResult AddMaterialExpression(std::string_view, std::string_view, int, int) override;
	AddSequenceTrackResult AddSequenceTrack(std::string_view, std::string_view, std::string_view) override;
	AddStateTreeStateResult AddStateTreeState(std::string_view, std::string_view, std::string_view) override;
	AddWidgetResult AddWidget(std::string_view, std::string_view, std::string_view, std::string_view) override;
	AttachComponentResult AttachComponent(std::string_view assetPath, std::string_view name, std::string_view newParentName, std::string_view socket) override;
	BindWidgetEventResult BindWidgetEvent(std::string_view, std::string_view, std::string_view, std::string_view) override;
	BuildLightingResult BuildLighting(std::string_view quality) override;
	CompileAnimBlueprintResult CompileAnimBlueprint(std::string_view) override;
	CompileBehaviorTreeResult CompileBehaviorTree(std::string_view) override;
	CompileMaterialResult CompileMaterial(std::string_view) override;
	CompileStateTreeResult CompileStateTree(std::string_view) override;
	CompileWidgetBlueprintResult CompileWidgetBlueprint(std::string_view) override;
	ConnectMaterialResult ConnectMaterialExpressions(std::string_view, std::string_view, std::string_view, std::string_view, std::string_view) override;
	ConsoleCommandResult ConsoleCommand(std::string_view command) override;
	CookResult CookContent(std::string_view) override;
	CreateDataAssetResult CreateDataAsset(std::string_view, std::string_view) override;
	CreateFolderResult CreateFolder(std::string_view folderPath) override;
	CreateNiagaraSystemResult CreateNiagaraSystem(std::string_view) override;
	DeleteActorResult DeleteActor(std::string_view actorName) override;
	DeleteAssetResult DeleteAsset(std::string_view assetPath, bool force) override;
	FindClassResult FindClass(std::string_view) override;
	FocusActorResult FocusActor(std::string_view) override;
	CVarValue GetCVar(std::string_view name) override;
	AssetGraphResult GetDependencies(std::string_view assetPath) override;
	BPRJson GetEditorState() override;
	BPRJson ListTimelines(std::string_view a) override;
	BPRJson ReadTimeline(std::string_view a, std::string_view n) override;
	BPRJson ListAnimMontages(std::string_view p) override;
	BPRJson ReadAnimMontage(std::string_view a) override;
	AssetGraphResult GetReferencers(std::string_view assetPath) override;
	SelectionResult GetSelectedActors() override;
	StatGroupResult GetStats(std::string_view) override;
	ClassInfo IntrospectClass(std::string_view) override;
	std::vector<BPAssetSummary> ListAnimBlueprints(std::string_view) override;
	std::vector<BPAssetSummary> ListBehaviorTrees(std::string_view) override;
	std::vector<BPAssetSummary> ListDataAssets(std::string_view) override;
	std::vector<BPAssetSummary> ListDataTables(std::string_view path) override;
	std::vector<ClassFunctionInfo> ListFunctions(std::string_view) override;
	GameplayTagListResult ListGameplayTags(std::string_view) override;
	std::vector<BPAssetSummary> ListLevelSequences(std::string_view) override;
	std::vector<BPAssetSummary> ListMaterials(std::string_view) override;
	std::vector<BPAssetSummary> ListNiagaraSystems(std::string_view) override;
	std::vector<BPAssetSummary> ListStateTrees(std::string_view) override;
	LiveCodingResult LiveCodingCompile() override;
	MoveAssetResult MoveAsset(std::string_view sourcePath, std::string_view destPath) override;
	CookResult PackageProject(std::string_view, std::string_view) override;
	PieResult PieStart(std::string_view mode) override;
	PieResult PieStop() override;
	AbilitySetInfo ReadAbilitySet(std::string_view) override;
	AnimBlueprintInfo ReadAnimBlueprint(std::string_view) override;
	BehaviorTreeInfo ReadBehaviorTree(std::string_view) override;
	ConfigReadResult ReadConfigValue(std::string_view section, std::string_view key, std::string_view file) override;
	DataAssetInfo ReadDataAsset(std::string_view) override;
	DataTableInfo ReadDataTable(std::string_view assetPath) override;
	LevelSequenceInfo ReadLevelSequence(std::string_view) override;
	MaterialInfo ReadMaterial(std::string_view) override;
	NiagaraSystemInfo ReadNiagaraSystem(std::string_view) override;
	OutputLogResult ReadOutputLog(int limit, std::string_view minSeverity) override;
	StateTreeInfo ReadStateTree(std::string_view) override;
	WidgetBlueprintInfo ReadWidgetBlueprint(std::string_view) override;
	RemoveComponentResult RemoveComponent(std::string_view assetPath, std::string_view name) override;
	AutomationRunResult RunAutomationTests(std::string_view pattern) override;
	SaveAllResult SaveAll(bool dirtyOnly) override;
	void SetActorTransform(std::string_view actorName, double locX, double locY, double locZ, double rotPitch, double rotYaw, double rotRoll, double scaleX, double scaleY, double scaleZ) override;
	SetBTNodePropertyResult SetBTNodeProperty(std::string_view, std::string_view, std::string_view, std::string_view) override;
	SetCameraResult SetCameraTransform(double, double, double, double, double, double) override;
	SetComponentPropertyResult SetComponentProperty(std::string_view assetPath, std::string_view componentName, std::string_view propertyName, std::string_view value) override;
	ConfigWriteResult SetConfigValue(std::string_view section, std::string_view key, std::string_view value, std::string_view file) override;
	CVarValue SetCVar(std::string_view name, std::string_view value) override;
	SetDataAssetPropertyResult SetDataAssetProperty(std::string_view, std::string_view, std::string_view) override;
	SetDataRowValueResult SetDataRowValue(std::string_view assetPath, std::string_view rowName, std::string_view fieldName, std::string_view value) override;
	SetMIParameterResult SetMaterialInstanceParameter(std::string_view, std::string_view, std::string_view, std::string_view) override;
	SetMaterialParameterResult SetMaterialParameter(std::string_view, std::string_view, std::string_view) override;
	SetNiagaraParameterResult SetNiagaraParameter(std::string_view, std::string_view, std::string_view) override;
	SelectionResult SetSelection(const std::vector<std::string>& actorNames, bool replace) override;
	SetSequencePlaybackRangeResult SetSequencePlaybackRange(std::string_view, double, double) override;
	SetShowFlagResult SetShowFlag(std::string_view, bool) override;
	SetStateTreeTransitionResult SetStateTreeTransition(std::string_view, std::string_view, std::string_view, std::string_view) override;
	SetWidgetPropertyResult SetWidgetProperty(std::string_view, std::string_view, std::string_view, std::string_view) override;
	SpawnActorResult SpawnActor(std::string_view classPath, double locX, double locY, double locZ, double rotPitch, double rotYaw, double rotRoll, double scaleX, double scaleY, double scaleZ) override;
	StartProfileResult StartProfile(std::string_view) override;
	StopProfileResult StopProfile() override;
	ScreenshotResult TakeScreenshot(std::string_view, int, int) override;
	ViewportScreenshotResult TakeViewportScreenshot(std::string_view) override;

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

	// After a socket route (live editor or commandlet daemon) fails at the
	// transport/auth layer, drop it, switch to the commandlet for a cooldown
	// window, and return that commandlet to retry the call on. Caller holds mu_.
	IBlueprintReader& FallBackToCommandlet(const SocketTransportError& e);

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
