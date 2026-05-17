#include "tools/Decompile.h"
#include "tools/Bpir.h"

#include <fmt/core.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace bpr::tools {

namespace decompile_detail {

// ----- Type→shorthand reverse mapper -------------------------------------
// Convert a BPPinType back to the shorthand grammar BPIR / TypeShorthand
// use. The forward direction lives in TypeShorthand.cpp; this is the
// inverse for serialization. Common cases handled; weird ones fall back
// to a canonical object form embedded as a JSON string (callers can
// still consume it via ParseTypeArg's object path).
std::string TypeToShorthand(const BPPinType& t) {
	auto sub = t.SubCategory.value_or("");
	auto subObj = t.SubCategoryObject.value_or("");

	auto wrapContainer = [&](std::string inner) {
		if (t.IsArray)
		{
			return std::string("[]") + inner;
		}
		if (t.IsSet)   return std::string("{}") + inner;
		if (t.IsMap) {
			// Map key encoded in SubCategory (matches TypeShorthand's
			// {K:V} forward path); value is the rest.
			std::string keyTy = sub.empty() ? "string" : sub;
			return std::string("{") + keyTy + ":" + inner + "}";
		}
		return inner;
	};

	if (t.Category == "real") {
		if (sub == "double")
		{
			return wrapContainer("double");
		}
		return wrapContainer("float");
	}
	if (t.Category == "bool" || t.Category == "int"   || t.Category == "int64" ||
		t.Category == "byte" || t.Category == "string" || t.Category == "name"  ||
		t.Category == "text" || t.Category == "exec") {
		// For maps, byte+enum-ref needs the "enum:" hint, but we're
		// mapping to the basic forms here.
		return wrapContainer(t.Category);
	}
	if (t.Category == "object" || t.Category == "class" ||
		t.Category == "softobject" || t.Category == "softclass" ||
		t.Category == "interface" || t.Category == "struct") {
		std::string inner = t.Category;
		if (!subObj.empty()) {
			inner += ":";
			inner += subObj;
		}
		return wrapContainer(inner);
	}
	// Unknown / unhandled — use a defensive fallback so codegen has
	// SOMETHING. Object-form would also work but the round-trip would
	// be lossier.
	return t.Category;
}

// Convert BPVariable to the BPIR variable-decl shape.
nlohmann::json VariableDeclToJson(const BPVariable& v) {
	nlohmann::json j = {
		{"name", v.Name},
		{"type", TypeToShorthand(v.Type)},
	};
	if (v.DefaultValue && !v.DefaultValue->empty())
	{
		j["default"]    = *v.DefaultValue;
	}
	if (v.Category && !v.Category->empty())
	{
		j["category"]    = *v.Category;
	}
	if (v.IsReplicated)
	{
		j["replicated"]  = true;
	}
	if (v.IsEditable)
	{
		j["editable"]    = true;
	}
	// Batch 2 wire-format extensions — pass through so CppClassEmit
	// can render UPROPERTY meta=(ExposeOnSpawn=true),
	// ReplicatedUsing=OnRep_X, and DOREPLIFETIME_CONDITION specifiers.
	if (v.RepCondition && !v.RepCondition->empty())
	{
		j["rep_condition"]   = *v.RepCondition;
	}
	if (v.ExposeOnSpawn)
	{
		j["expose_on_spawn"] = true;
	}
	if (v.RepNotifyFunc && !v.RepNotifyFunc->empty())
	{
		j["rep_notify_func"] = *v.RepNotifyFunc;
	}
	// Multicast delegate signature params: surface so CppClassEmit
	// can choose the matching DECLARE_DYNAMIC_MULTICAST_DELEGATE_<N>Params
	// variant. Omit when empty.
	if (!v.DelegateParams.empty()) {
		nlohmann::json params = nlohmann::json::array();
		for (const auto& dp : v.DelegateParams) {
			params.push_back(nlohmann::json{
				{"name", dp.Name},
				{"type", dp.Type},
			});
		}
		j["delegate_params"] = std::move(params);
	}
	return j;
}

// ----- Walk context ------------------------------------------------------
// State threaded through the recursive AST builder. Holds indices over
// the BPGraph for fast lookup, plus the function's variable scope so
// VariableGet/Set can be tagged with "local"/"member"/"input"/"output".

struct Walker {
	const BPGraph& graph;
	const BPFunction& function;
	const std::set<std::string>& memberVarNames;

	// node_id → BPNode* for O(1) lookup.
	std::unordered_map<std::string, const BPNode*> nodeById;

	// pin_id → owning node_id, pin_name (used to resolve refs the
	// commandlet emits as GUIDs).
	std::unordered_map<std::string, std::string> pinOwnerById;
	std::unordered_map<std::string, std::string> pinNameById;

	// Adjacency: from_node:from_pin → list of {to_node, to_pin}.
	struct EdgeEnd {
		std::string node;
		std::string pin;
	};
	std::map<std::pair<std::string, std::string>, std::vector<EdgeEnd>> outEdges;
	// And the reverse, for tracing data flow backward.
	std::map<std::pair<std::string, std::string>, std::vector<EdgeEnd>> inEdges;

	// Synthetic member variables emitted by stateful-macro lowering
	// (DoOnce / FlipFlop / DoN) and by latent-action lowering
	// (Delay → FTimerHandle). Keyed by var name so duplicate
	// registrations from multiple visit paths collapse into one decl.
	// Value is a BPIR variable-decl object (same shape as VariableDeclToJson
	// output) so DecompileBlueprint can merge them straight into the
	// class's variables[] array.
	//
	// mutable: per-node handlers take `const Walker&`, but the synth-var
	// set is logically session-state that grows during the walk. Avoids
	// threading a separate mutable-collector parameter through every
	// handler signature.
	mutable std::map<std::string, nlohmann::json> autoSynthVars;

	// Synthetic class-level functions emitted by latent-action lowering.
	// Each Delay/Timeline boundary splits the parent function: the
	// post-delay exec flow lives as a generated continuation UFUNCTION
	// that the timer callback invokes. Each entry is a complete BPIR
	// function doc (kind="function", with its own body[] etc.) ready
	// for DecompileBlueprint to hoist into the class's functions[]
	// array.
	//
	// Stored as a vector (not a map) because continuation names are
	// already unique per node GUID and the emission order is meaningful
	// (sibling continuations should appear in walk order for stable
	// diffs).
	mutable std::vector<nlohmann::json> autoSynthFunctions;

	Walker(const BPGraph& g, const BPFunction& f,
		   const std::set<std::string>& memberVars)
		: graph(g), function(f), memberVarNames(memberVars)
	{
		for (const auto& n : graph.Nodes) {
			nodeById[n.Id] = &n;
			for (const auto& p : n.Pins) {
				pinOwnerById[p.Id] = n.Id;
				pinNameById[p.Id]  = p.Name;
			}
		}
		for (const auto& c : graph.Connections) {
			outEdges[{c.FromNode, c.FromPin}].push_back({c.ToNode, c.ToPin});
			inEdges [{c.ToNode,   c.ToPin  }].push_back({c.FromNode, c.FromPin});
		}
	}

	const BPNode* GetNode(const std::string& id) const {
		auto it = nodeById.find(id);
		return it == nodeById.end() ? nullptr : it->second;
	}

	const BPPin* GetPin(const BPNode& n, std::string_view name) const {
		for (const auto& p : n.Pins) {
			if (p.Name == name)
			{
				return &p;
			}
		}
		return nullptr;
	}

	// Find the unique exec output the K2 schema gives a node, by name.
	// Returns null if not found.
	const BPPin* FindExecOut(const BPNode& n, std::string_view pinName) const {
		const BPPin* p = GetPin(n, pinName);
		return (p && p->Direction == "Output" &&
				p->Type.Category == "exec") ? p : nullptr;
	}

	// Where does this node's `pin` exec-out lead? Returns null if no
	// outgoing connection (BP path terminates here).
	const BPNode* FollowExec(const BPNode& n, std::string_view pinName) const {
		const BPPin* out = FindExecOut(n, pinName);
		if (!out)
		{
			return nullptr;
		}
		auto it = outEdges.find({n.Id, out->Id});
		if (it == outEdges.end() || it->second.empty())
		{
			return nullptr;
		}
		const auto& target = it->second.front();
		return GetNode(target.node);
	}

