// Test the BlueprintTools layer directly — bypasses MCP framing, calls the
// tool handlers as plain functions of `arguments`.

#include <doctest/doctest.h>

#include "backends/MockBlueprintReader.h"
#include "tools/BlueprintTools.h"
#include "tools/ToolRegistry.h"

#include "test_helpers.h"

using namespace bpr;
using nlohmann::json;

namespace test_tools_detail {

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

}    // namespace test_tools_detail
using namespace test_tools_detail;

TEST_CASE("ToolRegistry exposes 228 tools (227 prior + Phase 17 +1 — get_workspace_layout) with input schemas") {
	Fixture f;
	auto spec = f.registry.ListSpec();
	CHECK(spec.size() == 228);
	for (const auto& t : spec) {
		CHECK(t["inputSchema"]["type"] == "object");
	}
}

TEST_CASE("ValidateToolName accepts spec-compliant names, rejects others") {
	// MCP 2025-11-25 tool name rule: 1-128 chars, [A-Za-z0-9_.-]
	CHECK(tools::ValidateToolName("read_blueprint").empty());
	CHECK(tools::ValidateToolName("a").empty());
	CHECK(tools::ValidateToolName("name-with-hyphen").empty());
	CHECK(tools::ValidateToolName("name.with.dot").empty());
	CHECK(tools::ValidateToolName("MixedCase123").empty());

	CHECK_FALSE(tools::ValidateToolName("").empty());          // empty
	CHECK_FALSE(tools::ValidateToolName("has space").empty()); // space
	CHECK_FALSE(tools::ValidateToolName("slash/in").empty());  // slash
	CHECK_FALSE(tools::ValidateToolName(std::string(129, 'x')).empty()); // > 128 chars
}

TEST_CASE("ToolRegistry::Add only HARD-rejects empty names; soft-warns on others") {
	// Per Epic 5.8: only empty names are unrecoverable. Length / invalid
	// chars warn but accept — strict MCP clients may reject downstream,
	// but other registrations stay healthy.
	tools::ToolRegistry r;
	auto noop = [](const nlohmann::json&) { return nlohmann::json::object(); };
	// Empty name still throws (no key to dispatch on).
	CHECK_THROWS_AS(r.Add({"", "d", nlohmann::json::object(), nullptr}, noop),
		std::invalid_argument);
	// Space and slash now warn-not-throw — registration succeeds.
	CHECK_NOTHROW(r.Add({"has space", "d", nlohmann::json::object(), nullptr}, noop));
	CHECK_NOTHROW(r.Add({"slash/in", "d", nlohmann::json::object(), nullptr}, noop));
	CHECK_NOTHROW(r.Add({"valid_name", "d", nlohmann::json::object(), nullptr}, noop));
	CHECK(r.TotalRegistered() == 3);
}

// ===== sort opt-in =======================================================
// Phase B's `sort` arg on list_* tools. Default "natural" preserves the
// backend's order so zero-break holds for clients that never opt in.

TEST_CASE("list_blueprints: default sort is natural (backend order preserved)") {
	Fixture f;
	auto out = f.Call("list_blueprints", json{{"path", "/Game"}});
	REQUIRE(out.is_array());
	// Mock fixtures define a specific order; just verify the call works
	// without sort and returns something non-empty.
	CHECK(out.size() > 0);
}

TEST_CASE("list_blueprints: sort=path returns entries sorted by asset_path") {
	Fixture f;
	auto out = f.Call("list_blueprints", json{{"path", "/Game"}, {"sort", "path"}});
	REQUIRE(out.is_array());
	REQUIRE(out.size() >= 2);
	// Pairwise check: each entry's asset_path is <= the next.
	for (size_t i = 1; i < out.size(); ++i) {
		const auto a = out[i-1].value("asset_path", "");
		const auto b = out[i].value("asset_path", "");
		CAPTURE(a);
		CAPTURE(b);
		CHECK(a <= b);
	}
}

