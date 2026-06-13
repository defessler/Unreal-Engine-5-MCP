#include "tools/ToolAnnotations.h"

#include <map>
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
		"diff_asset",
		"prepare_merge",
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
		"describe_k2node",  // EDIT-5: transient-instance K2-node introspection
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
		"get_focused_widget",
		"ui_list_widgets",  // TEST-2 P0: read-only Slate-tree walk
		// --- BP↔C++ pure compute (no write) -----------------------------
		"decompile_function", "decompile_blueprint",
		"transpile_function", "transpile_blueprint",
		"parse_cpp_function",
		"generate_k2node_skeleton",  // EDIT-5: emits text, writes nothing
		// --- apply_ops dry-run ------------------------------------------
		"preview_ops",
		// --- Lazy-discovery meta-tools ----------------------------------
		"list_toolsets", "describe_toolset",
		// --- Actor instance introspection --------------------------------
		"read_actor_instance",
		// --- Phase 8 EA-pull Wave 2/3: editor-state reads ---------------
		"get_camera_transform", "get_view_mode", "get_show_flags",
		"get_selected_components", "get_selected_assets",
		"get_selected_folders", "get_content_browser_path",
		"world_to_screen", "screen_to_world",
		"get_camera_bookmarks",
		"get_hover_target",
		"get_isolate_mode",
		"get_async_compile_state",
		"get_shader_compile_state",
		"get_current_level",
		"list_loaded_levels",
		"get_source_control_provider",
		"get_asset_registry_state",
		"get_data_layer_states",
		"get_autosave_status",
		"get_recovery_state",
		"get_source_control_status",
		"get_file_lock_status",
		"get_active_culture",
		"get_editor_theme",
		"get_monitors",
		"get_live_coding_state",
		"get_recently_opened_assets",
		"get_debug_instance",
		"get_blueprint_breakpoints",
		"get_watched_pins",
		"get_active_stats",
		"get_streaming_sources",
		"get_recently_saved_packages",
		"list_project_settings",
		"get_project_setting_values",
		"list_automation_tests",
		"get_editor_events",
		"get_active_cook_target",
		"get_workspace_layout",
		"get_trace_state",
		"get_ui_state_stub",
		// --- Editor UI/UMG reads -----------------------------------------
		"get_viewport_camera_settings",
		"get_snapping_settings",
		"get_viewport_realtime",
		"get_hidden_layers",
		"get_hidden_actors",
		"get_visible_actors",
		"get_gizmo_state",
		"get_buffer_visualization_mode",
		"get_active_viewport",
		"get_curve_editor_selection",
		"get_niagara_module_selection",
		"get_anim_editor_state",
		"get_sequencer_state",
		"get_cinematic_camera",
		"get_mesh_preview_state",
		"get_material_editor_state",
		"get_umg_editor_state",
		"get_static_mesh_info",
		"get_material_instance_params",
		"get_blueprint_editor_state",
		"list_actor_abilities", "list_actor_gameplay_effects",
		"list_actor_attributes", "list_actor_gameplay_tags",
		"list_game_features", "get_game_feature_state",
		"list_plugins", "get_plugin_descriptor", "get_plugin_dependencies",
		"list_desktop_windows",
		// --- Timeline reads (EDIT-2) ------------------------------------
		"list_timelines", "read_timeline",
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
		"clone_graph", "implement_interface",
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
		"create_material", "create_material_instance",
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
		// MCP-2: editor-state setters added to WriteSet so they get
		// readOnlyHint=false / openWorldHint=false explicitly.
		"open_asset_editor", "close_asset_editor",
		"set_selection", "set_selected_assets",
		"set_content_browser_path",
		"set_viewport_realtime", "set_view_mode", "set_gizmo_mode",
		"set_actor_visibility", "set_layer_visibility",
		"goto_camera_bookmark",
		"activate_game_feature", "deactivate_game_feature",
		"set_plugin_enabled",
		"set_project_setting",
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

