// Phase 9 (C3) — runtime HTTP/1.1 socket loop for the MCP Streamable
// HTTP transport. Opt-in: main() calls RunHttpServer instead of the
// stdio loop when BP_READER_HTTP_PORT is set. Stdio remains the default,
// so this is purely additive — nothing on the default path changes.
//
// The request parsing + JSON-RPC dispatch + DNS-rebinding Origin guard
// are the already-unit-tested HttpTransport layer (ParseRequest /
// Handle / FormatResponse). This file is *only* the socket plumbing the
// transport docs referred to as "HttpServerMain".
//
// v1 scope (C3): blocking, single-connection-at-a-time accept loop —
// read one request, http::Handle it, write the response, close. Binds
// 127.0.0.1 only (localhost transport; the Origin guard backs it up).
// Follow-ups (own commits): SSE GET streaming (Handle currently returns
// 405 for GET — needs a persistent socket + FormatSseFrame flush) and
// Mcp-Session-Id sessions (stateless today).
//
// Live-validated: POST initialize round-trips 200 + JSON-RPC body;
// external Origin -> 403 (guard enforced); GET -> 405.

#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>

namespace bpr::jsonrpc { class Server; }
namespace bpr::backends { class IBlueprintReader; }
namespace bpr::tools { class EditorSubscriptions; }

namespace bpr::jsonrpc::http {

// Run the blocking localhost HTTP listener. Dispatches each request via
// http::Handle(req, server, mcpPath). Returns a process exit code; the
// accept loop runs until a fatal listen-socket error (the process is
// killed to stop it, same lifecycle as the stdio EOF path).
//
// `reader` + `editorSubs` (Phase 10 auto-push, both optional): when both
// are set, an SSE GET stream periodically drains reader->GetEditorEvents()
// and queues each subscribed event as a notifications/editor/<name>
// notification (which the same stream then emits). Backend access happens
// under the server mutex that already serializes POST dispatch, so no new
// concurrency surface. Pass nullptr to disable auto-push (POST/SSE still
// work; the stream just relays server-generated notifications).
int RunHttpServer(Server& server, uint16_t port, const std::string& mcpPath,
                  std::ostream& log,
                  backends::IBlueprintReader* reader = nullptr,
                  tools::EditorSubscriptions* editorSubs = nullptr);

}  // namespace bpr::jsonrpc::http
