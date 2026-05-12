#include "tools/Decompile.h"
#include "tools/Bpir.h"

#include <fmt/core.h>

#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace bpr::tools {

namespace {

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
        if (t.IsArray) return std::string("[]") + inner;
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
        if (sub == "double") return wrapContainer("double");
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
    if (v.DefaultValue && !v.DefaultValue->empty()) j["default"]    = *v.DefaultValue;
    if (v.Category && !v.Category->empty())        j["category"]    = *v.Category;
    if (v.IsReplicated)                            j["replicated"]  = true;
    if (v.IsEditable)                              j["editable"]    = true;
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
            if (p.Name == name) return &p;
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
        if (!out) return nullptr;
        auto it = outEdges.find({n.Id, out->Id});
        if (it == outEdges.end() || it->second.empty()) return nullptr;
        const auto& target = it->second.front();
        return GetNode(target.node);
    }

    // Tag a variable name with its scope. Inputs/outputs come from the
    // function's signature; locals from `function.Locals`; otherwise
    // it's a member.
    std::string ScopeForVariable(const std::string& name) const {
        for (const auto& v : function.Inputs)  if (v.Name == name) return "input";
        for (const auto& v : function.Outputs) if (v.Name == name) return "output";
        for (const auto& v : function.Locals)  if (v.Name == name) return "local";
        if (memberVarNames.count(name)) return "member";
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
    if (producer.Class.find("K2Node_VariableGet") != std::string::npos) {
        std::string varName;
        if (producer.Meta.is_object()) {
            varName = producer.Meta.value("variableName", std::string{});
        }
        nlohmann::json out = {{"var", varName}};
        std::string scope = w.ScopeForVariable(varName);
        if (!scope.empty()) out["scope"] = scope;
        return out;
    }
    if (producer.Class.find("K2Node_Self") != std::string::npos) {
        return nlohmann::json{{"self", nullptr}};
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
            if (p.Direction != "Input" || p.Type.Category == "exec") continue;
            if (p.Name == "self") continue;  // implicit; codegen handles
            args[p.Name] = BuildExpression(w, producer, p);
        }
        if (!args.empty()) out["args"] = std::move(args);
        return out;
    }
    if (producer.Class.find("K2Node_MakeArray") != std::string::npos) {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& p : producer.Pins) {
            if (p.Direction != "Input" || p.Type.Category == "exec") continue;
            arr.push_back(BuildExpression(w, producer, p));
        }
        return nlohmann::json{{"new_array", std::move(arr)}};
    }
    if (producer.Class.find("K2Node_MakeStruct") != std::string::npos) {
        std::string structType;
        if (producer.Meta.is_object()) {
            structType = producer.Meta.value("structType", std::string{});
        }
        nlohmann::json fields = nlohmann::json::object();
        for (const auto& p : producer.Pins) {
            if (p.Direction != "Input" || p.Type.Category == "exec") continue;
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
    if (!producer) return LiteralFromDefault(consumerPin);
    auto pIt = w.pinNameById.find(src.pin);
    const std::string& outPinName = pIt != w.pinNameById.end() ? pIt->second : std::string{};
    const BPPin* outPin = w.GetPin(*producer, outPinName);
    if (!outPin) return LiteralFromDefault(consumerPin);
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
    const BPNode* next = nullptr;
    bool terminatesExec = false;  // return / break / continue / dangling
};

DecompileResult DecompileStatement(const Walker& w, const BPNode& n,
                                   const BPNode* stopAt,
                                   std::set<std::string>& visited);

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
        // Skip the no-op pseudo-statement that DecompileStatement uses
        // to mean "this node is structural-only" (FunctionEntry).
        if (!res.statement.is_null() &&
            !(res.statement.is_object() && res.statement.empty())) {
            out.push_back(std::move(res.statement));
        }
        if (res.terminatesExec) break;
        cur = res.next;
    }
    return out;
}

