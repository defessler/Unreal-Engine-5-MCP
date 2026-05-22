// Smoke-tests the full MCP handshake + tools/list + tools/call sequence by
// piping framed JSON-RPC through Server::Run, reading a real Claude-style
// initialize → initialized → tools/list → tools/call list_blueprints flow.

#include <doctest/doctest.h>

#include "backends/MockBlueprintReader.h"
#include "jsonrpc/Mcp.h"
#include "jsonrpc/Server.h"
#include "tools/BlueprintTools.h"
#include "tools/ContentBlocks.h"
#include "tools/ToolRegistry.h"
#include "jsonrpc/CallContext.h"
#include "tools/ToolsetMeta.h"

#include "test_helpers.h"

#include <algorithm>
#include <sstream>
#include <vector>

using namespace bpr;
using nlohmann::json;

namespace test_mcp_detail {

std::string Frame(const json& body) {
	std::ostringstream os;
	jsonrpc::WriteFrame(os, body);
	return os.str();
}

std::vector<json> ReadAllFrames(std::istream& in) {
	std::vector<json> out;
	while (true) {
		auto raw = jsonrpc::ReadFrame(in);
		if (!raw)
		{
			break;
		}
		out.push_back(json::parse(*raw));
	}
	return out;
}

}    // namespace test_mcp_detail
using namespace test_mcp_detail;

TEST_CASE("MCP initialize: instructions field shipped when ServerInfo sets it") {
	// The MCP `instructions` field on the initialize response is optional.
	// When ServerInfo.instructions is non-empty, the server MUST surface
	// it on the result so clients can feed it to the LLM as system-prompt
	// context. When empty, the field is omitted (older clients without
	// the field stay happy).
	auto reader = test::MakeMockReader();
	tools::ToolRegistry registry;
	tools::RegisterBlueprintTools(registry, reader);

	jsonrpc::Server server;
	mcp::ServerInfo info;
	info.instructions = mcp::DefaultInstructions();
	mcp::RegisterHandlers(server, registry, info);

	std::string in = Frame(json{
		{"jsonrpc","2.0"}, {"id",1}, {"method","initialize"},
		{"params", json{
			{"protocolVersion","2025-06-18"},
			{"capabilities", json::object()},
			{"clientInfo", json{{"name","test"}, {"version","0"}}}
		}}
	});
	std::istringstream is(in);
	std::ostringstream os, log;
	server.Run(is, os, log);
	std::istringstream replay(os.str());
	auto frames = ReadAllFrames(replay);
	REQUIRE(frames.size() == 1);
	REQUIRE(frames[0]["result"].contains("instructions"));
	const auto txt = frames[0]["result"]["instructions"].get<std::string>();
	CHECK(!txt.empty());
	// Should reference our BPIR pivot — the highest-signal hint we want
	// the LLM to internalize.
	CHECK(txt.find("BPIR") != std::string::npos);
}

TEST_CASE("MCP initialize: instructions field omitted when ServerInfo leaves it empty") {
	auto reader = test::MakeMockReader();
	tools::ToolRegistry registry;
	tools::RegisterBlueprintTools(registry, reader);

	jsonrpc::Server server;
	mcp::ServerInfo info;
	// Deliberately leave info.instructions empty.
	mcp::RegisterHandlers(server, registry, info);

	std::string in = Frame(json{
		{"jsonrpc","2.0"}, {"id",1}, {"method","initialize"},
		{"params", json{
			{"protocolVersion","2025-06-18"},
			{"capabilities", json::object()},
			{"clientInfo", json{{"name","test"}, {"version","0"}}}
		}}
	});
	std::istringstream is(in);
	std::ostringstream os, log;
	server.Run(is, os, log);
	std::istringstream replay(os.str());
	auto frames = ReadAllFrames(replay);
	REQUIRE(frames.size() == 1);
	CHECK_FALSE(frames[0]["result"].contains("instructions"));
}

