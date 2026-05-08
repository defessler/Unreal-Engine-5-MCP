// CppParse — C++ subset → BPIR. Phase 3 of the BP↔C++ plan.
//
// Closes the loop: source language → BPIR → BP graph (via the existing
// compile_function tool). Pairs with CppEmit (Phase 1C/2A) to give a
// round-trip BPIR → C++ → BPIR identity for the patterns CppEmit
// produces.
//
// The parser accepts a small controlled subset of C++ — enough to
// round-trip what compile_function emits, plus reasonable extensions
// the user might write by hand:
//
//   Statements:    if/else, for(auto& x : c), while, switch+case+default,
//                  return, break, continue, expression-statement,
//                  variable-declaration (auto/typed locals via Cast).
//   Expressions:   identifiers, qualified names, literals, function
//                  calls, member access (. and ->), array index,
//                  Cast<T>(), this, unary (!, -), binary
//                  (arithmetic / comparison / logical / assign).
//   Operator precedence: standard C++ subset.
//
// What's intentionally out of scope (parser throws with a clear error):
//   - The C preprocessor — caller passes a bare function body, no
//     #include / #define / #if.
//   - Templates beyond Cast<T>.
//   - Lambdas, decltype, exception machinery.
//   - Pointer arithmetic.
//
// Output: a BPIR `{kind:"function", ...}` document that ValidateBpir
// accepts. Pipe through compile_function to materialize a BP.
//
// Plan upgrade path: this module's interface is `ParseCppFunction` —
// when libclang vendoring becomes worth it (UE-header awareness, full
// C++ parsing), the implementation is swapped without touching any
// caller. ParseCppExpression / ParseCppStatement are the recursive
// internals; not exposed for now.

#pragma once

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>
#include <string_view>

namespace bpr::tools {

class CppParseError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Parse a single C++ function body (or full function definition) into a
// BPIR function doc. Two forms accepted:
//
//   1. Full function definition:
//        bool TakeDamage(float Damage) {
//            if (bIsAlive) { Health -= Damage; }
//            return true;
//        }
//      The signature is parsed for inputs/outputs.
//
//   2. Bare body block (caller passes signature out-of-band via
//      `signature` arg, or accepts the empty defaults):
//        { if (bIsAlive) { Health -= Damage; } return true; }
//      Or with no surrounding braces.
//
// Throws CppParseError with `<line>:<col>: <message>` on syntax issues.
// The returned doc is validated against the BPIR schema before return —
// validation failures surface as CppParseError.
nlohmann::json ParseCppFunction(std::string_view source);

// Same but takes a pre-built signature when the source is a bare body.
// `signature` is itself a BPIR function-doc shell with `name`, `inputs`,
// `outputs`, `locals` already populated; we fill in `body` from the
// parsed source. Useful when the caller knows the BP's signature
// (e.g. transpiling INTO an existing BP function).
nlohmann::json ParseCppFunction(std::string_view source,
                                const nlohmann::json& signature);

} // namespace bpr::tools
