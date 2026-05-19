#include "tools/codegen/CppEmit.h"
#include "tools/Bpir.h"
#include "tools/codegen/UnsupportedTreatment.h"

#include <fmt/core.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace bpr::tools {

namespace cpp_emit_detail {

// SanitizeIdentifier lives at file scope (defined below, outside the
// anon namespace) so CppClassEmit.cpp -- a separate translation unit --
// can call it through the public header. All call sites in this TU
// use the unqualified name; ADL + the file-scope declaration resolve
// to the external-linkage version.

// Escape a string for embedding inside a TEXT("...") wide-string literal.
// Without this, a BP default like Hello "world" leaks into the C++ as
// `TEXT("Hello "world"")` which doesn't compile. Handles the C++
// literal escapes: backslash, double-quote, newline, carriage return,
// tab. Tabs and CRs in BP data are rare but possible.
std::string EscapeForCppStringLiteral(std::string_view in) {
	std::string out;
	out.reserve(in.size() + 4);
	for (char c : in) {
		switch (c) {
		case '\\': out += "\\\\"; break;
		case '"':  out += "\\\""; break;
		case '\n': out += "\\n";  break;
		case '\r': out += "\\r";  break;
		case '\t': out += "\\t";  break;
		default:   out.push_back(c); break;
		}
	}
	return out;
}

// ResolvedRef: classifies an asset-path-ish identifier and gives the
// clean C++ form. BPIR carries fully-qualified asset paths in `cast.to`,
// `call`-form fnName, and some metadata fields; without classification +
// stripping, those paths leak into C++ output verbatim as illegal
// identifiers like `/Script/MyGame.FooSubsystem::IsBar(...)`.
//
// Kinds:
//   Plain    — no `/Script/` or `/Game/` prefix detected; pass through.
//   Native   — `/Script/<Module>.<Class>[::<Member>]` — strip prefix to bare class.
//   BpClass  — `/Game/<Path>.<Asset>_C[::<Member>]` — BP-only class. The
//              member call must go through reflection (FindFunction +
//              ProcessEvent) since the C++ binary has no symbol for it.
struct ResolvedRef {
	enum class Kind { Plain, Native, BpClass };
	Kind kind = Kind::Plain;
	std::string bareOwner;   // stripped class name (no path, no _C suffix)
	std::string memberName;  // function / property after `::`, or empty
};

ResolvedRef ResolveAssetPath(std::string_view ref) {
	ResolvedRef out;
	// Split owner / member at the LAST `::` so we don't accidentally
	// split inside template arguments (rare here but defensive).
	std::string_view owner = ref;
	std::string_view member;
	if (auto pos = ref.rfind("::"); pos != std::string_view::npos) {
		owner  = ref.substr(0, pos);
		member = ref.substr(pos + 2);
	}
	out.memberName = std::string(member);

	auto stripTrailingC = [](std::string_view s) -> std::string_view {
		if (s.size() > 2 && s.substr(s.size() - 2) == "_C") {
			return s.substr(0, s.size() - 2);
		}
		return s;
	};

	if (owner.rfind("/Script/", 0) == 0) {
		// /Script/<Module>.<Class>
		auto dot = owner.find_last_of('.');
		std::string_view bare = (dot == std::string_view::npos) ? owner : owner.substr(dot + 1);
		out.kind = ResolvedRef::Kind::Native;
		out.bareOwner = std::string(stripTrailingC(bare));
		return out;
	}
	if (owner.rfind("/Game/", 0) == 0) {
		// /Game/<Path>.<Asset>_C  -- BP class.
		auto dot = owner.find_last_of('.');
		std::string_view bare = (dot == std::string_view::npos) ? owner : owner.substr(dot + 1);
		out.kind = ResolvedRef::Kind::BpClass;
		out.bareOwner = std::string(stripTrailingC(bare));
		return out;
	}
	// Plain identifier. Still strip _C if present (sometimes Cast<X_C>
	// shows up post-decompile without the path prefix).
	out.bareOwner = std::string(stripTrailingC(owner));
	out.kind = ResolvedRef::Kind::Plain;
	return out;
}

// ----- Type shorthand → C++ -----------------------------------------------
// Inverse of TypeShorthand.cpp's parser. Bidirectional with a caveat:
// some BPPinType shapes don't have a clean shorthand round-trip
// (multi-level template types), but the common surface does.
//
// `forMember` toggles UE5's TObjectPtr<> wrapping for class-member
// UObject* fields — Epic recommends since 5.0 and the editor reflects
// pointer fields more efficiently when wrapped. Function args and
// local pointers stay raw (TObjectPtr is only for member properties).

std::string MapInner(std::string_view inner, bool forMember);

std::string MapTypeRecursive(std::string_view t, bool forMember) {
	// Container prefix dispatch.
	if (t.size() >= 2 && t[0] == '[' && t[1] == ']') {
		// TArray of pointers inside a UPROPERTY also benefits from
		// TObjectPtr at the element level — propagate the flag.
		return std::string("TArray<") + MapTypeRecursive(t.substr(2), forMember) + ">";
	}
	// {}T form (set, post-prefix): two-char prefix + rest.
	if (t.size() >= 2 && t[0] == '{' && t[1] == '}') {
		return std::string("TSet<") + MapTypeRecursive(t.substr(2), forMember) + ">";
	}
	// {K:V} or {T} form (the matching-brace variants TypeShorthand also accepts).
	if (t.size() >= 2 && t[0] == '{' && t.back() == '}') {
		std::string_view inner = t.substr(1, t.size() - 2);
		auto colon = inner.find(':');
		if (colon == std::string_view::npos) {
			return std::string("TSet<") + MapTypeRecursive(inner, forMember) + ">";
		}
		return std::string("TMap<") +
			   MapTypeRecursive(inner.substr(0, colon), forMember) + ", " +
			   MapTypeRecursive(inner.substr(colon + 1), forMember) + ">";
	}
	return MapInner(t, forMember);
}

std::string MapInner(std::string_view inner, bool forMember) {
	// Accept two BPIR-shorthand subcategory delimiters: ":" (canonical,
	// e.g. "object:/Script/Engine.Actor") and "(...)" (the form the
	// plugin's TerminalToString produces for object/class/struct pins,
	// e.g. "object(/Script/Engine.Actor)"). Without the paren branch
	// types like "object(/Script/...)" pass through unmatched and emit
	// literally — UHT then rejects the result.
	auto colon = inner.find(':');
	auto paren = inner.find('(');
	std::string_view name, sub;
	if (colon != std::string_view::npos &&
	    (paren == std::string_view::npos || colon < paren)) {
		name = inner.substr(0, colon);
		sub  = inner.substr(colon + 1);
	} else if (paren != std::string_view::npos && inner.back() == ')') {
		name = inner.substr(0, paren);
		sub  = inner.substr(paren + 1, inner.size() - paren - 2);
	} else {
		name = inner;
		sub  = std::string_view{};
	}

	if (name == "bool")
	{
		return "bool";
	}
	if (name == "byte")
	{
		return "uint8";
	}
	if (name == "int")
	{
		return "int32";
	}
	if (name == "int64")
	{
		return "int64";
	}
	if (name == "float")
	{
		return "float";
	}
	if (name == "real")
	{
		return "float";    // BP "real" defaults to float without subcategory
	}
	if (name == "double")
	{
		return "double";
	}
	if (name == "string")
	{
		return "FString";
	}
	if (name == "name")
	{
		return "FName";
	}
	if (name == "text")
	{
		return "FText";
	}
	if (name == "exec")
	{
		return "void";
	}

	if (name == "object" || name == "soft_object") {
		// Helper: render the bare class name with its UE prefix.
		auto renderBare = [](std::string_view bare) -> std::string {
			// Already-prefixed: A/U followed by another uppercase letter.
			if (bare.size() >= 2 &&
				(bare[0] == 'A' || bare[0] == 'U') &&
				(bare[1] >= 'A' && bare[1] <= 'Z')) {
				return std::string(bare);
			}
			// Actor-derived heuristic.
			if (bare == "Actor" ||
				(bare.size() > 5 && bare.substr(bare.size() - 5) == "Actor") ||
				bare == "Pawn" || bare == "Character" ||
				bare == "Controller" || bare == "PlayerController" ||
				bare == "PlayerState" || bare == "GameMode" ||
				bare == "GameState" || bare == "HUD") {
				return std::string("A") + std::string(bare);
			}
			return std::string("U") + std::string(bare);
		};

		const bool isSoft = (name == "soft_object");
		// Wrap rendered class for member context. Soft refs always use
		// TSoftObjectPtr (their whole point is deferred loading + safe
		// disk references); hard refs use TObjectPtr in member context,
		// raw pointer in arg / local context.
		auto wrap = [forMember, isSoft](const std::string& cls) {
			if (isSoft)
			{
				return std::string("TSoftObjectPtr<") + cls + ">";
			}
			return forMember
				? std::string("TObjectPtr<") + cls + ">"
				: cls + "*";
		};

		if (sub.empty())
		{
			return wrap("UObject");
		}

		// Strip /Script/Module.ClassName path prefix down to bare
		// ClassName.
		std::string_view bare = sub;
		if (auto dot = bare.find_last_of('.'); dot != std::string_view::npos) {
			bare = bare.substr(dot + 1);
		}
		// Strip the BP "_C" suffix UE serializes for generated classes.
		if (bare.size() > 2 &&
			bare.substr(bare.size() - 2) == "_C") {
			bare = bare.substr(0, bare.size() - 2);
		}
		return wrap(renderBare(bare));
	}
	if (name == "class" || name == "soft_class") {
		// Soft class refs survive package boundaries — TSoftClassPtr is
		// the canonical UE5 type. Hard class refs use TSubclassOf which
		// gives compile-time type safety + the editor's class picker.
		// Strip "/Script/Module.Class" -> "Class" so the template
		// parameter is a valid C++ identifier (otherwise we emit forms
		// like TSubclassOf</Script/GameplayAbilities.GameplayEffect>).
		auto barename = [](std::string_view in) -> std::string {
			std::string_view b = in;
			if (auto dot = b.find_last_of('.'); dot != std::string_view::npos) {
				b = b.substr(dot + 1);
			}
			if (b.size() > 2 && b.substr(b.size() - 2) == "_C") {
				b = b.substr(0, b.size() - 2);
			}
			std::string out(b);
			if (out.empty()) return out;
			if (out.size() >= 2 &&
				(out[0] == 'A' || out[0] == 'U' || out[0] == 'I') &&
				out[1] >= 'A' && out[1] <= 'Z') return out;
			auto endsWith = [&](std::string_view s) {
				return out.size() >= s.size() &&
				       out.substr(out.size() - s.size()) == s;
			};
			if (endsWith("Actor") || endsWith("Pawn") || endsWith("Character") ||
			    endsWith("Controller") || endsWith("PlayerState") ||
			    endsWith("GameMode") || endsWith("GameModeBase") ||
			    endsWith("GameState") || endsWith("HUD") || endsWith("Info")) {
				return "A" + out;
			}
			return "U" + out;
		};
		if (name == "soft_class") {
			if (sub.empty())
			{
				return "TSoftClassPtr<UObject>";
			}
			return std::string("TSoftClassPtr<") + barename(sub) + ">";
		}
		if (sub.empty())
		{
			return "UClass*";
		}
		return std::string("TSubclassOf<") + barename(sub) + ">";
	}
	if (name == "interface") {
		return std::string("TScriptInterface<I") + std::string(sub) + ">";
	}
	if (name == "struct") {
		if (sub.empty())
		{
			return "FStruct";
		}
		// Strip "/Script/Module.<StructName>" -> "<StructName>" so the
		// F-prefix joins the bare identifier instead of yielding garbage
		// like "F/Script/CoreUObject.Vector".
		std::string_view bare = sub;
		if (auto dot = bare.find_last_of('.'); dot != std::string_view::npos) {
			bare = bare.substr(dot + 1);
		}
		if (!bare.empty() && bare[0] == 'F')
		{
			return std::string(bare);
		}
		return std::string("F") + std::string(bare);
	}
	return std::string(name);  // unknown — pass through
}

// ----- Operator-alias reverse map ----------------------------------------
// The forward direction (CompileFunction.cpp) maps "+" → "Add_IntInt".
// We reverse it here. When `useOperatorAliases` is on and the qualified
// call name matches one of these, render as `lhs OP rhs` instead of
// `Owner::Func(lhs, rhs)`.

struct OpReverse {
	std::string op;
	int arity;  // 1 or 2
};
const std::map<std::string, OpReverse>& OpReverseMap() {
	static const std::map<std::string, OpReverse> m = {
		{"KismetMathLibrary::Add_IntInt",        {"+",  2}},
		{"KismetMathLibrary::Subtract_IntInt",   {"-",  2}},
		{"KismetMathLibrary::Multiply_IntInt",   {"*",  2}},
		{"KismetMathLibrary::Divide_IntInt",     {"/",  2}},
		{"KismetMathLibrary::Percent_IntInt",    {"%",  2}},
		{"KismetMathLibrary::EqualEqual_IntInt", {"==", 2}},
		{"KismetMathLibrary::NotEqual_IntInt",   {"!=", 2}},
		{"KismetMathLibrary::Less_IntInt",       {"<",  2}},
		{"KismetMathLibrary::LessEqual_IntInt",  {"<=", 2}},
		{"KismetMathLibrary::Greater_IntInt",    {">",  2}},
		{"KismetMathLibrary::GreaterEqual_IntInt",{">=", 2}},
		{"KismetMathLibrary::BooleanAND",        {"&&", 2}},
		{"KismetMathLibrary::BooleanOR",         {"||", 2}},
		{"KismetMathLibrary::Not_PreBool",       {"!",  1}},
		{"KismetMathLibrary::Add_FloatFloat",    {"+",  2}},
		{"KismetMathLibrary::Subtract_FloatFloat",{"-", 2}},
		{"KismetMathLibrary::Multiply_FloatFloat",{"*", 2}},
		{"KismetMathLibrary::Divide_FloatFloat", {"/",  2}},
		{"KismetMathLibrary::EqualEqual_FloatFloat",{"==", 2}},
		{"KismetMathLibrary::Less_FloatFloat",   {"<",  2}},
		{"KismetMathLibrary::LessEqual_FloatFloat",{"<=", 2}},
		// Vector arithmetic (FVector overloads binary operators).
		{"KismetMathLibrary::Add_VectorVector",       {"+",  2}},
		{"KismetMathLibrary::Subtract_VectorVector",  {"-",  2}},
		{"KismetMathLibrary::Multiply_VectorVector",  {"*",  2}},
		{"KismetMathLibrary::Multiply_VectorFloat",   {"*",  2}},
		{"KismetMathLibrary::Multiply_VectorInt",     {"*",  2}},
		{"KismetMathLibrary::Divide_VectorVector",    {"/",  2}},
		{"KismetMathLibrary::Divide_VectorFloat",     {"/",  2}},
		{"KismetMathLibrary::Divide_VectorInt",       {"/",  2}},
		{"KismetMathLibrary::Negate_VectorVector",    {"-",  1}},
		{"KismetMathLibrary::EqualEqual_VectorVector",{"==", 2}},
		{"KismetMathLibrary::NotEqual_VectorVector",  {"!=", 2}},
		// Vector2D arithmetic.
		{"KismetMathLibrary::Add_Vector2DVector2D",      {"+", 2}},
		{"KismetMathLibrary::Subtract_Vector2DVector2D", {"-", 2}},
		{"KismetMathLibrary::Multiply_Vector2DFloat",    {"*", 2}},
		// Rotator arithmetic (FRotator overloads + / -).
		{"KismetMathLibrary::ComposeRotators",          {"+",  2}},  // BP name
		{"KismetMathLibrary::Add_RotatorRotator",       {"+",  2}},
		{"KismetMathLibrary::Subtract_RotatorRotator",  {"-",  2}},
		{"KismetMathLibrary::Multiply_RotatorFloat",    {"*",  2}},
		{"KismetMathLibrary::Multiply_RotatorInt",      {"*",  2}},
		{"KismetMathLibrary::NegateRotator",            {"-",  1}},
		{"KismetMathLibrary::EqualEqual_RotatorRotator",{"==", 2}},
		{"KismetMathLibrary::NotEqual_RotatorRotator",  {"!=", 2}},
		// String / Name / Text comparisons.
		{"KismetStringLibrary::EqualEqual_StrStr",     {"==", 2}},
		{"KismetStringLibrary::NotEqual_StrStr",       {"!=", 2}},
		{"KismetStringLibrary::Concat_StrStr",         {"+",  2}},
		{"KismetNameLibrary::EqualEqual_NameName",     {"==", 2}},
		{"KismetNameLibrary::NotEqual_NameName",       {"!=", 2}},
	};
	return m;
}

// ----- Method-call alias table --------------------------------------------
// BP's UKismetArrayLibrary / KismetStringLibrary functions take the
// container as a specifically-named argument; the C++ canonical form
// is a method call on the container (`Array.Add(Item)` rather than
// `Array_Add(Array, Item)`). Each entry maps the qualified BP name to
// `{method, receiverArgKey}` — we look up the receiver by its arg
// name (e.g. "TargetArray", "SourceString") rather than position, so
// the alphabetical JSON-key iteration doesn't reshuffle the receiver
// off the front.
struct MethodAlias {
	std::string method;
	std::string receiverArg;
};
const std::map<std::string, MethodAlias>& MethodCallAliases() {
	static const std::map<std::string, MethodAlias> m = {
		// KismetArrayLibrary — receiver pin is "TargetArray"
		{"KismetArrayLibrary::Array_Add",         {"Add",         "TargetArray"}},
		{"KismetArrayLibrary::Array_AddUnique",   {"AddUnique",   "TargetArray"}},
		{"KismetArrayLibrary::Array_Append",      {"Append",      "TargetArray"}},
		{"KismetArrayLibrary::Array_Length",      {"Num",         "TargetArray"}},
		{"KismetArrayLibrary::Array_IsEmpty",     {"IsEmpty",     "TargetArray"}},
		{"KismetArrayLibrary::Array_IsNotEmpty",  {"IsEmpty",     "TargetArray"}},  // negated below
		{"KismetArrayLibrary::Array_Contains",    {"Contains",    "TargetArray"}},
		{"KismetArrayLibrary::Array_Find",        {"Find",        "TargetArray"}},
		{"KismetArrayLibrary::Array_RemoveItem",  {"Remove",      "TargetArray"}},
		{"KismetArrayLibrary::Array_RemoveIndex", {"RemoveAt",    "TargetArray"}},
		{"KismetArrayLibrary::Array_Clear",       {"Empty",       "TargetArray"}},
		{"KismetArrayLibrary::Array_Resize",      {"SetNum",      "TargetArray"}},
		{"KismetArrayLibrary::Array_Reverse",     {"Reverse",     "TargetArray"}},
		{"KismetArrayLibrary::Array_IsValidIndex",{"IsValidIndex","TargetArray"}},
		{"KismetArrayLibrary::Array_LastIndex",   {"Num",         "TargetArray"}},
		// KismetStringLibrary — receiver pin is "SourceString" (some
		// overloads use "InString"; pre-check the alt below).
		{"KismetStringLibrary::Len",              {"Len",         "SourceString"}},
		{"KismetStringLibrary::ToLower",          {"ToLower",     "SourceString"}},
		{"KismetStringLibrary::ToUpper",          {"ToUpper",     "SourceString"}},
		{"KismetStringLibrary::TrimStart",        {"TrimStart",   "SourceString"}},
		{"KismetStringLibrary::TrimEnd",          {"TrimEnd",     "SourceString"}},
		{"KismetStringLibrary::IsEmpty",          {"IsEmpty",     "InString"}},
		{"KismetStringLibrary::Contains",         {"Contains",    "SearchIn"}},
		{"KismetStringLibrary::StartsWith",       {"StartsWith",  "SearchIn"}},
		{"KismetStringLibrary::EndsWith",         {"EndsWith",    "SearchIn"}},
		// TSet / TMap don't have a separate Kismet library; their BP
		// ops route through K2Node_CallFunction directly on a set/map.
	};
	return m;
}

// ----- Qualified-name shortening table ------------------------------------
// Some UE helpers have a clean unqualified C++ form (IsValid, GetWorld,
// GetGameInstance, etc.) — BP routes through KismetSystemLibrary for the
// boolean version, but C++ uses the global form. We rewrite these to
// the unqualified form so the generated code reads idiomatically.
const std::map<std::string, std::string>& NameAliases() {
	static const std::map<std::string, std::string> m = {
		{"KismetSystemLibrary::IsValid",       "IsValid"},
		{"KismetSystemLibrary::IsValidClass",  "IsValid"},
		{"KismetSystemLibrary::MakeLiteralInt",      ""},   // identity (returns the literal)
		{"KismetSystemLibrary::MakeLiteralFloat",    ""},
		{"KismetSystemLibrary::MakeLiteralBool",     ""},
		{"KismetSystemLibrary::MakeLiteralString",   ""},
		{"KismetSystemLibrary::MakeLiteralName",     ""},
		{"KismetSystemLibrary::MakeLiteralText",     ""},
		// GetClass node renders as `<obj>->GetClass()` not as a free call,
		// but BP's GetObjectClass routes through this — render bare.
		{"GameplayStatics::GetClass",          "GetClass"},
	};
	return m;
}

// ----- WorldContext injection table ---------------------------------------
// BP hides the WorldContextObject pin on these functions and passes
// `this` implicitly during compile. The C++ signatures still require
// the world-context object as the first argument, so we must inject it
// when transpiling. List is intentionally narrow — only well-known UE5
// helpers; missing entries get rendered without the world arg and the
// agent can patch if needed.
const std::set<std::string>& WorldContextFunctions() {
	static const std::set<std::string> s = {
		// KismetSystemLibrary
		"KismetSystemLibrary::PrintString",
		"KismetSystemLibrary::PrintText",
		"KismetSystemLibrary::PrintWarning",
		"KismetSystemLibrary::SphereTraceSingle",
		"KismetSystemLibrary::SphereTraceMulti",
		"KismetSystemLibrary::LineTraceSingle",
		"KismetSystemLibrary::LineTraceMulti",
		"KismetSystemLibrary::BoxTraceSingle",
		"KismetSystemLibrary::BoxTraceMulti",
		"KismetSystemLibrary::CapsuleTraceSingle",
		"KismetSystemLibrary::CapsuleTraceMulti",
		"KismetSystemLibrary::DrawDebugLine",
		"KismetSystemLibrary::DrawDebugBox",
		"KismetSystemLibrary::DrawDebugSphere",
		"KismetSystemLibrary::DrawDebugString",
		"KismetSystemLibrary::DrawDebugArrow",
		"KismetSystemLibrary::GetGameTimeInSeconds",
		"KismetSystemLibrary::K2_SetTimer",
		// GameplayStatics
		"GameplayStatics::GetPlayerController",
		"GameplayStatics::GetPlayerPawn",
		"GameplayStatics::GetPlayerCharacter",
		"GameplayStatics::GetPlayerCameraManager",
		"GameplayStatics::GetGameMode",
		"GameplayStatics::GetGameState",
		"GameplayStatics::GetGameInstance",
		"GameplayStatics::SpawnActor",
		"GameplayStatics::SpawnActorAtLocation",
		"GameplayStatics::SpawnEmitterAtLocation",
		"GameplayStatics::SpawnSoundAtLocation",
		"GameplayStatics::SpawnSound2D",
		"GameplayStatics::PlaySoundAtLocation",
		"GameplayStatics::PlaySound2D",
		"GameplayStatics::OpenLevel",
		"GameplayStatics::GetAllActorsOfClass",
		"GameplayStatics::GetAllActorsWithTag",
		"GameplayStatics::GetActorOfClass",
		"GameplayStatics::ApplyDamage",
		"GameplayStatics::ApplyPointDamage",
		"GameplayStatics::ApplyRadialDamage",
		// KismetMaterialLibrary (rare in actor BPs but world-aware)
		"KismetMaterialLibrary::CreateDynamicMaterialInstance",
	};
	return s;
}

// ----- Emitter state ------------------------------------------------------
struct Emitter {
	std::ostringstream out;
	int indentLevel = 0;
	CppEmitOptions opts;
	nlohmann::json notes = nlohmann::json::array();

