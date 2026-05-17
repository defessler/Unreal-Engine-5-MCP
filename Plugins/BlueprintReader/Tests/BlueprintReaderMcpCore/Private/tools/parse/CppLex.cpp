#include "tools/parse/CppLex.h"

#include <fmt/core.h>

#include <cctype>
#include <map>
#include <stdexcept>

namespace bpr::tools {

std::string TokenKindName(CppTokenKind k) {
	switch (k) {
		case CppTokenKind::Identifier:    return "identifier";
		case CppTokenKind::QualifiedName: return "qualified-name";
		case CppTokenKind::NumberLiteral: return "number";
		case CppTokenKind::StringLiteral: return "string";
		case CppTokenKind::BoolLiteral:   return "bool";
		case CppTokenKind::NullLiteral:   return "nullptr";
		case CppTokenKind::KwIf:          return "if";
		case CppTokenKind::KwElse:        return "else";
		case CppTokenKind::KwFor:         return "for";
		case CppTokenKind::KwWhile:       return "while";
		case CppTokenKind::KwSwitch:      return "switch";
		case CppTokenKind::KwCase:        return "case";
		case CppTokenKind::KwDefault:     return "default";
		case CppTokenKind::KwReturn:      return "return";
		case CppTokenKind::KwBreak:       return "break";
		case CppTokenKind::KwContinue:    return "continue";
		case CppTokenKind::KwThis:        return "this";
		case CppTokenKind::KwAuto:        return "auto";
		case CppTokenKind::KwConst:       return "const";
		case CppTokenKind::Plus:          return "+";
		case CppTokenKind::Minus:         return "-";
		case CppTokenKind::Star:          return "*";
		case CppTokenKind::Slash:         return "/";
		case CppTokenKind::Percent:       return "%";
		case CppTokenKind::EqEq:          return "==";
		case CppTokenKind::NotEq:         return "!=";
		case CppTokenKind::Less:          return "<";
		case CppTokenKind::LessEq:        return "<=";
		case CppTokenKind::Greater:       return ">";
		case CppTokenKind::GreaterEq:     return ">=";
		case CppTokenKind::AmpAmp:        return "&&";
		case CppTokenKind::PipePipe:      return "||";
		case CppTokenKind::Bang:          return "!";
		case CppTokenKind::Assign:        return "=";
		case CppTokenKind::PlusEq:        return "+=";
		case CppTokenKind::MinusEq:       return "-=";
		case CppTokenKind::StarEq:        return "*=";
		case CppTokenKind::SlashEq:       return "/=";
		case CppTokenKind::Arrow:         return "->";
		case CppTokenKind::Dot:           return ".";
		case CppTokenKind::Comma:         return ",";
		case CppTokenKind::Semicolon:     return ";";
		case CppTokenKind::Colon:         return ":";
		case CppTokenKind::LParen:        return "(";
		case CppTokenKind::RParen:        return ")";
		case CppTokenKind::LBrace:        return "{";
		case CppTokenKind::RBrace:        return "}";
		case CppTokenKind::LBracket:      return "[";
		case CppTokenKind::RBracket:      return "]";
		case CppTokenKind::Ampersand:     return "&";
		case CppTokenKind::Cast:          return "Cast";
		case CppTokenKind::Eof:           return "<eof>";
	}
	return "<unknown>";
}

namespace {

// Map keywords to their token kinds. Identifiers that aren't keywords
// stay as Identifier.
const std::map<std::string, CppTokenKind>& KeywordMap() {
	static const std::map<std::string, CppTokenKind> m = {
		{"if",       CppTokenKind::KwIf},
		{"else",     CppTokenKind::KwElse},
		{"for",      CppTokenKind::KwFor},
		{"while",    CppTokenKind::KwWhile},
		{"switch",   CppTokenKind::KwSwitch},
		{"case",     CppTokenKind::KwCase},
		{"default",  CppTokenKind::KwDefault},
		{"return",   CppTokenKind::KwReturn},
		{"break",    CppTokenKind::KwBreak},
		{"continue", CppTokenKind::KwContinue},
		{"true",     CppTokenKind::BoolLiteral},
		{"false",    CppTokenKind::BoolLiteral},
		{"nullptr",  CppTokenKind::NullLiteral},
		{"this",     CppTokenKind::KwThis},
		{"auto",     CppTokenKind::KwAuto},
		{"const",    CppTokenKind::KwConst},
		{"Cast",     CppTokenKind::Cast},
	};
	return m;
}

class Lexer {
public:
	explicit Lexer(std::string_view src) : src_(src) {}

