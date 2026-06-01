// Tests for the response-projection helper that backs the `fields`
// parameter on every read tool. JSON shapes here are minimal stand-ins
// for what BlueprintReader actually emits — the goal is to nail the
// parser + filter behavior in isolation.

#include <doctest/doctest.h>

#include "tools/JsonProjection.h"

#include <stdexcept>

using nlohmann::json;
using bpr::tools::ApplyProjection;
using bpr::tools::ParseFieldsArg;

namespace test_json_projection_detail {

json SampleBlueprint() {
	return json{
		{"name", "BP_Enemy"},
		{"parent_class", "AActor"},
		{"interfaces", json::array({"IDamageable", "IInteractable"})},
		{"variables", json::array({
			{{"name","Health"},{"type",{{"category","real"},{"sub_category","float"}}},{"replicated",true}},
			{{"name","Speed"}, {"type",{{"category","real"},{"sub_category","float"}}},{"replicated",false}},
			{{"name","Tags"},  {"type",{{"category","name"}}},                          {"replicated",false}},
		})},
		{"functions", json::array({
			{{"name","TakeDamage"},{"locals", json::array({{{"name","amount"}}})}},
			{{"name","Heal"},      {"locals", json::array()}},
		})},
	};
}

}    // namespace test_json_projection_detail
using namespace test_json_projection_detail;

TEST_CASE("ApplyProjection: empty paths is a no-op") {
	json doc = SampleBlueprint();
	json before = doc;
	ApplyProjection(doc, {});
	CHECK(doc == before);
}

TEST_CASE("ApplyProjection: top-level key keeps just that key") {
	json doc = SampleBlueprint();
	ApplyProjection(doc, {"parent_class"});
	CHECK(doc.size() == 1);
	CHECK(doc["parent_class"] == "AActor");
}

TEST_CASE("ApplyProjection: multiple top-level keys keep all of them") {
	json doc = SampleBlueprint();
	ApplyProjection(doc, {"name", "parent_class"});
	CHECK(doc.size() == 2);
	CHECK(doc.contains("name"));
	CHECK(doc.contains("parent_class"));
	CHECK_FALSE(doc.contains("variables"));
}

TEST_CASE("ApplyProjection: array element selection with []") {
	json doc = SampleBlueprint();
	ApplyProjection(doc, {"variables[].name"});
	REQUIRE(doc.size() == 1);
	REQUIRE(doc["variables"].is_array());
	CHECK(doc["variables"].size() == 3);
	for (auto& v : doc["variables"]) {
		CHECK(v.size() == 1);
		CHECK(v.contains("name"));
		CHECK_FALSE(v.contains("type"));
		CHECK_FALSE(v.contains("replicated"));
	}
}

TEST_CASE("ApplyProjection: nested object path") {
	json doc = SampleBlueprint();
	ApplyProjection(doc, {"variables[].type.category"});
	REQUIRE(doc["variables"].is_array());
	for (auto& v : doc["variables"]) {
		CHECK(v.size() == 1);
		REQUIRE(v.contains("type"));
		CHECK(v["type"].size() == 1);
		CHECK(v["type"].contains("category"));
		CHECK_FALSE(v["type"].contains("sub_category"));
	}
}

TEST_CASE("ApplyProjection: sibling paths into the same array merge") {
	json doc = SampleBlueprint();
	ApplyProjection(doc, {"variables[].name", "variables[].replicated"});
	REQUIRE(doc["variables"].is_array());
	CHECK(doc["variables"].size() == 3);
	for (auto& v : doc["variables"]) {
		CHECK(v.size() == 2);
		CHECK(v.contains("name"));
		CHECK(v.contains("replicated"));
		CHECK_FALSE(v.contains("type"));
	}
}

TEST_CASE("ApplyProjection: nonexistent path is silently ignored") {
	json doc = SampleBlueprint();
	ApplyProjection(doc, {"nope", "also_nope.deep"});
	// Everything top-level except `nope` (which doesn't exist) gets dropped.
	CHECK(doc.is_object());
	CHECK(doc.empty());
}

// --- Field aliasing (client feedback #2) -----------------------------
// Callers guess the bare adjective; the wire key carries an is_ / verbose
// form. Aliasing keeps the real key instead of silently dropping it.

TEST_CASE("ApplyProjection: `editable` alias keeps is_editable") {
	json doc = json{{"name","Health"},{"is_editable",true},{"is_replicated",false}};
	ApplyProjection(doc, {"name", "editable"});
	CHECK(doc.size() == 2);
	CHECK(doc["name"] == "Health");
	CHECK(doc["is_editable"] == true);
	CHECK_FALSE(doc.contains("is_replicated"));
}

TEST_CASE("ApplyProjection: `replicated` alias keeps is_replicated") {
	json doc = json{{"name","Health"},{"is_editable",true},{"is_replicated",true}};
	ApplyProjection(doc, {"replicated"});
	CHECK(doc.size() == 1);
	CHECK(doc["is_replicated"] == true);
}

TEST_CASE("ApplyProjection: generic is_ prefix aliases an arbitrary bool flag") {
	json doc = json{{"name","X"},{"is_transient",true},{"other",1}};
	ApplyProjection(doc, {"transient"});
	CHECK(doc.size() == 1);
	CHECK(doc["is_transient"] == true);
}