	// Names of the function's outputs[]. Empty for void / single-return
	// functions; populated with sanitized parameter names when the
	// function declared 2+ outputs (which become reference-out params).
	// EmitStatement's return-form uses this to lower
	// {return: [<e1>, <e2>]} into per-output assignments + bare return,
	// instead of std::make_tuple which isn't part of UE's allowed stdlib.
	std::vector<std::string> outputNames;

	// Sanitized parameter + local names. Used to disambiguate member
	// accesses that shadow a parameter: `void SetPlayer(Player) { Player
	// = Player; }` is a self-assignment unless we emit `this->Player =
	// Player;` for the LHS. Populated by EmitCppFunctionBody from
	// doc["inputs"] + doc["locals"].
	std::set<std::string> paramAndLocalNames;

	void Indent() {
		for (int i = 0; i < indentLevel * opts.indentSpaces; ++i)
		{
			out << ' ';
		}
	}
	void Line(std::string_view s) { Indent(); out << s << "\n"; }
	void RawLine(std::string_view s) { out << s << "\n"; }

	// ----- Expression emitters ------------------------------------------
	std::string EmitExpr(const nlohmann::json& e) {
		std::string form = DetectExpressionForm(e);
		if (form == "var") {
			std::string name = SanitizeIdentifier(e["var"].get<std::string>());
			// Disambiguate member access shadowed by a parameter / local.
			// BP allows a function param to share its name with a member
			// (`SetPlayer(Player) { Player = Player; }` -- the LHS is the
			// member, the RHS is the param). Without `this->`, both sides
			// refer to the param and the assignment is a self-assign.
			std::string scope = e.value("scope", "");
			if (scope == "member" && paramAndLocalNames.count(name) > 0) {
				return "this->" + name;
			}
			return name;
		}
		if (form == "lit") {
			return EmitLit(e["lit"]);
		}
		if (form == "call") {
			return EmitCallExpr(e);
		}
		if (form == "cast") {
			// Cast<T> target. Strip /Script/<Mod>. or /Game/<Path>. asset
			// paths down to the bare class name; otherwise the C++ output
			// contains `Cast</Game/UI/X.WB_X_C>(...)` which doesn't
			// compile.
			ResolvedRef r = ResolveAssetPath(e.value("to", ""));
			std::string target = r.bareOwner;
			if (target.empty())
			{
				target = "UObject";
			}
			std::string inner = EmitExpr(e["cast"]);
			return fmt::format("Cast<{}>({})", target, inner);
		}
		if (form == "member") {
			return fmt::format("{}.{}", EmitExpr(e["member"]), e.value("name", ""));
		}
		if (form == "index") {
			return fmt::format("{}[{}]", EmitExpr(e["index"]), EmitExpr(e["idx"]));
		}
		if (form == "self") {
			return "this";
		}
		if (form == "new_array") {
			std::string s = "{";
			bool first = true;
			for (const auto& el : e["new_array"]) {
				if (!first)
				{
					s += ", ";
				}
				first = false;
				s += EmitExpr(el);
			}
			s += "}";
			return s;
		}
		if (form == "new_set") {
			// TSet<T> supports brace-init from an initializer_list in
			// UE 5.x. The element type comes from context (the
			// assignment LHS), so we emit a bare braced list and let
			// the C++ compiler / target type drive deduction.
			std::string s = "{";
			bool first = true;
			for (const auto& el : e["new_set"]) {
				if (!first)
				{
					s += ", ";
				}
				first = false;
				s += EmitExpr(el);
			}
			s += "}";
			return s;
		}
		if (form == "new_map") {
			// TMap<K,V> supports initializer_list of TPair<K,V> in
			// UE 5.x via `TMap<K,V>{{k,v}, {k,v}}`. Bare braces with
			// {key, value} pairs let the LHS type drive deduction.
			std::string s = "{";
			bool first = true;
			for (const auto& kv : e["new_map"]) {
				if (!first)
				{
					s += ", ";
				}
				first = false;
				s += fmt::format("{{ {}, {} }}",
					EmitExpr(kv["key"]), EmitExpr(kv["value"]));
			}
			s += "}";
			return s;
		}
		if (form == "new_struct") {
			std::string type = e.value("new_struct", "");
			std::string s = type + "{";
			if (e.contains("fields") && e["fields"].is_object()) {
				bool first = true;
				for (auto& [k, v] : e["fields"].items()) {
					if (!first)
					{
						s += ", ";
					}
					first = false;
					s += fmt::format("/*{}=*/{}", k, EmitExpr(v));
				}
			}
			s += "}";
			return s;
		}
		// Should never happen if validation passed.
		return "/*<unknown-expr>*/";
	}

