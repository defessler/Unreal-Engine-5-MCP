// Phase 6 tests: MCP logging primitive — Logger class behaviour +
// logging/setLevel JSON-RPC handler + capability advertisement +
// notifications/message emission.

#include <doctest/doctest.h>

#include "backends/MockBlueprintReader.h"
#include "jsonrpc/Mcp.h"
#include "jsonrpc/Server.h"
#include "tools/BlueprintTools.h"
#include "tools/Logger.h"
#include "tools/Prompts.h"
#include "tools/ToolRegistry.h"

#include "test_helpers.h"

#include <sstream>
#include <string>
#include <vector>

using namespace bpr;
using nlohmann::json;

namespace test_logging_detail {

std::string FrameJson(const json& body) {
	std::ostringstream os;
	jsonrpc::WriteFrame(os, body);
	return os.str();
}

std::vector<json> ReadAllFrames(std::istream& in) {
	std::vector<json> out;
	while (true) {
		auto raw = jsonrpc::ReadFrame(in);
		if (!raw) break;
		out.push_back(json::parse(*raw));
	}
	return out;
}

}    // namespace test_logging_detail
using namespace test_logging_detail;

// =====================================================================
// LogLevel parsing
// =====================================================================

TEST_CASE("LogLevel: name round-trip for the 8 RFC 5424 levels") {
	for (auto l : {tools::LogLevel::Debug,
				   tools::LogLevel::Info,
				   tools::LogLevel::Notice,
				   tools::LogLevel::Warning,
				   tools::LogLevel::Error,
				   tools::LogLevel::Critical,
				   tools::LogLevel::Alert,
				   tools::LogLevel::Emergency}) {
		const std::string name(tools::LogLevelName(l));
		tools::LogLevel parsed;
		REQUIRE(tools::TryParseLogLevel(name, parsed));
		CHECK(parsed == l);
	}
}

TEST_CASE("LogLevel: case-insensitive parsing + 3 common aliases") {
	tools::LogLevel parsed;
	CHECK(tools::TryParseLogLevel("DEBUG", parsed));
	CHECK(parsed == tools::LogLevel::Debug);
	CHECK(tools::TryParseLogLevel("Info", parsed));
	CHECK(parsed == tools::LogLevel::Info);
	// Common shortened aliases — clients may send these.
	CHECK(tools::TryParseLogLevel("warn", parsed));
	CHECK(parsed == tools::LogLevel::Warning);
	CHECK(tools::TryParseLogLevel("crit", parsed));
	CHECK(parsed == tools::LogLevel::Critical);
	CHECK(tools::TryParseLogLevel("emerg", parsed));
	CHECK(parsed == tools::LogLevel::Emergency);
}

TEST_CASE("LogLevel: unknown names rejected") {
	tools::LogLevel parsed;
	CHECK_FALSE(tools::TryParseLogLevel("foobar", parsed));
	CHECK_FALSE(tools::TryParseLogLevel("", parsed));
	CHECK_FALSE(tools::TryParseLogLevel("trace", parsed));  // not RFC 5424
}

// =====================================================================
// Logger behaviour (no Server attached)
// =====================================================================

TEST_CASE("Logger: default level is Info; WouldEmit gates correctly") {
	tools::Logger logger(nullptr);  // null server is fine for WouldEmit
	CHECK(logger.GetLevel() == tools::LogLevel::Info);
	CHECK_FALSE(logger.WouldEmit(tools::LogLevel::Debug));
	CHECK(logger.WouldEmit(tools::LogLevel::Info));
	CHECK(logger.WouldEmit(tools::LogLevel::Warning));
	CHECK(logger.WouldEmit(tools::LogLevel::Emergency));
}

TEST_CASE("Logger: SetLevel raises/lowers the emit filter") {
	tools::Logger logger(nullptr);
	logger.SetLevel(tools::LogLevel::Warning);
	CHECK_FALSE(logger.WouldEmit(tools::LogLevel::Info));
	CHECK(logger.WouldEmit(tools::LogLevel::Warning));
	logger.SetLevel(tools::LogLevel::Debug);
	CHECK(logger.WouldEmit(tools::LogLevel::Debug));
	CHECK(logger.WouldEmit(tools::LogLevel::Info));
}

