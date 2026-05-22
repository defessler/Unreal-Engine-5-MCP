// Phase 8 (EA-pull Wave 1, partial) tests: editor-awareness reads.
//
// The 5 shipped tools all require a live editor, so coverage at this
// level is shape-focused: tools are registered, declare expected
// schemas, and route through the IBlueprintReader interface.
// Mock-backend integration tests verify the unsupported-error path.

#include <doctest/doctest.h>

#include "backends/MockBlueprintReader.h"
#include "tools/BlueprintTools.h"
#include "tools/ToolRegistry.h"

#include "test_helpers.h"

#include <string>

using namespace bpr;
using nlohmann::json;

namespace test_ea_pull_detail {

struct Fixture {
	backends::MockBlueprintReader reader;
	tools::ToolRegistry registry;
	Fixture() : reader(test::FixturesDir()) {
		tools::RegisterBlueprintTools(registry, reader);
	}
	json Call(const std::string& name, json args) {
		const auto* fn = registry.Find(name);
		REQUIRE(fn != nullptr);
		return (*fn)(args);
	}
};

}    // namespace test_ea_pull_detail
using namespace test_ea_pull_detail;

// =====================================================================
// Registration + schema declarations
// =====================================================================

TEST_CASE("Phase 8: all 5 EA-pull tools register with the registry") {
	Fixture f;
	auto spec = f.registry.ListSpec();
	std::vector<std::string> names;
	for (const auto& t : spec) {
		names.push_back(t["name"].get<std::string>());
	}
	auto has = [&](const std::string& n) {
		return std::find(names.begin(), names.end(), n) != names.end();
	};
	for (const char* n : {"list_open_assets", "get_active_asset",
						  "get_compile_status", "get_dirty_packages",
						  "get_focused_window"}) {
		CAPTURE(n);
		CHECK(has(n));
	}
}

TEST_CASE("Phase 8: get_compile_status declares asset_path required") {
	Fixture f;
	auto spec = f.registry.ListSpec();
	for (const auto& t : spec) {
		if (t["name"] == "get_compile_status") {
			REQUIRE(t["inputSchema"].contains("required"));
			const auto& req = t["inputSchema"]["required"];
			REQUIRE(req.is_array());
			bool sawAssetPath = false;
			for (const auto& r : req) {
				if (r == "asset_path") sawAssetPath = true;
			}
			CHECK(sawAssetPath);
			return;
		}
	}
	FAIL("get_compile_status not found");
}

TEST_CASE("Phase 8: all 5 tools advertise outputSchema") {
	Fixture f;
	auto spec = f.registry.ListSpec();
	int matched = 0;
	for (const auto& t : spec) {
		const std::string n = t["name"].get<std::string>();
		if (n == "list_open_assets" || n == "get_active_asset" ||
			n == "get_compile_status" || n == "get_dirty_packages" ||
			n == "get_focused_window") {
			++matched;
			CAPTURE(n);
			REQUIRE(t.contains("outputSchema"));
			REQUIRE(t["outputSchema"].contains("type"));
		}
	}
	CHECK(matched == 5);
}

TEST_CASE("Phase 8: 4 collection tools have type=object|array outputSchema") {
	Fixture f;
	auto spec = f.registry.ListSpec();
	for (const auto& t : spec) {
		const std::string n = t["name"].get<std::string>();
		if (n == "list_open_assets" || n == "get_dirty_packages") {
			CAPTURE(n);
			CHECK(t["outputSchema"]["type"] == "array");
		}
		if (n == "get_active_asset" || n == "get_compile_status" ||
			n == "get_focused_window") {
			CAPTURE(n);
			CHECK(t["outputSchema"]["type"] == "object");
		}
	}
}

// =====================================================================
// Mock backend: all 5 tools throw "not supported"
// =====================================================================

TEST_CASE("Phase 8: mock backend rejects list_open_assets") {
	Fixture f;
	CHECK_THROWS(f.Call("list_open_assets", json::object()));
}

TEST_CASE("Phase 8: mock backend rejects get_active_asset") {
	Fixture f;
	CHECK_THROWS(f.Call("get_active_asset", json::object()));
}

TEST_CASE("Phase 8: mock backend rejects get_compile_status") {
	Fixture f;
	CHECK_THROWS(f.Call("get_compile_status",
		json{{"asset_path", "/Game/AI/BP_Enemy"}}));
}

TEST_CASE("Phase 8: get_compile_status validates asset_path is required") {
	Fixture f;
	CHECK_THROWS_AS(f.Call("get_compile_status", json::object()),
					std::invalid_argument);
}

TEST_CASE("Phase 8: mock backend rejects get_dirty_packages") {
	Fixture f;
	CHECK_THROWS(f.Call("get_dirty_packages", json::object()));
}

TEST_CASE("Phase 8: mock backend rejects get_focused_window") {
	Fixture f;
	CHECK_THROWS(f.Call("get_focused_window", json::object()));
}

// =====================================================================
// Mock backend declares them in UnsupportedTools()
// =====================================================================

TEST_CASE("Phase 8: mock UnsupportedTools includes all 5 EA-pull tools") {
	backends::MockBlueprintReader r(test::FixturesDir());
	auto unsupported = r.UnsupportedTools();
	auto has = [&](const std::string& n) {
		return std::find(unsupported.begin(), unsupported.end(), n)
			!= unsupported.end();
	};
	for (const char* n : {"list_open_assets", "get_active_asset",
						  "get_compile_status", "get_dirty_packages",
						  "get_focused_window"}) {
		CAPTURE(n);
		CHECK(has(n));
	}
}

// =====================================================================
// Annotations — these tools are read-only
// =====================================================================

TEST_CASE("Phase 8: all 5 EA-pull tools advertise readOnlyHint=true") {
	Fixture f;
	auto spec = f.registry.ListSpec();
	for (const auto& t : spec) {
		const std::string n = t["name"].get<std::string>();
		if (n == "list_open_assets" || n == "get_active_asset" ||
			n == "get_compile_status" || n == "get_dirty_packages" ||
			n == "get_focused_window") {
			CAPTURE(n);
			REQUIRE(t.contains("annotations"));
			CHECK(t["annotations"]["readOnlyHint"] == true);
		}
	}
}