// MCP-1: curated human-readable titles for the highest-traffic tools.
// ToolRegistry::Add auto-derives a title from the snake_case name for
// any tool not in this table, so only the most important ones need
// explicit entries here.
std::string TitleFor(const std::string& name) {
	static const std::map<std::string, std::string, std::less<>> kTitles = {
		// Core Blueprint reads
		{"list_blueprints",          "List Blueprint Assets"},
		{"list_assets",              "List Assets"},
		{"find_asset",               "Find Asset by Keyword"},
		{"read_blueprint",           "Read Blueprint (Full)"},
		{"summarize_blueprint",      "Summarize Blueprint"},
		{"get_graph",                "Get Blueprint Graph"},
		{"peek_graph",               "Preview Graph (Histogram)"},
		{"get_function",             "Get Function Graph"},
		{"get_node",                 "Get Node by ID"},
		{"find_node",                "Find Nodes by Query"},
		{"find_overriders",          "Find Overriding Blueprints"},
		{"find_dangling_references", "Find Dangling References"},
		{"list_variables",           "List Blueprint Variables"},
		{"list_functions",           "List Blueprint Functions"},
		{"get_components",           "Get Blueprint Components"},
		{"bp_structural_diff",       "Compare Blueprint Structure"},
		{"get_class_info",           "Inspect C++ Class"},
		{"find_class",               "Find C++ Class"},
		// Blueprint mutation
		{"add_variable",             "Add Blueprint Variable"},
		{"delete_variable",          "Delete Blueprint Variable"},
		{"rename_variable",          "Rename Blueprint Variable"},
		{"retype_variable",          "Retype Blueprint Variable"},
		{"set_variable_default",     "Set Variable Default Value"},
		{"set_variable_category",    "Set Variable Category"},
		{"add_function",             "Add Blueprint Function"},
		{"delete_function",          "Delete Blueprint Function"},
		{"add_function_input",       "Add Function Input"},
		{"add_function_output",      "Add Function Output"},
		{"add_node",                 "Add Node to Graph"},
		{"delete_node",              "Delete Graph Node"},
		{"wire_pins",                "Wire Pins Together"},
		{"set_pin_default",          "Set Pin Default Value"},
		{"set_node_position",        "Move Node in Graph"},
		{"apply_ops",                "Apply Batch Operations"},
		{"compile_blueprint",        "Compile Blueprint"},
		{"create_blueprint",         "Create New Blueprint"},
		{"duplicate_blueprint",      "Duplicate Blueprint"},
		{"delete_asset",             "Delete Asset"},
		{"move_asset",               "Move/Rename Asset"},
		{"clone_graph",              "Clone Graph into Blueprint"},
		{"implement_interface",      "Add Interface to Blueprint"},
		// Actor instance
		{"read_actor_instance",      "Read Actor Instance Overrides"},
		// Transpile / BPIR
		{"decompile_function",       "Decompile Function to BPIR"},
		{"decompile_blueprint",      "Decompile Blueprint to BPIR"},
		{"transpile_function",       "Transpile Function to C++"},
		{"transpile_blueprint",      "Transpile Blueprint to C++"},
		{"parse_cpp_function",       "Parse C++ Function to BPIR"},
		{"describe_k2node",          "Describe K2 Node Class"},
		{"generate_k2node_skeleton", "Generate K2 Node Skeleton"},
		{"write_generated_source",   "Write Generated C++ Source"},
		// Editor control
		{"start_pie",                "Start Play-in-Editor"},
		{"stop_pie",                 "Stop Play-in-Editor"},
		{"save_all",                 "Save All Modified Assets"},
		{"compile_live_coding",      "Compile via Live Coding"},
		{"build_lighting",           "Build Level Lighting"},
		{"run_automation_tests",     "Run Automation Tests"},
		{"ui_list_widgets",          "List Editor UI Widgets"},
		{"ui_click",                 "Click Editor Widget"},
		{"ui_type",                  "Type Into Editor Widget"},
		{"ui_focus_tab",             "Focus Editor Dock Tab"},
		{"ui_invoke_menu",           "Invoke Editor Menu"},
		// Discovery / meta
		{"list_node_kinds",          "List Spawnable Node Kinds"},
		{"list_pin_categories",      "List Pin Type Categories"},
		{"enable_tool_category",     "Enable Tool Category"},
		{"list_toolsets",            "List Tool Categories"},
		{"describe_toolset",         "Describe Tool Category"},
		// Per-asset-type reads
		{"read_data_table",          "Read Data Table"},
		{"read_material",            "Read Material"},
		{"read_widget_blueprint",    "Read Widget Blueprint"},
		{"read_behavior_tree",       "Read Behavior Tree"},
		{"read_anim_blueprint",      "Read Anim Blueprint"},
		{"read_niagara_system",      "Read Niagara System"},
		{"read_level_sequence",      "Read Level Sequence"},
		{"get_referencers",          "Get Asset Referencers"},
		{"get_dependencies",         "Get Asset Dependencies"},
	};
	auto it = kTitles.find(name);
	return it != kTitles.end() ? it->second : std::string{};
}

// MCP-9: returns true when the tool may irreversibly remove or replace data.
// Used by the confirmation guard in Mcp.cpp (enabled via BP_READER_REQUIRE_CONFIRM).
bool IsDestructive(const std::string& name) {
	return DestructiveSet().count(name) > 0;
}

}    // namespace bpr::tools
