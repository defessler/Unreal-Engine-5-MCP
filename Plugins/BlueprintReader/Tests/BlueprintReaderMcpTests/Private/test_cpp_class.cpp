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

// ===== Virtual-override emission ==========================================
//
// BP functions whose name matches a known-void-return UE base-class
// virtual (BeginPlay, Tick, EndPlay, Destroyed, ...) AND whose BP
// signature is also void get emitted as `virtual void Name(args)
// override;`. The Angelscript equivalent is
// `UFUNCTION(BlueprintOverride) void BeginPlay() {}`. Reserved names
// with non-void BP returns (or non-whitelisted names like TakeDamage
// where the parent has a non-void return) fall back to UFUNCTION +
// a sidecar collision warning -- the agent verifies the parent
// signature before refactoring.

TEST_CASE("EmitCppClass: BeginPlay emits as virtual override (no UFUNCTION)") {
    auto cls = MakeMinimalClass();
    cls["functions"] = json::array({
        json{
            {"version", 1}, {"kind", "function"}, {"name", "BeginPlay"},
            {"body", json::array()},
        },
    });
    auto out = EmitCppClass(cls);
    CHECK(Contains(out.headerSource, "virtual void BeginPlay() override;"));
    CHECK_FALSE(Contains(out.headerSource, "UFUNCTION(BlueprintCallable)\n    void BeginPlay"));
    // Sidecar note with the override treatment.
    bool foundOverride = false;
    for (const auto& n : out.notes) {
        if (n.value("treatment", "") == "virtual_override" &&
            n.value("function", "")  == "BeginPlay") {
            foundOverride = true;
        }
    }
    CHECK(foundOverride);
}

TEST_CASE("EmitCppClass: Tick emits as virtual override with DeltaSeconds arg") {
    auto cls = MakeMinimalClass();
    cls["functions"] = json::array({
        json{
            {"version", 1}, {"kind", "function"}, {"name", "Tick"},
            {"inputs", json::array({json{{"name","DeltaSeconds"},{"type","float"}}})},
            {"body", json::array()},
        },
    });
    auto out = EmitCppClass(cls);
    CHECK(Contains(out.headerSource, "virtual void Tick(float DeltaSeconds) override;"));
}

TEST_CASE("EmitCppClass: TakeDamage NOT auto-overridden (parent returns float, risky)") {
    // TakeDamage's parent virtual has a non-void return. Auto-emitting
    // override would risk a signature mismatch. Fall back to UFUNCTION
    // + collision warning so the agent verifies + refactors.
    auto cls = MakeMinimalClass();
    cls["functions"] = json::array({
        json{
            {"version", 1}, {"kind", "function"}, {"name", "TakeDamage"},
            {"inputs", json::array({json{{"name","Amount"},{"type","real"}}})},
            {"body", json::array()},
        },
    });
    auto out = EmitCppClass(cls);
    CHECK_FALSE(Contains(out.headerSource, "virtual void TakeDamage"));
    CHECK(Contains(out.headerSource, "UFUNCTION("));
    bool foundCollision = false;
    for (const auto& n : out.notes) {
        if (n.value("treatment", "") == "todo_comment" &&
            n.value("function", "")  == "TakeDamage") {
            foundCollision = true;
        }
    }
    CHECK(foundCollision);
}

TEST_CASE("EmitCppClass: reserved name with BP outputs gets collision warning") {
    // BeginPlay is on the override whitelist, but if the BP author
    // added a return value, the BP signature doesn't match the
    // parent. Warn instead of emitting an incorrect override.
    auto cls = MakeMinimalClass();
    cls["functions"] = json::array({
        json{
            {"version", 1}, {"kind", "function"}, {"name", "BeginPlay"},
            {"outputs", json::array({json{{"name","Ready"},{"type","bool"}}})},
            {"body", json::array()},
        },
    });
    auto out = EmitCppClass(cls);
    CHECK_FALSE(Contains(out.headerSource, "virtual void BeginPlay"));
    bool foundCollision = false;
    for (const auto& n : out.notes) {
        if (n.value("treatment", "") == "todo_comment" &&
            n.value("function", "")  == "BeginPlay") {
            foundCollision = true;
        }
    }
    CHECK(foundCollision);
}

TEST_CASE("EmitCppClass: non-reserved name emits as normal UFUNCTION") {
    // Regression check: only the whitelisted reserved-virtual names
    // get the override treatment; everything else stays UFUNCTION.
    auto cls = MakeMinimalClass();
    cls["functions"] = json::array({
        json{
            {"version", 1}, {"kind", "function"}, {"name", "CustomLogic"},
            {"body", json::array()},
        },
    });
    auto out = EmitCppClass(cls);
    CHECK(Contains(out.headerSource, "UFUNCTION("));
    CHECK(Contains(out.headerSource, "void CustomLogic("));
    CHECK_FALSE(Contains(out.headerSource, "virtual void CustomLogic"));
}

