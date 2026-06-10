#include "tools/codegen/K2NodeSkeletonEmit.h"

#include "tools/codegen/CppEmit.h"    // SanitizeIdentifier

#include <fmt/core.h>

#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace bpr::tools {

namespace k2node_skeleton_detail {

// Lower-case helper (spec categories are case-insensitive).
std::string Lower(std::string_view s) {
	std::string out(s);
	for (char& c : out) {
		if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c - 'A' + 'a'); }
	}
	return out;
}

// Escape free text for splicing inside a generated C++ string literal. Without
// this, a double-quote/backslash/newline in an agent-supplied title/tooltip/
// default produces uncompilable generated code — or worse, injects statements
// past the closing quote.
std::string EscapeCppStringLiteral(std::string_view in) {
	std::string out;
	out.reserve(in.size());
	for (const char c : in) {
		switch (c) {
			case '\\': out += "\\\\"; break;
			case '"':  out += "\\\""; break;
			case '\n': out += "\\n";  break;
			case '\r': out += "\\r";  break;
			case '\t': out += "\\t";  break;
			default:
				// Drop other control chars — they have no business in a title/
				// tooltip and keep the literal printable.
				if (static_cast<unsigned char>(c) >= 0x20) { out += c; }
		}
	}
	return out;
}

// Strip comment-terminators from text spliced into a /* ... */ block comment —
// a value containing "*/" would otherwise end the comment early.
std::string SafeForBlockComment(std::string_view in) {
	std::string out(in);
	for (std::string::size_type p = out.find("*/"); p != std::string::npos; p = out.find("*/", p)) {
		out.replace(p, 2, "* /");
	}
	return out;
}

struct PinCategoryInfo {
	const char* constant;       // UEdGraphSchema_K2::PC_*
	const char* subConstant;    // optional sub-category constant ("" = none)
	bool wantsSubObject;        // object/class/struct family — takes a UObject* subtype
};

// Map a spec category to the schema constants. Throws on unknown.
PinCategoryInfo MapCategory(const std::string& cat) {
	if (cat == "bool" || cat == "boolean") { return {"UEdGraphSchema_K2::PC_Boolean", "", false}; }
	if (cat == "byte")                     { return {"UEdGraphSchema_K2::PC_Byte", "", false}; }
	if (cat == "int" || cat == "integer")  { return {"UEdGraphSchema_K2::PC_Int", "", false}; }
	if (cat == "int64")                    { return {"UEdGraphSchema_K2::PC_Int64", "", false}; }
	// UE5: "float" pins are PC_Real with a PC_Double sub-category.
	if (cat == "float" || cat == "double" || cat == "real") {
		return {"UEdGraphSchema_K2::PC_Real", "UEdGraphSchema_K2::PC_Double", false};
	}
	if (cat == "name")       { return {"UEdGraphSchema_K2::PC_Name", "", false}; }
	if (cat == "string")     { return {"UEdGraphSchema_K2::PC_String", "", false}; }
	if (cat == "text")       { return {"UEdGraphSchema_K2::PC_Text", "", false}; }
	if (cat == "object")     { return {"UEdGraphSchema_K2::PC_Object", "", true}; }
	if (cat == "class")      { return {"UEdGraphSchema_K2::PC_Class", "", true}; }
	if (cat == "softobject") { return {"UEdGraphSchema_K2::PC_SoftObject", "", true}; }
	if (cat == "softclass")  { return {"UEdGraphSchema_K2::PC_SoftClass", "", true}; }
	if (cat == "struct")     { return {"UEdGraphSchema_K2::PC_Struct", "", true}; }
	if (cat == "wildcard")   { return {"UEdGraphSchema_K2::PC_Wildcard", "", false}; }
	if (cat == "exec") {
		throw std::invalid_argument(
			"generate_k2node_skeleton: exec pins are generated automatically "
			"(omit them from `pins`; set `pure: true` for a node with no exec pins)");
	}
	throw std::invalid_argument(fmt::format(
		"generate_k2node_skeleton: unknown pin category '{}' — valid: bool, byte, "
		"int, int64, float, name, string, text, object, class, softobject, "
		"softclass, struct, wildcard", cat));
}

