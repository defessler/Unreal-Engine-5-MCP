// Tests for MCP 2025-03-26 §tools/annotations support — verifies that
// tools/list emits the right hints so clients (Copilot, Claude Code)
// can filter the tool surface to "read-only only" / "non-destructive
// only" / etc.

#include <doctest/doctest.h>

#include "tools/ToolAnnotations.h"
#include "tools/ToolRegistry.h"

#include <string>

using namespace bpr;
using nlohmann::json;

namespace test_tool_annotations_detail {

// Minimal stub handler — these tests don't dispatch, only inspect
// the registry's emitted spec.
tools::ToolFn StubFn() {
	return [](const json&) { return json::object(); };
}

}    // namespace test_tool_annotations_detail
using namespace test_tool_annotations_detail;

TEST_CASE("AnnotationsFor: read-only tools advertise readOnlyHint=true with consistent defaults") {
	for (const char* name : {
			"list_blueprints", "read_blueprint", "summarize_blueprint",
			"get_graph", "get_function", "get_node", "get_components",
			"find_node", "find_overriders",
			"list_variables", "list_functions",
			"get_project_metadata",
			"bp_structural_diff",
			"decompile_function", "decompile_blueprint",
			"transpile_function", "transpile_blueprint",
			"parse_cpp_function",
			"preview_ops",
			"read_data_table", "read_material",
			"read_widget_blueprint", "read_behavior_tree",
			"read_data_asset", "read_state_tree",
			"read_niagara_system", "read_level_sequence",
			"read_anim_blueprint", "read_ability_set",
			"find_class", "get_class_info",
			"list_node_kinds", "list_pin_categories",
			"read_output_log", "get_cvar",
			"get_selected_actors", "get_editor_state",
			"get_referencers", "get_dependencies",
			"read_config_value",
		}) {
		const auto a = tools::AnnotationsFor(name);
		CAPTURE(name);
		REQUIRE(a.read_only_hint.has_value());
		CHECK(*a.read_only_hint == true);
		REQUIRE(a.destructive_hint.has_value());
		CHECK(*a.destructive_hint == false);
		REQUIRE(a.idempotent_hint.has_value());
		CHECK(*a.idempotent_hint == true);
		REQUIRE(a.open_world_hint.has_value());
		CHECK(*a.open_world_hint == false);
	}
}

TEST_CASE("AnnotationsFor: write tools advertise readOnlyHint=false") {
	for (const char* name : {
			"add_variable", "set_variable_default", "rename_variable",
			"add_node", "wire_pins", "auto_layout_graph",
			"create_blueprint", "save_all",
			"add_component", "set_component_property",
			"compile_function", "apply_ops",
		}) {
		const auto a = tools::AnnotationsFor(name);
		CAPTURE(name);
		REQUIRE(a.read_only_hint.has_value());
		CHECK(*a.read_only_hint == false);
	}
}

TEST_CASE("AnnotationsFor: delete_* / remove_* / shutdown_daemon advertise destructiveHint=true") {
	for (const char* name : {
			"delete_variable", "delete_function", "delete_node",
			"delete_actor", "delete_asset",
			"remove_component",
			"shutdown_daemon",
		}) {
		const auto a = tools::AnnotationsFor(name);
		CAPTURE(name);
		REQUIRE(a.destructive_hint.has_value());
		CHECK(*a.destructive_hint == true);
	}
}

TEST_CASE("AnnotationsFor: non-destructive writes advertise destructiveHint=false") {
	// The spec defaults destructiveHint to true. We explicitly set =false
	// on additive writes so Copilot's "non-destructive only" filter
	// doesn't drop them. Spot-check the obvious candidates.
	for (const char* name : {
			"add_variable", "add_function", "add_node", "add_component",
			"set_variable_default", "set_node_position",
			"wire_pins", "auto_layout_graph",
			"create_blueprint", "create_data_asset", "create_niagara_system",
			"compile_function", "compile_material", "save_all",
		}) {
		const auto a = tools::AnnotationsFor(name);
		CAPTURE(name);
		REQUIRE(a.destructive_hint.has_value());
		CHECK(*a.destructive_hint == false);
	}
}

TEST_CASE("AnnotationsFor: console_command + run_python_script + cook + package are open-world") {
	for (const char* name : {
			"console_command",
			"run_python_script",
			"cook_content",
			"package_project",
		}) {
		const auto a = tools::AnnotationsFor(name);
		CAPTURE(name);
		REQUIRE(a.open_world_hint.has_value());
		CHECK(*a.open_world_hint == true);
	}
}

TEST_CASE("AnnotationsFor: in-editor writes advertise openWorldHint=false") {
	// Spec defaults openWorldHint to true. We override on every write
	// that touches only the editor's own in-memory state so the agent
	// can tell editor mutations apart from genuinely-external effects.
	for (const char* name : {
			"add_node", "set_variable_default",
			"pie_start", "spawn_actor", "set_actor_transform",
			"build_lighting", "live_coding_compile",
			"compile_function", "save_all",
		}) {
		const auto a = tools::AnnotationsFor(name);
		CAPTURE(name);
		REQUIRE(a.open_world_hint.has_value());
		CHECK(*a.open_world_hint == false);
	}
}

