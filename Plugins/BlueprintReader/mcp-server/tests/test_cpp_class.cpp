// Tests for CppClassEmit — full UCLASS .h/.cpp generation from a BPIR
// class doc. Same snapshot-style approach as test_cpp_codegen.cpp:
// substring asserts on rendered source so the test isn't brittle
// against whitespace / ordering tweaks.

#include <doctest/doctest.h>

#include "tools/codegen/CppClassEmit.h"
#include "tools/Bpir.h"

using namespace bpr::tools;
using nlohmann::json;

namespace {

bool Contains(const std::string& s, const std::string& sub) {
    return s.find(sub) != std::string::npos;
}

json MakeMinimalClass(const std::string& parent = "ACharacter") {
    return json{
        {"version", 1}, {"kind", "class"}, {"name", "BP_Enemy"},
        {"metadata", json{{"parent_class", parent}, {"asset_path", "/Game/AI/BP_Enemy"}}},
        {"variables", json::array()},
        {"functions", json::array()},
    };
}

} // namespace

// ===== Name + prefix logic ================================================

TEST_CASE("PrefixClassName: bare BP name + Actor parent → A prefix") {
    CHECK(PrefixClassName("BP_Enemy", "AActor") == "ABP_Enemy");
    CHECK(PrefixClassName("BP_Enemy", "ACharacter") == "ABP_Enemy");
    CHECK(PrefixClassName("BP_Enemy", "Actor") == "ABP_Enemy");
}

TEST_CASE("PrefixClassName: bare BP name + UObject parent → U prefix") {
    CHECK(PrefixClassName("BP_DataAsset", "UObject") == "UBP_DataAsset");
    CHECK(PrefixClassName("BP_DataAsset", "Object") == "UBP_DataAsset");
}

TEST_CASE("PrefixClassName: BP_C suffix gets stripped") {
    CHECK(PrefixClassName("BP_Enemy_C", "AActor") == "ABP_Enemy");
}

TEST_CASE("PrefixClassName: already-prefixed names pass through") {
    CHECK(PrefixClassName("ABP_Enemy", "AActor") == "ABP_Enemy");
    CHECK(PrefixClassName("UBP_Data", "UObject")  == "UBP_Data");
}

// ===== Parent header lookup ===============================================

TEST_CASE("ParentClassToHeader: well-known UE bases resolve") {
    CHECK(ParentClassToHeader("Actor")             == "GameFramework/Actor.h");
    CHECK(ParentClassToHeader("AActor")            == "GameFramework/Actor.h");
    CHECK(ParentClassToHeader("ACharacter")        == "GameFramework/Character.h");
    CHECK(ParentClassToHeader("APlayerController") == "GameFramework/PlayerController.h");
    CHECK(ParentClassToHeader("UObject")           == "UObject/Object.h");
    CHECK(ParentClassToHeader("UUserWidget")       == "Blueprint/UserWidget.h");
    CHECK(ParentClassToHeader("UAnimInstance")     == "Animation/AnimInstance.h");
}

TEST_CASE("ParentClassToHeader: dotted long-form path strips the dot prefix") {
    CHECK(ParentClassToHeader("/Script/Engine.Actor") == "GameFramework/Actor.h");
    CHECK(ParentClassToHeader("/Script/Engine.ACharacter") == "GameFramework/Character.h");
}

TEST_CASE("ParentClassToHeader: unknown falls back to a project-local include") {
    // Strips A/U prefix per UE convention (file naming = unprefixed).
    CHECK(ParentClassToHeader("AMyGameSpecificBase") == "MyGameSpecificBase.h");
    CHECK(ParentClassToHeader("UMyGameSpecificBase") == "MyGameSpecificBase.h");
    // Unprefixed identifiers pass through unchanged.
    CHECK(ParentClassToHeader("MyBase") == "MyBase.h");
}

// ===== UPROPERTY inference ================================================

TEST_CASE("BuildUPropertyList: editable + replicated + category") {
    json v = {{"name","Health"},{"type","float"},
              {"editable", true},{"replicated", true},{"category","Stats"}};
    std::string list = BuildUPropertyList(v);
    CHECK(Contains(list, "EditAnywhere"));
    CHECK(Contains(list, "BlueprintReadWrite"));
    CHECK(Contains(list, "Replicated"));
    CHECK(Contains(list, R"(Category="Stats")"));
}

