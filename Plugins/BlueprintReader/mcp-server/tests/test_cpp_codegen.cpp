// Snapshot-style tests for BPIR → C++ codegen. Each case constructs a
// small hand-crafted BPIR doc and checks that the rendered C++ contains
// expected fragments. Not full string-equality (whitespace + ordering
// stability isn't worth the test brittleness); substring asserts let
// the codegen evolve without flaky tests.

#include <doctest/doctest.h>

#include "tools/codegen/CppEmit.h"
#include "tools/Bpir.h"

using namespace bpr::tools;
using nlohmann::json;

namespace {

// Wrap a body of statements as a minimal BPIR function doc.
json MakeFn(const json& body, std::vector<json> inputs = {},
            std::vector<json> outputs = {}) {
    json doc = {
        {"version", 1},
        {"kind", "function"},
        {"name", "TestFn"},
        {"body", body},
    };
    if (!inputs.empty())  doc["inputs"]  = inputs;
    if (!outputs.empty()) doc["outputs"] = outputs;
    return doc;
}

bool Contains(const std::string& s, const std::string& sub) {
    return s.find(sub) != std::string::npos;
}

} // namespace

// ===== Type mapping ========================================================

TEST_CASE("MapBpirTypeToCpp: scalars") {
    CHECK(MapBpirTypeToCpp("bool")   == "bool");
    CHECK(MapBpirTypeToCpp("int")    == "int32");
    CHECK(MapBpirTypeToCpp("int64")  == "int64");
    CHECK(MapBpirTypeToCpp("byte")   == "uint8");
    CHECK(MapBpirTypeToCpp("float")  == "float");
    CHECK(MapBpirTypeToCpp("double") == "double");
    CHECK(MapBpirTypeToCpp("string") == "FString");
    CHECK(MapBpirTypeToCpp("name")   == "FName");
    CHECK(MapBpirTypeToCpp("text")   == "FText");
}

TEST_CASE("MapBpirTypeToCpp: object refs use proper U/A prefix") {
    CHECK(MapBpirTypeToCpp("object:Actor")   == "AActor*");
    CHECK(MapBpirTypeToCpp("object:Pawn")    == "APawn*");
    CHECK(MapBpirTypeToCpp("object:UObject") == "UObject*");
    CHECK(MapBpirTypeToCpp("object:Widget")  == "UWidget*");
    // Already-prefixed names pass through.
    CHECK(MapBpirTypeToCpp("object:AActor")  == "AActor*");
}

TEST_CASE("MapBpirTypeToCpp: object refs with full /Script/ path strip path prefix") {
    // Caught live: BP variables sometimes serialize SubCategoryObject as
    // the canonical UE path. The mapper should strip /Script/Module. and
    // the trailing _C if present.
    CHECK(MapBpirTypeToCpp("object:/Script/Engine.Actor")  == "AActor*");
    CHECK(MapBpirTypeToCpp("object:/Script/Engine.Pawn")   == "APawn*");
    CHECK(MapBpirTypeToCpp("object:/Script/Engine.Object") == "UObject*");
    CHECK(MapBpirTypeToCpp("object:/Game/AI/BP_Boss.BP_Boss_C") == "UBP_Boss*");
}

TEST_CASE("MapBpirTypeToCpp: structs") {
    CHECK(MapBpirTypeToCpp("struct:Vector")  == "FVector");
    CHECK(MapBpirTypeToCpp("struct:FVector") == "FVector");
    CHECK(MapBpirTypeToCpp("struct:Transform") == "FTransform");
}

TEST_CASE("MapBpirTypeToCpp: containers") {
    CHECK(MapBpirTypeToCpp("[]float")     == "TArray<float>");
    CHECK(MapBpirTypeToCpp("[]int")       == "TArray<int32>");
    CHECK(MapBpirTypeToCpp("[]object:Actor") == "TArray<AActor*>");
    CHECK(MapBpirTypeToCpp("{}int")       == "TSet<int32>");
    CHECK(MapBpirTypeToCpp("{string:int}")== "TMap<FString, int32>");
}

TEST_CASE("MapBpirTypeToCpp: class + interface refs") {
    CHECK(MapBpirTypeToCpp("class:Actor")       == "TSubclassOf<Actor>");
    CHECK(MapBpirTypeToCpp("interface:Damageable") == "TScriptInterface<IDamageable>");
}