TEST_CASE("list_blueprints: sort=invalid is rejected with a clear error") {
	Fixture f;
	CHECK_THROWS_AS(f.Call("list_blueprints",
		json{{"path","/Game"}, {"sort","bogus"}}), std::invalid_argument);
}

TEST_CASE("list_assets: sort=name returns entries sorted alphabetically by name field") {
	Fixture f;
	// Mock backend doesn't have list_assets (registry not present), so
	// the call surfaces as a tool error. We check that the SORT arg
	// is accepted at parse-time (schema-validation) by reaching the
	// backend dispatch — the error then comes from the backend itself.
	// If the sort parser rejected the call, we'd get an invalid_argument
	// throw INSTEAD of the backend's "not supported" error.
	try {
		f.Call("list_assets", json{{"path","/Game"}, {"sort","name"}});
		// On mock backend with no assets, may return empty array — fine.
	} catch (const std::invalid_argument&) {
		FAIL("sort=name should not be rejected as an invalid argument");
	} catch (...) {
		// Backend-not-supported on mock — expected; doesn't fail the test.
	}
}

TEST_CASE("take_screenshot rejects path-traversal in dest_path") {
	Fixture f;
	// Mock backend doesn't actually take a screenshot — but the path
	// validation runs in the tool handler BEFORE hitting the backend.
	// A traversal arg should surface as an MCP-level exception thrown
	// from the lambda; CHECK_THROWS catches that.
	CHECK_THROWS_AS(f.Call("take_screenshot",
		json{{"dest_path", "../escape.png"}}), std::invalid_argument);
}

TEST_CASE("take_viewport_screenshot rejects path-traversal in dest_path") {
	Fixture f;
	CHECK_THROWS_AS(f.Call("take_viewport_screenshot",
		json{{"dest_path", "/safe/../escape.png"}}), std::invalid_argument);
}

TEST_CASE("take_annotated_screenshot rejects path-traversal in dest_path") {
	Fixture f;
	CHECK_THROWS_AS(f.Call("take_annotated_screenshot",
		json{{"dest_path", "C:\\foo\\..\\escape.png"}}), std::invalid_argument);
}

TEST_CASE("Discoverability: list_node_kinds returns the dispatch table") {
	Fixture f;
	auto out = f.Call("list_node_kinds", json::object());
	REQUIRE(out.is_array());
	CHECK(out.size() == 12);
	std::vector<std::string> kinds;
	for (auto& k : out)
	{
		kinds.push_back(k["kind"].get<std::string>());
	}
	auto has = [&](const std::string& s) {
		return std::find(kinds.begin(), kinds.end(), s) != kinds.end();
	};
	for (const char* k : {"Branch","Sequence","VariableGet","VariableSet","CallFunction",
						  "CustomEvent","Cast","Self","MakeArray","MakeStruct",
						  "FormatText","Knot"}) {
		CHECK(has(k));
	}
	// CallFunction declares its required extras.
	for (auto& k : out) {
		if (k["kind"] == "CallFunction") {
			REQUIRE(k["extras"].is_array());
			CHECK(k["extras"].size() == 2);
		}
	}
}

TEST_CASE("Discoverability: list_pin_categories returns categories + containers") {
	Fixture f;
	auto out = f.Call("list_pin_categories", json::object());
	REQUIRE(out.contains("categories"));
	REQUIRE(out["categories"].is_array());
	CHECK(out["categories"].size() >= 16);  // soft_object + soft_class added
	REQUIRE(out.contains("containers"));
	CHECK(out["containers"].size() == 3);
	// soft refs are first-class — verify they appear in the discovery list.
	bool sawSoftObject = false, sawSoftClass = false;
	for (const auto& c : out["categories"]) {
		if (c.value("category", "") == "soft_object")
		{
			sawSoftObject = true;
		}
		if (c.value("category", "") == "soft_class")
		{
			sawSoftClass  = true;
		}
	}
	CHECK(sawSoftObject);
	CHECK(sawSoftClass);
}