TEST_CASE("ApplyProjection: static-table alias `exposed` keeps expose_on_spawn") {
	json doc = json{{"name","X"},{"expose_on_spawn",true},{"category","Default"}};
	ApplyProjection(doc, {"exposed"});
	CHECK(doc.size() == 1);
	CHECK(doc["expose_on_spawn"] == true);
}

TEST_CASE("ApplyProjection: alias works per-element inside an array") {
	json doc = json{{"variables", json::array({
		{{"name","Health"},{"is_editable",true},{"is_replicated",true}},
		{{"name","Speed"}, {"is_editable",false},{"is_replicated",false}},
	})}};
	ApplyProjection(doc, {"variables[].name", "variables[].editable"});
	REQUIRE(doc["variables"].is_array());
	CHECK(doc["variables"].size() == 2);
	for (auto& v : doc["variables"]) {
		CHECK(v.size() == 2);
		CHECK(v.contains("name"));
		CHECK(v.contains("is_editable"));
		CHECK_FALSE(v.contains("is_replicated"));
	}
}

TEST_CASE("ApplyProjection: alias never fabricates an absent key") {
	// `expose_on_spawn` is conditionally emitted; if it's absent, asking
	// for `exposed` keeps nothing (and must not invent it).
	json doc = json{{"name","X"},{"is_editable",true}};
	ApplyProjection(doc, {"exposed"});
	CHECK(doc.is_object());
	CHECK(doc.empty());
	CHECK_FALSE(doc.contains("expose_on_spawn"));
}

TEST_CASE("ApplyProjection: missing array still keeps unrelated keys") {
	json doc = SampleBlueprint();
	ApplyProjection(doc, {"name", "macros[].name"});
	CHECK(doc.size() == 1);
	CHECK(doc["name"] == "BP_Enemy");
}

TEST_CASE("ApplyProjection: deep nested array of arrays not in real schema is OK") {
	json doc = json{
		{"functions", json::array({
			{{"name","Foo"}, {"locals", json::array({
				{{"name","x"},{"type","int"}},
				{{"name","y"},{"type","real"}},
			})}},
		})},
	};
	ApplyProjection(doc, {"functions[].locals[].name"});
	REQUIRE(doc["functions"][0]["locals"].is_array());
	CHECK(doc["functions"][0]["locals"].size() == 2);
	for (auto& loc : doc["functions"][0]["locals"]) {
		CHECK(loc.size() == 1);
		CHECK(loc.contains("name"));
	}
}

TEST_CASE("ApplyProjection: bare paths auto-descend into a top-level array") {
	// list_blueprints returns a top-level array. Callers write
	// `fields: ["asset_path"]` (no leading "[]"), and the projection
	// applies per-element automatically.
	json doc = json::array({
		{{"asset_path","/Game/A"}, {"parent_class","AActor"}, {"modified_iso","2025-01-01"}},
		{{"asset_path","/Game/B"}, {"parent_class","UObject"},{"modified_iso","2025-01-02"}},
	});
	ApplyProjection(doc, {"asset_path"});
	REQUIRE(doc.is_array());
	REQUIRE(doc.size() == 2);
	for (auto& el : doc) {
		CHECK(el.size() == 1);
		CHECK(el.contains("asset_path"));
	}
}

TEST_CASE("ApplyProjection: explicit [] on top-level array works the same way") {
	json doc = json::array({
		{{"asset_path","/Game/A"}, {"parent_class","AActor"}},
		{{"asset_path","/Game/B"}, {"parent_class","UObject"}},
	});
	ApplyProjection(doc, {"[].asset_path"});
	REQUIRE(doc.is_array());
	for (auto& el : doc) {
		CHECK(el.size() == 1);
		CHECK(el.contains("asset_path"));
	}
}

TEST_CASE("SplitPath: unmatched bracket throws") {
	json doc = SampleBlueprint();
	CHECK_THROWS_AS(ApplyProjection(doc, {"variables[.name"}), std::invalid_argument);
}

TEST_CASE("ParseFieldsArg: missing returns empty") {
	auto v = ParseFieldsArg(json::object());
	CHECK(v.empty());
}

TEST_CASE("ParseFieldsArg: null returns empty") {
	auto v = ParseFieldsArg(json{{"fields", nullptr}});
	CHECK(v.empty());
}

TEST_CASE("ParseFieldsArg: array of strings parses") {
	auto v = ParseFieldsArg(json{{"fields", json::array({"a","b.c","d[].e"})}});
	REQUIRE(v.size() == 3);
	CHECK(v[0] == "a");
	CHECK(v[1] == "b.c");
	CHECK(v[2] == "d[].e");
}

TEST_CASE("ParseFieldsArg: non-array throws") {
	CHECK_THROWS_AS(ParseFieldsArg(json{{"fields", "single"}}), std::invalid_argument);
	CHECK_THROWS_AS(ParseFieldsArg(json{{"fields", 42}}),       std::invalid_argument);
}

TEST_CASE("ParseFieldsArg: non-string element throws") {
	CHECK_THROWS_AS(ParseFieldsArg(json{{"fields", json::array({"ok", 42})}}),
					std::invalid_argument);
}