// ===== BlueprintImplementableEvent / BlueprintNativeEvent ==================
//
// BlueprintImplementableEvent: header decl only. UE generates the
// dispatcher in generated.cpp -- emitting an impl would conflict.
//
// BlueprintNativeEvent: same _Implementation suffix as RPCs. UHT
// generates the dispatch wrapper that calls _Implementation by
// default, with BPs free to override the bare name.

TEST_CASE("EmitCppClass: BlueprintImplementableEvent has no impl body") {
    auto cls = MakeMinimalClass();
    cls["functions"] = json::array({
        json{
            {"version", 1}, {"kind", "function"}, {"name", "OnSomething"},
            {"metadata", json{
                {"ufunction_specifiers", json::array({"BlueprintImplementableEvent"})},
            }},
            {"body", json::array()},
        },
    });
    auto out = EmitCppClass(cls);
    // Header decl is present.
    CHECK(Contains(out.headerSource, "UFUNCTION(BlueprintImplementableEvent)"));
    CHECK(Contains(out.headerSource, "void OnSomething("));
    // No impl at all -- if we emitted one, UHT would conflict.
    CHECK_FALSE(Contains(out.implSource, "::OnSomething("));
    CHECK_FALSE(Contains(out.implSource, "OnSomething_Implementation"));
}

