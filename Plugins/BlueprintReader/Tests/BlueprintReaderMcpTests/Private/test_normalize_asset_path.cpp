// Unit tests for NormalizeAssetPath -- the inbound asset-path normaliser that
// makes both the package form (/Game/AI/BP_Foo) and the object form
// (/Game/AI/BP_Foo.BP_Foo) acceptable at the MCP tool boundary.  These tests
// don't need a UE editor; they exercise the string transform in isolation.

#include <doctest/doctest.h>
#include "tools/BlueprintToolsDetail.h"

using bpr::tools::blueprint_tools_detail::NormalizeAssetPath;
using bpr::tools::blueprint_tools_detail::RequireAssetPath;
using bpr::tools::blueprint_tools_detail::OptAssetPath;

TEST_CASE("NormalizeAssetPath: package path passes through unchanged") {
    CHECK(NormalizeAssetPath("/Game/AI/BP_Foo") == "/Game/AI/BP_Foo");
    CHECK(NormalizeAssetPath("/Game/Weapons/BP_Rifle") == "/Game/Weapons/BP_Rifle");
}

TEST_CASE("NormalizeAssetPath: strips trailing object-path suffix") {
    CHECK(NormalizeAssetPath("/Game/AI/BP_Foo.BP_Foo") == "/Game/AI/BP_Foo");
    CHECK(NormalizeAssetPath("/Game/Weapons/BP_Rifle.BP_Rifle") == "/Game/Weapons/BP_Rifle");
    // Non-BP object suffix (e.g. DataTable)
    CHECK(NormalizeAssetPath("/Game/Data/DT_Items.DT_Items") == "/Game/Data/DT_Items");
    // Different suffix class name
    CHECK(NormalizeAssetPath("/Game/AI/BP_Foo.SomeOtherClass") == "/Game/AI/BP_Foo");
}

TEST_CASE("NormalizeAssetPath: trims whitespace") {
    CHECK(NormalizeAssetPath("  /Game/AI/BP_Foo  ") == "/Game/AI/BP_Foo");
    CHECK(NormalizeAssetPath("\t/Game/AI/BP_Foo.BP_Foo\n") == "/Game/AI/BP_Foo");
}

TEST_CASE("NormalizeAssetPath: normalises backslashes") {
    CHECK(NormalizeAssetPath("\\Game\\AI\\BP_Foo") == "/Game/AI/BP_Foo");
    CHECK(NormalizeAssetPath("\\Game\\AI\\BP_Foo.BP_Foo") == "/Game/AI/BP_Foo");
}

TEST_CASE("NormalizeAssetPath: dot in directory name is not stripped") {
    // /Game/v1.0/BP_Foo — the dot is in a directory segment, not after the last slash
    CHECK(NormalizeAssetPath("/Game/v1.0/BP_Foo") == "/Game/v1.0/BP_Foo");
}

TEST_CASE("NormalizeAssetPath: idempotent") {
    const std::string once  = NormalizeAssetPath("/Game/AI/BP_Foo.BP_Foo");
    const std::string twice = NormalizeAssetPath(once);
    CHECK(once == twice);
    CHECK(once == "/Game/AI/BP_Foo");
}

TEST_CASE("RequireAssetPath: normalises the value") {
    nlohmann::json args = {{"asset_path", "/Game/AI/BP_Foo.BP_Foo"}};
    CHECK(RequireAssetPath(args) == "/Game/AI/BP_Foo");
}

TEST_CASE("OptAssetPath: normalises the value and returns fallback") {
    nlohmann::json args1 = {{"asset_path", "/Game/AI/BP_Foo.BP_Foo"}};
    CHECK(OptAssetPath(args1, "asset_path") == "/Game/AI/BP_Foo");

    nlohmann::json args2 = {};
    CHECK(OptAssetPath(args2, "asset_path", "/default") == "/default");
}
