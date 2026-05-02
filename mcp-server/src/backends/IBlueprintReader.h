// IBlueprintReader — the inner contract between the MCP tool layer and
// whatever is actually reading blueprint data (mock fixtures, commandlet
// subprocess, live-editor socket). All backends return the same canonical
// shapes from Shared/BlueprintReaderTypes.h.
#pragma once

#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

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
};

} // namespace bpr::backends
