#include "tools/BlueprintTools.h"
#include "tools/ApplyOps.h"
#include "tools/Bpir.h"
#include "tools/CompileFunction.h"
#include "tools/Decompile.h"
#include "tools/JsonProjection.h"
#include "tools/TypeShorthand.h"
#include "tools/codegen/CppClassEmit.h"
#include "tools/codegen/CppEmit.h"
#include "tools/codegen/UnsupportedTreatment.h"
#include "tools/parse/CppParse.h"

#include "backends/IBlueprintReader.h"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <fmt/core.h>
#include <nlohmann/json.hpp>

namespace bpr::tools {

namespace {

// Returns by value (copy) instead of const-ref into the json. The ref form
// was safe today — the caller's lambda holds the json alive for the
// duration — but it was a footgun for any future caller binding the result
// to something with longer lifetime. Sub-microsecond extra cost; not on a
// hot path that anyone cares about.
std::string RequireString(const nlohmann::json& obj, std::string_view key) {
	auto it = obj.find(key);
	if (it == obj.end() || !it->is_string()) {
		throw std::invalid_argument(fmt::format(R"(missing or non-string argument "{}")", key));
	}
	return it->get<std::string>();
}

std::string OptString(const nlohmann::json& obj, std::string_view key,
					  std::string fallback) {
	auto it = obj.find(key);
	if (it == obj.end() || it->is_null())
	{
		return fallback;
	}
	if (!it->is_string()) {
		throw std::invalid_argument(fmt::format(R"(argument "{}" must be a string)", key));
	}
	return it->get<std::string>();
}

nlohmann::json AssetPathSchema() {
	return nlohmann::json{
		{"type", "object"},
		{"properties", {
			{"asset_path", {
				{"type", "string"},
				{"description", "UE asset path, e.g. /Game/AI/BP_Enemy"},
			}},
		}},
		{"required", nlohmann::json::array({"asset_path"})},
	};
}

// Returns an int arg or fallback. Negative values raise.
int OptInt(const nlohmann::json& obj, std::string_view key, int fallback) {
	auto it = obj.find(key);
	if (it == obj.end() || it->is_null())
	{
		return fallback;
	}
	if (!it->is_number_integer()) {
		throw std::invalid_argument(fmt::format(R"(argument "{}" must be an integer)", key));
	}
	return it->get<int>();
}

// Property fragments shared across tool input schemas. Composed into the
// `properties` block by ApplyResponseControls below.
nlohmann::json FieldsProperty() {
	return {
		{"type", "array"},
		{"items", {{"type", "string"}}},
		{"description",
		 "Optional response projection. Each entry is a dotted field path; "
		 "use `[]` to apply the path to every element of an array. Example: "
		 "[\"name\", \"variables[].name\"] returns just the BP name and the "
		 "names of its variables. Omit to get the full payload."},
	};
}
nlohmann::json LimitProperty() {
	return {
		{"type", "integer"},
		{"description", "Optional cap on the number of items returned (after offset). "
						"Use to keep responses small on big projects."},
	};
}
nlohmann::json OffsetProperty() {
	return {
		{"type", "integer"},
		{"description", "Optional 0-based offset into the result array. Pair with "
						"`limit` for paging through long lists."},
	};
}

// Mutate `body` to apply offset/limit (when body is an array) and
// field projection. Convenience helper called from every read-tool handler.
struct ResponseControls {
	int offset = 0;
	int limit  = -1;  // -1 => no cap
	std::vector<std::string> fields;
};
ResponseControls ParseResponseControls(const nlohmann::json& args) {
	ResponseControls ctl;
	ctl.offset = OptInt(args, "offset", 0);
	ctl.limit  = OptInt(args, "limit", -1);
	ctl.fields = ParseFieldsArg(args);
	if (ctl.offset < 0)
	{
		throw std::invalid_argument(R"(argument "offset" must be >= 0)");
	}
	if (ctl.limit < -1)
	{
		throw std::invalid_argument(R"(argument "limit" must be >= 0)");
	}
	return ctl;
}
void ApplyResponseControls(nlohmann::json& body, const ResponseControls& ctl) {
	if (body.is_array() && (ctl.offset > 0 || ctl.limit >= 0)) {
		std::size_t off = std::min<std::size_t>(ctl.offset, body.size());
		std::size_t end = (ctl.limit < 0)
							  ? body.size()
							  : std::min<std::size_t>(off + ctl.limit, body.size());
		nlohmann::json sliced = nlohmann::json::array();
		for (std::size_t i = off; i < end; ++i)
		{
			sliced.push_back(std::move(body[i]));
		}
		body = std::move(sliced);
	}
	ApplyProjection(body, ctl.fields);
}

} // namespace

void RegisterBlueprintTools(ToolRegistry& registry, backends::IBlueprintReader& reader) {
	// ----- list_blueprints -------------------------------------------------
	{
		ToolDescriptor d;
		d.name = "list_blueprints";
		d.description =
			"[blueprint] List Blueprint assets under a content path. Defaults to /Game. "
			"On big projects this can return thousands of entries — use "
			"`limit`/`offset` to page, and `fields` (e.g. [\"asset_path\"]) "
			"to drop columns you don't need.";
		d.input_schema = {
			{"type", "object"},
			{"properties", {
				{"path", {
					{"type", "string"},
					{"description", "Content path filter, e.g. /Game/AI. Defaults to /Game."},
				}},
				{"limit",  LimitProperty()},
				{"offset", OffsetProperty()},
				{"fields", FieldsProperty()},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string path = OptString(args, "path", "/Game");
			auto ctl = ParseResponseControls(args);
			auto items = reader.ListBlueprints(path);
			nlohmann::json body = items;
			ApplyResponseControls(body, ctl);
			return body;
		});
	}

	// ----- read_blueprint --------------------------------------------------
	{
		ToolDescriptor d;
		d.name = "read_blueprint";
		d.description =
			"[blueprint] Read top-level metadata for a Blueprint: parent class, interfaces, "
			"variables, function/graph summaries, macros. "
			"Pass `fields` (e.g. [\"parent_class\", \"variables[].name\"]) to "
			"project just what you need — full payloads can be many KB on busy BPs.";
		d.input_schema = {
			{"type", "object"},
			{"properties", {
				{"asset_path", {{"type", "string"},
								{"description", "UE asset path, e.g. /Game/AI/BP_Enemy"}}},
				{"fields",     FieldsProperty()},
			}},
			{"required", nlohmann::json::array({"asset_path"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireString(args, "asset_path");
			auto ctl = ParseResponseControls(args);
			nlohmann::json body = reader.ReadBlueprint(asset);
			ApplyResponseControls(body, ctl);
			return body;
		});
	}

	// ----- decompile_function (BP graph → BPIR) ---------------------------
	// Walk a BP function's graph and reconstruct a structured BPIR AST
	// (see wiki/BPIR.md). Pure server-side; reuses get_function's data.
	{
		ToolDescriptor d;
		d.name = "decompile_function";
		d.description =
			"[cpp] Convert a BP function to BPIR (Blueprint Intermediate "
			"Representation) — a versioned JSON AST that's the pivot for "
			"BP ↔ source-language conversions. Pattern-matches K2 nodes "
			"(Branch / Cast / Sequence / VariableSet / CallFunction / "
			"FunctionResult) into structured statements + expressions. "
			"Anything unrecognized appears as `{unsupported: {...}}` in "
			"the body, AND in a top-level `unsupported_nodes` summary "
			"for quick \"what couldn't I represent?\" inspection.\n\n"
			"Pair with `transpile_function` (BPIR → C++) for the full "
			"BP→source pipeline. The BPIR returned here is also valid "
			"input for `compile_function` (existing tool) — round-trip "
			"BP → BPIR → BP works for the patterns BPIR covers cleanly.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path",    {{"type","string"}}},
				{"function_name", {{"type","string"}}},
				{"fields",        FieldsProperty()},
			}},
			{"required", nlohmann::json::array({"asset_path","function_name"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireString(args, "asset_path");
			std::string fname = RequireString(args, "function_name");
			auto ctl = ParseResponseControls(args);
			nlohmann::json body = DecompileFunction(reader, asset, fname);
			ApplyResponseControls(body, ctl);
			return body;
		});
	}

	// ----- decompile_blueprint --------------------------------------------
	{
		ToolDescriptor d;
		d.name = "decompile_blueprint";
		d.description =
			"[cpp] Whole-class BPIR extraction: variables + interfaces + every "
			"function's BPIR. Returns `{kind: \"class\", ...}` doc — the "
			"input shape `transpile_blueprint` expects for full UCLASS "
			"C++ generation. Per-function decompile failures don't tank "
			"the whole call; failed functions appear with `<decompile-"
			"failure>` markers in their unsupported_nodes.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path", {{"type","string"}}},
				{"fields",     FieldsProperty()},
			}},
			{"required", nlohmann::json::array({"asset_path"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireString(args, "asset_path");
			auto ctl = ParseResponseControls(args);
			nlohmann::json body = DecompileBlueprint(reader, asset);
			ApplyResponseControls(body, ctl);
			return body;
		});
	}

