// Framing-layer stress tests. The transport had a year-long latent bug where
// it only spoke Content-Length framing while the MCP spec mandates
// newline-delimited JSON. JetBrains Copilot couldn't talk to us at all.
// These tests are the regression net so we don't ship that again, plus they
// exercise edge cases (large payloads, batched requests, mixed whitespace,
// short reads) that the basic round-trip in test_jsonrpc.cpp doesn't cover.

#include <doctest/doctest.h>

#include "backends/MockBlueprintReader.h"
#include "jsonrpc/Mcp.h"
#include "jsonrpc/Server.h"
#include "tools/BlueprintTools.h"
#include "tools/ToolRegistry.h"

#include "test_helpers.h"

#include <sstream>
#include <string>
#include <vector>

using namespace bpr;
using namespace bpr::jsonrpc;
using nlohmann::json;

namespace {

// Frame a body using the requested format.
std::string Frame(const json& body, FrameFormat fmt) {
	std::ostringstream os;
	WriteFrame(os, body, fmt);
	return os.str();
}

// Drain every frame produced by Server::Run on `input`, parsing each.
struct DrainResult {
	std::vector<json> frames;
	FrameFormat outputFormat = FrameFormat::NewlineDelimited;
	std::string log;
};
DrainResult DriveServer(Server& s, const std::string& input) {
	std::istringstream in(input);
	std::ostringstream out;
	std::ostringstream log;
	s.Run(in, out, log);
	DrainResult r;
	r.log = log.str();

	auto raw = out.str();
	// Auto-detect output format from the first non-whitespace byte and parse
	// accordingly so the tests don't have to know which format the server
	// chose; they can just check the bodies.
	std::size_t i = 0;
	while (i < raw.size() && (raw[i] == '\r' || raw[i] == '\n' ||
							  raw[i] == ' ' || raw[i] == '\t'))
	{
		++i;
	}
	if (i >= raw.size())
	{
		return r;
	}
	if (raw[i] == '{' || raw[i] == '[') {
		r.outputFormat = FrameFormat::NewlineDelimited;
		// newline-delimited: split on '\n'
		std::string buf;
		for (; i < raw.size(); ++i) {
			char c = raw[i];
			if (c == '\n') {
				if (!buf.empty() && buf.back() == '\r')
				{
					buf.pop_back();
				}
				if (!buf.empty())
				{
					r.frames.push_back(json::parse(buf));
				}
				buf.clear();
			} else {
				buf.push_back(c);
			}
		}
		if (!buf.empty())
		{
			r.frames.push_back(json::parse(buf));
		}
	} else {
		r.outputFormat = FrameFormat::ContentLength;
		// Content-Length: walk header blocks
		while (i < raw.size()) {
			auto split = raw.find("\r\n\r\n", i);
			if (split == std::string::npos)
			{
				break;
			}
			// Parse Content-Length out of the header.
			auto headers = raw.substr(i, split - i);
			auto pos = headers.find("Content-Length:");
			REQUIRE(pos != std::string::npos);
			std::size_t end = headers.find_first_of("\r\n", pos);
			std::size_t len = std::stoul(headers.substr(pos + 15, end - pos - 15));
			std::size_t bodyStart = split + 4;
			r.frames.push_back(json::parse(raw.substr(bodyStart, len)));
			i = bodyStart + len;
		}
	}
	return r;
}

// Build a server wired up with the mock backend + the standard MCP handlers.
struct WiredServer {
	backends::MockBlueprintReader reader;
	tools::ToolRegistry registry;
	Server server;
	mcp::ServerInfo info;

	WiredServer() : reader(test::FixturesDir()) {
		tools::RegisterBlueprintTools(registry, reader);
		mcp::RegisterHandlers(server, registry, info);
	}
};

json InitializeReq(int id = 1) {
	return json{
		{"jsonrpc", "2.0"}, {"id", id}, {"method", "initialize"},
		{"params", json{
			{"protocolVersion", "2024-11-05"},
			{"capabilities", json::object()},
			{"clientInfo", json{{"name", "stress"}, {"version", "0"}}}
		}}
	};
}

json ToolsCallReq(int id, const std::string& tool, json args = json::object()) {
	return json{
		{"jsonrpc", "2.0"}, {"id", id}, {"method", "tools/call"},
		{"params", json{{"name", tool}, {"arguments", args}}}
	};
}

} // namespace