TEST_CASE("Write tools throw on the mock backend (read-only by design)") {
	Fixture f;
	CHECK_THROWS_AS(f.Call("add_variable", json{
		{"asset_path", "/Game/AI/BP_Enemy"},
		{"name", "NewVar"},
		{"type", json{{"category","bool"}}}
	}), bpr::backends::BlueprintReaderError);
	CHECK_THROWS_AS(f.Call("set_node_position", json{
		{"asset_path","/Game/AI/BP_Enemy"}, {"graph_name","EventGraph"},
		{"node_id","00000000-0000-0000-0000-000000000000"}, {"x",0}, {"y",0}
	}), bpr::backends::BlueprintReaderError);
	CHECK_THROWS_AS(f.Call("delete_node", json{
		{"asset_path","/Game/AI/BP_Enemy"}, {"graph_name","EventGraph"},
		{"node_id","00000000-0000-0000-0000-000000000000"}
	}), bpr::backends::BlueprintReaderError);
}

TEST_CASE("list_blueprints returns canonical BPAssetSummary array") {
	Fixture f;
	auto out = f.Call("list_blueprints", json{{"path", "/Game"}});
	REQUIRE(out.is_array());
	CHECK(out.size() == 7);
	CHECK(out[0].contains("asset_path"));
	CHECK(out[0].contains("parent_class"));
	CHECK(out[0].contains("modified_iso"));
}

TEST_CASE("find_dangling_references skips inherited parent-class variable references") {
	Fixture f;
	auto out = f.Call("find_dangling_references",
		json{{"asset_path", "/Game/Test/BP_Inherited"}});
	// Only MissingVar (an own-class member that isn't declared) is dangling.
	// PlayerCameraManager is inherited from APlayerController, and OwnVar
	// exists — neither may be flagged.
	REQUIRE(out["total"].get<int>() == 1);
	REQUIRE(out["dangling"].is_array());
	REQUIRE(out["dangling"].size() == 1);
	CHECK(out["dangling"][0]["missing"] == "MissingVar");
	CHECK(out["dangling"][0]["symbol_type"] == "variable");
	for (const auto& d : out["dangling"]) {
		CHECK(d["missing"] != "PlayerCameraManager");
		CHECK(d["missing"] != "OwnVar");
	}
}

TEST_CASE("read_blueprint returns canonical BPMetadata") {
	Fixture f;
	auto out = f.Call("read_blueprint", json{{"asset_path", "/Game/Items/BP_Pickup"}});
	CHECK(out["name"] == "BP_Pickup");
	CHECK(out["parent_class"] == "AActor");
	REQUIRE(out["interfaces"].is_array());
	CHECK(out["interfaces"][0] == "IInteractable");
}

TEST_CASE("get_graph default name is EventGraph") {
	Fixture f;
	auto out = f.Call("get_graph", json{{"asset_path", "/Game/AI/BP_Enemy"}});
	CHECK(out["name"] == "EventGraph");
	CHECK(out["nodes"].size() >= 6);
}

TEST_CASE("get_function returns full function shape") {
	Fixture f;
	auto out = f.Call("get_function", json{
		{"asset_path", "/Game/Player/BP_PlayerController"},
		{"function_name", "AddScore"}});
	CHECK(out["name"] == "AddScore");
	CHECK(out["locals"].size() == 2);
	CHECK(out["graph"]["type"] == "Function");
}

TEST_CASE("list_variables returns the variables array") {
	Fixture f;
	auto out = f.Call("list_variables", json{{"asset_path", "/Game/AI/BP_Enemy"}});
	REQUIRE(out.is_array());
	CHECK(out.size() == 3);
	CHECK(out[0].contains("is_replicated"));
}

