// Tests for the BPIR schema validator. Schema fragility bites hardest
// when validation is too lax (a malformed doc passes validation, then
// crashes deep inside codegen) or too strict (legitimate docs are
// rejected). These tests pin the contract for both.

#include <doctest/doctest.h>

#include "tools/Bpir.h"

#include <stdexcept>

using namespace bpr::tools;
using nlohmann::json;

namespace {

// Minimal valid function doc — used as a baseline that other tests
// mutate to exercise specific validation paths.
json MakeMinimalFunction() {
    return json{
        {"version", 1},
        {"kind", "function"},
        {"name", "TakeDamage"},
        {"body", json::array()},
    };
}

} // namespace

// ===== Top-level shape =====================================================

TEST_CASE("BPIR: minimal function doc validates") {
    CHECK_NOTHROW(ValidateBpir(MakeMinimalFunction()));
    CHECK(IsBpirFunction(MakeMinimalFunction()));
    CHECK_FALSE(IsBpirClass(MakeMinimalFunction()));
}

TEST_CASE("BPIR: minimal class doc validates") {
    json doc = {
        {"version", 1}, {"kind", "class"}, {"name", "BP_Enemy"},
    };
    CHECK_NOTHROW(ValidateBpir(doc));
    CHECK(IsBpirClass(doc));
}

TEST_CASE("BPIR: missing version is rejected") {
    json doc = {{"kind", "function"}, {"name", "F"}, {"body", json::array()}};
    CHECK_THROWS_AS(ValidateBpir(doc), std::invalid_argument);
}

TEST_CASE("BPIR: future version is rejected") {
    json doc = {
        {"version", 999}, {"kind", "function"},
        {"name", "F"}, {"body", json::array()}};
    try {
        ValidateBpir(doc);
        FAIL("expected throw");
    } catch (const std::invalid_argument& e) {
        CHECK(std::string(e.what()).find("999") != std::string::npos);
    }
}

TEST_CASE("BPIR: unknown kind is rejected") {
    json doc = {{"version", 1}, {"kind", "weird"}, {"name", "X"}};
    CHECK_THROWS_AS(ValidateBpir(doc), std::invalid_argument);
}

TEST_CASE("BPIR: function doc requires name + body") {
    json doc = {{"version", 1}, {"kind", "function"}};
    CHECK_THROWS_AS(ValidateBpir(doc), std::invalid_argument);
}

// ===== Statement forms =====================================================

TEST_CASE("BPIR: every statement form validates with a minimal example") {
    json fn = MakeMinimalFunction();
    fn["body"] = json::array({
        json{{"if", json{{"var", "bIsAlive"}}}, {"then", json::array()}},
        json{{"set", "Health"}, {"to", json{{"lit", 100}}}},
        json{{"call", "OnDeath"}},
        json{{"comment", "do the thing"}},
        json{{"return", json{{"var", "Health"}}}},
        json{{"return", nullptr}},                                 // bare return
        json{{"return", json::array({json{{"var","X"}}, json{{"var","Y"}}})}},  // multi-return
        json{{"cast", json{{"var","Other"}}}, {"to","Pawn"},
             {"as","P"}, {"success", json::array()}, {"fail", json::array()}},
        json{{"switch", json{{"var","Mode"}}},
             {"cases", json{{"0", json::array()}, {"1", json::array()}}},
             {"default", json::array()}},
        json{{"for_each", "It"}, {"in", json{{"var","Items"}}}, {"body", json::array()}},
        json{{"while", json{{"lit", true}}}, {"body", json::array()}},
        json{{"sequence", json::array({json::array(), json::array()})}},
        json{{"break", nullptr}},
        json{{"continue", nullptr}},
        json{{"unsupported", json{{"node_class","K2Node_Timeline"}}}},
    });
    CHECK_NOTHROW(ValidateBpir(fn));
}

TEST_CASE("BPIR: statement with no recognized form is rejected") {
    json fn = MakeMinimalFunction();
    fn["body"] = json::array({json{{"unknown_form", "nope"}}});
    CHECK_THROWS_AS(ValidateBpir(fn), std::invalid_argument);
}

TEST_CASE("BPIR: cast statement requires success + fail branches") {
    json fn = MakeMinimalFunction();
    fn["body"] = json::array({
        json{{"cast", json{{"var","Other"}}}, {"to","Pawn"}}
    });
    CHECK_THROWS_AS(ValidateBpir(fn), std::invalid_argument);
}

TEST_CASE("BPIR: for_each requires elem name as string") {
    json fn = MakeMinimalFunction();
    fn["body"] = json::array({
        json{{"for_each", 42}, {"in", json{{"var","X"}}}, {"body", json::array()}}
    });
    CHECK_THROWS_AS(ValidateBpir(fn), std::invalid_argument);
}

TEST_CASE("BPIR: error path includes the body index") {
    json fn = MakeMinimalFunction();
    fn["body"] = json::array({
        json{{"comment", "ok"}},
        json{{"set", "Health"}}  // missing "to"
    });
    try {
        ValidateBpir(fn);
        FAIL("expected throw");
    } catch (const std::invalid_argument& e) {
        std::string msg = e.what();
        CHECK(msg.find("body[1]") != std::string::npos);
        CHECK(msg.find("\"to\"") != std::string::npos);
    }
}

// ===== Expression forms ====================================================

