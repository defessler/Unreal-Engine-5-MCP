#include "tools/parse/CppParse.h"
#include "tools/parse/CppLex.h"
#include "tools/Bpir.h"

#include <fmt/core.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace bpr::tools {

namespace {

// ---- Forward operator alias map -------------------------------------------
// Mirror of CompileFunction.cpp's AliasMap and CppEmit.cpp's reverse map.
// When the parser sees `a + b`, it emits a {call} BPIR node with the
// canonical UE name. Round-trip with CppEmit's output is identity.
struct AliasInfo {
    std::string canonical;  // e.g. "KismetMathLibrary::Add_IntInt"
    int arity;              // 1 for prefix unary, 2 for binary
};
const std::map<std::string, AliasInfo>& OpForwardMap() {
    static const std::map<std::string, AliasInfo> m = {
        {"+",  {"KismetMathLibrary::Add_IntInt",        2}},
        {"-",  {"KismetMathLibrary::Subtract_IntInt",   2}},
        {"*",  {"KismetMathLibrary::Multiply_IntInt",   2}},
        {"/",  {"KismetMathLibrary::Divide_IntInt",     2}},
        {"%",  {"KismetMathLibrary::Percent_IntInt",    2}},
        {"==", {"KismetMathLibrary::EqualEqual_IntInt", 2}},
        {"!=", {"KismetMathLibrary::NotEqual_IntInt",   2}},
        {"<",  {"KismetMathLibrary::Less_IntInt",       2}},
        {"<=", {"KismetMathLibrary::LessEqual_IntInt",  2}},
        {">",  {"KismetMathLibrary::Greater_IntInt",    2}},
        {">=", {"KismetMathLibrary::GreaterEqual_IntInt", 2}},
        {"&&", {"KismetMathLibrary::BooleanAND",        2}},
        {"||", {"KismetMathLibrary::BooleanOR",         2}},
        {"!",  {"KismetMathLibrary::Not_PreBool",       1}},
        // Prefix unary minus is ambiguous (negation vs. subtract); we
        // synthesize `0 - x` for unary minus.
    };
    return m;
}

// ---- C++ type string → BPIR type shorthand --------------------------------
// The reverse of CppEmit's MapBpirTypeToCpp. Stable for the patterns
// CppEmit produces; permissive on hand-written input.

bool starts_with(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}
bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() &&
           s.substr(s.size() - suffix.size()) == suffix;
}
std::string trim(std::string_view s) {
    std::size_t a = 0;
    while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    std::size_t b = s.size();
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return std::string(s.substr(a, b - a));
}

// Find a comma at template-depth 0 inside an already-stripped TMap inner.
std::size_t FindTemplateComma(std::string_view inner) {
    int depth = 0;
    for (std::size_t i = 0; i < inner.size(); ++i) {
        char c = inner[i];
        if (c == '<') ++depth;
        else if (c == '>') --depth;
        else if (c == ',' && depth == 0) return i;
    }
    return std::string_view::npos;
}

std::string ReverseMapType(std::string_view cpp);

