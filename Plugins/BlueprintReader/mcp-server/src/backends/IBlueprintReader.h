// IBlueprintReader — the inner contract between the MCP tool layer and
// whatever is actually reading blueprint data (mock fixtures, commandlet
// subprocess, live-editor socket). All backends return the same canonical
// shapes from BlueprintReaderTypes.h.
#pragma once

#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "BlueprintReaderTypes.h"

namespace bpr::backends {

// Backend-side error type. The MCP layer catches this (and any std::exception)
// and surfaces it as an MCP tool error envelope.
class BlueprintReaderError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Thrown when an asset path doesn't resolve. Sub-class so callers can
// distinguish "you typed the path wrong" from "I couldn't talk to the engine".
class AssetNotFound : public BlueprintReaderError {
public:
    using BlueprintReaderError::BlueprintReaderError;
};

class IBlueprintReader {
public:
    virtual ~IBlueprintReader() = default;

    virtual std::vector<BPAssetSummary> ListBlueprints(std::string_view path) = 0;
    virtual BPMetadata                  ReadBlueprint(std::string_view assetPath) = 0;
    virtual BPGraph                     GetGraph(std::string_view assetPath, std::string_view graphName) = 0;
    virtual BPFunction                  GetFunction(std::string_view assetPath, std::string_view fnName) = 0;
    virtual std::vector<BPVariable>     ListVariables(std::string_view assetPath) = 0;
    virtual std::vector<BPComponent>    GetComponents(std::string_view assetPath) = 0;
    // `kind`, when non-empty, additionally filters matches by their K2 extras
    // "kind" entry (e.g. "CallFunction", "VariableGet", "Event"). The text
    // `query` matches case-insensitively against class or title; `kind` is an
    // exact match (case-insensitive) against meta["kind"].
    virtual std::vector<BPNode>         FindNode(std::string_view assetPath, std::string_view query,
                                                 std::string_view kind = {}) = 0;

    // Write tools (Phase 1.5). Backends that don't support mutation throw
    // BlueprintReaderError. Each call should leave the .uasset compilable.

    // Add a member variable to a blueprint. `type` is the wire BPPinType.
    // `defaultValue`, `category` may be empty.
    virtual void AddVariable(std::string_view assetPath, std::string_view name,
                             const BPPinType& type, std::string_view defaultValue,
                             std::string_view category, bool replicated, bool editable) = 0;

    // Reposition a node by its GUID inside `graphName`.
    virtual void SetNodePosition(std::string_view assetPath, std::string_view graphName,
                                 std::string_view nodeId, int x, int y) = 0;

    // Delete a node by its GUID. Breaks any incoming/outgoing links first.
    virtual void DeleteNode(std::string_view assetPath, std::string_view graphName,
                            std::string_view nodeId) = 0;

    // Spawn a new node in `graphName` at (x, y). `kind` is one of:
    //   "Branch", "Sequence", "VariableGet", "VariableSet", "CallFunction",
    //   "CustomEvent". Kind-specific extras passed through the `extras` map
    //   (e.g. "Variable", "Function", "FunctionOwner", "EventName").
    // Returns the new node's GUID so the caller can wire pins to it.
    virtual std::string AddNode(std::string_view assetPath, std::string_view graphName,
                                std::string_view kind, int x, int y,
                                const std::map<std::string, std::string, std::less<>>& extras) = 0;

    // Connect two pins by node GUID + pin spec (GUID or name).
    virtual void WirePins(std::string_view assetPath, std::string_view graphName,
                          std::string_view fromNodeId, std::string_view fromPinSpec,
                          std::string_view toNodeId,   std::string_view toPinSpec) = 0;

    // Remove a member variable. Throws AssetNotFound if missing.
    virtual void DeleteVariable(std::string_view assetPath, std::string_view name) = 0;

    // Rename a member variable. Updates references in graphs.
    virtual void RenameVariable(std::string_view assetPath, std::string_view oldName,
                                std::string_view newName) = 0;

    // Add a new BP function graph. Returns the function name (echoed back)
    // plus the FunctionEntry node's GUID so callers can wire its `then`
    // exec output into their first statement without a follow-up read.
    struct AddFunctionResult {
        std::string functionName;
        std::string entryNodeId;  // empty if the plugin couldn't locate it
    };
    virtual AddFunctionResult AddFunction(std::string_view assetPath, std::string_view name) = 0;

