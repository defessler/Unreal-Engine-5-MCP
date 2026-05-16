// Tests for MCP protocolVersion negotiation. Spec says: server should echo
// the client's protocolVersion in the initialize response when the version
// is one the server supports; otherwise return its preferred version.
//
// We had previously hardcoded "2024-11-05" — the new behavior accepts any
// version in our known list, falling back to the default for unknowns.

#include <doctest/doctest.h>

#include "backends/MockBlueprintReader.h"
#include "jsonrpc/Mcp.h"
#include "jsonrpc/Server.h"
#include "tools/BlueprintTools.h"
#include "tools/ToolRegistry.h"

#include "test_helpers.h"

#include <sstream>

using namespace bpr;
using namespace bpr::jsonrpc;
using nlohmann::json;

namespace {

json DriveInitialize(const json& clientProtocolVersion) {
	auto reader = test::MakeMockReader();
	tools::ToolRegistry registry;
	tools::RegisterBlueprintTools(registry, reader);
	Server server;
	mcp::ServerInfo info;
	mcp::RegisterHandlers(server, registry, info);

	std::ostringstream framed;
	json req = {
		{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"},
		{"params", json{
			{"protocolVersion", clientProtocolVersion},
			{"capabilities", json::object()},
			{"clientInfo", json{{"name", "test"}, {"version", "0"}}}}}};
	WriteFrame(framed, req, FrameFormat::NewlineDelimited);

	std::istringstream in(framed.str());
	std::ostringstream out;
	std::ostringstream log;
	server.Run(in, out, log);

	auto raw = out.str();
	auto nl = raw.find('\n');
	REQUIRE(nl != std::string::npos);
	return json::parse(raw.substr(0, nl));
}

} // namespace

TEST_CASE("initialize: echoes the client's protocolVersion when known") {
	auto resp = DriveInitialize("2025-03-26");
	CHECK(resp["result"]["protocolVersion"] == "2025-03-26");
}

TEST_CASE("initialize: echoes 2024-11-05 (the original spec)") {
	auto resp = DriveInitialize("2024-11-05");
	CHECK(resp["result"]["protocolVersion"] == "2024-11-05");
}

TEST_CASE("initialize: unknown version falls back to server default") {
	auto resp = DriveInitialize("9999-99-99");
	// Server default is 2024-11-05 (mcp::ServerInfo's hardcoded default).
	CHECK(resp["result"]["protocolVersion"] == "2024-11-05");
}

TEST_CASE("initialize: missing protocolVersion in params -> server default") {
	auto resp = DriveInitialize(json(nullptr));
	CHECK(resp["result"]["protocolVersion"] == "2024-11-05");
}

TEST_CASE("initialize: echoes the most recent known spec (2025-06-18)") {
	auto resp = DriveInitialize("2025-06-18");
	CHECK(resp["result"]["protocolVersion"] == "2025-06-18");
}
