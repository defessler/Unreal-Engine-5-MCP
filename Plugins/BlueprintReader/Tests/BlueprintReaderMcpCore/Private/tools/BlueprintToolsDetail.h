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

// Mutate `body` to apply sort + offset/limit (when body is an array) and
// field projection. Convenience helper called from every read-tool handler.
struct ResponseControls {
	int offset = 0;
	int limit  = -1;  // -1 => no cap
	std::vector<std::string> fields;
	std::string sort = "natural";  // "natural" | "name" | "path"
};
ResponseControls ParseResponseControls(const nlohmann::json& args) {
	ResponseControls ctl;
	ctl.offset = OptInt(args, "offset", 0);
	ctl.limit  = OptInt(args, "limit", -1);
	ctl.fields = ParseFieldsArg(args);
	ctl.sort   = OptString(args, "sort", "natural");
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
void ApplyResponseControls(nlohmann::json& body, const ResponseControls& ctl) {
	// Sort first — page through the sorted view, not the raw view.
	if (body.is_array() && (ctl.sort == "name" || ctl.sort == "path")) {
		const std::string key = (ctl.sort == "name") ? "name" : "asset_path";
		std::stable_sort(body.begin(), body.end(),
			[&key](const nlohmann::json& a, const nlohmann::json& b) {
				// Defensive: entries without the key sort to the end so
				// a heterogeneous response doesn't crash with an
				// undefined comparison. Bare strings (no object shape)
				// fall back to direct string compare.
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
