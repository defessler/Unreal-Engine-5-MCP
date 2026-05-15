// Tests for ToolRegistry::ApplyFilter — the env-var-driven tool subset
// that lets users fit under MCP clients' tool-count caps (GitHub
// Copilot caps at 128 total).

#include <doctest/doctest.h>

#include "tools/ToolCategories.h"
#include "tools/ToolRegistry.h"

#include <nlohmann/json.hpp>

using namespace bpr::tools;

namespace {
// Minimal stub handler — these tests don't dispatch, only check
// registration / filtering.
ToolFn StubFn() {
    return [](const nlohmann::json&) { return nlohmann::json::object(); };
}

void AddNamed(ToolRegistry& r, const std::string& name) {
    r.Add({name, "stub", nlohmann::json::object()}, StubFn());
}

ToolRegistry MakeWith(std::initializer_list<const char*> names) {
    ToolRegistry r;
    for (const char* n : names) AddNamed(r, n);
    return r;
}

size_t CountSpec(const ToolRegistry& r) {
    return r.ListSpec().size();
}
}  // namespace

TEST_CASE("ApplyFilter: empty allow + empty deny is a no-op") {
    auto r = MakeWith({"read_blueprint", "add_node", "wire_pins"});
    REQUIRE(CountSpec(r) == 3);
    r.ApplyFilter({}, {});
    CHECK(CountSpec(r) == 3);
}

TEST_CASE("ApplyFilter: allow-list with literal names keeps only those") {
    auto r = MakeWith({"read_blueprint", "add_node", "wire_pins"});
    r.ApplyFilter({"read_blueprint", "wire_pins"}, {});
    CHECK(CountSpec(r) == 2);
    CHECK(r.Find("read_blueprint") != nullptr);
    CHECK(r.Find("wire_pins") != nullptr);
    CHECK(r.Find("add_node") == nullptr);  // dispatch also pruned
}

TEST_CASE("ApplyFilter: deny-list with no allow starts from full set") {
    auto r = MakeWith({"read_blueprint", "add_node", "wire_pins"});
    r.ApplyFilter({}, {"add_node"});
    CHECK(CountSpec(r) == 2);
    CHECK(r.Find("add_node") == nullptr);
    CHECK(r.Find("read_blueprint") != nullptr);
}

TEST_CASE("ApplyFilter: allow then deny composes") {
    auto r = MakeWith({"read_blueprint", "add_node", "wire_pins", "save_all"});
    // allow = read + write tools (categories), deny = save_all specifically
    r.ApplyFilter({"read", "write"}, {"save_all"});
    CHECK(r.Find("read_blueprint") != nullptr);
    CHECK(r.Find("add_node") != nullptr);
    CHECK(r.Find("save_all") == nullptr);
}

TEST_CASE("ApplyFilter: 'all' is a valid token that means every tool") {
    auto r = MakeWith({"read_blueprint", "add_node", "shutdown_daemon"});
    r.ApplyFilter({"all"}, {});
    CHECK(CountSpec(r) == 3);
}

TEST_CASE("ApplyFilter: typo'd literal tool name silently keeps nothing") {
    auto r = MakeWith({"read_blueprint", "add_node"});
    r.ApplyFilter({"reed_blueprint"}, {});  // typo
    CHECK(CountSpec(r) == 0);
    // This is the safe failure mode: silently dropping is better than
    // silently keeping. The post-filter log line in main.cpp surfaces
    // the count so a typo is visible to anyone reading stderr.
}

TEST_CASE("ApplyFilter: category expands to its tools") {
    auto r = MakeWith({
        "read_blueprint",          // in `read`
        "list_materials",          // in `read` AND `materials`
        "set_material_parameter",  // in `materials` only
        "add_node",                // in `write` only
        "shutdown_daemon",         // in `discover`
    });
    r.ApplyFilter({"read"}, {});
    CHECK(r.Find("read_blueprint") != nullptr);
    CHECK(r.Find("list_materials") != nullptr);
    CHECK(r.Find("set_material_parameter") == nullptr);
    CHECK(r.Find("add_node") == nullptr);
    CHECK(r.Find("shutdown_daemon") == nullptr);
}

TEST_CASE("ApplyFilter: same category name as a tool name treats it as a category") {
    // None of our actual tool names collide with category names, but
    // pin the precedence: when a token IS a known category, expansion
    // wins. (If someone later names a tool "core", they get told off in
    // code review; the filter UX won't quietly half-do the right thing.)
    auto r = MakeWith({"add_node", "save_all", "list_blueprints"});
    r.ApplyFilter({"core"}, {});  // `core` is a category
    // `add_node`, `save_all`, `list_blueprints` are all in `core`
    CHECK(CountSpec(r) == 3);
}

TEST_CASE("Workflow presets: each is a known category with a non-empty tool list") {
    // Pins the workflow-shaped categories so a rename or removal of one
    // is caught by the test suite, not by a downstream config breaking.
    for (const char* name : {
            "bp-authoring",
            "material-tuning",
            "cpp-roundtrip",
            "editor-control",
            "widget-design",
            "gameplay-tuning",
        }) {
        CAPTURE(name);
        REQUIRE(IsKnownCategory(name));
        auto tools = ExpandCategory(name);
        CHECK(tools.size() >= 6);  // each workflow has at least a handful
        CHECK(tools.size() <= 40); // and none is so big it defeats the point
    }
}

TEST_CASE("Workflow preset shapes contain the expected anchor tools") {
    // Spot-check that the canonical tool for each workflow is in its set.
    // Catches typos in ToolCategories.cpp's lists.
    auto contains = [](const std::vector<std::string>& v, const char* needle) {
        return std::find(v.begin(), v.end(), std::string(needle)) != v.end();
    };
    CHECK(contains(ExpandCategory("material-tuning"), "set_material_parameter"));
    CHECK(contains(ExpandCategory("material-tuning"), "compile_material"));
    CHECK(contains(ExpandCategory("cpp-roundtrip"), "transpile_function"));
    CHECK(contains(ExpandCategory("cpp-roundtrip"), "parse_cpp_function"));
    CHECK(contains(ExpandCategory("editor-control"), "pie_start"));
    CHECK(contains(ExpandCategory("editor-control"), "console_command"));
    CHECK(contains(ExpandCategory("widget-design"), "add_widget"));
    CHECK(contains(ExpandCategory("widget-design"), "compile_widget_blueprint"));
    CHECK(contains(ExpandCategory("gameplay-tuning"), "set_variable_default"));
    CHECK(contains(ExpandCategory("gameplay-tuning"), "pie_start"));
}

TEST_CASE("ToolCategories: known + unknown lookups") {
    CHECK(IsKnownCategory("core"));
    CHECK(IsKnownCategory("cpp"));
    CHECK(IsKnownCategory("editor"));
    CHECK(!IsKnownCategory("nonexistent"));
    CHECK(!IsKnownCategory(""));
    // Spot-check a category has expected entries.
    auto core = ExpandCategory("core");
    CHECK(!core.empty());
    auto hasReadBp = std::find(core.begin(), core.end(),
                               std::string("read_blueprint")) != core.end();
    CHECK(hasReadBp);
}