    // Add an input parameter to an existing function. `type` is a wire BPPinType.
    virtual void AddFunctionInput(std::string_view assetPath, std::string_view functionName,
                                  std::string_view paramName, const BPPinType& type) = 0;

    // Add an output parameter to an existing function. Spawns a FunctionResult
    // node if there isn't one yet.
    virtual void AddFunctionOutput(std::string_view assetPath, std::string_view functionName,
                                   std::string_view paramName, const BPPinType& type) = 0;

    // Delete a function and its graph.
    virtual void DeleteFunction(std::string_view assetPath, std::string_view name) = 0;

    // Change a variable's default value (string form, as displayed in the Details panel).
    virtual void SetVariableDefault(std::string_view assetPath, std::string_view name,
                                    std::string_view newDefault) = 0;

    // Create a brand-new BP under `assetPath` (must be `/Game/...`) extending
    // `parentClass` (UClass path or short name). Idempotent — if the asset
    // already exists, returns without throwing. Required so AI agents can
    // generate whole new BPs, not just mutate existing ones (A3).
    struct CreateBlueprintResult {
        bool alreadyExisted = false;
        std::string parentClass;  // resolved full path, for echo
    };
    virtual CreateBlueprintResult CreateBlueprint(std::string_view assetPath,
                                                  std::string_view parentClass) = 0;

    // Set the literal default value on a node's pin (B1). Used by
    // compile_function's {lit:value} support — UE has no first-class
    // literal node, so the value is materialized as the consumer pin's
    // default. `pinSpec` accepts a pin GUID or a pin name.
    virtual void SetPinDefault(std::string_view assetPath,
                               std::string_view graphName,
                               std::string_view nodeId,
                               std::string_view pinSpec,
                               std::string_view value) = 0;

    // ----- Batch sentinels (A1) ------------------------------------------------
    // BeginBatch / EndBatch wrap a sequence of write ops so the expensive
    // CompileBlueprint + SavePackage runs once per affected BP at EndBatch
    // instead of once per op. apply_ops uses this to collapse N×compile to 1.
    //
    // Default no-op so backends that don't care (mock, future read-only) need
    // no changes. CommandletBlueprintReader overrides to emit the matching
    // -Op=BeginBatch / -Op=EndBatch lines to the daemon.
    //
    // Best-effort failure semantics: if a batch is open and a write op throws,
    // EndBatch should still be called by the caller (in a try/finally pattern)
    // and will compile+save whatever ops landed before the failure.
    //
    // EndBatch returns a JSON object describing the flush: `{ok, recompiled,
    // diagnostics, error_count, warning_count}` (C1). Default implementation
    // returns an empty object — backends without a real compile step have
    // nothing to surface.
    //
    // `skipCompile` is the on_failure="skip" path — the caller knows
    // something failed mid-batch and doesn't want partial state on disk.
    // Plugin honors this by skipping the per-BP compile + save loop in
    // EndBatch (in-memory state stays dirty until daemon restarts;
    // documented as a limitation of strict-atomic mode).
    virtual void BeginBatch() {}
    virtual nlohmann::json EndBatch(bool /*skipCompile*/ = false) {
        return nlohmann::json::object();
    }

    // Tear down any backing process / connection / cache the backend holds
    // open. Optional — default is a no-op for backends that don't have one
    // (mock, future live). The CommandletBlueprintReader override sends
    // QUIT to its daemon and joins, freeing the project lock so the user
    // can launch the full editor (or another tool) without contention.
    //
    // Subsequent tool calls auto-respawn the daemon — same path the
    // existing daemon-died fallback uses. So this is safe to call ad-hoc;
    // the next read just pays a one-time cold start.
    //
    // Returns a JSON object describing what happened: {ok:true,
    // was_running:bool, ...}. Backends without a teardownable resource
    // return {ok:true, was_running:false}.
    virtual nlohmann::json ShutdownDaemon() {
        return nlohmann::json{{"ok", true}, {"was_running", false}};
    }
};

} // namespace bpr::backends
