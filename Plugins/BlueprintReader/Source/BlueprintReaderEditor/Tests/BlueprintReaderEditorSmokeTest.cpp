#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBlueprintReaderEditorSmokeTest,
	"BlueprintReader.Editor.Smoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintReaderEditorSmokeTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("Smoke test sanity"), 1 + 1 == 2);
	return true;
}

#endif
