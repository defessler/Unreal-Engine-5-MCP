// Phase 4 tests: Resources primitive — resources/list + resources/read,
// the three bp:// providers, and capability advertisement.

#include <doctest/doctest.h>

#include "backends/MockBlueprintReader.h"
#include "jsonrpc/Mcp.h"
#include "jsonrpc/Server.h"
#include "tools/BlueprintTools.h"
#include "tools/Resources.h"
#include "tools/ToolRegistry.h"

#include "test_helpers.h"

#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace bpr;
using nlohmann::json;

namespace test_resources_detail {

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

// A toy provider used by the registry-mechanics tests so we don't
// need a real reader.
class StaticProvider : public tools::resources::IResourceProvider {
public:
	StaticProvider(std::string uri, std::string text)
		: uri_(std::move(uri)), text_(std::move(text)) {}
	bool Handles(std::string_view uri) const override {
		return uri == uri_;
	}
	std::vector<tools::resources::ResourceDescriptor> ListResources() override {
		tools::resources::ResourceDescriptor d;
		d.uri = uri_;
		d.name = "static";
		d.mimeType = "text/plain";
		return {d};
	}
	tools::resources::ResourceContents ReadResource(std::string_view uri) override {
		if (!Handles(uri)) {
			throw std::invalid_argument("uri mismatch");
		}
		tools::resources::ResourceContents c;
		c.uri = uri_;
		c.mimeType = "text/plain";
		c.text = text_;
		return c;
	}
private:
	std::string uri_;
	std::string text_;
};

}    // namespace test_resources_detail
using namespace test_resources_detail;

// =====================================================================
// ResourceRegistry mechanics
// =====================================================================

TEST_CASE("ResourceRegistry: empty has no providers, lists nothing") {
	tools::resources::ResourceRegistry reg;
	CHECK(reg.ProviderCount() == 0);
	auto all = reg.ListAll();
	REQUIRE(all.is_array());
	CHECK(all.empty());
	CHECK_FALSE(reg.Handles("bp:///anything"));
}

TEST_CASE("ResourceRegistry: Add + ListAll aggregates across providers") {
	tools::resources::ResourceRegistry reg;
	reg.Add(std::make_unique<StaticProvider>("bp:///_a", "alpha"));
	reg.Add(std::make_unique<StaticProvider>("bp:///_b", "beta"));
	CHECK(reg.ProviderCount() == 2);
	auto all = reg.ListAll();
	REQUIRE(all.size() == 2);
	CHECK(all[0]["uri"] == "bp:///_a");
	CHECK(all[1]["uri"] == "bp:///_b");
	// Each descriptor includes name + mimeType.
	CHECK(all[0]["name"] == "static");
	CHECK(all[0]["mimeType"] == "text/plain");
}

TEST_CASE("ResourceRegistry: Read dispatches to the first matching provider") {
	tools::resources::ResourceRegistry reg;
	reg.Add(std::make_unique<StaticProvider>("bp:///_a", "alpha-body"));
	reg.Add(std::make_unique<StaticProvider>("bp:///_b", "beta-body"));
	auto body = reg.Read("bp:///_b");
	REQUIRE(body["contents"].is_array());
	REQUIRE(body["contents"].size() == 1);
	CHECK(body["contents"][0]["uri"] == "bp:///_b");
	CHECK(body["contents"][0]["text"] == "beta-body");
	CHECK(body["contents"][0]["mimeType"] == "text/plain");
}

TEST_CASE("ResourceRegistry: Read of unknown URI throws invalid_argument") {
	tools::resources::ResourceRegistry reg;
	reg.Add(std::make_unique<StaticProvider>("bp:///_a", "alpha"));
	CHECK_THROWS_AS(reg.Read("bp:///_nope"), std::invalid_argument);
}

TEST_CASE("ResourceRegistry: Handles returns true only for known URIs") {
	tools::resources::ResourceRegistry reg;
	reg.Add(std::make_unique<StaticProvider>("bp:///_a", "alpha"));
	CHECK(reg.Handles("bp:///_a"));
	CHECK_FALSE(reg.Handles("bp:///_nope"));
	CHECK_FALSE(reg.Handles(""));
}

// =====================================================================
// Built-in providers
// =====================================================================

TEST_CASE("Built-in: BlueprintAssetProvider handles bp:///Game/ URIs") {
	auto reader = test::MakeMockReader();
	auto p = tools::resources::MakeBlueprintAssetProvider(reader);
	CHECK(p->Handles("bp:///Game/AI/BP_Enemy"));
	CHECK(p->Handles("bp:///Game/UI/W_Foo"));
	CHECK_FALSE(p->Handles("bp:///_project"));
	CHECK_FALSE(p->Handles("bp:///_output_log"));
	CHECK_FALSE(p->Handles("not-bp-scheme"));
}

