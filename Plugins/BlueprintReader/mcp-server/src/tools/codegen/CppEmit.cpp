#include "tools/codegen/CppEmit.h"
#include "tools/Bpir.h"
#include "tools/codegen/UnsupportedTreatment.h"

#include <fmt/core.h>

#include <map>
#include <sstream>
#include <stdexcept>
#include <string>

namespace bpr::tools {

namespace {

// ----- Type shorthand → C++ -----------------------------------------------
// Inverse of TypeShorthand.cpp's parser. Bidirectional with a caveat:
// some BPPinType shapes don't have a clean shorthand round-trip
// (multi-level template types), but the common surface does.
//
// `forMember` toggles UE5's TObjectPtr<> wrapping for class-member
// UObject* fields — Epic recommends since 5.0 and the editor reflects
// pointer fields more efficiently when wrapped. Function args and
// local pointers stay raw (TObjectPtr is only for member properties).

std::string MapInner(std::string_view inner, bool forMember);

std::string MapTypeRecursive(std::string_view t, bool forMember) {
    // Container prefix dispatch.
    if (t.size() >= 2 && t[0] == '[' && t[1] == ']') {
        // TArray of pointers inside a UPROPERTY also benefits from
        // TObjectPtr at the element level — propagate the flag.
        return std::string("TArray<") + MapTypeRecursive(t.substr(2), forMember) + ">";
    }
    // {}T form (set, post-prefix): two-char prefix + rest.
    if (t.size() >= 2 && t[0] == '{' && t[1] == '}') {
        return std::string("TSet<") + MapTypeRecursive(t.substr(2), forMember) + ">";
    }
    // {K:V} or {T} form (the matching-brace variants TypeShorthand also accepts).
    if (t.size() >= 2 && t[0] == '{' && t.back() == '}') {
        std::string_view inner = t.substr(1, t.size() - 2);
        auto colon = inner.find(':');
        if (colon == std::string_view::npos) {
            return std::string("TSet<") + MapTypeRecursive(inner, forMember) + ">";
        }
        return std::string("TMap<") +
               MapTypeRecursive(inner.substr(0, colon), forMember) + ", " +
               MapTypeRecursive(inner.substr(colon + 1), forMember) + ">";
    }
    return MapInner(t, forMember);
}

std::string MapInner(std::string_view inner, bool forMember) {
    auto colon = inner.find(':');
    std::string_view name = (colon == std::string_view::npos) ? inner : inner.substr(0, colon);
    std::string_view sub  = (colon == std::string_view::npos) ? std::string_view{} : inner.substr(colon + 1);

    if (name == "bool")    return "bool";
    if (name == "byte")    return "uint8";
    if (name == "int")     return "int32";
    if (name == "int64")   return "int64";
    if (name == "float")   return "float";
    if (name == "real")    return "float";    // BP "real" defaults to float without subcategory
    if (name == "double")  return "double";
    if (name == "string")  return "FString";
    if (name == "name")    return "FName";
    if (name == "text")    return "FText";
    if (name == "exec")    return "void";

    if (name == "object" || name == "soft_object") {
        // Helper: render the bare class name with its UE prefix.
        auto renderBare = [](std::string_view bare) -> std::string {
            // Already-prefixed: A/U followed by another uppercase letter.
            if (bare.size() >= 2 &&
                (bare[0] == 'A' || bare[0] == 'U') &&
                (bare[1] >= 'A' && bare[1] <= 'Z')) {
                return std::string(bare);
            }
            // Actor-derived heuristic.
            if (bare == "Actor" ||
                (bare.size() > 5 && bare.substr(bare.size() - 5) == "Actor") ||
                bare == "Pawn" || bare == "Character" ||
                bare == "Controller" || bare == "PlayerController" ||
                bare == "PlayerState" || bare == "GameMode" ||
                bare == "GameState" || bare == "HUD") {
                return std::string("A") + std::string(bare);
            }
            return std::string("U") + std::string(bare);
        };

        // Wrap rendered class with TObjectPtr<> for member context,
        // bare-pointer for arg / local context.
        auto wrap = [forMember](const std::string& cls) {
            return forMember
                ? std::string("TObjectPtr<") + cls + ">"
                : cls + "*";
        };

        if (sub.empty()) return wrap("UObject");

        // Strip /Script/Module.ClassName path prefix down to bare
        // ClassName.
        std::string_view bare = sub;
        if (auto dot = bare.find_last_of('.'); dot != std::string_view::npos) {
            bare = bare.substr(dot + 1);
        }
        // Strip the BP "_C" suffix UE serializes for generated classes.
        if (bare.size() > 2 &&
            bare.substr(bare.size() - 2) == "_C") {
            bare = bare.substr(0, bare.size() - 2);
        }
        return wrap(renderBare(bare));
    }
    if (name == "class" || name == "soft_class") {
        if (sub.empty()) return "UClass*";
        return std::string("TSubclassOf<") + std::string(sub) + ">";
    }
    if (name == "interface") {
        return std::string("TScriptInterface<I") + std::string(sub) + ">";
    }
    if (name == "struct") {
        if (sub.empty()) return "FStruct";
        if (!sub.empty() && sub[0] == 'F') return std::string(sub);
        return std::string("F") + std::string(sub);
    }
    return std::string(name);  // unknown — pass through
}

// ----- Operator-alias reverse map ----------------------------------------
// The forward direction (CompileFunction.cpp) maps "+" → "Add_IntInt".
// We reverse it here. When `useOperatorAliases` is on and the qualified
// call name matches one of these, render as `lhs OP rhs` instead of
// `Owner::Func(lhs, rhs)`.

struct OpReverse {
    std::string op;
    int arity;  // 1 or 2
};
const std::map<std::string, OpReverse>& OpReverseMap() {
    static const std::map<std::string, OpReverse> m = {
        {"KismetMathLibrary::Add_IntInt",        {"+",  2}},
        {"KismetMathLibrary::Subtract_IntInt",   {"-",  2}},
        {"KismetMathLibrary::Multiply_IntInt",   {"*",  2}},
        {"KismetMathLibrary::Divide_IntInt",     {"/",  2}},
        {"KismetMathLibrary::Percent_IntInt",    {"%",  2}},
        {"KismetMathLibrary::EqualEqual_IntInt", {"==", 2}},
        {"KismetMathLibrary::NotEqual_IntInt",   {"!=", 2}},
        {"KismetMathLibrary::Less_IntInt",       {"<",  2}},
        {"KismetMathLibrary::LessEqual_IntInt",  {"<=", 2}},
        {"KismetMathLibrary::Greater_IntInt",    {">",  2}},
        {"KismetMathLibrary::GreaterEqual_IntInt",{">=", 2}},
        {"KismetMathLibrary::BooleanAND",        {"&&", 2}},
        {"KismetMathLibrary::BooleanOR",         {"||", 2}},
        {"KismetMathLibrary::Not_PreBool",       {"!",  1}},
        {"KismetMathLibrary::Add_FloatFloat",    {"+",  2}},
        {"KismetMathLibrary::Subtract_FloatFloat",{"-", 2}},
        {"KismetMathLibrary::Multiply_FloatFloat",{"*", 2}},
        {"KismetMathLibrary::Divide_FloatFloat", {"/",  2}},
        {"KismetMathLibrary::EqualEqual_FloatFloat",{"==", 2}},
        {"KismetMathLibrary::Less_FloatFloat",   {"<",  2}},
        {"KismetMathLibrary::LessEqual_FloatFloat",{"<=", 2}},
    };
    return m;
}

// ----- Emitter state ------------------------------------------------------
struct Emitter {
    std::ostringstream out;
    int indentLevel = 0;
    CppEmitOptions opts;
    nlohmann::json notes = nlohmann::json::array();

