// Tests for ToolRegistry::ApplyFilter — the env-var-driven tool subset
// that lets users fit under MCP clients' tool-count caps (GitHub
// Copilot caps at 128 total).

#include <doctest/doctest.h>

#include "tools/ToolCategories.h"
#include "tools/ToolRegistry.h"

#include <nlohmann/json.hpp>

#include <regex>

using namespace bpr::tools;

namespace test_tool_filter_detail {
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
	for (const char* n : names)
	{
		AddNamed(r, n);
	}
	return r;
}

size_t CountSpec(const ToolRegistry& r) {
	return r.ListSpec().size();
}
}    // namespace test_tool_filter_detail
using namespace test_tool_filter_detail;

TEST_CASE("ApplyFilter: empty allow + empty deny is a no-op") {
	auto r = MakeWith({"read_blueprint", "add_node", "wire_pins"});
	REQUIRE(CountSpec(r) == 3);
	r.ApplyFilter({}, {});
	CHECK(CountSpec(r) == 3);
}

TEST_CASE("ApplyFilter: regex-delimited tokens match against tool names") {
	auto r = MakeWith({"read_blueprint", "list_blueprints", "add_node",
					   "delete_node", "wire_pins"});
	r.ApplyFilter({"/^.*_node$/"}, {});  // tools ending in "_node"
	CHECK(CountSpec(r) == 2);
	CHECK(r.Find("add_node") != nullptr);
	CHECK(r.Find("delete_node") != nullptr);
	CHECK(r.Find("read_blueprint") == nullptr);
}

TEST_CASE("ApplyFilter: regex deny subtracts after the allow step") {
	auto r = MakeWith({"read_blueprint", "list_blueprints", "add_node",
					   "delete_node", "wire_pins"});
	// Allow everything, deny anything containing "node".
	r.ApplyFilter({"all"}, {"/node/"});
	CHECK(r.Find("read_blueprint") != nullptr);
	CHECK(r.Find("list_blueprints") != nullptr);
	CHECK(r.Find("wire_pins") != nullptr);
	CHECK(r.Find("add_node") == nullptr);
	CHECK(r.Find("delete_node") == nullptr);
}

TEST_CASE("ApplyFilter: regex matches anywhere in the name (substring semantics)") {
	auto r = MakeWith({"read_blueprint", "list_blueprints", "compile_function"});
	r.ApplyFilter({"/blueprint/"}, {});  // matches both BP names
	CHECK(CountSpec(r) == 2);
	CHECK(r.Find("compile_function") == nullptr);
}