TEST_CASE("EmitCppClass: BlueprintNativeEvent gets _Implementation suffix on impl") {
    auto cls = MakeMinimalClass();
    cls["functions"] = json::array({
        json{
            {"version", 1}, {"kind", "function"}, {"name", "OnReady"},
            {"metadata", json{
                {"ufunction_specifiers", json::array({"BlueprintNativeEvent"})},
            }},
            {"body", json::array()},
        },
    });
    auto out = EmitCppClass(cls);
    // Header decl with the specifier; bare name (no suffix on header).
    CHECK(Contains(out.headerSource, "UFUNCTION(BlueprintNativeEvent)"));
    CHECK(Contains(out.headerSource, "void OnReady("));
    CHECK_FALSE(Contains(out.headerSource, "OnReady_Implementation"));
    // Impl has the _Implementation suffix.
    CHECK(Contains(out.implSource, "::OnReady_Implementation("));
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

// ===== Multicast delegate variable handling ================================
//
// BP multicast delegate variables arrive with type category `mcdelegate`
// (or the prefixed variants). Without a typedef, that literal string
// leaks into the C++ header and breaks compilation. We emit a
// DECLARE_DYNAMIC_MULTICAST_DELEGATE between the includes and the
// UCLASS, with the F<VarName> typedef, then use that typedef as the
// member's type. Param threading is a future PR -- for now the macro
// is zero-args + a TODO that flags it.

TEST_CASE("EmitCppClass: mcdelegate var emits DECLARE macro + F<Name> typedef") {
    auto cls = MakeMinimalClass();
    cls["variables"] = json::array({
        json{{"name","OnSomethingHappened"}, {"type","mcdelegate"}},
    });
    auto out = EmitCppClass(cls);
    CHECK(Contains(out.headerSource,
        "DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnSomethingHappened);"));
    // The UPROPERTY uses the typedef, not the literal "mcdelegate".
    CHECK(Contains(out.headerSource, "FOnSomethingHappened OnSomethingHappened"));
    CHECK_FALSE(Contains(out.headerSource, "mcdelegate OnSomethingHappened"));
    // TODO marker present so the agent knows the params are stubbed.
    CHECK(Contains(out.headerSource, "TODO[bpr-delegate-signature]"));
    // Sidecar note carries the treatment marker.
    bool found = false;
    for (const auto& n : out.notes) {
        if (n.value("treatment", "") == "delegate_typedef_stub" &&
            n.value("bp_var_name", "") == "OnSomethingHappened") {
            found = true; break;
        }
    }
    CHECK(found);
}

TEST_CASE("EmitCppClass: mcdelegate-prefixed types (with sub-category) also match") {
    auto cls = MakeMinimalClass();
    cls["variables"] = json::array({
        json{{"name","OnReady"}, {"type","MulticastInlineDelegate"}},
    });
    auto out = EmitCppClass(cls);
    CHECK(Contains(out.headerSource, "DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnReady);"));
    CHECK(Contains(out.headerSource, "FOnReady OnReady"));
}

TEST_CASE("EmitCppClass: mcdelegate var with F-prefix name isn't double-prefixed") {
    auto cls = MakeMinimalClass();
    cls["variables"] = json::array({
        json{{"name","FOnStuff"}, {"type","mcdelegate"}},
    });
    auto out = EmitCppClass(cls);
    CHECK(Contains(out.headerSource, "DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnStuff);"));
    CHECK_FALSE(Contains(out.headerSource, "FFOnStuff"));
}

TEST_CASE("EmitCppClass: no mcdelegate vars -> no DECLARE emitted") {
    auto cls = MakeMinimalClass();
    cls["variables"] = json::array({
        json{{"name","Health"}, {"type","float"}},
    });
    auto out = EmitCppClass(cls);
    CHECK_FALSE(Contains(out.headerSource, "DECLARE_DYNAMIC_MULTICAST_DELEGATE"));
}

// ===== Public API: class_name_prefix =======================================

TEST_CASE("EmitCppClass: class_name_prefix injects house prefix and CamelCases the BP name") {
    auto cls = MakeMinimalClass("AActor");
    cls["name"] = "BP_Enemy";
    CppClassEmitOptions opts;
    opts.classNamePrefix = "Foo";
    opts.classNameSuffix = "";  // drop-in replacement
    auto out = EmitCppClass(cls, opts);
    // Expect AFooBPEnemy (UE letter + house prefix + CamelCased name).
    CHECK(out.className == "AFooBPEnemy");
    CHECK(Contains(out.headerSource, "class AFooBPEnemy"));
}

TEST_CASE("EmitCppClass: class_name_prefix empty preserves legacy ABP_Enemy form") {
    auto cls = MakeMinimalClass("AActor");
    cls["name"] = "BP_Enemy";
    auto out = EmitCppClass(cls);  // default opts (empty prefix)
    CHECK(out.className == "ABP_Enemy_Generated");
}

// ===== Public API: category_default / category_remap =======================

TEST_CASE("EmitCppClass: category_default fills in when var has no category") {
    auto cls = MakeMinimalClass();
    cls["variables"] = json::array({
        json{{"name","Health"}, {"type","float"}, {"editable", true}},
    });
    CppClassEmitOptions opts;
    opts.categoryDefault = "MyGame";
    auto out = EmitCppClass(cls, opts);
    CHECK(Contains(out.headerSource, "Category=\"MyGame\""));
}

TEST_CASE("EmitCppClass: category_default does NOT override existing categories") {
    auto cls = MakeMinimalClass();
    cls["variables"] = json::array({
        json{{"name","Health"}, {"type","float"}, {"editable", true},
             {"category", "Stats"}},
    });
    CppClassEmitOptions opts;
    opts.categoryDefault = "MyGame";
    auto out = EmitCppClass(cls, opts);
    CHECK(Contains(out.headerSource, "Category=\"Stats\""));
    CHECK_FALSE(Contains(out.headerSource, "Category=\"MyGame\""));
}

TEST_CASE("EmitCppClass: category_remap rewrites BP categories to project ones") {
    auto cls = MakeMinimalClass();
    cls["variables"] = json::array({
        json{{"name","Health"}, {"type","float"}, {"editable", true},
             {"category", "Default"}},
        json{{"name","State"},  {"type","int"},   {"editable", true},
             {"category", "Internal State"}},
    });
    CppClassEmitOptions opts;
    opts.categoryRemap = {
        {"Default",        "MyGame"},
        {"Internal State", "MyGame|Debug"},
    };
    auto out = EmitCppClass(cls, opts);
    CHECK(Contains(out.headerSource, "Category=\"MyGame\""));
    CHECK(Contains(out.headerSource, "Category=\"MyGame|Debug\""));
    CHECK_FALSE(Contains(out.headerSource, "Category=\"Default\""));
    CHECK_FALSE(Contains(out.headerSource, "Category=\"Internal State\""));
}

// ===== Public API: uclass_meta =============================================

TEST_CASE("EmitCppClass: uclass_meta folds into UCLASS(...) macro") {
    auto cls = MakeMinimalClass();
    CppClassEmitOptions opts;
    opts.uclassMeta = {
        {"PrioritizeCategories", "MyGame"},
        {"DisplayName",          "Friendly Name"},
    };
    auto out = EmitCppClass(cls, opts);
    CHECK(Contains(out.headerSource, "UCLASS("));
    CHECK(Contains(out.headerSource, "meta=("));
    CHECK(Contains(out.headerSource, "PrioritizeCategories=\"MyGame\""));
    CHECK(Contains(out.headerSource, "DisplayName=\"Friendly Name\""));
}

TEST_CASE("EmitCppClass: empty uclass_meta keeps UCLASS line bare") {
    auto cls = MakeMinimalClass();
    auto out = EmitCppClass(cls);
    CHECK_FALSE(Contains(out.headerSource, "meta=("));
}

// ===== Public API: delegate_typedef_pattern ================================

TEST_CASE("EmitCppClass: delegate_typedef_pattern lets projects rename derived typedefs") {
    auto cls = MakeMinimalClass();
    cls["variables"] = json::array({
        json{{"name","OnReady"}, {"type","mcdelegate"}},
    });
    CppClassEmitOptions opts;
    opts.delegateTypedefPattern = "F{Name}Delegate";
    auto out = EmitCppClass(cls, opts);
    CHECK(Contains(out.headerSource,
        "DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnReadyDelegate);"));
    CHECK(Contains(out.headerSource, "FOnReadyDelegate OnReady"));
    CHECK_FALSE(Contains(out.headerSource, "DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnReady);"));
}

TEST_CASE("EmitCppClass: delegate_typedef_pattern handles F-prefixed BP var names cleanly") {
    auto cls = MakeMinimalClass();
    cls["variables"] = json::array({
        json{{"name","FOnReady"}, {"type","mcdelegate"}},
    });
    CppClassEmitOptions opts;
    opts.delegateTypedefPattern = "F{Name}Delegate";
    auto out = EmitCppClass(cls, opts);
    // F stripped before substitution -> FOnReadyDelegate, not FFOnReadyDelegate.
    CHECK(Contains(out.headerSource, "FOnReadyDelegate"));
    CHECK_FALSE(Contains(out.headerSource, "FFOnReady"));
}

// ===== RPC functions: _Implementation suffix ===============================
//
// UFUNCTION(Server) / UFUNCTION(Client) / UFUNCTION(NetMulticast) all
// follow the same pattern in UE C++: the header decl uses the bare
// function name, and the impl renames to <FnName>_Implementation. UHT
// generates the dispatch wrapper that calls _Implementation when the
// RPC fires.

TEST_CASE("EmitCppClass: Server RPC renames impl to <FnName>_Implementation") {
    auto cls = MakeMinimalClass();
    cls["functions"] = json::array({
        json{
            {"version", 1}, {"kind", "function"}, {"name", "ServerDoThing"},
            {"metadata", json{
                {"ufunction_specifiers", json::array({"Server", "Reliable"})},
            }},
            {"body", json::array()},
        },
    });
    auto out = EmitCppClass(cls);
    // Header keeps the bare name with the Server specifier.
    CHECK(Contains(out.headerSource, "UFUNCTION(Server, Reliable)"));
    CHECK(Contains(out.headerSource, "void ServerDoThing("));
    // Impl gets _Implementation.
    CHECK(Contains(out.implSource, "::ServerDoThing_Implementation("));
    // No accidental _Implementation in the header decl.
    CHECK_FALSE(Contains(out.headerSource, "ServerDoThing_Implementation"));
}

TEST_CASE("EmitCppClass: Client RPC renames impl to <FnName>_Implementation") {
    auto cls = MakeMinimalClass();
    cls["functions"] = json::array({
        json{
            {"version", 1}, {"kind", "function"}, {"name", "ClientNotify"},
            {"metadata", json{
                {"ufunction_specifiers", json::array({"Client"})},
            }},
            {"body", json::array()},
        },
    });
    auto out = EmitCppClass(cls);
    CHECK(Contains(out.implSource, "::ClientNotify_Implementation("));
}

TEST_CASE("EmitCppClass: NetMulticast RPC renames impl to <FnName>_Implementation") {
    auto cls = MakeMinimalClass();
    cls["functions"] = json::array({
        json{
            {"version", 1}, {"kind", "function"}, {"name", "BroadcastEffect"},
            {"metadata", json{
                {"ufunction_specifiers", json::array({"NetMulticast", "Unreliable"})},
            }},
            {"body", json::array()},
        },
    });
    auto out = EmitCppClass(cls);
    CHECK(Contains(out.implSource, "::BroadcastEffect_Implementation("));
}

TEST_CASE("EmitCppClass: non-RPC functions keep bare impl name") {
    auto cls = MakeMinimalClass();
    cls["functions"] = json::array({
        json{
            {"version", 1}, {"kind", "function"}, {"name", "RegularFn"},
            {"body", json::array()},
        },
    });
    auto out = EmitCppClass(cls);
    CHECK(Contains(out.implSource, "::RegularFn("));
    CHECK_FALSE(Contains(out.implSource, "RegularFn_Implementation"));
}

TEST_CASE("EmitCppClass: BlueprintAuthorityOnly is NOT treated as an RPC marker") {
    // BlueprintAuthorityOnly is a BP-side specifier, not an actual
    // RPC. UHT doesn't generate an _Implementation dispatch for it.
    auto cls = MakeMinimalClass();
    cls["functions"] = json::array({
        json{
            {"version", 1}, {"kind", "function"}, {"name", "AuthOnly"},
            {"metadata", json{
                {"ufunction_specifiers", json::array({"BlueprintAuthorityOnly"})},
            }},
            {"body", json::array()},
        },
    });
    auto out = EmitCppClass(cls);
    CHECK(Contains(out.implSource, "::AuthOnly("));
    CHECK_FALSE(Contains(out.implSource, "AuthOnly_Implementation"));
}

// ===== SCS components ======================================================
//
// Angelscript-validated. BP's Components panel (the SCS hierarchy) is a
// separate stream from BP variables. C++ binding pattern: each component
// gets a UPROPERTY field on the class + a CreateDefaultSubobject call
// in the constructor + SetupAttachment to wire the hierarchy.

TEST_CASE("EmitCppClass: components emit UPROPERTY + CreateDefaultSubobject in ctor") {
    auto cls = MakeMinimalClass("AActor");
    cls["components"] = json::array({
        json{{"name", "Root"},
             {"class", "/Script/Engine.SceneComponent"},
             {"is_root", true}},
        json{{"name", "Mesh"},
             {"class", "/Script/Engine.StaticMeshComponent"},
             {"parent", "Root"}},
    });
    auto out = EmitCppClass(cls);
    // UPROPERTY decls.
    CHECK(Contains(out.headerSource, "TObjectPtr<USceneComponent> Root;"));
    CHECK(Contains(out.headerSource, "TObjectPtr<UStaticMeshComponent> Mesh;"));
    CHECK(Contains(out.headerSource, R"(Category="Components")"));
    // Forward decls.
    CHECK(Contains(out.headerSource, "class USceneComponent;"));
    CHECK(Contains(out.headerSource, "class UStaticMeshComponent;"));
    // Constructor body.
    CHECK(Contains(out.implSource,
        R"(Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root")))"));
    CHECK(Contains(out.implSource,
        R"(Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh")))"));
    CHECK(Contains(out.implSource, "Mesh->SetupAttachment(Root);"));
    CHECK(Contains(out.implSource, "RootComponent = Root;"));
}

