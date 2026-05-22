#include "backends/ReadOnlyBlueprintReader.h"

#include <stdexcept>

namespace bpr::backends {

namespace read_only_blueprint_reader_detail {

// One canonical message — keeps every write op's error identical so the
// agent can pattern-match it once. Calls out the exact env var so the
// user knows what to set/unset.
[[noreturn]] void Reject(const char* op) {
	throw BlueprintReaderError(
		std::string("write tool '") + op +
		"' is disabled: this MCP server is running in read-only mode "
		"(BP_READER_READ_ONLY=1). This mode is intended for coexistence "
		"with an open UE editor — two processes mutating the same .uasset "
		"concurrently corrupts state. To make changes, edit in the editor "
		"directly, or unset BP_READER_READ_ONLY and restart the MCP server.");
}

}    // namespace read_only_blueprint_reader_detail
using namespace read_only_blueprint_reader_detail;

ReadOnlyBlueprintReader::ReadOnlyBlueprintReader(std::unique_ptr<IBlueprintReader> inner)
	: inner_(std::move(inner)) {}

// ----- reads -------------------------------------------------------------
std::vector<BPAssetSummary>
ReadOnlyBlueprintReader::ListBlueprints(std::string_view p) {
	return inner_->ListBlueprints(p);
}
BPMetadata ReadOnlyBlueprintReader::ReadBlueprint(std::string_view a) {
	return inner_->ReadBlueprint(a);
}
BPGraph ReadOnlyBlueprintReader::GetGraph(std::string_view a, std::string_view g) {
	return inner_->GetGraph(a, g);
}
BPFunction ReadOnlyBlueprintReader::GetFunction(std::string_view a, std::string_view f) {
	return inner_->GetFunction(a, f);
}
std::vector<BPVariable> ReadOnlyBlueprintReader::ListVariables(std::string_view a) {
	return inner_->ListVariables(a);
}
std::vector<BPComponent> ReadOnlyBlueprintReader::GetComponents(std::string_view a) {
	return inner_->GetComponents(a);
}
std::vector<BPNode> ReadOnlyBlueprintReader::FindNode(std::string_view a,
													  std::string_view q,
													  std::string_view k) {
	return inner_->FindNode(a, q, k);
}

// ----- writes (all reject) -----------------------------------------------
void ReadOnlyBlueprintReader::AddVariable(std::string_view, std::string_view,
										  const BPPinType&, std::string_view,
										  std::string_view, bool, bool) {
	Reject("add_variable");
}
void ReadOnlyBlueprintReader::SetNodePosition(std::string_view, std::string_view,
											  std::string_view, int, int) {
	Reject("set_node_position");
}
void ReadOnlyBlueprintReader::DeleteNode(std::string_view, std::string_view,
										 std::string_view) {
	Reject("delete_node");
}
std::string ReadOnlyBlueprintReader::AddNode(std::string_view, std::string_view,
											 std::string_view, int, int,
											 const std::map<std::string, std::string, std::less<>>&) {
	Reject("add_node");
}
void ReadOnlyBlueprintReader::WirePins(std::string_view, std::string_view,
									   std::string_view, std::string_view,
									   std::string_view, std::string_view) {
	Reject("wire_pins");
}
void ReadOnlyBlueprintReader::DeleteVariable(std::string_view, std::string_view) {
	Reject("delete_variable");
}
void ReadOnlyBlueprintReader::RenameVariable(std::string_view, std::string_view,
											 std::string_view) {
	Reject("rename_variable");
}
IBlueprintReader::AddFunctionResult
ReadOnlyBlueprintReader::AddFunction(std::string_view, std::string_view) {
	Reject("add_function");
}
void ReadOnlyBlueprintReader::AddFunctionInput(std::string_view, std::string_view,
											   std::string_view, const BPPinType&) {
	Reject("add_function_input");
}
void ReadOnlyBlueprintReader::AddFunctionOutput(std::string_view, std::string_view,
												std::string_view, const BPPinType&) {
	Reject("add_function_output");
}
void ReadOnlyBlueprintReader::DeleteFunction(std::string_view, std::string_view) {
	Reject("delete_function");
}
void ReadOnlyBlueprintReader::SetVariableDefault(std::string_view, std::string_view,
												 std::string_view) {
	Reject("set_variable_default");
}
IBlueprintReader::CreateBlueprintResult
ReadOnlyBlueprintReader::CreateBlueprint(std::string_view, std::string_view) {
	Reject("create_blueprint");
}
void ReadOnlyBlueprintReader::SetPinDefault(std::string_view, std::string_view,
											std::string_view, std::string_view,
											std::string_view) {
	Reject("set_pin_default");
}
void ReadOnlyBlueprintReader::RetypeVariable(std::string_view, std::string_view,
											 const BPPinType&) {
	Reject("retype_variable");
}
void ReadOnlyBlueprintReader::SetVariableCategory(std::string_view, std::string_view,
												  std::string_view) {
	Reject("set_variable_category");
}
IBlueprintReader::DuplicateBlueprintResult
ReadOnlyBlueprintReader::DuplicateBlueprint(std::string_view, std::string_view) {
	Reject("duplicate_blueprint");
}
IBlueprintReader::WriteGeneratedSourceResult
ReadOnlyBlueprintReader::WriteGeneratedSource(std::string_view, std::string_view, bool) {
	// Writing source files isn't strictly a BP-graph mutation, but it
	// does modify the project tree — and read-only mode's whole point
	// is "don't touch anything." Reject for consistency with the
	// BP-mutation tools.
	Reject("write_generated_source");
}

nlohmann::json ReadOnlyBlueprintReader::StructuralDiff(
	std::string_view a, std::string_view b, const StructuralDiffOptions& opts) {
	// Diff is a read op — passes through.
	return inner_->StructuralDiff(a, b, opts);
}

// ----- batch sentinels ---------------------------------------------------
// Pass through. apply_ops calls these unconditionally; in read-only mode
// no individual op will mutate, so EndBatch's compile/save loop has
// nothing to do — the wrapped inner handles that gracefully.
void ReadOnlyBlueprintReader::BeginBatch() {
	inner_->BeginBatch();
}
nlohmann::json ReadOnlyBlueprintReader::EndBatch(bool skipCompile) {
	return inner_->EndBatch(skipCompile);
}

nlohmann::json ReadOnlyBlueprintReader::ShutdownDaemon() {
	return inner_->ShutdownDaemon();
}

// ----- Asset-registry queries --------------------------------------------

IBlueprintReader::AssetRegistryListResult
ReadOnlyBlueprintReader::ListAssets(std::string_view path, bool recursive) {
	return inner_->ListAssets(path, recursive);
}

IBlueprintReader::AssetRegistryListResult
ReadOnlyBlueprintReader::FindAsset(std::string_view query, std::string_view path) {
	return inner_->FindAsset(query, path);
}

// ----- Project + Content Browser ops -------------------------------------

IBlueprintReader::ProjectMetadata
ReadOnlyBlueprintReader::GetProjectMetadata() {
	return inner_->GetProjectMetadata();
}

IBlueprintReader::SaveAllResult ReadOnlyBlueprintReader::SaveAll(bool) {
	Reject("save_all");
}

IBlueprintReader::MoveAssetResult
ReadOnlyBlueprintReader::MoveAsset(std::string_view, std::string_view) {
	Reject("move_asset");
}

IBlueprintReader::DeleteAssetResult
ReadOnlyBlueprintReader::DeleteAsset(std::string_view, bool) {
	Reject("delete_asset");
}

IBlueprintReader::CreateFolderResult
ReadOnlyBlueprintReader::CreateFolder(std::string_view) {
	Reject("create_folder");
}

std::vector<BPAssetSummary>
ReadOnlyBlueprintReader::ListDataTables(std::string_view path) {
	return inner_->ListDataTables(path);
}

IBlueprintReader::DataTableInfo
ReadOnlyBlueprintReader::ReadDataTable(std::string_view assetPath) {
	return inner_->ReadDataTable(assetPath);
}

IBlueprintReader::AddDataRowResult
ReadOnlyBlueprintReader::AddDataRow(std::string_view, std::string_view,
									const nlohmann::json&, bool) {
	Reject("add_data_row");
}
IBlueprintReader::SetDataRowValueResult
ReadOnlyBlueprintReader::SetDataRowValue(std::string_view, std::string_view,
										 std::string_view, std::string_view) {
	Reject("set_data_row_value");
}

IBlueprintReader::AddComponentResult
ReadOnlyBlueprintReader::AddComponent(std::string_view, std::string_view,
									  std::string_view, std::string_view,
									  std::string_view) {
	Reject("add_component");
}
IBlueprintReader::RemoveComponentResult
ReadOnlyBlueprintReader::RemoveComponent(std::string_view, std::string_view) {
	Reject("remove_component");
}
IBlueprintReader::AttachComponentResult
ReadOnlyBlueprintReader::AttachComponent(std::string_view, std::string_view,
										 std::string_view, std::string_view) {
	Reject("attach_component");
}
IBlueprintReader::SetComponentPropertyResult
ReadOnlyBlueprintReader::SetComponentProperty(std::string_view, std::string_view,
											  std::string_view, std::string_view) {
	Reject("set_component_property");
}

// ----- Live editor ops ----------------------------------------------------

// Reads through.
IBlueprintReader::ConsoleCommandResult
ReadOnlyBlueprintReader::ConsoleCommand(std::string_view c) {
	return inner_->ConsoleCommand(c);
}
IBlueprintReader::CVarValue
ReadOnlyBlueprintReader::GetCVar(std::string_view n) { return inner_->GetCVar(n); }
IBlueprintReader::SelectionResult
ReadOnlyBlueprintReader::GetSelectedActors() { return inner_->GetSelectedActors(); }
IBlueprintReader::OutputLogResult
ReadOnlyBlueprintReader::ReadOutputLog(int limit, std::string_view minSev) {
	return inner_->ReadOutputLog(limit, minSev);
}
IBlueprintReader::PieResult
ReadOnlyBlueprintReader::PieStart(std::string_view m) { return inner_->PieStart(m); }
IBlueprintReader::LiveCodingResult
ReadOnlyBlueprintReader::LiveCodingCompile() { return inner_->LiveCodingCompile(); }

// Writes reject.
IBlueprintReader::CVarValue
ReadOnlyBlueprintReader::SetCVar(std::string_view, std::string_view) { Reject("set_cvar"); }
IBlueprintReader::SelectionResult
ReadOnlyBlueprintReader::SetSelection(const std::vector<std::string>&, bool) { Reject("set_selection"); }
IBlueprintReader::SpawnActorResult
ReadOnlyBlueprintReader::SpawnActor(std::string_view,
	double, double, double, double, double, double,
	double, double, double) { Reject("spawn_actor"); }
void ReadOnlyBlueprintReader::SetActorTransform(std::string_view,
	double, double, double, double, double, double,
	double, double, double) { Reject("set_actor_transform"); }
IBlueprintReader::DeleteActorResult
ReadOnlyBlueprintReader::DeleteActor(std::string_view) { Reject("delete_actor"); }
IBlueprintReader::PieResult ReadOnlyBlueprintReader::PieStop() { Reject("pie_stop"); }

IBlueprintReader::AutomationRunResult
ReadOnlyBlueprintReader::RunAutomationTests(std::string_view pattern) {
	return inner_->RunAutomationTests(pattern);
}

// ----- Phase 8 EA-pull Wave 1 (all pass through; no .uasset mutation) ----

IBlueprintReader::OpenAssetsResult ReadOnlyBlueprintReader::ListOpenAssets() {
	return inner_->ListOpenAssets();
}
IBlueprintReader::ActiveAssetResult ReadOnlyBlueprintReader::GetActiveAsset() {
	return inner_->GetActiveAsset();
}
IBlueprintReader::CompileStatusResult
ReadOnlyBlueprintReader::GetCompileStatus(std::string_view a) {
	return inner_->GetCompileStatus(a);
}
IBlueprintReader::DirtyPackagesResult ReadOnlyBlueprintReader::GetDirtyPackages() {
	return inner_->GetDirtyPackages();
}
IBlueprintReader::FocusedWindowResult ReadOnlyBlueprintReader::GetFocusedWindow() {
	return inner_->GetFocusedWindow();
}
IBlueprintReader::PieStateResult ReadOnlyBlueprintReader::GetPieState() {
	return inner_->GetPieState();
}
IBlueprintReader::ModalStateResult ReadOnlyBlueprintReader::GetModalState() {
	return inner_->GetModalState();
}
IBlueprintReader::EditorModesResult ReadOnlyBlueprintReader::GetActiveEditorMode() {
	return inner_->GetActiveEditorMode();
}
IBlueprintReader::FocusedWidgetResult ReadOnlyBlueprintReader::GetFocusedWidget() {
	return inner_->GetFocusedWidget();
}
IBlueprintReader::OpenAssetEditorResult
ReadOnlyBlueprintReader::OpenAssetEditor(std::string_view a) {
	return inner_->OpenAssetEditor(a);
}
IBlueprintReader::CloseAssetEditorResult
ReadOnlyBlueprintReader::CloseAssetEditor(std::string_view a) {
	return inner_->CloseAssetEditor(a);
}
IBlueprintReader::CameraTransformResult ReadOnlyBlueprintReader::GetCameraTransform() {
	return inner_->GetCameraTransform();
}
IBlueprintReader::ViewModeResult ReadOnlyBlueprintReader::GetViewMode() {
	return inner_->GetViewMode();
}
IBlueprintReader::ShowFlagsResult ReadOnlyBlueprintReader::GetShowFlags() {
	return inner_->GetShowFlags();
}
IBlueprintReader::SelectedComponentsResult ReadOnlyBlueprintReader::GetSelectedComponents() {
	return inner_->GetSelectedComponents();
}
IBlueprintReader::ContentBrowserSelectionResult ReadOnlyBlueprintReader::GetSelectedAssets() {
	return inner_->GetSelectedAssets();
}
IBlueprintReader::ContentBrowserSelectionResult
ReadOnlyBlueprintReader::SetSelectedAssets(const std::vector<std::string>& a) {
	// Content browser selection is UI state, not .uasset mutation —
	// pass through even in read-only mode.
	return inner_->SetSelectedAssets(a);
}
IBlueprintReader::ContentBrowserFoldersResult ReadOnlyBlueprintReader::GetSelectedFolders() {
	return inner_->GetSelectedFolders();
}
IBlueprintReader::ContentBrowserPathResult ReadOnlyBlueprintReader::GetContentBrowserPath() {
	return inner_->GetContentBrowserPath();
}
IBlueprintReader::ContentBrowserPathResult
ReadOnlyBlueprintReader::SetContentBrowserPath(std::string_view p) {
	// UI navigation, not file mutation — allowed.
	return inner_->SetContentBrowserPath(p);
}
IBlueprintReader::WorldToScreenResult
ReadOnlyBlueprintReader::WorldToScreen(double x, double y, double z) {
	return inner_->WorldToScreen(x, y, z);
}
IBlueprintReader::ScreenToWorldResult
ReadOnlyBlueprintReader::ScreenToWorld(double x, double y, double d) {
	return inner_->ScreenToWorld(x, y, d);
}
IBlueprintReader::UiSnapshotResult
ReadOnlyBlueprintReader::UiSnapshot(std::string_view w, int d) {
	return inner_->UiSnapshot(w, d);
}
IBlueprintReader::UiSnapshotResult
ReadOnlyBlueprintReader::UiFind(std::string_view t, std::string_view r) {
	return inner_->UiFind(t, r);
}
IBlueprintReader::DesktopWindowsResult ReadOnlyBlueprintReader::ListDesktopWindows() {
	return inner_->ListDesktopWindows();
}
IBlueprintReader::GameFeaturesListResult ReadOnlyBlueprintReader::ListGameFeatures() {
	return inner_->ListGameFeatures();
}
IBlueprintReader::GameFeatureStateResult
ReadOnlyBlueprintReader::GetGameFeatureState(std::string_view p) {
	return inner_->GetGameFeatureState(p);
}
IBlueprintReader::PluginListResult ReadOnlyBlueprintReader::ListPlugins() {
	return inner_->ListPlugins();
}
IBlueprintReader::PluginDescriptorResult
ReadOnlyBlueprintReader::GetPluginDescriptor(std::string_view p) {
	return inner_->GetPluginDescriptor(p);
}
IBlueprintReader::PluginDependenciesResult
ReadOnlyBlueprintReader::GetPluginDependencies(std::string_view p) {
	return inner_->GetPluginDependencies(p);
}
IBlueprintReader::ActorAbilitiesResult
ReadOnlyBlueprintReader::ListActorAbilities(std::string_view a) {
	return inner_->ListActorAbilities(a);
}
IBlueprintReader::ActorTagsResult
ReadOnlyBlueprintReader::ListActorGameplayTags(std::string_view a) {
	return inner_->ListActorGameplayTags(a);
}
IBlueprintReader::ActorAttributesResult
ReadOnlyBlueprintReader::ListActorAttributes(std::string_view a) {
	return inner_->ListActorAttributes(a);
}
IBlueprintReader::ActorEffectsResult
ReadOnlyBlueprintReader::ListActorGameplayEffects(std::string_view a) {
	return inner_->ListActorGameplayEffects(a);
}
IBlueprintReader::BlueprintEditorStateResult
ReadOnlyBlueprintReader::GetBlueprintEditorState(std::string_view a) {
	return inner_->GetBlueprintEditorState(a);
}
IBlueprintReader::MaterialInstanceParamsResult
ReadOnlyBlueprintReader::GetMaterialInstanceParams(std::string_view a) {
	return inner_->GetMaterialInstanceParams(a);
}

// ----- Material authoring (reads pass through, writes reject) -----------

std::vector<BPAssetSummary>
ReadOnlyBlueprintReader::ListMaterials(std::string_view p) {
	return inner_->ListMaterials(p);
}
IBlueprintReader::MaterialInfo
ReadOnlyBlueprintReader::ReadMaterial(std::string_view a) {
	return inner_->ReadMaterial(a);
}
IBlueprintReader::AddMaterialExpressionResult
ReadOnlyBlueprintReader::AddMaterialExpression(std::string_view,
	std::string_view, int, int) {
	Reject("add_material_expression");
}
IBlueprintReader::ConnectMaterialResult
ReadOnlyBlueprintReader::ConnectMaterialExpressions(std::string_view,
	std::string_view, std::string_view, std::string_view, std::string_view) {
	Reject("connect_material_expressions");
}
IBlueprintReader::SetMaterialParameterResult
ReadOnlyBlueprintReader::SetMaterialParameter(std::string_view,
	std::string_view, std::string_view) {
	Reject("set_material_parameter");
}
IBlueprintReader::SetMIParameterResult
ReadOnlyBlueprintReader::SetMaterialInstanceParameter(std::string_view,
	std::string_view, std::string_view, std::string_view) {
	Reject("set_material_instance_parameter");
}
IBlueprintReader::CompileMaterialResult
ReadOnlyBlueprintReader::CompileMaterial(std::string_view) {
	Reject("compile_material");
}

// ----- UMG widget authoring (read passes through, writes reject) --------

IBlueprintReader::WidgetBlueprintInfo
ReadOnlyBlueprintReader::ReadWidgetBlueprint(std::string_view a) {
	return inner_->ReadWidgetBlueprint(a);
}
IBlueprintReader::AddWidgetResult
ReadOnlyBlueprintReader::AddWidget(std::string_view, std::string_view,
	std::string_view, std::string_view) {
	Reject("add_widget");
}
IBlueprintReader::SetWidgetPropertyResult
ReadOnlyBlueprintReader::SetWidgetProperty(std::string_view, std::string_view,
	std::string_view, std::string_view) {
	Reject("set_widget_property");
}
IBlueprintReader::BindWidgetEventResult
ReadOnlyBlueprintReader::BindWidgetEvent(std::string_view, std::string_view,
	std::string_view, std::string_view) {
	Reject("bind_widget_event");
}
IBlueprintReader::CompileWidgetBlueprintResult
ReadOnlyBlueprintReader::CompileWidgetBlueprint(std::string_view) {
	Reject("compile_widget_blueprint");
}

// ----- Behavior Tree (reads pass through, writes reject) ---------------

std::vector<BPAssetSummary>
ReadOnlyBlueprintReader::ListBehaviorTrees(std::string_view p) {
	return inner_->ListBehaviorTrees(p);
}
IBlueprintReader::BehaviorTreeInfo
ReadOnlyBlueprintReader::ReadBehaviorTree(std::string_view a) {
	return inner_->ReadBehaviorTree(a);
}
IBlueprintReader::AddBTNodeResult
ReadOnlyBlueprintReader::AddBTNode(std::string_view, std::string_view,
	std::string_view, std::string_view) {
	Reject("add_bt_node");
}
IBlueprintReader::SetBTNodePropertyResult
ReadOnlyBlueprintReader::SetBTNodeProperty(std::string_view,
	std::string_view, std::string_view, std::string_view) {
	Reject("set_bt_node_property");
}
IBlueprintReader::CompileBehaviorTreeResult
ReadOnlyBlueprintReader::CompileBehaviorTree(std::string_view) {
	Reject("compile_behavior_tree");
}

// ----- DataAsset (reads pass through, writes reject) -------------------

std::vector<BPAssetSummary>
ReadOnlyBlueprintReader::ListDataAssets(std::string_view p) {
	return inner_->ListDataAssets(p);
}
IBlueprintReader::DataAssetInfo
ReadOnlyBlueprintReader::ReadDataAsset(std::string_view a) {
	return inner_->ReadDataAsset(a);
}
IBlueprintReader::CreateDataAssetResult
ReadOnlyBlueprintReader::CreateDataAsset(std::string_view, std::string_view) {
	Reject("create_data_asset");
}
IBlueprintReader::SetDataAssetPropertyResult
ReadOnlyBlueprintReader::SetDataAssetProperty(std::string_view,
	std::string_view, std::string_view) {
	Reject("set_data_asset_property");
}

// ----- StateTree (reads pass through, writes reject) -------------------

std::vector<BPAssetSummary>
ReadOnlyBlueprintReader::ListStateTrees(std::string_view p) {
	return inner_->ListStateTrees(p);
}
IBlueprintReader::StateTreeInfo
ReadOnlyBlueprintReader::ReadStateTree(std::string_view a) {
	return inner_->ReadStateTree(a);
}
IBlueprintReader::AddStateTreeStateResult
ReadOnlyBlueprintReader::AddStateTreeState(std::string_view,
	std::string_view, std::string_view) {
	Reject("add_state_tree_state");
}
IBlueprintReader::SetStateTreeTransitionResult
ReadOnlyBlueprintReader::SetStateTreeTransition(std::string_view,
	std::string_view, std::string_view, std::string_view) {
	Reject("set_state_tree_transition");
}
IBlueprintReader::CompileStateTreeResult
ReadOnlyBlueprintReader::CompileStateTree(std::string_view) {
	Reject("compile_state_tree");
}

// ----- Stage 3 (all read-shaped diagnostics; pass through) -------------

IBlueprintReader::StartProfileResult
ReadOnlyBlueprintReader::StartProfile(std::string_view m) { return inner_->StartProfile(m); }
IBlueprintReader::StopProfileResult
ReadOnlyBlueprintReader::StopProfile() { return inner_->StopProfile(); }
IBlueprintReader::StatGroupResult
ReadOnlyBlueprintReader::GetStats(std::string_view g) { return inner_->GetStats(g); }
IBlueprintReader::ScreenshotResult
ReadOnlyBlueprintReader::TakeScreenshot(std::string_view d, int w, int h) {
	return inner_->TakeScreenshot(d, w, h);
}
IBlueprintReader::CookResult
ReadOnlyBlueprintReader::CookContent(std::string_view p) { return inner_->CookContent(p); }
IBlueprintReader::CookResult
ReadOnlyBlueprintReader::PackageProject(std::string_view p, std::string_view o) {
	return inner_->PackageProject(p, o);
}
IBlueprintReader::ClassInfo
ReadOnlyBlueprintReader::IntrospectClass(std::string_view c) { return inner_->IntrospectClass(c); }
IBlueprintReader::FindClassResult
ReadOnlyBlueprintReader::FindClass(std::string_view q) { return inner_->FindClass(q); }
std::vector<IBlueprintReader::ClassFunctionInfo>
ReadOnlyBlueprintReader::ListFunctions(std::string_view c) { return inner_->ListFunctions(c); }
IBlueprintReader::FocusActorResult
ReadOnlyBlueprintReader::FocusActor(std::string_view a) { return inner_->FocusActor(a); }
IBlueprintReader::SetCameraResult
ReadOnlyBlueprintReader::SetCameraTransform(double lx, double ly, double lz,
	double rp, double ry, double rr) {
	return inner_->SetCameraTransform(lx, ly, lz, rp, ry, rr);
}
IBlueprintReader::ViewportScreenshotResult
ReadOnlyBlueprintReader::TakeViewportScreenshot(std::string_view d) {
	return inner_->TakeViewportScreenshot(d);
}
IBlueprintReader::SetShowFlagResult
ReadOnlyBlueprintReader::SetShowFlag(std::string_view f, bool e) {
	return inner_->SetShowFlag(f, e);
}

// ----- Stage 4 (reads pass through, writes reject) ---------------------

std::vector<BPAssetSummary>
ReadOnlyBlueprintReader::ListNiagaraSystems(std::string_view p) { return inner_->ListNiagaraSystems(p); }
IBlueprintReader::NiagaraSystemInfo
ReadOnlyBlueprintReader::ReadNiagaraSystem(std::string_view a) { return inner_->ReadNiagaraSystem(a); }
IBlueprintReader::CreateNiagaraSystemResult
ReadOnlyBlueprintReader::CreateNiagaraSystem(std::string_view) { Reject("create_niagara_system"); }
IBlueprintReader::SetNiagaraParameterResult
ReadOnlyBlueprintReader::SetNiagaraParameter(std::string_view,
	std::string_view, std::string_view) { Reject("set_niagara_parameter"); }

std::vector<BPAssetSummary>
ReadOnlyBlueprintReader::ListLevelSequences(std::string_view p) { return inner_->ListLevelSequences(p); }
IBlueprintReader::LevelSequenceInfo
ReadOnlyBlueprintReader::ReadLevelSequence(std::string_view a) { return inner_->ReadLevelSequence(a); }
IBlueprintReader::AddSequenceTrackResult
ReadOnlyBlueprintReader::AddSequenceTrack(std::string_view,
	std::string_view, std::string_view) { Reject("add_sequence_track"); }
IBlueprintReader::SetSequencePlaybackRangeResult
ReadOnlyBlueprintReader::SetSequencePlaybackRange(std::string_view,
	double, double) { Reject("set_sequence_playback_range"); }

IBlueprintReader::GameplayTagListResult
ReadOnlyBlueprintReader::ListGameplayTags(std::string_view f) { return inner_->ListGameplayTags(f); }
IBlueprintReader::AddGameplayTagResult
ReadOnlyBlueprintReader::AddGameplayTag(std::string_view, std::string_view) {
	Reject("add_gameplay_tag");
}
IBlueprintReader::AbilitySetInfo
ReadOnlyBlueprintReader::ReadAbilitySet(std::string_view a) { return inner_->ReadAbilitySet(a); }

std::vector<BPAssetSummary>
ReadOnlyBlueprintReader::ListAnimBlueprints(std::string_view p) { return inner_->ListAnimBlueprints(p); }
IBlueprintReader::AnimBlueprintInfo
ReadOnlyBlueprintReader::ReadAnimBlueprint(std::string_view a) { return inner_->ReadAnimBlueprint(a); }
IBlueprintReader::AddAnimStateResult
ReadOnlyBlueprintReader::AddAnimState(std::string_view, std::string_view, std::string_view) {
	Reject("add_anim_state");
}
IBlueprintReader::CompileAnimBlueprintResult
ReadOnlyBlueprintReader::CompileAnimBlueprint(std::string_view) {
	Reject("compile_anim_blueprint");
}

// ----- factory -----------------------------------------------------------
std::unique_ptr<IBlueprintReader>
MaybeWrapReadOnly(std::unique_ptr<IBlueprintReader> inner, bool readOnly) {
	if (!readOnly)
	{
		return inner;
	}
	return std::make_unique<ReadOnlyBlueprintReader>(std::move(inner));
}

}    // namespace bpr::backends
