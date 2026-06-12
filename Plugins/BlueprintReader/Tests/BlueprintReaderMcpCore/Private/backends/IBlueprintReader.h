// IBlueprintReader — the inner contract between the MCP tool layer and
// whatever is actually reading blueprint data (mock fixtures, commandlet
// subprocess, live-editor socket). All backends return the same canonical
// shapes from BlueprintReaderTypes.h.
#pragma once

#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "BlueprintReaderTypes.h"

namespace bpr::backends {

// Backend-side error type. The MCP layer catches this (and any std::exception)
// and surfaces it as an MCP tool error envelope.
class BlueprintReaderError : public std::runtime_error {
public:
	using std::runtime_error::runtime_error;
};

// Thrown when an asset path doesn't resolve. Sub-class so callers can
// distinguish "you typed the path wrong" from "I couldn't talk to the engine".
class AssetNotFound : public BlueprintReaderError {
public:
	using BlueprintReaderError::BlueprintReaderError;
};

// Thrown by SocketBlueprintReader when the TRANSPORT fails — connect
// refused, connection dropped mid-frame, or auth rejected — as distinct
// from a tool-level error the editor returned. AutoBlueprintReader catches
// this to fall back to the commandlet instead of stranding the session on
// an unreachable or auth-broken socket route.
class SocketTransportError : public BlueprintReaderError {
public:
	using BlueprintReaderError::BlueprintReaderError;
};

class IBlueprintReader {
public:
	virtual ~IBlueprintReader() = default;

	virtual std::vector<BPAssetSummary> ListBlueprints(std::string_view path) = 0;
	virtual BPMetadata                  ReadBlueprint(std::string_view assetPath) = 0;
	virtual BPGraph                     GetGraph(std::string_view assetPath, std::string_view graphName) = 0;
	virtual BPFunction                  GetFunction(std::string_view assetPath, std::string_view fnName) = 0;
	virtual std::vector<BPVariable>     ListVariables(std::string_view assetPath) = 0;
	virtual std::vector<BPComponent>    GetComponents(std::string_view assetPath) = 0;
	// `kind`, when non-empty, additionally filters matches by their K2 extras
	// "kind" entry (e.g. "CallFunction", "VariableGet", "Event"). The text
	// `query` matches case-insensitively against class or title; `kind` is an
	// exact match (case-insensitive) against meta["kind"].
	virtual std::vector<BPNode>         FindNode(std::string_view assetPath, std::string_view query,
												 std::string_view kind = {}) = 0;

	// Compare two Blueprints structurally — returns the diff JSON shape
	// produced by BlueprintStructuralDiff::FResult::ToJson(). Position-
	// and GUID-independent so a freshly-rebuilt clone diffs cleanly
	// against its source. Requires the live or commandlet backend (mock
	// fixtures don't carry UBlueprint reflection). Read-only — passes
	// through the ReadOnly decorator.
	struct StructuralDiffOptions {
		bool ignoreNodePositions = true;
		bool ignoreCommentNodes  = false;
	};
	virtual nlohmann::json StructuralDiff(std::string_view a, std::string_view b,
										   const StructuralDiffOptions& opts = {}) {
		(void)a; (void)b; (void)opts;
		throw BlueprintReaderError("StructuralDiff not supported by this backend");
	}

	// Read ANY UObject instance by package path — including a level-placed
	// actor stored in its own external (OFPA / World Partition) package under
	// /<Mount>/__ExternalActors__/... that the Blueprint-only read path can't
	// open. Returns {ok, asset_path, object_class, object_name, is_actor,
	// is_external, label?, transform?, owning_level?, overrides[], override_count}
	// where `overrides` are the properties this instance changed relative to
	// its archetype (exported text). Read-only — passes through ReadOnly.
	// Requires the live or commandlet backend (mock has no UObject world).
	virtual nlohmann::json ReadActorInstance(std::string_view assetPath) {
		(void)assetPath;
		throw BlueprintReaderError("ReadActorInstance not supported by this backend");
	}

	// Write tools. Backends that don't support mutation throw
	// BlueprintReaderError. Each call should leave the .uasset compilable.

	// Add a member variable to a blueprint. `type` is the wire BPPinType.
	// `defaultValue`, `category` may be empty.
	virtual void AddVariable(std::string_view assetPath, std::string_view name,
							 const BPPinType& type, std::string_view defaultValue,
							 std::string_view category, bool replicated, bool editable) = 0;

	// Reposition a node by its GUID inside `graphName`.
	virtual void SetNodePosition(std::string_view assetPath, std::string_view graphName,
								 std::string_view nodeId, int x, int y) = 0;

	// Delete a node by its GUID. Breaks any incoming/outgoing links first.
	virtual void DeleteNode(std::string_view assetPath, std::string_view graphName,
							std::string_view nodeId) = 0;

	// Spawn a new node in `graphName` at (x, y). `kind` is one of:
	//   "Branch", "Sequence", "VariableGet", "VariableSet", "CallFunction",
	//   "CustomEvent". Kind-specific extras passed through the `extras` map
	//   (e.g. "Variable", "Function", "FunctionOwner", "EventName").
	// Returns the new node's GUID so the caller can wire pins to it.
	virtual std::string AddNode(std::string_view assetPath, std::string_view graphName,
								std::string_view kind, int x, int y,
								const std::map<std::string, std::string, std::less<>>& extras) = 0;

	// Connect two pins by node GUID + pin spec (GUID or name).
	virtual void WirePins(std::string_view assetPath, std::string_view graphName,
						  std::string_view fromNodeId, std::string_view fromPinSpec,
						  std::string_view toNodeId,   std::string_view toPinSpec) = 0;

	// Remove a member variable. Throws AssetNotFound if missing.
	virtual void DeleteVariable(std::string_view assetPath, std::string_view name) = 0;

	// Rename a member variable. Updates references in graphs.
	virtual void RenameVariable(std::string_view assetPath, std::string_view oldName,
								std::string_view newName) = 0;

	// Add a new BP function graph. Returns the function name (echoed back)
	// plus the FunctionEntry node's GUID so callers can wire its `then`
	// exec output into their first statement without a follow-up read.
	struct AddFunctionResult {
		std::string functionName;
		std::string entryNodeId;  // empty if the plugin couldn't locate it
	};
	virtual AddFunctionResult AddFunction(std::string_view assetPath, std::string_view name) = 0;

	// Add an input parameter to an existing function. `type` is a wire BPPinType.
	virtual void AddFunctionInput(std::string_view assetPath, std::string_view functionName,
								  std::string_view paramName, const BPPinType& type) = 0;

	// Add an output parameter to an existing function. Spawns a FunctionResult
	// node if there isn't one yet.
	virtual void AddFunctionOutput(std::string_view assetPath, std::string_view functionName,
								   std::string_view paramName, const BPPinType& type) = 0;

	// Delete a function and its graph.
	virtual void DeleteFunction(std::string_view assetPath, std::string_view name) = 0;

	// Change a variable's default value (string form, as displayed in the Details panel).
	virtual void SetVariableDefault(std::string_view assetPath, std::string_view name,
									std::string_view newDefault) = 0;

	// Create a brand-new BP under `assetPath` (must be `/Game/...`) extending
	// `parentClass` (UClass path or short name). Idempotent — if the asset
	// already exists, returns without throwing. Required so AI agents can
	// generate whole new BPs, not just mutate existing ones (A3).
	struct CreateBlueprintResult {
		bool alreadyExisted = false;
		std::string parentClass;  // resolved full path, for echo
	};
	// `blueprintType` is an optional EBlueprintType name (e.g.
	// "BPTYPE_Interface") — empty means the plugin's default (a normal
	// actor/object BP derived from `parentClass`).
	virtual CreateBlueprintResult CreateBlueprint(std::string_view assetPath,
												  std::string_view parentClass,
												  std::string_view blueprintType = "") = 0;

	// Clone every node in `graphName` from the `sourcePath` BP into the
	// same-named graph of the `targetPath` BP, preserving wiring. Used by
	// the BP recreate flow to copy a function/event-graph wholesale.
	struct CloneGraphResult {
		bool ok = false;
		int  importedNodes = 0;
	};
	virtual CloneGraphResult CloneGraph(std::string_view sourcePath,
										std::string_view targetPath,
										std::string_view graphName) = 0;

	// Add `interfacePath` (a BP-interface or native UInterface) to the
	// implemented-interfaces list of the `assetPath` BP, generating the
	// stub function graphs the interface requires.
	virtual void ImplementInterface(std::string_view assetPath,
									std::string_view interfacePath) = 0;

	// Set the literal default value on a node's pin (B1). Used by
	// compile_function's {lit:value} support — UE has no first-class
	// literal node, so the value is materialized as the consumer pin's
	// default. `pinSpec` accepts a pin GUID or a pin name.
	virtual void SetPinDefault(std::string_view assetPath,
							   std::string_view graphName,
							   std::string_view nodeId,
							   std::string_view pinSpec,
							   std::string_view value) = 0;

	// Change a member variable's type WITHOUT delete + re-add — UE
	// rewires every VariableGet / VariableSet node that references it
	// in place, so existing graphs survive (BP-2). For a brand-new
	// variable, use AddVariable instead.
	virtual void RetypeVariable(std::string_view assetPath,
								std::string_view name,
								const BPPinType& newType) = 0;

	// Change the My-Blueprint-panel category label on a member
	// variable. Empty `category` clears the label back to default
	// (BP-7).
	virtual void SetVariableCategory(std::string_view assetPath,
									 std::string_view name,
									 std::string_view category) = 0;

	// File-level duplicate of a blueprint (BP-5). `destAssetPath` must
	// be under /Game/. Idempotent — if the destination already exists,
	// returns alreadyExisted=true without overwriting.
	struct DuplicateBlueprintResult {
		bool alreadyExisted = false;
		std::string sourceAssetPath;
	};
	virtual DuplicateBlueprintResult DuplicateBlueprint(
		std::string_view sourceAssetPath, std::string_view destAssetPath) = 0;

	// Write a transpiled source file (.h or .cpp) into the project's
	// Source/ tree. Used by transpile_blueprint to drop the generated
	// UCLASS pair onto disk so UBT can compile it. The plugin validates
	// `destPath` is under <ProjectDir>/Source/ — no path-traversal escape.
	struct WriteGeneratedSourceResult {
		std::size_t bytesWritten = 0;
		std::string path;            // canonicalized absolute path
	};
	virtual WriteGeneratedSourceResult WriteGeneratedSource(
		std::string_view destPath, std::string_view content,
		bool createDirs = true) = 0;

	// ----- Project + Content Browser ops ---------------------------------
	//
	// Project-level introspection + asset-browser operations that complement
	// the per-Blueprint surface. These are surface-level ops users expect
	// when working with a UE project as a whole.

	// Read the project's metadata (`.uproject` JSON + a normalized view of
	// the most-asked-for fields). Backends override; default throws.
	struct ProjectMetadata {
		std::string projectName;        // derived from .uproject filename
		std::string projectPath;        // absolute path to the .uproject
		std::string engineAssociation;  // .uproject "EngineAssociation"
		std::string category;
		std::string description;
		nlohmann::json raw;             // full .uproject JSON for anything else
	};
	virtual ProjectMetadata GetProjectMetadata() {
		throw BlueprintReaderError("GetProjectMetadata not supported by this backend");
	}

	// Save every dirty package the editor has loaded. With `dirtyOnly=true`
	// (default), packages that aren't marked dirty are skipped — fast no-op
	// when nothing's changed. Live backend hits the editor's save path;
	// commandlet daemon walks loaded packages.
	struct SaveAllResult {
		int savedCount = 0;
		std::vector<std::string> failedAssets;
	};
	virtual SaveAllResult SaveAll(bool dirtyOnly = true) {
		(void)dirtyOnly;
		throw BlueprintReaderError("SaveAll not supported by this backend");
	}

	// Move (or rename) an asset. `dest` is the full destination package
	// path — pass the same folder with a different leaf for a rename, or
	// a different folder to move. Both must be under `/Game/`. Updates
	// the asset registry, fixes up references in other assets.
	struct MoveAssetResult {
		std::string sourcePath;
		std::string destPath;
		int redirectorsCreated = 0;
	};
	virtual MoveAssetResult MoveAsset(std::string_view sourcePath,
									  std::string_view destPath) {
		(void)sourcePath; (void)destPath;
		throw BlueprintReaderError("MoveAsset not supported by this backend");
	}

	// Delete an asset. Refuses if other assets reference it (default;
	// override with `force=true`). Returns the list of references found so
	// the caller can act on them — fix-up or force.
	struct DeleteAssetResult {
		std::string path;
		bool deleted = false;
		std::vector<std::string> referencingAssets;
	};
	virtual DeleteAssetResult DeleteAsset(std::string_view assetPath,
										  bool force = false) {
		(void)assetPath; (void)force;
		throw BlueprintReaderError("DeleteAsset not supported by this backend");
	}

	// Create a folder under `/Game/`. Idempotent — returns
	// {already_existed:true} if the folder is already present. UE folders
	// are just package paths with a stub asset; we use
	// `IAssetTools::CreateUniqueAssetName` semantics indirectly.
	struct CreateFolderResult {
		std::string path;
		bool alreadyExisted = false;
	};
	virtual CreateFolderResult CreateFolder(std::string_view folderPath) {
		(void)folderPath;
		throw BlueprintReaderError("CreateFolder not supported by this backend");
	}

	// List all UDataTable assets under a content path (mirrors
	// ListBlueprints but for the DataTable type).
	virtual std::vector<BPAssetSummary> ListDataTables(std::string_view path) {
		(void)path;
		throw BlueprintReaderError("ListDataTables not supported by this backend");
	}

	// Read a single DataTable's row structure + every row's field values.
	struct DataTableInfo {
		std::string assetPath;
		std::string rowStruct;          // e.g. "/Game/Data/ST_Item"
		std::vector<std::string> columns;
		// Each row: { "row_name": <FName>, "<col>": <serialized-string>, ... }
		nlohmann::json rows = nlohmann::json::array();
	};
	virtual DataTableInfo ReadDataTable(std::string_view assetPath) {
		(void)assetPath;
		throw BlueprintReaderError("ReadDataTable not supported by this backend");
	}