std::string ReverseMapTypeImpl(std::string cpp) {
    // Strip leading const / volatile — they're not part of the BPIR
    // type identity. The caller has already classified ref-vs-value
    // (and noted const-ref → input).
    while (true) {
        if (starts_with(cpp, "const ")) { cpp = cpp.substr(6); cpp = trim(cpp); continue; }
        if (starts_with(cpp, "volatile ")) { cpp = cpp.substr(9); cpp = trim(cpp); continue; }
        break;
    }
    // Strip trailing & — caller has already taken note of out-ref params.
    while (!cpp.empty() && cpp.back() == '&') {
        cpp.pop_back();
        cpp = trim(cpp);
    }

    // Container templates.
    if (starts_with(cpp, "TArray<") && ends_with(cpp, ">")) {
        std::string inner = cpp.substr(7, cpp.size() - 8);
        return "[]" + ReverseMapType(trim(inner));
    }
    if (starts_with(cpp, "TSet<") && ends_with(cpp, ">")) {
        std::string inner = cpp.substr(5, cpp.size() - 6);
        return "{}" + ReverseMapType(trim(inner));
    }
    if (starts_with(cpp, "TMap<") && ends_with(cpp, ">")) {
        std::string inner = cpp.substr(5, cpp.size() - 6);
        std::size_t comma = FindTemplateComma(inner);
        if (comma == std::string::npos) {
            // Malformed; fall through.
            return cpp;
        }
        std::string k = trim(inner.substr(0, comma));
        std::string v = trim(inner.substr(comma + 1));
        return "{" + ReverseMapType(k) + ":" + ReverseMapType(v) + "}";
    }
    if (starts_with(cpp, "TSubclassOf<") && ends_with(cpp, ">")) {
        std::string inner = cpp.substr(12, cpp.size() - 13);
        return "class:" + trim(inner);
    }
    if (starts_with(cpp, "TScriptInterface<") && ends_with(cpp, ">")) {
        std::string inner = cpp.substr(17, cpp.size() - 18);
        std::string trimmed = trim(inner);
        // Strip leading I if it's the UE interface convention (I + uppercase).
        if (trimmed.size() >= 2 && trimmed[0] == 'I' &&
            trimmed[1] >= 'A' && trimmed[1] <= 'Z') {
            trimmed = trimmed.substr(1);
        }
        return "interface:" + trimmed;
    }

    // Object pointer.
    if (!cpp.empty() && cpp.back() == '*') {
        std::string bare = cpp.substr(0, cpp.size() - 1);
        bare = trim(bare);
        // Strip leading A/U if followed by uppercase (UE class prefix).
        if (bare.size() >= 2 &&
            (bare[0] == 'A' || bare[0] == 'U') &&
            bare[1] >= 'A' && bare[1] <= 'Z') {
            return "object:" + bare.substr(1);
        }
        return "object:" + bare;
    }

    // Scalars and core wrappers.
    if (cpp == "bool")    return "bool";
    if (cpp == "int32" || cpp == "int") return "int";
    if (cpp == "int64")   return "int64";
    if (cpp == "uint8" || cpp == "byte") return "byte";
    if (cpp == "float")   return "float";
    if (cpp == "double")  return "double";
    if (cpp == "FString") return "string";
    if (cpp == "FName")   return "name";
    if (cpp == "FText")   return "text";
    if (cpp == "void")    return "exec";

    // F-prefixed struct (UE convention).
    if (cpp.size() >= 2 && cpp[0] == 'F' &&
        cpp[1] >= 'A' && cpp[1] <= 'Z') {
        return "struct:" + cpp.substr(1);
    }

    // E-prefixed enum.
    if (cpp.size() >= 2 && cpp[0] == 'E' &&
        cpp[1] >= 'A' && cpp[1] <= 'Z') {
        return "enum:" + cpp.substr(1);
    }

    // Pass through unknown type names — caller may have a context-specific
    // mapping table. BPIR validators don't reject custom type strings.
    return cpp;
}

std::string ReverseMapType(std::string_view cpp) {
    return ReverseMapTypeImpl(trim(cpp));
}

// ---- The parser proper ----------------------------------------------------
class Parser {
public:
    explicit Parser(std::vector<CppToken> tokens) : toks_(std::move(tokens)) {}

    // Parse the source as either a full function definition or as a bare
    // body. If `signature` is non-null, source must be a body block (the
    // signature is taken from the caller).
    nlohmann::json ParseTopLevel(const nlohmann::json* signature) {
        if (signature) {
            nlohmann::json doc = *signature;
            doc["body"] = ParseBareBody();
            return doc;
        }
        // Try function definition first; if first tokens don't look like a
        // signature, fall back to bare body.
        if (LooksLikeFunctionDefinition()) {
            return ParseFunctionDefinition();
        }
        nlohmann::json doc = {
            {"version", kBpirSchemaVersion},
            {"kind", "function"},
            {"name", "Anonymous"},
            {"body", ParseBareBody()},
        };
        if (!locals_.empty()) doc["locals"] = locals_;
        return doc;
    }

private:
    // ---- Cursor utilities ---------------------------------------------
    [[noreturn]] void Error(std::string_view msg) {
        const CppToken& t = toks_[std::min(pos_, toks_.size() - 1)];
        throw CppParseError(fmt::format(
            "{}:{}: {}", t.line, t.column, msg));
    }

    const CppToken& Cur(std::size_t off = 0) const {
        std::size_t i = pos_ + off;
        return toks_[std::min(i, toks_.size() - 1)];
    }
    bool At(CppTokenKind k) const { return Cur().kind == k; }
    bool At(CppTokenKind k, std::size_t off) const {
        return Cur(off).kind == k;
    }

    const CppToken& Eat() {
        const CppToken& t = toks_[pos_];
        if (pos_ + 1 < toks_.size()) ++pos_;
        return t;
    }

    bool Match(CppTokenKind k) {
        if (!At(k)) return false;
        Eat();
        return true;
    }

    const CppToken& Expect(CppTokenKind k, std::string_view what) {
        if (!At(k)) {
            Error(fmt::format("expected {} ({}) but got {}",
                              TokenKindName(k), what,
                              TokenKindName(Cur().kind)));
        }
        return Eat();
    }

    // ---- Top-level dispatch -------------------------------------------
    bool LooksLikeFunctionDefinition() {
        // Heuristic: "<type-tokens> <ident> (" within a small window.
        std::size_t save = pos_;
        bool ok = false;
        try {
            (void) ParseTypeString();          // return type
            if (At(CppTokenKind::Identifier) && At(CppTokenKind::LParen, 1)) {
                ok = true;
            }
        } catch (...) {
            ok = false;
        }
        pos_ = save;
        return ok;
    }

