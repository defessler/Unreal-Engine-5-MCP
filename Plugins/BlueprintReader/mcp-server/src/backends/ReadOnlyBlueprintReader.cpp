#include "backends/ReadOnlyBlueprintReader.h"

#include <stdexcept>

namespace bpr::backends {

namespace {

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

} // namespace

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

// ----- factory -----------------------------------------------------------
std::unique_ptr<IBlueprintReader>
MaybeWrapReadOnly(std::unique_ptr<IBlueprintReader> inner, bool readOnly) {
    if (!readOnly) return inner;
    return std::make_unique<ReadOnlyBlueprintReader>(std::move(inner));
}

} // namespace bpr::backends