// ===== Statement codegen ===================================================

TEST_CASE("Codegen: empty function emits valid block") {
    auto out = EmitCppFunction(MakeFn(json::array()));
    CHECK(Contains(out.source, "void TestFn() {"));
    CHECK(Contains(out.source, "}"));
}

TEST_CASE("Codegen: simple `set` statement") {
    auto out = EmitCppFunctionBody(MakeFn(json::array({
        json{{"set", "Health"}, {"to", json{{"lit", 100}}}}
    })));
    CHECK(Contains(out.source, "Health = 100;"));
}

TEST_CASE("Codegen: if/then/else") {
    auto out = EmitCppFunctionBody(MakeFn(json::array({
        json{{"if", json{{"var", "bAlive"}}},
             {"then", json::array({json{{"call","Foo"}}})},
             {"else", json::array({json{{"call","Bar"}}})}},
    })));
    CHECK(Contains(out.source, "if (bAlive) {"));
    CHECK(Contains(out.source, "Foo();"));
    CHECK(Contains(out.source, "} else {"));
    CHECK(Contains(out.source, "Bar();"));
}

TEST_CASE("Codegen: return with single value") {
    auto out = EmitCppFunctionBody(MakeFn(json::array({
        json{{"return", json{{"var","Health"}}}}
    })));
    CHECK(Contains(out.source, "return Health;"));
}

TEST_CASE("Codegen: bare return") {
    auto out = EmitCppFunctionBody(MakeFn(json::array({
        json{{"return", nullptr}}
    })));
    CHECK(Contains(out.source, "return;"));
}

TEST_CASE("Codegen: for_each") {
    auto out = EmitCppFunctionBody(MakeFn(json::array({
        json{{"for_each", "Item"},
             {"in",  json{{"var","Items"}}},
             {"body", json::array({json{{"call","UseItem"}}})}}
    })));
    CHECK(Contains(out.source, "for (auto& Item : Items) {"));
    CHECK(Contains(out.source, "UseItem();"));
}

TEST_CASE("Codegen: cast statement") {
    auto out = EmitCppFunctionBody(MakeFn(json::array({
        json{{"cast", json{{"var","Other"}}},
             {"to", "APawn"}, {"as", "AsPawn"},
             {"success", json::array({json{{"call","Hit"}}})},
             {"fail",    json::array({json{{"call","Miss"}}})}}
    })));
    CHECK(Contains(out.source, "if (auto* AsPawn = Cast<APawn>(Other)) {"));
    CHECK(Contains(out.source, "Hit();"));
    CHECK(Contains(out.source, "} else {"));
    CHECK(Contains(out.source, "Miss();"));
}

TEST_CASE("Codegen: switch") {
    auto out = EmitCppFunctionBody(MakeFn(json::array({
        json{{"switch", json{{"var","Mode"}}},
             {"cases", json{
                 {"0", json::array({json{{"call","ModeA"}}})},
                 {"1", json::array({json{{"call","ModeB"}}})},
             }},
             {"default", json::array({json{{"call","ModeOther"}}})}}
    })));
    CHECK(Contains(out.source, "switch (Mode) {"));
    CHECK(Contains(out.source, "case 0:"));
    CHECK(Contains(out.source, "case 1:"));
    CHECK(Contains(out.source, "default:"));
}

TEST_CASE("Codegen: unsupported emits TODO comment + populates notes") {
    auto out = EmitCppFunctionBody(MakeFn(json::array({
        json{{"unsupported", json{
            {"node_class","K2Node_Timeline"},
            {"guid","abc-123"},
            {"reason","no C++ equivalent"}
        }}}
    })));
    CHECK(Contains(out.source, "TODO[bpr-unsupported]"));
    CHECK(Contains(out.source, "K2Node_Timeline"));
    REQUIRE(out.notes.size() == 1);
    CHECK(out.notes[0]["node_class"] == "K2Node_Timeline");
    CHECK(out.notes[0]["treatment"] == "todo_comment");
}