    nlohmann::json ParseFunctionDefinition() {
        std::string returnType = ParseTypeString();
        const CppToken& nameTok = Expect(CppTokenKind::Identifier, "function name");
        std::string name = nameTok.text;
        Expect(CppTokenKind::LParen, "function parameter list");

        nlohmann::json inputs  = nlohmann::json::array();
        nlohmann::json outputs = nlohmann::json::array();

        if (!At(CppTokenKind::RParen)) {
            do {
                ParseParam(inputs, outputs);
            } while (Match(CppTokenKind::Comma));
        }
        Expect(CppTokenKind::RParen, "end of parameter list");

        // Optional `const` / `override` decorators after the param list.
        // We ignore decorators for v1.
        while (At(CppTokenKind::KwConst) ||
               (At(CppTokenKind::Identifier) &&
                (Cur().text == "override" || Cur().text == "final" ||
                 Cur().text == "noexcept"))) {
            Eat();
        }

        // If the return type isn't void, surface it as a single named
        // output so signature round-trips through CppEmit.
        std::string bpirReturn = ReverseMapType(returnType);
        if (bpirReturn != "exec" && outputs.empty()) {
            outputs.push_back({{"name", "ReturnValue"}, {"type", bpirReturn}});
        }

        nlohmann::json doc = {
            {"version", kBpirSchemaVersion},
            {"kind", "function"},
            {"name", name},
            {"body", ParseBareBody()},
        };
        if (!inputs.empty())  doc["inputs"]  = inputs;
        if (!outputs.empty()) doc["outputs"] = outputs;
        if (!locals_.empty()) doc["locals"]  = locals_;
        return doc;
    }

    void ParseParam(nlohmann::json& inputs, nlohmann::json& outputs) {
        std::string typeStr = ParseTypeString();
        std::string paramName;
        if (At(CppTokenKind::Identifier)) {
            paramName = Eat().text;
        }
        // Skip default-value clauses (param = default).
        if (Match(CppTokenKind::Assign)) {
            // Eat tokens until we see Comma or RParen at depth 0.
            int parenDepth = 0;
            while (pos_ < toks_.size() && Cur().kind != CppTokenKind::Eof) {
                if (At(CppTokenKind::LParen)) { ++parenDepth; Eat(); continue; }
                if (At(CppTokenKind::RParen)) {
                    if (parenDepth == 0) break;
                    --parenDepth; Eat(); continue;
                }
                if (At(CppTokenKind::Comma) && parenDepth == 0) break;
                Eat();
            }
        }

        bool isOutRef = !typeStr.empty() && typeStr.back() == '&' &&
                        // Reject const T& — those are inputs.
                        typeStr.find("const") == std::string::npos;
        std::string bpirType = ReverseMapType(typeStr);
        nlohmann::json p = {{"name", paramName}, {"type", bpirType}};
        if (isOutRef) outputs.push_back(std::move(p));
        else          inputs.push_back(std::move(p));
    }

    // Read a type spelling as a contiguous string of tokens. Stops when
    // the next token isn't part of a type (i.e. when an identifier name
    // follows or an unrelated token appears).
    //
    // Grammar:
    //   type        ::= [const]* base [<args>]? [* | &]*
    //   base        ::= Identifier | QualifiedName | KwAuto
    //   args        ::= type (, type)*
    std::string ParseTypeString() {
        std::string out;
        // Modifiers.
        while (At(CppTokenKind::KwConst)) {
            Eat();
            if (!out.empty()) out += " ";
            out += "const";
        }
        // Base.
        if (At(CppTokenKind::Identifier) || At(CppTokenKind::QualifiedName)) {
            if (!out.empty()) out += " ";
            out += Eat().text;
        } else if (At(CppTokenKind::KwAuto)) {
            Eat();
            if (!out.empty()) out += " ";
            out += "auto";
        } else {
            Error(fmt::format("expected a type, got {}",
                              TokenKindName(Cur().kind)));
        }
        // Template arguments.
        if (At(CppTokenKind::Less)) {
            Eat();
            out += "<";
            out += ParseTypeString();
            while (Match(CppTokenKind::Comma)) {
                out += ", ";
                out += ParseTypeString();
            }
            Expect(CppTokenKind::Greater, "end of template arg list");
            out += ">";
        }
        // Pointer / reference suffix.
        while (At(CppTokenKind::Star) || At(CppTokenKind::Ampersand)) {
            out += (Eat().kind == CppTokenKind::Star ? "*" : "&");
        }
        // Trailing const (e.g. `const T* const`) — accept and drop.
        while (At(CppTokenKind::KwConst)) Eat();
        return out;
    }

    // ---- Statements ---------------------------------------------------
    nlohmann::json ParseBareBody() {
        // Body might or might not be wrapped in braces.
        if (Match(CppTokenKind::LBrace)) {
            nlohmann::json stmts = ParseStatementListUntil(CppTokenKind::RBrace);
            Expect(CppTokenKind::RBrace, "end of body");
            return stmts;
        }
        return ParseStatementListUntil(CppTokenKind::Eof);
    }

