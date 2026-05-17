// BPIRRoundtrip — drives the full BP → BPIR → C++ → UBT → BPIR' → BP
// pipeline. Used by the BPIR-path roundtrip tests; orchestrates calls
// across IBlueprintReader (decompile / transpile / write_generated_
// source) and the local UBT toolchain (compile step).
//
// Pipeline stages (mirrored by BPIRRoundtripResult::failing_stage):
//   1. "decompile"  — IBlueprintReader.read + tools::DecompileBlueprint
//                     captures the source BP as a BPIR class doc.
//   2. "emit"       — tools::EmitCppClass turns the BPIR doc into a
//                     compilable .h/.cpp pair (Generated/ subdir of
//                     BPRoundtripModule's source tree).
//   3. "compile"    — UBT builds BPRoundtripModule with the just-
//                     emitted code on disk. Build log captured.
//   4. "parse"      — Re-parse the emitted C++ back to BPIR. Today
//                     ParseCppFunction is per-function only; the
//                     whole-class re-parse path is not yet wired and
//                     this stage records that limitation.
//   5. "transpile"  — Materialize bpir_after into a fresh BP at
//                     output_package_path. Same caveat as parse:
//                     waiting on a whole-class compile_blueprint
//                     helper to lift the registry-tool lambda.
//
// On any stage failure, RunBPIRRoundtrip returns early with
// failing_stage / error_message populated. Successful runs set ok=true
// and leave bpir_before / bpir_after populated for diffing by callers.

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "backends/IBlueprintReader.h"

namespace bpr::roundtrip {

struct BPIRRoundtripResult {
	bool ok = false;
	std::string source_package_path;
	std::string output_package_path;
	std::string emitted_cpp_path;
	std::string emitted_h_path;
	std::string failing_stage;     // "decompile", "emit", "compile", "parse", "transpile"
	std::string failing_op;        // populated alongside failing_stage on transpile stage
	std::string error_message;
	std::string build_log_path;    // populated on compile failures
	nlohmann::json bpir_before;    // BPIR captured pre-compile (for diffing)
	nlohmann::json bpir_after;     // BPIR captured post-parse
	// Per-function body-compile failures. Non-fatal — Stage 5 still
	// returns ok=true even when some BPIR functions have body statement
	// forms beyond compile_function's supported subset (return, cast,
	// switch, loops, etc.). Each entry is "<function_name>: <reason>".
	std::vector<std::string> body_compile_failures;
};

// Runs the full pipeline. `engineDir` / `projectFile` point at the
// installation for the UBT compile step. Returns a result struct
// describing what succeeded and (on failure) which stage broke and
// why. Does not throw — callers inspect `ok` / `failing_stage`.
BPIRRoundtripResult RunBPIRRoundtrip(backends::IBlueprintReader& reader,
                                     std::string_view sourcePackagePath,
                                     std::string_view outputPackagePath,
                                     std::string_view engineDir,
                                     std::string_view projectFile);

}    // namespace bpr::roundtrip
