// Phase 3 tests: MCP prompts primitive (prompts/list + prompts/get) and
// the 8 built-in slash commands.
//
// Coverage:
//  - PromptRegistry behaviour (register, list, render, has, replace-in-place)
//  - Built-in prompt set (count + spec shape per prompt)
//  - prompts/list + prompts/get JSON-RPC dispatch via mcp::RegisterHandlers
//  - Capability advertisement on initialize (advertised when prompts > 0,
//    omitted when empty)
//  - Missing required argument → InvalidParams error
//  - Unknown prompt name → MethodNotFound error

#include <doctest/doctest.h>

#include "backends/MockBlueprintReader.h"
#include "jsonrpc/Mcp.h"
#include "jsonrpc/Server.h"
#include "tools/BlueprintTools.h"
#include "tools/Prompts.h"
#include "tools/ToolRegistry.h"

#include "test_helpers.h"

#include <sstream>
#include <string>
#include <vector>

using namespace bpr;
using nlohmann::json;

namespace test_prompts_detail {

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

}    // namespace test_prompts_detail
using namespace test_prompts_detail;

// =====================================================================
// PromptRegistry behaviour
// =====================================================================

TEST_CASE("PromptRegistry: empty registry has size 0 and Has() returns false") {
	tools::prompts::PromptRegistry r;
	CHECK(r.Size() == 0);
	CHECK_FALSE(r.Has("anything"));
}

TEST_CASE("PromptRegistry: Register + Has + ListSpec shape") {
	tools::prompts::PromptRegistry r;
	tools::prompts::PromptDescriptor d;
	d.name = "hello";
	d.description = "Greet the user.";
	d.arguments = {{"who", "Whom to greet.", /*required=*/true}};
	r.Register(std::move(d), [](const json& args) {
		std::string who = tools::prompts::RequirePromptArg(args, "hello", "who");
		return json::array({tools::prompts::UserMessage("Hello " + who + "!")});
	});
	CHECK(r.Size() == 1);
	CHECK(r.Has("hello"));
	auto spec = r.ListSpec();
	REQUIRE(spec.is_array());
	REQUIRE(spec.size() == 1);
	CHECK(spec[0]["name"] == "hello");
	CHECK(spec[0]["description"] == "Greet the user.");
	REQUIRE(spec[0]["arguments"].is_array());
	REQUIRE(spec[0]["arguments"].size() == 1);
	CHECK(spec[0]["arguments"][0]["name"] == "who");
	CHECK(spec[0]["arguments"][0]["required"] == true);
}

TEST_CASE("PromptRegistry: Render returns messages + description") {
	tools::prompts::PromptRegistry r;
	tools::prompts::PromptDescriptor d;
	d.name = "echo";
	d.description = "Echo back.";
	r.Register(std::move(d), [](const json&) {
		return json::array({tools::prompts::UserMessage("pong")});
	});
	auto rendered = r.Render("echo", json::object());
	CHECK(rendered["description"] == "Echo back.");
	REQUIRE(rendered["messages"].is_array());
	REQUIRE(rendered["messages"].size() == 1);
	CHECK(rendered["messages"][0]["role"] == "user");
	CHECK(rendered["messages"][0]["content"]["type"] == "text");
	CHECK(rendered["messages"][0]["content"]["text"] == "pong");
}

TEST_CASE("PromptRegistry: Render of unknown name throws invalid_argument") {
	tools::prompts::PromptRegistry r;
	CHECK_THROWS_AS(r.Render("nope", json::object()), std::invalid_argument);
}

TEST_CASE("PromptRegistry: Render propagates handler-side missing-arg errors") {
	tools::prompts::PromptRegistry r;
	tools::prompts::PromptDescriptor d;
	d.name = "need_arg";
	d.arguments = {{"x", "Required.", /*required=*/true}};
	r.Register(std::move(d), [](const json& args) {
		std::string x = tools::prompts::RequirePromptArg(args, "need_arg", "x");
		return json::array({tools::prompts::UserMessage("got " + x)});
	});
	CHECK_THROWS_AS(r.Render("need_arg", json::object()), std::invalid_argument);
}

