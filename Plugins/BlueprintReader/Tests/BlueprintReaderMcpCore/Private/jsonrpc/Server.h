// JSON-RPC 2.0 server — stdio transport with auto-detected framing.
//
// Supports BOTH framings interoperably:
//   * MCP-spec stdio: newline-delimited JSON (one JSON object per line).
//     This is what https://modelcontextprotocol.io/specification/transports
//     mandates and what the official SDKs + JetBrains Copilot use.
//   * LSP-style: "Content-Length: N\r\n\r\n<payload>" framing. Used by some
//     clients and by our smoke tests.
//
// On read, we auto-detect which the client is sending by peeking the first
// non-whitespace byte. On write, we mirror the format the client used so
// the response shape matches what they parse.
//
// Spec refs:
//   * JSON-RPC 2.0: https://www.jsonrpc.org/specification
//   * MCP transport: https://modelcontextprotocol.io/specification/transports
#pragma once

#include <functional>
#include <iosfwd>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace bpr::jsonrpc {

// Standard JSON-RPC error codes.
enum class ErrorCode : int {
	ParseError     = -32700,
	InvalidRequest = -32600,
	MethodNotFound = -32601,
	InvalidParams  = -32602,
	InternalError  = -32603,
};

struct Error {
	int code;
	std::string message;
	std::optional<nlohmann::json> data;
};

// Returned by handlers: either a JSON result or a JSON-RPC error.
struct Response {
	std::optional<nlohmann::json> result;
	std::optional<Error> error;

	static Response Ok(nlohmann::json result) {
		Response r;
		r.result = std::move(result);
		return r;
	}
	static Response Fail(ErrorCode code, std::string message,
						 std::optional<nlohmann::json> data = std::nullopt) {
		Response r;
		r.error = Error{static_cast<int>(code), std::move(message), std::move(data)};
		return r;
	}
	static Response Fail(int code, std::string message,
						 std::optional<nlohmann::json> data = std::nullopt) {
		Response r;
		r.error = Error{code, std::move(message), std::move(data)};
		return r;
	}
};

// Method handler. Receives the params (may be null/object/array). Returns a
// Response. For notifications (id absent), the return value is ignored.
using Handler = std::function<Response(const nlohmann::json& params)>;

// Wire format used for stdio framing.
enum class FrameFormat {
	NewlineDelimited,  // MCP spec: one JSON object per line
	ContentLength,     // LSP-style: "Content-Length: N\r\n\r\n<body>"
};

// Reads a single framed message from `in`, auto-detecting format on the
// first byte. Sets *outFormat (if non-null) to whichever format the frame
// used so callers can mirror it on writes. Returns the raw JSON body as a
// string. Returns std::nullopt on clean EOF before any data.
// Throws std::runtime_error on malformed framing.
std::optional<std::string> ReadFrame(std::istream& in, FrameFormat* outFormat = nullptr);

// Writes a single framed JSON message to `out` using the requested format,
// then flushes. Defaults to newline-delimited (MCP spec) for new code.
void WriteFrame(std::ostream& out, const nlohmann::json& body,
				FrameFormat format = FrameFormat::NewlineDelimited);

class Server {
public:
	// Register a method. Existing entries are replaced.
	void Register(std::string method, Handler handler);

	// Run the read/dispatch/write loop until EOF on `in`. Logs to `log`
	// (typically std::cerr — must NOT be the stdout stream used as transport).
	void Run(std::istream& in, std::ostream& out, std::ostream& log);

	// Dispatch a single decoded request body. Public for testing; in the
	// production loop this is called by Run().
	//
	// Returns the response JSON, or std::nullopt if the input was a
	// notification (no `id`) or a malformed request that we choose to drop.
	std::optional<nlohmann::json> Dispatch(const nlohmann::json& body);

	// Queue a server-initiated notification (no `id`). Run() flushes
	// the queue after each WriteFrame, so notifications interleave
	// cleanly between client request/response pairs.
	//
	// Used today by the MCP layer to send
	// `notifications/tools/list_changed` when a tools/call mutated the
	// advertised tool set (progressive disclosure path). The queue
	// is FIFO and best-effort — no per-recipient targeting, no retry.
	//
	// **Thread-safe** via `notifMu_`. Today's only caller is the
	// tools/call handler that runs on the dispatch thread, but any
	// future async path (a tool that spawns a background thread, the
	// HTTP/SSE transport tracked in issue #81) can call from any
	// thread without coordination.
	void QueueNotification(std::string method, nlohmann::json params);

	// Drain + return the queue. Run() calls this after writing each
	// response; tests can call it directly to assert queued notifs.
	std::vector<nlohmann::json> TakePendingNotifications();

private:
	std::map<std::string, Handler> handlers_;
	mutable std::mutex notifMu_;
	std::vector<nlohmann::json> pendingNotifications_;
};

// Helpers for building JSON-RPC envelopes (used by Dispatch and tests).
nlohmann::json MakeResultEnvelope(const nlohmann::json& id, nlohmann::json result);
nlohmann::json MakeErrorEnvelope(const nlohmann::json& id, const Error& err);

} // namespace bpr::jsonrpc