    nlohmann::json ParseStatementListUntil(CppTokenKind end) {
        nlohmann::json stmts = nlohmann::json::array();
        while (!At(end) && !At(CppTokenKind::Eof)) {
            stmts.push_back(ParseStatement());
        }
        return stmts;
    }

    nlohmann::json ParseStatement() {
        switch (Cur().kind) {
            case CppTokenKind::KwIf:        return ParseIf();
            case CppTokenKind::KwFor:       return ParseFor();
            case CppTokenKind::KwWhile:     return ParseWhile();
            case CppTokenKind::KwSwitch:    return ParseSwitch();
            case CppTokenKind::KwReturn:    return ParseReturn();
            case CppTokenKind::KwBreak:     Eat();
                                            Expect(CppTokenKind::Semicolon, "after break");
                                            return nlohmann::json{{"break", nullptr}};
            case CppTokenKind::KwContinue:  Eat();
                                            Expect(CppTokenKind::Semicolon, "after continue");
                                            return nlohmann::json{{"continue", nullptr}};
            case CppTokenKind::LBrace: {
                // Bare block — flatten as a sequence with one element.
                Eat();
                nlohmann::json inner = ParseStatementListUntil(CppTokenKind::RBrace);
                Expect(CppTokenKind::RBrace, "end of block");
                return nlohmann::json{{"sequence", nlohmann::json::array({inner})}};
            }
            default: break;
        }
        // Try variable declaration (auto / known type at statement start).
        if (auto decl = TryParseDeclaration()) {
            return *decl;
        }
        return ParseExpressionStatement();
    }

    nlohmann::json ParseIf() {
        Expect(CppTokenKind::KwIf, "if");
        Expect(CppTokenKind::LParen, "if condition");

        // Detect cast pattern: if (auto* X = Cast<T>(expr))
        if (At(CppTokenKind::KwAuto) &&
            At(CppTokenKind::Star, 1) &&
            At(CppTokenKind::Identifier, 2) &&
            At(CppTokenKind::Assign, 3) &&
            At(CppTokenKind::Cast, 4)) {
            return ParseCastIf();
        }

        nlohmann::json cond = ParseExpression();
        Expect(CppTokenKind::RParen, "end of if condition");

        nlohmann::json thenBlk = ParseBranchBody();
        nlohmann::json node = {{"if", cond}, {"then", thenBlk}};
        if (Match(CppTokenKind::KwElse)) {
            // else-if chains.
            if (At(CppTokenKind::KwIf)) {
                node["else"] = nlohmann::json::array({ParseIf()});
            } else {
                node["else"] = ParseBranchBody();
            }
        }
        return node;
    }

    nlohmann::json ParseCastIf() {
        // Already verified: auto * Ident = Cast < Type > ( expr ) )
        Eat();  // auto
        Eat();  // *
        std::string asName = Eat().text;
        Eat();  // =
        Eat();  // Cast
        Expect(CppTokenKind::Less, "Cast<...>");
        std::string toType = ParseTypeString();
        Expect(CppTokenKind::Greater, "end of Cast<>");
        Expect(CppTokenKind::LParen, "Cast(...)");
        nlohmann::json castExpr = ParseExpression();
        Expect(CppTokenKind::RParen, "end of Cast(...)");
        Expect(CppTokenKind::RParen, "end of if condition");

        nlohmann::json successBlk = ParseBranchBody();
        nlohmann::json node = {
            {"cast",    castExpr},
            {"to",      toType},
            {"as",      asName},
            {"success", successBlk},
        };
        if (Match(CppTokenKind::KwElse)) {
            node["fail"] = ParseBranchBody();
        } else {
            node["fail"] = nlohmann::json::array();
        }
        return node;
    }

    nlohmann::json ParseBranchBody() {
        // Either a brace-block or a single statement (we wrap as array).
        if (Match(CppTokenKind::LBrace)) {
            nlohmann::json stmts = ParseStatementListUntil(CppTokenKind::RBrace);
            Expect(CppTokenKind::RBrace, "end of block");
            return stmts;
        }
        return nlohmann::json::array({ParseStatement()});
    }

    nlohmann::json ParseFor() {
        Expect(CppTokenKind::KwFor, "for");
        Expect(CppTokenKind::LParen, "for (...)");
        // We only support range-based for: for (auto& X : coll) { ... }
        if (!At(CppTokenKind::KwAuto)) {
            Error("only range-based for (`for (auto& X : coll)`) is supported");
        }
        Eat();  // auto
        // Optional & — we accept both `auto& X` and `auto X`.
        Match(CppTokenKind::Ampersand);
        const CppToken& nameTok = Expect(CppTokenKind::Identifier, "loop variable");
        std::string elemName = nameTok.text;
        Expect(CppTokenKind::Colon, "for (auto& X : coll)");
        nlohmann::json coll = ParseExpression();
        Expect(CppTokenKind::RParen, "end of for header");
        nlohmann::json body = ParseBranchBody();
        return nlohmann::json{
            {"for_each", elemName},
            {"in",       coll},
            {"body",     body},
        };
    }