// The handful of core structs we can spell as a static lookup in generated
// code. Everything else gets a TODO placeholder + a note.
const char* KnownStructLookup(const std::string& shortName) {
	if (shortName == "Vector")      { return "TBaseStructure<FVector>::Get()"; }
	if (shortName == "Rotator")     { return "TBaseStructure<FRotator>::Get()"; }
	if (shortName == "Transform")   { return "TBaseStructure<FTransform>::Get()"; }
	if (shortName == "LinearColor") { return "TBaseStructure<FLinearColor>::Get()"; }
	if (shortName == "Vector2D")    { return "TBaseStructure<FVector2D>::Get()"; }
	return nullptr;
}

// Last path segment of "/Script/Module.Class" → "Class" (also handles bare names).
std::string ShortNameOf(std::string_view path) {
	std::string s(path);
	if (auto colon = s.rfind(':'); colon != std::string::npos) { s = s.substr(0, colon); }
	if (auto dot = s.rfind('.'); dot != std::string::npos) { s = s.substr(dot + 1); }
	if (auto slash = s.rfind('/'); slash != std::string::npos) { s = s.substr(slash + 1); }
	return s;
}

// UE type-letter spelling: known engine classes get their REAL prefix (the
// U-everything heuristic emits guaranteed-broken spellings like "UActor");
// names already prefixed [AUF] + uppercase are kept as-is; everything else
// falls back to the U-prefix heuristic with bConfident=false so the caller
// can TODO-mark it. Output is identifier-sanitized (identifier position).
std::string CppSpellingOf(const std::string& shortName, bool& bConfident) {
	struct Known { const char* shortName; const char* cpp; };
	static const Known kKnown[] = {
		{"Actor", "AActor"}, {"Pawn", "APawn"}, {"Character", "ACharacter"},
		{"PlayerController", "APlayerController"}, {"Controller", "AController"},
		{"GameModeBase", "AGameModeBase"}, {"GameStateBase", "AGameStateBase"},
		{"PlayerState", "APlayerState"}, {"HUD", "AHUD"},
		{"Object", "UObject"}, {"ActorComponent", "UActorComponent"},
		{"SceneComponent", "USceneComponent"}, {"Widget", "UWidget"},
		{"UserWidget", "UUserWidget"},
	};
	for (const Known& k : kKnown) {
		if (shortName == k.shortName) { bConfident = true; return k.cpp; }
	}
	if (shortName.size() >= 2 &&
		(shortName[0] == 'A' || shortName[0] == 'U' || shortName[0] == 'F') &&
		shortName[1] >= 'A' && shortName[1] <= 'Z') {
		bConfident = true;
		return SanitizeIdentifier(shortName);
	}
	bConfident = false;
	return SanitizeIdentifier("U" + shortName);
}

struct TargetFunction {
	std::string cppClass;        // "UMyFunctionLibrary" ("" = class unknown)
	bool classConfident = false; // false = U-prefix heuristic, TODO-mark it
	std::string funcName;        // "DoThing"
};

// Accepts "/Script/Module.Class:Func", "Class::Func", "Class.Func", "Class:Func",
// or a bare "Func" (class unknown). A /Script/ path WITHOUT a function part is
// rejected — its '.' separates module from class, so silently treating it as
// Class.Func would produce confident-looking garbage.
TargetFunction ParseTargetFunction(const std::string& spec) {
	TargetFunction out;
	std::string s = spec;
	std::string::size_type split = s.rfind("::");
	std::string::size_type splitLen = 2;
	if (split == std::string::npos) { split = s.rfind(':'); splitLen = 1; }
	if (split == std::string::npos) {
		if (s.rfind("/Script/", 0) == 0) {
			throw std::invalid_argument(fmt::format(
				"generate_k2node_skeleton: target_function \"{}\" looks like a class "
				"path with no function part — use /Script/Module.Class:Func", spec));
		}
		split = s.rfind('.');
	}
	if (split == std::string::npos) {
		out.funcName = s;
		return out;
	}
	out.funcName = s.substr(split + splitLen);
	out.cppClass = CppSpellingOf(ShortNameOf(s.substr(0, split)), out.classConfident);
	return out;
}