    void Indent() {
        for (int i = 0; i < indentLevel * opts.indentSpaces; ++i) out << ' ';
    }
    void Line(std::string_view s) { Indent(); out << s << "\n"; }
    void RawLine(std::string_view s) { out << s << "\n"; }

    // ----- Expression emitters ------------------------------------------
    std::string EmitExpr(const nlohmann::json& e) {
        std::string form = DetectExpressionForm(e);
        if (form == "var") {
            return e["var"].get<std::string>();
        }
        if (form == "lit") {
            return EmitLit(e["lit"]);
        }
        if (form == "call") {
            return EmitCallExpr(e);
        }
        if (form == "cast") {
            std::string target = e.value("to", "");
            std::string inner = EmitExpr(e["cast"]);
            return fmt::format("Cast<{}>({})", target, inner);
        }
        if (form == "member") {
            return fmt::format("{}.{}", EmitExpr(e["member"]), e.value("name", ""));
        }
        if (form == "index") {
            return fmt::format("{}[{}]", EmitExpr(e["index"]), EmitExpr(e["idx"]));
        }
        if (form == "self") {
            return "this";
        }
        if (form == "new_array") {
            std::string s = "{";
            bool first = true;
            for (const auto& el : e["new_array"]) {
                if (!first) s += ", ";
                first = false;
                s += EmitExpr(el);
            }
            s += "}";
            return s;
        }
        if (form == "new_struct") {
            std::string type = e.value("new_struct", "");
            std::string s = type + "{";
            if (e.contains("fields") && e["fields"].is_object()) {
                bool first = true;
                for (auto& [k, v] : e["fields"].items()) {
                    if (!first) s += ", ";
                    first = false;
                    s += fmt::format("/*{}=*/{}", k, EmitExpr(v));
                }
            }
            s += "}";
            return s;
        }
        // Should never happen if validation passed.
        return "/*<unknown-expr>*/";
    }

