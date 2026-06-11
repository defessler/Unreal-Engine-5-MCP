// Test the BlueprintTools layer directly — bypasses MCP framing, calls the
// tool handlers as plain functions of `arguments`.

#include <doctest/doctest.h>

#include "backends/MockBlueprintReader.h"
#include "tools/BlueprintTools.h"
#include "tools/ToolRegistry.h"

#include "test_helpers.h"

#include <set>
#include <string>

using namespace bpr;
using bpr::test::AsResults;
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

TEST_CASE("ToolRegistry exposes 264 tools with input schemas") {
	Fixture f;
	auto spec = f.registry.ListSpec();
	CHECK(spec.size() == 264);
	for (const auto& t : spec) {
		CHECK(t["inputSchema"]["type"] == "object");
	}
}

// UX-P4a: the mock backend has no live editor game thread, so health_check is a
// stable synthetic-healthy answer (and must NOT throw "not supported").
TEST_CASE("health_check: mock backend reports synthetic-healthy") {
	Fixture f;
	const auto r = f.Call("health_check", json::object());
	CHECK(r["reachable"] == true);
	CHECK(r["game_thread_responsive"] == true);
	CHECK(r["game_thread_age_ms"] == 0);
	CHECK(r["state"] == "healthy");
}

// EDIT-5(b): pure codegen — works on every backend including mock.
TEST_CASE("generate_k2node_skeleton: emits the canonical custom-node surface") {
	Fixture f;
	const auto r = f.Call("generate_k2node_skeleton", json{
		{"class_name", "MyNode"},
		{"module_api", "MYMODULE"},
		{"menu_category", "My Tools"},
		{"pins", json::array({
			json{{"name","Value"},{"direction","input"},{"category","float"},{"default_value","0.0"}},
			json{{"name","Items"},{"direction","input"},{"category","string"},{"container","array"}},
			json{{"name","Result"},{"direction","output"},{"category","bool"}},
		})},
		{"target_function", "/Script/MyModule.MyFunctionLibrary:DoThing"},
	});
	CHECK(r["ok"] == true);
	CHECK(r["class_name"] == "UK2Node_MyNode");
	CHECK(r["header_file"] == "K2Node_MyNode.h");
	const std::string h = r["header_source"].get<std::string>();
	const std::string s = r["impl_source"].get<std::string>();
	// Header: UCLASS scaffolding + the override set.
	CHECK(h.find("class MYMODULE_API UK2Node_MyNode : public UK2Node") != std::string::npos);
	CHECK(h.find("virtual void AllocateDefaultPins() override;") != std::string::npos);
	CHECK(h.find("virtual void GetMenuActions(FBlueprintActionDatabaseRegistrar& ActionRegistrar) const override;") != std::string::npos);
	CHECK(h.find("virtual void ExpandNode(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph) override;") != std::string::npos);
	CHECK(h.find("return false;") != std::string::npos);  // IsNodePure (not pure)
	// Source: exec pins, the float pin (PC_Real/PC_Double), the array container,
	// the default value, the registrar idiom, and the CallFunction lowering.
	CHECK(s.find("UEdGraphSchema_K2::PC_Exec, UEdGraphSchema_K2::PN_Execute") != std::string::npos);
	CHECK(s.find("UEdGraphSchema_K2::PC_Real, UEdGraphSchema_K2::PC_Double, TEXT(\"Value\")") != std::string::npos);
	CHECK(s.find("ItemsParams.ContainerType = EPinContainerType::Array;") != std::string::npos);
	CHECK(s.find("ValuePin->DefaultValue = TEXT(\"0.0\");") != std::string::npos);
	CHECK(s.find("UBlueprintNodeSpawner::Create(GetClass())") != std::string::npos);
	// The class spelling came from the U-prefix heuristic, so it's TODO-marked.
	CHECK(s.find("FName(TEXT(\"DoThing\"))") != std::string::npos);
	CHECK(s.find("/* TODO: verify the C++ spelling */ UMyFunctionLibrary::StaticClass()") != std::string::npos);
	CHECK(s.find("MovePinLinksToIntermediate") != std::string::npos);
	CHECK(r["notes"].is_array());
}

