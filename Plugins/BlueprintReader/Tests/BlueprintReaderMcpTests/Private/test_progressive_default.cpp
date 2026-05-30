// Verifies the default-on progressive-disclosure wiring that main.cpp
// applies: advertise the lean `core` surface plus the lazy-discovery
// meta-tools (list_toolsets / describe_toolset / call_tool /
// enable_tool_category), and let `call_tool` reach any non-advertised
// tool by name (the escape hatch) without widening.
//
// main.cpp itself is the production exe (can't be exercised here), so we
// mirror its exact wiring against a constructed registry to verify the
// underlying mechanism.

#include <doctest/doctest.h>

#include "tools/ToolRegistry.h"
#include "tools/BlueprintTools.h"
#include "tools/ToolsetMeta.h"
#include "test_helpers.h"

#include <algorithm>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

using nlohmann::json;
using namespace bpr;

namespace {

// Mirrors main.cpp's default-on progressive block.
void WireProgressiveDefault(tools::ToolRegistry& registry) {
	registry.ApplyFilter(std::vector<std::string>{"core"},
	                     std::vector<std::string>{});
	tools::RegisterProgressiveDisclosureMetaTool(registry);
	registry.ActivateToken("enable_tool_category");
	tools::RegisterToolsetMetaTools(registry);    // idempotent
	registry.ActivateToken("list_toolsets");
	registry.ActivateToken("describe_toolset");
	registry.ActivateToken("call_tool");
	registry.TakeListChangedFlag();
}

std::vector<std::string> SpecNames(const json& spec) {
	std::vector<std::string> names;
	for (const auto& t : spec) {
		names.push_back(t["name"].get<std::string>());
	}
	return names;
}

}    // namespace

TEST_CASE("progressive default-on: lean core surface + call_tool escape hatch") {
	auto reader = test::MakeMockReader();
	tools::ToolRegistry registry;
	tools::RegisterBlueprintTools(registry, reader);
	const size_t full = registry.ListSpec().size();
	CHECK(full == 249);    // full surface before progressive trimming

	WireProgressiveDefault(registry);
	const auto spec = registry.ListSpec();
	const auto names = SpecNames(spec);
	auto has = [&](const std::string& n) {
		return std::find(names.begin(), names.end(), n) != names.end();
	};

	// Lean: ~core (35) + the meta-tools — far below the full 249.
	CHECK(spec.size() < 60);
	CHECK(spec.size() < full);

	// Discovery + escape hatch are advertised.
	CHECK(has("call_tool"));
	CHECK(has("list_toolsets"));
	CHECK(has("describe_toolset"));
	CHECK(has("enable_tool_category"));
	// Core authoring tools are advertised directly.
	CHECK(has("add_node"));
	CHECK(has("read_blueprint"));
	// A non-core tool is filtered out of the advertised surface.
	CHECK_FALSE(has("list_data_tables"));

	// ...but call_tool reaches a non-advertised tool BY NAME, bypassing the
	// filter. Whether the mock backend supports the underlying tool or not,
	// call_tool must dispatch it — never reject it as unknown/filtered.
	const auto* callTool = registry.Find("call_tool");
	REQUIRE(callTool != nullptr);
	try {
		const json r = (*callTool)(json{
			{"name", "find_class"},
			{"arguments", json{{"name", "Actor"}}}});
		CHECK_FALSE(r.is_null());    // dispatched + returned
	} catch (const std::exception& e) {
		const std::string msg = e.what();
		CAPTURE(msg);
		// Acceptable: the underlying tool's own error. NOT acceptable: a
		// call_tool-level "unknown tool" / "filtered" rejection.
		CHECK(msg.find("unknown") == std::string::npos);
		CHECK(msg.find("filtered") == std::string::npos);
	}
}