// ===== Sentinel-call lowering =============================================
//
// Decompile recognizes a few K2 nodes (SpawnActorFromClass,
// AddComponent) as structured BPIR calls — sentinel-named so CppEmit
// can render them as real UE-side syntax instead of routing them
// through the unsupported-node TODO path. These tests pin the
// rendering for the patterns the user gets when transpiling those
// node classes.

TEST_CASE("Codegen: __bpr_spawn_actor_from_class without optional pins → simple SpawnActor<T>") {
    auto out = EmitCppFunctionBody(MakeFn(json::array({
        json{{"set", "Spawned"}, {"to",
             json{{"call", "__bpr_spawn_actor_from_class"},
                  {"args", json{
                      {"Class", json{{"var", "EnemyClass"}}},
                      {"SpawnTransform", json{{"var", "Xform"}}}}}}}}
    })));
    CHECK(Contains(out.source, "GetWorld()->SpawnActor<AActor>(EnemyClass, Xform)"));
    // No TODO — we have the values we need.
    CHECK_FALSE(Contains(out.source, "TODO[bpr-spawn]"));
}

TEST_CASE("Codegen: __bpr_spawn_actor_from_class with Owner → wraps in FActorSpawnParameters") {
    auto out = EmitCppFunctionBody(MakeFn(json::array({
        json{{"set", "Spawned"}, {"to",
             json{{"call", "__bpr_spawn_actor_from_class"},
                  {"args", json{
                      {"Class", json{{"var", "EnemyClass"}}},
                      {"SpawnTransform", json{{"var", "Xform"}}},
                      {"Owner", json{{"var", "this_actor"}}}}}}}}
    })));
    CHECK(Contains(out.source, "FActorSpawnParameters p;"));
    CHECK(Contains(out.source, "p.Owner = this_actor;"));
    CHECK(Contains(out.source, "GetWorld()->SpawnActor<AActor>(EnemyClass, Xform, p)"));
}

TEST_CASE("Codegen: __bpr_add_component renders NewObject + RegisterComponent block") {
    auto out = EmitCppFunctionBody(MakeFn(json::array({
        json{{"set", "Comp"}, {"to",
             json{{"call", "__bpr_add_component"},
                  {"args", json{
                      {"TemplateName", json{{"var", "MyComponentName"}}},
                      {"RelativeTransform", json{{"var", "Xform"}}}}}}}}
    })));
    CHECK(Contains(out.source, "NewObject<UActorComponent>(this, MyComponentName)"));
    CHECK(Contains(out.source, "Scene->SetRelativeTransform(Xform)"));
    CHECK(Contains(out.source, "Comp->RegisterComponent()"));
    CHECK_FALSE(Contains(out.source, "TODO[bpr-component]"));
}

// ===== Expression codegen ==================================================

TEST_CASE("Codegen: operator alias renders as infix") {
    auto out = EmitCppFunctionBody(MakeFn(json::array({
        json{{"set", "Sum"}, {"to",
             json{{"call","KismetMathLibrary::Add_IntInt"},
                  {"args", json{{"A", json{{"var","X"}}},
                                {"B", json{{"var","Y"}}}}}}}}
    })));
    CHECK(Contains(out.source, "Sum = (X + Y);"));
}

TEST_CASE("Codegen: operator alias DISABLED renders as canonical call") {
    CppEmitOptions opts;
    opts.useOperatorAliases = false;
    auto out = EmitCppFunctionBody(MakeFn(json::array({
        json{{"set", "Sum"}, {"to",
             json{{"call","KismetMathLibrary::Add_IntInt"},
                  {"args", json{{"A", json{{"var","X"}}},
                                {"B", json{{"var","Y"}}}}}}}}
    })), opts);
    CHECK(Contains(out.source, "KismetMathLibrary::Add_IntInt("));
}

TEST_CASE("Codegen: literal ints stay int, floats get f-suffix") {
    auto outI = EmitCppFunctionBody(MakeFn(json::array({
        json{{"set","X"},{"to",json{{"lit",42}}}}
    })));
    CHECK(Contains(outI.source, "X = 42;"));

    auto outF = EmitCppFunctionBody(MakeFn(json::array({
        json{{"set","X"},{"to",json{{"lit",3.14}}}}
    })));
    CHECK(Contains(outF.source, "f"));  // float literal suffix
}