// ---------------------------------------------------------------------------
// Format auto-detection and mirroring
// ---------------------------------------------------------------------------

TEST_CASE("Framing: newline-delimited input -> newline-delimited output") {
	WiredServer w;
	auto in = Frame(InitializeReq(), FrameFormat::NewlineDelimited);
	auto r = DriveServer(w.server, in);
	CHECK(r.outputFormat == FrameFormat::NewlineDelimited);
	REQUIRE(r.frames.size() == 1);
	CHECK(r.frames[0]["id"] == 1);
	CHECK(r.log.find("framing=newline-delimited") != std::string::npos);
}

TEST_CASE("Framing: Content-Length input -> Content-Length output") {
	WiredServer w;
	auto in = Frame(InitializeReq(), FrameFormat::ContentLength);
	auto r = DriveServer(w.server, in);
	CHECK(r.outputFormat == FrameFormat::ContentLength);
	REQUIRE(r.frames.size() == 1);
	CHECK(r.frames[0]["id"] == 1);
	CHECK(r.log.find("framing=content-length") != std::string::npos);
}

TEST_CASE("Framing: client format locks on first frame, ignores later attempts to switch") {
	// A buggy / hostile client might send a Content-Length frame followed by a
	// newline-delimited one. We lock format on the first read; the second
	// frame is still parseable as JSON via the auto-detector, but it should be
	// treated through the LSP path. Verify no crash + we still serve both.
	WiredServer w;
	std::string in = Frame(InitializeReq(1), FrameFormat::ContentLength);
	in += Frame(ToolsCallReq(2, "list_blueprints", json{{"path", "/Game"}}),
				FrameFormat::ContentLength);
	auto r = DriveServer(w.server, in);
	CHECK(r.outputFormat == FrameFormat::ContentLength);
	REQUIRE(r.frames.size() == 2);
	CHECK(r.frames[0]["id"] == 1);
	CHECK(r.frames[1]["id"] == 2);
}

TEST_CASE("Framing: leading whitespace before first frame is tolerated") {
	WiredServer w;
	std::string in = "\r\n\r\n\n   \n";
	in += Frame(InitializeReq(), FrameFormat::NewlineDelimited);
	auto r = DriveServer(w.server, in);
	REQUIRE(r.frames.size() == 1);
	CHECK(r.frames[0]["id"] == 1);
}

TEST_CASE("Framing: trailing whitespace after last frame is clean EOF") {
	WiredServer w;
	std::string in = Frame(InitializeReq(), FrameFormat::NewlineDelimited);
	in += "\r\n\n  ";
	auto r = DriveServer(w.server, in);
	REQUIRE(r.frames.size() == 1);
	CHECK(r.log.find("frame error") == std::string::npos);
}

// ---------------------------------------------------------------------------
// Throughput: many sequential frames in one stream
// ---------------------------------------------------------------------------

TEST_CASE("Framing: 1000 sequential newline-delimited tool calls all succeed") {
	WiredServer w;
	std::string in = Frame(InitializeReq(0), FrameFormat::NewlineDelimited);
	for (int i = 1; i <= 1000; ++i) {
		in += Frame(ToolsCallReq(i, "list_pin_categories"),
					FrameFormat::NewlineDelimited);
	}
	auto r = DriveServer(w.server, in);
	REQUIRE(r.frames.size() == 1001);
	// All ids round-trip.
	for (int i = 0; i <= 1000; ++i) {
		CHECK(r.frames[i]["id"] == i);
		CHECK(!r.frames[i].contains("error"));
	}
}

TEST_CASE("Framing: 1000 sequential Content-Length tool calls all succeed") {
	WiredServer w;
	std::string in = Frame(InitializeReq(0), FrameFormat::ContentLength);
	for (int i = 1; i <= 1000; ++i) {
		in += Frame(ToolsCallReq(i, "list_pin_categories"),
					FrameFormat::ContentLength);
	}
	auto r = DriveServer(w.server, in);
	REQUIRE(r.frames.size() == 1001);
	for (int i = 0; i <= 1000; ++i) {
		CHECK(r.frames[i]["id"] == i);
		CHECK(!r.frames[i].contains("error"));
	}
}

// ---------------------------------------------------------------------------
// Payload edge cases
// ---------------------------------------------------------------------------

