#include "tools/CompileFunction.h"
#include "tools/ApplyOps.h"
#include "tools/TypeShorthand.h"

#include <fmt/core.h>

#include <stdexcept>
#include <string>

namespace bpr::tools {

namespace compile_function_detail {

// ----- Op-list builder ---------------------------------------------------
// CompileFunction doesn't talk to the reader directly — it produces a
// list of apply_ops-shaped JSON ops and a final "preview" payload.
// We then run those through the apply_ops dispatch path so all the
// idempotency / pin-enrichment / slot-resolution gets reused.
//
// A separate driver in this file calls each op against the reader.
// (Inlining apply_ops's dispatcher would require exporting it from
// ApplyOps.cpp; the duplication is small and keeps this file
// self-contained.)

struct Compiler {
	std::string asset;
	std::string graph;       // function name == graph name in UE
	nlohmann::json ops = nlohmann::json::array();
	int slotCounter = 0;
	int nextX = 400;
	int nextY = 0;
	static constexpr int kRowHeight = 200;

	std::string Slot(std::string_view stem) {
		return fmt::format("{}_{}", stem, slotCounter++);
	}

	std::pair<int,int> NextPos() {
		auto p = std::pair{nextX, nextY};
		nextY += kRowHeight;
		return p;
	}

	// Emit add_node, return slot id.
	std::string AddNode(std::string_view kind, std::string_view stem,
						const nlohmann::json& extras = {}) {
		auto [x, y] = NextPos();
		std::string slot = Slot(stem);
		nlohmann::json op = {
			{"op", "add_node"},
			{"id", slot},
			{"asset_path", asset},
			{"graph_name", graph},
			{"kind", std::string(kind)},
			{"x", x}, {"y", y},
		};
		if (extras.is_object()) {
			for (auto& [k, v] : extras.items())
			{
				op[k] = v;
			}
		}
		ops.push_back(std::move(op));
		return slot;
	}