    std::string EmitLit(const nlohmann::json& v) {
        if (v.is_null())    return "nullptr";
        if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
        if (v.is_number_integer()) return std::to_string(v.get<long long>());
        if (v.is_number())  return fmt::format("{}f", v.get<double>());  // float literal
        if (v.is_string()) {
            const auto& s = v.get_ref<const std::string&>();
            // If the string starts with /* it's a sentinel comment from
            // decompile (unsupported expression); pass through as-is.
            if (s.find("/*") == 0) return s;
            // Otherwise quote it as a C++ string literal.
            return fmt::format("TEXT(\"{}\")", s);
        }
        return "/*<unknown-lit>*/";
    }

    std::string EmitCallExpr(const nlohmann::json& e) {
        std::string fnName = e.value("call", "");
        // Operator alias?
        if (opts.useOperatorAliases) {
            auto it = OpReverseMap().find(fnName);
            if (it != OpReverseMap().end()) {
                const auto& op = it->second;
                std::vector<std::string> argStrs;
                if (e.contains("args") && e["args"].is_object()) {
                    for (auto& [_, v] : e["args"].items()) {
                        argStrs.push_back(EmitExpr(v));
                    }
                }
                if (op.arity == 1 && argStrs.size() >= 1) {
                    return fmt::format("({}{})", op.op, argStrs[0]);
                }
                if (op.arity == 2 && argStrs.size() >= 2) {
                    return fmt::format("({} {} {})", argStrs[0], op.op, argStrs[1]);
                }
            }
        }
        // Sentinel-name lowering: Decompile emits structured calls for
        // a few K2 nodes that don't have a 1:1 BP function name. We
        // render them as the actual UE-side syntax here so the output
        // compiles without manual fixups.
        if (fnName == "__bpr_spawn_actor_from_class") {
            return EmitSpawnActorFromClass(e);
        }
        if (fnName == "__bpr_add_component") {
            return EmitAddComponent(e);
        }
        if (fnName == "__bpr_destroy_actor") {
            return EmitDestroyActor(e);
        }
        // Default: render as Foo(a, b, c). If the name is qualified
        // (Owner::Func), keep the qualifier — the agent / user resolves
        // include + scope.
        std::string args;
        if (e.contains("args") && e["args"].is_object()) {
            bool first = true;
            for (auto& [_, v] : e["args"].items()) {
                if (!first) args += ", ";
                first = false;
                args += EmitExpr(v);
            }
        }
        return fmt::format("{}({})", fnName, args);
    }