TEST_CASE("Codegen: bool / null literals") {
    auto outB = EmitCppFunctionBody(MakeFn(json::array({
        json{{"set","X"},{"to",json{{"lit",true}}}}
    })));
    CHECK(Contains(outB.source, "X = true;"));

    auto outN = EmitCppFunctionBody(MakeFn(json::array({
        json{{"set","X"},{"to",json{{"lit",nullptr}}}}
    })));
    CHECK(Contains(outN.source, "X = nullptr;"));
}

TEST_CASE("Codegen: self / member / index expressions") {
    auto out = EmitCppFunctionBody(MakeFn(json::array({
        json{{"set","Loc"},{"to",
             json{{"member", json{{"self", nullptr}}}, {"name","RootComp"}}}},
        json{{"set","First"},{"to",
             json{{"index", json{{"var","Items"}}}, {"idx", json{{"lit",0}}}}}},
    })));
    CHECK(Contains(out.source, "Loc = this.RootComp;"));
    CHECK(Contains(out.source, "First = Items[0];"));
}

// ===== Function signature ==================================================

TEST_CASE("Codegen: signature emits typed inputs") {
    auto out = EmitCppFunction(MakeFn(json::array(),
        {json{{"name","Amount"},{"type","float"}},
         json{{"name","Source"},{"type","object:Actor"}}}));
    CHECK(Contains(out.source, "TestFn(float Amount, AActor* Source)"));
}

TEST_CASE("Codegen: single output becomes return type") {
    auto out = EmitCppFunction(MakeFn(json::array({
        json{{"return", json{{"var","Killed"}}}}
    }), {}, {json{{"name","Killed"},{"type","bool"}}}));
    CHECK(Contains(out.source, "bool TestFn()"));
    CHECK(Contains(out.source, "return Killed;"));
}

TEST_CASE("Codegen: multi-output becomes out-ref params") {
    auto out = EmitCppFunction(MakeFn(json::array(), {},
        {json{{"name","A"},{"type","int"}},
         json{{"name","B"},{"type","float"}}}));
    CHECK(Contains(out.source, "int32& A"));
    CHECK(Contains(out.source, "float& B"));
}

// ===== Destroy-actor sentinel =============================================

TEST_CASE("Codegen: __bpr_destroy_actor with Target → Target->Destroy()") {
    auto out = EmitCppFunctionBody(MakeFn(json::array({
        json{{"call","__bpr_destroy_actor"},
             {"args", json{{"Target", json{{"var","Enemy"}}}}}},
    })));
    CHECK(Contains(out.source, "Enemy->Destroy();"));
}

TEST_CASE("Codegen: __bpr_destroy_actor without Target → this->Destroy()") {
    auto out = EmitCppFunctionBody(MakeFn(json::array({
        json{{"call","__bpr_destroy_actor"}},
    })));
    CHECK(Contains(out.source, "this->Destroy();"));
}

// ===== ConstructObjectFromClass sentinel ==================================

TEST_CASE("Codegen: __bpr_construct_object_from_class → NewObject<UObject>(Outer, Class)") {
    auto out = EmitCppFunctionBody(MakeFn(json::array({
        json{{"set","Spawned"},
             {"to",   json{{"call","__bpr_construct_object_from_class"},
                            {"args", json{{"Class", json{{"var","MyClass"}}},
                                          {"Outer", json{{"var","Owner"}}}}}}}},
    })));
    CHECK(Contains(out.source, "Spawned = NewObject<UObject>(Owner, MyClass)"));
}

TEST_CASE("Codegen: __bpr_construct_object_from_class without Outer → defaults to `this`") {
    auto out = EmitCppFunctionBody(MakeFn(json::array({
        json{{"set","Spawned"},
             {"to",   json{{"call","__bpr_construct_object_from_class"},
                            {"args", json{{"Class", json{{"var","MyClass"}}}}}}}},
    })));
    CHECK(Contains(out.source, "NewObject<UObject>(this, MyClass)"));
}

// ===== GetDataTableRow sentinel ===========================================