TEST_CASE("find_node returns matching nodes") {
	Fixture f;
	auto out = f.Call("find_node", json{
		{"asset_path", "/Game/AI/BP_Enemy"},
		{"query", "Sequence"}});
	REQUIRE(out.is_array());
	REQUIRE(out.size() == 1);
	CHECK(out[0]["class"] == "K2Node_ExecutionSequence");
}

TEST_CASE("Tool handlers throw on missing required arg") {
	Fixture f;
	CHECK_THROWS_AS(f.Call("read_blueprint", json::object()), std::invalid_argument);
	CHECK_THROWS_AS(f.Call("get_function", json{{"asset_path", "/Game/AI/BP_Enemy"}}),
					std::invalid_argument);
}

// ===== Response controls (fields / limit / offset) =========================

TEST_CASE("summarize_blueprint returns counts plus parent_class") {
	Fixture f;
	auto out = f.Call("summarize_blueprint", json{{"asset_path", "/Game/AI/BP_Enemy"}});
	CHECK(out["asset_path"] == "/Game/AI/BP_Enemy");
	CHECK(out["parent_class"] == "ACharacter");
	CHECK(out["variable_count"].is_number());
	CHECK(out["function_count"].is_number());
	CHECK(out["graph_count"].is_number());
	CHECK(out["macro_count"].is_number());
	CHECK(out["interface_count"].is_number());
	// Sanity: counts must agree with what list_variables / read_blueprint say.
	auto vars = f.Call("list_variables", json{{"asset_path", "/Game/AI/BP_Enemy"}});
	CHECK(out["variable_count"] == static_cast<int>(vars.size()));
}

TEST_CASE("summarize_blueprint payload is small (~few hundred bytes)") {
	Fixture f;
	auto out = f.Call("summarize_blueprint", json{{"asset_path", "/Game/AI/BP_Enemy"}});
	auto full = f.Call("read_blueprint", json{{"asset_path", "/Game/AI/BP_Enemy"}});
	CHECK(out.dump().size() < full.dump().size() / 2);
}

TEST_CASE("read_blueprint honors fields projection") {
	Fixture f;
	auto out = f.Call("read_blueprint", json{
		{"asset_path", "/Game/AI/BP_Enemy"},
		{"fields", json::array({"parent_class"})}});
	CHECK(out.size() == 1);
	CHECK(out["parent_class"] == "ACharacter");
	CHECK_FALSE(out.contains("variables"));
}

TEST_CASE("read_blueprint with array projection on variables[].name") {
	Fixture f;
	auto out = f.Call("read_blueprint", json{
		{"asset_path", "/Game/AI/BP_Enemy"},
		{"fields", json::array({"variables[].name"})}});
	CHECK(out.size() == 1);
	REQUIRE(out["variables"].is_array());
	for (auto& v : out["variables"]) {
		CHECK(v.size() == 1);
		CHECK(v.contains("name"));
	}
}

TEST_CASE("list_blueprints honors limit/offset pagination") {
	Fixture f;
	auto all = f.Call("list_blueprints", json{{"path","/Game"}});
	REQUIRE(all.is_array());
	REQUIRE(all.size() >= 2);

	auto first = f.Call("list_blueprints", json{{"path","/Game"},{"limit",1}});
	REQUIRE(first.is_array());
	CHECK(first.size() == 1);
	CHECK(first[0]["asset_path"] == all[0]["asset_path"]);

	auto second = f.Call("list_blueprints", json{{"path","/Game"},{"limit",1},{"offset",1}});
	REQUIRE(second.is_array());
	CHECK(second.size() == 1);
	CHECK(second[0]["asset_path"] == all[1]["asset_path"]);
}

TEST_CASE("list_blueprints with fields returns just the requested keys per element") {
	Fixture f;
	auto out = f.Call("list_blueprints", json{
		{"path","/Game"},
		{"fields", json::array({"asset_path"})}});
	REQUIRE(out.is_array());
	CHECK(out.size() >= 1);
	for (auto& el : out) {
		CHECK(el.size() == 1);
		CHECK(el.contains("asset_path"));
	}
}

