// CppLex — tokenizer for the controlled C++ subset CppParse accepts.
//
// Hand-rolled rather than libclang-based: vendoring LLVM binaries
// (~30-50MB) clashes with the project's no-network/no-fetch third_party
// policy. The hand-rolled lexer + parser are scoped to round-trip what
// compile_function / CppEmit produce. Swapping to libclang stays a
// clean future change — would replace tools/parse/* without touching
// BPIR or the rest of the pipeline.
//
// Subset supported:
//   - Keywords: if, else, for, while, switch, case, default, return,
//     break, continue, true, false, nullptr, this, auto, const
//   - Identifiers, qualified names (Foo::Bar)
//   - Integer + float literals (with optional 'f' suffix)
//   - String literals, including TEXT("...") wrapper
//   - Operators: + - * / % == != < <= > >= && || ! = -> . :: , ; ( ) { } [ ] &
//   - Cast<T>(expr) — special token for DynamicCast
//   - Comments (// and /* */) — skipped
//
// Out of scope (parser throws with a clear error):
//   - The C preprocessor (#include, #define, #ifdef)
//   - UE macros (UCLASS, UPROPERTY, UFUNCTION) — they're decoration
//     for the class scaffold, not function body content
//   - Templates beyond Cast<T>
//   - Lambdas, decltype, exceptions, raw pointer arithmetic
//
// The lexer produces a flat token stream the parser consumes
// recursively. Token positions track line + column for error messages.

#pragma once

#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace bpr::tools {

enum class CppTokenKind {
	// Literals + identifiers.
	Identifier,         // foo, MyClass
	QualifiedName,      // Owner::Func — emitted as one token
	NumberLiteral,      // 42, 3.14, 1.0f
	StringLiteral,      // "foo" or TEXT("foo")
	BoolLiteral,        // true / false
	NullLiteral,        // nullptr

	// Keywords.
	KwIf, KwElse, KwFor, KwWhile, KwSwitch, KwCase, KwDefault,
	KwReturn, KwBreak, KwContinue, KwThis, KwAuto, KwConst,

	// Operators / punctuation.
	Plus, Minus, Star, Slash, Percent,
	EqEq, NotEq, Less, LessEq, Greater, GreaterEq,
	AmpAmp, PipePipe, Bang,
	Assign,             // =
	PlusEq, MinusEq, StarEq, SlashEq,
	Arrow,              // ->
	Dot,                // .
	Comma, Semicolon, Colon,
	LParen, RParen, LBrace, RBrace, LBracket, RBracket,
	Ampersand,          // & — used for ref params / addr-of (rare)

	// Special tokens — the parser dispatches on these directly.
	Cast,               // "Cast" keyword (we treat as keyword to drive Cast<T>(...) parsing)
	Eof,
};

struct CppToken {
	CppTokenKind kind = CppTokenKind::Eof;
	std::string  text;        // raw lexeme (for identifiers, numbers, strings)
	int          line = 1;
	int          column = 1;
};

class CppLexError : public std::runtime_error {
public:
	using std::runtime_error::runtime_error;
};

// Tokenize a string into a token stream. Throws CppLexError with a
// "<line>:<col>: <message>" prefix on malformed input.
std::vector<CppToken> LexCpp(std::string_view source);

// Map a token kind to a printable name (for error messages + tests).
std::string TokenKindName(CppTokenKind k);

} // namespace bpr::tools
