#include "tools/ApplyOps.h"
#include "tools/TypeShorthand.h"

#include <fmt/core.h>

#include <cstdio>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace bpr::tools {

namespace apply_ops_detail {

// ----- Slot resolution ---------------------------------------------------
// id -> minted GUID. Populated as add_node ops complete; consumed by
// wire_pins / set_node_position / delete_node refs in later ops.
using SlotMap = std::map<std::string, std::string, std::less<>>;

// Inspect a node-id field and return the slot id it references, if any.
// Returns std::nullopt for:
//   - non-string, non-object values
//   - strings that aren't `$<id>` prefixed (raw GUIDs)
//   - malformed objects without a string "ref"
//
// Pure inspection — never throws. Used both by ResolveNodeRef (which
// does the actual slot lookup + throws on miss) and by the failed-slot
// pre-check in RunOps (which needs to peek at refs without bailing).
std::optional<std::string> ExtractSlotRefId(const nlohmann::json& field) {
	if (field.is_object()) {
		auto it = field.find("ref");
		if (it != field.end() && it->is_string()) {
			return it->get<std::string>();
		}
		return std::nullopt;
	}
	if (field.is_string()) {
		const auto& s = field.get_ref<const std::string&>();
		if (!s.empty() && s.front() == '$') {
			return s.substr(1);
		}
	}
	return std::nullopt;
}

// Resolve a node-id field. Supported forms (in priority order):
//   1. {"ref": "<id>"}       — explicit slot ref
//   2. "$<id>"               — sigil-prefixed slot ref
//   3. raw GUID string       — pass through unchanged
// Throws if a slot ref doesn't resolve, or if the field shape is wrong.
std::string ResolveNodeRef(const nlohmann::json& field, const SlotMap& slots,
						   std::string_view key) {
	if (auto slotId = ExtractSlotRefId(field); slotId.has_value()) {
		auto sit = slots.find(*slotId);
		if (sit == slots.end()) {
			// Distinct messages for the two forms — keeps backward-compat
			// with any callers grepping the error text.
			if (field.is_string()) {
				throw std::invalid_argument(fmt::format(
					R"(field "{}" references slot "${}" which has not been bound)",
					key, *slotId));
			}
			throw std::invalid_argument(fmt::format(
				R"(field "{}" references slot "{}" which has not been bound — )"
				"did an earlier op fail or did you typo the id?", key, *slotId));
		}
		return sit->second;
	}
	if (field.is_object()) {
		// Object form without a usable "ref" string.
		throw std::invalid_argument(fmt::format(
			"field '{}' is an object but lacks a string \"ref\"", key));
	}
	if (field.is_string()) {
		// Raw GUID passthrough — not a slot ref, not malformed.
		return field.get_ref<const std::string&>();
	}
	throw std::invalid_argument(fmt::format(
		"field '{}' must be a string GUID, a $slot reference, or {{\"ref\":\"<id>\"}}",
		key));
}

std::string GetString(const nlohmann::json& obj, std::string_view key) {
	auto it = obj.find(key);
	if (it == obj.end() || !it->is_string()) {
		throw std::invalid_argument(fmt::format(
			R"(missing or non-string field "{}")", key));
	}
	return it->get<std::string>();
}

std::string OptStr(const nlohmann::json& obj, std::string_view key,
				   std::string fallback = {}) {
	auto it = obj.find(key);
	if (it == obj.end() || it->is_null())
	{
		return fallback;
	}
	if (!it->is_string()) {
		throw std::invalid_argument(fmt::format(
			R"(field "{}" must be a string)", key));
	}
	return it->get<std::string>();
}

int GetInt(const nlohmann::json& obj, std::string_view key) {
	auto it = obj.find(key);
	if (it == obj.end() || !it->is_number_integer()) {
		throw std::invalid_argument(fmt::format(
			R"(missing or non-integer field "{}")", key));
	}
	return it->get<int>();
}

// ----- Op dispatchers ----------------------------------------------------
// Each dispatcher returns the per-op result JSON. They MUST NOT swallow
// exceptions — the outer driver catches and decides whether to bail or
// continue based on `atomic`.

nlohmann::json OpAddVariable(backends::IBlueprintReader& reader,
							 const nlohmann::json& op, SlotMap&) {
	std::string asset = GetString(op, "asset_path");
	std::string name  = GetString(op, "name");
	auto typeIt = op.find("type");
	if (typeIt == op.end()) {
		throw std::invalid_argument(R"(add_variable op requires "type")");
	}
	BPPinType type = ParseTypeArg(*typeIt);
	std::string defaultValue = OptStr(op, "default_value", "");
	std::string category     = OptStr(op, "category",      "");
	bool replicated = op.value("replicated", false);
	bool editable   = op.value("editable",   false);

	// Idempotency.
	try {
		for (const auto& v : reader.ListVariables(asset)) {
			if (v.Name == name) {
				return {{"ok", true}, {"already_existed", true}};
			}
		}
	} catch (...) { /* fall through */ }
	reader.AddVariable(asset, name, type, defaultValue, category, replicated, editable);
	return {{"ok", true}, {"already_existed", false}};
}

nlohmann::json OpAddFunction(backends::IBlueprintReader& reader,
							 const nlohmann::json& op, SlotMap& slots) {
	std::string asset = GetString(op, "asset_path");
	std::string name  = GetString(op, "name");
	try {
		auto meta = reader.ReadBlueprint(asset);
		for (const auto& fn : meta.Functions) {
			if (fn.Name == name) {
				return {{"ok", true}, {"function_name", name}, {"already_existed", true}};
			}
		}
	} catch (...) { /* fall through */ }
	auto out = reader.AddFunction(asset, name);
	// If the op carried a slot id, bind the entry node's GUID to it so
	// later ops in the same batch can wire FunctionEntry's `then` exec
	// into their first statement (compile_function relies on this).
	if (auto idIt = op.find("id"); idIt != op.end() && idIt->is_string() &&
		!out.entryNodeId.empty()) {
		slots[idIt->get<std::string>()] = out.entryNodeId;
	}
	nlohmann::json result = {
		{"ok", true},
		{"function_name", out.functionName},
		{"already_existed", false},
	};
	if (!out.entryNodeId.empty()) {
		result["entry_node_id"] = out.entryNodeId;
	}
	return result;
}

nlohmann::json OpAddFunctionParam(backends::IBlueprintReader& reader,
								  const nlohmann::json& op, SlotMap&,
								  bool isOutput) {
	std::string asset = GetString(op, "asset_path");
	std::string fn    = GetString(op, "function_name");
	std::string param = GetString(op, "param_name");
	auto typeIt = op.find("type");
	if (typeIt == op.end()) {
		throw std::invalid_argument(
			R"(add_function_input/output op requires "type")");
	}
	BPPinType type = ParseTypeArg(*typeIt);
	if (isOutput)
	{
		reader.AddFunctionOutput(asset, fn, param, type);
	}
	else          reader.AddFunctionInput (asset, fn, param, type);
	return {{"ok", true}};
}

nlohmann::json OpAddNode(backends::IBlueprintReader& reader,
						 const nlohmann::json& op, SlotMap& slots) {
	std::string asset = GetString(op, "asset_path");
	std::string graph = GetString(op, "graph_name");
	std::string kind  = GetString(op, "kind");
	int x = GetInt(op, "x");
	int y = GetInt(op, "y");
	std::map<std::string, std::string, std::less<>> extras;
	auto put = [&](const char* mcpKey, const char* flagKey) {
		std::string v = OptStr(op, mcpKey, "");
		if (!v.empty())
		{
			extras.emplace(flagKey, std::move(v));
		}
	};
	put("variable",       "Variable");
	put("function",       "Function");
	put("function_owner", "FunctionOwner");
	put("event_name",     "EventName");
	put("target_class",   "TargetClass");
	put("struct_type",    "StructType");
	std::string newId = reader.AddNode(asset, graph, kind, x, y, extras);

	// Bind to slot if `id` is set.
	if (auto idIt = op.find("id"); idIt != op.end() && idIt->is_string()) {
		slots[idIt->get<std::string>()] = newId;
	}

	// Pin enrichment — same as add_node tool.
	nlohmann::json pinsJson = nlohmann::json::array();
	try {
		auto g = reader.GetGraph(asset, graph);
		for (const auto& n : g.Nodes) {
			if (n.Id == newId) {
				for (const auto& p : n.Pins) {
					nlohmann::json t = {{"category", p.Type.Category}};
					if (p.Type.SubCategory)
					{
						t["sub_category"]        = *p.Type.SubCategory;
					}
					if (p.Type.SubCategoryObject)
					{
						t["sub_category_object"] = *p.Type.SubCategoryObject;
					}
					if (p.Type.IsArray)
					{
						t["is_array"] = true;
					}
					if (p.Type.IsSet)
					{
						t["is_set"]   = true;
					}
					if (p.Type.IsMap)
					{
						t["is_map"]   = true;
					}
					pinsJson.push_back({
						{"name",      p.Name},
						{"guid",      p.Id},
						{"direction", p.Direction},
						{"type",      std::move(t)},
					});
				}
				break;
			}
		}
	} catch (...) { /* best-effort */ }
	return {{"ok", true}, {"node_id", newId}, {"pins", std::move(pinsJson)}};
}

nlohmann::json OpWirePins(backends::IBlueprintReader& reader,
						  const nlohmann::json& op, SlotMap& slots) {
	std::string asset = GetString(op, "asset_path");
	std::string graph = GetString(op, "graph_name");
	std::string fromNode, toNode;
	{
		auto fnIt = op.find("from_node");
		auto tnIt = op.find("to_node");
		if (fnIt == op.end() || tnIt == op.end()) {
			throw std::invalid_argument(
				R"(wire_pins op requires "from_node" and "to_node")");
		}
		fromNode = ResolveNodeRef(*fnIt, slots, "from_node");
		toNode   = ResolveNodeRef(*tnIt, slots, "to_node");
	}
	std::string fromPin = GetString(op, "from_pin");
	std::string toPin   = GetString(op, "to_pin");
	reader.WirePins(asset, graph, fromNode, fromPin, toNode, toPin);
	return {{"ok", true}};
}

nlohmann::json OpSetNodePosition(backends::IBlueprintReader& reader,
								 const nlohmann::json& op, SlotMap& slots) {
	std::string asset = GetString(op, "asset_path");
	std::string graph = GetString(op, "graph_name");
	auto nIt = op.find("node_id");
	if (nIt == op.end()) {
		throw std::invalid_argument(
			R"(set_node_position op requires "node_id")");
	}
	std::string node = ResolveNodeRef(*nIt, slots, "node_id");
	int x = GetInt(op, "x");
	int y = GetInt(op, "y");
	reader.SetNodePosition(asset, graph, node, x, y);
	return {{"ok", true}};
}

nlohmann::json OpDeleteNode(backends::IBlueprintReader& reader,
							const nlohmann::json& op, SlotMap& slots) {
	std::string asset = GetString(op, "asset_path");
	std::string graph = GetString(op, "graph_name");
	auto nIt = op.find("node_id");
	if (nIt == op.end()) {
		throw std::invalid_argument(R"(delete_node op requires "node_id")");
	}
	std::string node = ResolveNodeRef(*nIt, slots, "node_id");
	reader.DeleteNode(asset, graph, node);
	return {{"ok", true}};
}

nlohmann::json OpDeleteVariable(backends::IBlueprintReader& reader,
								const nlohmann::json& op, SlotMap&) {
	reader.DeleteVariable(GetString(op, "asset_path"), GetString(op, "name"));
	return {{"ok", true}};
}

nlohmann::json OpRenameVariable(backends::IBlueprintReader& reader,
								const nlohmann::json& op, SlotMap&) {
	reader.RenameVariable(GetString(op, "asset_path"),
						  GetString(op, "old_name"),
						  GetString(op, "new_name"));
	return {{"ok", true}};
}

nlohmann::json OpDeleteFunction(backends::IBlueprintReader& reader,
								const nlohmann::json& op, SlotMap&) {
	reader.DeleteFunction(GetString(op, "asset_path"), GetString(op, "name"));
	return {{"ok", true}};
}

nlohmann::json OpSetVariableDefault(backends::IBlueprintReader& reader,
									const nlohmann::json& op, SlotMap&) {
	reader.SetVariableDefault(GetString(op, "asset_path"),
							  GetString(op, "name"),
							  OptStr(op, "default_value", ""));
	return {{"ok", true}};
}

nlohmann::json OpCreateBlueprint(backends::IBlueprintReader& reader,
								 const nlohmann::json& op, SlotMap&) {
	std::string asset  = GetString(op, "asset_path");
	std::string parent = GetString(op, "parent_class");
	auto r = reader.CreateBlueprint(asset, parent);
	return {
		{"ok", true},
		{"asset_path", asset},
		{"already_existed", r.alreadyExisted},
		{"parent_class", r.parentClass.empty() ? parent : r.parentClass},
	};
}

nlohmann::json OpSetPinDefault(backends::IBlueprintReader& reader,
							   const nlohmann::json& op, SlotMap& slots) {
	std::string asset = GetString(op, "asset_path");
	std::string graph = GetString(op, "graph_name");
	auto nIt = op.find("node_id");
	if (nIt == op.end()) {
		throw std::invalid_argument(R"(set_pin_default op requires "node_id")");
	}
	std::string node  = ResolveNodeRef(*nIt, slots, "node_id");
	std::string pin   = GetString(op, "pin_name");
	std::string value = OptStr(op, "value", "");
	reader.SetPinDefault(asset, graph, node, pin, value);
	return {{"ok", true}};
}

nlohmann::json OpRetypeVariable(backends::IBlueprintReader& reader,
								const nlohmann::json& op, SlotMap&) {
	std::string asset = GetString(op, "asset_path");
	std::string name  = GetString(op, "name");
	auto typeIt = op.find("type");
	if (typeIt == op.end()) {
		throw std::invalid_argument(R"(retype_variable op requires "type")");
	}
	BPPinType type = ParseTypeArg(*typeIt);
	reader.RetypeVariable(asset, name, type);
	return {{"ok", true}};
}

nlohmann::json OpSetVariableCategory(backends::IBlueprintReader& reader,
									 const nlohmann::json& op, SlotMap&) {
	reader.SetVariableCategory(
		GetString(op, "asset_path"),
		GetString(op, "name"),
		OptStr(op, "category", ""));
	return {{"ok", true}};
}

nlohmann::json OpDuplicateBlueprint(backends::IBlueprintReader& reader,
									const nlohmann::json& op, SlotMap&) {
	std::string source = GetString(op, "asset_path");
	std::string dest   = GetString(op, "dest_asset_path");
	auto r = reader.DuplicateBlueprint(source, dest);
	return {
		{"ok", true},
		{"asset_path", dest},
		{"source_asset_path", source},
		{"already_existed", r.alreadyExisted},
	};
}

// Dispatch one op. Caller chooses whether to catch.
nlohmann::json DispatchOp(backends::IBlueprintReader& reader,
						  const nlohmann::json& op, SlotMap& slots) {
	auto it = op.find("op");
	if (it == op.end() || !it->is_string()) {
		throw std::invalid_argument(R"(every op requires a string "op" field)");
	}
	const auto& kind = it->get_ref<const std::string&>();
	if (kind == "create_blueprint")
	{
		return OpCreateBlueprint(reader, op, slots);
	}
	if (kind == "set_pin_default")
	{
		return OpSetPinDefault(reader, op, slots);
	}
	if (kind == "retype_variable")
	{
		return OpRetypeVariable(reader, op, slots);
	}
	if (kind == "set_variable_category")
	{
		return OpSetVariableCategory(reader, op, slots);
	}
	if (kind == "duplicate_blueprint")
	{
		return OpDuplicateBlueprint(reader, op, slots);
	}
	if (kind == "add_variable")
	{
		return OpAddVariable    (reader, op, slots);
	}
	if (kind == "delete_variable")
	{
		return OpDeleteVariable (reader, op, slots);
	}
	if (kind == "rename_variable")
	{
		return OpRenameVariable (reader, op, slots);
	}
	if (kind == "set_variable_default")
	{
		return OpSetVariableDefault(reader, op, slots);
	}
	if (kind == "add_function")
	{
		return OpAddFunction    (reader, op, slots);
	}
	if (kind == "add_function_input")
	{
		return OpAddFunctionParam(reader, op, slots, /*isOutput=*/false);
	}
	if (kind == "add_function_output")
	{
		return OpAddFunctionParam(reader, op, slots, /*isOutput=*/true);
	}
	if (kind == "delete_function")
	{
		return OpDeleteFunction (reader, op, slots);
	}
	if (kind == "add_node")
	{
		return OpAddNode        (reader, op, slots);
	}
	if (kind == "wire_pins")
	{
		return OpWirePins       (reader, op, slots);
	}
	if (kind == "set_node_position")
	{
		return OpSetNodePosition(reader, op, slots);
	}
	if (kind == "delete_node")
	{
		return OpDeleteNode     (reader, op, slots);
	}
	throw std::invalid_argument(fmt::format(
		"unknown op '{}'. Supported: create_blueprint, duplicate_blueprint, "
		"add_variable, delete_variable, rename_variable, retype_variable, "
		"set_variable_default, set_variable_category, add_function, "
		"add_function_input, add_function_output, delete_function, "
		"add_node, wire_pins, set_node_position, delete_node, "
		"set_pin_default", kind));
}

// ----- Validate-only path (B2: preview_ops) ------------------------------
// One per dispatch op. Mirrors DispatchOp's table. Each fn validates
// required fields + resolves slots; reports the asset_path it would touch
// (so the caller can list affected BPs). No writes; reads are allowed.
//
// For ops that mint a slot (add_node / create_blueprint), we synthesize a
// placeholder GUID and bind it so subsequent ops in the same preview can
// resolve `$<id>` references successfully.
std::string MintPlaceholderGuid(int& counter) {
	char buf[40];
	std::snprintf(buf, sizeof(buf),
		"00000000-0000-0000-0000-%012d", counter++);
	return buf;
}

void ValidateOp(backends::IBlueprintReader& reader, const nlohmann::json& op,
				SlotMap& slots, int& placeholderCounter,
				std::set<std::string>& wouldCompile) {
	auto it = op.find("op");
	if (it == op.end() || !it->is_string()) {
		throw std::invalid_argument(R"(every op requires a string "op" field)");
	}
	const auto& kind = it->get_ref<const std::string&>();

	auto noteAsset = [&](std::string_view a) { wouldCompile.insert(std::string(a)); };

	if (kind == "create_blueprint") {
		std::string asset  = GetString(op, "asset_path");
		(void)GetString(op, "parent_class");
		if (auto idIt = op.find("id"); idIt != op.end() && idIt->is_string()) {
			slots[idIt->get<std::string>()] = MintPlaceholderGuid(placeholderCounter);
		}
		noteAsset(asset);
		return;
	}
	if (kind == "add_variable") {
		std::string asset = GetString(op, "asset_path");
		(void)GetString(op, "name");
		if (op.find("type") == op.end()) {
			throw std::invalid_argument(R"(add_variable op requires "type")");
		}
		// Type shorthand validation runs against the helper without writes.
		ParseTypeArg(op["type"]);
		noteAsset(asset);
		return;
	}
	if (kind == "delete_variable" || kind == "rename_variable" ||
		kind == "set_variable_default") {
		std::string asset = GetString(op, "asset_path");
		// Confirm variable exists (read-only check).
		if (kind == "delete_variable" || kind == "set_variable_default") {
			std::string name = GetString(op, "name");
			try {
				bool found = false;
				for (const auto& v : reader.ListVariables(asset)) {
					if (v.Name == name) { found = true; break; }
				}
				if (!found) {
					throw std::invalid_argument(fmt::format(
						R"(variable "{}" not found on "{}")", name, asset));
				}
			} catch (const bpr::backends::BlueprintReaderError&) {
				// Asset itself missing — bubble up.
				throw;
			}
		} else {
			(void)GetString(op, "old_name");
			(void)GetString(op, "new_name");
		}
		noteAsset(asset);
		return;
	}
	if (kind == "add_function" || kind == "delete_function") {
		std::string asset = GetString(op, "asset_path");
		(void)GetString(op, "name");
		noteAsset(asset);
		return;
	}
	if (kind == "add_function_input" || kind == "add_function_output") {
		std::string asset = GetString(op, "asset_path");
		(void)GetString(op, "function_name");
		(void)GetString(op, "param_name");
		if (op.find("type") == op.end()) {
			throw std::invalid_argument(
				R"(add_function_input/output op requires "type")");
		}
		ParseTypeArg(op["type"]);
		noteAsset(asset);
		return;
	}
	if (kind == "add_node") {
		std::string asset = GetString(op, "asset_path");
		(void)GetString(op, "graph_name");
		(void)GetString(op, "kind");
		(void)GetInt(op, "x");
		(void)GetInt(op, "y");
		if (auto idIt = op.find("id"); idIt != op.end() && idIt->is_string()) {
			slots[idIt->get<std::string>()] = MintPlaceholderGuid(placeholderCounter);
		}
		noteAsset(asset);
		return;
	}
	if (kind == "wire_pins") {
		std::string asset = GetString(op, "asset_path");
		(void)GetString(op, "graph_name");
		auto fnIt = op.find("from_node");
		auto tnIt = op.find("to_node");
		if (fnIt == op.end() || tnIt == op.end()) {
			throw std::invalid_argument(
				R"(wire_pins op requires "from_node" and "to_node")");
		}
		// ResolveNodeRef throws on unbound slots — exactly the validation
		// we want.
		(void)ResolveNodeRef(*fnIt, slots, "from_node");
		(void)ResolveNodeRef(*tnIt, slots, "to_node");
		(void)GetString(op, "from_pin");
		(void)GetString(op, "to_pin");
		noteAsset(asset);
		return;
	}
	if (kind == "set_node_position") {
		std::string asset = GetString(op, "asset_path");
		(void)GetString(op, "graph_name");
		auto nIt = op.find("node_id");
		if (nIt == op.end())
		{
			throw std::invalid_argument(R"(set_node_position requires "node_id")");
		}
		(void)ResolveNodeRef(*nIt, slots, "node_id");
		(void)GetInt(op, "x"); (void)GetInt(op, "y");
		noteAsset(asset);
		return;
	}
	if (kind == "delete_node") {
		std::string asset = GetString(op, "asset_path");
		(void)GetString(op, "graph_name");
		auto nIt = op.find("node_id");
		if (nIt == op.end())
		{
			throw std::invalid_argument(R"(delete_node requires "node_id")");
		}
		(void)ResolveNodeRef(*nIt, slots, "node_id");
		noteAsset(asset);
		return;
	}
	if (kind == "set_pin_default") {
		std::string asset = GetString(op, "asset_path");
		(void)GetString(op, "graph_name");
		auto nIt = op.find("node_id");
		if (nIt == op.end())
		{
			throw std::invalid_argument(R"(set_pin_default requires "node_id")");
		}
		(void)ResolveNodeRef(*nIt, slots, "node_id");
		(void)GetString(op, "pin_name");
		noteAsset(asset);
		return;
	}
	if (kind == "retype_variable") {
		std::string asset = GetString(op, "asset_path");
		(void)GetString(op, "name");
		if (op.find("type") == op.end()) {
			throw std::invalid_argument(R"(retype_variable op requires "type")");
		}
		ParseTypeArg(op["type"]);
		noteAsset(asset);
		return;
	}
	if (kind == "set_variable_category") {
		std::string asset = GetString(op, "asset_path");
		(void)GetString(op, "name");
		// category is optional (empty clears it)
		noteAsset(asset);
		return;
	}
	if (kind == "duplicate_blueprint") {
		std::string source = GetString(op, "asset_path");
		std::string dest   = GetString(op, "dest_asset_path");
		if (dest.size() < 6 || dest.compare(0, 6, "/Game/") != 0) {
			throw std::invalid_argument(
				R"(duplicate_blueprint: "dest_asset_path" must start with "/Game/")");
		}
		noteAsset(source);
		noteAsset(dest);
		return;
	}
	throw std::invalid_argument(fmt::format(
		"unknown op '{}' (preview_ops uses the same op set as apply_ops)", kind));
}

}    // namespace apply_ops_detail
using namespace apply_ops_detail;