TEST_CASE("list_variables honors limit/offset and fields together") {
	Fixture f;
	auto all = f.Call("list_variables", json{{"asset_path","/Game/AI/BP_Enemy"}});
	REQUIRE(all.is_array());

	auto sliced = f.Call("list_variables", json{
		{"asset_path","/Game/AI/BP_Enemy"},
		{"limit", 2},
		{"fields", json::array({"name"})}});
	REQUIRE(sliced.is_array());
	CHECK(sliced.size() == std::min<std::size_t>(2, all.size()));
	for (auto& v : sliced) {
		CHECK(v.size() == 1);
		CHECK(v.contains("name"));
	}
}

TEST_CASE("Negative limit/offset throw") {
	Fixture f;
	CHECK_THROWS_AS(f.Call("list_blueprints",
						   json{{"path","/Game"},{"offset",-1}}),
					std::invalid_argument);
	// limit < -1 disallowed; -1 is the sentinel (no cap) so it must be allowed.
	CHECK_THROWS_AS(f.Call("list_blueprints",
						   json{{"path","/Game"},{"limit",-2}}),
					std::invalid_argument);
	CHECK_NOTHROW(f.Call("list_blueprints",
						 json{{"path","/Game"},{"limit",-1}}));
}

TEST_CASE("offset past end yields empty array, not error") {
	Fixture f;
	auto out = f.Call("list_blueprints", json{
		{"path","/Game"}, {"offset", 9999}});
	REQUIRE(out.is_array());
	CHECK(out.empty());
}

TEST_CASE("fields with non-string element throws invalid_argument") {
	Fixture f;
	CHECK_THROWS_AS(f.Call("read_blueprint", json{
		{"asset_path","/Game/AI/BP_Enemy"},
		{"fields", json::array({"name", 42})}}),
		std::invalid_argument);
}

// ===== Composability tools (get_node / find_overriders) ====================

TEST_CASE("get_node: fetch a single node by GUID") {
	Fixture f;
	auto graph = f.Call("get_graph", json{{"asset_path","/Game/AI/BP_Enemy"}});
	REQUIRE(graph["nodes"].is_array());
	REQUIRE(!graph["nodes"].empty());
	std::string guid = graph["nodes"][0]["id"].get<std::string>();
	auto node = f.Call("get_node", json{
		{"asset_path","/Game/AI/BP_Enemy"},
		{"graph_name","EventGraph"},
		{"node_id", guid}});
	CHECK(node["id"] == guid);
	CHECK(node.contains("class"));
	CHECK(node.contains("pins"));
}

TEST_CASE("get_node: missing node throws AssetNotFound") {
	Fixture f;
	CHECK_THROWS_AS(f.Call("get_node", json{
		{"asset_path","/Game/AI/BP_Enemy"},
		{"graph_name","EventGraph"},
		{"node_id","00000000-0000-0000-0000-deadbeefdead"}}),
		bpr::backends::AssetNotFound);
}

TEST_CASE("read_blueprint: AssetNotFound unmodified when FindAsset throws (mock backend)") {
	// Mock's IBlueprintReader::FindAsset default throws — the hint
	// computation must swallow that and leave the original AssetNotFound
	// intact (no "did you mean" suffix appended). Positive-case hint
	// generation is exercised by live tests against a real asset
	// registry; here we just verify the failure mode is silent.
	Fixture f;
	try {
		f.Call("read_blueprint", json{{"asset_path","/Game/AI/NoSuchAsset"}});
		FAIL("expected AssetNotFound");
	} catch (const bpr::backends::AssetNotFound& e) {
		const std::string msg = e.what();
		CHECK(msg.find("did you mean") == std::string::npos);
	}
}

