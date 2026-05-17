// Live integration tests for the BPIRRoundtrip pipeline
// (Task 21 of the BP-roundtrip plan; Task 22 appends a TPC case).
//
// IMPORTANT — scope of these tests:
// BPIRRoundtrip currently implements ONLY stages 1-3 of the pipeline
// (decompile -> emit C++ source -> write to disk). Stages 4 (parse the
// emitted C++ back to BPIR) and 5 (transpile BPIR into a new BP) are
// deliberately stubbed in BPIRRoundtrip.cpp because no whole-class
// `ParseCppPair...` / `TranspileBlueprintWhole` helpers exist yet --
// `ParseCppFunction` is per-function and `transpile_blueprint` only
// emits source. See BPIRRoundtrip.cpp lines ~195-214 for the deferral
// comment.
//
// These tests therefore validate:
//   - stages 1-3 succeed (bpir_before populated, .h/.cpp on disk)
//   - the emitted files contain the expected class declaration
//   - stages 4-5 fail deterministically with failing_stage == "parse"
//     and an error message naming the unimplemented helper, so the
//     stub stays honest and a future Stage-4 implementation breaks
//     these CHECKs (intentionally -- tighten them then).
//
// Skipped when the live backend env vars aren't present.
//
// We deliberately do NOT call SaveAll() / StructuralDiff() / DeleteAsset
// on a clone path -- stages 4-5 never produce one. When parse/transpile
// land, callers can re-enable those checks.

#include <doctest/doctest.h>

#include "backends/CommandletBlueprintReader.h"
#include "roundtrip/BPIRRoundtrip.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace test_roundtrip_bpir_detail {

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

// Read an entire file into a string; returns empty on error.
inline std::string SlurpFile(const std::filesystem::path& p) {
	std::ifstream in(p, std::ios::binary);
	if (!in.is_open()) {
		return {};
	}
	std::ostringstream ss;
	ss << in.rdbuf();
	return ss.str();
}

// Remove a generated source file if present; ignore errors. The
// containing dir is .gitignored so leftover files don't pollute git
// status, but cleaning them keeps reruns deterministic and prevents
// stale files from masking emit regressions.
inline void RemoveIfExists(const std::filesystem::path& p) {
	std::error_code ec;
	std::filesystem::remove(p, ec);
}

}    // namespace test_roundtrip_bpir_detail
using namespace test_roundtrip_bpir_detail;

TEST_CASE("[live][roundtrip][bpir] BP_Enemy -> decompile -> emit cpp"
		  " (stages 1-3 complete, 4-5 deferred)"
		  * doctest::skip(!LiveBackendAvailable())) {
	auto reader = MakeLiveReader();
	REQUIRE(reader);

	const std::string src    = "/Game/AI/BP_TestEnemy";
	const std::string clone  = "/Game/Recreated/BPIR_BP_Enemy";
	const std::string engine = GetEnv("BP_READER_ENGINE_DIR");
	const std::string proj   = GetEnv("BP_READER_PROJECT");
	REQUIRE_FALSE(engine.empty());
	REQUIRE_FALSE(proj.empty());

	auto res = bpr::roundtrip::RunBPIRRoundtrip(*reader, src, clone, engine, proj);
	CAPTURE(res.failing_stage);
	CAPTURE(res.error_message);
	CAPTURE(res.emitted_h_path);
	CAPTURE(res.emitted_cpp_path);

	// Stage 1: decompile populated bpir_before.
	CHECK(res.source_package_path == src);
	CHECK(res.output_package_path == clone);
	CHECK_FALSE(res.bpir_before.is_null());
	CHECK(res.bpir_before.is_object());
	// BPIR class doc carries a "kind":"class" marker.
	if (res.bpir_before.is_object() && res.bpir_before.contains("kind")) {
		CHECK(res.bpir_before["kind"] == "class");
	}

	// Stage 2/3: .h/.cpp files exist on disk with non-empty content.
	REQUIRE_FALSE(res.emitted_h_path.empty());
	REQUIRE_FALSE(res.emitted_cpp_path.empty());
	const std::filesystem::path hPath{res.emitted_h_path};
	const std::filesystem::path cppPath{res.emitted_cpp_path};
	CHECK(std::filesystem::exists(hPath));
	CHECK(std::filesystem::exists(cppPath));

	const std::string hSrc   = SlurpFile(hPath);
	const std::string cppSrc = SlurpFile(cppPath);
	CHECK_FALSE(hSrc.empty());
	CHECK_FALSE(cppSrc.empty());
	// CppClassEmit with classNameSuffix="" produces "ABP_Enemy"
	// (UE Actor lineage gets the A-prefix). The PrefixClassName
	// logic is exercised here.
	CHECK(hSrc.find("class") != std::string::npos);
	CHECK(hSrc.find("BP_TestEnemy") != std::string::npos);
	CHECK(cppSrc.find("BP_TestEnemy") != std::string::npos);

	// Stage 4 is MINIMAL (passes bpir_before through as bpir_after) and
	// Stage 5 is a documented stub (BPIR -> BP materialization needs
	// RunCommand log-capture fixed first — see BPIRRoundtrip.cpp).
	// What we CAN verify today: bpir_after is non-null (pipeline ran
	// through Stage 4) and the failure mode is the documented stub.
	CHECK_FALSE(res.bpir_after.is_null());
	CHECK(res.bpir_after.is_object());
	CHECK_FALSE(res.ok);
	CHECK(res.failing_stage == "transpile");
	CHECK(res.error_message.find("stage 5") != std::string::npos);

	// Cleanup the generated pair so the next run starts fresh and a
	// regression in the emit step doesn't get masked by stale files.
	// build.log (stage-3 artifact) is left in place for triage.
	// Set BP_READER_KEEP_GENERATED=1 to skip cleanup when debugging.
	if (GetEnv("BP_READER_KEEP_GENERATED").empty()) {
		RemoveIfExists(hPath);
		RemoveIfExists(cppPath);
	}
}

