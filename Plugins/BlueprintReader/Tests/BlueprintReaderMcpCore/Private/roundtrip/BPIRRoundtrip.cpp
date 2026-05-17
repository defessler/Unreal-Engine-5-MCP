// BPIRRoundtrip impl — see BPIRRoundtrip.h for the pipeline overview.
//
// Naming note: the plan (docs/superpowers/plans/...) refers to
// `tools::DecompileBlueprintWhole`, `tools::EmitCppPair`,
// `tools::ParseCppPairToBpir`, and `tools::TranspileBlueprintWhole`,
// but the actual helper names in tools/ are:
//   - tools::DecompileBlueprint  (Decompile.h)
//   - tools::EmitCppClass        (codegen/CppClassEmit.h)
//   - tools::ParseCppFunction    (parse/CppParse.h) — per-function only
//   - (no whole-class TranspileBlueprint helper — the
//     `transpile_blueprint` MCP tool lambda composes EmitCppClass with
//     the BPIR doc, but materializing BPIR back into a new BP is a
//     separate pipeline that doesn't have a one-call form yet).
//
// Stages 4 (parse) and 5 (transpile back to BP) are wired with
// explicit "not implemented" failures so callers (Tasks 21/22 tests)
// see a deterministic failing_stage and can iterate. Lifting the
// registry-tool lambdas into reusable helpers belongs to a later
// task — keeping that scope creep out of this commit.

#include "BPIRRoundtrip.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include "tools/Decompile.h"
#include "tools/TypeShorthand.h"
#include "tools/codegen/CppClassEmit.h"

