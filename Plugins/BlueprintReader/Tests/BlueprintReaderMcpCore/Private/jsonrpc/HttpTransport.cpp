#include "jsonrpc/HttpTransport.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>

#include <fmt/core.h>

namespace bpr::jsonrpc::http {

namespace http_detail {

char AsciiLower(char c) {
	return (c >= 'A' && c <= 'Z') ? char(c + ('a' - 'A')) : c;
}

std::string ToLower(const std::string& s) {
	std::string out;
	out.reserve(s.size());
	for (char c : s) out.push_back(AsciiLower(c));
	return out;
}

bool CaseEq(const std::string& a, const std::string& b) {
	if (a.size() != b.size()) return false;
	for (size_t i = 0; i < a.size(); ++i) {
		if (AsciiLower(a[i]) != AsciiLower(b[i])) return false;
	}
	return true;
}

std::string Trim(const std::string& s) {
	size_t b = 0;
	while (b < s.size() && (s[b] == ' ' || s[b] == '\t')) ++b;
	size_t e = s.size();
	while (e > b && (s[e-1] == ' ' || s[e-1] == '\t' || s[e-1] == '\r')) --e;
	return s.substr(b, e - b);
}

const char* StatusTextDefault(int code) {
	switch (code) {
		case 200: return "OK";
		case 202: return "Accepted";
		case 204: return "No Content";
		case 400: return "Bad Request";
		case 403: return "Forbidden";
		case 404: return "Not Found";
		case 405: return "Method Not Allowed";
		case 500: return "Internal Server Error";
		case 501: return "Not Implemented";
		default:  return "OK";
	}
}

}  // namespace http_detail
using namespace http_detail;

size_t CaseInsensitiveHash::operator()(const std::string& s) const noexcept {
	size_t h = 14695981039346656037ULL;  // FNV-1a 64-bit basis
	for (char c : s) {
		h ^= static_cast<size_t>(AsciiLower(c));
		h *= 1099511628211ULL;
	}
	return h;
}

bool CaseInsensitiveEqual::operator()(const std::string& a, const std::string& b) const noexcept {
	return CaseEq(a, b);
}

HttpRequest ParseRequest(const std::string& raw) {
	HttpRequest req;
	// Find the end of headers: CRLF CRLF.
	const auto hdrEnd = raw.find("\r\n\r\n");
	if (hdrEnd == std::string::npos) {
		throw std::runtime_error("HTTP: malformed request (no CRLF CRLF)");
	}
	const std::string headerBlock = raw.substr(0, hdrEnd);
	const size_t bodyStart = hdrEnd + 4;

	// Parse request line: METHOD SP path SP HTTP/1.1 CRLF
	const auto firstLineEnd = headerBlock.find("\r\n");
	const std::string requestLine = (firstLineEnd == std::string::npos)
		? headerBlock
		: headerBlock.substr(0, firstLineEnd);

	{
		const auto sp1 = requestLine.find(' ');
		const auto sp2 = (sp1 == std::string::npos)
			? std::string::npos
			: requestLine.find(' ', sp1 + 1);
		if (sp1 == std::string::npos || sp2 == std::string::npos) {
			throw std::runtime_error("HTTP: malformed request line");
		}
		req.method = requestLine.substr(0, sp1);
		req.path   = requestLine.substr(sp1 + 1, sp2 - (sp1 + 1));
	}

	// Headers — fold continuation lines aren't supported (deprecated by spec).
	size_t lineStart = (firstLineEnd == std::string::npos) ? headerBlock.size() : firstLineEnd + 2;
	while (lineStart < headerBlock.size()) {
		const auto lineEnd = headerBlock.find("\r\n", lineStart);
		const std::string line = (lineEnd == std::string::npos)
			? headerBlock.substr(lineStart)
			: headerBlock.substr(lineStart, lineEnd - lineStart);
		const auto colon = line.find(':');
		if (colon != std::string::npos) {
			std::string name  = Trim(line.substr(0, colon));
			std::string value = Trim(line.substr(colon + 1));
			req.headers[std::move(name)] = std::move(value);
		}
		if (lineEnd == std::string::npos) break;
		lineStart = lineEnd + 2;
	}

	// Body length per Content-Length header.
	size_t contentLength = 0;
	if (auto it = req.headers.find("Content-Length"); it != req.headers.end()) {
		try { contentLength = static_cast<size_t>(std::stoull(it->second)); }
		catch (...) { contentLength = 0; }
	}
	if (contentLength > 0) {
		if (bodyStart + contentLength > raw.size()) {
			throw std::runtime_error("HTTP: Content-Length exceeds available body bytes");
		}
		req.body = raw.substr(bodyStart, contentLength);
	}
	return req;
}

std::string FormatResponse(const HttpResponse& resp) {
	std::string txt = fmt::format("HTTP/1.1 {} {}\r\n",
								  resp.statusCode,
								  resp.statusText.empty()
									? StatusTextDefault(resp.statusCode)
									: resp.statusText);
	txt += fmt::format("Content-Type: {}\r\n", resp.contentType);
	txt += fmt::format("Content-Length: {}\r\n", resp.body.size());
	for (const auto& [k, v] : resp.headers) {
		// Don't double-emit headers we already wrote.
		if (CaseEq(k, "Content-Type") || CaseEq(k, "Content-Length")) continue;
		txt += fmt::format("{}: {}\r\n", k, v);
	}
	txt += "\r\n";
	txt += resp.body;
	return txt;
}

bool IsOriginAllowed(const HttpRequest& req) {
	auto it = req.headers.find("Origin");
	if (it == req.headers.end() || it->second.empty()) {
		// No Origin = non-browser client. Allow.
		return true;
	}
	const std::string& origin = it->second;

	// Parse "scheme://host[:port]" — only look at host.
	const auto schemeEnd = origin.find("://");
	if (schemeEnd == std::string::npos) {
		return false;
	}
	std::string rest = origin.substr(schemeEnd + 3);
	std::string host;
	if (!rest.empty() && rest.front() == '[') {
		// Bracketed IPv6: [::1] or [::1]:8000
		const auto closeBracket = rest.find(']');
		if (closeBracket == std::string::npos) return false;
		host = rest.substr(0, closeBracket + 1);  // include brackets
	} else {
		const auto colon = rest.find(':');
		host = (colon == std::string::npos) ? rest : rest.substr(0, colon);
	}

	return host == "localhost" || host == "127.0.0.1" || host == "[::1]";
}

HttpResponse Handle(const HttpRequest& req, Server& server, const std::string& mcpPath) {
	HttpResponse resp;

	if (!IsOriginAllowed(req)) {
		resp.statusCode = 403;
		resp.statusText = StatusTextDefault(403);
		resp.body = R"({"error":"forbidden origin"})";
		return resp;
	}

	if (req.path != mcpPath) {
		resp.statusCode = 404;
		resp.statusText = StatusTextDefault(404);
		resp.body = R"({"error":"not found"})";
		return resp;
	}

	if (req.method == "POST") {
		if (req.body.empty()) {
			resp.statusCode = 400;
			resp.statusText = StatusTextDefault(400);
			resp.body = R"({"error":"empty body"})";
			return resp;
		}
		nlohmann::json body;
		try {
			body = nlohmann::json::parse(req.body);
		} catch (const std::exception& e) {
			resp.statusCode = 400;
			resp.statusText = StatusTextDefault(400);
			resp.body = fmt::format(R"({{"error":"invalid JSON: {}"}})", e.what());
			return resp;
		}
		auto dispatched = server.Dispatch(body);
		if (!dispatched.has_value()) {
			// Notification — no response body per JSON-RPC.
			resp.statusCode = 202;
			resp.statusText = StatusTextDefault(202);
			return resp;
		}
		resp.body = dispatched->dump();
		return resp;
	}

	if (req.method == "GET") {
		// SSE long-poll is the spec-correct path for server→client
		// notifications, but it's OPTIONAL — the spec requires POST,
		// makes GET-for-SSE elective. Until C3-C5 ship the SSE socket
		// loop, return 405 Method Not Allowed (matching Epic 5.8's
		// transport) instead of 501. 501 = "we don't implement this
		// method ever" semantics, while GET-for-SSE may ship later;
		// 405 is the right code for "this endpoint doesn't accept this
		// method right now."
		resp.statusCode = 405;
		resp.statusText = StatusTextDefault(405);
		resp.headers["Allow"] = "POST, DELETE";
		resp.body = R"({"error":"GET not supported (SSE not implemented); use POST for request/response"})";
		return resp;
	}

	if (req.method == "DELETE") {
		// Sessions are stateless in this minimum implementation — just ack.
		resp.statusCode = 204;
		resp.statusText = StatusTextDefault(204);
		resp.body.clear();
		return resp;
	}

	resp.statusCode = 405;
	resp.statusText = StatusTextDefault(405);
	resp.headers["Allow"] = "POST, GET, DELETE";
	resp.body = R"({"error":"method not allowed"})";
	return resp;
}

}  // namespace bpr::jsonrpc::http
