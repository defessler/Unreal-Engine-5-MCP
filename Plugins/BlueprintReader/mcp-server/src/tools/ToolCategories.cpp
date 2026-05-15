#include "tools/ToolCategories.h"

#include <map>

namespace bpr::tools {
namespace {

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
            // Discoverability
            "list_node_kinds",
            "list_pin_categories",
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
        }},

        // --- editor: live-editor surface (PIE, viewport, console, log) ---
        {"editor", {
            "pie_start", "pie_stop",
            "get_selected_actors", "set_selection",
            "spawn_actor", "set_actor_transform", "delete_actor",
            "focus_actor", "set_camera_transform",
            "take_screenshot", "take_viewport_screenshot",
            "set_show_flag",
            "console_command", "get_cvar", "set_cvar",
            "read_output_log",
            "live_coding_compile",
        }},

        // --- assets: project-level asset management ----------------------
        {"assets", {
            "list_blueprints",
            "move_asset", "delete_asset",
            "create_folder",
            "get_project_metadata",
            "save_all",
        }},

        // --- per-asset-type slices (~4–7 tools each) ---------------------
        {"materials", {
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
        }},
        {"discover", {
            "list_node_kinds", "list_pin_categories",
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

} // namespace

std::vector<std::string> ExpandCategory(const std::string& name) {
    const auto& table = CategoryTable();
    auto it = table.find(name);
    if (it == table.end()) return {};
    return it->second;
}

bool IsKnownCategory(const std::string& name) {
    return CategoryTable().count(name) > 0;
}

} // namespace bpr::tools