TEST_CASE("BuildUPropertyList: bare BP variable still BlueprintReadWrite") {
    json v = {{"name","X"},{"type","int"}};
    std::string list = BuildUPropertyList(v);
    CHECK(Contains(list, "BlueprintReadWrite"));
    CHECK_FALSE(Contains(list, "EditAnywhere"));
    CHECK_FALSE(Contains(list, "Replicated"));
}

// ===== UFUNCTION inference ================================================

TEST_CASE("BuildUFunctionList: default to BlueprintCallable") {
    json fn = {{"name","Foo"},{"body", json::array()}};
    std::string list = BuildUFunctionList(fn);
    CHECK(list == "BlueprintCallable");
}

TEST_CASE("BuildUFunctionList: explicit specifiers in metadata override default") {
    json fn = {
        {"name","Foo"},{"body", json::array()},
        {"metadata", json{{"ufunction_specifiers", json::array({"BlueprintNativeEvent","Server","Reliable"})}}},
    };
    std::string list = BuildUFunctionList(fn);
    CHECK(Contains(list, "BlueprintNativeEvent"));
    CHECK(Contains(list, "Server"));
    CHECK(Contains(list, "Reliable"));
}

// ===== Full class emission ================================================

TEST_CASE("EmitCppClass: minimal class produces compile-shaped header") {
    auto out = EmitCppClass(MakeMinimalClass());
    CHECK(out.className == "ABP_Enemy_Generated");
    CHECK(out.headerFileName == "BP_Enemy_Generated.h");
    CHECK(out.implFileName   == "BP_Enemy_Generated.cpp");

    CHECK(Contains(out.headerSource, "#pragma once"));
    CHECK(Contains(out.headerSource, "#include \"CoreMinimal.h\""));
    CHECK(Contains(out.headerSource, "GameFramework/Character.h"));
    CHECK(Contains(out.headerSource, "BP_Enemy_Generated.generated.h"));
    CHECK(Contains(out.headerSource, "UCLASS(Blueprintable)"));
    CHECK(Contains(out.headerSource, "class ABP_Enemy_Generated : public ACharacter"));
    CHECK(Contains(out.headerSource, "GENERATED_BODY()"));
}

TEST_CASE("EmitCppClass: variables emit UPROPERTY decls + initializers") {
    auto cls = MakeMinimalClass();
    cls["variables"] = json::array({
        json{{"name","Health"},{"type","float"},
             {"editable", true},{"replicated", true},
             {"category","Stats"},{"default","100.000000"}},
        json{{"name","bIsAlive"},{"type","bool"},
             {"editable", true},{"default","true"}},
        json{{"name","Score"},{"type","int"},{"default","0"}},
    });
    auto out = EmitCppClass(cls);
    CHECK(Contains(out.headerSource, "UPROPERTY(EditAnywhere, BlueprintReadWrite, Replicated, Category=\"Stats\")"));
    // BP float "100.000000" trims to "100.0f" — see TrimFloatDefault.
    CHECK(Contains(out.headerSource, "float Health = 100.0f;"));
    CHECK(Contains(out.headerSource, "bool bIsAlive = true;"));
    CHECK(Contains(out.headerSource, "int32 Score = 0;"));
}

TEST_CASE("EmitCppClass: any replicated var triggers GetLifetimeReplicatedProps + DOREPLIFETIME") {
    auto cls = MakeMinimalClass();
    cls["variables"] = json::array({
        json{{"name","Health"},{"type","float"},{"replicated", true}},
        json{{"name","Mana"},  {"type","float"},{"replicated", true}},
        json{{"name","Local"}, {"type","int"}},  // not replicated
    });
    auto out = EmitCppClass(cls);
    CHECK(Contains(out.headerSource, "GetLifetimeReplicatedProps"));
    CHECK(Contains(out.implSource,   "GetLifetimeReplicatedProps"));
    CHECK(Contains(out.implSource,   "DOREPLIFETIME(ABP_Enemy_Generated, Health)"));
    CHECK(Contains(out.implSource,   "DOREPLIFETIME(ABP_Enemy_Generated, Mana)"));
    CHECK_FALSE(Contains(out.implSource, "DOREPLIFETIME(ABP_Enemy_Generated, Local)"));
    CHECK(Contains(out.implSource,   "#include \"Net/UnrealNetwork.h\""));
}

