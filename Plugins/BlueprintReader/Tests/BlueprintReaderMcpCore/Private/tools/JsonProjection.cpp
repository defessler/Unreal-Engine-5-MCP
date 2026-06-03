#include "tools/JsonProjection.h"

#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

#include <fmt/core.h>

namespace bpr::tools {

namespace json_projection_detail {

// Split a dotted path into segments. A segment is either an object key,
// or the literal "[]" meaning "iterate this array".
//   "variables[].name" -> ["variables", "[]", "name"]
//   "parent_class"     -> ["parent_class"]
std::vector<std::string> SplitPath(const std::string& path) {
	std::vector<std::string> out;
	std::string cur;
	auto flushCur = [&]() {
		if (!cur.empty()) { out.push_back(std::move(cur)); cur.clear(); }
	};
	for (std::size_t i = 0; i < path.size(); ++i) {
		char c = path[i];
		if (c == '.') {
			flushCur();
		} else if (c == '[') {
			// expect "[]" — consume the closing bracket
			flushCur();
			if (i + 1 < path.size() && path[i + 1] == ']') {
				out.emplace_back("[]");
				++i;
			} else {
				throw std::invalid_argument(fmt::format(
					"fields path '{}' has '[' without matching ']'", path));
			}
		} else {
			cur.push_back(c);
		}
	}
	flushCur();
	return out;
}

// Map a small set of well-known field shorthands to their canonical
// wire key. Callers routinely guess the bare adjective (`editable`,
// `replicated`) when the wire key carries an `is_` / verbose form
// (`is_editable`, `expose_on_spawn`). Without this the projection would
// silently drop the unmatched name and the caller draws a wrong
// conclusion ("the BP has no editable flag"). Resolved at *match time*
// against the actual doc keys, so we only ever keep a key that exists —
// never invent one. See client feedback #2.
const std::map<std::string, std::string>& FieldAliases() {
	static const std::map<std::string, std::string> kAliases = {
		{"editable", "is_editable"},
		{"instance_editable", "is_editable"},
		{"replicated", "is_replicated"},
		{"exposed", "expose_on_spawn"},
		{"expose", "expose_on_spawn"},
		{"rep_notify", "rep_notify_func"},
		{"repnotify", "rep_notify_func"},
		{"replication_condition", "rep_condition"},
	};
	return kAliases;
}

// True if a requested field name should keep the actual doc key. Matches
// exactly, via the static alias table, or via the generic `is_<field>`
// boolean-flag convention — all doc-aware (the `actualKey` is a real key
// present in the body), so aliasing can never fabricate a field.
bool RequestMatchesKey(const std::string& requested, const std::string& actualKey) {
	if (requested == actualKey) { return true; }
	auto it = FieldAliases().find(requested);
	if (it != FieldAliases().end() && it->second == actualKey) { return true; }
	if ("is_" + requested == actualKey) { return true; }
	return false;
}

// One internal node: at this level, keep these keys, and for some keys
// recurse into them (object) or into their elements (array).
struct Node {
	std::set<std::string> keep;            // top-level keys to keep
	std::map<std::string, Node> children;  // recursion: key -> sub-filter
	bool descendArray = false;             // applies to *this* node — when
										   // true, treat the value as an
										   // array and recurse into each
										   // element with this Node's keep
										   // / children
};

// Build a Node tree from a flat list of paths.
void Insert(Node& root, const std::vector<std::string>& segments,
			std::size_t i = 0) {
	if (i >= segments.size())
	{
		return;
	}
	if (segments[i] == "[]") {
		// Mark "this is an array — descend per element with the rest of
		// the path applied at the same conceptual level."
		// We model this by setting descendArray on root and continuing
		// to populate root with the rest as if it were a non-array.
		root.descendArray = true;
		Insert(root, segments, i + 1);
		return;
	}
	const auto& key = segments[i];
	root.keep.insert(key);
	if (i + 1 < segments.size()) {
		Insert(root.children[key], segments, i + 1);
	}
}

void Apply(nlohmann::json& body, const Node& filter) {
	if (filter.descendArray && body.is_array()) {
		for (auto& el : body)
		{
			Apply(el, filter);
		}
		return;
	}
	if (!body.is_object())
	{
		return;
	}

	// Drop unwanted keys. A key is wanted if any requested field matches
	// it exactly or via an alias (`editable` -> `is_editable`, etc.).
	for (auto it = body.begin(); it != body.end();) {
		bool wanted = filter.keep.count(it.key()) != 0;
		if (!wanted) {
			for (const auto& req : filter.keep) {
				if (RequestMatchesKey(req, it.key())) { wanted = true; break; }
			}
		}
		if (!wanted) {
			it = body.erase(it);
		} else {
			++it;
		}
	}

	// Recurse into kept keys that have child filters.
	for (auto& [key, subFilter] : filter.children) {
		auto it = body.find(key);
		if (it == body.end())
		{
			continue;
		}
		Apply(*it, subFilter);
	}
}

}    // namespace json_projection_detail
using namespace json_projection_detail;

void ApplyProjection(nlohmann::json& body,
					 const std::vector<std::string>& paths) {
	if (paths.empty())
	{
		return;
	}
	Node root;
	for (const auto& path : paths) {
		auto segments = SplitPath(path);
		Insert(root, segments);
	}
	// Top-level array convenience: tools like list_blueprints return an
	// array, but the caller writes `fields: ["asset_path"]` not
	// `["[].asset_path"]`. Auto-descend so paths apply per-element.
	// If the caller wrote an explicit `[]` at the front, descendArray is
	// already set and Apply handles it the same way — this branch is only
	// for the bare-keys case.
	if (body.is_array() && !root.descendArray) {
		for (auto& el : body)
		{
			Apply(el, root);
		}
		return;
	}
	Apply(body, root);
}

namespace {
// Collect the distinct keys present at the level `fields` applies to: an
// object body's own keys, or (for an array body) the union of keys across
// object elements. Order-preserving.
std::vector<std::string> LevelKeys(const nlohmann::json& body) {
	std::vector<std::string> keys;
	std::set<std::string> seen;
	auto addObj = [&](const nlohmann::json& obj) {
		if (!obj.is_object()) { return; }
		for (auto it = obj.begin(); it != obj.end(); ++it) {
			if (seen.insert(it.key()).second) { keys.push_back(it.key()); }
		}
	};
	if (body.is_object()) {
		addObj(body);
	} else if (body.is_array()) {
		for (const auto& el : body) { addObj(el); }
	}
	return keys;
}
}    // namespace

std::vector<std::string> FieldsProjectionWarnings(
	const nlohmann::json& body, const std::vector<std::string>& paths) {
	std::vector<std::string> warnings;
	if (paths.empty()) {
		return warnings;
	}
	const auto keys = LevelKeys(body);
	if (keys.empty()) {
		// No keys to match against (e.g. an empty / not-found payload). We
		// can't tell a typo from a legitimately-absent field — stay silent.
		return warnings;
	}
	// Distinct top-level requested names (first non-"[]" segment), in order.
	std::vector<std::string> requested;
	std::set<std::string> seenReq;
	for (const auto& path : paths) {
		std::vector<std::string> segments;
		try {
			segments = SplitPath(path);
		} catch (...) {
			continue;  // malformed path — ApplyProjection reports it consistently
		}
		for (const auto& seg : segments) {
			if (seg != "[]") {
				if (seenReq.insert(seg).second) { requested.push_back(seg); }
				break;
			}
		}
	}
	// Available-keys hint (capped to keep the message digestible).
	std::string avail;
	constexpr std::size_t kMaxKeys = 12;
	for (std::size_t i = 0; i < keys.size() && i < kMaxKeys; ++i) {
		if (!avail.empty()) { avail += ", "; }
		avail += keys[i];
	}
	if (keys.size() > kMaxKeys) { avail += ", ..."; }
	for (const auto& req : requested) {
		bool matched = false;
		for (const auto& k : keys) {
			if (RequestMatchesKey(req, k)) { matched = true; break; }
		}
		if (!matched) {
			warnings.push_back(fmt::format(
				"fields: '{}' matched no response key (available: {})", req, avail));
		}
	}
	return warnings;
}

std::vector<std::string> ParseFieldsArg(const nlohmann::json& args) {
	auto it = args.find("fields");
	if (it == args.end() || it->is_null()) return {};
	if (!it->is_array()) {
		throw std::invalid_argument(R"(argument "fields" must be an array of strings)");
	}
	std::vector<std::string> out;
	out.reserve(it->size());
	for (const auto& el : *it) {
		if (!el.is_string()) {
			throw std::invalid_argument(R"(every entry in "fields" must be a string)");
		}
		out.push_back(el.get<std::string>());
	}
	return out;
}

}    // namespace bpr::tools