	std::string EmitLit(const nlohmann::json& v) {
		if (v.is_null())
		{
			return "nullptr";
		}
		if (v.is_boolean())
		{
			return v.get<bool>() ? "true" : "false";
		}
		if (v.is_number_integer())
		{
			return std::to_string(v.get<long long>());
		}
		if (v.is_number())  return fmt::format("{}f", v.get<double>());  // float literal
		if (v.is_string()) {
			const auto& s = v.get_ref<const std::string&>();
			// If the string starts with /* it's a sentinel comment from
			// decompile (unsupported expression); pass through as-is.
			if (s.find("/*") == 0) return s;
			// Otherwise quote it as a C++ string literal. Escape embedded
			// quotes / backslashes / newlines / etc.; without this, a
			// BP string default like Hello "world" leaks as
			// TEXT("Hello "world"") which doesn't compile.
			return fmt::format("TEXT(\"{}\")", EscapeForCppStringLiteral(s));
		}
		return "/*<unknown-lit>*/";
	}

	std::string EmitCallExpr(const nlohmann::json& e) {
		std::string fnName = e.value("call", "");
		// Method-call alias: BP's UKismetArrayLibrary / StringLibrary
		// helpers take the container as a named arg ("TargetArray",
		// "SourceString", etc.); the C++ form is a method call
		// (`Array.Add(Item)`). Look up the receiver pin by name (NOT
		// by position — JSON object iteration is alphabetical, so
		// positional lookup would reshuffle the receiver off the front).
		if (auto methodIt = MethodCallAliases().find(fnName);
			methodIt != MethodCallAliases().end()) {
			const auto& alias = methodIt->second;
			std::string receiver;
			std::string rest;
			bool first = true;
			if (e.contains("args") && e["args"].is_object()) {
				for (auto& [k, v] : e["args"].items()) {
					if (k == alias.receiverArg) {
						receiver = EmitExpr(v);
						continue;
					}
					if (!first)
					{
						rest += ", ";
					}
					first = false;
					rest += EmitExpr(v);
				}
			}
			if (!receiver.empty()) {
				std::string call = fmt::format("{}.{}({})", receiver, alias.method, rest);
				if (fnName == "KismetArrayLibrary::Array_IsNotEmpty") {
					call = "!" + call;
				}
				return call;
			}
		}
		// Name aliases: BP routes IsValid / GetClass etc. through
		// KismetSystemLibrary, but the canonical C++ form is unqualified.
		// Empty alias → identity (drop the call, return the single arg
		// unchanged — for MakeLiteralXxx etc. that are no-ops in C++).
		if (auto it = NameAliases().find(fnName); it != NameAliases().end()) {
			if (it->second.empty()) {
				// Identity: return the first non-self arg unchanged.
				if (e.contains("args") && e["args"].is_object() && !e["args"].empty()) {
					auto firstIt = e["args"].begin();
					return EmitExpr(firstIt.value());
				}
				return "/* empty MakeLiteral */";
			}
			fnName = it->second;
		}
		// Operator alias?
		if (opts.useOperatorAliases) {
			auto it = OpReverseMap().find(fnName);
			if (it != OpReverseMap().end()) {
				const auto& op = it->second;
				std::vector<std::string> argStrs;
				if (e.contains("args") && e["args"].is_object()) {
					for (auto& [_, v] : e["args"].items()) {
						argStrs.push_back(EmitExpr(v));
					}
				}
				if (op.arity == 1 && argStrs.size() >= 1) {
					return fmt::format("({}{})", op.op, argStrs[0]);
				}
				if (op.arity == 2 && argStrs.size() >= 2) {
					return fmt::format("({} {} {})", argStrs[0], op.op, argStrs[1]);
				}
			}
		}
		// Sentinel-name lowering: Decompile emits structured calls for
		// a few K2 nodes that don't have a 1:1 BP function name. We
		// render them as the actual UE-side syntax here so the output
		// compiles without manual fixups.
		if (fnName == "__bpr_spawn_actor_from_class") {
			return EmitSpawnActorFromClass(e);
		}
		if (fnName == "__bpr_add_component") {
			return EmitAddComponent(e);
		}
		if (fnName == "__bpr_destroy_actor") {
			return EmitDestroyActor(e);
		}
		if (fnName == "__bpr_construct_object_from_class") {
			return EmitConstructObjectFromClass(e);
		}
		if (fnName == "__bpr_format_text") {
			return EmitFormatText(e);
		}
		if (fnName == "__bpr_get_class_defaults") {
			return EmitGetClassDefaults(e);
		}
		if (fnName == "__bpr_select_ternary") {
			// 2-way Select node -> C++ ternary.
			//   (<Index> ? <True> : <False>)
			// Decompile guarantees all three args present.
			auto a = (e.contains("args") && e["args"].is_object())
						 ? e["args"] : nlohmann::json::object();
			std::string idx   = a.contains("Index") ? EmitExpr(a["Index"]) : "false";
			std::string trueV = a.contains("True")  ? EmitExpr(a["True"])  : "0";
			std::string falseV= a.contains("False") ? EmitExpr(a["False"]) : "0";
			return fmt::format("({} ? {} : {})", idx, trueV, falseV);
		}
		if (fnName == "__bpr_select_n") {
			// N-way Select -> chained ternaries based on Index value.
			//   (Index == 0 ? Option_0 : (Index == 1 ? Option_1 : Option_N))
			// The last option becomes the default fallback (no explicit
			// index check) to minimize nesting depth.
			auto a = (e.contains("args") && e["args"].is_object())
						 ? e["args"] : nlohmann::json::object();
			std::string idx = a.contains("Index") ? EmitExpr(a["Index"]) : "0";
			// Collect option slots in numerical order.
			std::vector<std::pair<int, std::string>> selectOpts;
			for (auto& [k, v] : a.items()) {
				if (k == "Index")
				{
					continue;
				}
				// "Option_<n>"
				if (k.size() > 7 && k.compare(0, 7, "Option_") == 0) {
					try {
						selectOpts.push_back({std::stoi(k.substr(7)), EmitExpr(v)});
					} catch (...) { /* skip */ }
				}
			}
			std::sort(selectOpts.begin(), selectOpts.end(),
					  [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });
			if (selectOpts.empty())
			{
				return "0";
			}
			// Build right-associative chain. The trailing option is the
			// default fallback.
			std::string chain = selectOpts.back().second;
			for (std::size_t i = selectOpts.size() - 1; i-- > 0; ) {
				chain = fmt::format("({} == {} ? {} : {})",
					idx, selectOpts[i].first, selectOpts[i].second, chain);
			}
			return chain;
		}
		// GetDataTableRow is statement-form (carries success+fail blocks)
		// — the statement dispatcher handles it directly; we'd only see
		// it here if someone misuses the sentinel in an expression slot.
		if (fnName == "__bpr_get_data_table_row") {
			return "/* GetDataTableRow appears in statement position only */";
		}
		// World-context injection: BP hides the WorldContextObject pin
		// on most UKismetSystemLibrary / KismetMathLibrary / Gameplay-
		// statics helpers via `HidePin=WorldContextObject` +
		// `DefaultToSelf=WorldContextObject`. The function still takes
		// it as its first argument at the C++ level — we inject `this`
		// so the generated call compiles. List captures the
		// commonly-hit functions; agent can edit the call if needed.
		const auto& worldCtx = WorldContextFunctions();
		bool needsThis = worldCtx.count(fnName) > 0;
		// Also resolve unqualified-name calls — BPIR strips the qualifier
		// for trivial calls so we recheck the bare name.
		if (!needsThis) {
			auto colon = fnName.rfind("::");
			std::string bare = (colon == std::string::npos) ? fnName : fnName.substr(colon + 2);
			std::string keyBare = "::" + bare;
			for (const auto& k : worldCtx) {
				if (k.size() >= keyBare.size() &&
					k.compare(k.size() - keyBare.size(), keyBare.size(), keyBare) == 0) {
					needsThis = true;
					break;
				}
			}
		}

		// Resolve the qualified call name. Strips /Script/<Mod>. and
		// /Game/<Path>. asset paths to bare class names, and classifies
		// BP-class targets so we can route them through reflection. Also
		// sanitizes the member name (BP function names can contain
		// spaces if the BP author renamed them).
		ResolvedRef ref = ResolveAssetPath(fnName);
		std::string resolvedMember = SanitizeIdentifier(ref.memberName);
		std::string resolvedOwner  = ref.bareOwner;

		// BP-only target: no C++ symbol exists for `resolvedOwner::resolvedMember`.
		// Lower to a UFunction reflection call so the C++ output at least
		// compiles and dispatches at runtime. We don't know the target
		// object expression -- BP `self` is implicit for cross-BP calls
		// routed through a sub-widget member -- so the agent fills in the
		// receiver via a TODO marker. Args are still emitted as a struct
		// skeleton so the layout is visible.
		if (ref.kind == ResolvedRef::Kind::BpClass && !resolvedMember.empty()) {
			return EmitBpReflectionCall(e, resolvedOwner, resolvedMember);
		}

		// Rebuild a usable C++ qualifier from the resolved parts. If we
		// had an owner originally, keep the qualifier; otherwise emit the
		// bare member.
		std::string emittedName;
		if (!resolvedOwner.empty() && !resolvedMember.empty()) {
			emittedName = resolvedOwner + "::" + resolvedMember;
		} else if (!resolvedMember.empty()) {
			emittedName = resolvedMember;
		} else {
			// Unqualified call (no `::`). Sanitize the whole thing -- BP
			// function names with display-name spaces appear here.
			emittedName = SanitizeIdentifier(ref.bareOwner);
		}

		std::string args;
		bool first = true;
		if (needsThis) { args = "this"; first = false; }
		if (e.contains("args") && e["args"].is_object()) {
			for (auto& [_, v] : e["args"].items()) {
				if (!first)
				{
					args += ", ";
				}
				first = false;
				args += EmitExpr(v);
			}
		}
		return fmt::format("{}({})", emittedName, args);
	}

