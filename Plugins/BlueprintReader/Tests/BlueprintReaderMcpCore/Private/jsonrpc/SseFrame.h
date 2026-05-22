// Server-Sent Events (SSE) frame formatting for the MCP 2025-06-18
// Streamable HTTP transport.
//
// SSE is a text/event-stream content-type pattern where each "event"
// looks like:
//
//   event: <name>\n
//   id: <opt id>\n
//   data: <line 1>\n
//   data: <line 2>\n
//   \n        ← double newline terminates the event
//
// The MCP server emits JSON-RPC notifications as SSE frames over a
// persistent HTTP GET socket (per the 2025-06-18 spec). The frame
// layer is pure-function so it's unit-testable without sockets — the
// socket loop just concatenates frames and flushes.
//
// Why a separate file: cpp-httplib (the future socket transport) is
// vendored at link time, but the framing is reusable for any transport
// we plug in later (raw Winsock, asio, etc.). Keeping the formatter
// transport-agnostic lets us spike the frame correctness ahead of the
// socket loop.
//
// MCP spec ref:
//   https://modelcontextprotocol.io/specification/2025-06-18/server/transports#server-sent-events

#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace bpr::jsonrpc::http {

// Format a single SSE frame carrying a JSON-RPC notification.
//
//   * `event` is the SSE event-name field — by convention we use
//     "message" for JSON-RPC payloads; clients differentiate further
//     via the `method` field in the JSON body.
//   * `body` is the JSON-RPC notification object (must include
//     `"jsonrpc":"2.0"`, `"method"`, and `"params"`).
//   * `id` is the optional SSE event id (clients use this to resume
//     after a disconnect via `Last-Event-ID` header). Pass nullopt to
//     omit.
//
// JSON is serialized with `.dump()` (compact form) and split on \n
// into multiple `data:` lines so the SSE parser on the client side
// reassembles the original JSON. Trailing `\n\n` terminates the frame.
//
// Returns the wire-ready bytes — append to the socket buffer as-is.
std::string FormatSseFrame(const std::string& event,
                            const nlohmann::json& body,
                            std::optional<std::string> id = std::nullopt);

// Convenience: format a JSON-RPC notification as an SSE frame with
// event-name "message". Generates the `{"jsonrpc":"2.0", method, params}`
// envelope from the method + params args. The most common caller path
// for tools/list_changed and friends.
std::string FormatNotificationFrame(const std::string& method,
                                     const nlohmann::json& params,
                                     std::optional<std::string> id = std::nullopt);

// SSE retry-hint frame: tells the client how long to wait before
// reconnecting after a dropped stream. Format:
//
//   retry: <ms>\n\n
//
// Useful at stream open to advise clients of our preferred reconnect
// cadence. Not necessary for correctness — clients have a default.
std::string FormatRetryFrame(int milliseconds);

// SSE comment / keep-alive frame. SSE allows comment lines starting
// with ":" — they're not events but DO reset the client's reconnect
// timer. Use these as keep-alives on a quiet stream so proxies /
// firewalls don't drop the connection during slow periods.
//
//   : <text>\n\n
//
// Empty `text` produces a minimal heartbeat (`:\n\n`, 3 bytes).
std::string FormatCommentFrame(const std::string& text = "");

// Parse a stream of SSE frame bytes into a vector of {event, data, id}
// triples. Helpful for tests + clients written in the same TU. Tolerant
// of CRLF line endings, multi-line data fields, and missing optional
// fields. Returns empty when input is empty or all-comment.
struct SseEvent {
	std::string event;          // "message" by default per SSE spec
	std::string data;           // payload (already-joined from multi-line)
	std::string id;             // empty when no id field present
};
std::vector<SseEvent> ParseSseStream(const std::string& raw);

}    // namespace bpr::jsonrpc::http