TEST_CASE("Codegen: __bpr_get_data_table_row with row_struct → FindRow<F...> + branches") {
    auto out = EmitCppFunctionBody(MakeFn(json::array({
        json{{"call","__bpr_get_data_table_row"},
             {"args", json{{"DataTable", json{{"var","ItemTable"}}},
                            {"RowName",   json{{"lit", "Sword01"}}}}},
             {"row_struct", "ItemRow"},
             {"success", json::array({
                 json{{"set","Item"}, {"to", json{{"var","Row"}}}}
             })},
             {"fail", json::array({
                 json{{"return", nullptr}}
             })}}
    })));
    // FindRow call with right template + RowName arg + context string.
    CHECK(Contains(out.source, "ItemTable->FindRow<FItemRow>"));
    CHECK(Contains(out.source, R"(TEXT("Sword01"))"));
    CHECK(Contains(out.source, R"(TEXT("BPR"))"));
    // Both branches.
    CHECK(Contains(out.source, "Item = Row;"));
    CHECK(Contains(out.source, "else"));
    CHECK(Contains(out.source, "return;"));
}

TEST_CASE("Codegen: __bpr_get_data_table_row without row_struct → falls back to FTableRowBase") {
    auto out = EmitCppFunctionBody(MakeFn(json::array({
        json{{"call","__bpr_get_data_table_row"},
             {"args", json{{"DataTable", json{{"var","T"}}},
                            {"RowName",   json{{"lit", "X"}}}}},
             {"success", json::array()}, {"fail", json::array()}}
    })));
    CHECK(Contains(out.source, "FindRow<FTableRowBase>"));
}

TEST_CASE("Codegen: __bpr_get_data_table_row strips path + ensures F prefix on row_struct") {
    auto out = EmitCppFunctionBody(MakeFn(json::array({
        json{{"call","__bpr_get_data_table_row"},
             {"args", json{{"DataTable", json{{"var","T"}}},
                            {"RowName",   json{{"lit", "X"}}}}},
             // Path-form + missing F prefix; emitter should normalize.
             {"row_struct", "/Script/Game.ItemRow"},
             {"success", json::array()}, {"fail", json::array()}}
    })));
    CHECK(Contains(out.source, "FindRow<FItemRow>"));
}

// ===== for_each form (ForEachLoop macro) ==================================

TEST_CASE("Codegen: for_each form renders range-based for") {
    auto out = EmitCppFunctionBody(MakeFn(json::array({
        json{{"for_each","Element"},
             {"in",   json{{"var","Enemies"}}},
             {"body", json::array({
                 json{{"call","Element::DoDamage"},
                      {"args", json{{"x", json{{"lit",10}}}}}}
             })}}
    })));
    CHECK(Contains(out.source, "for (auto& Element : Enemies)"));
    CHECK(Contains(out.source, "Element::DoDamage(10);"));
}

// ===== FormatText sentinel =================================================

TEST_CASE("Codegen: __bpr_format_text with args → FFormatNamedArguments + FText::Format") {
    auto out = EmitCppFunctionBody(MakeFn(json::array({
        json{{"set","Greeting"},
             {"to",   json{{"call","__bpr_format_text"},
                            {"format","Hello {Name}!"},
                            {"args", json{{"Name", json{{"var","PlayerName"}}}}}}}}
    })));
    CHECK(Contains(out.source, "FFormatNamedArguments Args;"));
    CHECK(Contains(out.source, "Args.Add(TEXT(\"Name\"), PlayerName);"));
    CHECK(Contains(out.source, "FText::Format("));
    CHECK(Contains(out.source, "NSLOCTEXT"));
    CHECK(Contains(out.source, "Hello {Name}!"));
}

TEST_CASE("Codegen: __bpr_format_text without args → bare FText::FromString") {
    auto out = EmitCppFunctionBody(MakeFn(json::array({
        json{{"set","Greeting"},
             {"to",   json{{"call","__bpr_format_text"},
                            {"format","Hello world"}}}}
    })));
    CHECK(Contains(out.source, "FText::FromString(TEXT(\"Hello world\"))"));
    CHECK_FALSE(Contains(out.source, "FFormatNamedArguments"));
}

// ===== Name aliases =======================================================

