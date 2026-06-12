#include "tools/ToolCategories.h"

#include <map>

namespace bpr::tools {
namespace tool_categories_detail {

// Each category maps to the tool names it contains. Tool registration
// in BlueprintTools.cpp is the source of truth for which names exist;
// these lists name the subset to include for a category. A tool can
// belong to multiple categories (e.g. `read_blueprint` is in both
// `core` and `read`).
//
// Keep entries grouped + alphabetized inside each category — easier to
// audit at code review. If you rename a tool, search this file too;
// the lists are plain strings, not symbolic references.
const std::map<std::string, std::vector<std::string>>& CategoryTable() {
	static const std::map<std::string, std::vector<std::string>> kTable = {
		// --- core: minimum viable Blueprint authoring surface ------------
		// ~35 tools. The everyday read+write set: enumeration, lookup,
		// var/function/node CRUD, batches, and the discoverability tools
		// an agent needs to know what's even possible.
		{"core", {
			// Enumeration + lookup
			"list_blueprints",
			"read_blueprint",
			"summarize_blueprint",
			"get_graph",
			"get_function",
			"get_node",
			"get_components",
			"find_node",
			"find_overriders",
			"list_variables",
			"list_functions",
			"get_project_metadata",
			// Variable CRUD
			"add_variable",
			"delete_variable",
			"rename_variable",
			"retype_variable",
			"set_variable_default",
			"set_variable_category",
			// Function CRUD
			"add_function",
			"add_function_input",
			"add_function_output",
			"delete_function",
			// Graph CRUD
			"add_node",
			"delete_node",
			"set_node_position",
			"wire_pins",
			"auto_layout_graph",
			// BP-level CRUD
			"create_blueprint",
			"duplicate_blueprint",
			"save_all",
			// Batches + ops
			"apply_ops",
			"compile_function",
			// Discoverability + situational awareness
			"list_node_kinds",
			"list_pin_categories",
			"get_editor_state",
			"health_check",
			"shutdown_daemon",
		}},

		// --- read: every read-only tool, no writes -----------------------
		{"read", {
			"list_blueprints", "read_blueprint", "summarize_blueprint",
			"get_graph", "get_function", "get_node", "get_components",
			"find_node", "find_overriders", "list_variables",
			"list_functions", "get_project_metadata",
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
			"find_class", "get_class_info",
			"describe_k2node",  // EDIT-5: K2-node class introspection
			// Diff / merge (read-only structural comparison) — UX-P4g:
			// these ship fully wired but were in NO category, so progressive
			// disclosure hid them (callable only via call_tool by exact name).
			"diff_asset", "prepare_merge", "bp_structural_diff",
			"list_node_kinds", "list_pin_categories",
			"read_output_log", "get_cvar", "get_stats",
			"get_selected_actors",
		}},

		// --- write: every mutating tool on a Blueprint or asset ----------
		// Pairs with `read` to give the full BP authoring set without
		// editor or specialized-asset surfaces.
		{"write", {
			"add_variable", "delete_variable", "rename_variable",
			"retype_variable", "set_variable_default", "set_variable_category",
			"add_function", "add_function_input", "add_function_output",
			"delete_function",
			"add_node", "delete_node", "set_node_position", "wire_pins",
			"auto_layout_graph",
			"create_blueprint", "duplicate_blueprint", "save_all",
			"add_component", "attach_component", "remove_component",
			"set_component_property",
			"apply_ops", "compile_function",
			"shutdown_daemon",
		}},

		// --- cpp: BP <-> source pipeline ---------------------------------
		{"cpp", {
			"decompile_function",
			"decompile_blueprint",
			"transpile_function",
			"transpile_blueprint",
			"parse_cpp_function",
			"compile_function",
			"write_generated_source",
			"generate_k2node_skeleton",  // EDIT-5: custom-node skeleton codegen
		}},

		// --- editor: live-editor surface (PIE, viewport, console, log) ---
		{"editor", {
			"pie_start", "pie_stop",
			"get_selected_actors", "set_selection",
			"get_editor_state",
			"spawn_actor", "set_actor_transform", "delete_actor",
			"focus_actor", "set_camera_transform",
			"take_screenshot", "take_viewport_screenshot",
			"set_show_flag",
			"console_command", "get_cvar", "set_cvar",
			"read_output_log",
			"run_python_script",
			"build_lighting",
			"live_coding_compile",
			"ui_list_widgets",  // TEST-2 P0: Slate widget-tree inspection
			"ui_click",         // TEST-2 P1b: click a widget by path (gated)
			"ui_type",          // TEST-2 P1b: type into a widget by path (gated)
			"ui_focus_tab",     // TEST-2 P1b: focus a dock tab by label (gated)
		}},

		// --- assets: project-level asset management ----------------------
		{"assets", {
			"list_blueprints",
			"move_asset", "delete_asset",
			"create_folder",
			"diff_asset", "prepare_merge", "bp_structural_diff",
			"get_project_metadata",
			"save_all",
			"get_referencers", "get_dependencies",
			"read_config_value", "set_config_value",
		}},

		// --- per-asset-type slices (~4–7 tools each) ---------------------
		{"materials", {
			"create_material", "create_material_instance",
			"list_materials", "read_material",
			"add_material_expression", "connect_material_expressions",
			"set_material_parameter", "set_material_instance_parameter",
			"compile_material",
		}},
		{"widgets", {
			"read_widget_blueprint",
			"add_widget", "set_widget_property", "bind_widget_event",
			"compile_widget_blueprint",
		}},
		{"behavior-trees", {
			"list_behavior_trees", "read_behavior_tree",
			"add_bt_node", "set_bt_node_property",
			"compile_behavior_tree",
		}},
		{"data-tables", {
			"list_data_tables", "read_data_table",
			"add_data_row", "set_data_row_value",
		}},
		{"data-assets", {
			"list_data_assets", "read_data_asset",
			"create_data_asset", "set_data_asset_property",
		}},
		{"state-trees", {
			"list_state_trees", "read_state_tree",
			"add_state_tree_state", "set_state_tree_transition",
			"compile_state_tree",
		}},
		{"niagara", {
			"list_niagara_systems", "read_niagara_system",
			"create_niagara_system", "set_niagara_parameter",
		}},
		{"sequencer", {
			"list_level_sequences", "read_level_sequence",
			"add_sequence_track", "set_sequence_playback_range",
		}},
		{"gameplay-tags", {
			"list_gameplay_tags", "add_gameplay_tag", "read_ability_set",
		}},
		{"anim-bp", {
			"list_anim_blueprints", "read_anim_blueprint",
			"add_anim_state", "compile_anim_blueprint",
		}},

		// --- ops surface (perf, cook, tests, class discovery, meta) ------
		{"profiling", {
			"start_profile", "stop_profile", "get_stats",
		}},
		{"cook", {
			"cook_content", "package_project",
		}},
		{"tests", {
			"run_automation_tests",
		}},
		{"class-info", {
			"find_class", "get_class_info", "list_functions",
			"describe_k2node",
		}},
		{"discover", {
			"list_node_kinds", "list_pin_categories",
			"describe_k2node",
			"shutdown_daemon",
		}},

		// --- workflow presets: cross-category sets tuned for a task --------
		//
		// Distinct from the per-domain categories above: each workflow is a
		// curated set of tools that REALISTICALLY ship together for a
		// specific kind of session. Picked to keep tool counts well under
		// any common client cap (Copilot's 128 leaves ~9 slots after our
		// surface; even the biggest workflow here is ~25 tools).
		//
		// Naming: hyphenated, task-shaped. Easy to distinguish from the
		// single-word per-domain categories.

		// `bp-authoring`: equivalent to `core`. Listed separately so users
		// can write the more task-shaped name in their env config.
		{"bp-authoring", {
			"list_blueprints", "read_blueprint", "summarize_blueprint",
			"get_graph", "get_function", "get_node", "get_components",
			"find_node", "find_overriders", "list_variables",
			"list_functions", "get_project_metadata",
			"add_variable", "delete_variable", "rename_variable",
			"retype_variable", "set_variable_default", "set_variable_category",
			"add_function", "add_function_input", "add_function_output",
			"delete_function",
			"add_node", "delete_node", "set_node_position", "wire_pins",
			"auto_layout_graph",
			"create_blueprint", "duplicate_blueprint", "save_all",
			"apply_ops", "compile_function",
			"list_node_kinds", "list_pin_categories", "shutdown_daemon",
		}},

		// `material-tuning`: read a BP to find its mesh, look at the
		// material on that component, tweak parameters, compile to apply.
		{"material-tuning", {
			// Find what's using the material
			"list_blueprints", "read_blueprint", "get_components",
			// Material editing
			"list_materials", "read_material",
			"set_material_parameter",
			"set_material_instance_parameter",
			"compile_material",
			// Verify
			"save_all", "read_output_log", "shutdown_daemon",
		}},

		// `cpp-roundtrip`: BP <-> source. Decompile a BP to BPIR, render
		// C++, parse C++ back, compile back to BP. Includes basic BP read
		// tools so the agent can locate the function it's working on.
		{"cpp-roundtrip", {
			"list_blueprints", "read_blueprint", "get_function",
			"decompile_function", "decompile_blueprint",
			"transpile_function", "transpile_blueprint",
			"parse_cpp_function", "write_generated_source",
			"describe_k2node", "generate_k2node_skeleton",  // EDIT-5
			"compile_function", "apply_ops",
			"save_all", "shutdown_daemon",
		}},

		// `editor-control`: viewport + PIE + console + log. Pure runtime-
		// editor surface, no BP authoring. For workflows where the agent
		// is driving the editor like a remote control.
		{"editor-control", {
			"pie_start", "pie_stop",
			"get_selected_actors", "set_selection",
			"spawn_actor", "set_actor_transform", "delete_actor",
			"focus_actor", "set_camera_transform",
			"take_screenshot", "take_viewport_screenshot",
			"set_show_flag",
			"console_command", "get_cvar", "set_cvar",
			"read_output_log",
			"live_coding_compile",
			"ui_list_widgets",  // TEST-2 P0: Slate widget-tree inspection
			"ui_click",         // TEST-2 P1b: click a widget by path (gated)
			"ui_type",          // TEST-2 P1b: type into a widget by path (gated)
			"ui_focus_tab",     // TEST-2 P1b: focus a dock tab by label (gated)
			"get_project_metadata", "shutdown_daemon",
		}},

		// `widget-design`: UMG widget authoring focused. Find widget BPs,
		// read their tree, add nodes, set props, wire events, compile.
		{"widget-design", {
			"list_blueprints", "read_widget_blueprint",
			"add_widget", "set_widget_property", "bind_widget_event",
			"compile_widget_blueprint", "save_all", "shutdown_daemon",
		}},

		// `gameplay-tuning`: read BPs to find designer-exposed knobs,
		// tweak variable defaults, batch-apply, run PIE to verify.
		// Doesn't include node-level CRUD — this is the variable-and-PIE
		// loop, not graph editing.
		{"gameplay-tuning", {
			"list_blueprints", "read_blueprint", "summarize_blueprint",
			"list_variables", "get_components",
			"set_variable_default", "set_component_property",
			"apply_ops", "save_all",
			"pie_start", "pie_stop",
			"console_command", "get_cvar", "set_cvar",
			"read_output_log", "shutdown_daemon",
		}},
	};
	return kTable;
}

}    // namespace tool_categories_detail
using namespace tool_categories_detail;

std::vector<std::string> ExpandCategory(const std::string& name) {
	const auto& table = CategoryTable();
	auto it = table.find(name);
	if (it == table.end()) return {};
	return it->second;
}

bool IsKnownCategory(const std::string& name) {
	return CategoryTable().count(name) > 0;
}

namespace category_description_detail {

// One-line summaries for each category — surfaced by the lazy-discovery
// `list_toolsets` meta-tool so a client sees enough to choose a toolset
// without expanding its full tool list. Kept in sync with the entries
// in CategoryTable above; if you add a category there, add a description
// here too.
const std::map<std::string, std::string>& DescriptionTable() {
	static const std::map<std::string, std::string> kTable = {
		{"core",          "Minimum viable Blueprint authoring surface: read, write, function/variable/node CRUD, batches, plus discoverability tools."},
		{"read",          "Every read-only tool — Blueprints, materials, widgets, behavior trees, data assets, state trees, niagara, sequencer, anim BPs, classes, log/output, cvars."},
		{"write",         "Every mutating tool on a Blueprint or asset: variables, functions, nodes, components, batches."},
		{"cpp",           "BP↔C++ pipeline: decompile, transpile, parse, compile_function, write_generated_source. Driven by the BPIR JSON AST."},
		{"editor",        "Live-editor surface: PIE, viewport, console, log, build_lighting, live_coding_compile, screenshots, show flags, python."},
		{"assets",        "Project-level asset management: move, delete, folders, project metadata, referencers/dependencies, config values."},
		{"materials",     "Material authoring: list/read, expressions, parameter binding, instance overrides, compile."},
		{"widgets",       "UMG widget authoring: read widget BPs, add widgets, set props, bind events, compile."},
		{"behavior-trees","Behavior tree authoring: list/read, add nodes, set props, compile."},
		{"data-tables",   "Data table CRUD: list/read, add rows, set row values."},
		{"data-assets",   "Data asset CRUD: list/read, create, set properties."},
		{"state-trees",   "State tree authoring: list/read, add states, set transitions, compile."},
		{"niagara",       "Niagara VFX: list/read systems, create, parameter binding."},
		{"sequencer",     "Level sequencer: list/read, add tracks, set playback range."},
		{"gameplay-tags", "Gameplay tag taxonomy + ability sets."},
		{"anim-bp",       "Animation Blueprints: list/read, add states, compile."},
		{"profiling",     "CPU/GPU profile capture and stats."},
		{"cook",          "Headless cook + package via UAT."},
		{"tests",         "Automation test runner."},
		{"class-info",    "UClass introspection — properties, functions, ancestor chain."},
		{"discover",      "Self-discovery tools: list_node_kinds, list_pin_categories, shutdown_daemon."},
		{"bp-authoring",  "Workflow preset: full BP authoring set (= `core` under a task-shaped name)."},
		{"material-tuning","Workflow preset: locate the material a BP uses, tweak params, compile to apply, verify."},
		{"cpp-roundtrip", "Workflow preset: decompile BP→BPIR→C++, edit, parse C++ back, compile back to BP."},
		{"editor-control","Workflow preset: drive the editor like a remote (viewport, PIE, console, log, live coding) without BP authoring."},
		{"widget-design", "Workflow preset: UMG widget authoring focused — find, edit, compile."},
		{"gameplay-tuning","Workflow preset: tweak designer-exposed knobs (variable defaults, component props), PIE to verify."},
	};
	return kTable;
}

}    // namespace category_description_detail
using namespace category_description_detail;

std::string CategoryDescription(const std::string& name) {
	const auto& table = DescriptionTable();
	auto it = table.find(name);
	return (it == table.end()) ? std::string{} : it->second;
}

std::vector<std::string> AllCategoryNames() {
	std::vector<std::string> out;
	out.reserve(CategoryTable().size());
	for (const auto& [k, _] : CategoryTable()) {
		out.push_back(k);
	}
	return out;
}

}    // namespace bpr::tools