DecompileResult DecompileStatement(const Walker& w, const BPNode& n,
                                   const BPNode* stopAt,
                                   std::set<std::string>& visited) {
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
            if (p.Direction != "Input" || p.Type.Category == "exec") continue;
            returns.push_back(BuildExpression(w, n, p));
        }
        if (returns.empty())      r.statement = nlohmann::json{{"return", nullptr}};
        else if (returns.size() == 1) r.statement = nlohmann::json{{"return", returns[0]}};
        else                          r.statement = nlohmann::json{{"return", returns}};
        r.terminatesExec = true;
        return r;
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
        if (!scope.empty()) stmt["scope"] = scope;
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
                if (!cur) break;
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
            if (p.Direction != "Output" || p.Type.Category != "exec") continue;
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
            if (p.Direction != "Input" || p.Type.Category == "exec") continue;
            args[p.Name] = BuildExpression(w, n, p);
        }
        nlohmann::json stmt = {{"call", "__bpr_spawn_actor_from_class"}};
        if (!args.empty()) stmt["args"] = std::move(args);
        r.statement = std::move(stmt);
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
        if (!args.empty()) stmt["args"] = std::move(args);
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
        if (!selPin) selPin = w.GetPin(n, "TargetExpression");
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
            if (p.Direction != "Output" || p.Type.Category != "exec") continue;
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
        if (hasDefault) stmt["default"] = defaultBody;
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
            if (p.Direction != "Input" || p.Type.Category == "exec") continue;
            args[p.Name] = BuildExpression(w, n, p);
        }
        nlohmann::json stmt = {{"call", "__bpr_add_component"}};
        if (!args.empty()) stmt["args"] = std::move(args);
        r.statement = std::move(stmt);
        r.next = w.FollowExec(n, "then");
        return r;
    }

    // CallFunction (statement form — return value, if any, is unused).
    if (n.Class.find("K2Node_CallFunction") != std::string::npos) {
        std::string fnName, ownerCl;
        if (n.Meta.is_object()) {
            fnName  = n.Meta.value("targetFunction", std::string{});
            ownerCl = n.Meta.value("targetClass",    std::string{});
        }
        std::string fqName = ownerCl.empty() ? fnName : (ownerCl + "::" + fnName);
        nlohmann::json args = nlohmann::json::object();
        for (const auto& p : n.Pins) {
            if (p.Direction != "Input" || p.Type.Category == "exec") continue;
            if (p.Name == "self") continue;
            args[p.Name] = BuildExpression(w, n, p);
        }
        nlohmann::json stmt = {{"call", fqName}};
        if (!args.empty()) stmt["args"] = std::move(args);
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
    if (n.Meta.is_object()) fields = n.Meta;
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
        if (n.Class.find("K2Node_FunctionEntry") != std::string::npos) return &n;
        if (n.Class.find("K2Node_Event") != std::string::npos)         return &n;
    }
    // Fallback: first node with an outgoing exec edge.
    for (const auto& n : g.Nodes) {
        for (const auto& p : n.Pins) {
            if (p.Direction == "Output" && p.Type.Category == "exec") return &n;
        }
    }
    return nullptr;
}

} // namespace

nlohmann::json DecompileFunction(backends::IBlueprintReader& reader,
                                 std::string_view assetPath,
                                 std::string_view functionName) {
    BPMetadata meta = reader.ReadBlueprint(assetPath);
    BPFunction fn   = reader.GetFunction(assetPath, functionName);

    std::set<std::string> memberVarNames;
    for (const auto& v : meta.Variables) memberVarNames.insert(v.Name);

    Walker walker(fn.Graph, fn, memberVarNames);
    const BPNode* entry = FindFunctionEntry(fn.Graph);
    nlohmann::json body;
    nlohmann::json unsupportedSummary = nlohmann::json::array();

    if (!entry) {
        body = nlohmann::json::array();
    } else {
        std::set<std::string> visited;
        const BPNode* startsAt = walker.FollowExec(*entry, "then");
        body = DecompileStatementsFrom(walker, startsAt, nullptr, visited);
    }

    // Walk the resulting body to collect unsupported-node summary at
    // the top level for quick "what couldn't I represent?" agent reads.
    std::function<void(const nlohmann::json&)> collectUnsupported =
        [&](const nlohmann::json& v) {
            if (v.is_array()) {
                for (const auto& el : v) collectUnsupported(el);
            } else if (v.is_object()) {
                if (v.contains("unsupported")) {
                    unsupportedSummary.push_back(v["unsupported"]);
                }
                for (auto& [k, child] : v.items()) collectUnsupported(child);
            }
        };
    collectUnsupported(body);

    // Build inputs/outputs/locals from the function spec.
    auto varListToJson = [](const std::vector<BPVariable>& vs) {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& v : vs) arr.push_back(VariableDeclToJson(v));
        return arr;
    };

    nlohmann::json doc = {
        {"version", kBpirSchemaVersion},
        {"kind", "function"},
        {"name", fn.Name},
        {"metadata", {
            {"asset_path", std::string(assetPath)},
        }},
        {"inputs",  varListToJson(fn.Inputs)},
        {"outputs", varListToJson(fn.Outputs)},
        {"locals",  varListToJson(fn.Locals)},
        {"body",    std::move(body)},
    };
    if (!unsupportedSummary.empty()) {
        doc["unsupported_nodes"] = std::move(unsupportedSummary);
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
    for (const auto& v : meta.Variables) variables.push_back(VariableDeclToJson(v));

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

    nlohmann::json interfaces = nlohmann::json::array();
    for (const auto& i : meta.Interfaces) interfaces.push_back(i);

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
    };
    ValidateBpir(doc);
    return doc;
}

} // namespace bpr::tools