nlohmann::json ValidateOps(backends::IBlueprintReader& reader,
						   const nlohmann::json& ops) {
	if (!ops.is_array()) {
		throw std::invalid_argument(R"(ValidateOps requires "ops" to be an array)");
	}
	SlotMap slots;
	std::set<std::string> wouldCompile;
	int placeholderCounter = 1;
	nlohmann::json results = nlohmann::json::array();
	int validated = 0, failed = 0;
	for (std::size_t i = 0; i < ops.size(); ++i) {
		const auto& op = ops[i];
		try {
			ValidateOp(reader, op, slots, placeholderCounter, wouldCompile);
			++validated;
			results.push_back({
				{"ok", true},
				{"op", op.contains("op") && op["op"].is_string()
						   ? op["op"].get<std::string>() : ""},
			});
		} catch (const std::exception& e) {
			++failed;
			results.push_back({
				{"ok", false},
				{"op_index", i},
				{"op", op.contains("op") && op["op"].is_string()
						   ? op["op"].get<std::string>() : ""},
				{"error", e.what()},
			});
		}
	}
	return nlohmann::json{
		{"ok", failed == 0},
		{"validated", validated},
		{"failed",    failed},
		{"slots",     slots},
		{"results",   std::move(results)},
		{"would_compile", std::vector<std::string>(wouldCompile.begin(), wouldCompile.end())},
	};
}

