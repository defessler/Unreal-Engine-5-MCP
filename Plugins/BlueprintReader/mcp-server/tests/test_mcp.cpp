// Smoke-tests the full MCP handshake + tools/list + tools/call sequence by
// piping framed JSON-RPC through Server::Run, reading a real Claude-style
// initialize → initialized → tools/list → tools/call list_blueprints flow.

#include <doctest/doctest.h>

#include "backends/MockBlueprintReader.h"
#include "jsonrpc/Mcp.h"
#include "jsonrpc/Server.h"
#include "tools/BlueprintTools.h"
#include "tools/ToolRegistry.h"

#include "test_helpers.h"

#include <sstream>
#include <vector>

using namespace bpr;
using nlohmann::json;

namespace {

std::string Frame(const json& body) {
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

} // namespace

TEST_CASE("MCP handshake + tools/list + tools/call list_blueprints") {
    backends::MockBlueprintReader reader(test::FixturesDir());
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
    CHECK(list.size() == 21);
    std::vector<std::string> names;
    for (auto& t : list) names.push_back(t["name"].get<std::string>());
    auto has = [&](const std::string& n) {
        return std::find(names.begin(), names.end(), n) != names.end();
    };
    for (const char* n : {"list_blueprints","read_blueprint","get_graph","get_function",
                          "list_variables","get_components","find_node","add_variable",
                          "set_node_position","delete_node","add_node","wire_pins",
                          "delete_variable","rename_variable","list_node_kinds",
                          "list_pin_categories","add_function","add_function_input",
                          "add_function_output","delete_function","set_variable_default"}) {
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
    CHECK(inner.size() == 3); // 3 fixtures all under /Game
    // Make sure the canonical asset_path key is present, not assetPath.
    CHECK(inner[0].contains("asset_path"));
    CHECK(inner[0].contains("parent_class"));
}

TEST_CASE("tools/call surfaces unknown tool as MCP error envelope, not JSON-RPC error") {
    backends::MockBlueprintReader reader(test::FixturesDir());
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
    backends::MockBlueprintReader reader(test::FixturesDir());
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
