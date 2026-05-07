#include "tools/CompileFunction.h"
#include "tools/ApplyOps.h"
#include "tools/TypeShorthand.h"

#include <fmt/core.h>

#include <stdexcept>
#include <string>

namespace bpr::tools {

namespace {

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
            for (auto& [k, v] : extras.items()) op[k] = v;
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

// Forward decl.
std::string CompileExpr(Compiler& c, const nlohmann::json& expr);

// Returns the slot id of a Branch node whose Condition pin is wired from
// the expression's value pin. Caller wires "Then"/"Else" exec out.
std::string CompileBranch(Compiler& c, const nlohmann::json& cond) {
    std::string condSlot = CompileExpr(c, cond);
    std::string branch = c.AddNode("Branch", "branch");
    // Wire the value pin of the expression node (we conventionally use
    // the first output data pin) to the Branch's Condition input. The
    // commandlet's wire_pins will surface a useful type-mismatch error
    // if `cond` doesn't return a bool.
    c.ops.push_back({
        {"op", "wire_pins"},
        {"asset_path", c.asset}, {"graph_name", c.graph},
        {"from_node", fmt::format("${}", condSlot)}, {"from_pin", "ReturnValue"},
        {"to_node",   fmt::format("${}", branch)},   {"to_pin",   "Condition"},
    });
    return branch;
}

// Compile a value-producing expression to a slot id. The slot's
// "ReturnValue" pin (or the first output data pin, by convention) holds
// the value. Throws on unrecognized forms.
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
        // We don't have a "literal node" in the K2 dispatcher today —
        // but a CustomEvent-spawned literal node is one option in v2.
        // For v1, document the limitation and ask the agent to hold
        // literals on default pin values via set_variable_default.
        throw std::invalid_argument(
            "literal expressions ({lit:...}) not yet supported in compile_function — "
            "model a literal as a const variable + {var:\"name\"} for now, or "
            "drop to apply_ops with an explicit add_node for the literal node kind");
    }
    if (auto it = expr.find("call"); it != expr.end()) {
        if (!it->is_string()) {
            throw std::invalid_argument(R"(expression "call" must be a string)");
        }
        std::string fn = it->get<std::string>();
        std::string owner;
        // Allow "Owner::Func" qualified names for cross-class calls.
        auto sep = fn.find("::");
        if (sep != std::string::npos) {
            owner = fn.substr(0, sep);
            fn    = fn.substr(sep + 2);
        }
        nlohmann::json extras = {{"function", fn}};
        if (!owner.empty()) extras["function_owner"] = owner;
        std::string slot = c.AddNode("CallFunction", "call", extras);
        // Wire args by pin-name match — best-effort; mismatches surface
        // through the wire_pins type-aware error.
        if (auto argsIt = expr.find("args");
            argsIt != expr.end() && argsIt->is_object()) {
            for (auto& [pinName, valExpr] : argsIt->items()) {
                std::string argSlot = CompileExpr(c, valExpr);
                c.ops.push_back({
                    {"op", "wire_pins"},
                    {"asset_path", c.asset}, {"graph_name", c.graph},
                    {"from_node", fmt::format("${}", argSlot)},
                    {"from_pin", "ReturnValue"},
                    {"to_node", fmt::format("${}", slot)},
                    {"to_pin", pinName},
                });
            }
        }
        return slot;
    }
    throw std::invalid_argument(fmt::format(
        "unrecognized expression form. Supported: {{var:\"name\"}}, "
        "{{lit:value}}, {{call:\"fn\", args:{{...}}}}. Got: {}",
        expr.dump()));
}

// Compile a list of statements into nodes + wires, threading exec from
// `entryExecOut` (a "<slot>:<pin>" anchor) down through the statements.
// Returns the new exec tail "<slot>:<pin>" after the list — caller wires
// to whatever comes next.
struct ExecTail {
    std::string slot;
    std::string pin;
    bool valid() const { return !slot.empty(); }
};

ExecTail CompileStatements(Compiler& c, ExecTail tail,
                           const nlohmann::json& stmts);