TEST_CASE("EmitCppClass: component without explicit parent attaches to root") {
    auto cls = MakeMinimalClass("AActor");
    cls["components"] = json::array({
        json{{"name", "Root"}, {"class", "/Script/Engine.SceneComponent"},
             {"is_root", true}},
        json{{"name", "Floater"}, {"class", "/Script/Engine.StaticMeshComponent"}},
    });
    auto out = EmitCppClass(cls);
    // Floater has no explicit parent -> attaches to Root.
    CHECK(Contains(out.implSource, "Floater->SetupAttachment(Root);"));
}

TEST_CASE("EmitCppClass: no components -> no ctor / no SCS scaffolding") {
    auto cls = MakeMinimalClass("AActor");
    auto out = EmitCppClass(cls);
    CHECK_FALSE(Contains(out.implSource, "CreateDefaultSubobject"));
    CHECK_FALSE(Contains(out.implSource, "RootComponent"));
}

TEST_CASE("EmitCppClass: components + replication share one ctor") {
    auto cls = MakeMinimalClass("AActor");
    cls["components"] = json::array({
        json{{"name", "Root"}, {"class", "/Script/Engine.SceneComponent"},
             {"is_root", true}},
    });
    cls["variables"] = json::array({
        json{{"name", "Hp"}, {"type", "float"}, {"replicated", true}},
    });
    auto out = EmitCppClass(cls);
    // Both behaviors in one ctor body.
    CHECK(Contains(out.implSource, "Root = CreateDefaultSubobject"));
    CHECK(Contains(out.implSource, "bReplicates = true;"));
    // Only one ctor declared in the header.
    auto countCtor = [&](const std::string& s, const std::string& sub) {
        std::size_t count = 0;
        for (std::size_t pos = 0; (pos = s.find(sub, pos)) != std::string::npos; ++pos) {
            ++count;
        }
        return count;
    };
    // Header ctor decl `: public AActor {` followed by `    AMyClass();`
    // -- check the header ctor decl appears exactly once.
    std::string ctorDecl = "    " + out.className + "();";
    CHECK(countCtor(out.headerSource, ctorDecl) == 1);
}

