#include "tools/BlueprintTools.h"
#include "tools/ApplyOps.h"
#include "tools/Bpir.h"
#include "tools/codegen/CppClassEmit.h"
#include "tools/codegen/CppEmit.h"
#include "tools/codegen/UnsupportedTreatment.h"
#include "tools/CompileFunction.h"
#include "tools/ContentBlocks.h"
#include "tools/Cursor.h"
#include "tools/Decompile.h"
#include "tools/ImageReader.h"
#include "tools/JsonProjection.h"
#include "tools/parse/CppParse.h"
#include "tools/TypeShorthand.h"

#include "Env.h"
#include "backends/IBlueprintReader.h"

#include <filesystem>
#include <fstream>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <fmt/core.h>
#include <nlohmann/json.hpp>
#include "tools/BlueprintToolsDetail.h"

namespace bpr::tools {
using namespace blueprint_tools_detail;

void RegisterTools_03(ToolRegistry& registry, backends::IBlueprintReader& reader) {
	// ----- get_active_asset-----------------------------------------------
	{
		ToolDescriptor d;
		d.name = "get_active_asset";
		d.description =
			"[editor] Asset whose editor was most recently activated — the one the "
			"user is most likely focused on. Returns "
			"`{asset_path, asset_class, last_activation_seconds}`. "
			"`asset_path` is empty when no asset editor is open. "
			"Requires a live editor.";
		d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
		d.output_schema = {
			{"type", "object"},
			{"properties", {
				{"asset_path",                 {{"type", "string"}}},
				{"asset_class",                {{"type", "string"}}},
				{"last_activation_seconds",    {{"type", "number"}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json&) {
			auto r = reader.GetActiveAsset();
			return nlohmann::json{
				{"asset_path",                 r.assetPath},
				{"asset_class",                r.assetClass},
				{"last_activation_seconds",    r.lastActivationSeconds},
			};
		});
	}

	// ----- get_compile_status ---------------------------------------------
	{
		ToolDescriptor d;
		d.name = "get_compile_status";
		d.description =
			"[editor] Blueprint compile status — wraps `UBlueprint::Status` into a "
			"stable string. Useful after a write op to confirm the BP is "
			"compile-clean before further mutations. Values: \"uncompiled\", "
			"\"dirty\", \"good\", \"warning\", \"error\", \"compiling\", "
			"\"unknown\". Requires a live editor.";
		d.input_schema = {
			{"type","object"},
			{"properties", {{"asset_path", {{"type","string"}}}}},
			{"required", nlohmann::json::array({"asset_path"})},
		};
		d.output_schema = {
			{"type", "object"},
			{"properties", {
				{"asset_path",         {{"type","string"}}},
				{"status",             {{"type","string"}}},
				{"last_compile_error", {{"type","string"}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			const std::string asset = RequireAssetPath(args);
			auto r = reader.GetCompileStatus(asset);
			nlohmann::json body = {
				{"asset_path", r.assetPath},
				{"status",     r.status},
			};
			if (!r.lastCompileError.empty()) {
				body["last_compile_error"] = r.lastCompileError;
			}
			return body;
		});
	}

	// ----- get_dirty_packages ----------------------------------------------
	{
		ToolDescriptor d;
		d.name = "get_dirty_packages";
		d.description =
			"[editor] List loaded packages with unsaved changes. Pairs with "
			"`save_all` — agents that just mutated multiple BPs use this to "
			"confirm there's nothing unsaved before disconnecting. Each entry "
			"flags `is_content` so the agent can ignore editor-only / engine "
			"packages. Requires a live editor.";
		d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
		d.output_schema = {
			{"type", "array"},
			{"items", {
				{"type", "object"},
				{"properties", {
					{"package_name", {{"type","string"}}},
					{"is_content",   {{"type","boolean"}}},
				}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			auto ctl = ParseResponseControls(args);
			auto r = reader.GetDirtyPackages();
			nlohmann::json body = nlohmann::json::array();
			for (const auto& p : r.packages) {
				body.push_back({
					{"package_name", p.packageName},
					{"is_content",   p.isContentPackage},
				});
			}
			return ListResponse(std::move(body), ctl);
		});
	}

	// ----- get_focused_window ----------------------------------------------
	{
		ToolDescriptor d;
		d.name = "get_focused_window";
		d.description =
			"[editor] Title + Slate widget-class of the currently-focused top-level "
			"window. Useful for \"is the user in the BP editor right now?\" "
			"routing. Returns `{title, class_name}`. Both empty when no Slate "
			"window has focus. Requires a live editor.";
		d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
		d.output_schema = {
			{"type", "object"},
			{"properties", {
				{"title",      {{"type", "string"}}},
				{"class_name", {{"type", "string"}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json&) {
			auto r = reader.GetFocusedWindow();
			return nlohmann::json{
				{"title",      r.title},
				{"class_name", r.className},
			};
		});
	}

	// ----- get_pie_state ---------------------------------------------------
	{
		ToolDescriptor d;
		d.name = "get_pie_state";
		d.description =
			"[editor] Is Play-In-Editor running right now? Returns "
			"`{is_playing, mode, instance_count}`. `mode` is the PIE mode "
			"string (selected_viewport / new_editor_window / standalone / "
			"vr_preview) or empty when not playing. instance_count >= 2 for "
			"multi-PIE (Client/Server split). Requires a live editor.";
		d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
		d.output_schema = {
			{"type", "object"},
			{"properties", {
				{"is_playing",     {{"type", "boolean"}}},
				{"mode",           {{"type", "string"}}},
				{"instance_count", {{"type", "integer"}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json&) {
			auto r = reader.GetPieState();
			return nlohmann::json{
				{"is_playing",     r.isPlaying},
				{"mode",           r.mode},
				{"instance_count", r.instanceCount},
			};
		});
	}

	// ----- get_modal_state -------------------------------------------------
	{
		ToolDescriptor d;
		d.name = "get_modal_state";
		d.description =
			"[editor] Is a modal Slate window blocking input right now? "
			"Returns `{is_open, title, buttons, buttons_truncated}` — `buttons` "
			"lists the modal's SButtons as `[{path, label?}]` so you can see what "
			"answers the dialog offers (label comes from the button's text "
			"block; `buttons_truncated` is true in the rare case the modal walk "
			"hit its budget). Common modals: confirm-deletion, asset-picker, "
			"save-as. Agents should refuse mutation ops when is_open=true — the "
			"editor is gated on user input. Requires a live editor.";
		d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
		d.output_schema = {
			{"type", "object"},
			{"properties", {
				{"is_open", {{"type", "boolean"}}},
				{"title",   {{"type", "string"}}},
				{"buttons", {{"type", "array"},
							 {"description", "The modal's buttons: [{path, label?}]. Empty when no modal."}}},
				{"buttons_truncated", {{"type", "boolean"},
							 {"description", "True when the button list may be incomplete (modal walk hit its budget)."}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json&) {
			auto r = reader.GetModalState();
			return nlohmann::json{
				{"is_open", r.isOpen},
				{"title",   r.title},
				{"buttons", r.buttons},
				{"buttons_truncated", r.buttonsTruncated},
			};
		});
	}

	// ----- ui_list_widgets ---------------------------------------------------
	// TEST-2 P0: read-only Slate widget-tree inspection — the editor-UI analog
	// of a DOM dump. Foundation for the gated UI-interaction tools (P1b) and
	// AutomationDriver By::Path locators (P2).
	{
		ToolDescriptor d;
		d.name = "ui_list_widgets";
		d.description =
			"[editor] Walk the live editor's Slate widget tree (Selenium-style "
			"UI inspection). Returns `{ui_available, windows: [{title, is_modal, "
			"truncated, widgets: [{path, type, tag?, text?, visible, enabled, x, "
			"y, w, h}]}], truncated}`. `path` is a child-index:Type selector (e.g. "
			"`0:SWindow/3:SButton`); it is RESPONSE-LOCAL — the leading window "
			"index is ordinal in z-order and shifts as windows/menus/modals come "
			"and go, so re-query rather than caching it across calls. `text` is "
			"filled for STextBlock — button/label captions live on descendant "
			"text blocks. Filter with `window` (title substring) and `type` "
			"(widget-type substring, e.g. SButton). `max_widgets` is a single "
			"global budget consumed window-by-window in z-order, so to inspect a "
			"specific window pass `window=` to focus the budget on it. "
			"`truncated` (top-level and per-window) is true when a depth or "
			"budget cap dropped widgets. `ui_available` is false on a headless "
			"daemon (no Slate) — branch on that bool, not on the `note` string.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"max_depth",   {{"type","integer"},
								 {"description","Max tree depth to descend (default 25, clamp 1-100)."}}},
				{"max_widgets", {{"type","integer"},
								 {"description","Max widgets EMITTED across all windows (default 800, clamp 1-10000). Traversal cost is bounded separately (derived from this)."}}},
				{"window",      {{"type","string"},
								 {"description","Only walk windows whose title contains this substring. Use to focus the shared max_widgets budget on one window."}}},
				{"type",        {{"type","string"},
								 {"description","Only emit widgets whose Slate type contains this substring (e.g. SButton). The walk still descends through non-matching widgets (bounded by an internal visit cap)."}}},
			}},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",           {{"type","boolean"}}},
				{"ui_available", {{"type","boolean"},
								  {"description","False on a headless daemon (no Slate); true in a GUI/-RenderOffscreen editor."}}},
				{"windows",      {{"type","array"},
								  {"description","[{title, is_modal, truncated, widgets: [{path, type, tag?, text?, visible, enabled, x, y, w, h}]}]"}}},
				{"truncated",    {{"type","boolean"}}},
				{"note",         {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"ok"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			const int maxDepth   = args.value("max_depth", 25);
			const int maxWidgets = args.value("max_widgets", 800);
			const std::string window = args.value("window", std::string{});
			const std::string type   = args.value("type",   std::string{});
			return reader.UiListWidgets(maxDepth, maxWidgets, window, type);
		});
	}

	// ----- ui_click ----------------------------------------------------------
	// TEST-2 P1b: click an editor widget located by its ui_list_widgets `path`.
	// An ACTION (injects synthetic input), gated editor-side by
	// BP_READER_ALLOW_UI=1 — off by default. Foundation for ui_type / menu
	// invocation (later P1b slices).
	{
		ToolDescriptor d;
		d.name = "ui_click";
		d.description =
			"[editor] Click an editor widget located by its `widget_path` (from "
			"`ui_list_widgets`). Injects a real synthetic mouse down+up at the "
			"widget's geometry center via the same path OS input takes, so Slate "
			"hit-tests and routes it normally (e.g. clicking a toolbar combo opens "
			"its dropdown). GATED: returns an error unless the editor was started "
			"with BP_READER_ALLOW_UI=1 (off by default — it drives the live UI). "
			"`widget_path` is RESPONSE-LOCAL (re-run ui_list_widgets first); pass "
			"`expect_type` / `expect_text` to revalidate the target before clicking "
			"(a shifted tree errors instead of clicking the wrong widget). Widgets "
			"with zero rendered geometry (collapsed/hidden/overflow) are refused. "
			"Returns `{ok, clicked, widget_type, x, y}` (the injected screen point) "
			"or `{ok:false, error}`. Requires a GUI / -RenderOffscreen editor.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"widget_path", {{"type","string"},
								 {"description","The target widget's path from ui_list_widgets (e.g. 0:SWindow/.../1:SButton)."}}},
				{"expect_type", {{"type","string"},
								 {"description","Optional: the widget type must CONTAIN this (e.g. SButton) or the click is refused (path-shift guard)."}}},
				{"expect_text", {{"type","string"},
								 {"description","Optional: reserved for text revalidation of the target."}}},
			}},
			{"required", nlohmann::json::array({"widget_path"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",          {{"type","boolean"}}},
				{"clicked",     {{"type","boolean"}}},
				{"widget_type", {{"type","string"}}},
				{"x",           {{"type","number"}}},
				{"y",           {{"type","number"}}},
				{"error",       {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"ok"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			const std::string widgetPath = RequireString(args, "widget_path");
			if (widgetPath.empty()) {
				throw std::invalid_argument("ui_click: `widget_path` must be non-empty (from ui_list_widgets).");
			}
			const std::string expectType = OptString(args, "expect_type", "");
			const std::string expectText = OptString(args, "expect_text", "");
			return reader.UiClick(widgetPath, expectType, expectText);
		});
	}

	// ----- ui_type -----------------------------------------------------------
	// TEST-2 P1b slice 2: type text into an editor widget located by its
	// ui_list_widgets `path`. Sibling of ui_click — an ACTION (injects
	// synthetic key-char events), gated editor-side by BP_READER_ALLOW_UI=1.
	{
		ToolDescriptor d;
		d.name = "ui_type";
		d.description =
			"[editor] Type `text` into an editor widget located by its `widget_path` "
			"(from `ui_list_widgets`). Sets keyboard focus to the target, then injects "
			"one real synthetic character event per char via the same path OS input "
			"takes, so Slate routes it normally (e.g. typing into a search box filters "
			"its list). GATED: returns an error unless the editor was started with "
			"BP_READER_ALLOW_UI=1 (off by default — it drives the live UI). "
			"`widget_path` is RESPONSE-LOCAL (re-run ui_list_widgets first); pass "
			"`expect_type` to revalidate the target is still an editable-text widget "
			"before typing (a shifted tree errors instead of typing into the wrong "
			"widget). Returns `{ok, typed, widget_type, char_count}` or "
			"`{ok:false, error}`. Requires a GUI / -RenderOffscreen editor.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"widget_path", {{"type","string"},
								 {"description","The target widget's path from ui_list_widgets (e.g. 0:SWindow/.../1:SEditableTextBox)."}}},
				{"text",        {{"type","string"},
								 {"description","The text to type, one character event per char."}}},
				{"expect_type", {{"type","string"},
								 {"description","Optional: the widget type must CONTAIN this (e.g. SEditableText) or the type is refused (path-shift guard)."}}},
			}},
			{"required", nlohmann::json::array({"widget_path","text"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",          {{"type","boolean"}}},
				{"typed",       {{"type","boolean"}}},
				{"widget_type", {{"type","string"}}},
				{"char_count",  {{"type","integer"}}},
				{"error",       {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"ok"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			const std::string widgetPath = RequireString(args, "widget_path");
			if (widgetPath.empty()) {
				throw std::invalid_argument("ui_type: `widget_path` must be non-empty (from ui_list_widgets).");
			}
			const std::string text = RequireString(args, "text");
			const std::string expectType = OptString(args, "expect_type", "");
			return reader.UiType(widgetPath, text, expectType);
		});
	}

	// ----- get_focused_widget ---------------------------------------------
	{
		ToolDescriptor d;
		d.name = "get_focused_widget";
		d.description =
			"[editor] Slate widget the user is currently typing into / focused on. "
			"Finer-grained than get_focused_window: \"the user is in the BP "
			"graph search box\" vs \"the user is on the BP editor window\". "
			"Returns `{widget_type, parent_window_title}`. Both empty when "
			"no widget has focus. Requires a live editor.";
		d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
		d.output_schema = {
			{"type", "object"},
			{"properties", {
				{"widget_type",         {{"type", "string"}}},
				{"parent_window_title", {{"type", "string"}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json&) {
			auto r = reader.GetFocusedWidget();
			return nlohmann::json{
				{"widget_type",         r.widgetType},
				{"parent_window_title", r.parentWindowTitle},
			};
		});
	}

	// ----- open_asset_editor ---------------------------------------------
	{
		ToolDescriptor d;
		d.name = "open_asset_editor";
		d.description =
			"[editor] Open the asset editor for the given asset (Blueprint, "
			"Material, etc.). Idempotent — opening an already-open asset "
			"brings the existing editor window to front. `asset_path` is "
			"package form: `/Game/AI/BP_Foo`. Returns `{opened, asset_path}`. "
			"`opened` is false when the asset couldn't be loaded. Requires a "
			"live editor.";
		d.input_schema = {
			{"type", "object"},
			{"properties", {
				{"asset_path", {{"type", "string"}}},
			}},
			{"required", nlohmann::json::array({"asset_path"})},
		};
		d.output_schema = {
			{"type", "object"},
			{"properties", {
				{"opened",     {{"type", "boolean"}}},
				{"asset_path", {{"type", "string"}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string assetPath = RequireAssetPath(args);
			auto r = reader.OpenAssetEditor(assetPath);
			return nlohmann::json{
				{"ok",         true},
				{"opened",     r.opened},
				{"asset_path", r.assetPath},
			};
		});
	}

	// ----- close_asset_editor --------------------------------------------
	{
		ToolDescriptor d;
		d.name = "close_asset_editor";
		d.description =
			"[editor] Close all editor windows for the given asset. "
			"Idempotent — closing an asset with no editor open is a no-op "
			"(`closed:false`, no error). `asset_path` is package form. "
			"Returns `{closed, asset_path}`. Requires a live editor.";
		d.input_schema = {
			{"type", "object"},
			{"properties", {
				{"asset_path", {{"type", "string"}}},
			}},
			{"required", nlohmann::json::array({"asset_path"})},
		};
		d.output_schema = {
			{"type", "object"},
			{"properties", {
				{"closed",     {{"type", "boolean"}}},
				{"asset_path", {{"type", "string"}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string assetPath = RequireAssetPath(args);
			auto r = reader.CloseAssetEditor(assetPath);
			return nlohmann::json{
				{"ok",         true},
				{"closed",     r.closed},
				{"asset_path", r.assetPath},
			};
		});
	}

	// ----- get_anim_editor_state (Phase 12 Wave 2) --------------------
	{
		ToolDescriptor d;
		d.name = "get_anim_editor_state";
		d.description =
			"[editor] Persona / animation editor selection state. v1 is a "
			"documented stub returning `valid:false` for all paths — "
			"surfacing the wire shape `{valid, asset_path, selected_bone_index, "
			"selected_socket_name}` so callers can see the planned contract. "
			"Implementation deferred: Persona editors use multi-inheritance "
			"(FPersonaAssetEditorToolkit + IHasPersonaToolkit) that UE's "
			"no-RTTI environment can't safely cross-cast. Future fix "
			"requires a sidecar registry hooked into "
			"UAssetEditorSubsystem::OnAssetOpenedInEditor.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path", {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"asset_path"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"valid",                 {{"type","boolean"}}},
				{"asset_path",            {{"type","string"}}},
				{"selected_bone_index",   {{"type","integer"}}},
				{"selected_socket_name",  {{"type","string"}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireAssetPath(args);
			auto r = reader.GetAnimEditorState(asset);
			return nlohmann::json{
				{"valid",                r.valid},
				{"asset_path",           r.assetPath},
				{"selected_bone_index",  r.selectedBoneIndex},
				{"selected_socket_name", r.selectedSocketName},
			};
		});
	}

	// ----- get_visible_actors (Phase 13 Wave 3) -----------------------
	{
		ToolDescriptor d;
		d.name = "get_visible_actors";
		d.description =
			"[editor] Actors visible in the active level viewport — "
			"frustum-tested against the camera, filtered by class "
			"substring (`class_filter`, case-sensitive) and max distance "
			"(`max_distance` in cm; 0 = no limit). Skips hidden actors. "
			"Per-actor `{name, label, actor_class, world_x/y/z, "
			"distance_cm, screen_x/y, has_screen_pos}`. `screen_x/y` are "
			"normalized [0,1]; `has_screen_pos:false` means the actor "
			"is in frustum but projects behind the camera plane. Capped "
			"at 500 actors. Requires a live editor.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"class_filter", {{"type","string"}}},
				{"max_distance", {{"type","number"}}},
			}},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"actors",    {{"type","array"}, {"items", {{"type","object"}}}}},
				{"truncated", {{"type","boolean"}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string cls = args.value("class_filter", std::string{});
			double dist     = args.value("max_distance", 0.0);
			auto r = reader.GetVisibleActors(cls, dist);
			nlohmann::json actors = nlohmann::json::array();
			for (const auto& a : r.actors) {
				actors.push_back({
					{"name",           a.name},
					{"label",          a.label},
					{"actor_class",    a.actorClass},
					{"world_x",        a.worldX},
					{"world_y",        a.worldY},
					{"world_z",        a.worldZ},
					{"distance_cm",    a.distanceCm},
					{"screen_x",       a.screenX},
					{"screen_y",       a.screenY},
					{"has_screen_pos", a.hasScreenPos},
				});
			}
			return nlohmann::json{
				{"actors",    actors},
				{"truncated", r.truncated},
			};
		});
	}

	// ----- get_hidden_layers (Phase 13 Wave 3) ------------------------
	{
		ToolDescriptor d;
		d.name = "get_hidden_layers";
		d.description =
			"[editor] Names of editor layers whose visibility is off "
			"(`ULayersSubsystem`). Capped at 500 — `truncated:true` signals "
			"a larger set. Pair with `set_layer_visibility` to toggle. "
			"Requires a live editor.";
		d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"layer_names", {{"type","array"}, {"items", {{"type","string"}}}}},
				{"truncated",   {{"type","boolean"}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json&) {
			auto r = reader.GetHiddenLayers();
			return nlohmann::json{
				{"layer_names", r.layerNames},
				{"truncated",   r.truncated},
			};
		});
	}

	// ----- set_layer_visibility (Phase 13 Wave 3) ---------------------
	{
		ToolDescriptor d;
		d.name = "set_layer_visibility";
		d.description =
			"[editor] Show or hide an editor layer by name "
			"(`ULayersSubsystem`). `valid:false` means no layer with that "
			"name exists. Blocked in read-only mode (mutates level-domain "
			"state). Requires a live editor.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"layer",   {{"type","string"}}},
				{"visible", {{"type","boolean"}}},
			}},
			{"required", nlohmann::json::array({"layer","visible"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",      {{"type","boolean"}}},
				{"valid",   {{"type","boolean"}}},
				{"layer",   {{"type","string"}}},
				{"visible", {{"type","boolean"}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string layer = RequireString(args, "layer");
			bool visible = args.value("visible", true);
			auto r = reader.SetLayerVisibility(layer, visible);
			return nlohmann::json{
				{"ok",      true},
				{"valid",   r.valid},
				{"layer",   r.layer},
				{"visible", r.visible},
			};
		});
	}

	// ----- set_actor_visibility (Phase 13 Wave 3) ---------------------
	{
		ToolDescriptor d;
		d.name = "set_actor_visibility";
		d.description =
			"[editor] Show or hide an actor in the editor viewport by name "
			"via `SetIsTemporarilyHiddenInEditor` (does not dirty the "
			"package). `valid:false` means no actor with that name was "
			"found. Blocked in read-only mode. Pairs with "
			"`get_hidden_actors`. Requires a live editor.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"name",    {{"type","string"}}},
				{"visible", {{"type","boolean"}}},
			}},
			{"required", nlohmann::json::array({"name","visible"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",      {{"type","boolean"}}},
				{"valid",   {{"type","boolean"}}},
				{"name",    {{"type","string"}}},
				{"visible", {{"type","boolean"}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string name = RequireString(args, "name");
			bool visible = args.value("visible", true);
			auto r = reader.SetActorVisibility(name, visible);
			return nlohmann::json{
				{"ok",      true},
				{"valid",   r.valid},
				{"name",    r.name},
				{"visible", r.visible},
			};
		});
	}

}

void RegisterTools_03b(ToolRegistry& registry, backends::IBlueprintReader& reader) {
	// ----- set_view_mode (Phase 13 Wave 3) ----------------------------
	{
		ToolDescriptor d;
		d.name = "set_view_mode";
		d.description =
			"[editor] Set the active level viewport's view mode. Accepts "
			"`Lit`, `Unlit`, `Wireframe`, `BrushWireframe`, `LightingOnly`, "
			"`LightComplexity`, `ShaderComplexity`, `StationaryLightOverlap`, "
			"`LightmapDensity`, `LitLightmapDensity`, `ReflectionOverride`, "
			"`CollisionPawn`, `CollisionVisibility` (case-insensitive). "
			"`valid:false` means no viewport or an unrecognized mode. Read "
			"counterpart: `get_view_mode`. Requires a live editor.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"mode", {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"mode"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",    {{"type","boolean"}}},
				{"valid", {{"type","boolean"}}},
				{"mode",  {{"type","string"}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string mode = RequireString(args, "mode");
			auto r = reader.SetViewMode(mode);
			return nlohmann::json{
				{"ok",    true},
				{"valid", r.valid},
				{"mode",  r.mode},
			};
		});
	}

	// ----- set_gizmo_mode (Phase 13 Wave 3) ---------------------------
	{
		ToolDescriptor d;
		d.name = "set_gizmo_mode";
		d.description =
			"[editor] Set the transform gizmo mode in the active level "
			"viewport: `translate`, `rotate`, or `scale` (case-insensitive). "
			"`valid:false` means no viewport or an unrecognized mode. Read "
			"counterpart: `get_gizmo_state`. Requires a live editor.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"mode", {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"mode"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",    {{"type","boolean"}}},
				{"valid", {{"type","boolean"}}},
				{"mode",  {{"type","string"}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string mode = RequireString(args, "mode");
			auto r = reader.SetGizmoMode(mode);
			return nlohmann::json{
				{"ok",    true},
				{"valid", r.valid},
				{"mode",  r.mode},
			};
		});
	}

	// ----- set_viewport_realtime (Phase 13 Wave 3) --------------------
	{
		ToolDescriptor d;
		d.name = "set_viewport_realtime";
		d.description =
			"[editor] Enable or disable realtime rendering in the active "
			"level viewport. `valid:false` means no viewport; `is_realtime` "
			"echoes the resulting state. Read counterpart: "
			"`get_viewport_realtime`. Requires a live editor.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"enabled", {{"type","boolean"}}},
			}},
			{"required", nlohmann::json::array({"enabled"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",          {{"type","boolean"}}},
				{"valid",       {{"type","boolean"}}},
				{"is_realtime", {{"type","boolean"}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			bool enabled = args.value("enabled", true);
			auto r = reader.SetViewportRealtime(enabled);
			return nlohmann::json{
				{"ok",          true},
				{"valid",       r.valid},
				{"is_realtime", r.isRealtime},
			};
		});
	}

	// ----- get_camera_bookmarks (Phase 13 Wave 3) ---------------------
	{
		ToolDescriptor d;
		d.name = "get_camera_bookmarks";
		d.description =
			"[editor] Saved viewport camera bookmarks (Ctrl-1..9 poses) from "
			"the world settings. Returns only populated slots: `{slot, "
			"loc_x/y/z, pitch, yaw, roll}`. `max_slots` is the allocated "
			"bookmark array size. Pair with `goto_camera_bookmark`. Requires "
			"a live editor.";
		d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"bookmarks", {{"type","array"}, {"items", {{"type","object"}}}}},
				{"max_slots", {{"type","integer"}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json&) {
			auto r = reader.GetCameraBookmarks();
			nlohmann::json bms = nlohmann::json::array();
			for (const auto& b : r.bookmarks) {
				bms.push_back({
					{"slot",  b.slot},
					{"loc_x", b.locX}, {"loc_y", b.locY}, {"loc_z", b.locZ},
					{"pitch", b.pitch}, {"yaw", b.yaw}, {"roll", b.roll},
				});
			}
			return nlohmann::json{
				{"bookmarks", bms},
				{"max_slots", r.maxSlots},
			};
		});
	}

	// ----- goto_camera_bookmark (Phase 13 Wave 3) ---------------------
	{
		ToolDescriptor d;
		d.name = "goto_camera_bookmark";
		d.description =
			"[editor] Jump the active viewport camera to a saved bookmark "
			"slot (0-based). `jumped:false` means the slot is empty or no "
			"viewport is focused. View-state only — allowed in read-only "
			"mode. Requires a live editor.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"slot", {{"type","integer"}}},
			}},
			{"required", nlohmann::json::array({"slot"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",     {{"type","boolean"}}},
				{"jumped", {{"type","boolean"}}},
				{"slot",   {{"type","integer"}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			int slot = args.value("slot", 0);
			auto r = reader.GotoCameraBookmark(slot);
			return nlohmann::json{
				{"ok",     true},
				{"jumped", r.jumped},
				{"slot",   r.slot},
			};
		});
	}

	// ----- get_hover_target (Phase 13 Wave 3 — v1 stub) ---------------
	{
		ToolDescriptor d;
		d.name = "get_hover_target";
		d.description =
			"[editor] Hit-proxy target under the cursor (actor/surface/"
			"component). v1 stub: resolving the hit proxy needs a render-"
			"thread readback + RTTI cross-cast that isn't safe out-of-"
			"process — returns `valid:false` until an editor-module sidecar "
			"captures hover state. Shape is stable.";
		d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"valid",          {{"type","boolean"}}},
				{"hit_proxy_type", {{"type","string"}}},
				{"actor_name",     {{"type","string"}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json&) {
			auto r = reader.GetHoverTarget();
			return nlohmann::json{
				{"valid",          r.valid},
				{"hit_proxy_type", r.hitProxyType},
				{"actor_name",     r.actorName},
			};
		});
	}

	// ----- get_isolate_mode (Phase 13 Wave 3 — v1 stub) ---------------
	{
		ToolDescriptor d;
		d.name = "get_isolate_mode";
		d.description =
			"[editor] Show-only-selected / isolate mode state (UE 5.6+). "
			"v1 stub: no stable public accessor for the level-viewport "
			"isolate flag — returns `valid:false`. Shape is stable.";
		d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"valid",    {{"type","boolean"}}},
				{"isolated", {{"type","boolean"}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json&) {
			auto r = reader.GetIsolateMode();
			return nlohmann::json{
				{"valid",    r.valid},
				{"isolated", r.isolated},
			};
		});
	}

	// ----- get_hidden_actors (Phase 13 Wave 3) ------------------------
	{
		ToolDescriptor d;
		d.name = "get_hidden_actors";
		d.description =
			"[editor] Names of actors hidden in the editor viewport "
			"(temporary or level hide). Capped at 500 — `truncated:true` "
			"signals a larger hidden set. Sorted by iteration order, not "
			"by name. Pair with `set_actor_visibility` to toggle. "
			"Requires a live editor.";
		d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"actor_names", {{"type","array"}, {"items", {{"type","string"}}}}},
				{"truncated",   {{"type","boolean"}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json&) {
			auto r = reader.GetHiddenActors();
			return nlohmann::json{
				{"actor_names", r.actorNames},
				{"truncated",   r.truncated},
			};
		});
	}

	// ----- get_snapping_settings (Phase 13 Wave 3) --------------------
	{
		ToolDescriptor d;
		d.name = "get_snapping_settings";
		d.description =
			"[editor] Editor snapping settings: grid + rotation snap "
			"toggles, current grid sizes (position + rotation), vertex "
			"snapping, snap-to-actor distance. Reads "
			"`ULevelEditorViewportSettings`. Requires a live editor.";
		d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"valid",                 {{"type","boolean"}}},
				{"grid_enabled",          {{"type","boolean"}}},
				{"rot_grid_enabled",      {{"type","boolean"}}},
				{"snap_vertices",         {{"type","boolean"}}},
				{"current_pos_grid_size", {{"type","integer"}}},
				{"current_rot_grid_size", {{"type","integer"}}},
				{"actor_snap_distance",   {{"type","number"}}},
				{"snap_distance",         {{"type","number"}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json&) {
			auto r = reader.GetSnappingSettings();
			return nlohmann::json{
				{"valid",                 r.valid},
				{"grid_enabled",          r.gridEnabled},
				{"rot_grid_enabled",      r.rotGridEnabled},
				{"snap_vertices",         r.snapVertices},
				{"current_pos_grid_size", r.currentPosGridSize},
				{"current_rot_grid_size", r.currentRotGridSize},
				{"actor_snap_distance",   r.actorSnapDistance},
				{"snap_distance",         r.snapDistance},
			};
		});
	}

	// ----- get_active_viewport (Phase 13 Wave 3) ----------------------
	{
		ToolDescriptor d;
		d.name = "get_active_viewport";
		d.description =
			"[editor] Which level viewport has focus right now (1/2/4 "
			"layout). Returns `{valid, viewport_index, is_perspective, "
			"size_x, size_y}`. `viewport_index` is 0-based across the "
			"level-editor viewport clients; `is_perspective` distinguishes "
			"perspective from ortho (top/front/side). `valid:false` when "
			"no viewport has focus. Requires a live editor.";
		d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"valid",          {{"type","boolean"}}},
				{"viewport_index", {{"type","integer"}}},
				{"is_perspective", {{"type","boolean"}}},
				{"size_x",         {{"type","integer"}}},
				{"size_y",         {{"type","integer"}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json&) {
			auto r = reader.GetActiveViewport();
			return nlohmann::json{
				{"valid",          r.valid},
				{"viewport_index", r.viewportIndex},
				{"is_perspective", r.isPerspective},
				{"size_x",         r.sizeX},
				{"size_y",         r.sizeY},
			};
		});
	}

	// ----- get_buffer_visualization_mode (Phase 13 Wave 3) ------------
	{
		ToolDescriptor d;
		d.name = "get_buffer_visualization_mode";
		d.description =
			"[editor] Active level viewport's buffer-visualization "
			"override (`base_color`, `roughness`, `normals`, `metallic`, "
			"etc.). Returns `{valid, mode}`. Empty `mode` means no "
			"override — the viewport renders in its current view-mode "
			"(Lit/Unlit/etc.). Requires a live editor.";
		d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"valid", {{"type","boolean"}}},
				{"mode",  {{"type","string"}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json&) {
			auto r = reader.GetBufferVisualizationMode();
			return nlohmann::json{{"valid", r.valid}, {"mode", r.mode}};
		});
	}

	// ----- get_gizmo_state (Phase 13 Wave 3) ---------------------------
	{
		ToolDescriptor d;
		d.name = "get_gizmo_state";
		d.description =
			"[editor] Translation gizmo state for the active level "
			"viewport. Returns `{valid, mode, coord_space}`. `mode` is "
			"`translate` / `rotate` / `scale` / `translaterotatez` / "
			"`2d` / `none`. `coord_space` is `world` or `local`. "
			"Requires a live editor.";
		d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"valid",       {{"type","boolean"}}},
				{"mode",        {{"type","string"}}},
				{"coord_space", {{"type","string"}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json&) {
			auto r = reader.GetGizmoState();
			return nlohmann::json{
				{"valid",       r.valid},
				{"mode",        r.mode},
				{"coord_space", r.coordSpace},
			};
		});
	}

	// ----- get_viewport_realtime (Phase 13 Wave 3) --------------------
	{
		ToolDescriptor d;
		d.name = "get_viewport_realtime";
		d.description =
			"[editor] Active level viewport's realtime flag — whether "
			"the viewport renders every frame (true) or only on user "
			"interaction (false). Returns `{valid, is_realtime}`. "
			"Requires a live editor.";
		d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"valid",       {{"type","boolean"}}},
				{"is_realtime", {{"type","boolean"}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json&) {
			auto r = reader.GetViewportRealtime();
			return nlohmann::json{
				{"valid",       r.valid},
				{"is_realtime", r.isRealtime},
			};
		});
	}

	// ----- get_viewport_camera_settings (Phase 13 Wave 3) -------------
	{
		ToolDescriptor d;
		d.name = "get_viewport_camera_settings";
		d.description =
			"[editor] Active level viewport's camera settings: FOV, "
			"camera speed multiplier, near/far clip planes. Returns "
			"`{valid, fov, camera_speed, near_clip, far_clip}`. `far_clip` "
			"is 0 when no override is set (engine default). Requires a "
			"live editor.";
		d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"valid",        {{"type","boolean"}}},
				{"fov",          {{"type","number"}}},
				{"camera_speed", {{"type","number"}}},
				{"near_clip",    {{"type","number"}}},
				{"far_clip",     {{"type","number"}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json&) {
			auto r = reader.GetViewportCameraSettings();
			return nlohmann::json{
				{"valid",        r.valid},
				{"fov",          r.fov},
				{"camera_speed", r.cameraSpeed},
				{"near_clip",    r.nearClip},
				{"far_clip",     r.farClip},
			};
		});
	}

	// ----- get_niagara_module_selection (Phase 12 Wave 2 — stub) -----
	{
		ToolDescriptor d;
		d.name = "get_niagara_module_selection";
		d.description =
			"[editor] Niagara system / emitter / module editor selection. "
			"v1 is a documented stub returning `valid:false` for all paths. "
			"Niagara editors use specialized toolkit types in the Niagara "
			"plugin's editor module — safe cross-cast without RTTI "
			"requires a sidecar registry. Same upgrade path as "
			"`get_anim_editor_state`.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path", {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"asset_path"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"valid",                  {{"type","boolean"}}},
				{"asset_path",             {{"type","string"}}},
				{"selected_module_names",  {{"type","array"}, {"items", {{"type","string"}}}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireAssetPath(args);
			auto r = reader.GetNiagaraModuleSelection(asset);
			return nlohmann::json{
				{"valid",                 r.valid},
				{"asset_path",            r.assetPath},
				{"selected_module_names", r.selectedModuleNames},
			};
		});
	}

	// ----- get_curve_editor_selection (Phase 12 Wave 2 — stub) -------
	{
		ToolDescriptor d;
		d.name = "get_curve_editor_selection";
		d.description =
			"[editor] Generic curve editor selection. v1 is a documented "
			"stub returning `valid:false`. The curve editor is a "
			"SCurveEditor widget hosted inside 5+ different editors "
			"(Anim, Sequencer, Particle, Material), so there's no single "
			"instance keyed on asset_path. Upgrade requires per-host-"
			"editor tracking of the active FCurveEditor pointer.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path", {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"asset_path"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"valid",                {{"type","boolean"}}},
				{"asset_path",           {{"type","string"}}},
				{"selected_key_count",   {{"type","integer"}}},
				{"selected_curve_names", {{"type","array"}, {"items", {{"type","string"}}}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireAssetPath(args);
			auto r = reader.GetCurveEditorSelection(asset);
			return nlohmann::json{
				{"valid",                r.valid},
				{"asset_path",           r.assetPath},
				{"selected_key_count",   r.selectedKeyCount},
				{"selected_curve_names", r.selectedCurveNames},
			};
		});
	}

	// ----- get_sequencer_state (Phase 12 Wave 2) ----------------------
	{
		ToolDescriptor d;
		d.name = "get_sequencer_state";
		d.description =
			"[editor] Sequencer state for an open ULevelSequence editor. "
			"Returns `{valid, asset_path, playhead_seconds, playback_status, "
			"playback_range_start_seconds, playback_range_end_seconds}`. "
			"`playback_status` is one of `stopped`, `playing`, `scrubbing`, "
			"`jumping`, `stepping`, `paused`, `unknown`. `valid:false` when "
			"the level sequence isn't open in the editor. Selection state "
			"(tracks/sections/keys) not yet exposed; v1 covers playhead "
			"+ range + status only. Requires a live editor.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path", {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"asset_path"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"valid",                         {{"type","boolean"}}},
				{"asset_path",                    {{"type","string"}}},
				{"playhead_seconds",              {{"type","number"}}},
				{"playback_status",               {{"type","string"}}},
				{"playback_range_start_seconds",  {{"type","number"}}},
				{"playback_range_end_seconds",    {{"type","number"}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireAssetPath(args);
			auto r = reader.GetSequencerState(asset);
			return nlohmann::json{
				{"valid",                         r.valid},
				{"asset_path",                    r.assetPath},
				{"playhead_seconds",              r.playheadSeconds},
				{"playback_status",               r.playbackStatus},
				{"playback_range_start_seconds",  r.playbackRangeStartSeconds},
				{"playback_range_end_seconds",    r.playbackRangeEndSeconds},
			};
		});
	}

}

void RegisterTools_04(ToolRegistry& registry, backends::IBlueprintReader& reader) {
	// ----- get_cinematic_camera(Phase 12 Wave 2) ----------------------
	{
		ToolDescriptor d;
		d.name = "get_cinematic_camera";
		d.description =
			"[editor] Currently-active camera in PIE — reads "
			"`APlayerCameraManager::GetViewTarget()` from the first PIE "
			"player controller. Returns `{valid, actor_name, loc_x/y/z, "
			"pitch/yaw/roll, fov}`. `valid:false` when PIE isn't running or "
			"no view target is set. Useful for inspecting what a cinematic "
			"sequence is showing the player right now.";
		d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"valid",      {{"type","boolean"}}},
				{"actor_name", {{"type","string"}}},
				{"loc_x",      {{"type","number"}}},
				{"loc_y",      {{"type","number"}}},
				{"loc_z",      {{"type","number"}}},
				{"pitch",      {{"type","number"}}},
				{"yaw",        {{"type","number"}}},
				{"roll",       {{"type","number"}}},
				{"fov",        {{"type","number"}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json&) {
			auto r = reader.GetCinematicCamera();
			return nlohmann::json{
				{"valid",      r.valid},
				{"actor_name", r.actorName},
				{"loc_x", r.locX}, {"loc_y", r.locY}, {"loc_z", r.locZ},
				{"pitch", r.pitch},{"yaw",   r.yaw},  {"roll",  r.roll},
				{"fov",   r.fov},
			};
		});
	}

	// ----- get_mesh_preview_state (Phase 12 Wave 2) --------------------
	{
		ToolDescriptor d;
		d.name = "get_mesh_preview_state";
		d.description =
			"[editor] Static mesh editor preview state: forced LOD level "
			"and currently-displayed LOD index. `current_lod_level` is -1 "
			"when on auto-LOD; an explicit integer otherwise. "
			"`current_lod_index` is the LOD being rendered right now (resolved "
			"by the editor's distance/auto selection). `valid:false` when the "
			"static mesh editor for the asset isn't open.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path", {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"asset_path"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"valid",             {{"type","boolean"}}},
				{"asset_path",        {{"type","string"}}},
				{"current_lod_level", {{"type","integer"}}},
				{"current_lod_index", {{"type","integer"}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireAssetPath(args);
			auto r = reader.GetMeshPreviewState(asset);
			return nlohmann::json{
				{"valid",             r.valid},
				{"asset_path",        r.assetPath},
				{"current_lod_level", r.currentLODLevel},
				{"current_lod_index", r.currentLODIndex},
			};
		});
	}

	// ----- get_material_editor_state (Phase 12 Wave 2) ----------------
	{
		ToolDescriptor d;
		d.name = "get_material_editor_state";
		d.description =
			"[editor] Material editor selection state. Returns `{valid, "
			"asset_path, selected_node_count, selected_expression_classes}`. "
			"`selected_expression_classes` lists short class names of "
			"currently-selected material expression nodes (e.g. "
			"`MaterialExpressionAdd`, `MaterialExpressionTextureSample`). "
			"`valid:false` when the material editor for the asset isn't "
			"open. Requires a live editor.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path", {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"asset_path"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"valid",                       {{"type","boolean"}}},
				{"asset_path",                  {{"type","string"}}},
				{"selected_node_count",         {{"type","integer"}}},
				{"selected_expression_classes", {{"type","array"}, {"items", {{"type","string"}}}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireAssetPath(args);
			auto r = reader.GetMaterialEditorState(asset);
			return nlohmann::json{
				{"valid",                       r.valid},
				{"asset_path",                  r.assetPath},
				{"selected_node_count",         r.selectedNodeCount},
				{"selected_expression_classes", r.selectedExpressionClasses},
			};
		});
	}

	// ----- get_umg_editor_state (Phase 12 Wave 2) ---------------------
	{
		ToolDescriptor d;
		d.name = "get_umg_editor_state";
		d.description =
			"[editor] Selection state for a UMG Widget Blueprint editor. "
			"Returns `{valid, asset_path, selected_widget_names, "
			"current_designer_tab}`. `selected_widget_names` are the "
			"template-side widget names (matching the widget hierarchy "
			"panel). `valid:false` when the UMG editor for the asset isn't "
			"open. Requires a live editor.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path", {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"asset_path"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"valid",                  {{"type","boolean"}}},
				{"asset_path",             {{"type","string"}}},
				{"selected_widget_names",  {{"type","array"}, {"items", {{"type","string"}}}}},
				{"current_designer_tab",   {{"type","string"}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireAssetPath(args);
			auto r = reader.GetUmgEditorState(asset);
			return nlohmann::json{
				{"valid",                 r.valid},
				{"asset_path",            r.assetPath},
				{"selected_widget_names", r.selectedWidgetNames},
				{"current_designer_tab",  r.currentDesignerTab},
			};
		});
	}

	// ----- get_static_mesh_info (Phase 12 Wave 2 — asset-direct) ------
	{
		ToolDescriptor d;
		d.name = "get_static_mesh_info";
		d.description =
			"[editor] Static mesh LOD / triangle / vertex info. Reads "
			"directly from the UStaticMesh asset — no editor instance "
			"needed. Returns `{valid, asset_path, lod_count, "
			"is_nanite_enabled, lods: [{triangle_count, vertex_count, "
			"screen_size}]}`. `screen_size` is the LOD-streaming "
			"threshold (0.0 when not set on the source model).";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path", {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"asset_path"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"valid",             {{"type","boolean"}}},
				{"asset_path",        {{"type","string"}}},
				{"lod_count",         {{"type","integer"}}},
				{"is_nanite_enabled", {{"type","boolean"}}},
				{"lods",              {{"type","array"}, {"items", {{"type","object"}}}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireAssetPath(args);
			auto r = reader.GetStaticMeshInfo(asset);
			nlohmann::json lods = nlohmann::json::array();
			for (const auto& l : r.lods) {
				lods.push_back({
					{"triangle_count", l.triangleCount},
					{"vertex_count",   l.vertexCount},
					{"screen_size",    l.screenSize},
				});
			}
			return nlohmann::json{
				{"valid",             r.valid},
				{"asset_path",        r.assetPath},
				{"lod_count",         r.lodCount},
				{"is_nanite_enabled", r.isNaniteEnabled},
				{"lods",              lods},
			};
		});
	}

	// ----- get_material_instance_params (Phase 12 Wave 2) -------------
	{
		ToolDescriptor d;
		d.name = "get_material_instance_params";
		d.description =
			"[editor] Dump scalar/vector/texture parameter values from a "
			"UMaterialInstanceConstant asset. No open editor required — "
			"reads parameters directly from the asset. Returns `{valid, "
			"asset_path, parent_path, scalars, vectors, textures}`. "
			"Switch params + RVT params not included as a scope cap.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path", {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"asset_path"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"valid",       {{"type","boolean"}}},
				{"asset_path",  {{"type","string"}}},
				{"parent_path", {{"type","string"}}},
				{"scalars",     {{"type","array"}, {"items", {{"type","object"}}}}},
				{"vectors",     {{"type","array"}, {"items", {{"type","object"}}}}},
				{"textures",    {{"type","array"}, {"items", {{"type","object"}}}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireAssetPath(args);
			auto r = reader.GetMaterialInstanceParams(asset);
			nlohmann::json scalars  = nlohmann::json::array();
			nlohmann::json vectors  = nlohmann::json::array();
			nlohmann::json textures = nlohmann::json::array();
			for (const auto& s : r.scalars) {
				scalars.push_back({{"name", s.name}, {"value", s.value}});
			}
			for (const auto& v : r.vectors) {
				vectors.push_back({
					{"name", v.name}, {"r", v.r}, {"g", v.g},
					{"b", v.b}, {"a", v.a},
				});
			}
			for (const auto& t : r.textures) {
				textures.push_back({{"name", t.name}, {"texture_path", t.texturePath}});
			}
			return nlohmann::json{
				{"valid",       r.valid},
				{"asset_path",  r.assetPath},
				{"parent_path", r.parentPath},
				{"scalars",     scalars},
				{"vectors",     vectors},
				{"textures",    textures},
			};
		});
	}

	// ----- get_blueprint_editor_state (Phase 12 Wave 2) ----------------
	{
		ToolDescriptor d;
		d.name = "get_blueprint_editor_state";
		d.description =
			"[editor] Per-Blueprint-editor state for the named BP asset. "
			"Returns `{valid, asset_path, current_graph_name, compile_status, "
			"selected_node_ids}`. `selected_node_ids` are FGuid strings; "
			"empty when no nodes selected. `compile_status` mirrors the "
			"`get_compile_status` enum strings. `valid:true` requires the "
			"BP's editor to be open (else still returns compile_status but "
			"no selection data). Requires a live editor.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path", {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"asset_path"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"valid",               {{"type","boolean"}}},
				{"asset_path",          {{"type","string"}}},
				{"current_graph_name",  {{"type","string"}}},
				{"compile_status",      {{"type","string"}}},
				{"selected_node_ids",   {{"type","array"}, {"items", {{"type","string"}}}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireAssetPath(args);
			auto r = reader.GetBlueprintEditorState(asset);
			return nlohmann::json{
				{"valid",              r.valid},
				{"asset_path",         r.assetPath},
				{"current_graph_name", r.currentGraphName},
				{"compile_status",     r.compileStatus},
				{"selected_node_ids",  r.selectedNodeIds},
			};
		});
	}

	// ----- list_actor_attributes (Phase 11 GAS) ------------------------
	{
		ToolDescriptor d;
		d.name = "list_actor_attributes";
		d.description =
			"[editor] Gameplay attributes on the named actor's "
			"`UAbilitySystemComponent`. Returns `{valid, actor_name, "
			"attributes: [{name, base_value, current_value}]}`. `base_value` "
			"is the raw value before active-effect modifications; "
			"`current_value` is after. `name` uses 'AttrSet.Attribute' "
			"form (e.g. 'LyraHealthSet.Health'). Requires a live editor + "
			"GAS plugin.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"actor_name", {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"actor_name"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"valid",      {{"type","boolean"}}},
				{"actor_name", {{"type","string"}}},
				{"attributes", {{"type","array"}, {"items", {{"type","object"}}}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string actor = RequireString(args, "actor_name");
			auto r = reader.ListActorAttributes(actor);
			nlohmann::json attrs = nlohmann::json::array();
			for (const auto& a : r.attributes) {
				attrs.push_back({
					{"name",          a.name},
					{"base_value",    a.baseValue},
					{"current_value", a.currentValue},
				});
			}
			return nlohmann::json{
				{"valid",      r.valid},
				{"actor_name", r.actorName},
				{"attributes", attrs},
			};
		});
	}

	// ----- list_actor_gameplay_effects ----------------------------------
	{
		ToolDescriptor d;
		d.name = "list_actor_gameplay_effects";
		d.description =
			"[editor] Active gameplay effects on the named actor's "
			"`UAbilitySystemComponent`. Returns `{valid, actor_name, "
			"effects: [{effect_class, stack_count, duration_remaining, "
			"level, granted_tags}]}`. `duration_remaining` is -1 for "
			"infinite-duration effects, else remaining seconds. "
			"Requires a live editor + GAS plugin.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"actor_name", {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"actor_name"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"valid",      {{"type","boolean"}}},
				{"actor_name", {{"type","string"}}},
				{"effects",    {{"type","array"}, {"items", {{"type","object"}}}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string actor = RequireString(args, "actor_name");
			auto r = reader.ListActorGameplayEffects(actor);
			nlohmann::json effs = nlohmann::json::array();
			for (const auto& e : r.effects) {
				effs.push_back({
					{"effect_class",       e.effectClass},
					{"stack_count",        e.stackCount},
					{"duration_remaining", e.durationRemaining},
					{"level",              e.level},
					{"granted_tags",       e.grantedTags},
				});
			}
			return nlohmann::json{
				{"valid",      r.valid},
				{"actor_name", r.actorName},
				{"effects",    effs},
			};
		});
	}

	// ----- list_actor_abilities (Phase 11 H Tier 1 — GAS) -------------
	{
		ToolDescriptor d;
		d.name = "list_actor_abilities";
		d.description =
			"[editor] Abilities granted to the named actor's "
			"`UAbilitySystemComponent`. Returns `{valid, actor_name, "
			"abilities: [{ability_class, is_active, level, "
			"instanced_count}]}`. `valid:false` when actor doesn't exist "
			"or has no ASC. `actor_name` matches the names from "
			"`get_selected_actors`. Looks at PIE world first, then editor. "
			"Lyra: useful for inspecting a pawn's runtime ability set. "
			"Requires a live editor + GAS plugin (always present in Lyra).";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"actor_name", {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"actor_name"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"valid",      {{"type","boolean"}}},
				{"actor_name", {{"type","string"}}},
				{"abilities",  {{"type","array"}, {"items", {{"type","object"}}}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string actor = RequireString(args, "actor_name");
			auto r = reader.ListActorAbilities(actor);
			nlohmann::json abs = nlohmann::json::array();
			for (const auto& a : r.abilities) {
				abs.push_back({
					{"ability_class",   a.abilityClass},
					{"is_active",       a.isActive},
					{"level",           a.level},
					{"instanced_count", a.instancedCount},
				});
			}
			return nlohmann::json{
				{"valid",      r.valid},
				{"actor_name", r.actorName},
				{"abilities",  abs},
			};
		});
	}

	// ----- list_actor_gameplay_tags -------------------------------------
	{
		ToolDescriptor d;
		d.name = "list_actor_gameplay_tags";
		d.description =
			"[editor] Owned gameplay tags on the named actor's "
			"`UAbilitySystemComponent`. Union of tags granted by abilities, "
			"by effects, and loose tags. Returns `{valid, actor_name, tags: "
			"['Status.Buffed', ...]}`. `valid:false` when actor doesn't "
			"exist or has no ASC. Requires a live editor + GAS plugin.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"actor_name", {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"actor_name"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"valid",      {{"type","boolean"}}},
				{"actor_name", {{"type","string"}}},
				{"tags",       {{"type","array"}, {"items", {{"type","string"}}}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string actor = RequireString(args, "actor_name");
			auto r = reader.ListActorGameplayTags(actor);
			return nlohmann::json{
				{"valid",      r.valid},
				{"actor_name", r.actorName},
				{"tags",       r.tags},
			};
		});
	}

}

void RegisterTools_04b(ToolRegistry& registry, backends::IBlueprintReader& reader) {
	// ----- list_plugins (Phase 11 H Tier 1) ----------------------------
	{
		ToolDescriptor d;
		d.name = "list_plugins";
		d.description =
			"[editor] Enumerate all discovered plugins via IPluginManager. "
			"Returns `{plugins: [{name, descriptor_path, category, version, "
			"is_enabled, is_built_in, is_content_only}]}`. Includes both "
			"enabled and disabled plugins (use `is_enabled` to filter). "
			"Requires a live editor.";
		d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"plugins", {{"type","array"}, {"items", {{"type","object"}}}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json&) {
			auto r = reader.ListPlugins();
			nlohmann::json plugs = nlohmann::json::array();
			for (const auto& p : r.plugins) {
				plugs.push_back({
					{"name",            p.name},
					{"descriptor_path", p.descriptorPath},
					{"category",        p.category},
					{"version",         p.version},
					{"is_enabled",      p.isEnabled},
					{"is_built_in",     p.isBuiltIn},
					{"is_content_only", p.isContentOnly},
				});
			}
			return nlohmann::json{{"plugins", plugs}};
		});
	}

	// ----- get_plugin_descriptor ----------------------------------------
	{
		ToolDescriptor d;
		d.name = "get_plugin_descriptor";
		d.description =
			"[editor] Read the full .uplugin descriptor for `plugin_name`. "
			"Returns `{valid, descriptor}` where `descriptor` is the raw "
			"parsed JSON from the .uplugin file (UE's FPluginDescriptor "
			"schema — name, version, description, modules, plugins, etc.). "
			"`valid:false` when plugin name doesn't resolve. Requires a "
			"live editor.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"plugin_name", {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"plugin_name"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"valid",      {{"type","boolean"}}},
				{"descriptor", {{"type","object"}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string name = RequireString(args, "plugin_name");
			auto r = reader.GetPluginDescriptor(name);
			return nlohmann::json{
				{"valid",      r.valid},
				{"descriptor", r.descriptor.is_null() ? nlohmann::json::object() : r.descriptor},
			};
		});
	}

	// ----- get_plugin_dependencies --------------------------------------
	{
		ToolDescriptor d;
		d.name = "get_plugin_dependencies";
		d.description =
			"[editor] Plugins that `plugin_name` depends on (extracted from "
			"the `Plugins:` array in its descriptor). Returns `{valid, "
			"dependencies: [plugin_name, ...]}`. Useful for understanding "
			"dependency chains before disabling a plugin. Requires a live "
			"editor.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"plugin_name", {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"plugin_name"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"valid",        {{"type","boolean"}}},
				{"dependencies", {{"type","array"}, {"items", {{"type","string"}}}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string name = RequireString(args, "plugin_name");
			auto r = reader.GetPluginDependencies(name);
			return nlohmann::json{
				{"valid",        r.valid},
				{"dependencies", r.dependencies},
			};
		});
	}

	// ----- list_game_features (Phase 11 H Tier 1) ----------------------
	{
		ToolDescriptor d;
		d.name = "list_game_features";
		d.description =
			"[editor] List all known Game Feature Plugins (GFPs) and their "
			"simplified state. Lyra uses GFPs extensively as its modular "
			"content mechanism. Returns `{features: [{plugin_name, "
			"plugin_url, state}]}`. `state` is one of: `unknown`, "
			"`registered`, `loading`, `loaded`, `active`, `deactivating`. "
			"Requires a live editor (GameFeaturesSubsystem only lives in a "
			"running engine context).";
		d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"features", {{"type","array"}, {"items", {{"type","object"}}}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json&) {
			auto r = reader.ListGameFeatures();
			nlohmann::json feats = nlohmann::json::array();
			for (const auto& f : r.features) {
				feats.push_back({
					{"plugin_name", f.pluginName},
					{"plugin_url",  f.pluginUrl},
					{"state",       f.state},
				});
			}
			return nlohmann::json{{"features", feats}};
		});
	}

	// ----- get_game_feature_state ---------------------------------------
	{
		ToolDescriptor d;
		d.name = "get_game_feature_state";
		d.description =
			"[editor] Look up a single Game Feature Plugin's state by name "
			"(case-insensitive match against `plugin_name`). Returns "
			"`{valid, plugin_name, plugin_url, state}`. `valid:false` when "
			"the plugin name doesn't resolve. Requires a live editor.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"plugin_name", {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"plugin_name"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"valid",       {{"type","boolean"}}},
				{"plugin_name", {{"type","string"}}},
				{"plugin_url",  {{"type","string"}}},
				{"state",       {{"type","string"}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string name = RequireString(args, "plugin_name");
			auto r = reader.GetGameFeatureState(name);
			return nlohmann::json{
				{"valid",       r.valid},
				{"plugin_name", r.pluginName},
				{"plugin_url",  r.pluginUrl},
				{"state",       r.state},
			};
		});
	}

	// ----- ui_snapshot ----------------------------------------------------
	{
		ToolDescriptor d;
		d.name = "ui_snapshot";
		d.description =
			"[editor] Recursive Slate widget tree snapshot. Walks every "
			"visible top-level window (or only the one whose title contains "
			"`window`) and returns each widget as `{depth, widget_type, text, "
			"parent_window}`. `text` is best-effort (extracted from "
			"`STextBlock`-like widgets); empty otherwise. Capped at 500 "
			"nodes; `truncated:true` when a subtree was cut off. Default "
			"max_depth=8. Requires a live editor.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"window",    {{"type","string"}}},
				{"max_depth", {{"type","integer"}}},
			}},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"nodes", {
					{"type","array"},
					{"items", {{"type","object"}}},
				}},
				{"truncated", {{"type","boolean"}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string window = args.value("window", std::string{});
			int depth = args.value("max_depth", 8);
			auto r = reader.UiSnapshot(window, depth);
			nlohmann::json nodes = nlohmann::json::array();
			for (const auto& n : r.nodes) {
				nodes.push_back({
					{"depth",         n.depth},
					{"widget_type",   n.widgetType},
					{"text",          n.text},
					{"parent_window", n.parentWindow},
				});
			}
			return nlohmann::json{
				{"nodes", nodes}, {"truncated", r.truncated},
			};
		});
	}

	// ----- ui_find -------------------------------------------------------
	{
		ToolDescriptor d;
		d.name = "ui_find";
		d.description =
			"[editor] Locate Slate widgets by visible text. Returns the same "
			"`{nodes, truncated}` shape as `ui_snapshot` but filtered: only "
			"widgets whose extracted text contains `text` (case-sensitive). "
			"`role` (optional) further restricts to widgets whose type name "
			"contains the substring (e.g. `Button` matches `SButton`). "
			"Capped at 200 hits. Requires a live editor.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"text", {{"type","string"}}},
				{"role", {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"text"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"nodes",     {{"type","array"}, {"items", {{"type","object"}}}}},
				{"truncated", {{"type","boolean"}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string text = RequireString(args, "text");
			std::string role = args.value("role", std::string{});
			auto r = reader.UiFind(text, role);
			nlohmann::json nodes = nlohmann::json::array();
			for (const auto& n : r.nodes) {
				nodes.push_back({
					{"depth",         n.depth},
					{"widget_type",   n.widgetType},
					{"text",          n.text},
					{"parent_window", n.parentWindow},
				});
			}
			return nlohmann::json{
				{"nodes", nodes}, {"truncated", r.truncated},
			};
		});
	}

	// ----- list_desktop_windows -----------------------------------------
	{
		ToolDescriptor d;
		d.name = "list_desktop_windows";
		d.description =
			"[editor] List visible top-level Slate windows: "
			"`{windows: [{title, widget_type, pos_x/y, size_x/y, is_active}]}`. "
			"Lighter-weight alternative to a desktop screenshot when an "
			"agent just needs to know which windows are open. Z-order from "
			"`GetAllVisibleWindowsOrdered`. Requires a live editor. NOTE: "
			"full `take_desktop_screenshot` (multi-window composite image) "
			"is deferred — see plan doc Wave 1 remainders.";
		d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"windows", {
					{"type","array"},
					{"items", {{"type","object"}}},
				}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json&) {
			auto r = reader.ListDesktopWindows();
			nlohmann::json wins = nlohmann::json::array();
			for (const auto& w : r.windows) {
				wins.push_back({
					{"title",       w.title},
					{"widget_type", w.widgetType},
					{"pos_x",       w.posX},
					{"pos_y",       w.posY},
					{"size_x",      w.sizeX},
					{"size_y",      w.sizeY},
					{"is_active",   w.isActive},
				});
			}
			return nlohmann::json{{"windows", wins}};
		});
	}

	// ----- world_pos_to_screen ------------------------------------------
	{
		ToolDescriptor d;
		d.name = "world_pos_to_screen";
		d.description =
			"[editor] Project a 3D world position to the active level "
			"viewport's screen space. Returns `{valid, screen_x, screen_y, "
			"is_on_screen}`. `screen_x`/`screen_y` are normalized [0,1] "
			"across the viewport. `is_on_screen` is true only when the "
			"projection is in front of the camera AND inside the viewport. "
			"Requires a live editor.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"x", {{"type","number"}}},
				{"y", {{"type","number"}}},
				{"z", {{"type","number"}}},
			}},
			{"required", nlohmann::json::array({"x","y","z"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"valid",        {{"type","boolean"}}},
				{"screen_x",     {{"type","number"}}},
				{"screen_y",     {{"type","number"}}},
				{"is_on_screen", {{"type","boolean"}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			double x = args.value("x", 0.0);
			double y = args.value("y", 0.0);
			double z = args.value("z", 0.0);
			auto r = reader.WorldToScreen(x, y, z);
			return nlohmann::json{
				{"valid",        r.valid},
				{"screen_x",     r.screenX},
				{"screen_y",     r.screenY},
				{"is_on_screen", r.isOnScreen},
			};
		});
	}

	// ----- screen_to_world ----------------------------------------------
	{
		ToolDescriptor d;
		d.name = "screen_to_world";
		d.description =
			"[editor] Inverse of `world_pos_to_screen`. Cast a ray from "
			"screen-normalized [0,1] coordinates out to `max_distance` cm "
			"in world space. Returns `{valid, hit, world_x, world_y, "
			"world_z, hit_actor_name}`. When `hit` is true, the world coords "
			"are the line-trace impact point and `hit_actor_name` identifies "
			"the actor. When false, the coords are the ray endpoint. "
			"Default max_distance is 10000 cm. Requires a live editor.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"screen_x",     {{"type","number"}}},
				{"screen_y",     {{"type","number"}}},
				{"max_distance", {{"type","number"}}},
			}},
			{"required", nlohmann::json::array({"screen_x","screen_y"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"valid",          {{"type","boolean"}}},
				{"hit",            {{"type","boolean"}}},
				{"world_x",        {{"type","number"}}},
				{"world_y",        {{"type","number"}}},
				{"world_z",        {{"type","number"}}},
				{"hit_actor_name", {{"type","string"}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			double sx = args.value("screen_x", 0.5);
			double sy = args.value("screen_y", 0.5);
			double md = args.value("max_distance", 10000.0);
			auto r = reader.ScreenToWorld(sx, sy, md);
			return nlohmann::json{
				{"valid",          r.valid},
				{"hit",            r.hit},
				{"world_x",        r.worldX},
				{"world_y",        r.worldY},
				{"world_z",        r.worldZ},
				{"hit_actor_name", r.hitActorName},
			};
		});
	}

	// ----- get_selected_assets (content browser) ------------------------
	{
		ToolDescriptor d;
		d.name = "get_selected_assets";
		d.description =
			"[editor] Currently-selected assets in the Content Browser. "
			"Returns `{asset_paths: [/Game/AI/BP_Foo, ...]}` (package form). "
			"Multi-select preserved in order. Empty when nothing selected. "
			"Requires a live editor.";
		d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"asset_paths", {{"type","array"}, {"items", {{"type","string"}}}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json&) {
			auto r = reader.GetSelectedAssets();
			return nlohmann::json{{"asset_paths", r.assetPaths}};
		});
	}

	// ----- set_selected_assets (content browser) ------------------------
	{
		ToolDescriptor d;
		d.name = "set_selected_assets";
		d.description =
			"[editor] Replace the Content Browser asset selection. "
			"`asset_paths` are package form (`/Game/AI/BP_Foo`). Returns the "
			"post-call selection so the caller can verify (UI may not have "
			"settled instantly — poll if needed). Requires a live editor.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_paths", {{"type","array"}, {"items", {{"type","string"}}}}},
			}},
			{"required", nlohmann::json::array({"asset_paths"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"asset_paths", {{"type","array"}, {"items", {{"type","string"}}}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::vector<std::string> paths;
			if (auto it = args.find("asset_paths"); it != args.end() && it->is_array()) {
				for (const auto& v : *it) {
					if (v.is_string()) paths.push_back(v.get<std::string>());
				}
			}
			auto r = reader.SetSelectedAssets(paths);
			return nlohmann::json{{"ok", true}, {"asset_paths", r.assetPaths}};
		});
	}

	// ----- get_selected_folders (content browser) -----------------------
	{
		ToolDescriptor d;
		d.name = "get_selected_folders";
		d.description =
			"[editor] Currently-selected folder paths in the Content Browser "
			"tree (distinct from assets — folders are navigation rows). "
			"Returns `{folder_paths: [...]}` in package form. Requires a "
			"live editor.";
		d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"folder_paths", {{"type","array"}, {"items", {{"type","string"}}}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json&) {
			auto r = reader.GetSelectedFolders();
			return nlohmann::json{{"folder_paths", r.folderPaths}};
		});
	}

	// ----- get_content_browser_path -------------------------------------
	{
		ToolDescriptor d;
		d.name = "get_content_browser_path";
		d.description =
			"[editor] The folder the Content Browser is currently displaying "
			"(its address-bar path). Returns `{current_path}`. Empty when no "
			"path is selected. Requires a live editor.";
		d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"current_path", {{"type","string"}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json&) {
			auto r = reader.GetContentBrowserPath();
			return nlohmann::json{{"current_path", r.currentPath}};
		});
	}

	// ----- set_content_browser_path -------------------------------------
	{
		ToolDescriptor d;
		d.name = "set_content_browser_path";
		d.description =
			"[editor] Navigate the Content Browser to `folder_path` "
			"(package form, e.g. `/Game/AI/Behaviors`). Returns the post-"
			"call current_path for verification. Requires a live editor.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"folder_path", {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"folder_path"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"current_path", {{"type","string"}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string folder = RequireString(args, "folder_path");
			auto r = reader.SetContentBrowserPath(folder);
			return nlohmann::json{{"ok", true}, {"current_path", r.currentPath}};
		});
	}

	// ----- get_selected_components ---------------------------------------
	{
		ToolDescriptor d;
		d.name = "get_selected_components";
		d.description =
			"[editor] Components owned by each currently-selected actor. "
			"Returns `[{actor_name, components: [{name, component_class}]}]`. "
			"Empty actors array when nothing is selected. Useful when an "
			"agent wants to operate on a specific component (mesh, collision, "
			"trigger) without first asking the user to drill into the actor.";
		d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
		d.output_schema = {
			{"type", "object"},
			{"properties", {
				{"actors", {
					{"type", "array"},
					{"items", {
						{"type", "object"},
						{"properties", {
							{"actor_name", {{"type","string"}}},
							{"components", {
								{"type","array"},
								{"items", {
									{"type","object"},
									{"properties", {
										{"name",            {{"type","string"}}},
										{"component_class", {{"type","string"}}},
									}},
								}},
							}},
						}},
					}},
				}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json&) {
			auto r = reader.GetSelectedComponents();
			nlohmann::json actors = nlohmann::json::array();
			for (const auto& a : r.actors) {
				nlohmann::json comps = nlohmann::json::array();
				for (const auto& c : a.components) {
					comps.push_back({
						{"name",            c.name},
						{"component_class", c.componentClass},
					});
				}
				actors.push_back({
					{"actor_name", a.actorName},
					{"components", comps},
				});
			}
			return nlohmann::json{{"actors", actors}};
		});
	}

}

void RegisterTools_05(ToolRegistry& registry, backends::IBlueprintReader& reader) {
	// ----- get_camera_transform------------------------------------------
	{
		ToolDescriptor d;
		d.name = "get_camera_transform";
		d.description =
			"[editor] Read the active level viewport's camera state. Picks "
			"the focused level viewport (or first perspective if none has "
			"focus). Returns `{valid, loc_x/y/z, pitch/yaw/roll, fov}`. "
			"`valid:false` means no level viewport exists (PIE teardown, "
			"headless commandlet). Requires a live editor.";
		d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
		d.output_schema = {
			{"type", "object"},
			{"properties", {
				{"valid", {{"type","boolean"}}},
				{"loc_x", {{"type","number"}}},
				{"loc_y", {{"type","number"}}},
				{"loc_z", {{"type","number"}}},
				{"pitch", {{"type","number"}}},
				{"yaw",   {{"type","number"}}},
				{"roll",  {{"type","number"}}},
				{"fov",   {{"type","number"}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json&) {
			auto r = reader.GetCameraTransform();
			return nlohmann::json{
				{"valid", r.valid},
				{"loc_x", r.locX}, {"loc_y", r.locY}, {"loc_z", r.locZ},
				{"pitch", r.pitch},{"yaw",   r.yaw},  {"roll",  r.roll},
				{"fov",   r.fov},
			};
		});
	}

	// ----- get_view_mode --------------------------------------------------
	{
		ToolDescriptor d;
		d.name = "get_view_mode";
		d.description =
			"[editor] Read the active level viewport's view mode "
			"(Lit / Unlit / Wireframe / DetailLighting / ShaderComplexity / "
			"LightingOnly / ReflectionOverride / VisualizeBuffer / etc.). "
			"Returns `{valid, mode}`. Requires a live editor.";
		d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
		d.output_schema = {
			{"type", "object"},
			{"properties", {
				{"valid", {{"type","boolean"}}},
				{"mode",  {{"type","string"}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json&) {
			auto r = reader.GetViewMode();
			return nlohmann::json{{"valid", r.valid}, {"mode", r.mode}};
		});
	}

	// ----- get_show_flags ------------------------------------------------
	{
		ToolDescriptor d;
		d.name = "get_show_flags";
		d.description =
			"[editor] Read the active level viewport's commonly-toggled show "
			"flags. Returns a curated subset of FEngineShowFlags' ~100 fields: "
			"wireframe, collision, grid, bounds, navigation, atmosphere, fog, "
			"lighting, post_processing, antialiasing, shadows. Boolean each. "
			"Requires a live editor.";
		d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
		d.output_schema = {
			{"type", "object"},
			{"properties", {
				{"valid",           {{"type","boolean"}}},
				{"wireframe",       {{"type","boolean"}}},
				{"collision",       {{"type","boolean"}}},
				{"grid",            {{"type","boolean"}}},
				{"bounds",          {{"type","boolean"}}},
				{"navigation",      {{"type","boolean"}}},
				{"atmosphere",      {{"type","boolean"}}},
				{"fog",             {{"type","boolean"}}},
				{"lighting",        {{"type","boolean"}}},
				{"post_processing", {{"type","boolean"}}},
				{"antialiasing",    {{"type","boolean"}}},
				{"shadows",         {{"type","boolean"}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json&) {
			auto r = reader.GetShowFlags();
			return nlohmann::json{
				{"valid",           r.valid},
				{"wireframe",       r.wireframe},
				{"collision",       r.collision},
				{"grid",            r.grid},
				{"bounds",          r.bounds},
				{"navigation",      r.navigation},
				{"atmosphere",      r.atmosphere},
				{"fog",             r.fog},
				{"lighting",        r.lighting},
				{"post_processing", r.postProcessing},
				{"antialiasing",    r.antialiasing},
				{"shadows",         r.shadows},
			};
		});
	}

	// ----- get_active_editor_mode -----------------------------------------
	{
		ToolDescriptor d;
		d.name = "get_active_editor_mode";
		d.description =
			"[editor] Active level-editor mode(s) — what tool is the user "
			"working in? Returns `{active_modes: [...]}`. Common modes: "
			"`EM_Default` (selection), `EM_Placement`, `EM_Landscape`, "
			"`EM_Foliage`, `EM_MeshPaint`, `EM_ModelingMode`. Multi-mode "
			"is possible (UE supports concurrent modes); primary is "
			"element [0]. Requires a live editor.";
		d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
		d.output_schema = {
			{"type", "object"},
			{"properties", {
				{"active_modes", {
					{"type", "array"},
					{"items", {{"type", "string"}}},
				}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json&) {
			auto r = reader.GetActiveEditorMode();
			return nlohmann::json{{"active_modes", r.activeModes}};
		});
	}

	// ----- live_coding_compile -------------------------------------------
	{
		ToolDescriptor d;
		d.name = "live_coding_compile";
		d.description =
			"[editor] Trigger UE's Live Coding compile + patch. The compile runs "
			"asynchronously; Live Coding emits its own progress + result "
			"to the editor log (use `read_output_log` to follow).";
		d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",      {{"type","boolean"}}},
				{"queued",  {{"type","boolean"}}},
				{"message", {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"ok","queued","message"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json&) {
			auto r = reader.LiveCodingCompile();
			return nlohmann::json{{"ok", true},
								  {"queued", r.queued}, {"message", r.message}};
		});
	}

	// ----- get_selected_actors -------------------------------------------
	{
		ToolDescriptor d;
		d.name = "get_selected_actors";
		d.description =
			"[editor] List the names of currently-selected actors in the level editor. "
			"Names are the stable in-package names (not display labels). "
			"Empty array when nothing is selected.";
		d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",          {{"type","boolean"}}},
				{"actor_names", {{"type","array"}, {"items", {{"type","string"}}}}},
			}},
			{"required", nlohmann::json::array({"ok","actor_names"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json&) {
			auto r = reader.GetSelectedActors();
			return nlohmann::json{{"ok", true}, {"actor_names", r.actorNames}};
		});
	}

	// ----- set_selection --------------------------------------------------
	{
		ToolDescriptor d;
		d.name = "set_selection";
		d.description =
			"[editor] Replace (or extend) the editor viewport's actor selection. `replace:true` "
			"(default) clears existing selection first; `false` adds to it. "
			"Returns the post-call selected names so the caller can verify.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"actor_names", {{"type","array"}, {"items", {{"type","string"}}}}},
				{"replace",     {{"type","boolean"}}},
			}},
			{"required", nlohmann::json::array({"actor_names"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",          {{"type","boolean"}}},
				{"actor_names", {{"type","array"}, {"items", {{"type","string"}}}}},
			}},
			{"required", nlohmann::json::array({"ok","actor_names"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::vector<std::string> names;
			if (auto it = args.find("actor_names"); it != args.end() && it->is_array()) {
				for (const auto& v : *it) {
					if (v.is_string())
					{
						names.push_back(v.get<std::string>());
					}
				}
			}
			bool replace = args.value("replace", true);
			auto r = reader.SetSelection(names, replace);
			return nlohmann::json{{"ok", true}, {"actor_names", r.actorNames}};
		});
	}

	// ----- spawn_actor ----------------------------------------------------
	{
		ToolDescriptor d;
		d.name = "spawn_actor";
		d.description =
			"[editor] Spawn an actor of the given UClass in the current level. "
			"`class_path` is the full path (e.g. `/Script/Engine.StaticMeshActor` "
			"or a BP class like `/Game/AI/BP_Enemy.BP_Enemy_C`). All transform "
			"fields are optional and default to identity.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"class_path", {{"type","string"}}},
				{"location",   {{"type","object"}, {"properties", {
					{"x",{{"type","number"}}}, {"y",{{"type","number"}}}, {"z",{{"type","number"}}}}}}},
				{"rotation",   {{"type","object"}, {"properties", {
					{"pitch",{{"type","number"}}}, {"yaw",{{"type","number"}}}, {"roll",{{"type","number"}}}}}}},
				{"scale",      {{"type","object"}, {"properties", {
					{"x",{{"type","number"}}}, {"y",{{"type","number"}}}, {"z",{{"type","number"}}}}}}},
			}},
			{"required", nlohmann::json::array({"class_path"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",          {{"type","boolean"}}},
				{"actor_name",  {{"type","string"}}},
				{"actor_label", {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"ok","actor_name","actor_label"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string cls = RequireString(args, "class_path");
			auto loc   = args.value("location", nlohmann::json::object());
			auto rot   = args.value("rotation", nlohmann::json::object());
			auto scl   = args.value("scale",    nlohmann::json::object());
			auto r = reader.SpawnActor(cls,
				loc.value("x", 0.0), loc.value("y", 0.0), loc.value("z", 0.0),
				rot.value("pitch", 0.0), rot.value("yaw", 0.0), rot.value("roll", 0.0),
				scl.value("x", 1.0), scl.value("y", 1.0), scl.value("z", 1.0));
			return nlohmann::json{{"ok", true},
								  {"actor_name",  r.actorName},
								  {"actor_label", r.actorLabel}};
		});
	}

	// ----- set_actor_transform -------------------------------------------
	{
		ToolDescriptor d;
		d.name = "set_actor_transform";
		d.description =
			"[editor] Update a placed actor's world transform in the current level. `actor_name` is from "
			"`get_selected_actors` or `spawn_actor`'s response. All transform "
			"fields are absolute (not delta).";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"actor_name", {{"type","string"}}},
				{"location",   {{"type","object"}}},
				{"rotation",   {{"type","object"}}},
				{"scale",      {{"type","object"}}},
			}},
			{"required", nlohmann::json::array({"actor_name"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",         {{"type","boolean"}}},
				{"actor_name", {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"ok","actor_name"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string name = RequireString(args, "actor_name");
			auto loc = args.value("location", nlohmann::json::object());
			auto rot = args.value("rotation", nlohmann::json::object());
			auto scl = args.value("scale",    nlohmann::json::object());
			reader.SetActorTransform(name,
				loc.value("x", 0.0), loc.value("y", 0.0), loc.value("z", 0.0),
				rot.value("pitch", 0.0), rot.value("yaw", 0.0), rot.value("roll", 0.0),
				scl.value("x", 1.0), scl.value("y", 1.0), scl.value("z", 1.0));
			return nlohmann::json{{"ok", true}, {"actor_name", name}};
		});
	}

	// ----- delete_actor --------------------------------------------------
	{
		ToolDescriptor d;
		d.name = "delete_actor";
		d.description =
			"[editor] Destroy an actor by name. Returns `{deleted: false}` if the "
			"actor wasn't found.";
		d.input_schema = {
			{"type","object"},
			{"properties", {{"actor_name", {{"type","string"}}}}},
			{"required", nlohmann::json::array({"actor_name"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",         {{"type","boolean"}}},
				{"actor_name", {{"type","string"}}},
				{"deleted",    {{"type","boolean"}}},
			}},
			{"required", nlohmann::json::array({"ok","actor_name","deleted"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string name = RequireString(args, "actor_name");
			auto r = reader.DeleteActor(name);
			return nlohmann::json{{"ok", true},
								  {"actor_name", name}, {"deleted", r.deleted}};
		});
	}

	// ----- read_output_log -----------------------------------------------
	{
		ToolDescriptor d;
		d.name = "read_output_log";
		d.description =
			"[editor] Read recent entries from the editor's output log. The plugin "
			"module installs a ring-buffer log sink at startup; this returns "
			"up to `limit` of the most recent entries (default 200), "
			"optionally filtered by `min_severity` (Display / Log / Warning "
			"/ Error / Fatal).";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"limit",         {{"type","integer"}}},
				{"min_severity",  {{"type","string"},
					{"enum", nlohmann::json::array({"Display","Log","Warning","Error","Fatal"})}}},
			}},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",      {{"type","boolean"}}},
				{"entries", {{"type","array"}, {"items", {
					{"type","object"},
					{"properties", {
						{"severity",  {{"type","string"}}},
						{"category",  {{"type","string"}}},
						{"message",   {{"type","string"}}},
						{"timestamp", {{"type","string"}}},
					}},
				}}}},
			}},
			{"required", nlohmann::json::array({"ok","entries"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			int limit = OptInt(args, "limit", 200);
			std::string minSev = OptString(args, "min_severity", "");
			auto r = reader.ReadOutputLog(limit, minSev);
			nlohmann::json entries = nlohmann::json::array();
			for (const auto& e : r.entries) {
				entries.push_back(nlohmann::json{
					{"severity",  e.severity},
					{"category",  e.category},
					{"message",   e.message},
					{"timestamp", e.timestamp},
				});
			}
			return nlohmann::json{{"ok", true}, {"entries", entries}};
		});
	}

	// ----- add_data_row ---------------------------------------------------
	{
		ToolDescriptor d;
		d.name = "add_data_row";
		d.description =
			"[data table] Add a row to an existing DataTable. The row name must be "
			"unique within the table; existing names return "
			"`{already_existed:true}` unless `overwrite:true` is passed. "
			"`values` is an object whose keys map to the row struct's "
			"field names; values are stringified and coerced via "
			"FProperty::ImportText (works for scalars, enums, and structs "
			"that round-trip through text). Pair with `read_data_table` to "
			"see the row-struct shape before calling.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path", {{"type","string"}}},
				{"row_name",   {{"type","string"}}},
				{"values",     {{"type","object"},
								{"description","Field-name → value map. Values are stringified; ImportText coerces to the property's type."}}},
				{"overwrite",  {{"type","boolean"},
								{"description","Default false. Set true to replace an existing row."}}},
			}},
			{"required", nlohmann::json::array({"asset_path","row_name","values"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",             {{"type","boolean"}}},
				{"asset_path",     {{"type","string"}}},
				{"row_name",       {{"type","string"}}},
				{"already_existed",{{"type","boolean"}}},
				{"created",        {{"type","boolean"}}},
			}},
			{"required", nlohmann::json::array({"ok","asset_path","row_name","already_existed","created"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireAssetPath(args);
			std::string row   = RequireString(args, "row_name");
			nlohmann::json values = args.value("values", nlohmann::json::object());
			bool overwrite = args.value("overwrite", false);
			auto r = reader.AddDataRow(asset, row, values, overwrite);
			return nlohmann::json{
				{"ok", true},
				{"asset_path",      r.assetPath},
				{"row_name",        r.rowName},
				{"already_existed", r.alreadyExisted},
				{"created",         r.created},
			};
		});
	}

	// ----- set_data_row_value --------------------------------------------
	{
		ToolDescriptor d;
		d.name = "set_data_row_value";
		d.description =
			"[data table] Update a single field on an existing DataTable row. "
			"`field_name` must match a property on the row struct; "
			"`value` is its string form (ImportText input). Returns the "
			"pre-set and post-set ExportText'd values so the caller can "
			"verify the coercion landed.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path", {{"type","string"}}},
				{"row_name",   {{"type","string"}}},
				{"field_name", {{"type","string"}}},
				{"value",      {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"asset_path","row_name","field_name","value"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",         {{"type","boolean"}}},
				{"asset_path", {{"type","string"}}},
				{"row_name",   {{"type","string"}}},
				{"field_name", {{"type","string"}}},
				{"old_value",  {{"type","string"}}},
				{"new_value",  {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"ok","asset_path","row_name","field_name","old_value","new_value"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireAssetPath(args);
			std::string row   = RequireString(args, "row_name");
			std::string field = RequireString(args, "field_name");
			std::string value = RequireString(args, "value");
			auto r = reader.SetDataRowValue(asset, row, field, value);
			return nlohmann::json{
				{"ok", true},
				{"asset_path", r.assetPath},
				{"row_name",   r.rowName},
				{"field_name", r.fieldName},
				{"old_value",  r.oldValue},
				{"new_value",  r.newValue},
			};
		});
	}

	// ----- add_component / remove_component / attach_component /
	//       set_component_property ------------------------------------
	//
	// BP component authoring: SCS (SimpleConstructionScript) tree
	// manipulation + property edits on component templates.
	{
		ToolDescriptor d;
		d.name = "add_component";
		d.description =
			"[blueprint] Add a component (StaticMeshComponent, AudioComponent, etc.) to a Blueprint's SimpleConstructionScript tree — author-time component setup, not runtime spawning. For runtime actor spawning use `spawn_actor`. "
			"`component_class` is the full UClass path (e.g. "
			"`/Script/Engine.StaticMeshComponent`). Pass `parent` to attach "
			"as a child of an existing node; omit for root attachment. "
			"`socket` applies to SceneComponent children only. Idempotent on "
			"`name`: existing names return `{already_existed:true}`.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path",      {{"type","string"}}},
				{"name",            {{"type","string"}}},
				{"component_class", {{"type","string"}}},
				{"parent",          {{"type","string"}}},
				{"socket",          {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"asset_path","name","component_class"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",              {{"type","boolean"}}},
				{"asset_path",      {{"type","string"}}},
				{"name",            {{"type","string"}}},
				{"component_class", {{"type","string"}}},
				{"already_existed", {{"type","boolean"}}},
				{"created",         {{"type","boolean"}}},
			}},
			{"required", nlohmann::json::array({"ok","asset_path","name","component_class","already_existed","created"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireAssetPath(args);
			std::string name  = RequireString(args, "name");
			std::string cls   = RequireString(args, "component_class");
			std::string parent = OptString(args, "parent", "");
			std::string socket = OptString(args, "socket", "");
			auto r = reader.AddComponent(asset, name, cls, parent, socket);
			return nlohmann::json{
				{"ok", true},
				{"asset_path",       r.assetPath},
				{"name",             r.name},
				{"component_class",  r.componentClass},
				{"already_existed",  r.alreadyExisted},
				{"created",          r.created},
			};
		});
	}
	{
		ToolDescriptor d;
		d.name = "remove_component";
		d.description =
			"[blueprint] Remove a component from a Blueprint's SCS tree by name. "
			"Returns `{removed:false}` when the component isn't found.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path", {{"type","string"}}},
				{"name",       {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"asset_path","name"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",         {{"type","boolean"}}},
				{"asset_path", {{"type","string"}}},
				{"name",       {{"type","string"}}},
				{"removed",    {{"type","boolean"}}},
			}},
			{"required", nlohmann::json::array({"ok","asset_path","name","removed"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireAssetPath(args);
			std::string name  = RequireString(args, "name");
			auto r = reader.RemoveComponent(asset, name);
			return nlohmann::json{
				{"ok", true},
				{"asset_path", r.assetPath},
				{"name",       r.name},
				{"removed",    r.removed},
			};
		});
	}
	{
		ToolDescriptor d;
		d.name = "attach_component";
		d.description =
			"[blueprint] Re-parent an SCS component on a Blueprint. Pass `new_parent` to attach the "
			"component as a child of that node; pass empty to attach at "
			"the SCS root. `socket` applies to SceneComponent children "
			"only.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path", {{"type","string"}}},
				{"name",       {{"type","string"}}},
				{"new_parent", {{"type","string"}}},
				{"socket",     {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"asset_path","name"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",         {{"type","boolean"}}},
				{"asset_path", {{"type","string"}}},
				{"name",       {{"type","string"}}},
				{"new_parent", {{"type","string"}}},
				{"socket",     {{"type","string"}}},
				{"reparented", {{"type","boolean"}}},
			}},
			{"required", nlohmann::json::array({"ok","asset_path","name","new_parent","socket","reparented"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset  = RequireAssetPath(args);
			std::string name   = RequireString(args, "name");
			std::string parent = OptString(args, "new_parent", "");
			std::string socket = OptString(args, "socket", "");
			auto r = reader.AttachComponent(asset, name, parent, socket);
			return nlohmann::json{
				{"ok", true},
				{"asset_path",     r.assetPath},
				{"name",           r.name},
				{"new_parent",     r.newParentName},
				{"socket",         r.socket},
				{"reparented",     r.reparented},
			};
		});
	}
	{
		ToolDescriptor d;
		d.name = "set_component_property";
		d.description =
			"[blueprint] Set a UPROPERTY on a Blueprint component's template "
			"(the author-time default values, what the BP Details panel shows "
			"for that component). For widget UPROPERTYs use "
			"`set_widget_property`; for behavior tree nodes use "
			"`set_bt_node_property`. Same string→type coercion as "
			"`set_data_row_value` (FProperty::ImportText). Returns "
			"pre-set and post-set ExportText'd values for verification, plus "
			"`default_value` (the component class-default for this property) "
			"and `has_override` — so a `new_value` that equals the default "
			"(which exports as the default's text, e.g. \"\" / \"False\") isn't "
			"misread as cleared.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path",    {{"type","string"}}},
				{"component",     {{"type","string"}}},
				{"property",      {{"type","string"}}},
				{"value",         {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"asset_path","component","property","value"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",            {{"type","boolean"}}},
				{"asset_path",    {{"type","string"}}},
				{"component",     {{"type","string"}}},
				{"property",      {{"type","string"}}},
				{"old_value",     {{"type","string"}}},
				{"new_value",     {{"type","string"}}},
				{"default_value", {{"type","string"}}},
				{"has_override",  {{"type","boolean"}}},
			}},
			{"required", nlohmann::json::array({"ok","asset_path","component","property","old_value","new_value","default_value","has_override"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireAssetPath(args);
			std::string comp  = RequireString(args, "component");
			std::string prop  = RequireString(args, "property");
			std::string value = RequireString(args, "value");
			auto r = reader.SetComponentProperty(asset, comp, prop, value);
			return nlohmann::json{
				{"ok", true},
				{"asset_path",     r.assetPath},
				{"component",      r.componentName},
				{"property",       r.propertyName},
				{"old_value",      r.oldValue},
				{"new_value",      r.newValue},
				{"default_value",  r.defaultValue},
				{"has_override",   r.hasOverride},
			};
		});
	}

	// ----- run_automation_tests ------------------------------------------
	{
		ToolDescriptor d;
		d.name = "run_automation_tests";
		d.description =
			"[tests] Trigger UE's automation test framework. `pattern` is the "
			"test-name wildcard (e.g. `BlueprintReader.*`, `*Smoke*`); empty "
			"means every registered test. The run is async — this tool "
			"kicks it off and returns. Use `read_output_log` to follow "
			"results, or check `Saved/Automation/index.json` after for the "
			"structured report.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"pattern", {{"type","string"},
							 {"description","Test-name wildcard. Empty = all tests."}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string pattern = OptString(args, "pattern", "");
			auto r = reader.RunAutomationTests(pattern);
			return nlohmann::json{
				{"ok", true},
				{"started", r.started},
				{"message", r.message},
			};
		});
	}

	// ===== Material authoring (Stage 1) ====================================
	// The material expression graph is a separate UObject tree from
	// Blueprint event graphs — `ReadMaterial` returns expression nodes +
	// their connections + parameter names. Writes (add_expression,
	// connect, set_parameter, compile) mutate the UMaterial directly;
	// mark dirty + SavePackage after if you want it to persist on disk.

	// ----- create_material -----------------------------------------------
	{
		ToolDescriptor d;
		d.name = "create_material";
		d.description =
			"[material] Create a new UMaterial asset under `/Game/...` (default "
			"Surface / Opaque / DefaultLit). The material analogue of "
			"`create_blueprint`: a fresh material to populate with "
			"`add_material_expression` / `connect_material_expressions` / "
			"`set_material_parameter` / `compile_material`. Idempotent — returns "
			"`{ok:true, already_existed:true}` if the asset is already present.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path", {{"type","string"},
								{"description","Must start with /Game/. Example: /Game/Materials/M_New"}}},
			}},
			{"required", nlohmann::json::array({"asset_path"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",              {{"type","boolean"}}},
				{"asset_path",      {{"type","string"}}},
				{"already_existed", {{"type","boolean"}}},
				{"saved",           {{"type","boolean"}}},
			}},
			{"required", nlohmann::json::array({"ok","asset_path"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireAssetPath(args);
			auto r = reader.CreateMaterial(asset);
			return nlohmann::json{
				{"ok", true},
				{"asset_path", asset},
				{"already_existed", r.alreadyExisted},
				{"saved", r.saved},
			};
		});
	}

	// ----- create_material_instance --------------------------------------
	{
		ToolDescriptor d;
		d.name = "create_material_instance";
		d.description =
			"[material] Create a UMaterialInstanceConstant under `/Game/...`, "
			"parented to `parent` (a material or material instance). The enabler "
			"for recreating material instances + the safe way to use "
			"`set_material_instance_parameter` (which needs a parent that declares "
			"the parameter). Idempotent.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path", {{"type","string"},
								{"description","Must start with /Game/. Example: /Game/Materials/MI_New"}}},
				{"parent",     {{"type","string"},
								{"description","Parent material/MI path. Example: /Game/Materials/M_Base. Optional (empty = no parent)."}}},
			}},
			{"required", nlohmann::json::array({"asset_path"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",              {{"type","boolean"}}},
				{"asset_path",      {{"type","string"}}},
				{"already_existed", {{"type","boolean"}}},
				{"saved",           {{"type","boolean"}}},
				{"parent_path",     {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"ok","asset_path"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset  = RequireAssetPath(args);
			std::string parent = OptString(args, "parent", "");
			auto r = reader.CreateMaterialInstance(asset, parent);
			return nlohmann::json{
				{"ok", true},
				{"asset_path", asset},
				{"already_existed", r.alreadyExisted},
				{"saved", r.saved},
				{"parent_path", r.parentPath},
			};
		});
	}

	// ----- list_materials ------------------------------------------------
	{
		ToolDescriptor d;
		d.name = "list_materials";
		d.description =
			"[material] List all UMaterial / UMaterialInstance assets under a content "
			"path. Mirrors `list_blueprints` but filters by class. Defaults "
			"to `/Game`.";
		d.input_schema = {
			{"type","object"},
			{"properties", {{"path", {{"type","string"}}}}},
		};
		d.output_schema = {
			{"type","array"},
			{"items", {
				{"type","object"},
				{"properties", {
					{"asset_path",   {{"type","string"}}},
					{"name",         {{"type","string"}}},
					{"parent_class", {{"type","string"}}},
					{"modified_iso", {{"type","string"}}},
				}},
				{"required", nlohmann::json::array({"asset_path","name","parent_class"})},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string path = OptString(args, "path", "/Game");
			auto ctl = ParseResponseControls(args);
			auto summaries = reader.ListMaterials(path);
			nlohmann::json arr = nlohmann::json::array();
			for (const auto& s : summaries)
			{
				arr.push_back(s);
			}
			return ListResponse(std::move(arr), ctl);
		});
	}

	// ----- read_material -------------------------------------------------
	{
		ToolDescriptor d;
		d.name = "read_material";
		d.description =
			"[material] Read a material's expression graph: every UMaterialExpression "
			"node (id, class, parameter name, x/y), every connection (from "
			"expression output → expression input or master-material slot "
			"like BaseColor / Roughness), and the names of all exposed "
			"scalar/vector parameters.";
		d.input_schema = {
			{"type","object"},
			{"properties", {{"asset_path", {{"type","string"}}}}},
			{"required", nlohmann::json::array({"asset_path"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",              {{"type","boolean"}}},
				{"asset_path",      {{"type","string"}}},
				{"expressions",     {{"type","array"}}},
				{"connections",     {{"type","array"}}},
				{"parameter_names", {{"type","array"}}},
			}},
			{"required", nlohmann::json::array({"ok","asset_path","expressions","connections","parameter_names"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireAssetPath(args);
			auto m = reader.ReadMaterial(asset);
			nlohmann::json exprs = nlohmann::json::array();
			for (const auto& e : m.expressions) {
				exprs.push_back({
					{"id", e.id}, {"class", e.className},
					{"parameter_name", e.parameterName},
					{"x", e.x}, {"y", e.y},
				});
			}
			nlohmann::json conns = nlohmann::json::array();
			for (const auto& c : m.connections) {
				conns.push_back({
					{"from_node", c.fromNodeId}, {"from_pin", c.fromPin},
					{"to_node",   c.toNodeId},   {"to_pin",   c.toPin},
				});
			}
			return nlohmann::json{
				{"ok", true},
				{"asset_path",      m.assetPath},
				{"expressions",     exprs},
				{"connections",     conns},
				{"parameter_names", m.parameterNames},
			};
		});
	}

	// ----- add_material_expression ---------------------------------------
	{
		ToolDescriptor d;
		d.name = "add_material_expression";
		d.description =
			"[material] Add a UMaterialExpression node to a Material graph. For Blueprint graph nodes use `add_node`. `expression_class` "
			"is the short class name like `MaterialExpressionConstant3Vector`, "
			"`MaterialExpressionScalarParameter`, "
			"`MaterialExpressionTextureSampleParameter2D`. x/y are graph "
			"coordinates. Returns the new expression's id (use in "
			"`connect_material_expressions`).";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path",       {{"type","string"}}},
				{"expression_class", {{"type","string"}}},
				{"x", {{"type","integer"}}},
				{"y", {{"type","integer"}}},
			}},
			{"required", nlohmann::json::array(
				{"asset_path","expression_class"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",            {{"type","boolean"}}},
				{"asset_path",    {{"type","string"}}},
				{"expression_id", {{"type","string"}}},
				{"class",         {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"ok","asset_path","expression_id","class"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireAssetPath(args);
			std::string cls   = RequireString(args, "expression_class");
			int x = args.value("x", 0);
			int y = args.value("y", 0);
			auto r = reader.AddMaterialExpression(asset, cls, x, y);
			return nlohmann::json{
				{"ok", true},
				{"asset_path",    r.assetPath},
				{"expression_id", r.expressionId},
				{"class",         r.className},
			};
		});
	}

}

}  // namespace bpr::tools