TEST_CASE("find_overriders: requires at least one filter") {
	Fixture f;
	CHECK_THROWS_AS(f.Call("find_overriders", json{{"path","/Game"}}),
					std::invalid_argument);
}

TEST_CASE("find_overriders: parent_class filter (short name match)") {
	Fixture f;
	auto out = f.Call("find_overriders", json{
		{"path", "/Game"},
		{"parent_class", "ACharacter"}});
	REQUIRE(out.is_array());
	// BP_Enemy parent is ACharacter in the fixtures; should match.
	bool foundEnemy = false;
	for (auto& el : out) {
		CHECK(el.contains("matched"));
		CHECK(el.contains("asset_path"));
		if (el["asset_path"] == "/Game/AI/BP_Enemy")
		{
			foundEnemy = true;
		}
	}
	CHECK(foundEnemy);
}

TEST_CASE("find_overriders: function_name filter requires parent_class") {
	// Unscoped function_name scans were timing out on real projects
	// (every BP needs a full ReadBlueprint). Tool now rejects the
	// unscoped path — pair with parent_class to narrow.
	Fixture f;
	auto out = f.Call("find_overriders", json{
		{"path", "/Game"},
		{"parent_class", "PlayerController"},  // narrows the candidate set
		{"function_name", "AddScore"}});
	REQUIRE(out.is_array());
	// BP_PlayerController has AddScore in fixtures.
	bool found = false;
	for (auto& el : out) {
		if (el["asset_path"] == "/Game/Player/BP_PlayerController") {
			found = true;
			REQUIRE(el["matched"].is_array());
			bool sawFn = false;
			for (auto& m : el["matched"])
			{
				if (m == "function_name") sawFn = true;
			}
			CHECK(sawFn);
		}
	}
	CHECK(found);
}

TEST_CASE("find_overriders: function_name without parent_class is rejected") {
	Fixture f;
	CHECK_THROWS_AS(f.Call("find_overriders", json{
		{"path", "/Game"},
		{"function_name", "AddScore"}}),
		std::invalid_argument);
}

// ===== Idempotency =========================================================

TEST_CASE("add_variable on existing name returns already_existed:true (mock)") {
	// The MOCK backend throws on writes — but the idempotency probe runs
	// BEFORE the write, so calling add_variable for an existing variable
	// name short-circuits cleanly without ever hitting the throw.
	Fixture f;
	auto out = f.Call("add_variable", json{
		{"asset_path","/Game/AI/BP_Enemy"},
		{"name", "Health"},          // exists in BP_Enemy fixture
		{"type", "float"}});
	CHECK(out["ok"] == true);
	CHECK(out["already_existed"] == true);
}

TEST_CASE("add_function on existing name returns already_existed:true (mock)") {
	Fixture f;
	auto out = f.Call("add_function", json{
		{"asset_path","/Game/AI/BP_Enemy"},
		{"name", "TakeDamage"}});  // exists in BP_Enemy fixture
	CHECK(out["ok"] == true);
	CHECK(out["already_existed"] == true);
}

// ===== Type shorthand on tool surface ======================================

TEST_CASE("add_variable accepts type shorthand string (still throws on mock write path)") {
	Fixture f;
	// For a *new* variable, the mock backend's write throws — but the
	// shorthand path should be exercised before that. Use a try/catch
	// pattern: we just want to confirm "float" parses and reaches the
	// backend (which then throws because mock is read-only).
	CHECK_THROWS_AS(f.Call("add_variable", json{
		{"asset_path","/Game/AI/BP_Enemy"},
		{"name", "BrandNew"},
		{"type", "float"}}),
		bpr::backends::BlueprintReaderError);

	// And bad shorthand throws std::invalid_argument before the backend
	// is even consulted.
	CHECK_THROWS_AS(f.Call("add_variable", json{
		{"asset_path","/Game/AI/BP_Enemy"},
		{"name", "BadVar"},
		{"type", "garbage_type"}}),
		std::invalid_argument);
}

