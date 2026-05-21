// Streamable HTTP transport per MCP 2025-06-18 (sketch).
//
//   * POST <path>          — body is a single JSON-RPC request, response is
//                            the JSON-RPC reply (200 OK, application/json).
//   * GET  <path>          — long-poll SSE for server-initiated
//                            notifications (text/event-stream).
//   * DELETE <path>        — terminate the session.
//
// Sessions are identified by `Mcp-Session-Id` HTTP header (server
// assigns on the first POST handshake response, client echoes on
// subsequent calls). DNS rebinding is mitigated by Origin-header
// validation: only requests with Origin matching localhost (any port)
// or with no Origin header at all (non-browser clients) are accepted.
//
// This file ships the **frame-level** logic — parse a raw HTTP/1.1
// request string into an HttpRequest, build an HttpResponse, route to
// the JSON-RPC dispatcher. Socket I/O is intentionally separated so
// the dispatcher logic is unit-testable in isolation (the existing
// stdio/TCP transport works the same way: Server::Dispatch is the
// pure function, Server::Run is the I/O wrapper).
//
// To actually expose this on a network port, see HttpServerMain (the
// runtime socket loop) — gated on BP_READER_HTTP_PORT being set.
//
// MCP spec refs:
//   * https://modelcontextprotocol.io/specification/2025-06-18/server/transports#streamable-http
//   * https://modelcontextprotocol.io/specification/2025-06-18/utilities/cancellation
#pragma once

#include "jsonrpc/Server.h"

#include <optional>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace bpr::jsonrpc::http {

// Case-insensitive ASCII compare for HTTP headers (a-z mapped to A-Z).
struct CaseInsensitiveHash {
	size_t operator()(const std::string& s) const noexcept;
};
struct CaseInsensitiveEqual {
	bool operator()(const std::string& a, const std::string& b) const noexcept;
};
using HeaderMap = std::unordered_map<std::string, std::string,
									 CaseInsensitiveHash, CaseInsensitiveEqual>;

struct HttpRequest {
	std::string method;   // "POST", "GET", "DELETE"
	std::string path;     // "/mcp" or whatever
	std::string body;     // raw bytes (may be empty)
	HeaderMap   headers;
};

struct HttpResponse {
	int statusCode = 200;
	std::string statusText = "OK";
	std::string contentType = "application/json";
	std::string body;
	HeaderMap   headers;
};

// Parse a raw HTTP/1.1 request. Throws std::runtime_error on malformed
// input. Only parses what we need: request line + headers + body of
// exactly Content-Length bytes. Doesn't support chunked transfer
// encoding (clients we care about all use Content-Length for POST).
HttpRequest ParseRequest(const std::string& raw);

// Format an HttpResponse as an HTTP/1.1 response string ready to write
// to the socket.
std::string FormatResponse(const HttpResponse& resp);

// Validate Origin header per MCP §security. Returns true if the
// request should be allowed (no Origin, or Origin matching localhost
// /127.0.0.1 / [::1] on any port). False = reject with 403.
bool IsOriginAllowed(const HttpRequest& req);

// Process a single HTTP request: validates Origin, parses body as
// JSON-RPC, dispatches via `server`, builds the HTTP response.
//
//   * POST with JSON body → dispatch as request, return the response
//     as a single-frame JSON body. If the JSON-RPC layer returns no
//     response (notification), reply 202 Accepted with empty body.
//   * GET (SSE long-poll) → for now returns 501 Not Implemented;
//     SSE streaming requires a persistent socket and the existing
//     Server::QueueNotification flush; future work.
//   * DELETE → 204 No Content (sessions are stateless today; just ack).
//
// `path` is the MCP server path the listener bound to (e.g. "/mcp").
// Requests against a different path get 404.
HttpResponse Handle(const HttpRequest& req, Server& server, const std::string& mcpPath);

}  // namespace bpr::jsonrpc::http
