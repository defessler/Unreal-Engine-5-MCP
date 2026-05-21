// Tool categories — symbolic groupings of tool names used by
// ToolRegistry::ApplyFilter. Keeps the env-var filter UX concise
// ("BP_READER_TOOLS=core,cpp") instead of forcing users to list every
// tool by name.
//
// Adding a new tool: include it in at least one category here so the
// preset users don't lose it silently. Most authoring tools belong in
// `core` (the BP CRUD surface) or `write` (extended write surface).
//
// Adding a new category: declare here, expand in ToolCategories.cpp.
// Keep the category names lowercase, hyphenated, descriptive.

#pragma once

#include <string>
#include <vector>

namespace bpr::tools {

// Returns the list of tool names belonging to a category. Unknown
// category names return an empty vector — the caller (ApplyFilter)
// then treats the token as a literal tool name instead.
std::vector<std::string> ExpandCategory(const std::string& name);

// Whether the given token is a recognized category name.
bool IsKnownCategory(const std::string& name);

// One-line description for a category, used by the lazy-discovery
// `list_toolsets` meta-tool. Returns empty for unknown categories.
std::string CategoryDescription(const std::string& name);

// Names of every category in deterministic iteration order. Used by
// list_toolsets to enumerate. The list is the union of per-domain
// categories and workflow presets.
std::vector<std::string> AllCategoryNames();

}    // namespace bpr::tools
