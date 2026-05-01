// Registers the six v0 MCP tools on a ToolRegistry. Each tool delegates to
// an IBlueprintReader instance.
#pragma once

#include "backends/IBlueprintReader.h"
#include "tools/ToolRegistry.h"

namespace bpr::tools {

// `reader` must outlive the registry — handlers capture it by reference.
void RegisterBlueprintTools(ToolRegistry& registry,
                            backends::IBlueprintReader& reader);

} // namespace bpr::tools
