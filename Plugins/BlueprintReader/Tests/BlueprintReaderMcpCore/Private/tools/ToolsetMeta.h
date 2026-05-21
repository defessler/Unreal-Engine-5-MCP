// Lazy-discovery meta-tools per MCP 2025-06-18 + Epic 5.8's lazy
// `bEnableToolSearch` pattern.
//
// When BP_READER_TOOL_SEARCH is on, the registry's active set is
// trimmed to just three meta-tools instead of advertising all ~127:
//
//   list_toolsets    — returns name + description + tool_count for each
//                      toolset (mapped from our existing categories).
//   describe_toolset — given a toolset name, returns the full tool list
//                      with each tool's description and inputSchema (and
//                      outputSchema when set).
//   call_tool        — dispatches a named tool with arguments. Tools
//                      that aren't in tools/list still callable through
//                      this path. Matches Epic's lazy-discovery flow.
//
// The benefit: an MCP client's `tools/list` response stays small
// (~3 tools instead of ~127) and the LLM context isn't paying for
// schemas it'll never use this turn. The agent expands per toolset
// only when it needs to — same total surface, vastly cheaper baseline.
//
// Caller is responsible for the env-var toggle (main.cpp). Once the
// meta-tools are registered, restrict the active set to just those
// three with `registry.ApplyFilter({"meta-toolset"}, {});` — the
// `meta-toolset` category resolves to the three names.
#pragma once

#include "tools/ToolRegistry.h"

namespace bpr::tools {

// Registers list_toolsets, describe_toolset, and call_tool against
// `registry`. Idempotent — safe to call multiple times.
void RegisterToolsetMetaTools(ToolRegistry& registry);

// Helper used by main.cpp to flip the registry into "tool search" mode
// AFTER all real tools have been registered. Restricts the active set
// to the 3 meta-tools (plus a small `always_visible` allowlist for
// `shutdown_daemon` and any other tool that should remain top-level).
//
// Implementation: ApplyFilter with token list ["meta-toolset"] + the
// always-visible names. ToolCategories.cpp resolves "meta-toolset" to
// {list_toolsets, describe_toolset, call_tool}.
void EnableToolSearchMode(ToolRegistry& registry);

}  // namespace bpr::tools
