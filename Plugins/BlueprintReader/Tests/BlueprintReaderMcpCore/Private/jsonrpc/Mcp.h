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
#include "tools/EditorSubscriptions.h"
#include "tools/Logger.h"
#include "tools/Prompts.h"
#include "tools/Resources.h"
#include "tools/ToolRegistry.h"

#include <string>

namespace bpr::mcp {

struct ServerInfo {
    std::string name = "bp-reader-mcp";
    std::string version = "0.4.0";
    // Optional human-readable description for the server; emitted in the
    // `serverInfo` block of the InitializeResult per MCP 2025-11-25 §lifecycle.
    // Clients display this in their MCP server management UIs.
    std::string description =
        "UE5 Blueprint introspection, mutation, and BP<->C++ transpile server. "
        "258 tools: read, write, editor-control, transpile. Read-only by default "
        "(BP_READER_ALLOW_WRITE=1 for writes).";
    // Latest MCP spec we target by default. Mcp.cpp's initialize handler
    // performs version negotiation: if the client requests an older known
    // version (2024-11-05, 2025-03-26, 2025-06-18), the server echoes it
    // back so older clients keep working. Bump this when adopting a newer
    // spec's primitives (outputSchema, structuredContent, audience, etc.).
    // 2025-11-25 adds the optional `tasks` primitive; we don't advertise it,
    // so claiming the version costs nothing and keeps us level with clients.
    std::string protocolVersion = "2025-11-25";

    // Free-form context shipped on the initialize response per MCP
    // spec `instructions` field. The LLM consumes this once at session
    // start as system-prompt context — explains what the server is for,
    // calls out our BPIR pivot, points at where docs live. Empty means
    // omit the field. Default is the multi-paragraph onboarding text
    // from DefaultInstructions(); main.cpp suppresses it when env var
    // BP_READER_INSTRUCTIONS=0.
    std::string instructions;
};

// Returns the canonical onboarding text for the `instructions` field
// on the initialize response. Pure function — same output every call.
// Exported so main.cpp can decide whether to set it on the ServerInfo
// (gated by env var) without duplicating the literal.
std::string DefaultInstructions();

// Registers the MCP method handlers against `server`, wiring them to
// `registry`. `registry` is non-const because progressive-disclosure
// tools (e.g. `enable_tool_category`) mutate the active subset at
// runtime. The `tools/call` handler also drains the registry's
// list-changed flag and queues a `notifications/tools/list_changed`
// notification on the server when set.
void RegisterHandlers(jsonrpc::Server& server,
                      tools::ToolRegistry& registry,
                      const ServerInfo& info);

// Same shape but also wires the prompts/list + prompts/get handlers
// against `prompts`. When `prompts` is empty (no slash commands
// registered), the `prompts` capability is omitted from the initialize
// response — older clients see the same surface as before. Phase 3.
void RegisterHandlers(jsonrpc::Server& server,
                      tools::ToolRegistry& registry,
                      tools::prompts::PromptRegistry& prompts,
                      const ServerInfo& info);

// Full surface — also wires `logging/setLevel` against `logger`, and
// advertises `logging: {}` on initialize. Pass nullptr for prompts or
// logger to skip those capabilities independently. Phase 6.
void RegisterHandlers(jsonrpc::Server& server,
                      tools::ToolRegistry& registry,
                      tools::prompts::PromptRegistry* prompts,
                      tools::Logger* logger,
                      const ServerInfo& info);

// Full surface including resources/list + resources/read. Phase 4.
// Same nullable-pointer pattern: pass nullptr for any of
// prompts/logger/resources to suppress the matching capability +
// handlers.
//
// `editorSubs` (Phase 10, EA-push): when non-null, advertises the
// `experimental.editor` capability and registers editor/subscribe +
// editor/unsubscribe. nullptr (the default) keeps push events off —
// editor/subscribe is unregistered, so it yields -32601. main.cpp wires
// it only under BP_READER_PUSH_EVENTS.
void RegisterHandlers(jsonrpc::Server& server,
                      tools::ToolRegistry& registry,
                      tools::prompts::PromptRegistry* prompts,
                      tools::Logger* logger,
                      tools::resources::ResourceRegistry* resources,
                      const ServerInfo& info,
                      tools::EditorSubscriptions* editorSubs = nullptr);

}    // namespace bpr::mcp
