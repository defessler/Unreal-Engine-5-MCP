// Tests for the type-shorthand parser used by every write tool that
// takes a `type` argument.

#include <doctest/doctest.h>

#include "tools/TypeShorthand.h"

#include <stdexcept>

using bpr::tools::ParseTypeArg;
using bpr::tools::ParseTypeShorthand;
using nlohmann::json;

TEST_CASE("Shorthand: primitive aliases") {
    auto t = ParseTypeShorthand("float");
    CHECK(t.Category == "real");
    REQUIRE(t.SubCategory.has_value());
    CHECK(*t.SubCategory == "float");
    CHECK_FALSE(t.IsArray);

    CHECK(ParseTypeShorthand("real").Category   == "real");
    CHECK(ParseTypeShorthand("double").Category == "real");
    REQUIRE(ParseTypeShorthand("double").SubCategory.has_value());
    CHECK(*ParseTypeShorthand("double").SubCategory == "double");

    CHECK(ParseTypeShorthand("int").Category    == "int");
    CHECK(ParseTypeShorthand("int64").Category  == "int64");
    CHECK(ParseTypeShorthand("bool").Category   == "bool");
    CHECK(ParseTypeShorthand("string").Category == "string");
    CHECK(ParseTypeShorthand("name").Category   == "name");
    CHECK(ParseTypeShorthand("text").Category   == "text");
    CHECK(ParseTypeShorthand("byte").Category   == "byte");
    CHECK(ParseTypeShorthand("exec").Category   == "exec");
}

TEST_CASE("Shorthand: object/class/struct/interface refs") {
    auto t = ParseTypeShorthand("object:Actor");
    CHECK(t.Category == "object");
    REQUIRE(t.SubCategoryObject.has_value());
    CHECK(*t.SubCategoryObject == "Actor");

    auto u = ParseTypeShorthand("struct:FVector");
    CHECK(u.Category == "struct");
    REQUIRE(u.SubCategoryObject.has_value());
    CHECK(*u.SubCategoryObject == "FVector");

    auto i = ParseTypeShorthand("interface:IDamageable");
    CHECK(i.Category == "interface");
    REQUIRE(i.SubCategoryObject.has_value());
    CHECK(*i.SubCategoryObject == "IDamageable");

    auto e = ParseTypeShorthand("enum:EWeaponType");
    CHECK(e.Category == "byte");  // UE enums encode as byte+enum-ref
    REQUIRE(e.SubCategoryObject.has_value());
    CHECK(*e.SubCategoryObject == "EWeaponType");

    // Long path also accepted.
    auto p = ParseTypeShorthand("object:/Game/AI/MyClass.MyClass_C");
    CHECK(p.Category == "object");
    CHECK(*p.SubCategoryObject == "/Game/AI/MyClass.MyClass_C");
}

TEST_CASE("Shorthand: array container") {
    auto t = ParseTypeShorthand("[]float");
    CHECK(t.IsArray);
    CHECK(t.Category == "real");
    CHECK(*t.SubCategory == "float");

    auto u = ParseTypeShorthand("[]object:Actor");
    CHECK(u.IsArray);
    CHECK(u.Category == "object");
    CHECK(*u.SubCategoryObject == "Actor");
}

TEST_CASE("Shorthand: set container") {
    auto t = ParseTypeShorthand("{}int");
    CHECK(t.IsSet);
    CHECK(t.Category == "int");
}

TEST_CASE("Shorthand: map container") {
    auto t = ParseTypeShorthand("{string:int}");
    CHECK(t.IsMap);
    CHECK(t.Category == "int");
    REQUIRE(t.SubCategory.has_value());
    CHECK(*t.SubCategory == "string");
}

TEST_CASE("Shorthand: errors on unknown / malformed") {
    CHECK_THROWS_AS(ParseTypeShorthand(""),               std::invalid_argument);
    CHECK_THROWS_AS(ParseTypeShorthand("nope"),           std::invalid_argument);
    CHECK_THROWS_AS(ParseTypeShorthand("float:Actor"),    std::invalid_argument);
    CHECK_THROWS_AS(ParseTypeShorthand("object"),         std::invalid_argument);
    CHECK_THROWS_AS(ParseTypeShorthand("[]"),             std::invalid_argument);
}

TEST_CASE("ParseTypeArg: accepts canonical object form unchanged") {
    json obj = {{"category","real"},{"sub_category","float"}};
    auto t = ParseTypeArg(obj);
    CHECK(t.Category == "real");
    CHECK(*t.SubCategory == "float");
}

TEST_CASE("ParseTypeArg: accepts string shorthand") {
    auto t = ParseTypeArg(json("object:Actor"));
    CHECK(t.Category == "object");
    CHECK(*t.SubCategoryObject == "Actor");
}

TEST_CASE("ParseTypeArg: object form requires category") {
    CHECK_THROWS_AS(ParseTypeArg(json::object()), std::invalid_argument);
}

TEST_CASE("ParseTypeArg: rejects non-string non-object") {
    CHECK_THROWS_AS(ParseTypeArg(json(42)),           std::invalid_argument);
    CHECK_THROWS_AS(ParseTypeArg(json::array()),      std::invalid_argument);
    CHECK_THROWS_AS(ParseTypeArg(json(nullptr)),      std::invalid_argument);
}

TEST_CASE("ParseTypeArg: object form full options") {
    json obj = {
        {"category","struct"},
        {"sub_category_object","FVector"},
        {"is_array", true},
    };
    auto t = ParseTypeArg(obj);
    CHECK(t.Category == "struct");
    CHECK(*t.SubCategoryObject == "FVector");
    CHECK(t.IsArray);
}