	// Add a row to an existing DataTable. The row name must be unique
	// within the table. `values` is a JSON object whose keys map to the
	// row struct's field names; UE's FProperty::ImportText handles the
	// string→native coercion (works for scalars, enums, structs that
	// serialize to text). Idempotent: if `rowName` already exists,
	// returns `{already_existed:true}` without overwriting unless
	// `overwrite=true` is passed.
	struct AddDataRowResult {
		std::string assetPath;
		std::string rowName;
		bool alreadyExisted = false;
		bool created = false;
	};
	virtual AddDataRowResult AddDataRow(std::string_view assetPath,
										std::string_view rowName,
										const nlohmann::json& values,
										bool overwrite = false) {
		(void)assetPath; (void)rowName; (void)values; (void)overwrite;
		throw BlueprintReaderError("AddDataRow not supported by this backend");
	}

	// Update a single field on an existing row. `fieldName` must match
	// a property on the row struct; `value` is its string form
	// (ImportText input). Use this for surgical edits — a full-row
	// overwrite is a separate AddDataRow call with `overwrite=true`.
	struct SetDataRowValueResult {
		std::string assetPath;
		std::string rowName;
		std::string fieldName;
		std::string oldValue;       // ExportText'd
		std::string newValue;       // post-set ExportText'd
	};
	virtual SetDataRowValueResult SetDataRowValue(std::string_view assetPath,
												  std::string_view rowName,
												  std::string_view fieldName,
												  std::string_view value) {
		(void)assetPath; (void)rowName; (void)fieldName; (void)value;
		throw BlueprintReaderError("SetDataRowValue not supported by this backend");
	}

	// ----- Component (SCS) authoring ------------------------------------
	//
	// BP components live in the class's SimpleConstructionScript tree.
	// Each USCS_Node holds a ComponentClass, a ComponentTemplate
	// (UActorComponent with the BP-author's default values), and an
	// attach-parent + socket. These ops manipulate that tree.
	//
	// Add: creates a new node, optionally child of `parent_name` (root
	// attachment when empty), with an optional `socket` for SceneComp
	// children. Idempotent on `name`: returns `{already_existed:true}`
	// when a node by that name already exists.
	struct AddComponentResult {
		std::string assetPath;
		std::string name;
		std::string componentClass;
		bool alreadyExisted = false;
		bool created = false;
	};
	virtual AddComponentResult AddComponent(std::string_view assetPath,
											std::string_view name,
											std::string_view componentClass,
											std::string_view parentName,
											std::string_view socket) {
		(void)assetPath; (void)name; (void)componentClass;
		(void)parentName; (void)socket;
		throw BlueprintReaderError("AddComponent not supported by this backend");
	}

	// Remove a component node from the BP's SCS. Returns
	// `{removed:false}` when the node doesn't exist.
	struct RemoveComponentResult {
		std::string assetPath;
		std::string name;
		bool removed = false;
	};
	virtual RemoveComponentResult RemoveComponent(std::string_view assetPath,
												  std::string_view name) {
		(void)assetPath; (void)name;
		throw BlueprintReaderError("RemoveComponent not supported by this backend");
	}

	// Re-parent a component node. Pass empty `newParentName` to attach
	// at the SCS root. `socket` applies to SceneComp children only and
	// is otherwise ignored.
	struct AttachComponentResult {
		std::string assetPath;
		std::string name;
		std::string newParentName;
		std::string socket;
		bool reparented = false;
	};
	virtual AttachComponentResult AttachComponent(std::string_view assetPath,
												  std::string_view name,
												  std::string_view newParentName,
												  std::string_view socket) {
		(void)assetPath; (void)name; (void)newParentName; (void)socket;
		throw BlueprintReaderError("AttachComponent not supported by this backend");
	}

	// Set a property on a component template (the BP-author's default
	// values). Same ImportText coercion as set_data_row_value: value is
	// stringified and the property's type handles the conversion.
	struct SetComponentPropertyResult {
		std::string assetPath;
		std::string componentName;
		std::string propertyName;
		std::string oldValue;
		std::string newValue;
		// #8: the component class-default for this property + whether the
		// template now differs from it. Disambiguates new_value:"" ("set to
		// the default, no override stored") from an actually-empty value.
		std::string defaultValue;
		bool        hasOverride = true;
	};
	virtual SetComponentPropertyResult SetComponentProperty(
		std::string_view assetPath, std::string_view componentName,
		std::string_view propertyName, std::string_view value) {
		(void)assetPath; (void)componentName; (void)propertyName; (void)value;
		throw BlueprintReaderError("SetComponentProperty not supported by this backend");
	}

	// ----- Material authoring -------------------------------------------
	//
	// Material graphs are author-time DAGs of UMaterialExpression nodes
	// that connect into the master material's input pins (BaseColor /
	// Roughness / Normal / etc.). Reading dumps every expression + every
	// connection; writing creates expressions, wires them, sets named
	// parameters, recompiles.
	//
	// Material instances (UMaterialInstanceConstant) override their parent
	// material's exposed parameters without changing the graph. We expose
	// scalar / vector / texture / static-switch param setters as one tool
	// dispatched by `param_type`.

	// Create a new UMaterial asset (default Surface/Opaque/DefaultLit) at
	// `assetPath`. The enabler for granular material recreation, parallel to
	// CreateBlueprint: a fresh material to populate with AddMaterialExpression /
	// ConnectMaterialExpressions / SetMaterialParameter / CompileMaterial.
	// Idempotent — `alreadyExisted` true if the asset is already present.
	struct CreateMaterialResult {
		bool alreadyExisted = false;
		bool saved = false;
	};
	virtual CreateMaterialResult CreateMaterial(std::string_view assetPath) {
		(void)assetPath;
		throw BlueprintReaderError("CreateMaterial not supported by this backend");
	}

	// Create a UMaterialInstanceConstant at `assetPath`, parented to
	// `parentPath` (a material or material instance; empty leaves no parent).
	// The enabler for recreating material instances + exercising
	// SetMaterialInstanceParameter. Idempotent.
	struct CreateMaterialInstanceResult {
		bool alreadyExisted = false;
		bool saved = false;
		std::string parentPath;
	};
	virtual CreateMaterialInstanceResult CreateMaterialInstance(
		std::string_view assetPath, std::string_view parentPath) {
		(void)assetPath; (void)parentPath;
		throw BlueprintReaderError("CreateMaterialInstance not supported by this backend");
	}

	virtual std::vector<BPAssetSummary> ListMaterials(std::string_view path) {
		(void)path;
		throw BlueprintReaderError("ListMaterials not supported by this backend");
	}

	struct MaterialExpression {
		std::string id;       // GUID-style id mintable per session
		std::string className;
		std::string parameterName;  // for parameter expressions
		int x = 0;
		int y = 0;
	};
	struct MaterialConnection {
		std::string fromNodeId;
		std::string fromPin;        // e.g. "RGBA", or "Output"
		std::string toNodeId;       // empty toNodeId = master material slot
		std::string toPin;          // e.g. "BaseColor", "Roughness"
	};
	struct MaterialInfo {
		std::string assetPath;
		std::vector<MaterialExpression> expressions;
		std::vector<MaterialConnection> connections;
		std::vector<std::string> parameterNames;  // names of exposed params
	};
	virtual MaterialInfo ReadMaterial(std::string_view assetPath) {
		(void)assetPath;
		throw BlueprintReaderError("ReadMaterial not supported by this backend");
	}

	struct AddMaterialExpressionResult {
		std::string assetPath;
		std::string expressionId;
		std::string className;
	};
	virtual AddMaterialExpressionResult AddMaterialExpression(
		std::string_view assetPath, std::string_view expressionClass,
		int x, int y) {
		(void)assetPath; (void)expressionClass; (void)x; (void)y;
		throw BlueprintReaderError("AddMaterialExpression not supported by this backend");
	}

	struct ConnectMaterialResult {
		std::string assetPath;
		bool connected = false;
	};
	virtual ConnectMaterialResult ConnectMaterialExpressions(
		std::string_view assetPath,
		std::string_view fromNodeId, std::string_view fromPin,
		std::string_view toNodeId, std::string_view toPin) {
		(void)assetPath; (void)fromNodeId; (void)fromPin;
		(void)toNodeId; (void)toPin;
		throw BlueprintReaderError("ConnectMaterialExpressions not supported by this backend");
	}

	struct SetMaterialParameterResult {
		std::string assetPath;
		std::string parameterName;
		std::string oldValue;
		std::string newValue;
	};
	virtual SetMaterialParameterResult SetMaterialParameter(
		std::string_view assetPath, std::string_view parameterName,
		std::string_view value) {
		(void)assetPath; (void)parameterName; (void)value;
		throw BlueprintReaderError("SetMaterialParameter not supported by this backend");
	}

	// Set a parameter override on a UMaterialInstanceConstant.
	// `param_type` is one of: "scalar" / "vector" / "texture" /
	// "static_switch". The value is stringified; ImportText handles the
	// type-specific parse.
	struct SetMIParameterResult {
		std::string assetPath;
		std::string parameterName;
		std::string paramType;
		std::string newValue;
	};
	virtual SetMIParameterResult SetMaterialInstanceParameter(
		std::string_view assetPath, std::string_view parameterName,
		std::string_view paramType, std::string_view value) {
		(void)assetPath; (void)parameterName; (void)paramType; (void)value;
		throw BlueprintReaderError("SetMaterialInstanceParameter not supported by this backend");
	}

	struct CompileMaterialResult {
		std::string assetPath;
		bool compiled = false;
	};
	virtual CompileMaterialResult CompileMaterial(std::string_view assetPath) {
		(void)assetPath;
		throw BlueprintReaderError("CompileMaterial not supported by this backend");
	}

	// ----- UMG widget authoring -----------------------------------------
	//
	// Widget Blueprints (UMG) author UI hierarchies on a UWidgetTree
	// hung off the WBP class. Each widget is a UWidget subclass with a
	// unique FName. Read dumps the tree + each widget's class +
	// parent. Write adds widgets to existing parents, sets properties,
	// binds events, recompiles.

	struct WidgetNode {
		std::string name;
		std::string className;
		std::string parentName;  // empty = root
	};
	struct WidgetBlueprintInfo {
		std::string assetPath;
		std::string rootName;
		std::vector<WidgetNode> nodes;
	};
	virtual WidgetBlueprintInfo ReadWidgetBlueprint(std::string_view assetPath) {
		(void)assetPath;
		throw BlueprintReaderError("ReadWidgetBlueprint not supported by this backend");
	}

	struct AddWidgetResult {
		std::string assetPath;
		std::string name;
		std::string widgetClass;
		bool alreadyExisted = false;
		bool created = false;
	};
	virtual AddWidgetResult AddWidget(std::string_view assetPath,
									   std::string_view parentName,
									   std::string_view widgetClass,
									   std::string_view name) {
		(void)assetPath; (void)parentName; (void)widgetClass; (void)name;
		throw BlueprintReaderError("AddWidget not supported by this backend");
	}

	struct SetWidgetPropertyResult {
		std::string assetPath;
		std::string widgetName;
		std::string propertyName;
		std::string oldValue;
		std::string newValue;
	};
	virtual SetWidgetPropertyResult SetWidgetProperty(
		std::string_view assetPath, std::string_view widgetName,
		std::string_view propertyName, std::string_view value) {
		(void)assetPath; (void)widgetName; (void)propertyName; (void)value;
		throw BlueprintReaderError("SetWidgetProperty not supported by this backend");
	}

	struct BindWidgetEventResult {
		std::string assetPath;
		std::string widgetName;
		std::string eventName;
		std::string handlerFunction;
		bool bound = false;
		// UX-P4b: when bound=false, why (widget/delegate/event-graph missing);
		// when bound=true, optionally how it was satisfied (e.g. already_existed).
		std::string reason;
	};
	virtual BindWidgetEventResult BindWidgetEvent(
		std::string_view assetPath, std::string_view widgetName,
		std::string_view eventName, std::string_view handlerFunction) {
		(void)assetPath; (void)widgetName; (void)eventName; (void)handlerFunction;
		throw BlueprintReaderError("BindWidgetEvent not supported by this backend");
	}

	struct CompileWidgetBlueprintResult {
		std::string assetPath;
		bool compiled = false;
	};
	virtual CompileWidgetBlueprintResult CompileWidgetBlueprint(
		std::string_view assetPath) {
		(void)assetPath;
		throw BlueprintReaderError("CompileWidgetBlueprint not supported by this backend");
	}

	// ----- Behavior Tree authoring (Stage 2) ----------------------------
	//
	// UBehaviorTree exposes a root composite node, decorators, services,
	// and tasks. Node ids are stable UObject names within the tree.
	struct BTNode {
		std::string nodeId;
		std::string className;     // Composite / Decorator / Task class
		std::string nodeKind;      // "composite" | "decorator" | "service" | "task"
		std::string parentNodeId;  // empty = root
	};
	struct BehaviorTreeInfo {
		std::string assetPath;
		std::string rootNodeId;
		std::vector<BTNode> nodes;
	};
	virtual BehaviorTreeInfo ReadBehaviorTree(std::string_view assetPath) {
		(void)assetPath;
		throw BlueprintReaderError("ReadBehaviorTree not supported by this backend");
	}

	struct AddBTNodeResult {
		std::string assetPath;
		std::string nodeId;
		std::string className;
		std::string nodeKind;
	};
	virtual AddBTNodeResult AddBTNode(std::string_view assetPath,
		std::string_view parentNodeId, std::string_view nodeKind,
		std::string_view nodeClass) {
		(void)assetPath; (void)parentNodeId; (void)nodeKind; (void)nodeClass;
		throw BlueprintReaderError("AddBTNode not supported by this backend");
	}

	struct SetBTNodePropertyResult {
		std::string assetPath;
		std::string nodeId;
		std::string propertyName;
		std::string oldValue;
		std::string newValue;
	};
	virtual SetBTNodePropertyResult SetBTNodeProperty(std::string_view assetPath,
		std::string_view nodeId, std::string_view propertyName,
		std::string_view value) {
		(void)assetPath; (void)nodeId; (void)propertyName; (void)value;
		throw BlueprintReaderError("SetBTNodeProperty not supported by this backend");
	}

	struct CompileBehaviorTreeResult {
		std::string assetPath;
		bool compiled = false;
	};
	virtual CompileBehaviorTreeResult CompileBehaviorTree(std::string_view assetPath) {
		(void)assetPath;
		throw BlueprintReaderError("CompileBehaviorTree not supported by this backend");
	}

	virtual std::vector<BPAssetSummary> ListBehaviorTrees(std::string_view path) {
		(void)path;
		throw BlueprintReaderError("ListBehaviorTrees not supported by this backend");
	}

