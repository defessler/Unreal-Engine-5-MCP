// Parser tests for the C++ → BPIR translator. Each case parses a
// snippet (full function or bare body) and asserts on the produced
// BPIR's shape. The validator runs at the end of every parse, so these
// tests double as schema-conformance checks.
//
// Round-trip identity (BPIR → C++ → BPIR) is covered separately in
// test_transpile_roundtrip.cpp.

#include <doctest/doctest.h>

#include "tools/Bpir.h"
#include "tools/parse/CppParse.h"

using namespace bpr::tools;
using nlohmann::json;

namespace test_cpp_parse_detail {

// Bare-body parser shorthand: parse a snippet with no surrounding
// signature so the body shape is what we assert against.
json ParseBody(std::string_view src) {
	return ParseCppFunction(src);
}

}    // namespace test_cpp_parse_detail
using namespace test_cpp_parse_detail;

// ===== Statement forms ====================================================

TEST_CASE("Parse: simple assignment becomes set form") {
	auto doc = ParseBody("Health = 100;");
	REQUIRE(doc["body"].size() == 1);
	const auto& stmt = doc["body"][0];
	CHECK(stmt["set"] == "Health");
	CHECK(stmt["to"]["lit"] == 100);
}

TEST_CASE("Parse: compound assignment expands to set + binop") {
	auto doc = ParseBody("Health -= Damage;");
	const auto& stmt = doc["body"][0];
	CHECK(stmt["set"] == "Health");
	// RHS is `Health - Damage` as a binop call.
	CHECK(stmt["to"]["call"] == "KismetMathLibrary::Subtract_IntInt");
	CHECK(stmt["to"]["args"]["A"]["var"] == "Health");
	CHECK(stmt["to"]["args"]["B"]["var"] == "Damage");
}

TEST_CASE("Parse: bare call becomes call statement") {
	auto doc = ParseBody("DoStuff();");
	const auto& stmt = doc["body"][0];
	CHECK(stmt["call"] == "DoStuff");
	CHECK(stmt["args"].is_object());
	CHECK(stmt["args"].empty());
}

TEST_CASE("Parse: call with positional args keyed alphabetically") {
	auto doc = ParseBody("Foo(X, Y);");
	const auto& stmt = doc["body"][0];
	CHECK(stmt["call"] == "Foo");
	CHECK(stmt["args"]["A"]["var"] == "X");
	CHECK(stmt["args"]["B"]["var"] == "Y");
}

TEST_CASE("Parse: if / then") {
	auto doc = ParseBody("if (bAlive) { Foo(); }");
	const auto& stmt = doc["body"][0];
	CHECK(stmt["if"]["var"] == "bAlive");
	REQUIRE(stmt["then"].size() == 1);
	CHECK(stmt["then"][0]["call"] == "Foo");
}

TEST_CASE("Parse: if / then / else") {
	auto doc = ParseBody("if (bAlive) { Foo(); } else { Bar(); }");
	const auto& stmt = doc["body"][0];
	CHECK(stmt["if"]["var"] == "bAlive");
	CHECK(stmt["then"][0]["call"] == "Foo");
	CHECK(stmt["else"][0]["call"] == "Bar");
}

TEST_CASE("Parse: cast statement (auto* X = Cast<T>(expr))") {
	auto doc = ParseBody(
		"if (auto* AsPawn = Cast<APawn>(Other)) { Hit(); } else { Miss(); }");
	const auto& stmt = doc["body"][0];
	CHECK(stmt["cast"]["var"] == "Other");
	CHECK(stmt["to"] == "APawn");
	CHECK(stmt["as"] == "AsPawn");
	CHECK(stmt["success"][0]["call"] == "Hit");
	CHECK(stmt["fail"][0]["call"] == "Miss");
}

TEST_CASE("Parse: range-based for") {
	auto doc = ParseBody("for (auto& Item : Items) { UseItem(); }");
	const auto& stmt = doc["body"][0];
	CHECK(stmt["for_each"] == "Item");
	CHECK(stmt["in"]["var"] == "Items");
	CHECK(stmt["body"][0]["call"] == "UseItem");
}

TEST_CASE("Parse: while loop") {
	auto doc = ParseBody("while (bRunning) { Tick(); }");
	const auto& stmt = doc["body"][0];
	CHECK(stmt["while"]["var"] == "bRunning");
	CHECK(stmt["body"][0]["call"] == "Tick");
}

TEST_CASE("Parse: switch with cases + default") {
	auto doc = ParseBody(
		"switch (Mode) {"
		"  case 0: ModeA(); break;"
		"  case 1: ModeB(); break;"
		"  default: ModeOther(); break;"
		"}");
	const auto& stmt = doc["body"][0];
	CHECK(stmt["switch"]["var"] == "Mode");
	CHECK(stmt["cases"]["0"][0]["call"] == "ModeA");
	CHECK(stmt["cases"]["1"][0]["call"] == "ModeB");
	CHECK(stmt["default"][0]["call"] == "ModeOther");
}