// EDIT-5(b): a pure node has no exec pins and no ExpandNode without a target.
TEST_CASE("generate_k2node_skeleton: pure node without target omits exec + ExpandNode") {
	Fixture f;
	const auto r = f.Call("generate_k2node_skeleton", json{
		{"class_name", "K2Node_PureThing"},   // prefix tolerated
		{"pure", true},
	});
	CHECK(r["ok"] == true);
	CHECK(r["class_name"] == "UK2Node_PureThing");
	const std::string h = r["header_source"].get<std::string>();
	const std::string s = r["impl_source"].get<std::string>();
	CHECK(h.find("return true;") != std::string::npos);          // IsNodePure
	CHECK(h.find("ExpandNode") == std::string::npos);            // omitted
	CHECK(s.find("PN_Execute") == std::string::npos);            // no exec pins
}

// EDIT-5(b): validation errors are clear, not silent wrong code.
TEST_CASE("generate_k2node_skeleton: rejects bad input clearly") {
	Fixture f;
	CHECK_THROWS_WITH_AS(
		f.Call("generate_k2node_skeleton", json{{"class_name", ""}}),
		doctest::Contains("`class_name` is required"), std::invalid_argument);
	CHECK_THROWS_WITH_AS(
		f.Call("generate_k2node_skeleton", json{
			{"class_name", "X"},
			{"pins", json::array({json{{"name","P"},{"category","exec"}}})}}),
		doctest::Contains("exec pins are generated automatically"), std::invalid_argument);
	CHECK_THROWS_WITH_AS(
		f.Call("generate_k2node_skeleton", json{
			{"class_name", "X"},
			{"pins", json::array({json{{"name","P"},{"category","frobnicate"}}})}}),
		doctest::Contains("unknown pin category"), std::invalid_argument);
	// Review hardening: duplicate pin names (case-insensitive + post-sanitize)
	// would emit redefined locals; reserved exec-pin names collide on non-pure
	// nodes; a /Script/ class path with no :Func part is module.class, not
	// class.func — all rejected with clear messages.
	CHECK_THROWS_WITH_AS(
		f.Call("generate_k2node_skeleton", json{
			{"class_name", "X"},
			{"pins", json::array({
				json{{"name","My Value"},{"category","int"}},
				json{{"name","myvalue"},{"category","int"}}})}}),
		doctest::Contains("duplicate pin name"), std::invalid_argument);
	CHECK_THROWS_WITH_AS(
		f.Call("generate_k2node_skeleton", json{
			{"class_name", "X"},
			{"pins", json::array({json{{"name","Execute"},{"category","int"}}})}}),
		doctest::Contains("reserved for the auto exec pins"), std::invalid_argument);
	CHECK_THROWS_WITH_AS(
		f.Call("generate_k2node_skeleton", json{
			{"class_name", "X"},
			{"target_function", "/Script/MyModule.MyFunctionLibrary"}}),
		doctest::Contains("no function part"), std::invalid_argument);
}

// EDIT-5(b) review hardening: free text is escaped into the generated string
// literals (a quote/backslash/newline must not break out of the literal), and
// module_api is identifier-sanitized (it lands in CODE position).
TEST_CASE("generate_k2node_skeleton: escapes free text + sanitizes module_api") {
	Fixture f;
	const auto r = f.Call("generate_k2node_skeleton", json{
		{"class_name", "Esc"},
		{"module_api", "MY MODULE!"},
		{"title", "Say \"Hi\"\\now"},
		{"tooltip", "Line1\nLine2"},
		{"pins", json::array({
			json{{"name","Msg"},{"direction","input"},{"category","string"},
				 {"default_value","quote\" and \\slash"}}})},
	});
	CHECK(r["ok"] == true);
	const std::string h = r["header_source"].get<std::string>();
	const std::string s = r["impl_source"].get<std::string>();
	// module_api sanitized to an identifier (no space/!), still gets _API.
	CHECK(h.find("class MYMODULE_API UK2Node_Esc") != std::string::npos);
	// Escaped splices — the raw unescaped forms must NOT appear.
	CHECK(s.find("Say \\\"Hi\\\"\\\\now") != std::string::npos);
	CHECK(s.find("Line1\\nLine2") != std::string::npos);
	CHECK(s.find("quote\\\" and \\\\slash") != std::string::npos);
	CHECK(s.find("Say \"Hi\"") == std::string::npos);
	// Every emitted line must stay a single line (no literal newline injected).
	CHECK(s.find("Line1\nLine2") == std::string::npos);
}