	// ----- DataAsset CRUD (Stage 2) -------------------------------------
	//
	// UDataAsset subclasses are pure data containers. We expose them as
	// {class, properties} pairs where properties is the JSON projection
	// of every UPROPERTY on the asset.
	struct DataAssetInfo {
		std::string assetPath;
		std::string className;
		nlohmann::json properties;  // {propName: stringifiedValue}
	};
	virtual std::vector<BPAssetSummary> ListDataAssets(std::string_view path) {
		(void)path;
		throw BlueprintReaderError("ListDataAssets not supported by this backend");
	}
	virtual DataAssetInfo ReadDataAsset(std::string_view assetPath) {
		(void)assetPath;
		throw BlueprintReaderError("ReadDataAsset not supported by this backend");
	}

	struct CreateDataAssetResult {
		std::string assetPath;
		std::string className;
		bool created = false;
		bool alreadyExisted = false;
	};
	virtual CreateDataAssetResult CreateDataAsset(std::string_view assetPath,
		std::string_view className) {
		(void)assetPath; (void)className;
		throw BlueprintReaderError("CreateDataAsset not supported by this backend");
	}

	struct SetDataAssetPropertyResult {
		std::string assetPath;
		std::string propertyName;
		std::string oldValue;
		std::string newValue;
	};
	virtual SetDataAssetPropertyResult SetDataAssetProperty(std::string_view assetPath,
		std::string_view propertyName, std::string_view value) {
		(void)assetPath; (void)propertyName; (void)value;
		throw BlueprintReaderError("SetDataAssetProperty not supported by this backend");
	}

	// ----- StateTree authoring (Stage 2) --------------------------------
	//
	// UStateTree (experimental in UE 5.x) — hierarchical FSM with state +
	// transition nodes. State ids are stable names within the asset.
	struct StateTreeState {
		std::string stateId;
		std::string name;
		std::string parentStateId;  // empty = root
	};
	struct StateTreeTransition {
		std::string fromStateId;
		std::string toStateId;
		std::string trigger;  // e.g. "OnTick", "OnEvent"
	};
	struct StateTreeInfo {
		std::string assetPath;
		std::vector<StateTreeState> states;
		std::vector<StateTreeTransition> transitions;
	};
	virtual std::vector<BPAssetSummary> ListStateTrees(std::string_view path) {
		(void)path;
		throw BlueprintReaderError("ListStateTrees not supported by this backend");
	}
	virtual StateTreeInfo ReadStateTree(std::string_view assetPath) {
		(void)assetPath;
		throw BlueprintReaderError("ReadStateTree not supported by this backend");
	}

	struct AddStateTreeStateResult {
		std::string assetPath;
		std::string stateId;
		std::string name;
	};
	virtual AddStateTreeStateResult AddStateTreeState(std::string_view assetPath,
		std::string_view parentStateId, std::string_view name) {
		(void)assetPath; (void)parentStateId; (void)name;
		throw BlueprintReaderError("AddStateTreeState not supported by this backend");
	}

	struct SetStateTreeTransitionResult {
		std::string assetPath;
		std::string fromStateId;
		std::string toStateId;
		std::string trigger;
		bool added = false;
	};
	virtual SetStateTreeTransitionResult SetStateTreeTransition(
		std::string_view assetPath, std::string_view fromStateId,
		std::string_view toStateId, std::string_view trigger) {
		(void)assetPath; (void)fromStateId; (void)toStateId; (void)trigger;
		throw BlueprintReaderError("SetStateTreeTransition not supported by this backend");
	}

	struct CompileStateTreeResult {
		std::string assetPath;
		bool compiled = false;
	};
	virtual CompileStateTreeResult CompileStateTree(std::string_view assetPath) {
		(void)assetPath;
		throw BlueprintReaderError("CompileStateTree not supported by this backend");
	}

	// ----- Stage 3: profiling / cook / class introspection / viewport ---

	struct StartProfileResult {
		std::string outputFile;   // empty until StopProfile
		bool started = false;
	};
	virtual StartProfileResult StartProfile(std::string_view mode) {
		(void)mode;
		throw BlueprintReaderError("StartProfile not supported by this backend");
	}

	struct StopProfileResult {
		std::string outputFile;
		bool stopped = false;
	};
	virtual StopProfileResult StopProfile() {
		throw BlueprintReaderError("StopProfile not supported by this backend");
	}

	struct StatGroupResult {
		std::string group;
		std::string snapshot;  // text output of "stat <group>"
	};
	virtual StatGroupResult GetStats(std::string_view group) {
		(void)group;
		throw BlueprintReaderError("GetStats not supported by this backend");
	}

	struct ScreenshotResult {
		std::string outputFile;
		bool captured = false;
		// Optional explanation — e.g. why nothing was captured in a headless
		// (-nullrhi) session that has no rendering viewport.
		std::string note;
	};
	virtual ScreenshotResult TakeScreenshot(std::string_view destPath, int width, int height) {
		(void)destPath; (void)width; (void)height;
		throw BlueprintReaderError("TakeScreenshot not supported by this backend");
	}

	// Headless cook / package — async; returns "started" + a message
	// describing where to look for results.
	struct CookResult {
		bool started = false;
		std::string platform;
		std::string message;
	};
	virtual CookResult CookContent(std::string_view platform) {
		(void)platform;
		throw BlueprintReaderError("CookContent not supported by this backend");
	}
	virtual CookResult PackageProject(std::string_view platform, std::string_view outputDir) {
		(void)platform; (void)outputDir;
		throw BlueprintReaderError("PackageProject not supported by this backend");
	}

	// Class introspection — return parent chain + UPROPERTY + UFUNCTION
	// list for a UClass given by short name or class path.
	struct ClassPropertyInfo {
		std::string name;
		std::string typeName;
		std::string category;
		// The class that originally declared this property. Empty when the
		// backend doesn't surface it (older plugin payloads). The MCP layer
		// filters on this when `include_inherited=false`.
		std::string declaredOn;
	};
	struct ClassFunctionInfo {
		std::string name;
		std::string flagsCsv;  // e.g. "BlueprintCallable,BlueprintPure"
		std::string declaredOn;
	};
	struct ClassInfo {
		std::string className;
		std::string parentClass;
		std::vector<std::string> ancestors;
		std::vector<ClassPropertyInfo> properties;
		std::vector<ClassFunctionInfo> functions;
	};
	// NB: `IntrospectClass`, not `GetClassInfo` — <windows.h> defines
	// `GetClassInfo` as a macro (→ `GetClassInfoA`/`GetClassInfoW`) which
	// wins the preprocessor pass on any TU that pulls it in (winsock2.h
	// does, transitively). Renaming the C++ method avoids needing
	// #undef tricks in every consumer.
	virtual ClassInfo IntrospectClass(std::string_view className) {
		(void)className;
		throw BlueprintReaderError("IntrospectClass not supported by this backend");
	}

	struct FindClassResult {
		std::vector<std::string> classNames;
	};
	virtual FindClassResult FindClass(std::string_view query) {
		(void)query;
		throw BlueprintReaderError("FindClass not supported by this backend");
	}

	virtual std::vector<ClassFunctionInfo> ListFunctions(std::string_view className) {
		(void)className;
		throw BlueprintReaderError("ListFunctions not supported by this backend");
	}

	// Viewport ergonomics — frame an actor, set camera transform, take
	// viewport screenshot, toggle show flags.
	struct FocusActorResult {
		std::string actorName;
		bool focused = false;
	};
	virtual FocusActorResult FocusActor(std::string_view actorName) {
		(void)actorName;
		throw BlueprintReaderError("FocusActor not supported by this backend");
	}

	struct SetCameraResult {
		bool moved = false;
	};
	virtual SetCameraResult SetCameraTransform(
		double locX, double locY, double locZ,
		double rotPitch, double rotYaw, double rotRoll) {
		(void)locX; (void)locY; (void)locZ;
		(void)rotPitch; (void)rotYaw; (void)rotRoll;
		throw BlueprintReaderError("SetCameraTransform not supported by this backend");
	}

	struct ViewportScreenshotResult {
		std::string outputFile;
		bool captured = false;
		// Optional explanation — e.g. why nothing was captured in a headless
		// (-nullrhi) session that has no rendering viewport.
		std::string note;
	};
	virtual ViewportScreenshotResult TakeViewportScreenshot(std::string_view destPath) {
		(void)destPath;
		throw BlueprintReaderError("TakeViewportScreenshot not supported by this backend");
	}

	struct SetShowFlagResult {
		std::string flagName;
		bool enabled = false;
	};
	virtual SetShowFlagResult SetShowFlag(std::string_view flagName, bool enabled) {
		(void)flagName; (void)enabled;
		throw BlueprintReaderError("SetShowFlag not supported by this backend");
	}

	// ----- Phase 13 Wave 3 writes: viewport view-state + visibility -----
	// Viewport writes (view mode, gizmo, realtime) are per-viewport-client
	// view state — reversible, no package dirty, so read-only mode allows
	// them. Layer/actor visibility toggle level-domain state and are
	// rejected in read-only mode (same line as set_selection).

	struct SetViewModeResult {
		bool valid = false;
		std::string mode;   // canonical mode applied
	};
	virtual SetViewModeResult SetViewMode(std::string_view mode) {
		(void)mode;
		throw BlueprintReaderError("SetViewMode not supported by this backend");
	}

	struct SetGizmoModeResult {
		bool valid = false;
		std::string mode;   // translate|rotate|scale
	};
	virtual SetGizmoModeResult SetGizmoMode(std::string_view mode) {
		(void)mode;
		throw BlueprintReaderError("SetGizmoMode not supported by this backend");
	}

	struct SetViewportRealtimeResult {
		bool valid = false;
		bool isRealtime = false;
	};
	virtual SetViewportRealtimeResult SetViewportRealtime(bool enabled) {
		(void)enabled;
		throw BlueprintReaderError("SetViewportRealtime not supported by this backend");
	}

	struct SetActorVisibilityResult {
		bool valid = false;       // false when actor not found
		std::string name;
		bool visible = false;     // resulting visibility
	};
	virtual SetActorVisibilityResult SetActorVisibility(std::string_view actorName,
														bool visible) {
		(void)actorName; (void)visible;
		throw BlueprintReaderError("SetActorVisibility not supported by this backend");
	}

	// Layers hidden in the editor (ULayersSubsystem) — names of layers whose
	// visibility flag is off. Capped at 500 to bound the payload.
	struct HiddenLayersResult {
		std::vector<std::string> layerNames;
		bool truncated = false;
	};
	virtual HiddenLayersResult GetHiddenLayers() {
		throw BlueprintReaderError("GetHiddenLayers not supported by this backend");
	}

	struct SetLayerVisibilityResult {
		bool valid = false;       // false when layer not found
		std::string layer;
		bool visible = false;
	};
	virtual SetLayerVisibilityResult SetLayerVisibility(std::string_view layer,
														bool visible) {
		(void)layer; (void)visible;
		throw BlueprintReaderError("SetLayerVisibility not supported by this backend");
	}

	// Saved viewport camera bookmarks (Ctrl-1..9 poses). Read from the
	// world settings; only populated slots are returned. `max_slots` is
	// the allocated bookmark array size.
	struct CameraBookmarkInfo {
		int slot = 0;
		double locX = 0.0, locY = 0.0, locZ = 0.0;
		double pitch = 0.0, yaw = 0.0, roll = 0.0;
	};
	struct CameraBookmarksResult {
		std::vector<CameraBookmarkInfo> bookmarks;
		int maxSlots = 0;
	};
	virtual CameraBookmarksResult GetCameraBookmarks() {
		throw BlueprintReaderError("GetCameraBookmarks not supported by this backend");
	}

	// Jump the active viewport camera to a saved bookmark slot. `jumped`
	// is false when the slot is empty or no viewport is focused. View-state
	// only (moves the camera) — allowed in read-only mode.
	struct GotoBookmarkResult {
		bool jumped = false;
		int slot = 0;
	};
	virtual GotoBookmarkResult GotoCameraBookmark(int slot) {
		(void)slot;
		throw BlueprintReaderError("GotoCameraBookmark not supported by this backend");
	}

	// Hover target under the cursor (hit proxy). v1 stub: resolving the
	// hit proxy requires a render-thread readback + RTTI cross-cast that
	// isn't safe out-of-process; returns valid:false until a sidecar in
	// the editor module captures hover state. Shape is locked here.
	struct HoverTargetResult {
		bool valid = false;
		std::string hitProxyType;
		std::string actorName;
	};
	virtual HoverTargetResult GetHoverTarget() {
		throw BlueprintReaderError("GetHoverTarget not supported by this backend");
	}

	// Show-only-selected / isolate mode (UE 5.6+). v1 stub: no stable
	// public accessor for the level-viewport isolate flag; returns
	// valid:false. Shape is locked here.
	struct IsolateModeResult {
		bool valid = false;
		bool isolated = false;
	};
	virtual IsolateModeResult GetIsolateMode() {
		throw BlueprintReaderError("GetIsolateMode not supported by this backend");
	}

	// ===== Phase 14 — World + SCC + system state =======================

	// Async asset compilation backlog (textures / static meshes / etc.)
	// via FAssetCompilingManager. `remaining_assets` is the aggregate.
	struct AsyncCompileStateResult {
		int remainingAssets = 0;
	};
	virtual AsyncCompileStateResult GetAsyncCompileState() {
		throw BlueprintReaderError("GetAsyncCompileState not supported by this backend");
	}

	// Shader compilation backlog via GShaderCompilingManager.
	struct ShaderCompileStateResult {
		bool isCompiling = false;
		int outstandingJobs = 0;
		int pendingJobs = 0;
	};
	virtual ShaderCompileStateResult GetShaderCompileState() {
		throw BlueprintReaderError("GetShaderCompileState not supported by this backend");
	}

	// Editor "current level" — where newly-spawned actors land. Names are
	// package paths (e.g. /Game/Maps/L_Foo).
	struct CurrentLevelResult {
		bool valid = false;
		std::string levelName;   // current level package
		std::string worldName;   // owning world package
	};
	virtual CurrentLevelResult GetCurrentLevel() {
		throw BlueprintReaderError("GetCurrentLevel not supported by this backend");
	}

	// All loaded levels (persistent + streaming sublevels). Package paths.
	struct LoadedLevelsResult {
		std::vector<std::string> levelNames;
	};
	virtual LoadedLevelsResult ListLoadedLevels() {
		throw BlueprintReaderError("ListLoadedLevels not supported by this backend");
	}

	// Active source-control provider. `name` is "None" when disabled.
	struct SourceControlProviderResult {
		std::string name;
		bool enabled = false;
		bool available = false;
	};
	virtual SourceControlProviderResult GetSourceControlProvider() {
		throw BlueprintReaderError("GetSourceControlProvider not supported by this backend");
	}

