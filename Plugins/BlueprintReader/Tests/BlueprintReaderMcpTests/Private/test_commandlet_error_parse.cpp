// UX-P5 e1 (follow-up): engine-free unit test for the structured-error
// extraction the commandlet backend's RunOp does on a non-zero exit. This is
// the CI-runnable guard for the NodeRefError-survives-apply_ops behavior that
// is otherwise reachable only through a live editor (Saved/verify-e1-polish.ps1).

#include <doctest/doctest.h>

#include "backends/CommandletErrorParse.h"
#include "backends/CommandletResultParse.h"

#include <filesystem>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

using bpr::backends::detail::ExtractStructuredError;
using bpr::backends::detail::ParseAssetRegistryRows;

namespace {
std::filesystem::path WriteTemp(const std::string& name, const std::string& content) {
	auto p = std::filesystem::temp_directory_path() / name;
	std::ofstream(p) << content;
	return p;
}
}    // namespace

TEST_CASE("ExtractStructuredError: surfaces the detailed NodeRefError message") {
	auto p = WriteTemp("bpr_err_node.json",
		R"({"ok":false,"error":"SetNodePosition: node ref 'ab' not found in graph )"
		R"('EventGraph' — need a full 32-hex GUID. Known node GUIDs: 024DEDED-45CA"})");
	auto s = ExtractStructuredError(p);
	REQUIRE(s.has_value());
	CHECK(s->find("Known node GUIDs") != std::string::npos);    // did-you-mean survives
	std::filesystem::remove(p);
}

TEST_CASE("ExtractStructuredError: nullopt for the asset-not-found fallback cases") {
	// (a) missing file — a genuine asset-not-found does a bare `return 4` with
	//     no output file, so this must fall through to AssetNotFound.
	CHECK_FALSE(ExtractStructuredError(
		std::filesystem::temp_directory_path() / "bpr_err_does_not_exist_zzz.json").has_value());

	// (b) non-JSON / truncated output → nullopt (don't surface garbage).
	auto p1 = WriteTemp("bpr_err_garbage.json", "not json at all {");
	CHECK_FALSE(ExtractStructuredError(p1).has_value());
	std::filesystem::remove(p1);

	// (c) JSON object with no string `error` field → nullopt.
	auto p2 = WriteTemp("bpr_err_ok.json", R"({"ok":true})");
	CHECK_FALSE(ExtractStructuredError(p2).has_value());
	std::filesystem::remove(p2);
	auto p3 = WriteTemp("bpr_err_num.json", R"({"error":123})");
	CHECK_FALSE(ExtractStructuredError(p3).has_value());
	std::filesystem::remove(p3);

	// (d) empty error string → nullopt (degenerate — never throw "").
	auto p4 = WriteTemp("bpr_err_empty.json", R"({"error":""})");
	CHECK_FALSE(ExtractStructuredError(p4).has_value());
	std::filesystem::remove(p4);
}

TEST_CASE("ParseAssetRegistryRows: maps rows under the named key") {
	const nlohmann::json j = {
		{"assets", nlohmann::json::array({
			{{"asset_path", "/Game/AI/BP_A"}, {"name", "BP_A"}, {"class_name", "/Script/Engine.Blueprint"}},
			{{"asset_path", "/Game/AI/BP_B"}, {"name", "BP_B"}, {"class_name", "/Script/Engine.Blueprint"}},
		})},
	};
	const auto r = ParseAssetRegistryRows(j, "assets");
	REQUIRE(r.entries.size() == 2);
	CHECK(r.entries[0].assetPath == "/Game/AI/BP_A");
	CHECK(r.entries[1].name == "BP_B");
	CHECK(r.entries[0].className == "/Script/Engine.Blueprint");
}

TEST_CASE("ParseAssetRegistryRows: empty for a missing/wrong key, a non-object, or a non-array value") {
	const nlohmann::json withAssets = {{"assets", nlohmann::json::array()}};
	CHECK(ParseAssetRegistryRows(withAssets, "matches").entries.empty());          // wrong key
	CHECK(ParseAssetRegistryRows(nlohmann::json::array(), "assets").entries.empty()); // not an object
	const nlohmann::json notArray = {{"assets", 42}};
	CHECK(ParseAssetRegistryRows(notArray, "assets").entries.empty());             // value isn't an array
}
