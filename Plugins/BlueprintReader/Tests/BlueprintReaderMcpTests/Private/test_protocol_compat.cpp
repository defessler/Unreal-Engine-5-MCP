// Backwards-compatibility test matrix.
//
// Pins the initialize / tools/list response shape across the three
// protocol versions we negotiate (2024-11-05, 2025-03-26, 2025-06-18).
// Every later phase's "additive, zero-break" claim rests on these
// tests staying green.
//
// Two kinds of pin:
//
// * Initialize: assert the negotiated protocolVersion echoes back
//   correctly for each known version + that capabilities + serverInfo
//   shape doesn't drift. Per MCP spec, the server MUST echo the
//   client's protocolVersion if it's a version the server knows;
//   that's how older clients keep working as we adopt newer specs.
//
// * tools/list: compute a deterministic hash of the (sorted) tool
//   names + per-tool description-prefix + inputSchema type. The hash
//   pins the *advertised tool inventory* — an unintended addition,
//   removal, or schema change to any tool flips it and fails the
//   test. Re-baseline by reading the failure message's new hash into
//   the kCurrentToolsHash constant in this file.
//
// Note: when a tool legitimately changes (Phase B intentionally
// edits a description, Phase D adds outputSchema), update
// kCurrentToolsHash in the SAME commit as the source change. The
// hash + the inventory are then re-baselined together.
#include <doctest/doctest.h>

#include "jsonrpc/Mcp.h"
#include "jsonrpc/Server.h"
#include "tools/BlueprintTools.h"
#include "tools/ToolRegistry.h"

#include "test_helpers.h"

#include <algorithm>
#include <cstdint>
#include <sstream>

using namespace bpr;
using nlohmann::json;

namespace test_protocol_compat_detail {

// Simple FNV-1a 64-bit hash so we don't pull in OpenSSL / Crypto++
// for what's essentially a typo-catcher. Collision probability for a
// few hundred unique tool inventories is astronomically low.
uint64_t Fnv1a64(const std::string& s) {
	uint64_t h = 14695981039346656037ULL;
	for (unsigned char c : s) {
		h ^= c;
		h *= 1099511628211ULL;
	}
	return h;
}

// Canonical, deterministic dump of the advertised tool inventory.
// Sorts tools by name, joins {name, description.substr(0,40),
// inputSchema.type} per line so a churn-y description change only
// touches the prefix. Hash this string to get the "snapshot".
std::string CanonicalToolsDump(const json& spec) {
	std::vector<std::string> rows;
	rows.reserve(spec.size());
	for (const auto& t : spec) {
		std::string name = t.value("name", "");
		std::string desc = t.value("description", "");
		if (desc.size() > 40) desc = desc.substr(0, 40);
		std::string schemaType = "object";
		if (t.contains("inputSchema") && t["inputSchema"].contains("type")) {
			schemaType = t["inputSchema"]["type"].get<std::string>();
		}
		rows.push_back(name + "|" + desc + "|" + schemaType);
	}
	std::sort(rows.begin(), rows.end());
	std::string out;
	for (const auto& r : rows) {
		out += r;
		out += '\n';
	}
	return out;
}

json RunInitialize(jsonrpc::Server& server, const std::string& version) {
	json req = {
		{"jsonrpc","2.0"}, {"id",1}, {"method","initialize"},
		{"params", json{
			{"protocolVersion", version},
			{"capabilities", json::object()},
			{"clientInfo", json{{"name","test"}, {"version","0"}}}
		}}
	};
	auto resp = server.Dispatch(req);
	REQUIRE(resp.has_value());
	return *resp;
}

}  // namespace test_protocol_compat_detail
using namespace test_protocol_compat_detail;

// ===== Initialize fixtures =================================================

TEST_CASE("Initialize: 2024-11-05 client gets 2024-11-05 echo + tool capability") {
	auto reader = test::MakeMockReader();
	tools::ToolRegistry registry;
	tools::RegisterBlueprintTools(registry, reader);

	jsonrpc::Server server;
	mcp::ServerInfo info;
	mcp::RegisterHandlers(server, registry, info);

	auto resp = RunInitialize(server, "2024-11-05");
	const auto& r = resp["result"];
	CHECK(r["protocolVersion"] == "2024-11-05");
	REQUIRE(r["capabilities"].contains("tools"));
	CHECK(r["capabilities"]["tools"]["listChanged"] == true);
	CHECK(r["serverInfo"]["name"] == "bp-reader-mcp");
}

