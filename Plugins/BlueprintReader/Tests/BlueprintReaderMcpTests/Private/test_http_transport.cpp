// Tests for the Streamable HTTP transport layer (request parse,
// response format, dispatch routing, Origin guard).
//
// Socket layer is intentionally not exercised here — these tests drive
// the transport's pure functions against raw HTTP strings, mirroring
// how jsonrpc::Server::Dispatch is tested without an actual stdio pipe.

#include <doctest/doctest.h>

#include "jsonrpc/HttpTransport.h"
#include "jsonrpc/Mcp.h"
#include "jsonrpc/Server.h"
#include "tools/BlueprintTools.h"
#include "tools/ToolRegistry.h"

#include "backends/MockBlueprintReader.h"
#include "test_helpers.h"

using namespace bpr;
using nlohmann::json;

TEST_CASE("HTTP: parses POST request with Content-Length body") {
	const std::string raw =
		"POST /mcp HTTP/1.1\r\n"
		"Host: localhost:8000\r\n"
		"Content-Type: application/json\r\n"
		"Content-Length: 17\r\n"
		"\r\n"
		R"({"jsonrpc":"2.0"})";
	auto req = jsonrpc::http::ParseRequest(raw);
	CHECK(req.method == "POST");
	CHECK(req.path == "/mcp");
	CHECK(req.body == R"({"jsonrpc":"2.0"})");
	CHECK(req.headers.at("Content-Length") == "17");
	// Header lookup is case-insensitive
	CHECK(req.headers.at("content-length") == "17");
	CHECK(req.headers.at("CONTENT-LENGTH") == "17");
}

TEST_CASE("HTTP: ParseRequest throws on malformed input") {
	CHECK_THROWS_AS(jsonrpc::http::ParseRequest("not-an-http-request"), std::runtime_error);
}

TEST_CASE("HTTP: Origin allowlist accepts localhost variants, rejects others") {
	jsonrpc::http::HttpRequest r;
	CHECK(jsonrpc::http::IsOriginAllowed(r));  // no Origin header
	r.headers["Origin"] = "http://localhost:8000";
	CHECK(jsonrpc::http::IsOriginAllowed(r));
	r.headers["Origin"] = "http://127.0.0.1";
	CHECK(jsonrpc::http::IsOriginAllowed(r));
	r.headers["Origin"] = "http://[::1]:8000";
	CHECK(jsonrpc::http::IsOriginAllowed(r));
	r.headers["Origin"] = "http://example.com";
	CHECK_FALSE(jsonrpc::http::IsOriginAllowed(r));
	r.headers["Origin"] = "https://attacker.com:8000";
	CHECK_FALSE(jsonrpc::http::IsOriginAllowed(r));
}

TEST_CASE("HTTP: POST /mcp dispatches a JSON-RPC initialize and returns the result") {
	auto reader = test::MakeMockReader();
	tools::ToolRegistry registry;
	tools::RegisterBlueprintTools(registry, reader);
	jsonrpc::Server server;
	mcp::ServerInfo info;
	mcp::RegisterHandlers(server, registry, info);

	jsonrpc::http::HttpRequest req;
	req.method = "POST";
	req.path = "/mcp";
	req.headers["Content-Type"] = "application/json";
	req.body = R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"test","version":"0"}}})";

	auto resp = jsonrpc::http::Handle(req, server, "/mcp");
	CHECK(resp.statusCode == 200);
	CHECK(resp.contentType == "application/json");
	auto body = json::parse(resp.body);
	CHECK(body["id"] == 1);
	CHECK(body["result"]["protocolVersion"] == "2025-06-18");
	CHECK(body["result"]["serverInfo"]["name"] == "bp-reader-mcp");
}

TEST_CASE("HTTP: POST without body returns 400") {
	jsonrpc::Server server;
	jsonrpc::http::HttpRequest req;
	req.method = "POST";
	req.path = "/mcp";
	auto resp = jsonrpc::http::Handle(req, server, "/mcp");
	CHECK(resp.statusCode == 400);
}

TEST_CASE("HTTP: wrong path returns 404") {
	jsonrpc::Server server;
	jsonrpc::http::HttpRequest req;
	req.method = "POST";
	req.path = "/not-mcp";
	req.body = "{}";
	auto resp = jsonrpc::http::Handle(req, server, "/mcp");
	CHECK(resp.statusCode == 404);
}

TEST_CASE("HTTP: disallowed Origin returns 403") {
	jsonrpc::Server server;
	jsonrpc::http::HttpRequest req;
	req.method = "POST";
	req.path = "/mcp";
	req.headers["Origin"] = "http://evil.com";
	req.body = "{}";
	auto resp = jsonrpc::http::Handle(req, server, "/mcp");
	CHECK(resp.statusCode == 403);
}

TEST_CASE("HTTP: GET returns 405 with Allow header (SSE optional, not implemented)") {
	// Per MCP spec, GET-for-SSE is optional; until C3-C5 ship SSE we
	// return 405 (matching Epic 5.8) rather than 501, since SSE may
	// land later and 501 implies "never." Header lists the methods
	// the endpoint actually accepts right now.
	jsonrpc::Server server;
	jsonrpc::http::HttpRequest req;
	req.method = "GET";
	req.path = "/mcp";
	auto resp = jsonrpc::http::Handle(req, server, "/mcp");
	CHECK(resp.statusCode == 405);
	CHECK(resp.headers.at("Allow") == "POST, DELETE");
}

TEST_CASE("HTTP: DELETE returns 204 (session ack)") {
	jsonrpc::Server server;
	jsonrpc::http::HttpRequest req;
	req.method = "DELETE";
	req.path = "/mcp";
	auto resp = jsonrpc::http::Handle(req, server, "/mcp");
	CHECK(resp.statusCode == 204);
}

TEST_CASE("HTTP: unsupported method returns 405 with Allow header") {
	jsonrpc::Server server;
	jsonrpc::http::HttpRequest req;
	req.method = "PUT";
	req.path = "/mcp";
	auto resp = jsonrpc::http::Handle(req, server, "/mcp");
	CHECK(resp.statusCode == 405);
	CHECK(resp.headers.at("Allow") == "POST, GET, DELETE");
}

TEST_CASE("HTTP: FormatResponse builds spec-compliant HTTP/1.1 response") {
	jsonrpc::http::HttpResponse resp;
	resp.statusCode = 200;
	resp.statusText = "OK";
	resp.body = R"({"ok":true})";
	auto wire = jsonrpc::http::FormatResponse(resp);
	CHECK(wire.find("HTTP/1.1 200 OK\r\n") == 0);
	CHECK(wire.find("Content-Type: application/json\r\n") != std::string::npos);
	CHECK(wire.find("Content-Length: 11\r\n") != std::string::npos);
	CHECK(wire.find(R"({"ok":true})") != std::string::npos);
}
