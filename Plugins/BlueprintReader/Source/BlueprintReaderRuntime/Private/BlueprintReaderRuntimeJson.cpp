#include "BlueprintReaderRuntimeJson.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	TSharedRef<FJsonObject> VariableToJson(const FBPRRVariable& V)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), V.Name);
		Obj->SetStringField(TEXT("type"), V.TypeShorthand);
		Obj->SetStringField(TEXT("default"), V.DefaultValue);
		Obj->SetStringField(TEXT("category"), V.Category);
		Obj->SetBoolField(TEXT("replicated"), V.bIsReplicated);
		Obj->SetBoolField(TEXT("editable"), V.bIsEditable);
		if (V.bIsExposeOnSpawn)
		{
			Obj->SetBoolField(TEXT("expose_on_spawn"), true);
		}
		if (!V.RepCondition.IsEmpty())
		{
			Obj->SetStringField(TEXT("rep_condition"), V.RepCondition);
		}
		if (!V.RepNotifyFunc.IsEmpty())
		{
			Obj->SetStringField(TEXT("rep_notify_func"), V.RepNotifyFunc);
		}
		return Obj;
	}

	TSharedRef<FJsonObject> FunctionToJson(const FBPRRFunction& F)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), F.Name);

		TArray<TSharedPtr<FJsonValue>> InputsArr;
		for (const FBPRRVariable& V : F.Inputs)
		{
			InputsArr.Add(MakeShared<FJsonValueObject>(VariableToJson(V)));
		}
		Obj->SetArrayField(TEXT("inputs"), InputsArr);

		TArray<TSharedPtr<FJsonValue>> OutputsArr;
		for (const FBPRRVariable& V : F.Outputs)
		{
			OutputsArr.Add(MakeShared<FJsonValueObject>(VariableToJson(V)));
		}
		Obj->SetArrayField(TEXT("outputs"), OutputsArr);

		// Function flags as a metadata sub-object — keeps the wire flat
		// for the common case but lets the agent see RPC / Pure / etc.
		// when present.
		TSharedRef<FJsonObject> Flags = MakeShared<FJsonObject>();
		Flags->SetBoolField(TEXT("blueprint_callable"), F.bIsBlueprintCallable);
		Flags->SetBoolField(TEXT("blueprint_pure"), F.bIsBlueprintPure);
		if (F.bIsBlueprintImplementableEvent)
		{
			Flags->SetBoolField(TEXT("blueprint_implementable_event"), true);
		}
		if (F.bIsBlueprintNativeEvent)
		{
			Flags->SetBoolField(TEXT("blueprint_native_event"), true);
		}
		if (F.bIsNetServer)
		{
			Flags->SetBoolField(TEXT("net_server"), true);
		}
		if (F.bIsNetClient)
		{
			Flags->SetBoolField(TEXT("net_client"), true);
		}
		if (F.bIsNetMulticast)
		{
			Flags->SetBoolField(TEXT("net_multicast"), true);
		}
		if (F.bIsNetReliable)
		{
			Flags->SetBoolField(TEXT("net_reliable"), true);
		}
		Obj->SetObjectField(TEXT("flags"), Flags);

		// graphs[] is intentionally empty — runtime can't see source
		// graphs (stripped during cook). Editor-side read fills it.
		Obj->SetArrayField(TEXT("graphs"), TArray<TSharedPtr<FJsonValue>>{});
		return Obj;
	}

	TSharedRef<FJsonObject> ComponentToJson(const FBPRRComponent& C)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), C.Name);
		Obj->SetStringField(TEXT("class_path"), C.ClassPath);
		Obj->SetStringField(TEXT("parent_name"), C.ParentName);
		Obj->SetBoolField(TEXT("is_root"), C.bIsRoot);
		return Obj;
	}
}

TSharedRef<FJsonObject> FBlueprintReaderRuntimeJson::SummaryToJson(
	const FBPRRAssetSummary& Summary)
{
	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("asset_path"), Summary.AssetPath);
	Obj->SetStringField(TEXT("name"), Summary.Name);
	Obj->SetStringField(TEXT("parent_class"), Summary.ParentClass);
	// modified_iso is editor-only (depends on file mtime); cooked
	// assets live in .pak files with no per-asset stat available.
	Obj->SetStringField(TEXT("modified_iso"), TEXT(""));
	return Obj;
}

TSharedRef<FJsonObject> FBlueprintReaderRuntimeJson::BlueprintToJson(
	const FBPRRBlueprint& BP)
{
	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("asset_path"), BP.AssetPath);
	Obj->SetStringField(TEXT("name"), BP.Name);
	Obj->SetStringField(TEXT("parent_class"), BP.ParentClassPath);

	TArray<TSharedPtr<FJsonValue>> Interfaces;
	for (const FString& I : BP.Interfaces)
	{
		Interfaces.Add(MakeShared<FJsonValueString>(I));
	}
	Obj->SetArrayField(TEXT("interfaces"), Interfaces);

	TArray<TSharedPtr<FJsonValue>> Variables = VariablesToJson(BP);
	Obj->SetArrayField(TEXT("variables"), Variables);

	TArray<TSharedPtr<FJsonValue>> Components = ComponentsToJson(BP);
	Obj->SetArrayField(TEXT("components"), Components);

	TArray<TSharedPtr<FJsonValue>> Functions;
	for (const FBPRRFunction& F : BP.Functions)
	{
		Functions.Add(MakeShared<FJsonValueObject>(FunctionToJson(F)));
	}
	Obj->SetArrayField(TEXT("functions"), Functions);

	// Source carried through from the runtime side — agent can tell
	// it's a cooked-runtime read vs editor read (the latter populates
	// graphs[]).
	Obj->SetStringField(TEXT("source"), TEXT("runtime-introspector"));
	return Obj;
}

TArray<TSharedPtr<FJsonValue>> FBlueprintReaderRuntimeJson::VariablesToJson(
	const FBPRRBlueprint& BP)
{
	TArray<TSharedPtr<FJsonValue>> Out;
	for (const FBPRRVariable& V : BP.Variables)
	{
		Out.Add(MakeShared<FJsonValueObject>(VariableToJson(V)));
	}
	return Out;
}

TArray<TSharedPtr<FJsonValue>> FBlueprintReaderRuntimeJson::ComponentsToJson(
	const FBPRRBlueprint& BP)
{
	TArray<TSharedPtr<FJsonValue>> Out;
	for (const FBPRRComponent& C : BP.Components)
	{
		Out.Add(MakeShared<FJsonValueObject>(ComponentToJson(C)));
	}
	return Out;
}

FString FBlueprintReaderRuntimeJson::WriteListString(
	const TArray<FBPRRAssetSummary>& Assets, bool bPretty)
{
	TArray<TSharedPtr<FJsonValue>> Arr;
	for (const FBPRRAssetSummary& A : Assets)
	{
		Arr.Add(MakeShared<FJsonValueObject>(SummaryToJson(A)));
	}
	FString Out;
	if (bPretty)
	{
		TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Arr, W);
	}
	else
	{
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> W =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Arr, W);
	}
	return Out;
}

FString FBlueprintReaderRuntimeJson::WriteObjectString(
	const TSharedRef<FJsonObject>& Object, bool bPretty)
{
	FString Out;
	if (bPretty)
	{
		TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Object, W);
	}
	else
	{
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> W =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Object, W);
	}
	return Out;
}