	// Asset-registry scan status. `is_loading_assets` true while the
	// initial/background scan is still running.
	struct AssetRegistryStateResult {
		bool isLoadingAssets = false;
		bool searchAllAssets = false;
	};
	virtual AssetRegistryStateResult GetAssetRegistryState() {
		throw BlueprintReaderError("GetAssetRegistryState not supported by this backend");
	}

	// World Partition data layers + per-layer effective runtime state
	// (Unloaded/Loaded/Activated). `has_world_partition` false on
	// non-partitioned maps (layers will be empty).
	struct DataLayerStateInfo {
		std::string shortName;
		std::string fullName;
		std::string runtimeState;
	};
	struct DataLayerStatesResult {
		std::vector<DataLayerStateInfo> layers;
		bool hasWorldPartition = false;
	};
	virtual DataLayerStatesResult GetDataLayerStates() {
		throw BlueprintReaderError("GetDataLayerStates not supported by this backend");
	}

	// Editor autosave status via IPackageAutoSaver.
	struct AutosaveStatusResult {
		bool isAutoSaving = false;
	};
	virtual AutosaveStatusResult GetAutosaveStatus() {
		throw BlueprintReaderError("GetAutosaveStatus not supported by this backend");
	}

	// Crash-recovery state — whether autosave restore files are pending.
	struct RecoveryStateResult {
		bool hasPackagesToRestore = false;
	};
	virtual RecoveryStateResult GetRecoveryState() {
		throw BlueprintReaderError("GetRecoveryState not supported by this backend");
	}

	// Per-file source-control status (cached state; reads what the editor
	// already knows without hitting the server). `valid:false` when SCC is
	// disabled or no cached state exists for the file.
	struct SourceControlStatusResult {
		bool valid = false;
		bool controlled = false;
		bool checkedOut = false;
		bool checkedOutOther = false;
		bool modified = false;
		bool current = false;
	};
	virtual SourceControlStatusResult GetSourceControlStatus(std::string_view assetPath) {
		(void)assetPath;
		throw BlueprintReaderError("GetSourceControlStatus not supported by this backend");
	}

	// Who (if anyone) has the file checked out / locked by another user.
	struct FileLockStatusResult {
		bool valid = false;
		bool checkedOutByOther = false;
		std::string otherUser;
	};
	virtual FileLockStatusResult GetFileLockStatus(std::string_view assetPath) {
		(void)assetPath;
		throw BlueprintReaderError("GetFileLockStatus not supported by this backend");
	}

	// ===== Phase 17 — Advanced / niche editor state ====================

	// Active editor culture/language (FInternationalization).
	struct ActiveCultureResult {
		std::string language;      // e.g. "en"
		std::string culture;       // e.g. "en-US"
		std::string displayName;   // e.g. "English (United States)"
	};
	virtual ActiveCultureResult GetActiveCulture() {
		throw BlueprintReaderError("GetActiveCulture not supported by this backend");
	}

	// Current editor theme id (UEditorStyleSettings::CurrentAppliedTheme
	// GUID). Well-known: Dark/Light ship with fixed GUIDs; custom themes
	// get their own. Agents can detect theme changes by id.
	struct EditorThemeResult {
		std::string themeId;
	};
	virtual EditorThemeResult GetEditorTheme() {
		throw BlueprintReaderError("GetEditorTheme not supported by this backend");
	}

	// Connected monitors (FDisplayMetrics): name + native resolution +
	// primary flag. Useful for multi-monitor placement reasoning.
	struct MonitorInfo {
		std::string name;
		int nativeWidth = 0;
		int nativeHeight = 0;
		bool isPrimary = false;
	};
	struct MonitorInfoResult {
		std::vector<MonitorInfo> monitors;
	};
	virtual MonitorInfoResult GetMonitors() {  // not GetMonitorInfo: Win32 macro collision
		throw BlueprintReaderError("GetMonitors not supported by this backend");
	}

	// Live Coding (C++ hot-patch) state. `available:false` on non-Windows
	// or when the module isn't loaded.
	struct LiveCodingStateResult {
		bool available = false;
		bool hasStarted = false;
		bool isCompiling = false;
	};
	virtual LiveCodingStateResult GetLiveCodingState() {
		throw BlueprintReaderError("GetLiveCodingState not supported by this backend");
	}

	// ===== Phase 11 — GameFeatures activate/deactivate (writes) ========
	// Async fire-and-forget: `requested:true` means the state change was
	// queued (resolve `plugin` to a URL succeeded). The actual result is
	// asynchronous — poll get_game_feature_state to confirm. `plugin`
	// accepts a plugin name or a file:-protocol URL. `requested:false`
	// means the name couldn't be resolved to a GFP. Rejected in
	// read-only mode (mutates runtime game state).
	struct GameFeatureActionResult {
		bool requested = false;
		std::string url;
	};
	virtual GameFeatureActionResult ActivateGameFeature(std::string_view plugin) {
		(void)plugin;
		throw BlueprintReaderError("ActivateGameFeature not supported by this backend");
	}
	virtual GameFeatureActionResult DeactivateGameFeature(std::string_view plugin) {
		(void)plugin;
		throw BlueprintReaderError("DeactivateGameFeature not supported by this backend");
	}

	// Recently-opened assets (editor MRU list, most-recent first). Package
	// paths. Phase 14 — recent activity.
	struct RecentAssetsResult {
		std::vector<std::string> assetPaths;
	};
	virtual RecentAssetsResult GetRecentlyOpenedAssets() {
		throw BlueprintReaderError("GetRecentlyOpenedAssets not supported by this backend");
	}

	// PIE-attached debug object for a Blueprint (UBlueprint::
	// GetObjectBeingDebugged). `valid:false` when the BP can't be loaded;
	// `has_debug_object:false` when nothing is attached (e.g. not in PIE).
	// Phase 17 — BP debug.
	struct DebugInstanceResult {
		bool valid = false;
		bool hasDebugObject = false;
		std::string debugObjectName;
		std::string debugObjectPath;
	};
	virtual DebugInstanceResult GetDebugInstance(std::string_view assetPath) {
		(void)assetPath;
		throw BlueprintReaderError("GetDebugInstance not supported by this backend");
	}

	// Blueprint breakpoints (FKismetDebugUtilities). Per-breakpoint: the
	// node it's on (guid + name), a human description, and enabled flag.
	// `valid:false` when the BP can't be loaded. Phase 17 — BP debug.
	struct BreakpointInfo {
		std::string nodeGuid;
		std::string nodeName;
		std::string location;   // GetLocationDescription
		bool enabled = false;
	};
	struct BreakpointsResult {
		bool valid = false;
		std::vector<BreakpointInfo> breakpoints;
	};
	virtual BreakpointsResult GetBlueprintBreakpoints(std::string_view assetPath) {
		(void)assetPath;
		throw BlueprintReaderError("GetBlueprintBreakpoints not supported by this backend");
	}

	// Watched pins on a Blueprint (FKismetDebugUtilities). Per-pin: name,
	// owning node (guid + name), direction. `valid:false` when the BP
	// can't be loaded. Phase 17 — BP debug.
	struct WatchedPinInfo {
		std::string pinName;
		std::string nodeGuid;
		std::string nodeName;
		std::string direction;   // input | output
	};
	struct WatchedPinsResult {
		bool valid = false;
		std::vector<WatchedPinInfo> pins;
	};
	virtual WatchedPinsResult GetWatchedPins(std::string_view assetPath) {
		(void)assetPath;
		throw BlueprintReaderError("GetWatchedPins not supported by this backend");
	}

	// Stat overlays enabled in the active level viewport (StatUnit, StatFPS,
	// StatGPU, etc.) via FViewportClient::GetEnabledStats. `valid:false`
	// when no viewport is focused. Phase 17 — advanced.
	struct ActiveStatsResult {
		bool valid = false;
		std::vector<std::string> stats;
	};
	virtual ActiveStatsResult GetActiveStats() {
		throw BlueprintReaderError("GetActiveStats not supported by this backend");
	}

	// ===== Phase 11 H Tier 1 — PluginToolset write =====================
	// Enable/disable a plugin in the project's .uproject descriptor
	// (IProjectManager). `applied` = the in-memory descriptor changed;
	// `saved` = the .uproject was written to disk. Takes effect on the
	// next editor restart (modules load/unload at startup). Rejected in
	// read-only mode (mutates the project file).
	struct SetPluginEnabledResult {
		bool applied = false;
		bool saved = false;
		std::string message;   // fail reason, when applied/saved is false
	};
	virtual SetPluginEnabledResult SetPluginEnabled(std::string_view pluginName, bool enabled) {
		(void)pluginName; (void)enabled;
		throw BlueprintReaderError("SetPluginEnabled not supported by this backend");
	}

	// World Partition streaming sources (camera / player providers driving
	// cell streaming). Each: name + world transform. `has_world_partition`
	// false on non-partitioned maps. Phase 14 — world/partition.
	struct StreamingSourceInfo {
		std::string name;
		double locX = 0.0, locY = 0.0, locZ = 0.0;
		double pitch = 0.0, yaw = 0.0, roll = 0.0;
	};
	struct StreamingSourcesResult {
		std::vector<StreamingSourceInfo> sources;
		bool hasWorldPartition = false;
	};
	virtual StreamingSourcesResult GetStreamingSources() {
		throw BlueprintReaderError("GetStreamingSources not supported by this backend");
	}

	// Packages saved during this editor session, most-recent first
	// (package paths). Backed by a ring buffer the editor populates from
	// UPackage::PackageSavedWithContextEvent — empty in a fresh one-shot
	// commandlet, accumulates in a live editor. Phase 14 — recent activity.
	struct RecentSavedPackagesResult {
		std::vector<std::string> packagePaths;
	};
	virtual RecentSavedPackagesResult GetRecentlySavedPackages() {
		throw BlueprintReaderError("GetRecentlySavedPackages not supported by this backend");
	}

	// ===== Phase 16 H Tier 2 — ConfigSettings nav (read) ===============
	// All Project/Editor Settings sections (UDeveloperSettings CDOs):
	// container (Project/Editor) → category → section, plus the settings
	// class path. The 3-tier nav read that get/set tools drill into.
	struct ProjectSettingInfo {
		std::string container;   // "Project" | "Editor"
		std::string category;
		std::string section;
		std::string classPath;
	};
	struct ProjectSettingsResult {
		std::vector<ProjectSettingInfo> sections;
	};
	virtual ProjectSettingsResult ListProjectSettings() {
		throw BlueprintReaderError("ListProjectSettings not supported by this backend");
	}

	// All property values of one settings section (UDeveloperSettings CDO),
	// each `{name, value, type}` (value is the reflection-exported text).
	// `class_path` is a section's class path from list_project_settings.
	// `valid:false` when the class can't be resolved. Phase 16.
	struct SettingValueInfo {
		std::string name;
		std::string value;
		std::string type;
	};
	struct ProjectSettingValuesResult {
		bool valid = false;
		std::string classPath;
		std::vector<SettingValueInfo> values;
	};
	virtual ProjectSettingValuesResult GetProjectSettingValues(std::string_view classPath) {
		(void)classPath;
		throw BlueprintReaderError("GetProjectSettingValues not supported by this backend");
	}

	// Set one property on a settings section (UDeveloperSettings CDO) by
	// class path + property name, importing `value` from text, then
	// persisting to the class's Default*.ini. `applied` = the import +
	// save succeeded. Rejected in read-only mode (writes config). Phase 16.
	struct SetProjectSettingResult {
		bool applied = false;
		std::string message;   // failure detail when applied is false
	};
	virtual SetProjectSettingResult SetProjectSetting(std::string_view classPath,
													  std::string_view property,
													  std::string_view value) {
		(void)classPath; (void)property; (void)value;
		throw BlueprintReaderError("SetProjectSetting not supported by this backend");
	}

	// Registered automation tests (FAutomationTestFramework, synchronous).
	// Each `{display_name, full_path, test_name}` — `test_name` / `full_path`
	// feed the pattern arg of run_automation_tests. Phase 16 — AutomationTest.
	struct AutomationTestEntry {
		std::string displayName;
		std::string fullPath;
		std::string testName;
	};
	struct AutomationTestsResult {
		std::vector<AutomationTestEntry> tests;
		bool truncated = false;
	};
	virtual AutomationTestsResult ListAutomationTests() {
		throw BlueprintReaderError("ListAutomationTests not supported by this backend");
	}

	// ===== Phase 10 (EA-push) — editor event SOURCES ===================
	// Drains buffered Tier-A editor events (UE delegates the editor module
	// subscribes to: selection-changed, asset-opened, PIE start/stop, ...).
	// Each `{name, params_json}` where name is a notifications/editor/<x>
	// suffix and params_json is the event's JSON params. Most-recent last;
	// draining clears the buffer. Empty in a fresh one-shot commandlet;
	// accumulates in a live/daemon editor. This is the event-capture half
	// of push — agents can poll it now; an SSE auto-push (drain ->
	// Server::QueueNotification) is a thin follow-up on top of C5.
	struct EditorEventEntry {
		std::string name;
		std::string paramsJson;
	};
	struct EditorEventsResult {
		std::vector<EditorEventEntry> events;
	};
	virtual EditorEventsResult GetEditorEvents() {
		throw BlueprintReaderError("GetEditorEvents not supported by this backend");
	}

	// Active cook target platforms (ITargetPlatformManagerModule) +
	// the editor's running platform. Phase 14 — system state.
	struct CookTargetResult {
		std::vector<std::string> platforms;
		std::string runningPlatform;
	};
	virtual CookTargetResult GetActiveCookTarget() {
		throw BlueprintReaderError("GetActiveCookTarget not supported by this backend");
	}

	// Editor workspace docking layout, serialized via
	// FGlobalTabmanager::PersistLayout()->ToString(). Phase 17 — advanced.
	struct WorkspaceLayoutResult {
		std::string layout;  // empty when Slate isn't initialized (commandlet)
	};
	virtual WorkspaceLayoutResult GetWorkspaceLayout() {
		throw BlueprintReaderError("GetWorkspaceLayout not supported by this backend");
	}

	// Unreal Insights trace connection state (FTraceAuxiliary, Core module).
	// Phase 17 — advanced. Real out-of-process (no live editor strictly
	// required; the commandlet's own process reports its trace state).
	struct TraceStateResult {
		bool connected = false;
		bool paused = false;
		std::string connectionType;  // "network" | "file" | "none"
		std::string destination;     // server host or file path
		std::string activeChannels;  // comma-separated enabled channels
	};
	virtual TraceStateResult GetTraceState() {
		throw BlueprintReaderError("GetTraceState not supported by this backend");
	}