TEST_CASE("EmitCppClass: BP-class component types get _C suffix stripped") {
    auto cls = MakeMinimalClass("AActor");
    cls["components"] = json::array({
        json{{"name", "Custom"},
             {"class", "/Game/Components/BPC_Custom.BPC_Custom_C"},
             {"is_root", true}},
    });
    auto out = EmitCppClass(cls);
    CHECK(Contains(out.headerSource, "TObjectPtr<UBPC_Custom>"));
    // The _C suffix shouldn't leak into the component type specifically.
    // (Avoid bare "_C" check -- GENERATED_BODY etc. legitimately contain it.)
    CHECK_FALSE(Contains(out.headerSource, "BPC_Custom_C"));
    CHECK_FALSE(Contains(out.headerSource, "TObjectPtr<UBPC_Custom_C>"));
}

// ===== SCS component default property values ==============================
//
// Each SCS subobject carries property overrides authored in the BP
// Components panel (RelativeLocation, mesh asset, ScaleFactor, etc.).
// The plugin's introspector surfaces them as
// {name, type, value} entries in the component's `properties` array;
// CppClassEmit emits matching `Comp->Property = X;` lines in the ctor.

TEST_CASE("EmitCppClass: component scalar property -> direct assignment") {
    auto cls = MakeMinimalClass("AActor");
    cls["components"] = json::array({
        json{{"name", "Mesh"},
             {"class", "/Script/Engine.StaticMeshComponent"},
             {"is_root", true},
             {"properties", json::array({
                 json{{"name", "CastShadow"}, {"type", "BoolProperty"}, {"value", "false"}},
                 json{{"name", "Mobility"},   {"type", "ByteProperty"}, {"value", "EComponentMobility::Movable"}},
             })}},
    });
    auto out = EmitCppClass(cls);
    CHECK(Contains(out.implSource, "Mesh->CastShadow = false;"));
    CHECK(Contains(out.implSource, "Mesh->Mobility = EComponentMobility::Movable;"));
}