	// ----- transpile_function (BPIR → C++) --------------------------------
	{
		ToolDescriptor d;
		d.name = "transpile_function";
		d.description =
			"[cpp] Convert a Blueprint function to C++ source. Composes "
			"decompile_function (BP → BPIR) + C++ codegen (BPIR → "
			"source). Default `mode=readable` emits annotated C++ "
			"(type names + valid blocks, UCLASS/UFUNCTION shown as "
			"comments); for compilable .h/.cpp pairs, use "
			"`transpile_blueprint`. Unsupported nodes appear as "
			"`// TODO[bpr-unsupported]` comments + a `notes` array "
			"the agent can iterate over.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path",    {{"type","string"}}},
				{"function_name", {{"type","string"}}},
				{"target_lang",   {{"type","string"},
								   {"enum", nlohmann::json::array({"cpp"})},
								   {"description","Target language. Currently C++ only; the BPIR pivot is designed to extend to Lua / Python / JS later."}}},
				{"mode",          {{"type","string"},
								   {"enum", nlohmann::json::array({"readable","compilable"})},
								   {"description","\"readable\" (default) emits annotated C++ for review; \"compilable\" emits drop-in .h/.cpp pairs (use transpile_blueprint for whole-class output)."}}},
				{"use_operator_aliases", {{"type","boolean"},
										  {"description","Render +, ==, && etc. instead of UKismetMathLibrary calls. Default true."}}},
			}},
			{"required", nlohmann::json::array({"asset_path","function_name"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireString(args, "asset_path");
			std::string fname = RequireString(args, "function_name");
			std::string lang  = OptString(args, "target_lang", "cpp");
			std::string mode  = OptString(args, "mode", "readable");
			if (lang != "cpp") {
				throw std::invalid_argument(fmt::format(
					"transpile_function: target_lang=\"{}\" not yet supported; only \"cpp\" is implemented today.",
					lang));
			}
			CppEmitOptions opts;
			opts.mode = (mode == "compilable") ? CppEmitOptions::Mode::Compilable
												: CppEmitOptions::Mode::Readable;
			opts.useOperatorAliases = args.value("use_operator_aliases", true);

			// BP → BPIR → C++.
			nlohmann::json bpir = DecompileFunction(reader, asset, fname);
			CppEmitResult result = EmitCppFunction(bpir, opts);
			return nlohmann::json{
				{"ok", true},
				{"asset_path", asset},
				{"function_name", fname},
				{"target_lang", lang},
				{"mode", mode},
				{"source", result.source},
				{"notes", result.notes},
				{"unsupported_count", result.notes.size()},
			};
		});
	}

	// ----- transpile_blueprint (whole-class .h/.cpp) ----------------------
	{
		ToolDescriptor d;
		d.name = "transpile_blueprint";
		d.description =
			"[cpp] Convert a whole Blueprint class to a compilable UE C++ .h/.cpp pair. "
			"Composes decompile_blueprint + CppClassEmit: emits a UCLASS "
			"declaration with UPROPERTY decls (Replicated / EditAnywhere "
			"/ Category specifiers inferred from BP variable metadata), "
			"UFUNCTION decls (BlueprintCallable + Category from BP "
			"function metadata), function bodies, and "
			"GetLifetimeReplicatedProps() registration when any variable "
			"is Replicated.\n\n"
			"Class name follows UE convention: a BP named \"BP_Enemy\" "
			"with parent ACharacter becomes \"ABP_Enemy_Generated\". "
			"The suffix is configurable via class_name_suffix (default "
			"\"_Generated\"); pass \"\" if you want the class to drop in "
			"place of the BP entirely.\n\n"
			"Returns header + impl source strings, suggested filenames, "
			"and a `notes` array listing every unsupported BP construct "
			"encountered (timelines, latent actions, etc.) for the agent "
			"to triage.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path", {{"type","string"}}},
				{"target_lang", {{"type","string"},
								 {"enum", nlohmann::json::array({"cpp"})}}},
				{"module_api_macro", {{"type","string"},
									  {"description","E.g. \"MYGAME_API\". Empty for bare class decl."}}},
				{"class_name_suffix", {{"type","string"},
									   {"description","Default \"_Generated\". Empty drops in place of the BP."}}},
				{"class_name_prefix", {{"type","string"},
									   {"description","Inserted between UE's type letter (A/U/I) and the BP's CamelCased base name. Empty preserves legacy `ABP_Enemy` form."}}},
				{"category_default", {{"type","string"},
									  {"description","Fallback UPROPERTY Category when the BP variable doesn't carry one. Empty -> no Category specifier emitted."}}},
				{"category_remap", {{"type","object"},
									{"description","BP category -> output category map. Applied before category_default. Useful for normalizing common BP categories like \"Default\" to a project's house category."},
									{"additionalProperties", {{"type","string"}}}}},
				{"uclass_meta", {{"type","object"},
								 {"description","Extra UCLASS() meta=(K=V, ...) entries. E.g. {\"PrioritizeCategories\":\"MyGame\"} folds into the UCLASS macro."},
								 {"additionalProperties", {{"type","string"}}}}},
				{"delegate_typedef_pattern", {{"type","string"},
											  {"description","Pattern for derived multicast-delegate typedefs. `{Name}` is the variable name. Default `F{Name}` -> `FOnReady` for var `OnReady`. Use `F{Name}Delegate` for the `FOnReadyDelegate` house style."}}},
				{"use_operator_aliases", {{"type","boolean"}}},
			}},
			{"required", nlohmann::json::array({"asset_path"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireString(args, "asset_path");
			std::string lang  = OptString(args, "target_lang", "cpp");
			if (lang != "cpp") {
				throw std::invalid_argument(fmt::format(
					"transpile_blueprint: target_lang=\"{}\" not yet supported; only \"cpp\" today.", lang));
			}
			CppClassEmitOptions opts;
			opts.moduleApiMacro    = OptString(args, "module_api_macro", "");
			if (auto it = args.find("class_name_suffix"); it != args.end() && it->is_string()) {
				opts.classNameSuffix = it->get<std::string>();
			}
			opts.classNamePrefix   = OptString(args, "class_name_prefix",   "");
			opts.categoryDefault   = OptString(args, "category_default",    "");
			opts.delegateTypedefPattern = OptString(args, "delegate_typedef_pattern", "F{Name}");
			// Object-shaped args -> std::map<string,string>. Skip silently
			// if the caller passed a non-object or a non-string value;
			// we don't want a typo to fail the whole transpile.
			auto loadStringMap = [&](const char* key, std::map<std::string, std::string>& out) {
				auto it = args.find(key);
				if (it == args.end() || !it->is_object())
				{
					return;
				}
				for (auto& [k, v] : it->items()) {
					if (v.is_string())
					{
						out[k] = v.get<std::string>();
					}
				}
			};
			loadStringMap("category_remap", opts.categoryRemap);
			loadStringMap("uclass_meta",    opts.uclassMeta);
			opts.emitOpts.useOperatorAliases = args.value("use_operator_aliases", true);

			nlohmann::json bpir = DecompileBlueprint(reader, asset);
			CppClassEmitResult result = EmitCppClass(bpir, opts);
			// Build the sidecar JSON describing every unsupported /
			// approximation node + manual steps. Caller writes this
			// alongside the .h/.cpp as <Class>.transpile-notes.json so
			// the agent can iterate over what's left to port.
			std::vector<std::string> filenames = {
				result.headerFileName, result.implFileName,
			};
			nlohmann::json sidecar = BuildSidecar(asset, filenames, result.notes);
			// Sidecar filename: strip the .h extension and add a
			// distinctive suffix so it sits next to the .cpp/.h pair.
			std::string sidecarFile = result.headerFileName;
			if (sidecarFile.size() > 2 &&
				sidecarFile.substr(sidecarFile.size() - 2) == ".h") {
				sidecarFile.resize(sidecarFile.size() - 2);
			}
			sidecarFile += ".transpile-notes.json";
			return nlohmann::json{
				{"ok", true},
				{"asset_path", asset},
				{"target_lang", lang},
				{"class_name", result.className},
				{"header_file", result.headerFileName},
				{"impl_file",   result.implFileName},
				{"header_source", result.headerSource},
				{"impl_source",   result.implSource},
				{"notes", result.notes},
				{"sidecar", sidecar},
				{"sidecar_file", sidecarFile},
				{"unsupported_count", result.notes.size()},
			};
		});
	}

	// ----- write_generated_source -----------------------------------------
	// Write a transpiled source file (.h or .cpp) into the project's
	// Source/ tree. Path-validated by the plugin (must start with
	// <ProjectDir>/Source/) — no path-traversal escape. Use after
	// transpile_blueprint to drop the generated UCLASS pair onto disk
	// so UBT can compile it.
	{
		ToolDescriptor d;
		d.name = "write_generated_source";
		d.description =
			"[cpp] Write a transpiled .h/.cpp file into the project's Source/ "
			"tree. Confined by the plugin to paths under "
			"<ProjectDir>/Source/ — anything else is rejected. Pair with "
			"`transpile_blueprint`: pass the `header_source` / "
			"`impl_source` strings the transpile returned, plus the "
			"destination paths under your game module's Source dir.\n\n"
			"After all files are written, run UBT (or use the editor's "
			"Live Coding) to compile the new class — the BP can then "
			"reparent to the C++ class for hybrid workflows.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"path",        {{"type","string"},
								 {"description","Absolute destination path under <ProjectDir>/Source/."}}},
				{"content",     {{"type","string"},
								 {"description","File content — typically transpile_blueprint's header_source or impl_source."}}},
				{"create_dirs", {{"type","boolean"},
								 {"description","Create parent directories if missing. Default true."}}},
			}},
			{"required", nlohmann::json::array({"path","content"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string path    = RequireString(args, "path");
			std::string content = RequireString(args, "content");
			bool createDirs     = args.value("create_dirs", true);
			auto r = reader.WriteGeneratedSource(path, content, createDirs);
			return nlohmann::json{
				{"ok", true},
				{"path", r.path},
				{"bytes_written", r.bytesWritten},
			};
		});
	}

	// ----- parse_cpp_function (C++ → BPIR) --------------------------------
	// Closes the BP↔C++ loop: source language → BPIR → BP graph (via
	// compile_function). Pairs with transpile_function (BPIR → C++) for a
	// round-trip identity on the patterns CppEmit produces. Accepts both
	// a full function definition (returnType name(args) { body }) and a
	// bare body block — when bare, the caller passes the signature
	// out-of-band as a partially-built BPIR doc.
	//
	// The supported subset (if/else, for(auto&...), while, switch, cast,
	// arithmetic + comparison + logical operators, member/index access,
	// Cast<T>(), this) is enough to round-trip what compile_function /
	// CppEmit emit, plus reasonable hand-written extensions. Out of
	// scope: preprocessor, templates beyond Cast<T>, lambdas, decltype,
	// exception machinery, pointer arithmetic. Anything outside the
	// subset throws CppParseError with a `<line>:<col>: <message>`
	// pointer into the source.
	{
		ToolDescriptor d;
		d.name = "parse_cpp_function";
		d.description =
			"[cpp] Parse a C++ function (or bare body) into a BPIR document — "
			"the inverse of `transpile_function`. The returned BPIR can "
			"be fed straight to `compile_function` to materialize a BP "
			"graph from the source.\n\n"
			"Two input forms:\n"
			"  1. Full definition: `bool TakeDamage(float Damage) { ... }`. "
			"Signature is parsed for inputs/outputs/return type.\n"
			"  2. Bare body: `{ ... }` or `<stmts>`. Pass `signature` "
			"(a BPIR function shell with name/inputs/outputs/locals "
			"already populated) so the parser knows how to scope "
			"variables.\n\n"
			"Subset accepted: if/else, range-based for, while, switch + "
			"case + default, return, break, continue, expression "
			"statements (assignment / call / compound-assign), local "
			"declarations, plus expressions over identifiers / qualified "
			"names / literals / function calls / member access (./->) / "
			"array index / Cast<T>() / this / unary (!, -) / binary "
			"(arithmetic / comparison / logical / assign).\n\n"
			"Errors are reported as `<line>:<col>: <message>`; the "
			"produced BPIR is validated against the schema before "
			"return — a malformed parse surfaces as a clear error rather "
			"than corrupt downstream graphs.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"source",    {{"type","string"},
							   {"description","C++ source: full function definition or bare body."}}},
				{"signature", {{"type","object"},
							   {"description","Optional BPIR function-doc shell (name/inputs/outputs/locals) when `source` is a bare body."}}},
			}},
			{"required", nlohmann::json::array({"source"})},
		};
		registry.Add(std::move(d), [](const nlohmann::json& args) {
			std::string source = RequireString(args, "source");
			nlohmann::json bpir;
			if (auto sigIt = args.find("signature");
				sigIt != args.end() && sigIt->is_object()) {
				bpir = ParseCppFunction(source, *sigIt);
			} else {
				bpir = ParseCppFunction(source);
			}
			return nlohmann::json{
				{"ok", true},
				{"bpir", bpir},
			};
		});
	}

	// ----- summarize_blueprint --------------------------------------------
	{
		ToolDescriptor d;
		d.name = "summarize_blueprint";
		d.description =
			"[blueprint] Tiny orientation response for a Blueprint: parent class plus "
			"counts of variables, functions, graphs, macros, and interfaces. "
			"Use this BEFORE `read_blueprint` when you don't yet know how big "
			"the BP is — saves loading the full payload to find out it has "
			"hundreds of variables.";
		d.input_schema = AssetPathSchema();
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireString(args, "asset_path");
			nlohmann::json full = reader.ReadBlueprint(asset);
			auto countOf = [&](const char* key) -> int {
				auto it = full.find(key);
				return (it != full.end() && it->is_array())
						   ? static_cast<int>(it->size())
						   : 0;
			};
			nlohmann::json out = {
				{"name",            full.value("name", asset)},
				{"asset_path",      asset},
				{"parent_class",    full.value("parent_class", "")},
				{"variable_count",  countOf("variables")},
				{"function_count",  countOf("functions")},
				{"graph_count",     countOf("graphs")},
				{"macro_count",     countOf("macros")},
				{"interface_count", countOf("interfaces")},
			};
			return out;
		});
	}

	// ----- get_graph -------------------------------------------------------
	{
		ToolDescriptor d;
		d.name = "get_graph";
		d.description =
			"[blueprint] Fetch a Blueprint graph (nodes + connections) by name. Defaults to EventGraph. "
			"Big graphs are big — pass `fields` (e.g. [\"nodes[].title\", \"nodes[].kind\"]) "
			"to drop fields you don't need.";
		d.input_schema = {
			{"type", "object"},
			{"properties", {
				{"asset_path", {{"type", "string"},
								{"description", "UE asset path, e.g. /Game/AI/BP_Enemy"}}},
				{"graph_name", {{"type", "string"},
								{"description", "Graph name. Defaults to \"EventGraph\"."}}},
				{"fields", FieldsProperty()},
			}},
			{"required", nlohmann::json::array({"asset_path"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireString(args, "asset_path");
			std::string graph = OptString(args, "graph_name", "EventGraph");
			auto ctl = ParseResponseControls(args);
			nlohmann::json body = reader.GetGraph(asset, graph);
			ApplyResponseControls(body, ctl);
			return body;
		});
	}

	// ----- get_function ----------------------------------------------------
	{
		ToolDescriptor d;
		d.name = "get_function";
		d.description =
			"[blueprint] Fetch a Blueprint function: signature (inputs/outputs), locals, "
			"and body graph. Use `fields` to project (e.g. "
			"[\"inputs[].name\", \"outputs[].name\"] for just the signature).";
		d.input_schema = {
			{"type", "object"},
			{"properties", {
				{"asset_path", {{"type", "string"},
								{"description", "UE asset path, e.g. /Game/AI/BP_Enemy"}}},
				{"function_name", {{"type", "string"},
								   {"description", "Function name as it appears in the blueprint."}}},
				{"fields", FieldsProperty()},
			}},
			{"required", nlohmann::json::array({"asset_path", "function_name"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireString(args, "asset_path");
			std::string fn = RequireString(args, "function_name");
			auto ctl = ParseResponseControls(args);
			nlohmann::json body = reader.GetFunction(asset, fn);
			ApplyResponseControls(body, ctl);
			return body;
		});
	}

	// ----- get_components --------------------------------------------------
	{
		ToolDescriptor d;
		d.name = "get_components";
		d.description =
			"[blueprint] List the SCS components (StaticMeshComponent, LightComponent, "
			"child actors, etc.) attached to a blueprint, with parent/child "
			"hierarchy. Each entry: {name, class, parent, is_root}. Supports "
			"`fields`/`limit`/`offset`.";
		d.input_schema = {
			{"type", "object"},
			{"properties", {
				{"asset_path", {{"type", "string"},
								{"description", "UE asset path, e.g. /Game/AI/BP_Enemy"}}},
				{"limit",  LimitProperty()},
				{"offset", OffsetProperty()},
				{"fields", FieldsProperty()},
			}},
			{"required", nlohmann::json::array({"asset_path"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireString(args, "asset_path");
			auto ctl = ParseResponseControls(args);
			nlohmann::json body = reader.GetComponents(asset);
			ApplyResponseControls(body, ctl);
			return body;
		});
	}

	// ----- list_variables --------------------------------------------------
	{
		ToolDescriptor d;
		d.name = "list_variables";
		d.description =
			"[blueprint] List all member variables on a Blueprint, with type, default, "
			"category, and replication state. Big BPs can have 100+ variables "
			"— use `fields` (e.g. [\"name\", \"type.category\"]) and "
			"`limit`/`offset` to keep responses small.";
		d.input_schema = {
			{"type", "object"},
			{"properties", {
				{"asset_path", {{"type", "string"},
								{"description", "UE asset path, e.g. /Game/AI/BP_Enemy"}}},
				{"limit",  LimitProperty()},
				{"offset", OffsetProperty()},
				{"fields", FieldsProperty()},
			}},
			{"required", nlohmann::json::array({"asset_path"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireString(args, "asset_path");
			auto ctl = ParseResponseControls(args);
			nlohmann::json body = reader.ListVariables(asset);
			ApplyResponseControls(body, ctl);
			return body;
		});
	}

	// ----- find_node -------------------------------------------------------
	{
		ToolDescriptor d;
		d.name = "find_node";
		d.description =
			"[blueprint] Search nodes within a Blueprint by class or title (case-insensitive substring). "
			"Optional `kind` further filters by the K2 extras kind, e.g. CallFunction, "
			"VariableGet, Event, CustomEvent, DynamicCast, MacroInstance, "
			"FunctionEntry, FunctionResult. Supports `fields`/`limit`/`offset` — typical "
			"use is `fields=[\"id\", \"title\", \"kind\"]` to keep responses small.";
		d.input_schema = {
			{"type", "object"},
			{"properties", {
				{"asset_path", {{"type", "string"},
								{"description", "UE asset path, e.g. /Game/AI/BP_Enemy"}}},
				{"query", {{"type", "string"},
						   {"description", "Substring matched against node class or title. "
										   "Pass an empty string to match any node when filtering by `kind` only."}}},
				{"kind",   {{"type", "string"},
							{"description", "Optional. K2 extras `kind` to match exactly (case-insensitive)."}}},
				{"limit",  LimitProperty()},
				{"offset", OffsetProperty()},
				{"fields", FieldsProperty()},
			}},
			{"required", nlohmann::json::array({"asset_path", "query"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireString(args, "asset_path");
			std::string q = RequireString(args, "query");
			std::string kind = OptString(args, "kind", "");
			auto ctl = ParseResponseControls(args);
			nlohmann::json body = reader.FindNode(asset, q, kind);
			ApplyResponseControls(body, ctl);
			return body;
		});
	}

	// ===== Write tools =====================================================

	// ----- create_blueprint (A3) -------------------------------------------
	{
		ToolDescriptor d;
		d.name = "create_blueprint";
		d.description =
			"[blueprint] Create a new Blueprint asset under `/Game/...` extending `parent_class`. "
			"`parent_class` accepts short names (\"Actor\", \"ACharacter\") "
			"and full UClass paths (\"/Script/Engine.Actor\"). Idempotent: "
			"calling with an existing asset returns "
			"`{ok:true, already_existed:true}` without modifying it. Pair "
			"with `apply_ops` to create + populate a BP in one batch.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path",   {{"type","string"},
								  {"description","Must start with /Game/. Example: /Game/AI/BP_Boss"}}},
				{"parent_class", {{"type","string"},
								  {"description","UClass short name or full path. Example: Actor or /Script/Engine.Actor"}}},
			}},
			{"required", nlohmann::json::array({"asset_path","parent_class"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset  = RequireString(args, "asset_path");
			std::string parent = RequireString(args, "parent_class");
			auto r = reader.CreateBlueprint(asset, parent);
			return nlohmann::json{
				{"ok", true},
				{"asset_path", asset},
				{"already_existed", r.alreadyExisted},
				{"parent_class", r.parentClass.empty() ? parent : r.parentClass},
			};
		});
	}

	// ----- add_variable ----------------------------------------------------
	{
		ToolDescriptor d;
		d.name = "add_variable";
		d.description =
			"[blueprint] Add a member variable to a Blueprint class. For graph nodes use `add_node`; for function parameters use `add_function_input`/`add_function_output`. `type` accepts either a "
			"shorthand string (\"float\", \"int\", \"bool\", \"string\", "
			"\"object:Actor\", \"struct:FVector\", \"[]float\", "
			"\"{string:int}\") or the canonical BPPinType object "
			"{category, sub_category, sub_category_object, is_array, is_set, is_map}. "
			"Idempotent: if a variable with this name already exists, returns "
			"{ok:true, already_existed:true} without modifying it.";
		d.input_schema = {
			{"type", "object"},
			{"properties", {
				{"asset_path",    {{"type","string"}}},
				{"name",          {{"type","string"}}},
				{"type",          {{"description","Type shorthand string or BPPinType object — see tool description."}}},
				{"default_value", {{"type","string"}}},
				{"category",      {{"type","string"}}},
				{"replicated",    {{"type","boolean"}}},
				{"editable",      {{"type","boolean"}}},
			}},
			{"required", nlohmann::json::array({"asset_path","name","type"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			const std::string& asset = RequireString(args, "asset_path");
			const std::string& name  = RequireString(args, "name");
			auto typeIt = args.find("type");
			if (typeIt == args.end()) {
				throw std::invalid_argument(R"(missing argument "type")");
			}
			BPPinType type = ParseTypeArg(*typeIt);

			std::string defaultValue = OptString(args, "default_value", "");
			std::string category     = OptString(args, "category",      "");
			bool replicated = args.value("replicated", false);
			bool editable   = args.value("editable",   false);

			// Idempotency: check first. The mock backend doesn't implement
			// pre-flight checks, so the existence probe goes through the
			// same ListVariables path the agent would use to inspect.
			try {
				auto existing = reader.ListVariables(asset);
				for (const auto& v : existing) {
					if (v.Name == name) {
						return nlohmann::json{{"ok", true}, {"already_existed", true}};
					}
				}
			} catch (...) {
				// ListVariables failure shouldn't block the write — fall
				// through; the write itself will surface the real error.
			}
			reader.AddVariable(asset, name, type, defaultValue, category, replicated, editable);
			return nlohmann::json{{"ok", true}, {"already_existed", false}};
		});
	}

	// ----- set_node_position -----------------------------------------------
	{
		ToolDescriptor d;
		d.name = "set_node_position";
		d.description = "[blueprint] Move a K2 node (by GUID) to (x, y) inside a Blueprint graph. Recompiles + saves the BP.";
		d.input_schema = {
			{"type", "object"},
			{"properties", {
				{"asset_path", {{"type","string"}}},
				{"graph_name", {{"type","string"}}},
				{"node_id",    {{"type","string"}, {"description","Node GUID"}}},
				{"x",          {{"type","integer"}}},
				{"y",          {{"type","integer"}}},
			}},
			{"required", nlohmann::json::array({"asset_path","graph_name","node_id","x","y"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			const std::string& asset = RequireString(args, "asset_path");
			const std::string& graph = RequireString(args, "graph_name");
			const std::string& node  = RequireString(args, "node_id");
			int x = args.at("x").get<int>();
			int y = args.at("y").get<int>();
			reader.SetNodePosition(asset, graph, node, x, y);
			return nlohmann::json{{"ok", true}};
		});
	}

	// ----- delete_node -----------------------------------------------------
	{
		ToolDescriptor d;
		d.name = "delete_node";
		d.description = "[blueprint] Delete a K2 node by GUID. Breaks any links into/out of it. Recompiles + saves.";
		d.input_schema = {
			{"type", "object"},
			{"properties", {
				{"asset_path", {{"type","string"}}},
				{"graph_name", {{"type","string"}}},
				{"node_id",    {{"type","string"}, {"description","Node GUID"}}},
			}},
			{"required", nlohmann::json::array({"asset_path","graph_name","node_id"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			const std::string& asset = RequireString(args, "asset_path");
			const std::string& graph = RequireString(args, "graph_name");
			const std::string& node  = RequireString(args, "node_id");
			reader.DeleteNode(asset, graph, node);
			return nlohmann::json{{"ok", true}};
		});
	}

	// ----- add_node --------------------------------------------------------
	{
		ToolDescriptor d;
		d.name = "add_node";
		d.description =
			"[blueprint] Spawn a new K2 (Blueprint) node in a Blueprint graph. NOT for UMG widgets (`add_widget`), Behavior Tree (`add_bt_node`), AnimBP state (`add_anim_state`), StateTree (`add_state_tree_state`), Material expressions (`add_material_expression`), or Level Sequence tracks (`add_sequence_track`). `kind` is one of: "
			"Branch, Sequence, VariableGet, VariableSet, CallFunction, CustomEvent. "
			"Kind-specific args: VariableGet/VariableSet -> `variable`; "
			"CallFunction -> `function` + `function_owner` (UClass path or short name); "
			"CustomEvent -> `event_name`. Returns {ok, node_id, pins:[...]}. "
			"The `pins` array carries each pin's name/guid/direction/type so "
			"you can wire it without a follow-up get_graph call.";
		d.input_schema = {
			{"type", "object"},
			{"properties", {
				{"asset_path",     {{"type","string"}}},
				{"graph_name",     {{"type","string"}}},
				{"kind",           {{"type","string"}}},
				{"x",              {{"type","integer"}}},
				{"y",              {{"type","integer"}}},
				{"variable",       {{"type","string"}}},
				{"function",       {{"type","string"}}},
				{"function_owner", {{"type","string"}}},
				{"event_name",     {{"type","string"}}},
				{"target_class",   {{"type","string"}}},
				{"struct_type",    {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"asset_path","graph_name","kind","x","y"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			const std::string& asset = RequireString(args, "asset_path");
			const std::string& graph = RequireString(args, "graph_name");
			const std::string& kind  = RequireString(args, "kind");
			int x = args.at("x").get<int>();
			int y = args.at("y").get<int>();
			std::map<std::string, std::string, std::less<>> extras;
			// Map MCP-side names → plugin commandlet flag names.
			auto put = [&](const char* mcpKey, const char* flagKey) {
				std::string v = OptString(args, mcpKey, "");
				if (!v.empty())
				{
					extras.emplace(flagKey, std::move(v));
				}
			};
			put("variable",       "Variable");
			put("function",       "Function");
			put("function_owner", "FunctionOwner");
			put("event_name",     "EventName");
			put("target_class",   "TargetClass");
			put("struct_type",    "StructType");
			std::string newId = reader.AddNode(asset, graph, kind, x, y, extras);

			// Post-fetch the graph to extract the new node's pins. The
			// CachingBlueprintReader will absorb most of the cost on the
			// common case where the agent just got the graph or is about
			// to. This eliminates a class of round-trips: the caller now
			// has everything they need to call wire_pins.
			nlohmann::json pinsJson = nlohmann::json::array();
			std::string title;
			std::string nodeClass;
			try {
				auto g = reader.GetGraph(asset, graph);
				for (const auto& n : g.Nodes) {
					if (n.Id == newId) {
						title = n.Title;
						nodeClass = n.Class;
						for (const auto& p : n.Pins) {
							nlohmann::json t = {{"category", p.Type.Category}};
							if (p.Type.SubCategory)
							{
								t["sub_category"]        = *p.Type.SubCategory;
							}
							if (p.Type.SubCategoryObject)
							{
								t["sub_category_object"] = *p.Type.SubCategoryObject;
							}
							if (p.Type.IsArray)
							{
								t["is_array"] = true;
							}
							if (p.Type.IsSet)
							{
								t["is_set"]   = true;
							}
							if (p.Type.IsMap)
							{
								t["is_map"]   = true;
							}
							pinsJson.push_back({
								{"name",      p.Name},
								{"guid",      p.Id},
								{"direction", p.Direction},
								{"type",      std::move(t)},
							});
						}
						break;
					}
				}
			} catch (...) { /* pin enrichment is best-effort */ }
			nlohmann::json out = {
				{"ok", true},
				{"node_id", newId},
				{"pins", std::move(pinsJson)},
			};
			if (!title.empty())
			{
				out["title"] = title;
			}
			if (!nodeClass.empty())
			{
				out["class"] = nodeClass;
			}
			return out;
		});
	}

	// ----- get_node --------------------------------------------------------
	{
		ToolDescriptor d;
		d.name = "get_node";
		d.description =
			"[blueprint] Fetch a single K2 node by GUID inside a graph. Returns the node's "
			"class, title, position, pins, and links — same shape as one "
			"entry from `get_graph`'s nodes array, minus the round-trip cost "
			"of fetching every node. Pairs with `find_node` (which gives you "
			"the GUID) for targeted inspection.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path", {{"type","string"}}},
				{"graph_name", {{"type","string"}}},
				{"node_id",    {{"type","string"}, {"description","Node GUID"}}},
				{"fields",     FieldsProperty()},
			}},
			{"required", nlohmann::json::array({"asset_path","graph_name","node_id"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			const std::string& asset = RequireString(args, "asset_path");
			const std::string& graph = RequireString(args, "graph_name");
			const std::string& node  = RequireString(args, "node_id");
			auto ctl = ParseResponseControls(args);
			auto g = reader.GetGraph(asset, graph);
			for (auto& n : g.Nodes) {
				if (n.Id == node) {
					nlohmann::json body = n;
					ApplyResponseControls(body, ctl);
					return body;
				}
			}
			throw bpr::backends::AssetNotFound(fmt::format(
				"node '{}' not found in graph '{}' of '{}'", node, graph, asset));
		});
	}

	// ----- find_overriders -------------------------------------------------
	{
		ToolDescriptor d;
		d.name = "find_overriders";
		d.description =
			"[blueprint] Find Blueprints under `path` that match a structural query: "
			"extend `parent_class`, override `function_name`, and/or implement "
			"`interface`. All filters are optional but at least one must be "
			"set. Returns a list of `{asset_path, parent_class, matched: [...]}` "
			"entries. Replaces the manual `list_blueprints` + N×`read_blueprint` "
			"loop the agent would otherwise walk.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"path",           {{"type","string"},
									{"description","Content-path scope. Defaults to /Game."}}},
				{"parent_class",   {{"type","string"},
									{"description","Match BPs whose parent class equals this (short name OR /Script/... path)."}}},
				{"function_name",  {{"type","string"},
									{"description","Match BPs that define a function with this name (e.g. \"BeginPlay\")."}}},
				{"interface",      {{"type","string"},
									{"description","Match BPs that implement this interface (short name)."}}},
				{"limit",  LimitProperty()},
				{"offset", OffsetProperty()},
				{"fields", FieldsProperty()},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string path = OptString(args, "path", "/Game");
			std::string parent = OptString(args, "parent_class", "");
			std::string fn     = OptString(args, "function_name", "");
			std::string iface  = OptString(args, "interface", "");
			if (parent.empty() && fn.empty() && iface.empty()) {
				throw std::invalid_argument(
					"find_overriders requires at least one of "
					"parent_class / function_name / interface");
			}
			auto ctl = ParseResponseControls(args);

			// Helper: short-name vs full-path equality (UE parent_class fields
			// can be either depending on engine version + how the BP was saved).
			auto matchesClass = [](std::string_view candidate,
								   std::string_view query) {
				if (candidate == query)
				{
					return true;
				}
				// Strip path / class-suffix to compare short names.
				auto last = candidate.find_last_of("/.");
				std::string_view candShort = last == std::string_view::npos
												 ? candidate
												 : candidate.substr(last + 1);
				if (candShort.size() > 2 &&
					candShort.substr(candShort.size() - 2) == "_C") {
					candShort = candShort.substr(0, candShort.size() - 2);
				}
				return candShort == query;
			};

			nlohmann::json out = nlohmann::json::array();
			auto items = reader.ListBlueprints(path);
			for (const auto& s : items) {
				std::vector<std::string> matched;
				BPMetadata meta;
				bool needFullRead = !fn.empty() || !iface.empty();
				if (!parent.empty() && matchesClass(s.ParentClass, parent)) {
					matched.push_back("parent_class");
				}
				if (needFullRead) {
					try { meta = reader.ReadBlueprint(s.AssetPath); }
					catch (...) { continue; }
					if (!fn.empty()) {
						for (const auto& f : meta.Functions) {
							if (f.Name == fn) { matched.push_back("function_name"); break; }
						}
					}
					if (!iface.empty()) {
						for (const auto& i : meta.Interfaces) {
							if (i == iface) { matched.push_back("interface"); break; }
						}
					}
				}
				if (!matched.empty()) {
					out.push_back({
						{"asset_path",   s.AssetPath},
						{"name",         s.Name},
						{"parent_class", s.ParentClass},
						{"matched",      matched},
					});
				}
			}
			ApplyResponseControls(out, ctl);
			return out;
		});
	}

	// ----- wire_pins -------------------------------------------------------
	{
		ToolDescriptor d;
		d.name = "wire_pins";
		d.description =
			"[blueprint] Connect two K2 pins. `from_pin` and `to_pin` accept either a pin GUID "
			"(preferred — see get_graph) or a pin name. The schema's pin "
			"compatibility rules are enforced; on failure, the error message "
			"includes the actual pin types so the caller can self-correct.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path",  {{"type","string"}}},
				{"graph_name",  {{"type","string"}}},
				{"from_node",   {{"type","string"}}},
				{"from_pin",    {{"type","string"}}},
				{"to_node",     {{"type","string"}}},
				{"to_pin",      {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"asset_path","graph_name","from_node","from_pin","to_node","to_pin"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			const std::string asset    = RequireString(args, "asset_path");
			const std::string graph    = RequireString(args, "graph_name");
			const std::string fromNode = RequireString(args, "from_node");
			const std::string fromPin  = RequireString(args, "from_pin");
			const std::string toNode   = RequireString(args, "to_node");
			const std::string toPin    = RequireString(args, "to_pin");
			try {
				reader.WirePins(asset, graph, fromNode, fromPin, toNode, toPin);
				return nlohmann::json{{"ok", true}};
			} catch (const std::exception& e) {
				// Try to enrich the error with the pin types of both ends.
				// The agent gets actionable info ("can't wire object:Actor
				// to bool") instead of just "wire failed."
				std::string fromType, toType;
				auto findPin = [](const BPNode& n, const std::string& spec) -> const BPPin* {
					for (const auto& p : n.Pins) {
						if (p.Id == spec || p.Name == spec)
						{
							return &p;
						}
					}
					return nullptr;
				};
				auto describeType = [](const BPPinType& t) {
					std::string s = t.Category;
					if (t.SubCategory)
					{
						s += ":" + *t.SubCategory;
					}
					if (t.SubCategoryObject)
					{
						s += "(" + *t.SubCategoryObject + ")";
					}
					if (t.IsArray)
					{
						s = "[]" + s;
					}
					if (t.IsSet)   s = "{}" + s;
					if (t.IsMap)   s = "{key:" + s + "}";
					return s;
				};
				try {
					auto g = reader.GetGraph(asset, graph);
					for (const auto& n : g.Nodes) {
						if (n.Id == fromNode) {
							if (auto* p = findPin(n, fromPin))
							{
								fromType = describeType(p->Type);
							}
						}
						if (n.Id == toNode) {
							if (auto* p = findPin(n, toPin))
							{
								toType = describeType(p->Type);
							}
						}
					}
				} catch (...) { /* type lookup is best-effort */ }
				std::string msg = e.what();
				if (!fromType.empty() || !toType.empty()) {
					msg += fmt::format(
						" [from_pin type={}, to_pin type={}]",
						fromType.empty() ? "<unknown>" : fromType,
						toType.empty()   ? "<unknown>" : toType);
				}
				throw bpr::backends::BlueprintReaderError(msg);
			}
		});
	}

	// ----- retype_variable (BP-2) ------------------------------------------
	{
		ToolDescriptor d;
		d.name = "retype_variable";
		d.description =
			"[blueprint] Change a Blueprint member variable's type without delete + re-add. UE "
			"rewires every VariableGet / VariableSet node that references "
			"the variable in place — existing graphs survive. For a "
			"brand-new variable, use add_variable instead. `type` accepts "
			"the same shorthand strings (\"float\", \"object:Actor\", "
			"\"[]float\") and BPPinType objects as add_variable.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path", {{"type","string"}}},
				{"name",       {{"type","string"}}},
				{"type",       {{"description","Type shorthand string or BPPinType object."}}},
			}},
			{"required", nlohmann::json::array({"asset_path","name","type"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			const std::string& asset = RequireString(args, "asset_path");
			const std::string& name  = RequireString(args, "name");
			auto typeIt = args.find("type");
			if (typeIt == args.end()) {
				throw std::invalid_argument(R"(missing argument "type")");
			}
			reader.RetypeVariable(asset, name, ParseTypeArg(*typeIt));
			return nlohmann::json{{"ok", true}};
		});
	}

	// ----- set_variable_category (BP-7) ------------------------------------
	{
		ToolDescriptor d;
		d.name = "set_variable_category";
		d.description =
			"[blueprint] Change the My-Blueprint-panel category label on a "
			"Blueprint member variable (the \"Stats\" / \"Combat\" group header in the "
			"BP editor). Empty `category` clears the label back to "
			"default. For a brand-new variable, pass `category` to "
			"add_variable instead — this tool is for retroactive edits.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path", {{"type","string"}}},
				{"name",       {{"type","string"}}},
				{"category",   {{"type","string"},
								{"description","Empty clears the category back to default."}}},
			}},
			{"required", nlohmann::json::array({"asset_path","name"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			reader.SetVariableCategory(
				RequireString(args, "asset_path"),
				RequireString(args, "name"),
				args.value("category", std::string{}));
			return nlohmann::json{{"ok", true}};
		});
	}

	// ----- duplicate_blueprint (BP-5) --------------------------------------
	{
		ToolDescriptor d;
		d.name = "duplicate_blueprint";
		d.description =
			"[asset] File-level duplicate: source BP at `asset_path` → new BP at "
			"`dest_asset_path`. Both must be under /Game/. Idempotent: if "
			"the destination already exists, returns "
			"{ok:true, already_existed:true} without overwriting. The new "
			"BP starts identical to the source (same vars, functions, "
			"graphs, components) and is registered with the asset registry "
			"so a follow-up apply_ops batch can mutate it.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path",      {{"type","string"},
									 {"description","Source BP under /Game/. Must exist."}}},
				{"dest_asset_path", {{"type","string"},
									 {"description","Destination BP under /Game/. Must NOT exist (idempotent: returns already_existed:true if it does)."}}},
			}},
			{"required", nlohmann::json::array({"asset_path","dest_asset_path"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string source = RequireString(args, "asset_path");
			std::string dest   = RequireString(args, "dest_asset_path");
			auto r = reader.DuplicateBlueprint(source, dest);
			return nlohmann::json{
				{"ok", true},
				{"asset_path", dest},
				{"source_asset_path", source},
				{"already_existed", r.alreadyExisted},
			};
		});
	}

	// ----- delete_variable -------------------------------------------------
	{
		ToolDescriptor d;
		d.name = "delete_variable";
		d.description = "[blueprint] Remove a Blueprint member variable by name. Recompiles + saves.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path", {{"type","string"}}},
				{"name",       {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"asset_path","name"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			reader.DeleteVariable(
				RequireString(args, "asset_path"),
				RequireString(args, "name"));
			return nlohmann::json{{"ok", true}};
		});
	}

	// ----- rename_variable -------------------------------------------------
	{
		ToolDescriptor d;
		d.name = "rename_variable";
		d.description =
			"[blueprint] Rename a Blueprint member variable. Updates references in graphs. "
			"Recompiles + saves.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path", {{"type","string"}}},
				{"old_name",   {{"type","string"}}},
				{"new_name",   {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"asset_path","old_name","new_name"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			reader.RenameVariable(
				RequireString(args, "asset_path"),
				RequireString(args, "old_name"),
				RequireString(args, "new_name"));
			return nlohmann::json{{"ok", true}};
		});
	}

	// ----- add_function ----------------------------------------------------
	{
		ToolDescriptor d;
		d.name = "add_function";
		d.description =
			"[blueprint] Create a new BP function graph with the given name. Returns "
			"{ok, function_name, already_existed}. Use add_function_input / "
			"add_function_output to declare its signature. Idempotent — "
			"calling with an existing name returns already_existed:true.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path", {{"type","string"}}},
				{"name",       {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"asset_path","name"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			const std::string& asset = RequireString(args, "asset_path");
			const std::string& name  = RequireString(args, "name");
			// Idempotency probe via ReadBlueprint (returns the function list).
			try {
				auto meta = reader.ReadBlueprint(asset);
				for (const auto& fn : meta.Functions) {
					if (fn.Name == name) {
						return nlohmann::json{
							{"ok", true},
							{"function_name", name},
							{"already_existed", true}};
					}
				}
			} catch (...) { /* fall through; the write surfaces real errors */ }
			auto out = reader.AddFunction(asset, name);
			nlohmann::json result = {
				{"ok", true},
				{"function_name", out.functionName},
				{"already_existed", false},
			};
			if (!out.entryNodeId.empty()) {
				result["entry_node_id"] = out.entryNodeId;
			}
			return result;
		});
	}

	// ----- add_function_input ----------------------------------------------
	{
		ToolDescriptor d;
		d.name = "add_function_input";
		d.description =
			"[blueprint] Add an input parameter to an existing function. `type` accepts "
			"either a shorthand string (e.g. \"float\", \"object:Actor\") or "
			"a BPPinType object.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path",    {{"type","string"}}},
				{"function_name", {{"type","string"}}},
				{"param_name",    {{"type","string"}}},
				{"type",          {{"description","Type shorthand string or BPPinType object."}}},
			}},
			{"required", nlohmann::json::array({"asset_path","function_name","param_name","type"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			const std::string& asset = RequireString(args, "asset_path");
			const std::string& fn    = RequireString(args, "function_name");
			const std::string& param = RequireString(args, "param_name");
			auto typeIt = args.find("type");
			if (typeIt == args.end()) {
				throw std::invalid_argument(R"(missing argument "type")");
			}
			reader.AddFunctionInput(asset, fn, param, ParseTypeArg(*typeIt));
			return nlohmann::json{{"ok", true}};
		});
	}

	// ----- add_function_output ---------------------------------------------
	{
		ToolDescriptor d;
		d.name = "add_function_output";
		d.description =
			"[blueprint] Add an output parameter to an existing function. Spawns a "
			"FunctionResult node if there isn't one yet. `type` accepts "
			"either a shorthand string (e.g. \"float\", \"object:Actor\") or "
			"a BPPinType object.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path",    {{"type","string"}}},
				{"function_name", {{"type","string"}}},
				{"param_name",    {{"type","string"}}},
				{"type",          {{"description","Type shorthand string or BPPinType object."}}},
			}},
			{"required", nlohmann::json::array({"asset_path","function_name","param_name","type"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			const std::string& asset = RequireString(args, "asset_path");
			const std::string& fn    = RequireString(args, "function_name");
			const std::string& param = RequireString(args, "param_name");
			auto typeIt = args.find("type");
			if (typeIt == args.end()) {
				throw std::invalid_argument(R"(missing argument "type")");
			}
			reader.AddFunctionOutput(asset, fn, param, ParseTypeArg(*typeIt));
			return nlohmann::json{{"ok", true}};
		});
	}

	// ----- delete_function -------------------------------------------------
	{
		ToolDescriptor d;
		d.name = "delete_function";
		d.description = "[blueprint] Delete a Blueprint function graph by name.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path", {{"type","string"}}},
				{"name",       {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"asset_path","name"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			reader.DeleteFunction(
				RequireString(args, "asset_path"),
				RequireString(args, "name"));
			return nlohmann::json{{"ok", true}};
		});
	}

	// ----- set_variable_default --------------------------------------------
	{
		ToolDescriptor d;
		d.name = "set_variable_default";
		d.description =
			"[blueprint] Change a Blueprint member variable's default value (string form, as displayed "
			"in the Details panel — e.g. \"100.0\" for a float, \"true\"/\"false\" "
			"for a bool). Pass empty string to clear.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path",    {{"type","string"}}},
				{"name",          {{"type","string"}}},
				{"default_value", {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"asset_path","name","default_value"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			reader.SetVariableDefault(
				RequireString(args, "asset_path"),
				RequireString(args, "name"),
				args.value("default_value", ""));
			return nlohmann::json{{"ok", true}};
		});
	}

	// ===== Discoverability =================================================
	// These two tools return static metadata about the writable surface so a
	// model can ask "what are the valid `kind` values for add_node?" or
	// "what does a BPPinType for a struct ref look like?" without scanning
	// documentation. The lists are baked in to match the plugin's actual
	// dispatch — keep them in sync with BlueprintReaderCommandlet.cpp's
	// RunAddNodeOp + BlueprintReaderWireJson::ParseWirePinType.

	// ----- list_node_kinds -------------------------------------------------
	{
		ToolDescriptor d;
		d.name = "list_node_kinds";
		d.description =
			"[discover] List the `kind` values that `add_node` accepts, with required "
			"extras for each. Pure metadata — no backend call.";
		d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
		registry.Add(std::move(d), [](const nlohmann::json&) {
			return nlohmann::json::array({
				nlohmann::json{
					{"kind", "Branch"},
					{"class", "K2Node_IfThenElse"},
					{"description", "Two-way exec branch on a bool condition pin."},
					{"extras", nlohmann::json::array()},
				},
				nlohmann::json{
					{"kind", "Sequence"},
					{"class", "K2Node_ExecutionSequence"},
					{"description", "Fires Then 0..N in order. Add more outputs by hand in the editor."},
					{"extras", nlohmann::json::array()},
				},
				nlohmann::json{
					{"kind", "VariableGet"},
					{"class", "K2Node_VariableGet"},
					{"description", "Read a member variable on Self."},
					{"extras", nlohmann::json::array({
						nlohmann::json{{"name","variable"}, {"required",true},
									   {"description","Member variable name."}}
					})},
				},
				nlohmann::json{
					{"kind", "VariableSet"},
					{"class", "K2Node_VariableSet"},
					{"description", "Write a member variable on Self."},
					{"extras", nlohmann::json::array({
						nlohmann::json{{"name","variable"}, {"required",true},
									   {"description","Member variable name."}}
					})},
				},
				nlohmann::json{
					{"kind", "CallFunction"},
					{"class", "K2Node_CallFunction"},
					{"description", "Call a UFUNCTION on a class."},
					{"extras", nlohmann::json::array({
						nlohmann::json{{"name","function"}, {"required",true},
									   {"description","Function name as declared on the owning class."}},
						nlohmann::json{{"name","function_owner"}, {"required",true},
									   {"description","UClass path (`/Script/Engine.KismetSystemLibrary`) or short name (`KismetSystemLibrary`)."}}
					})},
				},
				nlohmann::json{
					{"kind", "CustomEvent"},
					{"class", "K2Node_CustomEvent"},
					{"description", "Defines a custom event entry point."},
					{"extras", nlohmann::json::array({
						nlohmann::json{{"name","event_name"}, {"required",true},
									   {"description","FName for the new event."}}
					})},
				},
				nlohmann::json{
					{"kind", "Cast"},
					{"class", "K2Node_DynamicCast"},
					{"description", "Cast an object to a target class. Provides Cast Failed and As<TargetClass> output pins."},
					{"extras", nlohmann::json::array({
						nlohmann::json{{"name","target_class"}, {"required",true},
									   {"description","UClass path or short name to cast to."}}
					})},
				},
				nlohmann::json{
					{"kind", "Self"},
					{"class", "K2Node_Self"},
					{"description", "Reference to Self (the owning blueprint instance)."},
					{"extras", nlohmann::json::array()},
				},
				nlohmann::json{
					{"kind", "MakeArray"},
					{"class", "K2Node_MakeArray"},
					{"description", "Build a TArray literal. Element type is wildcard until connected."},
					{"extras", nlohmann::json::array()},
				},
				nlohmann::json{
					{"kind", "MakeStruct"},
					{"class", "K2Node_MakeStruct"},
					{"description", "Build a USTRUCT literal. Each member becomes an input pin."},
					{"extras", nlohmann::json::array({
						nlohmann::json{{"name","struct_type"}, {"required",true},
									   {"description","UScriptStruct path, e.g. /Script/CoreUObject.Vector."}}
					})},
				},
				nlohmann::json{
					{"kind", "FormatText"},
					{"class", "K2Node_FormatText"},
					{"description", "Format an FText with named arguments. Add args by editing the Format string."},
					{"extras", nlohmann::json::array()},
				},
				nlohmann::json{
					{"kind", "Knot"},
					{"class", "K2Node_Knot"},
					{"description", "Reroute node — pure visual, useful for cleaning up wire crossings."},
					{"extras", nlohmann::json::array()},
				},
			});
		});
	}

	// ----- list_pin_categories ---------------------------------------------
	{
		ToolDescriptor d;
		d.name = "list_pin_categories";
		d.description =
			"[discover] List the canonical BPPinType.category values + container modifiers. "
			"Useful when constructing the `type` argument for add_variable. "
			"Pure metadata — no backend call.";
		d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
		registry.Add(std::move(d), [](const nlohmann::json&) {
			auto cat = [](const char* c, const char* desc, std::vector<std::string> subs = {},
						  const char* objHint = nullptr) {
				nlohmann::json j = {
					{"category", c},
					{"description", desc},
					{"sub_categories", subs},
				};
				if (objHint)
				{
					j["sub_category_object_hint"] = objHint;
				}
				return j;
			};
			return nlohmann::json{
				{"categories", nlohmann::json::array({
					cat("exec",      "Execution flow (white wires)."),
					cat("bool",      "Boolean."),
					cat("byte",      "8-bit unsigned integer (also used for enums).",
						{}, "Optional UEnum path if the byte is an enum."),
					cat("int",       "32-bit signed integer."),
					cat("int64",     "64-bit signed integer."),
					cat("real",      "Floating point. `sub_category` selects precision.",
						{"float","double"}),
					cat("string",    "FString."),
					cat("name",      "FName."),
					cat("text",      "FText."),
					cat("object",    "UObject reference. `sub_category_object` = the UClass path.",
						{}, "UClass path, e.g. /Script/Engine.Actor"),
					cat("soft_object", "Soft (deferred-load) UObject reference. C++ form is "
									   "TSoftObjectPtr<X>; survives package boundaries and "
									   "deleted target assets.",
						{}, "UClass path, e.g. /Script/Engine.Texture2D"),
					cat("class",     "UClass reference. `sub_category_object` = the meta-class.",
						{}, "UClass path"),
					cat("soft_class", "Soft UClass reference. C++ form is TSoftClassPtr<X>; "
									  "useful for lazy-loaded ability/weapon class references.",
						{}, "UClass path"),
					cat("interface", "Interface reference.",
						{}, "UClass path of the interface"),
					cat("struct",    "USTRUCT.",
						{}, "UScriptStruct path, e.g. /Script/CoreUObject.Vector"),
					cat("delegate",  "Single-cast delegate."),
					cat("wildcard",  "Pin matches any type until connected (used in macros)."),
				})},
				{"containers", nlohmann::json::array({
					nlohmann::json{{"flag","is_array"}, {"description","TArray<T>"}},
					nlohmann::json{{"flag","is_set"},   {"description","TSet<T>"}},
					nlohmann::json{{"flag","is_map"},   {"description","TMap<K,V> — note: only the key type is exposed via BPPinType today."}},
				})},
			};
		});
	}

	// ----- shutdown_daemon -------------------------------------------------
	// Tear down the editor daemon process so the project lock releases —
	// useful when you want to open the full UE editor without daemon
	// contention. Subsequent reads auto-respawn the daemon (cold-start
	// cost on first call). No-op on backends that don't have a daemon
	// (mock).
	{
		ToolDescriptor d;
		d.name = "shutdown_daemon";
		d.description =
			"[discover] Force-terminate the backing editor daemon process. In shared-daemon "
			"mode (the default), one daemon serves every MCP session against a "
			"given project — so this affects EVERY session, not just yours. "
			"Other sessions' next call simply spawns a fresh daemon (cold-start "
			"cost ~5–30 s depending on project size). The original use cases "
			"still work: releases the project's file locks (DDC, asset registry, "
			".uasset handles) so you can launch the full UE editor, or forces a "
			"fresh spawn after upgrading the plugin. Pair with "
			"BP_READER_READ_ONLY=1 if you want to keep the MCP server running "
			"for queries while you work in the editor.\n\n"
			"Returns {ok, was_running, hint}. Idempotent: calling when no "
			"daemon is alive returns was_running:false without erroring.";
		d.input_schema = {{"type", "object"}, {"properties", nlohmann::json::object()}};
		registry.Add(std::move(d), [&reader](const nlohmann::json&) {
			return reader.ShutdownDaemon();
		});
	}

	// ----- auto_layout_graph -----------------------------------------------
	// v1: a simple topological grid layout computed server-side. Walks the
	// exec edges from any FunctionEntry / Event node to produce columns,
	// ranks the rest by class. Calls set_node_position on every node.
	//
	// The "right" implementation is a plugin-side hook into UE's actual
	// graph-tidy code (KismetGraphSchema::DistributeNodesAlongAxis +
	// friends), which gets the same neat output as the editor's
	// right-click "Tidy" command. That's tracked as plugin work; this
	// server-side version keeps the agent unblocked and produces a
	// readable (if not pretty) layout in the meantime.
	{
		ToolDescriptor d;
		d.name = "auto_layout_graph";
		d.description =
			"[blueprint] Auto-position the nodes in a Blueprint graph so they don't overlap. "
			"v1 is a column-grid layout computed from exec connectivity — "
			"readable but not as tidy as UE's built-in graph-tidy. Plugin-"
			"side integration with KismetGraphSchema's actual tidy pass is "
			"tracked separately. Returns {ok, placed: <n>}.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path", {{"type","string"}}},
				{"graph_name", {{"type","string"}}},
				{"col_width",  {{"type","integer"},
								{"description","Horizontal spacing between columns. Default 400."}}},
				{"row_height", {{"type","integer"},
								{"description","Vertical spacing between rows. Default 200."}}},
			}},
			{"required", nlohmann::json::array({"asset_path","graph_name"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireString(args, "asset_path");
			std::string graph = RequireString(args, "graph_name");
			int colWidth  = OptInt(args, "col_width",  400);
			int rowHeight = OptInt(args, "row_height", 200);

			auto g = reader.GetGraph(asset, graph);

			// Build forward-exec adjacency: node_id -> list of downstream node_ids.
			// Identify exec edges by pin name "exec/then/else/loop/loopBody/completed"
			// — best-effort but covers the common K2 nodes.
			auto isExecPinName = [](std::string_view n) {
				return n == "exec" || n == "execute" || n == "then" || n == "else" ||
					   n == "loop" || n == "loopBody" || n == "completed";
			};
			std::map<std::string, std::vector<std::string>> downstream;
			// Map pin GUID -> owning node_id (for connections that reference pins by GUID).
			std::map<std::string, std::string, std::less<>> pinOwner;
			std::map<std::string, std::string, std::less<>> pinName;
			for (const auto& n : g.Nodes) {
				for (const auto& p : n.Pins) {
					pinOwner[p.Id] = n.Id;
					pinName[p.Id]  = p.Name;
				}
			}
			for (const auto& c : g.Connections) {
				// Exec edge if either endpoint pin's name looks exec-y.
				std::string fpName = c.FromPin;
				std::string tpName = c.ToPin;
				if (auto it = pinName.find(c.FromPin); it != pinName.end())
				{
					fpName = it->second;
				}
				if (auto it = pinName.find(c.ToPin);   it != pinName.end())
				{
					tpName = it->second;
				}
				if (!isExecPinName(fpName) && !isExecPinName(tpName))
				{
					continue;
				}
				downstream[c.FromNode].push_back(c.ToNode);
			}

			// Roots: nodes with class containing "Entry" or "Event" (no
			// upstream exec). If we don't find any, fall back to all nodes
			// as their own roots — at least they'll be ranked into columns.
			std::set<std::string> hasUpstream;
			for (const auto& [_, dsts] : downstream)
				for (const auto& d : dsts)
				{
					hasUpstream.insert(d);
				}
			std::vector<std::string> roots;
			for (const auto& n : g.Nodes) {
				bool isEntryClass = n.Class.find("FunctionEntry") != std::string::npos ||
									n.Class.find("Event")         != std::string::npos ||
									n.Class.find("CustomEvent")   != std::string::npos;
				if (isEntryClass && !hasUpstream.count(n.Id))
				{
					roots.push_back(n.Id);
				}
			}
			if (roots.empty()) {
				for (const auto& n : g.Nodes)
				{
					if (!hasUpstream.count(n.Id)) roots.push_back(n.Id);
				}
			}

			// BFS from roots; column = depth, row = order-of-discovery within column.
			std::map<std::string, int, std::less<>> col;  // node_id -> column
			std::vector<std::string> bfs = roots;
			for (const auto& r : roots)
			{
				col[r] = 0;
			}
			for (std::size_t i = 0; i < bfs.size(); ++i) {
				int c = col[bfs[i]];
				for (const auto& d : downstream[bfs[i]]) {
					auto [it, inserted] = col.insert({d, c + 1});
					if (inserted)
					{
						bfs.push_back(d);
					}
				}
			}
			// Any disconnected leftovers get their own column at the right.
			int maxCol = 0;
			for (auto& [_, v] : col)
			{
				maxCol = std::max(maxCol, v);
			}
			int leftoverCol = maxCol + 1;
			for (const auto& n : g.Nodes) {
				if (col.find(n.Id) == col.end())
				{
					col[n.Id] = leftoverCol;
				}
			}

			// Group by column, assign rows.
			std::map<int, std::vector<std::string>> byCol;
			for (const auto& [id, c] : col)
			{
				byCol[c].push_back(id);
			}

			// Apply positions via set_node_position. Done as individual
			// ops here (not apply_ops) because the reader saves per-call
			// anyway — extra batching wouldn't help until plugin-side
			// single-recompile lands.
			int placed = 0;
			for (const auto& [c, ids] : byCol) {
				int row = 0;
				for (const auto& id : ids) {
					int x = c * colWidth;
					int y = row * rowHeight;
					try {
						reader.SetNodePosition(asset, graph, id, x, y);
						++placed;
					} catch (...) { /* skip — can't move what doesn't exist */ }
					++row;
				}
			}
			return nlohmann::json{
				{"ok",      true},
				{"placed",  placed},
				{"strategy","grid"},
			};
		});
	}

	// ===== Project + Content Browser ops ===================================

	// ----- get_project_metadata -------------------------------------------
	{
		ToolDescriptor d;
		d.name = "get_project_metadata";
		d.description =
			"[asset] Read the project's `.uproject` file and return parsed metadata "
			"(project name, EngineAssociation, category, description, plus "
			"the raw JSON for anything else). Useful for orienting an agent "
			"to which project + engine version it's working against.";
		d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
		registry.Add(std::move(d), [&reader](const nlohmann::json&) {
			auto m = reader.GetProjectMetadata();
			return nlohmann::json{
				{"ok", true},
				{"project_name",       m.projectName},
				{"project_path",       m.projectPath},
				{"engine_association", m.engineAssociation},
				{"category",           m.category},
				{"description",        m.description},
				{"raw",                m.raw},
			};
		});
	}

	// ----- save_all -------------------------------------------------------
	{
		ToolDescriptor d;
		d.name = "save_all";
		d.description =
			"[asset] Save every dirty package the editor has loaded. With "
			"`dirty_only=true` (default), clean packages are skipped — fast "
			"no-op when nothing's changed. Returns count saved + any failed "
			"asset paths.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"dirty_only", {{"type","boolean"},
								{"description","Default true. Set false to save every loaded package, dirty or not."}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			bool dirtyOnly = args.value("dirty_only", true);
			auto r = reader.SaveAll(dirtyOnly);
			return nlohmann::json{
				{"ok", true},
				{"saved_count",    r.savedCount},
				{"failed_assets",  r.failedAssets},
			};
		});
	}

	// ----- move_asset (covers both move and rename) ----------------------
	{
		ToolDescriptor d;
		d.name = "move_asset";
		d.description =
			"[asset] Move or rename an asset. `dest_asset_path` is the full destination "
			"package path — pass the same folder with a different leaf for a "
			"rename, or a different folder to move. Both must be under "
			"`/Game/`. Updates the asset registry and fixes references in "
			"other assets. Returns the redirector count if UE created any.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path",      {{"type","string"}}},
				{"dest_asset_path", {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"asset_path","dest_asset_path"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string src  = RequireString(args, "asset_path");
			std::string dest = RequireString(args, "dest_asset_path");
			auto r = reader.MoveAsset(src, dest);
			return nlohmann::json{
				{"ok", true},
				{"source_path",          r.sourcePath},
				{"dest_path",            r.destPath},
				{"redirectors_created",  r.redirectorsCreated},
			};
		});
	}

	// ----- delete_asset ---------------------------------------------------
	{
		ToolDescriptor d;
		d.name = "delete_asset";
		d.description =
			"[asset] Delete an asset. Refuses by default if other assets reference "
			"it; set `force=true` to delete anyway (leaves dangling references "
			"as null objects). Returns the list of referencing assets when "
			"the refusal fires so the caller can act on them.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path", {{"type","string"}}},
				{"force",      {{"type","boolean"},
								{"description","Default false. Set true to delete even when references exist."}}},
			}},
			{"required", nlohmann::json::array({"asset_path"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string path = RequireString(args, "asset_path");
			bool force = args.value("force", false);
			auto r = reader.DeleteAsset(path, force);
			return nlohmann::json{
				{"ok", true},
				{"path",                r.path},
				{"deleted",             r.deleted},
				{"referencing_assets",  r.referencingAssets},
			};
		});
	}

	// ----- create_folder --------------------------------------------------
	{
		ToolDescriptor d;
		d.name = "create_folder";
		d.description =
			"[asset] Create a content-browser folder under `/Game/`. Idempotent — returns "
			"`{already_existed:true}` when the folder is already present.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"folder_path", {{"type","string"},
								 {"description","Package path of the folder to create, e.g. /Game/AI/Boss."}}},
			}},
			{"required", nlohmann::json::array({"folder_path"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string folderPath = RequireString(args, "folder_path");
			auto r = reader.CreateFolder(folderPath);
			return nlohmann::json{
				{"ok", true},
				{"path",            r.path},
				{"already_existed", r.alreadyExisted},
			};
		});
	}

	// ----- list_data_tables -----------------------------------------------
	{
		ToolDescriptor d;
		d.name = "list_data_tables";
		d.description =
			"[data table] List all UDataTable assets under a content path. Mirrors "
			"`list_blueprints` but filters for UDataTable. Defaults to "
			"`/Game`.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"path", {{"type","string"}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string path = OptString(args, "path", "/Game");
			auto summaries = reader.ListDataTables(path);
			nlohmann::json arr = nlohmann::json::array();
			for (const auto& s : summaries)
			{
				arr.push_back(s);
			}
			return arr;
		});
	}

	// ----- read_data_table ------------------------------------------------
	{
		ToolDescriptor d;
		d.name = "read_data_table";
		d.description =
			"[data table] Load a DataTable and return its row-struct type, column names, "
			"and every row's field values. Useful for orienting an agent to "
			"an existing data table before mutating it.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path", {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"asset_path"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireString(args, "asset_path");
			auto dt = reader.ReadDataTable(asset);
			return nlohmann::json{
				{"ok", true},
				{"asset_path", dt.assetPath},
				{"row_struct", dt.rowStruct},
				{"columns",    dt.columns},
				{"rows",       dt.rows},
			};
		});
	}

	// ===== Live editor ops =================================================
	// These work best with the live backend (open editor); they also
	// route through commandlet daemon mode but PIE / live-coding don't
	// make semantic sense in a headless editor.

	// ----- console_command -----------------------------------------------
	{
		ToolDescriptor d;
		d.name = "console_command";
		d.description =
			"[editor] Execute a UE console command (e.g. `stat unit`, `showflag.bones 1`, "
			"`r.ScreenPercentage 75`). Returns whatever the command echoed to "
			"the log.";
		d.input_schema = {
			{"type","object"},
			{"properties", {{"command", {{"type","string"}}}}},
			{"required", nlohmann::json::array({"command"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string cmd = RequireString(args, "command");
			auto r = reader.ConsoleCommand(cmd);
			return nlohmann::json{{"ok", true}, {"output", r.output}};
		});
	}

	// ----- get_editor_state -----------------------------------------------
	// Situational-awareness one-shot. Mirrors Epic AIAssistant's Slate
	// querier surface in a single MCP call: open assets + active asset +
	// current level + viewport camera + actor selection + PIE state.
	{
		ToolDescriptor d;
		d.name = "get_editor_state";
		d.description =
			"[editor] One-call situational awareness: what assets are open in "
			"the editor (which is active), the currently-loaded level and "
			"its dirty state, the viewport camera transform, currently-"
			"selected actors, and whether PIE is running. Use this BEFORE "
			"starting a multi-step edit to ground the agent on what the "
			"human is looking at right now. Live-editor only — returns "
			"empty/null fields in commandlet mode (no editor UI). The "
			"selected_actors field duplicates `get_selected_actors` for "
			"convenience; prefer this tool for first-call orientation.";
		d.input_schema = {
			{"type", "object"},
			{"properties", nlohmann::json::object()},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json&) {
			return reader.GetEditorState();
		});
	}

	// ----- run_python_script ----------------------------------------------
	// Code-as-universal-tool capability (inspired by Epic AIAssistant's
	// ExecPythonCommandEx surface). Gated server-side by
	// BP_READER_ALLOW_PYTHON=1 — off by default because arbitrary Python
	// in the editor bypasses every safety convention the curated tool
	// surface establishes.
	{
		ToolDescriptor d;
		d.name = "run_python_script";
		d.description =
			"[editor] Execute an Unreal Python script in the live editor. "
			"Wrapped in a single undo transaction so mutations are "
			"reversible. **Gated:** the MCP server's process env must "
			"include `BP_READER_ALLOW_PYTHON=1`. Off by default — "
			"arbitrary Python has full access to the `unreal.*` API and "
			"bypasses the curated tool surface. When disabled, returns "
			"`{ok: false, error: 'python_disabled', hint: ...}`. Returns "
			"the captured stdout/stderr in `log[]` plus the script's "
			"command result. For curated authoring, prefer the named BP/"
			"asset/material/widget tools.";
		d.input_schema = {
			{"type", "object"},
			{"properties", {
				{"code", {
					{"type", "string"},
					{"description",
					 "Unreal Python source. Can be multi-line. Has access "
					 "to the full `unreal.*` API."},
				}},
			}},
			{"required", nlohmann::json::array({"code"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string code = RequireString(args, "code");
			auto r = reader.RunPythonScript(code);
			nlohmann::json out = {
				{"ok", r.ok},
				{"command_result", r.commandResult},
				{"log", r.log.is_null() ? nlohmann::json::array() : r.log},
			};
			if (!r.error.empty()) {
				out["error"] = r.error;
				if (r.error == "python_disabled") {
					out["hint"] =
						"Set BP_READER_ALLOW_PYTHON=1 in the MCP server's env "
						"to enable. Off by default for safety.";
				}
			}
			return out;
		});
	}

	// ----- get_referencers / get_dependencies (asset-graph queries) ------
	// "What uses this asset?" / "What does this asset use?". Sourced from
	// the asset registry's in-memory dependency graph — O(1) per query.
	{
		ToolDescriptor d;
		d.name = "get_referencers";
		d.description =
			"[asset] Return every asset (package path) that REFERENCES "
			"this asset. Useful for impact analysis before renaming, "
			"deleting, or refactoring. Sourced from the asset registry's "
			"dependency graph (in-memory; fast). Inverse of "
			"`get_dependencies`.";
		d.input_schema = {
			{"type", "object"},
			{"properties", {{"asset_path", {{"type", "string"}}}}},
			{"required", nlohmann::json::array({"asset_path"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string ap = RequireString(args, "asset_path");
			auto r = reader.GetReferencers(ap);
			return nlohmann::json{
				{"ok", true},
				{"asset_path", ap},
				{"referencers", r.packagePaths},
				{"count", r.packagePaths.size()},
			};
		});
	}
	{
		ToolDescriptor d;
		d.name = "get_dependencies";
		d.description =
			"[asset] Return every asset (package path) that this asset "
			"DEPENDS on. Inverse of `get_referencers`. Useful for "
			"understanding what loads when this asset loads.";
		d.input_schema = {
			{"type", "object"},
			{"properties", {{"asset_path", {{"type", "string"}}}}},
			{"required", nlohmann::json::array({"asset_path"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string ap = RequireString(args, "asset_path");
			auto r = reader.GetDependencies(ap);
			return nlohmann::json{
				{"ok", true},
				{"asset_path", ap},
				{"dependencies", r.packagePaths},
				{"count", r.packagePaths.size()},
			};
		});
	}

	// ----- read_config_value / set_config_value (.ini editing) -----------
	// Project settings via GConfig — DefaultEngine.ini, DefaultGame.ini,
	// etc. Writes flush to disk on success.
	{
		ToolDescriptor d;
		d.name = "read_config_value";
		d.description =
			"[asset] Read a UE config (.ini) value. `file` is one of "
			"\"Engine\" (default), \"Game\", \"Input\", \"Editor\", "
			"\"EditorPerProjectIni\", or a full path. Returns "
			"`{exists, value}` — `exists:false` if the key isn't set "
			"in any branch of the resolved file.";
		d.input_schema = {
			{"type", "object"},
			{"properties", {
				{"section", {{"type", "string"},
							 {"description", "Ini section, e.g. \"/Script/Engine.RendererSettings\""}}},
				{"key",     {{"type", "string"}}},
				{"file",    {{"type", "string"},
							 {"description", "Engine|Game|Input|Editor|EditorPerProjectIni, or full path. Defaults to Engine."}}},
			}},
			{"required", nlohmann::json::array({"section", "key"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string section = RequireString(args, "section");
			std::string key     = RequireString(args, "key");
			std::string file    = args.contains("file") && args["file"].is_string()
									  ? args["file"].get<std::string>() : "";
			auto r = reader.ReadConfigValue(section, key, file);
			nlohmann::json out = {
				{"ok", true},
				{"section", section},
				{"key", key},
				{"file", file.empty() ? std::string("Engine") : file},
				{"exists", r.exists},
			};
			if (r.exists)
			{
				out["value"] = r.value;
			}
			return out;
		});
	}
	{
		ToolDescriptor d;
		d.name = "set_config_value";
		d.description =
			"[asset] Write a UE config (.ini) value + flush to disk. "
			"`file` defaults to Engine (DefaultEngine.ini). Returns "
			"`{previous_value, value}` so the caller can verify the "
			"change. **Destructive** — writes the project's ini file "
			"on disk. Use only when the agent has explicit intent to "
			"modify project settings.";
		d.input_schema = {
			{"type", "object"},
			{"properties", {
				{"section", {{"type", "string"}}},
				{"key",     {{"type", "string"}}},
				{"value",   {{"type", "string"}}},
				{"file",    {{"type", "string"}}},
			}},
			{"required", nlohmann::json::array({"section", "key", "value"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string section = RequireString(args, "section");
			std::string key     = RequireString(args, "key");
			std::string value   = RequireString(args, "value");
			std::string file    = args.contains("file") && args["file"].is_string()
									  ? args["file"].get<std::string>() : "";
			auto r = reader.SetConfigValue(section, key, value, file);
			nlohmann::json out = {
				{"ok", true},
				{"section", section},
				{"key", key},
				{"file", file.empty() ? std::string("Engine") : file},
				{"value", value},
			};
			if (r.previousExisted)
			{
				out["previous_value"] = r.previousValue;
			}
			else                   out["previous_value"] = nullptr;
			return out;
		});
	}

	// ----- build_lighting ------------------------------------------------
	{
		ToolDescriptor d;
		d.name = "build_lighting";
		d.description =
			"[editor] Trigger a lighting build (Lightmass) on the "
			"currently-loaded level. Async — returns once the build is "
			"QUEUED, not once it finishes. `quality` is one of "
			"\"Preview\", \"Medium\", \"High\", \"Production\" (default "
			"Production). Poll `read_output_log` for completion: "
			"Lightmass emits \"Lighting build complete\" or "
			"\"Lighting build failed\" when it finishes. Live-editor only.";
		d.input_schema = {
			{"type", "object"},
			{"properties", {
				{"quality", {{"type", "string"},
							 {"enum", nlohmann::json::array({
								 "Preview", "Medium", "High", "Production"})}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string quality = args.contains("quality") && args["quality"].is_string()
									  ? args["quality"].get<std::string>()
									  : std::string("Production");
			auto r = reader.BuildLighting(quality);
			return nlohmann::json{
				{"ok", true},
				{"queued", r.queued},
				{"quality", r.quality},
				{"note", "Async — poll read_output_log for completion."},
			};
		});
	}

	// ----- get_cvar -------------------------------------------------------
	{
		ToolDescriptor d;
		d.name = "get_cvar";
		d.description =
			"[editor] Read a console variable's current value. Returns "
			"`{exists, value, help}` — `exists:false` if the CVar isn't "
			"registered.";
		d.input_schema = {
			{"type","object"},
			{"properties", {{"name", {{"type","string"}}}}},
			{"required", nlohmann::json::array({"name"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string name = RequireString(args, "name");
			auto v = reader.GetCVar(name);
			return nlohmann::json{
				{"ok", true},
				{"name",   v.name}, {"value", v.value},
				{"help",   v.help}, {"exists", v.exists},
			};
		});
	}

	// ----- set_cvar -------------------------------------------------------
	{
		ToolDescriptor d;
		d.name = "set_cvar";
		d.description =
			"[editor] Set a UE console variable (e.g. `r.ScreenPercentage`). Runtime-style; for Blueprint variable defaults use `set_variable_default`. Forces `ECVF_SetByCode` priority — "
			"overrides values set from ini files / scalability settings.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"name",  {{"type","string"}}},
				{"value", {{"type","string"}, {"description","Stringified value; UE coerces to the CVar's type."}}},
			}},
			{"required", nlohmann::json::array({"name","value"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string name  = RequireString(args, "name");
			std::string value = RequireString(args, "value");
			auto v = reader.SetCVar(name, value);
			return nlohmann::json{
				{"ok", true},
				{"name", v.name}, {"value", v.value}, {"exists", v.exists},
			};
		});
	}

	// ----- pie_start / pie_stop ------------------------------------------
	{
		ToolDescriptor d;
		d.name = "pie_start";
		d.description =
			"[editor] Start a Play-In-Editor session. `mode` is one of "
			"`selected_viewport` (default), `new_editor_window`, "
			"`standalone`, `vr_preview`. Most useful with the live backend.";
		d.input_schema = {
			{"type","object"},
			{"properties", {{"mode", {{"type","string"},
				{"enum", nlohmann::json::array({"selected_viewport","new_editor_window","standalone","vr_preview"})}}}}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string mode = OptString(args, "mode", "selected_viewport");
			auto r = reader.PieStart(mode);
			return nlohmann::json{{"ok", true},
								  {"started", r.started}, {"mode", r.mode}};
		});
	}
	{
		ToolDescriptor d;
		d.name = "pie_stop";
		d.description =
			"[editor] End the active PIE session. No-op when PIE isn't running.";
		d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
		registry.Add(std::move(d), [&reader](const nlohmann::json&) {
			auto r = reader.PieStop();
			return nlohmann::json{{"ok", true}, {"stopped", r.stopped}};
		});
	}

	// ----- live_coding_compile -------------------------------------------
	{
		ToolDescriptor d;
		d.name = "live_coding_compile";
		d.description =
			"[editor] Trigger UE's Live Coding compile + patch. The compile runs "
			"asynchronously; Live Coding emits its own progress + result "
			"to the editor log (use `read_output_log` to follow).";
		d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
		registry.Add(std::move(d), [&reader](const nlohmann::json&) {
			auto r = reader.LiveCodingCompile();
			return nlohmann::json{{"ok", true},
								  {"queued", r.queued}, {"message", r.message}};
		});
	}

	// ----- get_selected_actors -------------------------------------------
	{
		ToolDescriptor d;
		d.name = "get_selected_actors";
		d.description =
			"[editor] List the names of currently-selected actors in the level editor. "
			"Names are the stable in-package names (not display labels). "
			"Empty array when nothing is selected.";
		d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
		registry.Add(std::move(d), [&reader](const nlohmann::json&) {
			auto r = reader.GetSelectedActors();
			return nlohmann::json{{"ok", true}, {"actor_names", r.actorNames}};
		});
	}

	// ----- set_selection --------------------------------------------------
	{
		ToolDescriptor d;
		d.name = "set_selection";
		d.description =
			"[editor] Replace (or extend) the editor viewport's actor selection. `replace:true` "
			"(default) clears existing selection first; `false` adds to it. "
			"Returns the post-call selected names so the caller can verify.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"actor_names", {{"type","array"}, {"items", {{"type","string"}}}}},
				{"replace",     {{"type","boolean"}}},
			}},
			{"required", nlohmann::json::array({"actor_names"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::vector<std::string> names;
			if (auto it = args.find("actor_names"); it != args.end() && it->is_array()) {
				for (const auto& v : *it) {
					if (v.is_string())
					{
						names.push_back(v.get<std::string>());
					}
				}
			}
			bool replace = args.value("replace", true);
			auto r = reader.SetSelection(names, replace);
			return nlohmann::json{{"ok", true}, {"actor_names", r.actorNames}};
		});
	}

	// ----- spawn_actor ----------------------------------------------------
	{
		ToolDescriptor d;
		d.name = "spawn_actor";
		d.description =
			"[editor] Spawn an actor of the given UClass in the current level. "
			"`class_path` is the full path (e.g. `/Script/Engine.StaticMeshActor` "
			"or a BP class like `/Game/AI/BP_Enemy.BP_Enemy_C`). All transform "
			"fields are optional and default to identity.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"class_path", {{"type","string"}}},
				{"location",   {{"type","object"}, {"properties", {
					{"x",{{"type","number"}}}, {"y",{{"type","number"}}}, {"z",{{"type","number"}}}}}}},
				{"rotation",   {{"type","object"}, {"properties", {
					{"pitch",{{"type","number"}}}, {"yaw",{{"type","number"}}}, {"roll",{{"type","number"}}}}}}},
				{"scale",      {{"type","object"}, {"properties", {
					{"x",{{"type","number"}}}, {"y",{{"type","number"}}}, {"z",{{"type","number"}}}}}}},
			}},
			{"required", nlohmann::json::array({"class_path"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string cls = RequireString(args, "class_path");
			auto loc   = args.value("location", nlohmann::json::object());
			auto rot   = args.value("rotation", nlohmann::json::object());
			auto scl   = args.value("scale",    nlohmann::json::object());
			auto r = reader.SpawnActor(cls,
				loc.value("x", 0.0), loc.value("y", 0.0), loc.value("z", 0.0),
				rot.value("pitch", 0.0), rot.value("yaw", 0.0), rot.value("roll", 0.0),
				scl.value("x", 1.0), scl.value("y", 1.0), scl.value("z", 1.0));
			return nlohmann::json{{"ok", true},
								  {"actor_name",  r.actorName},
								  {"actor_label", r.actorLabel}};
		});
	}

	// ----- set_actor_transform -------------------------------------------
	{
		ToolDescriptor d;
		d.name = "set_actor_transform";
		d.description =
			"[editor] Update a placed actor's world transform in the current level. `actor_name` is from "
			"`get_selected_actors` or `spawn_actor`'s response. All transform "
			"fields are absolute (not delta).";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"actor_name", {{"type","string"}}},
				{"location",   {{"type","object"}}},
				{"rotation",   {{"type","object"}}},
				{"scale",      {{"type","object"}}},
			}},
			{"required", nlohmann::json::array({"actor_name"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string name = RequireString(args, "actor_name");
			auto loc = args.value("location", nlohmann::json::object());
			auto rot = args.value("rotation", nlohmann::json::object());
			auto scl = args.value("scale",    nlohmann::json::object());
			reader.SetActorTransform(name,
				loc.value("x", 0.0), loc.value("y", 0.0), loc.value("z", 0.0),
				rot.value("pitch", 0.0), rot.value("yaw", 0.0), rot.value("roll", 0.0),
				scl.value("x", 1.0), scl.value("y", 1.0), scl.value("z", 1.0));
			return nlohmann::json{{"ok", true}, {"actor_name", name}};
		});
	}

	// ----- delete_actor --------------------------------------------------
	{
		ToolDescriptor d;
		d.name = "delete_actor";
		d.description =
			"[editor] Destroy an actor by name. Returns `{deleted: false}` if the "
			"actor wasn't found.";
		d.input_schema = {
			{"type","object"},
			{"properties", {{"actor_name", {{"type","string"}}}}},
			{"required", nlohmann::json::array({"actor_name"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string name = RequireString(args, "actor_name");
			auto r = reader.DeleteActor(name);
			return nlohmann::json{{"ok", true},
								  {"actor_name", name}, {"deleted", r.deleted}};
		});
	}

	// ----- read_output_log -----------------------------------------------
	{
		ToolDescriptor d;
		d.name = "read_output_log";
		d.description =
			"[editor] Read recent entries from the editor's output log. The plugin "
			"module installs a ring-buffer log sink at startup; this returns "
			"up to `limit` of the most recent entries (default 200), "
			"optionally filtered by `min_severity` (Display / Log / Warning "
			"/ Error / Fatal).";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"limit",         {{"type","integer"}}},
				{"min_severity",  {{"type","string"},
					{"enum", nlohmann::json::array({"Display","Log","Warning","Error","Fatal"})}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			int limit = OptInt(args, "limit", 200);
			std::string minSev = OptString(args, "min_severity", "");
			auto r = reader.ReadOutputLog(limit, minSev);
			nlohmann::json entries = nlohmann::json::array();
			for (const auto& e : r.entries) {
				entries.push_back(nlohmann::json{
					{"severity",  e.severity},
					{"category",  e.category},
					{"message",   e.message},
					{"timestamp", e.timestamp},
				});
			}
			return nlohmann::json{{"ok", true}, {"entries", entries}};
		});
	}

	// ----- add_data_row ---------------------------------------------------
	{
		ToolDescriptor d;
		d.name = "add_data_row";
		d.description =
			"[data table] Add a row to an existing DataTable. The row name must be "
			"unique within the table; existing names return "
			"`{already_existed:true}` unless `overwrite:true` is passed. "
			"`values` is an object whose keys map to the row struct's "
			"field names; values are stringified and coerced via "
			"FProperty::ImportText (works for scalars, enums, and structs "
			"that round-trip through text). Pair with `read_data_table` to "
			"see the row-struct shape before calling.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path", {{"type","string"}}},
				{"row_name",   {{"type","string"}}},
				{"values",     {{"type","object"},
								{"description","Field-name → value map. Values are stringified; ImportText coerces to the property's type."}}},
				{"overwrite",  {{"type","boolean"},
								{"description","Default false. Set true to replace an existing row."}}},
			}},
			{"required", nlohmann::json::array({"asset_path","row_name","values"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireString(args, "asset_path");
			std::string row   = RequireString(args, "row_name");
			nlohmann::json values = args.value("values", nlohmann::json::object());
			bool overwrite = args.value("overwrite", false);
			auto r = reader.AddDataRow(asset, row, values, overwrite);
			return nlohmann::json{
				{"ok", true},
				{"asset_path",      r.assetPath},
				{"row_name",        r.rowName},
				{"already_existed", r.alreadyExisted},
				{"created",         r.created},
			};
		});
	}

	// ----- set_data_row_value --------------------------------------------
	{
		ToolDescriptor d;
		d.name = "set_data_row_value";
		d.description =
			"[data table] Update a single field on an existing DataTable row. "
			"`field_name` must match a property on the row struct; "
			"`value` is its string form (ImportText input). Returns the "
			"pre-set and post-set ExportText'd values so the caller can "
			"verify the coercion landed.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path", {{"type","string"}}},
				{"row_name",   {{"type","string"}}},
				{"field_name", {{"type","string"}}},
				{"value",      {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"asset_path","row_name","field_name","value"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireString(args, "asset_path");
			std::string row   = RequireString(args, "row_name");
			std::string field = RequireString(args, "field_name");
			std::string value = RequireString(args, "value");
			auto r = reader.SetDataRowValue(asset, row, field, value);
			return nlohmann::json{
				{"ok", true},
				{"asset_path", r.assetPath},
				{"row_name",   r.rowName},
				{"field_name", r.fieldName},
				{"old_value",  r.oldValue},
				{"new_value",  r.newValue},
			};
		});
	}

	// ----- add_component / remove_component / attach_component /
	//       set_component_property ------------------------------------
	//
	// BP component authoring: SCS (SimpleConstructionScript) tree
	// manipulation + property edits on component templates.
	{
		ToolDescriptor d;
		d.name = "add_component";
		d.description =
			"[blueprint] Add a component (StaticMeshComponent, AudioComponent, etc.) to a Blueprint's SimpleConstructionScript tree — author-time component setup, not runtime spawning. For runtime actor spawning use `spawn_actor`. "
			"`component_class` is the full UClass path (e.g. "
			"`/Script/Engine.StaticMeshComponent`). Pass `parent` to attach "
			"as a child of an existing node; omit for root attachment. "
			"`socket` applies to SceneComponent children only. Idempotent on "
			"`name`: existing names return `{already_existed:true}`.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path",      {{"type","string"}}},
				{"name",            {{"type","string"}}},
				{"component_class", {{"type","string"}}},
				{"parent",          {{"type","string"}}},
				{"socket",          {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"asset_path","name","component_class"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireString(args, "asset_path");
			std::string name  = RequireString(args, "name");
			std::string cls   = RequireString(args, "component_class");
			std::string parent = OptString(args, "parent", "");
			std::string socket = OptString(args, "socket", "");
			auto r = reader.AddComponent(asset, name, cls, parent, socket);
			return nlohmann::json{
				{"ok", true},
				{"asset_path",       r.assetPath},
				{"name",             r.name},
				{"component_class",  r.componentClass},
				{"already_existed",  r.alreadyExisted},
				{"created",          r.created},
			};
		});
	}
	{
		ToolDescriptor d;
		d.name = "remove_component";
		d.description =
			"[blueprint] Remove a component from a Blueprint's SCS tree by name. "
			"Returns `{removed:false}` when the component isn't found.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path", {{"type","string"}}},
				{"name",       {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"asset_path","name"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireString(args, "asset_path");
			std::string name  = RequireString(args, "name");
			auto r = reader.RemoveComponent(asset, name);
			return nlohmann::json{
				{"ok", true},
				{"asset_path", r.assetPath},
				{"name",       r.name},
				{"removed",    r.removed},
			};
		});
	}
	{
		ToolDescriptor d;
		d.name = "attach_component";
		d.description =
			"[blueprint] Re-parent an SCS component on a Blueprint. Pass `new_parent` to attach the "
			"component as a child of that node; pass empty to attach at "
			"the SCS root. `socket` applies to SceneComponent children "
			"only.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path", {{"type","string"}}},
				{"name",       {{"type","string"}}},
				{"new_parent", {{"type","string"}}},
				{"socket",     {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"asset_path","name"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset  = RequireString(args, "asset_path");
			std::string name   = RequireString(args, "name");
			std::string parent = OptString(args, "new_parent", "");
			std::string socket = OptString(args, "socket", "");
			auto r = reader.AttachComponent(asset, name, parent, socket);
			return nlohmann::json{
				{"ok", true},
				{"asset_path",     r.assetPath},
				{"name",           r.name},
				{"new_parent",     r.newParentName},
				{"socket",         r.socket},
				{"reparented",     r.reparented},
			};
		});
	}
	{
		ToolDescriptor d;
		d.name = "set_component_property";
		d.description =
			"[blueprint] Set a UPROPERTY on a Blueprint component's template "
			"(the author-time default values, what the BP Details panel shows "
			"for that component). For widget UPROPERTYs use "
			"`set_widget_property`; for behavior tree nodes use "
			"`set_bt_node_property`. Same string→type coercion as "
			"`set_data_row_value` (FProperty::ImportText). Returns "
			"pre-set and post-set ExportText'd values for verification.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path",    {{"type","string"}}},
				{"component",     {{"type","string"}}},
				{"property",      {{"type","string"}}},
				{"value",         {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"asset_path","component","property","value"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireString(args, "asset_path");
			std::string comp  = RequireString(args, "component");
			std::string prop  = RequireString(args, "property");
			std::string value = RequireString(args, "value");
			auto r = reader.SetComponentProperty(asset, comp, prop, value);
			return nlohmann::json{
				{"ok", true},
				{"asset_path",    r.assetPath},
				{"component",     r.componentName},
				{"property",      r.propertyName},
				{"old_value",     r.oldValue},
				{"new_value",     r.newValue},
			};
		});
	}

	// ----- run_automation_tests ------------------------------------------
	{
		ToolDescriptor d;
		d.name = "run_automation_tests";
		d.description =
			"[tests] Trigger UE's automation test framework. `pattern` is the "
			"test-name wildcard (e.g. `BlueprintReader.*`, `*Smoke*`); empty "
			"means every registered test. The run is async — this tool "
			"kicks it off and returns. Use `read_output_log` to follow "
			"results, or check `Saved/Automation/index.json` after for the "
			"structured report.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"pattern", {{"type","string"},
							 {"description","Test-name wildcard. Empty = all tests."}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string pattern = OptString(args, "pattern", "");
			auto r = reader.RunAutomationTests(pattern);
			return nlohmann::json{
				{"ok", true},
				{"started", r.started},
				{"message", r.message},
			};
		});
	}

	// ===== Material authoring (Stage 1) ====================================
	// The material expression graph is a separate UObject tree from
	// Blueprint event graphs — `ReadMaterial` returns expression nodes +
	// their connections + parameter names. Writes (add_expression,
	// connect, set_parameter, compile) mutate the UMaterial directly;
	// mark dirty + SavePackage after if you want it to persist on disk.

	// ----- list_materials ------------------------------------------------
	{
		ToolDescriptor d;
		d.name = "list_materials";
		d.description =
			"[material] List all UMaterial / UMaterialInstance assets under a content "
			"path. Mirrors `list_blueprints` but filters by class. Defaults "
			"to `/Game`.";
		d.input_schema = {
			{"type","object"},
			{"properties", {{"path", {{"type","string"}}}}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string path = OptString(args, "path", "/Game");
			auto summaries = reader.ListMaterials(path);
			nlohmann::json arr = nlohmann::json::array();
			for (const auto& s : summaries)
			{
				arr.push_back(s);
			}
			return arr;
		});
	}

	// ----- read_material -------------------------------------------------
	{
		ToolDescriptor d;
		d.name = "read_material";
		d.description =
			"[material] Read a material's expression graph: every UMaterialExpression "
			"node (id, class, parameter name, x/y), every connection (from "
			"expression output → expression input or master-material slot "
			"like BaseColor / Roughness), and the names of all exposed "
			"scalar/vector parameters.";
		d.input_schema = {
			{"type","object"},
			{"properties", {{"asset_path", {{"type","string"}}}}},
			{"required", nlohmann::json::array({"asset_path"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireString(args, "asset_path");
			auto m = reader.ReadMaterial(asset);
			nlohmann::json exprs = nlohmann::json::array();
			for (const auto& e : m.expressions) {
				exprs.push_back({
					{"id", e.id}, {"class", e.className},
					{"parameter_name", e.parameterName},
					{"x", e.x}, {"y", e.y},
				});
			}
			nlohmann::json conns = nlohmann::json::array();
			for (const auto& c : m.connections) {
				conns.push_back({
					{"from_node", c.fromNodeId}, {"from_pin", c.fromPin},
					{"to_node",   c.toNodeId},   {"to_pin",   c.toPin},
				});
			}
			return nlohmann::json{
				{"ok", true},
				{"asset_path",      m.assetPath},
				{"expressions",     exprs},
				{"connections",     conns},
				{"parameter_names", m.parameterNames},
			};
		});
	}

	// ----- add_material_expression ---------------------------------------
	{
		ToolDescriptor d;
		d.name = "add_material_expression";
		d.description =
			"[material] Add a UMaterialExpression node to a Material graph. For Blueprint graph nodes use `add_node`. `expression_class` "
			"is the short class name like `MaterialExpressionConstant3Vector`, "
			"`MaterialExpressionScalarParameter`, "
			"`MaterialExpressionTextureSampleParameter2D`. x/y are graph "
			"coordinates. Returns the new expression's id (use in "
			"`connect_material_expressions`).";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path",       {{"type","string"}}},
				{"expression_class", {{"type","string"}}},
				{"x", {{"type","integer"}}},
				{"y", {{"type","integer"}}},
			}},
			{"required", nlohmann::json::array(
				{"asset_path","expression_class"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireString(args, "asset_path");
			std::string cls   = RequireString(args, "expression_class");
			int x = args.value("x", 0);
			int y = args.value("y", 0);
			auto r = reader.AddMaterialExpression(asset, cls, x, y);
			return nlohmann::json{
				{"ok", true},
				{"asset_path",    r.assetPath},
				{"expression_id", r.expressionId},
				{"class",         r.className},
			};
		});
	}

	// ----- connect_material_expressions ----------------------------------
	{
		ToolDescriptor d;
		d.name = "connect_material_expressions";
		d.description =
			"[material] Wire one expression's output pin to another expression's input "
			"pin, or to a master-material slot. Pass empty `to_node` to wire "
			"to a master slot (`to_pin` then names the slot, e.g. "
			"`BaseColor`, `Metallic`, `Roughness`, `EmissiveColor`, `Normal`).";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path", {{"type","string"}}},
				{"from_node",  {{"type","string"}}},
				{"from_pin",   {{"type","string"}}},
				{"to_node",    {{"type","string"}}},
				{"to_pin",     {{"type","string"}}},
			}},
			{"required", nlohmann::json::array(
				{"asset_path","from_node","from_pin","to_pin"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireString(args, "asset_path");
			std::string fn = RequireString(args, "from_node");
			std::string fp = RequireString(args, "from_pin");
			std::string tn = OptString(args, "to_node", "");
			std::string tp = RequireString(args, "to_pin");
			auto r = reader.ConnectMaterialExpressions(asset, fn, fp, tn, tp);
			return nlohmann::json{
				{"ok", true},
				{"asset_path", r.assetPath},
				{"connected",  r.connected},
			};
		});
	}

	// ----- set_material_parameter ----------------------------------------
	{
		ToolDescriptor d;
		d.name = "set_material_parameter";
		d.description =
			"[material] Set the default value of a named scalar/vector "
			"parameter on a UMaterial (the base material). `value` is the "
			"parameter's text representation (scalar: `0.5`; vector: "
			"`(R=1,G=0,B=0,A=1)`). For per-instance overrides use "
			"`set_material_instance_parameter`.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path",     {{"type","string"}}},
				{"parameter_name", {{"type","string"}}},
				{"value",          {{"type","string"}}},
			}},
			{"required", nlohmann::json::array(
				{"asset_path","parameter_name","value"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireString(args, "asset_path");
			std::string name  = RequireString(args, "parameter_name");
			std::string value = RequireString(args, "value");
			auto r = reader.SetMaterialParameter(asset, name, value);
			return nlohmann::json{
				{"ok", true},
				{"asset_path",     r.assetPath},
				{"parameter_name", r.parameterName},
				{"old_value",      r.oldValue},
				{"new_value",      r.newValue},
			};
		});
	}

	// ----- set_material_instance_parameter -------------------------------
	{
		ToolDescriptor d;
		d.name = "set_material_instance_parameter";
		d.description =
			"[material] Override a parameter on a UMaterialInstanceConstant. For base-material defaults use `set_material_parameter`. `type` is "
			"`scalar`, `vector`, or `texture`; `value` is its text form "
			"(scalar `0.5`, vector `(R=...,G=...,B=...,A=...)`, texture "
			"`/Game/Textures/T_Foo.T_Foo`).";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path",     {{"type","string"}}},
				{"parameter_name", {{"type","string"}}},
				{"type",           {{"type","string"}}},
				{"value",          {{"type","string"}}},
			}},
			{"required", nlohmann::json::array(
				{"asset_path","parameter_name","type","value"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireString(args, "asset_path");
			std::string name  = RequireString(args, "parameter_name");
			std::string type  = RequireString(args, "type");
			std::string value = RequireString(args, "value");
			auto r = reader.SetMaterialInstanceParameter(asset, name, type, value);
			return nlohmann::json{
				{"ok", true},
				{"asset_path",     r.assetPath},
				{"parameter_name", r.parameterName},
				{"type",           r.paramType},
				{"new_value",      r.newValue},
			};
		});
	}

	// ----- compile_material ----------------------------------------------
	{
		ToolDescriptor d;
		d.name = "compile_material";
		d.description =
			"[material] Recompile a material's shader code. UE normally compiles "
			"incrementally on edit; call this explicitly to flush pending "
			"recompiles or recover from a stuck shader compile state.";
		d.input_schema = {
			{"type","object"},
			{"properties", {{"asset_path", {{"type","string"}}}}},
			{"required", nlohmann::json::array({"asset_path"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireString(args, "asset_path");
			auto r = reader.CompileMaterial(asset);
			return nlohmann::json{
				{"ok", true},
				{"asset_path", r.assetPath},
				{"compiled",   r.compiled},
			};
		});
	}

	// ===== UMG widget authoring (Stage 1) ==================================
	// UMG widget blueprints store their hierarchy in a UWidgetTree rather
	// than a USimpleConstructionScript — different shape from actor
	// components, so they get their own tool surface.

	// ----- read_widget_blueprint -----------------------------------------
	{
		ToolDescriptor d;
		d.name = "read_widget_blueprint";
		d.description =
			"[widget] Read a UWidgetBlueprint's widget tree: every UWidget node "
			"(name, class, parent name) and the root widget's name. "
			"Mirrors `get_components` but for UMG.";
		d.input_schema = {
			{"type","object"},
			{"properties", {{"asset_path", {{"type","string"}}}}},
			{"required", nlohmann::json::array({"asset_path"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireString(args, "asset_path");
			auto w = reader.ReadWidgetBlueprint(asset);
			nlohmann::json nodes = nlohmann::json::array();
			for (const auto& n : w.nodes) {
				nodes.push_back({
					{"name",   n.name},
					{"class",  n.className},
					{"parent", n.parentName},
				});
			}
			return nlohmann::json{
				{"ok", true},
				{"asset_path", w.assetPath},
				{"root_name",  w.rootName},
				{"nodes",      nodes},
			};
		});
	}

	// ----- add_widget ----------------------------------------------------
	{
		ToolDescriptor d;
		d.name = "add_widget";
		d.description =
			"[widget] Add a UWidget node (Button, TextBlock, etc.) to a UWidgetBlueprint's UMG tree. For Blueprint graph nodes use `add_node`. `widget_class` is the "
			"short class name (`Button`, `TextBlock`, `Image`, `VerticalBox`, "
			"etc.). `parent_name` empty = becomes the new root (replaces "
			"the existing root only if the tree was empty). Otherwise "
			"appends as a child of `parent_name`.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path",   {{"type","string"}}},
				{"parent_name",  {{"type","string"}}},
				{"widget_class", {{"type","string"}}},
				{"name",         {{"type","string"}}},
			}},
			{"required", nlohmann::json::array(
				{"asset_path","widget_class","name"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset  = RequireString(args, "asset_path");
			std::string parent = OptString(args, "parent_name", "");
			std::string cls    = RequireString(args, "widget_class");
			std::string name   = RequireString(args, "name");
			auto r = reader.AddWidget(asset, parent, cls, name);
			return nlohmann::json{
				{"ok", true},
				{"asset_path",      r.assetPath},
				{"name",            r.name},
				{"widget_class",    r.widgetClass},
				{"already_existed", r.alreadyExisted},
				{"created",         r.created},
			};
		});
	}

	// ----- set_widget_property -------------------------------------------
	{
		ToolDescriptor d;
		d.name = "set_widget_property";
		d.description =
			"[widget] Set a UPROPERTY on a UWidget in a UWidgetBlueprint. For Blueprint component UPROPERTYs use `set_component_property`. `property_name` "
			"is the property's name as authored in C++ (`Text`, "
			"`ColorAndOpacity`, `Visibility`). `value` is the property's text "
			"form (text: a string; FLinearColor: `(R=1,G=0,B=0,A=1)`).";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path",    {{"type","string"}}},
				{"widget_name",   {{"type","string"}}},
				{"property_name", {{"type","string"}}},
				{"value",         {{"type","string"}}},
			}},
			{"required", nlohmann::json::array(
				{"asset_path","widget_name","property_name","value"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireString(args, "asset_path");
			std::string w     = RequireString(args, "widget_name");
			std::string p     = RequireString(args, "property_name");
			std::string v     = RequireString(args, "value");
			auto r = reader.SetWidgetProperty(asset, w, p, v);
			return nlohmann::json{
				{"ok", true},
				{"asset_path",    r.assetPath},
				{"widget_name",   r.widgetName},
				{"property_name", r.propertyName},
				{"old_value",     r.oldValue},
				{"new_value",     r.newValue},
			};
		});
	}

	// ----- bind_widget_event ---------------------------------------------
	{
		ToolDescriptor d;
		d.name = "bind_widget_event";
		d.description =
			"[widget] Bind a widget's event (e.g. `OnClicked` on a Button) to a "
			"named handler function in the widget blueprint's graph. If the "
			"handler function doesn't exist, it's created with the event's "
			"signature. Pairs with `add_function` if you want to author the "
			"handler explicitly first.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path",       {{"type","string"}}},
				{"widget_name",      {{"type","string"}}},
				{"event_name",       {{"type","string"}}},
				{"handler_function", {{"type","string"}}},
			}},
			{"required", nlohmann::json::array(
				{"asset_path","widget_name","event_name","handler_function"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireString(args, "asset_path");
			std::string w     = RequireString(args, "widget_name");
			std::string e     = RequireString(args, "event_name");
			std::string h     = RequireString(args, "handler_function");
			auto r = reader.BindWidgetEvent(asset, w, e, h);
			return nlohmann::json{
				{"ok", true},
				{"asset_path",       r.assetPath},
				{"widget_name",      r.widgetName},
				{"event_name",       r.eventName},
				{"handler_function", r.handlerFunction},
				{"bound",            r.bound},
			};
		});
	}

	// ----- compile_widget_blueprint --------------------------------------
	{
		ToolDescriptor d;
		d.name = "compile_widget_blueprint";
		d.description =
			"[widget] Compile a UWidgetBlueprint. Equivalent to clicking Compile in "
			"the UMG designer. Returns `{compiled: true|false}` — false "
			"means compile failed; check `read_output_log` for errors.";
		d.input_schema = {
			{"type","object"},
			{"properties", {{"asset_path", {{"type","string"}}}}},
			{"required", nlohmann::json::array({"asset_path"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireString(args, "asset_path");
			auto r = reader.CompileWidgetBlueprint(asset);
			return nlohmann::json{
				{"ok", true},
				{"asset_path", r.assetPath},
				{"compiled",   r.compiled},
			};
		});
	}

	// ===== Behavior Tree authoring (Stage 2) ===============================
	// Behavior Trees are AIModule UObjects: root composite + decorators +
	// services + tasks. node_kind = "composite" | "decorator" | "service"
	// | "task". Node ids are stable UObject names within the tree.

	{
		ToolDescriptor d;
		d.name = "list_behavior_trees";
		d.description = "[behavior tree] List UBehaviorTree assets under a content path "
						"(default `/Game`).";
		d.input_schema = {
			{"type","object"},
			{"properties", {{"path", {{"type","string"}}}}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string path = OptString(args, "path", "/Game");
			auto summaries = reader.ListBehaviorTrees(path);
			nlohmann::json arr = nlohmann::json::array();
			for (const auto& s : summaries)
			{
				arr.push_back(s);
			}
			return arr;
		});
	}

	{
		ToolDescriptor d;
		d.name = "read_behavior_tree";
		d.description = "[behavior tree] Walk a UBehaviorTree's node graph. Returns every "
						"node (id, class, kind, parent) and the root node id.";
		d.input_schema = {
			{"type","object"},
			{"properties", {{"asset_path", {{"type","string"}}}}},
			{"required", nlohmann::json::array({"asset_path"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireString(args, "asset_path");
			auto bt = reader.ReadBehaviorTree(asset);
			nlohmann::json nodes = nlohmann::json::array();
			for (const auto& n : bt.nodes) {
				nodes.push_back({
					{"node_id",   n.nodeId},
					{"class",     n.className},
					{"node_kind", n.nodeKind},
					{"parent",    n.parentNodeId},
				});
			}
			return nlohmann::json{
				{"ok", true},
				{"asset_path",   bt.assetPath},
				{"root_node_id", bt.rootNodeId},
				{"nodes",        nodes},
			};
		});
	}

	{
		ToolDescriptor d;
		d.name = "add_bt_node";
		d.description = "[behavior tree] Add a node (composite/decorator/service/task) to a UBehaviorTree. NOT for Blueprint graph nodes (`add_node`). `node_kind` is "
						"`composite` / `decorator` / `service` / `task`; "
						"`node_class` is the short class name (e.g. "
						"`BTComposite_Selector`, `BTTask_MoveTo`, "
						"`BTDecorator_Blackboard`). Empty `parent_node_id` "
						"becomes the root composite (only allowed for the "
						"first composite added).";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path",     {{"type","string"}}},
				{"parent_node_id", {{"type","string"}}},
				{"node_kind",      {{"type","string"}}},
				{"node_class",     {{"type","string"}}},
			}},
			{"required", nlohmann::json::array(
				{"asset_path","node_kind","node_class"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset  = RequireString(args, "asset_path");
			std::string parent = OptString(args, "parent_node_id", "");
			std::string kind   = RequireString(args, "node_kind");
			std::string cls    = RequireString(args, "node_class");
			auto r = reader.AddBTNode(asset, parent, kind, cls);
			return nlohmann::json{
				{"ok", true},
				{"asset_path", r.assetPath},
				{"node_id",    r.nodeId},
				{"class",      r.className},
				{"node_kind",  r.nodeKind},
			};
		});
	}

	{
		ToolDescriptor d;
		d.name = "set_bt_node_property";
		d.description = "[behavior tree] Set a UPROPERTY on a UBehaviorTree node (e.g. "
						"MoveTo's `AcceptableRadius`, Blackboard decorator's "
						"`KeyName`). `value` is the property's text form.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path",    {{"type","string"}}},
				{"node_id",       {{"type","string"}}},
				{"property_name", {{"type","string"}}},
				{"value",         {{"type","string"}}},
			}},
			{"required", nlohmann::json::array(
				{"asset_path","node_id","property_name","value"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireString(args, "asset_path");
			std::string n     = RequireString(args, "node_id");
			std::string p     = RequireString(args, "property_name");
			std::string v     = RequireString(args, "value");
			auto r = reader.SetBTNodeProperty(asset, n, p, v);
			return nlohmann::json{
				{"ok", true},
				{"asset_path",    r.assetPath},
				{"node_id",       r.nodeId},
				{"property_name", r.propertyName},
				{"old_value",     r.oldValue},
				{"new_value",     r.newValue},
			};
		});
	}

	{
		ToolDescriptor d;
		d.name = "compile_behavior_tree";
		d.description = "[behavior tree] Compile a behavior tree (recompiles + marks the "
						"asset dirty). Returns `{compiled: true|false}`.";
		d.input_schema = {
			{"type","object"},
			{"properties", {{"asset_path", {{"type","string"}}}}},
			{"required", nlohmann::json::array({"asset_path"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireString(args, "asset_path");
			auto r = reader.CompileBehaviorTree(asset);
			return nlohmann::json{
				{"ok", true},
				{"asset_path", r.assetPath},
				{"compiled",   r.compiled},
			};
		});
	}

	// ===== DataAsset CRUD (Stage 2) ========================================
	// UDataAsset subclasses are pure data containers. read_data_asset
	// returns every UPROPERTY's text projection in a JSON map; mutations go
	// through ImportText_Direct/ExportText_Direct (same pattern as
	// set_component_property + set_widget_property).

	{
		ToolDescriptor d;
		d.name = "list_data_assets";
		d.description = "[data asset] List all UDataAsset subclass instances under a "
						"content path. Mirrors `list_blueprints` but "
						"filters by base class.";
		d.input_schema = {
			{"type","object"},
			{"properties", {{"path", {{"type","string"}}}}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string path = OptString(args, "path", "/Game");
			auto summaries = reader.ListDataAssets(path);
			nlohmann::json arr = nlohmann::json::array();
			for (const auto& s : summaries)
			{
				arr.push_back(s);
			}
			return arr;
		});
	}

	{
		ToolDescriptor d;
		d.name = "read_data_asset";
		d.description = "[data asset] Read every UPROPERTY on a UDataAsset. Returns the "
						"asset's class + a `{property: stringified_value}` "
						"map.";
		d.input_schema = {
			{"type","object"},
			{"properties", {{"asset_path", {{"type","string"}}}}},
			{"required", nlohmann::json::array({"asset_path"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireString(args, "asset_path");
			auto da = reader.ReadDataAsset(asset);
			return nlohmann::json{
				{"ok", true},
				{"asset_path", da.assetPath},
				{"class",      da.className},
				{"properties", da.properties},
			};
		});
	}

	{
		ToolDescriptor d;
		d.name = "create_data_asset";
		d.description = "[data asset] Create a new UDataAsset instance. `class_name` is "
						"the short C++ class name (or BP path) of a "
						"UDataAsset subclass.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path", {{"type","string"}}},
				{"class_name", {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"asset_path","class_name"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireString(args, "asset_path");
			std::string cls   = RequireString(args, "class_name");
			auto r = reader.CreateDataAsset(asset, cls);
			return nlohmann::json{
				{"ok", true},
				{"asset_path",      r.assetPath},
				{"class",           r.className},
				{"created",         r.created},
				{"already_existed", r.alreadyExisted},
			};
		});
	}

	{
		ToolDescriptor d;
		d.name = "set_data_asset_property";
		d.description = "[data asset] Set a UPROPERTY on a UDataAsset instance. `value` is the "
						"text form UE's property system uses.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path",    {{"type","string"}}},
				{"property_name", {{"type","string"}}},
				{"value",         {{"type","string"}}},
			}},
			{"required", nlohmann::json::array(
				{"asset_path","property_name","value"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireString(args, "asset_path");
			std::string p     = RequireString(args, "property_name");
			std::string v     = RequireString(args, "value");
			auto r = reader.SetDataAssetProperty(asset, p, v);
			return nlohmann::json{
				{"ok", true},
				{"asset_path",    r.assetPath},
				{"property_name", r.propertyName},
				{"old_value",     r.oldValue},
				{"new_value",     r.newValue},
			};
		});
	}

	// ===== StateTree authoring (Stage 2) ===================================
	// UStateTree (experimental in UE 5.x) — hierarchical FSM with state +
	// transition nodes. State ids are stable names within the asset.

	{
		ToolDescriptor d;
		d.name = "list_state_trees";
		d.description = "[state tree] List UStateTree assets under a content path "
						"(default `/Game`).";
		d.input_schema = {
			{"type","object"},
			{"properties", {{"path", {{"type","string"}}}}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string path = OptString(args, "path", "/Game");
			auto summaries = reader.ListStateTrees(path);
			nlohmann::json arr = nlohmann::json::array();
			for (const auto& s : summaries)
			{
				arr.push_back(s);
			}
			return arr;
		});
	}

	{
		ToolDescriptor d;
		d.name = "read_state_tree";
		d.description = "[state tree] Read a UStateTree's hierarchy + transitions: "
						"every state (id, name, parent) and every "
						"transition (from, to, trigger).";
		d.input_schema = {
			{"type","object"},
			{"properties", {{"asset_path", {{"type","string"}}}}},
			{"required", nlohmann::json::array({"asset_path"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireString(args, "asset_path");
			auto st = reader.ReadStateTree(asset);
			nlohmann::json states = nlohmann::json::array();
			for (const auto& s : st.states) {
				states.push_back({
					{"state_id", s.stateId},
					{"name",     s.name},
					{"parent",   s.parentStateId},
				});
			}
			nlohmann::json trans = nlohmann::json::array();
			for (const auto& t : st.transitions) {
				trans.push_back({
					{"from",    t.fromStateId},
					{"to",      t.toStateId},
					{"trigger", t.trigger},
				});
			}
			return nlohmann::json{
				{"ok", true},
				{"asset_path",  st.assetPath},
				{"states",      states},
				{"transitions", trans},
			};
		});
	}

	{
		ToolDescriptor d;
		d.name = "add_state_tree_state";
		d.description = "[state tree] Add a state to a UStateTree. NOT to be confused with `add_anim_state` (AnimBP state machine). Empty "
						"`parent_state_id` makes it a top-level state. "
						"Returns the new state id.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path",      {{"type","string"}}},
				{"parent_state_id", {{"type","string"}}},
				{"name",            {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"asset_path","name"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset  = RequireString(args, "asset_path");
			std::string parent = OptString(args, "parent_state_id", "");
			std::string name   = RequireString(args, "name");
			auto r = reader.AddStateTreeState(asset, parent, name);
			return nlohmann::json{
				{"ok", true},
				{"asset_path", r.assetPath},
				{"state_id",   r.stateId},
				{"name",       r.name},
			};
		});
	}

	{
		ToolDescriptor d;
		d.name = "set_state_tree_transition";
		d.description = "[state tree] Define a transition between two UStateTree states. `trigger` "
						"names the event class or tick condition "
						"(e.g. `OnTick`, `OnEvent.Damage`).";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path",    {{"type","string"}}},
				{"from_state_id", {{"type","string"}}},
				{"to_state_id",   {{"type","string"}}},
				{"trigger",       {{"type","string"}}},
			}},
			{"required", nlohmann::json::array(
				{"asset_path","from_state_id","to_state_id","trigger"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireString(args, "asset_path");
			std::string from  = RequireString(args, "from_state_id");
			std::string to    = RequireString(args, "to_state_id");
			std::string trig  = RequireString(args, "trigger");
			auto r = reader.SetStateTreeTransition(asset, from, to, trig);
			return nlohmann::json{
				{"ok", true},
				{"asset_path",    r.assetPath},
				{"from_state_id", r.fromStateId},
				{"to_state_id",   r.toStateId},
				{"trigger",       r.trigger},
				{"added",         r.added},
			};
		});
	}

	{
		ToolDescriptor d;
		d.name = "compile_state_tree";
		d.description = "[state tree] Compile a UStateTree. Returns `{compiled: "
						"true|false}` — false means compile failed; check "
						"`read_output_log` for errors.";
		d.input_schema = {
			{"type","object"},
			{"properties", {{"asset_path", {{"type","string"}}}}},
			{"required", nlohmann::json::array({"asset_path"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireString(args, "asset_path");
			auto r = reader.CompileStateTree(asset);
			return nlohmann::json{
				{"ok", true},
				{"asset_path", r.assetPath},
				{"compiled",   r.compiled},
			};
		});
	}

	// ===== Profiling (Stage 3) =============================================

	{
		ToolDescriptor d;
		d.name = "start_profile";
		d.description = "[profiling] Start a profiling capture. `mode` selects the "
						"backend: `stats` (UE's built-in stat group "
						"file, default), `insights` (UnrealInsights "
						"trace), or `csv` (CSVProfiler). Returns "
						"`{started, output_file}` — the file path may be "
						"empty until `stop_profile` finalizes the capture.";
		d.input_schema = {
			{"type","object"},
			{"properties", {{"mode", {{"type","string"}}}}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string mode = OptString(args, "mode", "stats");
			auto r = reader.StartProfile(mode);
			return nlohmann::json{
				{"ok", true},
				{"started",     r.started},
				{"output_file", r.outputFile},
			};
		});
	}

	{
		ToolDescriptor d;
		d.name = "stop_profile";
		d.description = "[profiling] Stop the active profile capture and return its "
						"output file path. No-op if nothing is in progress.";
		d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
		registry.Add(std::move(d), [&reader](const nlohmann::json&) {
			auto r = reader.StopProfile();
			return nlohmann::json{
				{"ok", true},
				{"stopped",     r.stopped},
				{"output_file", r.outputFile},
			};
		});
	}

	{
		ToolDescriptor d;
		d.name = "get_stats";
		d.description = "[profiling] Snapshot a stat group's current values. `group` "
						"is the name passed to UE's `stat` command "
						"(`Unit`, `Game`, `GPU`, `Memory`). Returns the "
						"text snapshot the stat system produces.";
		d.input_schema = {
			{"type","object"},
			{"properties", {{"group", {{"type","string"}}}}},
			{"required", nlohmann::json::array({"group"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string g = RequireString(args, "group");
			auto r = reader.GetStats(g);
			return nlohmann::json{
				{"ok", true},
				{"group",    r.group},
				{"snapshot", r.snapshot},
			};
		});
	}

	{
		ToolDescriptor d;
		d.name = "take_screenshot";
		d.description = "[editor] Capture a high-res screenshot to disk. `dest_path` "
						"is the output file; `width`/`height` default to the "
						"current viewport size if omitted. Routed via UE's "
						"`HighResShot` exec command.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"dest_path", {{"type","string"}}},
				{"width",     {{"type","integer"}}},
				{"height",    {{"type","integer"}}},
			}},
			{"required", nlohmann::json::array({"dest_path"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string dest = RequireString(args, "dest_path");
			int w = args.value("width",  0);
			int h = args.value("height", 0);
			auto r = reader.TakeScreenshot(dest, w, h);
			return nlohmann::json{
				{"ok", true},
				{"captured",    r.captured},
				{"output_file", r.outputFile},
			};
		});
	}

	// ===== Headless cook / package (Stage 3) ==============================

	{
		ToolDescriptor d;
		d.name = "cook_content";
		d.description = "[cook] Run UE's content cook for a target platform "
						"(`Windows`, `Linux`, etc.). Asynchronous; the "
						"tool returns once the cook is dispatched. Follow "
						"progress via `read_output_log` or the editor's "
						"Cook Status panel.";
		d.input_schema = {
			{"type","object"},
			{"properties", {{"platform", {{"type","string"}}}}},
			{"required", nlohmann::json::array({"platform"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string p = RequireString(args, "platform");
			auto r = reader.CookContent(p);
			return nlohmann::json{
				{"ok", true},
				{"started",  r.started},
				{"platform", r.platform},
				{"message",  r.message},
			};
		});
	}

	{
		ToolDescriptor d;
		d.name = "package_project";
		d.description = "[cook] Package the project for a target platform via "
						"UAT. `output_dir` is where the packaged build "
						"lands. Async — tool returns once UAT is "
						"dispatched.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"platform",   {{"type","string"}}},
				{"output_dir", {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"platform","output_dir"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string p = RequireString(args, "platform");
			std::string o = RequireString(args, "output_dir");
			auto r = reader.PackageProject(p, o);
			return nlohmann::json{
				{"ok", true},
				{"started",  r.started},
				{"platform", r.platform},
				{"message",  r.message},
			};
		});
	}

	// ===== Class introspection / API docs (Stage 3) =======================

	{
		ToolDescriptor d;
		d.name = "get_class_info";
		d.description = "[class info] Inspect a UClass: parent + ancestor chain + every "
						"UPROPERTY + UFUNCTION. `class_name` is the short "
						"class name (e.g. `Actor`, `PlayerController`) or a "
						"full class path.";
		d.input_schema = {
			{"type","object"},
			{"properties", {{"class_name", {{"type","string"}}}}},
			{"required", nlohmann::json::array({"class_name"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string n = RequireString(args, "class_name");
			auto ci = reader.IntrospectClass(n);
			nlohmann::json props = nlohmann::json::array();
			for (const auto& p : ci.properties) {
				props.push_back({
					{"name",     p.name},
					{"type",     p.typeName},
					{"category", p.category},
				});
			}
			nlohmann::json fns = nlohmann::json::array();
			for (const auto& f : ci.functions) {
				fns.push_back({{"name", f.name}, {"flags", f.flagsCsv}});
			}
			return nlohmann::json{
				{"ok", true},
				{"class",      ci.className},
				{"parent",     ci.parentClass},
				{"ancestors",  ci.ancestors},
				{"properties", props},
				{"functions",  fns},
			};
		});
	}

	{
		ToolDescriptor d;
		d.name = "find_class";
		d.description = "[class info] Search the UClass registry by substring. Returns "
						"an array of class names matching `query` "
						"(case-insensitive).";
		d.input_schema = {
			{"type","object"},
			{"properties", {{"query", {{"type","string"}}}}},
			{"required", nlohmann::json::array({"query"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string q = RequireString(args, "query");
			auto r = reader.FindClass(q);
			return nlohmann::json{{"ok", true}, {"classes", r.classNames}};
		});
	}

	{
		ToolDescriptor d;
		d.name = "list_functions";
		d.description = "[class info] List every UFUNCTION on a class with its flags "
						"(BlueprintCallable, BlueprintPure, etc.). Cheaper "
						"projection than `get_class_info` when you only "
						"need the call surface.";
		d.input_schema = {
			{"type","object"},
			{"properties", {{"class_name", {{"type","string"}}}}},
			{"required", nlohmann::json::array({"class_name"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string n = RequireString(args, "class_name");
			auto fns = reader.ListFunctions(n);
			nlohmann::json arr = nlohmann::json::array();
			for (const auto& f : fns) {
				arr.push_back({{"name", f.name}, {"flags", f.flagsCsv}});
			}
			return arr;
		});
	}

	// ===== Viewport ergonomics (Stage 3) ==================================

	{
		ToolDescriptor d;
		d.name = "focus_actor";
		d.description = "[editor] Frame an actor in the editor viewport — equivalent "
						"to clicking the actor and pressing F. `actor_name` "
						"is the actor's level label.";
		d.input_schema = {
			{"type","object"},
			{"properties", {{"actor_name", {{"type","string"}}}}},
			{"required", nlohmann::json::array({"actor_name"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string n = RequireString(args, "actor_name");
			auto r = reader.FocusActor(n);
			return nlohmann::json{
				{"ok", true},
				{"actor_name", r.actorName},
				{"focused",    r.focused},
			};
		});
	}

	{
		ToolDescriptor d;
		d.name = "set_camera_transform";
		d.description = "[editor] Move the editor viewport camera to a "
						"specific location + rotation. Rotation is in "
						"degrees (pitch / yaw / roll).";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"loc_x", {{"type","number"}}},
				{"loc_y", {{"type","number"}}},
				{"loc_z", {{"type","number"}}},
				{"rot_pitch", {{"type","number"}}},
				{"rot_yaw",   {{"type","number"}}},
				{"rot_roll",  {{"type","number"}}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			auto r = reader.SetCameraTransform(
				args.value("loc_x", 0.0), args.value("loc_y", 0.0), args.value("loc_z", 0.0),
				args.value("rot_pitch", 0.0), args.value("rot_yaw", 0.0), args.value("rot_roll", 0.0));
			return nlohmann::json{{"ok", true}, {"moved", r.moved}};
		});
	}

	{
		ToolDescriptor d;
		d.name = "take_viewport_screenshot";
		d.description = "[editor] Quick capture of the active editor viewport to "
						"disk (vs `take_screenshot` which uses HighResShot "
						"for offline-quality output).";
		d.input_schema = {
			{"type","object"},
			{"properties", {{"dest_path", {{"type","string"}}}}},
			{"required", nlohmann::json::array({"dest_path"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string dest = RequireString(args, "dest_path");
			auto r = reader.TakeViewportScreenshot(dest);
			return nlohmann::json{
				{"ok", true},
				{"captured",    r.captured},
				{"output_file", r.outputFile},
			};
		});
	}

	{
		ToolDescriptor d;
		d.name = "set_show_flag";
		d.description = "[editor] Toggle a viewport show flag (`Bones`, `Bounds`, "
						"`Collision`, `Wireframe`, `Lighting`). Equivalent "
						"to the `showflag.<name> <0|1>` console command.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"flag_name", {{"type","string"}}},
				{"enabled",   {{"type","boolean"}}},
			}},
			{"required", nlohmann::json::array({"flag_name","enabled"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string f = RequireString(args, "flag_name");
			bool e = args.value("enabled", true);
			auto r = reader.SetShowFlag(f, e);
			return nlohmann::json{
				{"ok", true},
				{"flag_name", r.flagName},
				{"enabled",   r.enabled},
			};
		});
	}

	// ===== Niagara (Stage 4) ===============================================

	{
		ToolDescriptor d;
		d.name = "list_niagara_systems";
		d.description = "[niagara] List UNiagaraSystem assets under a content path "
						"(default `/Game`).";
		d.input_schema = {{"type","object"},
			{"properties", {{"path", {{"type","string"}}}}}};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string path = OptString(args, "path", "/Game");
			auto s = reader.ListNiagaraSystems(path);
			nlohmann::json arr = nlohmann::json::array();
			for (const auto& v : s)
			{
				arr.push_back(v);
			}
			return arr;
		});
	}

	{
		ToolDescriptor d;
		d.name = "read_niagara_system";
		d.description = "[niagara] Read a UNiagaraSystem's emitter handles (each "
						"names an underlying UNiagaraEmitter) and its "
						"exposed user parameter names.";
		d.input_schema = {{"type","object"},
			{"properties", {{"asset_path", {{"type","string"}}}}},
			{"required", nlohmann::json::array({"asset_path"})}};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireString(args, "asset_path");
			auto n = reader.ReadNiagaraSystem(asset);
			nlohmann::json emitters = nlohmann::json::array();
			for (const auto& e : n.emitters) {
				emitters.push_back({
					{"name",         e.name},
					{"emitter_path", e.emitterPath},
					{"enabled",      e.enabled},
				});
			}
			return nlohmann::json{
				{"ok", true},
				{"asset_path",      n.assetPath},
				{"emitters",        emitters},
				{"parameter_names", n.parameterNames},
			};
		});
	}

	{
		ToolDescriptor d;
		d.name = "create_niagara_system";
		d.description = "[niagara] Create a new (empty) UNiagaraSystem asset at "
						"the given path. Idempotent.";
		d.input_schema = {{"type","object"},
			{"properties", {{"asset_path", {{"type","string"}}}}},
			{"required", nlohmann::json::array({"asset_path"})}};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireString(args, "asset_path");
			auto r = reader.CreateNiagaraSystem(asset);
			return nlohmann::json{
				{"ok", true},
				{"asset_path",      r.assetPath},
				{"created",         r.created},
				{"already_existed", r.alreadyExisted},
			};
		});
	}

	{
		ToolDescriptor d;
		d.name = "set_niagara_parameter";
		d.description = "[niagara] Override a user-exposed parameter on a "
						"UNiagaraSystem. `value` is the parameter's text form.";
		d.input_schema = {{"type","object"},
			{"properties", {
				{"asset_path",     {{"type","string"}}},
				{"parameter_name", {{"type","string"}}},
				{"value",          {{"type","string"}}},
			}},
			{"required", nlohmann::json::array(
				{"asset_path","parameter_name","value"})}};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireString(args, "asset_path");
			std::string p     = RequireString(args, "parameter_name");
			std::string v     = RequireString(args, "value");
			auto r = reader.SetNiagaraParameter(asset, p, v);
			return nlohmann::json{
				{"ok", true},
				{"asset_path",     r.assetPath},
				{"parameter_name", r.parameterName},
				{"new_value",      r.newValue},
				{"applied",        r.applied},
			};
		});
	}

	// ===== LevelSequence (Stage 4) ========================================

	{
		ToolDescriptor d;
		d.name = "list_level_sequences";
		d.description = "[level sequence] List ULevelSequence assets under a content path.";
		d.input_schema = {{"type","object"},
			{"properties", {{"path", {{"type","string"}}}}}};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string path = OptString(args, "path", "/Game");
			auto s = reader.ListLevelSequences(path);
			nlohmann::json arr = nlohmann::json::array();
			for (const auto& v : s)
			{
				arr.push_back(v);
			}
			return arr;
		});
	}

	{
		ToolDescriptor d;
		d.name = "read_level_sequence";
		d.description = "[level sequence] Read a sequence's playback range (start/end "
						"seconds) and its top-level tracks.";
		d.input_schema = {{"type","object"},
			{"properties", {{"asset_path", {{"type","string"}}}}},
			{"required", nlohmann::json::array({"asset_path"})}};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireString(args, "asset_path");
			auto ls = reader.ReadLevelSequence(asset);
			nlohmann::json tracks = nlohmann::json::array();
			for (const auto& t : ls.tracks) {
				tracks.push_back({
					{"name",          t.trackName},
					{"class",         t.trackClass},
					{"section_count", t.sectionCount},
				});
			}
			return nlohmann::json{
				{"ok", true},
				{"asset_path",    ls.assetPath},
				{"start_seconds", ls.startSeconds},
				{"end_seconds",   ls.endSeconds},
				{"tracks",        tracks},
			};
		});
	}

	{
		ToolDescriptor d;
		d.name = "add_sequence_track";
		d.description = "[level sequence] Add a master track to a ULevelSequence (Sequencer). "
						"`track_class` is the short class name "
						"(`MovieSceneAudioTrack`, `MovieScene3DTransformTrack`).";
		d.input_schema = {{"type","object"},
			{"properties", {
				{"asset_path",  {{"type","string"}}},
				{"track_class", {{"type","string"}}},
				{"track_name",  {{"type","string"}}},
			}},
			{"required", nlohmann::json::array(
				{"asset_path","track_class","track_name"})}};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireString(args, "asset_path");
			std::string c     = RequireString(args, "track_class");
			std::string n     = RequireString(args, "track_name");
			auto r = reader.AddSequenceTrack(asset, c, n);
			return nlohmann::json{
				{"ok", true},
				{"asset_path",  r.assetPath},
				{"track_name",  r.trackName},
				{"track_class", r.trackClass},
				{"added",       r.added},
			};
		});
	}

	{
		ToolDescriptor d;
		d.name = "set_sequence_playback_range";
		d.description = "[level sequence] Set the playback range of a "
						"ULevelSequence in seconds (start <= end).";
		d.input_schema = {{"type","object"},
			{"properties", {
				{"asset_path",    {{"type","string"}}},
				{"start_seconds", {{"type","number"}}},
				{"end_seconds",   {{"type","number"}}},
			}},
			{"required", nlohmann::json::array(
				{"asset_path","start_seconds","end_seconds"})}};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireString(args, "asset_path");
			double s = args.value("start_seconds", 0.0);
			double e = args.value("end_seconds",   0.0);
			auto r = reader.SetSequencePlaybackRange(asset, s, e);
			return nlohmann::json{
				{"ok", true},
				{"asset_path",    r.assetPath},
				{"start_seconds", r.startSeconds},
				{"end_seconds",   r.endSeconds},
				{"applied",       r.applied},
			};
		});
	}

	// ===== GAS / GameplayTags (Stage 4) ===================================

	{
		ToolDescriptor d;
		d.name = "list_gameplay_tags";
		d.description = "[gameplay tag] Query the project's GameplayTagsManager. `filter` "
						"is an optional substring; empty returns every "
						"registered tag.";
		d.input_schema = {{"type","object"},
			{"properties", {{"filter", {{"type","string"}}}}}};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string f = OptString(args, "filter", "");
			auto r = reader.ListGameplayTags(f);
			return nlohmann::json{{"ok", true}, {"tags", r.tags}};
		});
	}

	{
		ToolDescriptor d;
		d.name = "add_gameplay_tag";
		d.description = "[gameplay tag] Add a tag to the project's gameplay tag dictionary. `name` "
						"uses dot-separated form (e.g. `Status.Damage.Fire`). "
						"`comment` is optional.";
		d.input_schema = {{"type","object"},
			{"properties", {
				{"name",    {{"type","string"}}},
				{"comment", {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"name"})}};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string n = RequireString(args, "name");
			std::string c = OptString(args, "comment", "");
			auto r = reader.AddGameplayTag(n, c);
			return nlohmann::json{
				{"ok", true},
				{"tag_name",        r.tagName},
				{"added",           r.added},
				{"already_existed", r.alreadyExisted},
			};
		});
	}

	{
		ToolDescriptor d;
		d.name = "read_ability_set";
		d.description = "[gameplay tag] Read a GAS ability-set DataAsset: every granted "
						"ability class + its level.";
		d.input_schema = {{"type","object"},
			{"properties", {{"asset_path", {{"type","string"}}}}},
			{"required", nlohmann::json::array({"asset_path"})}};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireString(args, "asset_path");
			auto s = reader.ReadAbilitySet(asset);
			nlohmann::json abilities = nlohmann::json::array();
			for (const auto& a : s.abilities) {
				abilities.push_back({{"class", a.abilityClass}, {"level", a.level}});
			}
			return nlohmann::json{
				{"ok", true},
				{"asset_path", s.assetPath},
				{"abilities",  abilities},
			};
		});
	}

	// ===== AnimGraph (Stage 4) ============================================

	{
		ToolDescriptor d;
		d.name = "list_anim_blueprints";
		d.description = "[anim] List UAnimBlueprint assets under a content path.";
		d.input_schema = {{"type","object"},
			{"properties", {{"path", {{"type","string"}}}}}};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string path = OptString(args, "path", "/Game");
			auto s = reader.ListAnimBlueprints(path);
			nlohmann::json arr = nlohmann::json::array();
			for (const auto& v : s)
			{
				arr.push_back(v);
			}
			return arr;
		});
	}

	{
		ToolDescriptor d;
		d.name = "read_anim_blueprint";
		d.description = "[anim] Walk a UAnimBlueprint: parent class + each "
						"state machine's states (state / conduit / "
						"transition / entry).";
		d.input_schema = {{"type","object"},
			{"properties", {{"asset_path", {{"type","string"}}}}},
			{"required", nlohmann::json::array({"asset_path"})}};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireString(args, "asset_path");
			auto a = reader.ReadAnimBlueprint(asset);
			nlohmann::json machines = nlohmann::json::array();
			for (const auto& m : a.stateMachines) {
				nlohmann::json states = nlohmann::json::array();
				for (const auto& s : m.states) {
					states.push_back({{"name", s.name}, {"kind", s.kind}});
				}
				machines.push_back({{"name", m.name}, {"states", states}});
			}
			return nlohmann::json{
				{"ok", true},
				{"asset_path",     a.assetPath},
				{"parent_class",   a.parentClass},
				{"state_machines", machines},
			};
		});
	}

	{
		ToolDescriptor d;
		d.name = "add_anim_state";
		d.description = "[anim] Add a state to a named state machine inside a "
						"UAnimBlueprint. NOT to be confused with "
						"`add_state_tree_state` (StateTree). Scaffold only — "
						"final state authoring still uses the AnimGraph editor.";
		d.input_schema = {{"type","object"},
			{"properties", {
				{"asset_path",    {{"type","string"}}},
				{"state_machine", {{"type","string"}}},
				{"state_name",    {{"type","string"}}},
			}},
			{"required", nlohmann::json::array(
				{"asset_path","state_machine","state_name"})}};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireString(args, "asset_path");
			std::string m     = RequireString(args, "state_machine");
			std::string n     = RequireString(args, "state_name");
			auto r = reader.AddAnimState(asset, m, n);
			return nlohmann::json{
				{"ok", true},
				{"asset_path",    r.assetPath},
				{"state_machine", r.stateMachine},
				{"state_name",    r.stateName},
				{"added",         r.added},
			};
		});
	}

	{
		ToolDescriptor d;
		d.name = "compile_anim_blueprint";
		d.description = "[anim] Compile a UAnimBlueprint.";
		d.input_schema = {{"type","object"},
			{"properties", {{"asset_path", {{"type","string"}}}}},
			{"required", nlohmann::json::array({"asset_path"})}};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireString(args, "asset_path");
			auto r = reader.CompileAnimBlueprint(asset);
			return nlohmann::json{
				{"ok", true},
				{"asset_path", r.assetPath},
				{"compiled",   r.compiled},
			};
		});
	}

	// ===== Batch + DSL =====================================================
	// apply_ops and compile_function live in their own files because their
	// dispatch tables are bigger than the per-tool handlers above.
	RegisterApplyOps(registry, reader);
	RegisterCompileFunction(registry, reader);
}

