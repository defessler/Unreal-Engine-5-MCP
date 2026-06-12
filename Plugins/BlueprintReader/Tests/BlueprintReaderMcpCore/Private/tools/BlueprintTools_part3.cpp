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

void RegisterTools_06(ToolRegistry& registry, backends::IBlueprintReader& reader) {
	// ----- connect_material_expressions----------------------------------
	{
		ToolDescriptor d;
		d.name = "connect_material_expressions";
		d.description =
			"[material] Wire one expression's output pin to another expression's input "
			"pin, or to a master-material slot. Pass empty `to_node` to wire "
			"to a master slot (`to_pin` then names the slot, e.g. "
			"`BaseColor`, `Metallic`, `Roughness`, `EmissiveColor`, `Normal`).";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path", {{"type","string"}}},
				{"from_node",  {{"type","string"}}},
				{"from_pin",   {{"type","string"}}},
				{"to_node",    {{"type","string"}}},
				{"to_pin",     {{"type","string"}}},
			}},
			{"required", nlohmann::json::array(
				{"asset_path","from_node","from_pin","to_pin"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",         {{"type","boolean"}}},
				{"asset_path", {{"type","string"}}},
				{"connected",  {{"type","boolean"}}},
			}},
			{"required", nlohmann::json::array({"ok","asset_path","connected"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireAssetPath(args);
			std::string fn = RequireString(args, "from_node");
			std::string fp = RequireString(args, "from_pin");
			std::string tn = OptString(args, "to_node", "");
			std::string tp = RequireString(args, "to_pin");
			auto r = reader.ConnectMaterialExpressions(asset, fn, fp, tn, tp);
			return nlohmann::json{
				{"ok", true},
				{"asset_path", r.assetPath},
				{"connected",  r.connected},
			};
		});
	}

	// ----- set_material_parameter ----------------------------------------
	{
		ToolDescriptor d;
		d.name = "set_material_parameter";
		d.description =
			"[material] Set the default value of a named scalar/vector "
			"parameter on a UMaterial (the base material). `value` is the "
			"parameter's text representation (scalar: `0.5`; vector: "
			"`(R=1,G=0,B=0,A=1)`). For per-instance overrides use "
			"`set_material_instance_parameter`.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path",     {{"type","string"}}},
				{"parameter_name", {{"type","string"}}},
				{"value",          {{"type","string"}}},
			}},
			{"required", nlohmann::json::array(
				{"asset_path","parameter_name","value"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",             {{"type","boolean"}}},
				{"asset_path",     {{"type","string"}}},
				{"parameter_name", {{"type","string"}}},
				{"old_value",      {{"type","string"}}},
				{"new_value",      {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"ok","asset_path","parameter_name","old_value","new_value"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireAssetPath(args);
			std::string name  = RequireString(args, "parameter_name");
			std::string value = RequireString(args, "value");
			auto r = reader.SetMaterialParameter(asset, name, value);
			return nlohmann::json{
				{"ok", true},
				{"asset_path",     r.assetPath},
				{"parameter_name", r.parameterName},
				{"old_value",      r.oldValue},
				{"new_value",      r.newValue},
			};
		});
	}

	// ----- set_material_instance_parameter -------------------------------
	{
		ToolDescriptor d;
		d.name = "set_material_instance_parameter";
		d.description =
			"[material] Override a parameter on a UMaterialInstanceConstant. For base-material defaults use `set_material_parameter`. `type` is "
			"`scalar`, `vector`, or `texture`; `value` is its text form "
			"(scalar `0.5`, vector `(R=...,G=...,B=...,A=...)`, texture "
			"`/Game/Textures/T_Foo.T_Foo`).";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path",     {{"type","string"}}},
				{"parameter_name", {{"type","string"}}},
				{"type",           {{"type","string"}}},
				{"value",          {{"type","string"}}},
			}},
			{"required", nlohmann::json::array(
				{"asset_path","parameter_name","type","value"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",             {{"type","boolean"}}},
				{"asset_path",     {{"type","string"}}},
				{"parameter_name", {{"type","string"}}},
				{"type",           {{"type","string"}}},
				{"new_value",      {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"ok","asset_path","parameter_name","type","new_value"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireAssetPath(args);
			std::string name  = RequireString(args, "parameter_name");
			std::string type  = RequireString(args, "type");
			std::string value = RequireString(args, "value");
			auto r = reader.SetMaterialInstanceParameter(asset, name, type, value);
			return nlohmann::json{
				{"ok", true},
				{"asset_path",     r.assetPath},
				{"parameter_name", r.parameterName},
				{"type",           r.paramType},
				{"new_value",      r.newValue},
			};
		});
	}

	// ----- compile_material ----------------------------------------------
	{
		ToolDescriptor d;
		d.name = "compile_material";
		d.description =
			"[material] Recompile a material's shader code. UE normally compiles "
			"incrementally on edit; call this explicitly to flush pending "
			"recompiles or recover from a stuck shader compile state.";
		d.input_schema = {
			{"type","object"},
			{"properties", {{"asset_path", {{"type","string"}}}}},
			{"required", nlohmann::json::array({"asset_path"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",         {{"type","boolean"}}},
				{"asset_path", {{"type","string"}}},
				{"compiled",   {{"type","boolean"}}},
			}},
			{"required", nlohmann::json::array({"ok","asset_path","compiled"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireAssetPath(args);
			auto r = reader.CompileMaterial(asset);
			return nlohmann::json{
				{"ok", true},
				{"asset_path", r.assetPath},
				{"compiled",   r.compiled},
			};
		});
	}

	// ===== UMG widget authoring (Stage 1) ==================================
	// UMG widget blueprints store their hierarchy in a UWidgetTree rather
	// than a USimpleConstructionScript — different shape from actor
	// components, so they get their own tool surface.

	// ----- read_widget_blueprint -----------------------------------------
	{
		ToolDescriptor d;
		d.name = "read_widget_blueprint";
		d.description =
			"[widget] Read a UWidgetBlueprint's widget tree: every UWidget node "
			"(name, class, parent name) and the root widget's name. "
			"Mirrors `get_components` but for UMG.";
		d.input_schema = {
			{"type","object"},
			{"properties", {{"asset_path", {{"type","string"}}}}},
			{"required", nlohmann::json::array({"asset_path"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",        {{"type","boolean"}}},
				{"asset_path",{{"type","string"}}},
				{"root_name", {{"type","string"}}},
				{"nodes",     {{"type","array"}}},
			}},
			{"required", nlohmann::json::array({"ok","asset_path","root_name","nodes"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireAssetPath(args);
			auto w = reader.ReadWidgetBlueprint(asset);
			nlohmann::json nodes = nlohmann::json::array();
			for (const auto& n : w.nodes) {
				nodes.push_back({
					{"name",   n.name},
					{"class",  n.className},
					{"parent", n.parentName},
				});
			}
			return nlohmann::json{
				{"ok", true},
				{"asset_path", w.assetPath},
				{"root_name",  w.rootName},
				{"nodes",      nodes},
			};
		});
	}

	// ----- add_widget ----------------------------------------------------
	{
		ToolDescriptor d;
		d.name = "add_widget";
		d.description =
			"[widget] Add a UWidget node (Button, TextBlock, etc.) to a UWidgetBlueprint's UMG tree. For Blueprint graph nodes use `add_node`. `widget_class` is the "
			"short class name (`Button`, `TextBlock`, `Image`, `VerticalBox`, "
			"etc.). `parent_name` empty = becomes the new root (replaces "
			"the existing root only if the tree was empty). Otherwise "
			"appends as a child of `parent_name`.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path",   {{"type","string"}}},
				{"parent_name",  {{"type","string"}}},
				{"widget_class", {{"type","string"}}},
				{"name",         {{"type","string"}}},
			}},
			{"required", nlohmann::json::array(
				{"asset_path","widget_class","name"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",             {{"type","boolean"}}},
				{"asset_path",     {{"type","string"}}},
				{"name",           {{"type","string"}}},
				{"widget_class",   {{"type","string"}}},
				{"already_existed",{{"type","boolean"}}},
				{"created",        {{"type","boolean"}}},
			}},
			{"required", nlohmann::json::array({"ok","asset_path","name","widget_class","already_existed","created"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset  = RequireAssetPath(args);
			std::string parent = OptString(args, "parent_name", "");
			std::string cls    = RequireString(args, "widget_class");
			std::string name   = RequireString(args, "name");
			auto r = reader.AddWidget(asset, parent, cls, name);
			return nlohmann::json{
				{"ok", true},
				{"asset_path",      r.assetPath},
				{"name",            r.name},
				{"widget_class",    r.widgetClass},
				{"already_existed", r.alreadyExisted},
				{"created",         r.created},
			};
		});
	}

	// ----- set_widget_property -------------------------------------------
	{
		ToolDescriptor d;
		d.name = "set_widget_property";
		d.description =
			"[widget] Set a UPROPERTY on a UWidget in a UWidgetBlueprint. For Blueprint component UPROPERTYs use `set_component_property`. `property_name` "
			"is the property's name as authored in C++ (`Text`, "
			"`ColorAndOpacity`, `Visibility`). `value` is the property's text "
			"form (text: a string; FLinearColor: `(R=1,G=0,B=0,A=1)`).";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path",    {{"type","string"}}},
				{"widget_name",   {{"type","string"}}},
				{"property_name", {{"type","string"}}},
				{"value",         {{"type","string"}}},
			}},
			{"required", nlohmann::json::array(
				{"asset_path","widget_name","property_name","value"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",            {{"type","boolean"}}},
				{"asset_path",    {{"type","string"}}},
				{"widget_name",   {{"type","string"}}},
				{"property_name", {{"type","string"}}},
				{"old_value",     {{"type","string"}}},
				{"new_value",     {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"ok","asset_path","widget_name","property_name","old_value","new_value"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireAssetPath(args);
			std::string w     = RequireString(args, "widget_name");
			std::string p     = RequireString(args, "property_name");
			std::string v     = RequireString(args, "value");
			auto r = reader.SetWidgetProperty(asset, w, p, v);
			return nlohmann::json{
				{"ok", true},
				{"asset_path",    r.assetPath},
				{"widget_name",   r.widgetName},
				{"property_name", r.propertyName},
				{"old_value",     r.oldValue},
				{"new_value",     r.newValue},
			};
		});
	}

	// ----- bind_widget_event ---------------------------------------------
	{
		ToolDescriptor d;
		d.name = "bind_widget_event";
		d.description =
			"[widget] Bind a widget's event (e.g. `OnClicked` on a Button) to a "
			"named handler function in the widget blueprint's graph. If the "
			"handler function doesn't exist, it's created with the event's "
			"signature. Pairs with `add_function` if you want to author the "
			"handler explicitly first.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path",       {{"type","string"}}},
				{"widget_name",      {{"type","string"}}},
				{"event_name",       {{"type","string"}}},
				{"handler_function", {{"type","string"}}},
			}},
			{"required", nlohmann::json::array(
				{"asset_path","widget_name","event_name","handler_function"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",               {{"type","boolean"}}},
				{"asset_path",       {{"type","string"}}},
				{"widget_name",      {{"type","string"}}},
				{"event_name",       {{"type","string"}}},
				{"handler_function", {{"type","string"}}},
				{"bound",            {{"type","boolean"}}},
			}},
			{"required", nlohmann::json::array({"ok","asset_path","widget_name","event_name","handler_function","bound"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireAssetPath(args);
			std::string w     = RequireString(args, "widget_name");
			std::string e     = RequireString(args, "event_name");
			std::string h     = RequireString(args, "handler_function");
			auto r = reader.BindWidgetEvent(asset, w, e, h);
			// UX-P4b: ok reflects whether the bind actually happened — the old
			// hardcoded ok:true masked the commandlet's honest bound:false and
			// reported phantom success when no node was created.
			nlohmann::json out{
				{"ok",               r.bound},
				{"asset_path",       r.assetPath},
				{"widget_name",      r.widgetName},
				{"event_name",       r.eventName},
				{"handler_function", r.handlerFunction},
				{"bound",            r.bound},
			};
			if (!r.reason.empty()) {
				out["reason"] = r.reason;
			}
			return out;
		});
	}

	// ----- compile_widget_blueprint --------------------------------------
	{
		ToolDescriptor d;
		d.name = "compile_widget_blueprint";
		d.description =
			"[widget] Compile a UWidgetBlueprint. Equivalent to clicking Compile in "
			"the UMG designer. Returns `{compiled: true|false}` — false "
			"means compile failed; check `read_output_log` for errors.";
		d.input_schema = {
			{"type","object"},
			{"properties", {{"asset_path", {{"type","string"}}}}},
			{"required", nlohmann::json::array({"asset_path"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",         {{"type","boolean"}}},
				{"asset_path", {{"type","string"}}},
				{"compiled",   {{"type","boolean"}}},
			}},
			{"required", nlohmann::json::array({"ok","asset_path","compiled"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireAssetPath(args);
			auto r = reader.CompileWidgetBlueprint(asset);
			return nlohmann::json{
				{"ok", true},
				{"asset_path", r.assetPath},
				{"compiled",   r.compiled},
			};
		});
	}

	// ===== Behavior Tree authoring (Stage 2) ===============================
	// Behavior Trees are AIModule UObjects: root composite + decorators +
	// services + tasks. node_kind = "composite" | "decorator" | "service"
	// | "task". Node ids are stable UObject names within the tree.

	{
		ToolDescriptor d;
		d.name = "list_behavior_trees";
		d.description = "[behavior tree] List UBehaviorTree assets under a content path "
						"(default `/Game`).";
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
			auto summaries = reader.ListBehaviorTrees(path);
		auto ctl = ParseResponseControls(args);
			nlohmann::json arr = nlohmann::json::array();
			for (const auto& s : summaries)
			{
				arr.push_back(s);
			}
			return ListResponse(std::move(arr), ctl);
		});
	}

	{
		ToolDescriptor d;
		d.name = "read_behavior_tree";
		d.description = "[behavior tree] Walk a UBehaviorTree's node graph. Returns every "
						"node (id, class, kind, parent) and the root node id.";
		d.input_schema = {
			{"type","object"},
			{"properties", {{"asset_path", {{"type","string"}}}}},
			{"required", nlohmann::json::array({"asset_path"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",          {{"type","boolean"}}},
				{"asset_path",  {{"type","string"}}},
				{"root_node_id",{{"type","string"}}},
				{"nodes",       {{"type","array"}}},
			}},
			{"required", nlohmann::json::array({"ok","asset_path","root_node_id","nodes"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireAssetPath(args);
			auto bt = reader.ReadBehaviorTree(asset);
			nlohmann::json nodes = nlohmann::json::array();
			for (const auto& n : bt.nodes) {
				nodes.push_back({
					{"node_id",   n.nodeId},
					{"class",     n.className},
					{"node_kind", n.nodeKind},
					{"parent",    n.parentNodeId},
				});
			}
			return nlohmann::json{
				{"ok", true},
				{"asset_path",   bt.assetPath},
				{"root_node_id", bt.rootNodeId},
				{"nodes",        nodes},
			};
		});
	}

	{
		ToolDescriptor d;
		d.name = "add_bt_node";
		d.description = "[behavior tree] Add a node (composite/decorator/service/task) to a UBehaviorTree. NOT for Blueprint graph nodes (`add_node`). `node_kind` is "
						"`composite` / `decorator` / `service` / `task`; "
						"`node_class` is the short class name (e.g. "
						"`BTComposite_Selector`, `BTTask_MoveTo`, "
						"`BTDecorator_Blackboard`). Empty `parent_node_id` "
						"becomes the root composite (only allowed for the "
						"first composite added).";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path",     {{"type","string"}}},
				{"parent_node_id", {{"type","string"}}},
				{"node_kind",      {{"type","string"}}},
				{"node_class",     {{"type","string"}}},
			}},
			{"required", nlohmann::json::array(
				{"asset_path","node_kind","node_class"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",         {{"type","boolean"}}},
				{"asset_path", {{"type","string"}}},
				{"node_id",    {{"type","string"}}},
				{"class",      {{"type","string"}}},
				{"node_kind",  {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"ok","asset_path","node_id","class","node_kind"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset  = RequireAssetPath(args);
			std::string parent = OptString(args, "parent_node_id", "");
			std::string kind   = RequireString(args, "node_kind");
			std::string cls    = RequireString(args, "node_class");
			auto r = reader.AddBTNode(asset, parent, kind, cls);
			return nlohmann::json{
				{"ok", true},
				{"asset_path", r.assetPath},
				{"node_id",    r.nodeId},
				{"class",      r.className},
				{"node_kind",  r.nodeKind},
			};
		});
	}

	{
		ToolDescriptor d;
		d.name = "set_bt_node_property";
		d.description = "[behavior tree] Set a UPROPERTY on a UBehaviorTree node (e.g. "
						"MoveTo's `AcceptableRadius`, Blackboard decorator's "
						"`KeyName`). `value` is the property's text form.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path",    {{"type","string"}}},
				{"node_id",       {{"type","string"}}},
				{"property_name", {{"type","string"}}},
				{"value",         {{"type","string"}}},
			}},
			{"required", nlohmann::json::array(
				{"asset_path","node_id","property_name","value"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",            {{"type","boolean"}}},
				{"asset_path",    {{"type","string"}}},
				{"node_id",       {{"type","string"}}},
				{"property_name", {{"type","string"}}},
				{"old_value",     {{"type","string"}}},
				{"new_value",     {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"ok","asset_path","node_id","property_name","old_value","new_value"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireAssetPath(args);
			std::string n     = RequireString(args, "node_id");
			std::string p     = RequireString(args, "property_name");
			std::string v     = RequireString(args, "value");
			auto r = reader.SetBTNodeProperty(asset, n, p, v);
			return nlohmann::json{
				{"ok", true},
				{"asset_path",    r.assetPath},
				{"node_id",       r.nodeId},
				{"property_name", r.propertyName},
				{"old_value",     r.oldValue},
				{"new_value",     r.newValue},
			};
		});
	}

	{
		ToolDescriptor d;
		d.name = "compile_behavior_tree";
		d.description = "[behavior tree] Compile a behavior tree (recompiles + marks the "
						"asset dirty). Returns `{compiled: true|false}`.";
		d.input_schema = {
			{"type","object"},
			{"properties", {{"asset_path", {{"type","string"}}}}},
			{"required", nlohmann::json::array({"asset_path"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",         {{"type","boolean"}}},
				{"asset_path", {{"type","string"}}},
				{"compiled",   {{"type","boolean"}}},
			}},
			{"required", nlohmann::json::array({"ok","asset_path","compiled"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireAssetPath(args);
			auto r = reader.CompileBehaviorTree(asset);
			return nlohmann::json{
				{"ok", true},
				{"asset_path", r.assetPath},
				{"compiled",   r.compiled},
			};
		});
	}

	// ===== DataAsset CRUD (Stage 2) ========================================
	// UDataAsset subclasses are pure data containers. read_data_asset
	// returns every UPROPERTY's text projection in a JSON map; mutations go
	// through ImportText_Direct/ExportText_Direct (same pattern as
	// set_component_property + set_widget_property).

	{
		ToolDescriptor d;
		d.name = "list_data_assets";
		d.description = "[data asset] List all UDataAsset subclass instances under a "
						"content path. Mirrors `list_blueprints` but "
						"filters by base class.";
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
			auto summaries = reader.ListDataAssets(path);
		auto ctl = ParseResponseControls(args);
			nlohmann::json arr = nlohmann::json::array();
			for (const auto& s : summaries)
			{
				arr.push_back(s);
			}
			return ListResponse(std::move(arr), ctl);
		});
	}

	{
		ToolDescriptor d;
		d.name = "read_data_asset";
		d.description = "[data asset] Read every UPROPERTY on a UDataAsset. Returns the "
						"asset's class + a `{property: stringified_value}` "
						"map.";
		d.input_schema = {
			{"type","object"},
			{"properties", {{"asset_path", {{"type","string"}}}}},
			{"required", nlohmann::json::array({"asset_path"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",        {{"type","boolean"}}},
				{"asset_path",{{"type","string"}}},
				{"class",     {{"type","string"}}},
				{"properties",{{"type","object"}}},
			}},
			{"required", nlohmann::json::array({"ok","asset_path","class","properties"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireAssetPath(args);
			auto da = reader.ReadDataAsset(asset);
			return nlohmann::json{
				{"ok", true},
				{"asset_path", da.assetPath},
				{"class",      da.className},
				{"properties", da.properties},
			};
		});
	}

	{
		ToolDescriptor d;
		d.name = "create_data_asset";
		d.description = "[data asset] Create a new UDataAsset instance. `class_name` is "
						"the short C++ class name (or BP path) of a "
						"UDataAsset subclass.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path", {{"type","string"}}},
				{"class_name", {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"asset_path","class_name"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",             {{"type","boolean"}}},
				{"asset_path",     {{"type","string"}}},
				{"class",          {{"type","string"}}},
				{"created",        {{"type","boolean"}}},
				{"already_existed",{{"type","boolean"}}},
			}},
			{"required", nlohmann::json::array({"ok","asset_path","class","created","already_existed"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireAssetPath(args);
			std::string cls   = RequireString(args, "class_name");
			auto r = reader.CreateDataAsset(asset, cls);
			return nlohmann::json{
				{"ok", true},
				{"asset_path",      r.assetPath},
				{"class",           r.className},
				{"created",         r.created},
				{"already_existed", r.alreadyExisted},
			};
		});
	}

	{
		ToolDescriptor d;
		d.name = "set_data_asset_property";
		d.description = "[data asset] Set a UPROPERTY on a UDataAsset instance. `value` is the "
						"text form UE's property system uses.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path",    {{"type","string"}}},
				{"property_name", {{"type","string"}}},
				{"value",         {{"type","string"}}},
			}},
			{"required", nlohmann::json::array(
				{"asset_path","property_name","value"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",            {{"type","boolean"}}},
				{"asset_path",    {{"type","string"}}},
				{"property_name", {{"type","string"}}},
				{"old_value",     {{"type","string"}}},
				{"new_value",     {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"ok","asset_path","property_name","old_value","new_value"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireAssetPath(args);
			std::string p     = RequireString(args, "property_name");
			std::string v     = RequireString(args, "value");
			auto r = reader.SetDataAssetProperty(asset, p, v);
			return nlohmann::json{
				{"ok", true},
				{"asset_path",    r.assetPath},
				{"property_name", r.propertyName},
				{"old_value",     r.oldValue},
				{"new_value",     r.newValue},
			};
		});
	}

	// ===== StateTree authoring (Stage 2) ===================================
	// UStateTree (experimental in UE 5.x) — hierarchical FSM with state +
	// transition nodes. State ids are stable names within the asset.

	{
		ToolDescriptor d;
		d.name = "list_state_trees";
		d.description = "[state tree] List UStateTree assets under a content path "
						"(default `/Game`).";
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
			auto summaries = reader.ListStateTrees(path);
		auto ctl = ParseResponseControls(args);
			nlohmann::json arr = nlohmann::json::array();
			for (const auto& s : summaries)
			{
				arr.push_back(s);
			}
			return ListResponse(std::move(arr), ctl);
		});
	}

	{
		ToolDescriptor d;
		d.name = "read_state_tree";
		d.description = "[state tree] Read a UStateTree's hierarchy + transitions: "
						"every state (id, name, parent) and every "
						"transition (from, to, trigger).";
		d.input_schema = {
			{"type","object"},
			{"properties", {{"asset_path", {{"type","string"}}}}},
			{"required", nlohmann::json::array({"asset_path"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",         {{"type","boolean"}}},
				{"asset_path", {{"type","string"}}},
				{"states",     {{"type","array"}}},
				{"transitions",{{"type","array"}}},
			}},
			{"required", nlohmann::json::array({"ok","asset_path","states","transitions"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireAssetPath(args);
			auto st = reader.ReadStateTree(asset);
			nlohmann::json states = nlohmann::json::array();
			for (const auto& s : st.states) {
				states.push_back({
					{"state_id", s.stateId},
					{"name",     s.name},
					{"parent",   s.parentStateId},
				});
			}
			nlohmann::json trans = nlohmann::json::array();
			for (const auto& t : st.transitions) {
				trans.push_back({
					{"from",    t.fromStateId},
					{"to",      t.toStateId},
					{"trigger", t.trigger},
				});
			}
			return nlohmann::json{
				{"ok", true},
				{"asset_path",  st.assetPath},
				{"states",      states},
				{"transitions", trans},
			};
		});
	}

	{
		ToolDescriptor d;
		d.name = "add_state_tree_state";
		d.description = "[state tree] Add a state to a UStateTree. NOT to be confused with `add_anim_state` (AnimBP state machine). Empty "
						"`parent_state_id` makes it a top-level state. "
						"Returns the new state id.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path",      {{"type","string"}}},
				{"parent_state_id", {{"type","string"}}},
				{"name",            {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"asset_path","name"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",         {{"type","boolean"}}},
				{"asset_path", {{"type","string"}}},
				{"state_id",   {{"type","string"}}},
				{"name",       {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"ok","asset_path","state_id","name"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset  = RequireAssetPath(args);
			std::string parent = OptString(args, "parent_state_id", "");
			std::string name   = RequireString(args, "name");
			auto r = reader.AddStateTreeState(asset, parent, name);
			return nlohmann::json{
				{"ok", true},
				{"asset_path", r.assetPath},
				{"state_id",   r.stateId},
				{"name",       r.name},
			};
		});
	}

	{
		ToolDescriptor d;
		d.name = "set_state_tree_transition";
		d.description = "[state tree] Define a transition between two UStateTree states. `trigger` "
						"names the event class or tick condition "
						"(e.g. `OnTick`, `OnEvent.Damage`).";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path",    {{"type","string"}}},
				{"from_state_id", {{"type","string"}}},
				{"to_state_id",   {{"type","string"}}},
				{"trigger",       {{"type","string"}}},
			}},
			{"required", nlohmann::json::array(
				{"asset_path","from_state_id","to_state_id","trigger"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",            {{"type","boolean"}}},
				{"asset_path",    {{"type","string"}}},
				{"from_state_id", {{"type","string"}}},
				{"to_state_id",   {{"type","string"}}},
				{"trigger",       {{"type","string"}}},
				{"added",         {{"type","boolean"}}},
			}},
			{"required", nlohmann::json::array({"ok","asset_path","from_state_id","to_state_id","trigger","added"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireAssetPath(args);
			std::string from  = RequireString(args, "from_state_id");
			std::string to    = RequireString(args, "to_state_id");
			std::string trig  = RequireString(args, "trigger");
			auto r = reader.SetStateTreeTransition(asset, from, to, trig);
			return nlohmann::json{
				{"ok", true},
				{"asset_path",    r.assetPath},
				{"from_state_id", r.fromStateId},
				{"to_state_id",   r.toStateId},
				{"trigger",       r.trigger},
				{"added",         r.added},
			};
		});
	}

	{
		ToolDescriptor d;
		d.name = "compile_state_tree";
		d.description = "[state tree] Compile a UStateTree. Returns `{compiled: "
						"true|false}` — false means compile failed; check "
						"`read_output_log` for errors.";
		d.input_schema = {
			{"type","object"},
			{"properties", {{"asset_path", {{"type","string"}}}}},
			{"required", nlohmann::json::array({"asset_path"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",         {{"type","boolean"}}},
				{"asset_path", {{"type","string"}}},
				{"compiled",   {{"type","boolean"}}},
			}},
			{"required", nlohmann::json::array({"ok","asset_path","compiled"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireAssetPath(args);
			auto r = reader.CompileStateTree(asset);
			return nlohmann::json{
				{"ok", true},
				{"asset_path", r.assetPath},
				{"compiled",   r.compiled},
			};
		});
	}

	// ===== Profiling (Stage 3) =============================================

	{
		ToolDescriptor d;
		d.name = "start_profile";
		d.description = "[profiling] Start a profiling capture. `mode` selects the "
						"backend: `stats` (UE's built-in stat group "
						"file, default), `insights` (UnrealInsights "
						"trace), or `csv` (CSVProfiler). Returns "
						"`{started, output_file}` — the file path may be "
						"empty until `stop_profile` finalizes the capture.";
		d.input_schema = {
			{"type","object"},
			{"properties", {{"mode", {{"type","string"}}}}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string mode = OptString(args, "mode", "stats");
			auto r = reader.StartProfile(mode);
			return nlohmann::json{
				{"ok", true},
				{"started",     r.started},
				{"output_file", r.outputFile},
			};
		});
	}

	{
		ToolDescriptor d;
		d.name = "stop_profile";
		d.description = "[profiling] Stop the active profile capture and return its "
						"output file path. No-op if nothing is in progress.";
		d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
		registry.Add(std::move(d), [&reader](const nlohmann::json&) {
			auto r = reader.StopProfile();
			return nlohmann::json{
				{"ok", true},
				{"stopped",     r.stopped},
				{"output_file", r.outputFile},
			};
		});
	}

	{
		ToolDescriptor d;
		d.name = "get_stats";
		d.description = "[profiling] Toggle a stat group overlay in the editor. `group` "
						"is the name passed to UE's `stat` command "
						"(`Unit`, `Game`, `GPU`, `Memory`). The name reads like "
						"a getter but this is actually a stateful TOGGLE — "
						"first call enables the overlay, second call with the "
						"same `group` disables it. The numeric snapshot is "
						"NOT returned; `stat <group>` writes to the engine "
						"log overlay, and you need to capture it via "
						"`read_output_log` (or screenshot the viewport) after "
						"the toggle. Treat as a side-effecting probe, not a "
						"read.";
		d.input_schema = {
			{"type","object"},
			{"properties", {{"group", {{"type","string"}}}}},
			{"required", nlohmann::json::array({"group"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string g = RequireString(args, "group");
			auto r = reader.GetStats(g);
			return nlohmann::json{
				{"ok", true},
				{"group",    r.group},
				{"snapshot", r.snapshot},
			};
		});
	}

	{
		ToolDescriptor d;
		d.name = "take_screenshot";
		d.description = "[editor] Capture a high-res screenshot to disk. `dest_path` "
						"is the output file; `width`/`height` default to the "
						"current viewport size if omitted. Routed via UE's "
						"`HighResShot` exec command. Requires a rendering-capable "
						"(GPU) editor; a headless (-nullrhi) session returns "
						"`captured:false` with an explanatory `note`.\n\n"
						"Pass `return_inline: true` (default false) to also "
						"receive the PNG as an MCP Image content block — "
						"capped at 1280px max-dim; larger captures are rejected "
						"with a hint to re-capture smaller. "
						"BP_READER_NEVER_INLINE_IMAGES=1 forces classic path-only "
						"behaviour regardless of the arg.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"dest_path",     {{"type","string"}}},
				{"width",         {{"type","integer"}}},
				{"height",        {{"type","integer"}}},
				{"return_inline", {{"type","boolean"},
								   {"description","Emit the captured PNG as an Image content block "
													"(<=1280px max-dim). Default false."}}},
			}},
			{"required", nlohmann::json::array({"dest_path"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",          {{"type","boolean"}}},
				{"captured",    {{"type","boolean"}}},
				{"output_file", {{"type","string"}}},
				{"note",        {{"type","string"}}},
				{"image_width",  {{"type","integer"}}},
				{"image_height", {{"type","integer"}}},
			}},
			{"required", nlohmann::json::array({"ok","captured","output_file"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string dest = RequireString(args, "dest_path");
			RequireSafeFilePath(dest);  // refuse `..` traversal early
			int w = args.value("width",  0);
			int h = args.value("height", 0);
			const bool returnInline = args.value("return_inline", false);
			auto r = reader.TakeScreenshot(dest, w, h);
			return BuildScreenshotResponse(dest, r.captured, r.outputFile, returnInline, r.note);
		});
	}

	// ===== Headless cook / package (Stage 3) ==============================

	{
		ToolDescriptor d;
		d.name = "cook_content";
		d.description = "[cook] Run UE's content cook for a target platform "
						"(`Windows`, `Linux`, etc.). Asynchronous; the "
						"tool returns once the cook is dispatched. Follow "
						"progress via `read_output_log` or the editor's "
						"Cook Status panel.";
		d.input_schema = {
			{"type","object"},
			{"properties", {{"platform", {{"type","string"}}}}},
			{"required", nlohmann::json::array({"platform"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string p = RequireString(args, "platform");
			auto r = reader.CookContent(p);
			return nlohmann::json{
				{"ok", true},
				{"started",  r.started},
				{"platform", r.platform},
				{"message",  r.message},
			};
		});
	}

	{
		ToolDescriptor d;
		d.name = "package_project";
		d.description = "[cook] Package the project for a target platform via "
						"UAT. `output_dir` is where the packaged build "
						"lands. Async — tool returns once UAT is "
						"dispatched.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"platform",   {{"type","string"}}},
				{"output_dir", {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"platform","output_dir"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string p = RequireString(args, "platform");
			std::string o = RequireString(args, "output_dir");
			auto r = reader.PackageProject(p, o);
			return nlohmann::json{
				{"ok", true},
				{"started",  r.started},
				{"platform", r.platform},
				{"message",  r.message},
			};
		});
	}

	// ===== Class introspection / API docs (Stage 3) =======================

	{
		ToolDescriptor d;
		d.name = "get_class_info";
		d.description = "[class info] Inspect a UClass: parent + ancestor chain + every "
						"UPROPERTY + UFUNCTION. `class_name` is the short "
						"class name (e.g. `Actor`, `PlayerController`) or a "
						"full class path.\n\n"
						"By default, only members **declared on this class** are returned "
						"— inherited members are filtered out so the response stays small "
						"(UCharacter alone inherits ~100+ properties from Actor / Pawn / "
						"UObject). Pass `include_inherited: true` to get the full "
						"transitive surface; walk the `ancestors` chain with repeat "
						"`get_class_info` calls when you want layered detail. "
						"`limit`/`offset` page the properties + functions arrays "
						"(both share the window); `fields` projects the response.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"class_name", {{"type","string"}}},
				{"fields", FieldsProperty()},
				{"limit", LimitProperty()},
				{"offset", OffsetProperty()},
				{"include_inherited", {
					{"type","boolean"},
					{"description","Include properties and functions inherited from "
								   "ancestor classes. Default false (own members only) "
								   "to keep responses focused; set true for the full "
								   "transitive surface."},
				}},
			}},
			{"required", nlohmann::json::array({"class_name"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string n = RequireString(args, "class_name");
			const bool includeInherited = args.value("include_inherited", false);
			auto ctl = ParseResponseControls(args);
			auto ci = reader.IntrospectClass(n);
			// Filter using declared_on. When the plugin payload omits the
			// field (older backends, mock fixtures) we keep the row — the
			// alternative is silently dropping everything.
			auto keep = [&](const std::string& declaredOn) -> bool {
				if (includeInherited) {
					return true;
				}
				if (declaredOn.empty()) {
					return true;
				}
				return declaredOn == ci.className;
			};
			nlohmann::json props = nlohmann::json::array();
			for (const auto& p : ci.properties) {
				if (!keep(p.declaredOn)) {
					continue;
				}
				nlohmann::json row = {
					{"name",     p.name},
					{"type",     p.typeName},
					{"category", p.category},
				};
				if (!p.declaredOn.empty()) {
					row["declared_on"] = p.declaredOn;
				}
				props.push_back(std::move(row));
			}
			nlohmann::json fns = nlohmann::json::array();
			for (const auto& f : ci.functions) {
				if (!keep(f.declaredOn)) {
					continue;
				}
				nlohmann::json row = {
					{"name", f.name}, {"flags", f.flagsCsv}
				};
				if (!f.declaredOn.empty()) {
					row["declared_on"] = f.declaredOn;
				}
				fns.push_back(std::move(row));
			}
			nlohmann::json body = {
				{"ok", true},
				{"class",             ci.className},
				{"parent",            ci.parentClass},
				{"ancestors",         ci.ancestors},
				{"properties",        props},
				{"functions",         fns},
				{"include_inherited", includeInherited},
			};
			// UX-P2a: page the (potentially large) member arrays; then
			// sort/project/typo-warn via the shared response controls.
			PaginateField(body, "properties", ctl);
			PaginateField(body, "functions", ctl);
			return ListResponse(std::move(body), ctl);
		});
	}

	{
		ToolDescriptor d;
		d.name = "find_class";
		d.description = "[class info] Search the UClass registry by substring. Returns "
						"an array of class names matching `query` "
						"(case-insensitive). Compiler-generated companion classes "
						"(`SKEL_*`, `REINST_*`, `TRASHCLASS_*`) are filtered by "
						"default — pass `include_skeleton: true` to include them.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"query", {{"type","string"}}},
				{"include_skeleton", {
					{"type","boolean"},
					{"description","Include compiler-generated companion classes "
								   "(SKEL_*, REINST_*, TRASHCLASS_*). Default false."},
				}},
			}},
			{"required", nlohmann::json::array({"query"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string q = RequireString(args, "query");
			const bool includeSkeleton = args.value("include_skeleton", false);
			auto r = reader.FindClass(q);
			if (!includeSkeleton) {
				// SKEL_*: UBlueprint::SkeletonGeneratedClass — a "skeleton"
				// UClass the editor keeps for compilation. REINST_*:
				// reinstancer classes left behind by hot reload.
				// TRASHCLASS_*: trash bins for renamed classes pending GC.
				// Agents searching by class name almost never want these.
				auto isCompilerCompanion = [](std::string_view n) {
					return n.rfind("SKEL_", 0) == 0 ||
						   n.rfind("REINST_", 0) == 0 ||
						   n.rfind("TRASHCLASS_", 0) == 0;
				};
				r.classNames.erase(
					std::remove_if(r.classNames.begin(), r.classNames.end(),
								   isCompilerCompanion),
					r.classNames.end());
			}
			return nlohmann::json{{"ok", true}, {"classes", r.classNames}};
		});
	}

	{
		ToolDescriptor d;
		d.name = "list_functions";
		d.description = "[class info] List every UFUNCTION on a class with its flags "
						"(BlueprintCallable, BlueprintPure, etc.). Cheaper "
						"projection than `get_class_info` when you only "
						"need the call surface.";
		d.input_schema = {
			{"type","object"},
			{"properties", {{"class_name", {{"type","string"}}}}},
			{"required", nlohmann::json::array({"class_name"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string n = RequireString(args, "class_name");
			auto fns = reader.ListFunctions(n);
		auto ctl = ParseResponseControls(args);
			nlohmann::json arr = nlohmann::json::array();
			for (const auto& f : fns) {
				arr.push_back({{"name", f.name}, {"flags", f.flagsCsv}});
			}
			return ListResponse(std::move(arr), ctl);
		});
	}

	// ===== Viewport ergonomics (Stage 3) ==================================

	{
		ToolDescriptor d;
		d.name = "focus_actor";
		d.description = "[editor] Frame an actor in the editor viewport — equivalent "
						"to clicking the actor and pressing F. `actor_name` "
						"is the actor's level label.";
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
				{"focused",    {{"type","boolean"}}},
			}},
			{"required", nlohmann::json::array({"ok","actor_name","focused"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string n = RequireString(args, "actor_name");
			auto r = reader.FocusActor(n);
			return nlohmann::json{
				{"ok", true},
				{"actor_name", r.actorName},
				{"focused",    r.focused},
			};
		});
	}

	{
		ToolDescriptor d;
		d.name = "set_camera_transform";
		d.description = "[editor] Move the editor viewport camera to a "
						"specific location + rotation. Rotation is in "
						"degrees (pitch / yaw / roll).";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"loc_x", {{"type","number"}}},
				{"loc_y", {{"type","number"}}},
				{"loc_z", {{"type","number"}}},
				{"rot_pitch", {{"type","number"}}},
				{"rot_yaw",   {{"type","number"}}},
				{"rot_roll",  {{"type","number"}}},
			}},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",    {{"type","boolean"}}},
				{"moved", {{"type","boolean"}}},
			}},
			{"required", nlohmann::json::array({"ok","moved"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			auto r = reader.SetCameraTransform(
				args.value("loc_x", 0.0), args.value("loc_y", 0.0), args.value("loc_z", 0.0),
				args.value("rot_pitch", 0.0), args.value("rot_yaw", 0.0), args.value("rot_roll", 0.0));
			return nlohmann::json{{"ok", true}, {"moved", r.moved}};
		});
	}

	{
		ToolDescriptor d;
		d.name = "take_viewport_screenshot";
		d.description = "[editor] Quick capture of the active editor viewport to "
						"disk at native resolution (`take_screenshot` adds optional "
						"`width`/`height`). Both route through HighResShot in-editor "
						"(the game-only `Shot` command does not work there), and the "
						"capture is ASYNCHRONOUS — the PNG lands at `output_file` a "
						"frame or two after the call. Requires a GPU / -RenderOffscreen "
						"editor; a headless (-nullrhi) session returns `captured:false` "
						"with an explanatory `note`.\n\n"
						"Pass `return_inline: true` (default false) to also "
						"receive the PNG as an MCP Image content block — "
						"capped at 1280px max-dim; larger captures are rejected "
						"with a hint to re-capture smaller. "
						"BP_READER_NEVER_INLINE_IMAGES=1 forces classic path-only "
						"behaviour.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"dest_path",     {{"type","string"}}},
				{"return_inline", {{"type","boolean"},
								   {"description","Emit the captured PNG as an Image content block "
													"(<=1280px max-dim). Default false."}}},
			}},
			{"required", nlohmann::json::array({"dest_path"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",          {{"type","boolean"}}},
				{"captured",    {{"type","boolean"}}},
				{"output_file", {{"type","string"}}},
				{"note",        {{"type","string"}}},
				{"image_width",  {{"type","integer"}}},
				{"image_height", {{"type","integer"}}},
			}},
			{"required", nlohmann::json::array({"ok","captured","output_file"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string dest = RequireString(args, "dest_path");
			RequireSafeFilePath(dest);  // refuse `..` traversal early
			const bool returnInline = args.value("return_inline", false);
			auto r = reader.TakeViewportScreenshot(dest);
			return BuildScreenshotResponse(dest, r.captured, r.outputFile, returnInline, r.note);
		});
	}

}

void RegisterTools_07(ToolRegistry& registry, backends::IBlueprintReader& reader) {
	// ----- take_annotated_screenshot--------------------------------------
	// Captures the viewport + composes structured spatial metadata in one
	// call: camera transform, selected actors, optional broader editor
	// state. The image itself is written to disk (same as
	// take_viewport_screenshot); the *annotation* is the structured
	// metadata returned alongside the path, giving a vision agent enough
	// context to reason about what's in frame without separate calls to
	// get_camera_transform / get_selected_actors / get_editor_state.
	//
	// Matches the metadata shape of Epic 5.8's FViewportCapture struct
	// (camera + grid + labeled_actors) so a future tool revision can fill
	// in the per-actor screen positions + drawn-overlay image without
	// changing the wire schema. Today's response always sets
	// annotation_overlay_rendered=false to signal that the IMAGE is a raw
	// viewport capture; metadata is informational.
	{
		ToolDescriptor d;
		d.name = "take_annotated_screenshot";
		d.description =
			"[editor] Capture the viewport + return structured spatial metadata "
			"(camera transform, selected actors, optional editor state) so a "
			"vision-capable agent can reason about scene contents without "
			"separate `get_*` calls. The image is written to `dest_path` (same "
			"as take_viewport_screenshot); the annotation data is the structured "
			"metadata returned alongside. `annotation_overlay_rendered` is false "
			"in this version — image is a raw capture, future revisions may add "
			"a projected 3D grid + actor labels drawn into the image itself.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"dest_path", {{"type","string"},
							   {"description","Output file path for the captured image."}}},
				{"include_selected_actors", {{"type","boolean"},
											 {"description","Include selected actor metadata. Default true."}}},
				{"include_editor_state",    {{"type","boolean"},
											 {"description","Include editor state (camera, level, PIE status). Default true."}}},
			}},
			{"required", nlohmann::json::array({"dest_path"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",          {{"type","boolean"}}},
				{"captured",    {{"type","boolean"}}},
				{"output_file", {{"type","string"}}},
				{"annotation_overlay_rendered", {{"type","boolean"}}},
				{"camera", {
					{"type","object"},
					{"properties", {
						{"location", {{"type","array"},{"items",{{"type","number"}}}, {"description","[x,y,z] cm"}}},
						{"rotation", {{"type","array"},{"items",{{"type","number"}}}, {"description","[pitch,yaw,roll] deg"}}},
						{"fov",      {{"type","number"}}},
					}},
				}},
				{"selected_actors", {{"type","array"},
									 {"items", {{"type","object"},
												{"properties", {
													{"name",     {{"type","string"}}},
													{"label",    {{"type","string"}}},
													{"class",    {{"type","string"}}},
													{"location", {{"type","array"},{"items",{{"type","number"}}}}},
												}}}}}},
				{"editor_state", {{"type","object"}}},
			}},
			{"required", nlohmann::json::array({"ok","captured","output_file","annotation_overlay_rendered"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			const std::string dest = RequireString(args, "dest_path");
			RequireSafeFilePath(dest);  // refuse `..` traversal early
			const bool includeSelected = args.value("include_selected_actors", true);
			const bool includeEditorState = args.value("include_editor_state", true);

			auto cap = reader.TakeViewportScreenshot(dest);

			nlohmann::json out = {
				{"ok", true},
				{"captured",    cap.captured},
				{"output_file", cap.outputFile},
				// Future revisions can render a 3D grid + actor labels on top
				// of the image; until then, surface honestly that we didn't.
				{"annotation_overlay_rendered", false},
			};

			if (includeEditorState) {
				try {
					auto state = reader.GetEditorState();
					// Surface camera info (when present in the editor-state
					// payload) as a structured `camera` block; surface the
					// full editor state as `editor_state` for callers that
					// want the broader picture (active level, PIE status,
					// time of day, etc.).
					if (state.is_object()) {
						if (auto camIt = state.find("camera"); camIt != state.end()) {
							out["camera"] = *camIt;
						}
					}
					out["editor_state"] = std::move(state);
				} catch (const std::exception&) {
					// Best-effort — if GetEditorState isn't supported by the
					// active backend (or fails for any reason), the agent
					// still gets the image path. Don't fail the whole call.
				}
			}

			if (includeSelected) {
				try {
					auto sel = reader.GetSelectedActors();
					nlohmann::json actors = nlohmann::json::array();
					for (const auto& n : sel.actorNames) {
						actors.push_back(nlohmann::json{{"name", n}});
					}
					out["selected_actors"] = std::move(actors);
				} catch (const std::exception&) {
					out["selected_actors"] = nlohmann::json::array();
				}
			}

			return out;
		});
	}

	{
		ToolDescriptor d;
		d.name = "set_show_flag";
		d.description = "[editor] Toggle a viewport show flag (`Bones`, `Bounds`, "
						"`Collision`, `Wireframe`, `Lighting`). Equivalent "
						"to the `showflag.<name> <0|1>` console command.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"flag_name", {{"type","string"}}},
				{"enabled",   {{"type","boolean"}}},
			}},
			{"required", nlohmann::json::array({"flag_name","enabled"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",        {{"type","boolean"}}},
				{"flag_name", {{"type","string"}}},
				{"enabled",   {{"type","boolean"}}},
			}},
			{"required", nlohmann::json::array({"ok","flag_name","enabled"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string f = RequireString(args, "flag_name");
			bool e = args.value("enabled", true);
			auto r = reader.SetShowFlag(f, e);
			return nlohmann::json{
				{"ok", true},
				{"flag_name", r.flagName},
				{"enabled",   r.enabled},
			};
		});
	}

	// ===== Phase 14 — World + SCC + system state =========================

	// ----- get_async_compile_state -----
	{
		ToolDescriptor d;
		d.name = "get_async_compile_state";
		d.description =
			"[editor] Async asset-compilation backlog (textures, static "
			"meshes, etc.) via FAssetCompilingManager. `remaining_assets` "
			"is the aggregate still compiling; 0 means idle. Requires a "
			"live editor.";
		d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
		d.output_schema = {
			{"type","object"},
			{"properties", {{"remaining_assets", {{"type","integer"}}}}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json&) {
			auto r = reader.GetAsyncCompileState();
			return nlohmann::json{{"remaining_assets", r.remainingAssets}};
		});
	}

	// ----- get_shader_compile_state -----
	{
		ToolDescriptor d;
		d.name = "get_shader_compile_state";
		d.description =
			"[editor] Shader-compilation backlog via GShaderCompilingManager: "
			"`{is_compiling, outstanding_jobs, pending_jobs}`. Requires a "
			"live editor.";
		d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"is_compiling",     {{"type","boolean"}}},
				{"outstanding_jobs", {{"type","integer"}}},
				{"pending_jobs",     {{"type","integer"}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json&) {
			auto r = reader.GetShaderCompileState();
			return nlohmann::json{
				{"is_compiling",     r.isCompiling},
				{"outstanding_jobs", r.outstandingJobs},
				{"pending_jobs",     r.pendingJobs},
			};
		});
	}

	// ----- get_current_level -----
	{
		ToolDescriptor d;
		d.name = "get_current_level";
		d.description =
			"[editor] The editor's current level (where newly-spawned actors "
			"land) + owning world. Names are package paths. `valid:false` "
			"means no editor world. Requires a live editor.";
		d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"valid",      {{"type","boolean"}}},
				{"level_name", {{"type","string"}}},
				{"world_name", {{"type","string"}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json&) {
			auto r = reader.GetCurrentLevel();
			return nlohmann::json{
				{"valid",      r.valid},
				{"level_name", r.levelName},
				{"world_name", r.worldName},
			};
		});
	}

	// ----- list_loaded_levels -----
	{
		ToolDescriptor d;
		d.name = "list_loaded_levels";
		d.description =
			"[editor] All loaded levels (persistent + streaming sublevels) "
			"as package paths. Requires a live editor.";
		d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
		d.output_schema = {
			{"type","object"},
			{"properties", {{"level_names", {{"type","array"}, {"items", {{"type","string"}}}}}}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json&) {
			auto r = reader.ListLoadedLevels();
			return nlohmann::json{{"level_names", r.levelNames}};
		});
	}

	// ----- get_source_control_provider -----
	{
		ToolDescriptor d;
		d.name = "get_source_control_provider";
		d.description =
			"[editor] Active source-control provider: `{name, enabled, "
			"available}`. `name` is e.g. \"Git\"/\"Perforce\"/\"None\". "
			"Requires a live editor.";
		d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"name",      {{"type","string"}}},
				{"enabled",   {{"type","boolean"}}},
				{"available", {{"type","boolean"}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json&) {
			auto r = reader.GetSourceControlProvider();
			return nlohmann::json{
				{"name",      r.name},
				{"enabled",   r.enabled},
				{"available", r.available},
			};
		});
	}

}

void RegisterTools_08(ToolRegistry& registry, backends::IBlueprintReader& reader) {
	// ----- get_asset_registry_state-----
	{
		ToolDescriptor d;
		d.name = "get_asset_registry_state";
		d.description =
			"[editor] Asset-registry scan status: `{is_loading_assets, "
			"search_all_assets}`. `is_loading_assets:true` means the "
			"background scan is still running (asset queries may be "
			"incomplete). Requires a live editor.";
		d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"is_loading_assets", {{"type","boolean"}}},
				{"search_all_assets", {{"type","boolean"}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json&) {
			auto r = reader.GetAssetRegistryState();
			return nlohmann::json{
				{"is_loading_assets", r.isLoadingAssets},
				{"search_all_assets", r.searchAllAssets},
			};
		});
	}

	// ----- get_data_layer_states -----
	{
		ToolDescriptor d;
		d.name = "get_data_layer_states";
		d.description =
			"[editor] World Partition data layers + per-layer effective "
			"runtime state (Unloaded/Loaded/Activated). Each: `{short_name, "
			"full_name, runtime_state}`. `has_world_partition:false` on "
			"non-partitioned maps (layers empty). Requires a live editor.";
		d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"layers", {{"type","array"}, {"items", {{"type","object"}}}}},
				{"has_world_partition", {{"type","boolean"}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json&) {
			auto r = reader.GetDataLayerStates();
			nlohmann::json layers = nlohmann::json::array();
			for (const auto& l : r.layers) {
				layers.push_back({
					{"short_name",    l.shortName},
					{"full_name",     l.fullName},
					{"runtime_state", l.runtimeState},
				});
			}
			return nlohmann::json{
				{"layers", layers},
				{"has_world_partition", r.hasWorldPartition},
			};
		});
	}

	// ----- get_autosave_status -----
	{
		ToolDescriptor d;
		d.name = "get_autosave_status";
		d.description =
			"[editor] Editor autosave status: `{is_auto_saving}` (true "
			"while an autosave is in progress). Requires a live editor.";
		d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
		d.output_schema = {
			{"type","object"},
			{"properties", {{"is_auto_saving", {{"type","boolean"}}}}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json&) {
			auto r = reader.GetAutosaveStatus();
			return nlohmann::json{{"is_auto_saving", r.isAutoSaving}};
		});
	}

	// ----- get_recovery_state -----
	{
		ToolDescriptor d;
		d.name = "get_recovery_state";
		d.description =
			"[editor] Crash-recovery state: `{has_packages_to_restore}` "
			"(true when autosave restore files are pending from a prior "
			"unclean shutdown). Requires a live editor.";
		d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
		d.output_schema = {
			{"type","object"},
			{"properties", {{"has_packages_to_restore", {{"type","boolean"}}}}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json&) {
			auto r = reader.GetRecoveryState();
			return nlohmann::json{{"has_packages_to_restore", r.hasPackagesToRestore}};
		});
	}

	// ----- get_source_control_status -----
	{
		ToolDescriptor d;
		d.name = "get_source_control_status";
		d.description =
			"[editor] Per-file source-control status (cached): `{valid, "
			"controlled, checked_out, checked_out_other, modified, "
			"current}`. `valid:false` when SCC is disabled or the file has "
			"no cached state. Reads cached state only (no server round-"
			"trip). Requires a live editor.";
		d.input_schema = {
			{"type","object"},
			{"properties", {{"asset_path", {{"type","string"}}}}},
			{"required", nlohmann::json::array({"asset_path"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"valid",             {{"type","boolean"}}},
				{"controlled",        {{"type","boolean"}}},
				{"checked_out",       {{"type","boolean"}}},
				{"checked_out_other", {{"type","boolean"}}},
				{"modified",          {{"type","boolean"}}},
				{"current",           {{"type","boolean"}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string path = RequireAssetPath(args);
			auto r = reader.GetSourceControlStatus(path);
			return nlohmann::json{
				{"valid",             r.valid},
				{"controlled",        r.controlled},
				{"checked_out",       r.checkedOut},
				{"checked_out_other", r.checkedOutOther},
				{"modified",          r.modified},
				{"current",           r.current},
			};
		});
	}

	// ----- get_file_lock_status -----
	{
		ToolDescriptor d;
		d.name = "get_file_lock_status";
		d.description =
			"[editor] Whether an asset is checked out / locked by another "
			"user: `{valid, checked_out_by_other, other_user}`. Reads "
			"cached SCC state. Requires a live editor.";
		d.input_schema = {
			{"type","object"},
			{"properties", {{"asset_path", {{"type","string"}}}}},
			{"required", nlohmann::json::array({"asset_path"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"valid",                {{"type","boolean"}}},
				{"checked_out_by_other", {{"type","boolean"}}},
				{"other_user",           {{"type","string"}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string path = RequireAssetPath(args);
			auto r = reader.GetFileLockStatus(path);
			return nlohmann::json{
				{"valid",                r.valid},
				{"checked_out_by_other", r.checkedOutByOther},
				{"other_user",           r.otherUser},
			};
		});
	}

	// ===== Phase 17 — Advanced / niche editor state ======================

	// ----- get_active_culture -----
	{
		ToolDescriptor d;
		d.name = "get_active_culture";
		d.description =
			"[editor] Active editor culture/language: `{language, culture, "
			"display_name}` (e.g. en / en-US / \"English (United States)\"). "
			"Requires a live editor.";
		d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"language",     {{"type","string"}}},
				{"culture",      {{"type","string"}}},
				{"display_name", {{"type","string"}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json&) {
			auto r = reader.GetActiveCulture();
			return nlohmann::json{
				{"language",     r.language},
				{"culture",      r.culture},
				{"display_name", r.displayName},
			};
		});
	}

	// ----- get_editor_theme -----
	{
		ToolDescriptor d;
		d.name = "get_editor_theme";
		d.description =
			"[editor] Current editor theme id (`UEditorStyleSettings::"
			"CurrentAppliedTheme` GUID). Dark/Light ship with fixed GUIDs; "
			"custom themes get their own. Agents can detect theme changes "
			"by id. Requires a live editor.";
		d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
		d.output_schema = {
			{"type","object"},
			{"properties", {{"theme_id", {{"type","string"}}}}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json&) {
			auto r = reader.GetEditorTheme();
			return nlohmann::json{{"theme_id", r.themeId}};
		});
	}

	// ----- get_monitor_info -----
	{
		ToolDescriptor d;
		d.name = "get_monitor_info";
		d.description =
			"[editor] Connected monitors (FDisplayMetrics): each "
			"`{name, native_width, native_height, is_primary}`. Useful for "
			"multi-monitor placement reasoning. Requires a live editor.";
		d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
		d.output_schema = {
			{"type","object"},
			{"properties", {{"monitors", {{"type","array"}, {"items", {{"type","object"}}}}}}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json&) {
			auto r = reader.GetMonitors();
			nlohmann::json mons = nlohmann::json::array();
			for (const auto& m : r.monitors) {
				mons.push_back({
					{"name",          m.name},
					{"native_width",  m.nativeWidth},
					{"native_height", m.nativeHeight},
					{"is_primary",    m.isPrimary},
				});
			}
			return nlohmann::json{{"monitors", mons}};
		});
	}

	// ----- get_live_coding_state -----
	{
		ToolDescriptor d;
		d.name = "get_live_coding_state";
		d.description =
			"[editor] Live Coding (C++ hot-patch) state: `{available, "
			"has_started, is_compiling}`. `available:false` on non-Windows "
			"or when the module isn't loaded. Requires a live editor.";
		d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"available",    {{"type","boolean"}}},
				{"has_started",  {{"type","boolean"}}},
				{"is_compiling", {{"type","boolean"}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json&) {
			auto r = reader.GetLiveCodingState();
			return nlohmann::json{
				{"available",    r.available},
				{"has_started",  r.hasStarted},
				{"is_compiling", r.isCompiling},
			};
		});
	}

	// ----- get_streaming_sources (Phase 14 — World Partition) -----
	{
		ToolDescriptor d;
		d.name = "get_streaming_sources";
		d.description =
			"[editor] World Partition streaming sources (camera/player "
			"providers driving cell streaming). Each: `{name, loc_x/y/z, "
			"pitch, yaw, roll}`. `has_world_partition:false` on "
			"non-partitioned maps (sources empty). Requires a live editor.";
		d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"sources", {{"type","array"}, {"items", {{"type","object"}}}}},
				{"has_world_partition", {{"type","boolean"}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json&) {
			auto r = reader.GetStreamingSources();
			nlohmann::json srcs = nlohmann::json::array();
			for (const auto& s : r.sources) {
				srcs.push_back({
					{"name",  s.name},
					{"loc_x", s.locX}, {"loc_y", s.locY}, {"loc_z", s.locZ},
					{"pitch", s.pitch}, {"yaw", s.yaw}, {"roll", s.roll},
				});
			}
			return nlohmann::json{
				{"sources", srcs},
				{"has_world_partition", r.hasWorldPartition},
			};
		});
	}

	// ----- get_recently_opened_assets (Phase 14) -----
	{
		ToolDescriptor d;
		d.name = "get_recently_opened_assets";
		d.description =
			"[editor] Recently-opened assets from the editor MRU list, "
			"most-recent first (package paths). Reflects the current editor "
			"session's history. Requires a live editor.";
		d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
		d.output_schema = {
			{"type","object"},
			{"properties", {{"asset_paths", {{"type","array"}, {"items", {{"type","string"}}}}}}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json&) {
			auto r = reader.GetRecentlyOpenedAssets();
			return nlohmann::json{{"asset_paths", r.assetPaths}};
		});
	}

	// ----- set_plugin_enabled (Phase 11 H Tier 1 — PluginToolset write) ---
	{
		ToolDescriptor d;
		d.name = "set_plugin_enabled";
		d.description =
			"[editor] Enable or disable a plugin in the project's .uproject "
			"descriptor (IProjectManager). `applied` = the descriptor "
			"changed; `saved` = the .uproject was written. Takes effect on "
			"the next editor restart (modules load/unload at startup). "
			"Blocked in read-only mode (mutates the project file). Requires "
			"a live editor.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"plugin",  {{"type","string"}}},
				{"enabled", {{"type","boolean"}}},
			}},
			{"required", nlohmann::json::array({"plugin","enabled"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",      {{"type","boolean"}}},
				{"applied", {{"type","boolean"}}},
				{"saved",   {{"type","boolean"}}},
				{"message", {{"type","string"}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string plugin = RequireString(args, "plugin");
			bool enabled = args.value("enabled", true);
			auto r = reader.SetPluginEnabled(plugin, enabled);
			return nlohmann::json{
				{"ok",      true},
				{"applied", r.applied},
				{"saved",   r.saved},
				{"message", r.message},
			};
		});
	}

	// ----- get_recently_saved_packages (Phase 14) -----
	{
		ToolDescriptor d;
		d.name = "get_recently_saved_packages";
		d.description =
			"[editor] Packages saved during this editor session, most-recent "
			"first (package paths). Backed by a ring buffer populated from "
			"the package-saved event — empty in a fresh headless commandlet, "
			"accumulates in a live editor. Requires a live editor.";
		d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
		d.output_schema = {
			{"type","object"},
			{"properties", {{"package_paths", {{"type","array"}, {"items", {{"type","string"}}}}}}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json&) {
			auto r = reader.GetRecentlySavedPackages();
			return nlohmann::json{{"package_paths", r.packagePaths}};
		});
	}

	// ===== Phase 11 H Tier 1 — GameFeatures activate/deactivate (writes) ==

	// ----- activate_game_feature -----
	{
		ToolDescriptor d;
		d.name = "activate_game_feature";
		d.description =
			"[editor] Request activation of a Game Feature Plugin by name or "
			"file:-protocol URL (async, fire-and-forget). `requested:true` "
			"means load+activate was queued; poll `get_game_feature_state` to "
			"confirm. `requested:false` means the name didn't resolve to a "
			"GFP. Blocked in read-only mode. Requires a live editor.";
		d.input_schema = {
			{"type","object"},
			{"properties", {{"plugin", {{"type","string"}}}}},
			{"required", nlohmann::json::array({"plugin"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",        {{"type","boolean"}}},
				{"requested", {{"type","boolean"}}},
				{"url",       {{"type","string"}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string plugin = RequireString(args, "plugin");
			auto r = reader.ActivateGameFeature(plugin);
			return nlohmann::json{
				{"ok",        true},
				{"requested", r.requested},
				{"url",       r.url},
			};
		});
	}

	// ----- deactivate_game_feature -----
	{
		ToolDescriptor d;
		d.name = "deactivate_game_feature";
		d.description =
			"[editor] Request deactivation of a Game Feature Plugin by name "
			"or file:-protocol URL (async, fire-and-forget). `requested:true` "
			"means deactivation was queued; poll `get_game_feature_state` to "
			"confirm. `requested:false` means the name didn't resolve. "
			"Blocked in read-only mode. Requires a live editor.";
		d.input_schema = {
			{"type","object"},
			{"properties", {{"plugin", {{"type","string"}}}}},
			{"required", nlohmann::json::array({"plugin"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",        {{"type","boolean"}}},
				{"requested", {{"type","boolean"}}},
				{"url",       {{"type","string"}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string plugin = RequireString(args, "plugin");
			auto r = reader.DeactivateGameFeature(plugin);
			return nlohmann::json{
				{"ok",        true},
				{"requested", r.requested},
				{"url",       r.url},
			};
		});
	}

	// ----- get_active_stats (Phase 17) -----
	{
		ToolDescriptor d;
		d.name = "get_active_stats";
		d.description =
			"[editor] Stat overlays enabled in the active level viewport "
			"(e.g. `Unit`, `FPS`, `GPU`, `SceneRendering`) via "
			"FViewportClient::GetEnabledStats. `valid:false` when no "
			"viewport is focused. Requires a live editor.";
		d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"valid", {{"type","boolean"}}},
				{"stats", {{"type","array"}, {"items", {{"type","string"}}}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json&) {
			auto r = reader.GetActiveStats();
			return nlohmann::json{{"valid", r.valid}, {"stats", r.stats}};
		});
	}

	// ----- get_watched_pins (Phase 17) -----
	{
		ToolDescriptor d;
		d.name = "get_watched_pins";
		d.description =
			"[editor] Watched pins on a Blueprint (FKismetDebugUtilities). "
			"Each: `{pin_name, node_guid, node_name, direction}`. "
			"`valid:false` when the BP can't be loaded. Requires a live "
			"editor.";
		d.input_schema = {
			{"type","object"},
			{"properties", {{"asset_path", {{"type","string"}}}}},
			{"required", nlohmann::json::array({"asset_path"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"valid", {{"type","boolean"}}},
				{"pins",  {{"type","array"}, {"items", {{"type","object"}}}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string path = RequireAssetPath(args);
			auto r = reader.GetWatchedPins(path);
			nlohmann::json pins = nlohmann::json::array();
			for (const auto& p : r.pins) {
				pins.push_back({
					{"pin_name",  p.pinName},
					{"node_guid", p.nodeGuid},
					{"node_name", p.nodeName},
					{"direction", p.direction},
				});
			}
			return nlohmann::json{{"valid", r.valid}, {"pins", pins}};
		});
	}

	// ----- get_blueprint_breakpoints (Phase 17) -----
	{
		ToolDescriptor d;
		d.name = "get_blueprint_breakpoints";
		d.description =
			"[editor] Breakpoints set on a Blueprint (FKismetDebugUtilities). "
			"Each: `{node_guid, node_name, location, enabled}`. "
			"`valid:false` when the BP can't be loaded. Requires a live "
			"editor.";
		d.input_schema = {
			{"type","object"},
			{"properties", {{"asset_path", {{"type","string"}}}}},
			{"required", nlohmann::json::array({"asset_path"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"valid",       {{"type","boolean"}}},
				{"breakpoints", {{"type","array"}, {"items", {{"type","object"}}}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string path = RequireAssetPath(args);
			auto r = reader.GetBlueprintBreakpoints(path);
			nlohmann::json bps = nlohmann::json::array();
			for (const auto& b : r.breakpoints) {
				bps.push_back({
					{"node_guid", b.nodeGuid},
					{"node_name", b.nodeName},
					{"location",  b.location},
					{"enabled",   b.enabled},
				});
			}
			return nlohmann::json{{"valid", r.valid}, {"breakpoints", bps}};
		});
	}

	// ----- get_debug_instance (Phase 17) -----
	{
		ToolDescriptor d;
		d.name = "get_debug_instance";
		d.description =
			"[editor] PIE-attached debug object for a Blueprint "
			"(`UBlueprint::GetObjectBeingDebugged`): `{valid, "
			"has_debug_object, debug_object_name, debug_object_path}`. "
			"`has_debug_object:false` when nothing is attached (e.g. not in "
			"PIE). Requires a live editor.";
		d.input_schema = {
			{"type","object"},
			{"properties", {{"asset_path", {{"type","string"}}}}},
			{"required", nlohmann::json::array({"asset_path"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"valid",             {{"type","boolean"}}},
				{"has_debug_object",  {{"type","boolean"}}},
				{"debug_object_name", {{"type","string"}}},
				{"debug_object_path", {{"type","string"}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string path = RequireAssetPath(args);
			auto r = reader.GetDebugInstance(path);
			return nlohmann::json{
				{"valid",             r.valid},
				{"has_debug_object",  r.hasDebugObject},
				{"debug_object_name", r.debugObjectName},
				{"debug_object_path", r.debugObjectPath},
			};
		});
	}

	// ===== Phase 16 H Tier 2 — ConfigSettings nav =========================

}

void RegisterTools_08b(ToolRegistry& registry, backends::IBlueprintReader& reader) {
	// ----- list_project_settings -----
	{
		ToolDescriptor d;
		d.name = "list_project_settings";
		d.description =
			"[editor] All Project/Editor Settings sections (UDeveloperSettings "
			"CDOs): each `{container, category, section, class_path}`. The "
			"3-tier nav read (Container -> Category -> Section) that future "
			"get/set/save/reset settings tools drill into. Requires a live "
			"editor.";
		d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
		d.output_schema = {
			{"type","object"},
			{"properties", {{"sections", {{"type","array"}, {"items", {{"type","object"}}}}}}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json&) {
			auto r = reader.ListProjectSettings();
			nlohmann::json secs = nlohmann::json::array();
			for (const auto& s : r.sections) {
				secs.push_back({
					{"container",  s.container},
					{"category",   s.category},
					{"section",    s.section},
					{"class_path", s.classPath},
				});
			}
			return nlohmann::json{{"sections", secs}};
		});
	}

	// ----- get_project_setting_values -----
	{
		ToolDescriptor d;
		d.name = "get_project_setting_values";
		d.description =
			"[editor] All property values of one settings section "
			"(UDeveloperSettings CDO), each `{name, value, type}` where "
			"`value` is the reflection-exported text. `class_path` comes "
			"from `list_project_settings`. `valid:false` when the class "
			"can't be resolved. Requires a live editor.";
		d.input_schema = {
			{"type","object"},
			{"properties", {{"class_path", {{"type","string"}}}}},
			{"required", nlohmann::json::array({"class_path"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"valid",      {{"type","boolean"}}},
				{"class_path", {{"type","string"}}},
				{"values",     {{"type","array"}, {"items", {{"type","object"}}}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string cp = RequireString(args, "class_path");
			auto r = reader.GetProjectSettingValues(cp);
			nlohmann::json vals = nlohmann::json::array();
			for (const auto& v : r.values) {
				vals.push_back({{"name", v.name}, {"value", v.value}, {"type", v.type}});
			}
			return nlohmann::json{
				{"valid",      r.valid},
				{"class_path", r.classPath},
				{"values",     vals},
			};
		});
	}

	// ----- set_project_setting -----
	{
		ToolDescriptor d;
		d.name = "set_project_setting";
		d.description =
			"[editor] Set one property on a settings section by `class_path` "
			"+ `property` name, importing `value` from text, then persisting "
			"to the class's Default*.ini. `applied:true` on success; "
			"`message` carries the failure detail otherwise. Best for scalar "
			"values (numbers/bools/enums/simple strings) — complex struct "
			"text may not survive arg parsing. Blocked in read-only mode. "
			"Requires a live editor.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"class_path", {{"type","string"}}},
				{"property",   {{"type","string"}}},
				{"value",      {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"class_path","property","value"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",      {{"type","boolean"}}},
				{"applied", {{"type","boolean"}}},
				{"message", {{"type","string"}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string cp = RequireString(args, "class_path");
			std::string prop = RequireString(args, "property");
			std::string val = args.value("value", std::string{});
			auto r = reader.SetProjectSetting(cp, prop, val);
			return nlohmann::json{
				{"ok",      true},
				{"applied", r.applied},
				{"message", r.message},
			};
		});
	}

	// ----- list_automation_tests (Phase 16 — AutomationTest discovery) ----
	{
		ToolDescriptor d;
		d.name = "list_automation_tests";
		d.description =
			"[editor] Registered automation tests (FAutomationTestFramework, "
			"synchronous). Each `{display_name, full_path, test_name}` — "
			"`full_path`/`test_name` feed the `pattern` arg of "
			"`run_automation_tests`. Capped at 2000 (`truncated:true` if "
			"more). Requires a live editor.";
		d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"tests",     {{"type","array"}, {"items", {{"type","object"}}}}},
				{"truncated", {{"type","boolean"}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json&) {
			auto r = reader.ListAutomationTests();
			nlohmann::json tests = nlohmann::json::array();
			for (const auto& t : r.tests) {
				tests.push_back({
					{"display_name", t.displayName},
					{"full_path",    t.fullPath},
					{"test_name",    t.testName},
				});
			}
			return nlohmann::json{{"tests", tests}, {"truncated", r.truncated}};
		});
	}

	// ----- get_editor_events (Phase 10 — EA-push event sources) -----
	{
		ToolDescriptor d;
		d.name = "get_editor_events";
		d.description =
			"[editor] Drain buffered editor events captured from UE delegates. "
			"Covers selection changes, asset open/remove/rename, PIE "
			"start/stop/pause/resume, package save, map open, actor "
			"add/delete, and Blueprint compile — each `{name, params}`. "
			"Draining clears the buffer. Empty in a one-shot commandlet; "
			"accumulates in a live/daemon editor and is also auto-pushed over "
			"the HTTP SSE stream as notifications. Requires a live editor.";
		d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
		d.output_schema = {
			{"type","object"},
			{"properties", {{"events", {{"type","array"}, {"items", {{"type","object"}}}}}}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json&) {
			auto r = reader.GetEditorEvents();
			nlohmann::json events = nlohmann::json::array();
			for (const auto& e : r.events) {
				nlohmann::json params = nlohmann::json::object();
				try { params = nlohmann::json::parse(e.paramsJson); } catch (...) {}
				events.push_back({{"name", e.name}, {"params", params}});
			}
			return nlohmann::json{{"events", events}};
		});
	}

	// ----- get_active_cook_target (Phase 14 — system state) -----
	{
		ToolDescriptor d;
		d.name = "get_active_cook_target";
		d.description =
			"[editor] List the editor's active cook target platforms and the "
			"running platform, via ITargetPlatformManagerModule. `platforms` is "
			"the set UBT/the cooker would target; `running_platform` is the "
			"host the editor itself runs on (e.g. Windows). Empty when the "
			"TargetPlatform module is unavailable. Requires a live editor.";
		d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"platforms", {{"type","array"}, {"items", {{"type","string"}}}}},
				{"running_platform", {{"type","string"}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json&) {
			auto r = reader.GetActiveCookTarget();
			nlohmann::json platforms = nlohmann::json::array();
			for (const auto& p : r.platforms) platforms.push_back(p);
			return nlohmann::json{
				{"platforms", platforms},
				{"running_platform", r.runningPlatform},
			};
		});
	}

	// ----- get_workspace_layout (Phase 17 — advanced state) -----
	{
		ToolDescriptor d;
		d.name = "get_workspace_layout";
		d.description =
			"[editor] The editor's current docking/workspace layout, serialized "
			"via FGlobalTabmanager::PersistLayout (`layout` string). Reflects "
			"which tabs/panels are open and how they're arranged. Empty when "
			"Slate isn't initialized (e.g. a -nullrhi commandlet); populated in "
			"a live editor. Requires a live editor for meaningful output.";
		d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
		d.output_schema = {
			{"type","object"},
			{"properties", {{"layout", {{"type","string"}}}}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json&) {
			auto r = reader.GetWorkspaceLayout();
			return nlohmann::json{{"layout", r.layout}};
		});
	}

	// ----- get_trace_state (Phase 17 — advanced state) -----
	{
		ToolDescriptor d;
		d.name = "get_trace_state";
		d.description =
			"[editor] Unreal Insights trace connection state via FTraceAuxiliary "
			"(Core): `connected`, `paused`, `connection_type` "
			"(network|file|none), `destination` (host or file path), and "
			"`active_channels` (comma-separated enabled trace channels). Reports "
			"the responding process's own trace state (the live editor when "
			"routed live; the commandlet otherwise).";
		d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"connected",       {{"type","boolean"}}},
				{"paused",          {{"type","boolean"}}},
				{"connection_type", {{"type","string"}}},
				{"destination",     {{"type","string"}}},
				{"active_channels", {{"type","string"}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json&) {
			auto r = reader.GetTraceState();
			return nlohmann::json{
				{"connected",       r.connected},
				{"paused",          r.paused},
				{"connection_type", r.connectionType},
				{"destination",     r.destination},
				{"active_channels", r.activeChannels},
			};
		});
	}

	// ----- Editor UI-state surfaces (Phase 14/17 — documented v1 stubs) ---
	// These live entirely in transient Slate widgets with no out-of-process
	// bridge today, so they return `valid:false` via the shared
	// GetUiStateStub responder (same pattern as get_hover_target /
	// get_isolate_mode). The tool name + output shape are the stable
	// contract for when an in-editor Slate-introspection op fills them in.
	{
		struct UiStub { const char* name; const char* feature; const char* desc; };
		static const UiStub kUiStubs[] = {
			{"get_outliner_state", "outliner",
			 "[editor] World Outliner UI state (search text, type filter, row "
			 "expansion, columns). v1 stub: outliner state lives in transient "
			 "Slate widgets not bridged out-of-process — returns valid:false."},
			{"get_pinned_actors", "pinned_actors",
			 "[editor] Actors pinned in the World Outliner. v1 stub: pin state "
			 "is Slate-widget-local and not bridged out-of-process — returns "
			 "valid:false."},
			{"get_details_panel_state", "details_panel",
			 "[editor] Details panel UI state (search filter, expanded "
			 "categories, pinned properties). v1 stub: this is transient "
			 "SDetailsView state not bridged out-of-process — returns "
			 "valid:false. (Selected objects are available via "
			 "get_selected_actors.)"},
			{"get_status_bar_messages", "status_bar",
			 "[editor] Active status-bar message text. v1 stub: the status bar "
			 "API is push-only (no public getter) — returns valid:false."},
			{"get_active_notifications", "notifications",
			 "[editor] Active toast notifications (FSlateNotificationManager). "
			 "v1 stub: the manager exposes no public enumeration of live "
			 "notifications — returns valid:false."},
			{"get_modeling_state", "modeling",
			 "[editor] Modeling Tools Editor Mode state (active tool, "
			 "sub-element selection mode: vertex/edge/face/group). v1 stub: "
			 "this is interactive-tools-context state internal to the mode "
			 "manager, not bridged out-of-process — returns valid:false."},
			{"get_landscape_paint_state", "landscape_paint",
			 "[editor] Landscape edit mode paint state (active brush/target "
			 "layer). v1 stub: landscape mode state is not bridged "
			 "out-of-process — returns valid:false."},
			{"get_foliage_paint_state", "foliage_paint",
			 "[editor] Foliage edit mode paint state (selected foliage types, "
			 "brush settings). v1 stub: foliage mode state is not bridged "
			 "out-of-process — returns valid:false."},
			{"get_mesh_paint_state", "mesh_paint",
			 "[editor] Mesh paint mode state (color/weight/texture sub-mode, "
			 "selected channel). v1 stub: mesh-paint mode state is not bridged "
			 "out-of-process — returns valid:false."},
			{"get_texture_paint_state", "texture_paint",
			 "[editor] Texture paint mode state (selected texture/UV channel). "
			 "v1 stub: texture-paint mode state is not bridged out-of-process "
			 "— returns valid:false."},
			// Phase 14 system-state / SCC tails. Demand-driven surfaces with
			// no clean out-of-process API (or env-limited); v1 stubs.
			{"get_cook_progress", "cook_progress",
			 "[editor] In-editor cook progress. v1 stub: no in-editor cook "
			 "session exists to report by default, and there's no clean "
			 "progress API — returns valid:false."},
			{"get_ddc_state", "ddc_state",
			 "[editor] Derived Data Cache state/usage. v1 stub: the DDC "
			 "interface exposes only get/put, no public usage-stats accessor "
			 "— returns valid:false."},
			{"get_lighting_build_progress", "lighting_build",
			 "[editor] Static lighting build progress. v1 stub: no clean "
			 "global lighting-build progress accessor — returns valid:false."},
			{"set_active_cook_target", "set_cook_target",
			 "[editor] Set the active cook target platforms. v1 stub: "
			 "ITargetPlatformManager has no runtime setter (active platforms "
			 "derive from the launch command line) — returns valid:false. Read "
			 "the current set with get_active_cook_target."},
			{"list_loaded_partition_cells", "partition_cells",
			 "[editor] Loaded World Partition runtime cells. v1 stub: cell "
			 "enumeration uses partly-internal WorldPartition runtime-hash "
			 "APIs not yet wired — returns valid:false. (get_streaming_sources "
			 "+ get_world_partition_state cover the queryable parts.)"},
			{"list_changelists", "changelists",
			 "[editor] Source-control changelists. v1 stub: changelists are "
			 "Perforce-centric (ISourceControlProvider::GetChangelists returns "
			 "empty for Git/None); a real enumeration is pending — returns "
			 "valid:false."},
			{"get_pending_changelist", "pending_changelist",
			 "[editor] The default/pending source-control changelist. v1 stub: "
			 "Perforce-centric (empty for Git/None) — returns valid:false."},
			// Phase 17 plugin-backed surfaces — real state needs linking the
			// respective editor plugin module; v1 stubs until then.
			{"get_take_recorder_state", "take_recorder",
			 "[editor] Take Recorder state (recording active, current take). "
			 "v1 stub: a real implementation must link the TakeRecorder plugin "
			 "module (UTakeRecorder::GetActiveRecorder) — returns valid:false."},
			{"get_render_queue", "render_queue",
			 "[editor] Movie Render Queue pending jobs. v1 stub: a real "
			 "implementation must link the MovieRenderPipeline plugin module "
			 "— returns valid:false."},
			// Phase 16 ConfigSettings — reset-to-default. v1 stub.
			{"reset_project_setting", "reset_project_setting",
			 "[editor] Reset a project setting to its default. v1 stub: a "
			 "correct factory-reset needs the editor details-panel's "
			 "default-value resolution — a config property's constructor-set "
			 "default is unrecoverable out-of-process after config load "
			 "(RemoveKey+ReloadConfig only drops the persisted override, "
			 "leaving the in-memory value unchanged). Use set_project_setting "
			 "to assign explicit values; get/set/save round-trip works."},
		};
		for (const auto& s : kUiStubs) {
			ToolDescriptor d;
			d.name = s.name;
			d.description = s.desc;
			d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
			d.output_schema = {
				{"type","object"},
				{"properties", {
					{"valid",  {{"type","boolean"}}},
					{"reason", {{"type","string"}}},
				}},
			};
			const std::string feature = s.feature;
			registry.Add(std::move(d), [&reader, feature](const nlohmann::json&) {
				auto r = reader.GetUiStateStub(feature);
				return nlohmann::json{{"valid", r.valid}, {"reason", r.reason}};
			});
		}
	}

	// ===== Niagara (Stage 4) ===============================================

	{
		ToolDescriptor d;
		d.name = "list_niagara_systems";
		d.description = "[niagara] List UNiagaraSystem assets under a content path "
						"(default `/Game`).";
		d.input_schema = {{"type","object"},
			{"properties", {{"path", {{"type","string"}}}}}};
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
			auto s = reader.ListNiagaraSystems(path);
		auto ctl = ParseResponseControls(args);
			nlohmann::json arr = nlohmann::json::array();
			for (const auto& v : s)
			{
				arr.push_back(v);
			}
			return ListResponse(std::move(arr), ctl);
		});
	}

	{
		ToolDescriptor d;
		d.name = "read_niagara_system";
		d.description = "[niagara] Read a UNiagaraSystem's emitter handles (each "
						"names an underlying UNiagaraEmitter) and its "
						"exposed user parameter names.";
		d.input_schema = {{"type","object"},
			{"properties", {{"asset_path", {{"type","string"}}}}},
			{"required", nlohmann::json::array({"asset_path"})}};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",              {{"type","boolean"}}},
				{"asset_path",      {{"type","string"}}},
				{"emitters",        {{"type","array"}}},
				{"parameter_names", {{"type","array"}}},
			}},
			{"required", nlohmann::json::array({"ok","asset_path","emitters","parameter_names"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireAssetPath(args);
			auto n = reader.ReadNiagaraSystem(asset);
			nlohmann::json emitters = nlohmann::json::array();
			for (const auto& e : n.emitters) {
				emitters.push_back({
					{"name",         e.name},
					{"emitter_path", e.emitterPath},
					{"enabled",      e.enabled},
				});
			}
			return nlohmann::json{
				{"ok", true},
				{"asset_path",      n.assetPath},
				{"emitters",        emitters},
				{"parameter_names", n.parameterNames},
			};
		});
	}

	{
		ToolDescriptor d;
		d.name = "create_niagara_system";
		d.description = "[niagara] Create a new (empty) UNiagaraSystem asset at "
						"the given path. Idempotent.";
		d.input_schema = {{"type","object"},
			{"properties", {{"asset_path", {{"type","string"}}}}},
			{"required", nlohmann::json::array({"asset_path"})}};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",             {{"type","boolean"}}},
				{"asset_path",     {{"type","string"}}},
				{"created",        {{"type","boolean"}}},
				{"already_existed",{{"type","boolean"}}},
			}},
			{"required", nlohmann::json::array({"ok","asset_path","created","already_existed"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireAssetPath(args);
			auto r = reader.CreateNiagaraSystem(asset);
			return nlohmann::json{
				{"ok", true},
				{"asset_path",      r.assetPath},
				{"created",         r.created},
				{"already_existed", r.alreadyExisted},
			};
		});
	}

	{
		ToolDescriptor d;
		d.name = "set_niagara_parameter";
		d.description = "[niagara] Override a user-exposed parameter on a "
						"UNiagaraSystem. `value` is the parameter's text form.";
		d.input_schema = {{"type","object"},
			{"properties", {
				{"asset_path",     {{"type","string"}}},
				{"parameter_name", {{"type","string"}}},
				{"value",          {{"type","string"}}},
			}},
			{"required", nlohmann::json::array(
				{"asset_path","parameter_name","value"})}};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",             {{"type","boolean"}}},
				{"asset_path",     {{"type","string"}}},
				{"parameter_name", {{"type","string"}}},
				{"new_value",      {{"type","string"}}},
				{"applied",        {{"type","boolean"}}},
			}},
			{"required", nlohmann::json::array({"ok","asset_path","parameter_name","new_value","applied"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireAssetPath(args);
			std::string p     = RequireString(args, "parameter_name");
			std::string v     = RequireString(args, "value");
			auto r = reader.SetNiagaraParameter(asset, p, v);
			return nlohmann::json{
				{"ok", true},
				{"asset_path",     r.assetPath},
				{"parameter_name", r.parameterName},
				{"new_value",      r.newValue},
				{"applied",        r.applied},
			};
		});
	}

	// ===== LevelSequence (Stage 4) ========================================

	{
		ToolDescriptor d;
		d.name = "list_level_sequences";
		d.description = "[level sequence] List ULevelSequence assets under a content path.";
		d.input_schema = {{"type","object"},
			{"properties", {{"path", {{"type","string"}}}}}};
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
			auto s = reader.ListLevelSequences(path);
		auto ctl = ParseResponseControls(args);
			nlohmann::json arr = nlohmann::json::array();
			for (const auto& v : s)
			{
				arr.push_back(v);
			}
			return ListResponse(std::move(arr), ctl);
		});
	}

	{
		ToolDescriptor d;
		d.name = "read_level_sequence";
		d.description = "[level sequence] Read a sequence's playback range (start/end "
						"seconds) and its top-level tracks.";
		d.input_schema = {{"type","object"},
			{"properties", {{"asset_path", {{"type","string"}}}}},
			{"required", nlohmann::json::array({"asset_path"})}};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",            {{"type","boolean"}}},
				{"asset_path",    {{"type","string"}}},
				{"start_seconds", {{"type","number"}}},
				{"end_seconds",   {{"type","number"}}},
				{"tracks",        {{"type","array"}}},
			}},
			{"required", nlohmann::json::array({"ok","asset_path","start_seconds","end_seconds","tracks"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireAssetPath(args);
			auto ls = reader.ReadLevelSequence(asset);
			nlohmann::json tracks = nlohmann::json::array();
			for (const auto& t : ls.tracks) {
				tracks.push_back({
					{"name",          t.trackName},
					{"class",         t.trackClass},
					{"section_count", t.sectionCount},
				});
			}
			return nlohmann::json{
				{"ok", true},
				{"asset_path",    ls.assetPath},
				{"start_seconds", ls.startSeconds},
				{"end_seconds",   ls.endSeconds},
				{"tracks",        tracks},
			};
		});
	}

	{
		ToolDescriptor d;
		d.name = "add_sequence_track";
		d.description = "[level sequence] Add a master track to a ULevelSequence (Sequencer). "
						"`track_class` is the short class name "
						"(`MovieSceneAudioTrack`, `MovieScene3DTransformTrack`).";
		d.input_schema = {{"type","object"},
			{"properties", {
				{"asset_path",  {{"type","string"}}},
				{"track_class", {{"type","string"}}},
				{"track_name",  {{"type","string"}}},
			}},
			{"required", nlohmann::json::array(
				{"asset_path","track_class","track_name"})}};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",          {{"type","boolean"}}},
				{"asset_path",  {{"type","string"}}},
				{"track_name",  {{"type","string"}}},
				{"track_class", {{"type","string"}}},
				{"added",       {{"type","boolean"}}},
			}},
			{"required", nlohmann::json::array({"ok","asset_path","track_name","track_class","added"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireAssetPath(args);
			std::string c     = RequireString(args, "track_class");
			std::string n     = RequireString(args, "track_name");
			auto r = reader.AddSequenceTrack(asset, c, n);
			return nlohmann::json{
				{"ok", true},
				{"asset_path",  r.assetPath},
				{"track_name",  r.trackName},
				{"track_class", r.trackClass},
				{"added",       r.added},
			};
		});
	}

	{
		ToolDescriptor d;
		d.name = "set_sequence_playback_range";
		d.description = "[level sequence] Set the playback range of a "
						"ULevelSequence in seconds (start <= end).";
		d.input_schema = {{"type","object"},
			{"properties", {
				{"asset_path",    {{"type","string"}}},
				{"start_seconds", {{"type","number"}}},
				{"end_seconds",   {{"type","number"}}},
			}},
			{"required", nlohmann::json::array(
				{"asset_path","start_seconds","end_seconds"})}};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",            {{"type","boolean"}}},
				{"asset_path",    {{"type","string"}}},
				{"start_seconds", {{"type","number"}}},
				{"end_seconds",   {{"type","number"}}},
				{"applied",       {{"type","boolean"}}},
			}},
			{"required", nlohmann::json::array({"ok","asset_path","start_seconds","end_seconds","applied"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireAssetPath(args);
			double s = args.value("start_seconds", 0.0);
			double e = args.value("end_seconds",   0.0);
			auto r = reader.SetSequencePlaybackRange(asset, s, e);
			return nlohmann::json{
				{"ok", true},
				{"asset_path",    r.assetPath},
				{"start_seconds", r.startSeconds},
				{"end_seconds",   r.endSeconds},
				{"applied",       r.applied},
			};
		});
	}

	// ===== GAS / GameplayTags (Stage 4) ===================================

	{
		ToolDescriptor d;
		d.name = "list_gameplay_tags";
		d.description = "[gameplay tag] Query the project's GameplayTagsManager. `filter` "
						"is an optional substring; empty returns every "
						"registered tag.";
		d.input_schema = {{"type","object"},
			{"properties", {{"filter", {{"type","string"}}}}}};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string f = OptString(args, "filter", "");
			auto r = reader.ListGameplayTags(f);
			return nlohmann::json{{"ok", true}, {"tags", r.tags}};
		});
	}

	{
		ToolDescriptor d;
		d.name = "add_gameplay_tag";
		d.description = "[gameplay tag] Add a tag to the project's gameplay tag dictionary. `name` "
						"uses dot-separated form (e.g. `Status.Damage.Fire`). "
						"`comment` is optional.";
		d.input_schema = {{"type","object"},
			{"properties", {
				{"name",    {{"type","string"}}},
				{"comment", {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"name"})}};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",             {{"type","boolean"}}},
				{"tag_name",       {{"type","string"}}},
				{"added",          {{"type","boolean"}}},
				{"already_existed",{{"type","boolean"}}},
			}},
			{"required", nlohmann::json::array({"ok","tag_name","added","already_existed"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string n = RequireString(args, "name");
			std::string c = OptString(args, "comment", "");
			auto r = reader.AddGameplayTag(n, c);
			return nlohmann::json{
				{"ok", true},
				{"tag_name",        r.tagName},
				{"added",           r.added},
				{"already_existed", r.alreadyExisted},
			};
		});
	}

	{
		ToolDescriptor d;
		d.name = "read_ability_set";
		d.description = "[gameplay tag] Read a GAS ability-set DataAsset: every granted "
						"ability class + its level.";
		d.input_schema = {{"type","object"},
			{"properties", {{"asset_path", {{"type","string"}}}}},
			{"required", nlohmann::json::array({"asset_path"})}};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",         {{"type","boolean"}}},
				{"asset_path", {{"type","string"}}},
				{"abilities",  {{"type","array"}}},
			}},
			{"required", nlohmann::json::array({"ok","asset_path","abilities"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireAssetPath(args);
			auto s = reader.ReadAbilitySet(asset);
			nlohmann::json abilities = nlohmann::json::array();
			for (const auto& a : s.abilities) {
				abilities.push_back({{"class", a.abilityClass}, {"level", a.level}});
			}
			return nlohmann::json{
				{"ok", true},
				{"asset_path", s.assetPath},
				{"abilities",  abilities},
			};
		});
	}

	// ===== AnimGraph (Stage 4) ============================================

	{
		ToolDescriptor d;
		d.name = "list_anim_blueprints";
		d.description = "[anim] List UAnimBlueprint assets under a content path.";
		d.input_schema = {{"type","object"},
			{"properties", {{"path", {{"type","string"}}}}}};
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
			auto s = reader.ListAnimBlueprints(path);
		auto ctl = ParseResponseControls(args);
			nlohmann::json arr = nlohmann::json::array();
			for (const auto& v : s)
			{
				arr.push_back(v);
			}
			return ListResponse(std::move(arr), ctl);
		});
	}

	{
		ToolDescriptor d;
		d.name = "read_anim_blueprint";
		d.description = "[anim] Walk a UAnimBlueprint: parent class + each "
						"state machine's states (state / conduit / "
						"transition / entry).";
		d.input_schema = {{"type","object"},
			{"properties", {{"asset_path", {{"type","string"}}}}},
			{"required", nlohmann::json::array({"asset_path"})}};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",             {{"type","boolean"}}},
				{"asset_path",     {{"type","string"}}},
				{"parent_class",   {{"type","string"}}},
				{"state_machines", {{"type","array"}}},
			}},
			{"required", nlohmann::json::array({"ok","asset_path","parent_class","state_machines"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireAssetPath(args);
			auto a = reader.ReadAnimBlueprint(asset);
			nlohmann::json machines = nlohmann::json::array();
			for (const auto& m : a.stateMachines) {
				nlohmann::json states = nlohmann::json::array();
				for (const auto& s : m.states) {
					states.push_back({{"name", s.name}, {"kind", s.kind}});
				}
				machines.push_back({{"name", m.name}, {"states", states}});
			}
			return nlohmann::json{
				{"ok", true},
				{"asset_path",     a.assetPath},
				{"parent_class",   a.parentClass},
				{"state_machines", machines},
			};
		});
	}

	// EDIT-4: AnimMontage read tools --------------------------------------------
	{
		ToolDescriptor d;
		d.name = "list_anim_montages";
		d.description =
			"[anim] List UAnimMontage assets under a content path (default `/Game`). "
			"Returns asset_path and name per montage. Use `read_anim_montage` for sections, "
			"notifies, and slot tracks.";
		d.input_schema = {
			{"type","object"},
			{"properties", {{"path", {{"type","string"},{"description","Content path prefix"}}}}},
		};
		d.output_schema = PaginatedSchema();
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string path = OptString(args, "path", "/Game");
			auto ctl = ParseResponseControls(args);
			return ListResponse(reader.ListAnimMontages(path), ctl);
		});
	}
	{
		ToolDescriptor d;
		d.name = "read_anim_montage";
		d.description =
			"[anim] Read a UAnimMontage: sections (name, start_time, next_section), "
			"notifies (name, trigger_time, duration, notify_class), slot tracks "
			"(slot_name, anim segments with anim_sequence path). Essential for GAS-based "
			"projects where abilities drive all character actions through montages.";
		d.input_schema = {
			{"type","object"},
			{"properties", {{"asset_path",{{"type","string"}}}}},
			{"required", nlohmann::json::array({"asset_path"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path",  {{"type","string"}}},
				{"name",        {{"type","string"}}},
				{"blend_in",    {{"type","number"}}},
				{"blend_out",   {{"type","number"}}},
				{"sections",    {{"type","array"}}},
				{"notifies",    {{"type","array"}}},
				{"slot_tracks", {{"type","array"}}},
			}},
			{"required", nlohmann::json::array({"asset_path","name","sections","notifies","slot_tracks"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireAssetPath(args);
			return reader.ReadAnimMontage(asset);
		});
	}

	// EDIT-2: Timeline read tools -----------------------------------------------
	{
		ToolDescriptor d;
		d.name = "list_timelines";
		d.description =
			"[blueprint] List all Timeline nodes in a Blueprint (names, track counts, "
			"length, loop, auto_play). Quick summary — use `read_timeline` for track keys.";
		d.input_schema = {
			{"type", "object"},
			{"properties", {{"asset_path", {{"type","string"}}}}},
			{"required", nlohmann::json::array({"asset_path"})},
		};
		d.output_schema = PaginatedSchema();
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireAssetPath(args);
			auto ctl = ParseResponseControls(args);
			auto rows = reader.ListTimelines(asset);
			return ListResponse(std::move(rows), ctl);
		});
	}
	{
		ToolDescriptor d;
		d.name = "read_timeline";
		d.description =
			"[blueprint] Read a Timeline from a Blueprint: float/vector/event/linear_color "
			"tracks with their FRichCurveKey arrays (time, value, interp). "
			"Provide `timeline_name` to select one; omit to read the first timeline.";
		d.input_schema = {
			{"type", "object"},
			{"properties", {
				{"asset_path",     {{"type","string"},{"description","Blueprint asset path"}}},
				{"timeline_name",  {{"type","string"},{"description","Timeline variable name (omit for first)"}}},
			}},
			{"required", nlohmann::json::array({"asset_path"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"name",       {{"type","string"}}},
				{"length",     {{"type","number"}}},
				{"loop",       {{"type","boolean"}}},
				{"auto_play",  {{"type","boolean"}}},
				{"replicated", {{"type","boolean"}}},
				{"tracks",     {{"type","array"}}},
			}},
			{"required", nlohmann::json::array({"name","length","loop","auto_play","replicated","tracks"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset  = RequireAssetPath(args);
			std::string tname  = OptString(args, "timeline_name", "");
			return reader.ReadTimeline(asset, tname);
		});
	}

	{
		ToolDescriptor d;
		d.name = "add_anim_state";
		d.description = "[anim] Add a state to a named state machine inside a "
						"UAnimBlueprint. NOT to be confused with "
						"`add_state_tree_state` (StateTree). Scaffold only — "
						"final state authoring still uses the AnimGraph editor.";
		d.input_schema = {{"type","object"},
			{"properties", {
				{"asset_path",    {{"type","string"}}},
				{"state_machine", {{"type","string"}}},
				{"state_name",    {{"type","string"}}},
			}},
			{"required", nlohmann::json::array(
				{"asset_path","state_machine","state_name"})}};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",            {{"type","boolean"}}},
				{"asset_path",    {{"type","string"}}},
				{"state_machine", {{"type","string"}}},
				{"state_name",    {{"type","string"}}},
				{"added",         {{"type","boolean"}}},
			}},
			{"required", nlohmann::json::array({"ok","asset_path","state_machine","state_name","added"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireAssetPath(args);
			std::string m     = RequireString(args, "state_machine");
			std::string n     = RequireString(args, "state_name");
			auto r = reader.AddAnimState(asset, m, n);
			return nlohmann::json{
				{"ok", true},
				{"asset_path",    r.assetPath},
				{"state_machine", r.stateMachine},
				{"state_name",    r.stateName},
				{"added",         r.added},
			};
		});
	}

	{
		ToolDescriptor d;
		d.name = "compile_anim_blueprint";
		d.description = "[anim] Compile a UAnimBlueprint.";
		d.input_schema = {{"type","object"},
			{"properties", {{"asset_path", {{"type","string"}}}}},
			{"required", nlohmann::json::array({"asset_path"})}};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",         {{"type","boolean"}}},
				{"asset_path", {{"type","string"}}},
				{"compiled",   {{"type","boolean"}}},
			}},
			{"required", nlohmann::json::array({"ok","asset_path","compiled"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireAssetPath(args);
			auto r = reader.CompileAnimBlueprint(asset);
			return nlohmann::json{
				{"ok", true},
				{"asset_path", r.assetPath},
				{"compiled",   r.compiled},
			};
		});
	}

}

void RegisterTools_09(ToolRegistry& registry, backends::IBlueprintReader& reader) {
	// ----- bp_structural_diff---------------------------------------------
	{
		ToolDescriptor d;
		d.name = "bp_structural_diff";
		d.description =
			"[blueprint] Compare two Blueprints structurally — variables, "
			"components, function/macro/event-graph node signatures, "
			"connection counts. Returns {ok, differences[]}. Position- and "
			"GUID-independent so a freshly-rebuilt clone diffs cleanly "
			"against its source. Requires live or commandlet backend.";
		d.input_schema = {
			{"type", "object"},
			{"properties", {
				{"source",    {{"type","string"},{"description","Source BP package path"}}},
				{"candidate", {{"type","string"},{"description","Clone BP package path"}}},
				{"options",   {{"type","object"},
							   {"properties", {
								   {"ignore_node_positions", {{"type","boolean"}}},
								   {"ignore_comment_nodes",  {{"type","boolean"}}},
							   }}}},
			}},
			{"required", nlohmann::json::array({"source","candidate"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string a = RequireString(args, "source");
			std::string b = RequireString(args, "candidate");
			backends::IBlueprintReader::StructuralDiffOptions opts;
			if (args.contains("options") && args["options"].is_object()) {
				const auto& o = args["options"];
				opts.ignoreNodePositions = o.value("ignore_node_positions", true);
				opts.ignoreCommentNodes  = o.value("ignore_comment_nodes", false);
			}
			return reader.StructuralDiff(a, b, opts);
		});
	}

	// ===== Batch + DSL =====================================================
	// apply_ops and compile_function live in their own files because their
	// dispatch tables are bigger than the per-tool handlers above.
	RegisterApplyOps(registry, reader);
	RegisterCompileFunction(registry, reader);
}

// Public entry point — registers every tool by fanning out to the
// RegisterTools_NN chunks (split only to keep each function within
// MSVC's front-end heap; see the note on RegisterTools_00).
}  // namespace bpr::tools