    nlohmann::json ParseWhile() {
        Expect(CppTokenKind::KwWhile, "while");
        Expect(CppTokenKind::LParen, "while condition");
        nlohmann::json cond = ParseExpression();
        Expect(CppTokenKind::RParen, "end of while condition");
        nlohmann::json body = ParseBranchBody();
        return nlohmann::json{{"while", cond}, {"body", body}};
    }

    nlohmann::json ParseSwitch() {
        Expect(CppTokenKind::KwSwitch, "switch");
        Expect(CppTokenKind::LParen, "switch (expr)");
        nlohmann::json scrutinee = ParseExpression();
        Expect(CppTokenKind::RParen, "end of switch header");
        Expect(CppTokenKind::LBrace, "switch body");

        nlohmann::json cases = nlohmann::json::object();
        nlohmann::json defaultBody = nlohmann::json::array();
        bool sawDefault = false;

        while (!At(CppTokenKind::RBrace) && !At(CppTokenKind::Eof)) {
            if (Match(CppTokenKind::KwCase)) {
                nlohmann::json labelExpr = ParseExpression();
                Expect(CppTokenKind::Colon, "after case label");
                std::string key = LiteralToKey(labelExpr);
                nlohmann::json body = ParseSwitchCaseBody();
                cases[key] = body;
            } else if (Match(CppTokenKind::KwDefault)) {
                Expect(CppTokenKind::Colon, "after default");
                defaultBody = ParseSwitchCaseBody();
                sawDefault = true;
            } else {
                Error("expected `case` or `default` inside switch");
            }
        }
        Expect(CppTokenKind::RBrace, "end of switch body");
        nlohmann::json node = {{"switch", scrutinee}, {"cases", cases}};
        if (sawDefault) node["default"] = defaultBody;
        return node;
    }

    // Read statements following a case label until we hit the next case,
    // default, or the closing brace. Trailing `break;` is consumed and
    // dropped. CppEmit emits each case body wrapped in `{ ... }`, so we
    // tolerate (but don't require) a leading brace.
    nlohmann::json ParseSwitchCaseBody() {
        nlohmann::json body = nlohmann::json::array();
        bool braced = Match(CppTokenKind::LBrace);
        while (!At(CppTokenKind::KwCase) &&
               !At(CppTokenKind::KwDefault) &&
               !At(CppTokenKind::RBrace) &&
               !At(CppTokenKind::Eof)) {
            // Drop a trailing break; stay simple about which case it
            // belongs to (it ends *this* case).
            if (At(CppTokenKind::KwBreak)) {
                Eat();
                Expect(CppTokenKind::Semicolon, "after break");
                break;
            }
            body.push_back(ParseStatement());
        }
        // If we entered the braced form (CppEmit's `case 0: { ... }`),
        // consume the matching closing brace. If we exited the inner
        // loop via RBrace already, this is the same `}`.
        if (braced) {
            Expect(CppTokenKind::RBrace, "end of case block");
        }
        return body;
    }

    std::string LiteralToKey(const nlohmann::json& expr) {
        if (!expr.is_object() || !expr.contains("lit")) {
            Error("case label must be a literal");
        }
        const auto& v = expr.at("lit");
        if (v.is_string()) return v.get<std::string>();
        if (v.is_number_integer()) return std::to_string(v.get<long long>());
        if (v.is_number()) return std::to_string(v.get<double>());
        if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
        Error("case label literal has unsupported type");
    }

    nlohmann::json ParseReturn() {
        Expect(CppTokenKind::KwReturn, "return");
        if (Match(CppTokenKind::Semicolon)) {
            return nlohmann::json{{"return", nullptr}};
        }
        nlohmann::json e = ParseExpression();
        Expect(CppTokenKind::Semicolon, "after return value");
        return nlohmann::json{{"return", e}};
    }

    // Variable declaration at statement scope: either `auto X = expr;` or
    // `T X = expr;` or `T X;`. Returns std::nullopt-equivalent (we use a
    // pointer return because optional<json> is awkward).
    std::optional<nlohmann::json> TryParseDeclaration() {
        std::size_t save = pos_;
        bool startsAuto = At(CppTokenKind::KwAuto);
        bool startsConst = At(CppTokenKind::KwConst);
        bool startsType  = At(CppTokenKind::Identifier) ||
                           At(CppTokenKind::QualifiedName);

        if (!startsAuto && !startsConst && !startsType) return std::nullopt;

        // Attempt to read type, then identifier, then `=` or `;`.
        try {
            std::string typeStr = ParseTypeString();
            if (!At(CppTokenKind::Identifier)) {
                pos_ = save;
                return std::nullopt;
            }
            std::size_t identPos = pos_;
            std::string name = Eat().text;

            if (Match(CppTokenKind::Semicolon)) {
                // T X; — no init. Track in locals; emit no statement.
                AddLocal(name, typeStr);
                return nlohmann::json{{"comment", fmt::format("decl {} {}", typeStr, name)}};
            }
            if (Match(CppTokenKind::Assign)) {
                nlohmann::json rhs = ParseExpression();
                Expect(CppTokenKind::Semicolon, "after declaration init");
                AddLocal(name, typeStr);
                return nlohmann::json{{"set", name}, {"to", rhs}};
            }
            // Not a declaration — rewind.
            pos_ = save;
            (void) identPos;
            return std::nullopt;
        } catch (...) {
            pos_ = save;
            return std::nullopt;
        }
    }

