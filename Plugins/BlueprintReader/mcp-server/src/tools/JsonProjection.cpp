#include "tools/JsonProjection.h"

#include <set>
#include <sstream>
#include <stdexcept>

#include <fmt/core.h>

namespace bpr::tools {

namespace {

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
    if (i >= segments.size()) return;
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
        for (auto& el : body) Apply(el, filter);
        return;
    }
    if (!body.is_object()) return;

    // Drop unwanted keys.
    for (auto it = body.begin(); it != body.end();) {
        if (filter.keep.count(it.key()) == 0) {
            it = body.erase(it);
        } else {
            ++it;
        }
    }

    // Recurse into kept keys that have child filters.
    for (auto& [key, subFilter] : filter.children) {
        auto it = body.find(key);
        if (it == body.end()) continue;
        Apply(*it, subFilter);
    }
}

} // namespace

void ApplyProjection(nlohmann::json& body,
                     const std::vector<std::string>& paths) {
    if (paths.empty()) return;
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
        for (auto& el : body) Apply(el, root);
        return;
    }
    Apply(body, root);
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

} // namespace bpr::tools