	// EmitBpReflectionCall: the target is a BP-only class (no C++ symbol).
	// Generate a UFunction lookup + ProcessEvent dispatch so the call
	// compiles and works at runtime against any object whose class
	// exposes the named UFUNCTION. Args are packed into a local struct
	// matching the BP function's parameter layout.
	//
	// The receiver is unknown to BPIR for a bare cross-BP call -- the BP
	// graph has it on the `self` input pin which BuildExpression already
	// captures, but the call statement we render here is the expression
	// form (no surrounding statement target). For safety we route through
	// `this` and emit a TODO so the agent can swap in the sub-widget
	// reference. Statement-form calls populate a `target` field that
	// EmitStatement passes through; expression form has to rely on this
	// TODO.
	//
	// No-arg case is the easy one and compiles directly. Arg-bearing
	// cases emit a struct with the call's argument values; arg types
	// can't be recovered from BPIR alone (the BP-only function has no
	// C++ signature), so the struct's member types come out as `auto`
	// and the user must retype them.
	std::string EmitBpReflectionCall(const nlohmann::json& e,
									 std::string_view ownerClass,
									 std::string_view memberName) {
		const bool hasArgs = e.contains("args") && e["args"].is_object() && !e["args"].empty();
		nlohmann::json note;
		note["node_class"]   = "K2Node_CallFunction";
		note["treatment"]    = "bp_reflection_call";
		note["target_class"] = std::string(ownerClass);
		note["target_func"]  = std::string(memberName);
		note["hint"]         = "Either also transpile the target BP class "
							   "(so the call binds to a C++ method) or "
							   "fill in the target receiver in the "
							   "generated ProcessEvent block.";
		notes.push_back(std::move(note));

		// We emit a parenthesized comma-expression-style lambda so the
		// call fits in an expression slot. Statement-form callers strip
		// the parens.
		if (!hasArgs) {
			return fmt::format(
				"([&]{{ if (UFunction* F__ = this->FindFunction(TEXT(\"{member}\"))) {{ "
				"this->ProcessEvent(F__, nullptr); }} /* TODO[bpr-bpcall]: receiver "
				"for {owner}::{member} -- swap `this` for the sub-widget reference. */ }})()",
				fmt::arg("member", memberName), fmt::arg("owner", ownerClass));
		}

		// Pack args into a local struct. Order matches BPIR `args`
		// iteration (alphabetical due to nlohmann::json object storage)
		// which is NOT the BP parameter order; the TODO tells the user
		// to reorder if the call signature is order-sensitive.
		std::string fields;
		std::string inits;
		bool first = true;
		for (auto& [k, v] : e["args"].items()) {
			std::string n = SanitizeIdentifier(k);
			if (n.empty())
			{
				n = "Arg";
			}
			fields += fmt::format("auto {}; ", n);
			if (!first)
			{
				inits += ", ";
			}
			first = false;
			inits += EmitExpr(v);
		}
		return fmt::format(
			"([&]{{ if (UFunction* F__ = this->FindFunction(TEXT(\"{member}\"))) {{ "
			"struct {{ {fields}}} P__ {{ {inits} }}; "
			"this->ProcessEvent(F__, &P__); }} /* TODO[bpr-bpcall]: receiver + "
			"arg types/order for {owner}::{member} */ }})()",
			fmt::arg("member", memberName),
			fmt::arg("fields", fields),
			fmt::arg("inits",  inits),
			fmt::arg("owner",  ownerClass));
	}