    void AddLocal(const std::string& name, const std::string& cppType) {
        // Skip if already declared (re-declarations / shadowing).
        for (const auto& l : locals_) {
            if (l.value("name", "") == name) return;
        }
        std::string bpirType = ReverseMapType(cppType);
        locals_.push_back({{"name", name}, {"type", bpirType}});
    }

    // Expression-statement: an expression terminated by `;`. May be an
    // assignment (becomes `{set, to}`), a call (becomes `{call, args}`),
    // or a compound assignment.
    nlohmann::json ParseExpressionStatement() {
        nlohmann::json e = ParseExpression();
        Expect(CppTokenKind::Semicolon, "end of expression statement");
        return ExprToStatement(e);
    }

    // Convert a top-level expression into a statement. The cases:
    //  - {assign: <lhs>, op: "=", rhs: <e>}      → {set, to}
    //  - {assign: <lhs>, op: "+=", rhs: <e>}     → {set, to: lhs + e}
    //  - call expression                         → {call, args}
    //  - anything else                           → error (unused expression)
    nlohmann::json ExprToStatement(const nlohmann::json& e) {
        if (e.is_object() && e.contains("__assign")) {
            const auto& assign = e["__assign"];
            std::string op   = assign.at("op").get<std::string>();
            const auto& lhs  = assign.at("lhs");
            const auto& rhs  = assign.at("rhs");

            // LHS must be a {var, name} or {member, name} chain. We
            // serialize it back to a name string for the {set} form. For
            // member chains we emit a comment; BPIR's `set` accepts only
            // a flat var name.
            std::string lhsName;
            if (lhs.is_object() && lhs.contains("var")) {
                lhsName = lhs["var"].get<std::string>();
            } else {
                Error("set target must be a plain variable name");
            }
            if (op == "=") {
                return nlohmann::json{{"set", lhsName}, {"to", rhs}};
            }
            // Compound assign: expand `X op= Y` to `X = X op Y`.
            std::string baseOp = op.substr(0, op.size() - 1);  // "+=" → "+"
            auto it = OpForwardMap().find(baseOp);
            if (it == OpForwardMap().end()) {
                Error(fmt::format("unsupported compound operator '{}'", op));
            }
            nlohmann::json rhsExpr = {
                {"call", it->second.canonical},
                {"args", nlohmann::json{{"A", nlohmann::json{{"var", lhsName}}},
                                        {"B", rhs}}}
            };
            return nlohmann::json{{"set", lhsName}, {"to", rhsExpr}};
        }
        if (e.is_object() && e.contains("call")) {
            return e;  // call statement
        }
        Error("expression has no effect (must be assignment, call, "
              "or control statement)");
    }

    // ---- Expressions: precedence climbing -----------------------------
    //
    // Precedence (low → high):
    //   1. assignment (=, +=, -=, *=, /=)        — right-assoc
    //   2. logical or (||)
    //   3. logical and (&&)
    //   4. equality   (==, !=)
    //   5. relational (<, <=, >, >=)
    //   6. additive   (+, -)
    //   7. multiplicative (*, /, %)
    //   8. unary  (!, -)
    //   9. postfix (., ->, [], (...))
    //  10. primary (lit, var, (..), Cast<>, this, qualified-call)

    nlohmann::json ParseExpression() {
        return ParseAssignment();
    }

    nlohmann::json ParseAssignment() {
        nlohmann::json lhs = ParseLogicalOr();
        if (At(CppTokenKind::Assign)  || At(CppTokenKind::PlusEq) ||
            At(CppTokenKind::MinusEq) || At(CppTokenKind::StarEq) ||
            At(CppTokenKind::SlashEq)) {
            std::string op = Eat().text;
            nlohmann::json rhs = ParseAssignment();
            // Sentinel form so ExprToStatement can see we have an assign.
            return nlohmann::json{
                {"__assign", nlohmann::json{
                    {"op", op}, {"lhs", lhs}, {"rhs", rhs}}}
            };
        }
        return lhs;
    }