TEST_CASE("Initialize: 2025-03-26 client gets 2025-03-26 echo (tool annotations spec)") {
	auto reader = test::MakeMockReader();
	tools::ToolRegistry registry;
	tools::RegisterBlueprintTools(registry, reader);

	jsonrpc::Server server;
	mcp::ServerInfo info;
	mcp::RegisterHandlers(server, registry, info);

	auto resp = RunInitialize(server, "2025-03-26");
	CHECK(resp["result"]["protocolVersion"] == "2025-03-26");
}

TEST_CASE("Initialize: 2025-06-18 client gets 2025-06-18 echo (current default)") {
	auto reader = test::MakeMockReader();
	tools::ToolRegistry registry;
	tools::RegisterBlueprintTools(registry, reader);

	jsonrpc::Server server;
	mcp::ServerInfo info;
	mcp::RegisterHandlers(server, registry, info);

	auto resp = RunInitialize(server, "2025-06-18");
	CHECK(resp["result"]["protocolVersion"] == "2025-06-18");
}

TEST_CASE("Initialize: unknown client version falls back to server default (no negotiation)") {
	// Pinning the negotiation rule: per spec, when the requested
	// version is unknown to the server, the server replies with its
	// preferred version. The client then decides whether to accept or
	// disconnect. We default to 2025-11-25.
	auto reader = test::MakeMockReader();
	tools::ToolRegistry registry;
	tools::RegisterBlueprintTools(registry, reader);

	jsonrpc::Server server;
	mcp::ServerInfo info;
	mcp::RegisterHandlers(server, registry, info);

	auto resp = RunInitialize(server, "2099-01-01");
	CHECK(resp["result"]["protocolVersion"] == "2025-11-25");
}

TEST_CASE("Initialize: missing protocolVersion falls back to server default") {
	auto reader = test::MakeMockReader();
	tools::ToolRegistry registry;
	tools::RegisterBlueprintTools(registry, reader);

	jsonrpc::Server server;
	mcp::ServerInfo info;
	mcp::RegisterHandlers(server, registry, info);

	json req = {
		{"jsonrpc","2.0"}, {"id",1}, {"method","initialize"},
		{"params", json::object()}  // no protocolVersion field at all
	};
	auto resp = server.Dispatch(req);
	REQUIRE(resp.has_value());
	CHECK((*resp)["result"]["protocolVersion"] == "2025-11-25");
}

// ===== tools/list inventory snapshot =======================================

