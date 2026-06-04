#include "tools/BlueprintTools.h"
#include "tools/ApplyOps.h"
#include "tools/Bpir.h"
#include "tools/codegen/CppClassEmit.h"
#include "tools/codegen/CppEmit.h"
#include "tools/codegen/UnsupportedTreatment.h"
#include "tools/CompileFunction.h"
#include "tools/ContentBlocks.h"
#include "tools/Cursor.h"
#include "tools/Decompile.h"
#include "tools/ImageReader.h"
#include "tools/JsonProjection.h"
#include "tools/parse/CppParse.h"
#include "tools/TypeShorthand.h"

#include "Env.h"
#include "backends/IBlueprintReader.h"

#include <filesystem>
#include <fstream>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <fmt/core.h>
#include <nlohmann/json.hpp>
#include "tools/BlueprintToolsDetail.h"

namespace bpr::tools {

using namespace blueprint_tools_detail;

const std::vector<std::string>& KnownNodeKinds() {
	// Keep in lockstep with the list_node_kinds descriptor table below — a
	// sync test (test_tools.cpp) asserts these match.
	static const std::vector<std::string> kKinds = {
		"GetSubsystem", "Branch", "Sequence", "VariableGet", "VariableSet",
		"CallFunction", "CustomEvent", "Event", "Cast", "Self", "MakeArray",
		"MakeStruct", "FormatText", "Knot",
		"Comment", "GetArrayItem", "Select", "SpawnActor", "BreakStruct",
		"MacroInstance", "CallParent", "PromotableOp", "CommutativeOp", "Message",
	};
	return kKinds;
}

// Phase D — assemble the response for a screenshot tool, optionally
// inlining the captured PNG as a content block. The classic shape is
// `{ok, captured, output_file}`; when `returnInline` is true AND the
// file exists AND its dimensions are within `kInlineImageMaxDim`, we
// wrap that JSON as structuredContent next to an Image content block
// so the client can render the capture without a follow-up tool call.
//
// Kill-switch + safety:
//   - BP_READER_NEVER_INLINE_IMAGES=1 forces classic shape regardless of arg
//   - Image dimensions > 1280px max-dim → throw invalid_argument
//     (helpful hint: pass width/height to the capture call instead)
//   - File-not-found → throw with the path so the agent can retry
nlohmann::json BuildScreenshotResponse(
	const std::string& destPath,
	bool captured,
	const std::string& outputFile,
	bool returnInline,
	const std::string& note) {
	nlohmann::json structured = {
		{"ok", true},
		{"captured", captured},
		{"output_file", outputFile},
	};
	if (!note.empty()) { structured["note"] = note; }
	const bool wantInline = returnInline && !env::NeverInlineImages();
	if (!wantInline || !captured) {
		return structured;
	}
	// Resolve the path the tool actually wrote — outputFile from the
	// reader is authoritative if non-empty; fall back to dest_path.
	std::filesystem::path resolved = outputFile.empty()
		? std::filesystem::path(destPath)
		: std::filesystem::path(outputFile);
	std::error_code ec;
	if (!std::filesystem::exists(resolved, ec)) {
		throw std::invalid_argument(fmt::format(
			"return_inline=true requested but capture file not found: {}",
			resolved.string()));
	}
	const auto dims = imageio::ReadPngDimensions(resolved);
	if (!dims) {
		throw std::invalid_argument(fmt::format(
			"return_inline=true requires a valid PNG at {} "
			"(IHDR parse failed — was the file actually written?)",
			resolved.string()));
	}
	const uint32_t maxDim = std::max(dims->width, dims->height);
	if (maxDim > kInlineImageMaxDim) {
		throw std::invalid_argument(fmt::format(
			"return_inline=true refused: capture is {}x{}, max-dim {} > {}px cap. "
			"Re-capture at a smaller size (pass width/height args) or omit "
			"return_inline to keep the disk-only path.",
			dims->width, dims->height, maxDim, kInlineImageMaxDim));
	}
	// Read the bytes; nlohmann's binary type isn't suitable for the JSON
	// emission path, so we build the base64 explicitly via ContentBlocks.
	std::ifstream in(resolved, std::ios::binary);
	std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
								std::istreambuf_iterator<char>());
	structured["image_width"]  = dims->width;
	structured["image_height"] = dims->height;
	return content::Envelope(
		{
			content::Image(bytes, "image/png", content::Audience::User),
			content::Text(structured.dump(), content::Audience::Assistant),
		},
		std::move(structured));
}

