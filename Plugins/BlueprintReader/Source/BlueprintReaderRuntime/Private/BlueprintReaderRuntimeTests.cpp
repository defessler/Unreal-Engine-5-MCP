// UE automation tests for the runtime introspector. These run via
// `UnrealEditor-Cmd.exe -ExecCmds="Automation RunTests
//   BlueprintReaderRuntime"` (or through the MCP server's
// `run_automation_tests` tool). They exercise the same code paths
// that run in a cooked build, so a regression in the runtime read
// surface gets caught here rather than only when someone actually
// packages a game.
//
// The fixture used is `/Game/AI/BP_TestEnemy` — also used by the
// editor-side live tests. Tests skip silently if the fixture asset
// isn't present in the running project (CI without the test BPs).

#include "BlueprintReaderRuntimeJson.h"
#include "BlueprintRuntimeIntrospector.h"

#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "UObject/Class.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	// Shared fixture asset for the runtime tests. Same one the editor
	// live tests use (Plugins/BlueprintReader/Scripts/Seed runs the
	// BlueprintReaderSeed commandlet to recreate it).
	constexpr const TCHAR* kFixtureAsset = TEXT("/Game/AI/BP_TestEnemy");

	// Auto-skip when the fixture isn't in the project. CI environments
	// without the test content shouldn't fail the suite outright.
	bool FixturePresent()
	{
		return FBlueprintRuntimeIntrospector::ResolveClass(kFixtureAsset) != nullptr;
	}
}

// -----------------------------------------------------------------
// ResolveClass: package path → UClass* via the _C-suffix convention.
// -----------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBPRuntimeResolveClassByPackagePath,
	"BlueprintReaderRuntime.ResolveClass.PackagePathResolves",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::SmokeFilter)

bool FBPRuntimeResolveClassByPackagePath::RunTest(const FString&)
{
	if (!FixturePresent())
	{
		AddInfo(TEXT("Fixture /Game/AI/BP_TestEnemy missing; skipping"));
		return true;
	}
	UClass* Class = FBlueprintRuntimeIntrospector::ResolveClass(kFixtureAsset);
	TestNotNull(TEXT("Class resolved"), Class);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBPRuntimeResolveClassByClassPath,
	"BlueprintReaderRuntime.ResolveClass.ClassPathPassesThrough",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::SmokeFilter)

bool FBPRuntimeResolveClassByClassPath::RunTest(const FString&)
{
	if (!FixturePresent()) { AddInfo(TEXT("Fixture missing; skipping")); return true; }
	// Caller may already have the _C-suffix class path.
	const FString ClassPath = FString(kFixtureAsset) + TEXT(".BP_TestEnemy_C");
	UClass* Class = FBlueprintRuntimeIntrospector::ResolveClass(ClassPath);
	TestNotNull(TEXT("Class path resolves"), Class);
	return true;
}

// -----------------------------------------------------------------
// Read: full blueprint introspection via UClass reflection.
// -----------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBPRuntimeReadBlueprintReturnsExpectedShape,
	"BlueprintReaderRuntime.Read.ReturnsExpectedShape",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::SmokeFilter)

bool FBPRuntimeReadBlueprintReturnsExpectedShape::RunTest(const FString&)
{
	if (!FixturePresent()) { AddInfo(TEXT("Fixture missing; skipping")); return true; }
	TOptional<FBPRRBlueprint> BP = FBlueprintRuntimeIntrospector::Read(kFixtureAsset);
	TestTrue(TEXT("Read returns a value"), BP.IsSet());
	if (!BP.IsSet())
	{
		return false;
	}

	TestEqual(TEXT("AssetPath preserved"), BP->AssetPath, FString(kFixtureAsset));
	TestEqual(TEXT("Name from class"), BP->Name, FString(TEXT("BP_TestEnemy")));
	TestFalse(TEXT("ParentClassPath populated"), BP->ParentClassPath.IsEmpty());

	// BP_TestEnemy has at least 4 variables (Health / MaxHealth /
	// AggroTarget / bIsAlive) in the seed. Don't pin the exact count —
	// the seed evolves; assert non-zero.
	TestTrue(TEXT("Variables[] populated"), BP->Variables.Num() > 0);

	// BP_TestEnemy has 2 functions (TakeDamage / OnDeath).
	TestTrue(TEXT("Functions[] populated"), BP->Functions.Num() > 0);
	return true;
}

// -----------------------------------------------------------------
// Variables: types decoded correctly + replicated flag propagates.
// -----------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBPRuntimeVariablesCarryTypeAndReplicationFlag,
	"BlueprintReaderRuntime.Read.VariablesCarryTypeAndReplicationFlag",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::SmokeFilter)

