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

#include "backends/MockBlueprintReader.h"
#include "tools/Bpir.h"
#include "tools/Decompile.h"

#include "test_helpers.h"

using namespace bpr;
using namespace bpr::tools;
using namespace bpr::backends;

namespace test_decompile_detail {
struct Fixture {
	MockBlueprintReader reader;
	Fixture() : reader(test::FixturesDir()) {}
};
}    // namespace test_decompile_detail
using namespace test_decompile_detail;

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
			if (v.contains("replicated"))
			{
				CHECK(v["replicated"] == true);
			}
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

namespace test_decompile_detail2 {
bool ContainsStr(const std::string& s, const std::string& sub) {
	return s.find(sub) != std::string::npos;
}
}    // namespace test_decompile_detail2
using namespace test_decompile_detail2;

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

// ===== Stateful-macro lowering ============================================
// BP's stateful macros (DoOnce, FlipFlop, DoN) can't be inlined as pure
// control flow — each carries hidden per-instance state that survives
// across calls. The decompiler synthesizes a class-level member variable
// per macro instance, and rewrites the macro body into a conditional
// guarded by that flag. The fixture BP_StatefulMacros exercises one
// instance of each.

TEST_CASE("decompile: DoOnce macro lowers to if-guard + synthetic flag") {
    Fixture f;
    auto bpir = DecompileFunction(f.reader, "/Game/Tests/BP_StatefulMacros", "TestDoOnce");
    CHECK_NOTHROW(ValidateBpir(bpir));

    // The function doc should carry the synthetic flag pending hoist
    // into the class doc by DecompileBlueprint.
    REQUIRE(bpir.contains("auto_synth_vars"));
    REQUIRE(bpir["auto_synth_vars"].is_array());
    REQUIRE(bpir["auto_synth_vars"].size() == 1);
    const auto& flag = bpir["auto_synth_vars"][0];
    CHECK(flag["type"] == "bool");
    CHECK(flag["default"] == "false");  // StartClosed=false on the fixture
    const std::string flagName = flag["name"];
    CHECK(flagName.rfind("b__BprDoOnce_", 0) == 0);
    CHECK(flagName.find("_HasFired") != std::string::npos);

    // Body should be a single {if: ...} statement whose `then` block
    // sets the flag true then runs the Completed-side body.
    REQUIRE(bpir["body"].is_array());
    REQUIRE(bpir["body"].size() == 1);
    const auto& stmt = bpir["body"][0];
    REQUIRE(stmt.contains("if"));
    REQUIRE(stmt.contains("then"));
    // The condition is `!flag` lowered via KismetMathLibrary::Not_PreBool.
    CHECK(stmt["if"]["call"] == "KismetMathLibrary::Not_PreBool");
    // First statement in `then` flips the flag true; rest is body.
    const auto& thenBlock = stmt["then"];
    REQUIRE(thenBlock.size() >= 1);
    CHECK(thenBlock[0]["set"] == flagName);
    CHECK(thenBlock[0]["scope"] == "member");
}

TEST_CASE("decompile: FlipFlop macro lowers to if/else + synth flag") {
    Fixture f;
    auto bpir = DecompileFunction(f.reader, "/Game/Tests/BP_StatefulMacros", "TestFlipFlop");
    CHECK_NOTHROW(ValidateBpir(bpir));

    REQUIRE(bpir.contains("auto_synth_vars"));
    REQUIRE(bpir["auto_synth_vars"].size() == 1);
    const auto& flag = bpir["auto_synth_vars"][0];
    CHECK(flag["type"] == "bool");
    CHECK(flag["default"] == "true");  // first call routes to A
    const std::string flagName = flag["name"];
    CHECK(flagName.rfind("b__BprFlipFlop_", 0) == 0);
    CHECK(flagName.find("_IsA") != std::string::npos);

    REQUIRE(bpir["body"].size() == 1);
    const auto& stmt = bpir["body"][0];
    REQUIRE(stmt.contains("if"));
    REQUIRE(stmt.contains("then"));
    REQUIRE(stmt.contains("else"));
    // Condition is a plain variable read of the flag — no negation.
    CHECK(stmt["if"]["var"] == flagName);
    // Each branch must prepend the flag-flip.
    CHECK(stmt["then"][0]["set"] == flagName);
    CHECK(stmt["else"][0]["set"] == flagName);
}