TEST_CASE("Parse: return with value") {
	auto doc = ParseBody("return Health;");
	const auto& stmt = doc["body"][0];
	CHECK(stmt["return"]["var"] == "Health");
}

TEST_CASE("Parse: bare return") {
	auto doc = ParseBody("return;");
	CHECK(doc["body"][0]["return"].is_null());
}

TEST_CASE("Parse: break + continue") {
	auto doc = ParseBody("while (true) { break; continue; }");
	const auto& body = doc["body"][0]["body"];
	CHECK(body[0].contains("break"));
	CHECK(body[1].contains("continue"));
}

TEST_CASE("Parse: local variable declaration becomes set + locals entry") {
	auto doc = ParseBody("int32 Buffer = 5;");
	REQUIRE(doc["body"].size() == 1);
	CHECK(doc["body"][0]["set"] == "Buffer");
	CHECK(doc["body"][0]["to"]["lit"] == 5);
	REQUIRE(doc["locals"].size() == 1);
	CHECK(doc["locals"][0]["name"] == "Buffer");
	CHECK(doc["locals"][0]["type"] == "int");
}

TEST_CASE("Parse: auto declaration from Cast<T>() expression") {
	auto doc = ParseBody("auto Result = Cast<APawn>(Other);");
	CHECK(doc["body"][0]["set"] == "Result");
	CHECK(doc["body"][0]["to"]["cast"]["var"] == "Other");
	CHECK(doc["body"][0]["to"]["to"] == "APawn");
}

// ===== Expression forms ===================================================

TEST_CASE("Parse: arithmetic uses canonical KismetMathLibrary aliases") {
	auto doc = ParseBody("Sum = X + Y;");
	const auto& to = doc["body"][0]["to"];
	CHECK(to["call"] == "KismetMathLibrary::Add_IntInt");
	CHECK(to["args"]["A"]["var"] == "X");
	CHECK(to["args"]["B"]["var"] == "Y");
}

TEST_CASE("Parse: comparison operators canonicalize") {
	auto doc = ParseBody("bResult = X < Y;");
	CHECK(doc["body"][0]["to"]["call"] == "KismetMathLibrary::Less_IntInt");
}

TEST_CASE("Parse: logical operators canonicalize") {
	auto doc = ParseBody("bResult = bA && bB;");
	CHECK(doc["body"][0]["to"]["call"] == "KismetMathLibrary::BooleanAND");
}

TEST_CASE("Parse: unary ! becomes Not_PreBool") {
	auto doc = ParseBody("bResult = !bAlive;");
	CHECK(doc["body"][0]["to"]["call"] == "KismetMathLibrary::Not_PreBool");
	CHECK(doc["body"][0]["to"]["args"]["A"]["var"] == "bAlive");
}

TEST_CASE("Parse: precedence — multiplication binds tighter than addition") {
	// X = A + B * C   should parse as A + (B * C).
	auto doc = ParseBody("X = A + B * C;");
	const auto& add = doc["body"][0]["to"];
	CHECK(add["call"] == "KismetMathLibrary::Add_IntInt");
	CHECK(add["args"]["A"]["var"] == "A");
	const auto& mul = add["args"]["B"];
	CHECK(mul["call"] == "KismetMathLibrary::Multiply_IntInt");
	CHECK(mul["args"]["A"]["var"] == "B");
	CHECK(mul["args"]["B"]["var"] == "C");
}

TEST_CASE("Parse: parenthesized expression overrides precedence") {
	auto doc = ParseBody("X = (A + B) * C;");
	const auto& mul = doc["body"][0]["to"];
	CHECK(mul["call"] == "KismetMathLibrary::Multiply_IntInt");
	const auto& add = mul["args"]["A"];
	CHECK(add["call"] == "KismetMathLibrary::Add_IntInt");
}

TEST_CASE("Parse: member access (.)") {
	auto doc = ParseBody("Loc = this.RootComp;");
	const auto& to = doc["body"][0]["to"];
	CHECK(to["member"]["self"].is_null());
	CHECK(to["name"] == "RootComp");
}

TEST_CASE("Parse: member access (->) treated same as .") {
	auto doc = ParseBody("Loc = Owner->RootComp;");
	const auto& to = doc["body"][0]["to"];
	CHECK(to["member"]["var"] == "Owner");
	CHECK(to["name"] == "RootComp");
}

TEST_CASE("Parse: array index") {
	auto doc = ParseBody("First = Items[0];");
	const auto& to = doc["body"][0]["to"];
	CHECK(to["index"]["var"] == "Items");
	CHECK(to["idx"]["lit"] == 0);
}

TEST_CASE("Parse: Cast<T>(expr) as expression") {
	auto doc = ParseBody("AsPawn = Cast<APawn>(Other);");
	const auto& to = doc["body"][0]["to"];
	CHECK(to["cast"]["var"] == "Other");
	CHECK(to["to"] == "APawn");
}

