#include "Misc/AutomationTest.h"

#include "BlueprintReaderJson.h"
#include "BlueprintReaderTypes.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FBlueprintInfo MakeFixture()
	{
		FBlueprintInfo Info;
		Info.Path = TEXT("/Game/Tests/MyBP.MyBP");
		Info.Name = TEXT("MyBP");
		Info.BlueprintType = TEXT("BPTYPE_Normal");
		Info.ParentClassPath = TEXT("/Script/Engine.Actor");
		Info.GeneratedClassPath = TEXT("/Game/Tests/MyBP.MyBP_C");

		FBPInterfaceInfo Iface;
		Iface.InterfacePath = TEXT("/Script/Engine.SomeInterface");
		Info.Interfaces.Add(Iface);

		FBPVariableInfo Var;
		Var.Name = TEXT("Health");
		Var.Category = TEXT("Stats");
		Var.Type = TEXT("float");
		Var.DefaultValue = TEXT("100.0");
		Var.bIsReplicated = true;
		Info.Variables.Add(Var);

		FBPComponentInfo Comp;
		Comp.Name = TEXT("Mesh");
		Comp.ClassPath = TEXT("/Script/Engine.StaticMeshComponent");
		Comp.ParentName = TEXT("DefaultSceneRoot");
		Info.Components.Add(Comp);

		FBPGraphInfo Graph;
		Graph.Name = TEXT("EventGraph");
		Graph.SchemaPath = TEXT("/Script/BlueprintGraph.EdGraphSchema_K2");

		FBPNodeInfo Node;
		Node.Guid = TEXT("00000000-0000-0000-0000-000000000001");
		Node.ClassName = TEXT("K2Node_Event");
		Node.Title = TEXT("Event BeginPlay");
		Node.PosX = 100;
		Node.PosY = 200;

		FBPPinInfo Pin;
		Pin.Name = TEXT("then");
		Pin.Direction = TEXT("Output");
		Pin.Type = TEXT("exec");
		Node.Pins.Add(Pin);

		Graph.Nodes.Add(Node);
		Info.EventGraphs.Add(Graph);
		return Info;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBlueprintReaderJsonRoundTripTest,
	"BlueprintReader.Editor.JsonRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintReaderJsonRoundTripTest::RunTest(const FString& /*Parameters*/)
{
	const FBlueprintInfo Fixture = MakeFixture();
	const FString Json = FBlueprintReaderJson::ToString(Fixture, /*bPretty=*/false);

	TSharedPtr<FJsonObject> Parsed;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	TestTrue(TEXT("JSON parses"), FJsonSerializer::Deserialize(Reader, Parsed) && Parsed.IsValid());
	if (!Parsed.IsValid()) return false;

	TestEqual(TEXT("path"), Parsed->GetStringField(TEXT("path")), Fixture.Path);
	TestEqual(TEXT("name"), Parsed->GetStringField(TEXT("name")), Fixture.Name);
	TestEqual(TEXT("blueprintType"), Parsed->GetStringField(TEXT("blueprintType")), Fixture.BlueprintType);
	TestEqual(TEXT("parentClass"), Parsed->GetStringField(TEXT("parentClass")), Fixture.ParentClassPath);

	const TArray<TSharedPtr<FJsonValue>>* Variables = nullptr;
	TestTrue(TEXT("variables array"), Parsed->TryGetArrayField(TEXT("variables"), Variables));
	TestEqual(TEXT("variables count"), Variables ? Variables->Num() : 0, 1);

	const TArray<TSharedPtr<FJsonValue>>* EventGraphs = nullptr;
	TestTrue(TEXT("eventGraphs array"), Parsed->TryGetArrayField(TEXT("eventGraphs"), EventGraphs));
	if (EventGraphs && EventGraphs->Num() == 1)
	{
		const TSharedPtr<FJsonObject>& Graph = (*EventGraphs)[0]->AsObject();
		TestEqual(TEXT("graph name"), Graph->GetStringField(TEXT("name")), TEXT("EventGraph"));
		const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
		TestTrue(TEXT("nodes array"), Graph->TryGetArrayField(TEXT("nodes"), Nodes));
		TestEqual(TEXT("nodes count"), Nodes ? Nodes->Num() : 0, 1);
	}
	else
	{
		AddError(TEXT("Expected exactly one event graph"));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBlueprintReaderJsonEmptyInfoTest,
	"BlueprintReader.Editor.JsonEmpty",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintReaderJsonEmptyInfoTest::RunTest(const FString& /*Parameters*/)
{
	FBlueprintInfo Empty;
	const FString Json = FBlueprintReaderJson::ToString(Empty, /*bPretty=*/false);

	TSharedPtr<FJsonObject> Parsed;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	TestTrue(TEXT("JSON parses"), FJsonSerializer::Deserialize(Reader, Parsed) && Parsed.IsValid());
	if (!Parsed.IsValid()) return false;

	for (const TCHAR* Field : { TEXT("interfaces"), TEXT("variables"), TEXT("components"),
		TEXT("functionGraphs"), TEXT("eventGraphs"), TEXT("macroGraphs"), TEXT("delegateSignatureGraphs") })
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		TestTrue(FString::Printf(TEXT("%s exists"), Field), Parsed->TryGetArrayField(Field, Arr));
		TestEqual(FString::Printf(TEXT("%s empty"), Field), Arr ? Arr->Num() : -1, 0);
	}
	return true;
}

#endif
