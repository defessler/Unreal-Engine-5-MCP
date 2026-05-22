#include "tools/ToolAnnotations.h"

#include <set>
#include <string>

namespace bpr::tools {

namespace tool_annotations_detail {

// Tools that don't mutate any asset, editor state, or server state.
// readOnlyHint=true, plus the canonical "what does that imply"
// values: destructive=false, idempotent=true, openWorld=false.
//
// Includes pure server-side compute (decompile/transpile/parse return
// derived JSON or source text without touching the BP) and meta-tools
// that only introspect.
const std::set<std::string>& ReadOnlySet() {
	static const std::set<std::string> kSet = {
		// --- Blueprint introspection -----------------------------------
		"list_blueprints", "read_blueprint", "summarize_blueprint",
		"get_graph", "peek_graph",
		"get_function", "get_node", "get_components",
		"find_node", "find_overriders",
		"find_dangling_references",
		"list_variables", "list_functions",
		"get_project_metadata",
		"bp_structural_diff",
		// --- Per-asset-type readers ------------------------------------
		"list_data_tables", "read_data_table",
		"list_materials", "read_material",
		"read_widget_blueprint",
		"list_behavior_trees", "read_behavior_tree",
		"list_data_assets", "read_data_asset",
		"list_state_trees", "read_state_tree",
		"list_niagara_systems", "read_niagara_system",
		"list_level_sequences", "read_level_sequence",
		"list_gameplay_tags", "read_ability_set",
		"list_anim_blueprints", "read_anim_blueprint",
		// --- Generic asset registry -------------------------------------
		"list_assets", "find_asset",
		// --- Class info --------------------------------------------------
		"find_class", "get_class_info",
		// --- Discoverability + situational awareness --------------------
		"list_node_kinds", "list_pin_categories",
		"read_output_log", "get_cvar",
		// NOTE: get_stats is NOT here — despite the name, it's a
		// stateful toggle (see its tool description). Moved to WriteSet.
		"get_selected_actors", "get_editor_state",
		"get_referencers", "get_dependencies",
		"read_config_value",
		// Phase 8 EA-pull Wave 1 (partial)
		"list_open_assets", "get_active_asset", "get_compile_status",
		"get_dirty_packages", "get_focused_window",
		"get_pie_state", "get_modal_state", "get_active_editor_mode",
		// --- BP↔C++ pure compute (no write) -----------------------------
		"decompile_function", "decompile_blueprint",
		"transpile_function", "transpile_blueprint",
		"parse_cpp_function",
		// --- apply_ops dry-run ------------------------------------------
		"preview_ops",
		// --- Lazy-discovery meta-tools ----------------------------------
		"list_toolsets", "describe_toolset",
	};
	return kSet;
}

// Tools that mutate BP/asset/editor/server state but are NOT
// classified read-only. Membership in this set is what flips a tool
// from "unknown" to "we explicitly classify it as a write" — that
// drives the destructive=false / openWorld=false overrides so
// non-delete writes don't inherit the spec's permissive defaults.
const std::set<std::string>& WriteSet() {
	static const std::set<std::string> kSet = {
		// Variable CRUD
		"add_variable", "delete_variable", "rename_variable",
		"retype_variable", "set_variable_default", "set_variable_category",
		// Function CRUD
		"add_function", "add_function_input", "add_function_output",
		"delete_function",
		// Graph CRUD
		"add_node", "delete_node", "set_node_position", "wire_pins", "set_pin_default",
		"auto_layout_graph",
		// BP-level
		"create_blueprint", "duplicate_blueprint", "save_all",
		// Components
		"add_component", "attach_component", "remove_component",
		"set_component_property",
		// Assets
		"move_asset", "delete_asset", "create_folder",
		// Data tables
		"add_data_row", "set_data_row_value",
		// Config
		"set_config_value",
		// Materials
		"add_material_expression", "connect_material_expressions",
		"set_material_parameter", "set_material_instance_parameter",
		"compile_material",
		// Widgets
		"add_widget", "set_widget_property", "bind_widget_event",
		"compile_widget_blueprint",
		// Behavior trees
		"add_bt_node", "set_bt_node_property", "compile_behavior_tree",
		// Data assets
		"create_data_asset", "set_data_asset_property",
		// State trees
		"add_state_tree_state", "set_state_tree_transition",
		"compile_state_tree",
		// Niagara
		"create_niagara_system", "set_niagara_parameter",
		// Sequencer
		"add_sequence_track", "set_sequence_playback_range",
		// Gameplay tags
		"add_gameplay_tag",
		// Anim BP
		"add_anim_state", "compile_anim_blueprint",
		// Transpile output
		"write_generated_source",
		// Profiling
		"start_profile", "stop_profile",
		// get_stats: stateful toggle of an editor overlay (see its
		// tool description). Not read-only despite the name.
		"get_stats",
		// Console + cvar
		"console_command", "set_cvar",
		// Editor / PIE / viewport / screenshots
		"pie_start", "pie_stop",
		"set_selection",
		"spawn_actor", "set_actor_transform", "delete_actor",
		"focus_actor", "set_camera_transform",
		"set_show_flag",
		"take_screenshot", "take_viewport_screenshot",
		"take_annotated_screenshot",
		"build_lighting", "live_coding_compile",
		// Scripting
		"run_python_script",
		// Cook + package
		"cook_content", "package_project",
		// Tests
		"run_automation_tests",
		// Daemon + batch
		"shutdown_daemon",
		"apply_ops", "compile_function",
		// Progressive disclosure
		"enable_tool_category",
		// call_tool dispatches arbitrary tools — annotations on the
		// dispatcher itself are not informative. Listed here so it's
		// stamped readOnlyHint=false (conservative) rather than
		// inheriting the unset state.
		"call_tool",
	};
	return kSet;
}

// Subset of WriteSet that may perform destructive updates. Per the
// spec, destructive removes/replaces existing state — pure additive
// writes (add_*, create_*, set_*) DON'T belong here. Re-typing a
// variable or renaming overwrites a single field but doesn't lose
// downstream data; we leave those as non-destructive.
//
// shutdown_daemon is destructive in the spec sense: it tears down
// the server process, which is destructive to the agent's working
// environment.
const std::set<std::string>& DestructiveSet() {
	static const std::set<std::string> kSet = {
		"delete_variable", "delete_function", "delete_node",
		"delete_actor", "delete_asset",
		"remove_component",
		"shutdown_daemon",
	};
	return kSet;
}

// Subset of WriteSet that talks to "external entities" — anything
// outside the editor's own in-memory + project filesystem. The spec
// example is "checking weather" (open world) vs "querying sqlite"
// (not). Things that shell out to UAT, run arbitrary Python, or
// execute arbitrary editor console commands all qualify.
//
// Live coding does invoke a local compiler — we treat that as NOT
// open-world (closed-system: known tool, known output paths) because
// flagging every compile invocation here would defeat the purpose
// of the toggle.
const std::set<std::string>& OpenWorldSet() {
	static const std::set<std::string> kSet = {
		"console_command",
		"run_python_script",
		"cook_content",
		"package_project",
	};
	return kSet;
}

// Subset of WriteSet where 2nd call with the same arguments is
// guaranteed to be a no-op. The compile_* family qualifies (same
// input → same compiled output), as does save_all (saves dirty;
// 2nd call has nothing to save), build_lighting (rebuilds same
// result from same scene), and write_generated_source (overwrites
// with same content).
//
// For set_* tools, idempotency is true IF the same value is passed,
// but the API surface allows different args, so we leave those
// unset. Same with rename/retype — leaving unset is more honest
// than claiming a guarantee.
const std::set<std::string>& IdempotentWriteSet() {
	static const std::set<std::string> kSet = {
		"save_all",
		"compile_function",
		"compile_material",
		"compile_widget_blueprint",
		"compile_behavior_tree",
		"compile_state_tree",
		"compile_anim_blueprint",
		"build_lighting",
		"live_coding_compile",
		"write_generated_source",
	};
	return kSet;
}

}    // namespace tool_annotations_detail
using namespace tool_annotations_detail;

ToolAnnotations AnnotationsFor(const std::string& name) {
	ToolAnnotations a;
	if (ReadOnlySet().count(name) > 0) {
		a.read_only_hint   = true;
		a.destructive_hint = false;
		a.idempotent_hint  = true;
		a.open_world_hint  = false;
		return a;
	}
	if (WriteSet().count(name) > 0) {
		a.read_only_hint   = false;
		a.destructive_hint = DestructiveSet().count(name) > 0;
		a.open_world_hint  = OpenWorldSet().count(name) > 0;
		if (IdempotentWriteSet().count(name) > 0) {
			a.idempotent_hint = true;
		}
		return a;
	}
	// Unknown name — return all-nullopt so the descriptor's annotations
	// stay untouched. Lets test stubs and future tools register
	// without inheriting a wrong classification.
	return a;
}

}    // namespace bpr::tools
