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
#include "Env.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <vector>

#include "tools/CompileFunction.h"
#include "tools/Decompile.h"
#include "tools/TypeShorthand.h"
#include "tools/codegen/CppClassEmit.h"
#include "tools/codegen/CppEmit.h"
#include "tools/parse/CppParse.h"

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
	// drive UBT against the consuming project's <Project>Editor target.
	// Target name comes from BP_READER_EDITOR_TARGET (default "LyraEditor"
	// since the repo ships LyraStarterGame.uproject); override for other
	// projects via the env var.
	// BP_READER_SKIP_PREBUILD=1 stops the editor target from re-invoking
	// the plugin's MCP-server PreBuildStep.
	{
		auto envTargetVal = bpr::env::GetOrDefault("BP_READER_EDITOR_TARGET", "LyraEditor");
		std::string editorTarget = envTargetVal;
		const std::string buildBat =
			std::string(engineDir) + "/Engine/Build/BatchFiles/Build.bat";
		const std::string cmd =
			"set BP_READER_SKIP_PREBUILD=1 && \"" + buildBat +
			"\" " + editorTarget + " Win64 Development "
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

	// Stage 4: re-parse each function's emitted C++ back to BPIR — a real
	// BPIR -> C++ -> BPIR round-trip of the function logic, replacing the
	// earlier pass-through cheat. Class-level metadata + the variable and
	// component lists are carried from bpir_before unchanged (our codegen
	// emits those faithfully — see CppClassEmit + Decompile tests; whole-
	// class HEADER re-parsing remains future work). Per-function fallback:
	// any function that doesn't survive the C++-subset round-trip keeps its
	// original BPIR so Stage 5 can still materialize it — the asymmetry is
	// recorded as a soft note, never fatal. Worst case this degrades to the
	// old pass-through behavior (every function falls back); best case the
	// function bodies are genuinely round-tripped through C++.
	res.bpir_after = res.bpir_before;
	if (res.bpir_after.is_object() && res.bpir_after.contains("functions")
	    && res.bpir_after["functions"].is_array()) {
		for (auto& fn : res.bpir_after["functions"]) {
			if (!fn.is_object()) {
				continue;
			}
			const std::string fname = fn.value("name", std::string{});
			try {
				const tools::CppEmitResult emitted = tools::EmitCppFunction(fn);
				nlohmann::json reparsed = tools::ParseCppFunction(emitted.source);
				if (reparsed.is_object()) {
					fn = std::move(reparsed);
				}
			} catch (const std::exception& e) {
				// Not fatal — a function outside the parser's C++ subset just
				// keeps its decompiled BPIR. Record so the round-trip report
				// shows where fidelity was carried rather than re-derived.
				res.body_compile_failures.push_back(
					std::string{"Stage4 reparse "} + fname + ": " + e.what() +
					" (kept original BPIR)");
			}
		}
	}

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

		// Self-healing: nuke any stale clone left over from a previous
		// run (variable-add isn't idempotent at the plugin layer, so a
		// partial leftover BP would block this run). Soft-failure mode
		// — when the asset doesn't exist, DeleteAsset(-Force) succeeds.
		(void)runOp("DeleteAsset:" + outPath, [&]{
			(void)reader.DeleteAsset(outPath, /*force=*/true);
		});
		// We intentionally ignore the boolean return — DeleteAsset failing
		// shouldn't block the roundtrip (the CreateBlueprint that follows
		// will surface the real error if any).
		res.failing_stage.clear();
		res.failing_op.clear();
		res.error_message.clear();

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

		// Components: SCS subobjects (CameraBoom, Mesh, etc.). Order
		// matters — parent components must exist before children, so
		// we do two passes: roots first (no parent), then a second
		// pass over the remainder where parent already exists. Repeat
		// until no progress, which handles arbitrary depth.
		if (res.bpir_after.is_object() && res.bpir_after.contains("components")
		    && res.bpir_after["components"].is_array()) {
			std::vector<nlohmann::json> pending;
			pending.reserve(res.bpir_after["components"].size());
			for (const auto& c : res.bpir_after["components"]) {
				if (c.is_object() && c.contains("name") && c["name"].is_string()) {
					pending.push_back(c);
				}
			}
			std::set<std::string> created;
			bool progress = true;
			while (progress && !pending.empty()) {
				progress = false;
				for (auto it = pending.begin(); it != pending.end(); ) {
					const std::string cname  = (*it).value("name",   std::string{});
					const std::string ccls   = (*it).value("class",  std::string{});
					const bool        isRoot = (*it).value("is_root", false);
					const std::string parent = isRoot ? std::string{}
					                                  : (*it).value("parent", std::string{});
					// Skip until parent is created (unless root or already
					// detected in source as root via missing parent).
					if (!parent.empty() && !created.count(parent)) {
						++it;
						continue;
					}
					if (cname.empty() || ccls.empty()) {
						it = pending.erase(it);
						continue;
					}
					const bool ok = runOp("AddComponent:" + cname, [&]{
						(void)reader.AddComponent(outPath, cname, ccls,
						                          parent, std::string{});
					});
					if (!ok) return res;
					// Apply property overrides (if any).
					if (it->contains("properties") && (*it)["properties"].is_array()) {
						for (const auto& p : (*it)["properties"]) {
							if (!p.is_object()) continue;
							const std::string pname = p.value("name",  std::string{});
							const std::string pval  = p.value("value", std::string{});
							if (pname.empty()) continue;
							(void)runOp(
								"SetComponentProperty:" + cname + "." + pname,
								[&]{
									(void)reader.SetComponentProperty(
										outPath, cname, pname, pval);
								});
							// Per-property failures are soft — Stage 5 keeps
							// going so missing/renamed properties don't lose
							// the whole component.
						}
					}
					created.insert(cname);
					it = pending.erase(it);
					progress = true;
				}
			}
			for (const auto& leftover : pending) {
				// Components whose parent never appeared — record as a soft
				// failure but don't fail the stage.
				const std::string cname = leftover.value("name", std::string{});
				res.body_compile_failures.push_back(
					"Component:" + cname + ": parent not found in components[]");
			}
		}

		if (res.bpir_after.is_object() && res.bpir_after.contains("functions")
		    && res.bpir_after["functions"].is_array()) {
			// Decompile can emit duplicate function names when the source
			// has multiple unresolved EnhancedInputAction nodes (one
			// synthetic OnUnknownTriggered per node — IA_* asset resolution
			// is lossy). AddFunction is not idempotent at the plugin layer
			// (variable-already-exists style check returns exit=1), so
			// dedupe by name and keep only the first body for each name.
			std::set<std::string> seenFunctionNames;
			for (const auto& fn : res.bpir_after["functions"]) {
				if (!fn.is_object()) continue;
				const std::string fname = fn.value("name", std::string{});
				if (fname.empty()) continue;
				if (!seenFunctionNames.insert(fname).second) {
					// Duplicate name from decompile-synthesized handlers —
					// soft-record so the test reflects what was skipped,
					// then continue (don't halt the whole stage).
					res.body_compile_failures.push_back(
						fname + ": duplicate decompile-synthesized function name, kept first");
					continue;
				}
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
				// statement DSL (if/set/call/comment/return) into add_node +
				// wire_pins ops. Statements outside the supported subset
				// (cast, switch, loops, etc.) raise — we count those as
				// soft failures so a single-statement edge case doesn't lose
				// the otherwise-complete skeleton. Counts surface in
				// body_compile_failures for callers to surface; res.ok stays
				// true.
				//
				// Pass `inputs`/`outputs` so compile_function's Compiler
				// populates outputNames — required for positional return
				// forms (which is how decompile emits single- and multi-
				// output returns). add_function_input/output ops are
				// idempotent so the re-emit is a no-op against the already-
				// declared signature.
				const auto& bodyJ = fn.value("body", nlohmann::json::array());
				if (bodyJ.is_array() && !bodyJ.empty()) {
					nlohmann::json compileArgs = {
						{"asset_path",    outPath},
						{"function_name", fname},
						{"body",          bodyJ},
						{"atomic",        false},
					};
					if (fn.contains("inputs") && fn["inputs"].is_array()) {
						compileArgs["inputs"] = fn["inputs"];
					}
					if (fn.contains("outputs") && fn["outputs"].is_array()) {
						compileArgs["outputs"] = fn["outputs"];
					}
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