	void WireExec(std::string_view fromSlot, std::string_view fromPin,
				  std::string_view toSlot,   std::string_view toPin) {
		ops.push_back({
			{"op", "wire_pins"},
			{"asset_path", asset},
			{"graph_name", graph},
			{"from_node", fmt::format("${}", fromSlot)},
			{"from_pin",  std::string(fromPin)},
			{"to_node",   fmt::format("${}", toSlot)},
			{"to_pin",    std::string(toPin)},
		});
	}
};

// ----- B1 helpers --------------------------------------------------------
// Sentinel slot id returned by CompileExpr for {lit:value} expressions.
// UE has no first-class literal node; consumers detect this prefix and
// emit set_pin_default against their input pin instead of wire_pins.
constexpr const char* kLitPrefix = "__lit:";

bool IsLitSlot(const std::string& slot) {
	return slot.size() > 6 && slot.compare(0, 6, kLitPrefix) == 0;
}

std::string LitValue(const std::string& slot) {
	return slot.substr(6);  // strip "__lit:"
}

// Resolve a value source into a connection: either a wire from an upstream
// node's ReturnValue pin, or a literal default written onto the consumer's
// input pin. Centralizing this lets every "use this value here" site DRY
// up its lit-handling.
void EmitValueConnect(Compiler& c, const std::string& fromSlot,
					  const std::string& fromPin,
					  const std::string& toSlot, const std::string& toPin) {
	if (IsLitSlot(fromSlot)) {
		// Literal: write the value onto the consumer pin's default. Pin
		// identified by name (we don't have a slot's pin GUIDs at compile
		// time without round-tripping through add_node's pin enrichment).
		c.ops.push_back({
			{"op", "set_pin_default"},
			{"asset_path", c.asset}, {"graph_name", c.graph},
			{"node_id", fmt::format("${}", toSlot)},
			{"pin_name", toPin},
			{"value",    LitValue(fromSlot)},
		});
		return;
	}
	c.ops.push_back({
		{"op", "wire_pins"},
		{"asset_path", c.asset}, {"graph_name", c.graph},
		{"from_node", fmt::format("${}", fromSlot)},
		{"from_pin",  fromPin},
		{"to_node",   fmt::format("${}", toSlot)},
		{"to_pin",    toPin},
	});
}

// Math / comparison alias map. Keys are user-friendly operators; values
// are (owner, function_name) pointing at canonical UE functions. The agent
// can write {call:"+", args:{A:..., B:...}} or {call:"==", ...} instead
// of remembering UKismetMathLibrary's full naming convention.
//
// We default to the int-int variants — UE's K2 schema auto-promotes
// int↔float on connection so this works for both. For float-explicit
// math, callers can still write the canonical name directly.
struct AliasTarget { const char* owner; const char* fn; };
const std::map<std::string, AliasTarget>& AliasMap() {
	static const std::map<std::string, AliasTarget> m = {
		{"+",  {"KismetMathLibrary", "Add_IntInt"}},
		{"-",  {"KismetMathLibrary", "Subtract_IntInt"}},
		{"*",  {"KismetMathLibrary", "Multiply_IntInt"}},
		{"/",  {"KismetMathLibrary", "Divide_IntInt"}},
		{"%",  {"KismetMathLibrary", "Percent_IntInt"}},
		{"==", {"KismetMathLibrary", "EqualEqual_IntInt"}},
		{"!=", {"KismetMathLibrary", "NotEqual_IntInt"}},
		{"<",  {"KismetMathLibrary", "Less_IntInt"}},
		{"<=", {"KismetMathLibrary", "LessEqual_IntInt"}},
		{">",  {"KismetMathLibrary", "Greater_IntInt"}},
		{">=", {"KismetMathLibrary", "GreaterEqual_IntInt"}},
		{"&&", {"KismetMathLibrary", "BooleanAND"}},
		{"||", {"KismetMathLibrary", "BooleanOR"}},
		{"!",  {"KismetMathLibrary", "Not_PreBool"}},
		// Float-explicit aliases for callers who care.
		{"+f", {"KismetMathLibrary", "Add_FloatFloat"}},
		{"-f", {"KismetMathLibrary", "Subtract_FloatFloat"}},
		{"*f", {"KismetMathLibrary", "Multiply_FloatFloat"}},
		{"/f", {"KismetMathLibrary", "Divide_FloatFloat"}},
		{"==f",{"KismetMathLibrary", "EqualEqual_FloatFloat"}},
		{"<f", {"KismetMathLibrary", "Less_FloatFloat"}},
		{"<=f",{"KismetMathLibrary", "LessEqual_FloatFloat"}},
	};
	return m;
}

// Forward decl.
std::string CompileExpr(Compiler& c, const nlohmann::json& expr);

// Returns the slot id of a Branch node whose Condition pin is wired from
// the expression's value pin. Caller wires "Then"/"Else" exec out.
std::string CompileBranch(Compiler& c, const nlohmann::json& cond) {
	std::string condSlot = CompileExpr(c, cond);
	std::string branch = c.AddNode("Branch", "branch");
	// Wire the value pin (or set default if cond is a literal) into
	// the Branch's Condition input.
	EmitValueConnect(c, condSlot, "ReturnValue", branch, "Condition");
	return branch;
}

// Compile a value-producing expression to a slot id. The slot's
// "ReturnValue" pin (or the first output data pin, by convention) holds
// the value. For {lit:value} expressions, returns a sentinel slot
// "__lit:<value>" — callers use EmitValueConnect to handle it.
std::string CompileExpr(Compiler& c, const nlohmann::json& expr) {
	if (!expr.is_object()) {
		throw std::invalid_argument(
			"expression must be an object — supported forms: "
			"{var:\"name\"}, {lit:value}, {call:\"fn\", args:{...}}");
	}
	if (auto it = expr.find("var"); it != expr.end()) {
		if (!it->is_string()) {
			throw std::invalid_argument(R"(expression "var" must be a string)");
		}
		return c.AddNode("VariableGet", "varget",
			{{"variable", it->get<std::string>()}});
	}
	if (auto it = expr.find("lit"); it != expr.end()) {
		// Literal: stringify the JSON value and return a sentinel slot.
		// Strings serialize as their raw text (no JSON quotes), other
		// types use their JSON form (numbers, booleans). The consumer
		// pin's TrySetDefaultValue validates the string against the pin
		// type at compile-time, surfacing useful errors via wire_pins's
		// enriched error path.
		std::string s;
		if (it->is_string())
		{
			s = it->get<std::string>();
		}
		else if (it->is_number())       s = it->dump();
		else if (it->is_boolean())      s = *it ? "true" : "false";
		else if (it->is_null())         s = "";
		else                            s = it->dump();  // arrays/objects (rare)
		return std::string(kLitPrefix) + s;
	}
	if (auto it = expr.find("call"); it != expr.end()) {
		if (!it->is_string()) {
			throw std::invalid_argument(R"(expression "call" must be a string)");
		}
		std::string fn = it->get<std::string>();
		std::string owner;
		// Alias check first — operator shorthand like "+" / "==" maps to
		// canonical UKismetMathLibrary names.
		if (auto aIt = AliasMap().find(fn); aIt != AliasMap().end()) {
			owner = aIt->second.owner;
			fn    = aIt->second.fn;
		} else {
			// "Owner::Func" qualified names for cross-class calls.
			auto sep = fn.find("::");
			if (sep != std::string::npos) {
				owner = fn.substr(0, sep);
				fn    = fn.substr(sep + 2);
			}
		}
		nlohmann::json extras = {{"function", fn}};
		if (!owner.empty())
		{
			extras["function_owner"] = owner;
		}
		std::string slot = c.AddNode("CallFunction", "call", extras);
		// Wire args by pin-name match (or set defaults for literals).
		if (auto argsIt = expr.find("args");
			argsIt != expr.end() && argsIt->is_object()) {
			for (auto& [pinName, valExpr] : argsIt->items()) {
				std::string argSlot = CompileExpr(c, valExpr);
				EmitValueConnect(c, argSlot, "ReturnValue", slot, pinName);
			}
		}
		return slot;
	}
	throw std::invalid_argument(fmt::format(
		"unrecognized expression form. Supported: {{var:\"name\"}}, "
		"{{lit:value}}, {{call:\"fn\", args:{{...}}}}. Got: {}",
		expr.dump()));
}

// ExecTail: a single "where exec exits this fragment" marker. A statement
// can produce 0, 1, or many tails. After an if/then/else with both branches
// non-empty, there are 2 tails — the next statement's exec input gets wired
// from BOTH (UE's K2 schema accepts multiple sources on an exec input pin,
// and the node fires when any source fires). This is the natural "merge"
// pattern in BPs without needing a Sequence/Join node.
struct ExecTail {
	std::string slot;
	std::string pin;
};
using ExecTails = std::vector<ExecTail>;

// Wire every tail in `prevs` to (toSlot, toPin). For 0 tails it's a no-op
// (function entry case). For 1 tail it's a single wire. For 2+ tails it's
// a merge — multiple incoming sources to the same exec input.
void WireTailsTo(Compiler& c, const ExecTails& prevs,
				 const std::string& toSlot, const std::string& toPin) {
	for (const auto& t : prevs) {
		if (t.slot.empty())
		{
			continue;
		}
		c.WireExec(t.slot, t.pin, toSlot, toPin);
	}
}

// Forward decl.
ExecTails CompileStatements(Compiler& c, ExecTails tails,
							const nlohmann::json& stmts);

// Compile a single statement with `prevs` inbound exec tails. Returns the
// outbound tails of this statement.
ExecTails CompileStatement(Compiler& c, ExecTails prevs,
						   const nlohmann::json& stmt) {
	if (!stmt.is_object()) {
		throw std::invalid_argument(
			"statement must be an object — supported forms: "
			"{if, then, [else]}, {set,to}, {call,args}, {comment}");
	}
	if (stmt.contains("comment")) {
		// Comments are a no-op for now — exec passes straight through.
		return prevs;
	}
	if (stmt.contains("if")) {
		std::string branch = CompileBranch(c, stmt["if"]);
		WireTailsTo(c, prevs, branch, "execute");
		ExecTails thenTails = {{branch, "then"}};
		if (auto t = stmt.find("then"); t != stmt.end()) {
			thenTails = CompileStatements(c, thenTails, *t);
		}
		ExecTails elseTails = {{branch, "else"}};
		if (auto e = stmt.find("else"); e != stmt.end()) {
			elseTails = CompileStatements(c, elseTails, *e);
		}
		// Merge: union of both branches' tails. The next statement's exec
		// input gets wired from each (B1: replaces v1's "else tail dangles").
		ExecTails out;
		out.reserve(thenTails.size() + elseTails.size());
		for (auto& t : thenTails)
		{
			out.push_back(std::move(t));
		}
		for (auto& t : elseTails)
		{
			out.push_back(std::move(t));
		}
		return out;
	}
	if (stmt.contains("set")) {
		if (!stmt["set"].is_string()) {
			throw std::invalid_argument(R"(statement "set" must be a string)");
		}
		if (!stmt.contains("to")) {
			throw std::invalid_argument(R"(statement "set" requires a "to" expression)");
		}
		std::string varName = stmt["set"].get<std::string>();
		std::string valSlot = CompileExpr(c, stmt["to"]);
		std::string setSlot = c.AddNode("VariableSet", "varset",
			{{"variable", varName}});
		// Wire the value (or set default for literals) into the SetVar's
		// data input (pin name matches the variable name in K2).
		EmitValueConnect(c, valSlot, "ReturnValue", setSlot, varName);
		WireTailsTo(c, prevs, setSlot, "execute");
		return {{setSlot, "then"}};
	}
	if (stmt.contains("call")) {
		if (!stmt["call"].is_string()) {
			throw std::invalid_argument(R"(statement "call" must be a string)");
		}
		std::string fn = stmt["call"].get<std::string>();
		std::string owner;
		if (auto aIt = AliasMap().find(fn); aIt != AliasMap().end()) {
			owner = aIt->second.owner;
			fn    = aIt->second.fn;
		} else {
			auto sep = fn.find("::");
			if (sep != std::string::npos) {
				owner = fn.substr(0, sep);
				fn    = fn.substr(sep + 2);
			}
		}
		nlohmann::json extras = {{"function", fn}};
		if (!owner.empty())
		{
			extras["function_owner"] = owner;
		}
		std::string slot = c.AddNode("CallFunction", "callstmt", extras);
		if (auto argsIt = stmt.find("args");
			argsIt != stmt.end() && argsIt->is_object()) {
			for (auto& [pinName, valExpr] : argsIt->items()) {
				std::string argSlot = CompileExpr(c, valExpr);
				EmitValueConnect(c, argSlot, "ReturnValue", slot, pinName);
			}
		}
		WireTailsTo(c, prevs, slot, "execute");
		return {{slot, "then"}};
	}
	throw std::invalid_argument(fmt::format(
		"unrecognized statement form. Supported: {{if,then,[else]}}, "
		"{{set,to}}, {{call,args}}, {{comment}}. Got: {}", stmt.dump()));
}

ExecTails CompileStatements(Compiler& c, ExecTails tails,
							const nlohmann::json& stmts) {
	if (!stmts.is_array()) {
		throw std::invalid_argument("statement block must be an array");
	}
	for (const auto& s : stmts) {
		tails = CompileStatement(c, tails, s);
	}
	return tails;
}

}    // namespace compile_function_detail
using namespace compile_function_detail;

void RegisterCompileFunction(ToolRegistry& registry,
							 backends::IBlueprintReader& reader) {
	ToolDescriptor d;
	d.name = "compile_function";
	d.description =
		"[cpp] Compile a tiny pseudocode DSL into a fully-wired Blueprint function. "
		"Accepts the function signature plus a `body` of statements; "
		"translates to add_node + wire_pins + set_pin_default ops and "
		"runs them as one batch (single recompile per affected BP).\n\n"
		"Statements: {if, then, [else]}, {set, to}, {call, args}, {comment}.\n"
		"Expressions:\n"
		"  {var:\"name\"}     — VariableGet for a member variable\n"
		"  {lit:value}      — literal pin default (string / number / bool)\n"
		"  {call:\"fn\", args:{...}} — CallFunction node\n\n"
		"`call` accepts operator aliases that map to UKismetMathLibrary:\n"
		"  +, -, *, /, %, ==, !=, <, <=, >, >=, &&, ||, !\n"
		"  Float-explicit variants: +f, -f, *f, /f, ==f, <f, <=f\n"
		"Or use canonical \"Owner::Function\" form for any other call.\n\n"
		"After an if/then/else, exec from BOTH branches converges into "
		"the next statement (UE's K2 schema accepts multiple sources on "
		"exec input pins — no Sequence node needed). If a branch's body "
		"is empty, that side's exec out from the Branch node is the tail.\n\n"
		"Pass `dry_run: true` to get the compiled op list without "
		"executing — useful for inspecting what compile_function would do "
		"before committing.\n\n"
		"On expression/statement form errors, the response says exactly "
		"which form was unrecognized so the agent can fall back to "
		"apply_ops for that statement only.";
	d.input_schema = {
		{"type","object"},
		{"properties", {
			{"asset_path",   {{"type","string"}}},
			{"function_name",{{"type","string"}}},
			{"inputs",  {{"type","array"},
						 {"description","[{name, type}, ...] — same shape as add_function_input"}}},
			{"outputs", {{"type","array"},
						 {"description","[{name, type}, ...] — same shape as add_function_output"}}},
			{"body",    {{"type","array"},
						 {"description","Sequence of statement objects — see tool description."}}},
			{"atomic",  {{"type","boolean"},
						 {"description","Bail on first op failure (default true)."}}},
			{"dry_run", {{"type","boolean"},
						 {"description","Return the compiled ops without executing — lets you preview."}}},
		}},
		{"required", nlohmann::json::array({"asset_path","function_name","body"})},
	};
	registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
		std::string asset = args.at("asset_path").get<std::string>();
		std::string fname = args.at("function_name").get<std::string>();
		if (!args.contains("body") || !args["body"].is_array()) {
			throw std::invalid_argument(R"(compile_function requires "body" to be an array)");
		}

		Compiler c;
		c.asset = asset;
		c.graph = fname;

		// Step 1: ensure function exists. Idempotent. Tag the op with a
		// slot id so apply_ops's OpAddFunction binds the FunctionEntry's
		// GUID to it — we can then wire the entry's `then` exec into the
		// first body statement automatically (no manual wire_pins call
		// needed by the caller).
		const std::string kEntrySlot = "__entry";
		c.ops.push_back({
			{"op", "add_function"},
			{"id", kEntrySlot},
			{"asset_path", asset},
			{"name", fname},
		});

		// Step 2: declare inputs.
		if (auto inIt = args.find("inputs"); inIt != args.end() && inIt->is_array()) {
			for (auto& p : *inIt) {
				if (!p.is_object() || !p.contains("name") || !p.contains("type")) {
					throw std::invalid_argument(
						R"(each "inputs" entry must be {name, type})");
				}
				// Pre-validate type so the user gets the parser error here,
				// not deep inside apply_ops.
				(void)ParseTypeArg(p["type"]);
				c.ops.push_back({
					{"op", "add_function_input"},
					{"asset_path", asset},
					{"function_name", fname},
					{"param_name", p["name"]},
					{"type", p["type"]},
				});
			}
		}
		if (auto outIt = args.find("outputs"); outIt != args.end() && outIt->is_array()) {
			for (auto& p : *outIt) {
				if (!p.is_object() || !p.contains("name") || !p.contains("type")) {
					throw std::invalid_argument(
						R"(each "outputs" entry must be {name, type})");
				}
				(void)ParseTypeArg(p["type"]);
				c.ops.push_back({
					{"op", "add_function_output"},
					{"asset_path", asset},
					{"function_name", fname},
					{"param_name", p["name"]},
					{"type", p["type"]},
				});
			}
		}

		// Step 3: compile the body. The body's inbound tails start at
		// the FunctionEntry node's `then` exec (slot `__entry` bound by
		// OpAddFunction). The first statement's WireTailsTo call wires
		// FunctionEntry's `then` straight into its execute pin —
		// matching what UE does when you build a function in the editor.
		ExecTails tails = {{kEntrySlot, "then"}};
		CompileStatements(c, tails, args["body"]);

		// Step 4: dry-run mode short-circuits to just returning the ops
		// (useful for the agent to inspect what we'd do before committing).
		bool dryRun = args.value("dry_run", false);
		if (dryRun) {
			return nlohmann::json{
				{"ok",            true},
				{"dry_run",       true},
				{"asset_path",    asset},
				{"function_name", fname},
				{"ops",           std::move(c.ops)},
			};
		}

		// Step 5: execute via the shared RunOps helper — same semantics as
		// apply_ops (slots, pin enrichment, idempotency, atomic on/off).
		bool atomic = args.value("atomic", true);
		nlohmann::json runResult = RunOps(reader, c.ops, atomic);
		// Stitch in the surrounding context so callers see the function
		// they targeted plus the per-op trace.
		runResult["asset_path"]    = asset;
		runResult["function_name"] = fname;
		return runResult;
	});
}

}    // namespace bpr::tools