TEST_CASE("MCP handshake + tools/list + tools/call list_blueprints") {
	auto reader = test::MakeMockReader();
	tools::ToolRegistry registry;
	tools::RegisterBlueprintTools(registry, reader);

	jsonrpc::Server server;
	mcp::ServerInfo info;
	mcp::RegisterHandlers(server, registry, info);

	std::string in = Frame(json{
		{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"},
		{"params", json{
			{"protocolVersion", "2024-11-05"},
			{"capabilities", json::object()},
			{"clientInfo", json{{"name", "test"}, {"version", "0"}}}
		}}
	});
	in += Frame(json{
		{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}
	});
	in += Frame(json{
		{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}
	});
	in += Frame(json{
		{"jsonrpc", "2.0"}, {"id", 3}, {"method", "tools/call"},
		{"params", json{
			{"name", "list_blueprints"},
			{"arguments", json{{"path", "/Game"}}}
		}}
	});

	std::istringstream is(in);
	std::ostringstream os;
	std::ostringstream log;
	server.Run(is, os, log);

	std::istringstream replay(os.str());
	auto frames = ReadAllFrames(replay);
	REQUIRE(frames.size() == 3); // notification produces no response

	// initialize
	CHECK(frames[0]["id"] == 1);
	CHECK(frames[0]["result"]["serverInfo"]["name"] == "bp-reader-mcp");
	CHECK(frames[0]["result"]["capabilities"].contains("tools"));

	// tools/list
	CHECK(frames[1]["id"] == 2);
	auto& list = frames[1]["result"]["tools"];
	REQUIRE(list.is_array());
	CHECK(list.size() == 169);  // +25 Phase 8 + 9 Phase 11 + 2 Phase 12 Wave 2
	std::vector<std::string> names;
	for (auto& t : list)
	{
		names.push_back(t["name"].get<std::string>());
	}
	auto has = [&](const std::string& n) {
		return std::find(names.begin(), names.end(), n) != names.end();
	};
	for (const char* n : {"list_blueprints","read_blueprint","summarize_blueprint",
						  "get_graph","get_function",
						  "list_variables","get_components","find_node",
						  "get_node","find_overriders",
						  "create_blueprint",
						  "add_variable",
						  "set_node_position","delete_node","add_node","wire_pins",
						  "delete_variable","rename_variable","list_node_kinds",
						  "list_pin_categories","add_function","add_function_input",
						  "add_function_output","delete_function","set_variable_default",
						  "apply_ops","preview_ops","compile_function","auto_layout_graph",
						  "shutdown_daemon",
						  "retype_variable","set_variable_category","duplicate_blueprint",
						  "decompile_function","decompile_blueprint","transpile_function",
						  "transpile_blueprint","write_generated_source"}) {
		CHECK(has(n));
	}

	// tools/call
	CHECK(frames[2]["id"] == 3);
	auto& callResult = frames[2]["result"];
	CHECK(callResult["isError"] == false);
	REQUIRE(callResult["content"].is_array());
	REQUIRE(callResult["content"].size() == 1);
	auto inner = json::parse(callResult["content"][0]["text"].get<std::string>());
	REQUIRE(inner.is_array());
	CHECK(inner.size() == 6); // 6 fixtures all under /Game
	// Make sure the canonical asset_path key is present, not assetPath.
	CHECK(inner[0].contains("asset_path"));
	CHECK(inner[0].contains("parent_class"));
}

TEST_CASE("tools/call surfaces unknown tool as MCP error envelope, not JSON-RPC error") {
	auto reader = test::MakeMockReader();
	tools::ToolRegistry registry;
	tools::RegisterBlueprintTools(registry, reader);

	jsonrpc::Server server;
	mcp::ServerInfo info;
	mcp::RegisterHandlers(server, registry, info);

	json req = {
		{"jsonrpc", "2.0"}, {"id", 9}, {"method", "tools/call"},
		{"params", json{{"name", "no_such_tool"}, {"arguments", json::object()}}}
	};
	auto resp = server.Dispatch(req);
	REQUIRE(resp.has_value());
	REQUIRE(resp->contains("result"));
	CHECK_FALSE(resp->contains("error"));
	CHECK((*resp)["result"]["isError"] == true);
	auto txt = (*resp)["result"]["content"][0]["text"].get<std::string>();
	CHECK(txt.find("unknown tool") != std::string::npos);
}

TEST_CASE("tools/call propagates handler error as MCP error envelope") {
	auto reader = test::MakeMockReader();
	tools::ToolRegistry registry;
	tools::RegisterBlueprintTools(registry, reader);

	jsonrpc::Server server;
	mcp::ServerInfo info;
	mcp::RegisterHandlers(server, registry, info);

	json req = {
		{"jsonrpc", "2.0"}, {"id", 10}, {"method", "tools/call"},
		{"params", json{
			{"name", "read_blueprint"},
			{"arguments", json{{"asset_path", "/Game/DoesNotExist"}}}
		}}
	};
	auto resp = server.Dispatch(req);
	REQUIRE(resp.has_value());
	REQUIRE(resp->contains("result"));
	CHECK((*resp)["result"]["isError"] == true);
	CHECK_FALSE(resp->contains("error"));
}

TEST_CASE("tools/call: tool returning content::Envelope unpacks to spec-shaped content array") {
	// Verifies the rich-content opt-in path: a tool that returns
	// `{"_mcp": {"content": [...], "structuredContent": ...}}` gets
	// its content used directly, with structuredContent surfaced as a
	// sibling of content per MCP 2025-06-18.
	tools::ToolRegistry registry;
	tools::ToolDescriptor d;
	d.name = "fake_screenshot";
	d.description = "test";
	d.input_schema = json{{"type","object"}};
	registry.Add(std::move(d), [](const json&) {
		return tools::content::Envelope(
			{
				tools::content::Text("Captured viewport.", tools::content::Audience::User),
				tools::content::ImageBase64("ZmFrZQ==", "image/png"),
			},
			json{{"width", 1280}, {"height", 720}});
	});

	jsonrpc::Server server;
	mcp::ServerInfo info;
	mcp::RegisterHandlers(server, registry, info);

	json req = {
		{"jsonrpc", "2.0"}, {"id", 30}, {"method", "tools/call"},
		{"params", json{{"name", "fake_screenshot"}, {"arguments", json::object()}}}
	};
	auto resp = server.Dispatch(req);
	REQUIRE(resp.has_value());
	auto& result = (*resp)["result"];
	CHECK(result["isError"] == false);
	REQUIRE(result["content"].is_array());
	REQUIRE(result["content"].size() == 2);
	// Block 0: text with explicit User audience
	CHECK(result["content"][0]["type"] == "text");
	CHECK(result["content"][0]["text"] == "Captured viewport.");
	REQUIRE(result["content"][0].contains("annotations"));
	CHECK(result["content"][0]["annotations"]["audience"] == json::array({"user"}));
	// Block 1: image (default audience User, also annotation-emitted)
	CHECK(result["content"][1]["type"] == "image");
	CHECK(result["content"][1]["data"] == "ZmFrZQ==");
	CHECK(result["content"][1]["mimeType"] == "image/png");
	// structuredContent sibling
	REQUIRE(result.contains("structuredContent"));
	CHECK(result["structuredContent"]["width"] == 1280);
	CHECK(result["structuredContent"]["height"] == 720);
	// _meta still attached
	CHECK(result["_meta"]["tool"] == "fake_screenshot");
}

TEST_CASE("Lazy discovery: tool search mode advertises just 4 tools but call_tool reaches all") {
	auto reader = test::MakeMockReader();
	tools::ToolRegistry registry;
	tools::RegisterBlueprintTools(registry, reader);
	REQUIRE(registry.TotalRegistered() == 169);

	tools::RegisterToolsetMetaTools(registry);
	REQUIRE(registry.TotalRegistered() == 172);  // +3 meta-tools

	tools::EnableToolSearchMode(registry);
	// Active set should now be 4: list_toolsets, describe_toolset, call_tool, shutdown_daemon.
	auto spec = registry.ListSpec();
	REQUIRE(spec.size() == 4);
	std::vector<std::string> names;
	for (auto& t : spec) names.push_back(t["name"].get<std::string>());
	std::sort(names.begin(), names.end());
	CHECK(names == std::vector<std::string>{
		"call_tool", "describe_toolset", "list_toolsets", "shutdown_daemon"});

	jsonrpc::Server server;
	mcp::ServerInfo info;
	mcp::RegisterHandlers(server, registry, info);

	// list_toolsets — should return a non-empty array of {name, description, tool_count}
	json req1 = {
		{"jsonrpc", "2.0"}, {"id", 100}, {"method", "tools/call"},
		{"params", json{{"name", "list_toolsets"}, {"arguments", json::object()}}}
	};
	auto r1 = server.Dispatch(req1);
	REQUIRE(r1.has_value());
	auto inner1 = json::parse((*r1)["result"]["content"][0]["text"].get<std::string>());
	REQUIRE(inner1.contains("toolsets"));
	REQUIRE(inner1["toolsets"].is_array());
	CHECK(inner1["toolsets"].size() >= 20);  // we have ~26 categories
	// Each entry has the right shape
	CHECK(inner1["toolsets"][0].contains("name"));
	CHECK(inner1["toolsets"][0].contains("description"));
	CHECK(inner1["toolsets"][0].contains("tool_count"));

	// describe_toolset(name="cpp") — should list the 7 cpp tools
	json req2 = {
		{"jsonrpc", "2.0"}, {"id", 101}, {"method", "tools/call"},
		{"params", json{{"name", "describe_toolset"},
		                {"arguments", json{{"name", "cpp"}}}}}
	};
	auto r2 = server.Dispatch(req2);
	REQUIRE(r2.has_value());
	auto inner2 = json::parse((*r2)["result"]["content"][0]["text"].get<std::string>());
	CHECK(inner2["name"] == "cpp");
	REQUIRE(inner2["tools"].is_array());
	CHECK(inner2["tools"].size() == 7);
	// Each tool entry has inputSchema
	for (const auto& t : inner2["tools"]) {
		CHECK(t.contains("name"));
		CHECK(t.contains("description"));
		CHECK(t["inputSchema"]["type"] == "object");
	}

	// call_tool(name="list_blueprints", arguments={path: "/Game"}) — invokes
	// a tool that's filtered out of tools/list, proving the lazy-discovery
	// dispatch reaches the underlying registry.
	json req3 = {
		{"jsonrpc", "2.0"}, {"id", 102}, {"method", "tools/call"},
		{"params", json{
			{"name", "call_tool"},
			{"arguments", json{
				{"name", "list_blueprints"},
				{"arguments", json{{"path", "/Game"}}}
			}}
		}}
	};
	auto r3 = server.Dispatch(req3);
	REQUIRE(r3.has_value());
	auto inner3 = json::parse((*r3)["result"]["content"][0]["text"].get<std::string>());
	REQUIRE(inner3.is_array());
	CHECK(inner3.size() == 6);  // same 6 fixtures the direct-call test checks
}

TEST_CASE("call_tool rejects unknown tool names with a clear error") {
	auto reader = test::MakeMockReader();
	tools::ToolRegistry registry;
	tools::RegisterBlueprintTools(registry, reader);
	tools::RegisterToolsetMetaTools(registry);
	tools::EnableToolSearchMode(registry);

	jsonrpc::Server server;
	mcp::ServerInfo info;
	mcp::RegisterHandlers(server, registry, info);
	json req = {
		{"jsonrpc", "2.0"}, {"id", 103}, {"method", "tools/call"},
		{"params", json{
			{"name", "call_tool"},
			{"arguments", json{{"name", "no_such_thing"}}}
		}}
	};
	auto r = server.Dispatch(req);
	REQUIRE(r.has_value());
	CHECK((*r)["result"]["isError"] == true);
	auto txt = (*r)["result"]["content"][0]["text"].get<std::string>();
	CHECK(txt.find("unknown tool") != std::string::npos);
}

TEST_CASE("Server in-flight registry: register/find/unregister round-trip") {
	// notifications/cancelled lookup wiring. Today's stdio dispatch is
	// single-threaded so the registry is always empty when the handler
	// runs, but the future async/HTTP path consults this directly.
	jsonrpc::Server server;
	jsonrpc::CallContext ctx(server, /*requestId=*/json(42), std::nullopt);
	CHECK(server.FindInFlight(json(42)) == nullptr);
	server.RegisterInFlight(&ctx);
	CHECK(server.FindInFlight(json(42)) == &ctx);
	CHECK(server.FindInFlight(json(99)) == nullptr);
	server.UnregisterInFlight(&ctx);
	CHECK(server.FindInFlight(json(42)) == nullptr);
}

TEST_CASE("notifications/cancelled marks the matching in-flight CallContext") {
	// Verify the handler path: dispatch a notifications/cancelled for a
	// requestId we've registered, then check IsCancelled() flipped.
	auto reader = test::MakeMockReader();
	tools::ToolRegistry registry;
	tools::RegisterBlueprintTools(registry, reader);
	jsonrpc::Server server;
	mcp::ServerInfo info;
	mcp::RegisterHandlers(server, registry, info);

	jsonrpc::CallContext ctx(server, json(42), std::nullopt);
	server.RegisterInFlight(&ctx);
	REQUIRE_FALSE(ctx.IsCancelled());

	json req = {
		{"jsonrpc","2.0"},
		{"method","notifications/cancelled"},
		{"params", json{{"requestId", 42}}}
	};
	auto r = server.Dispatch(req);
	CHECK_FALSE(r.has_value());  // notification — no response body
	CHECK(ctx.IsCancelled());

	// Non-matching id leaves a different context alone.
	jsonrpc::CallContext other(server, json(99), std::nullopt);
	server.RegisterInFlight(&other);
	json req2 = {
		{"jsonrpc","2.0"},
		{"method","notifications/cancelled"},
		{"params", json{{"requestId", 7}}}
	};
	server.Dispatch(req2);
	CHECK_FALSE(other.IsCancelled());

	server.UnregisterInFlight(&ctx);
	server.UnregisterInFlight(&other);
}

TEST_CASE("call_tool refuses to dispatch the meta-tools (recursion guard)") {
	// An agent that calls call_tool({name: "call_tool", arguments: {...}})
	// would loop infinitely if we didn't reject it. Same for the other
	// meta-tools: they're already at the top level under tool-search mode,
	// so tunneling them through call_tool is always a mistake.
	auto reader = test::MakeMockReader();
	tools::ToolRegistry registry;
	tools::RegisterBlueprintTools(registry, reader);
	tools::RegisterToolsetMetaTools(registry);
	tools::EnableToolSearchMode(registry);

	jsonrpc::Server server;
	mcp::ServerInfo info;
	mcp::RegisterHandlers(server, registry, info);

	for (const char* metaName : {"call_tool", "list_toolsets", "describe_toolset"}) {
		json req = {
			{"jsonrpc","2.0"}, {"id",200}, {"method","tools/call"},
			{"params", json{
				{"name", "call_tool"},
				{"arguments", json{
					{"name", metaName},
					{"arguments", json::object()}
				}}
			}}
		};
		auto r = server.Dispatch(req);
		REQUIRE(r.has_value());
		CHECK((*r)["result"]["isError"] == true);
		auto txt = (*r)["result"]["content"][0]["text"].get<std::string>();
		// The error message should name the offending meta-tool so the
		// agent knows what to do instead.
		CHECK(txt.find(metaName) != std::string::npos);
		CHECK(txt.find("meta-tool") != std::string::npos);
	}
}

TEST_CASE("Long-running tool can emit progress via CallContext, gets queued as notifications/progress") {
	tools::ToolRegistry registry;
	tools::ToolDescriptor d;
	d.name = "slow_op";
	d.description = "test";
	d.input_schema = json{{"type","object"}};
	registry.Add(std::move(d), [](const json&) {
		// Simulate progress emission inside the tool.
		auto* ctx = jsonrpc::CallContext::Current();
		REQUIRE(ctx != nullptr);
		ctx->EmitProgress(0.0, 100.0, "starting");
		ctx->EmitProgress(50.0, 100.0, "halfway");
		ctx->EmitProgress(100.0, 100.0, "done");
		return json{{"ok", true}};
	});

	jsonrpc::Server server;
	mcp::ServerInfo info;
	mcp::RegisterHandlers(server, registry, info);

	json req = {
		{"jsonrpc", "2.0"}, {"id", 200}, {"method", "tools/call"},
		{"params", json{
			{"name", "slow_op"},
			{"arguments", json::object()},
			// progressToken in _meta per MCP 2025-06-18.
			{"_meta", json{{"progressToken", "tok-200"}}}
		}}
	};
	auto resp = server.Dispatch(req);
	REQUIRE(resp.has_value());
	CHECK((*resp)["result"]["isError"] == false);

	auto notifs = server.TakePendingNotifications();
	// Three progress notifications queued — one per EmitProgress call.
	REQUIRE(notifs.size() == 3);
	CHECK(notifs[0]["method"] == "notifications/progress");
	CHECK(notifs[0]["params"]["progressToken"] == "tok-200");
	CHECK(notifs[0]["params"]["progress"] == 0.0);
	CHECK(notifs[0]["params"]["total"] == 100.0);
	CHECK(notifs[0]["params"]["message"] == "starting");
	CHECK(notifs[2]["params"]["progress"] == 100.0);
	CHECK(notifs[2]["params"]["message"] == "done");
}

TEST_CASE("Tool with no progressToken from client gets EmitProgress as no-op") {
	tools::ToolRegistry registry;
	tools::ToolDescriptor d;
	d.name = "slow_op_no_token";
	d.description = "test";
	d.input_schema = json{{"type","object"}};
	registry.Add(std::move(d), [](const json&) {
		auto* ctx = jsonrpc::CallContext::Current();
		REQUIRE(ctx != nullptr);
		ctx->EmitProgress(50.0, 100.0, "no one's listening");
		return json{{"ok", true}};
	});

	jsonrpc::Server server;
	mcp::ServerInfo info;
	mcp::RegisterHandlers(server, registry, info);

	json req = {
		{"jsonrpc", "2.0"}, {"id", 201}, {"method", "tools/call"},
		{"params", json{{"name", "slow_op_no_token"}, {"arguments", json::object()}}}
	};
	(void)server.Dispatch(req);
	CHECK(server.TakePendingNotifications().empty());
}

TEST_CASE("CallContext::IsCancelled defaults false, MarkCancelled flips it") {
	jsonrpc::Server server;
	jsonrpc::CallContext ctx(server, json("id-1"), std::nullopt);
	CHECK_FALSE(ctx.IsCancelled());
	ctx.MarkCancelled();
	CHECK(ctx.IsCancelled());
}

TEST_CASE("tools/call: text content with default audience omits annotations object") {
	// Audience::Both is the implicit MCP default — the helper deliberately
	// doesn't emit annotations in that case so the JSON stays minimal.
	auto block = tools::content::Text("hi");  // Audience defaults to Both
	CHECK(block["type"] == "text");
	CHECK_FALSE(block.contains("annotations"));
}

TEST_CASE("tools/call error envelope includes args + tool name in _meta") {
	// Agents debugging tool failures need to see what they passed —
	// helps re-drive the failing call and frame the error context
	// ("transpile_blueprint on /Game/X failed because…").
	auto reader = test::MakeMockReader();
	tools::ToolRegistry registry;
	tools::RegisterBlueprintTools(registry, reader);

	jsonrpc::Server server;
	mcp::ServerInfo info;
	mcp::RegisterHandlers(server, registry, info);

	json req = {
		{"jsonrpc", "2.0"}, {"id", 20}, {"method", "tools/call"},
		{"params", json{
			{"name", "read_blueprint"},
			{"arguments", json{{"asset_path", "/Game/DoesNotExist"}}}
		}}
	};
	auto resp = server.Dispatch(req);
	REQUIRE(resp.has_value());
	CHECK((*resp)["result"]["isError"] == true);
	auto& meta = (*resp)["result"]["_meta"];
	CHECK(meta["tool"] == "read_blueprint");
	REQUIRE(meta.contains("args"));
	CHECK(meta["args"]["asset_path"] == "/Game/DoesNotExist");
}