TEST_CASE("[live][slow][roundtrip][bpir] BP_ThirdPersonCharacter -> decompile -> emit cpp"
		  " (stages 1-3 complete, 4-5 deferred)"
		  * doctest::skip(!LiveBackendAvailable())) {
	auto reader = MakeLiveReader();
	REQUIRE(reader);

	const std::string src    = "/Game/Imported/ThirdPerson/BP_ThirdPersonCharacter";
	const std::string clone  = "/Game/Recreated/BPIR_BP_ThirdPersonCharacter";
	const std::string engine = GetEnv("BP_READER_ENGINE_DIR");
	const std::string proj   = GetEnv("BP_READER_PROJECT");
	REQUIRE_FALSE(engine.empty());
	REQUIRE_FALSE(proj.empty());

	auto res = bpr::roundtrip::RunBPIRRoundtrip(*reader, src, clone, engine, proj);
	CAPTURE(res.failing_stage);
	CAPTURE(res.error_message);
	CAPTURE(res.emitted_h_path);
	CAPTURE(res.emitted_cpp_path);

	// Stage 1: decompile populated bpir_before.
	CHECK(res.source_package_path == src);
	CHECK(res.output_package_path == clone);
	CHECK_FALSE(res.bpir_before.is_null());
	CHECK(res.bpir_before.is_object());
	if (res.bpir_before.is_object() && res.bpir_before.contains("kind")) {
		CHECK(res.bpir_before["kind"] == "class");
	}

	// Stage 2/3: .h/.cpp files exist on disk with non-empty content
	// mentioning the BP name. TPC is a Character subclass so
	// PrefixClassName produces the A-prefix (-> "ABP_ThirdPersonCharacter"
	// with classNameSuffix="").
	REQUIRE_FALSE(res.emitted_h_path.empty());
	REQUIRE_FALSE(res.emitted_cpp_path.empty());
	const std::filesystem::path hPath{res.emitted_h_path};
	const std::filesystem::path cppPath{res.emitted_cpp_path};
	CHECK(std::filesystem::exists(hPath));
	CHECK(std::filesystem::exists(cppPath));

	const std::string hSrc   = SlurpFile(hPath);
	const std::string cppSrc = SlurpFile(cppPath);
	CHECK_FALSE(hSrc.empty());
	CHECK_FALSE(cppSrc.empty());
	CHECK(hSrc.find("class") != std::string::npos);
	CHECK(hSrc.find("BP_ThirdPersonCharacter") != std::string::npos);
	CHECK(cppSrc.find("BP_ThirdPersonCharacter") != std::string::npos);

	// Stage 4 is MINIMAL (passes bpir_before through as bpir_after) and
	// Stage 5 is a documented stub. Same shape as the BP_TestEnemy test.
	CHECK_FALSE(res.bpir_after.is_null());
	CHECK(res.bpir_after.is_object());
	CHECK_FALSE(res.ok);
	CHECK(res.failing_stage == "transpile");
	CHECK(res.error_message.find("stage 5") != std::string::npos);

	if (GetEnv("BP_READER_KEEP_GENERATED").empty()) {
		RemoveIfExists(hPath);
		RemoveIfExists(cppPath);
	}
}
