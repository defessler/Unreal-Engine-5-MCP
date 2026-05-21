#include "jsonrpc/Server.h"

#include "jsonrpc/CallContext.h"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <stdexcept>
#include <utility>

#include <fmt/core.h>

namespace bpr::jsonrpc {

namespace server_detail {

std::string TrimAscii(std::string s) {
	auto notSpace = [](unsigned char c) { return !std::isspace(c); };
	s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
	s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
	return s;
}

bool ReadHeaderLine(std::istream& in, std::string& out) {
	out.clear();
	while (true) {
		int c = in.get();
		if (c == std::char_traits<char>::eof()) {
			return !out.empty(); // truncated header at EOF
		}
		if (c == '\r') {
			int n = in.get();
			if (n != '\n') {
				throw std::runtime_error("malformed header: CR not followed by LF");
			}
			return true;
		}
		if (c == '\n') {
			// Tolerate bare-LF line endings.
			return true;
		}
		out.push_back(static_cast<char>(c));
	}
}

}    // namespace server_detail
using namespace server_detail;

namespace server_detail2 {

// Read a single newline-delimited JSON message: everything up to the next \n
// (which we consume but don't include in the body). Tolerates \r\n. Returns
// nullopt on clean EOF before any non-whitespace byte. The first byte (already
// peeked by the caller) is consumed as part of the line.
std::optional<std::string> ReadNewlineFrame(std::istream& in) {
	std::string body;
	while (true) {
		int c = in.get();
		if (c == std::char_traits<char>::eof()) {
			// EOF in the middle of a line — return what we have if non-empty,
			// otherwise treat as clean EOF.
			return body.empty() ? std::nullopt : std::optional<std::string>(body);
		}
		if (c == '\n') {
			// Strip a trailing \r if the line was \r\n-terminated.
			if (!body.empty() && body.back() == '\r')
			{
				body.pop_back();
			}
			return body;
		}
		body.push_back(static_cast<char>(c));
	}
}

// Read a Content-Length-framed message. Headers are case-insensitive; we
// accept any extra headers and only require Content-Length.
std::optional<std::string> ReadContentLengthFrame(std::istream& in) {
	std::size_t contentLength = 0;
	bool sawContentLength = false;

	while (true) {
		std::string line;
		if (!ReadHeaderLine(in, line)) {
			return std::nullopt;
		}
		if (line.empty())
		{
			break;  // end of headers
		}

		auto colon = line.find(':');
		if (colon == std::string::npos) {
			throw std::runtime_error(fmt::format("malformed header line: '{}'", line));
		}
		std::string name = TrimAscii(line.substr(0, colon));
		std::string value = TrimAscii(line.substr(colon + 1));

		std::transform(name.begin(), name.end(), name.begin(),
					   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		if (name == "content-length") {
			try {
				contentLength = static_cast<std::size_t>(std::stoull(value));
				sawContentLength = true;
			} catch (...) {
				throw std::runtime_error(fmt::format("invalid Content-Length: '{}'", value));
			}
		}
	}

	if (!sawContentLength) {
		throw std::runtime_error("missing Content-Length header");
	}
	std::string body(contentLength, '\0');
	in.read(body.data(), static_cast<std::streamsize>(contentLength));
	auto got = in.gcount();
	if (static_cast<std::size_t>(got) != contentLength) {
		throw std::runtime_error(fmt::format(
			"short read on body: expected {}, got {}", contentLength, got));
	}
	return body;
}

}    // namespace server_detail2
using namespace server_detail2;

std::optional<std::string> ReadFrame(std::istream& in, FrameFormat* outFormat) {
	// Skip leading whitespace (newlines, CRs, spaces). Some clients pad with
	// a trailing \n after the framing terminator; tolerate it instead of
	// throwing "malformed header" on next read.
	//
	// Also tolerate a UTF-8 BOM (EF BB BF) at the start of the stream — some
	// clients on Windows insert one when piping JSON, and nlohmann::json's
	// parser throws on a leading BOM by default.
	while (true) {
		int c = in.peek();
		if (c == std::char_traits<char>::eof())
		{
			return std::nullopt;
		}
		if (c == '\r' || c == '\n' || c == ' ' || c == '\t') { in.get(); continue; }
		// BOM check: if the next 3 bytes are EF BB BF, eat them and re-skip.
		if (c == 0xEF) {
			// Read 3 bytes; check for BOM. Anything else is malformed but we
			// re-throw at parse time rather than guess.
			char tri[3];
			in.read(tri, 3);
			if (in.gcount() == 3 &&
				static_cast<unsigned char>(tri[0]) == 0xEF &&
				static_cast<unsigned char>(tri[1]) == 0xBB &&
				static_cast<unsigned char>(tri[2]) == 0xBF) {
				continue;  // BOM consumed, skip more whitespace
			}
			// Not a BOM — push back what we read and let it fail downstream.
			for (int i = 2; i >= 0; --i)
			{
				in.putback(tri[i]);
			}
			break;
		}
		break;
	}

	int peeked = in.peek();
	if (peeked == std::char_traits<char>::eof())
	{
		return std::nullopt;
	}

	// Auto-detect: JSON values start with `{` (object) or `[` (array). Anything
	// else (printable ASCII like "Content-Length:") is LSP-style header framing.
	if (peeked == '{' || peeked == '[') {
		if (outFormat)
		{
			*outFormat = FrameFormat::NewlineDelimited;
		}
		return ReadNewlineFrame(in);
	}
	if (outFormat)
	{
		*outFormat = FrameFormat::ContentLength;
	}
	return ReadContentLengthFrame(in);
}

void WriteFrame(std::ostream& out, const nlohmann::json& body, FrameFormat format) {
	std::string serialized = body.dump();
	if (format == FrameFormat::NewlineDelimited) {
		// MCP spec: one JSON object per line, no embedded newlines.
		// nlohmann::json::dump() with default settings produces a single line.
		out << serialized << '\n';
	} else {
		out << "Content-Length: " << serialized.size() << "\r\n\r\n" << serialized;
	}
	out.flush();
}

void Server::Register(std::string method, Handler handler) {
	handlers_[std::move(method)] = std::move(handler);
}

void Server::QueueNotification(std::string method, nlohmann::json params) {
	// JSON-RPC 2.0 notification envelope: no `id` field. Run() pulls
	// these off the queue after each WriteFrame to interleave them
	// cleanly between client-driven request/response pairs.
	nlohmann::json env = {
		{"jsonrpc", "2.0"},
		{"method", std::move(method)},
	};
	if (!params.empty())
	{
		env["params"] = std::move(params);
	}
	std::lock_guard<std::mutex> lock(notifMu_);
	pendingNotifications_.push_back(std::move(env));
}

std::vector<nlohmann::json> Server::TakePendingNotifications() {
	std::lock_guard<std::mutex> lock(notifMu_);
	return std::exchange(pendingNotifications_, {});
}

void Server::RegisterInFlight(CallContext* ctx) {
	if (ctx == nullptr) return;
	std::lock_guard<std::mutex> lock(inFlightMu_);
	inFlight_.push_back(ctx);
}

void Server::UnregisterInFlight(CallContext* ctx) {
	if (ctx == nullptr) return;
	std::lock_guard<std::mutex> lock(inFlightMu_);
	inFlight_.erase(std::remove(inFlight_.begin(), inFlight_.end(), ctx), inFlight_.end());
}

CallContext* Server::FindInFlight(const nlohmann::json& requestId) {
	std::lock_guard<std::mutex> lock(inFlightMu_);
	for (CallContext* ctx : inFlight_) {
		if (ctx != nullptr && ctx->Matches(requestId)) {
			return ctx;
		}
	}
	return nullptr;
}

nlohmann::json MakeResultEnvelope(const nlohmann::json& id, nlohmann::json result) {
	nlohmann::json env = {
		{"jsonrpc", "2.0"},
		{"id", id},
		{"result", std::move(result)},
	};
	return env;
}

nlohmann::json MakeErrorEnvelope(const nlohmann::json& id, const Error& err) {
	nlohmann::json e = {
		{"code", err.code},
		{"message", err.message},
	};
	if (err.data.has_value()) {
		e["data"] = *err.data;
	}
	nlohmann::json env = {
		{"jsonrpc", "2.0"},
		{"id", id},
		{"error", std::move(e)},
	};
	return env;
}

std::optional<nlohmann::json> Server::Dispatch(const nlohmann::json& body) {
	// Validate envelope.
	if (!body.is_object()) {
		return MakeErrorEnvelope(nullptr,
			Error{static_cast<int>(ErrorCode::InvalidRequest),
				  "request must be a JSON object", std::nullopt});
	}
	auto idIt = body.find("id");
	nlohmann::json id = (idIt != body.end()) ? *idIt : nlohmann::json(nullptr);
	const bool isNotification = (idIt == body.end());

	auto jsonrpcIt = body.find("jsonrpc");
	if (jsonrpcIt == body.end() || !jsonrpcIt->is_string() || jsonrpcIt->get<std::string>() != "2.0") {
		if (isNotification)
		{
			return std::nullopt;
		}
		return MakeErrorEnvelope(id,
			Error{static_cast<int>(ErrorCode::InvalidRequest),
				  R"(missing or unsupported "jsonrpc" version)", std::nullopt});
	}

	auto methodIt = body.find("method");
	if (methodIt == body.end() || !methodIt->is_string()) {
		if (isNotification)
		{
			return std::nullopt;
		}
		return MakeErrorEnvelope(id,
			Error{static_cast<int>(ErrorCode::InvalidRequest),
				  R"(missing or non-string "method")", std::nullopt});
	}
	std::string method = methodIt->get<std::string>();

	nlohmann::json params = nlohmann::json::object();
	auto paramsIt = body.find("params");
	if (paramsIt != body.end()) {
		params = *paramsIt;
	}

	auto h = handlers_.find(method);
	if (h == handlers_.end()) {
		if (isNotification)
		{
			return std::nullopt;
		}
		return MakeErrorEnvelope(id,
			Error{static_cast<int>(ErrorCode::MethodNotFound),
				  fmt::format("method not found: {}", method), std::nullopt});
	}

	Response resp;
	try {
		resp = h->second(params);
	} catch (const std::exception& e) {
		if (isNotification)
		{
			return std::nullopt;
		}
		return MakeErrorEnvelope(id,
			Error{static_cast<int>(ErrorCode::InternalError),
				  fmt::format("handler threw: {}", e.what()), std::nullopt});
	}

	if (isNotification) {
		return std::nullopt;
	}

	if (resp.error.has_value()) {
		return MakeErrorEnvelope(id, *resp.error);
	}
	return MakeResultEnvelope(id, resp.result.value_or(nlohmann::json::object()));
}

void Server::Run(std::istream& in, std::ostream& out, std::ostream& log) {
	// Mirror the framing format the client sends. Locked in on the first
	// frame we successfully read. Defaults to MCP-spec newline-delimited
	// for safety if we somehow have to write before reading.
	FrameFormat clientFormat = FrameFormat::NewlineDelimited;
	bool formatLocked = false;

	while (true) {
		std::optional<std::string> frame;
		FrameFormat thisFormat = clientFormat;
		try {
			frame = ReadFrame(in, &thisFormat);
		} catch (const std::exception& e) {
			log << "[bp-reader-mcp] frame error: " << e.what() << "\n";
			// Without framing we can't recover the stream; bail.
			return;
		}
		if (!frame) {
			// Clean EOF.
			return;
		}
		if (!formatLocked) {
			clientFormat = thisFormat;
			formatLocked = true;
			// One-shot per session — not info-gated. Always useful to know
			// which framing the client picked, and tests assert this surfaces.
			log << "[bp-reader-mcp] framing="
				<< (clientFormat == FrameFormat::NewlineDelimited ? "newline-delimited"
																  : "content-length")
				<< " (auto-detected from first request)\n";
		}

		nlohmann::json body;
		try {
			body = nlohmann::json::parse(*frame);
		} catch (const std::exception& e) {
			// Per spec: parse errors get a response with id=null.
			auto env = MakeErrorEnvelope(nullptr,
				Error{static_cast<int>(ErrorCode::ParseError),
					  fmt::format("parse error: {}", e.what()), std::nullopt});
			WriteFrame(out, env, clientFormat);
			continue;
		}

		// Batched requests: array of bodies. Per JSON-RPC 2.0 we respond with
		// an array of responses. MCP doesn't use batches in practice but it's
		// cheap to support.
		if (body.is_array()) {
			if (body.empty()) {
				auto env = MakeErrorEnvelope(nullptr,
					Error{static_cast<int>(ErrorCode::InvalidRequest),
						  "empty batch", std::nullopt});
				WriteFrame(out, env, clientFormat);
				continue;
			}
			nlohmann::json batchOut = nlohmann::json::array();
			for (const auto& sub : body) {
				if (auto r = Dispatch(sub)) {
					batchOut.push_back(std::move(*r));
				}
			}
			if (!batchOut.empty()) {
				WriteFrame(out, batchOut, clientFormat);
			}
			// Same notification-flush as the single-request path. In
			// practice MCP clients don't batch, but progressive
			// disclosure shouldn't silently drop notifications if they
			// somehow do.
			for (const auto& notif : TakePendingNotifications()) {
				WriteFrame(out, notif, clientFormat);
			}
			continue;
		}

		if (auto r = Dispatch(body)) {
			WriteFrame(out, *r, clientFormat);
		}

		// Flush any notifications queued by the dispatched handler.
		// Today that's `notifications/tools/list_changed` after a
		// tools/call that mutated the active tool surface (progressive
		// disclosure). The notifications are server-initiated, so the
		// client doesn't pair them with a request id.
		for (const auto& notif : TakePendingNotifications()) {
			WriteFrame(out, notif, clientFormat);
		}
	}
}

}    // namespace bpr::jsonrpc