TEST_CASE("EmitCppClass: component FVector property -> FVector(x, y, z)") {
    auto cls = MakeMinimalClass("AActor");
    cls["components"] = json::array({
        json{{"name", "SpringArm"},
             {"class", "/Script/Engine.SpringArmComponent"},
             {"is_root", true},
             {"properties", json::array({
                 json{{"name", "RelativeLocation"}, {"type", "StructProperty"},
                      {"value", "(X=0.0,Y=0.0,Z=50.0)"}},
             })}},
    });
    auto out = EmitCppClass(cls);
    CHECK(Contains(out.implSource, "SpringArm->RelativeLocation = FVector(0.0f, 0.0f, 50.0f);"));
}

TEST_CASE("EmitCppClass: component FRotator property -> FRotator(p, y, r)") {
    auto cls = MakeMinimalClass("AActor");
    cls["components"] = json::array({
        json{{"name", "Cam"},
             {"class", "/Script/Engine.CameraComponent"},
             {"is_root", true},
             {"properties", json::array({
                 json{{"name", "RelativeRotation"}, {"type", "StructProperty"},
                      {"value", "(Pitch=-15.0,Yaw=0.0,Roll=0.0)"}},
             })}},
    });
    auto out = EmitCppClass(cls);
    CHECK(Contains(out.implSource, "Cam->RelativeRotation = FRotator(-15.0f, 0.0f, 0.0f);"));
}

TEST_CASE("EmitCppClass: component float property -> float-literal") {
    auto cls = MakeMinimalClass("AActor");
    cls["components"] = json::array({
        json{{"name", "Spring"},
             {"class", "/Script/Engine.SpringArmComponent"},
             {"is_root", true},
             {"properties", json::array({
                 json{{"name", "TargetArmLength"}, {"type", "FloatProperty"}, {"value", "300.0"}},
             })}},
    });
    auto out = EmitCppClass(cls);
    CHECK(Contains(out.implSource, "Spring->TargetArmLength = 300.0f;"));
}

TEST_CASE("EmitCppClass: component asset-ref property emits FObjectFinder TODO + sidecar note") {
    auto cls = MakeMinimalClass("AActor");
    cls["components"] = json::array({
        json{{"name", "SK"},
             {"class", "/Script/Engine.SkeletalMeshComponent"},
             {"is_root", true},
             {"properties", json::array({
                 json{{"name", "SkeletalMesh"}, {"type", "ObjectProperty"},
                      {"value", "/Game/Mannequin/Character/Mesh/SK_Mannequin.SK_Mannequin"}},
             })}},
    });
    auto out = EmitCppClass(cls);
    CHECK(Contains(out.implSource, "TODO[bpr-asset-ref]"));
    CHECK(Contains(out.implSource, "ConstructorHelpers::FObjectFinder"));
    CHECK(Contains(out.implSource, "/Game/Mannequin/Character/Mesh/SK_Mannequin"));
    // Sidecar entry.
    bool found = false;
    for (const auto& n : out.notes) {
        if (n.value("treatment", "") == "asset_ref_objectfinder_stub" &&
            n.value("component", "") == "SK") {
            found = true;
        }
    }
    CHECK(found);
}

TEST_CASE("EmitCppClass: component property of unknown type emits TODO") {
    auto cls = MakeMinimalClass("AActor");
    cls["components"] = json::array({
        json{{"name", "X"},
             {"class", "/Script/Engine.SceneComponent"},
             {"is_root", true},
             {"properties", json::array({
                 json{{"name", "Weird"}, {"type", "MapProperty"}, {"value", "(a=1,b=2)"}},
             })}},
    });
    auto out = EmitCppClass(cls);
    CHECK(Contains(out.implSource, "TODO[bpr-component-default]"));
    CHECK(Contains(out.implSource, "Weird"));
    bool found = false;
    for (const auto& n : out.notes) {
        if (n.value("treatment", "") == "todo_unsupported_type") found = true;
    }
    CHECK(found);
}

// ===== Class-level identifier sanitization (bug-hunt pass 2) ==============
//
// Class-level emitters (RenderUPropertyDecl, RenderUFunctionDecl,
// RenderUFunctionImpl) previously didn't sanitize names. A BP variable
// or function name with spaces ("Selected Plot", "Apply Healing") would
// leak as `float Selected Plot;` / `void Apply Healing();` which don't
// compile.