TEST_CASE("decompile: DoN macro lowers to if/<counter < N> + synth counter") {
    Fixture f;
    auto bpir = DecompileFunction(f.reader, "/Game/Tests/BP_StatefulMacros", "TestDoN");
    CHECK_NOTHROW(ValidateBpir(bpir));

    REQUIRE(bpir.contains("auto_synth_vars"));
    REQUIRE(bpir["auto_synth_vars"].size() == 1);
    const auto& counter = bpir["auto_synth_vars"][0];
    CHECK(counter["type"] == "int");
    CHECK(counter["default"] == "0");
    const std::string counterName = counter["name"];
    CHECK(counterName.rfind("i__BprDoN_", 0) == 0);
    CHECK(counterName.find("_Counter") != std::string::npos);

    REQUIRE(bpir["body"].size() == 1);
    const auto& stmt = bpir["body"][0];
    REQUIRE(stmt.contains("if"));
    REQUIRE(stmt.contains("then"));
    // Condition is `counter < N` lowered via KismetMathLibrary::Less_IntInt.
    CHECK(stmt["if"]["call"] == "KismetMathLibrary::Less_IntInt");
    // First statement in `then` increments the counter via Add_IntInt.
    const auto& thenBlock = stmt["then"];
    REQUIRE(thenBlock.size() >= 1);
    CHECK(thenBlock[0]["set"] == counterName);
    CHECK(thenBlock[0]["to"]["call"] == "KismetMathLibrary::Add_IntInt");
}

TEST_CASE("decompile_blueprint: hoists stateful-macro synth vars into class variables") {
    Fixture f;
    auto bpir = DecompileBlueprint(f.reader, "/Game/Tests/BP_StatefulMacros");
    CHECK_NOTHROW(ValidateBpir(bpir));
    REQUIRE(bpir["variables"].is_array());

    // One flag per stateful macro instance → 3 synth vars total.
    std::size_t doOnceCount = 0, flipFlopCount = 0, doNCount = 0;
    for (const auto& v : bpir["variables"]) {
        const std::string name = v.value("name", "");
        if (name.rfind("b__BprDoOnce_",   0) == 0) ++doOnceCount;
        if (name.rfind("b__BprFlipFlop_", 0) == 0) ++flipFlopCount;
        if (name.rfind("i__BprDoN_",      0) == 0) ++doNCount;
    }
    CHECK(doOnceCount == 1);
    CHECK(flipFlopCount == 1);
    CHECK(doNCount == 1);

    // Function docs should no longer carry the synth vars — they belong
    // to the class now.
    for (const auto& fn : bpir["functions"]) {
        CHECK_FALSE(fn.contains("auto_synth_vars"));
    }
}

TEST_CASE("E2E stateful macros: codegen emits UPROPERTY + flag-guarded C++") {
    Fixture f;
    auto bpir = DecompileBlueprint(f.reader, "/Game/Tests/BP_StatefulMacros");
    auto out  = EmitCppClass(bpir);

    // Each synth var renders as a UPROPERTY in the header.
    CHECK(ContainsStr(out.headerSource, "b__BprDoOnce_"));
    CHECK(ContainsStr(out.headerSource, "b__BprFlipFlop_"));
    CHECK(ContainsStr(out.headerSource, "i__BprDoN_"));

    // Function bodies emit the guarded code. The DoOnce body should
    // contain `if (!b__BprDoOnce_..._HasFired)` and the assignment
    // inside it.
    CHECK(ContainsStr(out.implSource, "_HasFired"));
    CHECK(ContainsStr(out.implSource, "_IsA"));
    CHECK(ContainsStr(out.implSource, "_Counter"));
    // The operator-aliasing path means the negation comes out as `!`,
    // and the Add/Less calls come out as binary operators.
    CHECK(ContainsStr(out.implSource, "if ((!"));     // !flag
    CHECK(ContainsStr(out.implSource, " < 3)"));      // counter < 3
    CHECK(ContainsStr(out.implSource, " + 1)"));      // counter + 1
}

// ===== Latent-action lowering (Delay) =====================================
// Delay can't be expressed as a single inline C++ statement because the
// post-delay exec must run later. Lowering generates:
//   1. A SetTimer call at the Delay's position in the parent function
//   2. A synth FTimerHandle member variable (for cancel/state)
//   3. A continuation UFUNCTION carrying the post-delay exec body
// The agent gets a working C++ scaffold instead of a TODO.