TEST_CASE("EmitCppClass: no replicated vars omits replication boilerplate") {
    auto cls = MakeMinimalClass();
    cls["variables"] = json::array({
        json{{"name","Health"},{"type","float"}},
    });
    auto out = EmitCppClass(cls);
    CHECK_FALSE(Contains(out.headerSource, "GetLifetimeReplicatedProps"));
    CHECK_FALSE(Contains(out.implSource,   "Net/UnrealNetwork.h"));
}

TEST_CASE("EmitCppClass: functions emit UFUNCTION + impl") {
    auto cls = MakeMinimalClass();
    cls["functions"] = json::array({
        json{
            {"version", 1}, {"kind", "function"}, {"name","TakeDamage"},
            {"inputs", json::array({json{{"name","Amount"},{"type","float"}}})},
            {"outputs", json::array({json{{"name","Killed"},{"type","bool"}}})},
            {"body", json::array({
                json{{"return", json{{"var","Killed"}}}}
            })},
        },
    });
    auto out = EmitCppClass(cls);
    CHECK(Contains(out.headerSource, "UFUNCTION(BlueprintCallable)"));
    CHECK(Contains(out.headerSource, "bool TakeDamage(float Amount);"));
    CHECK(Contains(out.implSource,   "bool ABP_Enemy_Generated::TakeDamage(float Amount) {"));
    CHECK(Contains(out.implSource,   "return Killed;"));
}

TEST_CASE("EmitCppClass: module API macro is applied to the class declaration") {
    auto cls = MakeMinimalClass();
    CppClassEmitOptions opts;
    opts.moduleApiMacro = "MYGAME_API";
    auto out = EmitCppClass(cls, opts);
    CHECK(Contains(out.headerSource, "class MYGAME_API ABP_Enemy_Generated"));
}

TEST_CASE("EmitCppClass: classNameSuffix=\"\" drops in place of the BP") {
    auto cls = MakeMinimalClass();
    CppClassEmitOptions opts;
    opts.classNameSuffix = "";
    auto out = EmitCppClass(cls, opts);
    CHECK(out.className      == "ABP_Enemy");
    CHECK(out.headerFileName == "BP_Enemy.h");
}

TEST_CASE("EmitCppClass: unknown parent class falls back to a project-local include") {
    auto cls = MakeMinimalClass("UMyGameSpecificBase");
    auto out = EmitCppClass(cls);
    // Strips the U prefix per UE file-naming convention.
    CHECK(Contains(out.headerSource, "#include \"MyGameSpecificBase.h\""));
    // Doesn't emit the old TODO marker — the include is enough for UBT.
    CHECK_FALSE(Contains(out.headerSource, "TODO[bpr-include]"));
}

TEST_CASE("EmitCppClass: function body unsupported nodes propagate to top-level notes") {
    auto cls = MakeMinimalClass();
    // Use a non-UE-reserved function name so we don't also pick up a
    // <name-collision> warning here — this test is about Timeline.
    cls["functions"] = json::array({
        json{
            {"version", 1}, {"kind", "function"}, {"name","RunTimeline"},
            {"body", json::array({
                json{{"unsupported", json{{"node_class","K2Node_Timeline"},{"guid","abc"}}}}
            })},
        },
    });
    auto out = EmitCppClass(cls);
    REQUIRE(out.notes.size() >= 1);
    CHECK(out.notes[0]["node_class"] == "K2Node_Timeline");
}

TEST_CASE("EmitCppClass rejects malformed BPIR") {
    json bad = {{"version", 1}, {"kind", "class"}, {"name", "X"},
                {"variables", json::array({json{{"name","V"}}})}};  // missing type
    CHECK_THROWS_AS(EmitCppClass(bad), std::invalid_argument);
}

TEST_CASE("EmitCppClass rejects function-shaped doc") {
    json fn = {{"version", 1}, {"kind", "function"}, {"name","X"}, {"body", json::array()}};
    CHECK_THROWS_AS(EmitCppClass(fn), std::invalid_argument);
}