TEST_CASE("PromptRegistry: Re-register replaces in place (no duplicate entries)") {
	tools::prompts::PromptRegistry r;
	tools::prompts::PromptDescriptor d1;
	d1.name = "x";
	d1.description = "first";
	r.Register(std::move(d1), [](const json&) {
		return json::array({tools::prompts::UserMessage("first body")});
	});
	tools::prompts::PromptDescriptor d2;
	d2.name = "x";
	d2.description = "second";
	r.Register(std::move(d2), [](const json&) {
		return json::array({tools::prompts::UserMessage("second body")});
	});
	CHECK(r.Size() == 1);
	auto rendered = r.Render("x", json::object());
	CHECK(rendered["description"] == "second");
	CHECK(rendered["messages"][0]["content"]["text"] == "second body");
}

// =====================================================================
// Built-in prompt set
// =====================================================================

TEST_CASE("Built-in prompts: RegisterBuiltinPrompts registers exactly 8") {
	tools::prompts::PromptRegistry r;
	tools::prompts::RegisterBuiltinPrompts(r);
	CHECK(r.Size() == 8);
	for (const char* name : {
			"audit_bp", "explain_function", "suggest_refactor",
			"compare_blueprints", "transpile_to_cpp",
			"review_generated_cpp", "check_transpile_compat",
			"lyra_gameplay_review",
		}) {
		CAPTURE(name);
		CHECK(r.Has(name));
	}
}

TEST_CASE("Built-in prompts: transpile_to_cpp is the BP↔C++ moat prompt") {
	tools::prompts::PromptRegistry r;
	tools::prompts::RegisterBuiltinPrompts(r);
	auto rendered = r.Render("transpile_to_cpp",
		json{{"asset_path", "/Game/AI/BP_Foo"}});
	REQUIRE(rendered["messages"].is_array());
	REQUIRE(rendered["messages"].size() >= 1);
	const std::string text =
		rendered["messages"][0]["content"]["text"].get<std::string>();
	// Critical workflow markers — the prompt MUST mention these
	// because they're what separates us from other MCP servers.
	CHECK(text.find("BPIR") != std::string::npos);
	CHECK(text.find("BP_READER_ALLOW_TRANSPILE") != std::string::npos);
	CHECK(text.find("decompile_blueprint") != std::string::npos);
	CHECK(text.find("write_generated_source") != std::string::npos);
	// Path interpolation works.
	CHECK(text.find("/Game/AI/BP_Foo") != std::string::npos);
}

TEST_CASE("Built-in prompts: audit_bp requires asset_path") {
	tools::prompts::PromptRegistry r;
	tools::prompts::RegisterBuiltinPrompts(r);
	CHECK_THROWS_AS(r.Render("audit_bp", json::object()), std::invalid_argument);
}

TEST_CASE("Built-in prompts: every prompt declares at least one required arg") {
	// Loose contract check — every slash command should accept *some*
	// argument that scopes the work (asset_path, function_name, etc.).
	// Catches accidentally publishing a no-arg "go review everything"
	// prompt that's too broad to be useful.
	tools::prompts::PromptRegistry r;
	tools::prompts::RegisterBuiltinPrompts(r);
	auto spec = r.ListSpec();
	for (const auto& p : spec) {
		CAPTURE(p["name"].get<std::string>());
		REQUIRE(p.contains("arguments"));
		REQUIRE(p["arguments"].is_array());
		CHECK(p["arguments"].size() >= 1);
		// At least one of the declared args must be required=true.
		bool sawRequired = false;
		for (const auto& a : p["arguments"]) {
			if (a.value("required", false)) sawRequired = true;
		}
		CHECK(sawRequired);
	}
}