TEST_CASE("Logger: SetLevelFromString accepts 8 levels + off + aliases") {
	tools::Logger logger(nullptr);
	CHECK(logger.SetLevelFromString("debug"));
	CHECK(logger.GetLevel() == tools::LogLevel::Debug);
	CHECK(logger.SetLevelFromString("WARN"));
	CHECK(logger.GetLevel() == tools::LogLevel::Warning);
	CHECK(logger.SetLevelFromString("off"));
	CHECK(logger.IsDisabled());
	CHECK_FALSE(logger.WouldEmit(tools::LogLevel::Emergency));
	// `none` and `0` also disable.
	logger.SetLevel(tools::LogLevel::Info);  // re-enable via SetLevel
	CHECK_FALSE(logger.IsDisabled());
	CHECK(logger.SetLevelFromString("none"));
	CHECK(logger.IsDisabled());
	// Unrecognised → false, no change.
	logger.SetLevel(tools::LogLevel::Warning);
	CHECK_FALSE(logger.SetLevelFromString("xyzzy"));
	CHECK(logger.GetLevel() == tools::LogLevel::Warning);
}

TEST_CASE("Logger: Disable suppresses emission even at Emergency severity") {
	tools::Logger logger(nullptr);
	logger.Disable();
	CHECK(logger.IsDisabled());
	CHECK_FALSE(logger.WouldEmit(tools::LogLevel::Debug));
	CHECK_FALSE(logger.WouldEmit(tools::LogLevel::Emergency));
}

// =====================================================================
// Logger -> Server notification emission
// =====================================================================

TEST_CASE("Logger: emits notifications/message via Server::QueueNotification") {
	jsonrpc::Server server;
	tools::Logger logger(&server);

	logger.Log(tools::LogLevel::Info, std::string("hello world"));
	auto pending = server.TakePendingNotifications();
	REQUIRE(pending.size() == 1);
	CHECK(pending[0]["method"] == "notifications/message");
	REQUIRE(pending[0].contains("params"));
	CHECK(pending[0]["params"]["level"] == "info");
	CHECK(pending[0]["params"]["data"] == "hello world");
	CHECK_FALSE(pending[0]["params"].contains("logger"));
}

TEST_CASE("Logger: optional logger name appears as `logger` field") {
	jsonrpc::Server server;
	tools::Logger logger(&server);
	logger.Log(tools::LogLevel::Warning, json{{"k", "v"}}, "backend");
	auto pending = server.TakePendingNotifications();
	REQUIRE(pending.size() == 1);
	CHECK(pending[0]["params"]["logger"] == "backend");
	CHECK(pending[0]["params"]["level"] == "warning");
	CHECK(pending[0]["params"]["data"]["k"] == "v");
}

TEST_CASE("Logger: below-filter logs don't reach the Server queue") {
	jsonrpc::Server server;
	tools::Logger logger(&server);
	logger.SetLevel(tools::LogLevel::Warning);
	logger.Log(tools::LogLevel::Info,    std::string("should be filtered"));
	logger.Log(tools::LogLevel::Debug,   std::string("should be filtered"));
	logger.Log(tools::LogLevel::Warning, std::string("should pass"));
	auto pending = server.TakePendingNotifications();
	REQUIRE(pending.size() == 1);
	CHECK(pending[0]["params"]["data"] == "should pass");
}

TEST_CASE("Logger: disabled state suppresses everything") {
	jsonrpc::Server server;
	tools::Logger logger(&server);
	logger.Disable();
	logger.Log(tools::LogLevel::Emergency, std::string("world is ending"));
	auto pending = server.TakePendingNotifications();
	CHECK(pending.empty());
}

// =====================================================================
// MCP capability + dispatch wiring
// =====================================================================

TEST_CASE("MCP initialize advertises logging capability when Logger provided") {
	auto reader = test::MakeMockReader();
	tools::ToolRegistry toolReg;
	tools::RegisterBlueprintTools(toolReg, reader);
	tools::prompts::PromptRegistry prompts;
	tools::prompts::RegisterBuiltinPrompts(prompts);

	jsonrpc::Server server;
	tools::Logger logger(&server);
	mcp::ServerInfo info;
	mcp::RegisterHandlers(server, toolReg, &prompts, &logger, info);

	std::string in = FrameJson(json{
		{"jsonrpc","2.0"}, {"id",1}, {"method","initialize"},
		{"params", json{{"protocolVersion","2025-06-18"},
						{"capabilities", json::object()},
						{"clientInfo", json{{"name","t"},{"version","0"}}}}}});
	std::istringstream is(in);
	std::ostringstream os, log;
	server.Run(is, os, log);
	std::istringstream replay(os.str());
	auto frames = ReadAllFrames(replay);
	REQUIRE(frames.size() == 1);
	REQUIRE(frames[0]["result"]["capabilities"].contains("logging"));
	// Spec says the value is an empty object; assert that.
	CHECK(frames[0]["result"]["capabilities"]["logging"].is_object());
	CHECK(frames[0]["result"]["capabilities"]["logging"].empty());
}