	std::vector<CppToken> Run() {
		std::vector<CppToken> out;
		while (pos_ < src_.size()) {
			SkipWhitespaceAndComments();
			if (pos_ >= src_.size()) break;
			CppToken t = LexOne();
			out.push_back(std::move(t));
		}
		out.push_back({CppTokenKind::Eof, "", line_, col_});
		return out;
	}

private:
	[[noreturn]] void Bad(std::string_view msg) {
		throw CppLexError(fmt::format("{}:{}: {}", line_, col_, msg));
	}

	char Peek(std::size_t offset = 0) const {
		return pos_ + offset < src_.size() ? src_[pos_ + offset] : '\0';
	}

	char Advance() {
		char c = src_[pos_++];
		if (c == '\n') { ++line_; col_ = 1; }
		else            { ++col_; }
		return c;
	}

	bool Match(char c) {
		if (Peek() != c) return false;
		Advance();
		return true;
	}

	void SkipWhitespaceAndComments() {
		while (pos_ < src_.size()) {
			char c = Peek();
			if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { Advance(); }
			else if (c == '/' && Peek(1) == '/') {
				while (pos_ < src_.size() && Peek() != '\n') Advance();
			}
			else if (c == '/' && Peek(1) == '*') {
				Advance(); Advance();
				while (pos_ < src_.size()) {
					if (Peek() == '*' && Peek(1) == '/') {
						Advance(); Advance();
						break;
					}
					Advance();
				}
			}
			else break;
		}
	}

	CppToken MakeTok(CppTokenKind k, std::string text = {}) {
		CppToken t;
		t.kind = k;
		t.text = std::move(text);
		t.line = startLine_;
		t.column = startCol_;
		return t;
	}

	CppToken LexOne() {
		startLine_ = line_;
		startCol_  = col_;
		char c = Peek();

		// Identifier / keyword.
		if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
			std::string ident;
			while (pos_ < src_.size() &&
				   (std::isalnum(static_cast<unsigned char>(Peek())) ||
					Peek() == '_')) {
				ident.push_back(Advance());
			}
			// Look ahead for "::" for qualified names. We accumulate
			// greedily so Owner::Inner::Func is one token.
			while (Peek() == ':' && Peek(1) == ':') {
				ident.push_back(Advance());
				ident.push_back(Advance());
				while (pos_ < src_.size() &&
					   (std::isalnum(static_cast<unsigned char>(Peek())) ||
						Peek() == '_')) {
					ident.push_back(Advance());
				}
			}

			// Keyword check uses just the first segment for non-qualified
			// names; a qualified name is never a keyword.
			if (ident.find("::") == std::string::npos) {
				auto it = KeywordMap().find(ident);
				if (it != KeywordMap().end()) {
					return MakeTok(it->second, ident);
				}
				return MakeTok(CppTokenKind::Identifier, std::move(ident));
			}
			return MakeTok(CppTokenKind::QualifiedName, std::move(ident));
		}

		// Number literal: int or float, with optional f suffix.
		if (std::isdigit(static_cast<unsigned char>(c)) ||
			(c == '.' && std::isdigit(static_cast<unsigned char>(Peek(1))))) {
			std::string num;
			bool sawDot = false;
			while (pos_ < src_.size()) {
				char n = Peek();
				if (std::isdigit(static_cast<unsigned char>(n))) { num.push_back(Advance()); }
				else if (n == '.' && !sawDot) {
					sawDot = true;
					num.push_back(Advance());
				}
				else break;
			}
			// Optional exponent.
			if (Peek() == 'e' || Peek() == 'E') {
				num.push_back(Advance());
				if (Peek() == '+' || Peek() == '-') num.push_back(Advance());
				while (std::isdigit(static_cast<unsigned char>(Peek()))) num.push_back(Advance());
			}
			// Optional f / F / l / L / u / U suffix — consume but don't store.
			while (Peek() == 'f' || Peek() == 'F' || Peek() == 'l' ||
				   Peek() == 'L' || Peek() == 'u' || Peek() == 'U') {
				Advance();
			}
			return MakeTok(CppTokenKind::NumberLiteral, std::move(num));
		}

