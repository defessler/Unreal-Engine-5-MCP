// CppEmit — BPIR → C++ source code generation.
//
// Walks a BPIR function/class document and emits annotated C++. The
// "readable" mode emits syntactically valid C++ with real type names
// and indentation; UFUNCTION / UPROPERTY decoration appears as comments,
// and complex scaffolding (UCLASS body, GENERATED_BODY(), constructor,
// replication registration) is added by CppClassEmit's whole-class
// pipeline.

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
    // Each unsupported / approximation node encountered. Whole-class
    // codegen builds a sidecar JSON from this for triage.
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

// UPROPERTY-context variant: same as MapBpirTypeToCpp but wraps object
// references in `TObjectPtr<>` per UE5 convention (since 5.0, Epic
// recommends TObjectPtr for class-member UObject* properties; raw
// pointers still work but trip Epic-internal lint and miss the editor
// optimization). Use for class data members (UPROPERTY decls). Function
// arguments and local variables still use raw pointers — TObjectPtr is
// only for headers' UPROPERTY-marked fields.
std::string MapBpirTypeToCppMember(std::string_view bpirType);

} // namespace bpr::tools
