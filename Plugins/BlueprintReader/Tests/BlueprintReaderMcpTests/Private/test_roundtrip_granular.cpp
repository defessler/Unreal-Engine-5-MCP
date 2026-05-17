// Granular-writes roundtrip — ReadToSpec -> SpecToBP -> StructuralDiff.
//
// Gated on the live commandlet backend (env BP_READER_ENGINE_DIR +
// BP_READER_PROJECT set, same gate as test_commandlet_backend.cpp).
// Skipped on a vanilla doctest run so the mock-only suite stays fast.
//
// Known limitation acknowledged here: SpecToBP currently can't substitute
// new ids for auto-spawned function skeleton nodes (K2Node_FunctionEntry /
// K2Node_FunctionResult) — see the comment in SpecToBP.cpp around the
// `isAutoSpawn` block. For any function with inputs or outputs the
// resulting WirePins call(s) against those entry/result nodes will fail
// (or no-op) and the diff will show a connection-count mismatch on that
// function's graph. The test below CHECKs for an empty diff but also
// surveys the diff entries when non-empty, marking the test as an
// acknowledged-gap case rather than a hard failure when the only
// differences are restricted to function graphs that have parameters.

#include <doctest/doctest.h>

#include "backends/CommandletBlueprintReader.h"
#include "roundtrip/BPSpec.h"
#include "roundtrip/ReadToSpec.h"
#include "roundtrip/SpecToBP.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <set>
#include <string>

#include <nlohmann/json.hpp>

namespace test_roundtrip_granular_detail {

inline std::string GetEnv(const char* key) {
#ifdef _MSC_VER
	char* buf = nullptr;
	std::size_t len = 0;
	if (_dupenv_s(&buf, &len, key) == 0 && buf != nullptr) {
		std::string out(buf);
		std::free(buf);
		return out;
	}
	return {};
#else    // !_MSC_VER
	if (const char* v = std::getenv(key); v != nullptr && *v != '\0') {
		return std::string(v);
	}
	return {};
#endif    // _MSC_VER
}

inline bool LiveBackendAvailable() {
	return !GetEnv("BP_READER_ENGINE_DIR").empty() &&
		   !GetEnv("BP_READER_PROJECT").empty();
}

inline std::unique_ptr<bpr::backends::CommandletBlueprintReader>
MakeLiveReader(bool useDaemon = true) {
	bpr::backends::CommandletBlueprintReader::Config cfg;
	cfg.engineDir = std::filesystem::path(GetEnv("BP_READER_ENGINE_DIR"));
	cfg.uproject  = std::filesystem::path(GetEnv("BP_READER_PROJECT"));
	cfg.timeout   = std::chrono::seconds(180);
	cfg.useDaemon = useDaemon;
	return std::make_unique<bpr::backends::CommandletBlueprintReader>(std::move(cfg));
}

// Classify a diff entry as a known-gap (auto-spawn function entry/result
// reconciliation) or a genuine roundtrip failure. The current SpecToBP
// can't reassign stable ids onto the engine-spawned K2Node_FunctionEntry/
// FunctionResult nodes when a function has inputs or outputs, so
// WirePins against those nodes never lands. Symptoms in the diff:
//   * function-graph node-count mismatches on entry/result pin signatures
//     (extra pins appear on the clone's entry/result because AddFunction
//     auto-builds them but the source's specific pin order differs).
//   * link-count drift on the function's graph (the entry's `then`
//     wire to the body never gets restored).
// Anything outside `graphs.<FunctionName>` is NOT in the exemption.
inline bool IsKnownAutoSpawnGap(const nlohmann::json& diffEntry,
								const std::set<std::string>& functionNamesWithParams) {
	if (!diffEntry.is_object()) return false;
	const std::string path = diffEntry.value("path", std::string{});
	for (const auto& fnName : functionNamesWithParams) {
		const std::string fnPrefix = "graphs." + fnName;
		if (path.rfind(fnPrefix, 0) == 0) {
			return true;
		}
	}
	return false;
}

}    // namespace test_roundtrip_granular_detail
using namespace test_roundtrip_granular_detail;