struct PinSpec {
	std::string name;
	bool isInput = true;
	PinCategoryInfo cat{};
	std::string subObject;     // raw spec path, "" = none
	std::string container;     // "none" | "array" | "set" | "map"
	std::string defaultValue;  // "" = none
	bool isReference = false;
};

}    // namespace k2node_skeleton_detail

K2NodeSkeletonEmitResult EmitK2NodeSkeleton(const nlohmann::json& spec) {
	using namespace k2node_skeleton_detail;

	if (!spec.is_object()) {
		throw std::invalid_argument("generate_k2node_skeleton: spec must be a JSON object");
	}

	// ----- validate + normalize the spec -----------------------------------
	std::string rawName = spec.value("class_name", std::string());
	std::string base = SanitizeIdentifier(rawName);
	// Tolerate callers passing the full conventional name.
	for (const char* prefix : {"UK2Node_", "K2Node_"}) {
		if (base.rfind(prefix, 0) == 0) { base = base.substr(std::string(prefix).size()); break; }
	}
	if (base.empty()) {
		throw std::invalid_argument(
			"generate_k2node_skeleton: `class_name` is required (a C++-identifier-safe "
			"name like \"MyNode\"; the UK2Node_ prefix is added automatically)");
	}

	const std::string className = "UK2Node_" + base;
	// module_api lands in CODE position (`{api}_API class ...`) — sanitize, or a
	// non-identifier value breaks (or injects into) the generated declaration.
	const std::string rawApi = spec.value("module_api", std::string());
	const std::string apiMacro = SanitizeIdentifier(rawApi);
	if (!rawApi.empty() && apiMacro.empty()) {
		throw std::invalid_argument(fmt::format(
			"generate_k2node_skeleton: `module_api` \"{}\" has no identifier characters", rawApi));
	}
	const std::string title = EscapeCppStringLiteral(spec.value("title", base));
	const std::string tooltip = EscapeCppStringLiteral(spec.value("tooltip",
		fmt::format("Custom Blueprint node: {}.", spec.value("title", base))));
	const std::string menuCategory = EscapeCppStringLiteral(
		spec.value("menu_category", std::string("Custom")));
	const bool pure = spec.value("pure", false);

	K2NodeSkeletonEmitResult out;
	out.className = className;
	out.headerFileName = fmt::format("K2Node_{}.h", base);
	out.implFileName = fmt::format("K2Node_{}.cpp", base);

	std::vector<PinSpec> pins;
	std::vector<std::string> seenPinNames;    // lower-cased (FName compares case-insensitively)
	if (auto it = spec.find("pins"); it != spec.end() && it->is_array()) {
		for (const auto& p : *it) {
			if (!p.is_object()) {
				throw std::invalid_argument("generate_k2node_skeleton: each `pins` entry must be an object");
			}
			PinSpec ps;
			ps.name = SanitizeIdentifier(p.value("name", std::string()));
			if (ps.name.empty()) {
				throw std::invalid_argument("generate_k2node_skeleton: every pin needs a non-empty `name`");
			}
			// FName comparison is case-insensitive, and SanitizeIdentifier can
			// collapse distinct inputs ("My Value" / "MyValue") into the same
			// identifier — duplicates emit redefined locals (uncompilable) and
			// break FindPin at runtime. Reject clearly.
			const std::string lowered = Lower(ps.name);
			for (const std::string& seen : seenPinNames) {
				if (seen == lowered) {
					throw std::invalid_argument(fmt::format(
						"generate_k2node_skeleton: duplicate pin name '{}' (pin names are "
						"case-insensitive and sanitized — \"My Value\" and \"MyValue\" collide)",
						ps.name));
				}
			}
			seenPinNames.push_back(lowered);
			// "execute"/"then" are the auto exec pins on a non-pure node; a data
			// pin with either name collides AND gets skipped by the generated
			// ExpandNode reroute, silently dropping its links.
			if (!pure && (lowered == "execute" || lowered == "then")) {
				throw std::invalid_argument(fmt::format(
					"generate_k2node_skeleton: pin name '{}' is reserved for the auto "
					"exec pins — rename it, or set pure:true for a node with no exec pins",
					ps.name));
			}
			const std::string dir = Lower(p.value("direction", std::string("input")));
			if (dir != "input" && dir != "output") {
				throw std::invalid_argument(fmt::format(
					"generate_k2node_skeleton: pin '{}': `direction` must be \"input\" or \"output\" (got \"{}\")",
					ps.name, dir));
			}
			ps.isInput = (dir == "input");
			ps.cat = MapCategory(Lower(p.value("category", std::string())));
			ps.subObject = p.value("sub_object", std::string());
			ps.container = Lower(p.value("container", std::string("none")));
			if (ps.container != "none" && ps.container != "array" &&
				ps.container != "set" && ps.container != "map") {
				throw std::invalid_argument(fmt::format(
					"generate_k2node_skeleton: pin '{}': `container` must be none|array|set|map (got \"{}\")",
					ps.name, ps.container));
			}
			ps.defaultValue = p.value("default_value", std::string());
			ps.isReference = p.value("is_reference", false);
			pins.push_back(std::move(ps));
		}
	}

	TargetFunction target;
	const std::string targetSpec = spec.value("target_function", std::string());
	if (!targetSpec.empty()) {
		target = ParseTargetFunction(targetSpec);
		if (target.cppClass.empty()) {
			out.notes.push_back(fmt::format(
				"target_function \"{}\" has no class part — ExpandNode binds by FName on "
				"a TODO class; replace the placeholder with the owning class.", targetSpec));
		} else if (!target.classConfident) {
			out.notes.push_back(fmt::format(
				"ExpandNode calls {}::{} — the class spelling came from the U-prefix "
				"heuristic (TODO-marked in the source); verify the real C++ name + add "
				"its header to the generated .cpp's includes.",
				target.cppClass, target.funcName));
		} else {
			out.notes.push_back(fmt::format(
				"ExpandNode calls {}::{} — add that class's header to the generated "
				".cpp's includes and its module to your Build.cs deps.",
				target.cppClass, target.funcName));
		}
		out.notes.push_back(
			"ExpandNode matches pins by NAME — name input pins exactly after the "
			"target function's parameters and the output pin 'ReturnValue'; "
			"unmatched pins lose their links silently at compile.");
	}

	// ----- header -----------------------------------------------------------
	std::ostringstream H;
	H << "// AUTO-GENERATED by bp-reader generate_k2node_skeleton. Edit freely — this\n";
	H << "// is a starting skeleton, not a regenerated artifact.\n";
	H << "\n";
	H << "#pragma once\n";
	H << "\n";
	H << "#include \"CoreMinimal.h\"\n";
	H << "#include \"K2Node.h\"\n";
	H << fmt::format("#include \"K2Node_{}.generated.h\"\n", base);
	H << "\n";
	H << "class FBlueprintActionDatabaseRegistrar;\n";
	if (!targetSpec.empty()) {
		H << "class FKismetCompilerContext;\n";
	}
	H << "\n";
	H << "UCLASS()\n";
	H << fmt::format("class {}{} : public UK2Node\n",
		apiMacro.empty() ? "" : (apiMacro + "_API "), className);
	H << "{\n";
	H << "\tGENERATED_BODY()\n";
	H << "\n";
	H << "public:\n";
	H << "\t//~ Begin UEdGraphNode Interface\n";
	H << "\tvirtual void AllocateDefaultPins() override;\n";
	H << "\tvirtual FText GetNodeTitle(ENodeTitleType::Type TitleType) const override;\n";
	H << "\tvirtual FText GetTooltipText() const override;\n";
	H << "\t//~ End UEdGraphNode Interface\n";
	H << "\n";
	H << "\t//~ Begin UK2Node Interface\n";
	H << "\tvirtual void GetMenuActions(FBlueprintActionDatabaseRegistrar& ActionRegistrar) const override;\n";
	H << "\tvirtual FText GetMenuCategory() const override;\n";
	if (!targetSpec.empty()) {
		H << "\tvirtual void ExpandNode(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph) override;\n";
	}
	H << fmt::format("\tvirtual bool IsNodePure() const override {{ return {}; }}\n",
		pure ? "true" : "false");
	H << "\t//~ End UK2Node Interface\n";
	H << "};\n";
	out.headerSource = H.str();

	// ----- source -----------------------------------------------------------
	std::ostringstream I;
	I << fmt::format("#include \"K2Node_{}.h\"\n", base);
	I << "\n";
	I << "#include \"BlueprintActionDatabaseRegistrar.h\"\n";
	I << "#include \"BlueprintNodeSpawner.h\"\n";
	I << "#include \"EdGraphSchema_K2.h\"\n";
	if (!targetSpec.empty()) {
		I << "#include \"K2Node_CallFunction.h\"\n";
		I << "#include \"KismetCompiler.h\"\n";
	}
	I << "\n";
	I << fmt::format("#define LOCTEXT_NAMESPACE \"K2Node_{}\"\n", base);
	I << "\n";

	// AllocateDefaultPins
	I << fmt::format("void {}::AllocateDefaultPins()\n", className);
	I << "{\n";
	if (!pure) {
		I << "\t// Execution pins.\n";
		I << "\tCreatePin(EGPD_Input,  UEdGraphSchema_K2::PC_Exec, UEdGraphSchema_K2::PN_Execute);\n";
		I << "\tCreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Exec, UEdGraphSchema_K2::PN_Then);\n";
		if (!pins.empty()) { I << "\n"; }
	}
	for (const PinSpec& p : pins) {
		const char* dirConst = p.isInput ? "EGPD_Input" : "EGPD_Output";
		const bool hasParams = (p.container != "none") || p.isReference;
		std::string paramsVar;
		if (hasParams) {
			paramsVar = p.name + "Params";
			I << fmt::format("\tFCreatePinParams {};\n", paramsVar);
			if (p.container == "array") { I << fmt::format("\t{}.ContainerType = EPinContainerType::Array;\n", paramsVar); }
			if (p.container == "set")   { I << fmt::format("\t{}.ContainerType = EPinContainerType::Set;\n", paramsVar); }
			if (p.container == "map") {
				I << fmt::format("\t{}.ContainerType = EPinContainerType::Map;\n", paramsVar);
				I << fmt::format("\t// TODO: set {}.ValueTerminalType for the map's value type.\n", paramsVar);
				out.notes.push_back(fmt::format(
					"pin '{}': map containers need ValueTerminalType filled in — see the TODO.", p.name));
			}
			if (p.isReference) { I << fmt::format("\t{}.bIsReference = true;\n", paramsVar); }
		}

		// Sub-object argument for the object/class/struct family.
		std::string subObjectArg;
		if (p.cat.wantsSubObject) {
			const std::string shortName = ShortNameOf(p.subObject);
			if (std::string(p.cat.constant).find("PC_Struct") != std::string::npos) {
				if (const char* known = !shortName.empty() ? KnownStructLookup(shortName) : nullptr) {
					subObjectArg = known;
				} else {
					subObjectArg = "/* TODO: the pin's UScriptStruct, e.g. TBaseStructure<FVector>::Get() */ nullptr";
					out.notes.push_back(fmt::format(
						"pin '{}': struct sub-type '{}' isn't a known core struct — fill in the TODO UScriptStruct.",
						p.name, p.subObject.empty() ? "<unspecified>" : p.subObject));
				}
			} else if (!shortName.empty()) {
				bool bConfident = false;
				const std::string cppName = CppSpellingOf(shortName, bConfident);
				if (bConfident) {
					subObjectArg = fmt::format("{}::StaticClass()", cppName);
					out.notes.push_back(fmt::format(
						"pin '{}': sub-type {}::StaticClass() from \"{}\" — add its header to the includes.",
						p.name, cppName, p.subObject));
				} else {
					subObjectArg = fmt::format(
						"/* TODO: verify — derived from \"{}\" */ {}::StaticClass()",
						SafeForBlockComment(p.subObject), cppName);
					out.notes.push_back(fmt::format(
						"pin '{}': sub-type spelled as {}::StaticClass() from \"{}\" via the U-prefix "
						"heuristic — verify the real C++ name + include.",
						p.name, cppName, p.subObject));
				}
			} else {
				subObjectArg = "UObject::StaticClass()";
				out.notes.push_back(fmt::format(
					"pin '{}': no sub_object given — defaulted to UObject::StaticClass(); narrow it.", p.name));
			}
		}

		// The CreatePin call itself.
		std::string call;
		if (!subObjectArg.empty()) {
			call = fmt::format("CreatePin({}, {}, {}, TEXT(\"{}\")", dirConst, p.cat.constant, subObjectArg, p.name);
		} else if (p.cat.subConstant[0] != '\0') {
			call = fmt::format("CreatePin({}, {}, {}, TEXT(\"{}\")", dirConst, p.cat.constant, p.cat.subConstant, p.name);
		} else {
			call = fmt::format("CreatePin({}, {}, TEXT(\"{}\")", dirConst, p.cat.constant, p.name);
		}
		if (hasParams) { call += fmt::format(", {}", paramsVar); }
		call += ")";

		if (!p.defaultValue.empty() && p.isInput) {
			I << fmt::format("\tUEdGraphPin* {}Pin = {};\n", p.name, call);
			I << fmt::format("\t{}Pin->DefaultValue = TEXT(\"{}\");\n",
				p.name, EscapeCppStringLiteral(p.defaultValue));
		} else {
			if (!p.defaultValue.empty()) {
				out.notes.push_back(fmt::format(
					"pin '{}': default_value ignored — defaults only apply to input pins.", p.name));
			}
			I << fmt::format("\t{};\n", call);
		}
	}
	I << "\n";
	I << "\tSuper::AllocateDefaultPins();\n";
	I << "}\n";
	I << "\n";

	// Titles / tooltip / category
	I << fmt::format("FText {}::GetNodeTitle(ENodeTitleType::Type TitleType) const\n", className);
	I << "{\n";
	I << fmt::format("\treturn LOCTEXT(\"NodeTitle\", \"{}\");\n", title);
	I << "}\n";
	I << "\n";
	I << fmt::format("FText {}::GetTooltipText() const\n", className);
	I << "{\n";
	I << fmt::format("\treturn LOCTEXT(\"NodeTooltip\", \"{}\");\n", tooltip);
	I << "}\n";
	I << "\n";
	I << fmt::format("FText {}::GetMenuCategory() const\n", className);
	I << "{\n";
	I << fmt::format("\treturn LOCTEXT(\"MenuCategory\", \"{}\");\n", menuCategory);
	I << "}\n";
	I << "\n";

	// GetMenuActions — the registrar idiom (keyed on the node class so
	// hot-reload re-registration replaces instead of duplicating).
	I << fmt::format("void {}::GetMenuActions(FBlueprintActionDatabaseRegistrar& ActionRegistrar) const\n", className);
	I << "{\n";
	I << "\tUClass* ActionKey = GetClass();\n";
	I << "\tif (ActionRegistrar.IsOpenForRegistration(ActionKey))\n";
	I << "\t{\n";
	I << "\t\tUBlueprintNodeSpawner* NodeSpawner = UBlueprintNodeSpawner::Create(GetClass());\n";
	I << "\t\tcheck(NodeSpawner != nullptr);\n";
	I << "\t\tActionRegistrar.AddBlueprintAction(ActionKey, NodeSpawner);\n";
	I << "\t}\n";
	I << "}\n";

	// ExpandNode — only when there's a target to lower to.
	if (!targetSpec.empty()) {
		const std::string classExpr = target.cppClass.empty()
			? "/* TODO: the owning class */ nullptr"
			: (target.classConfident
				? fmt::format("{}::StaticClass()", target.cppClass)
				: fmt::format("/* TODO: verify the C++ spelling */ {}::StaticClass()", target.cppClass));
		I << "\n";
		I << fmt::format("void {}::ExpandNode(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph)\n", className);
		I << "{\n";
		I << "\tSuper::ExpandNode(CompilerContext, SourceGraph);\n";
		I << "\n";
		I << "\t// Lower this node to a plain CallFunction on the target function.\n";
		I << "\tUK2Node_CallFunction* CallFunction =\n";
		I << "\t\tCompilerContext.SpawnIntermediateNode<UK2Node_CallFunction>(this, SourceGraph);\n";
		I << fmt::format("\tCallFunction->FunctionReference.SetExternalMember(\n");
		I << fmt::format("\t\tFName(TEXT(\"{}\")), {});\n",
			EscapeCppStringLiteral(target.funcName), classExpr);
		I << "\tCallFunction->AllocateDefaultPins();\n";
		I << "\n";
		if (!pure) {
			I << "\t// Re-route exec wiring.\n";
			I << "\tCompilerContext.MovePinLinksToIntermediate(*GetExecPin(), *CallFunction->GetExecPin());\n";
			I << "\tCompilerContext.MovePinLinksToIntermediate(\n";
			I << "\t\t*FindPinChecked(UEdGraphSchema_K2::PN_Then), *CallFunction->GetThenPin());\n";
			I << "\n";
		}
		I << "\t// Re-route each data pin to the same-named pin on the call.\n";
		I << "\tfor (UEdGraphPin* Pin : Pins)\n";
		I << "\t{\n";
		I << "\t\tif (Pin->bOrphanedPin ||\n";
		I << "\t\t\tPin->PinName == UEdGraphSchema_K2::PN_Execute ||\n";
		I << "\t\t\tPin->PinName == UEdGraphSchema_K2::PN_Then)\n";
		I << "\t\t{\n";
		I << "\t\t\tcontinue;\n";
		I << "\t\t}\n";
		I << "\t\tif (UEdGraphPin* TargetPin = CallFunction->FindPin(Pin->PinName))\n";
		I << "\t\t{\n";
		I << "\t\t\tCompilerContext.MovePinLinksToIntermediate(*Pin, *TargetPin);\n";
		I << "\t\t}\n";
		I << "\t}\n";
		I << "\n";
		I << "\t// This node is fully replaced by the expansion.\n";
		I << "\tBreakAllNodeLinks();\n";
		I << "}\n";
		if (pure) {
			out.notes.push_back(
				"pure node with a target_function: the target must itself be BlueprintPure "
				"(no exec pins to re-route) — verify the call lowers cleanly.");
		}
	} else {
		out.notes.push_back(
			"no target_function given — ExpandNode is omitted (a node that doesn't "
			"expand shouldn't override it); the node compiles but does nothing until "
			"you add behavior.");
	}

	I << "\n";
	I << "#undef LOCTEXT_NAMESPACE\n";
	out.implSource = I.str();

	out.notes.push_back(
		"consuming editor module needs Build.cs deps: BlueprintGraph, KismetCompiler, "
		"UnrealEd (editor-only module — custom K2 nodes don't ship in cooked builds).");
	return out;
}

}    // namespace bpr::tools
