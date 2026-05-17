// Round-trip tests: BPIR → C++ → BPIR.
//
// For the patterns CppEmit produces, parsing the emitted C++ should
// yield BPIR equivalent to the original (modulo cosmetic fields like
// metadata that the parser can't recover from source). We assert
// equivalence on the body shape — the meaningful semantic structure.
//
// This pins the property that CppEmit and CppParse compose to identity
// on the language subset we ship. Drift in either direction breaks the
// build, which is the whole point of having both ends of the pipeline.

#include <doctest/doctest.h>

#include "tools/codegen/CppEmit.h"
#include "tools/parse/CppParse.h"
#include "tools/Bpir.h"

using namespace bpr::tools;
using nlohmann::json;

namespace {

json MakeFn(const json& body, std::vector<json> inputs = {},
			std::vector<json> outputs = {}) {
	json doc = {
		{"version", 1},
		{"kind", "function"},
		{"name", "TestFn"},
		{"body", body},
	};
	if (!inputs.empty())
	{
		doc["inputs"]  = inputs;
	}
	if (!outputs.empty())
	{
		doc["outputs"] = outputs;
	}
	return doc;
}

// Assert that two BPIR bodies are structurally equal. We diff JSON
// directly — nlohmann's == handles deep equality.
void CheckBodiesEqual(const json& a, const json& b) {
	INFO("expected: ", a.dump(2));
	INFO("got:      ", b.dump(2));
	CHECK(a == b);
}

// Run BPIR → CppEmit → CppParse and return the parsed BPIR's body.
json Roundtrip(const json& bpir) {
	auto emitted = EmitCppFunction(bpir);
	auto parsed = ParseCppFunction(emitted.source);
	return parsed["body"];
}

} // namespace

TEST_CASE("Roundtrip: simple set with int literal") {
	json original = MakeFn(json::array({
		json{{"set", "Health"}, {"to", json{{"lit", 100}}}}
	}));
	CheckBodiesEqual(original["body"], Roundtrip(original));
}

TEST_CASE("Roundtrip: if/then/else") {
	json original = MakeFn(json::array({
		json{{"if",   json{{"var","bAlive"}}},
			 {"then", json::array({json{{"call","Foo"},{"args",json::object()}}})},
			 {"else", json::array({json{{"call","Bar"},{"args",json::object()}}})}},
	}));
	CheckBodiesEqual(original["body"], Roundtrip(original));
}

TEST_CASE("Roundtrip: bare return + return with value") {
	json original = MakeFn(json::array({
		json{{"return", json{{"var","Health"}}}},
		json{{"return", nullptr}},
	}));
	CheckBodiesEqual(original["body"], Roundtrip(original));
}

TEST_CASE("Roundtrip: range-based for") {
	json original = MakeFn(json::array({
		json{{"for_each", "Item"},
			 {"in",  json{{"var","Items"}}},
			 {"body", json::array({json{{"call","UseItem"},{"args",json::object()}}})}}
	}));
	CheckBodiesEqual(original["body"], Roundtrip(original));
}

TEST_CASE("Roundtrip: cast statement") {
	json original = MakeFn(json::array({
		json{{"cast", json{{"var","Other"}}},
			 {"to", "APawn"}, {"as", "AsPawn"},
			 {"success", json::array({json{{"call","Hit"},{"args",json::object()}}})},
			 {"fail",    json::array({json{{"call","Miss"},{"args",json::object()}}})}}
	}));
	CheckBodiesEqual(original["body"], Roundtrip(original));
}

TEST_CASE("Roundtrip: arithmetic operator alias") {
	json original = MakeFn(json::array({
		json{{"set", "Sum"}, {"to",
			 json{{"call","KismetMathLibrary::Add_IntInt"},
				  {"args", json{{"A", json{{"var","X"}}},
								{"B", json{{"var","Y"}}}}}}}}
	}));
	CheckBodiesEqual(original["body"], Roundtrip(original));
}

TEST_CASE("Roundtrip: comparison + logical operators") {
	json original = MakeFn(json::array({
		json{{"set", "bResult"},
			 {"to", json{{"call","KismetMathLibrary::BooleanAND"},
						 {"args", json{
							 {"A", json{{"call","KismetMathLibrary::Less_IntInt"},
										{"args", json{{"A", json{{"var","X"}}},
													  {"B", json{{"var","Y"}}}}}}},
							 {"B", json{{"var","bAlive"}}}
						 }}}}}
	}));
	CheckBodiesEqual(original["body"], Roundtrip(original));
}

TEST_CASE("Roundtrip: while loop") {
	json original = MakeFn(json::array({
		json{{"while", json{{"var","bRunning"}}},
			 {"body",  json::array({json{{"call","Tick"},{"args",json::object()}}})}}
	}));
	CheckBodiesEqual(original["body"], Roundtrip(original));
}

TEST_CASE("Roundtrip: switch statement") {
	json original = MakeFn(json::array({
		json{{"switch", json{{"var","Mode"}}},
			 {"cases",  json{
				 {"0", json::array({json{{"call","ModeA"},{"args",json::object()}}})},
				 {"1", json::array({json{{"call","ModeB"},{"args",json::object()}}})},
			 }},
			 {"default", json::array({json{{"call","ModeOther"},{"args",json::object()}}})}}
	}));
	CheckBodiesEqual(original["body"], Roundtrip(original));
}

TEST_CASE("Roundtrip: member + index expressions") {
	json original = MakeFn(json::array({
		json{{"set","Loc"},{"to",
			 json{{"member", json{{"self", nullptr}}}, {"name","RootComp"}}}},
		json{{"set","First"},{"to",
			 json{{"index", json{{"var","Items"}}}, {"idx", json{{"lit",0}}}}}},
	}));
	CheckBodiesEqual(original["body"], Roundtrip(original));
}

TEST_CASE("Roundtrip: nested if + binop combination") {
	json original = MakeFn(json::array({
		json{{"if", json{{"call","KismetMathLibrary::Greater_IntInt"},
						 {"args", json{{"A", json{{"var","Health"}}},
									   {"B", json{{"lit", 0}}}}}}},
			 {"then", json::array({
				 json{{"set","Health"},{"to",
					  json{{"call","KismetMathLibrary::Subtract_IntInt"},
						   {"args", json{{"A", json{{"var","Health"}}},
										 {"B", json{{"var","Damage"}}}}}}}},
				 json{{"return", json{{"lit", true}}}},
			 })}},
	}));
	CheckBodiesEqual(original["body"], Roundtrip(original));
}
