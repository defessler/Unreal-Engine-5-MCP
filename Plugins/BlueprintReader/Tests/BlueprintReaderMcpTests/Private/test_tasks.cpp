// MCP-8: async Tasks primitive — TaskManager unit tests + the MCP
// tools/call(task) -> tasks/get -> tasks/cancel -> tasks/list protocol flow.
//
// The tasks run on background threads, so the lifecycle tests poll (bounded)
// rather than assume synchronous completion. The single-task "busy" model is
// exercised at the TaskManager layer with a controllable (latch-gated) worker
// so the assertion isn't timing-dependent.

#include <doctest/doctest.h>

#include "backends/MockBlueprintReader.h"
#include "jsonrpc/Mcp.h"
#include "jsonrpc/Server.h"
#include "tools/BlueprintTools.h"
#include "tools/TaskManager.h"
#include "tools/ToolRegistry.h"

#include "test_helpers.h"

#include <chrono>
#include <future>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace bpr;
using nlohmann::json;

namespace test_tasks_detail {

std::string Frame(const json& body) {
	std::ostringstream os;
	jsonrpc::WriteFrame(os, body);
	return os.str();
}

std::vector<json> ReadAllFrames(std::istream& in) {
	std::vector<json> out;
	while (true) {
		auto raw = jsonrpc::ReadFrame(in);
		if (!raw) {
			break;
		}
		out.push_back(json::parse(*raw));
	}
	return out;
}

// Run one request through the server (handlers persist across calls), return
// the response frame whose id matches the request.
json RunOne(jsonrpc::Server& server, const json& req) {
	std::istringstream is(Frame(req));
	std::ostringstream os, log;
	server.Run(is, os, log);
	std::istringstream replay(os.str());
	auto frames = ReadAllFrames(replay);
	for (auto& f : frames) {
		if (f.contains("id") && f["id"] == req.at("id")) {
			return f;
		}
	}
	REQUIRE_MESSAGE(false, "no response frame for request id");
	return json::object();
}

}    // namespace test_tasks_detail
using namespace test_tasks_detail;

// ---- TaskManager unit tests (controlled timing) -----------------------------