		// String literal: simple "..." or TEXT("..."). Newlines inside
		// are an error per C++ rules.
		if (c == '"') {
			Advance();
			std::string s;
			while (pos_ < src_.size() && Peek() != '"') {
				if (Peek() == '\\') {
					Advance();
					char escaped = Advance();
					switch (escaped) {
						case 'n': s.push_back('\n'); break;
						case 't': s.push_back('\t'); break;
						case 'r': s.push_back('\r'); break;
						case '\\': s.push_back('\\'); break;
						case '"':  s.push_back('"');  break;
						case '\'': s.push_back('\''); break;
						default: s.push_back(escaped); break;
					}
				} else {
					s.push_back(Advance());
				}
			}
			if (Peek() != '"') Bad("unterminated string literal");
			Advance();
			return MakeTok(CppTokenKind::StringLiteral, std::move(s));
		}

		// Operators / punctuation.
		Advance();
		switch (c) {
			case '+': return Match('=') ? MakeTok(CppTokenKind::PlusEq, "+=")
										: MakeTok(CppTokenKind::Plus, "+");
			case '-':
				if (Match('=')) return MakeTok(CppTokenKind::MinusEq, "-=");
				if (Match('>')) return MakeTok(CppTokenKind::Arrow, "->");
				return MakeTok(CppTokenKind::Minus, "-");
			case '*': return Match('=') ? MakeTok(CppTokenKind::StarEq, "*=")
										: MakeTok(CppTokenKind::Star, "*");
			case '/': return Match('=') ? MakeTok(CppTokenKind::SlashEq, "/=")
										: MakeTok(CppTokenKind::Slash, "/");
			case '%': return MakeTok(CppTokenKind::Percent, "%");
			case '=': return Match('=') ? MakeTok(CppTokenKind::EqEq, "==")
										: MakeTok(CppTokenKind::Assign, "=");
			case '!': return Match('=') ? MakeTok(CppTokenKind::NotEq, "!=")
										: MakeTok(CppTokenKind::Bang, "!");
			case '<': return Match('=') ? MakeTok(CppTokenKind::LessEq, "<=")
										: MakeTok(CppTokenKind::Less, "<");
			case '>': return Match('=') ? MakeTok(CppTokenKind::GreaterEq, ">=")
										: MakeTok(CppTokenKind::Greater, ">");
			case '&': return Match('&') ? MakeTok(CppTokenKind::AmpAmp, "&&")
										: MakeTok(CppTokenKind::Ampersand, "&");
			case '|':
				if (Match('|')) return MakeTok(CppTokenKind::PipePipe, "||");
				Bad("unexpected '|' (only '||' is recognized)");
			case '.':  return MakeTok(CppTokenKind::Dot, ".");
			case ',':  return MakeTok(CppTokenKind::Comma, ",");
			case ';':  return MakeTok(CppTokenKind::Semicolon, ";");
			case ':':  return MakeTok(CppTokenKind::Colon, ":");
			case '(':  return MakeTok(CppTokenKind::LParen, "(");
			case ')':  return MakeTok(CppTokenKind::RParen, ")");
			case '{':  return MakeTok(CppTokenKind::LBrace, "{");
			case '}':  return MakeTok(CppTokenKind::RBrace, "}");
			case '[':  return MakeTok(CppTokenKind::LBracket, "[");
			case ']':  return MakeTok(CppTokenKind::RBracket, "]");
		}
		Bad(fmt::format("unexpected character '{}'", c));
	}

	std::string_view src_;
	std::size_t pos_ = 0;
	int line_ = 1;
	int col_  = 1;
	int startLine_ = 1;
	int startCol_  = 1;
};

} // namespace

std::vector<CppToken> LexCpp(std::string_view source) {
	return Lexer(source).Run();
}

}    // namespace bpr::tools
