// Lexer tests for the C++ subset CppParse accepts. Each case feeds a
// snippet through LexCpp and asserts the produced token sequence by kind
// + text. Whitespace + comments are silently skipped; the lexer is
// deterministic with respect to position within input lines.

#include <doctest/doctest.h>

#include "tools/parse/CppLex.h"

using namespace bpr::tools;

namespace {

// Pull just the kinds out of a token list — most tests only assert
// shape, not text. EOF is included at the tail so every test can verify
// the lexer ran to completion.
std::vector<CppTokenKind> Kinds(const std::vector<CppToken>& toks) {
    std::vector<CppTokenKind> out;
    out.reserve(toks.size());
    for (const auto& t : toks) out.push_back(t.kind);
    return out;
}

} // namespace

TEST_CASE("Lex: identifiers and keywords") {
    auto toks = LexCpp("if for while return MyVar");
    REQUIRE(toks.size() == 6);  // 5 tokens + EOF
    CHECK(toks[0].kind == CppTokenKind::KwIf);
    CHECK(toks[1].kind == CppTokenKind::KwFor);
    CHECK(toks[2].kind == CppTokenKind::KwWhile);
    CHECK(toks[3].kind == CppTokenKind::KwReturn);
    CHECK(toks[4].kind == CppTokenKind::Identifier);
    CHECK(toks[4].text == "MyVar");
    CHECK(toks[5].kind == CppTokenKind::Eof);
}

TEST_CASE("Lex: qualified names accumulated as one token") {
    auto toks = LexCpp("Owner::Inner::Func");
    REQUIRE(toks.size() == 2);
    CHECK(toks[0].kind == CppTokenKind::QualifiedName);
    CHECK(toks[0].text == "Owner::Inner::Func");
    CHECK(toks[1].kind == CppTokenKind::Eof);
}

TEST_CASE("Lex: bool / null literals are keyword-classified") {
    auto toks = LexCpp("true false nullptr");
    CHECK(toks[0].kind == CppTokenKind::BoolLiteral);
    CHECK(toks[0].text == "true");
    CHECK(toks[1].kind == CppTokenKind::BoolLiteral);
    CHECK(toks[1].text == "false");
    CHECK(toks[2].kind == CppTokenKind::NullLiteral);
}

TEST_CASE("Lex: integer + float literals, with f suffix dropped") {
    auto toks = LexCpp("42 3.14 1.0f 100u 1e5");
    CHECK(toks[0].kind == CppTokenKind::NumberLiteral);
    CHECK(toks[0].text == "42");
    CHECK(toks[1].kind == CppTokenKind::NumberLiteral);
    CHECK(toks[1].text == "3.14");
    CHECK(toks[2].kind == CppTokenKind::NumberLiteral);
    CHECK(toks[2].text == "1.0");  // f stripped
    CHECK(toks[3].kind == CppTokenKind::NumberLiteral);
    CHECK(toks[3].text == "100");  // u stripped
    CHECK(toks[4].kind == CppTokenKind::NumberLiteral);
    CHECK(toks[4].text == "1e5");
}

TEST_CASE("Lex: string literal with escapes") {
    auto toks = LexCpp(R"("hello\nworld")");
    REQUIRE(toks.size() == 2);
    CHECK(toks[0].kind == CppTokenKind::StringLiteral);
    CHECK(toks[0].text == "hello\nworld");
}

TEST_CASE("Lex: operators and compound assigns") {
    auto toks = LexCpp("+ - * / % == != <= >= && || ! += -= *= /= -> .");
    auto kinds = Kinds(toks);
    using K = CppTokenKind;
    std::vector<K> expected = {
        K::Plus, K::Minus, K::Star, K::Slash, K::Percent,
        K::EqEq, K::NotEq, K::LessEq, K::GreaterEq,
        K::AmpAmp, K::PipePipe, K::Bang,
        K::PlusEq, K::MinusEq, K::StarEq, K::SlashEq,
        K::Arrow, K::Dot, K::Eof,
    };
    CHECK(kinds == expected);
}

TEST_CASE("Lex: punctuation pairs") {
    auto toks = LexCpp("( ) { } [ ] ; , : = & < >");
    auto kinds = Kinds(toks);
    using K = CppTokenKind;
    std::vector<K> expected = {
        K::LParen, K::RParen, K::LBrace, K::RBrace,
        K::LBracket, K::RBracket,
        K::Semicolon, K::Comma, K::Colon, K::Assign,
        K::Ampersand, K::Less, K::Greater, K::Eof,
    };
    CHECK(kinds == expected);
}

TEST_CASE("Lex: line + block comments are skipped") {
    auto toks = LexCpp("foo // ignore me\nbar /* block */ baz");
    auto kinds = Kinds(toks);
    using K = CppTokenKind;
    std::vector<K> expected = {
        K::Identifier, K::Identifier, K::Identifier, K::Eof,
    };
    CHECK(kinds == expected);
}

TEST_CASE("Lex: Cast is its own token kind") {
    auto toks = LexCpp("Cast<APawn>(Other)");
    REQUIRE(toks.size() >= 1);
    CHECK(toks[0].kind == CppTokenKind::Cast);
    CHECK(toks[0].text == "Cast");
}

TEST_CASE("Lex: line + column track through input") {
    auto toks = LexCpp("foo\n  bar");
    REQUIRE(toks.size() >= 2);
    CHECK(toks[0].line == 1);
    CHECK(toks[0].column == 1);
    CHECK(toks[1].line == 2);
    CHECK(toks[1].column == 3);
}

TEST_CASE("Lex: unterminated string throws") {
    CHECK_THROWS_AS(LexCpp("\"unterminated"), CppLexError);
}

TEST_CASE("Lex: unexpected '|' throws") {
    CHECK_THROWS_AS(LexCpp("a | b"), CppLexError);  // single | not supported
}