TEST_CASE("Built-in: BlueprintAssetProvider ReadResource returns BP metadata JSON") {
	auto reader = test::MakeMockReader();
	auto p = tools::resources::MakeBlueprintAssetProvider(reader);
	auto c = p->ReadResource("bp:///Game/AI/BP_Enemy");
	CHECK(c.uri == "bp:///Game/AI/BP_Enemy");
	CHECK(c.mimeType == "application/json");
	CHECK_FALSE(c.text.empty());
	// Parse it back to verify it's the BP metadata shape.
	auto parsed = json::parse(c.text);
	CHECK(parsed.is_object());
	CHECK(parsed.contains("asset_path"));
}

TEST_CASE("Built-in: BlueprintAssetProvider ListResources enumerates /Game") {
	auto reader = test::MakeMockReader();
	auto p = tools::resources::MakeBlueprintAssetProvider(reader);
	auto descriptors = p->ListResources();
	CHECK(descriptors.size() > 0);
	for (const auto& d : descriptors) {
		CAPTURE(d.uri);
		CHECK(d.uri.rfind("bp:///Game/", 0) == 0);
		CHECK(d.mimeType == "application/json");
	}
}

TEST_CASE("Built-in: ProjectMetadataProvider handles only bp:///_project") {
	auto reader = test::MakeMockReader();
	auto p = tools::resources::MakeProjectMetadataProvider(reader);
	CHECK(p->Handles("bp:///_project"));
	CHECK_FALSE(p->Handles("bp:///_output_log"));
	CHECK_FALSE(p->Handles("bp:///Game/AI/BP_Enemy"));
}

TEST_CASE("Built-in: ProjectMetadataProvider ReadResource on mock throws (no editor)") {
	// Mock backend's GetProjectMetadata throws — the provider lets
	// that bubble. The registry-level test verifies it surfaces as a
	// resources/read failure cleanly.
	auto reader = test::MakeMockReader();
	auto p = tools::resources::MakeProjectMetadataProvider(reader);
	CHECK_THROWS(p->ReadResource("bp:///_project"));
}

TEST_CASE("Built-in: OutputLogProvider handles only bp:///_output_log") {
	auto reader = test::MakeMockReader();
	auto p = tools::resources::MakeOutputLogProvider(reader);
	CHECK(p->Handles("bp:///_output_log"));
	CHECK_FALSE(p->Handles("bp:///_project"));
}

TEST_CASE("Built-in: 3 providers in a registry expose 3 advertised resources") {
	auto reader = test::MakeMockReader();
	tools::resources::ResourceRegistry reg;
	reg.Add(tools::resources::MakeBlueprintAssetProvider(reader));
	reg.Add(tools::resources::MakeProjectMetadataProvider(reader));
	reg.Add(tools::resources::MakeOutputLogProvider(reader));
	CHECK(reg.ProviderCount() == 3);
	auto all = reg.ListAll();
	REQUIRE(all.is_array());
	// At minimum the project + output-log are advertised; BPs depend
	// on mock having fixtures, which it does.
	std::vector<std::string> uris;
	for (const auto& d : all) {
		uris.push_back(d.value("uri", ""));
	}
	auto has = [&](const std::string& u) {
		return std::find(uris.begin(), uris.end(), u) != uris.end();
	};
	CHECK(has("bp:///_project"));
	CHECK(has("bp:///_output_log"));
}

// =====================================================================
// JSON-RPC dispatch
// =====================================================================

TEST_CASE("MCP initialize advertises resources capability when providers wired") {
	auto reader = test::MakeMockReader();
	tools::ToolRegistry toolReg;
	tools::RegisterBlueprintTools(toolReg, reader);
	tools::resources::ResourceRegistry res;
	res.Add(tools::resources::MakeBlueprintAssetProvider(reader));

	jsonrpc::Server server;
	mcp::ServerInfo info;
	mcp::RegisterHandlers(server, toolReg, /*prompts=*/nullptr,
						   /*logger=*/nullptr, &res, info);

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
	REQUIRE(frames[0]["result"]["capabilities"].contains("resources"));
	CHECK(frames[0]["result"]["capabilities"]["resources"].is_object());
}

TEST_CASE("MCP initialize omits resources capability when no providers") {
	auto reader = test::MakeMockReader();
	tools::ToolRegistry toolReg;
	tools::RegisterBlueprintTools(toolReg, reader);
	tools::resources::ResourceRegistry emptyRes;  // 0 providers

	jsonrpc::Server server;
	mcp::ServerInfo info;
	mcp::RegisterHandlers(server, toolReg, /*prompts=*/nullptr,
						   /*logger=*/nullptr, &emptyRes, info);

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
	CHECK_FALSE(frames[0]["result"]["capabilities"].contains("resources"));
}