TEST_CASE("EmitCppClass: variable name with spaces sanitizes at UPROPERTY emission") {
    auto cls = MakeMinimalClass();
    cls["variables"] = json::array({
        json{{"name", "Selected Plot"}, {"type", "object:Actor"}, {"editable", true}},
    });
    auto out = EmitCppClass(cls);
    CHECK(Contains(out.headerSource, "SelectedPlot"));
    CHECK_FALSE(Contains(out.headerSource, "Selected Plot;"));
}

TEST_CASE("EmitCppClass: function name with spaces sanitizes at decl + impl") {
    auto cls = MakeMinimalClass();
    cls["functions"] = json::array({
        json{
            {"version", 1}, {"kind", "function"}, {"name", "Apply Healing"},
            {"inputs", json::array({json{{"name", "Heal Amount"}, {"type", "float"}}})},
            {"body", json::array()},
        },
    });
    auto out = EmitCppClass(cls);
    // Header decl + impl both use the sanitized form.
    CHECK(Contains(out.headerSource, "ApplyHealing"));
    CHECK(Contains(out.headerSource, "HealAmount"));
    CHECK_FALSE(Contains(out.headerSource, "Apply Healing"));
    CHECK(Contains(out.implSource, "::ApplyHealing("));
    CHECK_FALSE(Contains(out.implSource, "Apply Healing"));
}

// ===== TEXT() escaping (bug-hunt pass 18) =================================
//
// BP string / name defaults with embedded `"` or `\` previously leaked
// into the output verbatim, producing un-compileable TEXT() literals.

TEST_CASE("EmitCppClass: FString default with embedded double-quote is escaped") {
    auto cls = MakeMinimalClass();
    cls["variables"] = json::array({
        json{{"name","Greeting"}, {"type","string"}, {"editable", true},
             {"default", "Hello \"world\""}},
    });
    auto out = EmitCppClass(cls);
    CHECK(Contains(out.headerSource, R"(TEXT("Hello \"world\""))"));
    // The unescaped form would be `TEXT("Hello "world"")` -- check absent.
    CHECK_FALSE(Contains(out.headerSource, R"(TEXT("Hello "world""))"));
}

TEST_CASE("EmitCppClass: FString default with backslash escapes both") {
    auto cls = MakeMinimalClass();
    cls["variables"] = json::array({
        json{{"name","Path"}, {"type","string"}, {"editable", true},
             {"default", "C:\\Path\\file"}},
    });
    auto out = EmitCppClass(cls);
    CHECK(Contains(out.headerSource, R"(TEXT("C:\\Path\\file"))"));
}

// ===== ConstructionScript -> AActor::OnConstruction override ==============
//
// BP exposes the construction script under "ConstructionScript" or
// "UserConstructionScript"; the matching C++ override is
// AActor::OnConstruction(const FTransform&). The BP-side function NAME
// doesn't match the C++ override NAME, so the rewriter has to translate
// at emission rather than just override-tagging.

TEST_CASE("EmitCppClass: ConstructionScript function emits as OnConstruction override") {
    auto cls = MakeMinimalClass();
    cls["functions"] = json::array({
        json{
            {"version", 1}, {"kind", "function"}, {"name", "ConstructionScript"},
            {"body", json::array()},
        },
    });
    auto out = EmitCppClass(cls);
    CHECK(Contains(out.headerSource,
        "virtual void OnConstruction(const FTransform& Transform) override;"));
    CHECK_FALSE(Contains(out.headerSource, "void ConstructionScript("));
    CHECK(Contains(out.implSource, "::OnConstruction(const FTransform& Transform)"));
    CHECK_FALSE(Contains(out.implSource, "::ConstructionScript"));
}

// ===== Delegate signature param threading =================================
//
// When the plugin introspector surfaces delegate_params on a multicast
// delegate variable, CppClassEmit emits the matching _NParams variant
// of DECLARE_DYNAMIC_MULTICAST_DELEGATE.

TEST_CASE("EmitCppClass: delegate with one param emits _OneParam variant") {
    auto cls = MakeMinimalClass();
    cls["variables"] = json::array({
        json{{"name", "OnPickup"}, {"type", "mcdelegate"},
             {"delegate_params", json::array({
                 json{{"name", "Item"}, {"type", "object:Actor"}},
             })}},
    });
    auto out = EmitCppClass(cls);
    CHECK(Contains(out.headerSource,
        "DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnPickup, AActor*, Item);"));
    bool found = false;
    for (const auto& n : out.notes) {
        if (n.value("treatment", "") == "delegate_typedef_resolved" &&
            n.value("param_count", 0)  == 1) found = true;
    }
    CHECK(found);
}