TEST_CASE("AnnotationsFor: compile_* and save_all advertise idempotentHint=true") {
	for (const char* name : {
			"save_all",
			"compile_function", "compile_material",
			"compile_widget_blueprint", "compile_behavior_tree",
			"compile_state_tree", "compile_anim_blueprint",
			"build_lighting", "live_coding_compile",
			"write_generated_source",
		}) {
		const auto a = tools::AnnotationsFor(name);
		CAPTURE(name);
		REQUIRE(a.idempotent_hint.has_value());
		CHECK(*a.idempotent_hint == true);
	}
}

TEST_CASE("AnnotationsFor: unknown tool name returns all-nullopt") {
	const auto a = tools::AnnotationsFor("no_such_tool_xyz");
	CHECK_FALSE(a.IsSet());
	CHECK_FALSE(a.read_only_hint.has_value());
	CHECK_FALSE(a.destructive_hint.has_value());
}

TEST_CASE("ToolRegistry::Add auto-applies annotations from AnnotationsFor") {
	// Stub registration with a name our table recognizes — the
	// registry should pick up the annotations automatically so existing
	// registration sites don't have to set them by hand.
	tools::ToolRegistry r;
	r.Add({"read_blueprint", "stub", json::object()}, StubFn());
	auto spec = r.ListSpec();
	REQUIRE(spec.size() == 1);
	REQUIRE(spec[0].contains("annotations"));
	CHECK(spec[0]["annotations"]["readOnlyHint"] == true);
	CHECK(spec[0]["annotations"]["destructiveHint"] == false);
	CHECK(spec[0]["annotations"]["idempotentHint"] == true);
	CHECK(spec[0]["annotations"]["openWorldHint"] == false);
}

TEST_CASE("ToolRegistry::Add: write tools get destructive=false + openWorld=false explicitly") {
	tools::ToolRegistry r;
	r.Add({"add_node", "stub", json::object()}, StubFn());
	auto spec = r.ListSpec();
	REQUIRE(spec.size() == 1);
	REQUIRE(spec[0].contains("annotations"));
	CHECK(spec[0]["annotations"]["readOnlyHint"] == false);
	CHECK(spec[0]["annotations"]["destructiveHint"] == false);
	CHECK(spec[0]["annotations"]["openWorldHint"] == false);
}

TEST_CASE("ToolRegistry::Add: destructive tools advertise destructiveHint=true") {
	tools::ToolRegistry r;
	r.Add({"delete_node", "stub", json::object()}, StubFn());
	auto spec = r.ListSpec();
	REQUIRE(spec.size() == 1);
	CHECK(spec[0]["annotations"]["destructiveHint"] == true);
	CHECK(spec[0]["annotations"]["readOnlyHint"] == false);
}

TEST_CASE("ToolRegistry::Add: open-world tools advertise openWorldHint=true") {
	tools::ToolRegistry r;
	r.Add({"console_command", "stub", json::object()}, StubFn());
	auto spec = r.ListSpec();
	REQUIRE(spec.size() == 1);
	CHECK(spec[0]["annotations"]["openWorldHint"] == true);
}

TEST_CASE("ToolRegistry::Add: explicit annotations override auto-classification") {
	// Caller-set annotations win — we never stamp over them.
	tools::ToolRegistry r;
	tools::ToolDescriptor d;
	d.name = "read_blueprint";
	d.description = "stub";
	d.input_schema = json::object();
	d.annotations.read_only_hint = false;  // wrong on purpose
	d.annotations.destructive_hint = true; // wrong on purpose
	r.Add(std::move(d), StubFn());

	auto spec = r.ListSpec();
	REQUIRE(spec.size() == 1);
	CHECK(spec[0]["annotations"]["readOnlyHint"] == false);
	CHECK(spec[0]["annotations"]["destructiveHint"] == true);
}

TEST_CASE("ToolRegistry::Add: unknown tool name leaves annotations off the spec") {
	tools::ToolRegistry r;
	r.Add({"some_custom_extension_tool", "stub", json::object()}, StubFn());
	auto spec = r.ListSpec();
	REQUIRE(spec.size() == 1);
	// No classification → no annotations object emitted.
	CHECK_FALSE(spec[0].contains("annotations"));
}

TEST_CASE("ToolAnnotations::ToJson omits unset fields") {
	tools::ToolAnnotations a;
	CHECK(a.ToJson() == json::object());
	a.read_only_hint = true;
	auto j = a.ToJson();
	CHECK(j.contains("readOnlyHint"));
	CHECK(j["readOnlyHint"] == true);
	CHECK_FALSE(j.contains("destructiveHint"));
	CHECK_FALSE(j.contains("idempotentHint"));
	CHECK_FALSE(j.contains("openWorldHint"));
}