    // SpawnActorFromClass → `GetWorld()->SpawnActor<AActor>(Class,
    // SpawnTransform, SpawnParameters)`. We pull whichever input pins
    // are present from `args`; missing pins fall back to UE defaults.
    // Class is the only required arg — without it the call is
    // ill-formed and we surface that as a TODO comment so the user can
    // fix.
    std::string EmitSpawnActorFromClass(const nlohmann::json& e) {
        auto a = (e.contains("args") && e["args"].is_object())
                     ? e["args"] : nlohmann::json::object();
        std::string clsExpr  = a.contains("Class")
                                   ? EmitExpr(a["Class"]) : std::string("nullptr");
        std::string xform    = a.contains("SpawnTransform")
                                   ? EmitExpr(a["SpawnTransform"])
                                   : std::string("FTransform::Identity");
        // Optional: Owner / Instigator wrapped in SpawnParameters.
        std::string owner    = a.contains("Owner")      ? EmitExpr(a["Owner"])      : "";
        std::string instig   = a.contains("Instigator") ? EmitExpr(a["Instigator"]) : "";
        std::string collison = a.contains("CollisionHandlingOverride")
                                   ? EmitExpr(a["CollisionHandlingOverride"]) : "";
        if (owner.empty() && instig.empty() && collison.empty()) {
            return fmt::format(
                "GetWorld()->SpawnActor<AActor>({}, {})", clsExpr, xform);
        }
        // With SpawnParameters: emit a ([&] { ... }()) immediately-invoked
        // lambda so the call slots into a single expression position.
        std::string params;
        if (!owner.empty())    params += fmt::format("    p.Owner = {};\n", owner);
        if (!instig.empty())   params += fmt::format("    p.Instigator = {};\n", instig);
        if (!collison.empty()) params += fmt::format(
            "    p.SpawnCollisionHandlingOverride = {};\n", collison);
        return fmt::format(
            "[&]{{ FActorSpawnParameters p;\n{}    return GetWorld()->SpawnActor<AActor>({}, {}, p); }}()",
            params, clsExpr, xform);
    }

    // DestroyActor → `Target->Destroy()` (or `this->Destroy()` when
    // Target is absent). The BP node's exec is statement-context, so
    // CppEmit invokes this from a statement-form CallExpr; we render the
    // expression and the caller drops a trailing semicolon.
    std::string EmitDestroyActor(const nlohmann::json& e) {
        auto a = (e.contains("args") && e["args"].is_object())
                     ? e["args"] : nlohmann::json::object();
        if (a.contains("Target")) {
            std::string tgt = EmitExpr(a["Target"]);
            // Self-target collapses to `this->Destroy()`; literal "this"
            // shows up if BuildExpression emitted a `{self:nullptr}` shape
            // upstream. Either way the rendering is the same.
            if (tgt == "this") return "this->Destroy()";
            return fmt::format("{}->Destroy()", tgt);
        }
        return "this->Destroy()";
    }

    // AddComponent → `NewObject<UActorComponent>(this) + RegisterComponent`.
    // Decompile carries TemplateName / RelativeTransform on the args;
    // we render a small block that creates + attaches + registers.
    // Returns the component pointer so the call slots into an
    // expression position.
    std::string EmitAddComponent(const nlohmann::json& e) {
        auto a = (e.contains("args") && e["args"].is_object())
                     ? e["args"] : nlohmann::json::object();
        std::string xform = a.contains("RelativeTransform")
                                ? EmitExpr(a["RelativeTransform"])
                                : std::string("FTransform::Identity");
        std::string templateName = a.contains("TemplateName")
                                       ? EmitExpr(a["TemplateName"])
                                       : std::string("NAME_None");
        // The template type isn't carried as a pin (it's stored on the
        // node as a class reference). Leave as UActorComponent — the
        // agent retypes when copying into real source.
        return fmt::format(
            "[&]{{ auto* Comp = NewObject<UActorComponent>(this, {});\n"
            "    if (auto* Scene = Cast<USceneComponent>(Comp)) {{\n"
            "        Scene->SetRelativeTransform({});\n"
            "    }}\n"
            "    Comp->RegisterComponent();\n"
            "    return Comp; }}()", templateName, xform);
    }

    // ----- Statement emitters -------------------------------------------
    void EmitStatementList(const nlohmann::json& stmts) {
        for (const auto& s : stmts) EmitStatement(s);
    }