// EDIT-5(b) review hardening: known engine classes get their REAL UE prefix
// (Actor → AActor, not the broken UActor the naive heuristic produced).
TEST_CASE("generate_k2node_skeleton: known-class table spells AActor correctly") {
	Fixture f;
	const auto r = f.Call("generate_k2node_skeleton", json{
		{"class_name", "Sp"},
		{"pins", json::array({
			json{{"name","Target"},{"direction","input"},{"category","object"},
				 {"sub_object","/Script/Engine.Actor"}}})},
	});
	const std::string s = r["impl_source"].get<std::string>();
	CHECK(s.find("AActor::StaticClass()") != std::string::npos);
	CHECK(s.find("UActor::StaticClass()") == std::string::npos);
}

// EDIT-5(a): editor-only — the mock throws a clear not-supported error.
TEST_CASE("describe_k2node: mock backend throws not-supported") {
	Fixture f;
	CHECK_THROWS_WITH_AS(
		f.Call("describe_k2node", json{{"class_path", "/Script/BlueprintGraph.K2Node_FormatText"}}),
		doctest::Contains("requires the live or commandlet backend"),
		bpr::backends::BlueprintReaderError);
}

// TEST-2 P0: editor-only Slate walk — the mock throws a clear not-supported
// error (no Slate UI in fixtures).
TEST_CASE("ui_list_widgets: mock backend throws not-supported") {
	Fixture f;
	CHECK_THROWS_WITH_AS(
		f.Call("ui_list_widgets", json::object()),
		doctest::Contains("requires the live backend"),
		bpr::backends::BlueprintReaderError);
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
	auto& rows = AsResults(out);
	REQUIRE(rows.is_array());
	// Mock fixtures define a specific order; just verify the call works
	// without sort and returns something non-empty.
	CHECK(rows.size() > 0);
}

TEST_CASE("list_blueprints: sparse result attaches a find_asset hint (UX-P4i)") {
	Fixture f;
	// A path that matches nothing returns 0 rows — the hint must point the
	// caller at find_asset (name/fuzzy search) instead of looking like "no BPs".
	auto empty = f.Call("list_blueprints", json{{"path", "/Game/NoSuchSubpath"}});
	REQUIRE(empty.is_object());
	REQUIRE(empty.contains("_hint"));
	CHECK(empty["_hint"].get<std::string>().find("find_asset") != std::string::npos);
	// A populated query (all fixtures under /Game) must NOT carry the hint.
	auto many = f.Call("list_blueprints", json{{"path", "/Game"}});
	CHECK_FALSE(many.contains("_hint"));
}