// ===== Constructor + replication wiring ===================================

TEST_CASE("EmitCppClass: Actor + replicated UPROPERTY → emits constructor that sets bReplicates") {
    auto cls = MakeMinimalClass();  // parent = ACharacter
    cls["variables"] = json::array({
        json{{"name","Health"}, {"type","real"},
             {"replicated", true}, {"editable", true},
             {"category", "Combat"}, {"default", "100.0"}},
    });
    auto out = EmitCppClass(cls);
    // Header has the constructor decl.
    CHECK(Contains(out.headerSource, "ABP_Enemy_Generated();"));
    // Impl has the constructor body + bReplicates = true.
    CHECK(Contains(out.implSource, "ABP_Enemy_Generated::ABP_Enemy_Generated()"));
    CHECK(Contains(out.implSource, "bReplicates = true;"));
    // Replication registration still emits as before.
    CHECK(Contains(out.implSource, "DOREPLIFETIME(ABP_Enemy_Generated, Health)"));
}

TEST_CASE("EmitCppClass: Actor without replicated UPROPERTY → no constructor needed") {
    auto cls = MakeMinimalClass();
    cls["variables"] = json::array({
        json{{"name","Health"}, {"type","real"}, {"replicated", false},
             {"editable", true}},
    });
    auto out = EmitCppClass(cls);
    CHECK_FALSE(Contains(out.headerSource, "ABP_Enemy_Generated();"));
    CHECK_FALSE(Contains(out.implSource, "bReplicates = true;"));
}

TEST_CASE("EmitCppClass: UObject parent + replicated UPROPERTY → no bReplicates "
          "(UObjects don't have the flag)") {
    auto cls = MakeMinimalClass("UObject");
    cls["variables"] = json::array({
        json{{"name","X"}, {"type","int"}, {"replicated", true}, {"editable", false}},
    });
    auto out = EmitCppClass(cls);
    CHECK_FALSE(Contains(out.implSource, "bReplicates = true;"));
}

// ===== Name-collision warning =============================================

TEST_CASE("EmitCppClass: UFUNCTION shadowing AActor::TakeDamage emits a sidecar warning") {
    auto cls = MakeMinimalClass();
    cls["functions"] = json::array({
        json{
            {"version", 1}, {"kind", "function"}, {"name", "TakeDamage"},
            {"inputs", json::array({json{{"name","Amount"},{"type","real"}}})},
            {"body", json::array()},
        },
    });
    auto out = EmitCppClass(cls);
    // Code still generates (we don't auto-rename — could break BP-side
    // expectations). The warning is in the sidecar.
    CHECK(Contains(out.headerSource, "TakeDamage"));
    bool foundCollision = false;
    for (const auto& n : out.notes) {
        if (n.value("node_class", "") == "<name-collision>") {
            foundCollision = true;
            CHECK(n.value("function", "") == "TakeDamage");
        }
    }
    CHECK(foundCollision);
}

// ===== Default-value cleanup ==============================================

TEST_CASE("EmitCppClass: float default trims trailing zeros") {
    auto cls = MakeMinimalClass();
    cls["variables"] = json::array({
        json{{"name","Health"}, {"type","real"}, {"editable", true},
             {"default", "100.000000"}},
    });
    auto out = EmitCppClass(cls);
    // "100.000000" → "100.0f", not "100.000000f".
    CHECK(Contains(out.headerSource, "Health = 100.0f"));
    CHECK_FALSE(Contains(out.headerSource, "100.000000f"));
}

TEST_CASE("EmitCppClass: FString / FName defaults wrap with TEXT()") {
    auto cls = MakeMinimalClass();
    cls["variables"] = json::array({
        json{{"name","Greeting"}, {"type","string"}, {"editable", true},
             {"default", "Hello, World"}},
        json{{"name","Tag"},      {"type","name"},   {"editable", true},
             {"default", "Player"}},
    });
    auto out = EmitCppClass(cls);
    CHECK(Contains(out.headerSource, R"(Greeting = TEXT("Hello, World"))"));
    CHECK(Contains(out.headerSource, R"(Tag = TEXT("Player"))"));
}
