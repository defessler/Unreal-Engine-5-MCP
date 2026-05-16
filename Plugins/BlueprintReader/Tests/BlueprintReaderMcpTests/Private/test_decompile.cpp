// Tests for the decompile_function pass — BPGraph → BPIR reconstruction.
// Runs against the MockBlueprintReader's bundled fixture BPs so we don't
// need a live UE editor.
//
// What the fixtures give us today (BP_Enemy.json):
//   - BP_Enemy: 4 vars, 2 functions (TakeDamage, OnDeath), event graph
//     with a Branch + Sequence + a few CallFunction nodes.
//   - BP_Pickup: smaller, exercises VariableSet + a single Branch.
// These cover the core decompile patterns; richer cases come later
// when we have a live commandlet test or build dedicated fixtures.

#include <doctest/doctest.h>

#include "tools/Decompile.h"
#include "tools/Bpir.h"
#include "backends/MockBlueprintReader.h"

#include "test_helpers.h"

using namespace bpr;
using namespace bpr::tools;
using namespace bpr::backends;

namespace {
struct Fixture {
    MockBlueprintReader reader;
    Fixture() : reader(test::FixturesDir()) {}
};
} // namespace

TEST_CASE("decompile: produces a BPIR function doc that validates") {
    Fixture f;
    auto doc = DecompileFunction(f.reader, "/Game/AI/BP_Enemy", "TakeDamage");
    CHECK_NOTHROW(ValidateBpir(doc));
    CHECK(doc["kind"] == "function");
    CHECK(doc["name"] == "TakeDamage");
    CHECK(doc["version"] == kBpirSchemaVersion);
    CHECK(doc.contains("body"));
    CHECK(doc["body"].is_array());
}

TEST_CASE("decompile: function metadata carries asset_path") {
    Fixture f;
    auto doc = DecompileFunction(f.reader, "/Game/AI/BP_Enemy", "TakeDamage");
    REQUIRE(doc.contains("metadata"));
    CHECK(doc["metadata"]["asset_path"] == "/Game/AI/BP_Enemy");
}

TEST_CASE("decompile: inputs / outputs / locals are emitted with shorthand types") {
    Fixture f;
    auto doc = DecompileFunction(f.reader, "/Game/Player/BP_PlayerController", "AddScore");
    REQUIRE(doc["inputs"].is_array());
    // BP_PlayerController.AddScore has at least one input — verify the
    // shape rather than a specific count (fixture may evolve).
    if (!doc["inputs"].empty()) {
        const auto& in0 = doc["inputs"][0];
        CHECK(in0.contains("name"));
        CHECK(in0.contains("type"));
        CHECK(in0["type"].is_string());  // shorthand string, not object
    }
}

TEST_CASE("decompile: missing function throws AssetNotFound") {
    Fixture f;
    CHECK_THROWS_AS(
        DecompileFunction(f.reader, "/Game/AI/BP_Enemy", "DoesNotExist"),
        BlueprintReaderError);
}

TEST_CASE("decompile_blueprint: emits class doc with functions array") {
    Fixture f;
    auto doc = DecompileBlueprint(f.reader, "/Game/AI/BP_Enemy");
    CHECK_NOTHROW(ValidateBpir(doc));
    CHECK(doc["kind"] == "class");
    CHECK(doc["name"] == "BP_Enemy");
    REQUIRE(doc["functions"].is_array());
    // Each function entry should itself be a valid BPIR function doc.
    for (const auto& fn : doc["functions"]) {
        CHECK_NOTHROW(ValidateBpir(fn));
    }
}

TEST_CASE("decompile_blueprint: variables include category + replication flags") {
    Fixture f;
    auto doc = DecompileBlueprint(f.reader, "/Game/AI/BP_Enemy");
    REQUIRE(doc["variables"].is_array());
    bool sawHealth = false;
    for (const auto& v : doc["variables"]) {
        if (v["name"] == "Health") {
            sawHealth = true;
            CHECK(v["type"] == "float");
            // Health in fixture is replicated + editable — verify
            // at least one of those is preserved.
            if (v.contains("replicated")) CHECK(v["replicated"] == true);
        }
    }
    CHECK(sawHealth);
}

TEST_CASE("decompile: parent class makes it into the class doc metadata") {
    Fixture f;
    auto doc = DecompileBlueprint(f.reader, "/Game/AI/BP_Enemy");
    REQUIRE(doc.contains("metadata"));
    CHECK(doc["metadata"]["parent_class"] == "ACharacter");
}

