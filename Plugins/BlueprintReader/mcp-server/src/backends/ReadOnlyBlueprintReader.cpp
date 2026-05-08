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

// ----- factory -----------------------------------------------------------
std::unique_ptr<IBlueprintReader>
MaybeWrapReadOnly(std::unique_ptr<IBlueprintReader> inner, bool readOnly) {
    if (!readOnly) return inner;
    return std::make_unique<ReadOnlyBlueprintReader>(std::move(inner));
}

} // namespace bpr::backends