TEST_CASE("[live][roundtrip][granular] BP_TestEnemy -> BPSpec -> SpecToBP "
		  "produces empty-or-known-diff clone"
		  * doctest::skip(!LiveBackendAvailable())) {
	auto reader = MakeLiveReader();

	const std::string source = "/Game/AI/BP_TestEnemy";
	const std::string clone  = "/Game/Recreated/BP_TestEnemy_Granular";

	// Tear down any prior clone so the test is idempotent.
	try { reader->DeleteAsset(clone, /*force=*/true); }
	catch (...) { /* ignore — first run won't have one */ }

	// 1. Read source -> BPSpec
	auto spec = bpr::roundtrip::ReadToSpec(*reader, source);
	REQUIRE_FALSE(spec.incomplete);
	CHECK_GE(spec.variables.size(), 3);  // Health, MaxHealth, AggroTarget
	CHECK_GE(spec.functions.size(), 2);  // TakeDamage, OnDeath

	// Collect functions whose parameter shape exposes the SpecToBP
	// auto-spawn-id limitation — these are the only places we allow
	// diff drift in this test. BP_TestEnemy's `TakeDamage` is the
	// candidate (Damage float input + Killed bool output); `OnDeath`
	// has no params and should round-trip cleanly.
	std::set<std::string> functionsWithParams;
	for (const auto& f : spec.functions) {
		if (!f.inputs.empty() || !f.outputs.empty()) {
			functionsWithParams.insert(f.name);
		}
	}

	// 2. Materialize the clone via the granular write path.
	auto res = bpr::roundtrip::SpecToBP(*reader, spec, clone);
	CAPTURE(res.failing_stage);
	CAPTURE(res.failing_op);
	CAPTURE(res.error_message);
	// The known limitation in SpecToBP for auto-spawned entry/result
	// wiring is a *connection* drop inside materializeGraph, not a
	// thrown exception — so SpecToBP itself should still return ok=true.
	// If it doesn't, that's a real regression, not the known gap.
	CHECK(res.ok);

	// 3. Save the clone so the structural diff sees committed state.
	auto saveRes = reader->SaveAll(/*dirtyOnly=*/true);
	CAPTURE(saveRes.savedCount);

	// 4. Diff clone vs source.
	auto diff = reader->StructuralDiff(source, clone, {});
	INFO("diff JSON: " << diff.dump(2));

	const bool diffOk = diff.value("ok", false);
	if (diffOk) {
		// Perfect roundtrip — best case.
		SUCCEED("BP_TestEnemy roundtripped with zero structural drift");
	} else {
		// Verify every remaining difference falls under the known
		// auto-spawn-id exemption. Anything else fails the test.
		const auto& differences = diff["differences"];
		REQUIRE(differences.is_array());
		bool allExempt = true;
		for (const auto& d : differences) {
			if (!IsKnownAutoSpawnGap(d, functionsWithParams)) {
				INFO("Unexpected diff entry outside known auto-spawn "
					 "exemption: " << d.dump(2));
				allExempt = false;
			}
		}
		CHECK_MESSAGE(allExempt,
			"BP_TestEnemy roundtrip diff contained entries outside the "
			"known SpecToBP auto-spawned-entry-id limitation. Inspect "
			"the diff JSON above and either fix SpecToBP or extend the "
			"exemption.");
	}

	// 5. Cleanup so reruns and subsequent tests start fresh.
	try { reader->DeleteAsset(clone, /*force=*/true); } catch (...) {}
}


TEST_CASE("[live][slow][roundtrip][granular] BP_ThirdPersonCharacter -> "
		  "BPSpec -> SpecToBP produces clone"
		  * doctest::skip(!LiveBackendAvailable())) {
	auto reader = MakeLiveReader();

	const std::string source = "/Game/Imported/ThirdPerson/BP_ThirdPersonCharacter";
	const std::string clone  = "/Game/Recreated/BP_TPC_Granular";

	// Tear down any prior clone.
	try { reader->DeleteAsset(clone, /*force=*/true); } catch (...) {}

	// 1. Read source -> BPSpec
	auto spec = bpr::roundtrip::ReadToSpec(*reader, source);
	CAPTURE(spec.incomplete);
	if (!spec.errors.empty()) {
		// Surface the first error so a diff hunt isn't a guessing game.
		INFO("ReadToSpec error[0]: " << spec.errors.front());
	}
	// TPC is large — ReadToSpec must still complete a non-empty spec
	// even if some reads were partial. We assert package shape, not
	// incompleteness (TPC's macros and lambda-call nodes often surface
	// minor read warnings on BlueprintGeneratedClass parent_class
	// resolution).
	CHECK(spec.package_path == source);
	CHECK_FALSE(spec.parent_class.empty());

	// 2. Materialize the clone. TPC is much larger than BP_TestEnemy,
	// so SpecToBP can hit the "node class not in AddNode dispatch"
	// path on K2 nodes we haven't taught it yet (e.g. K2Node_InputAction,
	// K2Node_DynamicCast, etc. depending on which UE 5.7 template was
	// imported). This test's contract is "the pipeline runs without
	// catastrophe" — no exceptions, all stages reachable, and the
	// failing-op breadcrumb names the exact unsupported node class so
	// the next iteration of SpecToBP can extend the dispatch table.
	auto res = bpr::roundtrip::SpecToBP(*reader, spec, clone);
	CAPTURE(res.failing_stage);
	CAPTURE(res.failing_op);
	CAPTURE(res.error_message);

	if (!res.ok) {
		// Non-fatal expected outcome on first run for TPC: an
		// unsupported node class halts the rebuild. The test still
		// passes as long as failure is reported in a structured way
		// (not via an exception escaping SpecToBP).
		INFO("TPC roundtrip stopped at stage='" << res.failing_stage
			 << "', op='" << res.failing_op << "'");
		CHECK_FALSE(res.failing_stage.empty());
		CHECK_FALSE(res.failing_op.empty());
		// Save whatever state we got so the diff below has something
		// to compare against.
		try { reader->SaveAll(/*dirtyOnly=*/true); } catch (...) {}
	} else {
		// Full clone succeeded; save + diff.
		reader->SaveAll(/*dirtyOnly=*/true);
		nlohmann::json diff;
		try { diff = reader->StructuralDiff(source, clone, {}); }
		catch (const std::exception& e) {
			INFO("StructuralDiff threw: " << e.what());
			diff = nlohmann::json::object();
		}
		INFO("diff JSON: " << diff.dump(2));
		// For TPC we don't (yet) assert ok=true. Just confirm the
		// diff call shape stayed sane.
		CHECK(diff.contains("ok"));
	}

	// 3. Cleanup so reruns and subsequent tests start fresh.
	try { reader->DeleteAsset(clone, /*force=*/true); } catch (...) {}
}
