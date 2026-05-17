#include "tools/Bpir.h"

#include <fmt/core.h>

#include <stdexcept>

namespace bpr::tools {

namespace {

// Forward decls so statement / expression validators can recurse into
// each other (a `cast` statement contains a `<expr>` value).
void ValidateStatementList(const nlohmann::json& list, std::string_view path);
void ValidateStatement(const nlohmann::json& stmt, std::string_view path);
void ValidateExpression(const nlohmann::json& expr, std::string_view path);

// Helper: throw with a structured "<path>: <message>" error.
[[noreturn]] void Bad(std::string_view path, std::string_view msg) {
	throw std::invalid_argument(fmt::format("BPIR at '{}': {}", path, msg));
}

void RequireObject(const nlohmann::json& v, std::string_view path) {
	if (!v.is_object())
	{
		Bad(path, "expected object");
	}
}

void RequireArray(const nlohmann::json& v, std::string_view path) {
	if (!v.is_array())
	{
		Bad(path, "expected array");
	}
}

void RequireString(const nlohmann::json& parent, std::string_view key,
				   std::string_view path) {
	auto it = parent.find(key);
	if (it == parent.end() || !it->is_string()) {
		Bad(path, fmt::format("missing or non-string field '{}'", key));
	}
}

// ----- Form recognition --------------------------------------------------

// Order matters for DetectStatementForm — the first key that matches
// determines the form. None overlap meaningfully, but we keep a stable
// order so error messages stay consistent.
const std::vector<std::string>& StatementFormsImpl() {
	static const std::vector<std::string> forms = {
		"if", "set", "call", "comment",
		"return", "cast", "switch", "for_each", "while", "sequence",
		"break", "continue",
		// Multicast-delegate ops. See K2Node_BaseMCDelegate subclasses
		// (CallDelegate, AddDelegate, RemoveDelegate, ClearDelegate).
		"broadcast", "bind_delegate", "unbind_delegate", "clear_delegate",
		"unsupported",
	};
	return forms;
}

const std::vector<std::string>& ExpressionFormsImpl() {
	static const std::vector<std::string> forms = {
		"var", "lit", "call",
		"cast", "member", "index", "self",
		"new_array", "new_struct", "new_set", "new_map",
	};
	return forms;
}

std::string FirstMatchingKey(const nlohmann::json& obj,
							 const std::vector<std::string>& candidates) {
	if (!obj.is_object()) return {};
	for (const auto& k : candidates) {
		if (obj.contains(k))
		{
			return k;
		}
	}
	return {};
}

// ----- Expression validators ---------------------------------------------

void ValidateExpression(const nlohmann::json& expr, std::string_view path) {
	RequireObject(expr, path);
	std::string form = DetectExpressionForm(expr);
	if (form.empty()) {
		Bad(path, fmt::format(
			"no recognized expression form found. Supported: {}",
			fmt::join(ExpressionFormsImpl(), ", ")));
	}
	if (form == "var") {
		// {var: "name", [scope: ...]}
		if (!expr["var"].is_string())
		{
			Bad(path, R"(field "var" must be a string)");
		}
		if (auto sIt = expr.find("scope"); sIt != expr.end()) {
			if (!sIt->is_string())
			{
				Bad(path, R"(field "scope" must be a string)");
			}
			const auto& s = sIt->get_ref<const std::string&>();
			if (s != "local" && s != "member" && s != "input" && s != "output") {
				Bad(path, fmt::format(
					R"(field "scope" = "{}", expected one of "local"/"member"/"input"/"output")", s));
			}
		}
	} else if (form == "lit") {
		// {lit: <any>} — any JSON scalar is allowed; container-shaped
		// literals are reserved (might be valid for new_array later but
		// we steer callers to that form).
		const auto& v = expr["lit"];
		if (!v.is_null() && !v.is_string() && !v.is_number() && !v.is_boolean()) {
			Bad(path, R"(field "lit" must be a JSON scalar (string|number|boolean|null))");
		}
	} else if (form == "call") {
		// {call: "fn", [args: {...}]}
		if (!expr["call"].is_string())
		{
			Bad(path, R"(field "call" must be a string)");
		}
		if (auto aIt = expr.find("args"); aIt != expr.end()) {
			if (!aIt->is_object())
			{
				Bad(path, R"(field "args" must be an object)");
			}
			for (auto& [pinName, valExpr] : aIt->items()) {
				ValidateExpression(valExpr,
					fmt::format("{}.args.{}", path, pinName));
			}
		}
	} else if (form == "cast") {
		// {cast: <expr>, to: "<class>"} — expr-form (no success/fail)
		ValidateExpression(expr["cast"], fmt::format("{}.cast", path));
		RequireString(expr, "to", path);
	} else if (form == "member") {
		// {member: <expr>, name: "<field>"}
		ValidateExpression(expr["member"], fmt::format("{}.member", path));
		RequireString(expr, "name", path);
	} else if (form == "index") {
		// {index: <arr>, idx: <expr>}
		ValidateExpression(expr["index"], fmt::format("{}.index", path));
		if (!expr.contains("idx"))
		{
			Bad(path, R"(missing "idx" expression)");
		}
		ValidateExpression(expr["idx"], fmt::format("{}.idx", path));
	} else if (form == "self") {
		// {self: null} — value is conventional, ignored.
	} else if (form == "new_array") {
		// {new_array: [<expr>...]}
		RequireArray(expr["new_array"], fmt::format("{}.new_array", path));
		std::size_t i = 0;
		for (const auto& el : expr["new_array"]) {
			ValidateExpression(el, fmt::format("{}.new_array[{}]", path, i++));
		}
	} else if (form == "new_set") {
		// {new_set: [<expr>...]}
		RequireArray(expr["new_set"], fmt::format("{}.new_set", path));
		std::size_t i = 0;
		for (const auto& el : expr["new_set"]) {
			ValidateExpression(el, fmt::format("{}.new_set[{}]", path, i++));
		}
	} else if (form == "new_map") {
		// {new_map: [{key: <expr>, value: <expr>}, ...]}
		RequireArray(expr["new_map"], fmt::format("{}.new_map", path));
		std::size_t i = 0;
		for (const auto& pair : expr["new_map"]) {
			RequireObject(pair, fmt::format("{}.new_map[{}]", path, i));
			if (!pair.contains("key")) {
				Bad(path, fmt::format(R"({}.new_map[{}] missing "key")", path, i));
			}
			if (!pair.contains("value")) {
				Bad(path, fmt::format(R"({}.new_map[{}] missing "value")", path, i));
			}
			ValidateExpression(pair["key"],   fmt::format("{}.new_map[{}].key",   path, i));
			ValidateExpression(pair["value"], fmt::format("{}.new_map[{}].value", path, i));
			++i;
		}
	} else if (form == "new_struct") {
		// {new_struct: "<type>", fields: {name: <expr>}}
		if (!expr["new_struct"].is_string()) {
			Bad(path, R"(field "new_struct" must be a string (the struct type))");
		}
		if (auto fIt = expr.find("fields"); fIt != expr.end()) {
			if (!fIt->is_object())
			{
				Bad(path, R"(field "fields" must be an object)");
			}
			for (auto& [fieldName, valExpr] : fIt->items()) {
				ValidateExpression(valExpr,
					fmt::format("{}.fields.{}", path, fieldName));
			}
		}
	}
}

// ----- Statement validators ----------------------------------------------

void ValidateStatementList(const nlohmann::json& list, std::string_view path) {
	RequireArray(list, path);
	std::size_t i = 0;
	for (const auto& s : list) {
		ValidateStatement(s, fmt::format("{}[{}]", path, i++));
	}
}

void ValidateStatement(const nlohmann::json& stmt, std::string_view path) {
	RequireObject(stmt, path);
	std::string form = DetectStatementForm(stmt);
	if (form.empty()) {
		Bad(path, fmt::format(
			"no recognized statement form found. Supported: {}",
			fmt::join(StatementFormsImpl(), ", ")));
	}
	if (form == "if") {
		ValidateExpression(stmt["if"], fmt::format("{}.if", path));
		if (auto t = stmt.find("then"); t != stmt.end()) {
			ValidateStatementList(*t, fmt::format("{}.then", path));
		}
		if (auto e = stmt.find("else"); e != stmt.end()) {
			ValidateStatementList(*e, fmt::format("{}.else", path));
		}
	} else if (form == "set") {
		if (!stmt["set"].is_string())
		{
			Bad(path, R"(field "set" must be a string (var name))");
		}
		if (!stmt.contains("to"))
		{
			Bad(path, R"(missing "to" expression)");
		}
		ValidateExpression(stmt["to"], fmt::format("{}.to", path));
	} else if (form == "call") {
		if (!stmt["call"].is_string())
		{
			Bad(path, R"(field "call" must be a string)");
		}
		if (auto aIt = stmt.find("args"); aIt != stmt.end()) {
			if (!aIt->is_object())
			{
				Bad(path, R"(field "args" must be an object)");
			}
			for (auto& [pinName, valExpr] : aIt->items()) {
				ValidateExpression(valExpr,
					fmt::format("{}.args.{}", path, pinName));
			}
		}
	} else if (form == "comment") {
		if (!stmt["comment"].is_string())
		{
			Bad(path, R"(field "comment" must be a string)");
		}
	} else if (form == "return") {
		const auto& r = stmt["return"];
		if (r.is_null()) {
			// Bare `return;`
		} else if (r.is_array()) {
			std::size_t i = 0;
			for (const auto& el : r) {
				ValidateExpression(el, fmt::format("{}.return[{}]", path, i++));
			}
		} else {
			ValidateExpression(r, fmt::format("{}.return", path));
		}
	} else if (form == "cast") {
		ValidateExpression(stmt["cast"], fmt::format("{}.cast", path));
		RequireString(stmt, "to", path);
		if (auto aIt = stmt.find("as"); aIt != stmt.end() && !aIt->is_string()) {
			Bad(path, R"(field "as" must be a string (local variable name for the cast result))");
		}
		if (!stmt.contains("success")) {
			Bad(path, R"(cast statement requires a "success" branch (use {} for empty))");
		}
		ValidateStatementList(stmt["success"], fmt::format("{}.success", path));
		if (!stmt.contains("fail")) {
			Bad(path, R"(cast statement requires a "fail" branch (use {} for empty))");
		}
		ValidateStatementList(stmt["fail"], fmt::format("{}.fail", path));
	} else if (form == "switch") {
		ValidateExpression(stmt["switch"], fmt::format("{}.switch", path));
		if (!stmt.contains("cases") || !stmt["cases"].is_object()) {
			Bad(path, R"(switch statement requires a "cases" object)");
		}
		for (auto& [caseValue, body] : stmt["cases"].items()) {
			ValidateStatementList(body, fmt::format("{}.cases.{}", path, caseValue));
		}
		if (auto d = stmt.find("default"); d != stmt.end()) {
			ValidateStatementList(*d, fmt::format("{}.default", path));
		}
	} else if (form == "for_each") {
		if (!stmt["for_each"].is_string()) {
			Bad(path, R"(field "for_each" must be a string (the loop-element variable name))");
		}
		if (!stmt.contains("in"))
		{
			Bad(path, R"(missing "in" expression (the collection))");
		}
		ValidateExpression(stmt["in"], fmt::format("{}.in", path));
		if (!stmt.contains("body"))
		{
			Bad(path, R"(missing "body" statement list)");
		}
		ValidateStatementList(stmt["body"], fmt::format("{}.body", path));
	} else if (form == "while") {
		ValidateExpression(stmt["while"], fmt::format("{}.while", path));
		if (!stmt.contains("body"))
		{
			Bad(path, R"(missing "body" statement list)");
		}
		ValidateStatementList(stmt["body"], fmt::format("{}.body", path));
	} else if (form == "sequence") {
		RequireArray(stmt["sequence"], fmt::format("{}.sequence", path));
		std::size_t i = 0;
		for (const auto& branch : stmt["sequence"]) {
			ValidateStatementList(branch, fmt::format("{}.sequence[{}]", path, i++));
		}
	} else if (form == "break" || form == "continue") {
		// Value of break/continue is conventionally null but ignored.
	} else if (form == "broadcast" ||
			   form == "bind_delegate" ||
			   form == "unbind_delegate" ||
			   form == "clear_delegate") {
		// Delegate ops carry the property name as the form's value and an
		// optional `target` expression (defaults to `self`).
		if (!stmt[form].is_string()) {
			Bad(path, fmt::format(
				R"(field "{}" must be a string (the delegate property name))", form));
		}
		if (auto tIt = stmt.find("target"); tIt != stmt.end()) {
			ValidateExpression(*tIt, fmt::format("{}.target", path));
		}
		if (form == "broadcast") {
			if (auto aIt = stmt.find("args"); aIt != stmt.end()) {
				if (!aIt->is_object())
				{
					Bad(path, R"(field "args" must be an object)");
				}
				for (auto& [pinName, valExpr] : aIt->items()) {
					ValidateExpression(valExpr,
						fmt::format("{}.args.{}", path, pinName));
				}
			}
		} else if (form == "bind_delegate" || form == "unbind_delegate") {
			// {bind_delegate: "<prop>", [target: <expr>], handler: "<fn>"}
			if (!stmt.contains("handler") || !stmt["handler"].is_string()) {
				Bad(path, fmt::format(
					R"({} requires a string "handler" field (the bound function name))",
					form));
			}
		}
	} else if (form == "unsupported") {
		const auto& u = stmt["unsupported"];
		if (!u.is_object()) {
			Bad(path, R"(field "unsupported" must be an object {node_class, guid?, reason?, fields?})");
		}
		if (!u.contains("node_class") || !u["node_class"].is_string()) {
			Bad(path, R"(unsupported statement requires a string "node_class" field)");
		}
	}
}

// ----- Top-level validators ----------------------------------------------

void ValidateVariableDecl(const nlohmann::json& v, std::string_view path) {
	RequireObject(v, path);
	RequireString(v, "name", path);
	RequireString(v, "type", path);
	// Optional fields validated when present:
	if (auto it = v.find("default"); it != v.end() && !it->is_string() && !it->is_null()) {
		Bad(path, R"(field "default" must be a string or null)");
	}
	if (auto it = v.find("category"); it != v.end() && !it->is_string()) {
		Bad(path, R"(field "category" must be a string)");
	}
	if (auto it = v.find("replicated"); it != v.end() && !it->is_boolean()) {
		Bad(path, R"(field "replicated" must be boolean)");
	}
	if (auto it = v.find("editable"); it != v.end() && !it->is_boolean()) {
		Bad(path, R"(field "editable" must be boolean)");
	}
}

void ValidateFunctionDoc(const nlohmann::json& doc, std::string_view path) {
	RequireObject(doc, path);
	RequireString(doc, "name", path);
	if (auto m = doc.find("metadata"); m != doc.end() && !m->is_object()) {
		Bad(path, R"(field "metadata" must be an object)");
	}
	auto checkVarList = [&](const char* key) {
		auto it = doc.find(key);
		if (it == doc.end())
		{
			return;
		}
		RequireArray(*it, fmt::format("{}.{}", path, key));
		std::size_t i = 0;
		for (const auto& v : *it) {
			ValidateVariableDecl(v, fmt::format("{}.{}[{}]", path, key, i++));
		}
	};
	checkVarList("inputs");
	checkVarList("outputs");
	checkVarList("locals");
	if (!doc.contains("body"))
	{
		Bad(path, R"(function doc requires a "body" array)");
	}
	ValidateStatementList(doc["body"], fmt::format("{}.body", path));
}

void ValidateClassDoc(const nlohmann::json& doc, std::string_view path) {
	RequireObject(doc, path);
	RequireString(doc, "name", path);
	if (auto it = doc.find("interfaces"); it != doc.end()) {
		RequireArray(*it, fmt::format("{}.interfaces", path));
		for (const auto& s : *it) {
			if (!s.is_string())
			{
				Bad(path, R"(interfaces[] entries must be strings)");
			}
		}
	}
	if (auto it = doc.find("variables"); it != doc.end()) {
		RequireArray(*it, fmt::format("{}.variables", path));
		std::size_t i = 0;
		for (const auto& v : *it) {
			ValidateVariableDecl(v, fmt::format("{}.variables[{}]", path, i++));
		}
	}
	if (auto it = doc.find("functions"); it != doc.end()) {
		RequireArray(*it, fmt::format("{}.functions", path));
		std::size_t i = 0;
		for (const auto& f : *it) {
			ValidateFunctionDoc(f, fmt::format("{}.functions[{}]", path, i++));
		}
	}
}

} // anonymous namespace

const std::vector<std::string>& StatementForms()  { return StatementFormsImpl(); }
const std::vector<std::string>& ExpressionForms() { return ExpressionFormsImpl(); }

std::string DetectStatementForm(const nlohmann::json& stmt) {
	return FirstMatchingKey(stmt, StatementFormsImpl());
}
std::string DetectExpressionForm(const nlohmann::json& expr) {
	return FirstMatchingKey(expr, ExpressionFormsImpl());
}

bool IsBpirFunction(const nlohmann::json& doc) {
	return doc.is_object() && doc.value("kind", "") == "function";
}
bool IsBpirClass(const nlohmann::json& doc) {
	return doc.is_object() && doc.value("kind", "") == "class";
}

void ValidateBpir(const nlohmann::json& doc) {
	RequireObject(doc, "");
	if (!doc.contains("version") || !doc["version"].is_number_integer()) {
		Bad("", R"(BPIR doc requires integer "version" field)");
	}
	int v = doc["version"].get<int>();
	if (v < 1 || v > kBpirSchemaVersion) {
		Bad("", fmt::format(
			"BPIR version {} not supported (this build accepts 1..{})",
			v, kBpirSchemaVersion));
	}
	if (!doc.contains("kind") || !doc["kind"].is_string()) {
		Bad("", R"(BPIR doc requires string "kind" field ("function" or "class"))");
	}
	const auto& kind = doc["kind"].get_ref<const std::string&>();
	if (kind == "function")
	{
		ValidateFunctionDoc(doc, "");
	}
	else if (kind == "class") ValidateClassDoc(doc, "");
	else Bad("", fmt::format(R"("kind" = "{}", expected "function" or "class")", kind));
}

nlohmann::json MigrateToCurrent(const nlohmann::json& doc) {
	// No-op until a breaking schema change lands. The seam is here so
	// adding a v2 path doesn't require touching every consumer.
	return doc;
}

}    // namespace bpr::tools
