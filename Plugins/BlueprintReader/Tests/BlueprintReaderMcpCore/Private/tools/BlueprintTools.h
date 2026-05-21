// Registers the v0 MCP tools on a ToolRegistry. Each tool delegates to
// an IBlueprintReader instance.
#pragma once

#include "backends/IBlueprintReader.h"
#include "tools/ToolRegistry.h"

namespace bpr::tools {

// `reader` must outlive the registry — handlers capture it by reference.
void RegisterBlueprintTools(ToolRegistry& registry,
                            backends::IBlueprintReader& reader);

// Registers the progressive-disclosure meta-tool `enable_tool_category`.
// Call only when BP_READER_PROGRESSIVE is on — otherwise the meta-tool
// would be a no-op surface eating one tool slot for no benefit.
//
// The tool's handler mutates the registry's active subset and sets the
// list-changed flag; the MCP dispatcher (Mcp.cpp) emits
// `notifications/tools/list_changed` accordingly. Tracked across calls
// so a client can call `enable_tool_category("cpp")` then later
// `enable_tool_category("editor")` to incrementally widen the surface.
void RegisterProgressiveDisclosureMetaTool(ToolRegistry& registry);

// Phase D — build the response shape for a screenshot tool, optionally
// embedding the captured PNG as a base64-encoded Image content block.
// Exposed here so tests can exercise the cap rejection + Envelope shape
// without needing a fully-fledged IBlueprintReader stub.
//
//   destPath      — input dest_path arg (used as a fallback when
//                   outputFile is empty)
//   captured      — whether the underlying tool reported success
//   outputFile    — authoritative file path the reader wrote
//   returnInline  — caller's return_inline arg (false → classic shape)
//
// Throws std::invalid_argument when return_inline is true and the
// captured PNG fails the 1280px max-dim cap, the file is missing, or
// the file isn't a valid PNG.
nlohmann::json BuildScreenshotResponse(const std::string& destPath,
                                       bool captured,
                                       const std::string& outputFile,
                                       bool returnInline);

}    // namespace bpr::tools
