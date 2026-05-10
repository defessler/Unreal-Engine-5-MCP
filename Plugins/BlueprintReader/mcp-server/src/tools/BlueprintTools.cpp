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
    if (it == obj.end() || it->is_null()) return fallback;
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
    if (it == obj.end() || it->is_null()) return fallback;
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
    if (ctl.offset < 0) throw std::invalid_argument(R"(argument "offset" must be >= 0)");
    if (ctl.limit < -1) throw std::invalid_argument(R"(argument "limit" must be >= 0)");
    return ctl;
}
void ApplyResponseControls(nlohmann::json& body, const ResponseControls& ctl) {
    if (body.is_array() && (ctl.offset > 0 || ctl.limit >= 0)) {
        std::size_t off = std::min<std::size_t>(ctl.offset, body.size());
        std::size_t end = (ctl.limit < 0)
                              ? body.size()
                              : std::min<std::size_t>(off + ctl.limit, body.size());
        nlohmann::json sliced = nlohmann::json::array();
        for (std::size_t i = off; i < end; ++i) sliced.push_back(std::move(body[i]));
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
            "List blueprint assets under a content path. Defaults to /Game. "
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
            "Read top-level metadata for a blueprint: parent class, interfaces, "
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
            "Convert a BP function to BPIR (Blueprint Intermediate "
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
            "Whole-class BPIR extraction: variables + interfaces + every "
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
            "Convert a BP function to C++ source. Composes "
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
            "Convert a whole BP class to a compilable UE C++ .h/.cpp pair. "
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
            "Write a transpiled .h/.cpp file into the project's Source/ "
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
            "Parse a C++ function (or bare body) into a BPIR document — "
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
            "Tiny orientation response for a blueprint: parent class plus "
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
            "Fetch a graph (nodes + connections) by name. Defaults to EventGraph. "
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
            "Fetch a blueprint function: signature (inputs/outputs), locals, "
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
            "List the SCS components (StaticMeshComponent, LightComponent, "
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
            "List all member variables on a blueprint, with type, default, "
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
            "Search nodes within a blueprint by class or title (case-insensitive substring). "
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
            "Create a new BP asset under `/Game/...` extending `parent_class`. "
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
            "Add a member variable to a blueprint. `type` accepts either a "
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
        d.description = "Move a node (by GUID) to (x, y) inside a graph. Recompiles + saves the BP.";
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
        d.description = "Delete a node by GUID. Breaks any links into/out of it. Recompiles + saves.";
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
            "Spawn a new K2 node in a graph. `kind` is one of: "
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
                if (!v.empty()) extras.emplace(flagKey, std::move(v));
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
                            if (p.Type.SubCategory)       t["sub_category"]        = *p.Type.SubCategory;
                            if (p.Type.SubCategoryObject) t["sub_category_object"] = *p.Type.SubCategoryObject;
                            if (p.Type.IsArray) t["is_array"] = true;
                            if (p.Type.IsSet)   t["is_set"]   = true;
                            if (p.Type.IsMap)   t["is_map"]   = true;
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
            if (!title.empty())     out["title"] = title;
            if (!nodeClass.empty()) out["class"] = nodeClass;
            return out;
        });
    }

    // ----- get_node --------------------------------------------------------
    {
        ToolDescriptor d;
        d.name = "get_node";
        d.description =
            "Fetch a single node by GUID inside a graph. Returns the node's "
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
            "Find blueprints under `path` that match a structural query: "
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
                if (candidate == query) return true;
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
            "Connect two pins. `from_pin` and `to_pin` accept either a pin GUID "
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
                        if (p.Id == spec || p.Name == spec) return &p;
                    }
                    return nullptr;
                };
                auto describeType = [](const BPPinType& t) {
                    std::string s = t.Category;
                    if (t.SubCategory) s += ":" + *t.SubCategory;
                    if (t.SubCategoryObject) s += "(" + *t.SubCategoryObject + ")";
                    if (t.IsArray) s = "[]" + s;
                    if (t.IsSet)   s = "{}" + s;
                    if (t.IsMap)   s = "{key:" + s + "}";
                    return s;
                };
                try {
                    auto g = reader.GetGraph(asset, graph);
                    for (const auto& n : g.Nodes) {
                        if (n.Id == fromNode) {
                            if (auto* p = findPin(n, fromPin)) fromType = describeType(p->Type);
                        }
                        if (n.Id == toNode) {
                            if (auto* p = findPin(n, toPin)) toType = describeType(p->Type);
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
            "Change a member variable's type without delete + re-add. UE "
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
            "Change the My-Blueprint-panel category label on a member "
            "variable (the \"Stats\" / \"Combat\" group header in the "
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
            "File-level duplicate: source BP at `asset_path` → new BP at "
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
        d.description = "Remove a member variable by name. Recompiles + saves.";
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
            "Rename a member variable. Updates references in graphs. "
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
            "Create a new BP function graph with the given name. Returns "
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
            "Add an input parameter to an existing function. `type` accepts "
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
            "Add an output parameter to an existing function. Spawns a "
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
        d.description = "Delete a function graph by name.";
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
            "Change a member variable's default value (string form, as displayed "
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
            "List the `kind` values that `add_node` accepts, with required "
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
            "List the canonical BPPinType.category values + container modifiers. "
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
                if (objHint) j["sub_category_object_hint"] = objHint;
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
                    cat("class",     "UClass reference. `sub_category_object` = the meta-class.",
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
            "Tear down the backing editor daemon process. After this returns, "
            "the project's file locks (DDC, asset registry, .uasset handles) "
            "are released so you can launch the full UE editor. The next "
            "read tool call auto-respawns the daemon — pay a one-time "
            "cold-start cost (~5–30 s depending on project size). Pair with "
            "BP_READER_READ_ONLY=1 if you want to keep the MCP server "
            "running for queries while you work in the editor.\n\n"
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
            "Auto-position the nodes in a graph so they don't overlap. "
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
                if (auto it = pinName.find(c.FromPin); it != pinName.end()) fpName = it->second;
                if (auto it = pinName.find(c.ToPin);   it != pinName.end()) tpName = it->second;
                if (!isExecPinName(fpName) && !isExecPinName(tpName)) continue;
                downstream[c.FromNode].push_back(c.ToNode);
            }

            // Roots: nodes with class containing "Entry" or "Event" (no
            // upstream exec). If we don't find any, fall back to all nodes
            // as their own roots — at least they'll be ranked into columns.
            std::set<std::string> hasUpstream;
            for (const auto& [_, dsts] : downstream)
                for (const auto& d : dsts) hasUpstream.insert(d);
            std::vector<std::string> roots;
            for (const auto& n : g.Nodes) {
                bool isEntryClass = n.Class.find("FunctionEntry") != std::string::npos ||
                                    n.Class.find("Event")         != std::string::npos ||
                                    n.Class.find("CustomEvent")   != std::string::npos;
                if (isEntryClass && !hasUpstream.count(n.Id)) roots.push_back(n.Id);
            }
            if (roots.empty()) {
                for (const auto& n : g.Nodes) if (!hasUpstream.count(n.Id)) roots.push_back(n.Id);
            }

            // BFS from roots; column = depth, row = order-of-discovery within column.
            std::map<std::string, int, std::less<>> col;  // node_id -> column
            std::vector<std::string> bfs = roots;
            for (const auto& r : roots) col[r] = 0;
            for (std::size_t i = 0; i < bfs.size(); ++i) {
                int c = col[bfs[i]];
                for (const auto& d : downstream[bfs[i]]) {
                    auto [it, inserted] = col.insert({d, c + 1});
                    if (inserted) bfs.push_back(d);
                }
            }
            // Any disconnected leftovers get their own column at the right.
            int maxCol = 0;
            for (auto& [_, v] : col) maxCol = std::max(maxCol, v);
            int leftoverCol = maxCol + 1;
            for (const auto& n : g.Nodes) {
                if (col.find(n.Id) == col.end()) col[n.Id] = leftoverCol;
            }

            // Group by column, assign rows.
            std::map<int, std::vector<std::string>> byCol;
            for (const auto& [id, c] : col) byCol[c].push_back(id);

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
            "Read the project's `.uproject` file and return parsed metadata "
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
            "Save every dirty package the editor has loaded. With "
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
            "Move or rename an asset. `dest_asset_path` is the full destination "
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
            "Delete an asset. Refuses by default if other assets reference "
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
            "Create a folder under `/Game/`. Idempotent — returns "
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
            "List all UDataTable assets under a content path. Mirrors "
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
            for (const auto& s : summaries) arr.push_back(s);
            return arr;
        });
    }

    // ----- read_data_table ------------------------------------------------
    {
        ToolDescriptor d;
        d.name = "read_data_table";
        d.description =
            "Load a DataTable and return its row-struct type, column names, "
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
            "Execute a UE console command (e.g. `stat unit`, `showflag.bones 1`, "
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

    // ----- get_cvar -------------------------------------------------------
    {
        ToolDescriptor d;
        d.name = "get_cvar";
        d.description =
            "Read a console variable's current value. Returns "
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
            "Set a console variable. Forces `ECVF_SetByCode` priority — "
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
            "Start a Play-In-Editor session. `mode` is one of "
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
            "End the active PIE session. No-op when PIE isn't running.";
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
            "Trigger UE's Live Coding compile + patch. The compile runs "
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
            "List the names of currently-selected actors in the level editor. "
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
            "Replace (or extend) the editor's selection. `replace:true` "
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
                    if (v.is_string()) names.push_back(v.get<std::string>());
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
            "Spawn an actor of the given UClass in the current level. "
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
            "Update an actor's world transform. `actor_name` is from "
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
            "Destroy an actor by name. Returns `{deleted: false}` if the "
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
            "Read recent entries from the editor's output log. The plugin "
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

    // ===== Batch + DSL =====================================================
    // apply_ops and compile_function live in their own files because their
    // dispatch tables are bigger than the per-tool handlers above.
    RegisterApplyOps(registry, reader);
    RegisterCompileFunction(registry, reader);
}

} // namespace bpr::tools