TEST_CASE("MCP initialize omits logging capability when no Logger provided") {
	auto reader = test::MakeMockReader();
	tools::ToolRegistry toolReg;
	tools::RegisterBlueprintTools(toolReg, reader);
	tools::prompts::PromptRegistry prompts;
	tools::prompts::RegisterBuiltinPrompts(prompts);

	jsonrpc::Server server;
	mcp::ServerInfo info;
	// 3-arg overload — no logger.
	mcp::RegisterHandlers(server, toolReg, prompts, info);

	std::string in = FrameJson(json{
		{"jsonrpc","2.0"}, {"id",1}, {"method","initialize"},
		{"params", json{{"protocolVersion","2025-06-18"},
						{"capabilities", json::object()},
						{"clientInfo", json{{"name","t"},{"version","0"}}}}}});
	std::istringstream is(in);
	std::ostringstream os, log;
	server.Run(is, os, log);
	std::istringstream replay(os.str());
	auto frames = ReadAllFrames(replay);
	REQUIRE(frames.size() == 1);
	CHECK_FALSE(frames[0]["result"]["capabilities"].contains("logging"));
}

TEST_CASE("MCP logging/setLevel: valid level changes filter") {
	auto reader = test::MakeMockReader();
	tools::ToolRegistry toolReg;
	tools::RegisterBlueprintTools(toolReg, reader);

	jsonrpc::Server server;
	tools::Logger logger(&server);
	mcp::ServerInfo info;
	mcp::RegisterHandlers(server, toolReg, /*prompts=*/nullptr, &logger, info);

	CHECK(logger.GetLevel() == tools::LogLevel::Info);
	std::string in = FrameJson(json{
		{"jsonrpc","2.0"}, {"id",1}, {"method","logging/setLevel"},
		{"params", json{{"level", "debug"}}}});
	std::istringstream is(in);
	std::ostringstream os, log;
	server.Run(is, os, log);
	CHECK(logger.GetLevel() == tools::LogLevel::Debug);
}

TEST_CASE("MCP logging/setLevel: unknown level returns InvalidParams") {
	auto reader = test::MakeMockReader();
	tools::ToolRegistry toolReg;
	tools::RegisterBlueprintTools(toolReg, reader);

	jsonrpc::Server server;
	tools::Logger logger(&server);
	mcp::ServerInfo info;
	mcp::RegisterHandlers(server, toolReg, /*prompts=*/nullptr, &logger, info);

	std::string in = FrameJson(json{
		{"jsonrpc","2.0"}, {"id",1}, {"method","logging/setLevel"},
		{"params", json{{"level", "trace"}}}});
	std::istringstream is(in);
	std::ostringstream os, log;
	server.Run(is, os, log);
	std::istringstream replay(os.str());
	auto frames = ReadAllFrames(replay);
	REQUIRE(frames.size() == 1);
	REQUIRE(frames[0].contains("error"));
	CHECK(frames[0]["error"]["code"] == -32602);
}

TEST_CASE("MCP logging/setLevel: not registered when no Logger provided") {
	auto reader = test::MakeMockReader();
	tools::ToolRegistry toolReg;
	tools::RegisterBlueprintTools(toolReg, reader);

	jsonrpc::Server server;
	mcp::ServerInfo info;
	mcp::RegisterHandlers(server, toolReg, info);  // no logger

	std::string in = FrameJson(json{
		{"jsonrpc","2.0"}, {"id",1}, {"method","logging/setLevel"},
		{"params", json{{"level","info"}}}});
	std::istringstream is(in);
	std::ostringstream os, log;
	server.Run(is, os, log);
	std::istringstream replay(os.str());
	auto frames = ReadAllFrames(replay);
	REQUIRE(frames.size() == 1);
	REQUIRE(frames[0].contains("error"));
	CHECK(frames[0]["error"]["code"] == -32601);
}