bool FBPRuntimeVariablesCarryTypeAndReplicationFlag::RunTest(const FString&)
{
	if (!FixturePresent()) { AddInfo(TEXT("Fixture missing; skipping")); return true; }
	TOptional<FBPRRBlueprint> BP = FBlueprintRuntimeIntrospector::Read(kFixtureAsset);
	if (!BP.IsSet())
	{
		return false;
	}

	bool bFoundHealth = false;
	for (const FBPRRVariable& V : BP->Variables)
	{
		if (V.Name == TEXT("Health"))
		{
			bFoundHealth = true;
			// Health is a float. Shorthand may be either "real:float"
			// (UE 5+ FFloatProperty maps through real) or "float"
			// depending on the seed; accept both.
			const bool bFloat = V.TypeShorthand.Contains(TEXT("float")) ||
			                    V.TypeShorthand.Contains(TEXT("real"));
			TestTrue(TEXT("Health is a real-ish type"), bFloat);
			TestTrue(TEXT("Health is replicated"), V.bIsReplicated);
		}
	}
	TestTrue(TEXT("Found Health variable"), bFoundHealth);
	return true;
}

// -----------------------------------------------------------------
// Functions: signature surface populated (inputs / outputs / flags).
// -----------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBPRuntimeFunctionSignaturesArePopulated,
	"BlueprintReaderRuntime.Read.FunctionSignaturesArePopulated",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::SmokeFilter)

bool FBPRuntimeFunctionSignaturesArePopulated::RunTest(const FString&)
{
	if (!FixturePresent()) { AddInfo(TEXT("Fixture missing; skipping")); return true; }
	TOptional<FBPRRBlueprint> BP = FBlueprintRuntimeIntrospector::Read(kFixtureAsset);
	if (!BP.IsSet())
	{
		return false;
	}

	bool bFoundTakeDamage = false;
	for (const FBPRRFunction& F : BP->Functions)
	{
		if (F.Name == TEXT("TakeDamage"))
		{
			bFoundTakeDamage = true;
			// BP TakeDamage has Damage:float input + Killed:bool output.
			TestEqual(TEXT("TakeDamage inputs"), F.Inputs.Num(), 1);
			TestEqual(TEXT("TakeDamage outputs"), F.Outputs.Num(), 1);
			TestTrue(TEXT("TakeDamage BP-callable"), F.bIsBlueprintCallable);
		}
	}
	TestTrue(TEXT("Found TakeDamage function"), bFoundTakeDamage);
	return true;
}

// -----------------------------------------------------------------
// JSON serializer round-trips through the wire format. Catches
// regressions in the field-name contract the MCP server depends on.
// -----------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBPRuntimeJsonShapeMatchesWireContract,
	"BlueprintReaderRuntime.Json.ShapeMatchesWireContract",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::SmokeFilter)

bool FBPRuntimeJsonShapeMatchesWireContract::RunTest(const FString&)
{
	if (!FixturePresent()) { AddInfo(TEXT("Fixture missing; skipping")); return true; }
	TOptional<FBPRRBlueprint> BP = FBlueprintRuntimeIntrospector::Read(kFixtureAsset);
	if (!BP.IsSet())
	{
		return false;
	}

	TSharedRef<FJsonObject> Json = FBlueprintReaderRuntimeJson::BlueprintToJson(*BP);
	// Required top-level keys the MCP server's wire-format parser
	// expects on a read_blueprint response.
	TestTrue(TEXT("asset_path present"), Json->HasField(TEXT("asset_path")));
	TestTrue(TEXT("name present"), Json->HasField(TEXT("name")));
	TestTrue(TEXT("parent_class present"), Json->HasField(TEXT("parent_class")));
	TestTrue(TEXT("interfaces present"), Json->HasField(TEXT("interfaces")));
	TestTrue(TEXT("variables present"), Json->HasField(TEXT("variables")));
	TestTrue(TEXT("functions present"), Json->HasField(TEXT("functions")));
	TestTrue(TEXT("components present"), Json->HasField(TEXT("components")));
	TestEqual(TEXT("source tag"), Json->GetStringField(TEXT("source")),
	          FString(TEXT("runtime-introspector")));
	return true;
}

// -----------------------------------------------------------------
// ListBlueprints — Asset Registry walk works in editor + cooked.
// -----------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBPRuntimeListSurfacesTestBPs,
	"BlueprintReaderRuntime.List.SurfacesTestBPs",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::SmokeFilter)

bool FBPRuntimeListSurfacesTestBPs::RunTest(const FString&)
{
	if (!FixturePresent()) { AddInfo(TEXT("Fixture missing; skipping")); return true; }
	TArray<FBPRRAssetSummary> Assets =
		FBlueprintRuntimeIntrospector::ListBlueprints(TEXT("/Game/AI"));
	// The seed creates BP_TestEnemy + BP_TestPickup under /Game/AI.
	// Cooked builds with a custom cook profile may filter; allow either
	// 1 (only one fixture got cooked) or 2 (both did).
	TestTrue(TEXT("Found at least one BP under /Game/AI"), Assets.Num() >= 1);

	bool bFoundEnemy = false;
	for (const FBPRRAssetSummary& S : Assets)
	{
		if (S.AssetPath == FString(kFixtureAsset))
		{
			bFoundEnemy = true;
		}
	}
	TestTrue(TEXT("BP_TestEnemy in list"), bFoundEnemy);
	return true;
}

#endif    // WITH_DEV_AUTOMATION_TESTS