	// Tag a variable name with its scope. Inputs/outputs come from the
	// function's signature; locals from `function.Locals`; otherwise
	// it's a member.
	std::string ScopeForVariable(const std::string& name) const {
		for (const auto& v : function.Inputs)
		{
			if (v.Name == name) return "input";
		}
		for (const auto& v : function.Outputs)
		{
			if (v.Name == name) return "output";
		}
		for (const auto& v : function.Locals)
		{
			if (v.Name == name) return "local";
		}
		if (memberVarNames.count(name))
		{
			return "member";
		}
		return {};  // unknown — leave scope unset (consumer can guess)
	}
};

// ----- Expression reconstruction -----------------------------------------
// Trace data edges backward from a consumer pin to figure out what
// expression feeds it. Recursive; each node class has a small visitor.

nlohmann::json BuildExpression(const Walker& w, const BPNode& consumer,
							   const BPPin& consumerPin);

nlohmann::json LiteralFromDefault(const BPPin& pin) {
	// A pin with a default value (no incoming edge) is a literal.
	// We emit the value as-is; callers can re-type via the consumer
	// pin's type.
	nlohmann::json out;
	if (!pin.DefaultValue || pin.DefaultValue->empty()) {
		out = nlohmann::json::object();
		out["lit"] = nullptr;
		return out;
	}
	const auto& v = *pin.DefaultValue;
	// Best-effort scalar parsing: bool first (BP serializes "true"/"false"),
	// then numeric, fallback string.
	if (v == "true")  { out["lit"] = true;  return out; }
	if (v == "false") { out["lit"] = false; return out; }
	try {
		// Numeric heuristic: if it parses, use it.
		std::size_t consumed = 0;
		double d = std::stod(v, &consumed);
		if (consumed == v.size()) {
			// Distinguish int from float for cleaner codegen.
			if (v.find('.') == std::string::npos && d == static_cast<double>(static_cast<long long>(d))) {
				out["lit"] = static_cast<long long>(d);
			} else {
				out["lit"] = d;
			}
			return out;
		}
	} catch (...) { /* fall through */ }
	out["lit"] = v;
	return out;
}

// Identify whether a node feeds a value (has an output data pin) and,
// if so, build the BPIR expression for its return / output.
nlohmann::json ProducerToExpression(const Walker& w, const BPNode& producer,
									const BPPin& outputPin) {
	// Knot (reroute) on data flow — pure passthrough. Walk back through
	// the knot's input pin to find the real producer. Without this,
	// value pins downstream of a reroute node lose their source.
	if (producer.Class.find("K2Node_Knot") != std::string::npos) {
		// Knots have one input pin and one output pin (both unnamed in
		// the UI). Walk the input back to its source.
		for (const auto& p : producer.Pins) {
			if (p.Direction == "Input") {
				return BuildExpression(w, producer, p);
			}
		}
		// No input → treat as a default literal of the output pin's type.
		return LiteralFromDefault(outputPin);
	}

	if (producer.Class.find("K2Node_VariableGet") != std::string::npos) {
		std::string varName;
		if (producer.Meta.is_object()) {
			varName = producer.Meta.value("variableName", std::string{});
		}
		nlohmann::json out = {{"var", varName}};
		std::string scope = w.ScopeForVariable(varName);
		if (!scope.empty())
		{
			out["scope"] = scope;
		}
		return out;
	}
	if (producer.Class.find("K2Node_Self") != std::string::npos) {
		return nlohmann::json{{"self", nullptr}};
	}
	if (producer.Class.find("K2Node_EnumLiteral") != std::string::npos) {
		// Enum literal: BP picks a single named value of an enum type.
		// Meta carries the enum type's path + the chosen value.
		// C++ form: `EEnumType::ValueName`. Without this lowering,
		// enum literals leak as TODO sentinels through any consumer.
		std::string enumType, valueName;
		if (producer.Meta.is_object()) {
			enumType  = producer.Meta.value("enumType",  std::string{});
			if (enumType.empty()) enumType = producer.Meta.value("enum",  std::string{});
			valueName = producer.Meta.value("enumValue", std::string{});
			if (valueName.empty()) valueName = producer.Meta.value("value", std::string{});
		}
		// Fallback: look at the named output pin for the value.
		if (valueName.empty())
		{
			valueName = outputPin.Name;
		}
		// Strip /Script/Module. path from enum type if present.
		if (auto dot = enumType.find_last_of('.'); dot != std::string::npos) {
			enumType = enumType.substr(dot + 1);
		}
		// Prepend E if not already E-prefixed (UE convention).
		if (!enumType.empty() && (enumType[0] != 'E' ||
								   (enumType.size() < 2 || enumType[1] < 'A' || enumType[1] > 'Z'))) {
			enumType = "E" + enumType;
		}
		std::string qualified;
		if (!enumType.empty())
		{
			qualified = enumType + "::" + valueName;
		}
		else                    qualified = valueName;
		// Emit as a literal so codegen passes it through unchanged
		// (string with leading '/' or '*' or '/*' is preserved verbatim
		// -- not the case here, but for safety use the explicit form).
		return nlohmann::json{{"lit", qualified}};
	}
	if (producer.Class.find("K2Node_Tunnel") != std::string::npos) {
		// Tunnel nodes appear in macros / composite (collapsed) nodes
		// as entry/exit points. On the data flow side they're pure
		// passthroughs -- the consumer pin's source is whichever pin
		// on the tunnel feeds it, which we already resolve via
		// BuildExpression. If we somehow hit a Tunnel here (rare on
		// post-expansion graphs), treat as a passthrough Knot.
		for (const auto& p : producer.Pins) {
			if (p.Direction == "Input" && p.Type.Category != "exec") {
				return BuildExpression(w, producer, p);
			}
		}
		return LiteralFromDefault(outputPin);
	}
	if (producer.Class.find("K2Node_Select") != std::string::npos) {
		// K2Node_Select: picks one of N input value pins based on the
		// "Index" pin. For the common N=2 bool case, lower to a C++
		// ternary `(<Index> ? <Option_1> : <Option_0>)`. For N>2 with
		// an int/enum index, lower to a chained ternary (more general
		// and renders cleanly in most cases). Fall back to the TODO
		// sentinel when the option pins can't be resolved.
		const BPPin* indexPin = w.GetPin(producer, "Index");
		if (!indexPin) {
			// Some BP variants use "TargetExpression" or the bool's name.
			for (const auto& p : producer.Pins) {
				if (p.Direction == "Input" && p.Type.Category != "exec" &&
					p.Name.find("Option") == std::string::npos) {
					indexPin = &p;
					break;
				}
			}
		}
		// Collect option pins. UE names them "Option 0", "Option 1", ...
		// or "False" / "True" for the bool case.
		struct Opt { int slot; const BPPin* pin; };
		std::vector<Opt> options;
		for (const auto& p : producer.Pins) {
			if (p.Direction != "Input" || p.Type.Category == "exec") continue;
			if (&p == indexPin) continue;
			if (p.Name == "False") { options.push_back({0, &p}); continue; }
			if (p.Name == "True")  { options.push_back({1, &p}); continue; }
			// "Option <n>" or "Option_<n>".
			std::size_t start = std::string::npos;
			if (p.Name.size() > 7 && p.Name.compare(0, 6, "Option") == 0) start = 6;
			if (start != std::string::npos) {
				std::size_t i = start;
				if (i < p.Name.size() && (p.Name[i] == ' ' || p.Name[i] == '_')) ++i;
				try { options.push_back({std::stoi(p.Name.substr(i)), &p}); }
				catch (...) { /* ignore unparseable */ }
			}
		}
		std::sort(options.begin(), options.end(),
				  [](const Opt& a, const Opt& b) { return a.slot < b.slot; });
		if (!indexPin || options.size() < 2) {
			return LiteralFromDefault(outputPin);
		}
		nlohmann::json indexExpr = BuildExpression(w, producer, *indexPin);
		// Bool case: emit a single ternary.
		if (options.size() == 2) {
			return nlohmann::json{{"call", "__bpr_select_ternary"},
				{"args", nlohmann::json{
					{"Index",  indexExpr},
					{"False",  BuildExpression(w, producer, *options[0].pin)},
					{"True",   BuildExpression(w, producer, *options[1].pin)},
				}}};
		}
		// N-way: emit a __bpr_select_n sentinel that CppEmit lowers
		// to chained ternaries.
		nlohmann::json args = nlohmann::json::object();
		args["Index"] = indexExpr;
		for (const auto& opt : options) {
			args[fmt::format("Option_{}", opt.slot)] =
				BuildExpression(w, producer, *opt.pin);
		}
		return nlohmann::json{{"call", "__bpr_select_n"}, {"args", std::move(args)}};
	}
	if (producer.Class.find("K2Node_Literal") != std::string::npos) {
		// Literal nodes carry their value in meta.literalObject (object
		// refs) or pin defaults; for v1 emit a sentinel + the meta
		// contents so codegen can pretty-print.
		std::string ref;
		if (producer.Meta.is_object()) {
			ref = producer.Meta.value("literalObject", std::string{});
		}
		nlohmann::json out;
		out["lit"] = ref.empty() ? nlohmann::json(nullptr) : nlohmann::json(ref);
		return out;
	}
	if (producer.Class.find("K2Node_CallFunction") != std::string::npos) {
		// CallFunction whose output is the value pin we're tracing.
		std::string fnName  = producer.Meta.is_object() ? producer.Meta.value("targetFunction", std::string{}) : "";
		std::string ownerCl = producer.Meta.is_object() ? producer.Meta.value("targetClass",    std::string{}) : "";
		std::string fqName  = ownerCl.empty() ? fnName : (ownerCl + "::" + fnName);
		nlohmann::json out = {{"call", fqName}};
		nlohmann::json args = nlohmann::json::object();
		for (const auto& p : producer.Pins) {
			if (p.Direction != "Input" || p.Type.Category == "exec")
			{
				continue;
			}
			if (p.Name == "self")
			{
				continue;  // implicit; codegen handles
			}
			args[p.Name] = BuildExpression(w, producer, p);
		}
		if (!args.empty())
		{
			out["args"] = std::move(args);
		}
		return out;
	}
	// GetClassDefaults → `GetDefault<T>()->Field` or
	// `Class->GetDefaultObject<T>()->Field`. The K2 node has a Class
	// input + one output pin per CDO field. We expose this via a
	// structured sentinel call that CppEmit unpacks at render time.
	if (producer.Class.find("K2Node_GetClassDefaults") != std::string::npos) {
		const BPPin* classPin = w.GetPin(producer, "Class");
		nlohmann::json classExpr = classPin
			? BuildExpression(w, producer, *classPin)
			: nlohmann::json{{"lit", nullptr}};
		// The output pin we're tracing identifies which field on the
		// CDO is being read.
		return nlohmann::json{
			{"call", "__bpr_get_class_defaults"},
			{"args", nlohmann::json{
				{"Class", classExpr},
				{"Field", nlohmann::json{{"lit", outputPin.Name}}},
			}},
		};
	}

	// FormatText: BP has a `Format` input pin (the format string with
	// {Name} placeholders) plus one input pin per named argument. The
	// C++ form is FText::Format(LOCTEXT(..., "Hello {Name}"), Args)
	// where Args is FFormatNamedArguments populated from the pins.
	// We emit a structured sentinel call that CppEmit renders.
	if (producer.Class.find("K2Node_FormatText") != std::string::npos) {
		std::string formatStr;
		nlohmann::json args = nlohmann::json::object();
		for (const auto& p : producer.Pins) {
			if (p.Direction != "Input")
			{
				continue;
			}
			if (p.Name == "Format") {
				// Format pin's literal string default. Build it as a lit.
				if (p.DefaultValue && !p.DefaultValue->empty()) {
					formatStr = *p.DefaultValue;
				}
				continue;
			}
			// Skip exec / structural pins; named-arg pins are data inputs.
			if (p.Type.Category == "exec")
			{
				continue;
			}
			args[p.Name] = BuildExpression(w, producer, p);
		}
		nlohmann::json out = {{"call", "__bpr_format_text"}};
		if (!formatStr.empty())
		{
			out["format"] = formatStr;
		}
		if (!args.empty())
		{
			out["args"] = std::move(args);
		}
		return out;
	}

	if (producer.Class.find("K2Node_MakeArray") != std::string::npos) {
		nlohmann::json arr = nlohmann::json::array();
		for (const auto& p : producer.Pins) {
			if (p.Direction != "Input" || p.Type.Category == "exec")
			{
				continue;
			}
			arr.push_back(BuildExpression(w, producer, p));
		}
		return nlohmann::json{{"new_array", std::move(arr)}};
	}
	if (producer.Class.find("K2Node_MakeSet") != std::string::npos) {
		// K2Node_MakeSet: like MakeArray but produces a TSet. Pin
		// shape is identical -- a series of value input pins, one
		// output. Element order isn't semantically significant for
		// sets but we preserve BP-pin order for stability.
		nlohmann::json arr = nlohmann::json::array();
		for (const auto& p : producer.Pins) {
			if (p.Direction != "Input" || p.Type.Category == "exec")
			{
				continue;
			}
			arr.push_back(BuildExpression(w, producer, p));
		}
		return nlohmann::json{{"new_set", std::move(arr)}};
	}
	if (producer.Class.find("K2Node_MakeMap") != std::string::npos) {
		// K2Node_MakeMap: input pins are pairs. UE names them
		// "Key_0", "Value_0", "Key_1", "Value_1", ... (or just
		// "Key 0" / "Value 0" in older versions). Match on prefix
		// so we pick up both shapes, then sort by the numeric tail
		// to keep declaration order.
		struct Pair { const BPPin* key = nullptr; const BPPin* val = nullptr; };
		std::map<int, Pair> bySlot;
		auto parseSlot = [](const std::string& s, const char* prefix) -> int {
			std::size_t plen = std::strlen(prefix);
			if (s.size() < plen)
			{
				return -1;
			}
			if (s.compare(0, plen, prefix) != 0)
			{
				return -1;
			}
			// Skip an optional separator (' ' or '_') after the prefix.
			std::size_t i = plen;
			if (i < s.size() && (s[i] == ' ' || s[i] == '_'))
			{
				++i;
			}
			if (i >= s.size())
			{
				return -1;
			}
			try { return std::stoi(s.substr(i)); } catch (...) { return -1; }
		};
		for (const auto& p : producer.Pins) {
			if (p.Direction != "Input" || p.Type.Category == "exec")
			{
				continue;
			}
			if (int slot = parseSlot(p.Name, "Key");   slot >= 0)
			{
				bySlot[slot].key = &p;
			}
			if (int slot = parseSlot(p.Name, "Value"); slot >= 0)
			{
				bySlot[slot].val = &p;
			}
		}
		nlohmann::json arr = nlohmann::json::array();
		for (const auto& [slot, pp] : bySlot) {
			if (!pp.key || !pp.val)
			{
				continue;  // skip incomplete pairs
			}
			nlohmann::json entry;
			entry["key"]   = BuildExpression(w, producer, *pp.key);
			entry["value"] = BuildExpression(w, producer, *pp.val);
			arr.push_back(std::move(entry));
		}
		return nlohmann::json{{"new_map", std::move(arr)}};
	}
	if (producer.Class.find("K2Node_BreakStruct") != std::string::npos) {
		// K2Node_BreakStruct: input is the struct, outputs are
		// per-field accessors. The downstream consumer's pin is named
		// for the field; we lower to {member: <struct>, name: <field>}.
		// BP also uses K2Node_BreakStructHelper for some variants --
		// the substring match catches both.
		// Locate the input struct pin. UE typically names it after
		// the struct type (e.g. "Vector"), but the safest path is
		// "the only Input data pin".
		const BPPin* inPin = nullptr;
		for (const auto& p : producer.Pins) {
			if (p.Direction == "Input" && p.Type.Category != "exec") {
				inPin = &p;
				break;
			}
		}
		if (!inPin)
		{
			return LiteralFromDefault(outputPin);
		}
		nlohmann::json structExpr = BuildExpression(w, producer, *inPin);
		return nlohmann::json{
			{"member", std::move(structExpr)},
			{"name",   outputPin.Name},
		};
	}
	if (producer.Class.find("K2Node_MakeStruct") != std::string::npos) {
		std::string structType;
		if (producer.Meta.is_object()) {
			structType = producer.Meta.value("structType", std::string{});
		}
		nlohmann::json fields = nlohmann::json::object();
		for (const auto& p : producer.Pins) {
			if (p.Direction != "Input" || p.Type.Category == "exec")
			{
				continue;
			}
			fields[p.Name] = BuildExpression(w, producer, p);
		}
		return nlohmann::json{
			{"new_struct", structType},
			{"fields", std::move(fields)},
		};
	}
	if (producer.Class.find("K2Node_DynamicCast") != std::string::npos) {
		// Pure cast (expression form). The "object" output pin is the
		// cast result. We expect outputPin.Name == "AsXxx" or similar.
		std::string targetClass;
		if (producer.Meta.is_object()) {
			targetClass = producer.Meta.value("targetClass", std::string{});
		}
		// Cast input is the "Object" pin.
		const BPPin* inPin = w.GetPin(producer, "Object");
		nlohmann::json castExpr = inPin
			? BuildExpression(w, producer, *inPin)
			: nlohmann::json{{"self", nullptr}};
		return nlohmann::json{
			{"cast", castExpr},
			{"to", targetClass},
		};
	}
	// FunctionEntry / Event / CustomEvent output data pin — resolve to
	// the parameter name. Without this, a downstream VariableSet that
	// reads from a FunctionEntry pin gets a TODO sentinel where the
	// parameter reference should be. See diagnostics finding #6.
	if (producer.Class.find("K2Node_FunctionEntry") != std::string::npos ||
		producer.Class.find("K2Node_Event")         != std::string::npos ||
		producer.Class.find("K2Node_CustomEvent")   != std::string::npos) {
		// outputPin.Name is the parameter's canonical name. Treat as a
		// variable in input scope so codegen renders it directly.
		return nlohmann::json{
			{"var",   outputPin.Name},
			{"scope", "input"},
		};
	}
	// Anything else: emit a BPIR `unsupported`-shaped expression. This
	// isn't a valid BPIR expression form per validator, so we wrap it
	// in a `lit` with a string telling consumers what we couldn't
	// express. Callers (codegen) check for this sentinel and emit a
	// TODO comment.
	return nlohmann::json{
		{"lit", fmt::format("/* TODO[bpr-expr]: {} (guid={}) */",
							producer.Class, producer.Id)},
	};
}

nlohmann::json BuildExpression(const Walker& w, const BPNode& consumer,
							   const BPPin& consumerPin) {
	auto it = w.inEdges.find({consumer.Id, consumerPin.Id});
	if (it == w.inEdges.end() || it->second.empty()) {
		return LiteralFromDefault(consumerPin);
	}
	// Multi-source data pins are unusual but possible (struct splits).
	// First-source wins for v1.
	const auto& src = it->second.front();
	const BPNode* producer = w.GetNode(src.node);
	if (!producer)
	{
		return LiteralFromDefault(consumerPin);
	}
	auto pIt = w.pinNameById.find(src.pin);
	const std::string& outPinName = pIt != w.pinNameById.end() ? pIt->second : std::string{};
	const BPPin* outPin = w.GetPin(*producer, outPinName);
	if (!outPin)
	{
		return LiteralFromDefault(consumerPin);
	}
	return ProducerToExpression(w, *producer, *outPin);
}

// ----- Statement reconstruction ------------------------------------------
// Walk forward from a starting exec edge, decompiling statements until
// we hit the stop node (post-dominator) or run out of edges.

nlohmann::json DecompileStatementsFrom(const Walker& w,
									   const BPNode* start,
									   const BPNode* stopAt,
									   std::set<std::string>& visited);

// Decompile a single node into one BPIR statement. Returns the next
// node to walk to (or null if exec terminates here, or `stopAt` if
// caller should stop).
struct DecompileResult {
	nlohmann::json statement;
	// Optional sidecar statements emitted BEFORE `statement` in the
	// caller's output array. Used by stateful-macro lowering to
	// surface "you also need to handle Reset elsewhere"-style
	// unsupported notes inline at the macro's call site, where they're
	// most discoverable, rather than as a top-level summary.
	nlohmann::json preStatements = nlohmann::json::array();
	const BPNode* next = nullptr;
	bool terminatesExec = false;  // return / break / continue / dangling
};

// `visited` is intentionally [[maybe_unused]] here: each statement-form
// recursion below creates its own fresh visited set (then-branch,
// else-branch, switch cases, for-each body, cast success/fail blocks).
// Diamond-shaped exec flow can legitimately revisit the same node from
// multiple branches, so sharing one visited set would produce false-
// positive "exec-cycle" reports. The DecompileStatementsFrom caller's
// visited set is the one that owns cycle detection at this level.
DecompileResult DecompileStatement(const Walker& w, const BPNode& n,
								   const BPNode* stopAt,
								   [[maybe_unused]] std::set<std::string>& visited);

nlohmann::json DecompileStatementsFrom(const Walker& w,
									   const BPNode* start,
									   const BPNode* stopAt,
									   std::set<std::string>& visited) {
	nlohmann::json out = nlohmann::json::array();
	const BPNode* cur = start;
	while (cur && cur != stopAt) {
		if (visited.count(cur->Id)) {
			// Cycle without a recognized loop pattern. Emit unsupported.
			out.push_back(nlohmann::json{{"unsupported", nlohmann::json{
				{"node_class", cur->Class},
				{"guid", cur->Id},
				{"reason", "exec-cycle (loop pattern not recognized)"},
			}}});
			break;
		}
		visited.insert(cur->Id);
		DecompileResult res = DecompileStatement(w, *cur, stopAt, visited);
		// Flush any sidecar statements emitted ahead of the main
		// statement (e.g. stateful-macro "Reset pin unhandled" notes
		// surface inline so the agent sees them at the relevant call
		// site).
		if (res.preStatements.is_array()) {
			for (auto& s : res.preStatements)
			{
				out.push_back(std::move(s));
			}
		}
		// Skip the no-op pseudo-statement that DecompileStatement uses
		// to mean "this node is structural-only" (FunctionEntry).
		if (!res.statement.is_null() &&
			!(res.statement.is_object() && res.statement.empty())) {
			out.push_back(std::move(res.statement));
		}
		if (res.terminatesExec)
		{
			break;
		}
		cur = res.next;
	}
	return out;
}

DecompileResult DecompileStatement(const Walker& w, const BPNode& n,
								   const BPNode* stopAt,
								   [[maybe_unused]] std::set<std::string>& visited) {
	DecompileResult r;

	// FunctionEntry: structural — skip and follow `then`.
	if (n.Class.find("K2Node_FunctionEntry") != std::string::npos) {
		r.next = w.FollowExec(n, "then");
		r.statement = nlohmann::json::object();  // skipped by caller
		return r;
	}

	// FunctionResult: emit `{return: ...}`.
	if (n.Class.find("K2Node_FunctionResult") != std::string::npos) {
		nlohmann::json returns = nlohmann::json::array();
		for (const auto& p : n.Pins) {
			if (p.Direction != "Input" || p.Type.Category == "exec")
			{
				continue;
			}
			returns.push_back(BuildExpression(w, n, p));
		}
		if (returns.empty())      r.statement = nlohmann::json{{"return", nullptr}};
		else if (returns.size() == 1) r.statement = nlohmann::json{{"return", returns[0]}};
		else                          r.statement = nlohmann::json{{"return", returns}};
		r.terminatesExec = true;
		return r;
	}

	// K2Node_AssignmentStatement: BP's `<Lhs> = <Rhs>` node. Pins are
	// "Variable" (LHS, must be a write-target -- VariableGet of a
	// settable property) and "Value" (RHS expression). Less common
	// than VariableSet but appears in macro-expanded graphs.
	if (n.Class.find("K2Node_AssignmentStatement") != std::string::npos) {
		const BPPin* lhsPin = w.GetPin(n, "Variable");
		const BPPin* rhsPin = w.GetPin(n, "Value");
		if (lhsPin && rhsPin) {
			// The LHS is read as an expression (BuildExpression
			// traces upstream to find what variable to write).
			// Codegen renders `{set: <name>, to: <rhs>}` if we can
			// narrow the LHS to a var name. Otherwise fall through
			// to unsupported.
			nlohmann::json lhsExpr = BuildExpression(w, n, *lhsPin);
			if (lhsExpr.contains("var") && lhsExpr["var"].is_string()) {
				nlohmann::json stmt = {
					{"set", lhsExpr["var"].get<std::string>()},
					{"to",  BuildExpression(w, n, *rhsPin)},
				};
				if (lhsExpr.contains("scope") && lhsExpr["scope"].is_string()) {
					stmt["scope"] = lhsExpr["scope"];
				}
				r.statement = std::move(stmt);
				r.next = w.FollowExec(n, "then");
				return r;
			}
		}
		// Fallthrough: emit a generic unsupported entry.
	}

	// VariableSet: `{set: name, to: <expr>}`.
	if (n.Class.find("K2Node_VariableSet") != std::string::npos) {
		std::string varName;
		if (n.Meta.is_object()) varName = n.Meta.value("variableName", std::string{});
		// The value pin is named the same as the variable in K2.
		const BPPin* valPin = w.GetPin(n, varName);
		nlohmann::json setExpr = valPin
			? BuildExpression(w, n, *valPin)
			: nlohmann::json{{"lit", nullptr}};
		nlohmann::json stmt = {{"set", varName}, {"to", setExpr}};
		std::string scope = w.ScopeForVariable(varName);
		if (!scope.empty())
		{
			stmt["scope"] = scope;
		}
		r.statement = std::move(stmt);
		r.next = w.FollowExec(n, "then");
		return r;
	}

	// Branch (K2Node_IfThenElse): emit `{if, then, else}`. Each branch
	// recursively decompiles until convergence.
	if (n.Class.find("K2Node_IfThenElse") != std::string::npos) {
		const BPPin* condPin = w.GetPin(n, "Condition");
		nlohmann::json cond = condPin
			? BuildExpression(w, n, *condPin)
			: nlohmann::json{{"lit", true}};

		const BPNode* thenStart = w.FollowExec(n, "then");
		const BPNode* elseStart = w.FollowExec(n, "else");

		// Find the convergence point — the first node both branches
		// reach. v1 heuristic: walk both branches, recording the
		// visited-order; first node both touch is the merge.
		const BPNode* merge = nullptr;
		{
			std::set<std::string> thenSeen;
			const BPNode* cur = thenStart;
			std::set<std::string> guard;
			while (cur && !guard.count(cur->Id)) {
				guard.insert(cur->Id);
				thenSeen.insert(cur->Id);
				cur = w.FollowExec(*cur, "then");
				if (!cur)
				{
					break;
				}
			}
			cur = elseStart;
			guard.clear();
			while (cur && !guard.count(cur->Id)) {
				guard.insert(cur->Id);
				if (thenSeen.count(cur->Id)) {
					merge = cur;
					break;
				}
				cur = w.FollowExec(*cur, "then");
			}
		}

		std::set<std::string> branchVisited;
		nlohmann::json thenBody = DecompileStatementsFrom(w, thenStart, merge, branchVisited);
		nlohmann::json elseBody = DecompileStatementsFrom(w, elseStart, merge, branchVisited);
		r.statement = {{"if", cond}, {"then", thenBody}, {"else", elseBody}};
		r.next = merge;  // continue from the merge point (or null = end)
		return r;
	}

	// DynamicCast (statement form — has both success + fail exec outs).
	if (n.Class.find("K2Node_DynamicCast") != std::string::npos) {
		const BPPin* objPin = w.GetPin(n, "Object");
		nlohmann::json castExpr = objPin
			? BuildExpression(w, n, *objPin)
			: nlohmann::json{{"self", nullptr}};
		std::string targetClass;
		if (n.Meta.is_object()) targetClass = n.Meta.value("targetClass", std::string{});
		const BPNode* successStart = w.FollowExec(n, "then");
		const BPNode* failStart    = w.FollowExec(n, "CastFailed");

		// Conservative: don't try to merge here — just walk until exec
		// terminates. The downstream caller's stopAt still applies.
		std::set<std::string> succV, failV;
		nlohmann::json successBody = DecompileStatementsFrom(w, successStart, stopAt, succV);
		nlohmann::json failBody    = DecompileStatementsFrom(w, failStart,    stopAt, failV);

		// The "as" local name comes from the As<Class> output pin.
		std::string asName = "AsCast";
		for (const auto& p : n.Pins) {
			if (p.Direction == "Output" && p.Name.find("As") == 0) {
				asName = p.Name;
				break;
			}
		}
		r.statement = {
			{"cast", castExpr},
			{"to", targetClass},
			{"as", asName},
			{"success", successBody},
			{"fail", failBody},
		};
		// Both branches may have followed past stopAt or terminated;
		// exec-merge after a cast statement isn't always clean. v1:
		// treat the cast as exec-terminating; the agent can issue a
		// follow-up wire_pins if the next-statement chain matters.
		r.terminatesExec = true;
		return r;
	}

	// ExecutionSequence: emit `{sequence: [[...], [...]]}`.
	if (n.Class.find("K2Node_ExecutionSequence") != std::string::npos) {
		nlohmann::json branches = nlohmann::json::array();
		// Sequence pins are named "Then 0", "Then 1", ...
		for (const auto& p : n.Pins) {
			if (p.Direction != "Output" || p.Type.Category != "exec")
			{
				continue;
			}
			const BPNode* branchStart = w.FollowExec(n, p.Name);
			std::set<std::string> branchVisited;
			branches.push_back(DecompileStatementsFrom(w, branchStart, stopAt, branchVisited));
		}
		r.statement = nlohmann::json{{"sequence", std::move(branches)}};
		r.terminatesExec = true;  // sequence covers all downstream paths
		return r;
	}

	// SpawnActorFromClass — emit a structured BPIR call; CppEmit
	// recognizes the sentinel and renders as
	// `GetWorld()->SpawnActor<T>(...)`. The args carry Class,
	// SpawnTransform, CollisionHandlingOverride, Owner, Instigator
	// straight from the BP node's input pins, so the generated C++
	// doesn't need a TODO — every value is preserved.
	if (n.Class.find("K2Node_SpawnActorFromClass") != std::string::npos) {
		nlohmann::json args = nlohmann::json::object();
		for (const auto& p : n.Pins) {
			if (p.Direction != "Input" || p.Type.Category == "exec")
			{
				continue;
			}
			args[p.Name] = BuildExpression(w, n, p);
		}
		nlohmann::json stmt = {{"call", "__bpr_spawn_actor_from_class"}};
		if (!args.empty())
		{
			stmt["args"] = std::move(args);
		}
		r.statement = std::move(stmt);
		r.next = w.FollowExec(n, "then");
		return r;
	}

	// GetDataTableRow — common BP node for table-driven gameplay.
	// Pins: DataTable (input), RowName (input), RowFound (output exec),
	// OutRow (output struct), plus failure exec.
	// C++ equivalent: `DataTable->FindRow<FRowType>(RowName, "context")`,
	// returning a pointer that's null on miss. We carry the args; CppEmit
	// renders the FindRow call + a nullness check that branches success/fail.
	if (n.Class.find("K2Node_GetDataTableRow") != std::string::npos) {
		nlohmann::json args = nlohmann::json::object();
		if (const BPPin* p = w.GetPin(n, "DataTable"))
		{
			args["DataTable"] = BuildExpression(w, n, *p);
		}
		if (const BPPin* p = w.GetPin(n, "RowName"))
		{
			args["RowName"]   = BuildExpression(w, n, *p);
		}
		// Try to capture the row struct type from node meta so CppEmit can
		// render the template arg. Different UE versions stamp this under
		// slightly different keys; check the common ones.
		std::string rowStruct;
		if (n.Meta.is_object()) {
			rowStruct = n.Meta.value("row_struct", std::string{});
			if (rowStruct.empty()) rowStruct = n.Meta.value("rowStruct", std::string{});
			if (rowStruct.empty()) rowStruct = n.Meta.value("structType", std::string{});
		}
		const BPNode* foundStart  = w.FollowExec(n, "RowFound");
		if (!foundStart)
		{
			foundStart = w.FollowExec(n, "then");
		}
		const BPNode* missedStart = w.FollowExec(n, "RowNotFound");

		std::set<std::string> succV, failV;
		nlohmann::json foundBody  = DecompileStatementsFrom(w, foundStart,  stopAt, succV);
		nlohmann::json missedBody = missedStart
			? DecompileStatementsFrom(w, missedStart, stopAt, failV)
			: nlohmann::json::array();

		nlohmann::json stmt = {{"call", "__bpr_get_data_table_row"}};
		if (!args.empty())
		{
			stmt["args"] = std::move(args);
		}
		if (!rowStruct.empty())
		{
			stmt["row_struct"] = rowStruct;
		}
		stmt["success"] = std::move(foundBody);
		stmt["fail"]    = std::move(missedBody);
		r.statement = std::move(stmt);
		r.terminatesExec = true;
		return r;
	}

	// ConstructObjectFromClass — `NewObject<T>(Outer, Class)`. The Outer
	// pin is named "Outer" or omitted (defaults to GetTransientPackage()
	// in C++; we use `this` as a sensible default for actor-context BPs).
	// The Class pin is the class to instantiate.
	if (n.Class.find("K2Node_ConstructObjectFromClass") != std::string::npos) {
		nlohmann::json args = nlohmann::json::object();
		if (const BPPin* p = w.GetPin(n, "Class"))
		{
			args["Class"] = BuildExpression(w, n, *p);
		}
		if (const BPPin* p = w.GetPin(n, "Outer"))
		{
			args["Outer"] = BuildExpression(w, n, *p);
		}
		nlohmann::json stmt = {{"call", "__bpr_construct_object_from_class"}};
		if (!args.empty())
		{
			stmt["args"] = std::move(args);
		}
		r.statement = std::move(stmt);
		r.next = w.FollowExec(n, "then");
		return r;
	}

	// CallParentFunction → `Super::FunctionName(args)`. BP authors
	// use this node when overriding a parent class's BlueprintNativeEvent
	// and want to call the C++ base implementation; the C++ idiom is
	// `Super::FunctionName(args)`.
	if (n.Class.find("K2Node_CallParentFunction") != std::string::npos) {
		std::string fnName;
		if (n.Meta.is_object()) {
			fnName = n.Meta.value("targetFunction", std::string{});
			if (fnName.empty()) fnName = n.Meta.value("functionReference", std::string{});
		}
		nlohmann::json args = nlohmann::json::object();
		for (const auto& p : n.Pins) {
			if (p.Direction != "Input" || p.Type.Category == "exec")
			{
				continue;
			}
			if (p.Name == "self")
			{
				continue;
			}
			args[p.Name] = BuildExpression(w, n, p);
		}
		nlohmann::json stmt = {{"call", "Super::" + fnName}};
		if (!args.empty())
		{
			stmt["args"] = std::move(args);
		}
		r.statement = std::move(stmt);
		r.next = w.FollowExec(n, "then");
		return r;
	}

	// MultiGate — opens N output exec paths in sequence/random, like a
	// stateful switch. No clean single-statement C++ idiom; we emit a
	// TODO with the canonical refactor (member int32 + switch). The
	// BP node's pin count is captured so the agent gets context.
	if (n.Class.find("K2Node_MultiGate") != std::string::npos) {
		nlohmann::json fields = nlohmann::json::object();
		if (n.Meta.is_object())
		{
			fields = n.Meta;
		}
		// Count the output exec pins (one per gate).
		int gateCount = 0;
		for (const auto& p : n.Pins) {
			if (p.Direction == "Output" && p.Type.Category == "exec")
			{
				gateCount++;
			}
		}
		fields["gate_count"] = gateCount;
		r.statement = nlohmann::json{{"unsupported", nlohmann::json{
			{"node_class", n.Class},
			{"guid", n.Id},
			{"reason",
			 "MultiGate — stateful exec router. Add an int32 member to "
			 "track the current gate, increment per call, switch on it. "
			 "Outputs to gate0..N follow."},
			{"fields", std::move(fields)},
		}}};
		r.next = w.FollowExec(n, "then");
		return r;
	}

	// LoadAsset / LoadAssetClass / LoadAssetBlocking — async asset
	// loading. The C++ idiom uses FStreamableManager via
	// UAssetManager::GetStreamableManager().RequestAsyncLoad().
	// Like Delay, the exec continues in a callback — not single-
	// statement-able. Emit a structured TODO with the canonical
	// refactor.
	// K2Node_BaseAsyncTask / K2Node_AsyncAction / K2Node_LatentAbilityCall /
	// K2Node_LatentGameplayTaskCall — UBlueprintAsyncActionBase pattern.
	//
	// BP shape: one async-task node with named exec output pins (typically
	// `OnSuccess` / `OnFailed` / `OnUpdate` per the task's delegate
	// signatures) plus typed data pins for each delegate's payload.
	//
	// C++ shape:
	//   auto* Action = U<TaskClass>::<Factory>(WorldContext, args...);
	//   Action->OnSuccess.AddDynamic(this, &ThisClass::On<X>Success);
	//   Action->OnFailed.AddDynamic(this, &ThisClass::On<X>Failed);
	//   Action->Activate();
	//   UFUNCTION() void On<X>Success(<PayloadT> Payload) { ... }
	//   UFUNCTION() void On<X>Failed() { ... }
	//
	// Full auto-lowering requires three things this decompile pass
	// doesn't yet have:
	//   1. Walker entry-pin tracking — to know which output exec pin
	//      maps to which downstream chain (same blocker as Gate, same
	//      planned refactor).
	//   2. Per-delegate-signature param introspection — to know that
	//      OnSuccess's payload pin is of type FPayloadStruct and goes
	//      to the callback's first parameter.
	//   3. A walker-side rewriter that synthesizes the callback functions
	//      with the right signatures + injects AddDynamic bindings into
	//      the pre-Activate sequence.
	//
	// Until those land, emit a rich structured sidecar that captures
	// every input arg, every wired output exec, and every output data
	// pin — enough for the agent to hand-translate at the call site.
	if (n.Class.find("K2Node_BaseAsyncTask")        != std::string::npos ||
		n.Class.find("K2Node_AsyncAction")           != std::string::npos ||
		n.Class.find("K2Node_LatentAbilityCall")     != std::string::npos ||
		n.Class.find("K2Node_LatentGameplayTaskCall") != std::string::npos) {
		nlohmann::json fields = nlohmann::json::object();
		if (n.Meta.is_object()) {
			fields = n.Meta;
		}
		// Factory function hint — pulled from meta if introspector
		// surfaced it; falls back to the conventional `Create`.
		std::string factory = n.Meta.is_object()
			? n.Meta.value("targetFunction", n.Meta.value("function_name", std::string{}))
			: std::string{};
		if (factory.empty()) {
			factory = "Create";
		}
		std::string targetClass = n.Meta.is_object()
			? n.Meta.value("targetClass", n.Meta.value("function_owner", std::string{}))
			: std::string{};

		// Input args: every non-exec, non-self input pin → its expression.
		nlohmann::json factoryArgs = nlohmann::json::object();
		for (const auto& p : n.Pins) {
			if (p.Direction != "Input" || p.Type.Category == "exec") {
				continue;
			}
			if (p.Name == "self") {
				continue;
			}
			factoryArgs[p.Name] = BuildExpression(w, n, p);
		}

		// Wired output exec pins → each becomes a UFUNCTION callback.
		// Suggested name pattern: `On<NodeTitle><PinName>`. Strip
		// spaces / non-ident chars from NodeTitle.
		std::string titleStem = n.Title;
		std::string sanitizedTitle;
		for (char c : titleStem) {
			if ((c >= '0' && c <= '9') ||
				(c >= 'a' && c <= 'z') ||
				(c >= 'A' && c <= 'Z')) {
				sanitizedTitle.push_back(c);
			}
		}
		if (sanitizedTitle.empty()) {
			sanitizedTitle = "AsyncTask";
		}

		nlohmann::json wiredCallbacks = nlohmann::json::array();
		nlohmann::json outputDataPins = nlohmann::json::array();
		for (const auto& p : n.Pins) {
			if (p.Direction != "Output") {
				continue;
			}
			if (p.Type.Category == "exec") {
				auto it = w.outEdges.find({n.Id, p.Id});
				const bool wired = (it != w.outEdges.end() && !it->second.empty());
				if (!wired) {
					continue;
				}
				nlohmann::json cb = {
					{"exec_pin",        p.Name},
					{"suggested_name",  fmt::format("On{}{}", sanitizedTitle, p.Name)},
				};
				wiredCallbacks.push_back(std::move(cb));
			} else {
				// Output data pin — likely a delegate-payload value
				// surfaced for downstream consumption. Capture name +
				// type so the agent can wire it into the matching
				// callback's signature.
				outputDataPins.push_back(nlohmann::json{
					{"name", p.Name},
					{"type", p.Type.Category},
				});
			}
		}

		fields["factory_function"] = factory;
		if (!targetClass.empty()) {
			fields["factory_class"] = targetClass;
		}
		if (!factoryArgs.empty()) {
			fields["factory_args"] = std::move(factoryArgs);
		}
		fields["wired_callbacks"]  = std::move(wiredCallbacks);
		fields["output_data_pins"] = std::move(outputDataPins);
		fields["cpp_recipe"] = nlohmann::json::array({
			"auto* Action = <FactoryClass>::<Factory>(<args>);",
			"Action-><Delegate>.AddDynamic(this, &ThisClass::<Callback>);",
			"Action->Activate();",
			"UFUNCTION() void <Callback>(<PayloadT> Payload) { ... }",
		});

		r.statement = nlohmann::json{{"unsupported", nlohmann::json{
			{"node_class", n.Class},
			{"guid", n.Id},
			{"reason",
			 "Async action (UBlueprintAsyncActionBase-style): each wired "
			 "exec output becomes a UFUNCTION callback bound via "
			 "AddDynamic, then Action->Activate(). See `fields` for "
			 "factory + per-output bindings + the canonical C++ recipe. "
			 "Full auto-lowering blocked on walker entry-pin tracking + "
			 "per-delegate param introspection."},
			{"fields", std::move(fields)},
		}}};
		r.terminatesExec = true;
		return r;
	}

	if (n.Class.find("K2Node_LoadAsset") != std::string::npos) {
		nlohmann::json fields = nlohmann::json::object();
		if (n.Meta.is_object())
		{
			fields = n.Meta;
		}
		if (const BPPin* ap = w.GetPin(n, "Asset")) {
			fields["asset_pin"] = BuildExpression(w, n, *ap);
		}
		bool isClass = n.Class.find("Class") != std::string::npos;
		r.statement = nlohmann::json{{"unsupported", nlohmann::json{
			{"node_class", n.Class},
			{"guid", n.Id},
			{"reason",
			 isClass
				? "LoadAssetClass — split into FStreamableManager async "
				  "load callback. UAssetManager::GetStreamableManager()"
				  ".RequestAsyncLoad(SoftClassPath, FStreamableDelegate)."
				: "LoadAsset — split into FStreamableManager async load "
				  "callback. UAssetManager::GetStreamableManager()"
				  ".RequestAsyncLoad(SoftObjectPath, FStreamableDelegate)."},
			{"fields", std::move(fields)},
		}}};
		r.terminatesExec = true;
		return r;
	}

	// MacroInstance — BP's looping constructs (ForEachLoop,
	// ForEachLoopWithBreak, ReverseForEachLoop, WhileLoop) are macro
	// instances. We pattern-match the macro path to lower them to
	// BPIR's native `for_each` / `while` forms. Unknown macros fall
	// through to the unsupported path.
	if (n.Class.find("K2Node_MacroInstance") != std::string::npos) {
		std::string macroPath;
		if (n.Meta.is_object()) {
			macroPath = n.Meta.value("macro_path", std::string{});
			if (macroPath.empty()) macroPath = n.Meta.value("MacroGraphReference", std::string{});
			if (macroPath.empty()) macroPath = n.Meta.value("macroName", std::string{});
		}
		// Bare macro name (after the last `:` or `.`).
		std::string bare = macroPath;
		if (auto pos = bare.find_last_of(":."); pos != std::string::npos) {
			bare = bare.substr(pos + 1);
		}

		// Gate — multi-input stateful macro (Enter / Open / Close /
		// Toggle inputs, Exit output). The auto-lowering pattern needs
		// the walker to know WHICH input pin the exec arrived at, but
		// the walker currently visits a node from a predecessor without
		// tracking the destination pin. Until the walker is refactored
		// to thread entry-pin info, Gate emits a structured sidecar
		// with the manual recipe instead of falling through to the
		// generic "unrecognized macro" path.
		auto isGate = (bare == "Gate");
		auto isForEach = (bare == "ForEachLoop" || bare == "ForEachLoopWithBreak");
		auto isReverseForEach = (bare == "ReverseForEachLoop");
		auto isWhile = (bare == "WhileLoop");
		auto isIsValid = (bare == "IsValid");

		// Stateful macros — BP's library encodes "do this once",
		// "alternate between A and B", "loop N times" as macro
		// instances backed by hidden state. They can't be inlined as
		// pure control flow; each needs a synthetic class-level
		// variable that survives across calls. We lower:
		//   DoOnce   -> bool flag, body fires once until reset
		//   FlipFlop -> bool flag, alternates A/B on each call
		//   DoN     -> int counter, body fires N times then stops
		// The synth-var name is derived from the node GUID so two
		// distinct DoOnce instances in the same class get separate
		// flags, but the same instance visited from multiple call
		// paths shares one flag (which matches BP semantics).
		auto isDoOnce  = (bare == "DoOnce");
		auto isFlipFlop = (bare == "FlipFlop");
		auto isDoN     = (bare == "DoN");

		if (isIsValid) {
			// IsValid macro pins: input InputObject, two exec outputs
			// (IsValid -> then branch, IsNotValid -> else branch). Lower
			// to a {if} statement form with the `IsValid(<input>)` call
			// as the condition. Most common BP macro by a wide margin;
			// without this lowering, every IsValid in a graph becomes a
			// sidecar TODO entry that breaks control flow.
			const BPPin* inputPin = w.GetPin(n, "InputObject");
			if (!inputPin)
			{
				inputPin = w.GetPin(n, "Object");  // alt name in some variants
			}
			nlohmann::json inputExpr = inputPin
				? BuildExpression(w, n, *inputPin)
				: nlohmann::json{{"self", nullptr}};

			nlohmann::json cond = {
				{"call", "IsValid"},
				{"args", nlohmann::json{{"Object", inputExpr}}},
			};

			const BPNode* thenStart = w.FollowExec(n, "IsValid");
			const BPNode* elseStart = w.FollowExec(n, "IsNotValid");
			std::set<std::string> vt, ve;
			nlohmann::json thenBody = DecompileStatementsFrom(w, thenStart, stopAt, vt);
			nlohmann::json elseBody = DecompileStatementsFrom(w, elseStart, stopAt, ve);

			nlohmann::json stmt = {{"if", std::move(cond)}};
			if (!thenBody.empty())
			{
				stmt["then"] = std::move(thenBody);
			}
			if (!elseBody.empty())
			{
				stmt["else"] = std::move(elseBody);
			}
			r.statement = std::move(stmt);
			// Both branches are walked into terminal positions; no
			// continuation past this node (IsValid's only exec outputs
			// are the two branches).
			r.terminatesExec = true;
			return r;
		}

		if (isForEach || isReverseForEach) {
			// ForEachLoop pins: input Array, output exec LoopBody +
			// Completed, output data ArrayElement + ArrayIndex.
			const BPPin* arrayPin = w.GetPin(n, "Array");
			nlohmann::json arrayExpr = arrayPin
				? BuildExpression(w, n, *arrayPin)
				: nlohmann::json{{"new_array", nlohmann::json::array()}};
			const BPNode* bodyStart = w.FollowExec(n, "LoopBody");
			if (!bodyStart)
			{
				bodyStart = w.FollowExec(n, "then");
			}
			std::set<std::string> v;
			// stopAt = &n: stop if we somehow recurse back to the loop
			// node (defensive; BP forEach bodies don't normally do that).
			nlohmann::json body = DecompileStatementsFrom(w, bodyStart, &n, v);
			nlohmann::json stmt = {
				{"for_each", "Element"},  // matches the ArrayElement pin name
				{"in",       arrayExpr},
				{"body",     std::move(body)},
			};
			if (isReverseForEach)
			{
				stmt["reverse"] = true;
			}
			r.statement = std::move(stmt);
			// Continue past the loop on the Completed exec.
			const BPNode* afterLoop = w.FollowExec(n, "Completed");
			r.next = afterLoop;
			return r;
		}

		if (isWhile) {
			const BPPin* condPin = w.GetPin(n, "Condition");
			nlohmann::json cond = condPin
				? BuildExpression(w, n, *condPin)
				: nlohmann::json{{"lit", true}};
			const BPNode* bodyStart = w.FollowExec(n, "LoopBody");
			if (!bodyStart)
			{
				bodyStart = w.FollowExec(n, "then");
			}
			std::set<std::string> v;
			nlohmann::json body = DecompileStatementsFrom(w, bodyStart, &n, v);
			r.statement = {{"while", cond}, {"body", std::move(body)}};
			r.next = w.FollowExec(n, "Completed");
			return r;
		}

		// Helper: derive a unique, identifier-safe tag for a stateful
		// macro instance from its node GUID. We use 8 hex chars so the
		// synth-var names stay readable (e.g. bBPRDoOnce_a1b2c3d4_HasFired)
		// while still being collision-free across a sane class size.
		auto tagFromGuid = [](const std::string& guid) {
			std::string out;
			out.reserve(8);
			for (char c : guid) {
				if (out.size() == 8)
				{
					break;
				}
				if ((c >= '0' && c <= '9') ||
					(c >= 'a' && c <= 'f') ||
					(c >= 'A' && c <= 'F'))
				{
					out.push_back(c);
				}
			}
			// Pad if a malformed/short GUID would otherwise produce
			// colliding tags. Unlikely but defensive.
			while (out.size() < 8)
			{
				out.push_back('0');
			}
			return out;
		};

		// Helper: register a synthetic class-level variable. Keyed by
		// name so re-registration from multiple visit paths is idempotent.
		auto registerSynthVar = [&w](const std::string& name,
									 const std::string& shorthandType,
									 const std::string& defaultValue) {
			if (w.autoSynthVars.count(name))
			{
				return;
			}
			nlohmann::json decl = {
				{"name", name},
				{"type", shorthandType},
				{"category", "BPR|AutoSynth"},
			};
			if (!defaultValue.empty())
			{
				decl["default"] = defaultValue;
			}
			w.autoSynthVars[name] = std::move(decl);
		};

		// DoOnce — semantics: the body after `Completed` runs the
		// first time the Start exec input fires, and not again until
		// Reset is pulsed. We lower the Start side to:
		//   if (!bBPRDoOnce_<tag>_HasFired) {
		//       bBPRDoOnce_<tag>_HasFired = true;
		//       <Completed body>
		//   }
		//
		// Multiple Start-side call paths in the same class share the
		// same flag — that matches BP's single-instance semantics
		// because the synth var name is derived from the macro node's
		// GUID, which is the same across all visit paths.
		//
		// Reset-side wiring isn't covered by this single visit: the
		// macro is visited once via the Start exec entry. If Reset
		// is also wired upstream, we surface a sidecar so the
		// transpile result flags the gap rather than silently dropping
		// the reset path. (Caller can manually add
		// `<flag> = false;` where the Reset exec line would have led.)
		if (isDoOnce) {
			const std::string tag = tagFromGuid(n.Id);
			const std::string flagName = fmt::format("bBPRDoOnce_{}_HasFired", tag);
			// bStartClosed input: if true, the macro "starts fired"
			// and Start does nothing until Reset. Read literal/default
			// off the input pin.
			bool startClosed = false;
			if (const BPPin* sc = w.GetPin(n, "StartClosed")) {
				if (sc->DefaultValue && *sc->DefaultValue == "true") {
					startClosed = true;
				}
			}
			registerSynthVar(flagName, "bool", startClosed ? "true" : "false");

			const BPNode* bodyStart = w.FollowExec(n, "Completed");
			std::set<std::string> v;
			nlohmann::json body = DecompileStatementsFrom(w, bodyStart, stopAt, v);

			// Build: if (!flag) { flag = true; <body> }
			// BPIR expression forms: variable read = {var, scope},
			// operators = {call: "KismetMathLibrary::<op>", args}.
			// CppEmit's OpReverseMap maps these back to the C++ operator.
			nlohmann::json setFlag = {
				{"set", flagName},
				{"scope", "member"},
				{"to",    nlohmann::json{{"lit", true}}},
			};
			nlohmann::json thenBlock = nlohmann::json::array();
			thenBlock.push_back(std::move(setFlag));
			if (body.is_array()) {
				for (auto& s : body)
				{
					thenBlock.push_back(std::move(s));
				}
			}
			nlohmann::json cond = {
				{"call", "KismetMathLibrary::Not_PreBool"},
				{"args", nlohmann::json{
					{"A", nlohmann::json{
						{"var", flagName},
						{"scope", "member"},
					}},
				}},
			};
			r.statement = {
				{"if",   std::move(cond)},
				{"then", std::move(thenBlock)},
			};

			// Sidecar if Reset is wired — agent-actionable note.
			if (const BPPin* rp = w.GetPin(n, "Reset")) {
				auto eIt = w.inEdges.find({n.Id, rp->Id});
				if (eIt != w.inEdges.end() && !eIt->second.empty()) {
					nlohmann::json note = {
						{"unsupported", nlohmann::json{
							{"node_class", n.Class},
							{"guid", n.Id},
							{"reason", fmt::format(
								"DoOnce Reset pin has incoming edge(s); "
								"synth flag '{}' isn't reset on that path. "
								"Add `{} = false;` at the upstream Reset call "
								"site to restore one-shot behavior.",
								flagName, flagName)},
						}},
					};
					// Attach as a peer statement before the if. Walker's
					// top-level collectUnsupported pass will pick it up.
					r.preStatements.push_back(std::move(note));
				}
			}
			r.terminatesExec = true;
			return r;
		}

		// FlipFlop — alternates between A and B exec outputs on each
		// call. Single exec input (Input), two outputs (A, B), plus
		// a data output IsA (bool, true when next call goes to A).
		// Lower to:
		//   if (bBPRFlipFlop_<tag>_IsA) {
		//       bBPRFlipFlop_<tag>_IsA = false;
		//       <A body>
		//   } else {
		//       bBPRFlipFlop_<tag>_IsA = true;
		//       <B body>
		//   }
		//
		// The flag is initialized to true to match BP behavior
		// (first call routes to A).
		if (isFlipFlop) {
			const std::string tag = tagFromGuid(n.Id);
			const std::string flagName = fmt::format("bBPRFlipFlop_{}_IsA", tag);
			registerSynthVar(flagName, "bool", "true");

			const BPNode* aStart = w.FollowExec(n, "A");
			const BPNode* bStart = w.FollowExec(n, "B");
			std::set<std::string> va, vb;
			nlohmann::json aBody = DecompileStatementsFrom(w, aStart, stopAt, va);
			nlohmann::json bBody = DecompileStatementsFrom(w, bStart, stopAt, vb);

			// Prepend the flag-flip to each branch so the *next* call
			// routes to the opposite side.
			auto prependFlip = [&](nlohmann::json& body, bool toValue) {
				nlohmann::json setFlag = {
					{"set", flagName},
					{"scope", "member"},
					{"to",    nlohmann::json{{"lit", toValue}}},
				};
				nlohmann::json out = nlohmann::json::array();
				out.push_back(std::move(setFlag));
				if (body.is_array()) {
					for (auto& s : body)
					{
						out.push_back(std::move(s));
					}
				}
				body = std::move(out);
			};
			prependFlip(aBody, false);
			prependFlip(bBody, true);

			// Condition is a plain bool read.
			nlohmann::json cond = {
				{"var", flagName},
				{"scope", "member"},
			};
			r.statement = {
				{"if",   std::move(cond)},
				{"then", std::move(aBody)},
				{"else", std::move(bBody)},
			};
			r.terminatesExec = true;
			return r;
		}

		// DoN — body runs at most N times across all Enter exec calls
		// (Reset zeroes the counter). N comes from the N input pin,
		// either as a literal default or a wired upstream value.
		//
		// Lower to:
		//   if (BPRDoN_<tag>_Counter < <N expr>) {
		//       ++BPRDoN_<tag>_Counter;
		//       <Exit body>
		//   }
		//
		// We pull N from the input pin's default value when possible;
		// if it's wired up (rare for DoN's N pin) we fall back to a
		// captured-expression form via __bpr_doN_n placeholder so the
		// codegen can render the upstream expression.
		if (isDoN) {
			const std::string tag = tagFromGuid(n.Id);
			const std::string counterName = fmt::format("BPRDoN_{}_Counter", tag);
			registerSynthVar(counterName, "int", "0");

			// N expression. Prefer wired expression; fall back to
			// pin default.
			nlohmann::json nExpr;
			if (const BPPin* np = w.GetPin(n, "N")) {
				nExpr = BuildExpression(w, n, *np);
			} else {
				nExpr = nlohmann::json{{"lit", 0}};
			}

			const BPNode* bodyStart = w.FollowExec(n, "Exit");
			std::set<std::string> v;
			nlohmann::json body = DecompileStatementsFrom(w, bodyStart, stopAt, v);

			// ++counter — emit as set-with-Add expression because BPIR
			// doesn't have a unary-increment statement form. Lowered
			// via the canonical UKismetMathLibrary::Add_IntInt operator
			// call which CppEmit reverses to `lhs + rhs`.
			nlohmann::json incrCounter = {
				{"set", counterName},
				{"scope", "member"},
				{"to", nlohmann::json{
					{"call", "KismetMathLibrary::Add_IntInt"},
					{"args", nlohmann::json{
						{"A", nlohmann::json{
							{"var", counterName},
							{"scope", "member"},
						}},
						{"B", nlohmann::json{{"lit", 1}}},
					}},
				}},
			};
			nlohmann::json thenBlock = nlohmann::json::array();
			thenBlock.push_back(std::move(incrCounter));
			if (body.is_array()) {
				for (auto& s : body)
				{
					thenBlock.push_back(std::move(s));
				}
			}
			nlohmann::json cond = {
				{"call", "KismetMathLibrary::Less_IntInt"},
				{"args", nlohmann::json{
					{"A", nlohmann::json{
						{"var", counterName},
						{"scope", "member"},
					}},
					{"B", std::move(nExpr)},
				}},
			};
			r.statement = {
				{"if",   std::move(cond)},
				{"then", std::move(thenBlock)},
			};

			// Reset-pin sidecar — same pattern as DoOnce.
			if (const BPPin* rp = w.GetPin(n, "Reset")) {
				auto eIt = w.inEdges.find({n.Id, rp->Id});
				if (eIt != w.inEdges.end() && !eIt->second.empty()) {
					nlohmann::json note = {
						{"unsupported", nlohmann::json{
							{"node_class", n.Class},
							{"guid", n.Id},
							{"reason", fmt::format(
								"DoN Reset pin has incoming edge(s); "
								"synth counter '{}' isn't reset on that path. "
								"Add `{} = 0;` at the upstream Reset call site.",
								counterName, counterName)},
						}},
					};
					r.preStatements.push_back(std::move(note));
				}
			}
			r.terminatesExec = true;
			return r;
		}

		// Gate — structured sidecar with the manual recipe. We can't
		// auto-lower today because the walker doesn't track which
		// input pin (Enter / Open / Close / Toggle) the exec arrived
		// at. But the sidecar gives the agent everything they need to
		// hand-translate at the upstream call site:
		//   * Synth `bool` member to declare.
		//   * The 4 patterns: Enter (if-then), Open (set true),
		//     Close (set false), Toggle (negate).
		// When walker entry-pin tracking lands (separate refactor),
		// this handler can be replaced with the auto-lowering version.
		if (isGate) {
			std::string tag;
			tag.reserve(8);
			for (char c : n.Id) {
				if (tag.size() == 8) {
					break;
				}
				if ((c >= '0' && c <= '9') ||
					(c >= 'a' && c <= 'f') ||
					(c >= 'A' && c <= 'F')) {
					tag.push_back(c);
				}
			}
			while (tag.size() < 8) {
				tag.push_back('0');
			}
			const std::string flagName = fmt::format("bBPRGate_{}_IsOpen", tag);

			// Honor bStartClosed when present (true → init flag false).
			bool startClosed = false;
			if (const BPPin* sc = w.GetPin(n, "bStartClosed")) {
				if (sc->DefaultValue && *sc->DefaultValue == "true") {
					startClosed = true;
				}
			}
			const std::string initVal = startClosed ? "false" : "true";

			// Identify which input pins are actually wired, so the
			// sidecar only describes patterns the agent needs.
			std::vector<std::string> wiredInputs;
			for (const char* pinName : {"Enter", "Open", "Close", "Toggle"}) {
				if (const BPPin* p = w.GetPin(n, pinName)) {
					auto it = w.inEdges.find({n.Id, p->Id});
					if (it != w.inEdges.end() && !it->second.empty()) {
						wiredInputs.emplace_back(pinName);
					}
				}
			}

			nlohmann::json fields = nlohmann::json::object();
			if (n.Meta.is_object()) {
				fields = n.Meta;
			}
			fields["synth_member"] = fmt::format("bool {} = {};", flagName, initVal);
			fields["wired_inputs"] = wiredInputs;
			fields["pattern_enter"]  = fmt::format("if ({}) {{ <Exit body> }}", flagName);
			fields["pattern_open"]   = fmt::format("{} = true;", flagName);
			fields["pattern_close"]  = fmt::format("{} = false;", flagName);
			fields["pattern_toggle"] = fmt::format("{} = !{};", flagName, flagName);

			r.statement = nlohmann::json{{"unsupported", nlohmann::json{
				{"node_class", n.Class},
				{"guid", n.Id},
				{"reason",
				 "Gate macro: multi-input stateful (Enter / Open / Close / "
				 "Toggle pins). Walker doesn't track which input pin "
				 "received the exec, so auto-lowering can't dispatch "
				 "correctly. The synth `bool` member and per-pin "
				 "C++ patterns are in the sidecar — apply them at the "
				 "upstream call site that connects to each Gate input. "
				 "(Tracked: walker entry-pin refactor unlocks the full "
				 "auto-lowering.)"},
				{"macro_path", macroPath},
				{"fields", std::move(fields)},
			}}};
			// Continue past the Gate's `Exit` output if anything is
			// wired there — typical placement is upstream-of-Gate.Enter
			// feeds Gate, downstream-of-Gate.Exit is the conditional
			// payload. Walking Exit means the agent's manual code
			// (the `if (flag) { ... }`) wraps that payload.
			r.next = w.FollowExec(n, "Exit");
			return r;
		}

		// Unrecognized macro — fall through to the generic unsupported
		// path with the macro_path captured so the sidecar is useful.
		nlohmann::json fields = nlohmann::json::object();
		if (n.Meta.is_object())
		{
			fields = n.Meta;
		}
		r.statement = nlohmann::json{{"unsupported", nlohmann::json{
			{"node_class", n.Class},
			{"guid", n.Id},
			{"reason", fmt::format("Unrecognized BP macro '{}'", bare)},
			{"macro_path", macroPath},
			{"fields", std::move(fields)},
		}}};
		r.next = w.FollowExec(n, "then");
		return r;
	}

	// DestroyActor — common BP node, maps cleanly to `Target->Destroy()`.
	// The Target pin is optional; absent = self. We carry the target
	// expression in args["Target"] for CppEmit to render.
	if (n.Class.find("K2Node_DestroyActor") != std::string::npos) {
		nlohmann::json args = nlohmann::json::object();
		if (const BPPin* tp = w.GetPin(n, "Target")) {
			args["Target"] = BuildExpression(w, n, *tp);
		}
		nlohmann::json stmt = {{"call", "__bpr_destroy_actor"}};
		if (!args.empty())
		{
			stmt["args"] = std::move(args);
		}
		r.statement = std::move(stmt);
		r.next = w.FollowExec(n, "then");
		return r;
	}

	// Knot (reroute) — pure passthrough; emit nothing, follow the exec
	// out. Knots also appear in data flow where BuildExpression handles
	// them; here we only see them on exec paths (rare but possible).
	if (n.Class.find("K2Node_Knot") != std::string::npos) {
		r.next = w.FollowExec(n, "then");
		r.statement = nlohmann::json::object();  // skipped by caller
		return r;
	}

	// Switch (K2Node_SwitchEnum / SwitchInteger / SwitchString / SwitchName).
	// Each case is a named exec output pin; we emit `{switch, cases, default}`
	// which CppEmit's `switch` form already renders.
	if (n.Class.find("K2Node_Switch") != std::string::npos) {
		// Selector input pin — by convention "Selection" or
		// "TargetExpression" depending on subclass. Try both.
		const BPPin* selPin = w.GetPin(n, "Selection");
		if (!selPin)
		{
			selPin = w.GetPin(n, "TargetExpression");
		}
		if (!selPin) {
			// Walk pins for the first non-exec input as a fallback.
			for (const auto& p : n.Pins) {
				if (p.Direction == "Input" && p.Type.Category != "exec") {
					selPin = &p;
					break;
				}
			}
		}
		nlohmann::json switchExpr = selPin
			? BuildExpression(w, n, *selPin)
			: nlohmann::json{{"lit", 0}};

		nlohmann::json cases = nlohmann::json::object();
		nlohmann::json defaultBody;
		bool hasDefault = false;
		for (const auto& p : n.Pins) {
			if (p.Direction != "Output" || p.Type.Category != "exec")
			{
				continue;
			}
			if (p.Name == "Default") {
				const BPNode* defStart = w.FollowExec(n, "Default");
				std::set<std::string> v;
				defaultBody = DecompileStatementsFrom(w, defStart, stopAt, v);
				hasDefault = true;
			} else {
				const BPNode* caseStart = w.FollowExec(n, p.Name);
				std::set<std::string> v;
				cases[p.Name] = DecompileStatementsFrom(w, caseStart, stopAt, v);
			}
		}
		nlohmann::json stmt = {{"switch", switchExpr}, {"cases", cases}};
		if (hasDefault)
		{
			stmt["default"] = defaultBody;
		}
		r.statement = std::move(stmt);
		r.terminatesExec = true;
		return r;
	}

	// AddComponent — same shape as SpawnActorFromClass. Carries the
	// template type + transform; CppEmit emits a NewObject +
	// RegisterComponent + AttachToComponent block.
	if (n.Class.find("K2Node_AddComponent") != std::string::npos) {
		nlohmann::json args = nlohmann::json::object();
		for (const auto& p : n.Pins) {
			if (p.Direction != "Input" || p.Type.Category == "exec")
			{
				continue;
			}
			args[p.Name] = BuildExpression(w, n, p);
		}
		nlohmann::json stmt = {{"call", "__bpr_add_component"}};
		if (!args.empty())
		{
			stmt["args"] = std::move(args);
		}
		r.statement = std::move(stmt);
		r.next = w.FollowExec(n, "then");
		return r;
	}

	// Delegate ops — Call/Add/Remove/Clear, all deriving from
	// UK2Node_BaseMCDelegate. The plugin introspector surfaces
	// `delegateProperty` (and `delegateClass`) in Meta via the
	// DelegateOp Extras handler; we use the K2 class to pick which
	// BPIR statement form to emit.
	//
	//   CallDelegate   -> {broadcast: <prop>, [target], [args]}
	//   AddDelegate    -> {bind_delegate: <prop>, [target], handler: <fn>}
	//   RemoveDelegate -> {unbind_delegate: <prop>, [target], handler: <fn>}
	//   ClearDelegate  -> {clear_delegate: <prop>, [target]}
	//
	// target defaults to `self` if the "self" input pin isn't connected.
	{
		const bool isCall   = n.Class.find("K2Node_CallDelegate")   != std::string::npos;
		const bool isAdd    = n.Class.find("K2Node_AddDelegate")    != std::string::npos;
		const bool isRemove = n.Class.find("K2Node_RemoveDelegate") != std::string::npos;
		const bool isClear  = n.Class.find("K2Node_ClearDelegate")  != std::string::npos;
		if (isCall || isAdd || isRemove || isClear) {
			std::string prop;
			if (n.Meta.is_object()) prop = n.Meta.value("delegateProperty", std::string{});
			// If introspector meta is missing (older plugin builds), fall
			// through to the generic unsupported path with the node class
			// captured so the sidecar is still actionable.
			if (prop.empty()) {
				nlohmann::json fields = nlohmann::json::object();
				if (n.Meta.is_object())
				{
					fields = n.Meta;
				}
				r.statement = nlohmann::json{{"unsupported", nlohmann::json{
					{"node_class", n.Class},
					{"guid", n.Id},
					{"reason",
					 "Delegate op missing 'delegateProperty' in meta. "
					 "Plugin introspector may predate BaseMCDelegate "
					 "support; rebuild the editor module."},
					{"fields", std::move(fields)},
				}}};
				r.next = w.FollowExec(n, "then");
				return r;
			}

			// Target = `self` input if connected, else the implicit self.
			// Connectedness is determined by inEdges (BPPin::Links isn't
			// populated by every backend; inEdges is the canonical map).
			nlohmann::json target;
			if (const BPPin* sp = w.GetPin(n, "self")) {
				auto eIt = w.inEdges.find({n.Id, sp->Id});
				if (eIt != w.inEdges.end() && !eIt->second.empty()) {
					target = BuildExpression(w, n, *sp);
				}
			}

			if (isCall) {
				nlohmann::json args = nlohmann::json::object();
				for (const auto& p : n.Pins) {
					if (p.Direction != "Input" || p.Type.Category == "exec")
					{
						continue;
					}
					if (p.Name == "self")
					{
						continue;
					}
					args[p.Name] = BuildExpression(w, n, p);
				}
				nlohmann::json stmt = {{"broadcast", prop}};
				if (!target.is_null())
				{
					stmt["target"] = std::move(target);
				}
				if (!args.empty())
				{
					stmt["args"] = std::move(args);
				}
				r.statement = std::move(stmt);
			} else if (isClear) {
				nlohmann::json stmt = {{"clear_delegate", prop}};
				if (!target.is_null())
				{
					stmt["target"] = std::move(target);
				}
				r.statement = std::move(stmt);
			} else {
				// Add or Remove. The bound function name comes from the
				// K2Node_CreateDelegate node connected to the "Delegate"
				// input pin -- that node's meta carries `delegateName`.
				std::string handler;
				if (const BPPin* dp = w.GetPin(n, "Delegate")) {
					auto eIt = w.inEdges.find({n.Id, dp->Id});
					if (eIt != w.inEdges.end() && !eIt->second.empty()) {
						if (const BPNode* maker = w.GetNode(eIt->second.front().node)) {
							if (maker->Meta.is_object()) {
								handler = maker->Meta.value("delegateName", std::string{});
							}
						}
					}
				}
				nlohmann::json stmt = {
					{isAdd ? "bind_delegate" : "unbind_delegate", prop},
				};
				if (!target.is_null())
				{
					stmt["target"] = std::move(target);
				}
				stmt["handler"] = handler;  // empty -> caller surfaces a sidecar note
				r.statement = std::move(stmt);
			}
			r.next = w.FollowExec(n, "then");
			return r;
		}
	}

	// CallFunction (statement form — return value, if any, is unused).
	if (n.Class.find("K2Node_CallFunction") != std::string::npos) {
		std::string fnName, ownerCl;
		if (n.Meta.is_object()) {
			fnName  = n.Meta.value("targetFunction", std::string{});
			ownerCl = n.Meta.value("targetClass",    std::string{});
		}
		std::string fqName = ownerCl.empty() ? fnName : (ownerCl + "::" + fnName);

		// Latent action lowering — Delay (and its kin) can't be inlined
		// as a single C++ statement because the post-delay exec must
		// run in a later stack frame. Approach:
		//   1. The pre-delay portion of the current function ends with a
		//      SetTimer call that schedules a continuation method.
		//   2. The post-delay exec flow is captured into a generated
		//      class-level UFUNCTION (the "continuation") which the timer
		//      callback invokes.
		//   3. A synthetic FTimerHandle member variable is added so the
		//      timer can be canceled / queried if needed.
		//
		// This solves the "continuation-passing" structural gap by
		// generating the missing artifact (a real callable method)
		// instead of leaving a TODO for the agent. Multiple delays in
		// sequence work naturally — the continuation's walk is just
		// another DecompileStatementsFrom call against the same Walker,
		// so nested delays produce nested continuations.
		//
		// Common latent calls: KismetSystemLibrary::Delay,
		// RetriggerableDelay, DelayUntilNextTick.
		if (fnName == "Delay" || fnName == "RetriggerableDelay" ||
			fnName == "DelayUntilNextTick") {
			// 1) Identifier-safe tag from the node GUID.
			std::string tag;
			tag.reserve(8);
			for (char c : n.Id) {
				if (tag.size() == 8)
				{
					break;
				}
				if ((c >= '0' && c <= '9') ||
					(c >= 'a' && c <= 'f') ||
					(c >= 'A' && c <= 'F'))
				{
					tag.push_back(c);
				}
			}
			while (tag.size() < 8)
			{
				tag.push_back('0');
			}

			// 2) Names. Parent function name prefixes both so two delays
			//    in different parents don't collide.
			const std::string parentName = w.function.Name;
			const std::string contName    = fmt::format("{}_DelayCont_{}", parentName, tag);
			const std::string handleName  = fmt::format("{}_DelayHandle_{}", parentName, tag);

			// 3) Synth FTimerHandle member. Emitted via the same
			//    auto_synth_vars channel the stateful-macro lowering
			//    uses, so DecompileBlueprint hoists it into class
			//    variables[].
			if (!w.autoSynthVars.count(handleName)) {
				nlohmann::json handleDecl = {
					{"name", handleName},
					{"type", "struct:TimerHandle"},
					{"category", "BPR|AutoSynth"},
				};
				w.autoSynthVars[handleName] = std::move(handleDecl);
			}

			// 4) Build the continuation body by walking the post-delay
			//    exec. Delay's exec output is named "Completed" on
			//    latent UFUNCTIONs (UE convention); fall back to "then"
			//    just in case the introspector serializes it differently.
			const BPNode* contStart = w.FollowExec(n, "Completed");
			if (!contStart)
			{
				contStart = w.FollowExec(n, "then");
			}
			std::set<std::string> contVisited;
			nlohmann::json contBody = DecompileStatementsFrom(
				w, contStart, /*stopAt=*/nullptr, contVisited);

			// 5) Synth continuation function doc. Bare UFUNCTION() is
			//    sufficient — timer callbacks are invoked via
			//    reflection, not BP-exposed (callers don't need
			//    BlueprintCallable). Empty ufunction_specifiers signals
			//    BuildUFunctionList to skip its BlueprintCallable
			//    default.
			nlohmann::json contMetadata = {
				{"asset_path", w.function.Name},  // placeholder; corrected below
				{"ufunction_specifiers", nlohmann::json::array()},
				{"generated_by", "bpr-delay-continuation"},
				{"source_node_guid", n.Id},
			};
			// Forward the parent's asset_path if available so downstream
			// tooling can locate the synthetic function in context.
			// (DecompileFunction sets the doc's metadata.asset_path post
			// hoc; we can't read it from here, so leave a placeholder.)
			nlohmann::json contDoc = {
				{"version", kBpirSchemaVersion},
				{"kind",    "function"},
				{"name",    contName},
				{"metadata", std::move(contMetadata)},
				{"inputs",  nlohmann::json::array()},
				{"outputs", nlohmann::json::array()},
				{"locals",  nlohmann::json::array()},
				{"body",    std::move(contBody)},
			};
			w.autoSynthFunctions.push_back(std::move(contDoc));

			// 6) Emit the SetTimer call as the pre-delay statement and
			//    terminate the current exec flow. CppEmit's
			//    __bpr_set_timer handler will render this as
			//    `GetWorld()->GetTimerManager().SetTimer(...)` with the
			//    right argument ordering.
			nlohmann::json args = nlohmann::json::object();
			// Duration: from the pin, or 0 (DelayUntilNextTick).
			if (const BPPin* dp = w.GetPin(n, "Duration")) {
				args["Duration"] = BuildExpression(w, n, *dp);
			} else {
				args["Duration"] = nlohmann::json{{"lit", 0.0}};
			}
			// Handle + callback: literal strings; CppEmit's sentinel
			// handler unwraps them as bare identifiers.
			args["Handle"]   = nlohmann::json{{"lit", handleName}};
			args["Callback"] = nlohmann::json{{"lit", contName}};
			// Looping flag — RetriggerableDelay reuses the same handle
			// semantically but the UE-side overload still wants a bool;
			// we set false here because BP's Delay-family is one-shot,
			// and RetriggerableDelay's "retrigger" semantics need
			// explicit cancel-and-rebind which the agent handles.
			args["Looping"] = nlohmann::json{{"lit", false}};
			args["Kind"]    = nlohmann::json{{"lit", fnName}};  // for diagnostics

			r.statement = nlohmann::json{
				{"call", "__bpr_set_timer"},
				{"args", std::move(args)},
			};
			// Post-Delay code lives in the continuation now; the parent
			// function's exec terminates here.
			r.terminatesExec = true;
			return r;
		}

		nlohmann::json args = nlohmann::json::object();
		for (const auto& p : n.Pins) {
			if (p.Direction != "Input" || p.Type.Category == "exec")
			{
				continue;
			}
			if (p.Name == "self")
			{
				continue;
			}
			args[p.Name] = BuildExpression(w, n, p);
		}
		nlohmann::json stmt = {{"call", fqName}};
		if (!args.empty())
		{
			stmt["args"] = std::move(args);
		}
		r.statement = std::move(stmt);
		r.next = w.FollowExec(n, "then");
		return r;
	}

	// Comment: a node-attached comment; emit if non-empty.
	if (n.Comment && !n.Comment->empty()) {
		r.statement = nlohmann::json{{"comment", *n.Comment}};
		r.next = w.FollowExec(n, "then");
		return r;
	}

	// Unrecognized — emit `{unsupported}` carrying the node info.
	nlohmann::json fields = nlohmann::json::object();
	if (n.Meta.is_object())
	{
		fields = n.Meta;
	}
	r.statement = nlohmann::json{{"unsupported", nlohmann::json{
		{"node_class", n.Class},
		{"guid", n.Id},
		{"reason", "no decompile pattern matched"},
		{"fields", std::move(fields)},
	}}};
	r.next = w.FollowExec(n, "then");
	return r;
}

// ----- Top-level orchestration ------------------------------------------

const BPNode* FindFunctionEntry(const BPGraph& g) {
	for (const auto& n : g.Nodes) {
		if (n.Class.find("K2Node_FunctionEntry") != std::string::npos)
		{
			return &n;
		}
		if (n.Class.find("K2Node_Event") != std::string::npos)
		{
			return &n;
		}
	}
	// Fallback: first node with an outgoing exec edge.
	for (const auto& n : g.Nodes) {
		for (const auto& p : n.Pins) {
			if (p.Direction == "Output" && p.Type.Category == "exec")
			{
				return &n;
			}
		}
	}
	return nullptr;
}

// ----- EnhancedInput auto-lowering ---------------------------------------
// Intent: in UE, the K2Node_EnhancedInputAction event node in an event
// graph binds a UInputAction asset to a chain of BP nodes that runs
// when the input fires. The C++ idiom is fundamentally different —
// inputs are bound centrally in `SetupPlayerInputComponent` and each
// trigger event becomes its own UFUNCTION callback.
//
// This pass scans event graphs for input action nodes, generates the
// per-trigger callbacks, registers the UInputAction* member, and
// synthesizes a SetupPlayerInputComponent override that wires
// everything together. Output is appended to the class's variables[]
// and functions[] arrays so downstream codegen treats it identically
// to BP-authored content.
//
// Per-class aggregation: bindings collected across ALL event graphs
// are routed into a single SetupPlayerInputComponent override (one per
// class is the UE rule). Multiple input action nodes referencing the
// same asset share one UInputAction* member.
//
// Returns true if any bindings were generated (caller can use this
// to bump test expectations).
void ProcessEnhancedInputBindings(
	backends::IBlueprintReader& reader,
	std::string_view assetPath,
	const BPMetadata& meta,
	const std::set<std::string>& memberVarNames,
	nlohmann::json& variables,
	nlohmann::json& functions) {
	// Aggregation state.
	struct Binding {
		std::string actionMember;
		std::string trigger;       // "Started" / "Triggered" / "Ongoing" / "Canceled" / "Completed"
		std::string callbackName;
	};
	std::vector<Binding> bindings;
	std::map<std::string, std::string> actionPathToMember;  // dedup
	// Mirror the class-level dedup of variables already on the class.
	std::set<std::string> alreadyHaveVarName;
	for (const auto& v : variables) {
		if (v.is_object() && v.contains("name") && v["name"].is_string()) {
			alreadyHaveVarName.insert(v["name"].get<std::string>());
		}
	}

	auto deriveActionName = [](const std::string& path) {
		std::string name = path;
		if (auto slash = name.find_last_of('/'); slash != std::string::npos) {
			name = name.substr(slash + 1);
		}
		if (auto dot = name.find('.'); dot != std::string::npos) {
			name = name.substr(0, dot);
		}
		// Strip trailing _C if a class path snuck through.
		if (name.size() > 2 && name.substr(name.size() - 2) == "_C") {
			name = name.substr(0, name.size() - 2);
		}
		// Sanitize for C++ identifier rules.
		std::string out;
		for (char c : name) {
			if ((c >= '0' && c <= '9') ||
				(c >= 'a' && c <= 'z') ||
				(c >= 'A' && c <= 'Z') ||
				c == '_')
			{
				out.push_back(c);
			}
		}
		if (out.empty())
		{
			out = "Unknown";
		}
		if (out.front() >= '0' && out.front() <= '9')
		{
			out.insert(out.begin(), '_');
		}
		return out;
	};

	for (const auto& gsum : meta.Graphs) {
		if (gsum.Type != "EventGraph")
		{
			continue;
		}
		BPGraph graph;
		try {
			graph = reader.GetGraph(assetPath, gsum.Name);
		} catch (...) {
			continue;
		}

		// Dummy BPFunction for the Walker — event graphs don't have
		// local var scope, so all variable references are members (or
		// unscoped). Walker's ScopeForVariable returns "" for unknown
		// names which is fine for input-event walks (no locals to
		// collide with).
		BPFunction dummyFn;
		dummyFn.Name = gsum.Name;
		Walker walker(graph, dummyFn, memberVarNames);

		for (const BPNode& n : graph.Nodes) {
			if (n.Class.find("K2Node_EnhancedInputAction") == std::string::npos)
			{
				continue;
			}

			// Locate the action asset. The plugin introspector writes
			// this under a few possible keys depending on version.
			std::string actionPath;
			if (n.Meta.is_object()) {
				actionPath = n.Meta.value("input_action", std::string{});
				if (actionPath.empty()) actionPath = n.Meta.value("inputAction", std::string{});
				if (actionPath.empty()) actionPath = n.Meta.value("action_path", std::string{});
				if (actionPath.empty()) actionPath = n.Meta.value("action", std::string{});
			}
			// Normalize: strip a leading IA_/UA_ prefix so downstream
			// names don't double up (avoid `OnIA_IA_Jump_Started` from
			// an asset already named `IA_Jump`). The bare name is used
			// in callback identifiers; the member is always re-prefixed
			// with IA_ for clarity at the class API surface.
			const std::string rawActionName = deriveActionName(actionPath);
			std::string actionName = rawActionName;
			if (actionName.rfind("IA_", 0) == 0)
			{
				actionName = actionName.substr(3);
			}
			else if (actionName.rfind("UA_", 0) == 0) actionName = actionName.substr(3);
			if (actionName.empty())
			{
				actionName = "Unknown";
			}

			const std::string memberName = std::string("IA_") + actionName;

			// Register the member (idempotent across multiple input
			// nodes referencing the same action). memberName is derived
			// deterministically from actionPath above, so seeing the
			// same action twice just skips the duplicate decl.
			if (!actionPathToMember.count(actionPath) && !alreadyHaveVarName.count(memberName)) {
				actionPathToMember[actionPath] = memberName;
				alreadyHaveVarName.insert(memberName);
				nlohmann::json varDecl = {
					{"name", memberName},
					{"type", "object:InputAction"},
					{"editable", true},
					{"category", "Input"},
				};
				variables.push_back(std::move(varDecl));
			}

			// Walk each wired output exec pin → generate one callback
			// UFUNCTION per trigger event.
			for (const BPPin& pin : n.Pins) {
				if (pin.Direction != "Output" || pin.Type.Category != "exec")
				{
					continue;
				}
				const std::string& trigger = pin.Name;
				auto outIt = walker.outEdges.find({n.Id, pin.Id});
				if (outIt == walker.outEdges.end() || outIt->second.empty())
				{
					continue;
				}

				// Callback name: pure CamelCase, no underscores. The
				// `IA_` prefix is already on the member variable
				// (`IA_<Action>`); duplicating it on the callback was
				// noise. UE convention: `OnJumpStarted`,
				// `OnFireTriggered`, etc.
				const std::string callbackName = fmt::format("On{}{}", actionName, trigger);

				// Build the callback body by walking the post-event exec.
				const BPNode* bodyStart = walker.GetNode(outIt->second.front().node);
				std::set<std::string> visited;
				nlohmann::json cbBody = DecompileStatementsFrom(walker, bodyStart, nullptr, visited);

				// Flush any auto-synth state the body walk produced
				// (e.g. a DoOnce or Delay inside the callback) into the
				// class. They aren't tied to the callback specifically.
				for (auto& [name, decl] : walker.autoSynthVars) {
					if (!alreadyHaveVarName.count(name)) {
						alreadyHaveVarName.insert(name);
						variables.push_back(decl);
					}
				}
				walker.autoSynthVars.clear();
				for (auto& contFn : walker.autoSynthFunctions) {
					functions.push_back(std::move(contFn));
				}
				walker.autoSynthFunctions.clear();

				// Synthesize the callback function doc. Takes the input
				// value as a parameter so the agent can read action
				// value / axis data; mirrors UE's EnhancedInput delegate
				// signature `void(const FInputActionValue&)`. We emit
				// it as by-value `FInputActionValue Value` for now —
				// it's a small struct (cheap to copy) and the agent
				// can switch to const-ref if they want.
				nlohmann::json cbDoc = {
					{"version", kBpirSchemaVersion},
					{"kind",    "function"},
					{"name",    callbackName},
					{"metadata", nlohmann::json{
						{"asset_path", std::string(assetPath)},
						// No specifiers → bare UFUNCTION(). EnhancedInput
						// dispatch uses member-pointer binding, so the
						// function doesn't need BP exposure.
						{"ufunction_specifiers", nlohmann::json::array()},
						{"generated_by", "bpr-enhanced-input"},
						{"source_node_guid", n.Id},
					}},
					{"inputs", nlohmann::json::array({
						nlohmann::json{
							{"name", "Value"},
							{"type", "struct:InputActionValue"},
						}
					})},
					{"outputs", nlohmann::json::array()},
					{"locals",  nlohmann::json::array()},
					{"body",    std::move(cbBody)},
				};
				functions.push_back(std::move(cbDoc));

				bindings.push_back({memberName, trigger, callbackName});
			}
		}
	}

	if (bindings.empty())
	{
		return;  // No input bindings; nothing more to do.
	}

	// Synthesize the SetupPlayerInputComponent override. Body:
	//   Super::SetupPlayerInputComponent(PlayerInputComponent);
	//   if (UEnhancedInputComponent* EIC = Cast<UEnhancedInputComponent>(PlayerInputComponent)) {
	//       EIC->BindAction(<Action>, ETriggerEvent::<Trigger>, this, &ThisClass::<Cb>);
	//       ...
	//   }
	nlohmann::json setupBody = nlohmann::json::array();

	setupBody.push_back(nlohmann::json{
		{"call", "Super::SetupPlayerInputComponent"},
		{"args", nlohmann::json{
			{"PlayerInputComponent", nlohmann::json{
				{"var", "PlayerInputComponent"},
				{"scope", "input"},
			}},
		}},
	});

	nlohmann::json castSuccess = nlohmann::json::array();
	for (const auto& b : bindings) {
		castSuccess.push_back(nlohmann::json{
			{"call", "__bpr_bind_input_action"},
			{"args", nlohmann::json{
				{"Action",   nlohmann::json{{"var", b.actionMember}, {"scope", "member"}}},
				{"Trigger",  nlohmann::json{{"lit", b.trigger}}},
				{"Callback", nlohmann::json{{"lit", b.callbackName}}},
			}},
		});
	}

	setupBody.push_back(nlohmann::json{
		{"cast", nlohmann::json{
			{"var", "PlayerInputComponent"},
			{"scope", "input"},
		}},
		// Prefixed name so the renderer's Cast<{}>() lands on the right
		// C++ class. ResolveAssetPath in CppEmit doesn't add the U
		// prefix automatically for bare identifiers.
		{"to", "UEnhancedInputComponent"},
		{"as", "EIC"},
		{"success", std::move(castSuccess)},
		{"fail", nlohmann::json::array()},
	});

	nlohmann::json setupFn = {
		{"version", kBpirSchemaVersion},
		{"kind",    "function"},
		{"name",    "SetupPlayerInputComponent"},
		{"metadata", nlohmann::json{
			{"asset_path", std::string(assetPath)},
			// Empty specifiers + the whitelisted virtual name causes
			// CppClassEmit to emit it as `virtual void
			// SetupPlayerInputComponent(UInputComponent*) override;`
			// without a UFUNCTION decoration.
			{"ufunction_specifiers", nlohmann::json::array()},
			{"generated_by", "bpr-enhanced-input"},
		}},
		{"inputs", nlohmann::json::array({
			nlohmann::json{
				{"name", "PlayerInputComponent"},
				{"type", "object:InputComponent"},
			},
		})},
		{"outputs", nlohmann::json::array()},
		{"locals",  nlohmann::json::array()},
		{"body",    std::move(setupBody)},
	};
	functions.push_back(std::move(setupFn));
}

}    // namespace decompile_detail
using namespace decompile_detail;