TEST_CASE("Codegen: KismetSystemLibrary::IsValid → bare IsValid()") {
    auto out = EmitCppFunctionBody(MakeFn(json::array({
        json{{"if", json{{"call","KismetSystemLibrary::IsValid"},
                          {"args", json{{"Object", json{{"var","Target"}}}}}}},
             {"then", json::array()}, {"else", json::array()}}
    })));
    CHECK(Contains(out.source, "IsValid(Target)"));
    CHECK_FALSE(Contains(out.source, "KismetSystemLibrary::IsValid"));
}

TEST_CASE("Codegen: MakeLiteralXxx identity calls drop to bare literal") {
    auto out = EmitCppFunctionBody(MakeFn(json::array({
        json{{"set","X"},
             {"to", json{{"call","KismetSystemLibrary::MakeLiteralInt"},
                          {"args", json{{"Value", json{{"lit", 42}}}}}}}}
    })));
    // The wrapping call disappears — render the literal directly.
    CHECK(Contains(out.source, "X = 42;"));
    CHECK_FALSE(Contains(out.source, "MakeLiteralInt"));
}

// ===== WorldContext injection =============================================

TEST_CASE("Codegen: PrintString injects `this` as world context") {
    // BP hides the WorldContextObject pin; the C++ signature requires it.
    auto out = EmitCppFunctionBody(MakeFn(json::array({
        json{{"call","KismetSystemLibrary::PrintString"},
             {"args", json{{"InString", json{{"lit","Hello"}}}}}},
    })));
    CHECK(Contains(out.source, "KismetSystemLibrary::PrintString(this, TEXT(\"Hello\"))"));
}

TEST_CASE("Codegen: GetAllActorsOfClass injects `this` as world context") {
    auto out = EmitCppFunctionBody(MakeFn(json::array({
        json{{"call","GameplayStatics::GetAllActorsOfClass"},
             {"args", json{{"ActorClass", json{{"var","Cls"}}},
                            {"OutActors",  json{{"var","Out"}}}}}},
    })));
    CHECK(Contains(out.source, "GetAllActorsOfClass(this, Cls, Out)"));
}

TEST_CASE("Codegen: non-world-context calls don't inject `this`") {
    auto out = EmitCppFunctionBody(MakeFn(json::array({
        json{{"call","MyLib::DoStuff"},
             {"args", json{{"x", json{{"lit",5}}}}}},
    })));
    CHECK(Contains(out.source, "MyLib::DoStuff(5)"));
    CHECK_FALSE(Contains(out.source, "MyLib::DoStuff(this"));
}

// ===== Soft-reference type mapping ========================================

TEST_CASE("Type mapping: soft_object → TSoftObjectPtr") {
    // Soft refs are always TSoftObjectPtr (point is deferred loading
    // + safe cross-package references) — context-independent.
    CHECK(MapBpirTypeToCpp("soft_object:Texture2D") == "TSoftObjectPtr<UTexture2D>");
    CHECK(MapBpirTypeToCppMember("soft_object:Texture2D") == "TSoftObjectPtr<UTexture2D>");
    // Soft actor refs still get the A prefix.
    CHECK(MapBpirTypeToCpp("soft_object:Actor") == "TSoftObjectPtr<AActor>");
    // No subtype → bare UObject.
    CHECK(MapBpirTypeToCpp("soft_object") == "TSoftObjectPtr<UObject>");
}

TEST_CASE("Type mapping: soft_class → TSoftClassPtr") {
    CHECK(MapBpirTypeToCpp("soft_class:Pawn") == "TSoftClassPtr<Pawn>");
    CHECK(MapBpirTypeToCpp("soft_class") == "TSoftClassPtr<UObject>");
}

TEST_CASE("Type mapping: hard class → TSubclassOf (unchanged)") {
    CHECK(MapBpirTypeToCpp("class:Actor") == "TSubclassOf<Actor>");
    CHECK(MapBpirTypeToCpp("class") == "UClass*");
}

// ===== Validation pass-through ============================================

TEST_CASE("Codegen rejects malformed BPIR") {
    json bad = {
        {"version", 1},
        {"kind", "function"},
        {"name", "Bad"},
        {"body", json::array({
            json{{"set", 42}}  // set must be string
        })}
    };
    CHECK_THROWS_AS(EmitCppFunctionBody(bad), std::invalid_argument);
}