TEST_CASE("Parse: literal types") {
	auto doc = ParseBody(
		"X = 42; Y = 3.14; Z = true; W = nullptr;");
	CHECK(doc["body"][0]["to"]["lit"] == 42);
	CHECK(doc["body"][1]["to"]["lit"].is_number());
	CHECK(doc["body"][2]["to"]["lit"] == true);
	CHECK(doc["body"][3]["to"]["lit"].is_null());
}

TEST_CASE("Parse: brace-init list becomes new_array") {
	auto doc = ParseBody("Nums = {1, 2, 3};");
	const auto& to = doc["body"][0]["to"];
	REQUIRE(to["new_array"].size() == 3);
	CHECK(to["new_array"][0]["lit"] == 1);
	CHECK(to["new_array"][2]["lit"] == 3);
}

// ===== Function signatures ================================================

TEST_CASE("Parse: full function definition extracts inputs + return type") {
	auto doc = ParseCppFunction(
		"bool TakeDamage(float Damage) { return true; }");
	CHECK(doc["name"] == "TakeDamage");
	REQUIRE(doc["inputs"].size() == 1);
	CHECK(doc["inputs"][0]["name"] == "Damage");
	CHECK(doc["inputs"][0]["type"] == "float");
	REQUIRE(doc["outputs"].size() == 1);
	CHECK(doc["outputs"][0]["type"] == "bool");
}

TEST_CASE("Parse: ref params are recognized as outputs") {
	auto doc = ParseCppFunction(
		"void Compute(int32& OutA, float& OutB) { return; }");
	CHECK(doc["name"] == "Compute");
	CHECK(doc["inputs"].size() == 0);
	REQUIRE(doc["outputs"].size() == 2);
	CHECK(doc["outputs"][0]["name"] == "OutA");
	CHECK(doc["outputs"][0]["type"] == "int");
	CHECK(doc["outputs"][1]["name"] == "OutB");
	CHECK(doc["outputs"][1]["type"] == "float");
}

TEST_CASE("Parse: const ref param stays as input (not out-ref)") {
	auto doc = ParseCppFunction(
		"void Take(const FString& Tag) { return; }");
	REQUIRE(doc["inputs"].size() == 1);
	CHECK(doc["inputs"][0]["name"] == "Tag");
	// type carries const for documentation; the BPIR mapper still
	// resolves the underlying type.
	CHECK(doc["inputs"][0]["type"].get<std::string>().find("string") !=
		  std::string::npos);
}

TEST_CASE("Parse: object pointer param maps to BPIR object: type") {
	auto doc = ParseCppFunction(
		"void Hit(AActor* Source) { return; }");
	CHECK(doc["inputs"][0]["type"] == "object:Actor");
}

TEST_CASE("Parse: container types reverse-map") {
	auto doc = ParseCppFunction(
		"void Foo(TArray<float> Vals, TMap<FString, int32> Lookup) { return; }");
	CHECK(doc["inputs"][0]["type"] == "[]float");
	CHECK(doc["inputs"][1]["type"] == "{string:int}");
}

// ===== Bare-body w/ explicit signature ====================================

TEST_CASE("Parse: bare body w/ caller-supplied signature") {
	json sig = {
		{"version", 1},
		{"kind", "function"},
		{"name", "Existing"},
		{"inputs", json::array({json{{"name","X"},{"type","int"}}})},
		{"outputs", json::array()},
	};
	json doc = ParseCppFunction("X = X + 1;", sig);
	CHECK(doc["name"] == "Existing");
	CHECK(doc["inputs"][0]["name"] == "X");
	CHECK(doc["body"][0]["set"] == "X");
}

// ===== Validation pass-through ============================================

TEST_CASE("Parse: useless expression statement fails") {
	// A bare `42;` has no effect — parser rejects.
	CHECK_THROWS_AS(ParseBody("42;"), CppParseError);
}

TEST_CASE("Parse: malformed input surfaces line:col error") {
	try {
		ParseBody("if (x { Foo(); }");
		FAIL("expected throw");
	} catch (const CppParseError& e) {
		std::string msg = e.what();
		// Format is "<line>:<col>: <message>" with a colon prefix.
		CHECK(msg.find(":") != std::string::npos);
	}
}

TEST_CASE("Parse: produced BPIR is valid against the schema") {
	auto doc = ParseCppFunction(
		"bool Big(float Amount, AActor* Src) {"
		"  if (bIsAlive) {"
		"    Health -= Amount;"
		"    if (auto* AsPawn = Cast<APawn>(Src)) { Hit(); } else { Miss(); }"
		"  }"
		"  return true;"
		"}");
	// Already validated internally by ParseCppFunction; if we got here,
	// the doc passed. Re-running asserts the public validator agrees.
	CHECK_NOTHROW(ValidateBpir(doc));
}