nlohmann::json DecompileFunction(backends::IBlueprintReader& reader,
								 std::string_view assetPath,
								 std::string_view functionName) {
	BPMetadata meta = reader.ReadBlueprint(assetPath);
	BPFunction fn   = reader.GetFunction(assetPath, functionName);

	std::set<std::string> memberVarNames;
	for (const auto& v : meta.Variables)
	{
		memberVarNames.insert(v.Name);
	}

	Walker walker(fn.Graph, fn, memberVarNames);
	const BPNode* entry = FindFunctionEntry(fn.Graph);
	nlohmann::json body;
	nlohmann::json unsupportedSummary = nlohmann::json::array();

	// Pure-function detection: BP's pure functions render without an
	// exec output on the FunctionEntry node — they're invoked lazily
	// by their consumers. Mirrors UE's BlueprintPure detection:
	// const + has-return. Codegen uses this to emit
	// UFUNCTION(BlueprintPure) instead of BlueprintCallable.
	bool isPureFunction = false;
	if (entry && !fn.Outputs.empty()) {
		bool hasExecOut = false;
		for (const auto& p : entry->Pins) {
			if (p.Direction == "Output" && p.Type.Category == "exec") {
				hasExecOut = true;
				break;
			}
		}
		isPureFunction = !hasExecOut;
	}

	if (!entry) {
		body = nlohmann::json::array();
	} else {
		std::set<std::string> visited;
		const BPNode* startsAt = walker.FollowExec(*entry, "then");
		body = DecompileStatementsFrom(walker, startsAt, nullptr, visited);
	}

	// Synthetic return: if the function returns a value but the
	// decompile walk didn't reach a FunctionResult node, append a
	// return statement using the output defaults. Happens when the
	// BP author left an unwired return path or attached the
	// FunctionResult to a side branch the walker didn't see. Without
	// this the generated C++ has `bool TakeDamage() {}` — non-void
	// with no return → compile error.
	auto endsWithReturn = [&]() {
		if (!body.is_array() || body.empty())
		{
			return false;
		}
		const auto& last = body.back();
		return last.is_object() && last.contains("return");
	};
	if (!fn.Outputs.empty() && !endsWithReturn()) {
		nlohmann::json defaultRet;
		if (fn.Outputs.size() == 1) {
			// Single-return: emit the value directly.
			const auto& o = fn.Outputs[0];
			std::string defStr = o.DefaultValue.value_or(std::string{});
			// Render the default as a BPIR literal.
			if (o.Type.Category == "bool") {
				defaultRet = nlohmann::json{{"return", nlohmann::json{{"lit", defStr == "true"}}}};
			} else if (o.Type.Category == "int" || o.Type.Category == "int64" ||
					   o.Type.Category == "byte") {
				int64_t v = 0;
				try { v = defStr.empty() ? 0 : std::stoll(defStr); } catch (...) {}
				defaultRet = nlohmann::json{{"return", nlohmann::json{{"lit", v}}}};
			} else if (o.Type.Category == "real" || o.Type.Category == "float" ||
					   o.Type.Category == "double") {
				double v = 0.0;
				try { v = defStr.empty() ? 0.0 : std::stod(defStr); } catch (...) {}
				defaultRet = nlohmann::json{{"return", nlohmann::json{{"lit", v}}}};
			} else if (o.Type.Category == "object" || o.Type.Category == "class" ||
					   o.Type.Category == "soft_object" || o.Type.Category == "soft_class") {
				defaultRet = nlohmann::json{{"return", nlohmann::json{{"lit", nullptr}}}};
			} else {
				// FString / FName / FText / struct: rely on a default-
				// constructed temporary. Emit empty literal so codegen
				// can render `return {};` or similar via type context.
				defaultRet = nlohmann::json{{"return", nlohmann::json{{"lit", std::string{}}}}};
			}
		} else {
			// Multi-return: emit empty literal vector — caller can patch.
			// Better than no return at all.
			nlohmann::json parts = nlohmann::json::array();
			for (const auto& o : fn.Outputs) {
				(void)o;
				parts.push_back(nlohmann::json{{"lit", nullptr}});
			}
			defaultRet = nlohmann::json{{"return", parts}};
		}
		body.push_back(std::move(defaultRet));
	}

	// Walk the resulting body to collect unsupported-node summary at
	// the top level for quick "what couldn't I represent?" agent reads.
	std::function<void(const nlohmann::json&)> collectUnsupported =
		[&](const nlohmann::json& v) {
			if (v.is_array()) {
				for (const auto& el : v)
				{
					collectUnsupported(el);
				}
			} else if (v.is_object()) {
				if (v.contains("unsupported")) {
					unsupportedSummary.push_back(v["unsupported"]);
				}
				for (auto& [k, child] : v.items())
				{
					collectUnsupported(child);
				}
			}
		};
	collectUnsupported(body);

	// Build inputs/outputs/locals from the function spec.
	auto varListToJson = [](const std::vector<BPVariable>& vs) {
		nlohmann::json arr = nlohmann::json::array();
		for (const auto& v : vs)
		{
			arr.push_back(VariableDeclToJson(v));
		}
		return arr;
	};

	nlohmann::json metadata = {
		{"asset_path", std::string(assetPath)},
	};
	if (isPureFunction)
	{
		metadata["pure"] = true;
	}
	nlohmann::json doc = {
		{"version", kBpirSchemaVersion},
		{"kind", "function"},
		{"name", fn.Name},
		{"metadata", std::move(metadata)},
		{"inputs",  varListToJson(fn.Inputs)},
		{"outputs", varListToJson(fn.Outputs)},
		{"locals",  varListToJson(fn.Locals)},
		{"body",    std::move(body)},
	};
	if (!unsupportedSummary.empty()) {
		doc["unsupported_nodes"] = std::move(unsupportedSummary);
	}

	// Auto-synthesized member variables — populated during the walk
	// by stateful-macro handlers (DoOnce/FlipFlop/DoN) and latent-
	// action handlers (Delay → FTimerHandle). Surface them so
	// DecompileBlueprint can merge them into the class's variables[]
	// array. Keyed by name in the walker so emission order is stable
	// (map iteration order).
	if (!walker.autoSynthVars.empty()) {
		nlohmann::json synth = nlohmann::json::array();
		for (const auto& [name, decl] : walker.autoSynthVars) {
			(void)name;
			synth.push_back(decl);
		}
		doc["auto_synth_vars"] = std::move(synth);
	}

	// Auto-synthesized continuation functions — emitted by latent-
	// action lowering (each Delay produces one). Fix up the metadata's
	// asset_path here so it matches the parent function's context;
	// the handler couldn't know it at emission time. Order is preserved
	// (vector iteration).
	if (!walker.autoSynthFunctions.empty()) {
		nlohmann::json synth = nlohmann::json::array();
		for (auto& fnDoc : walker.autoSynthFunctions) {
			if (fnDoc.is_object() && fnDoc.contains("metadata")
				&& fnDoc["metadata"].is_object()) {
				fnDoc["metadata"]["asset_path"] = std::string(assetPath);
			}
			synth.push_back(fnDoc);
		}
		doc["auto_synth_funcs"] = std::move(synth);
	}

	// Sanity-check before returning. If the decompile pass produced
	// something the validator rejects, that's a bug in this file —
	// surface it loudly rather than passing through to codegen.
	ValidateBpir(doc);
	return doc;
}