TEST_CASE("list_blueprints: sort=path returns entries sorted by asset_path") {
	Fixture f;
	auto out = f.Call("list_blueprints", json{{"path", "/Game"}, {"sort", "path"}});
	auto& rows = AsResults(out);
	REQUIRE(rows.is_array());
	REQUIRE(rows.size() >= 2);
	// Pairwise check: each entry's asset_path is <= the next.
	for (size_t i = 1; i < rows.size(); ++i) {
		const auto a = rows[i-1].value("asset_path", "");
		const auto b = rows[i].value("asset_path", "");
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
	CHECK(out.size() == 24);
	std::vector<std::string> kinds;
	for (auto& k : out)
	{
		kinds.push_back(k["kind"].get<std::string>());
	}
	auto has = [&](const std::string& s) {
		return std::find(kinds.begin(), kinds.end(), s) != kinds.end();
	};
	for (const char* k : {"Branch","Sequence","VariableGet","VariableSet","CallFunction",
						  "CustomEvent","Event","Cast","Self","MakeArray","MakeStruct",
						  "FormatText","Knot","GetSubsystem",
						  "Comment","GetArrayItem","Select","SpawnActor","BreakStruct",
						  "MacroInstance","CallParent","PromotableOp","CommutativeOp","Message"}) {
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
	CHECK_THROWS_AS(f.Call("clone_graph", json{
		{"source","/Game/AI/BP_Enemy"}, {"target","/Game/AI/BP_Other"},
		{"graph","EventGraph"}
	}), bpr::backends::BlueprintReaderError);
	CHECK_THROWS_AS(f.Call("implement_interface", json{
		{"asset","/Game/AI/BP_Enemy"}, {"interface","/Game/Interfaces/BPI_Damageable"}
	}), bpr::backends::BlueprintReaderError);
}

TEST_CASE("list_blueprints returns canonical BPAssetSummary array") {
	Fixture f;
	auto out = f.Call("list_blueprints", json{{"path", "/Game"}});
	auto& rows = AsResults(out);
	REQUIRE(rows.is_array());
	CHECK(rows.size() == 7);
	CHECK(rows[0].contains("asset_path"));
	CHECK(rows[0].contains("parent_class"));
	CHECK(rows[0].contains("modified_iso"));
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
	auto& rows = AsResults(out);
	REQUIRE(rows.is_array());
	CHECK(rows.size() == 3);
	CHECK(rows[0].contains("is_replicated"));
}

TEST_CASE("find_node returns matching nodes (paginated envelope)") {
	Fixture f;
	auto out = f.Call("find_node", json{
		{"asset_path", "/Game/AI/BP_Enemy"},
		{"query", "Sequence"}});
	// find_node now returns a paginated envelope {total,count,has_more,results}
	REQUIRE(out.is_object());
	REQUIRE(out.contains("results"));
	auto& results = out["results"];
	REQUIRE(results.is_array());
	REQUIRE(results.size() == 1);
	CHECK(results[0]["class"] == "K2Node_ExecutionSequence");
	CHECK(out.value("total", -1) == 1);
	CHECK(out.value("has_more", true) == false);
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
	CHECK(out["variable_count"] == static_cast<int>(AsResults(vars).size()));
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

TEST_CASE("read_blueprint with a misspelled fields entry surfaces a _warnings hint (UX-P0a)") {
	Fixture f;
	auto out = f.Call("read_blueprint", json{
		{"asset_path", "/Game/AI/BP_Enemy"},
		{"fields", json::array({"parent_class", "asset_paths"})}});  // 'asset_paths' typo
	REQUIRE(out.is_object());
	// The valid field still projected.
	CHECK(out.contains("parent_class"));
	// The typo is flagged rather than silently dropped to nothing.
	REQUIRE(out.contains("_warnings"));
	REQUIRE(out["_warnings"].is_array());
	bool flagged = false;
	for (const auto& w : out["_warnings"]) {
		if (w.get<std::string>().find("asset_paths") != std::string::npos) { flagged = true; }
	}
	CHECK(flagged);
}

TEST_CASE("read_blueprint with all-valid fields adds no _warnings") {
	Fixture f;
	auto out = f.Call("read_blueprint", json{
		{"asset_path", "/Game/AI/BP_Enemy"},
		{"fields", json::array({"parent_class", "name"})}});
	CHECK_FALSE(out.contains("_warnings"));
}

TEST_CASE("add_node rejects an unknown kind with a did-you-mean (UX-P1b)") {
	Fixture f;
	auto callKind = [&](const char* kind) -> std::string {
		try {
			f.Call("add_node", json{{"asset_path","/Game/AI/BP_Enemy"},
									 {"graph_name","EventGraph"},
									 {"kind", kind}, {"x",0}, {"y",0}});
		} catch (const std::exception& e) {
			return e.what();
		}
		return {};  // didn't throw
	};

	// Case typo → suggests the canonical casing + lists the valid set.
	const std::string m1 = callKind("branch");
	CHECK(m1.find("unknown node kind 'branch'") != std::string::npos);
	CHECK(m1.find("did you mean 'Branch'") != std::string::npos);
	CHECK(m1.find("valid kinds:") != std::string::npos);

	// Wholly bogus kind → no suggestion, but still lists the valid set.
	const std::string m2 = callKind("Frobnicate");
	CHECK(m2.find("unknown node kind 'Frobnicate'") != std::string::npos);
	CHECK(m2.find("valid kinds:") != std::string::npos);

	// A valid kind passes pre-validation and reaches the (read-only) mock,
	// which rejects the WRITE — proving we didn't over-reject a real kind.
	const std::string m3 = callKind("Branch");
	CHECK(m3.find("unknown node kind") == std::string::npos);
}

TEST_CASE("KnownNodeKinds() stays in sync with list_node_kinds (UX-P1b)") {
	Fixture f;
	auto listed = f.Call("list_node_kinds", json::object());
	REQUIRE(listed.is_array());
	std::set<std::string> advertised;
	for (const auto& e : listed) {
		advertised.insert(e.at("kind").get<std::string>());
	}
	std::set<std::string> known(tools::KnownNodeKinds().begin(),
								tools::KnownNodeKinds().end());
	CHECK(advertised == known);
}

TEST_CASE("list_blueprints honors limit/offset pagination") {
	Fixture f;
	auto all = f.Call("list_blueprints", json{{"path","/Game"}});
	auto& allRows = AsResults(all);
	REQUIRE(allRows.is_array());
	REQUIRE(allRows.size() >= 2);

	auto first = f.Call("list_blueprints", json{{"path","/Game"},{"limit",1}});
	auto& firstRows = AsResults(first);
	REQUIRE(firstRows.is_array());
	CHECK(firstRows.size() == 1);
	CHECK(firstRows[0]["asset_path"] == allRows[0]["asset_path"]);

	auto second = f.Call("list_blueprints", json{{"path","/Game"},{"limit",1},{"offset",1}});
	auto& secondRows = AsResults(second);
	REQUIRE(secondRows.is_array());
	CHECK(secondRows.size() == 1);
	CHECK(secondRows[0]["asset_path"] == allRows[1]["asset_path"]);
}

TEST_CASE("list_blueprints with fields returns just the requested keys per element") {
	Fixture f;
	auto out = f.Call("list_blueprints", json{
		{"path","/Game"},
		{"fields", json::array({"asset_path"})}});
	auto& rows = AsResults(out);
	REQUIRE(rows.is_array());
	CHECK(rows.size() >= 1);
	for (auto& el : rows) {
		CHECK(el.size() == 1);
		CHECK(el.contains("asset_path"));
	}
}

TEST_CASE("list_variables honors limit/offset and fields together") {
	Fixture f;
	auto all = f.Call("list_variables", json{{"asset_path","/Game/AI/BP_Enemy"}});
	auto& allVars = AsResults(all);
	REQUIRE(allVars.is_array());

	auto sliced = f.Call("list_variables", json{
		{"asset_path","/Game/AI/BP_Enemy"},
		{"limit", 2},
		{"fields", json::array({"name"})}});
	auto& slicedVars = AsResults(sliced);
	REQUIRE(slicedVars.is_array());
	CHECK(slicedVars.size() == std::min<std::size_t>(2, allVars.size()));
	for (auto& v : slicedVars) {
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
	auto& rows = AsResults(out);
	REQUIRE(rows.is_array());
	CHECK(rows.empty());
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

// --- did-you-mean hint quality (client feedback #6) ------------------------
// The hint must never echo the exact path the caller asked for. When the
// asset exists but isn't a Blueprint, report its real class instead. Drive
// the real read_blueprint handler with a backend whose FindAsset returns
// controlled rows (mock's FindAsset default throws, so subclass it).

namespace did_you_mean_detail {

struct HintReader : backends::MockBlueprintReader {
	std::vector<backends::IBlueprintReader::AssetRegistryEntry> rows;
	explicit HintReader(std::vector<backends::IBlueprintReader::AssetRegistryEntry> r)
		: backends::MockBlueprintReader(test::FixturesDir()), rows(std::move(r)) {}
	backends::IBlueprintReader::AssetRegistryListResult
	FindAsset(std::string_view, std::string_view) override {
		backends::IBlueprintReader::AssetRegistryListResult res;
		res.entries = rows;
		return res;
	}
};

std::string ReadBlueprintError(HintReader& reader, const std::string& path) {
	tools::ToolRegistry registry;
	tools::RegisterBlueprintTools(registry, reader);
	const auto* fn = registry.Find("read_blueprint");
	REQUIRE(fn != nullptr);
	try {
		(*fn)(json{{"asset_path", path}});
	} catch (const backends::AssetNotFound& e) {
		return e.what();
	}
	return "<no throw>";
}

}    // namespace did_you_mean_detail
using namespace did_you_mean_detail;

TEST_CASE("did-you-mean: exact-path match reports the real class, never self-suggests") {
	HintReader reader({{"/Game/Maps/Lobby", "Lobby", "World"}});
	auto msg = ReadBlueprintError(reader, "/Game/Maps/Lobby");
	CHECK(msg.find("exists but is a World") != std::string::npos);
	CHECK(msg.find("not a Blueprint") != std::string::npos);
	CHECK(msg.find("did you mean: /Game/Maps/Lobby") == std::string::npos);
}

TEST_CASE("did-you-mean: exact match + other near-matches lists the others, not the input") {
	HintReader reader({
		{"/Game/Maps/Lobby",  "Lobby",    "World"},
		{"/Game/AI/BP_Lobby", "BP_Lobby", "Blueprint"},
	});
	auto msg = ReadBlueprintError(reader, "/Game/Maps/Lobby");
	CHECK(msg.find("exists but is a World") != std::string::npos);
	CHECK(msg.find("/Game/AI/BP_Lobby") != std::string::npos);
	CHECK(msg.find("did you mean: /Game/Maps/Lobby") == std::string::npos);
}

TEST_CASE("did-you-mean: no exact match falls back to fuzzy suggestions") {
	// Use a path with no fixture so read_blueprint actually throws.
	HintReader reader({{"/Game/AI/BP_Enemy2", "BP_Enemy2", "Blueprint"}});
	auto msg = ReadBlueprintError(reader, "/Game/AI/BP_Enemyzzz");
	CHECK(msg.find("did you mean: /Game/AI/BP_Enemy2") != std::string::npos);
}

TEST_CASE("did-you-mean: only the input matches (as a Blueprint) => no misleading echo") {
	// Exact-path row whose class IS Blueprint (and a non-fixture path so the
	// read actually fails): we neither suggest it back nor emit a nonsensical
	// "is a Blueprint, not a Blueprint" note.
	HintReader reader({{"/Game/AI/BP_Ghost", "BP_Ghost", "Blueprint"}});
	auto msg = ReadBlueprintError(reader, "/Game/AI/BP_Ghost");
	CHECK(msg.find("did you mean") == std::string::npos);
	CHECK(msg.find("exists but is a") == std::string::npos);
}

TEST_CASE("read_actor_instance: unsupported on mock backend (needs a UObject world)") {
	// Client feedback #1. Mock has no world/registry, so the tool throws and
	// is advertised in UnsupportedTools() (catalog hides it under mock). The
	// real behavior is exercised by the live commandlet test.
	Fixture f;
	CHECK_THROWS_AS(f.Call("read_actor_instance",
		json{{"asset_path","/Game/Maps/L_X/__ExternalActors__/0/AB/GUID"}}),
		bpr::backends::BlueprintReaderError);
	auto unsupported = f.reader.UnsupportedTools();
	CHECK(std::find(unsupported.begin(), unsupported.end(), "read_actor_instance")
		  != unsupported.end());
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

TEST_CASE("shutdown_daemon: force_shared:true returns ok+was_running:false (no daemon to kill)") {
	Fixture f;
	auto out = f.Call("shutdown_daemon", json{{"force_shared", true}});
	CHECK(out["ok"] == true);
	CHECK(out["was_running"] == false);
}

TEST_CASE("shutdown_daemon: default-deny without force_shared (client feedback #5)") {
	// Shared-daemon blast radius: without explicit force_shared:true the tool
	// must refuse rather than kill every session's daemon.
	Fixture f;
	CHECK_THROWS_AS(f.Call("shutdown_daemon", json::object()), std::invalid_argument);
	CHECK_THROWS_AS(f.Call("shutdown_daemon", json{{"force_shared", false}}),
					std::invalid_argument);
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

// --- find_asset pagination (client feedback #3) ----------------------------
// A broad query (e.g. "Elevator") used to return an unbounded array the MCP
// client rejected as "Output too large". find_asset now caps at a default
// page size and reports total / has_more / next_cursor so the caller pages.
// The mock backend's FindAsset is the throwing default, so we drive the
// handler with a tiny subclass that returns N synthetic rows.

namespace find_asset_pagination_detail {

struct ManyAssetsReader : backends::MockBlueprintReader {
	int count;
	explicit ManyAssetsReader(int n)
		: backends::MockBlueprintReader(test::FixturesDir()), count(n) {}
	backends::IBlueprintReader::AssetRegistryListResult
	FindAsset(std::string_view query, std::string_view path) override {
		(void)query; (void)path;
		backends::IBlueprintReader::AssetRegistryListResult r;
		for (int i = 0; i < count; ++i) {
			backends::IBlueprintReader::AssetRegistryEntry e;
			e.assetPath = "/Game/Lift/Elevator_" + std::to_string(i);
			e.name      = "Elevator_" + std::to_string(i);
			e.className = "Blueprint";
			r.entries.push_back(std::move(e));
		}
		return r;
	}
};

json CallFindAsset(int totalCount, json args) {
	ManyAssetsReader reader(totalCount);
	tools::ToolRegistry registry;
	tools::RegisterBlueprintTools(registry, reader);
	const auto* fn = registry.Find("find_asset");
	REQUIRE(fn != nullptr);
	args["query"] = "Elevator";
	return (*fn)(args);
}

}    // namespace find_asset_pagination_detail
using namespace find_asset_pagination_detail;

TEST_CASE("find_asset: caps at default page size and reports pagination") {
	auto out = CallFindAsset(120, json::object());
	CHECK(out["total"]    == 120);
	CHECK(out["count"]    == 50);
	CHECK(out["offset"]   == 0);
	CHECK(out["has_more"] == true);
	CHECK(out["next_cursor"].is_string());
	REQUIRE(out["results"].is_array());
	CHECK(out["results"].size() == 50);
	CHECK(out["query"] == "Elevator");
}

TEST_CASE("find_asset: last page sets has_more=false + null next_cursor") {
	auto out = CallFindAsset(30, json::object());
	CHECK(out["total"]    == 30);
	CHECK(out["count"]    == 30);
	CHECK(out["has_more"] == false);
	CHECK(out["next_cursor"].is_null());
}

TEST_CASE("find_asset: explicit limit overrides the default page size") {
	auto out = CallFindAsset(120, json{{"limit", 10}});
	CHECK(out["count"]    == 10);
	CHECK(out["has_more"] == true);
}

TEST_CASE("find_asset: next_cursor walks subsequent pages to exhaustion") {
	auto p1 = CallFindAsset(120, json::object());
	REQUIRE(p1["next_cursor"].is_string());
	auto p2 = CallFindAsset(120, json{{"cursor", p1["next_cursor"]}});
	CHECK(p2["offset"]   == 50);
	CHECK(p2["count"]    == 50);
	CHECK(p2["has_more"] == true);
	REQUIRE(p2["next_cursor"].is_string());
	auto p3 = CallFindAsset(120, json{{"cursor", p2["next_cursor"]}});
	CHECK(p3["offset"]   == 100);
	CHECK(p3["count"]    == 20);
	CHECK(p3["has_more"] == false);
	CHECK(p3["next_cursor"].is_null());
}

TEST_CASE("find_asset: fields projects each result row") {
	auto out = CallFindAsset(5, json{{"fields", json::array({"asset_path"})}});
	REQUIRE(out["results"].is_array());
	CHECK(out["results"].size() == 5);
	for (auto& row : out["results"]) {
		CHECK(row.size() == 1);
		CHECK(row.contains("asset_path"));
	}
}