nlohmann::json RunOps(backends::IBlueprintReader& reader,
					  const nlohmann::json& ops, bool atomic,
					  std::string_view onFailure) {
	if (!ops.is_array()) {
		throw std::invalid_argument(R"(RunOps requires "ops" to be an array)");
	}
	// Resolve on_failure once. Unknown values fall back to "compile" with
	// a clear error so misspellings don't silently change behavior.
	bool skipOnFailure = false;
	if (onFailure == "compile" || onFailure.empty()) {
		skipOnFailure = false;
	} else if (onFailure == "skip") {
		skipOnFailure = true;
	} else if (onFailure == "rollback") {
		// Not implemented in v1 — would need plugin-side FScopedTransaction.
		// Fall through to compile semantics.
		skipOnFailure = false;
	} else {
		throw std::invalid_argument(fmt::format(
			R"(unknown on_failure value "{}" — supported: "compile" (default) | "skip")",
			onFailure));
	}

	SlotMap slots;
	nlohmann::json results = nlohmann::json::array();
	int succeeded = 0, failed = 0;

	// A1: open a batch around the dispatch loop. The plugin defers
	// CompileBlueprint+SavePackage until EndBatch, collapsing N×compile
	// (typically 100ms-2s each) into a single recompile per affected BP.
	// RAII guard guarantees EndBatch runs even on early-exit paths
	// (atomic=true throw, bug in dispatcher, etc.) — best-effort failure
	// semantics by default: the daemon still saves whatever ops landed.
	// The guard's `skipOnExit` flag flips when we've already handled the
	// explicit EndBatch path (success or atomic-failure).
	struct BatchGuard {
		backends::IBlueprintReader& r;
		bool active = true;
		bool skipOnEarlyExit = false;
		BatchGuard(backends::IBlueprintReader& r_) : r(r_) { r.BeginBatch(); }
		~BatchGuard() {
			if (active) {
				try { (void)r.EndBatch(skipOnEarlyExit); } catch (...) {}
			}
		}
		void release() { active = false; }
	};
	BatchGuard guard(reader);

	// For diagnostic attribution: track which op_index minted each
	// node_guid. Snapshot the slot map before each op, diff after.
	// When EndBatch surfaces compile diagnostics tagged with a node_guid,
	// we look up the op that produced that node and tag the diagnostic
	// with `op_index` so the caller can attribute warnings to a specific
	// batch operation.
	std::map<std::string, std::size_t> guidToOpIndex;
	// Issue #8: when an op that mints a slot (`id` field) fails, every
	// downstream op referencing `$<slotId>` previously got a generic
	// "slot ... has not been bound" error. Track failed-slot reasons
	// here so we can surface a much more useful message linking the
	// downstream failure back to its upstream cause.
	std::map<std::string, std::string, std::less<>> failedSlotReasons;

	// Walk an op JSON and report the first slot reference that points
	// at a slot in `failedSlotReasons`. Returns empty string if every
	// ref resolves cleanly (already bound, raw GUID, or unbound but
	// not from a failed upstream). Uses the shared ExtractSlotRefId
	// helper for ref detection so the parsing rules stay in sync with
	// ResolveNodeRef.
	auto findFailedSlotRef = [&](const nlohmann::json& op) -> std::string {
		static constexpr const char* kRefFields[] = {
			"from_node", "to_node", "node_id",
		};
		for (const char* key : kRefFields) {
			auto it = op.find(key);
			if (it == op.end())
			{
				continue;
			}
			auto slotId = ExtractSlotRefId(*it);
			if (!slotId.has_value())
			{
				continue;     // raw GUID or malformed
			}
			if (slots.find(*slotId) != slots.end())
			{
				continue;  // already bound
			}
			auto fIt = failedSlotReasons.find(*slotId);
			if (fIt != failedSlotReasons.end()) {
				return fmt::format(
					"field \"{}\" references slot \"${}\", which was supposed to be "
					"bound by an earlier op that failed: {}",
					key, *slotId, fIt->second);
			}
		}
		return {};
	};

	auto runDispatch = [&]() {
		for (std::size_t i = 0; i < ops.size(); ++i) {
			const auto& op = ops[i];
			std::size_t prevSlotCount = slots.size();
			// Pre-check: if this op references a slot bound to a
			// previously failed op, short-circuit with a rich error
			// (issue #8). Skips dispatching entirely so we don't bother
			// the backend with a doomed call.
			std::string cascadedReason = findFailedSlotRef(op);
			if (!cascadedReason.empty()) {
				++failed;
				if (atomic) {
					throw bpr::backends::BlueprintReaderError(fmt::format(
						"apply_ops failed at op[{}] (op=\"{}\"): {}",
						i,
						op.contains("op") && op["op"].is_string()
							? op["op"].get<std::string>() : "<missing>",
						cascadedReason));
				}
				results.push_back({
					{"ok", false},
					{"op_index", i},
					{"error", cascadedReason},
					{"cause", "upstream-slot-failed"},
				});
				// If this op itself names a slot, propagate the failure
				// forward so the next dependent op gets the same rich
				// diagnostic chain.
				if (auto idIt = op.find("id"); idIt != op.end() && idIt->is_string()) {
					failedSlotReasons[idIt->get<std::string>()] = cascadedReason;
				}
				continue;
			}
			try {
				results.push_back(DispatchOp(reader, op, slots));
				++succeeded;
				// Diff slot map: any new entries were minted by this op.
				if (slots.size() > prevSlotCount) {
					for (const auto& [slotId, guid] : slots) {
						if (!guid.empty() && guidToOpIndex.find(guid) == guidToOpIndex.end()) {
							guidToOpIndex[guid] = i;
						}
					}
				}
			} catch (const std::exception& e) {
				++failed;
				// If this op named a slot, record why it failed so the
				// pre-check above can surface this reason to dependents.
				if (auto idIt = op.find("id"); idIt != op.end() && idIt->is_string()) {
					failedSlotReasons[idIt->get<std::string>()] = fmt::format(
						"op[{}] failed: {}", i, e.what());
				}
				if (atomic) {
					throw bpr::backends::BlueprintReaderError(fmt::format(
						"apply_ops failed at op[{}] (op=\"{}\"): {}",
						i,
						op.contains("op") && op["op"].is_string()
							? op["op"].get<std::string>() : "<missing>",
						e.what()));
				}
				results.push_back({
					{"ok", false},
					{"op_index", i},
					{"error", e.what()},
				});
			}
		}
	};
	nlohmann::json flushAck;
	try {
		runDispatch();
	} catch (...) {
		// Mid-batch failure path. on_failure="compile" (default) flushes
		// partial state to disk; "skip" discards it. Either way, EndBatch
		// runs so the plugin's BatchPending state is cleared.
		try { (void)reader.EndBatch(skipOnFailure); } catch (...) {}
		guard.release();
		throw;
	}
	// Normal path — explicit EndBatch so we surface any flush errors and
	// capture the diagnostics ack (C1). on_failure doesn't apply here
	// because nothing failed.
	flushAck = reader.EndBatch(/*skipCompile=*/false);
	guard.release();

	nlohmann::json out = {
		{"ok", failed == 0},
		{"succeeded", succeeded},
		{"failed",    failed},
		{"slots",     slots},
		{"results",   std::move(results)},
	};
	// C1: lift compile diagnostics from the EndBatch ack to the top level
	// so callers see them without digging. EndBatch returns {} on backends
	// that don't have a compile step (Mock, etc.) — gracefully skipped.
	//
	// Op-attribution: for each diagnostic that carries a node_guid, look
	// up which op_index in this batch minted that node and tag the
	// diagnostic with `op_index`. Lets callers correlate "warning X" to
	// "the wire_pins op at index 7". Best-effort — if a diagnostic
	// points at a node not minted by this batch (existing nodes, e.g.
	// referenced by var name), no op_index is attached.
	if (flushAck.is_object()) {
		if (flushAck.contains("diagnostics") && flushAck["diagnostics"].is_array()) {
			nlohmann::json tagged = nlohmann::json::array();
			for (auto& d : flushAck["diagnostics"]) {
				nlohmann::json copy = d;
				if (copy.is_object() && copy.contains("node_guid") &&
					copy["node_guid"].is_string()) {
					auto it = guidToOpIndex.find(copy["node_guid"].get<std::string>());
					if (it != guidToOpIndex.end()) {
						copy["op_index"] = it->second;
					}
				}
				tagged.push_back(std::move(copy));
			}
			out["diagnostics"] = std::move(tagged);
		}
		if (flushAck.contains("error_count"))
		{
			out["compile_errors"]   = flushAck["error_count"];
		}
		if (flushAck.contains("warning_count"))
		{
			out["compile_warnings"] = flushAck["warning_count"];
		}
		if (flushAck.contains("recompiled"))
		{
			out["recompiled"]       = flushAck["recompiled"];
		}
	}
	return out;
}