nlohmann::json DecompileBlueprint(backends::IBlueprintReader& reader,
								  std::string_view assetPath) {
	BPMetadata meta = reader.ReadBlueprint(assetPath);

	nlohmann::json variables = nlohmann::json::array();
	for (const auto& v : meta.Variables)
	{
		variables.push_back(VariableDeclToJson(v));
	}

	nlohmann::json functions = nlohmann::json::array();
	for (const auto& fnSummary : meta.Functions) {
		try {
			functions.push_back(DecompileFunction(reader, assetPath, fnSummary.Name));
		} catch (const std::exception& e) {
			// Per-function failures shouldn't tank whole-class decompile.
			functions.push_back(nlohmann::json{
				{"version", kBpirSchemaVersion},
				{"kind", "function"},
				{"name", fnSummary.Name},
				{"body", nlohmann::json::array()},
				{"unsupported_nodes", nlohmann::json::array({
					nlohmann::json{
						{"node_class", "<decompile-failure>"},
						{"reason", e.what()},
					}
				})},
			});
		}
	}

	// Hoist auto-synthesized vars from each function doc into the
	// class-level variables[] array. Stateful-macro lowering
	// (DoOnce/FlipFlop/DoN) and latent-action lowering
	// (Delay → FTimerHandle) need real class members to persist
	// across calls, so their decls live next to user-declared BP
	// variables and go through the same UPROPERTY emission path.
	//
	// Deduped by name in case the same conceptual flag was registered
	// from multiple functions (unlikely — node GUIDs are per-instance
	// — but the per-name idempotency mirrors the in-walker dedup).
	{
		std::set<std::string> alreadyHaveVar;
		for (const auto& v : variables) {
			if (v.is_object() && v.contains("name") && v["name"].is_string()) {
				alreadyHaveVar.insert(v["name"].get<std::string>());
			}
		}
		for (auto& fn : functions) {
			if (!fn.is_object())
			{
				continue;
			}
			auto it = fn.find("auto_synth_vars");
			if (it == fn.end() || !it->is_array())
			{
				continue;
			}
			for (auto& v : *it) {
				if (!v.is_object() || !v.contains("name") || !v["name"].is_string())
				{
					continue;
				}
				const auto& name = v["name"].get<std::string>();
				if (alreadyHaveVar.count(name))
				{
					continue;
				}
				alreadyHaveVar.insert(name);
				variables.push_back(v);
			}
			// Remove from the function doc — the class-level surface
			// is canonical; leaving stale copies on each function would
			// confuse downstream readers.
			fn.erase("auto_synth_vars");
		}
	}

	// Hoist auto-synthesized continuation functions into the class's
	// functions[] array. Each Delay node in a parent function
	// produces one continuation; we append them directly after the
	// parent so the generated header reads in roughly walk order
	// (parent / cont_1 / cont_2 / next-parent / ...).
	//
	// The continuation docs are full BPIR function docs ready for
	// CppClassEmit. The validator runs against them via the
	// ValidateClassDoc call at the end (which walks functions[]).
	{
		nlohmann::json hoisted = nlohmann::json::array();
		for (auto& fn : functions) {
			// Extract continuations BEFORE pushing the parent so the
			// pushed copy doesn't carry the auto_synth_funcs key.
			nlohmann::json continuations = nlohmann::json::array();
			if (fn.is_object()) {
				auto it = fn.find("auto_synth_funcs");
				if (it != fn.end() && it->is_array()) {
					continuations = std::move(*it);
					fn.erase("auto_synth_funcs");
				}
			}
			hoisted.push_back(std::move(fn));
			for (auto& contFn : continuations) {
				hoisted.push_back(std::move(contFn));
			}
		}
		functions = std::move(hoisted);
	}

	// EnhancedInput pass: scan event graphs for K2Node_EnhancedInputAction
	// nodes and synthesize the per-trigger callbacks + a
	// SetupPlayerInputComponent override. Mutates variables[] and
	// functions[] in place, appending the generated content. No-op when
	// the class has no input action events.
	{
		std::set<std::string> memberVarNamesForInput;
		for (const auto& v : meta.Variables)
		{
			memberVarNamesForInput.insert(v.Name);
		}
		ProcessEnhancedInputBindings(reader, assetPath, meta,
									 memberVarNamesForInput,
									 variables, functions);
	}

	nlohmann::json interfaces = nlohmann::json::array();
	for (const auto& i : meta.Interfaces)
	{
		interfaces.push_back(i);
	}

	// Components: surface SCS-tracked subobjects so CppClassEmit can
	// generate the CreateDefaultSubobject + SetupAttachment scaffolding
	// a UE actor class needs. Without this, the transpiled C++ class
	// would have no components even though the BP source does.
	// Failure to fetch components doesn't fail the whole class
	// decompile; we just emit an empty array.
	nlohmann::json components = nlohmann::json::array();
	try {
		for (const auto& c : reader.GetComponents(assetPath)) {
			nlohmann::json cj = {
				{"name",    c.Name},
				{"class",   c.Class},
				{"is_root", c.IsRoot},
			};
			if (c.Parent.has_value())
			{
				cj["parent"] = *c.Parent;
			}
			// Property overrides -- per-component values authored in
			// the BP Components panel that differ from the component
			// class's CDO. CppClassEmit emits these as
			// `Comp->Property = X;` in the C++ constructor.
			if (!c.Properties.empty()) {
				nlohmann::json props = nlohmann::json::array();
				for (const auto& p : c.Properties) {
					nlohmann::json pj = {
						{"name",  p.Name},
						{"type",  p.Type},
						{"value", p.ValueText},
					};
					if (!p.PropertyClass.empty()) {
						pj["property_class"] = p.PropertyClass;
					}
					props.push_back(std::move(pj));
				}
				cj["properties"] = std::move(props);
			}
			components.push_back(std::move(cj));
		}
	} catch (...) {
		// Backend without component support, or fetch errored --
		// proceed with empty components[]. Class still transpiles.
	}

	nlohmann::json doc = {
		{"version", kBpirSchemaVersion},
		{"kind", "class"},
		{"name", meta.Name},
		{"metadata", {
			{"asset_path", std::string(assetPath)},
			{"parent_class", meta.ParentClass},
		}},
		{"interfaces", std::move(interfaces)},
		{"variables",  std::move(variables)},
		{"functions",  std::move(functions)},
		{"components", std::move(components)},
	};
	ValidateBpir(doc);
	return doc;
}

} // namespace bpr::tools