// ===== auto_layout_graph ===================================================

TEST_CASE("shutdown_daemon on mock backend returns ok+was_running:false (no daemon to kill)") {
	Fixture f;
	auto out = f.Call("shutdown_daemon", json::object());
	CHECK(out["ok"] == true);
	CHECK(out["was_running"] == false);
}

TEST_CASE("auto_layout_graph: throws on read-only mock (records intent)") {
	// Tests that the dispatcher + topology pass at least exercises the
	// SetNodePosition call. Mock throws on write, but the fact that we
	// reached SetNodePosition means the graph was traversed correctly.
	Fixture f;
	auto out = f.Call("auto_layout_graph", json{
		{"asset_path","/Game/AI/BP_Enemy"},
		{"graph_name","EventGraph"}});
	// Mock's SetNodePosition throws, which the layout tool catches
	// per-node. So we should see placed=0 and ok=true.
	CHECK(out["ok"] == true);
	CHECK(out["placed"] == 0);
	CHECK(out["strategy"] == "grid");
}

// ===== compile_function dispatch ===========================================

TEST_CASE("compile_function: dry_run returns the planned ops") {
	Fixture f;
	auto out = f.Call("compile_function", json{
		{"asset_path","/Game/AI/BP_Enemy"},
		{"function_name","NewFn"},
		{"inputs",  json::array({json{{"name","Amount"},{"type","float"}}})},
		{"body",    json::array({
			json{{"set","Health"}, {"to", json{{"var","Health"}}}},
		})},
		{"dry_run", true}});
	CHECK(out["dry_run"] == true);
	REQUIRE(out["ops"].is_array());
	REQUIRE(out["ops"].size() >= 4);  // add_function + add_input + varget + varset (+ wires)
	CHECK(out["ops"][0]["op"] == "add_function");
}

TEST_CASE("compile_function: rejects unknown statement form") {
	Fixture f;
	CHECK_THROWS_AS(f.Call("compile_function", json{
		{"asset_path","/Game/AI/BP_Enemy"},
		{"function_name","NewFn"},
		{"body", json::array({json{{"unknown_form", "nope"}}})},
		{"dry_run", true}}),
		std::invalid_argument);
}

// ===== B1: literals + math aliases + exec-tail merge =======================

TEST_CASE("compile_function v2: lit expression emits set_pin_default") {
	Fixture f;
	auto out = f.Call("compile_function", json{
		{"asset_path","/Game/AI/BP_Enemy"},
		{"function_name","NewFn"},
		{"body", json::array({
			json{{"set","Health"}, {"to", json{{"lit", 100}}}},
		})},
		{"dry_run", true}});
	REQUIRE(out["ops"].is_array());
	bool sawSetPinDefault = false;
	bool sawWireFromLit = false;
	for (auto& op : out["ops"]) {
		if (op.value("op", "") == "set_pin_default")
		{
			sawSetPinDefault = true;
		}
		// The literal must NOT have produced a wire_pins from a __lit slot —
		// the slot ref is consumed by set_pin_default's pin_name path.
		if (op.value("op", "") == "wire_pins" &&
			op.value("from_node", "").find("__lit") != std::string::npos) {
			sawWireFromLit = true;
		}
	}
	CHECK(sawSetPinDefault);
	CHECK_FALSE(sawWireFromLit);
}

TEST_CASE("compile_function v2: math alias '+' resolves to Add_IntInt") {
	Fixture f;
	auto out = f.Call("compile_function", json{
		{"asset_path","/Game/AI/BP_Enemy"},
		{"function_name","NewFn"},
		{"body", json::array({
			json{{"set","Health"},
				 {"to", json{{"call","+"}, {"args", json{
					 {"A", json{{"var","Health"}}},
					 {"B", json{{"lit", 1}}}
				 }}}}},
		})},
		{"dry_run", true}});
	REQUIRE(out["ops"].is_array());
	bool sawAddIntInt = false;
	for (auto& op : out["ops"]) {
		if (op.value("op", "") == "add_node" &&
			op.value("kind", "") == "CallFunction" &&
			op.value("function", "") == "Add_IntInt" &&
			op.value("function_owner", "") == "KismetMathLibrary") {
			sawAddIntInt = true;
		}
	}
	CHECK(sawAddIntInt);
}

