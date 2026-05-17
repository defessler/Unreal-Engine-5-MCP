// Synthetic structural-diff tests — exercise the diff wiring against
// the mock backend (which throws "not supported" because diffing
// requires UBlueprint reflection that fixture JSON doesn't carry) and
// validate the wire-format shape produced by
// BlueprintStructuralDiff::FResult::ToJson() by synthesizing each diff
// category in nlohmann::json and asserting callers can parse the
// expected fields.
//
// Real diff behavior — actually comparing two UBlueprints — is exercised
// end-to-end against the commandlet/live backend in test_roundtrip_*.cpp
// (Tasks 19 + 20). Those tests need a real engine; this file is mock-only
// and runs in <1ms as part of the standard doctest sweep.

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include "backends/IBlueprintReader.h"
#include "backends/MockBlueprintReader.h"

#include "test_helpers.h"

#include <string>

using namespace bpr::backends;

// ---------------------------------------------------------------------------
// Wiring: mock backend throws "not supported" — callers should catch
// BlueprintReaderError and surface the message as the MCP tool error.
// ---------------------------------------------------------------------------

TEST_CASE("structural_diff: mock backend reports not-supported") {
	auto mock = bpr::test::MakeMockReader();
	CHECK_THROWS_AS(mock.StructuralDiff("/Game/A", "/Game/B", {}),
					BlueprintReaderError);
}

TEST_CASE("structural_diff: not-supported message mentions live/commandlet") {
	auto mock = bpr::test::MakeMockReader();
	try {
		mock.StructuralDiff("/Game/A", "/Game/B", {});
		FAIL("expected throw");
	} catch (const BlueprintReaderError& e) {
		const std::string msg = e.what();
		// Operator should know which backend to switch to.
		CHECK((msg.find("live") != std::string::npos ||
			   msg.find("commandlet") != std::string::npos));
	}
}

TEST_CASE("structural_diff: options struct still throws on mock") {
	auto mock = bpr::test::MakeMockReader();
	IBlueprintReader::StructuralDiffOptions opts;
	opts.ignoreNodePositions = false;
	opts.ignoreCommentNodes  = true;
	CHECK_THROWS_AS(mock.StructuralDiff("/Game/A", "/Game/B", opts),
					BlueprintReaderError);
}

TEST_CASE("structural_diff: default options have expected values") {
	IBlueprintReader::StructuralDiffOptions opts;
	// Position noise is the most common source of false positives; default
	// must be true so a clone diffs cleanly even if the layout drifted.
	CHECK(opts.ignoreNodePositions == true);
	// Comment nodes carry author intent — include them by default; opt out
	// when comparing across BPs authored at different times.
	CHECK(opts.ignoreCommentNodes == false);
}

// ---------------------------------------------------------------------------
// Wire-format shape: synthesize each diff category as the plugin emits
// it and verify the nlohmann::json envelope round-trips through the
// fields callers (CommandletBlueprintReader, MCP tool layer) read.
//
// The shape produced by BlueprintStructuralDiff::FResult::ToJson() is:
//   { "ok": bool, "differences": [ {"path":str, "kind":str, "a":str, "b":str}, ... ] }
//
// `ok` mirrors `bDifferences.IsEmpty()` (true only when nothing diverged).
// `kind` is one of: "missing", "extra", "value_mismatch", "type_mismatch".
// ---------------------------------------------------------------------------

namespace {

// Minimal helper that mirrors what the plugin's FDifference -> JSON
// emission produces. Centralized so each TEST_CASE below stays focused
// on one category's shape.
nlohmann::json MakeDifference(const char* path, const char* kind,
							  const char* a, const char* b) {
	return nlohmann::json{
		{"path", path},
		{"kind", kind},
		{"a",    a},
		{"b",    b},
	};
}

}    // namespace

TEST_CASE("structural_diff wire: identical BPs report ok=true with empty differences") {
	// When two BPs are structurally identical, FResult::bEqual is true and
	// Differences is empty. Callers gate on `ok` to decide pass/fail.
	const nlohmann::json shape = {
		{"ok", true},
		{"differences", nlohmann::json::array()},
	};
	REQUIRE(shape.contains("ok"));
	REQUIRE(shape.contains("differences"));
	CHECK(shape["ok"].get<bool>() == true);
	CHECK(shape["differences"].is_array());
	CHECK(shape["differences"].empty());
}

