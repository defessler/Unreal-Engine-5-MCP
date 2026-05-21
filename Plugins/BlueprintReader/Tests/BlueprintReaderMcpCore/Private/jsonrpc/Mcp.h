// MCP handshake + dispatch glue — wires a ToolRegistry into a Server by
// registering the MCP-defined methods (initialize, tools/list, tools/call,
// notifications/initialized, ping).
//
// MCP semantics:
//   * initialize: returns capabilities + serverInfo.
//   * notifications/initialized: client signals it's ready. We just ack
//     (no response body — it's a notification).
//   * tools/list: returns the registry spec.
//   * tools/call: looks up the named tool, invokes it, wraps the result in
//     `{ content: [{ type: "text", text: "<json>" }], isError: false }`.
//     If the tool throws, returns the same envelope shape with isError: true
//     and the error text in the content. Per MCP, tool errors are reported
//     this way rather than as JSON-RPC error responses.
#pragma once

#include "jsonrpc/Server.h"
#include "tools/ToolRegistry.h"

#include <string>

namespace bpr::mcp {

struct ServerInfo {
    std::string name = "bp-reader-mcp";
    std::string version = "0.1.0";
    // Latest MCP spec we target by default. Mcp.cpp's initialize handler
    // performs version negotiation: if the client requests an older known
    // version (2024-11-05, 2025-03-26, 2025-06-18), the server echoes it
    // back so older clients keep working. Bump this when adopting a newer
    // spec's primitives (outputSchema, structuredContent, audience, etc.).
    std::string protocolVersion = "2025-06-18";
};

// Registers the MCP method handlers against `server`, wiring them to
// `registry`. `registry` is non-const because progressive-disclosure
// tools (e.g. `enable_tool_category`) mutate the active subset at
// runtime. The `tools/call` handler also drains the registry's
// list-changed flag and queues a `notifications/tools/list_changed`
// notification on the server when set.
void RegisterHandlers(jsonrpc::Server& server,
                      tools::ToolRegistry& registry,
                      const ServerInfo& info);

}    // namespace bpr::mcp
