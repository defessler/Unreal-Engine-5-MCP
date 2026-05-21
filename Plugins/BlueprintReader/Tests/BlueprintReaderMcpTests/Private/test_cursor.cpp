// Phase 5 tests: opaque base64 cursors for paginated list_* tools.

#include <doctest/doctest.h>

#include "backends/MockBlueprintReader.h"
#include "tools/BlueprintTools.h"
#include "tools/Cursor.h"
#include "tools/ToolRegistry.h"

#include "test_helpers.h"

#include <cstdint>
#include <stdexcept>
#include <string>

using namespace bpr;
using nlohmann::json;

namespace test_cursor_detail {

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

}    // namespace test_cursor_detail
using namespace test_cursor_detail;

// =====================================================================
// Cursor encode/decode unit tests
// =====================================================================

TEST_CASE("Cursor: encode/decode round-trip for 0, 1, 100, large") {
	for (std::int64_t v : {0, 1, 5, 100, 1024, 1'000'000}) {
		const auto cursor = tools::EncodeCursor(v);
		CAPTURE(v);
		CAPTURE(cursor);
		auto decoded = tools::DecodeCursor(cursor);
		REQUIRE(decoded.has_value());
		CHECK(*decoded == v);
	}
}

TEST_CASE("Cursor: encoded form is base64-alphabet-only (no `=`-padding rejection)") {
	const auto cursor = tools::EncodeCursor(42);
	// Base64 alphabet is A-Z, a-z, 0-9, +, /, =. Sanity check that
	// the encoded string only contains those — clients that pipe
	// the cursor through URL-encode shouldn't get surprises.
	for (char c : cursor) {
		const bool ok =
			(c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
			(c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=';
		CAPTURE(c);
		CHECK(ok);
	}
	// Should end in `=` padding (our payload is ~8 bytes → 12 base64).
	CHECK((cursor.back() == '='));
}

TEST_CASE("Cursor: malformed inputs return nullopt") {
	CHECK_FALSE(tools::DecodeCursor("").has_value());
	CHECK_FALSE(tools::DecodeCursor("!!!").has_value());
	CHECK_FALSE(tools::DecodeCursor("not_base64_padded").has_value());
	// Valid base64 but garbage JSON.
	CHECK_FALSE(tools::DecodeCursor("aGVsbG8=").has_value());  // "hello"
	// Wrong base64 length (not a multiple of 4).
	CHECK_FALSE(tools::DecodeCursor("abc").has_value());
}

TEST_CASE("Cursor: negative offset clamps to 0 on encode") {
	const auto c = tools::EncodeCursor(-5);
	auto decoded = tools::DecodeCursor(c);
	REQUIRE(decoded.has_value());
	CHECK(*decoded == 0);
}

// =====================================================================
// Tool integration — cursor takes precedence over offset
// =====================================================================

TEST_CASE("list_blueprints: cursor=encoded(2) skips first 2 entries") {
	Fixture f;
	auto all = f.Call("list_blueprints", json{{"path", "/Game"}});
	REQUIRE(all.is_array());
	REQUIRE(all.size() >= 3);

	const auto cursor = tools::EncodeCursor(2);
	auto page = f.Call("list_blueprints", json{
		{"path", "/Game"},
		{"cursor", cursor}});
	REQUIRE(page.is_array());
	CHECK(page.size() == all.size() - 2);
	// First entry of the cursored page should match the third entry
	// of the full list.
	CHECK(page[0] == all[2]);
}

TEST_CASE("list_blueprints: cursor overrides offset when both are passed") {
	Fixture f;
	auto all = f.Call("list_blueprints", json{{"path", "/Game"}});
	REQUIRE(all.size() >= 4);

	const auto cursor = tools::EncodeCursor(3);
	auto page = f.Call("list_blueprints", json{
		{"path", "/Game"},
		{"offset", 1},     // would skip 1 if cursor wasn't present
		{"cursor", cursor}});
	REQUIRE(page.is_array());
	CHECK(page.size() == all.size() - 3);
	CHECK(page[0] == all[3]);
}

TEST_CASE("list_blueprints: cursor + limit gives a single page") {
	Fixture f;
	const auto cursor = tools::EncodeCursor(1);
	auto page = f.Call("list_blueprints", json{
		{"path", "/Game"},
		{"cursor", cursor},
		{"limit", 2}});
	REQUIRE(page.is_array());
	CHECK(page.size() == 2);
}

TEST_CASE("list_blueprints: malformed cursor → invalid_argument") {
	Fixture f;
	CHECK_THROWS_AS(f.Call("list_blueprints", json{
		{"path", "/Game"}, {"cursor", "not-a-cursor"}}), std::invalid_argument);
}

TEST_CASE("list_blueprints: non-string cursor → invalid_argument") {
	Fixture f;
	CHECK_THROWS_AS(f.Call("list_blueprints", json{
		{"path", "/Game"}, {"cursor", 42}}), std::invalid_argument);
}

TEST_CASE("list_blueprints: empty cursor (null) is ignored — offset stays at 0") {
	Fixture f;
	auto all = f.Call("list_blueprints", json{{"path", "/Game"}});
	auto same = f.Call("list_blueprints", json{
		{"path", "/Game"}, {"cursor", nullptr}});
	CHECK(same == all);
}

TEST_CASE("list_blueprints: input_schema declares cursor property") {
	Fixture f;
	auto spec = f.registry.ListSpec();
	for (const auto& t : spec) {
		if (t["name"] == "list_blueprints") {
			REQUIRE(t["inputSchema"]["properties"].contains("cursor"));
			CHECK(t["inputSchema"]["properties"]["cursor"]["type"] == "string");
			return;
		}
	}
	FAIL("list_blueprints not found in tools/list");
}

TEST_CASE("cursor walking: page through full list using returned offsets") {
	// Simulates the agent walking: read page 1 (limit 2), get last
	// element, encode cursor of offset 2, fetch page 2.
	Fixture f;
	auto all = f.Call("list_blueprints", json{{"path", "/Game"}});
	REQUIRE(all.size() >= 4);

	auto page1 = f.Call("list_blueprints", json{
		{"path", "/Game"}, {"limit", 2}});
	REQUIRE(page1.is_array());
	CHECK(page1.size() == 2);

	const auto cursor = tools::EncodeCursor(2);
	auto page2 = f.Call("list_blueprints", json{
		{"path", "/Game"}, {"cursor", cursor}, {"limit", 2}});
	REQUIRE(page2.is_array());
	CHECK(page2.size() == 2);
	// Combined, page1 + page2 should equal the first 4 of `all`.
	CHECK(page1[0] == all[0]);
	CHECK(page1[1] == all[1]);
	CHECK(page2[0] == all[2]);
	CHECK(page2[1] == all[3]);
}

TEST_CASE("list_variables: cursor pagination works (smoke test for other list_*)") {
	Fixture f;
	const auto cursor = tools::EncodeCursor(0);
	// Just verify it doesn't throw — list_variables uses ParseResponseControls.
	auto out = f.Call("list_variables", json{
		{"asset_path", "/Game/AI/BP_Enemy"}, {"cursor", cursor}});
	CHECK(out.is_array());
}

TEST_CASE("Cursor: EncodeCursor(0) round-trips through DecodeCursor") {
	const auto c = tools::EncodeCursor(0);
	auto decoded = tools::DecodeCursor(c);
	REQUIRE(decoded.has_value());
	CHECK(*decoded == 0);
}
