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
    std::string protocolVersion = "2024-11-05"; // MCP spec we target
};

void RegisterHandlers(jsonrpc::Server& server,
                      const tools::ToolRegistry& registry,
                      const ServerInfo& info);

} // namespace bpr::mcp