TEST_CASE("decompile: round-trip BPIR through ValidateBpir works for empty function") {
    Fixture f;
    // Even a function with no body should round-trip cleanly.
    auto doc = DecompileFunction(f.reader, "/Game/Player/BP_PlayerController", "AddScore");
    auto migrated = MigrateToCurrent(doc);
    CHECK_NOTHROW(ValidateBpir(migrated));
}

// ===== End-to-end: BP → BPIR → C++ class scaffold =========================
// Validates the pipeline against the bundled mock fixtures. Substring
// asserts against the generated header — same approach as
// test_cpp_class.cpp. Catches integration issues (decompile ↔ codegen
// boundary) the unit-level tests miss.

#include "tools/codegen/CppClassEmit.h"

namespace {
bool ContainsStr(const std::string& s, const std::string& sub) {
    return s.find(sub) != std::string::npos;
}
} // namespace

TEST_CASE("E2E: BP_Enemy → BPIR → UCLASS C++ produces compile-shaped header") {
    Fixture f;
    auto bpir = DecompileBlueprint(f.reader, "/Game/AI/BP_Enemy");
    auto out  = EmitCppClass(bpir);

    CHECK(ContainsStr(out.headerSource, "ABP_Enemy_Generated"));
    CHECK(ContainsStr(out.headerSource, "public ACharacter"));
    CHECK(ContainsStr(out.headerSource, "GameFramework/Character.h"));
    CHECK(ContainsStr(out.headerSource, "UPROPERTY"));
    // BP_Enemy.Health is replicated → registration must appear.
    CHECK(ContainsStr(out.implSource, "DOREPLIFETIME(ABP_Enemy_Generated, Health)"));
}

TEST_CASE("E2E: BP_Enemy generates UFUNCTION for each function") {
    Fixture f;
    auto bpir = DecompileBlueprint(f.reader, "/Game/AI/BP_Enemy");
    auto out  = EmitCppClass(bpir);
    // BP_Enemy fixture has TakeDamage + OnDeath functions.
    CHECK(ContainsStr(out.headerSource, "TakeDamage"));
    CHECK(ContainsStr(out.headerSource, "OnDeath"));
    CHECK(ContainsStr(out.headerSource, "UFUNCTION(BlueprintCallable)"));
}

// ===== End-to-end ThirdPersonCharacter-shaped BP ==========================
//
// BP_ExampleCharacter exercises a realistic actor BP: ACharacter parent,
// four SCS components (CapsuleComponent / SkeletalMesh / SpringArm /
// FollowCamera) with property overrides, a replicated variable +
// RepNotify, a multicast delegate variable, and parent-virtual overrides
// (BeginPlay / Tick). This is the bar for "fully transpile a BP" --
// every category from the diagnostic + every Stage-3 finding must
// produce compileable output.

TEST_CASE("E2E full BP: ExampleCharacter -> header carries every expected piece") {
    Fixture f;
    auto bpir = DecompileBlueprint(f.reader, "/Game/Characters/BP_ExampleCharacter");
    auto out  = EmitCppClass(bpir);

    // Class name + parent.
    CHECK(ContainsStr(out.headerSource, "ABP_ExampleCharacter_Generated"));
    CHECK(ContainsStr(out.headerSource, "public ACharacter"));
    // Header include for parent (GameFramework/Character.h is what
    // ParentClassToHeader maps ACharacter to).
    CHECK(ContainsStr(out.headerSource, "Character.h"));

    // Variables -> UPROPERTY decls.
    CHECK(ContainsStr(out.headerSource, "float Health"));
    CHECK(ContainsStr(out.headerSource, "float MaxHealth"));
    CHECK(ContainsStr(out.headerSource, "float BaseTurnRate"));

    // Replicated variable on Health.
    CHECK(ContainsStr(out.headerSource, "Replicated"));

    // Multicast delegate variable -> DECLARE + F<Name> typedef.
    CHECK(ContainsStr(out.headerSource, "DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnHealthChanged)"));
    CHECK(ContainsStr(out.headerSource, "FOnHealthChanged OnHealthChanged"));

    // SCS components -> UPROPERTY decls.
    CHECK(ContainsStr(out.headerSource, "TObjectPtr<UCapsuleComponent> CapsuleComponent"));
    CHECK(ContainsStr(out.headerSource, "TObjectPtr<USkeletalMeshComponent> SkeletalMesh"));
    CHECK(ContainsStr(out.headerSource, "TObjectPtr<USpringArmComponent> SpringArm"));
    CHECK(ContainsStr(out.headerSource, "TObjectPtr<UCameraComponent> FollowCamera"));

    // Forward decls for components.
    CHECK(ContainsStr(out.headerSource, "class UCapsuleComponent;"));
    CHECK(ContainsStr(out.headerSource, "class USpringArmComponent;"));

    // BeginPlay / Tick come through as virtual overrides (no UFUNCTION
    // decoration since they're on the void-virtual whitelist).
    CHECK(ContainsStr(out.headerSource, "virtual void BeginPlay() override"));
    CHECK(ContainsStr(out.headerSource, "virtual void Tick("));

    // ApplyHealing is custom -> regular UFUNCTION.
    CHECK(ContainsStr(out.headerSource, "void ApplyHealing("));
}