ExecTail CompileStatement(Compiler& c, ExecTail tail,
                          const nlohmann::json& stmt) {
    if (!stmt.is_object()) {
        throw std::invalid_argument(
            "statement must be an object — supported forms: "
            "{if, then, [else]}, {set,to}, {call,args}, {comment}");
    }
    if (stmt.contains("comment")) {
        // Comments don't generate nodes; just a no-op for now. (A v2
        // could attach the comment to the next-spawned node's metadata.)
        return tail;
    }
    if (stmt.contains("if")) {
        std::string branch = CompileBranch(c, stmt["if"]);
        if (tail.valid()) c.WireExec(tail.slot, tail.pin, branch, "execute");
        // Compile then/else.
        ExecTail thenTail = {branch, "then"};
        if (auto t = stmt.find("then"); t != stmt.end()) {
            thenTail = CompileStatements(c, thenTail, *t);
        }
        ExecTail elseTail = {branch, "else"};
        if (auto e = stmt.find("else"); e != stmt.end()) {
            elseTail = CompileStatements(c, elseTail, *e);
        }
        // After a Branch, exec tail is ambiguous — can't merge two
        // exec paths in a BP without a Join node, which doesn't exist
        // by default. v1 convention: the next statement attaches to the
        // `then` branch's tail. The else branch's tail is left dangling
        // (the agent can wire it explicitly via apply_ops if needed).
        // Document this in the tool description.
        return thenTail;
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
        // Wire the value pin into the SetVar's data input (pin name
        // matches the variable name in K2).
        c.ops.push_back({
            {"op", "wire_pins"},
            {"asset_path", c.asset}, {"graph_name", c.graph},
            {"from_node", fmt::format("${}", valSlot)}, {"from_pin", "ReturnValue"},
            {"to_node", fmt::format("${}", setSlot)},   {"to_pin", varName},
        });
        // Wire exec.
        if (tail.valid()) c.WireExec(tail.slot, tail.pin, setSlot, "execute");
        return ExecTail{setSlot, "then"};
    }
    if (stmt.contains("call")) {
        if (!stmt["call"].is_string()) {
            throw std::invalid_argument(R"(statement "call" must be a string)");
        }
        std::string fn = stmt["call"].get<std::string>();
        std::string owner;
        auto sep = fn.find("::");
        if (sep != std::string::npos) {
            owner = fn.substr(0, sep);
            fn    = fn.substr(sep + 2);
        }
        nlohmann::json extras = {{"function", fn}};
        if (!owner.empty()) extras["function_owner"] = owner;
        std::string slot = c.AddNode("CallFunction", "callstmt", extras);
        // Wire args.
        if (auto argsIt = stmt.find("args");
            argsIt != stmt.end() && argsIt->is_object()) {
            for (auto& [pinName, valExpr] : argsIt->items()) {
                std::string argSlot = CompileExpr(c, valExpr);
                c.ops.push_back({
                    {"op", "wire_pins"},
                    {"asset_path", c.asset}, {"graph_name", c.graph},
                    {"from_node", fmt::format("${}", argSlot)}, {"from_pin", "ReturnValue"},
                    {"to_node", fmt::format("${}", slot)},      {"to_pin", pinName},
                });
            }
        }
        if (tail.valid()) c.WireExec(tail.slot, tail.pin, slot, "execute");
        return ExecTail{slot, "then"};
    }
    throw std::invalid_argument(fmt::format(
        "unrecognized statement form. Supported: {{if,then,[else]}}, "
        "{{set,to}}, {{call,args}}, {{comment}}. Got: {}", stmt.dump()));
}

ExecTail CompileStatements(Compiler& c, ExecTail tail,
                           const nlohmann::json& stmts) {
    if (!stmts.is_array()) {
        throw std::invalid_argument("statement block must be an array");
    }
    for (const auto& s : stmts) {
        tail = CompileStatement(c, tail, s);
    }
    return tail;
}

} // namespace

void RegisterCompileFunction(ToolRegistry& registry,
                             backends::IBlueprintReader& reader) {
    ToolDescriptor d;
    d.name = "compile_function";
    d.description =
        "Compile a tiny pseudocode DSL into a fully-wired BP function. "
        "Accepts the function signature plus a `body` of statements; "
        "translates to add_node + wire_pins ops and runs them as one "
        "batch.\n\n"
        "Statements: {if, then, [else]}, {set, to}, {call, args}, {comment}.\n"
        "Expressions: {var:\"name\"}, {call:\"fn\", args:{...}}.\n\n"
        "Limitations (v1):\n"
        "  - {lit:value} not yet supported — model literals as const vars.\n"
        "  - After an if/then/else, exec continues from the `then` branch's "
        "    tail; the `else` tail is left dangling. Use apply_ops + explicit "
        "    Sequence/Join wiring if you need both branches to merge.\n"
        "  - Each underlying op still saves+recompiles individually (same "
        "    limitation as apply_ops). True single-recompile is plugin work.\n\n"
        "On expression/statement form errors, the response says exactly "
        "which form was unrecognized so the agent can fall back to apply_ops "
        "for that statement only.";
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

        // Step 1: ensure function exists. Idempotent.
        c.ops.push_back({
            {"op", "add_function"},
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

        // Step 3: compile the body. Entry exec tail = the function's
        // entry node's "then" pin. We use the well-known FunctionEntry
        // node's "then" pin name; mismatch surfaces via wire_pins.
        // For v1, we leave entry-pin wiring to the agent — they can call
        // wire_pins explicitly afterwards if their first statement
        // doesn't connect automatically. (Most real BP functions don't
        // need this because UE auto-wires the entry's exec to the first
        // statement on compile if the path is unbroken, but our isolated
        // batch can't rely on that.)
        ExecTail tail{};
        CompileStatements(c, tail, args["body"]);

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

} // namespace bpr::tools