	// Shared responder for editor UI-state surfaces that live entirely in
	// transient Slate widgets (World Outliner expansion, Details-panel
	// filter, active toast notifications, status-bar text, …). An
	// out-of-process server can't read them today; the commandlet returns
	// `valid:false` with a reason. Routed through the backend (not a fixed
	// constant) so a future in-editor Slate-introspection op can fill a
	// given `feature` in without changing the tool surface. Phase 14/17.
	struct UiStateStubResult {
		bool valid = false;
		std::string reason;
	};
	virtual UiStateStubResult GetUiStateStub(std::string_view /*feature*/) {
		throw BlueprintReaderError("GetUiStateStub not supported by this backend");
	}

	// ----- Stage 4: Niagara / Sequencer / GAS / AnimGraph ---------------

	struct NiagaraEmitterHandleInfo {
		std::string name;
		std::string emitterPath;  // package path of the underlying UNiagaraEmitter
		bool enabled = false;
	};
	struct NiagaraSystemInfo {
		std::string assetPath;
		std::vector<NiagaraEmitterHandleInfo> emitters;
		std::vector<std::string> parameterNames;
	};
	virtual std::vector<BPAssetSummary> ListNiagaraSystems(std::string_view path) {
		(void)path;
		throw BlueprintReaderError("ListNiagaraSystems not supported by this backend");
	}
	virtual NiagaraSystemInfo ReadNiagaraSystem(std::string_view assetPath) {
		(void)assetPath;
		throw BlueprintReaderError("ReadNiagaraSystem not supported by this backend");
	}
	struct CreateNiagaraSystemResult {
		std::string assetPath;
		bool created = false;
		bool alreadyExisted = false;
	};
	virtual CreateNiagaraSystemResult CreateNiagaraSystem(std::string_view assetPath) {
		(void)assetPath;
		throw BlueprintReaderError("CreateNiagaraSystem not supported by this backend");
	}
	struct SetNiagaraParameterResult {
		std::string assetPath;
		std::string parameterName;
		std::string newValue;
		bool applied = false;
	};
	virtual SetNiagaraParameterResult SetNiagaraParameter(std::string_view assetPath,
		std::string_view parameterName, std::string_view value) {
		(void)assetPath; (void)parameterName; (void)value;
		throw BlueprintReaderError("SetNiagaraParameter not supported by this backend");
	}

	struct SequenceTrackInfo {
		std::string trackName;
		std::string trackClass;
		int sectionCount = 0;
	};
	struct LevelSequenceInfo {
		std::string assetPath;
		double startSeconds = 0.0;
		double endSeconds = 0.0;
		std::vector<SequenceTrackInfo> tracks;
	};
	virtual std::vector<BPAssetSummary> ListLevelSequences(std::string_view path) {
		(void)path;
		throw BlueprintReaderError("ListLevelSequences not supported by this backend");
	}
	virtual LevelSequenceInfo ReadLevelSequence(std::string_view assetPath) {
		(void)assetPath;
		throw BlueprintReaderError("ReadLevelSequence not supported by this backend");
	}
	struct AddSequenceTrackResult {
		std::string assetPath;
		std::string trackName;
		std::string trackClass;
		bool added = false;
	};
	virtual AddSequenceTrackResult AddSequenceTrack(std::string_view assetPath,
		std::string_view trackClass, std::string_view trackName) {
		(void)assetPath; (void)trackClass; (void)trackName;
		throw BlueprintReaderError("AddSequenceTrack not supported by this backend");
	}
	struct SetSequencePlaybackRangeResult {
		std::string assetPath;
		double startSeconds = 0.0;
		double endSeconds = 0.0;
		bool applied = false;
	};
	virtual SetSequencePlaybackRangeResult SetSequencePlaybackRange(std::string_view assetPath,
		double startSeconds, double endSeconds) {
		(void)assetPath; (void)startSeconds; (void)endSeconds;
		throw BlueprintReaderError("SetSequencePlaybackRange not supported by this backend");
	}

	// GAS / GameplayTags
	struct GameplayTagListResult {
		std::vector<std::string> tags;
	};
	virtual GameplayTagListResult ListGameplayTags(std::string_view filter) {
		(void)filter;
		throw BlueprintReaderError("ListGameplayTags not supported by this backend");
	}
	struct AddGameplayTagResult {
		std::string tagName;
		bool added = false;
		bool alreadyExisted = false;
	};
	virtual AddGameplayTagResult AddGameplayTag(std::string_view tagName,
		std::string_view comment) {
		(void)tagName; (void)comment;
		throw BlueprintReaderError("AddGameplayTag not supported by this backend");
	}
	struct AbilitySetEntry {
		std::string abilityClass;
		int level = 1;
	};
	struct AbilitySetInfo {
		std::string assetPath;
		std::vector<AbilitySetEntry> abilities;
	};
	virtual AbilitySetInfo ReadAbilitySet(std::string_view assetPath) {
		(void)assetPath;
		throw BlueprintReaderError("ReadAbilitySet not supported by this backend");
	}

	// AnimGraph
	struct AnimStateInfo {
		std::string name;
		std::string kind;  // "state" | "conduit" | "transition" | "entry"
	};
	struct AnimStateMachineInfo {
		std::string name;
		std::vector<AnimStateInfo> states;
	};
	struct AnimBlueprintInfo {
		std::string assetPath;
		std::string parentClass;
		std::vector<AnimStateMachineInfo> stateMachines;
	};
	virtual std::vector<BPAssetSummary> ListAnimBlueprints(std::string_view path) {
		(void)path;
		throw BlueprintReaderError("ListAnimBlueprints not supported by this backend");
	}
	virtual AnimBlueprintInfo ReadAnimBlueprint(std::string_view assetPath) {
		(void)assetPath;
		throw BlueprintReaderError("ReadAnimBlueprint not supported by this backend");
	}
	struct AddAnimStateResult {
		std::string assetPath;
		std::string stateMachine;
		std::string stateName;
		bool added = false;
	};
	virtual AddAnimStateResult AddAnimState(std::string_view assetPath,
		std::string_view stateMachine, std::string_view stateName) {
		(void)assetPath; (void)stateMachine; (void)stateName;
		throw BlueprintReaderError("AddAnimState not supported by this backend");
	}
	struct CompileAnimBlueprintResult {
		std::string assetPath;
		bool compiled = false;
	};
	virtual CompileAnimBlueprintResult CompileAnimBlueprint(std::string_view assetPath) {
		(void)assetPath;
		throw BlueprintReaderError("CompileAnimBlueprint not supported by this backend");
	}

	// ----- Live editor ops -----------------------------------------------
	//
	// These are most useful with an open editor (live backend). The
	// commandlet daemon still routes them — they operate on the headless
	// editor's state — but PIE start/stop in commandlet mode is weird
	// semantically. Auto backend prefers live whenever the editor is open.

	// Execute a UE console command (e.g. "stat unit", "showflag.bones 1").
	// Returns whatever the command echoed to the log + an `ok` flag.
	struct ConsoleCommandResult {
		std::string output;   // log buffer captured during execution
	};
	virtual ConsoleCommandResult ConsoleCommand(std::string_view command) {
		(void)command;
		throw BlueprintReaderError("ConsoleCommand not supported by this backend");
	}

	// Read / write a console variable (CVar). Get returns the current
	// value as a string; Set forces ECVF_SetByCode priority.
	struct CVarValue {
		std::string name;
		std::string value;
		std::string help;     // CVar's help text, if registered
		bool        exists = false;
	};
	virtual CVarValue GetCVar(std::string_view name) {
		(void)name;
		throw BlueprintReaderError("GetCVar not supported by this backend");
	}
	virtual CVarValue SetCVar(std::string_view name, std::string_view value) {
		(void)name; (void)value;
		throw BlueprintReaderError("SetCVar not supported by this backend");
	}

	// Start / stop Play-In-Editor. PIE modes: "selected_viewport" (default),
	// "new_editor_window", "standalone", "vr_preview". Stop is a no-op when
	// PIE isn't active.
	struct PieResult {
		bool started = false;
		bool stopped = false;
		std::string mode;
		// Optional human-readable explanation — e.g. why `started` is false
		// in a headless (-nullrhi) session that can't sustain a play world.
		std::string note;
	};
	virtual PieResult PieStart(std::string_view mode = "selected_viewport") {
		(void)mode;
		throw BlueprintReaderError("PieStart not supported by this backend");
	}
	virtual PieResult PieStop() {
		throw BlueprintReaderError("PieStop not supported by this backend");
	}

	// ===== Phase 8 (EA-pull Wave 1, partial) ============================
	// Editor-awareness reads. "What is the user doing right now?" — the
	// reactive-workflow foundation. All require a live editor; commandlet
	// mode throws "live editor required" by default. Mock throws
	// "not supported by this backend" so apply_ops can plan against them.

	// Asset opened in some asset editor right now (one entry per editor
	// window). Sourced from `UAssetEditorSubsystem::GetAllEditedAssets`.
	struct OpenAssetInfo {
		std::string assetPath;          // /Game/...
		std::string assetClass;         // short UClass name (Blueprint, Material, ...)
		double      lastActivationSeconds = 0.0;  // GetLastActivationTime — seconds since boot
	};
	struct OpenAssetsResult {
		std::vector<OpenAssetInfo> entries;
	};
	virtual OpenAssetsResult ListOpenAssets() {
		throw BlueprintReaderError("ListOpenAssets not supported by this backend");
	}

	// The asset whose editor was most recently activated. Mirrors what
	// the editor UI considers "in focus". `assetPath` is empty when no
	// asset editor is open.
	struct ActiveAssetResult {
		std::string assetPath;
		std::string assetClass;
		double      lastActivationSeconds = 0.0;
	};
	virtual ActiveAssetResult GetActiveAsset() {
		throw BlueprintReaderError("GetActiveAsset not supported by this backend");
	}

	// Blueprint compile status — wraps the `UBlueprint::Status` enum into
	// a stable string. Useful after a write op to verify the BP is in a
	// healthy state before further mutations. Status strings:
	// "uncompiled", "dirty", "good", "warning", "error", "compiling",
	// "unknown".
	struct CompileStatusResult {
		std::string assetPath;
		std::string status;
		std::string lastCompileError;     // empty when status != "error"
	};
	virtual CompileStatusResult GetCompileStatus(std::string_view assetPath) {
		(void)assetPath;
		throw BlueprintReaderError("GetCompileStatus not supported by this backend");
	}

	// Walk loaded UPackages, return those whose IsDirty flag is set.
	// Pairs with `save_all` — agents that just mutated multiple BPs use
	// this to confirm there's nothing unsaved before walking away.
	struct DirtyPackageInfo {
		std::string packageName;          // /Game/...
		bool isContentPackage = false;    // true for /Game/... false for editor / engine packages
	};
	struct DirtyPackagesResult {
		std::vector<DirtyPackageInfo> packages;
	};
	virtual DirtyPackagesResult GetDirtyPackages() {
		throw BlueprintReaderError("GetDirtyPackages not supported by this backend");
	}

	// Title + class name of the currently-focused top-level window.
	// Empty when no Slate window has focus (rare — usually the active
	// editor or one of its tabs). Useful for "is the user in the BP
	// editor right now?" routing.
	struct FocusedWindowResult {
		std::string title;
		std::string className;            // Slate widget class — e.g. "SLevelEditor"
	};
	virtual FocusedWindowResult GetFocusedWindow() {
		throw BlueprintReaderError("GetFocusedWindow not supported by this backend");
	}

	// PIE state — is Play-In-Editor active right now? Mirrors what
	// PieStart/PieStop manage from the write side. `mode` is the mode
	// string PIE was started with (when known). Multi-instance PIE
	// (Client/Server world split) returns instance_count >= 2.
	struct PieStateResult {
		bool isPlaying = false;
		std::string mode;           // "" when not playing
		int  instanceCount = 0;     // number of PIE worlds
	};
	virtual PieStateResult GetPieState() {
		throw BlueprintReaderError("GetPieState not supported by this backend");
	}

	// Modal state — is a modal Slate window blocking input right now?
	// Common cause: a confirm-deletion dialog, asset-picker, save-as.
	// Agents should refuse mutation ops while modal — the editor is
	// gated waiting for user confirmation.
	struct ModalStateResult {
		bool isOpen = false;
		std::string title;          // "" when no modal
		// TEST-2 P0: the modal's buttons, [{path, label?}] — so an agent can
		// see what answers the dialog offers. Empty when no modal is open.
		nlohmann::json buttons = nlohmann::json::array();
		// True when the modal's widget walk hit its budget and some buttons may
		// be missing (practically unreachable — the cap comfortably exceeds a
		// real modal — but surfaced rather than silently dropping a button).
		bool buttonsTruncated = false;
	};
	virtual ModalStateResult GetModalState() {
		throw BlueprintReaderError("GetModalState not supported by this backend");
	}

	// Active editor mode — what mode is the level editor in
	// (Selection / Placement / Landscape / Foliage / Modeling / etc.).
	// Returns a vector because the level editor supports multiple
	// concurrent modes; the primary is element [0].
	struct EditorModesResult {
		std::vector<std::string> activeModes;  // mode IDs (FName strings)
	};
	virtual EditorModesResult GetActiveEditorMode() {
		throw BlueprintReaderError("GetActiveEditorMode not supported by this backend");
	}

	// Slate widget the user is currently typing into / focused on.
	// Finer-grained than get_focused_window: tells you "the user is in
	// the BP graph search box" vs "the user is on the BP editor window".
	// `widget_type` is the Slate widget class (SEditableTextBox, etc.).
	// Empty when no widget has focus.
	struct FocusedWidgetResult {
		std::string widgetType;
		std::string parentWindowTitle;  // title of the window the widget lives in
	};
	virtual FocusedWidgetResult GetFocusedWidget() {
		throw BlueprintReaderError("GetFocusedWidget not supported by this backend");
	}

	// Open the asset editor for the given asset (if not already open).
	// Idempotent: opening an asset whose editor is already up just brings
	// it to front. `assetPath` is package-form (/Game/AI/BP_Foo).
	// `opened` true on success; false if the asset couldn't be loaded.
	struct OpenAssetEditorResult {
		bool opened = false;
		std::string assetPath;
	};
	virtual OpenAssetEditorResult OpenAssetEditor(std::string_view assetPath) {
		(void)assetPath;
		throw BlueprintReaderError("OpenAssetEditor not supported by this backend");
	}

	// Close all editors for the given asset (if any). Idempotent: closing
	// an asset with no editor open is a no-op (closed=false, no error).
	struct CloseAssetEditorResult {
		bool closed = false;
		std::string assetPath;
	};
	virtual CloseAssetEditorResult CloseAssetEditor(std::string_view assetPath) {
		(void)assetPath;
		throw BlueprintReaderError("CloseAssetEditor not supported by this backend");
	}