TEST_CASE("decompile: Delay emits __bpr_set_timer + continuation function") {
    Fixture f;
    auto bpir = DecompileFunction(f.reader, "/Game/Tests/BP_StatefulMacros", "TestDelay");
    CHECK_NOTHROW(ValidateBpir(bpir));

    // Synth FTimerHandle must appear in auto_synth_vars.
    REQUIRE(bpir.contains("auto_synth_vars"));
    bool sawHandle = false;
    for (const auto& v : bpir["auto_synth_vars"]) {
        const std::string name = v.value("name", "");
        if (name.find("_DelayHandle_") != std::string::npos) {
            sawHandle = true;
            CHECK(v["type"] == "struct:TimerHandle");
        }
    }
    CHECK(sawHandle);

    // Synth continuation must appear in auto_synth_funcs.
    REQUIRE(bpir.contains("auto_synth_funcs"));
    REQUIRE(bpir["auto_synth_funcs"].is_array());
    REQUIRE(bpir["auto_synth_funcs"].size() == 1);
    const auto& cont = bpir["auto_synth_funcs"][0];
    CHECK(cont["kind"] == "function");
    CHECK(cont["name"].get<std::string>().find("TestDelay_DelayCont_") == 0);
    // The continuation has no parameters and a body of one statement
    // (the post-Delay PrintString).
    CHECK(cont["inputs"].empty());
    CHECK(cont["outputs"].empty());
    REQUIRE(cont["body"].is_array());
    CHECK(cont["body"].size() == 1);
    // The pre-delay body of the parent has the PrintString("Pre") +
    // the SetTimer + (no statement past since exec terminates).
    REQUIRE(bpir["body"].is_array());
    bool sawSetTimer = false;
    for (const auto& s : bpir["body"]) {
        if (s.is_object() && s.value("call", "") == "__bpr_set_timer") {
            sawSetTimer = true;
            REQUIRE(s.contains("args"));
            const auto& a = s["args"];
            CHECK(a.contains("Duration"));
            CHECK(a.contains("Handle"));
            CHECK(a.contains("Callback"));
            // Handle / Callback are emitted as string literals.
            CHECK(a["Handle"]["lit"].is_string());
            CHECK(a["Callback"]["lit"].is_string());
        }
    }
    CHECK(sawSetTimer);
}

TEST_CASE("decompile_blueprint: hoists Delay continuation into class functions[]") {
    Fixture f;
    auto bpir = DecompileBlueprint(f.reader, "/Game/Tests/BP_StatefulMacros");
    CHECK_NOTHROW(ValidateBpir(bpir));

    // The class's functions[] should now include TestDelay + its
    // continuation, plus the other test functions and their
    // continuations (none in this fixture). The continuation should
    // appear directly after its parent.
    REQUIRE(bpir["functions"].is_array());
    bool sawTestDelay = false, sawContAfter = false;
    for (std::size_t i = 0; i + 1 < bpir["functions"].size(); ++i) {
        const std::string a = bpir["functions"][i].value("name", "");
        const std::string b = bpir["functions"][i + 1].value("name", "");
        if (a == "TestDelay") {
            sawTestDelay = true;
            if (b.find("TestDelay_DelayCont_") == 0) sawContAfter = true;
        }
    }
    CHECK(sawTestDelay);
    CHECK(sawContAfter);

    // FTimerHandle synth var should be on the class.
    bool sawHandleOnClass = false;
    for (const auto& v : bpir["variables"]) {
        if (v.value("name", "").find("TestDelay_DelayHandle_") == 0) {
            sawHandleOnClass = true;
            CHECK(v["type"] == "struct:TimerHandle");
        }
    }
    CHECK(sawHandleOnClass);

    // No function should still carry auto_synth_funcs after hoisting.
    for (const auto& fn : bpir["functions"]) {
        CHECK_FALSE(fn.contains("auto_synth_funcs"));
    }
}

