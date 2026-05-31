// CommandletBlueprintReader — drives the BlueprintReader plugin's UE
// commandlet to read blueprint data.
//
// Two modes:
//   * One-shot (default): spawn `UnrealEditor-Cmd.exe -run=BPR
//     -Op=<...>` per tool call. Each call pays the editor cold-start cost
//     (~5–7 s on a Dev box).
//   * Daemon (`Config::useDaemon = true`): spawn the editor once with
//     `-run=BPR -Daemon` and reuse the same process across
//     all calls. The plugin's daemon publishes a TCP listener; this
//     class attaches to (or spawns + attaches to) that listener via
//     `SocketBlueprintReader`, so the heavy startup is paid once and
//     subsequent calls cost only the per-call work (~1 s). Replaces
//     the older stdin/stdout pipe protocol — the daemon owns its own
//     I/O via TCP now (Phase 1 of the multi-session shared-daemon work).
//
// Both modes write JSON payloads to a temp file under %TEMP% for the
// one-shot path so noisy editor log output on stdout doesn't pollute
// it. The daemon path uses the socket's structured frame transport.
//
// Configuration (env, defaults set by ConfigFromEnv):
//   * BP_READER_ENGINE_DIR       — path to the source-built engine
//   * BP_READER_PROJECT          — path to the .uproject
//   * BP_READER_TIMEOUT_SECONDS  — per-call timeout (default 120)
//   * BP_READER_DAEMON           — set to 1/true/yes to enable daemon mode
#pragma once

#include "backends/IBlueprintReader.h"
#include "backends/SocketBlueprintReader.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#if defined(_WIN32)
	#define WIN32_LEAN_AND_MEAN
	#include <windows.h>
#endif    // defined(_WIN32)

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
		std::string editorConfig;                  // "Development" (default) /
												   // "DebugGame" / etc. Picks
												   // which UnrealEditor-Cmd
												   // variant the daemon launches.
		std::string editorExtraArgs;               // appended to commandlet command
												   // line, whitespace-separated
		std::string pluginDenylist;                // comma-separated plugin names;
												   // each becomes -DisablePlugin=<name>
												   // so a known-bad plugin (e.g. one
												   // that crashes in StartupModule)
												   // can be skipped non-interactively
		bool useDaemon = false;

		// Test hook: when set, EnsureDaemonAttached invokes this instead
		// of the built-in Win32 CreateProcessW spawn. Lets the spawn-race
		// unit test (test_daemon_lifecycle.cpp) verify that two-lock
		// coordination collapses concurrent spawn attempts to one without
		// actually launching UnrealEditor-Cmd.exe. Production callers
		// (BackendFactory) leave this empty — the real Win32 path runs.
		std::function<void(const std::filesystem::path& uproject)>
			spawnDaemonHook;
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

	// Batch sentinels (A1) — wraps a write batch so the daemon defers
	// CompileBlueprint+SavePackage to one combined call at EndBatch.
	// EndBatch returns the daemon's flush ack (recompiled list + compile
	// diagnostics, C1).
	void BeginBatch() override;
	nlohmann::json EndBatch(bool skipCompile = false) override;

	// Tear down the daemon (sends QUIT, joins, closes pipes) so the user
	// can release the project lock without restarting the MCP server.
	// The next tool call auto-respawns via the existing fallback path.
	nlohmann::json ShutdownDaemon() override;

	// Spin up the editor daemon now in a background thread. Tool calls
	// that arrive before the daemon's TCP handshake file is published
	// block on the same daemonMutex_ used by EnsureDaemonAttached, so
	// this is racy-safe: a real call either completes the prewarm work
	// itself (if it lost the race) or finds a hot daemon to attach to.
	// No-op if useDaemon is false or the platform doesn't support
	// daemon mode.
	void Prewarm();

private:
	// Dispatches to RunOpOneShot or the daemon socket. Always writes its
	// JSON payload to a temp file under %TEMP% (one-shot path) or
	// routes the op-frame through SocketBlueprintReader (daemon path)
	// and returns the parsed JSON.
	nlohmann::json RunOp(const std::vector<std::wstring>& opArgs);

	nlohmann::json RunOpOneShot(const std::vector<std::wstring>& opArgs);

	// Daemon-attach helpers. Try to attach to an existing daemon via
	// `<Project>/Saved/bp-reader-cmdlet.json`; if none is alive, spawn
	// one and wait for its handshake file. The race-naive Phase 2 path:
	// two MCP servers spawning concurrently will fight over the
	// lifetime lock on the editor side, and the loser will see a clean
	// attach on retry. Two-lock spawn coordination lands in Phase 4.
	std::unique_ptr<SocketBlueprintReader> TryAttachExistingDaemon() const;
	SocketBlueprintReader& EnsureDaemonAttached();
	void PollForHandshake(std::chrono::seconds timeout);

#if defined(_WIN32)
	void SpawnDaemon();   // launches UnrealEditor-Cmd.exe -Daemon, no pipes
	void TerminateDaemon(); // terminates the child we spawned (if any)

	HANDLE daemonProcess_ = nullptr;
#else    // defined(_WIN32)
	void SpawnDaemon();   // throws on non-Windows
	void TerminateDaemon();
#endif    // defined(_WIN32)

	std::mutex daemonMutex_;  // serializes EnsureDaemonAttached + spawn/respawn
	std::thread prewarmThread_;  // joined in destructor
	Config cfg_;
	std::filesystem::path editorCmdExe_;

	// Lazily populated by EnsureDaemonAttached. Cleared on shutdown or
	// when the underlying TCP transport throws (the next call will
	// re-attach or re-spawn).
	std::unique_ptr<SocketBlueprintReader> socket_;
};

}    // namespace bpr::backends