TEST_CASE("tools/list inventory snapshot: hash of canonical dump") {
	// Pinned hash of the advertised tool inventory at Phase B commit.
	// Catches unintended additions, removals, or schema-type changes
	// across the full 132-tool surface. When a tool change is
	// intentional (Phase D adds outputSchema; H Tier 1 adds toolsets),
	// update this constant IN THE SAME COMMIT as the source change.
	//
	// Computed from CanonicalToolsDump(spec) above. The format is
	// "name|description-prefix-40|inputSchema.type" per line, sorted
	// by name. Changes to descriptions BEYOND the first 40 chars do
	// NOT affect the hash (intentional — we don't want every word-
	// smithing change to require a re-baseline).
	auto reader = test::MakeMockReader();
	tools::ToolRegistry registry;
	tools::RegisterBlueprintTools(registry, reader);

	const auto spec = registry.ListSpec();
	const auto dump = CanonicalToolsDump(spec);
	const auto hash = Fnv1a64(dump);

	// Phase B commit (2026-05-21): 132 tools advertised; mock backend
	// does NOT filter unsupported ones (RegisterBlueprintTools registers
	// all, but the per-backend filter in main.cpp does the prune in
	// production). In test, full inventory = 132.
	REQUIRE(spec.size() == 260);

	// The hash anchor — update on intentional inventory change.
	// First baselined 2026-05-21 (Phase B commit) at 132 tools.
	// Re-baselined 2026-05-21 at 133 tools after exposing
	// `set_pin_default` as a standalone tool (was previously only
	// reachable inside apply_ops batches).
	// Re-baselined 2026-05-21 at 138 tools after Phase 8 EA-pull
	// Wave 1 (partial): list_open_assets, get_active_asset,
	// get_compile_status, get_dirty_packages, get_focused_window.
	// Re-baselined 2026-05-21 at 141 tools after +3 more Phase 8
	// tools: get_pie_state, get_modal_state, get_active_editor_mode.
	// Re-baselined 2026-05-21 at 142 tools after +1 more Phase 8
	// tool: get_focused_widget.
	// Re-baselined 2026-05-21 at 144 tools after Phase 8 asset-editor
	// lifecycle: open_asset_editor + close_asset_editor. Also fixed the
	// wrapper-chain forwarding bug for all 11 Phase 8 tools (Auto +
	// Caching + ReadOnly now route them through to inner backends).
	// Re-baselined 2026-05-21 at 147 tools after +3 viewport tools:
	// get_camera_transform, get_view_mode, get_show_flags.
	// Re-baselined 2026-05-21 at 148 tools after +1: get_selected_components.
	// Re-baselined 2026-05-22 at 153 tools after +5 content-browser:
	// get_selected_assets, set_selected_assets, get_selected_folders,
	// get_content_browser_path, set_content_browser_path.
	// Re-baselined 2026-05-22 at 155 tools after +2 spatial:
	// world_pos_to_screen, screen_to_world.
	// Re-baselined 2026-05-22 at 158 tools after +3 Slate UI:
	// ui_snapshot, ui_find, list_desktop_windows. take_desktop_screenshot
	// composite deferred.
	// Re-baselined 2026-05-22 at 160 tools after Phase 11 starter:
	// list_game_features + get_game_feature_state (GameFeaturesToolset
	// read ops). Write ops (activate/deactivate) still deferred.
	// Re-baselined 2026-05-22 at 163 tools after +3 PluginToolset reads:
	// list_plugins, get_plugin_descriptor, get_plugin_dependencies.
	// Re-baselined 2026-05-22 at 165 tools after +2 GAS introspection:
	// list_actor_abilities, list_actor_gameplay_tags.
	// Re-baselined 2026-05-22 at 167 tools after +2 GAS:
	// list_actor_attributes, list_actor_gameplay_effects.
	// Re-baselined 2026-05-22 at 168 tools after Phase 12 Wave 2 starter:
	// get_blueprint_editor_state.
	// Re-baselined 2026-05-22 at 169 tools after +1 Wave 2:
	// get_material_instance_params.
	// Re-baselined 2026-05-22 at 170 tools after +1 Wave 2:
	// get_static_mesh_info.
	// Re-baselined 2026-05-22 at 171 tools after +1 Wave 2:
	// get_umg_editor_state.
	// Re-baselined 2026-05-22 at 172 tools after +1 Wave 2:
	// get_material_editor_state.
	// Re-baselined 2026-05-22 at 173 tools after +1 Wave 2:
	// get_mesh_preview_state.
	// Re-baselined 2026-05-22 at 174 tools after +1 Wave 2:
	// get_cinematic_camera.
	// Re-baselined 2026-05-22 at 175 tools after +1 Wave 2:
	// get_sequencer_state.
	// Re-baselined 2026-05-22 at 176 tools after +1 Wave 2:
	// get_anim_editor_state (stub — Persona toolkit cross-cast requires
	// per-editor-class registry without RTTI; documented + valid:false).
	// Re-baselined 2026-05-22 at 178 tools after +2 Wave 2 stubs:
	// get_niagara_module_selection, get_curve_editor_selection.
	// Re-baselined 2026-05-22 at 182 tools after Phase 13 Wave 3 batch 1:
	// get_buffer_visualization_mode, get_gizmo_state, get_viewport_realtime,
	// get_viewport_camera_settings.
	// Re-baselined 2026-05-22 at 184 tools after Phase 13 Wave 3 batch 2:
	// get_snapping_settings, get_active_viewport.
	// Re-baselined 2026-05-22 at 185 tools after +1 Wave 3:
	// get_hidden_actors.
	// Re-baselined 2026-05-25 at 226 tools after +1 Phase 10:
	// get_editor_events (EA-push event-source capture).
	// Re-baselined 2026-05-25 at 227 tools after +1 Phase 14:
	// get_active_cook_target (ITargetPlatformManagerModule).
	// Re-baselined 2026-05-25 at 228 tools after +1 Phase 17:
	// get_workspace_layout (FGlobalTabmanager::PersistLayout).
	// Re-baselined 2026-05-25 at 229 tools after +1 Phase 17:
	// get_trace_state (FTraceAuxiliary).
	// Re-baselined 2026-05-25 at 234 tools after +5 Phase 14/17 UI-state
	// v1 stubs (outliner/pinned/details-panel/status-bar/notifications).
	// Re-baselined 2026-05-25 at 239 tools after +5 Phase 17 editor-mode
	// v1 stubs (modeling + landscape/foliage/mesh/texture paint).
	// Re-baselined 2026-05-25 at 248 tools after +9 Phase 14/17 tail v1
	// stubs (cook/ddc/lighting/set-cook/partition-cells/changelists/
	// pending-changelist/take-recorder/render-queue).
	// Re-baselined 2026-05-25 at 249 tools after +1 Phase 16 reset
	// (reset_project_setting v1 stub).
	// Re-baselined 2026-05-31 at 251 tools after +2 write tools:
	// clone_graph + implement_interface.
	// Re-baselined 2026-06-01 at 252 tools after +1 read tool:
	// read_actor_instance (OFPA / arbitrary-UObject reader).
	// Re-baselined 2026-06-04 at 258 tools after MCP-7 description quality pass:
	// get_graph, get_function, find_node descriptions improved with activation criteria.
	// Re-baselined 2026-06-05 at 260 tools after +2 diff tools:
	// diff_asset + prepare_merge.
	constexpr uint64_t kCurrentToolsHash = 0x54FDD365ECA92207ULL;

	if (hash != kCurrentToolsHash) {
		// Re-baseline aid: when the inventory legitimately changes, the
		// test fails with a clear "this is the new hash" message rather
		// than just "values differ." Paste the printed hex into
		// kCurrentToolsHash IN THE SAME COMMIT as the source change.
		MESSAGE("tools/list inventory hash drifted. New hash: 0x"
				<< std::hex << hash
				<< " (was 0x" << std::hex << kCurrentToolsHash << ")");
	}
	CHECK(hash == kCurrentToolsHash);
}

