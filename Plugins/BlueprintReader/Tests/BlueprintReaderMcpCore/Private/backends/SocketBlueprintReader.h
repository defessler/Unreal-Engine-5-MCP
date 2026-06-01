// SocketBlueprintReader — talks to a running UE editor over TCP instead
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
//    throw too. Caller must construct a new SocketBlueprintReader.

#pragma once

#include "backends/IBlueprintReader.h"

#include <chrono>
#include <functional>
#include <mutex>
#include <string>

namespace bpr::backends {

class SocketBlueprintReader : public IBlueprintReader {
public:
	struct Config {
		std::string host = "127.0.0.1";
		int         port = 0;            // 0 → throw at construct time
		std::string token;               // required (auth)
		std::chrono::seconds connectTimeout{5};
		std::chrono::seconds opTimeout{60};

		// Optional path to `<Project>/Saved/bp-reader-live.json`. When
		// set, the reader will re-read this file on connect failure
		// (ECONNREFUSED — symptom of an editor restart that produced a
		// new port/token). The cached cfg gets updated in-place + the
		// connection is retried once before we surface a failure to
		// the agent. Empty → no re-probe, stale-port failures bubble
		// up as before. The auto backend always sets this; the static
		// `live` backend wires it whenever BackendFactory could find
		// the handshake file at construction time.
		std::string handshakeFilePath;

		// Optional .uproject path. Used by GetProjectMetadata to read
		// project metadata directly from disk (same as
		// CommandletBlueprintReader does) — the editor side doesn't
		// currently implement a GetProjectMetadata op, so going over
		// the wire returns code=1. Reading the file locally is the
		// same data anyway. Empty → GetProjectMetadata returns empty.
		std::string projectPath;
	};

	explicit SocketBlueprintReader(Config cfg);
	~SocketBlueprintReader() override;

	SocketBlueprintReader(const SocketBlueprintReader&) = delete;
	SocketBlueprintReader& operator=(const SocketBlueprintReader&) = delete;

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

	// ----- Asset-registry queries ---------------------------------------
	AssetRegistryListResult ListAssets(std::string_view path, bool recursive) override;
	AssetRegistryListResult FindAsset(std::string_view query, std::string_view path) override;

	// ----- Project + Content Browser ops --------------------------------
	ProjectMetadata GetProjectMetadata() override;
	SaveAllResult SaveAll(bool dirtyOnly) override;
	MoveAssetResult MoveAsset(std::string_view sourcePath,
							  std::string_view destPath) override;
	DeleteAssetResult DeleteAsset(std::string_view assetPath,
								  bool force) override;
	CreateFolderResult CreateFolder(std::string_view folderPath) override;
	std::vector<BPAssetSummary> ListDataTables(std::string_view path) override;
	DataTableInfo ReadDataTable(std::string_view assetPath) override;
	AddDataRowResult AddDataRow(std::string_view assetPath,
								std::string_view rowName,
								const nlohmann::json& values,
								bool overwrite) override;
	SetDataRowValueResult SetDataRowValue(std::string_view assetPath,
										  std::string_view rowName,
										  std::string_view fieldName,
										  std::string_view value) override;
	AddComponentResult AddComponent(std::string_view assetPath,
									std::string_view name,
									std::string_view componentClass,
									std::string_view parentName,
									std::string_view socket) override;
	RemoveComponentResult RemoveComponent(std::string_view assetPath,
										  std::string_view name) override;
	AttachComponentResult AttachComponent(std::string_view assetPath,
										  std::string_view name,
										  std::string_view newParentName,
										  std::string_view socket) override;
	SetComponentPropertyResult SetComponentProperty(std::string_view assetPath,
													std::string_view componentName,
													std::string_view propertyName,
													std::string_view value) override;

	// ----- Live editor ops ----------------------------------------------
	ConsoleCommandResult ConsoleCommand(std::string_view command) override;
	CVarValue GetCVar(std::string_view name) override;
	CVarValue SetCVar(std::string_view name, std::string_view value) override;
	PieResult PieStart(std::string_view mode) override;
	PieResult PieStop() override;