	// Active level-editor viewport's camera state — location, rotation, FOV.
	// Picks the focused level viewport (or first perspective if none has
	// focus). `valid` false when no level viewport client exists yet (PIE
	// teardown, headless commandlet).
	struct CameraTransformResult {
		bool valid = false;
		double locX = 0.0, locY = 0.0, locZ = 0.0;
		double pitch = 0.0, yaw = 0.0, roll = 0.0;
		double fov = 0.0;
	};
	virtual CameraTransformResult GetCameraTransform() {
		throw BlueprintReaderError("GetCameraTransform not supported by this backend");
	}

	// Active level-editor viewport's view mode (Lit, Unlit, Wireframe,
	// DetailLighting, etc.). String form of the EViewModeIndex enum.
	// `valid` false when no viewport is available.
	struct ViewModeResult {
		bool valid = false;
		std::string mode;
	};
	virtual ViewModeResult GetViewMode() {
		throw BlueprintReaderError("GetViewMode not supported by this backend");
	}

	// Active level-editor viewport's commonly-toggled show flags. Returns a
	// curated subset of FEngineShowFlags' ~100 fields — the ones agents
	// actually care about when describing what's drawn. Full enumeration of
	// every flag is intentionally not exposed.
	struct ShowFlagsResult {
		bool valid = false;
		bool wireframe = false;
		bool collision = false;
		bool grid = false;
		bool bounds = false;
		bool navigation = false;
		bool atmosphere = false;
		bool fog = false;
		bool lighting = false;
		bool postProcessing = false;
		bool antialiasing = false;
		bool shadows = false;
	};
	virtual ShowFlagsResult GetShowFlags() {
		throw BlueprintReaderError("GetShowFlags not supported by this backend");
	}

	// Components of currently-selected actors. Returns nested
	// `[{actor_name, components: [{name, component_class}]}]`. Useful when
	// an agent wants to operate on a specific component without first asking
	// the user to drill into the actor.
	struct SelectedComponentInfo {
		std::string name;
		std::string componentClass;
	};
	struct SelectedActorComponents {
		std::string actorName;
		std::vector<SelectedComponentInfo> components;
	};
	struct SelectedComponentsResult {
		std::vector<SelectedActorComponents> actors;
	};
	virtual SelectedComponentsResult GetSelectedComponents() {
		throw BlueprintReaderError("GetSelectedComponents not supported by this backend");
	}

	// Content browser selected-asset paths (package form). Multi-select
	// preserves order. Empty when nothing is selected.
	struct ContentBrowserSelectionResult {
		std::vector<std::string> assetPaths;
	};
	virtual ContentBrowserSelectionResult GetSelectedAssets() {
		throw BlueprintReaderError("GetSelectedAssets not supported by this backend");
	}

	// Set content browser asset selection. Replaces any prior selection.
	// `assetPaths` are package-form (/Game/...). `selected` is the
	// post-call confirmation; agents poll-for-settle if needed since
	// IContentBrowserSingleton::SyncBrowserToAssets is async on UI.
	virtual ContentBrowserSelectionResult SetSelectedAssets(
		const std::vector<std::string>& assetPaths) {
		(void)assetPaths;
		throw BlueprintReaderError("SetSelectedAssets not supported by this backend");
	}

	// Content browser selected folder paths (package form). Distinct
	// from `GetSelectedAssets` — folders aren't .uasset, they're tree
	// navigation rows.
	struct ContentBrowserFoldersResult {
		std::vector<std::string> folderPaths;
	};
	virtual ContentBrowserFoldersResult GetSelectedFolders() {
		throw BlueprintReaderError("GetSelectedFolders not supported by this backend");
	}

	// The folder the content browser is currently displaying (the
	// "address-bar" path). Single string in package form.
	struct ContentBrowserPathResult {
		std::string currentPath;
	};
	virtual ContentBrowserPathResult GetContentBrowserPath() {
		throw BlueprintReaderError("GetContentBrowserPath not supported by this backend");
	}

	// Navigate the content browser to `folderPath`. Returns the
	// post-call path (verifies the navigation took effect). `folderPath`
	// is package-form (e.g. /Game/AI/Behaviors).
	virtual ContentBrowserPathResult SetContentBrowserPath(
		std::string_view folderPath) {
		(void)folderPath;
		throw BlueprintReaderError("SetContentBrowserPath not supported by this backend");
	}

	// Project a 3D world position to viewport screen space using the
	// active level viewport's camera. `valid` false when no viewport
	// or projection fails (behind camera, W=0). `screen_x`/`screen_y`
	// are normalized [0,1] across the viewport; `is_on_screen` is
	// the conjunction of (valid AND coords in [0,1] AND in front of camera).
	struct WorldToScreenResult {
		bool valid = false;
		double screenX = 0.0;
		double screenY = 0.0;
		bool isOnScreen = false;
	};
	virtual WorldToScreenResult WorldToScreen(double worldX, double worldY, double worldZ) {
		(void)worldX; (void)worldY; (void)worldZ;
		throw BlueprintReaderError("WorldToScreen not supported by this backend");
	}

	// Inverse: cast a ray from viewport screen-normalized [0,1] coords
	// out to `maxDistance` cm in world space. Returns the hit world
	// position if anything was hit (line-trace through visibility);
	// otherwise the ray endpoint. `hit` true on intersection. `valid`
	// false when no viewport is active.
	struct ScreenToWorldResult {
		bool valid = false;
		bool hit = false;
		double worldX = 0.0, worldY = 0.0, worldZ = 0.0;
		std::string hitActorName;     // empty when no hit
	};
	virtual ScreenToWorldResult ScreenToWorld(double screenX, double screenY,
											  double maxDistance) {
		(void)screenX; (void)screenY; (void)maxDistance;
		throw BlueprintReaderError("ScreenToWorld not supported by this backend");
	}

	// Recursive Slate widget-tree snapshot. Walks descendants from each
	// visible top-level window (or only the one whose title contains
	// `windowFilter` when set). Each node returns `{depth, type, ?text}`.
	// `maxDepth` caps recursion (default 8). Deeper trees get truncated;
	// `truncated` flags when a subtree was cut off. Useful for "what's
	// on screen?" reasoning without taking an actual screenshot.
	struct UiNode {
		int depth = 0;
		std::string widgetType;          // SWidget::GetTypeAsString
		std::string text;                // visible text when available
		std::string parentWindow;        // window title the widget lives in
	};
	struct UiSnapshotResult {
		std::vector<UiNode> nodes;
		bool truncated = false;
	};
	virtual UiSnapshotResult UiSnapshot(std::string_view windowFilter,
										int maxDepth) {
		(void)windowFilter; (void)maxDepth;
		throw BlueprintReaderError("UiSnapshot not supported by this backend");
	}

	// Find Slate widgets whose visible text contains `text` (case-sensitive
	// substring). Optional `roleFilter` restricts to widgets whose type
	// contains `roleFilter` (e.g. "Button" matches SButton). Walks the
	// same tree as `UiSnapshot`. Empty `text` returns nothing.
	virtual UiSnapshotResult UiFind(std::string_view text,
									std::string_view roleFilter) {
		(void)text; (void)roleFilter;
		throw BlueprintReaderError("UiFind not supported by this backend");
	}

	// List visible top-level windows (titles + types + positions + sizes).
	// Lighter-weight alternative to a desktop screenshot when the agent
	// just wants to know "what windows are open?". Full screenshot
	// composite is deferred — multi-window blitting requires a render
	// thread + texture readback. This is the pure-introspection variant.
	struct DesktopWindowInfo {
		std::string title;
		std::string widgetType;
		double posX = 0.0, posY = 0.0;
		double sizeX = 0.0, sizeY = 0.0;
		bool isActive = false;
	};
	struct DesktopWindowsResult {
		std::vector<DesktopWindowInfo> windows;
	};
	virtual DesktopWindowsResult ListDesktopWindows() {
		throw BlueprintReaderError("ListDesktopWindows not supported by this backend");
	}

	// ===== Phase 11 H Tier 1 — GameFeaturesToolset (read ops) ============
	// Reads against UGameFeaturesSubsystem. Game-feature plugins (GFPs)
	// are the modular content distribution mechanism Lyra uses heavily;
	// these tools let an agent see what's loaded/active and (later) toggle.
	//
	// State strings (simplified from Epic's 34 internal states to 6):
	//   "unknown"      — not yet discovered or unmapped
	//   "registered"   — plugin discovered, not loaded
	//   "loading"      — in-flight transition (any *ing state)
	//   "loaded"       — assets loaded into memory, gameplay actions not active
	//   "active"       — fully running (assets loaded + actions applied)
	//   "deactivating" — in-flight teardown
	struct GameFeatureInfo {
		std::string pluginName;
		std::string pluginUrl;            // e.g. "/Game/Features/Foo/Foo.uplugin"
		std::string state;                // simplified 6-state string
	};
	struct GameFeaturesListResult {
		std::vector<GameFeatureInfo> features;
	};
	virtual GameFeaturesListResult ListGameFeatures() {
		throw BlueprintReaderError("ListGameFeatures not supported by this backend");
	}

	// Single GFP state lookup. `valid` false when the plugin name doesn't
	// resolve to a registered GFP.
	struct GameFeatureStateResult {
		bool valid = false;
		std::string pluginName;
		std::string state;
		std::string pluginUrl;
	};
	virtual GameFeatureStateResult GetGameFeatureState(std::string_view pluginName) {
		(void)pluginName;
		throw BlueprintReaderError("GetGameFeatureState not supported by this backend");
	}

	// ===== Phase 11 H Tier 1 — PluginToolset (read ops) =================
	// Listing + descriptor reads against IPluginManager. Write ops
	// (create, modify-descriptor with SCC awareness) are deferred.

	struct PluginInfo {
		std::string name;
		std::string descriptorPath;     // /full/path/to/Foo.uplugin
		std::string category;           // from FPluginDescriptor::Category
		std::string version;            // VersionName from descriptor
		bool isEnabled = false;
		bool isBuiltIn = false;
		bool isContentOnly = false;
	};
	struct PluginListResult {
		std::vector<PluginInfo> plugins;
	};
	virtual PluginListResult ListPlugins() {
		throw BlueprintReaderError("ListPlugins not supported by this backend");
	}

	// Full descriptor dump for a single plugin (parsed JSON from the
	// .uplugin file). Returns the raw descriptor JSON so callers don't
	// have to enumerate every field; the structure matches Epic's
	// FPluginDescriptor schema. `valid` false when name doesn't resolve.
	struct PluginDescriptorResult {
		bool valid = false;
		std::string name;
		nlohmann::json descriptor;      // full descriptor JSON or null
	};
	virtual PluginDescriptorResult GetPluginDescriptor(std::string_view pluginName) {
		(void)pluginName;
		throw BlueprintReaderError("GetPluginDescriptor not supported by this backend");
	}

	// Plugin dependency list — extracted from FPluginDescriptor::Plugins.
	// Returns the names of plugins this one depends on. Empty when the
	// plugin has no `Plugins:` array in its descriptor.
	struct PluginDependenciesResult {
		bool valid = false;
		std::string name;
		std::vector<std::string> dependencies;
	};
	virtual PluginDependenciesResult GetPluginDependencies(std::string_view pluginName) {
		(void)pluginName;
		throw BlueprintReaderError("GetPluginDependencies not supported by this backend");
	}

	// ===== Phase 11 H Tier 1 — AbilitySystemInspectorToolset =============
	// Introspection against an actor's `UAbilitySystemComponent`. Lyra
	// uses GAS heavily; these let an agent see what abilities/tags are
	// granted to a given actor at runtime.

	// Abilities currently granted to the actor. `instanced_count`
	// reports how many instances of each ability are spawned (relevant
	// for abilities marked InstancedPerExecution / InstancedPerActor).
	struct ActorAbilityInfo {
		std::string abilityClass;        // UGameplayAbility class path
		bool isActive = false;
		int  level = 1;
		int  instancedCount = 0;
	};
	struct ActorAbilitiesResult {
		bool valid = false;              // false when actor has no ASC
		std::string actorName;
		std::vector<ActorAbilityInfo> abilities;
	};
	virtual ActorAbilitiesResult ListActorAbilities(std::string_view actorName) {
		(void)actorName;
		throw BlueprintReaderError("ListActorAbilities not supported by this backend");
	}

	// Owned gameplay tags on the actor (the union of all tag containers
	// the ASC is tracking: granted-by-abilities, granted-by-effects,
	// loose tags). Returned as flat string list.
	struct ActorTagsResult {
		bool valid = false;
		std::string actorName;
		std::vector<std::string> tags;
	};
	virtual ActorTagsResult ListActorGameplayTags(std::string_view actorName) {
		(void)actorName;
		throw BlueprintReaderError("ListActorGameplayTags not supported by this backend");
	}

	// Gameplay attribute values on the actor's ASC. Returns both base
	// (raw value before mods) and current (after active effects). The
	// attribute name format matches what UAttributeSet uses internally:
	// "AttributeSetClassName.AttributeName" (e.g. "LyraHealthSet.Health").
	struct ActorAttributeInfo {
		std::string name;
		double baseValue = 0.0;
		double currentValue = 0.0;
	};
	struct ActorAttributesResult {
		bool valid = false;
		std::string actorName;
		std::vector<ActorAttributeInfo> attributes;
	};
	virtual ActorAttributesResult ListActorAttributes(std::string_view actorName) {
		(void)actorName;
		throw BlueprintReaderError("ListActorAttributes not supported by this backend");
	}

	// Active gameplay effects on the actor's ASC. Per-effect metadata
	// includes stack count, remaining duration (-1 for infinite), level
	// when set, and tags granted by the effect.
	struct ActorEffectInfo {
		std::string effectClass;
		int  stackCount = 1;
		double durationRemaining = 0.0;  // -1 sentinel for infinite
		double level = 1.0;
		std::vector<std::string> grantedTags;
	};
	struct ActorEffectsResult {
		bool valid = false;
		std::string actorName;
		std::vector<ActorEffectInfo> effects;
	};
	virtual ActorEffectsResult ListActorGameplayEffects(std::string_view actorName) {
		(void)actorName;
		throw BlueprintReaderError("ListActorGameplayEffects not supported by this backend");
	}

	// ===== Phase 12 EA-pull Wave 2 — Per-asset-editor selection ========
	// Drill into editor-specific state for specific asset editors.
	// Each tool resolves via IAssetEditorInstance + cast to the specific
	// editor type. `valid` false when asset editor isn't open or asset
	// type doesn't match the expected editor class.

