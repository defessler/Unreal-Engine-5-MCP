// JSON-RPC 2.0 server — Content-Length framed stdio transport.
//
// Spec refs:
//   * JSON-RPC 2.0: https://www.jsonrpc.org/specification
//   * MCP transport: LSP-style "Content-Length: N\r\n\r\n<payload>" framing.
#pragma once

#include <functional>
#include <iosfwd>
#include <map>
#include <optional>
#include <string>

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

// Reads a single Content-Length framed message from `in`. Returns the raw
// JSON body string. Returns std::nullopt on clean EOF before any header.
// Throws std::runtime_error on malformed framing.
std::optional<std::string> ReadFrame(std::istream& in);

// Writes a single framed JSON message to `out`. Flushes after write.
void WriteFrame(std::ostream& out, const nlohmann::json& body);

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

private:
    std::map<std::string, Handler> handlers_;
};

// Helpers for building JSON-RPC envelopes (used by Dispatch and tests).
nlohmann::json MakeResultEnvelope(const nlohmann::json& id, nlohmann::json result);
nlohmann::json MakeErrorEnvelope(const nlohmann::json& id, const Error& err);

} // namespace bpr::jsonrpc
