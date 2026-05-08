// CppEmit — BPIR → C++ source code generation (Phase 1C).
//
// Walks a BPIR function/class document and emits annotated C++. Phase 1
// emits "readable" mode: syntactically valid-looking C++ with real type
// names and indentation, but not necessarily compilable as-is — UFUNCTION
// / UPROPERTY decoration markers appear as comments, and complex
// scaffolding (UCLASS body, GENERATED_BODY(), constructor, replication
// registration) is added by Phase 2's CppClassEmit.
//
// Phase 2 layers on top: same AST walker, "compilable" mode flips on
// the scaffolding generators.

#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace bpr::tools {

struct CppEmitOptions {
    enum class Mode { Readable, Compilable };
    Mode mode = Mode::Readable;
    int  indentSpaces = 4;
    // When true, every BPIR `{call: "+"}` etc. is rendered with the
    // operator alias (e.g. `a + b`) instead of the canonical
    // `UKismetMathLibrary::Add_IntInt(a, b)` call. Default true.
    bool useOperatorAliases = true;
};

struct CppEmitResult {
    std::string source;
    // Each unsupported / approximation node encountered. Phase 2's
    // sidecar JSON is built from this.
    nlohmann::json notes = nlohmann::json::array();
};

// Emit C++ for a BPIR function doc. Caller handles surrounding scaffold
// (`<className>::<funcName>(args) { ... }`) — this fn renders only the
// body block (the {} contents). For a free-standing function or a
// snippet in test fixtures, wrap with EmitFunctionSignature manually.
//
// Throws std::invalid_argument if `bpirFunctionDoc` doesn't validate
// (we re-run the validator here so codegen never sees a malformed doc).
CppEmitResult EmitCppFunctionBody(const nlohmann::json& bpirFunctionDoc,
                                  CppEmitOptions opts = {});

// Emit a complete `<returnType> <name>(<args>) { <body> }` block.
// Useful for standalone-snippet rendering and for the tests that pin
// specific outputs.
CppEmitResult EmitCppFunction(const nlohmann::json& bpirFunctionDoc,
                              CppEmitOptions opts = {});

// Convert a BPIR type-shorthand string ("float", "object:Actor",
// "[]float", "{string:int}") to the C++ form ("float", "AActor*",
// "TArray<float>", "TMap<FString, int32>"). Exposed so other codegen
// passes (CppClassEmit's UPROPERTY decls) can reuse it.
std::string MapBpirTypeToCpp(std::string_view bpirType);

} // namespace bpr::tools
