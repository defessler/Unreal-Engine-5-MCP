// CppClassEmit — full UCLASS .h/.cpp generation from a BPIR class doc.
//
// Phase 2A of the BP↔C++ plan. Layers on top of CppEmit (function-body
// codegen) by adding the UCLASS scaffolding the editor's UBT requires:
//   - #pragma once + matching .generated.h include
//   - UCLASS() macro with inferred specifiers
//   - GENERATED_BODY()
//   - UPROPERTY() decls for every BP variable, with Replicated /
//     EditAnywhere / BlueprintReadWrite / Category specifiers inferred
//     from the BP's variable metadata
//   - UFUNCTION() decls for every BP function, with BlueprintCallable
//     and Category specifiers inferred from function metadata
//   - .cpp file with function bodies + GetLifetimeReplicatedProps()
//     when any variable is Replicated
//
// What v1 generates:
//   - Class declared as `<Prefix><Name>_Generated : public <ParentClass>`
//     (so "BP_Enemy" → "ABP_Enemy_Generated" with parent AActor).
//   - Parent class header inferred from a small lookup table for
//     well-known UE base classes; unknown parents emit a TODO include.
//
// What v1 punts on (deliberately):
//   - SCS component initialization in the constructor (timeline /
//     subobject creation). 2B's unsupported-node treatment will surface
//     these as TODO + sidecar entries.
//   - Per-variable CDO defaults set in the editor's Class Defaults
//     panel (we emit BPVariable.DefaultValue inline as the field's
//     initializer; complex defaults get a TODO).
//   - Native event signatures (BP overrides of ReceiveBeginPlay etc.
//     becoming `virtual void BeginPlay() override;`). Future work.

#pragma once

#include "tools/codegen/CppEmit.h"

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace bpr::tools {

struct CppClassEmitOptions {
    // The MODULE_API export macro (e.g. "MYGAME_API"). Defaults to "" —
    // produces a bare `class Name : public Base {...}`. Configurable so
    // callers writing the file into a real module can pass the right
    // export macro for their build.
    std::string moduleApiMacro;

    // Suffix appended to the class name. Default "_Generated" matches
    // the plan's "companion file" convention. Set to empty to drop in
    // place of the BP entirely.
    std::string classNameSuffix = "_Generated";

    // Pass through to CppEmit for function bodies.
    CppEmitOptions emitOpts;
};

struct CppClassEmitResult {
    std::string headerSource;
    std::string implSource;
    std::string headerFileName;   // suggested filename like "BP_Enemy_Generated.h"
    std::string implFileName;
    std::string className;        // resolved class name e.g. "ABP_Enemy_Generated"
    nlohmann::json notes = nlohmann::json::array();  // unsupported / approximation entries
};

// Emit a full .h/.cpp pair for a BPIR class doc. Throws if the doc
// doesn't validate or isn't a class doc.
CppClassEmitResult EmitCppClass(const nlohmann::json& bpirClassDoc,
                                CppClassEmitOptions opts = {});

// ----- Utilities exposed for tests + Phase 2C -----------------------------

// Map a parent class short name (e.g. "Actor", "ACharacter") to its
// header path ("GameFramework/Actor.h"). Returns empty for unknowns;
// caller emits a TODO include.
std::string ParentClassToHeader(std::string_view parentClassName);

// Build the UPROPERTY specifier list from a BPIR variable decl.
// Returns the inside-parens text — caller wraps with `UPROPERTY(...)`.
std::string BuildUPropertyList(const nlohmann::json& varDecl);

// Build the UFUNCTION specifier list from a BPIR function doc.
std::string BuildUFunctionList(const nlohmann::json& fnDoc);

// Apply the UE class-name prefix convention. "BP_Enemy" with parent
// "AActor" → "ABP_Enemy"; with parent "UObject" → "UBP_Enemy". Already-
// prefixed names pass through unchanged.
std::string PrefixClassName(std::string_view bpName, std::string_view parentClass);

} // namespace bpr::tools
