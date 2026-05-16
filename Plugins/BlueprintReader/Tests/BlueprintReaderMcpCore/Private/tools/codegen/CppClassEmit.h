// CppClassEmit — full UCLASS .h/.cpp generation from a BPIR class doc.
//
// Layers on top of CppEmit (function-body codegen) by adding the UCLASS
// scaffolding UBT requires:
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
// Class naming: `<Prefix><Name>_Generated : public <ParentClass>`
// (so "BP_Enemy" → "ABP_Enemy_Generated" with parent AActor). Parent
// class header inferred from a lookup table for well-known UE base
// classes; unknown parents emit a TODO include.
//
// Deliberate gaps (surface as TODO + sidecar entries via the
// unsupported-node treatment table):
//   - SCS component initialization in the constructor (timelines,
//     CreateDefaultSubobject calls).
//   - Per-variable CDO defaults set in the editor's Class Defaults
//     panel (we emit BPVariable.DefaultValue inline; complex defaults
//     get a TODO).
//   - Native event signatures — BP overrides of ReceiveBeginPlay don't
//     auto-rewrite into `virtual void BeginPlay() override;`.

#pragma once

#include "tools/codegen/CppEmit.h"

#include <nlohmann/json.hpp>

#include <map>
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

	// Prefix inserted between UE's type letter (A/U/I) and the BP's
	// base name. Empty by default. Projects with a house naming
	// convention can pass e.g. "Foo" to get "AFooBP_Enemy_Generated"
	// instead of "ABP_Enemy_Generated". The BP base name itself is
	// also CamelCased: "BP_Enemy" -> "BPEnemy" when this is non-empty
	// (otherwise legacy "BP_Enemy" stays as-is for backward compat).
	std::string classNamePrefix;

	// Fallback category for UPROPERTY decls when the BP variable
	// didn't carry one. Empty -> no Category= specifier emitted.
	std::string categoryDefault;

	// BP-category -> project-category remap. Applied after
	// categoryDefault. Useful for normalizing "Default" /
	// "Internal State" / typo'd variants to a project's house
	// categorization. Keys are matched exactly (case-sensitive).
	std::map<std::string, std::string> categoryRemap;

	// Extra UCLASS() meta key-value pairs. Folded into the macro as
	// `UCLASS(Blueprintable, meta=(K1="V1", K2="V2", ...))`. Projects
	// requiring e.g. `PrioritizeCategories="MyGame"` can pass it here.
	std::map<std::string, std::string> uclassMeta;

	// Pattern for deriving a delegate typedef name from the BP
	// multicast-delegate variable name. `{Name}` is replaced with the
	// variable name verbatim (after F-prefix logic). Default "F{Name}"
	// produces `FOnSomethingHappened` for var `OnSomethingHappened`.
	// Set to "F{Name}Delegate" for the `FOnSomethingHappenedDelegate`
	// house style.
	std::string delegateTypedefPattern = "F{Name}";

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

// ----- Utilities exposed for tests + write_generated_source ---------------

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