    void EmitStatement(const nlohmann::json& s) {
        std::string form = DetectStatementForm(s);
        if (form == "comment") {
            Line(fmt::format("// {}", s.value("comment", "")));
            return;
        }
        if (form == "if") {
            std::string cond = EmitExpr(s["if"]);
            Line(fmt::format("if ({}) {{", cond));
            ++indentLevel;
            if (s.contains("then")) EmitStatementList(s["then"]);
            --indentLevel;
            if (s.contains("else") && !s["else"].empty()) {
                Line("} else {");
                ++indentLevel;
                EmitStatementList(s["else"]);
                --indentLevel;
            }
            Line("}");
            return;
        }
        if (form == "set") {
            std::string varName = s.value("set", "");
            std::string val = EmitExpr(s["to"]);
            Line(fmt::format("{} = {};", varName, val));
            return;
        }
        if (form == "call") {
            // Statement form — discard return value.
            std::string call = EmitCallExpr(s);
            // Strip the surrounding parens if it's an operator (rare in
            // statement context but possible).
            if (!call.empty() && call.front() == '(' && call.back() == ')') {
                call = call.substr(1, call.size() - 2);
            }
            Line(fmt::format("{};", call));
            return;
        }
        if (form == "return") {
            const auto& r = s["return"];
            if (r.is_null())          { Line("return;"); }
            else if (r.is_array()) {
                if (r.empty()) {
                    Line("return;");
                } else {
                    // Multi-return: emit `return std::make_tuple(...)`.
                    // The agent gets to choose whether to refactor to OUT params.
                    std::string parts;
                    bool first = true;
                    for (const auto& el : r) {
                        if (!first) parts += ", ";
                        first = false;
                        parts += EmitExpr(el);
                    }
                    Line(fmt::format("return std::make_tuple({});", parts));
                }
            } else {
                Line(fmt::format("return {};", EmitExpr(r)));
            }
            return;
        }
        if (form == "cast") {
            std::string castVal = EmitExpr(s["cast"]);
            std::string target  = s.value("to", "");
            std::string asName  = s.value("as", "AsCast");
            Line(fmt::format("if (auto* {} = Cast<{}>({})) {{", asName, target, castVal));
            ++indentLevel;
            EmitStatementList(s["success"]);
            --indentLevel;
            if (s.contains("fail") && !s["fail"].empty()) {
                Line("} else {");
                ++indentLevel;
                EmitStatementList(s["fail"]);
                --indentLevel;
            }
            Line("}");
            return;
        }
        if (form == "switch") {
            Line(fmt::format("switch ({}) {{", EmitExpr(s["switch"])));
            ++indentLevel;
            if (s.contains("cases") && s["cases"].is_object()) {
                for (auto& [val, body] : s["cases"].items()) {
                    Line(fmt::format("case {}: {{", val));
                    ++indentLevel;
                    EmitStatementList(body);
                    Line("break;");
                    --indentLevel;
                    Line("}");
                }
            }
            if (s.contains("default")) {
                Line("default: {");
                ++indentLevel;
                EmitStatementList(s["default"]);
                Line("break;");
                --indentLevel;
                Line("}");
            }
            --indentLevel;
            Line("}");
            return;
        }
        if (form == "for_each") {
            std::string elem = s.value("for_each", "");
            std::string in   = EmitExpr(s["in"]);
            Line(fmt::format("for (auto& {} : {}) {{", elem, in));
            ++indentLevel;
            EmitStatementList(s["body"]);
            --indentLevel;
            Line("}");
            return;
        }
        if (form == "while") {
            Line(fmt::format("while ({}) {{", EmitExpr(s["while"])));
            ++indentLevel;
            EmitStatementList(s["body"]);
            --indentLevel;
            Line("}");
            return;
        }
        if (form == "sequence") {
            // Emit each branch as its own block, in order. Comments
            // around each branch make the structure obvious.
            int idx = 0;
            for (const auto& branch : s["sequence"]) {
                Line(fmt::format("// sequence branch {}", idx++));
                EmitStatementList(branch);
            }
            return;
        }
        if (form == "break")    { Line("break;");    return; }
        if (form == "continue") { Line("continue;"); return; }
        if (form == "unsupported") {
            const auto& u = s["unsupported"];
            std::string nodeClass = u.value("node_class", "?");
            std::string guid      = u.value("guid", "");
            // Classify the node and emit a richer treatment — best-effort
            // C++ stub when we know the pattern, else a TODO. Either way,
            // the sidecar entry captures the note so the agent can iterate
            // over manual steps.
            auto cls = ClassifyUnsupported(u);
            if (cls.kind == UnsupportedClassification::Kind::Approximation &&
                !cls.snippet.empty()) {
                // Snippet may span multiple lines — emit each at the
                // current indent.
                std::size_t pos = 0;
                while (pos < cls.snippet.size()) {
                    auto nl = cls.snippet.find('\n', pos);
                    std::string lineText = cls.snippet.substr(
                        pos, nl == std::string::npos ? std::string::npos : nl - pos);
                    if (!lineText.empty()) Line(lineText);
                    if (nl == std::string::npos) break;
                    pos = nl + 1;
                }
            } else {
                Line(fmt::format(
                    "// TODO[bpr-unsupported]: {} (guid={}) — manual port required.",
                    nodeClass, guid));
                if (!cls.note.empty()) {
                    Line(fmt::format("// Manual: {}", cls.note));
                }
            }
            // Sidecar entry — keep the original BPIR fields plus the
            // treatment classification.
            nlohmann::json note = u;
            note["treatment"] =
                (cls.kind == UnsupportedClassification::Kind::Approximation)
                    ? "approximation" : "todo_comment";
            if (!cls.note.empty()) note["manual_note"] = cls.note;
            notes.push_back(std::move(note));
            return;
        }
        Line(fmt::format("// <unknown-statement-form: {}>", form));
    }
};

} // namespace

std::string MapBpirTypeToCpp(std::string_view bpirType) {
    return MapTypeRecursive(bpirType, /*forMember=*/false);
}

std::string MapBpirTypeToCppMember(std::string_view bpirType) {
    return MapTypeRecursive(bpirType, /*forMember=*/true);
}

CppEmitResult EmitCppFunctionBody(const nlohmann::json& doc, CppEmitOptions opts) {
    ValidateBpir(doc);
    if (!IsBpirFunction(doc)) {
        throw std::invalid_argument(
            "EmitCppFunctionBody requires a BPIR function doc (kind=\"function\")");
    }
    Emitter em;
    em.opts = opts;
    em.indentLevel = 1;  // body is one level deep inside the function block
    if (doc.contains("body")) em.EmitStatementList(doc["body"]);
    return CppEmitResult{em.out.str(), std::move(em.notes)};
}

CppEmitResult EmitCppFunction(const nlohmann::json& doc, CppEmitOptions opts) {
    ValidateBpir(doc);
    if (!IsBpirFunction(doc)) {
        throw std::invalid_argument(
            "EmitCppFunction requires a BPIR function doc (kind=\"function\")");
    }
    // Build signature: <returnType> <name>(<args>).
    std::string returnType = "void";
    if (doc.contains("metadata") && doc["metadata"].is_object()) {
        returnType = doc["metadata"].value("return_type", returnType);
    }
    // If outputs[] has exactly one entry, prefer that as the return type.
    if (doc.contains("outputs") && doc["outputs"].is_array() &&
        doc["outputs"].size() == 1) {
        returnType = MapBpirTypeToCpp(doc["outputs"][0].value("type", "void"));
    }

    std::string args;
    if (doc.contains("inputs")) {
        bool first = true;
        for (const auto& in : doc["inputs"]) {
            if (!first) args += ", ";
            first = false;
            args += MapBpirTypeToCpp(in.value("type", "void"));
            args += " ";
            args += in.value("name", "Arg");
        }
    }
    // Multi-return → reference-out parameters.
    if (doc.contains("outputs") && doc["outputs"].is_array() &&
        doc["outputs"].size() > 1) {
        for (const auto& out : doc["outputs"]) {
            if (!args.empty()) args += ", ";
            args += MapBpirTypeToCpp(out.value("type", "void"));
            args += "& ";
            args += out.value("name", "Out");
        }
    }

    auto bodyResult = EmitCppFunctionBody(doc, opts);

    std::ostringstream s;
    s << returnType << " " << doc.value("name", "Fn") << "(" << args << ") {\n";
    s << bodyResult.source;
    s << "}\n";
    return CppEmitResult{s.str(), std::move(bodyResult.notes)};
}

} // namespace bpr::tools