TEST_CASE("MCP resources/list returns all registered resources") {
	auto reader = test::MakeMockReader();
	tools::ToolRegistry toolReg;
	tools::RegisterBlueprintTools(toolReg, reader);
	tools::resources::ResourceRegistry res;
	res.Add(tools::resources::MakeProjectMetadataProvider(reader));
	res.Add(tools::resources::MakeOutputLogProvider(reader));

	jsonrpc::Server server;
	mcp::ServerInfo info;
	mcp::RegisterHandlers(server, toolReg, /*prompts=*/nullptr,
						   /*logger=*/nullptr, &res, info);

	std::string in = FrameJson(json{
		{"jsonrpc","2.0"}, {"id",1}, {"method","resources/list"}});
	std::istringstream is(in);
	std::ostringstream os, log;
	server.Run(is, os, log);
	std::istringstream replay(os.str());
	auto frames = ReadAllFrames(replay);
	REQUIRE(frames.size() == 1);
	REQUIRE(frames[0]["result"]["resources"].is_array());
	CHECK(frames[0]["result"]["resources"].size() == 2);
}

TEST_CASE("MCP resources/read returns contents for known URI") {
	auto reader = test::MakeMockReader();
	tools::ToolRegistry toolReg;
	tools::RegisterBlueprintTools(toolReg, reader);
	tools::resources::ResourceRegistry res;
	res.Add(tools::resources::MakeBlueprintAssetProvider(reader));

	jsonrpc::Server server;
	mcp::ServerInfo info;
	mcp::RegisterHandlers(server, toolReg, /*prompts=*/nullptr,
						   /*logger=*/nullptr, &res, info);

	std::string in = FrameJson(json{
		{"jsonrpc","2.0"}, {"id",1}, {"method","resources/read"},
		{"params", json{{"uri", "bp:///Game/AI/BP_Enemy"}}}});
	std::istringstream is(in);
	std::ostringstream os, log;
	server.Run(is, os, log);
	std::istringstream replay(os.str());
	auto frames = ReadAllFrames(replay);
	REQUIRE(frames.size() == 1);
	REQUIRE(frames[0]["result"]["contents"].is_array());
	CHECK(frames[0]["result"]["contents"][0]["uri"] == "bp:///Game/AI/BP_Enemy");
	CHECK(frames[0]["result"]["contents"][0]["mimeType"] == "application/json");
	CHECK_FALSE(frames[0]["result"]["contents"][0]["text"].get<std::string>().empty());
}

TEST_CASE("MCP resources/read for unknown URI returns -32002 ResourceNotFound") {
	auto reader = test::MakeMockReader();
	tools::ToolRegistry toolReg;
	tools::RegisterBlueprintTools(toolReg, reader);
	tools::resources::ResourceRegistry res;
	res.Add(tools::resources::MakeBlueprintAssetProvider(reader));

	jsonrpc::Server server;
	mcp::ServerInfo info;
	mcp::RegisterHandlers(server, toolReg, /*prompts=*/nullptr,
						   /*logger=*/nullptr, &res, info);

	std::string in = FrameJson(json{
		{"jsonrpc","2.0"}, {"id",1}, {"method","resources/read"},
		{"params", json{{"uri", "bp:///_nonsense"}}}});
	std::istringstream is(in);
	std::ostringstream os, log;
	server.Run(is, os, log);
	std::istringstream replay(os.str());
	auto frames = ReadAllFrames(replay);
	REQUIRE(frames.size() == 1);
	REQUIRE(frames[0].contains("error"));
	CHECK(frames[0]["error"]["code"] == -32002);
}

TEST_CASE("MCP resources/read with non-string uri → InvalidParams") {
	auto reader = test::MakeMockReader();
	tools::ToolRegistry toolReg;
	tools::RegisterBlueprintTools(toolReg, reader);
	tools::resources::ResourceRegistry res;
	res.Add(tools::resources::MakeBlueprintAssetProvider(reader));

	jsonrpc::Server server;
	mcp::ServerInfo info;
	mcp::RegisterHandlers(server, toolReg, /*prompts=*/nullptr,
						   /*logger=*/nullptr, &res, info);

	std::string in = FrameJson(json{
		{"jsonrpc","2.0"}, {"id",1}, {"method","resources/read"},
		{"params", json{{"uri", 42}}}});
	std::istringstream is(in);
	std::ostringstream os, log;
	server.Run(is, os, log);
	std::istringstream replay(os.str());
	auto frames = ReadAllFrames(replay);
	REQUIRE(frames.size() == 1);
	REQUIRE(frames[0].contains("error"));
	CHECK(frames[0]["error"]["code"] == -32602);
}

TEST_CASE("MCP resources/* not registered when no resources provided") {
	auto reader = test::MakeMockReader();
	tools::ToolRegistry toolReg;
	tools::RegisterBlueprintTools(toolReg, reader);

	jsonrpc::Server server;
	mcp::ServerInfo info;
	mcp::RegisterHandlers(server, toolReg, info);  // 3-arg overload

	std::string in = FrameJson(json{
		{"jsonrpc","2.0"}, {"id",1}, {"method","resources/list"}});
	std::istringstream is(in);
	std::ostringstream os, log;
	server.Run(is, os, log);
	std::istringstream replay(os.str());
	auto frames = ReadAllFrames(replay);
	REQUIRE(frames.size() == 1);
	REQUIRE(frames[0].contains("error"));
	CHECK(frames[0]["error"]["code"] == -32601);
}