    nlohmann::json ParseLogicalOr() {
        nlohmann::json lhs = ParseLogicalAnd();
        while (At(CppTokenKind::PipePipe)) {
            Eat();
            nlohmann::json rhs = ParseLogicalAnd();
            lhs = MakeBinop("||", lhs, rhs);
        }
        return lhs;
    }
    nlohmann::json ParseLogicalAnd() {
        nlohmann::json lhs = ParseEquality();
        while (At(CppTokenKind::AmpAmp)) {
            Eat();
            nlohmann::json rhs = ParseEquality();
            lhs = MakeBinop("&&", lhs, rhs);
        }
        return lhs;
    }
    nlohmann::json ParseEquality() {
        nlohmann::json lhs = ParseRelational();
        while (At(CppTokenKind::EqEq) || At(CppTokenKind::NotEq)) {
            std::string op = Eat().text;
            nlohmann::json rhs = ParseRelational();
            lhs = MakeBinop(op, lhs, rhs);
        }
        return lhs;
    }
    nlohmann::json ParseRelational() {
        nlohmann::json lhs = ParseAdditive();
        while (At(CppTokenKind::Less) || At(CppTokenKind::LessEq) ||
               At(CppTokenKind::Greater) || At(CppTokenKind::GreaterEq)) {
            std::string op = Eat().text;
            nlohmann::json rhs = ParseAdditive();
            lhs = MakeBinop(op, lhs, rhs);
        }
        return lhs;
    }
    nlohmann::json ParseAdditive() {
        nlohmann::json lhs = ParseMultiplicative();
        while (At(CppTokenKind::Plus) || At(CppTokenKind::Minus)) {
            std::string op = Eat().text;
            nlohmann::json rhs = ParseMultiplicative();
            lhs = MakeBinop(op, lhs, rhs);
        }
        return lhs;
    }
    nlohmann::json ParseMultiplicative() {
        nlohmann::json lhs = ParseUnary();
        while (At(CppTokenKind::Star) || At(CppTokenKind::Slash) ||
               At(CppTokenKind::Percent)) {
            std::string op = Eat().text;
            nlohmann::json rhs = ParseUnary();
            lhs = MakeBinop(op, lhs, rhs);
        }
        return lhs;
    }

    nlohmann::json ParseUnary() {
        if (At(CppTokenKind::Bang)) {
            Eat();
            nlohmann::json operand = ParseUnary();
            return MakeUnop("!", operand);
        }
        if (At(CppTokenKind::Minus)) {
            Eat();
            nlohmann::json operand = ParseUnary();
            // Synthesize `0 - operand` so we don't need a separate unary
            // minus alias in BPIR.
            nlohmann::json zero = {{"lit", 0}};
            return MakeBinop("-", zero, operand);
        }
        return ParsePostfix();
    }

    nlohmann::json ParsePostfix() {
        nlohmann::json e = ParsePrimary();
        for (;;) {
            if (At(CppTokenKind::Dot) || At(CppTokenKind::Arrow)) {
                Eat();
                const CppToken& m = Expect(CppTokenKind::Identifier,
                                           "member name");
                e = nlohmann::json{{"member", e}, {"name", m.text}};
            } else if (At(CppTokenKind::LBracket)) {
                Eat();
                nlohmann::json idx = ParseExpression();
                Expect(CppTokenKind::RBracket, "end of [...]");
                e = nlohmann::json{{"index", e}, {"idx", idx}};
            } else if (At(CppTokenKind::LParen)) {
                // Call: e(args). We require e to resolve to a name string.
                std::string fn;
                if (e.is_object()) {
                    if (e.contains("var")) {
                        fn = e["var"].get<std::string>();
                    } else if (e.contains("member")) {
                        // Translate (target.method) into "target::method".
                        // Heuristic: BP doesn't have arbitrary member-fn
                        // calls, but UE static calls show up as
                        // qualified names. We render as "owner::method".
                        std::string method = e["name"].get<std::string>();
                        const auto& tgt = e["member"];
                        if (tgt.is_object() && tgt.contains("var")) {
                            fn = tgt["var"].get<std::string>() + "::" + method;
                        } else {
                            // Fall back: attach the method only.
                            fn = method;
                        }
                    }
                }
                if (fn.empty()) {
                    Error("function call target must be an identifier or "
                          "member access");
                }
                Eat();  // consume '('
                nlohmann::json args = ParseCallArgs();
                Expect(CppTokenKind::RParen, "end of call args");
                e = nlohmann::json{{"call", fn}, {"args", args}};
            } else {
                break;
            }
        }
        return e;
    }

    nlohmann::json ParseCallArgs() {
        // Args become {A: ..., B: ...} keyed by alphabetic position so
        // alphabetical iteration in CppEmit reproduces input order.
        nlohmann::json args = nlohmann::json::object();
        if (At(CppTokenKind::RParen)) return args;
        std::size_t i = 0;
        do {
            nlohmann::json a = ParseAssignment();
            std::string key;
            // 'A', 'B', 'C', ..., 'Z', 'AA', 'AB', ... — letter sequence.
            // For typical short arg lists we never exceed Z.
            if (i < 26) {
                key = std::string(1, char('A' + i));
            } else {
                key = "arg" + std::to_string(i);
            }
            args[key] = a;
            ++i;
        } while (Match(CppTokenKind::Comma));
        return args;
    }