	// SpawnActorFromClass → `GetWorld()->SpawnActor<AActor>(Class,
	// SpawnTransform, SpawnParameters)`. We pull whichever input pins
	// are present from `args`; missing pins fall back to UE defaults.
	// Class is the only required arg — without it the call is
	// ill-formed and we surface that as a TODO comment so the user can
	// fix.
	std::string EmitSpawnActorFromClass(const nlohmann::json& e) {
		auto a = (e.contains("args") && e["args"].is_object())
					 ? e["args"] : nlohmann::json::object();
		std::string clsExpr  = a.contains("Class")
								   ? EmitExpr(a["Class"]) : std::string("nullptr");
		std::string xform    = a.contains("SpawnTransform")
								   ? EmitExpr(a["SpawnTransform"])
								   : std::string("FTransform::Identity");
		// Optional: Owner / Instigator wrapped in SpawnParameters.
		std::string owner    = a.contains("Owner")      ? EmitExpr(a["Owner"])      : "";
		std::string instig   = a.contains("Instigator") ? EmitExpr(a["Instigator"]) : "";
		std::string collison = a.contains("CollisionHandlingOverride")
								   ? EmitExpr(a["CollisionHandlingOverride"]) : "";
		if (owner.empty() && instig.empty() && collison.empty()) {
			return fmt::format(
				"GetWorld()->SpawnActor<AActor>({}, {})", clsExpr, xform);
		}
		// With SpawnParameters: emit a ([&] { ... }()) immediately-invoked
		// lambda so the call slots into a single expression position.
		std::string params;
		if (!owner.empty())    params += fmt::format("    p.Owner = {};\n", owner);
		if (!instig.empty())   params += fmt::format("    p.Instigator = {};\n", instig);
		if (!collison.empty()) params += fmt::format(
			"    p.SpawnCollisionHandlingOverride = {};\n", collison);
		return fmt::format(
			"[&]{{ FActorSpawnParameters p;\n{}    return GetWorld()->SpawnActor<AActor>({}, {}, p); }}()",
			params, clsExpr, xform);
	}

	// GetDataTableRow → `if (auto* Row = DataTable->FindRow<FRowType>(
	// RowName, "BPR")) { /* success */ } else { /* fail */ }`. The row
	// struct type comes from the BPIR `row_struct` field when the BP
	// node carries it; otherwise we default to `FTableRowBase` and let
	// the agent retype.
	void EmitGetDataTableRow(const nlohmann::json& s) {
		auto a = (s.contains("args") && s["args"].is_object())
					 ? s["args"] : nlohmann::json::object();
		std::string dt   = a.contains("DataTable") ? EmitExpr(a["DataTable"]) : "DataTable";
		std::string row  = a.contains("RowName")   ? EmitExpr(a["RowName"])   : R"(TEXT(""))";
		std::string rowStruct = s.value("row_struct", std::string{});
		if (rowStruct.empty())
		{
			rowStruct = "FTableRowBase";
		}
		// Strip path prefix on row struct (BP encodes as `/Script/X.FFoo`).
		if (auto dot = rowStruct.find_last_of('.'); dot != std::string::npos) {
			rowStruct = rowStruct.substr(dot + 1);
		}
		// Ensure F prefix on the struct name.
		if (!rowStruct.empty() && rowStruct[0] != 'F')
		{
			rowStruct = "F" + rowStruct;
		}

		Line(fmt::format(
			"if (auto* Row = {}->FindRow<{}>({}, TEXT(\"BPR\")))", dt, rowStruct, row));
		Line("{");
		++indentLevel;
		if (s.contains("success"))
		{
			EmitStatementList(s["success"]);
		}
		--indentLevel;
		Line("}");
		if (s.contains("fail") && !s["fail"].empty()) {
			Line("else");
			Line("{");
			++indentLevel;
			EmitStatementList(s["fail"]);
			--indentLevel;
			Line("}");
		}
	}

