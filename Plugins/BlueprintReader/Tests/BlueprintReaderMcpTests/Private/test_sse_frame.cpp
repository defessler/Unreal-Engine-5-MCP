// Phase 9 spike — SSE frame layer correctness.
//
// Validates the FormatSseFrame / FormatNotificationFrame / ParseSseStream
// roundtrip independent of any socket. The 13-15 day Phase 9 budget for
// HTTP+SSE rests on this layer being correct before we wire a TCP loop
// underneath.
//
// The test cases cover:
//   * canonical SSE format (event:/data:/blank-line terminator)
//   * JSON-RPC notification envelope (jsonrpc:2.0, method, params)
//   * multi-line data lines (defensive; nlohmann compact-dump shouldn't
//     produce newlines but pretty-print might)
//   * id field threading + Last-Event-ID resumption semantics
//   * retry: frame (client reconnect cadence)
//   * comment / keep-alive frames
//   * parser tolerance: CRLF, comments, empty input

#include <doctest/doctest.h>

#include "jsonrpc/SseFrame.h"

#include <nlohmann/json.hpp>
#include <string>

using namespace bpr::jsonrpc::http;
using nlohmann::json;

TEST_CASE("SSE: FormatSseFrame produces canonical event/data/terminator") {
	json body = {{"hello", "world"}};
	auto frame = FormatSseFrame("message", body);
	// Expected: "event: message\ndata: {\"hello\":\"world\"}\n\n"
	CHECK(frame.find("event: message\n") == 0);
	CHECK(frame.find("data: ") != std::string::npos);
	CHECK(frame.substr(frame.size() - 2) == "\n\n");
}

TEST_CASE("SSE: FormatSseFrame omits event field when empty") {
	json body = {{"x", 1}};
	auto frame = FormatSseFrame("", body);
	CHECK(frame.find("event:") == std::string::npos);
	CHECK(frame.find("data: ") == 0);
}

TEST_CASE("SSE: FormatSseFrame threads id field when provided") {
	json body = {{"k", "v"}};
	auto frame = FormatSseFrame("message", body, std::string("42"));
	CHECK(frame.find("event: message\n") != std::string::npos);
	CHECK(frame.find("id: 42\n") != std::string::npos);
}

TEST_CASE("SSE: FormatSseFrame skips empty id (no `id:` line)") {
	json body = {{"k", "v"}};
	auto frame = FormatSseFrame("message", body, std::string(""));
	CHECK(frame.find("id:") == std::string::npos);
}

TEST_CASE("SSE: FormatNotificationFrame wraps payload in jsonrpc envelope") {
	json params = {{"count", 5}};
	auto frame = FormatNotificationFrame(
		"notifications/tools/list_changed", params);
	CHECK(frame.find("event: message\n") == 0);
	CHECK(frame.find("\"jsonrpc\":\"2.0\"") != std::string::npos);
	CHECK(frame.find("\"method\":\"notifications/tools/list_changed\"")
		!= std::string::npos);
	CHECK(frame.find("\"params\":{\"count\":5}") != std::string::npos);
}

TEST_CASE("SSE: FormatRetryFrame emits retry: <ms>") {
	CHECK(FormatRetryFrame(3000) == "retry: 3000\n\n");
	CHECK(FormatRetryFrame(0)    == "retry: 0\n\n");
}

TEST_CASE("SSE: FormatCommentFrame emits :<text> with empty fallback") {
	CHECK(FormatCommentFrame()         == ":\n\n");
	CHECK(FormatCommentFrame("ping")   == ": ping\n\n");
}

TEST_CASE("SSE: ParseSseStream roundtrips a single-frame notification") {
	json params = {{"asset_path", "/Game/AI/BP_TestEnemy"}};
	auto wire = FormatNotificationFrame(
		"notifications/editor/asset_opened", params, std::string("1"));

	auto events = ParseSseStream(wire);
	REQUIRE(events.size() == 1);
	CHECK(events[0].event == "message");
	CHECK(events[0].id    == "1");

	json roundtrip = json::parse(events[0].data);
	CHECK(roundtrip["jsonrpc"] == "2.0");
	CHECK(roundtrip["method"]  == "notifications/editor/asset_opened");
	CHECK(roundtrip["params"]["asset_path"] == "/Game/AI/BP_TestEnemy");
}

TEST_CASE("SSE: ParseSseStream handles multi-frame stream") {
	std::string wire;
	wire += FormatNotificationFrame("notifications/editor/pie_started",
									  json::object(), std::string("1"));
	wire += FormatNotificationFrame("notifications/editor/pie_stopped",
									  json::object(), std::string("2"));

	auto events = ParseSseStream(wire);
	REQUIRE(events.size() == 2);
	CHECK(events[0].id == "1");
	CHECK(events[1].id == "2");
	CHECK(json::parse(events[0].data)["method"]
		== "notifications/editor/pie_started");
	CHECK(json::parse(events[1].data)["method"]
		== "notifications/editor/pie_stopped");
}

TEST_CASE("SSE: ParseSseStream tolerates CRLF line endings") {
	// Some HTTP clients (curl with --output-format) might re-emit \r\n.
	std::string wire =
		"event: message\r\n"
		"data: {\"x\":1}\r\n"
		"\r\n";
	auto events = ParseSseStream(wire);
	REQUIRE(events.size() == 1);
	CHECK(events[0].event == "message");
	CHECK(events[0].data  == "{\"x\":1}");
}

TEST_CASE("SSE: ParseSseStream skips comment lines (heartbeats)") {
	std::string wire =
		": keep-alive\n"
		"\n"
		"event: message\n"
		"data: {\"k\":\"v\"}\n"
		"\n"
		":\n"
		"\n";
	auto events = ParseSseStream(wire);
	REQUIRE(events.size() == 1);
	CHECK(json::parse(events[0].data) == json{{"k", "v"}});
}

TEST_CASE("SSE: ParseSseStream returns empty on empty input") {
	CHECK(ParseSseStream("").empty());
	CHECK(ParseSseStream(":\n\n").empty());  // comment-only
}

TEST_CASE("SSE: defensive — multi-line data joined back to one payload") {
	// Hand-built frame with embedded \n in JSON (atypical but valid).
	std::string wire =
		"event: message\n"
		"data: {\n"
		"data:   \"k\": \"v\"\n"
		"data: }\n"
		"\n";
	auto events = ParseSseStream(wire);
	REQUIRE(events.size() == 1);
	// data lines joined with \n between them
	CHECK(events[0].data.find("\"k\"") != std::string::npos);
	CHECK(events[0].data.find("\"v\"") != std::string::npos);
}

TEST_CASE("SSE: FormatSseFrame compact-encodes nested JSON without breaking") {
	json body = {
		{"jsonrpc", "2.0"},
		{"method",  "notifications/editor/level_actor_selection_changed"},
		{"params", {
			{"added",   json::array({"Cube_1", "Cube_2"})},
			{"removed", json::array()},
			{"current", json::array({"Cube_1", "Cube_2", "Light_1"})},
		}},
	};
	auto frame = FormatSseFrame("message", body);
	// Parse back
	auto events = ParseSseStream(frame);
	REQUIRE(events.size() == 1);
	json parsed = json::parse(events[0].data);
	CHECK(parsed["params"]["added"].size()   == 2);
	CHECK(parsed["params"]["removed"].size() == 0);
	CHECK(parsed["params"]["current"].size() == 3);
	CHECK(parsed["params"]["current"][2] == "Light_1");
}