TEST_CASE("tools/list shape: every entry has name + description + inputSchema") {
	// Spec-conformance assert. tools/list responses MUST carry
	// these three fields on every tool; missing any one breaks
	// clients that walk the array generically.
	auto reader = test::MakeMockReader();
	tools::ToolRegistry registry;
	tools::RegisterBlueprintTools(registry, reader);
	const auto spec = registry.ListSpec();
	REQUIRE(spec.size() > 0);
	for (const auto& t : spec) {
		CAPTURE(t.value("name", "?"));
		CHECK(t.contains("name"));
		CHECK(t.contains("description"));
		CHECK(t.contains("inputSchema"));
		CHECK(t["inputSchema"].contains("type"));
	}
}

TEST_CASE("tools/list inventory sanity: no duplicate tool names") {
	// Two tools with the same name collide in dispatch — the second
	// silently overrides the first. ToolRegistry::Add deduplicates
	// already, but the snapshot is the right place to verify the
	// invariant holds after every change.
	auto reader = test::MakeMockReader();
	tools::ToolRegistry registry;
	tools::RegisterBlueprintTools(registry, reader);
	const auto spec = registry.ListSpec();
	std::vector<std::string> names;
	for (const auto& t : spec) {
		names.push_back(t["name"].get<std::string>());
	}
	std::sort(names.begin(), names.end());
	for (size_t i = 1; i < names.size(); ++i) {
		CAPTURE(names[i]);
		CHECK(names[i] != names[i-1]);
	}
}