TEST_CASE("EmitCppClass: delegate with two params emits _TwoParams variant") {
    auto cls = MakeMinimalClass();
    cls["variables"] = json::array({
        json{{"name", "OnDamaged"}, {"type", "mcdelegate"},
             {"delegate_params", json::array({
                 json{{"name", "Amount"},   {"type", "float"}},
                 json{{"name", "Attacker"}, {"type", "object:Actor"}},
             })}},
    });
    auto out = EmitCppClass(cls);
    CHECK(Contains(out.headerSource, "DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnDamaged, "));
    CHECK(Contains(out.headerSource, "float, Amount"));
    CHECK(Contains(out.headerSource, "AActor*, Attacker"));
}

TEST_CASE("EmitCppClass: delegate with empty delegate_params keeps zero-arg DECLARE") {
    auto cls = MakeMinimalClass();
    cls["variables"] = json::array({
        json{{"name", "OnReady"}, {"type", "mcdelegate"}},
    });
    auto out = EmitCppClass(cls);
    CHECK(Contains(out.headerSource, "DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnReady);"));
    CHECK(Contains(out.headerSource, "TODO[bpr-delegate-signature]"));
}

TEST_CASE("EmitCppClass: delegate with sanitized param name") {
    auto cls = MakeMinimalClass();
    cls["variables"] = json::array({
        json{{"name", "OnFoo"}, {"type", "mcdelegate"},
             {"delegate_params", json::array({
                 json{{"name", "Bad Name"}, {"type", "int"}},
             })}},
    });
    auto out = EmitCppClass(cls);
    CHECK(Contains(out.headerSource, "DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnFoo, int32, BadName);"));
    CHECK_FALSE(Contains(out.headerSource, "Bad Name"));
}

// ===== Asset-ref property_class threading =================================

TEST_CASE("EmitCppClass: component asset-ref with property_class fills in FObjectFinder<T>") {
    auto cls = MakeMinimalClass("AActor");
    cls["components"] = json::array({
        json{{"name", "SK"},
             {"class", "/Script/Engine.SkeletalMeshComponent"},
             {"is_root", true},
             {"properties", json::array({
                 json{{"name", "SkeletalMesh"},
                      {"type", "ObjectProperty"},
                      {"value", "/Game/Mannequin/Character/Mesh/SK_Mannequin.SK_Mannequin"},
                      {"property_class", "USkeletalMesh"}},
             })}},
    });
    auto out = EmitCppClass(cls);
    // Compileable: FObjectFinder<USkeletalMesh> instead of FObjectFinder<T>.
    CHECK(Contains(out.implSource,
        "ConstructorHelpers::FObjectFinder<USkeletalMesh> SkeletalMeshFinder"));
    CHECK(Contains(out.implSource, "if (SkeletalMeshFinder.Succeeded())"));
    CHECK(Contains(out.implSource, "SK->SkeletalMesh = SkeletalMeshFinder.Object"));
    // No TODO when class is resolved.
    CHECK_FALSE(Contains(out.implSource, "TODO[bpr-asset-ref]"));
    // Sidecar note carries the resolved class.
    bool found = false;
    for (const auto& n : out.notes) {
        if (n.value("treatment", "") == "asset_ref_objectfinder_resolved" &&
            n.value("asset_class", "") == "USkeletalMesh") found = true;
    }
    CHECK(found);
}

TEST_CASE("EmitCppClass: component asset-ref without property_class still emits FObjectFinder with T placeholder + TODO") {
    auto cls = MakeMinimalClass("AActor");
    cls["components"] = json::array({
        json{{"name", "SK"},
             {"class", "/Script/Engine.SkeletalMeshComponent"},
             {"is_root", true},
             {"properties", json::array({
                 json{{"name", "SkeletalMesh"},
                      {"type", "ObjectProperty"},
                      {"value", "/Game/Mannequin/Character/Mesh/SK_Mannequin.SK_Mannequin"}},
             })}},
    });
    auto out = EmitCppClass(cls);
    // Compileable shell, but T needs to be replaced.
    CHECK(Contains(out.implSource, "ConstructorHelpers::FObjectFinder<T>"));
    CHECK(Contains(out.implSource, "TODO[bpr-asset-ref]"));
}

TEST_CASE("EmitCppClass: UserConstructionScript alias also routes to OnConstruction") {
    auto cls = MakeMinimalClass();
    cls["functions"] = json::array({
        json{
            {"version", 1}, {"kind", "function"}, {"name", "UserConstructionScript"},
            {"body", json::array()},
        },
    });
    auto out = EmitCppClass(cls);
    CHECK(Contains(out.headerSource, "virtual void OnConstruction"));
}

TEST_CASE("EmitCppClass: component string property with embedded quote is escaped") {
    auto cls = MakeMinimalClass("AActor");
    cls["components"] = json::array({
        json{{"name", "X"},
             {"class", "/Script/Engine.SceneComponent"},
             {"is_root", true},
             {"properties", json::array({
                 json{{"name", "Tooltip"}, {"type", "StrProperty"},
                      {"value", "Press \"E\" to use"}},
             })}},
    });
    auto out = EmitCppClass(cls);
    CHECK(Contains(out.implSource, R"(TEXT("Press \"E\" to use"))"));
}
