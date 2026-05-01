// IBlueprintReader — the inner contract between the MCP tool layer and
// whatever is actually reading blueprint data (mock fixtures, commandlet
// subprocess, live-editor socket). All backends return the same canonical
// shapes from Shared/BlueprintReaderTypes.h.
#pragma once

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
    virtual std::vector<BPNode>         FindNode(std::string_view assetPath, std::string_view query) = 0;
};

} // namespace bpr::backends
