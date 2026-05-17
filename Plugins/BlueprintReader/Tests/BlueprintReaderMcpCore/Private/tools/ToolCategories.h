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

}    // namespace bpr::tools