// RegisterBlueprintTools is split across several RegisterTools_NN helpers
// below purely to keep any single function small enough for MSVC's
// front-end heap (a single 8k-line function trips C1060 "out of heap
// space" on memory-limited CI runners). The split points are arbitrary
// tool boundaries; each helper takes the same (registry, reader). The
// public RegisterBlueprintTools at the bottom calls them in order.
void RegisterTools_00(ToolRegistry& registry, backends::IBlueprintReader& reader) {
	// ----- list_assets -----------------------------------------------------
	// General-purpose asset enumeration. The list_blueprints / list_materials
	// / list_widgets typed family answers "give me every X"; list_assets
	// answers "give me every asset under this path". Asset-registry-backed,
	// O(1) per asset.
	{
		ToolDescriptor d;
		d.name = "list_assets";
		d.description =
			"[assets] List every asset (any UClass) under `path`. Asset-registry-backed; "
			"O(1) per asset. Use when you don't know the asset's UClass — "
			"reach for `list_blueprints` / `list_materials` / etc. instead "
			"when you know the type up front (less to filter on the agent "
			"side). Returns `[{asset_path, name, class_name}, ...]`. "
			"`sort=name` or `sort=path` for deterministic ordering.";
		d.input_schema = {
			{"type", "object"},
			{"properties", {
				{"path", {{"type", "string"},
						  {"description", "Content path filter, e.g. /Game/UI. Defaults to /Game."}}},
				{"recursive", {{"type", "boolean"},
							   {"description", "Descend into subfolders. Default true."}}},
				{"limit",  LimitProperty()},
				{"offset", OffsetProperty()},
				{"cursor", CursorProperty()},
				{"fields", FieldsProperty()},
				{"sort",   SortProperty()},
			}},
		};
		d.output_schema = {
			{"type", "array"},
			{"items", {
				{"type", "object"},
				{"properties", {
					{"asset_path", {{"type", "string"}}},
					{"name",       {{"type", "string"}}},
					{"class_name", {{"type", "string"}}},
				}},
				{"required", nlohmann::json::array({"asset_path","class_name"})},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string path = OptString(args, "path", "/Game");
			const bool recursive = args.value("recursive", true);
			auto ctl = ParseResponseControls(args);
			auto res = reader.ListAssets(path, recursive);
			nlohmann::json body = nlohmann::json::array();
			for (const auto& e : res.entries) {
				body.push_back({
					{"asset_path", e.assetPath},
					{"name",       e.name},
					{"class_name", e.className},
				});
			}
			ApplyResponseControls(body, ctl);
			return body;
		});
	}

	// ----- find_asset ------------------------------------------------------
	// Substring-search the asset registry. Pairs with list_assets but
	// answers a different question — "I'm looking for an asset with X in
	// its name, anywhere in /Game (or scoped to a subtree)". Returns a
	// paginated envelope (a broad query can match thousands of rows, which a
	// client rejects as "Output too large"); `results` rows share
	// list_assets' shape.
	{
		ToolDescriptor d;
		d.name = "find_asset";
		d.description =
			"[assets] Find assets whose name or package path contains `query` (case-insensitive). "
			"Scoped to `path` (defaults to /Game). Use this instead of "
			"shelling out to `Get-ChildItem` / `find` — asset registry is "
			"O(N) once and lives in memory. Returns a paginated envelope "
			"`{total, count, offset, has_more, next_cursor, results:[{asset_path, name, class_name}, ...]}`. "
			"Capped at 50 results per page by default (a broad `query` can "
			"match thousands); pass `limit` to change the page size and "
			"`next_cursor` (or `offset`) to fetch the next page. `total` is "
			"the full match count so you know how many pages remain.";
		d.input_schema = {
			{"type", "object"},
			{"properties", {
				{"query", {{"type", "string"},
						   {"description", "Substring matched (case-insensitive) against the "
											"asset's short name or full package path."}}},
				{"path",  {{"type", "string"},
						   {"description", "Scope path. Defaults to /Game."}}},
				{"limit", LimitProperty()},
				{"offset", OffsetProperty()},
				{"cursor", CursorProperty()},
				{"fields", FieldsProperty()},
			}},
			{"required", nlohmann::json::array({"query"})},
		};
		d.output_schema = {
			{"type", "object"},
			{"properties", {
				{"query",       {{"type", "string"}}},
				{"path",        {{"type", "string"}}},
				{"total",       {{"type", "integer"}, {"minimum", 0}}},
				{"count",       {{"type", "integer"}, {"minimum", 0}}},
				{"offset",      {{"type", "integer"}, {"minimum", 0}}},
				{"has_more",    {{"type", "boolean"}}},
				{"next_cursor", {{"type", nlohmann::json::array({"string", "null"})}}},
				{"results", {
					{"type", "array"},
					{"items", {
						{"type", "object"},
						{"properties", {
							{"asset_path", {{"type", "string"}}},
							{"name",       {{"type", "string"}}},
							{"class_name", {{"type", "string"}}},
						}},
						{"required", nlohmann::json::array({"asset_path","class_name"})},
					}},
				}},
			}},
			{"required", nlohmann::json::array({"total","count","has_more","results"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string q = RequireString(args, "query");
			std::string path = OptString(args, "path", "/Game");
			auto ctl = ParseResponseControls(args);
			auto res = reader.FindAsset(q, path);
			nlohmann::json rows = nlohmann::json::array();
			for (const auto& e : res.entries) {
				rows.push_back({
					{"asset_path", e.assetPath},
					{"name",       e.name},
					{"class_name", e.className},
				});
			}
			nlohmann::json body = BuildPaginatedBody(std::move(rows), ctl, /*defaultLimit=*/50);
			body["query"] = q;
			body["path"]  = path;
			return body;
		});
	}

	// ----- list_blueprints -------------------------------------------------
	{
		ToolDescriptor d;
		d.name = "list_blueprints";
		d.description =
			"[blueprint] List Blueprint assets under a content path. Defaults to /Game. "
			"On big projects this can return thousands of entries — use "
			"`limit`/`offset` to page, and `fields` (e.g. [\"asset_path\"]) "
			"to drop columns you don't need. `sort=path` orders by asset path.";
		d.input_schema = {
			{"type", "object"},
			{"properties", {
				{"path", {
					{"type", "string"},
					{"description", "Content path filter, e.g. /Game/AI. Defaults to /Game."},
				}},
				{"limit",  LimitProperty()},
				{"offset", OffsetProperty()},
				{"cursor", CursorProperty()},
				{"fields", FieldsProperty()},
				{"sort",   SortProperty()},
			}},
		};
		d.output_schema = {
			{"type", "array"},
			{"items", {
				{"type", "object"},
				{"properties", {
					{"asset_path",   {{"type", "string"}}},
					{"parent_class", {{"type", "string"}}},
				}},
				{"required", nlohmann::json::array({"asset_path"})},
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
			"project just what you need — full payloads can be many KB on busy BPs.\n\n"
			"**Typed-BP hint:** this tool reads the BP layer common to every "
			"asset type, but specialized assets carry extra structure not "
			"surfaced here. Prefer the typed reader when you need it: "
			"`read_widget_blueprint` (UMG widget tree), `read_anim_blueprint` "
			"(state machines + anim graph), `read_behavior_tree` (BT node "
			"hierarchy). `read_blueprint` still works on those assets — just "
			"returns the union without the asset-specific structure.";
		d.input_schema = {
			{"type", "object"},
			{"properties", {
				{"asset_path", {{"type", "string"},
								{"description", "UE asset path, e.g. /Game/AI/BP_Enemy"}}},
				{"fields",     FieldsProperty()},
			}},
			{"required", nlohmann::json::array({"asset_path"})},
		};
		// Top-level shape of the full BP metadata. Array items vary (variables,
		// functions, graphs) so only `interfaces` (string names) declares an
		// item type; `fields` projection narrows the response.
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path",   {{"type","string"}}},
				{"name",         {{"type","string"}}},
				{"parent_class", {{"type","string"}}},
				{"interfaces",   {{"type","array"}, {"items", {{"type","string"}}}}},
				{"variables",    {{"type","array"}}},
				{"functions",    {{"type","array"}}},
				{"macros",       {{"type","array"}}},
				{"graphs",       {{"type","array"}}},
			}},
			{"required", nlohmann::json::array({"asset_path","name","parent_class",
				"interfaces","variables","functions","macros","graphs"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireAssetPath(args);
			auto ctl = ParseResponseControls(args);
			nlohmann::json body = WithAssetNotFoundHint(reader, asset, [&] {
				return nlohmann::json(reader.ReadBlueprint(asset));
			});
			ApplyResponseControls(body, ctl);
			return body;
		});
	}

	// ----- read_actor_instance ---------------------------------------------
	{
		ToolDescriptor d;
		d.name = "read_actor_instance";
		d.description =
			"[assets] Read ANY object instance by package path — including a "
			"level-placed actor stored in its own external package under "
			"`/<Mount>/__ExternalActors__/...` (World Partition / One-File-Per-"
			"Actor), which `read_blueprint` can't open. Returns the object's "
			"class/name, and for actors the label + transform + owning level, "
			"plus `overrides` — the properties this instance changed relative to "
			"its archetype (exported-text values). Use this to inspect a "
			"misconfigured *placed instance* (the common real-world bug) rather "
			"than the Blueprint class it came from. `fields` projects the "
			"response (e.g. [\"object_class\",\"overrides[].name\"]).";
		d.input_schema = {
			{"type", "object"},
			{"properties", {
				{"asset_path", {{"type", "string"},
								{"description", "Package path of the instance — e.g. an external-actor "
												"path /Game/__ExternalActors__/Maps/L_X/0/AB/GUID, or any "
												"/Game/... UObject."}}},
				{"fields", FieldsProperty()},
				{"limit", LimitProperty()},
				{"offset", OffsetProperty()},
			}},
			{"required", nlohmann::json::array({"asset_path"})},
		};
		d.output_schema = {
			{"type", "object"},
			{"properties", {
				{"ok",            {{"type", "boolean"}}},
				{"asset_path",    {{"type", "string"}}},
				{"object_class",  {{"type", "string"}}},
				{"object_name",   {{"type", "string"}}},
				{"is_actor",      {{"type", "boolean"}}},
				{"is_external",   {{"type", "boolean"}}},
				{"label",         {{"type", "string"}}},
				{"owning_level",  {{"type", "string"}}},
				{"transform",     {{"type", "object"}}},
				{"override_count",{{"type", "integer"}, {"minimum", 0}}},
				{"overrides", {
					{"type", "array"},
					{"items", {
						{"type", "object"},
						{"properties", {
							{"name",  {{"type", "string"}}},
							{"type",  {{"type", "string"}}},
							{"value", {{"type", "string"}}},
						}},
						{"required", nlohmann::json::array({"name","value"})},
					}},
				}},
			}},
			{"required", nlohmann::json::array({"object_class","is_actor","overrides"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireAssetPath(args);
			auto ctl = ParseResponseControls(args);
			nlohmann::json body = reader.ReadActorInstance(asset);
			// UX-P2a: limit/offset page the (potentially large) overrides[]
			// array — body is an object, so ApplyResponseControls' top-level
			// slice wouldn't reach it.
			PaginateField(body, "overrides", ctl);
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
			"BP → BPIR → BP works for the patterns BPIR covers cleanly.\n\n**Gated:** the MCP server's process env must include `BP_READER_ALLOW_TRANSPILE=1`. Off by default — when disabled, returns `{ok: false, error: 'transpile_disabled', hint: ...}`.";
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
			if (!TranspileEnabled()) {
				return TranspileDisabledResponse("decompile_function");
			}
			std::string asset = RequireAssetPath(args);
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
			"failure>` markers in their unsupported_nodes.\n\n**Gated:** the MCP server's process env must include `BP_READER_ALLOW_TRANSPILE=1`. Off by default — when disabled, returns `{ok: false, error: 'transpile_disabled', hint: ...}`.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path", {{"type","string"}}},
				{"fields",     FieldsProperty()},
			}},
			{"required", nlohmann::json::array({"asset_path"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			if (!TranspileEnabled()) {
				return TranspileDisabledResponse("decompile_blueprint");
			}
			std::string asset = RequireAssetPath(args);
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
			"the agent can iterate over.\n\n**Gated:** the MCP server's process env must include `BP_READER_ALLOW_TRANSPILE=1`. Off by default — when disabled, returns `{ok: false, error: 'transpile_disabled', hint: ...}`.";
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
		// Dual-shape: success (ok:true + fields below) or, when transpile is
		// disabled (default), {ok:false, error:"transpile_disabled", tool, hint}.
		// Only `ok` is common to both, so it's the sole required key.
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",                {{"type","boolean"}}},
				{"asset_path",        {{"type","string"}}},
				{"function_name",     {{"type","string"}}},
				{"target_lang",       {{"type","string"}}},
				{"mode",              {{"type","string"}}},
				{"source",            {{"type","string"}}},
				{"notes",             {{"type","array"}}},
				{"unsupported_count", {{"type","integer"}}},
				{"error",             {{"type","string"}}},
				{"tool",              {{"type","string"}}},
				{"hint",              {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"ok"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			if (!TranspileEnabled()) {
				return TranspileDisabledResponse("transpile_function");
			}
			std::string asset = RequireAssetPath(args);
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
			"to triage.\n\n**Gated:** the MCP server's process env must include `BP_READER_ALLOW_TRANSPILE=1`. Off by default — when disabled, returns `{ok: false, error: 'transpile_disabled', hint: ...}`.";
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
		// Dual-shape: success (ok:true + fields) or transpile-disabled
		// ({ok:false, error, tool, hint}). Only `ok` is common → sole required.
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",                {{"type","boolean"}}},
				{"asset_path",        {{"type","string"}}},
				{"target_lang",       {{"type","string"}}},
				{"class_name",        {{"type","string"}}},
				{"header_file",       {{"type","string"}}},
				{"impl_file",         {{"type","string"}}},
				{"header_source",     {{"type","string"}}},
				{"impl_source",       {{"type","string"}}},
				{"notes",             {{"type","array"}}},
				{"sidecar",           {{"type","object"}}},
				{"sidecar_file",      {{"type","string"}}},
				{"unsupported_count", {{"type","integer"}}},
				{"error",             {{"type","string"}}},
				{"tool",              {{"type","string"}}},
				{"hint",              {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"ok"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			if (!TranspileEnabled()) {
				return TranspileDisabledResponse("transpile_blueprint");
			}
			std::string asset = RequireAssetPath(args);
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
			"reparent to the C++ class for hybrid workflows.\n\n**Gated:** the MCP server's process env must include `BP_READER_ALLOW_TRANSPILE=1`. Off by default — when disabled, returns `{ok: false, error: 'transpile_disabled', hint: ...}`.";
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
		// Dual-shape: success ({ok:true, path, bytes_written}) or
		// transpile-disabled ({ok:false, error, tool, hint}). `ok` common.
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",            {{"type","boolean"}}},
				{"path",          {{"type","string"}}},
				{"bytes_written", {{"type","integer"}}},
				{"error",         {{"type","string"}}},
				{"tool",          {{"type","string"}}},
				{"hint",          {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"ok"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			if (!TranspileEnabled()) {
				return TranspileDisabledResponse("write_generated_source");
			}
			std::string path    = RequireString(args, "path");
			RequireSafeFilePath(path);  // refuse `..` traversal early
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

}

void RegisterTools_00b(ToolRegistry& registry, backends::IBlueprintReader& reader) {
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
			"than corrupt downstream graphs.\n\n**Gated:** the MCP server's process env must include `BP_READER_ALLOW_TRANSPILE=1`. Off by default — when disabled, returns `{ok: false, error: 'transpile_disabled', hint: ...}`.";
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
		// Dual-shape: success ({ok:true, bpir}) or transpile-disabled
		// ({ok:false, error, tool, hint}). `ok` is the only common key.
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",    {{"type","boolean"}}},
				{"bpir",  {{"type","object"}}},
				{"error", {{"type","string"}}},
				{"tool",  {{"type","string"}}},
				{"hint",  {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"ok"})},
		};
		registry.Add(std::move(d), [](const nlohmann::json& args) {
			if (!TranspileEnabled()) {
				return TranspileDisabledResponse("parse_cpp_function");
			}
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
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"name",            {{"type","string"}}},
				{"asset_path",      {{"type","string"}}},
				{"parent_class",    {{"type","string"}}},
				{"variable_count",  {{"type","integer"}}},
				{"function_count",  {{"type","integer"}}},
				{"graph_count",     {{"type","integer"}}},
				{"macro_count",     {{"type","integer"}}},
				{"interface_count", {{"type","integer"}}},
			}},
			{"required", nlohmann::json::array({"name","asset_path","parent_class",
				"variable_count","function_count","graph_count","macro_count","interface_count"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireAssetPath(args);
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
			"to drop fields you don't need, or `summary: true` to drop "
			"per-node pin arrays and the connections list in one shot.";
		d.input_schema = {
			{"type", "object"},
			{"properties", {
				{"asset_path", {{"type", "string"},
								{"description", "UE asset path, e.g. /Game/AI/BP_Enemy"}}},
				{"graph_name", {{"type", "string"},
								{"description", "Graph name. Defaults to \"EventGraph\"."}}},
				{"summary", {{"type", "boolean"},
							 {"description", "When true, omits per-node pin arrays and the "
							  "top-level connections array. Keeps {id, kind, title, "
							  "comment, position} per node. Default TRUE (lean reads); pass "
				  "false for the full graph with pin arrays + connections."}}},
				{"fields", FieldsProperty()},
			}},
			{"required", nlohmann::json::array({"asset_path"})},
		};
		// Graph object. name/type/nodes always present; `connections` only in
		// full mode (summary:false), `_summary` only in lean mode (default).
		// `nodes` items are BPNode (validated via get_node); `fields` narrows.
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"name",        {{"type","string"}}},
				{"type",        {{"type","string"}}},
				{"nodes",       {{"type","array"}}},
				{"connections", {{"type","array"}}},
				{"_summary",    {{"type","boolean"}}},
			}},
			{"required", nlohmann::json::array({"name","type","nodes"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireAssetPath(args);
			std::string graph = OptString(args, "graph_name", "EventGraph");
			// Lean by default: graph reads return the summary shape (node
		// identity, no per-node pin arrays or the connections list) unless
		// the caller asks for full detail with summary:false. Big token
		// savings on the common "what's the shape of this graph" read.
		const bool summary = args.value("summary", true);
			auto ctl = ParseResponseControls(args);
			nlohmann::json body = WithAssetNotFoundHint(reader, asset, [&] {
				return nlohmann::json(reader.GetGraph(asset, graph));
			});
			if (summary && ctl.fields.empty()) {  // explicit `fields` wins over `summary`
				// Drop pin detail from each node + the global connections
				// array. Node identity (id, kind, title, comment, position)
				// is preserved — enough to map the graph's structure
				// without paying for tens of KB of pin metadata.
				if (body.contains("nodes") && body["nodes"].is_array()) {
					for (auto& n : body["nodes"]) {
						if (n.is_object()) {
							n.erase("pins");
						}
					}
				}
				body.erase("connections");
				body["_summary"] = true;
			}
			ApplyResponseControls(body, ctl);
			return body;
		});
	}

	// ----- peek_graph ------------------------------------------------------
	// Cheap "is this worth pulling in full?" probe. Returns node count +
	// kind histogram + connection count without any node/pin/connection
	// detail. ~50 bytes per response regardless of graph size — vs
	// get_graph's tens of KB for a busy EventGraph.
	{
		ToolDescriptor d;
		d.name = "peek_graph";
		d.description =
			"[blueprint] Lightweight probe for a Blueprint graph — returns node count + "
			"kind histogram + connection count without any node/pin/connection "
			"detail. Use before `get_graph` to decide whether a graph is worth "
			"reading in full (event graphs on busy BPs can be tens of KB). "
			"Defaults to EventGraph.";
		d.input_schema = {
			{"type", "object"},
			{"properties", {
				{"asset_path", {{"type", "string"},
								{"description", "UE asset path, e.g. /Game/AI/BP_Enemy"}}},
				{"graph_name", {{"type", "string"},
								{"description", "Graph name. Defaults to \"EventGraph\"."}}},
			}},
			{"required", nlohmann::json::array({"asset_path"})},
		};
		d.output_schema = {
			{"type", "object"},
			{"properties", {
				{"name",              {{"type", "string"}}},
				{"type",              {{"type", "string"}}},
				{"nodes_count",       {{"type", "integer"}, {"minimum", 0}}},
				{"connections_count", {{"type", "integer"}, {"minimum", 0}}},
				{"by_kind",           {{"type", "object"},
									   {"additionalProperties", {{"type", "integer"}}}}},
			}},
			{"required", nlohmann::json::array({"name","nodes_count","by_kind"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireAssetPath(args);
			std::string graph = OptString(args, "graph_name", "EventGraph");
			BPGraph g = WithAssetNotFoundHint(reader, asset, [&] {
				return reader.GetGraph(asset, graph);
			});
			std::map<std::string, int, std::less<>> byKind;
			for (const auto& n : g.Nodes) {
				// Prefer meta.kind (the semantic K2-node kind we emit
				// in the wire) over the raw UClass. Falls back to the
				// class name when meta is absent (older fixtures).
				std::string kind;
				if (n.Meta.is_object()) {
					auto it = n.Meta.find("kind");
					if (it != n.Meta.end() && it->is_string()) {
						kind = it->get<std::string>();
					}
				}
				if (kind.empty()) {
					kind = n.Class;
				}
				++byKind[kind];
			}
			nlohmann::json kindJson = nlohmann::json::object();
			for (const auto& [k, v] : byKind) {
				kindJson[k] = v;
			}
			return nlohmann::json{
				{"name",              g.Name},
				{"type",              g.Type},
				{"nodes_count",       static_cast<int>(g.Nodes.size())},
				{"connections_count", static_cast<int>(g.Connections.size())},
				{"by_kind",           std::move(kindJson)},
			};
		});
	}

	// ----- get_function ----------------------------------------------------
	{
		ToolDescriptor d;
		d.name = "get_function";
		d.description =
			"[blueprint] Fetch a Blueprint function: signature (inputs/outputs), locals, "
			"and body graph. Use `fields` to project (e.g. "
			"[\"inputs[].name\", \"outputs[].name\"] for just the signature). "
			"The body graph is under the `graph` key — use `graph.nodes[].id` "
			"(not `body.nodes[].id`) in `fields` paths. "
			"Use `summary: true` to drop per-node pin arrays and the graph's "
			"connections list.";
		d.input_schema = {
			{"type", "object"},
			{"properties", {
				{"asset_path", {{"type", "string"},
								{"description", "UE asset path, e.g. /Game/AI/BP_Enemy"}}},
				{"function_name", {{"type", "string"},
								   {"description", "Function name as it appears in the blueprint."}}},
				{"summary", {{"type", "boolean"},
							 {"description", "When true, the body graph's nodes drop their "
							  "pin arrays and the graph's connections list is omitted. "
							  "Function signature (inputs/outputs/locals) is unchanged. "
							  "Default TRUE (lean reads); pass false for the full body graph."}}},
				{"fields", FieldsProperty()},
			}},
			{"required", nlohmann::json::array({"asset_path", "function_name"})},
		};
		// Function: signature (inputs/outputs/locals) + body graph, all always
		// emitted (BPFunction::to_json) → required. `_summary` only in the
		// default lean mode. `graph` is the BPGraph shape; `fields` narrows.
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"name",     {{"type","string"}}},
				{"inputs",   {{"type","array"}}},
				{"outputs",  {{"type","array"}}},
				{"locals",   {{"type","array"}}},
				{"graph",    {{"type","object"}}},
				{"_summary", {{"type","boolean"}}},
			}},
			{"required", nlohmann::json::array({"name","inputs","outputs","locals","graph"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireAssetPath(args);
			std::string fn = RequireString(args, "function_name");
			// Lean by default: graph reads return the summary shape (node
		// identity, no per-node pin arrays or the connections list) unless
		// the caller asks for full detail with summary:false. Big token
		// savings on the common "what's the shape of this graph" read.
		const bool summary = args.value("summary", true);
			auto ctl = ParseResponseControls(args);
			nlohmann::json body = WithAssetNotFoundHint(reader, asset, [&] {
				return nlohmann::json(reader.GetFunction(asset, fn));
			});
			if (summary && ctl.fields.empty()) {  // explicit `fields` wins over `summary`
				if (body.contains("graph") && body["graph"].is_object()) {
					auto& g = body["graph"];
					if (g.contains("nodes") && g["nodes"].is_array()) {
						for (auto& n : g["nodes"]) {
							if (n.is_object()) {
								n.erase("pins");
							}
						}
					}
					g.erase("connections");
				}
				body["_summary"] = true;
			}
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
				{"cursor", CursorProperty()},
				{"fields", FieldsProperty()},
			}},
			{"required", nlohmann::json::array({"asset_path"})},
		};
		// Array of SCS components. Shape verified against BPComponent::to_json:
		// all 5 keys always emitted (parent is null for root components;
		// properties is the override list, possibly empty). `fields` narrows it.
		d.output_schema = {
			{"type","array"},
			{"items", {
				{"type","object"},
				{"properties", {
					{"name",       {{"type","string"}}},
					{"class",      {{"type","string"}}},
					{"parent",     {{"type", nlohmann::json::array({"string","null"})}}},
					{"is_root",    {{"type","boolean"}}},
					{"properties", {{"type","array"}}},
				}},
				{"required", nlohmann::json::array({"name","class","parent","is_root","properties"})},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireAssetPath(args);
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
				{"cursor", CursorProperty()},
				{"fields", FieldsProperty()},
			}},
			{"required", nlohmann::json::array({"asset_path"})},
		};
		// Array of member variables. Only `name` + `type` are structurally
		// guaranteed; the rest are present when non-default (or null for empty
		// optional strings). `fields` projection narrows this shape.
		d.output_schema = {
			{"type","array"},
			{"items", {
				{"type","object"},
				{"properties", {
					{"name", {{"type","string"}}},
					{"type", {{"type","object"},
						{"properties", {
							{"category",            {{"type","string"}}},
							{"sub_category",        {{"type","string"}}},
							{"sub_category_object", {{"type", nlohmann::json::array({"string","null"})}}},
							{"is_array",            {{"type","boolean"}}},
							{"is_set",              {{"type","boolean"}}},
							{"is_map",              {{"type","boolean"}}},
						}}}},
					{"default_value",   {{"type", nlohmann::json::array({"string","null"})}}},
					{"category",        {{"type", nlohmann::json::array({"string","null"})}}},
					{"is_replicated",   {{"type","boolean"}}},
					{"is_editable",     {{"type","boolean"}}},
					{"expose_on_spawn", {{"type","boolean"}}},
					{"rep_condition",   {{"type", nlohmann::json::array({"string","null"})}}},
					{"rep_notify_func", {{"type", nlohmann::json::array({"string","null"})}}},
				}},
				{"required", nlohmann::json::array({"name","type"})},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireAssetPath(args);
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
				{"cursor", CursorProperty()},
				{"fields", FieldsProperty()},
			}},
			{"required", nlohmann::json::array({"asset_path", "query"})},
		};
		// Array of matching nodes (BPNode shape). id/class/title/position/
		// comment/pins/meta always emitted; kind (mirrored from meta.kind) and
		// graph_name/graph_type (find_node hits) are conditional. `fields` narrows.
		d.output_schema = {
			{"type","array"},
			{"items", {
				{"type","object"},
				{"properties", {
					{"id",         {{"type","string"}}},
					{"class",      {{"type","string"}}},
					{"title",      {{"type","string"}}},
					{"position",   {{"type","object"}}},
					{"comment",    {{"type", nlohmann::json::array({"string","null"})}}},
					{"pins",       {{"type","array"}}},
					{"meta",       {{"type","object"}}},
					{"kind",       {{"type","string"}}},
					{"graph_name", {{"type","string"}}},
					{"graph_type", {{"type","string"}}},
				}},
				{"required", nlohmann::json::array({"id","class","title","position","comment","pins","meta"})},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireAssetPath(args);
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
				{"blueprint_type", {{"type","string"},
								  {"description","Optional EBlueprintType name (e.g. BPTYPE_Interface, BPTYPE_FunctionLibrary, BPTYPE_MacroLibrary). Omit for a normal BP derived from parent_class."}}},
			}},
			{"required", nlohmann::json::array({"asset_path","parent_class"})},
		};
		d.output_schema = {
			{"type", "object"},
			{"properties", {
				{"ok",              {{"type","boolean"}}},
				{"asset_path",      {{"type","string"}}},
				{"parent_class",    {{"type","string"}}},
				{"already_existed", {{"type","boolean"}}},
			}},
			{"required", nlohmann::json::array({"ok","asset_path"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset  = RequireAssetPath(args);
			std::string parent = RequireString(args, "parent_class");
			std::string bpType = OptString(args, "blueprint_type", "");
			auto r = reader.CreateBlueprint(asset, parent, bpType);
			return nlohmann::json{
				{"ok", true},
				{"asset_path", asset},
				{"already_existed", r.alreadyExisted},
				{"parent_class", r.parentClass.empty() ? parent : r.parentClass},
			};
		});
	}

	// ----- clone_graph -----------------------------------------------------
	{
		ToolDescriptor d;
		d.name = "clone_graph";
		d.description =
			"[blueprint] Copy every node in `graph` from the `source` Blueprint "
			"into the same-named graph of the `target` Blueprint, preserving "
			"wiring. The `target` must already have a graph named `graph` (create "
			"the function/event graph first with `add_function` if needed). Used "
			"by the BP-recreate flow to reproduce a graph wholesale. Recompiles + "
			"saves the target.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"source", {{"type","string"},
							{"description","Source BP package path. Example: /Game/AI/BP_Src"}}},
				{"target", {{"type","string"},
							{"description","Target BP package path. Example: /Game/AI/BP_Dst"}}},
				{"graph",  {{"type","string"},
							{"description","Graph name present in both BPs. Example: EventGraph or MyFunction"}}},
			}},
			{"required", nlohmann::json::array({"source","target","graph"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",             {{"type","boolean"}}},
				{"imported_nodes", {{"type","integer"}}},
			}},
			{"required", nlohmann::json::array({"ok","imported_nodes"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string source = RequireString(args, "source");
			std::string target = RequireString(args, "target");
			std::string graph  = RequireString(args, "graph");
			auto r = reader.CloneGraph(source, target, graph);
			return nlohmann::json{
				{"ok", r.ok},
				{"imported_nodes", r.importedNodes},
			};
		});
	}

	// ----- implement_interface ---------------------------------------------
	{
		ToolDescriptor d;
		d.name = "implement_interface";
		d.description =
			"[blueprint] Add `interface` to the implemented-interfaces list of "
			"the `asset` Blueprint, generating the stub function graphs the "
			"interface requires. `interface` accepts a BP-interface package path "
			"(/Game/...) or a native UInterface path (/Script/...). Recompiles + "
			"saves the BP.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset",     {{"type","string"},
							   {"description","BP package path. Example: /Game/AI/BP_Boss"}}},
				{"interface", {{"type","string"},
							   {"description","Interface path. Example: /Game/Interfaces/BPI_Damageable or /Script/Engine.SomeInterface"}}},
			}},
			{"required", nlohmann::json::array({"asset","interface"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok", {{"type","boolean"}}},
			}},
			{"required", nlohmann::json::array({"ok"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireString(args, "asset");
			std::string iface = RequireString(args, "interface");
			reader.ImplementInterface(asset, iface);
			return nlohmann::json{{"ok", true}};
		});
	}

}

void RegisterTools_01(ToolRegistry& registry, backends::IBlueprintReader& reader) {
	// ----- add_variable----------------------------------------------------
	{
		ToolDescriptor d;
		d.name = "add_variable";
		d.description =
			"[blueprint] Add a member variable to a Blueprint class. For graph nodes use `add_node`; for function parameters use `add_function_input`/`add_function_output`. `type` accepts either a "
			"shorthand string (\"float\", \"int\", \"bool\", \"string\", "
			"\"object:Actor\", \"struct:FVector\", \"[]float\", "
			"\"{string:int}\") or the canonical BPPinType object "
			"{category, sub_category, sub_category_object, is_array, is_set, is_map}. "
			"For a TMap the main fields are the KEY and a nested `value_type` "
			"{category, sub_category, sub_category_object} object gives the VALUE "
			"(e.g. {category:object, sub_category_object:/Script/Engine.Pawn, is_map:true, "
			"value_type:{category:object, sub_category_object:/Game/.../IndicatorDescriptor.IndicatorDescriptor_C}}). "
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
			const std::string& asset = RequireAssetPath(args);
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
			const std::string& asset = RequireAssetPath(args);
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
			const std::string& asset = RequireAssetPath(args);
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
			"[blueprint] Spawn a new K2 (Blueprint) node in a graph. For non-K2 "
			"nodes use the typed tools instead (add_widget / add_bt_node / "
			"add_anim_state / add_state_tree_state / add_material_expression / "
			"add_sequence_track). Common `kind`s: Branch, Sequence, VariableGet, "
			"VariableSet, CallFunction, CustomEvent, Event, Cast, Self, MakeArray, "
			"MakeStruct, FormatText, Knot, GetSubsystem — call list_node_kinds for "
			"the authoritative set + each kind's required arg(s). Returns "
			"{ok, node_id, pins:[...]} (pins carry name/guid/direction/type so you "
			"can wire without a follow-up get_graph).";
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
				{"subsystem_class",{{"type","string"}}},
				{"comment",        {{"type","string"}}},
				{"w",              {{"type","integer"}}},
				{"h",              {{"type","integer"}}},
				{"macro_graph",    {{"type","string"}}},
				{"variable_class", {{"type","string"}}},
				{"num_outputs",    {{"type","integer"}}},
				{"num_inputs",     {{"type","integer"}}},
				{"returns_ref",    {{"type","boolean"}}},
			}},
			{"required", nlohmann::json::array({"asset_path","graph_name","kind","x","y"})},
		};
		// node_id is the new node's GUID — agents chain it into wire_pins /
		// set_pin_default. title/class are present when the spawned node
		// resolved them.
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",      {{"type","boolean"}}},
				{"node_id", {{"type","string"}}},
				{"title",   {{"type","string"}}},
				{"class",   {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"ok","node_id"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			const std::string& asset = RequireAssetPath(args);
			const std::string& graph = RequireString(args, "graph_name");
			const std::string& kind  = RequireString(args, "kind");
			// UX-P1b: pre-validate the kind client-side so a typo yields a fast
			// did-you-mean (with the full valid set) instead of an opaque
			// backend failure. list_node_kinds is the authoritative set.
			{
				const auto& kinds = KnownNodeKinds();
				if (std::find(kinds.begin(), kinds.end(), kind) == kinds.end()) {
					std::string suggestion;  // canonical-casing match for a case typo
					for (const auto& k : kinds) {
						if (k.size() == kind.size() &&
							std::equal(k.begin(), k.end(), kind.begin(),
								[](char a, char b) {
									return std::tolower((unsigned char)a) ==
										   std::tolower((unsigned char)b);
								})) {
							suggestion = k;
							break;
						}
					}
					std::string valid;
					for (const auto& k : kinds) {
						if (!valid.empty()) { valid += ", "; }
						valid += k;
					}
					throw std::invalid_argument(fmt::format(
						"unknown node kind '{}'{} — valid kinds: {}. Call "
						"list_node_kinds for the required arg(s) per kind.",
						kind,
						suggestion.empty()
							? std::string{}
							: fmt::format(" (did you mean '{}'?)", suggestion),
						valid));
				}
			}
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
			put("subsystem_class", "SubsystemClass");
			put("comment",        "Comment");
			put("macro_graph",    "MacroGraph");
			put("variable_class", "VariableClass");
			// Integer / bool extras serialize to the string form the commandlet's
			// FParse expects (Comment box size, variadic Sequence/operator pin
			// counts, GetArrayItem ref/copy mode).
			auto putInt = [&](const char* mcpKey, const char* flagKey) {
				if (args.contains(mcpKey) && args.at(mcpKey).is_number_integer()) {
					extras.emplace(flagKey, std::to_string(args.at(mcpKey).get<int>()));
				} else {
					std::string v = OptString(args, mcpKey, "");
					if (!v.empty()) { extras.emplace(flagKey, std::move(v)); }
				}
			};
			auto putBool = [&](const char* mcpKey, const char* flagKey) {
				if (args.contains(mcpKey) && args.at(mcpKey).is_boolean()) {
					extras.emplace(flagKey, args.at(mcpKey).get<bool>() ? "true" : "false");
				} else {
					std::string v = OptString(args, mcpKey, "");
					if (!v.empty()) { extras.emplace(flagKey, std::move(v)); }
				}
			};
			putInt("w",           "W");
			putInt("h",           "H");
			putInt("num_outputs", "NumOutputs");
			putInt("num_inputs",  "NumInputs");
			putBool("returns_ref", "ReturnsRef");
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
		// Single node (BPNode shape) — same as one entry of get_graph's nodes.
		// id/class/title/position/comment/pins/meta always emitted; `kind`
		// (mirrored from meta.kind) is conditional. `fields` narrows it.
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"id",       {{"type","string"}}},
				{"class",    {{"type","string"}}},
				{"title",    {{"type","string"}}},
				{"position", {{"type","object"}}},
				{"comment",  {{"type", nlohmann::json::array({"string","null"})}}},
				{"pins",     {{"type","array"}}},
				{"meta",     {{"type","object"}}},
				{"kind",     {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"id","class","title","position","comment","pins","meta"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			const std::string& asset = RequireAssetPath(args);
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
			"loop the agent would otherwise walk.\n\n"
			"**Performance:** `parent_class` is cheap (matches against the "
			"asset summary). `function_name` and `interface` each require a "
			"full read of every candidate BP, so on a large project they "
			"can time out. When either is set, also pass `parent_class` "
			"to narrow the candidate set first — the request will be "
			"rejected otherwise.";
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
				{"cursor", CursorProperty()},
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
			// Reject the unscoped slow path. Function-name / interface
			// filters need a full ReadBlueprint per candidate, which on
			// projects with hundreds of BPs blows past the per-call
			// timeout (~60s). Force the agent to narrow with
			// parent_class — they get predictable performance and the
			// query is still expressive enough for every realistic
			// "find BPs that override X" question.
			if ((!fn.empty() || !iface.empty()) && parent.empty()) {
				throw std::invalid_argument(
					"find_overriders: when filtering by function_name or "
					"interface, you must also provide parent_class to "
					"narrow the candidate set. Unscoped scans read every "
					"BP in the project and time out at ~60s. For a true "
					"unscoped scan, use list_blueprints + N×read_blueprint "
					"with your own batching.");
			}
			auto ctl = ParseResponseControls(args);

			// Hard cap on per-BP reads so even a broad parent_class
			// (e.g. parent_class="Actor" on a Lyra-size project) can't
			// blow past the daemon timeout. When function_name or
			// interface forces full reads on every matching candidate,
			// stop after kMaxFullReads and surface a `truncated` marker.
			// `parent_class` alone uses cheap summary-only matching and
			// isn't capped — only the read path is.
			constexpr int kMaxFullReads = 200;
			int fullReads = 0;
			bool truncated = false;

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
					if (fullReads >= kMaxFullReads) {
						truncated = true;
						break;
					}
					++fullReads;
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
			if (truncated) {
				// Append a sentinel object so the agent sees the scan was
				// capped mid-flight. Same shape as a normal row plus a
				// `_truncated` flag for easy detection.
				out.push_back({
					{"_truncated",      true},
					{"_scanned_reads",  fullReads},
					{"_cap",            kMaxFullReads},
					{"_hint", fmt::format(
						"find_overriders stopped after {} full BP reads. "
						"Re-run with a narrower `path` (e.g. /Game/AI) or a "
						"more specific `parent_class` to scan fewer candidates.",
						kMaxFullReads)},
				});
			}
			return out;
		});
	}

	// ----- find_dangling_references ---------------------------------------
	// Walk a Blueprint's function bodies (and optionally top-level graphs)
	// looking for nodes that reference variables / functions which no
	// longer exist on the BP. Catches the post-refactor "renamed the
	// variable but kept the getter" scenario without forcing the agent to
	// enumerate every node by hand.
	//
	// Limitations of the MCP-side implementation:
	//   - External (cross-BP) function calls aren't validated. We can
	//     only confirm calls to functions declared on THIS BP.
	//   - Widget-binding references (UMG `bind_widget`) need typed-asset
	//     introspection we don't have access to from read_blueprint;
	//     left for a future plugin-side `validate_blueprint` op that
	//     can look at the full UClass + WidgetTree.
	//   - Component-reference nodes (variables typed as components)
	//     show up as plain VariableGet/Set nodes and ARE covered.
	{
		ToolDescriptor d;
		d.name = "find_dangling_references";
		d.description =
			"[blueprint] Walk a Blueprint's function bodies (and optionally top-level "
			"graphs) for nodes referencing variables / intra-BP functions that "
			"no longer exist on the BP. Catches the renamed-or-deleted-but-"
			"node-still-references scenario.\n\nReturns "
			"`{asset_path, dangling: [...], total}`. Each dangling entry is "
			"`{graph, node_id, node_class, title, missing, symbol_type}`. "
			"Empty `dangling` array means clean.\n\nCoverage limits: external "
			"(cross-BP) function calls aren't validated — we can only "
			"confirm intra-BP calls. UMG bind_widget references need a "
			"future plugin-side validator. Inherited parent-class members "
			"(e.g. PlayerCameraManager) are treated as valid, not dangling.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path", {{"type","string"},
								{"description","UE asset path, e.g. /Game/AI/BP_Enemy"}}},
				{"include_top_level", {{"type","boolean"},
									   {"description","Also fetch + scan top-level graphs "
										"(EventGraph, ConstructionScript). Costs N×get_graph "
										"on the wire. Default true."}}},
			}},
			{"required", nlohmann::json::array({"asset_path"})},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path", {{"type","string"}}},
				{"total",      {{"type","integer"},{"minimum",0}}},
				{"dangling", {
					{"type","array"},
					{"items", {
						{"type","object"},
						{"properties", {
							{"graph",       {{"type","string"}}},
							{"node_id",     {{"type","string"}}},
							{"node_class",  {{"type","string"}}},
							{"title",       {{"type","string"}}},
							{"missing",     {{"type","string"}}},
							{"symbol_type", {{"type","string"},
											 {"enum", nlohmann::json::array({"variable","function"})}}},
						}},
					}},
				}},
			}},
			{"required", nlohmann::json::array({"asset_path","dangling","total"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireAssetPath(args);
			const bool includeTopLevel = args.value("include_top_level", true);

			BPMetadata meta = reader.ReadBlueprint(asset);

			std::set<std::string, std::less<>> knownVars;
			for (const auto& v : meta.Variables) {
				knownVars.insert(v.Name);
			}
			std::set<std::string, std::less<>> knownFns;
			for (const auto& f : meta.Functions) {
				knownFns.insert(f.Name);
			}

			// Pull a string value out of node Meta accepting either
			// camelCase or snake_case — node Meta keys aren't fully
			// normalized across the plugin emitter vs mock fixtures.
			auto getMetaString = [](const nlohmann::json& meta,
									std::initializer_list<const char*> keys) -> std::string {
				if (!meta.is_object()) {
					return {};
				}
				for (const char* k : keys) {
					auto it = meta.find(k);
					if (it != meta.end() && it->is_string()) {
						return it->get<std::string>();
					}
				}
				return {};
			};

			nlohmann::json dangling = nlohmann::json::array();
			auto addEntry = [&](const std::string& graphName, const BPNode& n,
								const std::string& missing, const char* type) {
				dangling.push_back({
					{"graph",       graphName},
					{"node_id",     n.Id},
					{"node_class",  n.Class},
					{"title",       n.Title},
					{"missing",     missing},
					{"symbol_type", type},
				});
			};

			auto scanGraph = [&](const BPGraph& g) {
				for (const auto& n : g.Nodes) {
					// Variable references. K2 emits VariableGet/Set classes
					// (sometimes K2Node_VariableGet, sometimes shorter forms
					// in fixtures — match on substring).
					const bool isVarRef =
						n.Class.find("VariableGet") != std::string::npos ||
						n.Class.find("VariableSet") != std::string::npos;
					if (isVarRef) {
						std::string varName = getMetaString(n.Meta,
							{"variable_name", "variableName", "var_name", "VarName"});
						if (varName.empty()) {
							continue;
						}
						// Only flag self/own-class variable references. A ref whose
						// owner class is a parent/external class is INHERITED, not
						// dangling — knownVars enumerates only this BP's own declared
						// variables, so an inherited member (e.g. PlayerCameraManager
						// on APlayerController) would otherwise be a false positive.
						// Mirrors the self-call heuristic used for functions below.
						const std::string varClass = getMetaString(n.Meta,
							{"variable_class", "variableClass"});
						const bool isOwnMember = varClass.empty() ||
							varClass == meta.Name ||
							varClass.find(meta.Name) != std::string::npos;
						if (isOwnMember && knownVars.find(varName) == knownVars.end()) {
							addEntry(g.Name, n, varName, "variable");
						}
						continue;
					}
					// Intra-BP function calls. Only flag calls whose
					// target_class is this BP (or empty, meaning "self") —
					// external calls (Engine APIs, other BPs) need a
					// project-wide registry we don't have MCP-side.
					if (n.Class.find("CallFunction") != std::string::npos) {
						std::string fnName = getMetaString(n.Meta,
							{"function_name", "functionName"});
						std::string targetClass = getMetaString(n.Meta,
							{"target_class", "targetClass"});
						if (fnName.empty()) {
							continue;
						}
						// Treat empty target_class as a self-call. If
						// target_class is set, treat as intra-BP only when
						// it matches the BP's class (heuristic: short name
						// or full path containing the BP's name).
						const bool isSelfCall = targetClass.empty() ||
							targetClass == meta.Name ||
							targetClass.find(meta.Name) != std::string::npos;
						if (isSelfCall && knownFns.find(fnName) == knownFns.end()) {
							addEntry(g.Name, n, fnName, "function");
						}
					}
				}
			};

			// BPMetadata only has function summaries. Pull each function's
			// full body graph one at a time. (Plugin-side may consolidate
			// this in a future find_dangling_references op.)
			for (const auto& fnSummary : meta.Functions) {
				try {
					BPFunction fn = reader.GetFunction(asset, fnSummary.Name);
					scanGraph(fn.Graph);
				} catch (...) {
					// Skip functions we can't fetch — interface stubs,
					// inherited UE base functions, etc.
				}
			}

			// Top-level graphs (EventGraph, UserConstructionScript, etc.)
			// need a separate fetch since BPMetadata.Graphs is summary-only.
			if (includeTopLevel) {
				for (const auto& gs : meta.Graphs) {
					try {
						BPGraph g = reader.GetGraph(asset, gs.Name);
						scanGraph(g);
					} catch (...) {
						// Skip graphs we can't fetch — animation state
						// machines, delegate graphs, etc. may need typed
						// readers that don't apply here.
					}
				}
			}

			return nlohmann::json{
				{"asset_path", asset},
				{"dangling",   dangling},
				{"total",      static_cast<int>(dangling.size())},
			};
		});
	}

}

void RegisterTools_01b(ToolRegistry& registry, backends::IBlueprintReader& reader) {
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
			const std::string asset    = RequireAssetPath(args);
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

	// ----- set_pin_default -------------------------------------------------
	// Sets the literal default on a pin. UE doesn't have a first-class
	// literal node, so the value is materialized as the consumer pin's
	// default — same backing op apply_ops uses and the same code path
	// compile_function's `{lit: value}` emits internally. Surfaced as a
	// standalone tool because README/SKILL.md advertise it.
	{
		ToolDescriptor d;
		d.name = "set_pin_default";
		d.description =
			"[blueprint] Set the literal default value on a node's input pin. UE has no "
			"first-class literal node — values flow in as the consumer pin's "
			"default. `pin` accepts a pin GUID (preferred — get_graph) or a "
			"pin name. `value` is the literal as a string (the editor parses "
			"it per the pin's type: numbers, bool true/false, strings raw, "
			"vectors as `(X=...,Y=...,Z=...)`). This is what "
			"`compile_function`'s `{lit: ...}` expression emits under the "
			"hood; reach for `set_pin_default` directly when you already "
			"have the node + pin and just want the default set.";
		d.input_schema = {
			{"type","object"},
			{"properties", {
				{"asset_path", {{"type","string"}}},
				{"graph_name", {{"type","string"},
								{"description","Graph holding the node. EventGraph if omitted."}}},
				{"node_id",    {{"type","string"},
								{"description","Target node's GUID."}}},
				{"pin",        {{"type","string"},
								{"description","Pin GUID (preferred) or pin name. Alias: `pin_name` is also accepted, for parity with the apply_ops set_pin_default op."}}},
				{"value",      {{"type","string"},
								{"description","Literal value as a string; editor parses per pin type."}}},
			}},
			{"required", nlohmann::json::array({"asset_path","node_id","value"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			const std::string asset = RequireAssetPath(args);
			const std::string graph = OptString(args, "graph_name", "EventGraph");
			const std::string node  = RequireString(args, "node_id");
			// Accept `pin` (this tool's field) or `pin_name` (the apply_ops
			// op's field) so the two surfaces are interchangeable.
			std::string pin = OptString(args, "pin", "");
			if (pin.empty()) { pin = RequireString(args, "pin_name"); }
			const std::string value = RequireString(args, "value");
			reader.SetPinDefault(asset, graph, node, pin, value);
			return nlohmann::json{{"ok", true}};
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
			const std::string& asset = RequireAssetPath(args);
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
				RequireAssetPath(args),
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
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",                {{"type","boolean"}}},
				{"asset_path",        {{"type","string"}}},
				{"source_asset_path", {{"type","string"}}},
				{"already_existed",   {{"type","boolean"}}},
			}},
			{"required", nlohmann::json::array({"ok","asset_path","source_asset_path","already_existed"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string source = RequireAssetPath(args);
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
				RequireAssetPath(args),
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
				RequireAssetPath(args),
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
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",              {{"type","boolean"}}},
				{"function_name",   {{"type","string"}}},
				{"already_existed", {{"type","boolean"}}},
				{"entry_node_id",   {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"ok","function_name","already_existed"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			const std::string& asset = RequireAssetPath(args);
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
			const std::string& asset = RequireAssetPath(args);
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
			const std::string& asset = RequireAssetPath(args);
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
				RequireAssetPath(args),
				RequireString(args, "name"));
			return nlohmann::json{{"ok", true}};
		});
	}

}

void RegisterTools_02(ToolRegistry& registry, backends::IBlueprintReader& reader) {
	// ----- set_variable_default--------------------------------------------
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
				RequireAssetPath(args),
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
		d.output_schema = {
			{"type","array"},
			{"items", {
				{"type","object"},
				{"properties", {
					{"kind",        {{"type","string"}}},
					{"class",       {{"type","string"}}},
					{"description", {{"type","string"}}},
					{"extras",      {{"type","array"}}},
				}},
				{"required", nlohmann::json::array({"kind","class","description"})},
			}},
		};
		registry.Add(std::move(d), [](const nlohmann::json&) {
			return nlohmann::json::array({
				nlohmann::json{
					{"kind", "GetSubsystem"},
						{"class", "K2Node_GetSubsystem"},
						{"description", "Get a USubsystem instance (GameInstance / World / Engine subsystem). The output pin is typed to the subsystem class. (LocalPlayer subsystems use a different node and aren't covered.)"},
						{"extras", nlohmann::json::array({
							nlohmann::json{{"name","subsystem_class"}, {"required",true},
										   {"description","USubsystem subclass — UClass path (/Script/Module.MySubsystem) or short name."}}
						})},
					},
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
					{"kind", "Event"},
					{"class", "K2Node_Event"},
					{"description", "Bind an OVERRIDABLE engine/parent event (Event BeginPlay, Event Tick, Event ActorBeginOverlap, ...) — distinct from CustomEvent (a new event the BP declares). Reproduce a K2Node_Event from its eventName/eventClass meta."},
					{"extras", nlohmann::json::array({
						nlohmann::json{{"name","event_name"}, {"required",true},
									   {"description","UFunction name of the override, e.g. ReceiveBeginPlay (the K2Node_Event's eventName meta)."}},
						nlohmann::json{{"name","function_owner"}, {"required",false},
									   {"description","Class declaring the event, e.g. /Script/Engine.Actor (eventClass meta). Omit to search the parent hierarchy."}}
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
				nlohmann::json{
					{"kind", "Comment"},
					{"class", "EdGraphNode_Comment"},
					{"description", "Cosmetic comment box around nodes. No pins; the box title is its comment text."},
					{"extras", nlohmann::json::array({
						nlohmann::json{{"name","comment"}, {"required",false},
									   {"description","Comment box text (also its node title)."}},
						nlohmann::json{{"name","w"}, {"required",false}, {"description","Box width (default 400)."}},
						nlohmann::json{{"name","h"}, {"required",false}, {"description","Box height (default 100)."}}
					})},
				},
				nlohmann::json{
					{"kind", "GetArrayItem"},
					{"class", "K2Node_GetArrayItem"},
					{"description", "Array element accessor (GET []). Array/element types resolve from the connected Array pin."},
					{"extras", nlohmann::json::array()},
				},
				nlohmann::json{
					{"kind", "Select"},
					{"class", "K2Node_Select"},
					{"description", "Select node — pick one of N option values by an index/bool/enum. Wildcard until connected."},
					{"extras", nlohmann::json::array()},
				},
				nlohmann::json{
					{"kind", "SpawnActor"},
					{"class", "K2Node_SpawnActorFromClass"},
					{"description", "Spawn an actor from a class. The spawn class is set via the Class pin (default or wired)."},
					{"extras", nlohmann::json::array()},
				},
				nlohmann::json{
					{"kind", "BreakStruct"},
					{"class", "K2Node_BreakStruct"},
					{"description", "Break a USTRUCT into its members (mirror of MakeStruct). Each member becomes an output pin."},
					{"extras", nlohmann::json::array({
						nlohmann::json{{"name","struct_type"}, {"required",true},
									   {"description","UScriptStruct path (the BreakStruct's structType meta)."}}
					})},
				},
				nlohmann::json{
					{"kind", "MacroInstance"},
					{"class", "K2Node_MacroInstance"},
					{"description", "Instance of a macro graph (ForEachLoop, IsValid, project macro libraries, ...)."},
					{"extras", nlohmann::json::array({
						nlohmann::json{{"name","macro_graph"}, {"required",true},
									   {"description","Full object path of the macro's UEdGraph (the macroGraph meta), e.g. /Engine/EditorBlueprintResources/StandardMacros.StandardMacros:IsValid."}}
					})},
				},
				nlohmann::json{
					{"kind", "CallParent"},
					{"class", "K2Node_CallParentFunction"},
					{"description", "Call the parent class's implementation of a (usually overridden) function, e.g. Parent: BeginPlay."},
					{"extras", nlohmann::json::array({
						nlohmann::json{{"name","function"}, {"required",true},
									   {"description","Function name (the targetFunction meta)."}},
						nlohmann::json{{"name","function_owner"}, {"required",false},
									   {"description","Declaring class path (targetClass meta). Omit to search the parent hierarchy."}}
					})},
				},
				nlohmann::json{
					{"kind", "PromotableOp"},
					{"class", "K2Node_PromotableOperator"},
					{"description", "Wildcard math/comparison operator (+, -, *, ==, ...) backed by a KismetMathLibrary function."},
					{"extras", nlohmann::json::array({
						nlohmann::json{{"name","function"}, {"required",true},
									   {"description","Backing operator function, e.g. Multiply_DoubleDouble (targetFunction meta)."}},
						nlohmann::json{{"name","function_owner"}, {"required",true},
									   {"description","Class path, e.g. /Script/Engine.KismetMathLibrary (targetClass meta)."}}
					})},
				},
				nlohmann::json{
					{"kind", "CommutativeOp"},
					{"class", "K2Node_CommutativeAssociativeBinaryOperator"},
					{"description", "Variadic commutative/associative operator (Append, AND, OR, ...) backed by a library function."},
					{"extras", nlohmann::json::array({
						nlohmann::json{{"name","function"}, {"required",true},
									   {"description","Backing function, e.g. Concat_StrStr (targetFunction meta)."}},
						nlohmann::json{{"name","function_owner"}, {"required",true},
									   {"description","Class path (targetClass meta)."}}
					})},
				},
				nlohmann::json{
					{"kind", "Message"},
					{"class", "K2Node_Message"},
					{"description", "Call a Blueprint Interface message on a target (safe no-op if the target doesn't implement it)."},
					{"extras", nlohmann::json::array({
						nlohmann::json{{"name","function"}, {"required",true},
									   {"description","Interface function name (targetFunction meta)."}},
						nlohmann::json{{"name","function_owner"}, {"required",true},
									   {"description","Interface class path (targetClass meta)."}}
					})},
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
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"categories", {{"type","array"}}},
			}},
			{"required", nlohmann::json::array({"categories"})},
		};
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
					nlohmann::json{{"flag","is_map"},   {"description","TMap<K,V> — the main fields are the key; add a nested `value_type` {category, sub_category, sub_category_object} for the value (or use shorthand {K:V})."}},
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
			"daemon is alive returns was_running:false without erroring.\n\n"
			"GUARD: because this affects every session, you must pass "
			"`force_shared:true` to actually tear it down — a default-deny "
			"safeguard against accidentally killing other sessions' daemon.";
		d.input_schema = {{"type", "object"}, {"properties", {
			{"force_shared", {
				{"type", "boolean"},
				{"description",
				 "Required confirmation. shutdown_daemon terminates the daemon "
				 "shared by EVERY MCP session on this project, not just yours. "
				 "Must be true to proceed; omitting it (default-deny) returns an "
				 "error explaining the blast radius rather than tearing down."},
			}},
		}}};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			// Client feedback #5: the shared-daemon model means this kills the
			// daemon for ALL sessions. Default-deny so a routine "clean up my
			// session" call can't blindside other sessions; require explicit
			// force_shared:true to proceed.
			const bool forceShared = args.is_object() && args.value("force_shared", false);
			if (!forceShared) {
				throw std::invalid_argument(
					"shutdown_daemon tears down the editor daemon shared by EVERY "
					"MCP session on this project (releasing its file locks), not "
					"just yours — other sessions then pay a cold-start respawn. If "
					"that's what you intend, call again with force_shared:true. To "
					"keep querying while you open the full editor instead, set "
					"BP_READER_READ_ONLY=1 and leave the daemon running.");
			}
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
			std::string asset = RequireAssetPath(args);
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
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",                {{"type","boolean"}}},
				{"project_name",      {{"type","string"}}},
				{"project_path",      {{"type","string"}}},
				{"engine_association",{{"type","string"}}},
				{"category",          {{"type","string"}}},
				{"description",       {{"type","string"}}},
				{"raw",               {{"type","object"}}},
			}},
			{"required", nlohmann::json::array({"ok","project_name","project_path","engine_association","category","description"})},
		};
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

}

void RegisterTools_02b(ToolRegistry& registry, backends::IBlueprintReader& reader) {
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
		d.output_schema = {
			{"type", "object"},
			{"properties", {
				{"ok",        {{"type","boolean"}}},
				{"saved",     {{"type","integer"}, {"minimum", 0}}},
				{"failed",    {{"type","array"},   {"items", {{"type","string"}}}}},
			}},
			{"required", nlohmann::json::array({"ok","saved"})},
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
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",                  {{"type","boolean"}}},
				{"source_path",         {{"type","string"}}},
				{"dest_path",           {{"type","string"}}},
				{"redirectors_created", {{"type","integer"}}},
			}},
			{"required", nlohmann::json::array({"ok","source_path","dest_path","redirectors_created"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string src  = RequireAssetPath(args);
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
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",                 {{"type","boolean"}}},
				{"path",               {{"type","string"}}},
				{"deleted",            {{"type","boolean"}}},
				{"referencing_assets", {{"type","array"}, {"items", {{"type","string"}}}}},
			}},
			{"required", nlohmann::json::array({"ok","path","deleted","referencing_assets"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string path = RequireAssetPath(args);
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
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",             {{"type","boolean"}}},
				{"path",           {{"type","string"}}},
				{"already_existed",{{"type","boolean"}}},
			}},
			{"required", nlohmann::json::array({"ok","path","already_existed"})},
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
		d.output_schema = {
			{"type","array"},
			{"items", {
				{"type","object"},
				{"properties", {
					{"asset_path",   {{"type","string"}}},
					{"name",         {{"type","string"}}},
					{"parent_class", {{"type","string"}}},
					{"modified_iso", {{"type","string"}}},
				}},
				{"required", nlohmann::json::array({"asset_path","name","parent_class"})},
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
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",         {{"type","boolean"}}},
				{"asset_path", {{"type","string"}}},
				{"row_struct", {{"type","string"}}},
				{"columns",    {{"type","array"}}},
				{"rows",       {{"type","array"}}},
			}},
			{"required", nlohmann::json::array({"ok","asset_path","row_struct","columns","rows"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string asset = RequireAssetPath(args);
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
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",     {{"type","boolean"}}},
				{"output", {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"ok","output"})},
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
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",              {{"type","boolean"}}},
				{"command_result",  {{"type","string"}}},
				{"log",             {{"type","array"}}},
				{"error",           {{"type","string"},
					{"description","Set on failure, e.g. \"python_disabled\"."}}},
				{"hint",            {{"type","string"}}},
				{"backend_message", {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"ok","command_result","log"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string code = RequireString(args, "code");
			// Catch the "not supported by this backend" path (mock,
			// or any Auto/Caching/ReadOnly chain that doesn't override
			// RunPythonScript) and present it as the documented
			// `python_disabled` envelope. Agents reading the README expect
			// this shape; the raw exception text leaks an
			// implementation detail.
			bpr::backends::IBlueprintReader::PythonResult r;
			try {
				r = reader.RunPythonScript(code);
			} catch (const bpr::backends::BlueprintReaderError& e) {
				return nlohmann::json{
					{"ok", false},
					{"error", "python_disabled"},
					{"hint",
					 "Editor side rejected the request — either the Python "
					 "plugin isn't loaded in this editor build, or "
					 "BP_READER_ALLOW_PYTHON isn't set on the MCP server. "
					 "Set BP_READER_ALLOW_PYTHON=1 and launch an editor "
					 "with the Python Editor Script Plugin enabled."},
					{"backend_message", std::string(e.what())},
					{"command_result", ""},
					{"log", nlohmann::json::array()},
				};
			}
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
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",          {{"type","boolean"}}},
				{"asset_path",  {{"type","string"}}},
				{"referencers", {{"type","array"}, {"items", {{"type","string"}}}}},
				{"count",       {{"type","integer"}}},
			}},
			{"required", nlohmann::json::array({"ok","asset_path","referencers","count"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string ap = RequireAssetPath(args);
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
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",           {{"type","boolean"}}},
				{"asset_path",   {{"type","string"}}},
				{"dependencies", {{"type","array"}, {"items", {{"type","string"}}}}},
				{"count",        {{"type","integer"}}},
			}},
			{"required", nlohmann::json::array({"ok","asset_path","dependencies","count"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string ap = RequireAssetPath(args);
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
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",      {{"type","boolean"}}},
				{"section", {{"type","string"}}},
				{"key",     {{"type","string"}}},
				{"file",    {{"type","string"}}},
				{"exists",  {{"type","boolean"}}},
				{"value",   {{"type","string"},
					{"description","Present only when exists=true."}}},
			}},
			{"required", nlohmann::json::array({"ok","section","key","file","exists"})},
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
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",      {{"type","boolean"}}},
				{"queued",  {{"type","boolean"}}},
				{"quality", {{"type","string"}}},
				{"note",    {{"type","string"}}},
			}},
			{"required", nlohmann::json::array({"ok","queued","quality"})},
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
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",     {{"type","boolean"}}},
				{"name",   {{"type","string"}}},
				{"value",  {{"type","string"}}},
				{"help",   {{"type","string"}}},
				{"exists", {{"type","boolean"}}},
			}},
			{"required", nlohmann::json::array({"ok","name","value","help","exists"})},
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
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",     {{"type","boolean"}}},
				{"name",   {{"type","string"}}},
				{"value",  {{"type","string"}}},
				{"exists", {{"type","boolean"}}},
			}},
			{"required", nlohmann::json::array({"ok","name","value","exists"})},
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
			"`standalone`, `vr_preview`. Requires a rendering-capable (GPU) "
			"editor: in a headless (-nullrhi) commandlet/daemon it returns "
			"`started:false` with an explanatory `note` rather than silently "
			"queuing a play session that never sustains. Use the live backend.";
		d.input_schema = {
			{"type","object"},
			{"properties", {{"mode", {{"type","string"},
				{"enum", nlohmann::json::array({"selected_viewport","new_editor_window","standalone","vr_preview"})}}}}},
		};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",      {{"type","boolean"}}},
				{"started", {{"type","boolean"}}},
				{"mode",    {{"type","string"}}},
				{"note",    {{"type","string"},
					{"description","Present only when started=false — why PIE could "
						"not start (e.g. a headless -nullrhi session)."}}},
			}},
			{"required", nlohmann::json::array({"ok","started","mode"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
			std::string mode = OptString(args, "mode", "selected_viewport");
			auto r = reader.PieStart(mode);
			nlohmann::json out{{"ok", true},
							   {"started", r.started}, {"mode", r.mode}};
			if (!r.note.empty()) { out["note"] = r.note; }
			return out;
		});
	}
	{
		ToolDescriptor d;
		d.name = "pie_stop";
		d.description =
			"[editor] End the active PIE session. No-op when PIE isn't running.";
		d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
		d.output_schema = {
			{"type","object"},
			{"properties", {
				{"ok",      {{"type","boolean"}}},
				{"stopped", {{"type","boolean"}}},
			}},
			{"required", nlohmann::json::array({"ok","stopped"})},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json&) {
			auto r = reader.PieStop();
			return nlohmann::json{{"ok", true}, {"stopped", r.stopped}};
		});
	}

	// ===== Phase 8 EA-pull Wave 1 (partial) =================================
	// Editor-awareness reads. Reactive-workflow foundation —
	// "what is the user doing right now?" without polling individual ops.
	// All require a live editor; commandlet/mock backends throw a clean
	// "not supported by this backend" error.

	// ----- list_open_assets -----------------------------------------------
	{
		ToolDescriptor d;
		d.name = "list_open_assets";
		d.description =
			"[editor] List every asset the user currently has open in some asset "
			"editor (one entry per editor window). Returns "
			"`[{asset_path, asset_class, last_activation_seconds}, ...]`. "
			"Sort by `last_activation_seconds` to see most-recently-used first. "
			"Requires a live editor — commandlet mode throws.";
		d.input_schema = {{"type","object"}, {"properties", nlohmann::json::object()}};
		d.output_schema = {
			{"type", "array"},
			{"items", {
				{"type", "object"},
				{"properties", {
					{"asset_path",                 {{"type", "string"}}},
					{"asset_class",                {{"type", "string"}}},
					{"last_activation_seconds",    {{"type", "number"}}},
				}},
			}},
		};
		registry.Add(std::move(d), [&reader](const nlohmann::json&) {
			auto r = reader.ListOpenAssets();
			nlohmann::json body = nlohmann::json::array();
			for (const auto& e : r.entries) {
				body.push_back({
					{"asset_path",                 e.assetPath},
					{"asset_class",                e.assetClass},
					{"last_activation_seconds",    e.lastActivationSeconds},
				});
			}
			return body;
		});
	}

}

void RegisterBlueprintTools(ToolRegistry& registry, backends::IBlueprintReader& reader) {
	RegisterTools_00(registry, reader);
	RegisterTools_00b(registry, reader);
	RegisterTools_01(registry, reader);
	RegisterTools_01b(registry, reader);
	RegisterTools_02(registry, reader);
	RegisterTools_02b(registry, reader);
	RegisterTools_03(registry, reader);
	RegisterTools_03b(registry, reader);
	RegisterTools_04(registry, reader);
	RegisterTools_04b(registry, reader);
	RegisterTools_05(registry, reader);
	RegisterTools_06(registry, reader);
	RegisterTools_07(registry, reader);
	RegisterTools_08(registry, reader);
	RegisterTools_08b(registry, reader);
	RegisterTools_09(registry, reader);
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
		// UX-P0b: an unknown / misspelled category would otherwise be a silent
		// no-op (ActivateToken skips tokens it can't expand) — the agent thinks
		// it widened the surface, then can't find the tool. Turn it into a
		// one-turn-correctable did-you-mean error. A genuine already-active
		// category still reaches ActivateToken below and returns ok:true with
		// added:[].
		if (!registry.IsKnownToken(token)) {
			const std::string suggestion = registry.SuggestToken(token);
			throw std::invalid_argument(fmt::format(
				"unknown tool category '{}'{} — call list_toolsets for the full "
				"set, or pass \"all\" to activate every tool.",
				token,
				suggestion.empty()
					? std::string{}
					: fmt::format(" (did you mean '{}'?)", suggestion)));
		}
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