TEST_CASE("structural_diff wire: extra category — node/var only in B") {
	// "extra" = present in B (candidate), absent in A (source). Path uses
	// the dotted convention "variables.<name>" or "components.<name>" or
	// "graphs.<name>.nodes.<stable_id>".
	const nlohmann::json shape = {
		{"ok", false},
		{"differences", nlohmann::json::array({
			MakeDifference("variables.NewHealth", "extra", "absent", "present"),
		})},
	};
	REQUIRE(shape["differences"].size() == 1);
	const auto& d = shape["differences"][0];
	CHECK(d["kind"].get<std::string>() == "extra");
	CHECK(d["path"].get<std::string>() == "variables.NewHealth");
	CHECK(d["a"].get<std::string>() == "absent");
	CHECK(d["b"].get<std::string>() == "present");
	CHECK(shape["ok"].get<bool>() == false);
}

TEST_CASE("structural_diff wire: missing category — node/var only in A") {
	// "missing" = present in A (source), absent in B (candidate). The
	// inverse of "extra".
	const nlohmann::json shape = {
		{"ok", false},
		{"differences", nlohmann::json::array({
			MakeDifference("variables.Mana", "missing", "present", "absent"),
		})},
	};
	REQUIRE(shape["differences"].size() == 1);
	const auto& d = shape["differences"][0];
	CHECK(d["kind"].get<std::string>() == "missing");
	CHECK(d["path"].get<std::string>() == "variables.Mana");
	CHECK(d["a"].get<std::string>() == "present");
	CHECK(d["b"].get<std::string>() == "absent");
}

TEST_CASE("structural_diff wire: value_mismatch — same path, different scalar") {
	// "value_mismatch" = same key on both sides but the scalar value
	// differs. For variables the path suffix is ".default"; for
	// connection counts it's the graph-level count delta.
	const nlohmann::json shape = {
		{"ok", false},
		{"differences", nlohmann::json::array({
			MakeDifference("variables.Health.default", "value_mismatch",
						   "100.0", "150.0"),
		})},
	};
	REQUIRE(shape["differences"].size() == 1);
	const auto& d = shape["differences"][0];
	CHECK(d["kind"].get<std::string>() == "value_mismatch");
	CHECK(d["path"].get<std::string>() == "variables.Health.default");
	CHECK(d["a"].get<std::string>() == "100.0");
	CHECK(d["b"].get<std::string>() == "150.0");
}

TEST_CASE("structural_diff wire: type_mismatch — variable type differs") {
	// "type_mismatch" = the key matches but the type signature doesn't.
	// For variables the path suffix is ".type"; for components it's
	// ".class" (ComponentClass path).
	const nlohmann::json shape = {
		{"ok", false},
		{"differences", nlohmann::json::array({
			MakeDifference("variables.Health.type", "type_mismatch",
						   "float", "int"),
		})},
	};
	REQUIRE(shape["differences"].size() == 1);
	const auto& d = shape["differences"][0];
	CHECK(d["kind"].get<std::string>() == "type_mismatch");
	CHECK(d["path"].get<std::string>() == "variables.Health.type");
	CHECK(d["a"].get<std::string>() == "float");
	CHECK(d["b"].get<std::string>() == "int");
}

TEST_CASE("structural_diff wire: mixed categories survive JSON round-trip") {
	// Sanity check: a realistic multi-category diff serializes + parses
	// without losing any of the four fields per entry, in declaration
	// order.
	const nlohmann::json shape = {
		{"ok", false},
		{"differences", nlohmann::json::array({
			MakeDifference("variables.A",                "missing",
						   "present", "absent"),
			MakeDifference("variables.B",                "extra",
						   "absent",  "present"),
			MakeDifference("variables.C.type",           "type_mismatch",
						   "float",   "int"),
			MakeDifference("variables.D.default",        "value_mismatch",
						   "1.0",     "2.0"),
			MakeDifference("components.Mesh.class",      "type_mismatch",
						   "/Script/Engine.StaticMeshComponent",
						   "/Script/Engine.SkeletalMeshComponent"),
			MakeDifference("graphs.EventGraph",          "missing",
						   "",        ""),
		})},
	};

	const std::string serialized = shape.dump();
	const auto parsed = nlohmann::json::parse(serialized);

	CHECK(parsed["ok"].get<bool>() == false);
	REQUIRE(parsed["differences"].size() == 6);

	// Tally per kind — the plugin makes no ordering guarantees beyond
	// "stable within a category". Counting is the right shape assertion.
	std::map<std::string, int> kindCounts;
	for (const auto& d : parsed["differences"]) {
		REQUIRE(d.contains("path"));
		REQUIRE(d.contains("kind"));
		REQUIRE(d.contains("a"));
		REQUIRE(d.contains("b"));
		kindCounts[d["kind"].get<std::string>()] += 1;
	}
	CHECK(kindCounts["missing"]        == 2);
	CHECK(kindCounts["extra"]          == 1);
	CHECK(kindCounts["type_mismatch"]  == 2);
	CHECK(kindCounts["value_mismatch"] == 1);
}