	// ConstructObjectFromClass → `NewObject<UObject>(Outer, Class)`. The
	// BP node's Class pin carries the class to instantiate (typed
	// expression — we render it as-is and trust the caller's type).
	// Without an Outer pin, we default to `this` which is the most
	// common case (actor-scoped BPs).
	std::string EmitConstructObjectFromClass(const nlohmann::json& e) {
		auto a = (e.contains("args") && e["args"].is_object())
					 ? e["args"] : nlohmann::json::object();
		std::string outer = a.contains("Outer") ? EmitExpr(a["Outer"]) : std::string("this");
		std::string cls   = a.contains("Class") ? EmitExpr(a["Class"]) : std::string("UObject::StaticClass()");
		// We don't have a strong type signal at the call site — keep the
		// template arg as UObject and let the assignment context narrow
		// via the LHS UPROPERTY type. Cast<> on the result is the agent's
		// job (we can't infer the target type from BPIR alone).
		return fmt::format("NewObject<UObject>({}, {})", outer, cls);
	}

	// GetClassDefaults → `Class->GetDefaultObject<UObject>()->Field`.
	// The class expression comes from the Class arg; the Field arg
	// carries the BP output-pin name (which is the CDO's property
	// name). When the class is statically known (a literal class ref),
	// `GetDefault<T>()->Field` is shorter — but we don't have strong
	// type info, so we emit the runtime form.
	std::string EmitGetClassDefaults(const nlohmann::json& e) {
		auto a = (e.contains("args") && e["args"].is_object())
					 ? e["args"] : nlohmann::json::object();
		std::string cls = a.contains("Class") ? EmitExpr(a["Class"]) : "nullptr";
		std::string field = "Field";
		if (a.contains("Field") && a["Field"].is_object() &&
			a["Field"].contains("lit") && a["Field"]["lit"].is_string()) {
			field = a["Field"]["lit"].get<std::string>();
		}
		return fmt::format("{}->GetDefaultObject<UObject>()->{}", cls, field);
	}

	// FormatText → `FText::Format(NSLOCTEXT("BPR", "...", "..."),
	// FFormatNamedArguments{...})`. The C++ canonical form uses
	// FFormatNamedArguments populated from the named pins; we emit an
	// immediately-invoked lambda so the call slots into an expression
	// position even though Args needs statements to populate.
	std::string EmitFormatText(const nlohmann::json& e) {
		std::string fmtStr = e.value("format", std::string{});
		// Escape any embedded " for safe TEXT() embedding.
		std::string escaped;
		escaped.reserve(fmtStr.size());
		for (char c : fmtStr) {
			if (c == '"' || c == '\\')
			{
				escaped.push_back('\\');
			}
			escaped.push_back(c);
		}
		std::string body;
		if (e.contains("args") && e["args"].is_object()) {
			for (auto& [k, v] : e["args"].items()) {
				// BP arg names are bare identifiers; we wrap with TEXT()
				// for the key and emit the value expression.
				body += fmt::format(
					"    Args.Add(TEXT(\"{}\"), {});\n", k, EmitExpr(v));
			}
		}
		// No args → drop the formatting machinery, just emit the literal.
		if (body.empty()) {
			return fmt::format(
				"FText::FromString(TEXT(\"{}\"))", escaped);
		}
		return fmt::format(
			"[&]{{ FFormatNamedArguments Args;\n{}    return FText::Format("
			"NSLOCTEXT(\"BPR\", \"FormatText\", \"{}\"), Args); }}()",
			body, escaped);
	}

	// DestroyActor → `Target->Destroy()` (or `this->Destroy()` when
	// Target is absent). The BP node's exec is statement-context, so
	// CppEmit invokes this from a statement-form CallExpr; we render the
	// expression and the caller drops a trailing semicolon.
	std::string EmitDestroyActor(const nlohmann::json& e) {
		auto a = (e.contains("args") && e["args"].is_object())
					 ? e["args"] : nlohmann::json::object();
		if (a.contains("Target")) {
			std::string tgt = EmitExpr(a["Target"]);
			// Self-target collapses to `this->Destroy()`; literal "this"
			// shows up if BuildExpression emitted a `{self:nullptr}` shape
			// upstream. Either way the rendering is the same.
			if (tgt == "this")
			{
				return "this->Destroy()";
			}
			return fmt::format("{}->Destroy()", tgt);
		}
		return "this->Destroy()";
	}

	// AddComponent → `NewObject<UActorComponent>(this) + RegisterComponent`.
	// Decompile carries TemplateName / RelativeTransform on the args;
	// we render a small block that creates + attaches + registers.
	// Returns the component pointer so the call slots into an
	// expression position.
	std::string EmitAddComponent(const nlohmann::json& e) {
		auto a = (e.contains("args") && e["args"].is_object())
					 ? e["args"] : nlohmann::json::object();
		std::string xform = a.contains("RelativeTransform")
								? EmitExpr(a["RelativeTransform"])
								: std::string("FTransform::Identity");
		std::string templateName = a.contains("TemplateName")
									   ? EmitExpr(a["TemplateName"])
									   : std::string("NAME_None");
		// The template type isn't carried as a pin (it's stored on the
		// node as a class reference). Leave as UActorComponent — the
		// agent retypes when copying into real source.
		return fmt::format(
			"[&]{{ auto* Comp = NewObject<UActorComponent>(this, {});\n"
			"    if (auto* Scene = Cast<USceneComponent>(Comp)) {{\n"
			"        Scene->SetRelativeTransform({});\n"
			"    }}\n"
			"    Comp->RegisterComponent();\n"
			"    return Comp; }}()", templateName, xform);
	}

	// ----- Statement emitters -------------------------------------------
	void EmitStatementList(const nlohmann::json& stmts) {
		for (const auto& s : stmts)
		{
			EmitStatement(s);
		}
	}

