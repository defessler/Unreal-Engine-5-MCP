#include "backends/MockBlueprintReader.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>

#include <fmt/core.h>
#include <nlohmann/json.hpp>

namespace bpr::backends {

namespace mock_blueprint_reader_detail {

std::string LowerAscii(std::string_view s) {
	std::string out;
	out.reserve(s.size());
	for (char c : s) {
		out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
	}
	return out;
}

bool ContainsCI(std::string_view haystack, std::string_view needle) {
	if (needle.empty())
	{
		return true;
	}
	auto h = LowerAscii(haystack);
	auto n = LowerAscii(needle);
	return h.find(n) != std::string::npos;
}

// Path matching for ListBlueprints: prefix match on the fixture's asset_path.
// `/Game` matches everything under /Game; `/Game/AI` matches only that subtree.
bool PathMatches(std::string_view filter, std::string_view assetPath) {
	if (filter.empty() || filter == "/" || filter == "/Game") {
		// Root filter — anything under /Game counts.
		return assetPath.rfind("/Game", 0) == 0;
	}
	if (assetPath.size() < filter.size())
	{
		return false;
	}
	if (assetPath.compare(0, filter.size(), filter) != 0)
	{
		return false;
	}
	if (assetPath.size() == filter.size())
	{
		return true;
	}
	char next = assetPath[filter.size()];
	return next == '/';
}

}    // namespace mock_blueprint_reader_detail
using namespace mock_blueprint_reader_detail;

MockBlueprintReader::MockBlueprintReader(const std::filesystem::path& fixturesDir) {
	if (!std::filesystem::exists(fixturesDir)) {
		throw BlueprintReaderError(
			fmt::format("fixture directory does not exist: {}", fixturesDir.string()));
	}
	if (!std::filesystem::is_directory(fixturesDir)) {
		throw BlueprintReaderError(
			fmt::format("fixture path is not a directory: {}", fixturesDir.string()));
	}
	LoadDir(fixturesDir);
}

void MockBlueprintReader::LoadDir(const std::filesystem::path& dir) {
	for (const auto& entry : std::filesystem::directory_iterator(dir)) {
		if (!entry.is_regular_file())
		{
			continue;
		}
		if (entry.path().extension() != ".json")
		{
			continue;
		}
		LoadFile(entry.path());
	}
}

void MockBlueprintReader::LoadFile(const std::filesystem::path& file) {
	std::ifstream in(file);
	if (!in) {
		throw BlueprintReaderError(fmt::format("failed to open fixture: {}", file.string()));
	}
	nlohmann::json j;
	try {
		in >> j;
	} catch (const std::exception& e) {
		throw BlueprintReaderError(
			fmt::format("failed to parse fixture {}: {}", file.string(), e.what()));
	}

	FixtureEntry entry;
	try {
		entry.summary = j.at("summary").get<BPAssetSummary>();
		entry.metadata = j.at("metadata").get<BPMetadata>();
		if (auto graphsIt = j.find("graphs"); graphsIt != j.end()) {
			entry.graphs = graphsIt->get<std::vector<BPGraph>>();
		}
		if (auto fnIt = j.find("functions"); fnIt != j.end()) {
			entry.functions = fnIt->get<std::vector<BPFunction>>();
		}
		if (auto compIt = j.find("components"); compIt != j.end()) {
			entry.components = compIt->get<std::vector<BPComponent>>();
		}
	} catch (const std::exception& e) {
		throw BlueprintReaderError(
			fmt::format("malformed fixture {}: {}", file.string(), e.what()));
	}

	if (entry.summary.AssetPath.empty()) {
		throw BlueprintReaderError(
			fmt::format("fixture {} has empty summary.asset_path", file.string()));
	}

	auto key = entry.summary.AssetPath;
	if (assets_.find(key) != assets_.end()) {
		throw BlueprintReaderError(
			fmt::format("duplicate fixture asset_path: {}", key));
	}
	assets_.emplace(std::move(key), std::move(entry));
}

const MockBlueprintReader::FixtureEntry&
MockBlueprintReader::Require(std::string_view assetPath) const {
	auto it = assets_.find(assetPath);
	if (it == assets_.end()) {
		throw AssetNotFound(fmt::format("asset not found: {}", assetPath));
	}
	return it->second;
}

std::vector<BPAssetSummary> MockBlueprintReader::ListBlueprints(std::string_view path) {
	std::vector<BPAssetSummary> out;
	for (const auto& [k, entry] : assets_) {
		if (PathMatches(path, entry.summary.AssetPath)) {
			out.push_back(entry.summary);
		}
	}
	std::sort(out.begin(), out.end(),
			  [](const BPAssetSummary& a, const BPAssetSummary& b) {
				  return a.AssetPath < b.AssetPath;
			  });
	return out;
}

BPMetadata MockBlueprintReader::ReadBlueprint(std::string_view assetPath) {
	return Require(assetPath).metadata;
}

BPGraph MockBlueprintReader::GetGraph(std::string_view assetPath,
									  std::string_view graphName) {
	const auto& entry = Require(assetPath);
	for (const auto& g : entry.graphs) {
		if (g.Name == graphName)
		{
			return g;
		}
	}
	throw BlueprintReaderError(
		fmt::format("graph not found in {}: {}", assetPath, graphName));
}

BPFunction MockBlueprintReader::GetFunction(std::string_view assetPath,
											std::string_view fnName) {
	const auto& entry = Require(assetPath);
	for (const auto& f : entry.functions) {
		if (f.Name == fnName)
		{
			return f;
		}
	}
	throw BlueprintReaderError(
		fmt::format("function not found in {}: {}", assetPath, fnName));
}

std::vector<BPVariable> MockBlueprintReader::ListVariables(std::string_view assetPath) {
	return Require(assetPath).metadata.Variables;
}

std::vector<BPComponent> MockBlueprintReader::GetComponents(std::string_view assetPath) {
	return Require(assetPath).components;
}

void MockBlueprintReader::AddVariable(std::string_view, std::string_view,
									  const BPPinType&, std::string_view,
									  std::string_view, bool, bool) {
	throw BlueprintReaderError(
		"AddVariable: mock backend is read-only; set BP_READER_BACKEND=commandlet");
}

void MockBlueprintReader::SetNodePosition(std::string_view, std::string_view,
										  std::string_view, int, int) {
	throw BlueprintReaderError(
		"SetNodePosition: mock backend is read-only; set BP_READER_BACKEND=commandlet");
}

void MockBlueprintReader::DeleteNode(std::string_view, std::string_view,
									 std::string_view) {
	throw BlueprintReaderError(
		"DeleteNode: mock backend is read-only; set BP_READER_BACKEND=commandlet");
}

std::string MockBlueprintReader::AddNode(std::string_view, std::string_view,
										 std::string_view, int, int,
										 const std::map<std::string, std::string, std::less<>>&) {
	throw BlueprintReaderError(
		"AddNode: mock backend is read-only; set BP_READER_BACKEND=commandlet");
}

void MockBlueprintReader::WirePins(std::string_view, std::string_view,
								   std::string_view, std::string_view,
								   std::string_view, std::string_view) {
	throw BlueprintReaderError(
		"WirePins: mock backend is read-only; set BP_READER_BACKEND=commandlet");
}

void MockBlueprintReader::DeleteVariable(std::string_view, std::string_view) {
	throw BlueprintReaderError(
		"DeleteVariable: mock backend is read-only; set BP_READER_BACKEND=commandlet");
}

void MockBlueprintReader::RenameVariable(std::string_view, std::string_view, std::string_view) {
	throw BlueprintReaderError(
		"RenameVariable: mock backend is read-only; set BP_READER_BACKEND=commandlet");
}

IBlueprintReader::AddFunctionResult
MockBlueprintReader::AddFunction(std::string_view, std::string_view) {
	throw BlueprintReaderError(
		"AddFunction: mock backend is read-only; set BP_READER_BACKEND=commandlet");
}

void MockBlueprintReader::AddFunctionInput(std::string_view, std::string_view,
										   std::string_view, const BPPinType&) {
	throw BlueprintReaderError(
		"AddFunctionInput: mock backend is read-only; set BP_READER_BACKEND=commandlet");
}

void MockBlueprintReader::AddFunctionOutput(std::string_view, std::string_view,
											std::string_view, const BPPinType&) {
	throw BlueprintReaderError(
		"AddFunctionOutput: mock backend is read-only; set BP_READER_BACKEND=commandlet");
}

void MockBlueprintReader::DeleteFunction(std::string_view, std::string_view) {
	throw BlueprintReaderError(
		"DeleteFunction: mock backend is read-only; set BP_READER_BACKEND=commandlet");
}

void MockBlueprintReader::SetVariableDefault(std::string_view, std::string_view, std::string_view) {
	throw BlueprintReaderError(
		"SetVariableDefault: mock backend is read-only; set BP_READER_BACKEND=commandlet");
}

IBlueprintReader::CreateBlueprintResult
MockBlueprintReader::CreateBlueprint(std::string_view, std::string_view, std::string_view) {
	throw BlueprintReaderError(
		"CreateBlueprint: mock backend is read-only; set BP_READER_BACKEND=commandlet");
}

IBlueprintReader::CreateMaterialResult
MockBlueprintReader::CreateMaterial(std::string_view) {
	throw BlueprintReaderError(
		"CreateMaterial: mock backend is read-only; set BP_READER_BACKEND=commandlet");
}

IBlueprintReader::CreateMaterialInstanceResult
MockBlueprintReader::CreateMaterialInstance(std::string_view, std::string_view) {
	throw BlueprintReaderError(
		"CreateMaterialInstance: mock backend is read-only; set BP_READER_BACKEND=commandlet");
}

IBlueprintReader::CloneGraphResult
MockBlueprintReader::CloneGraph(std::string_view, std::string_view, std::string_view) {
	throw BlueprintReaderError(
		"CloneGraph: mock backend is read-only; set BP_READER_BACKEND=commandlet");
}

void MockBlueprintReader::ImplementInterface(std::string_view, std::string_view) {
	throw BlueprintReaderError(
		"ImplementInterface: mock backend is read-only; set BP_READER_BACKEND=commandlet");
}

void MockBlueprintReader::SetPinDefault(std::string_view, std::string_view,
										std::string_view, std::string_view,
										std::string_view) {
	throw BlueprintReaderError(
		"SetPinDefault: mock backend is read-only; set BP_READER_BACKEND=commandlet");
}

void MockBlueprintReader::RetypeVariable(std::string_view, std::string_view,
										 const BPPinType&) {
	throw BlueprintReaderError(
		"RetypeVariable: mock backend is read-only; set BP_READER_BACKEND=commandlet");
}

void MockBlueprintReader::SetVariableCategory(std::string_view, std::string_view,
											  std::string_view) {
	throw BlueprintReaderError(
		"SetVariableCategory: mock backend is read-only; set BP_READER_BACKEND=commandlet");
}

IBlueprintReader::DuplicateBlueprintResult
MockBlueprintReader::DuplicateBlueprint(std::string_view, std::string_view) {
	throw BlueprintReaderError(
		"DuplicateBlueprint: mock backend is read-only; set BP_READER_BACKEND=commandlet");
}

IBlueprintReader::WriteGeneratedSourceResult
MockBlueprintReader::WriteGeneratedSource(std::string_view, std::string_view, bool) {
	throw BlueprintReaderError(
		"WriteGeneratedSource: mock backend is read-only; set BP_READER_BACKEND=commandlet");
}

nlohmann::json MockBlueprintReader::StructuralDiff(
	std::string_view, std::string_view, const StructuralDiffOptions&) {
	throw BlueprintReaderError(
		"StructuralDiff requires the live or commandlet backend "
		"(needs UBlueprint reflection that mock fixtures don't provide)");
}

nlohmann::json MockBlueprintReader::ReadActorInstance(std::string_view) {
	throw BlueprintReaderError(
		"ReadActorInstance requires the live or commandlet backend "
		"(needs a UObject world that mock fixtures don't provide)");
}

std::vector<std::string> MockBlueprintReader::UnsupportedTools() const {
	// Tools the mock backend doesn't implement and would otherwise
	// throw "not supported by this backend" for. Main.cpp deny-filters
	// these from the registry at startup so the catalog only advertises
	// what's actually callable on the active backend.
	//
	// Write tools that mock overrides to throw a clear "mock backend
	// is read-only" error are NOT included here — that error is
	// actionable, the agent learns the right env var to set, so we
	// keep them visible.
	return {
		// Editor live state — no editor to query
		"get_editor_state",
		"get_selected_actors", "set_selection",
		"spawn_actor", "set_actor_transform", "delete_actor",
		"focus_actor", "set_camera_transform",
		"take_screenshot", "take_viewport_screenshot",
		"take_annotated_screenshot", "set_show_flag",
		"pie_start", "pie_stop", "live_coding_compile",
		"build_lighting",
		"console_command", "get_cvar", "set_cvar",
		"read_output_log", "run_python_script",
		// Asset registry — no registry without editor
		"get_referencers", "get_dependencies",
		// Per-asset-type readers — fixtures are blueprints only
		"list_data_tables", "read_data_table", "add_data_row", "set_data_row_value",
		"list_materials", "read_material",
		"add_material_expression", "connect_material_expressions",
		"set_material_parameter", "set_material_instance_parameter",
		"compile_material",
		"read_widget_blueprint", "add_widget", "set_widget_property",
		"bind_widget_event", "compile_widget_blueprint",
		"list_behavior_trees", "read_behavior_tree",
		"add_bt_node", "set_bt_node_property", "compile_behavior_tree",
		"list_data_assets", "read_data_asset",
		"create_data_asset", "set_data_asset_property",
		"list_state_trees", "read_state_tree",
		"add_state_tree_state", "set_state_tree_transition", "compile_state_tree",
		"list_niagara_systems", "read_niagara_system",
		"create_niagara_system", "set_niagara_parameter",
		"list_level_sequences", "read_level_sequence",
		"add_sequence_track", "set_sequence_playback_range",
		"list_gameplay_tags", "add_gameplay_tag", "read_ability_set",
		"list_anim_blueprints", "read_anim_blueprint",
		"add_anim_state", "compile_anim_blueprint",
		"list_timelines", "read_timeline",  // EDIT-2: no timeline fixtures yet
		"list_anim_montages", "read_anim_montage",  // EDIT-4: no montage fixtures yet
		// Class info — fixtures don't include the UClass registry
		"find_class", "get_class_info", "list_functions",
		// Asset management
		"move_asset", "delete_asset", "create_folder",
		"read_config_value", "set_config_value",
		// Profiling
		"start_profile", "stop_profile", "get_stats",
		// Cook + package
		"cook_content", "package_project",
		// Tests
		"run_automation_tests",
		// Project metadata
		"get_project_metadata",
		// Generic asset registry — fixtures aren't the asset registry
		"list_assets", "find_asset",
		// Reads an arbitrary UObject / OFPA actor instance — needs a world
		"read_actor_instance",
		// Phase 8 EA-pull Wave 1 (partial) — all require a live editor
		"list_open_assets", "get_active_asset", "get_compile_status",
		"get_dirty_packages", "get_focused_window",
		"get_pie_state", "get_modal_state", "get_active_editor_mode",
		"get_focused_widget",

		// ---------------------------------------------------------------
		// Live-editor-dependent surface the mock backend does not serve.
		// These have no UClass / fixture analog and would otherwise fall
		// through to IBlueprintReader's throwing default ("not supported
		// by this backend") on a real mock server. Grouped by family.
		// ---------------------------------------------------------------

		// Package saving — no editor package state to flush
		"save_all",
		// Asset-editor windows — no editor host to open/close/inspect
		"open_asset_editor", "close_asset_editor",
		// Sub-editor live state — needs the corresponding open editor
		"get_anim_editor_state", "get_niagara_module_selection",
		"get_curve_editor_selection", "get_sequencer_state",
		"get_cinematic_camera", "get_mesh_preview_state",
		"get_material_editor_state", "get_umg_editor_state",
		"get_static_mesh_info", "get_material_instance_params",
		"get_blueprint_editor_state",
		// Viewport / level-editor state — no viewport without an editor
		"get_visible_actors", "get_hidden_layers", "set_view_mode",
		"set_gizmo_mode", "get_camera_bookmarks", "get_hover_target",
		"get_isolate_mode", "get_hidden_actors", "get_snapping_settings",
		"get_active_viewport", "get_buffer_visualization_mode",
		"get_gizmo_state", "get_viewport_realtime",
		"get_viewport_camera_settings", "get_camera_transform",
		"get_view_mode", "get_show_flags",
		// Gameplay-ability-system actor pulls — need a live actor
		"list_actor_attributes", "list_actor_gameplay_effects",
		"list_actor_abilities", "list_actor_gameplay_tags",
		// Plugins / game features — need the live plugin manager
		"list_plugins", "get_plugin_descriptor", "get_plugin_dependencies",
		"list_game_features", "get_game_feature_state",
		"activate_game_feature", "deactivate_game_feature",
		// Slate / desktop UI introspection — no Slate app in a CLI mock
		"ui_snapshot", "ui_find", "list_desktop_windows",
		// Content-browser + asset selection — needs the editor UI
		"get_selected_assets", "set_selected_assets", "get_selected_folders",
		"get_content_browser_path", "set_content_browser_path",
		"get_selected_components",
		// Component CRUD — mutates a live actor/SCS, not a fixture
		"add_component", "remove_component", "attach_component",
		"set_component_property",
		// Compile / shader / cook async state — no live build pipeline
		"get_async_compile_state", "get_shader_compile_state",
		"get_active_cook_target", "set_active_cook_target",
		"get_cook_progress", "get_ddc_state", "get_lighting_build_progress",
		// Level / world state — no loaded world in a CLI mock
		"get_current_level", "list_loaded_levels", "get_data_layer_states",
		"get_streaming_sources", "list_loaded_partition_cells",
		// Source control — no live SCM provider
		"get_source_control_provider", "get_source_control_status",
		"get_file_lock_status", "list_changelists", "get_pending_changelist",
		// Asset registry / recent-asset live state
		"get_asset_registry_state", "get_recently_opened_assets",
		"get_recently_saved_packages",
		// Save / recovery / autosave editor state
		"get_autosave_status", "get_recovery_state",
		// Editor environment introspection
		"get_active_culture", "get_editor_theme", "get_monitor_info",
		"get_live_coding_state",
		// Stats / debugging live state — needs a running session
		"get_active_stats", "get_watched_pins", "get_blueprint_breakpoints",
		"get_debug_instance",
		// Project-setting reads + writes — needs the live settings registry
		"list_project_settings", "get_project_setting_values",
		"set_project_setting", "reset_project_setting",
		// Automation / events / workspace / trace editor state
		"list_automation_tests", "get_editor_events", "get_workspace_layout",
		"get_trace_state",
		// Outliner / details / status-bar / notifications editor UI
		"get_outliner_state", "get_pinned_actors", "get_details_panel_state",
		"get_status_bar_messages", "get_active_notifications",
		// Modeling / painting tool live state
		"get_modeling_state", "get_landscape_paint_state",
		"get_foliage_paint_state", "get_mesh_paint_state",
		"get_texture_paint_state",
		// Take recorder / render queue editor state
		"get_take_recorder_state", "get_render_queue",
	};
}

std::vector<BPNode> MockBlueprintReader::FindNode(std::string_view assetPath,
												  std::string_view query,
												  std::string_view kind) {
	const auto& entry = Require(assetPath);
	std::vector<BPNode> out;
	const std::string kindLower = kind.empty() ? std::string{} : LowerAscii(kind);
	auto matchKind = [&](const BPNode& n) -> bool {
		if (kindLower.empty())
		{
			return true;
		}
		if (!n.Meta.is_object())
		{
			return false;
		}
		auto it = n.Meta.find("kind");
		if (it == n.Meta.end() || !it->is_string())
		{
			return false;
		}
		return LowerAscii(it->get<std::string>()) == kindLower;
	};
	auto match = [&](const BPNode& n) {
		if (!matchKind(n))
		{
			return false;
		}
		if (query.empty())
		{
			return true;
		}
		if (ContainsCI(n.Class, query) || ContainsCI(n.Title, query))
		{
			return true;
		}
		// Also match against meta.targetFunction / meta.variableName so
		// searches by function name find nodes whose title is operator-
		// aliased (e.g. Greater_IntInt → "integer > integer"). Issue #12.
		if (n.Meta.is_object()) {
			// Accept both camelCase (what the plugin emits) and snake_case
			// (what the mock fixtures use). Either is a valid wire shape;
			// the discrepancy predates this fix. Coverage spans the K2
			// node kinds whose underlying identifier differs from the
			// rendered title: CallFunction (function_name), VariableGet/
			// Set (variable_name), Event/CustomEvent (event_name).
			for (const char* key : {"targetFunction", "function_name",
									"variableName",   "variable_name",
									"eventName",      "event_name"}) {
				auto it = n.Meta.find(key);
				if (it != n.Meta.end() && it->is_string()) {
					if (ContainsCI(it->get<std::string>(), query))
					{
						return true;
					}
				}
			}
		}
		return false;
	};
	// find_node spans every graph in the BP, so each hit carries the
	// graph it lives in — otherwise the caller has no way to reach
	// get_node / delete_node / wire_pins on the result (issue #6).
	auto tagAndPush = [&out](const BPNode& src, std::string_view graphName,
							 std::string_view graphType) {
		BPNode copy = src;
		copy.GraphName = std::string(graphName);
		copy.GraphType = std::string(graphType);
		out.push_back(std::move(copy));
	};
	for (const auto& g : entry.graphs) {
		for (const auto& n : g.Nodes) {
			if (match(n))
			{
				tagAndPush(n, g.Name, g.Type);
			}
		}
	}
	for (const auto& f : entry.functions) {
		for (const auto& n : f.Graph.Nodes) {
			if (match(n))
			{
				tagAndPush(n, f.Graph.Name, "Function");
			}
		}
	}
	return out;
}

BPRJson MockBlueprintReader::ListTimelines(std::string_view) {
	throw BlueprintReaderError("ListTimelines not implemented in mock backend");
}
BPRJson MockBlueprintReader::ReadTimeline(std::string_view, std::string_view) {
	throw BlueprintReaderError("ReadTimeline not implemented in mock backend");
}
BPRJson MockBlueprintReader::ListAnimMontages(std::string_view) {
	throw BlueprintReaderError("ListAnimMontages not implemented in mock backend");
}
BPRJson MockBlueprintReader::ReadAnimMontage(std::string_view) {
	throw BlueprintReaderError("ReadAnimMontage not implemented in mock backend");
}


}    // namespace bpr::backends