	// Per-Blueprint-editor state: which nodes are selected, the active
	// graph tab, whether the BP compiled cleanly. Useful for "I'm in the
	// graph for BP_Foo, agent help me wire this"-style flows.
	struct BlueprintEditorStateResult {
		bool valid = false;
		std::string assetPath;
		std::vector<std::string> selectedNodeIds;   // FGuid strings
		std::string currentGraphName;               // empty when none focused
		std::string compileStatus;                  // shared with get_compile_status
	};
	virtual BlueprintEditorStateResult GetBlueprintEditorState(std::string_view assetPath) {
		(void)assetPath;
		throw BlueprintReaderError("GetBlueprintEditorState not supported by this backend");
	}

	// Material instance parameter dump. Unlike most Wave-2 tools this
	// doesn't need an open editor — UMaterialInstance exposes the
	// parameter arrays publicly via the UAsset. Returns scalar/vector/
	// texture parameter values; switch parameters and runtime virtual
	// textures are excluded as a scope cap.
	struct MaterialInstanceScalarParam {
		std::string name;
		double value = 0.0;
	};
	struct MaterialInstanceVectorParam {
		std::string name;
		double r = 0.0, g = 0.0, b = 0.0, a = 0.0;
	};
	struct MaterialInstanceTextureParam {
		std::string name;
		std::string texturePath;
	};
	struct MaterialInstanceParamsResult {
		bool valid = false;
		std::string assetPath;
		std::string parentPath;     // the parent UMaterial path
		std::vector<MaterialInstanceScalarParam>  scalars;
		std::vector<MaterialInstanceVectorParam>  vectors;
		std::vector<MaterialInstanceTextureParam> textures;
	};
	virtual MaterialInstanceParamsResult GetMaterialInstanceParams(std::string_view assetPath) {
		(void)assetPath;
		throw BlueprintReaderError("GetMaterialInstanceParams not supported by this backend");
	}

	// Static-mesh asset-direct info — LOD count, triangle / vertex counts
	// per LOD. No editor instance required (reads from the loaded asset).
	struct StaticMeshLODInfo {
		int triangleCount = 0;
		int vertexCount = 0;
		double screenSize = 0.0;        // for LOD streaming threshold
	};
	struct StaticMeshInfoResult {
		bool valid = false;
		std::string assetPath;
		int lodCount = 0;
		bool isNaniteEnabled = false;
		std::vector<StaticMeshLODInfo> lods;
	};
	virtual StaticMeshInfoResult GetStaticMeshInfo(std::string_view assetPath) {
		(void)assetPath;
		throw BlueprintReaderError("GetStaticMeshInfo not supported by this backend");
	}

	// UMG widget-blueprint editor state. `selected_widget_names` are
	// FWidgetReference template names (matches the names in the widget
	// hierarchy panel). `valid` false when UMG editor isn't open.
	struct UmgEditorStateResult {
		bool valid = false;
		std::string assetPath;
		std::vector<std::string> selectedWidgetNames;
		std::string currentDesignerTab;
	};
	virtual UmgEditorStateResult GetUmgEditorState(std::string_view assetPath) {
		(void)assetPath;
		throw BlueprintReaderError("GetUmgEditorState not supported by this backend");
	}

	// Material editor selection. `selected_expression_classes` lists the
	// short class names of currently-selected material expressions
	// (e.g. "MaterialExpressionAdd", "MaterialExpressionTextureSample").
	// `valid:false` when material editor isn't open for the asset.
	struct MaterialEditorStateResult {
		bool valid = false;
		std::string assetPath;
		int selectedNodeCount = 0;
		std::vector<std::string> selectedExpressionClasses;
	};
	virtual MaterialEditorStateResult GetMaterialEditorState(std::string_view assetPath) {
		(void)assetPath;
		throw BlueprintReaderError("GetMaterialEditorState not supported by this backend");
	}

	// Static mesh editor preview state: which LOD is being previewed,
	// LOD-auto-select state. `current_lod_level` is -1 for "auto" or the
	// explicit LOD index when forced.
	struct MeshPreviewStateResult {
		bool valid = false;
		std::string assetPath;
		int currentLODLevel = -1;
		int currentLODIndex = 0;
	};
	virtual MeshPreviewStateResult GetMeshPreviewState(std::string_view assetPath) {
		(void)assetPath;
		throw BlueprintReaderError("GetMeshPreviewState not supported by this backend");
	}

	// Currently-active camera in the PIE world. Reads
	// `APlayerCameraManager::GetViewTarget()` of the first player
	// controller. When PIE isn't running or no view target is set,
	// returns `valid:false`. Useful for inspecting what a cinematic
	// sequence is showing the player right now.
	struct CinematicCameraResult {
		bool valid = false;
		std::string actorName;        // view target's name
		double locX = 0.0, locY = 0.0, locZ = 0.0;
		double pitch = 0.0, yaw = 0.0, roll = 0.0;
		double fov = 0.0;
	};
	virtual CinematicCameraResult GetCinematicCamera() {
		throw BlueprintReaderError("GetCinematicCamera not supported by this backend");
	}

	// Sequencer state for an open ULevelSequence editor. Returns the
	// playhead time in seconds (global, i.e. unwrapped if sub-sequences
	// are nested), the playback status as a simplified string, and the
	// playback range bounds. `valid:false` when the level sequence isn't
	// open in the editor.
	//
	// Status strings:
	//   "stopped" / "playing" / "scrubbing" / "jumping" / "stepping" /
	//   "paused" / "unknown"
	struct SequencerStateResult {
		bool valid = false;
		std::string assetPath;
		double playheadSeconds = 0.0;
		std::string playbackStatus;
		double playbackRangeStartSeconds = 0.0;
		double playbackRangeEndSeconds   = 0.0;
	};
	virtual SequencerStateResult GetSequencerState(std::string_view assetPath) {
		(void)assetPath;
		throw BlueprintReaderError("GetSequencerState not supported by this backend");
	}

	// Persona / Animation editor selection state. v1 covers
	// IPersonaPreviewScene's selected bone / socket (the cheap subset of
	// the original plan's `{scrubber_seconds, selected_curves,
	// preview_paused}`). Scrubber position is on the preview mesh
	// component's AnimInstance and is more involved to surface.
	struct AnimEditorStateResult {
		bool valid = false;
		std::string assetPath;
		int  selectedBoneIndex = -1;
		std::string selectedSocketName;
	};
	virtual AnimEditorStateResult GetAnimEditorState(std::string_view assetPath) {
		(void)assetPath;
		throw BlueprintReaderError("GetAnimEditorState not supported by this backend");
	}

	// Niagara module editor selection. v1 stub — same RTTI / multi-
	// inheritance limit as anim editor. Niagara editors use specialized
	// toolkit types in the Niagara plugin's editor module; safe cross-
	// casting requires a sidecar registry.
	struct NiagaraModuleSelectionResult {
		bool valid = false;
		std::string assetPath;
		std::vector<std::string> selectedModuleNames;
	};
	virtual NiagaraModuleSelectionResult GetNiagaraModuleSelection(std::string_view assetPath) {
		(void)assetPath;
		throw BlueprintReaderError("GetNiagaraModuleSelection not supported by this backend");
	}

	// Curve editor selection — generic widget hosted by Anim / Sequencer
	// / Particle / Material editors. There's no single "current curve
	// editor" instance keyed on asset_path. v1 stub — the upgrade path
	// requires tracking the active curve editor via the host editor's
	// FCurveEditor pointer per asset.
	struct CurveEditorSelectionResult {
		bool valid = false;
		std::string assetPath;
		int selectedKeyCount = 0;
		std::vector<std::string> selectedCurveNames;
	};
	virtual CurveEditorSelectionResult GetCurveEditorSelection(std::string_view assetPath) {
		(void)assetPath;
		throw BlueprintReaderError("GetCurveEditorSelection not supported by this backend");
	}

	// ===== Phase 13 EA-pull Wave 3 — Viewport + visibility ===============

	// Buffer-visualization mode (base_color/roughness/normals/etc.) on the
	// active level viewport. Empty string when no override is set (default
	// Lit/etc. rendering).
	struct BufferVizModeResult {
		bool valid = false;
		std::string mode;
	};
	virtual BufferVizModeResult GetBufferVisualizationMode() {
		throw BlueprintReaderError("GetBufferVisualizationMode not supported by this backend");
	}

	// Gizmo state — translate/rotate/scale + world/local coord space.
	// Reads `FEditorViewportClient::GetWidgetMode` + `GetWidgetCoordSystemSpace`.
	struct GizmoStateResult {
		bool valid = false;
		std::string mode;       // "translate" / "rotate" / "scale" / "translaterotatez" / "none"
		std::string coordSpace; // "world" / "local"
	};
	virtual GizmoStateResult GetGizmoState() {
		throw BlueprintReaderError("GetGizmoState not supported by this backend");
	}

	// Active level viewport's realtime flag — whether it's rendering
	// every frame (or only on user interaction). `valid:false` when no
	// viewport is available.
	struct ViewportRealtimeResult {
		bool valid = false;
		bool isRealtime = false;
	};
	virtual ViewportRealtimeResult GetViewportRealtime() {
		throw BlueprintReaderError("GetViewportRealtime not supported by this backend");
	}

	// Active viewport's camera settings: FOV, speed, near/far clip.
	struct ViewportCameraSettingsResult {
		bool valid = false;
		double fov = 0.0;
		double cameraSpeed = 0.0;
		double nearClip = 0.0;
		double farClip = 0.0;
	};
	virtual ViewportCameraSettingsResult GetViewportCameraSettings() {
		throw BlueprintReaderError("GetViewportCameraSettings not supported by this backend");
	}

	// Snapping settings from ULevelEditorViewportSettings: whether
	// grid/rot snap is on, current grid sizes (position + rotation),
	// vertex snapping toggle, and snap-to-actor distance.
	struct SnappingSettingsResult {
		bool valid = false;
		bool gridEnabled = false;
		bool rotGridEnabled = false;
		bool snapVertices = false;
		int  currentPosGridSize = 0;       // index into GetPosGridSizes()
		int  currentRotGridSize = 0;       // index into GetRotGridSizes()
		double actorSnapDistance = 0.0;
		double snapDistance = 0.0;
	};
	virtual SnappingSettingsResult GetSnappingSettings() {
		throw BlueprintReaderError("GetSnappingSettings not supported by this backend");
	}

	// Active viewport selector — picks the focused level viewport client
	// and reports its layout slot. `viewport_index` is 0-based across
	// GEditor->GetAllViewportClients() filtered to level editor clients.
	// `is_perspective` distinguishes perspective from ortho (top/front/etc).
	struct ActiveViewportResult {
		bool valid = false;
		int  viewportIndex = -1;
		bool isPerspective = false;
		int  sizeX = 0;
		int  sizeY = 0;
	};
	virtual ActiveViewportResult GetActiveViewport() {
		throw BlueprintReaderError("GetActiveViewport not supported by this backend");
	}

	// List actors hidden in the editor viewport (bHiddenEdTemporary or
	// bHiddenEdLevel from FActorVisibilityChange). Returns actor names
	// only (lookup transforms via get_selected_actors / spawn_actor
	// response shapes). Capped at 500 actors to avoid blowing up on
	// large worlds.
	struct HiddenActorsResult {
		std::vector<std::string> actorNames;
		bool truncated = false;
	};
	virtual HiddenActorsResult GetHiddenActors() {
		throw BlueprintReaderError("GetHiddenActors not supported by this backend");
	}

	// Actors visible in the active level viewport — frustum-tested
	// against the current camera, filtered by class substring and max
	// distance. Hidden actors are skipped. Per-actor metadata: name,
	// label, class short-name, world location, distance from camera,
	// normalized screen position (when projectable).
	struct VisibleActorInfo {
		std::string name;
		std::string label;            // GetActorLabel — display name
		std::string actorClass;       // short class name
		double worldX = 0.0, worldY = 0.0, worldZ = 0.0;
		double distanceCm = 0.0;
		double screenX = 0.0;         // normalized [0,1]
		double screenY = 0.0;
		bool   hasScreenPos = false;  // false when behind camera
	};
	struct VisibleActorsResult {
		std::vector<VisibleActorInfo> actors;
		bool truncated = false;
	};
	virtual VisibleActorsResult GetVisibleActors(std::string_view classFilter,
												 double maxDistanceCm) {
		(void)classFilter; (void)maxDistanceCm;
		throw BlueprintReaderError("GetVisibleActors not supported by this backend");
	}

	// Trigger a Live Coding compile + patch. Returns whether the compile
	// was queued; the actual result is asynchronous (Live Coding emits
	// its own status messages to the log).
	struct LiveCodingResult {
		bool queued = false;
		std::string message;
	};
	virtual LiveCodingResult LiveCodingCompile() {
		throw BlueprintReaderError("LiveCodingCompile not supported by this backend");
	}

	// Editor selection — names of currently-selected actors in the level.
	struct SelectionResult {
		std::vector<std::string> actorNames;
	};
	virtual SelectionResult GetSelectedActors() {
		throw BlueprintReaderError("GetSelectedActors not supported by this backend");
	}

	// One-call situational awareness: what assets are open, which is
	// active, what level is loaded, where's the viewport camera, what
	// actors are selected, is PIE running. Inspired by Epic
	// AIAssistant's Slate-querier surface — a single call answers
	// "what is the user looking at right now?". Returns raw JSON
	// because the shape has 6+ optional fields each with their own
	// structure; constructing a flat struct hierarchy adds noise the
	// tools/call layer just re-serializes anyway.
	virtual BPRJson GetEditorState() {
		throw BlueprintReaderError("GetEditorState not supported by this backend");
	}

	/// EDIT-2: Timeline read tools. Return JSON directly (irregular shape).
	virtual BPRJson ListTimelines(std::string_view assetPath) {
		throw BlueprintReaderError("ListTimelines not supported by this backend");
	}
	virtual BPRJson ReadTimeline(std::string_view assetPath, std::string_view timelineName) {
		throw BlueprintReaderError("ReadTimeline not supported by this backend");
	}
	// EDIT-4: AnimMontage read tools.
	virtual BPRJson ListAnimMontages(std::string_view path) {
		throw BlueprintReaderError("ListAnimMontages not supported by this backend");
	}
	virtual BPRJson ReadAnimMontage(std::string_view assetPath) {
		throw BlueprintReaderError("ReadAnimMontage not supported by this backend");
	}