TEST_CASE("ApplyFilter: malformed regex token throws at startup") {
	auto r = MakeWith({"a", "b"});
	CHECK_THROWS_AS(r.ApplyFilter({"/[/"}, {}), std::regex_error);
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

// ===== ListSpec exclusion ================================================
// Verifies the user-facing contract: tools removed from the active set
// don't appear in tools/list at all. tools/call also rejects them
// (covered by the existing Find() tests above) — these check the
// /advertisement/ surface specifically.

namespace test_tool_filter_detail {
static std::vector<std::string> SpecNames(const ToolRegistry& r) {
	std::vector<std::string> out;
	for (const auto& entry : r.ListSpec()) {
		out.push_back(entry.at("name").get<std::string>());
	}
	return out;
}
static bool SpecHas(const ToolRegistry& r, const std::string& name) {
	auto names = SpecNames(r);
	return std::find(names.begin(), names.end(), name) != names.end();
}
}    // namespace test_tool_filter_detail

TEST_CASE("ListSpec: denied tools are excluded by name") {
	auto r = MakeWith({"read_blueprint", "add_node", "wire_pins", "save_all"});
	r.ApplyFilter({}, {"add_node"});
	// Excluded tool is gone:
	CHECK(!SpecHas(r, "add_node"));
	// Others remain:
	CHECK(SpecHas(r, "read_blueprint"));
	CHECK(SpecHas(r, "wire_pins"));
	CHECK(SpecHas(r, "save_all"));
}

TEST_CASE("ListSpec: denied category drops every tool in it") {
	// Approximation of `BP_READER_TOOLS_EXCLUDE=cpp` — the user's
	// canonical "hide the transpile surface" recipe. With the cpp
	// category denied, none of its tools should appear in tools/list.
	auto r = MakeWith({
		// `cpp` category members:
		"decompile_function",
		"decompile_blueprint",
		"transpile_function",
		"transpile_blueprint",
		"parse_cpp_function",
		"compile_function",
		"write_generated_source",
		// Non-cpp tools that should survive:
		"read_blueprint",
		"add_node",
	});
	r.ApplyFilter({}, {"cpp"});
	for (const char* hidden : {
			"decompile_function",
			"decompile_blueprint",
			"transpile_function",
			"transpile_blueprint",
			"parse_cpp_function",
			"compile_function",
			"write_generated_source",
		}) {
		CAPTURE(hidden);
		CHECK(!SpecHas(r, hidden));
	}
	CHECK(SpecHas(r, "read_blueprint"));
	CHECK(SpecHas(r, "add_node"));
}

TEST_CASE("ListSpec: allow-list narrows tools/list to exactly the allowed set") {
	auto r = MakeWith({"read_blueprint", "add_node", "wire_pins", "save_all"});
	r.ApplyFilter({"read_blueprint", "save_all"}, {});
	auto names = SpecNames(r);
	CHECK(names.size() == 2);
	CHECK(std::find(names.begin(), names.end(), std::string("read_blueprint")) != names.end());
	CHECK(std::find(names.begin(), names.end(), std::string("save_all"))       != names.end());
	CHECK(std::find(names.begin(), names.end(), std::string("add_node"))       == names.end());
	CHECK(std::find(names.begin(), names.end(), std::string("wire_pins"))      == names.end());
}

TEST_CASE("ListSpec: allow-then-deny composes — denied tools never appear") {
	auto r = MakeWith({"read_blueprint", "add_node", "save_all"});
	// Allow core (which includes all three), then deny one specifically.
	r.ApplyFilter({"core"}, {"save_all"});
	CHECK(SpecHas(r, "read_blueprint"));
	CHECK(SpecHas(r, "add_node"));
	CHECK(!SpecHas(r, "save_all"));
}

TEST_CASE("ListSpec: filtered tools are dropped from both list AND dispatch") {
	// The contract: a denied tool is invisible (tools/list) *and*
	// uncallable (tools/call). Pins both paths in one assertion so a
	// future change that splits them is caught.
	auto r = MakeWith({"read_blueprint", "add_node"});
	r.ApplyFilter({}, {"add_node"});
	CHECK(!SpecHas(r, "add_node"));       // not advertised
	CHECK(r.Find("add_node") == nullptr); // not dispatchable
}

// ===== Progressive disclosure ============================================
// Pins the runtime widening flow exposed via enable_tool_category +
// the listChanged notification, used when BP_READER_PROGRESSIVE=1.

TEST_CASE("ActivateToken: no-op when no filter has been applied") {
	auto r = MakeWith({"read_blueprint", "add_node", "save_all"});
	// Registry is in default "show all" mode. Activating more is
	// meaningless — there's nothing inactive to activate.
	auto added = r.ActivateToken("read");
	CHECK(added.empty());
	CHECK(CountSpec(r) == 3);
	CHECK(!r.TakeListChangedFlag());
}

TEST_CASE("ActivateToken: after filter, activates the named category") {
	auto r = MakeWith({"read_blueprint", "add_node", "save_all",
					   "list_blueprints", "get_components"});
	r.ApplyFilter({"core"}, {});                // most still active per `core`
	// Clear the listChanged flag set by ApplyFilter — for this test we
	// care only about ActivateToken's behavior.
	(void)r.TakeListChangedFlag();
	const size_t before = r.Size();

	auto added = r.ActivateToken("read_blueprint");  // already active in core
	CHECK(added.empty());                            // no change
	CHECK(r.Size() == before);
	CHECK(!r.TakeListChangedFlag());                 // no flag flip
}

TEST_CASE("ActivateToken: adds tools that the initial filter excluded") {
	auto r = MakeWith({"read_blueprint", "add_node",
					   "list_materials", "set_material_parameter",
					   "compile_material"});
	// Initial filter: just core. list_materials et al. are inactive.
	r.ApplyFilter({"core"}, {});
	(void)r.TakeListChangedFlag();
	const size_t before = r.Size();
	REQUIRE(r.Find("set_material_parameter") == nullptr);  // confirm gated

	auto added = r.ActivateToken("materials");
	CHECK(!added.empty());                           // we added some
	CHECK(r.Size() > before);                        // active set grew
	CHECK(r.Find("set_material_parameter") != nullptr);  // now callable
	CHECK(r.TakeListChangedFlag());                  // flag was set
	CHECK(!r.TakeListChangedFlag());                 // and cleared on take
}

TEST_CASE("ActivateToken: 'all' widens to every registered tool") {
	auto r = MakeWith({"read_blueprint", "add_node", "list_materials",
					   "compile_material", "shutdown_daemon"});
	r.ApplyFilter({"core"}, {});
	(void)r.TakeListChangedFlag();

	auto added = r.ActivateToken("all");
	CHECK(r.Size() == r.TotalRegistered());
	CHECK(!added.empty());
}

TEST_CASE("ActivateToken: typo'd token is a silent no-op (flag stays clear)") {
	auto r = MakeWith({"read_blueprint", "add_node"});
	r.ApplyFilter({"read_blueprint"}, {});  // active = {read_blueprint}
	(void)r.TakeListChangedFlag();

	auto added = r.ActivateToken("matrials");  // typo
	CHECK(added.empty());
	CHECK(!r.TakeListChangedFlag());  // no flag flip on no-op
	CHECK(r.Find("add_node") == nullptr);  // still gated
}

TEST_CASE("TakeListChangedFlag: ApplyFilter does NOT set the flag (startup-only path)") {
	// ApplyFilter is called once at startup before any MCP client has
	// connected. Setting listChanged_ then would queue a spurious
	// notifications/tools/list_changed for the very first tools/call.
	// ActivateToken (the runtime widening path) is the one that sets
	// it.
	auto r = MakeWith({"read_blueprint", "add_node", "list_materials",
					   "compile_material"});
	r.ApplyFilter({"core"}, {});
	CHECK(!r.TakeListChangedFlag());               // startup => no flag
}

TEST_CASE("TakeListChangedFlag: ActivateToken sets the flag, take clears it") {
	auto r = MakeWith({"read_blueprint", "add_node", "list_materials",
					   "compile_material"});
	r.ApplyFilter({"core"}, {});
	(void)r.TakeListChangedFlag();                 // belt + suspenders

	r.ActivateToken("materials");
	CHECK(r.TakeListChangedFlag());
	CHECK(!r.TakeListChangedFlag());               // taking clears it
}
