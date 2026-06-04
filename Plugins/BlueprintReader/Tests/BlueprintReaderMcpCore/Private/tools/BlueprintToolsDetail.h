#pragma once

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

namespace bpr::tools {
namespace blueprint_tools_detail {

// Strip a trailing object-path suffix from a UE asset path so both the package
// form (/Game/AI/BP_Foo) and the object form (/Game/AI/BP_Foo.BP_Foo) are
// accepted transparently.  The editor commandlet's LoadMutableBlueprint tolerates
// both forms for BP assets, but the mock backend does an exact-keyed lookup and
// non-BP LoadObject callsites (DataTable, Material, …) do not append the suffix
// themselves — so without this step the object form silently fails on mock and
// on those callsites.  The output side already normalises via ToPackagePath;
// this mirrors that on the input side so the two are symmetric.
//
// Declared outside the anonymous namespace so it can be included and called
// from test files (anonymous-namespace names have internal linkage and cannot
// be named from a different TU).
inline std::string NormalizeAssetPath(std::string path) {
	// Trim leading/trailing whitespace.
	auto trimL = path.find_first_not_of(" \t\r\n");
	if (trimL == std::string::npos) { return path; }
	auto trimR = path.find_last_not_of(" \t\r\n");
	path = path.substr(trimL, trimR - trimL + 1);
	// Normalise backslashes to forward slashes.
	for (char& c : path) { if (c == '\\') { c = '/'; } }
	// Strip the trailing ".ClassName" object suffix (only after the last '/').
	auto lastSlash = path.rfind('/');
	auto dotPos    = path.rfind('.');
	if (dotPos != std::string::npos && (lastSlash == std::string::npos || dotPos > lastSlash)) {
		path.erase(dotPos);
	}
	return path;
}

namespace {   // per-TU internal linkage: included by 3 TUs, no ODR clash, no inline churn

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

// Path-traversal guard. Used by file-writing tools (screenshots,
// write_generated_source) to refuse caller paths that escape the
// project sandbox. The plugin-side commandlet does its own check (e.g.
// write_generated_source restricts to <ProjectDir>/Source/) — this is
// defense in depth at the MCP boundary so an obvious `..` traversal is
// stopped before the call even leaves the server.
//
// What we reject:
//   * Any segment equal to `..` (after normalizing slashes)
//   * Bare `~` (home-dir expansion is shell-specific, never wanted here)
//   * Empty strings (caller almost certainly forgot to set the field)
//
// What we allow:
//   * Absolute Windows paths (`C:\...`) and Unix paths (`/...`)
//   * Relative paths without `..` (resolved by the receiver against
//     its own CWD; plugin / backend enforces further restrictions)
//
// Pure function — no I/O, no env lookup. Returns empty string on
// success, error message on rejection.
std::string ValidateFilePath(const std::string& path) {
	if (path.empty()) {
		return "path is empty";
	}
	if (path.front() == '~') {
		return "path uses '~' home-dir expansion (not supported): " + path;
	}
	// Normalize separators for scan; doesn't mutate the caller's path.
	std::string normalized;
	normalized.reserve(path.size());
	for (char c : path) {
		normalized.push_back(c == '\\' ? '/' : c);
	}
	// Walk segments, looking for `..` as a standalone segment. `foo..bar`
	// is fine — only the literal traversal segment is rejected.
	size_t i = 0;
	while (i < normalized.size()) {
		const auto next = normalized.find('/', i);
		const auto segEnd = (next == std::string::npos) ? normalized.size() : next;
		if (segEnd - i == 2 &&
			normalized[i] == '.' && normalized[i + 1] == '.') {
			return "path contains '..' traversal segment: " + path;
		}
		if (next == std::string::npos) break;
		i = next + 1;
	}
	return {};
}

// Throws std::invalid_argument when ValidateFilePath rejects. Helper
// for call sites that prefer exceptions to error strings.
void RequireSafeFilePath(const std::string& path) {
	if (auto err = ValidateFilePath(path); !err.empty()) {
		throw std::invalid_argument(err);
	}
}

// Phase D's inline-image cap. Captures over this max-dim are refused
// at inline-emit time so we don't ship multi-MB base64 over the wire.
// 1280px tracks the LLM-vision sweet spot for 2025-2026 frontier models.
constexpr uint32_t kInlineImageMaxDim = 1280;

// Transpile gate. The 6 BP-to-C++ tools (decompile_function,
// decompile_blueprint, transpile_function, transpile_blueprint,
// write_generated_source, parse_cpp_function) are off by default.
// Setting BP_READER_ALLOW_TRANSPILE=1 (or true / yes / on) in the
// MCP server's env enables them. Off by default keeps the surface
// explicit — the transpile path can write source files, parse
// untrusted C++, and shells out to UBT downstream, so opt-in is
// the safer default.
bool TranspileEnabled() {
	const char* raw = std::getenv("BP_READER_ALLOW_TRANSPILE");
	if (!raw || !*raw) {
		return false;
	}
	std::string v(raw);
	for (char& c : v) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return v == "1" || v == "true" || v == "yes" || v == "on";
}

nlohmann::json TranspileDisabledResponse(std::string_view toolName) {
	return nlohmann::json{
		{"ok", false},
		{"error", "transpile_disabled"},
		{"tool", std::string(toolName)},
		{"hint",
			"Set BP_READER_ALLOW_TRANSPILE=1 in the MCP server's env to "
			"enable the BP-to-C++ transpile tools (decompile_function, "
			"decompile_blueprint, transpile_function, transpile_blueprint, "
			"write_generated_source, parse_cpp_function). Off by default "
			"because these tools shell to UBT, write source files, and "
			"parse caller-supplied C++."},
	};
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
		 "names of its variables. Omit to get the full payload. Field names "
		 "must match the response keys: variable flags are `is_editable`, "
		 "`is_replicated`, `expose_on_spawn`, `rep_condition`, "
		 "`rep_notify_func`. Common shorthands are aliased (`editable`, "
		 "`replicated`, `exposed`, and any bare `foo` for an `is_foo` flag), "
		 "but an unrecognized name still projects nothing."},
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
nlohmann::json CursorProperty() {
	return {
		{"type", "string"},
		{"description", "Opaque pagination cursor (base64). Alternative to "
						"`offset` — pass the cursor returned from a previous "
						"call to advance to the next page. Eventually "
						"deprecates `offset`. Invalid cursor → -32602."},
	};
}
// The standard paginated-envelope output_schema used by all list_* tools.
// Items are plain objects; tools may add additional items metadata if needed.
nlohmann::json PaginatedSchema() {
	return {
		{"type","object"},
		{"properties", {
			{"total",       {{"type","integer"}}},
			{"count",       {{"type","integer"}}},
			{"has_more",    {{"type","boolean"}}},
			{"next_cursor", {{"type",nlohmann::json::array({"string","null"})}}},
			{"results",     {{"type","array"}, {"items",{{"type","object"}}}}},
		}},
		{"required", nlohmann::json::array({"total","count","has_more","results"})},
	};
}
nlohmann::json SortProperty() {
	return {
		{"type", "string"},
		{"enum", nlohmann::json::array({"natural", "name", "path"})},
		{"description", "Optional result ordering. `natural` (default) preserves "
						"the order the backend returned. `name` sorts by the "
						"`name` field of each entry; `path` sorts by `asset_path`. "
						"Sort applies BEFORE limit/offset, so paging works against "
						"the sorted view."},
	};
}

// Post-serialization JSON pruning — strips noise from tool responses so the
// AI receives a leaner payload.  Only touches the MCP-server-composed JSON;
// the editor-side emitters are untouched (their from_json decoders would throw
// on missing keys).  Controlled by ResponseControls::lean (default true); set
// BP_READER_VERBOSE=1 to disable session-wide.
//
// Rules applied (in order):
//  1. Nulls and empty strings are removed from every object, recursively.
//  2. Empty arrays are removed for known high-cardinality, low-signal keys.
//  3. When a graph-body has a top-level "connections[]" key, the per-pin
//     "linked_to[]" arrays are dropped from each node's "pins[]" — they are
//     redundant (connections[] already encodes the same link data) and keeping
//     both doubles the link payload.  Single-node bodies (get_node / find_node
//     rows) have no "connections[]" so their "linked_to[]" is preserved.
// Arrays that are guaranteed noise when empty — not declared `required` in any
// tool output_schema, and zero-length only bloats the payload.
// NOTE: we only remove empty ARRAYS for these specific keys.  We do NOT remove
// null or empty-string values because many schemas declare those keys as
// `required` (e.g. `comment`, `default_value`) — removing a required key breaks
// schema validation even if the value is null/empty.
// The real token savings come from these empty arrays (every pin emits
// `linked_to:[]`; every graph emits `connections:[]`) rather than from
// individual null fields.
static const std::vector<std::string> kEmptyArrayBloatKeys{
	"linked_to", "connections", "delegate_params",
};
void PruneEmpty(nlohmann::json& v, int depth = 0) {
	if (depth > 32) { return; }  // guard against pathological nesting
	if (v.is_object()) {
		std::vector<std::string> toErase;
		for (auto& [k, child] : v.items()) {
			if (child.is_array() && child.empty()) {
				for (const auto& bk : kEmptyArrayBloatKeys) {
					if (k == bk) { toErase.push_back(k); break; }
				}
			} else {
				PruneEmpty(child, depth + 1);
			}
		}
		for (const auto& k : toErase) { v.erase(k); }
	} else if (v.is_array()) {
		for (auto& elem : v) { PruneEmpty(elem, depth + 1); }
	}
}
// Drop per-pin linked_to[] when the enclosing graph body already has connections[].
// Called after PruneEmpty so we don't re-add empties; only runs on object bodies.
void DeduplicateConnections(nlohmann::json& body) {
	if (!body.is_object()) { return; }
	if (!body.contains("connections")) { return; }
	auto nit = body.find("nodes");
	if (nit == body.end() || !nit->is_array()) { return; }
	for (auto& node : *nit) {
		if (!node.is_object()) { continue; }
		auto pit = node.find("pins");
		if (pit == node.end() || !pit->is_array()) { continue; }
		for (auto& pin : *pit) {
			if (pin.is_object()) { pin.erase("linked_to"); }
		}
	}
}

// Mutate `body` to apply sort + offset/limit (when body is an array) and
// field projection. Convenience helper called from every read-tool handler.
struct ResponseControls {
	int offset = 0;
	int limit  = -1;  // -1 => no cap
	bool lean  = true; // strip null/empty fields + dedup connections/linked_to
	std::vector<std::string> fields;
	std::string sort = "natural";  // "natural" | "name" | "path"
};
ResponseControls ParseResponseControls(const nlohmann::json& args) {
	ResponseControls ctl;
	ctl.offset = OptInt(args, "offset", 0);
	ctl.limit  = OptInt(args, "limit", -1);
	ctl.fields = ParseFieldsArg(args);
	ctl.sort   = OptString(args, "sort", "natural");
	// Lean mode: strip null/empty fields and dedup connection data.  Default on;
	// set BP_READER_VERBOSE=1 (or pass verbose:true) to opt out session-wide.
	{
		const char* envVerbose = std::getenv("BP_READER_VERBOSE");
		if (envVerbose && *envVerbose && std::string_view(envVerbose) != "0") {
			ctl.lean = false;
		}
		if (args.is_object() && args.value("verbose", false)) {
			ctl.lean = false;
		}
	}
	// Phase 5: opaque cursors take precedence over `offset`. A client
	// that walks via cursor doesn't need to know about offsets at all —
	// they just pass back the next_cursor we returned previously.
	if (args.is_object()) {
		if (auto cIt = args.find("cursor"); cIt != args.end() && !cIt->is_null()) {
			if (!cIt->is_string()) {
				throw std::invalid_argument(R"(argument "cursor" must be a string)");
			}
			auto decoded = DecodeCursor(cIt->get<std::string>());
			if (!decoded.has_value()) {
				throw std::invalid_argument(
					R"(argument "cursor" is malformed — pass a cursor returned )"
					R"(from a previous list_* call, or omit to start from offset 0)");
			}
			ctl.offset = static_cast<int>(*decoded);
		}
	}
	// C4: absolute clamp — explicit limit can't exceed 1000, regardless of what
	// the caller requested. Prevents a runaway `limit=1000000` from returning a
	// multi-MB payload and crashing the MCP client.
	if (ctl.limit > 1000) { ctl.limit = 1000; }
	if (ctl.offset < 0)
	{
		throw std::invalid_argument(R"(argument "offset" must be >= 0)");
	}
	if (ctl.limit < -1)
	{
		throw std::invalid_argument(R"(argument "limit" must be >= 0)");
	}
	if (ctl.sort != "natural" && ctl.sort != "name" && ctl.sort != "path") {
		throw std::invalid_argument(
			R"(argument "sort" must be one of: natural, name, path)");
	}
	// Per Phase B: BP_READER_SORT_DEFAULT env var can force "natural"
	// behavior session-wide even when a client sends sort=name. Kill-
	// switch for clients that misbehave. Default empty = honor the arg.
	if (const char* envSort = std::getenv("BP_READER_SORT_DEFAULT")) {
		if (envSort && *envSort) {
			ctl.sort = envSort;
		}
	}
	return ctl;
}
// Sort a JSON array in place by the `name` or `asset_path` field
// ("natural" / non-arrays are a no-op). Factored so both
// ApplyResponseControls and BuildPaginatedBody page against the same view.
void SortJsonArray(nlohmann::json& body, const std::string& sort) {
	if (!body.is_array() || (sort != "name" && sort != "path")) {
		return;
	}
	const std::string key = (sort == "name") ? "name" : "asset_path";
	std::stable_sort(body.begin(), body.end(),
		[&key](const nlohmann::json& a, const nlohmann::json& b) {
			if (a.is_string() && b.is_string()) {
				return a.get<std::string>() < b.get<std::string>();
			}
			const bool aHas = a.is_object() && a.contains(key) && a[key].is_string();
			const bool bHas = b.is_object() && b.contains(key) && b[key].is_string();
			if (!aHas && !bHas) return false;
			if (!aHas) return false;  // a goes to end
			if (!bHas) return true;
			return a[key].get<std::string>() < b[key].get<std::string>();
		});
}

void ApplyResponseControls(nlohmann::json& body, const ResponseControls& ctl) {
	// Sort first — page through the sorted view, not the raw view.
	SortJsonArray(body, ctl.sort);
	// UX-P0a: detect `fields` typos against the pre-projection body so a
	// misspelled entry surfaces as a warning instead of silently projecting
	// nothing. Must run before the slice/projection mutate `body`.
	std::vector<std::string> fieldWarnings = FieldsProjectionWarnings(body, ctl.fields);
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
	// C1/C2: lean-mode post-processing — prune noise + deduplicate links.
	// Runs after projection so the user's fields= selection is already done.
	if (ctl.lean) {
		PruneEmpty(body);
		DeduplicateConnections(body);
	}
	// Attach the typo warnings. Only possible on an object body — a bare-array
	// response has nowhere to hang a sibling key; those flow through
	// BuildPaginatedBody (an object) when pagination warnings matter.
	if (!fieldWarnings.empty() && body.is_object()) {
		body["_warnings"] = fieldWarnings;
	}
}

// UX-P2a: slice a named array field of an OBJECT body per offset/limit, for
// tools whose payload wraps a large array (get_class_info.properties/functions,
// read_actor_instance.overrides). Sort + field projection are still handled by
// ApplyResponseControls/ApplyProjection on the whole body; this only trims the
// named array. When it actually trims, it records `<field>_total` +
// `<field>_has_more` siblings so the caller knows it paged. No-op when the
// field is absent / not an array / neither offset nor limit was set.
void PaginateField(nlohmann::json& body, const std::string& field,
				   const ResponseControls& ctl) {
	if (!body.is_object() || (ctl.offset <= 0 && ctl.limit < 0)) {
		return;
	}
	auto it = body.find(field);
	if (it == body.end() || !it->is_array()) {
		return;
	}
	nlohmann::json& arr = *it;
	const std::size_t total = arr.size();
	const std::size_t off = std::min<std::size_t>(ctl.offset, total);
	const std::size_t end = (ctl.limit < 0)
								? total
								: std::min<std::size_t>(off + ctl.limit, total);
	nlohmann::json sliced = nlohmann::json::array();
	for (std::size_t i = off; i < end; ++i) {
		sliced.push_back(std::move(arr[i]));
	}
	const bool trimmed = (off > 0) || (end < total);
	arr = std::move(sliced);
	if (trimmed) {
		body[field + "_total"]    = static_cast<int>(total);
		body[field + "_has_more"] = end < total;
	}
}

// Build a self-describing paginated response from a full result set. A broad
// query (e.g. find_asset "Elevator") can match thousands of rows; returning
// them all produces a response the MCP client rejects as "Output too large"
// before the caller learns to paginate. Cap at a default page size
// (overridable via `limit`) and report total / has_more / next_cursor so the
// caller can walk the rest. `fields` still projects each row. The default
// applies only when no explicit `limit` was passed. See client feedback #3.
nlohmann::json BuildPaginatedBody(nlohmann::json rows,
								  const ResponseControls& ctl,
								  int defaultLimit) {
	SortJsonArray(rows, ctl.sort);
	// UX-P0a: compute fields-typo warnings against the full row set before any
	// element is moved out into the page.
	std::vector<std::string> fieldWarnings = FieldsProjectionWarnings(rows, ctl.fields);
	const std::size_t total = rows.is_array() ? rows.size() : 0;
	const std::size_t off =
		std::min<std::size_t>(static_cast<std::size_t>(std::max(ctl.offset, 0)), total);
	const std::size_t effLimit = (ctl.limit >= 0)
									  ? static_cast<std::size_t>(ctl.limit)
									  : static_cast<std::size_t>(std::max(defaultLimit, 0));
	const std::size_t end = std::min<std::size_t>(off + effLimit, total);
	nlohmann::json results = nlohmann::json::array();
	for (std::size_t i = off; i < end; ++i) {
		results.push_back(std::move(rows[i]));
	}
	ApplyProjection(results, ctl.fields);
	const bool hasMore = end < total;
	nlohmann::json out = {
		{"total",       static_cast<int>(total)},
		{"count",       static_cast<int>(results.size())},
		{"offset",      static_cast<int>(off)},
		{"has_more",    hasMore},
		{"next_cursor", hasMore
							? nlohmann::json(EncodeCursor(static_cast<std::int64_t>(end)))
							: nlohmann::json(nullptr)},
		{"results",     std::move(results)},
	};
	if (!fieldWarnings.empty()) {
		out["_warnings"] = std::move(fieldWarnings);
	}
	return out;
}

// C4 helper: wrap a plain array response into the paginated envelope with a
// default page size of 200.  List/find tools that return a bare array call this
// instead of returning the array directly; lean-mode pruning is applied to
// each element before paging.
//
// When body is an OBJECT (not an array), falls back to ApplyResponseControls
// so the conversion of `ApplyResponseControls(body,ctl); return body;` to
// `return ListResponse(std::move(body),ctl);` is safe for all callers regardless
// of whether they return an array or an object.
//
// Usage:
//   return ListResponse(std::move(myArray), ParseResponseControls(args));
nlohmann::json ListResponse(nlohmann::json rows, const ResponseControls& ctl,
							int defaultLimit = 200) {
	if (!rows.is_array()) {
		// Object-body tool: apply normal response controls (sort/slice/project/prune).
		ApplyResponseControls(rows, ctl);
		return rows;
	}
	if (ctl.lean) {
		for (auto& n : rows) { PruneEmpty(n); }
	}
	return BuildPaginatedBody(std::move(rows), ctl, defaultLimit);
}

// On AssetNotFound, run a fuzzy basename lookup against the asset
// registry and return a "did you mean: …" suffix. Empty string when
// no candidates, when the backend can't do FindAsset, or anything
// throws — we never let the hint computation mask the original error.
//
// The matched scope is /Game; we extract the last `/`-separated
// segment of the requested path and search by substring against
// asset names + full package paths. Cap at 3 candidates to keep the
// error message digestible.
//
// Client feedback #6: the old version could suggest the *exact path the
// caller asked for* — which happens when the asset genuinely exists but
// isn't the type this tool reads (e.g. read_blueprint on a level / actor
// instance / data asset). Repeating the input as a suggestion is
// misleading and wastes a turn. So: when an exact-path match exists we
// report its real class ("exists but is a <Class>, not a Blueprint")
// instead of suggesting it, and we never list the input path among the
// fuzzy suggestions.
std::string ComputeDidYouMeanHint(backends::IBlueprintReader& reader,
								  std::string_view assetPath) {
	if (assetPath.empty()) {
		return {};
	}
	auto slash = assetPath.find_last_of('/');
	std::string basename = (slash == std::string_view::npos)
								  ? std::string(assetPath)
								  : std::string(assetPath.substr(slash + 1));
	if (basename.empty()) {
		return {};
	}
	// Normalize an object path (/Game/X.X) to its package form (/Game/X)
	// so the exact-match comparison against registry rows lines up.
	auto toPackage = [](std::string_view p) -> std::string {
		auto s = p.find_last_of('/');
		auto d = p.find_last_of('.');
		if (d != std::string_view::npos &&
			(s == std::string_view::npos || d > s)) {
			p = p.substr(0, d);
		}
		return std::string(p);
	};
	auto iequals = [](const std::string& a, const std::string& b) {
		if (a.size() != b.size()) { return false; }
		for (std::size_t i = 0; i < a.size(); ++i) {
			if (std::tolower(static_cast<unsigned char>(a[i])) !=
				std::tolower(static_cast<unsigned char>(b[i]))) {
				return false;
			}
		}
		return true;
	};
	const std::string wantPkg = toPackage(assetPath);
	try {
		auto matches = reader.FindAsset(basename, "/Game");
		if (matches.entries.empty()) {
			return {};
		}
		std::string exactClass;   // class of an exact-path match, if any
		std::string list;         // fuzzy suggestions (excluding the input)
		constexpr std::size_t kMax = 3;
		std::size_t count = 0;
		for (const auto& e : matches.entries) {
			if (iequals(e.assetPath, wantPkg)) {
				// The asset exists at the requested path. If it's a different
				// type than a Blueprint, that's the useful fact to report; if
				// it somehow IS a Blueprint, say nothing (a "not a Blueprint"
				// note would be nonsensical). Either way, never suggest the
				// input path back to the caller.
				if (!iequals(e.className, "Blueprint")) {
					exactClass = e.className;
				}
				continue;
			}
			if (count >= kMax) {
				continue;
			}
			if (!list.empty()) {
				list += ", ";
			}
			list += e.assetPath;
			++count;
		}
		if (!exactClass.empty()) {
			// The asset is really there, just not a Blueprint. Say so, and
			// fold in any other near-matches.
			std::string hint = fmt::format(
				" — '{}' exists but is a {}, not a Blueprint",
				wantPkg, exactClass);
			if (!list.empty()) {
				hint += fmt::format("; did you mean: {}?", list);
			}
			return hint;
		}
		if (list.empty()) {
			return {};   // nothing useful to suggest — don't echo the input
		}
		return fmt::format(" — did you mean: {}?", list);
	} catch (...) {
		return {};
	}
}

// Wrap a reader call that resolves an asset path; on AssetNotFound,
// run the did-you-mean lookup and rethrow with the hint appended.
// Pass-through for any other exception type.
template <typename Fn>
auto WithAssetNotFoundHint(backends::IBlueprintReader& reader,
						   std::string_view assetPath,
						   Fn&& fn) -> decltype(fn()) {
	try {
		return fn();
	} catch (const backends::AssetNotFound& e) {
		const std::string hint = ComputeDidYouMeanHint(reader, assetPath);
		if (hint.empty()) {
			throw;
		}
		throw backends::AssetNotFound(std::string(e.what()) + hint);
	}
}

}  // anonymous

// Require/opt wrappers that call NormalizeAssetPath before returning.
// Use these for every "asset_path" argument so both the package form
// (/Game/AI/BP_Foo) and the object form (/Game/AI/BP_Foo.BP_Foo) are accepted.
// Declared outside the anonymous namespace so they are nameable from test TUs.
inline std::string RequireAssetPath(const nlohmann::json& obj,
									std::string_view key = "asset_path") {
	auto it = obj.find(key);
	if (it == obj.end() || !it->is_string()) {
		throw std::invalid_argument(fmt::format(R"(missing or non-string argument "{}")", key));
	}
	return NormalizeAssetPath(it->get<std::string>());
}
inline std::string OptAssetPath(const nlohmann::json& obj, std::string_view key,
								std::string fallback = "") {
	auto it = obj.find(key);
	if (it == obj.end() || it->is_null()) { return NormalizeAssetPath(std::move(fallback)); }
	if (!it->is_string()) {
		throw std::invalid_argument(fmt::format(R"(argument "{}" must be a string)", key));
	}
	return NormalizeAssetPath(it->get<std::string>());
}

}  // namespace blueprint_tools_detail

// Chunk registration helpers (split across BlueprintTools*.cpp to keep each
// TU small enough for the compiler front-end heap; see the .cpp header note).
void RegisterTools_00(ToolRegistry& registry, backends::IBlueprintReader& reader);
void RegisterTools_00b(ToolRegistry& registry, backends::IBlueprintReader& reader);
void RegisterTools_01(ToolRegistry& registry, backends::IBlueprintReader& reader);
void RegisterTools_01b(ToolRegistry& registry, backends::IBlueprintReader& reader);
void RegisterTools_02(ToolRegistry& registry, backends::IBlueprintReader& reader);
void RegisterTools_02b(ToolRegistry& registry, backends::IBlueprintReader& reader);
void RegisterTools_03(ToolRegistry& registry, backends::IBlueprintReader& reader);
void RegisterTools_03b(ToolRegistry& registry, backends::IBlueprintReader& reader);
void RegisterTools_04(ToolRegistry& registry, backends::IBlueprintReader& reader);
void RegisterTools_04b(ToolRegistry& registry, backends::IBlueprintReader& reader);
void RegisterTools_05(ToolRegistry& registry, backends::IBlueprintReader& reader);
void RegisterTools_06(ToolRegistry& registry, backends::IBlueprintReader& reader);
void RegisterTools_07(ToolRegistry& registry, backends::IBlueprintReader& reader);
void RegisterTools_08(ToolRegistry& registry, backends::IBlueprintReader& reader);
void RegisterTools_08b(ToolRegistry& registry, backends::IBlueprintReader& reader);
void RegisterTools_09(ToolRegistry& registry, backends::IBlueprintReader& reader);
}  // namespace bpr::tools