TEST_CASE("compile_function v2: comparison '==' alias works") {
	Fixture f;
	auto out = f.Call("compile_function", json{
		{"asset_path","/Game/AI/BP_Enemy"},
		{"function_name","NewFn"},
		{"body", json::array({
			json{{"if", json{{"call","=="}, {"args", json{
					 {"A", json{{"var","Health"}}},
					 {"B", json{{"lit", 0}}}
				 }}}}, {"then", json::array()}, {"else", json::array()}},
		})},
		{"dry_run", true}});
	bool sawEqualEqual = false;
	for (auto& op : out["ops"]) {
		if (op.value("op", "") == "add_node" &&
			op.value("function", "") == "EqualEqual_IntInt") {
			sawEqualEqual = true;
		}
	}
	CHECK(sawEqualEqual);
}

TEST_CASE("compile_function v2: auto-wires FunctionEntry exec into first statement") {
	Fixture f;
	auto out = f.Call("compile_function", json{
		{"asset_path","/Game/AI/BP_Enemy"},
		{"function_name","NewFn"},
		{"body", json::array({
			json{{"call","Foo"}},
		})},
		{"dry_run", true}});
	REQUIRE(out["ops"].is_array());
	// The add_function op should carry an `id: "__entry"` slot tag —
	// OpAddFunction binds the FunctionEntry node's GUID to it, and the
	// first statement's exec wire references "$__entry".
	bool sawEntrySlot = false;
	bool sawEntryWire = false;
	for (auto& op : out["ops"]) {
		if (op.value("op", "") == "add_function" &&
			op.value("id", "") == "__entry") {
			sawEntrySlot = true;
		}
		if (op.value("op", "") == "wire_pins" &&
			op.value("from_node", "") == "$__entry" &&
			op.value("from_pin", "")  == "then" &&
			op.value("to_pin", "")    == "execute") {
			sawEntryWire = true;
		}
	}
	CHECK(sawEntrySlot);
	CHECK(sawEntryWire);
}

TEST_CASE("compile_function v2: if/else with following stmt fans both tails into next exec") {
	Fixture f;
	auto out = f.Call("compile_function", json{
		{"asset_path","/Game/AI/BP_Enemy"},
		{"function_name","NewFn"},
		{"body", json::array({
			json{{"if",   json{{"var","bAlive"}}},
				 {"then", json::array({
					 json{{"call","Foo"}}
				 })},
				 {"else", json::array({
					 json{{"call","Bar"}}
				 })}},
			// The merge: this Baz call's exec input gets wired from BOTH
			// the `then` chain's tail and the `else` chain's tail.
			json{{"call","Baz"}},
		})},
		{"dry_run", true}});
	REQUIRE(out["ops"].is_array());
	// Find the Baz node's slot id.
	std::string bazSlot;
	for (auto& op : out["ops"]) {
		if (op.value("op", "") == "add_node" &&
			op.value("function", "") == "Baz") {
			bazSlot = op.value("id", "");
		}
	}
	REQUIRE_FALSE(bazSlot.empty());
	// Count exec-wires whose to_node is "$bazSlot" and to_pin is "execute".
	int wiresToBaz = 0;
	std::string toRef = std::string("$") + bazSlot;
	for (auto& op : out["ops"]) {
		if (op.value("op", "") == "wire_pins" &&
			op.value("to_node", "") == toRef &&
			op.value("to_pin", "")  == "execute") {
			++wiresToBaz;
		}
	}
	CHECK(wiresToBaz == 2);
}