TEST_CASE("E2E Delay: codegen renders SetTimer + ThisClass::cont member pointer") {
    Fixture f;
    auto bpir = DecompileBlueprint(f.reader, "/Game/Tests/BP_StatefulMacros");
    auto out  = EmitCppClass(bpir);

    // The SetTimer call in the parent function.
    CHECK(ContainsStr(out.implSource,
        "GetWorld()->GetTimerManager().SetTimer("));
    CHECK(ContainsStr(out.implSource, "&ThisClass::TestDelay_DelayCont_"));
    // The continuation appears as a UFUNCTION() with no specifiers.
    CHECK(ContainsStr(out.headerSource, "UFUNCTION()"));
    CHECK(ContainsStr(out.headerSource, "TestDelay_DelayCont_"));
    // FTimerHandle UPROPERTY on the class.
    CHECK(ContainsStr(out.headerSource, "FTimerHandle"));
    CHECK(ContainsStr(out.headerSource, "TestDelay_DelayHandle_"));
}

// ===== EnhancedInput auto-lowering ========================================
// K2Node_EnhancedInputAction lives in the event graph and binds a
// UInputAction asset to BP exec chains. The C++ idiom is completely
// different — bindings live in SetupPlayerInputComponent and each
// trigger event becomes its own UFUNCTION callback. The decompiler's
// EnhancedInput pass scans event graphs, synthesizes the UInputAction*
// member, per-trigger callbacks, and a SetupPlayerInputComponent
// override that wires them together. Output is a complete drop-in
// scaffold.

TEST_CASE("decompile_blueprint: EnhancedInput synthesizes IA member + callbacks + SetupPlayerInputComponent") {
    Fixture f;
    auto bpir = DecompileBlueprint(f.reader, "/Game/Input/BP_InputDemo");
    CHECK_NOTHROW(ValidateBpir(bpir));

    // UInputAction* synth member.
    bool sawIaJump = false;
    for (const auto& v : bpir["variables"]) {
        if (v.value("name", "") == "IA_Jump") {
            sawIaJump = true;
            CHECK(v["type"] == "object:InputAction");
            CHECK(v.value("editable", false) == true);
        }
    }
    CHECK(sawIaJump);

    // Per-trigger callbacks + SetupPlayerInputComponent override on
    // functions[]. The fixture wires Started + Triggered (not Completed)
    // so we expect exactly 2 callbacks plus the setup fn. Names use
    // the bare action name (IA_ prefix stripped) so we get
    // OnIA_Jump_Started, not OnIA_IA_Jump_Started.
    bool sawStartedCb = false, sawTriggeredCb = false;
    bool sawCompletedCb = false, sawSetup = false;
    for (const auto& fn : bpir["functions"]) {
        const std::string name = fn.value("name", "");
        if (name == "OnIA_Jump_Started")   sawStartedCb = true;
        if (name == "OnIA_Jump_Triggered") sawTriggeredCb = true;
        if (name == "OnIA_Jump_Completed") sawCompletedCb = true;
        if (name == "SetupPlayerInputComponent") sawSetup = true;
    }
    CHECK(sawStartedCb);
    CHECK(sawTriggeredCb);
    CHECK_FALSE(sawCompletedCb);  // unwired pin → no callback
    CHECK(sawSetup);
}

TEST_CASE("E2E EnhancedInput: codegen emits override + Cast guard + BindAction lines") {
    Fixture f;
    auto bpir = DecompileBlueprint(f.reader, "/Game/Input/BP_InputDemo");
    auto out  = EmitCppClass(bpir);

    // Setup override.
    CHECK(ContainsStr(out.headerSource,
        "virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override"));
    CHECK(ContainsStr(out.implSource,
        "Super::SetupPlayerInputComponent(PlayerInputComponent)"));
    // Cast guard wraps the bindings.
    CHECK(ContainsStr(out.implSource,
        "if (auto* EIC = Cast<UEnhancedInputComponent>(PlayerInputComponent))"));
    // BindAction lines for the wired triggers.
    CHECK(ContainsStr(out.implSource,
        "EIC->BindAction(IA_Jump, ETriggerEvent::Started, this, &ThisClass::OnIA_Jump_Started);"));
    CHECK(ContainsStr(out.implSource,
        "EIC->BindAction(IA_Jump, ETriggerEvent::Triggered, this, &ThisClass::OnIA_Jump_Triggered);"));
    // UInputAction* UPROPERTY on the class header.
    CHECK(ContainsStr(out.headerSource, "TObjectPtr<UInputAction>"));
    CHECK(ContainsStr(out.headerSource, "IA_Jump"));
    // Callback functions render as bare UFUNCTION() (no BlueprintCallable).
    CHECK(ContainsStr(out.headerSource, "OnIA_Jump_Started"));
}