void RegisterProgressiveDisclosureMetaTool(ToolRegistry& registry) {
	ToolDescriptor d;
	d.name = "enable_tool_category";
	d.description =
		"[discover] Widen the active tool surface mid-session. Pass a "
		"category name (e.g. `materials`, `cpp`, `editor`) or a workflow "
		"preset (e.g. `material-tuning`); the matching tools are added "
		"to the advertised tools/list and become callable. The server "
		"emits `notifications/tools/list_changed` after this call so "
		"clients refetch tools/list to see the new surface. Useful when "
		"the server started under BP_READER_PROGRESSIVE=1 with a narrow "
		"initial set (default: `core`) and the agent's task needs more. "
		"Calling with `\"all\"` activates every registered tool. "
		"Categories already covered by the active set are a no-op. "
		"See the wiki's Tool filtering section for the full category "
		"list.";
	d.input_schema = {
		{"type", "object"},
		{"properties", {
			{"category", {
				{"type", "string"},
				{"description",
				 "Category name (e.g. `materials`, `cpp`, `editor`), "
				 "workflow preset (e.g. `material-tuning`), individual "
				 "tool name, or `all`."},
			}},
		}},
		{"required", nlohmann::json::array({"category"})},
	};

	auto handler = [&registry](const nlohmann::json& args) -> nlohmann::json {
		if (!args.contains("category") || !args["category"].is_string()) {
			throw std::runtime_error(
				"enable_tool_category requires a string `category` argument");
		}
		const std::string token = args["category"].get<std::string>();
		auto added = registry.ActivateToken(token);
		nlohmann::json result = {
			{"ok", true},
			{"token", token},
			{"added", added},
			{"newly_activated_count", added.size()},
			{"total_active", registry.Size()},
			{"total_registered", registry.TotalRegistered()},
		};
		return result;
	};
	registry.Add(std::move(d), std::move(handler));
}

}    // namespace bpr::tools