// =====================================================================
// JSON-RPC dispatch
// =====================================================================

TEST_CASE("MCP initialize advertises prompts capability when registry has entries") {
	auto reader = test::MakeMockReader();
	tools::ToolRegistry tools;
	tools::RegisterBlueprintTools(tools, reader);
	tools::prompts::PromptRegistry prompts;
	tools::prompts::RegisterBuiltinPrompts(prompts);

	jsonrpc::Server server;
	mcp::ServerInfo info;
	mcp::RegisterHandlers(server, tools, prompts, info);

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
	REQUIRE(frames[0]["result"]["capabilities"].contains("prompts"));
	CHECK(frames[0]["result"]["capabilities"]["prompts"]["listChanged"] == true);
}

TEST_CASE("MCP initialize omits prompts capability when registry is empty") {
	auto reader = test::MakeMockReader();
	tools::ToolRegistry tools;
	tools::RegisterBlueprintTools(tools, reader);
	tools::prompts::PromptRegistry emptyPrompts;  // intentionally empty

	jsonrpc::Server server;
	mcp::ServerInfo info;
	mcp::RegisterHandlers(server, tools, emptyPrompts, info);

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
	CHECK_FALSE(frames[0]["result"]["capabilities"].contains("prompts"));
}

TEST_CASE("MCP prompts/list returns the 8 built-in prompts") {
	auto reader = test::MakeMockReader();
	tools::ToolRegistry tools;
	tools::RegisterBlueprintTools(tools, reader);
	tools::prompts::PromptRegistry prompts;
	tools::prompts::RegisterBuiltinPrompts(prompts);

	jsonrpc::Server server;
	mcp::ServerInfo info;
	mcp::RegisterHandlers(server, tools, prompts, info);

	std::string in = FrameJson(json{
		{"jsonrpc","2.0"}, {"id",2}, {"method","prompts/list"}});
	std::istringstream is(in);
	std::ostringstream os, log;
	server.Run(is, os, log);
	std::istringstream replay(os.str());
	auto frames = ReadAllFrames(replay);
	REQUIRE(frames.size() == 1);
	REQUIRE(frames[0]["result"]["prompts"].is_array());
	CHECK(frames[0]["result"]["prompts"].size() == 8);
}

TEST_CASE("MCP prompts/get returns messages array for a known prompt") {
	auto reader = test::MakeMockReader();
	tools::ToolRegistry tools;
	tools::RegisterBlueprintTools(tools, reader);
	tools::prompts::PromptRegistry prompts;
	tools::prompts::RegisterBuiltinPrompts(prompts);

	jsonrpc::Server server;
	mcp::ServerInfo info;
	mcp::RegisterHandlers(server, tools, prompts, info);

	std::string in = FrameJson(json{
		{"jsonrpc","2.0"}, {"id",3}, {"method","prompts/get"},
		{"params", json{
			{"name", "audit_bp"},
			{"arguments", json{{"asset_path", "/Game/AI/BP_Foo"}}}
		}}});
	std::istringstream is(in);
	std::ostringstream os, log;
	server.Run(is, os, log);
	std::istringstream replay(os.str());
	auto frames = ReadAllFrames(replay);
	REQUIRE(frames.size() == 1);
	REQUIRE(frames[0]["result"]["messages"].is_array());
	REQUIRE(frames[0]["result"]["messages"].size() >= 1);
	const std::string text =
		frames[0]["result"]["messages"][0]["content"]["text"].get<std::string>();
	CHECK(text.find("/Game/AI/BP_Foo") != std::string::npos);
}