	void EmitStatement(const nlohmann::json& s) {
		std::string form = DetectStatementForm(s);
		if (form == "comment") {
			Line(fmt::format("// {}", s.value("comment", "")));
			return;
		}
		if (form == "if") {
			std::string cond = EmitExpr(s["if"]);
			Line(fmt::format("if ({}) {{", cond));
			++indentLevel;
			if (s.contains("then"))
			{
				EmitStatementList(s["then"]);
			}
			--indentLevel;
			if (s.contains("else") && !s["else"].empty()) {
				Line("} else {");
				++indentLevel;
				EmitStatementList(s["else"]);
				--indentLevel;
			}
			Line("}");
			return;
		}
		if (form == "set") {
			std::string varName = SanitizeIdentifier(s.value("set", ""));
			std::string val = EmitExpr(s["to"]);
			// Same shadowing rule as the `var` expression branch:
			// member-scoped writes must be prefixed `this->` when the
			// LHS name collides with a parameter / local.
			std::string scope = s.value("scope", "");
			std::string lhs = varName;
			if (scope == "member" && paramAndLocalNames.count(varName) > 0) {
				lhs = "this->" + varName;
			}
			Line(fmt::format("{} = {};", lhs, val));
			return;
		}
		if (form == "call") {
			// Special statement-form sentinel: GetDataTableRow carries
			// success/fail blocks like a cast — emit the FindRow lookup
			// and dispatch into the success block when the row is found.
			std::string fnName = s.value("call", "");
			if (fnName == "__bpr_get_data_table_row") {
				EmitGetDataTableRow(s);
				return;
			}
			// EnhancedInput binding sentinel — emitted by
			// ProcessEnhancedInputBindings inside a Cast<UEnhancedInputComponent>
			// success block, so the local `EIC` variable is in scope.
			// Always statement-form; takes Action (var expression),
			// Trigger (string lit), Callback (string lit). Renders as:
			//   EIC->BindAction(<Action>, ETriggerEvent::<Trigger>, this, &ThisClass::<Cb>);
			if (fnName == "__bpr_bind_input_action") {
				auto a = (s.contains("args") && s["args"].is_object())
							 ? s["args"] : nlohmann::json::object();
				std::string actionExpr = a.contains("Action")
					? EmitExpr(a["Action"]) : "/* missing */";
				auto unwrapLit = [](const nlohmann::json& v) -> std::string {
					if (v.is_object() && v.contains("lit") && v["lit"].is_string()) {
						return v["lit"].get<std::string>();
					}
					return {};
				};
				std::string trigger  = SanitizeIdentifier(unwrapLit(a.value("Trigger",  nlohmann::json{})));
				std::string callback = SanitizeIdentifier(unwrapLit(a.value("Callback", nlohmann::json{})));
				Line(fmt::format(
					"EIC->BindAction({}, ETriggerEvent::{}, this, &ThisClass::{});",
					actionExpr, trigger, callback));
				return;
			}
			// Async-task lowering sentinels — Decompile.cpp's
			// async-task handler emits a triplet:
			//   __bpr_async_factory  → factory call assigning to a local
			//   __bpr_async_bind     → AddDynamic for one wired delegate
			//   __bpr_async_activate → Action->Activate()
			// All three use {lit: "<ident>"} for action/factory/callback
			// names so the validator's expression-form check passes; we
			// unwrap them as bare identifiers here.
			auto unwrapLit = [](const nlohmann::json& v) -> std::string {
				if (v.is_object() && v.contains("lit") && v["lit"].is_string()) {
					return v["lit"].get<std::string>();
				}
				return {};
			};
			if (fnName == "__bpr_async_factory") {
				auto a = (s.contains("args") && s["args"].is_object())
							 ? s["args"] : nlohmann::json::object();
				const std::string actionLocal  = SanitizeIdentifier(unwrapLit(a.value("Action",       nlohmann::json{})));
				const std::string factoryClass = unwrapLit(a.value("FactoryClass", nlohmann::json{}));
				const std::string factoryFn    = SanitizeIdentifier(unwrapLit(a.value("Factory",      nlohmann::json{})));
				// Emit each FactoryArg expression positionally —
				// args are JSON-object so iteration order is alphabetical.
				std::string argList;
				if (auto fIt = a.find("FactoryArgs"); fIt != a.end() && fIt->is_object()) {
					bool first = true;
					for (auto& [k, v] : fIt->items()) {
						if (!first) {
							argList += ", ";
						}
						first = false;
						argList += EmitExpr(v);
					}
				}
				Line(fmt::format("auto* {} = {}::{}({});",
								 actionLocal, factoryClass, factoryFn, argList));
				return;
			}
			if (fnName == "__bpr_async_bind") {
				auto a = (s.contains("args") && s["args"].is_object())
							 ? s["args"] : nlohmann::json::object();
				const std::string actionLocal = SanitizeIdentifier(unwrapLit(a.value("Action",   nlohmann::json{})));
				const std::string delegate    = SanitizeIdentifier(unwrapLit(a.value("Delegate", nlohmann::json{})));
				const std::string callback    = SanitizeIdentifier(unwrapLit(a.value("Callback", nlohmann::json{})));
				Line(fmt::format("{}->{}.AddDynamic(this, &ThisClass::{});",
								 actionLocal, delegate, callback));
				return;
			}
			if (fnName == "__bpr_async_activate") {
				auto a = (s.contains("args") && s["args"].is_object())
							 ? s["args"] : nlohmann::json::object();
				const std::string actionLocal = SanitizeIdentifier(unwrapLit(a.value("Action", nlohmann::json{})));
				Line(fmt::format("{}->Activate();", actionLocal));
				return;
			}
			// Latent-action lowering sentinel — Decompile.cpp's Delay
			// handler emits this to wire up the timer + generated
			// continuation method. Args use string literals for
			// Handle/Callback names which we unwrap here as bare
			// identifiers (the validator only accepts string-typed
			// expressions, so this round-trip is intentional).
			if (fnName == "__bpr_set_timer") {
				auto a = (s.contains("args") && s["args"].is_object())
							 ? s["args"] : nlohmann::json::object();
				std::string duration = a.contains("Duration")
					? EmitExpr(a["Duration"]) : "0.0f";
				// Handle / Callback come over as {lit: "<ident>"}.
				auto unwrap = [](const nlohmann::json& v) -> std::string {
					if (v.is_object() && v.contains("lit") && v["lit"].is_string()) {
						return v["lit"].get<std::string>();
					}
					return {};
				};
				std::string handle   = SanitizeIdentifier(unwrap(a.value("Handle",   nlohmann::json{})));
				std::string callback = SanitizeIdentifier(unwrap(a.value("Callback", nlohmann::json{})));
				bool looping = false;
				if (auto lit = a.find("Looping"); lit != a.end()
					&& lit->is_object() && lit->contains("lit")
					&& (*lit)["lit"].is_boolean()) {
					looping = (*lit)["lit"].get<bool>();
				}
				// Idiomatic UE5 pattern — bare member-pointer overload
				// is fast and avoids the FTimerDelegate intermediate.
				Line(fmt::format(
					"GetWorld()->GetTimerManager().SetTimer({}, this, &ThisClass::{}, {}, {});",
					handle, callback, duration, looping ? "true" : "false"));
				return;
			}
			// Statement form — discard return value.
			std::string call = EmitCallExpr(s);
			// Strip the surrounding parens if it's an operator (rare in
			// statement context but possible).
			if (!call.empty() && call.front() == '(' && call.back() == ')') {
				call = call.substr(1, call.size() - 2);
			}
			Line(fmt::format("{};", call));
			return;
		}
		if (form == "return") {
			const auto& r = s["return"];
			if (r.is_null())          { Line("return;"); }
			else if (r.is_array()) {
				if (r.empty()) {
					Line("return;");
				} else if (r.size() == 1 && outputNames.empty()) {
					// Singleton array from decompile but no out-params --
					// treat as a by-value return.
					Line(fmt::format("return {};", EmitExpr(r[0])));
				} else {
					// Multi-output: assign each return expression to its
					// matching out-param (declared as `Type& <Name>` in
					// the signature), then bare `return;`. UE's allowed
					// stdlib subset doesn't include std::make_tuple, and
					// UE C++ convention for multi-output is by-reference
					// out-params anyway.
					for (std::size_t i = 0; i < r.size(); ++i) {
						std::string lhs;
						if (i < outputNames.size() && !outputNames[i].empty()) {
							lhs = outputNames[i];
						} else {
							lhs = fmt::format("Out{}", i);
						}
						Line(fmt::format("{} = {};", lhs, EmitExpr(r[i])));
					}
					Line("return;");
				}
			} else {
				Line(fmt::format("return {};", EmitExpr(r)));
			}
			return;
		}
		if (form == "cast") {
			std::string castVal = EmitExpr(s["cast"]);
			ResolvedRef r = ResolveAssetPath(s.value("to", ""));
			std::string target  = r.bareOwner.empty() ? std::string("UObject") : r.bareOwner;
			std::string asName  = SanitizeIdentifier(s.value("as", "AsCast"));
			if (asName.empty())
			{
				asName = "AsCast";
			}
			Line(fmt::format("if (auto* {} = Cast<{}>({})) {{", asName, target, castVal));
			++indentLevel;
			EmitStatementList(s["success"]);
			--indentLevel;
			if (s.contains("fail") && !s["fail"].empty()) {
				Line("} else {");
				++indentLevel;
				EmitStatementList(s["fail"]);
				--indentLevel;
			}
			Line("}");
			return;
		}
		if (form == "switch") {
			Line(fmt::format("switch ({}) {{", EmitExpr(s["switch"])));
			++indentLevel;
			if (s.contains("cases") && s["cases"].is_object()) {
				for (auto& [val, body] : s["cases"].items()) {
					Line(fmt::format("case {}: {{", val));
					++indentLevel;
					EmitStatementList(body);
					Line("break;");
					--indentLevel;
					Line("}");
				}
			}
			if (s.contains("default")) {
				Line("default: {");
				++indentLevel;
				EmitStatementList(s["default"]);
				Line("break;");
				--indentLevel;
				Line("}");
			}
			--indentLevel;
			Line("}");
			return;
		}
		if (form == "for_each") {
			std::string elem = SanitizeIdentifier(s.value("for_each", ""));
			if (elem.empty())
			{
				elem = "Element";
			}
			std::string in   = EmitExpr(s["in"]);
			Line(fmt::format("for (auto& {} : {}) {{", elem, in));
			++indentLevel;
			EmitStatementList(s["body"]);
			--indentLevel;
			Line("}");
			return;
		}
		if (form == "while") {
			Line(fmt::format("while ({}) {{", EmitExpr(s["while"])));
			++indentLevel;
			EmitStatementList(s["body"]);
			--indentLevel;
			Line("}");
			return;
		}
		if (form == "sequence") {
			// K2Node_ExecutionSequence is structurally equivalent to
			// ordered statements -- inline them. No "// sequence branch N"
			// markers, no empty-branch comments; both were noise that
			// grouped statements visually without adding semantic info.
			for (const auto& branch : s["sequence"]) {
				if (branch.is_array() && branch.empty())
				{
					continue;
				}
				EmitStatementList(branch);
			}
			return;
		}
		if (form == "break")    { Line("break;");    return; }
		if (form == "continue") { Line("continue;"); return; }
		// Multicast-delegate ops. Receiver is `<target>->` if target is
		// explicit, otherwise the delegate property is on `this` and we
		// emit a bare member access. SanitizeIdentifier wraps every name
		// emission to defend against BP display-name pollution.
		if (form == "broadcast" || form == "bind_delegate" ||
			form == "unbind_delegate" || form == "clear_delegate") {
			std::string prop = SanitizeIdentifier(s.value(form, ""));
			std::string receiver;
			if (s.contains("target")) {
				receiver = fmt::format("{}->", EmitExpr(s["target"]));
			}
			if (form == "broadcast") {
				std::string parts;
				if (s.contains("args") && s["args"].is_object()) {
					bool first = true;
					for (auto& [k, v] : s["args"].items()) {
						if (!first)
						{
							parts += ", ";
						}
						first = false;
						parts += EmitExpr(v);
					}
				}
				Line(fmt::format("{}{}.Broadcast({});", receiver, prop, parts));
			} else if (form == "clear_delegate") {
				Line(fmt::format("{}{}.Clear();", receiver, prop));
			} else {
				std::string handler = SanitizeIdentifier(s.value("handler", ""));
				const char* verb =
					(form == "bind_delegate") ? "AddDynamic" : "RemoveDynamic";
				if (handler.empty()) {
					// No handler resolved -- emit a TODO + the verb so the
					// shape is visible. (Without plugin-side meta this can
					// happen when CreateDelegate's outgoing pin can't be
					// traced back to a function name.)
					Line(fmt::format(
						"// TODO[bpr-delegate]: {}{}.{}(this, &ThisClass::<HandlerName>); "
						"(handler not resolved)",
						receiver, prop, verb));
				} else {
					Line(fmt::format(
						"{}{}.{}(this, &ThisClass::{});",
						receiver, prop, verb, handler));
				}
			}
			return;
		}
		if (form == "unsupported") {
			const auto& u = s["unsupported"];
			std::string nodeClass = u.value("node_class", "?");
			std::string guid      = u.value("guid", "");
			// Classify the node and emit a richer treatment — best-effort
			// C++ stub when we know the pattern, else a TODO. Either way,
			// the sidecar entry captures the note so the agent can iterate
			// over manual steps.
			auto cls = ClassifyUnsupported(u);
			if (cls.kind == UnsupportedClassification::Kind::Approximation &&
				!cls.snippet.empty()) {
				// Snippet may span multiple lines — emit each at the
				// current indent.
				std::size_t pos = 0;
				while (pos < cls.snippet.size()) {
					auto nl = cls.snippet.find('\n', pos);
					std::string lineText = cls.snippet.substr(
						pos, nl == std::string::npos ? std::string::npos : nl - pos);
					if (!lineText.empty())
					{
						Line(lineText);
					}
					if (nl == std::string::npos)
					{
						break;
					}
					pos = nl + 1;
				}
			} else {
				Line(fmt::format(
					"// TODO[bpr-unsupported]: {} (guid={}) — manual port required.",
					nodeClass, guid));
				if (!cls.note.empty()) {
					Line(fmt::format("// Manual: {}", cls.note));
				}
			}
			// Sidecar entry — keep the original BPIR fields plus the
			// treatment classification.
			nlohmann::json note = u;
			note["treatment"] =
				(cls.kind == UnsupportedClassification::Kind::Approximation)
					? "approximation" : "todo_comment";
			if (!cls.note.empty())
			{
				note["manual_note"] = cls.note;
			}
			notes.push_back(std::move(note));
			return;
		}
		Line(fmt::format("// <unknown-statement-form: {}>", form));
	}
};

}    // namespace cpp_emit_detail
using namespace cpp_emit_detail;