TEST_CASE("BPIR: every expression form validates with a minimal example") {
    json fn = MakeMinimalFunction();
    fn["body"] = json::array({
        // Wrap each expression as a `set` `to` so the validator visits it.
        json{{"set","X"}, {"to", json{{"var","Y"}}}},
        json{{"set","X"}, {"to", json{{"lit", 42}}}},
        json{{"set","X"}, {"to", json{{"call","Foo"}}}},
        json{{"set","X"}, {"to",
             json{{"cast", json{{"var","Y"}}}, {"to","Actor"}}}},
        json{{"set","X"}, {"to",
             json{{"member", json{{"var","V"}}}, {"name","X"}}}},
        json{{"set","X"}, {"to",
             json{{"index", json{{"var","Arr"}}}, {"idx", json{{"lit",0}}}}}},
        json{{"set","X"}, {"to", json{{"self", nullptr}}}},
        json{{"set","X"}, {"to", json{{"new_array", json::array({json{{"lit",1}}})}}}},
        json{{"set","X"}, {"to",
             json{{"new_struct","FVector"},
                  {"fields", json{{"X", json{{"lit",0}}},{"Y", json{{"lit",0}}},{"Z", json{{"lit",0}}}}}}}},
    });
    CHECK_NOTHROW(ValidateBpir(fn));
}

TEST_CASE("BPIR: lit value must be a JSON scalar") {
    json fn = MakeMinimalFunction();
    fn["body"] = json::array({
        json{{"set","X"}, {"to", json{{"lit", json::array({1,2,3})}}}}
    });
    CHECK_THROWS_AS(ValidateBpir(fn), std::invalid_argument);
}

TEST_CASE("BPIR: var scope must be a recognized value") {
    json fn = MakeMinimalFunction();
    fn["body"] = json::array({
        json{{"set","X"}, {"to", json{{"var","Y"}, {"scope","weird"}}}}
    });
    CHECK_THROWS_AS(ValidateBpir(fn), std::invalid_argument);
}

TEST_CASE("BPIR: nested expression error path bubbles up") {
    json fn = MakeMinimalFunction();
    fn["body"] = json::array({
        json{{"set","X"}, {"to",
             json{{"call","Foo"},
                  {"args", json{{"A", json{{"var", 42}}}}}}}}  // var must be string
    });
    try { ValidateBpir(fn); FAIL("expected throw"); }
    catch (const std::invalid_argument& e) {
        std::string msg = e.what();
        // Path should include both the statement and the args.A nesting.
        CHECK(msg.find("body[0].to.args.A") != std::string::npos);
    }
}

// ===== Variable decls + class shape =======================================

TEST_CASE("BPIR: function inputs/outputs/locals validate variable shape") {
    json fn = MakeMinimalFunction();
    fn["inputs"] = json::array({json{{"name","Amount"},{"type","float"}}});
    fn["outputs"] = json::array({json{{"name","Killed"},{"type","bool"}}});
    fn["locals"] = json::array({json{{"name","Buf"},{"type","int"}}});
    CHECK_NOTHROW(ValidateBpir(fn));
}

TEST_CASE("BPIR: variable decl missing type is rejected") {
    json fn = MakeMinimalFunction();
    fn["inputs"] = json::array({json{{"name","X"}}});
    CHECK_THROWS_AS(ValidateBpir(fn), std::invalid_argument);
}

TEST_CASE("BPIR: class doc with full payload validates") {
    json doc = {
        {"version", 1},
        {"kind", "class"},
        {"name", "BP_Enemy"},
        {"metadata", json{{"asset_path","/Game/AI/BP_Enemy"},{"parent_class","ACharacter"}}},
        {"interfaces", json::array({"IDamageable"})},
        {"variables", json::array({
            json{{"name","Health"},{"type","float"},{"replicated", true},{"editable", true}}
        })},
        {"functions", json::array({
            json{{"name","TakeDamage"},{"body", json::array()}}
        })},
    };
    CHECK_NOTHROW(ValidateBpir(doc));
}

// ===== Form catalogues =====================================================

TEST_CASE("BPIR: StatementForms includes every documented form") {
    auto& forms = StatementForms();
    auto has = [&](const std::string& f) {
        return std::find(forms.begin(), forms.end(), f) != forms.end();
    };
    for (const auto& expected : {"if","set","call","comment","return","cast",
                                  "switch","for_each","while","sequence",
                                  "break","continue","unsupported"}) {
        CHECK(has(expected));
    }
}

TEST_CASE("BPIR: ExpressionForms includes every documented form") {
    auto& forms = ExpressionForms();
    auto has = [&](const std::string& f) {
        return std::find(forms.begin(), forms.end(), f) != forms.end();
    };
    for (const auto& expected : {"var","lit","call","cast","member",
                                  "index","self","new_array","new_struct"}) {
        CHECK(has(expected));
    }
}

TEST_CASE("BPIR: DetectStatementForm picks the first matching key") {
    json stmt = {{"if", json{{"var","X"}}}, {"then", json::array()}};
    CHECK(DetectStatementForm(stmt) == "if");
}

TEST_CASE("BPIR: DetectStatementForm returns empty for unknown forms") {
    json stmt = {{"weird", "nope"}};
    CHECK(DetectStatementForm(stmt).empty());
}

TEST_CASE("BPIR: MigrateToCurrent is identity for v1 docs") {
    json doc = MakeMinimalFunction();
    auto migrated = MigrateToCurrent(doc);
    CHECK(migrated == doc);
}