	// ----- Phase 8 EA-pull Wave 1 (partial) -----------------------------
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
	LiveCodingResult LiveCodingCompile() override;
	SelectionResult GetSelectedActors() override;
	BPRJson GetEditorState() override;
	PythonResult RunPythonScript(std::string_view code) override;
	AssetGraphResult GetReferencers(std::string_view assetPath) override;
	AssetGraphResult GetDependencies(std::string_view assetPath) override;
	ConfigReadResult ReadConfigValue(std::string_view section, std::string_view key,
									 std::string_view file) override;
	ConfigWriteResult SetConfigValue(std::string_view section, std::string_view key,
									 std::string_view value, std::string_view file) override;
	BuildLightingResult BuildLighting(std::string_view quality) override;
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

	// ----- Automation tests ---------------------------------------------
	AutomationRunResult RunAutomationTests(std::string_view pattern) override;

	// ----- Material authoring -------------------------------------------
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

	// ----- UMG widget authoring -----------------------------------------
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

	// ----- Stage 3: profile / cook / class info / viewport --------------
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

	// ----- Stage 4: Niagara / Sequencer / GAS / AnimGraph ---------------
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
	void BeginBatch() override;
	nlohmann::json EndBatch(bool skipCompile = false) override;

	// ShutdownDaemon doesn't apply to live mode (the editor runs
	// independently of the MCP server's lifetime). Returns
	// {ok:true, was_running:false}.
	nlohmann::json ShutdownDaemon() override;

	// Public entry-point for clients that already have op-frame args
	// pre-encoded (CommandletBlueprintReader, which now routes its
	// -Op=... requests through SocketBlueprintReader's TCP transport
	// instead of stdin/stdout pipes). Wraps the same RunOp logic that
	// every typed method below uses internally — returns the parsed
	// `json` field from the result frame, throws BlueprintReaderError
	// on wire/protocol/handler failure.
	nlohmann::json RunOpRaw(const std::vector<std::string>& args) {
		return RunOp(args);
	}

	// Mid-op progress sink. When set, a `{"type":"progress", ...}` frame the
	// daemon sends BEFORE the result frame (for long ops — cook/package/
	// automation/lighting) is forwarded here as (current, total, message) so
	// the MCP layer can relay it as notifications/progress. No-op when unset;
	// progress frames are always consumed regardless (the read loop keeps going
	// until the result/error frame).
	using ProgressSink = std::function<void(double current, double total, const std::string& message)>;
	void SetProgressSink(ProgressSink sink) { progressSink_ = std::move(sink); }

private:
	// Send op-args, read result, return parsed `json` field. Throws
	// BlueprintReaderError on any wire/protocol/handler failure.
	nlohmann::json RunOp(const std::vector<std::string>& args);

	// Connect + handshake. Sets handshakeOk_ on success. Called lazily
	// from the first RunOp so construction can complete cleanly even
	// when the editor isn't running yet.
	void EnsureConnected();
	void Disconnect();

	// Re-read the editor's handshake file (`Saved/bp-reader-live.json`)
	// and update cfg_.host/port/token in place if the values changed.
	// Returns true when a real refresh happened — i.e. a retry might
	// now succeed. Used by EnsureConnected to recover from editor
	// restarts without needing to bounce the MCP server (issue #9).
	bool RefreshFromHandshakeFile();

	// One connect + handshake attempt. Returns success/failure plus a
	// hint about whether retrying after a handshake refresh might help.
	// EnsureConnected calls this up to twice — refresh-and-retry covers
	// both "new port" (connect refused) and "same port, new token"
	// (auth_fail) editor-restart cases (issue #9).
	struct AttemptResult {
		bool ok;
		bool retryWorthwhile;
		std::string error;
	};
	AttemptResult TryConnectAndHandshake();

	Config cfg_;
	std::mutex mu_;
	intptr_t   socket_ = -1;  // typed as intptr_t to avoid winsock2.h in header
	bool       handshakeOk_ = false;
	int        nextRequestId_ = 1;
	ProgressSink progressSink_;
};

}    // namespace bpr::backends