TEST_CASE("MCP prompts/get with unknown name returns MethodNotFound") {
	auto reader = test::MakeMockReader();
	tools::ToolRegistry tools;
	tools::RegisterBlueprintTools(tools, reader);
	tools::prompts::PromptRegistry prompts;
	tools::prompts::RegisterBuiltinPrompts(prompts);

	jsonrpc::Server server;
	mcp::ServerInfo info;
	mcp::RegisterHandlers(server, tools, prompts, info);

	std::string in = FrameJson(json{
		{"jsonrpc","2.0"}, {"id",4}, {"method","prompts/get"},
		{"params", json{{"name", "no_such_prompt"}}}});
	std::istringstream is(in);
	std::ostringstream os, log;
	server.Run(is, os, log);
	std::istringstream replay(os.str());
	auto frames = ReadAllFrames(replay);
	REQUIRE(frames.size() == 1);
	REQUIRE(frames[0].contains("error"));
	CHECK(frames[0]["error"]["code"] == -32601);  // MethodNotFound
}

TEST_CASE("MCP prompts/get with missing required arg returns InvalidParams") {
	auto reader = test::MakeMockReader();
	tools::ToolRegistry tools;
	tools::RegisterBlueprintTools(tools, reader);
	tools::prompts::PromptRegistry prompts;
	tools::prompts::RegisterBuiltinPrompts(prompts);

	jsonrpc::Server server;
	mcp::ServerInfo info;
	mcp::RegisterHandlers(server, tools, prompts, info);

	std::string in = FrameJson(json{
		{"jsonrpc","2.0"}, {"id",5}, {"method","prompts/get"},
		{"params", json{{"name", "audit_bp"}}}});  // missing asset_path
	std::istringstream is(in);
	std::ostringstream os, log;
	server.Run(is, os, log);
	std::istringstream replay(os.str());
	auto frames = ReadAllFrames(replay);
	REQUIRE(frames.size() == 1);
	REQUIRE(frames[0].contains("error"));
	CHECK(frames[0]["error"]["code"] == -32602);  // InvalidParams
}

TEST_CASE("MCP prompts/get with non-object arguments returns InvalidParams") {
	auto reader = test::MakeMockReader();
	tools::ToolRegistry tools;
	tools::RegisterBlueprintTools(tools, reader);
	tools::prompts::PromptRegistry prompts;
	tools::prompts::RegisterBuiltinPrompts(prompts);

	jsonrpc::Server server;
	mcp::ServerInfo info;
	mcp::RegisterHandlers(server, tools, prompts, info);

	std::string in = FrameJson(json{
		{"jsonrpc","2.0"}, {"id",6}, {"method","prompts/get"},
		{"params", json{{"name", "audit_bp"}, {"arguments", "not_an_object"}}}});
	std::istringstream is(in);
	std::ostringstream os, log;
	server.Run(is, os, log);
	std::istringstream replay(os.str());
	auto frames = ReadAllFrames(replay);
	REQUIRE(frames.size() == 1);
	REQUIRE(frames[0].contains("error"));
	CHECK(frames[0]["error"]["code"] == -32602);
}

TEST_CASE("MCP without prompts registered: prompts/list returns MethodNotFound") {
	// When the 2-arg RegisterHandlers overload is used (no PromptRegistry),
	// the prompts/list and prompts/get methods are NOT registered. Older
	// clients that probe for them get a clean JSON-RPC method-not-found.
	auto reader = test::MakeMockReader();
	tools::ToolRegistry tools;
	tools::RegisterBlueprintTools(tools, reader);

	jsonrpc::Server server;
	mcp::ServerInfo info;
	mcp::RegisterHandlers(server, tools, info);  // no prompts arg

	std::string in = FrameJson(json{
		{"jsonrpc","2.0"}, {"id",7}, {"method","prompts/list"}});
	std::istringstream is(in);
	std::ostringstream os, log;
	server.Run(is, os, log);
	std::istringstream replay(os.str());
	auto frames = ReadAllFrames(replay);
	REQUIRE(frames.size() == 1);
	REQUIRE(frames[0].contains("error"));
	CHECK(frames[0]["error"]["code"] == -32601);
}
