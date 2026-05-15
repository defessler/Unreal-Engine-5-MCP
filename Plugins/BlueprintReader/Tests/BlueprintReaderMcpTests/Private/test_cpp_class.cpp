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
    // No module API macro passed → MinimalAPI keeps Cast<> working
    // across modules without exporting every symbol.
    CHECK(Contains(out.headerSource, "UCLASS(MinimalAPI, Blueprintable)"));
    CHECK(Contains(out.headerSource, "class ABP_Enemy_Generated : public ACharacter"));
    CHECK(Contains(out.headerSource, "GENERATED_BODY()"));
}

TEST_CASE("EmitCppClass: module_api_macro present → no MinimalAPI") {
    CppClassEmitOptions opts;
    opts.moduleApiMacro = "MYGAME_API";
    auto out = EmitCppClass(MakeMinimalClass(), opts);
    CHECK(Contains(out.headerSource, "UCLASS(Blueprintable)"));
    CHECK_FALSE(Contains(out.headerSource, "MinimalAPI"));
    CHECK(Contains(out.headerSource, "class MYGAME_API ABP_Enemy_Generated"));
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

// ===== Safety defaults for primitive UPROPERTYs (E2E fix from PR #44) ======

TEST_CASE("EmitCppClass: bool UPROPERTY with no default → = false (not UB)") {
    // BP defaults bool to false; uninitialized primitive class members
    // are undefined behavior in C++. We auto-emit the safety default.
    auto cls = MakeMinimalClass();
    cls["variables"] = json::array({
        json{{"name","bIsAlive"}, {"type","bool"}, {"editable", true}},
    });
    auto out = EmitCppClass(cls);
    CHECK(Contains(out.headerSource, "bool bIsAlive = false;"));
}

TEST_CASE("EmitCppClass: numeric UPROPERTY with no default → = 0") {
    auto cls = MakeMinimalClass();
    cls["variables"] = json::array({
        json{{"name","Count"}, {"type","int"}},
        json{{"name","Rate"},  {"type","float"}},
    });
    auto out = EmitCppClass(cls);
    CHECK(Contains(out.headerSource, "int32 Count = 0"));
    CHECK(Contains(out.headerSource, "float Rate = 0"));
}

TEST_CASE("EmitCppClass: object UPROPERTY with no default → no initializer "
          "(TObjectPtr default-constructs to nullptr)") {
    auto cls = MakeMinimalClass();
    cls["variables"] = json::array({
        json{{"name","Target"}, {"type","object:Actor"}, {"editable", true}},
    });
    auto out = EmitCppClass(cls);
    // No explicit initializer for the pointer — TObjectPtr<> default ctor
    // sets it to nullptr safely.
    CHECK(Contains(out.headerSource, "TObjectPtr<AActor> Target;"));
    CHECK_FALSE(Contains(out.headerSource, "Target = "));
}

// ===== ExposeOnSpawn + RepNotify ===========================================

TEST_CASE("EmitCppClass: expose_on_spawn → UPROPERTY meta=(ExposeOnSpawn=\"true\")") {
    auto cls = MakeMinimalClass();
    cls["variables"] = json::array({
        json{{"name","StartHealth"}, {"type","float"}, {"editable", true},
             {"expose_on_spawn", true}},
    });
    auto out = EmitCppClass(cls);
    CHECK(Contains(out.headerSource, "meta=(ExposeOnSpawn=\"true\")"));
}

TEST_CASE("EmitCppClass: rep_notify_func → ReplicatedUsing= + OnRep callback") {
    auto cls = MakeMinimalClass();
    cls["variables"] = json::array({
        json{{"name","Health"}, {"type","float"},
             {"replicated", true}, {"editable", true},
             {"rep_notify_func", "OnRep_Health"}},
    });
    auto out = EmitCppClass(cls);
    CHECK(Contains(out.headerSource, "ReplicatedUsing=OnRep_Health"));
    CHECK_FALSE(Contains(out.headerSource, ", Replicated,"));  // no plain Replicated
    // OnRep_X callback decl emitted in the header.
    CHECK(Contains(out.headerSource, "UFUNCTION()"));
    CHECK(Contains(out.headerSource, "void OnRep_Health();"));
}

TEST_CASE("EmitCppClass: replicated without rep_notify_func → plain Replicated") {
    auto cls = MakeMinimalClass();
    cls["variables"] = json::array({
        json{{"name","Score"}, {"type","int"}, {"replicated", true}},
    });
    auto out = EmitCppClass(cls);
    CHECK(Contains(out.headerSource, "Replicated"));
    CHECK_FALSE(Contains(out.headerSource, "ReplicatedUsing"));
}

// ===== Replication conditions =============================================

TEST_CASE("EmitCppClass: rep_condition=OwnerOnly → DOREPLIFETIME_CONDITION") {
    auto cls = MakeMinimalClass();
    cls["variables"] = json::array({
        json{{"name","Health"}, {"type","float"},
             {"replicated", true}, {"editable", true},
             {"rep_condition", "OwnerOnly"}},
    });
    auto out = EmitCppClass(cls);
    CHECK(Contains(out.implSource,
        "DOREPLIFETIME_CONDITION(ABP_Enemy_Generated, Health, COND_OwnerOnly)"));
    CHECK_FALSE(Contains(out.implSource,
        "DOREPLIFETIME(ABP_Enemy_Generated, Health)"));
}

TEST_CASE("EmitCppClass: rep_condition with explicit COND_ prefix → passes through") {
    auto cls = MakeMinimalClass();
    cls["variables"] = json::array({
        json{{"name","Health"}, {"type","float"},
             {"replicated", true},
             {"rep_condition", "COND_SimulatedOnly"}},
    });
    auto out = EmitCppClass(cls);
    CHECK(Contains(out.implSource, "COND_SimulatedOnly"));
    CHECK_FALSE(Contains(out.implSource, "COND_COND_"));  // no double-prefix
}

TEST_CASE("EmitCppClass: rep_condition=None or absent → unconditional DOREPLIFETIME") {
    auto cls = MakeMinimalClass();
    cls["variables"] = json::array({
        json{{"name","A"}, {"type","float"},
             {"replicated", true}, {"rep_condition","None"}},
        json{{"name","B"}, {"type","int"},
             {"replicated", true}},  // no rep_condition at all
    });
    auto out = EmitCppClass(cls);
    CHECK(Contains(out.implSource, "DOREPLIFETIME(ABP_Enemy_Generated, A)"));
    CHECK(Contains(out.implSource, "DOREPLIFETIME(ABP_Enemy_Generated, B)"));
}

// ===== Interface implementation ============================================

TEST_CASE("EmitCppClass: implements interfaces from BPIR interfaces[]") {
    auto cls = MakeMinimalClass();
    cls["interfaces"] = json::array({
        "Damageable", "BPI_Interactable", "/Script/Game.UPickupable",
    });
    auto out = EmitCppClass(cls);
    // All three forms normalize to the I-prefix.
    CHECK(Contains(out.headerSource,
        "class ABP_Enemy_Generated : public ACharacter, "
        "public IDamageable, "
        "public IInteractable, "
        "public IPickupable {"));
}

TEST_CASE("EmitCppClass: no interfaces → no extra inheritance entries") {
    auto out = EmitCppClass(MakeMinimalClass());
    // Bare parent, no interface listing.
    CHECK(Contains(out.headerSource,
        "class ABP_Enemy_Generated : public ACharacter {"));
    CHECK_FALSE(Contains(out.headerSource, ", public I"));
}

// ===== BlueprintPure inference ============================================

TEST_CASE("EmitCppClass: function with metadata.pure → BlueprintPure + const") {
    auto cls = MakeMinimalClass();
    cls["functions"] = json::array({
        json{
            {"version", 1}, {"kind", "function"}, {"name","GetHealth"},
            {"metadata", json{{"pure", true}}},
            {"outputs", json::array({json{{"name","Result"},{"type","float"}}})},
            {"body",    json::array()},
        },
    });
    auto out = EmitCppClass(cls);
    CHECK(Contains(out.headerSource, "UFUNCTION(BlueprintPure)"));
    CHECK(Contains(out.headerSource, "float GetHealth() const;"));
    CHECK(Contains(out.implSource,   "float ABP_Enemy_Generated::GetHealth() const {"));
}

TEST_CASE("EmitCppClass: function without metadata.pure → BlueprintCallable, no const") {
    auto cls = MakeMinimalClass();
    cls["functions"] = json::array({
        json{
            {"version", 1}, {"kind", "function"}, {"name","DoStuff"},
            {"outputs", json::array({json{{"name","Result"},{"type","float"}}})},
            {"body",    json::array()},
        },
    });
    auto out = EmitCppClass(cls);
    CHECK(Contains(out.headerSource, "UFUNCTION(BlueprintCallable)"));
    CHECK(Contains(out.headerSource, "float DoStuff();"));
    CHECK_FALSE(Contains(out.headerSource, "const;"));
}

// ===== Forward declarations (UE convention: forward-decl in .h, include in .cpp)

TEST_CASE("EmitCppClass: UPROPERTY object refs add forward declarations") {
    auto cls = MakeMinimalClass();  // parent ACharacter
    cls["variables"] = json::array({
        json{{"name","Mesh"},   {"type","object:StaticMeshComponent"},
             {"editable", true}, {"category","Visuals"}},
        json{{"name","Target"}, {"type","object:Actor"},
             {"editable", true}, {"category","AI"}},
    });
    auto out = EmitCppClass(cls);
    CHECK(Contains(out.headerSource, "class UStaticMeshComponent;"));
    CHECK(Contains(out.headerSource, "class AActor;"));
    // The forward decl must come BEFORE the .generated.h include —
    // putting it after would be a UHT error.
    auto fwdPos = out.headerSource.find("class UStaticMeshComponent;");
    auto genPos = out.headerSource.find(".generated.h");
    REQUIRE(fwdPos != std::string::npos);
    REQUIRE(genPos != std::string::npos);
    CHECK(fwdPos < genPos);
}

TEST_CASE("EmitCppClass: parent class is not double-declared as forward decl") {
    auto cls = MakeMinimalClass();  // parent ACharacter
    cls["variables"] = json::array({
        json{{"name","Target"}, {"type","object:Character"},
             {"editable", true}},
    });
    auto out = EmitCppClass(cls);
    // ACharacter is the parent; it's #included transitively, not
    // forward-declared.
    CHECK_FALSE(Contains(out.headerSource, "class ACharacter;"));
}

TEST_CASE("EmitCppClass: TArray<TObjectPtr<X>> still forward-declares X") {
    auto cls = MakeMinimalClass();
    cls["variables"] = json::array({
        json{{"name","Enemies"}, {"type","[]object:Pawn"},
             {"editable", true}},
    });
    auto out = EmitCppClass(cls);
    CHECK(Contains(out.headerSource, "class APawn;"));
}

// ===== TObjectPtr for UPROPERTY object members (UE5 convention) ============

TEST_CASE("EmitCppClass: UPROPERTY object refs wrap in TObjectPtr<>") {
    auto cls = MakeMinimalClass();
    cls["variables"] = json::array({
        json{{"name","AggroTarget"}, {"type","object:Actor"},
             {"editable", true}, {"category","AI"}},
    });
    auto out = EmitCppClass(cls);
    // Epic switched the canonical UPROPERTY pointer form in 5.0 — raw
    // pointers still compile but TObjectPtr is the recommended convention.
    CHECK(Contains(out.headerSource, "TObjectPtr<AActor> AggroTarget;"));
    CHECK_FALSE(Contains(out.headerSource, "AActor* AggroTarget"));
}

TEST_CASE("EmitCppClass: TArray of object refs also wraps inner element with TObjectPtr") {
    auto cls = MakeMinimalClass();
    cls["variables"] = json::array({
        json{{"name","Enemies"}, {"type","[]object:Pawn"},
             {"editable", true}, {"category","AI"}},
    });
    auto out = EmitCppClass(cls);
    CHECK(Contains(out.headerSource, "TArray<TObjectPtr<APawn>> Enemies;"));
}

TEST_CASE("Codegen: function-arg pointer types stay raw (not TObjectPtr)") {
    // TObjectPtr is ONLY for class members; func args + locals stay raw.
    CHECK(MapBpirTypeToCpp("object:Actor") == "AActor*");
    CHECK(MapBpirTypeToCppMember("object:Actor") == "TObjectPtr<AActor>");
}

// ===== Component-style UPROPERTY specifier inference =======================

TEST_CASE("EmitCppClass: U*Component variables get VisibleAnywhere + BlueprintReadOnly") {
    // UE convention: reassigning a Component pointer at runtime orphans
    // the original, so we mark these VisibleAnywhere (editable cascade
    // into the inner properties) + BlueprintReadOnly (BP can't swap
    // out the ptr).
    auto cls = MakeMinimalClass();
    cls["variables"] = json::array({
        json{{"name","Mesh"}, {"type","object:StaticMeshComponent"},
             {"editable", true}, {"category","Visuals"}},
    });
    auto out = EmitCppClass(cls);
    CHECK(Contains(out.headerSource, "VisibleAnywhere"));
    CHECK(Contains(out.headerSource, "BlueprintReadOnly"));
    CHECK_FALSE(Contains(out.headerSource, "EditAnywhere"));
    CHECK_FALSE(Contains(out.headerSource, "BlueprintReadWrite"));
}

TEST_CASE("EmitCppClass: actor refs do NOT get the component-style specifiers") {
    auto cls = MakeMinimalClass();
    cls["variables"] = json::array({
        json{{"name","Player"}, {"type","object:Actor"},
             {"editable", true}, {"category","World"}},
    });
    auto out = EmitCppClass(cls);
    CHECK(Contains(out.headerSource, "EditAnywhere"));
    CHECK(Contains(out.headerSource, "BlueprintReadWrite"));
    CHECK_FALSE(Contains(out.headerSource, "VisibleAnywhere"));
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
