#include "BlueprintReaderJson.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	TSharedRef<FJsonObject> ToJson(const FBPPinLinkInfo& Link)
	{
		auto Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("nodeGuid"), Link.NodeGuid);
		Obj->SetStringField(TEXT("pinName"), Link.PinName);
		return Obj;
	}

	TSharedRef<FJsonObject> ToJson(const FBPPinInfo& Pin)
	{
		auto Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Pin.Name);
		Obj->SetStringField(TEXT("direction"), Pin.Direction);
		Obj->SetStringField(TEXT("type"), Pin.Type);
		Obj->SetStringField(TEXT("default"), Pin.DefaultValue);
		Obj->SetStringField(TEXT("defaultObject"), Pin.DefaultObjectPath);
		Obj->SetStringField(TEXT("defaultText"), Pin.DefaultText);
		Obj->SetBoolField(TEXT("hidden"), Pin.bIsHidden);
		Obj->SetBoolField(TEXT("reference"), Pin.bIsReference);
		Obj->SetBoolField(TEXT("const"), Pin.bIsConst);

		TArray<TSharedPtr<FJsonValue>> Links;
		Links.Reserve(Pin.LinkedTo.Num());
		for (const FBPPinLinkInfo& L : Pin.LinkedTo)
		{
			Links.Add(MakeShared<FJsonValueObject>(ToJson(L)));
		}
		Obj->SetArrayField(TEXT("linkedTo"), Links);
		return Obj;
	}

	TSharedRef<FJsonObject> ToJson(const FBPNodeInfo& Node)
	{
		auto Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("guid"), Node.Guid);
		Obj->SetStringField(TEXT("class"), Node.ClassName);
		Obj->SetStringField(TEXT("title"), Node.Title);
		Obj->SetStringField(TEXT("comment"), Node.Comment);
		Obj->SetNumberField(TEXT("posX"), Node.PosX);
		Obj->SetNumberField(TEXT("posY"), Node.PosY);
		Obj->SetBoolField(TEXT("enabled"), Node.bEnabled);

		TArray<TSharedPtr<FJsonValue>> Pins;
		Pins.Reserve(Node.Pins.Num());
		for (const FBPPinInfo& P : Node.Pins)
		{
			Pins.Add(MakeShared<FJsonValueObject>(ToJson(P)));
		}
		Obj->SetArrayField(TEXT("pins"), Pins);

		auto Extras = MakeShared<FJsonObject>();
		for (const TPair<FString, FString>& KV : Node.Extras)
		{
			Extras->SetStringField(KV.Key, KV.Value);
		}
		Obj->SetObjectField(TEXT("extras"), Extras);
		return Obj;
	}

	TSharedRef<FJsonObject> ToJson(const FBPVariableInfo& Var);

	TSharedRef<FJsonObject> ToJson(const FBPGraphInfo& Graph)
	{
		auto Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Graph.Name);
		Obj->SetStringField(TEXT("schema"), Graph.SchemaPath);

		TArray<TSharedPtr<FJsonValue>> Nodes;
		Nodes.Reserve(Graph.Nodes.Num());
		for (const FBPNodeInfo& N : Graph.Nodes)
		{
			Nodes.Add(MakeShared<FJsonValueObject>(ToJson(N)));
		}
		Obj->SetArrayField(TEXT("nodes"), Nodes);

		TArray<TSharedPtr<FJsonValue>> Locals;
		Locals.Reserve(Graph.LocalVariables.Num());
		for (const FBPVariableInfo& V : Graph.LocalVariables)
		{
			Locals.Add(MakeShared<FJsonValueObject>(ToJson(V)));
		}
		Obj->SetArrayField(TEXT("localVariables"), Locals);
		return Obj;
	}

	TSharedRef<FJsonObject> ToJson(const FBPVariableInfo& Var)
	{
		auto Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Var.Name);
		Obj->SetStringField(TEXT("friendlyName"), Var.FriendlyName);
		Obj->SetStringField(TEXT("category"), Var.Category);
		Obj->SetStringField(TEXT("type"), Var.Type);
		Obj->SetStringField(TEXT("default"), Var.DefaultValue);
		Obj->SetBoolField(TEXT("replicated"), Var.bIsReplicated);
		Obj->SetBoolField(TEXT("transient"), Var.bIsTransient);
		Obj->SetBoolField(TEXT("editable"), Var.bIsEditable);
		Obj->SetBoolField(TEXT("blueprintReadOnly"), Var.bIsBlueprintReadOnly);
		Obj->SetBoolField(TEXT("exposeOnSpawn"), Var.bIsExposeOnSpawn);
		return Obj;
	}

	TSharedRef<FJsonObject> ToJson(const FBPComponentInfo& Comp)
	{
		auto Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Comp.Name);
		Obj->SetStringField(TEXT("class"), Comp.ClassPath);
		Obj->SetStringField(TEXT("parent"), Comp.ParentName);
		Obj->SetBoolField(TEXT("root"), Comp.bIsRoot);
		return Obj;
	}

	template <typename T>
	TArray<TSharedPtr<FJsonValue>> ArrayToJson(const TArray<T>& Items)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		Out.Reserve(Items.Num());
		for (const T& Item : Items)
		{
			Out.Add(MakeShared<FJsonValueObject>(ToJson(Item)));
		}
		return Out;
	}
}

TSharedRef<FJsonObject> FBlueprintReaderJson::ToJson(const FBlueprintInfo& Info)
{
	auto Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("path"), Info.Path);
	Obj->SetStringField(TEXT("name"), Info.Name);
	Obj->SetStringField(TEXT("blueprintType"), Info.BlueprintType);
	Obj->SetStringField(TEXT("parentClass"), Info.ParentClassPath);
	Obj->SetStringField(TEXT("generatedClass"), Info.GeneratedClassPath);

	TArray<TSharedPtr<FJsonValue>> Interfaces;
	Interfaces.Reserve(Info.Interfaces.Num());
	for (const FBPInterfaceInfo& I : Info.Interfaces)
	{
		Interfaces.Add(MakeShared<FJsonValueString>(I.InterfacePath));
	}
	Obj->SetArrayField(TEXT("interfaces"), Interfaces);

	Obj->SetArrayField(TEXT("variables"),  ArrayToJson(Info.Variables));
	Obj->SetArrayField(TEXT("components"), ArrayToJson(Info.Components));
	Obj->SetArrayField(TEXT("functionGraphs"),         ArrayToJson(Info.FunctionGraphs));
	Obj->SetArrayField(TEXT("eventGraphs"),            ArrayToJson(Info.EventGraphs));
	Obj->SetArrayField(TEXT("macroGraphs"),            ArrayToJson(Info.MacroGraphs));
	Obj->SetArrayField(TEXT("delegateSignatureGraphs"),ArrayToJson(Info.DelegateSignatureGraphs));
	return Obj;
}

FString FBlueprintReaderJson::ToString(const FBlueprintInfo& Info, bool bPretty)
{
	const TSharedRef<FJsonObject> Json = ToJson(Info);
	FString Out;
	if (bPretty)
	{
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Json, Writer);
	}
	else
	{
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Json, Writer);
	}
	return Out;
}