	// Run a Python script in the editor. Gated server-side by
	// BP_READER_ALLOW_PYTHON=1 — when off, returns
	// {ok: false, error: "python_disabled"} rather than throwing.
	// Mirrors Epic AIAssistant's code-as-tool capability.
	struct PythonResult {
		bool        ok = false;
		std::string error;          // "python_disabled" when env-gated off
		std::string commandResult;
		BPRJson     log;            // array of {type, output} entries
	};
	virtual PythonResult RunPythonScript(std::string_view code) {
		(void)code;
		throw BlueprintReaderError("RunPythonScript not supported by this backend");
	}

	// Asset dependency queries — what references this asset, or what
	// does it reference. Sourced from the AssetRegistry's dependency
	// graph; runs in O(1) per query against the in-memory index.
	struct AssetGraphResult {
		std::vector<std::string> packagePaths;
	};
	virtual AssetGraphResult GetReferencers(std::string_view assetPath) {
		(void)assetPath;
		throw BlueprintReaderError("GetReferencers not supported by this backend");
	}
	virtual AssetGraphResult GetDependencies(std::string_view assetPath) {
		(void)assetPath;
		throw BlueprintReaderError("GetDependencies not supported by this backend");
	}

	// Tools this backend cannot fulfill. Main.cpp consults this at
	// startup to deny-filter the registry, so the catalog only
	// advertises tools that will actually work — agents don't burn
	// turns discovering capability gaps. Default returns empty (all
	// supported); concrete backends override when they're known to be
	// limited (e.g. mock, which has no editor or asset registry).
	virtual std::vector<std::string> UnsupportedTools() const {
		return {};
	}

	// General-purpose asset enumeration. The list_blueprints /
	// list_materials / list_data_tables family are typed slices;
	// FindAsset/ListAssets are the asset-registry-wide queries an
	// agent reaches for when they don't know the asset's UClass.
	//
	// Each row: { asset_path: "/Game/X/Y", name: "Y",
	//             class_name: "<short UClass name>" }.
	struct AssetRegistryEntry {
		std::string assetPath;
		std::string name;
		std::string className;
	};
	struct AssetRegistryListResult {
		std::vector<AssetRegistryEntry> entries;
	};
	virtual AssetRegistryListResult ListAssets(std::string_view path, bool recursive) {
		(void)path; (void)recursive;
		throw BlueprintReaderError("ListAssets not supported by this backend");
	}
	virtual AssetRegistryListResult FindAsset(std::string_view query,
											  std::string_view path) {
		(void)query; (void)path;
		throw BlueprintReaderError("FindAsset not supported by this backend");
	}

	// Read / write a UE config (.ini) value. `file` is one of
	// "Engine" (default), "Game", "Input", "Editor",
	// "EditorPerProjectIni", or a full path. Reads return {exists,
	// value}; writes flush the ini to disk + return the previous
	// value (or null) for verification.
	struct ConfigReadResult {
		bool        exists = false;
		std::string value;
	};
	struct ConfigWriteResult {
		bool        previousExisted = false;
		std::string previousValue;
	};
	virtual ConfigReadResult ReadConfigValue(std::string_view section,
											 std::string_view key,
											 std::string_view file) {
		(void)section; (void)key; (void)file;
		throw BlueprintReaderError("ReadConfigValue not supported by this backend");
	}
	virtual ConfigWriteResult SetConfigValue(std::string_view section,
											 std::string_view key,
											 std::string_view value,
											 std::string_view file) {
		(void)section; (void)key; (void)value; (void)file;
		throw BlueprintReaderError("SetConfigValue not supported by this backend");
	}

	// Trigger a lighting build on the currently-loaded level. Async —
	// returns once the build is QUEUED, not once it finishes. `quality`
	// is "Preview" / "Medium" / "High" / "Production" (default
	// "Production"). Poll read_output_log for completion ("Lighting
	// build complete" or "Lighting build failed" emitted by Lightmass).
	struct BuildLightingResult {
		bool        queued = false;
		std::string quality;
	};
	virtual BuildLightingResult BuildLighting(std::string_view quality = "Production") {
		(void)quality;
		throw BlueprintReaderError("BuildLighting not supported by this backend");
	}
	// `replace=true` clears existing selection first; `false` adds to it.
	virtual SelectionResult SetSelection(
		const std::vector<std::string>& actorNames, bool replace = true) {
		(void)actorNames; (void)replace;
		throw BlueprintReaderError("SetSelection not supported by this backend");
	}

	// Spawn an actor in the current level. `class_path` is the full UClass
	// path (e.g. "/Script/Engine.StaticMeshActor" or a BP class). Location
	// / rotation / scale default to zero / identity / one if unspecified.
	struct SpawnActorResult {
		std::string actorName;   // the spawned actor's GetName()
		std::string actorLabel;  // the user-facing label in the outliner
	};
	virtual SpawnActorResult SpawnActor(
		std::string_view classPath,
		double locX = 0, double locY = 0, double locZ = 0,
		double rotPitch = 0, double rotYaw = 0, double rotRoll = 0,
		double scaleX = 1, double scaleY = 1, double scaleZ = 1) {
		(void)classPath;
		(void)locX; (void)locY; (void)locZ;
		(void)rotPitch; (void)rotYaw; (void)rotRoll;
		(void)scaleX; (void)scaleY; (void)scaleZ;
		throw BlueprintReaderError("SpawnActor not supported by this backend");
	}

	// Update an existing actor's transform. Identified by name (the
	// string returned by SpawnActor / GetSelectedActors).
	virtual void SetActorTransform(
		std::string_view actorName,
		double locX, double locY, double locZ,
		double rotPitch, double rotYaw, double rotRoll,
		double scaleX, double scaleY, double scaleZ) {
		(void)actorName;
		(void)locX; (void)locY; (void)locZ;
		(void)rotPitch; (void)rotYaw; (void)rotRoll;
		(void)scaleX; (void)scaleY; (void)scaleZ;
		throw BlueprintReaderError("SetActorTransform not supported by this backend");
	}

	// Destroy an actor by name.
	struct DeleteActorResult {
		bool deleted = false;
	};
	virtual DeleteActorResult DeleteActor(std::string_view actorName) {
		(void)actorName;
		throw BlueprintReaderError("DeleteActor not supported by this backend");
	}

	// Read the recent output-log buffer. The plugin module installs a
	// ring-buffer output-device sink at startup; this returns up to
	// `limit` of the most recent entries, optionally filtered by minimum
	// severity ("Display" / "Log" / "Warning" / "Error" / "Fatal").
	struct LogEntry {
		std::string severity;   // verbosity name
		std::string category;
		std::string message;
		std::string timestamp;  // ISO-8601 (if captured)
	};
	struct OutputLogResult {
		std::vector<LogEntry> entries;
	};
	virtual OutputLogResult ReadOutputLog(int limit = 200,
										  std::string_view minSeverity = {}) {
		(void)limit; (void)minSeverity;
		throw BlueprintReaderError("ReadOutputLog not supported by this backend");
	}

	// ----- Automation tests ---------------------------------------------
	//
	// Trigger UE's automation test framework. `pattern` is the test-name
	// wildcard (e.g. "BlueprintReader.*", "*Smoke*"); empty means "every
	// registered test". Results land in the output log; this tool kicks
	// off the run and returns immediately.
	struct AutomationRunResult {
		bool started = false;
		std::string message;
	};
	virtual AutomationRunResult RunAutomationTests(std::string_view pattern) {
		(void)pattern;
		throw BlueprintReaderError("RunAutomationTests not supported by this backend");
	}

	// ----- Batch sentinels (A1) ------------------------------------------------
	// BeginBatch / EndBatch wrap a sequence of write ops so the expensive
	// CompileBlueprint + SavePackage runs once per affected BP at EndBatch
	// instead of once per op. apply_ops uses this to collapse N×compile to 1.
	//
	// Default no-op so backends that don't care (mock, future read-only) need
	// no changes. CommandletBlueprintReader overrides to emit the matching
	// -Op=BeginBatch / -Op=EndBatch lines to the daemon.
	//
	// Best-effort failure semantics: if a batch is open and a write op throws,
	// EndBatch should still be called by the caller (in a try/finally pattern)
	// and will compile+save whatever ops landed before the failure.
	//
	// EndBatch returns a JSON object describing the flush: `{ok, recompiled,
	// diagnostics, error_count, warning_count}` (C1). Default implementation
	// returns an empty object — backends without a real compile step have
	// nothing to surface.
	//
	// `skipCompile` is the on_failure="skip" path — the caller knows
	// something failed mid-batch and doesn't want partial state on disk.
	// Plugin honors this by skipping the per-BP compile + save loop in
	// EndBatch (in-memory state stays dirty until daemon restarts;
	// documented as a limitation of strict-atomic mode).
	virtual void BeginBatch() {}
	// skipCompile: true = don't compile/save (on_failure="skip").
	// rollback:    true = undo all in-memory mutations via FScopedTransaction
	//              (on_failure="rollback"); implies !skipCompile since there is
	//              nothing to save after a full undo.
	// saveOnError (REL-2): true = persist BPs even when their final compile
	//              produced errors (pre-REL-2 behavior). Default false — the
	//              flush refuses those saves so a batch can never bake a broken
	//              BP over the last good on-disk state; refused assets are
	//              listed in the ack's `save_skipped` array.
	virtual nlohmann::json EndBatch(bool /*skipCompile*/ = false,
									bool /*rollback*/    = false,
									bool /*saveOnError*/ = false) {
		return nlohmann::json::object();
	}

	// Tear down any backing process / connection / cache the backend holds
	// open. Optional — default is a no-op for backends that don't have one
	// (mock, future live). The CommandletBlueprintReader override sends
	// QUIT to its daemon and joins, freeing the project lock so the user
	// can launch the full editor (or another tool) without contention.
	//
	// Subsequent tool calls auto-respawn the daemon — same path the
	// existing daemon-died fallback uses. So this is safe to call ad-hoc;
	// the next read just pays a one-time cold start.
	//
	// Returns a JSON object describing what happened: {ok:true,
	// was_running:bool, ...}. Backends without a teardownable resource
	// return {ok:true, was_running:false}.
	virtual nlohmann::json ShutdownDaemon() {
		return nlohmann::json{{"ok", true}, {"was_running", false}};
	}

	// ---- diff + merge analysis (read-only) ----------------------------------

	// Structural diff between two Blueprint assets.
	// depth: "structural" (variables/components/functions, fast) |
	//        "topology"   (node-level graph diff, slower)
	virtual nlohmann::json DiffAsset(
		std::string_view assetA,
		std::string_view assetB,
		std::string_view depth = "structural")
	{
		throw BlueprintReaderError("diff_asset not supported by this backend");
	}

	// Compute base→mine and base→theirs diffs, identify conflicts, and return
	// a structured merge context the AI can reason over and then apply using
	// the existing write tools (add_variable, add_node, wire_pins, apply_ops).
	virtual nlohmann::json PrepareMerge(
		std::string_view base,
		std::string_view mine,
		std::string_view theirs,
		std::string_view target = "")
	{
		throw BlueprintReaderError("prepare_merge not supported by this backend");
	}

	// EDIT-5: introspect a custom (or engine) UK2Node class — spawn a transient
	// instance, call AllocateDefaultPins, and report its pins, purity, title,
	// tooltip, and menu category. Editor-only (needs the live UClass registry).
	virtual nlohmann::json DescribeK2Node(std::string_view classPath)
	{
		(void)classPath;
		throw BlueprintReaderError("DescribeK2Node not supported by this backend");
	}

	// TEST-2 P0: walk the live editor's Slate widget tree and return one entry
	// per widget (path selector, type, tag, text, visibility, enabled, screen
	// rect), grouped per visible window. Read-only; the foundation for the
	// gated UI-interaction tools (ui_click / ui_type) and AutomationDriver
	// By::Path locators. `window`/`type` are substring filters; empty = all.
	virtual nlohmann::json UiListWidgets(
		int maxDepth, int maxWidgets,
		std::string_view window, std::string_view type)
	{
		(void)maxDepth; (void)maxWidgets; (void)window; (void)type;
		throw BlueprintReaderError("UiListWidgets not supported by this backend");
	}

	// TEST-2 P1b: click a widget located by its ui_list_widgets `path`, by
	// injecting a synthetic mouse down+up at the widget's geometry center (the
	// same FSlateApplication path real OS input takes). Gated editor-side behind
	// BP_READER_ALLOW_UI=1. `expectType`/`expectText` revalidate the target
	// before clicking (paths are response-local). Editor-only; an ACTION.
	virtual nlohmann::json UiClick(std::string_view widgetPath,
								   std::string_view expectType,
								   std::string_view expectText)
	{
		(void)widgetPath; (void)expectType; (void)expectText;
		throw BlueprintReaderError("UiClick not supported by this backend");
	}

	// TEST-2 P1b: type `text` into a widget located by its ui_list_widgets
	// `path` (focus it, then inject one character event per char via OnKeyChar).
	// Gated editor-side behind BP_READER_ALLOW_UI=1. `expectType` revalidates
	// the target. Editor-only; an ACTION.
	virtual nlohmann::json UiType(std::string_view widgetPath,
								  std::string_view text,
								  std::string_view expectType)
	{
		(void)widgetPath; (void)text; (void)expectType;
		throw BlueprintReaderError("UiType not supported by this backend");
	}

	// TEST-2 P1b: focus (foreground) an editor dock tab by a substring of its
	// `tabLabel`, the geometry-independent way to bring a panel forward.
	// Gated editor-side behind BP_READER_ALLOW_UI=1. Editor-only; an ACTION.
	virtual nlohmann::json UiFocusTab(std::string_view tabLabel)
	{
		(void)tabLabel;
		throw BlueprintReaderError("UiFocusTab not supported by this backend");
	}

	// UX-P4a: a liveness/health probe a live editor can answer ON ITS WORKER
	// THREAD even when the game thread is halted — so a paused editor is a
	// distinct, fast answer instead of a generic op timeout. `state` is one of
	// "healthy" | "paused" | "unreachable"; `game_thread_age_ms` is how long
	// since the editor's game thread last advanced its heartbeat (-1 = unknown,
	// e.g. the commandlet/process-level backends that have no game-thread probe).
	struct HealthResult {
		bool reachable = false;             // could we talk to the backend at all
		bool gameThreadResponsive = false;  // worker answered AND the heartbeat is fresh
		long long gameThreadAgeMs = -1;     // now - last game-thread heartbeat; -1 = unknown
		std::string state;                  // "healthy" | "paused" | "unreachable"
		std::string note;                   // optional human-readable detail
	};
	virtual HealthResult HealthCheck()
	{
		throw BlueprintReaderError("HealthCheck not supported by this backend");
	}
};

}    // namespace bpr::backends