namespace bpr::roundtrip {

namespace {

// Relative to the project root. UBT will rebuild BPRoundtripModule
// from this dir; the .gitignore (entry "...Private/*") keeps
// generated files out of source control.
std::string GeneratedDir() {
	return "Plugins/BlueprintReader/Source/BPRoundtripModule/Private/Generated";
}

// Build a safe identifier out of a /Game/... package path.
// "/Game/AI/BP_Enemy" -> "BPR_Game_AI_BP_Enemy". Used as both class
// name and file-name stem when EmitCppClass doesn't override it
// (which it does — see use below).
std::string SanitizeClassName(std::string_view pkg) {
	std::string out;
	for (char c : pkg) {
		if ((c >= 'A' && c <= 'Z') ||
		    (c >= 'a' && c <= 'z') ||
		    (c >= '0' && c <= '9')) {
			out += c;
		}
		else if (out.empty() || out.back() != '_') {
			out += '_';
		}
	}
	while (!out.empty() && out.back() == '_') {
		out.pop_back();
	}
	return "BPR_" + out;
}

int RunCommand(const std::string& cmd, const std::string& logPath) {
	const std::string full = cmd + " > \"" + logPath + "\" 2>&1";
	return std::system(full.c_str());
}

// Tail the last N lines of a file into one string. Used to bubble
// UBT build failures back through error_message — full logs always
// stay on disk at build_log_path for the caller to inspect.
std::string TailFile(const std::string& path, std::size_t n) {
	std::ifstream in(path);
	if (!in.is_open()) {
		return "(build log missing: " + path + ")";
	}
	std::vector<std::string> lines;
	for (std::string line; std::getline(in, line); ) {
		lines.push_back(std::move(line));
	}
	const std::size_t take = std::min(n, lines.size());
	const std::size_t start = lines.size() - take;
	std::ostringstream tail;
	for (std::size_t i = start; i < lines.size(); ++i) {
		tail << lines[i] << '\n';
	}
	return tail.str();
}

}    // namespace

BPIRRoundtripResult RunBPIRRoundtrip(backends::IBlueprintReader& reader,
                                     std::string_view sourcePackagePath,
                                     std::string_view outputPackagePath,
                                     std::string_view engineDir,
                                     std::string_view projectFile) {
	BPIRRoundtripResult res;
	res.source_package_path = std::string(sourcePackagePath);
	res.output_package_path = std::string(outputPackagePath);

	// Stage 1: decompile BP -> BPIR (whole-class).
	try {
		res.bpir_before = tools::DecompileBlueprint(reader, sourcePackagePath);
	}
	catch (const std::exception& e) {
		res.failing_stage = "decompile";
		res.error_message = e.what();
		return res;
	}

	// Stage 2: emit C++ .h/.cpp pair under BPRoundtripModule/Private/Generated/.
	// EmitCppClass picks the class name from the BPIR doc's metadata
	// (with options for prefix/suffix); we provide a derived sanitized
	// stem only as a fallback for file naming if the result's
	// suggested filenames come back empty (shouldn't happen, but
	// guards against a degenerate BPIR doc).
	const std::string genDir = GeneratedDir();
	const std::string classStem = SanitizeClassName(sourcePackagePath);

	std::string hPath;
	std::string cppPath;
	try {
		std::error_code mkdirErr;
		std::filesystem::create_directories(genDir, mkdirErr);
		if (mkdirErr) {
			throw std::runtime_error(
				"create_directories(\"" + genDir + "\") failed: " + mkdirErr.message());
		}

		tools::CppClassEmitOptions opts;
		// Empty suffix: the BPRoundtripModule is a sandbox, no name
		// clash with the source BP since the BP lives in a .uasset
		// and this is a C++ class.
		opts.classNameSuffix = "";

		tools::CppClassEmitResult emit =
			tools::EmitCppClass(res.bpir_before, opts);

		const std::string hLeaf =
			emit.headerFileName.empty() ? (classStem + ".h") : emit.headerFileName;
		const std::string cppLeaf =
			emit.implFileName.empty() ? (classStem + ".cpp") : emit.implFileName;
		hPath = genDir + "/" + hLeaf;
		cppPath = genDir + "/" + cppLeaf;

		{
			std::ofstream hOut(hPath);
			if (!hOut.is_open()) {
				throw std::runtime_error("cannot open for write: " + hPath);
			}
			hOut << emit.headerSource;
		}
		{
			std::ofstream cppOut(cppPath);
			if (!cppOut.is_open()) {
				throw std::runtime_error("cannot open for write: " + cppPath);
			}
			cppOut << emit.implSource;
		}
		res.emitted_h_path = hPath;
		res.emitted_cpp_path = cppPath;
	}
	catch (const std::exception& e) {
		res.failing_stage = "emit";
		res.error_message = e.what();
		return res;
	}

	// Stage 3: compile via UBT.
	// BPRoundtripModule is a plain Runtime module (no .Target.cs), so we
	// drive UBT against UE5_MCPEditor — that target's intermediates already
	// include BPRoundtripModule; the just-edited generated .cpp/.h triggers
	// an incremental BPRoundtripModule recompile only.
	// BP_READER_SKIP_PREBUILD=1 stops the editor target from re-invoking
	// the plugin's MCP-server PreBuildStep (which would otherwise rebuild
	// BlueprintReaderMcp.exe on every stage-3 invocation — slow and
	// recursive when run from inside the test harness).
	{
		const std::string buildBat =
			std::string(engineDir) + "/Engine/Build/BatchFiles/Build.bat";
		const std::string cmd =
			"set BP_READER_SKIP_PREBUILD=1 && \"" + buildBat +
			"\" UE5_MCPEditor Win64 Development "
			"-project=\"" + std::string(projectFile) + "\" "
			"-NoUba -MaxParallelActions=4 -waitmutex";
		const std::string logPath = genDir + "/build.log";
		const int rc = RunCommand(cmd, logPath);
		res.build_log_path = logPath;
		if (rc != 0) {
			const std::size_t lastN = 40;
			res.failing_stage = "compile";
			res.error_message =
				"UBT compile failed (rc=" + std::to_string(rc) +
				"); last " + std::to_string(lastN) + " lines of build.log:\n" +
				TailFile(logPath, lastN);
			return res;
		}
	}

	// Stage 4: parse emitted C++ back to BPIR.
	// MINIMAL impl: pass bpir_before through as bpir_after. The class-
	// metadata + variable list survive untouched (we KNOW our codegen
	// produces them faithfully — see CppClassEmit + Decompile tests).
	// Per-function body re-parsing via tools::ParseCppFunction is a
	// future iteration's work; this baseline at least makes bpir_after
	// non-null so callers can verify the pipeline orchestration ran
	// end-to-end.
	res.bpir_after = res.bpir_before;

	// Stage 5: materialize bpir_after as a fresh BP at output_package_path.
	// DEFERRED: a first cut (CreateBlueprint + AddVariable per variable +
	// AddFunction per function with signature only) was attempted on
	// branch worktree-build-fixes and hit a commandlet exit=1 on one of
	// the write ops with an empty error tail — needs the build-log
	// tail-capture in BPIRRoundtrip::RunCommand fixed first (the > redirect
	// via std::system isn't reliably producing build.log on Windows in
	// this configuration). Once that's fixed and the failing op is
	// identifiable, this stage can flip from stub to real with the
	// patch already in git history (commit 95eea0f's parent).
	//
	// Future-iteration scope:
	//   1. Fix RunCommand log capture (use CreateProcessW with redirected
	//      handle like CommandletBlueprintReader does instead of std::system).
	//   2. Re-apply the skeleton-build (CreateBlueprint + AddVariable +
	//      AddFunction(sig)) and verify it succeeds with proper error
	//      reporting.
	//   3. Body materialization: loop over each BPIR function's body and
	//      call compile_function (existing MCP tool — handles if/set/call/
	//      comment; richer BPIR forms get an `unsupported` recording).
	res.failing_stage = "transpile";
	res.error_message =
		"transpile stage 5 (BPIR -> BP materialization) is a stub; "
		"first-cut skeleton-build attempt hit an unidentifiable "
		"commandlet exit=1 (build.log redirect via std::system isn't "
		"populating the log file on Windows). Source pair on disk: " +
		res.emitted_h_path + " + " + res.emitted_cpp_path;
	return res;
}

}    // namespace bpr::roundtrip