std::string SanitizeIdentifier(std::string_view in) {
	// Public-API wrapper around the anon-namespace helper so CppClassEmit
	// (a separate TU) can sanitize at its UPROPERTY / UFUNCTION emission
	// sites with the same algorithm the function-body emitter uses.
	// Disambiguates from the anon-namespace `SanitizeIdentifier` (same
	// name, internal linkage) by qualifying the call to the helper.
	std::string out;
	out.reserve(in.size());
	for (char c : in) {
		const unsigned char uc = static_cast<unsigned char>(c);
		if (std::isalnum(uc) || c == '_') {
			out.push_back(c);
		}
	}
	if (!out.empty() && std::isdigit(static_cast<unsigned char>(out.front()))) {
		out.insert(out.begin(), '_');
	}
	return out;
}

std::string MapBpirTypeToCpp(std::string_view bpirType) {
	return MapTypeRecursive(bpirType, /*forMember=*/false);
}

std::string MapBpirTypeToCppMember(std::string_view bpirType) {
	return MapTypeRecursive(bpirType, /*forMember=*/true);
}

std::string MapBpirTypeToCppArg(std::string_view bpirType) {
	// Wrap heavy types with `const X&`. Categories that should be
	// by-value: bool, byte, int, int64, float, real, double, exec
	// (void), and object/class refs (already pointers / small handles).
	// Everything else (string, name, text, structs, containers) is
	// const-ref.
	std::string base = MapTypeRecursive(bpirType, /*forMember=*/false);
	auto isLightweight = [](std::string_view t) {
		// Strip optional container/wrapper.
		auto head = t;
		if (auto pos = head.find_last_of('>'); pos != std::string_view::npos) {
			// Compound type — never lightweight.
			return false;
		}
		// FName fits in a register — pass by value by convention.
		if (head == "FName")
		{
			return true;
		}
		// Primitives.
		if (head == "bool" || head == "uint8" || head == "int32" ||
			head == "int64" || head == "uint16" || head == "uint32" ||
			head == "uint64" || head == "int16" || head == "int8" ||
			head == "float" || head == "double" || head == "void") {
			return true;
		}
		// Pointer types end in `*`.
		if (!head.empty() && head.back() == '*')
		{
			return true;
		}
		return false;
	};
	if (isLightweight(base))
	{
		return base;
	}
	return std::string("const ") + base + "&";
}

CppEmitResult EmitCppFunctionBody(const nlohmann::json& doc, CppEmitOptions opts) {
	ValidateBpir(doc);
	if (!IsBpirFunction(doc)) {
		throw std::invalid_argument(
			"EmitCppFunctionBody requires a BPIR function doc (kind=\"function\")");
	}
	Emitter em;
	em.opts = opts;
	em.indentLevel = 1;  // body is one level deep inside the function block
	// Stash output names when the function has 2+ outputs (each becomes
	// a reference-out param). EmitStatement's return-form uses these to
	// emit `<Name> = <expr>;` per output instead of std::make_tuple.
	// Single-output functions return by value -- skip.
	if (doc.contains("outputs") && doc["outputs"].is_array() &&
		doc["outputs"].size() > 1) {
		for (const auto& o : doc["outputs"]) {
			em.outputNames.push_back(SanitizeIdentifier(o.value("name", "")));
		}
	}
	// Collect param + local names for the `this->` shadowing check in
	// `var` / `set` emission. Output names matter too -- they're ref-out
	// params on the C++ side and share the namespace with inputs.
	auto addNames = [&](const char* field) {
		if (!doc.contains(field) || !doc[field].is_array())
		{
			return;
		}
		for (const auto& v : doc[field]) {
			std::string n = SanitizeIdentifier(v.value("name", ""));
			if (!n.empty())
			{
				em.paramAndLocalNames.insert(std::move(n));
			}
		}
	};
	addNames("inputs");
	addNames("outputs");
	addNames("locals");
	if (doc.contains("body"))
	{
		em.EmitStatementList(doc["body"]);
	}
	return CppEmitResult{em.out.str(), std::move(em.notes)};
}

CppEmitResult EmitCppFunction(const nlohmann::json& doc, CppEmitOptions opts) {
	ValidateBpir(doc);
	if (!IsBpirFunction(doc)) {
		throw std::invalid_argument(
			"EmitCppFunction requires a BPIR function doc (kind=\"function\")");
	}
	// Build signature: <returnType> <name>(<args>).
	std::string returnType = "void";
	if (doc.contains("metadata") && doc["metadata"].is_object()) {
		returnType = doc["metadata"].value("return_type", returnType);
	}
	// If outputs[] has exactly one entry, prefer that as the return type.
	if (doc.contains("outputs") && doc["outputs"].is_array() &&
		doc["outputs"].size() == 1) {
		returnType = MapBpirTypeToCpp(doc["outputs"][0].value("type", "void"));
	}

	std::string args;
	if (doc.contains("inputs")) {
		bool first = true;
		for (const auto& in : doc["inputs"]) {
			if (!first)
			{
				args += ", ";
			}
			first = false;
			args += MapBpirTypeToCppArg(in.value("type", "void"));
			args += " ";
			std::string nm = SanitizeIdentifier(in.value("name", ""));
			args += nm.empty() ? std::string("Arg") : nm;
		}
	}
	// Multi-return → reference-out parameters.
	if (doc.contains("outputs") && doc["outputs"].is_array() &&
		doc["outputs"].size() > 1) {
		for (const auto& out : doc["outputs"]) {
			if (!args.empty())
			{
				args += ", ";
			}
			args += MapBpirTypeToCppArg(out.value("type", "void"));
			args += "& ";
			std::string nm = SanitizeIdentifier(out.value("name", ""));
			args += nm.empty() ? std::string("Out") : nm;
		}
	}

	auto bodyResult = EmitCppFunctionBody(doc, opts);

	std::string fnName = SanitizeIdentifier(doc.value("name", ""));
	if (fnName.empty())
	{
		fnName = "Fn";
	}
	std::ostringstream s;
	s << returnType << " " << fnName << "(" << args << ") {\n";
	s << bodyResult.source;
	s << "}\n";
	return CppEmitResult{s.str(), std::move(bodyResult.notes)};
}

} // namespace bpr::tools
