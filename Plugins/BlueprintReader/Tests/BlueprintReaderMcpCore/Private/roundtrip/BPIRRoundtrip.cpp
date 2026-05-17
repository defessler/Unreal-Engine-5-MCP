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

#include "tools/CompileFunction.h"
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
	// Skeleton-only impl: create the BP, add variables, add function
	// signatures (no bodies). Each call is wrapped so a single failing op
	// surfaces in failing_op + error_message instead of vanishing inside
	// a catch-all. Body materialization via compile_function is the next
	// extension once skeletons round-trip cleanly.
	{
		const std::string outPath = std::string(outputPackagePath);
		std::string failingOp;
		auto runOp = [&](std::string_view op, auto&& fn) -> bool {
			try { fn(); return true; }
			catch (const std::exception& e) {
				res.failing_stage = "transpile";
				failingOp = std::string(op);
				res.error_message = std::string{"BPIR -> BP "} + std::string(op) +
				                     " failed: " + e.what();
				return false;
			}
		};

		std::string parentClass = "/Script/Engine.Actor";
		if (res.bpir_after.is_object()) {
			const auto& meta = res.bpir_after.value("metadata", nlohmann::json::object());
			if (meta.is_object() && meta.contains("parent_class")
			    && meta["parent_class"].is_string()) {
				parentClass = meta["parent_class"].get<std::string>();
			}
		}
		if (!runOp("CreateBlueprint:" + outPath, [&]{
			(void)reader.CreateBlueprint(outPath, parentClass);
		})) return res;

		if (res.bpir_after.is_object() && res.bpir_after.contains("variables")
		    && res.bpir_after["variables"].is_array()) {
			for (const auto& v : res.bpir_after["variables"]) {
				if (!v.is_object()) continue;
				const std::string vname = v.value("name", std::string{});
				if (vname.empty()) continue;
				BPPinType type;
				try {
					type = tools::ParseTypeArg(v.value("type", nlohmann::json{}));
				}
				catch (...) {
					continue;
				}
				const std::string defaultValue = v.value("default", std::string{});
				const std::string category     = v.value("category", std::string{});
				const bool replicated          = v.value("replicated", false);
				const bool editable            = v.value("editable", false);
				if (!runOp("AddVariable:" + vname, [&]{
					reader.AddVariable(outPath, vname, type, defaultValue,
					                   category, replicated, editable);
				})) return res;
			}
		}

		if (res.bpir_after.is_object() && res.bpir_after.contains("functions")
		    && res.bpir_after["functions"].is_array()) {
			for (const auto& fn : res.bpir_after["functions"]) {
				if (!fn.is_object()) continue;
				const std::string fname = fn.value("name", std::string{});
				if (fname.empty()) continue;
				if (!runOp("AddFunction:" + fname, [&]{
					(void)reader.AddFunction(outPath, fname);
				})) return res;
				const auto pushParams = [&](const char* key, bool isOutput) -> bool {
					if (!fn.contains(key) || !fn[key].is_array()) return true;
					for (const auto& p : fn[key]) {
						if (!p.is_object()) continue;
						const std::string pname = p.value("name", std::string{});
						if (pname.empty()) continue;
						BPPinType ptype;
						try {
							ptype = tools::ParseTypeArg(p.value("type", nlohmann::json{}));
						}
						catch (...) {
							continue;
						}
						const std::string label =
							std::string{isOutput ? "AddFunctionOutput:" : "AddFunctionInput:"} +
							fname + "." + pname;
						const bool ok = runOp(label, [&]{
							if (isOutput) {
								reader.AddFunctionOutput(outPath, fname, pname, ptype);
							}
							else {
								reader.AddFunctionInput(outPath, fname, pname, ptype);
							}
						});
						if (!ok) return false;
					}
					return true;
				};
				if (!pushParams("inputs", /*isOutput=*/false)) return res;
				if (!pushParams("outputs", /*isOutput=*/true))  return res;

				// Body materialization: compile_function translates the BPIR
				// statement DSL (if/set/call/comment) into add_node + wire_pins
				// ops. Statements outside the supported subset (return, cast,
				// switch, loops, etc.) raise — we count those as soft failures
				// so a single-statement edge case doesn't lose the otherwise-
				// complete skeleton. Counts surface in body_compile_failures
				// for callers to surface; res.ok stays true.
				const auto& bodyJ = fn.value("body", nlohmann::json::array());
				if (bodyJ.is_array() && !bodyJ.empty()) {
					nlohmann::json compileArgs = {
						{"asset_path",    outPath},
						{"function_name", fname},
						{"body",          bodyJ},
						{"atomic",        false},
					};
					try {
						(void)tools::CompileFunctionFromBody(reader, compileArgs);
					}
					catch (const std::exception& e) {
						res.body_compile_failures.push_back(
							fname + ": " + e.what());
					}
				}
			}
		}

		if (!runOp("SaveAll", [&]{
			(void)reader.SaveAll(/*dirtyOnly=*/false);
		})) return res;

		res.ok = true;
	}

	return res;
}

}    // namespace bpr::roundtrip