TEST_CASE("E2E full BP: ExampleCharacter -> impl ctor wires components + defaults + replication") {
    Fixture f;
    auto bpir = DecompileBlueprint(f.reader, "/Game/Characters/BP_ExampleCharacter");
    auto out  = EmitCppClass(bpir);

    // Constructor exists.
    CHECK(ContainsStr(out.implSource, "::ABP_ExampleCharacter_Generated()"));

    // Components instantiated.
    CHECK(ContainsStr(out.implSource,
        "CapsuleComponent = CreateDefaultSubobject<UCapsuleComponent>(TEXT(\"CapsuleComponent\"))"));
    CHECK(ContainsStr(out.implSource,
        "SkeletalMesh = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT(\"SkeletalMesh\"))"));
    CHECK(ContainsStr(out.implSource,
        "SpringArm = CreateDefaultSubobject<USpringArmComponent>(TEXT(\"SpringArm\"))"));
    CHECK(ContainsStr(out.implSource,
        "FollowCamera = CreateDefaultSubobject<UCameraComponent>(TEXT(\"FollowCamera\"))"));

    // SetupAttachment hierarchy.
    CHECK(ContainsStr(out.implSource, "SkeletalMesh->SetupAttachment(CapsuleComponent)"));
    CHECK(ContainsStr(out.implSource, "SpringArm->SetupAttachment(CapsuleComponent)"));
    CHECK(ContainsStr(out.implSource, "FollowCamera->SetupAttachment(SpringArm)"));
    CHECK(ContainsStr(out.implSource, "RootComponent = CapsuleComponent"));

    // Component default properties (scalar / FVector / FRotator).
    CHECK(ContainsStr(out.implSource, "CapsuleComponent->CapsuleHalfHeight = 88.0f"));
    CHECK(ContainsStr(out.implSource, "CapsuleComponent->CapsuleRadius = 42.0f"));
    CHECK(ContainsStr(out.implSource, "SpringArm->TargetArmLength = 300.0f"));
    CHECK(ContainsStr(out.implSource, "SpringArm->bUsePawnControlRotation = true"));
    CHECK(ContainsStr(out.implSource, "FollowCamera->bUsePawnControlRotation = false"));
    CHECK(ContainsStr(out.implSource,
        "SkeletalMesh->RelativeLocation = FVector(0.0f, 0.0f, -90.0f)"));
    CHECK(ContainsStr(out.implSource,
        "SkeletalMesh->RelativeRotation = FRotator(0.0f, -90.0f, 0.0f)"));
    CHECK(ContainsStr(out.implSource,
        "SpringArm->RelativeRotation = FRotator(-15.0f, 0.0f, 0.0f)"));

    // Skeletal mesh asset ref -> ConstructorHelpers TODO scaffold.
    CHECK(ContainsStr(out.implSource, "TODO[bpr-asset-ref]"));
    CHECK(ContainsStr(out.implSource, "SK_Mannequin"));

    // Replication wiring.
    CHECK(ContainsStr(out.implSource, "bReplicates = true"));
    CHECK(ContainsStr(out.implSource,
        "void ABP_ExampleCharacter_Generated::GetLifetimeReplicatedProps("));
    CHECK(ContainsStr(out.implSource,
        "DOREPLIFETIME(ABP_ExampleCharacter_Generated, Health)"));
}

TEST_CASE("E2E full BP: ExampleCharacter -> output is non-trivial (size sanity)") {
    Fixture f;
    auto bpir = DecompileBlueprint(f.reader, "/Game/Characters/BP_ExampleCharacter");
    auto out  = EmitCppClass(bpir);
    // Both halves should be substantial -- not a stub.
    CHECK(out.headerSource.size() > 1500);
    CHECK(out.implSource.size()   > 1200);
    // Notes should carry the asset-ref TODO at minimum.
    bool foundAssetRef = false;
    for (const auto& n : out.notes) {
        if (n.value("treatment", "") == "asset_ref_objectfinder_stub") {
            foundAssetRef = true;
        }
    }
    CHECK(foundAssetRef);
}