void RegisterApplyOps(ToolRegistry& registry, backends::IBlueprintReader& reader) {
	ToolDescriptor d;
	d.name = "apply_ops";
	d.description =
		"[blueprint] Execute a batch of write operations sequentially in a single tool "
		"call. Each op is `{op:\"add_variable\"|...|\"wire_pins\", ...args}`. "
		"Reduces N round-trips and N agent reasoning steps to one.\n\n"
		"Named slots: an `add_node` op may carry `id: \"<name>\"`. Subsequent "
		"ops can reference that node's GUID with `\"$<name>\"` or "
		"`{\"ref\":\"<name>\"}` in any node-id field. Eliminates the need to "
		"thread GUIDs through the agent's reasoning.\n\n"
		"`atomic: true` (default) bails on first error. `atomic: false` "
		"continues; failed ops appear as `{ok: false, error: \"...\"}` in "
		"the per-op results array, succeeded ops as their normal payload.\n\n"
		"Limitation: each underlying op still saves + recompiles the BP "
		"individually. True single-recompile batching needs plugin work — "
		"tracked separately.";
	d.input_schema = {
		{"type", "object"},
		{"properties", {
			{"ops", {
				{"type", "array"},
				{"items", {
					{"type", "object"},
					{"properties", {
						{"op", {{"type","string"}}},
						{"id", {{"type","string"},
								{"description","Optional slot name. For node-spawning ops, binds the new GUID under this name for later refs."}}},
					}},
					{"required", nlohmann::json::array({"op"})},
					{"additionalProperties", true},
				}},
			}},
			{"atomic", {{"type","boolean"},
						{"description","Bail on first failure (default true)."}}},
			{"on_failure", {{"type","string"},
							{"enum", nlohmann::json::array({"compile","skip"})},
							{"description",
							 "What EndBatch does after a mid-batch failure. "
							 "\"compile\" (default): best-effort — compile + save "
							 "what landed before the failure. Matches today's "
							 "per-op behavior. \"skip\": don't compile, don't "
							 "save — nothing reaches disk. The in-memory daemon "
							 "state stays dirty until restart, so subsequent "
							 "reads in the same session can see the partial "
							 "mutations; this is a documented limitation of "
							 "strict-atomic mode."}}},
		}},
		{"required", nlohmann::json::array({"ops"})},
	};
	registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
		auto opsIt = args.find("ops");
		if (opsIt == args.end() || !opsIt->is_array()) {
			throw std::invalid_argument(
				R"(apply_ops requires "ops" to be an array)");
		}
		bool atomic = args.value("atomic", true);
		std::string onFailure = args.value("on_failure", std::string{"compile"});
		return RunOps(reader, *opsIt, atomic, onFailure);
	});

	// ----- preview_ops (B2) ------------------------------------------------
	{
		ToolDescriptor pd;
		pd.name = "preview_ops";
		pd.description =
			"Validate an apply_ops batch without mutating anything. Walks "
			"the op array, parses each op's required fields, resolves "
			"named-slot refs against placeholder GUIDs, and uses read-only "
			"backend calls to confirm referenced vars/functions exist. "
			"Returns per-op `{ok}` results plus a `would_compile` list of "
			"asset paths the real apply_ops would touch. Useful for "
			"agent self-checks (\"is this batch syntactically valid before I "
			"run it?\") and for a human-in-the-loop confirmation step.";
		pd.input_schema = {
			{"type", "object"},
			{"properties", {
				{"ops", {
					{"type", "array"},
					{"description","Same shape as apply_ops's `ops` field."},
					{"items", {
						{"type", "object"},
						{"properties", {{"op", {{"type","string"}}}}},
						{"required", nlohmann::json::array({"op"})},
						{"additionalProperties", true},
					}},
				}},
			}},
			{"required", nlohmann::json::array({"ops"})},
		};
		registry.Add(std::move(pd), [&reader](const nlohmann::json& args) {
			auto opsIt = args.find("ops");
			if (opsIt == args.end() || !opsIt->is_array()) {
				throw std::invalid_argument(
					R"(preview_ops requires "ops" to be an array)");
			}
			return ValidateOps(reader, *opsIt);
		});
	}
}

}    // namespace bpr::tools