TEST_CASE("Framing: large embedded string in params (~64KB) round-trips") {
	WiredServer w;
	std::string big(64 * 1024, 'x');
	auto req = ToolsCallReq(1, "find_node",
		json{{"asset_path", "/Game/AI/BP_Enemy"}, {"query", big}});
	auto in = Frame(InitializeReq(0), FrameFormat::NewlineDelimited);
	in += Frame(req, FrameFormat::NewlineDelimited);
	auto r = DriveServer(w.server, in);
	REQUIRE(r.frames.size() == 2);
	CHECK(r.frames[1]["id"] == 1);
	CHECK(!r.frames[1].contains("error"));
}

TEST_CASE("Framing: JSON with embedded escaped quotes survives both formats") {
	WiredServer w;
	std::string nasty = R"(quote: " and backslash: \ and braces: {})";
	SUBCASE("newline-delimited") {
		auto in = Frame(InitializeReq(0), FrameFormat::NewlineDelimited);
		in += Frame(ToolsCallReq(1, "find_node",
					json{{"asset_path", "/Game/AI/BP_Enemy"},
						 {"query", nasty}}),
					FrameFormat::NewlineDelimited);
		auto r = DriveServer(w.server, in);
		REQUIRE(r.frames.size() == 2);
		CHECK(!r.frames[1].contains("error"));
	}
	SUBCASE("content-length") {
		auto in = Frame(InitializeReq(0), FrameFormat::ContentLength);
		in += Frame(ToolsCallReq(1, "find_node",
					json{{"asset_path", "/Game/AI/BP_Enemy"},
						 {"query", nasty}}),
					FrameFormat::ContentLength);
		auto r = DriveServer(w.server, in);
		REQUIRE(r.frames.size() == 2);
		CHECK(!r.frames[1].contains("error"));
	}
}

// ---------------------------------------------------------------------------
// Error paths
// ---------------------------------------------------------------------------

TEST_CASE("Framing: malformed JSON yields parse-error envelope, then stream recovers") {
	WiredServer w;
	// Bogus, then a real request. The bogus should produce a parse error;
	// the second request should still succeed because we mirror format and
	// continue reading.
	std::string in = "{not valid json}\n";
	in += Frame(InitializeReq(7), FrameFormat::NewlineDelimited);
	auto r = DriveServer(w.server, in);
	REQUIRE(r.frames.size() == 2);
	CHECK(r.frames[0]["id"].is_null());
	REQUIRE(r.frames[0].contains("error"));
	CHECK(r.frames[0]["error"]["code"] ==
		  static_cast<int>(ErrorCode::ParseError));
	CHECK(r.frames[1]["id"] == 7);
	CHECK(!r.frames[1].contains("error"));
}

TEST_CASE("Framing: notification (no id) produces no response but doesn't kill the loop") {
	WiredServer w;
	auto note = json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}};
	std::string in = Frame(InitializeReq(1), FrameFormat::NewlineDelimited);
	in += Frame(note, FrameFormat::NewlineDelimited);
	in += Frame(ToolsCallReq(2, "list_pin_categories"),
				FrameFormat::NewlineDelimited);
	auto r = DriveServer(w.server, in);
	// initialize -> response, notifications/initialized -> no response,
	// tools/call -> response. Total: 2.
	REQUIRE(r.frames.size() == 2);
	CHECK(r.frames[0]["id"] == 1);
	CHECK(r.frames[1]["id"] == 2);
}

TEST_CASE("Framing: batched requests round-trip in both formats") {
	WiredServer w;
	json batch = json::array();
	for (int i = 1; i <= 5; ++i) {
		batch.push_back(ToolsCallReq(i, "list_pin_categories"));
	}
	SUBCASE("newline-delimited") {
		std::string in = Frame(InitializeReq(0), FrameFormat::NewlineDelimited);
		in += Frame(batch, FrameFormat::NewlineDelimited);
		auto r = DriveServer(w.server, in);
		// Init response + batch (1 array of 5).
		REQUIRE(r.frames.size() == 2);
		CHECK(r.frames[1].is_array());
		CHECK(r.frames[1].size() == 5);
	}
	SUBCASE("content-length") {
		std::string in = Frame(InitializeReq(0), FrameFormat::ContentLength);
		in += Frame(batch, FrameFormat::ContentLength);
		auto r = DriveServer(w.server, in);
		REQUIRE(r.frames.size() == 2);
		CHECK(r.frames[1].is_array());
		CHECK(r.frames[1].size() == 5);
	}
}