    nlohmann::json ParsePrimary() {
        const CppToken& t = Cur();
        switch (t.kind) {
            case CppTokenKind::NumberLiteral: {
                Eat();
                if (t.text.find('.') != std::string::npos ||
                    t.text.find('e') != std::string::npos ||
                    t.text.find('E') != std::string::npos) {
                    return nlohmann::json{{"lit", std::stod(t.text)}};
                }
                return nlohmann::json{{"lit", std::stoll(t.text)}};
            }
            case CppTokenKind::StringLiteral:
                Eat();
                return nlohmann::json{{"lit", t.text}};
            case CppTokenKind::BoolLiteral:
                Eat();
                return nlohmann::json{{"lit", t.text == "true"}};
            case CppTokenKind::NullLiteral:
                Eat();
                return nlohmann::json{{"lit", nullptr}};
            case CppTokenKind::KwThis:
                Eat();
                return nlohmann::json{{"self", nullptr}};
            case CppTokenKind::Identifier:
                Eat();
                return nlohmann::json{{"var", t.text}};
            case CppTokenKind::QualifiedName: {
                // Treat as a callable name; if not followed by '(' it's
                // an enum reference or static value, which we just
                // surface as a {var}.
                Eat();
                return nlohmann::json{{"var", t.text}};
            }
            case CppTokenKind::Cast: {
                Eat();
                Expect(CppTokenKind::Less, "Cast<...>");
                std::string toType = ParseTypeString();
                Expect(CppTokenKind::Greater, "end of Cast<>");
                Expect(CppTokenKind::LParen, "Cast(...)");
                nlohmann::json inner = ParseExpression();
                Expect(CppTokenKind::RParen, "end of Cast(...)");
                return nlohmann::json{{"cast", inner}, {"to", toType}};
            }
            case CppTokenKind::LParen: {
                Eat();
                nlohmann::json e = ParseExpression();
                Expect(CppTokenKind::RParen, "end of (...)");
                return e;
            }
            case CppTokenKind::LBrace: {
                // Brace-init list — we treat as new_array. Hand-written
                // C++ might use this for struct-init too; the user can
                // disambiguate by writing `T{...}` (not yet supported).
                Eat();
                nlohmann::json elems = nlohmann::json::array();
                if (!At(CppTokenKind::RBrace)) {
                    do {
                        elems.push_back(ParseAssignment());
                    } while (Match(CppTokenKind::Comma));
                }
                Expect(CppTokenKind::RBrace, "end of {...}");
                return nlohmann::json{{"new_array", elems}};
            }
            default:
                break;
        }
        Error(fmt::format("unexpected token '{}' in expression",
                          TokenKindName(t.kind)));
    }

    nlohmann::json MakeBinop(const std::string& op,
                             const nlohmann::json& lhs,
                             const nlohmann::json& rhs) {
        auto it = OpForwardMap().find(op);
        if (it == OpForwardMap().end()) {
            Error(fmt::format("unsupported binary operator '{}'", op));
        }
        return nlohmann::json{
            {"call", it->second.canonical},
            {"args", nlohmann::json{{"A", lhs}, {"B", rhs}}},
        };
    }
    nlohmann::json MakeUnop(const std::string& op,
                            const nlohmann::json& operand) {
        auto it = OpForwardMap().find(op);
        if (it == OpForwardMap().end() || it->second.arity != 1) {
            Error(fmt::format("unsupported unary operator '{}'", op));
        }
        return nlohmann::json{
            {"call", it->second.canonical},
            {"args", nlohmann::json{{"A", operand}}},
        };
    }

    std::vector<CppToken> toks_;
    std::size_t pos_ = 0;
    nlohmann::json locals_ = nlohmann::json::array();
};

// ---- Public API ----------------------------------------------------------
nlohmann::json ParseInternal(std::string_view source,
                             const nlohmann::json* signature) {
    std::vector<CppToken> tokens;
    try {
        tokens = LexCpp(source);
    } catch (const CppLexError& e) {
        throw CppParseError(e.what());
    }
    Parser p(std::move(tokens));
    nlohmann::json doc = p.ParseTopLevel(signature);
    try {
        ValidateBpir(doc);
    } catch (const std::exception& e) {
        throw CppParseError(fmt::format(
            "parser produced invalid BPIR: {}", e.what()));
    }
    return doc;
}

} // namespace

nlohmann::json ParseCppFunction(std::string_view source) {
    return ParseInternal(source, nullptr);
}

nlohmann::json ParseCppFunction(std::string_view source,
                                const nlohmann::json& signature) {
    return ParseInternal(source, &signature);
}

} // namespace bpr::tools