TEST_CASE("TaskManager: single-task model rejects a second start while active") {
	tools::TaskManager mgr;
	std::promise<void> gate;
	std::shared_future<void> released = gate.get_future().share();

	auto id1 = mgr.Start("tool_a", 60000, [released](const std::string&) -> json {
		released.wait();    // block so the task stays "working"
		return json{{"ok", true}, {"who", "a"}};
	});
	REQUIRE(id1.has_value());
	CHECK(mgr.HasActive());

	// Second start while the first is active → busy (nullopt).
	auto id2 = mgr.Start("tool_b", 60000, [](const std::string&) { return json::object(); });
	CHECK_FALSE(id2.has_value());

	auto working = mgr.Get(*id1);
	REQUIRE(working.has_value());
	CHECK(working->status == "working");
	CHECK_FALSE(working->hasResult);

	gate.set_value();    // let the worker finish
	for (int i = 0; i < 400 && mgr.HasActive(); ++i) {
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	CHECK_FALSE(mgr.HasActive());

	auto done = mgr.Get(*id1);
	REQUIRE(done.has_value());
	CHECK(done->status == "completed");
	REQUIRE(done->hasResult);
	CHECK(done->result["ok"] == true);

	// With the first finished, a new task can start.
	auto id3 = mgr.Start("tool_c", 60000, [](const std::string&) { return json{{"ok", true}}; });
	CHECK(id3.has_value());
	for (int i = 0; i < 400 && mgr.HasActive(); ++i) {
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
}

TEST_CASE("TaskManager: a throwing worker is marked failed, not crashing") {
	tools::TaskManager mgr;
	auto id = mgr.Start("boom", 1000, [](const std::string&) -> json {
		throw std::runtime_error("kaboom");
	});
	REQUIRE(id.has_value());
	for (int i = 0; i < 400 && mgr.HasActive(); ++i) {
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	auto v = mgr.Get(*id);
	REQUIRE(v.has_value());
	CHECK(v->status == "failed");
	REQUIRE(v->hasResult);
	CHECK(v->result.value("isError", false) == true);
}

TEST_CASE("TaskManager: Get on an unknown id is nullopt; List enumerates") {
	tools::TaskManager mgr;
	CHECK_FALSE(mgr.Get("task-nope").has_value());
	auto id = mgr.Start("t", 1000, [](const std::string&) { return json{{"ok", true}}; });
	REQUIRE(id.has_value());
	for (int i = 0; i < 400 && mgr.HasActive(); ++i) {
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	auto list = mgr.List();
	REQUIRE(list.size() == 1);
	CHECK(list[0].taskId == *id);
	CHECK(list[0].tool == "t");
}

// ---- MCP protocol-flow integration tests ------------------------------------

namespace {
jsonrpc::Server& InitServer(jsonrpc::Server& server, tools::ToolRegistry& registry,
							backends::MockBlueprintReader& reader, mcp::ServerInfo& info) {
	tools::RegisterBlueprintTools(registry, reader);
	mcp::RegisterHandlers(server, registry, info);
	return server;
}
}    // namespace

TEST_CASE("MCP tasks: capability advertised + tools/call(task) runs async to completion") {
	auto reader = test::MakeMockReader();
	tools::ToolRegistry registry;
	jsonrpc::Server server;
	mcp::ServerInfo info;
	InitServer(server, registry, reader, info);

	// initialize → capabilities.tasks present
	const json initResp = RunOne(server, json{
		{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"},
		{"params", json{{"protocolVersion", "2025-11-25"},
						{"capabilities", json::object()},
						{"clientInfo", json{{"name", "t"}, {"version", "0"}}}}}});
	REQUIRE(initResp["result"]["capabilities"].contains("tasks"));

	// tools/call list_blueprints WITH a task augmentation → returns a taskId.
	const json callResp = RunOne(server, json{
		{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/call"},
		{"params", json{{"name", "list_blueprints"},
						{"arguments", json{{"path", "/Game"}}},
						{"task", json{{"ttl", 30000}}}}}});
	REQUIRE(callResp["result"].contains("task"));
	const std::string taskId = callResp["result"]["task"]["taskId"].get<std::string>();
	CHECK(callResp["result"]["task"]["status"] == "working");
	CHECK_FALSE(taskId.empty());

	// poll tasks/get until it leaves "working"
	json getResp;
	for (int i = 0; i < 400; ++i) {
		getResp = RunOne(server, json{
			{"jsonrpc", "2.0"}, {"id", 100 + i}, {"method", "tasks/get"},
			{"params", json{{"taskId", taskId}}}});
		if (getResp["result"]["task"]["status"] != "working") {
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	CHECK(getResp["result"]["task"]["status"] == "completed");
	// the finished CallToolResult envelope is returned under `result`
	REQUIRE(getResp["result"].contains("result"));
	CHECK(getResp["result"]["result"].contains("content"));

	// tasks/list shows it
	const json listResp = RunOne(server, json{
		{"jsonrpc", "2.0"}, {"id", 3}, {"method", "tasks/list"}});
	REQUIRE(listResp["result"]["tasks"].is_array());
	CHECK(listResp["result"]["tasks"].size() >= 1);
}

TEST_CASE("MCP tasks: tasks/get on an unknown id is an error; tasks/cancel acks") {
	auto reader = test::MakeMockReader();
	tools::ToolRegistry registry;
	jsonrpc::Server server;
	mcp::ServerInfo info;
	InitServer(server, registry, reader, info);
	RunOne(server, json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"},
		{"params", json{{"protocolVersion", "2025-11-25"}, {"capabilities", json::object()},
						{"clientInfo", json{{"name", "t"}, {"version", "0"}}}}}});

	const json badGet = RunOne(server, json{
		{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tasks/get"},
		{"params", json{{"taskId", "task-does-not-exist"}}}});
	CHECK(badGet.contains("error"));

	// Start a task, then cancel it (cooperative — list_blueprints won't poll
	// IsCancelled, but the cancel call must still ack cleanly).
	const json callResp = RunOne(server, json{
		{"jsonrpc", "2.0"}, {"id", 3}, {"method", "tools/call"},
		{"params", json{{"name", "list_blueprints"},
						{"arguments", json{{"path", "/Game"}}},
						{"task", json{{"ttl", 30000}}}}}});
	const std::string taskId = callResp["result"]["task"]["taskId"].get<std::string>();

	const json cancelResp = RunOne(server, json{
		{"jsonrpc", "2.0"}, {"id", 4}, {"method", "tasks/cancel"},
		{"params", json{{"taskId", taskId}}}});
	REQUIRE(cancelResp["result"].contains("task"));
	CHECK(cancelResp["result"]["task"]["taskId"] == taskId);

	// drain to completion so the worker thread isn't mid-flight at teardown
	for (int i = 0; i < 400; ++i) {
		const json g = RunOne(server, json{
			{"jsonrpc", "2.0"}, {"id", 200 + i}, {"method", "tasks/get"},
			{"params", json{{"taskId", taskId}}}});
		if (g["result"]["task"]["status"] != "working") {
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
}
