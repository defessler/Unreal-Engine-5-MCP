#include "BlueprintReaderWireJson.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Class.h"

namespace
{
	// Strip the object suffix to convert /Game/Foo/Bar.Bar -> /Game/Foo/Bar.
	// Wire format consistently uses package paths, not object paths.
	FString ToPackagePath(const FString& Maybe)
	{
		int32 DotIdx = INDEX_NONE;
		if (Maybe.FindChar(TEXT('.'), DotIdx))
		{
			return Maybe.Left(DotIdx);
		}
		return Maybe;
	}

	TSharedPtr<FJsonValue> StringOrNull(const FString& Str)
	{
		if (Str.IsEmpty()) return MakeShared<FJsonValueNull>();
		return MakeShared<FJsonValueString>(Str);
	}

	void SetStringOrNull(TSharedRef<FJsonObject> Obj, const FString& Key, const FString& Value)
	{
		if (Value.IsEmpty())
		{
			Obj->SetField(Key, MakeShared<FJsonValueNull>());
		}
		else
		{
			Obj->SetStringField(Key, Value);
		}
	}

	TSharedRef<FJsonObject> StructuredPinTypeToJson(const FBPStructuredPinType& T)
	{
		auto Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("category"), T.Category);
		SetStringOrNull(Obj, TEXT("sub_category"), T.SubCategory);
		SetStringOrNull(Obj, TEXT("sub_category_object"), T.SubCategoryObject);
		Obj->SetBoolField(TEXT("is_array"), T.bIsArray);
		Obj->SetBoolField(TEXT("is_set"), T.bIsSet);
		Obj->SetBoolField(TEXT("is_map"), T.bIsMap);
		return Obj;
	}

	TSharedRef<FJsonObject> VariableToJson(const FBPVariableInfo& V)
	{
		auto Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), V.Name);
		Obj->SetObjectField(TEXT("type"), StructuredPinTypeToJson(V.StructuredType));
		SetStringOrNull(Obj, TEXT("default_value"), V.DefaultValue);
		SetStringOrNull(Obj, TEXT("category"), V.Category);
		Obj->SetBoolField(TEXT("is_replicated"), V.bIsReplicated);
		Obj->SetBoolField(TEXT("is_editable"), V.bIsEditable);
		return Obj;
	}

	TSharedRef<FJsonObject> PinToJson(const FBPPinInfo& P)
	{
		auto Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("id"), P.PinId);
		Obj->SetStringField(TEXT("name"), P.Name);
		Obj->SetStringField(TEXT("direction"), P.Direction);
		Obj->SetObjectField(TEXT("type"), StructuredPinTypeToJson(P.StructuredType));
		SetStringOrNull(Obj, TEXT("default_value"), P.DefaultValue);
		// Emit each pin's outgoing/incoming connections inline so a
		// `get_node` (or a fields-filtered `get_graph`) call gives the
		// caller enough information to verify wiring without a separate
		// `connections[]` lookup (issue #5). Each entry carries:
		//   - node_id : GUID of the node the link goes to
		//   - pin_id  : GUID of the pin on that node
		//   - pin_name: human-readable name of the linked pin
		// pin_name is additive — it lets an agent verify "is this pin
		// connected to a slot named X?" without another get_node call,
		// while pin_id remains the canonical reference for follow-up ops.
		TArray<TSharedPtr<FJsonValue>> LinkedTo;
		LinkedTo.Reserve(P.LinkedTo.Num());
		for (const FBPPinLinkInfo& Link : P.LinkedTo)
		{
			auto L = MakeShared<FJsonObject>();
			L->SetStringField(TEXT("node_id"), Link.NodeGuid);
			L->SetStringField(TEXT("pin_id"),
				Link.PinId.IsEmpty() ? Link.PinName : Link.PinId);
			L->SetStringField(TEXT("pin_name"), Link.PinName);
			LinkedTo.Add(MakeShared<FJsonValueObject>(L));
		}
		Obj->SetArrayField(TEXT("linked_to"), LinkedTo);
		return Obj;
	}

	TSharedRef<FJsonObject> NodeMetaToJson(const TMap<FString, FString>& Extras)
	{
		// `meta` is an inline JSON object in the wire format — never a string.
		// Plugin-side extras are string→string; values are emitted verbatim as
		// JSON strings. (Wire format permits arbitrary JSON values, but the
		// plugin currently only produces strings. Consumers must tolerate.)
		auto Obj = MakeShared<FJsonObject>();
		for (const TPair<FString, FString>& KV : Extras)
		{
			Obj->SetStringField(KV.Key, KV.Value);
		}
		return Obj;
	}

	TSharedRef<FJsonObject> NodeToJson(const FBPNodeInfo& N)
	{
		auto Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("id"), N.Guid);
		Obj->SetStringField(TEXT("class"), N.ClassName);
		Obj->SetStringField(TEXT("title"), N.Title);

		auto Position = MakeShared<FJsonObject>();
		Position->SetNumberField(TEXT("x"), N.PosX);
		Position->SetNumberField(TEXT("y"), N.PosY);
		Obj->SetObjectField(TEXT("position"), Position);

		SetStringOrNull(Obj, TEXT("comment"), N.Comment);

		TArray<TSharedPtr<FJsonValue>> Pins;
		Pins.Reserve(N.Pins.Num());
		for (const FBPPinInfo& P : N.Pins)
		{
			Pins.Add(MakeShared<FJsonValueObject>(PinToJson(P)));
		}
		Obj->SetArrayField(TEXT("pins"), Pins);

		Obj->SetObjectField(TEXT("meta"), NodeMetaToJson(N.Extras));
		return Obj;
	}

	TArray<TSharedPtr<FJsonValue>> ConnectionsForGraph(const FBPGraphInfo& Graph)
	{
		// Synthesize connections from each pin's LinkedTo list. We only emit
		// each edge once: the canonical direction is from output → input, so
		// walk pins, emit only when the source pin is an Output.
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const FBPNodeInfo& N : Graph.Nodes)
		{
			for (const FBPPinInfo& P : N.Pins)
			{
				if (P.Direction != TEXT("Output")) continue;
				for (const FBPPinLinkInfo& Link : P.LinkedTo)
				{
					auto Conn = MakeShared<FJsonObject>();
					Conn->SetStringField(TEXT("from_node"), N.Guid);
					Conn->SetStringField(TEXT("from_pin"), P.PinId);
					Conn->SetStringField(TEXT("to_node"), Link.NodeGuid);
					// Prefer the linked pin's GUID; fall back to its name if
					// the introspector didn't capture an id (defensive).
					Conn->SetStringField(TEXT("to_pin"),
						Link.PinId.IsEmpty() ? Link.PinName : Link.PinId);
					Out.Add(MakeShared<FJsonValueObject>(Conn));
				}
			}
		}
		return Out;
	}

	TSharedRef<FJsonObject> GraphInfoToJson(const FBPGraphInfo& G)
	{
		auto Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), G.Name);
		Obj->SetStringField(TEXT("type"), G.WireType);

		TArray<TSharedPtr<FJsonValue>> Nodes;
		Nodes.Reserve(G.Nodes.Num());
		for (const FBPNodeInfo& N : G.Nodes)
		{
			Nodes.Add(MakeShared<FJsonValueObject>(NodeToJson(N)));
		}
		Obj->SetArrayField(TEXT("nodes"), Nodes);
		Obj->SetArrayField(TEXT("connections"), ConnectionsForGraph(G));
		return Obj;
	}

	const FBPGraphInfo* FindGraphByName(const FBlueprintInfo& Info, const FString& Name)
	{
		auto Search = [&](const TArray<FBPGraphInfo>& Graphs) -> const FBPGraphInfo*
		{
			for (const FBPGraphInfo& G : Graphs)
			{
				if (G.Name.Equals(Name, ESearchCase::IgnoreCase))
				{
					return &G;
				}
			}
			return nullptr;
		};
		if (auto* G = Search(Info.EventGraphs))               return G;
		if (auto* G = Search(Info.FunctionGraphs))            return G;
		if (auto* G = Search(Info.MacroGraphs))               return G;
		if (auto* G = Search(Info.DelegateSignatureGraphs))   return G;
		return nullptr;
	}

	// Build BPVariables for a function's inputs/outputs by reading the
	// FunctionEntry/FunctionResult node pins. Inputs come from FunctionEntry
	// output pins (excluding 'then'); Outputs come from FunctionResult input
	// pins (excluding 'execute'). Locals come from the graph's LocalVariables.
	void BuildSignature(const FBPGraphInfo& Graph,
	                    TArray<FBPVariableInfo>& OutInputs,
	                    TArray<FBPVariableInfo>& OutOutputs)
	{
		auto IsExecPin = [](const FBPPinInfo& P)
		{
			return P.Name.Equals(TEXT("then"), ESearchCase::IgnoreCase) ||
			       P.Name.Equals(TEXT("execute"), ESearchCase::IgnoreCase);
		};

		for (const FBPNodeInfo& N : Graph.Nodes)
		{
			const bool bIsEntry  = (N.ClassName == TEXT("K2Node_FunctionEntry"));
			const bool bIsResult = (N.ClassName == TEXT("K2Node_FunctionResult"));
			if (!bIsEntry && !bIsResult) continue;

			for (const FBPPinInfo& P : N.Pins)
			{
				if (IsExecPin(P)) continue;
				FBPVariableInfo V;
				V.Name = P.Name;
				V.Type = P.Type;
				V.StructuredType = P.StructuredType;
				V.DefaultValue = P.DefaultValue;
				V.bIsReplicated = false;
				V.bIsEditable   = false;
				if (bIsEntry  && P.Direction == TEXT("Output")) OutInputs.Add(MoveTemp(V));
				if (bIsResult && P.Direction == TEXT("Input"))  OutOutputs.Add(MoveTemp(V));
			}
		}
	}

	bool NodeKindMatches(const FBPNodeInfo& N, const FString& LowerKind)
	{
		if (LowerKind.IsEmpty()) return true;
		const FString* Kind = N.Extras.Find(TEXT("kind"));
		if (!Kind) return false;
		return Kind->ToLower() == LowerKind;
	}

	void GatherNodesMatching(const FBPGraphInfo& Graph,
	                         const FString& LowerQuery,
	                         const FString& LowerKind,
	                         TArray<TSharedPtr<FJsonValue>>& Out)
	{
		for (const FBPNodeInfo& N : Graph.Nodes)
		{
			if (!NodeKindMatches(N, LowerKind)) continue;
			if (!LowerQuery.IsEmpty())
			{
				const bool bClassMatch = N.ClassName.ToLower().Contains(LowerQuery);
				const bool bTitleMatch = N.Title.ToLower().Contains(LowerQuery);
				// Also match against meta.targetFunction (CallFunction /
				// CallParentFunction) and meta.targetVariable
				// (VariableGet/Set). Operator nodes like
				// K2Node_CallFunction whose function is Greater_IntInt
				// render as "integer > integer" — the agent that just
				// spawned the node via `add_node function=Greater_IntInt`
				// would naturally search for "Greater" and miss the hit
				// without this fallback (issue #12).
				bool bExtrasMatch = false;
				if (!bClassMatch && !bTitleMatch)
				{
					// Accept both camelCase (the canonical plugin emit
					// names) and snake_case (the mock-fixture variants)
					// — keeps live + mock backends matching identically
					// regardless of which side the query goes through.
					// Coverage spans the three main kinds of K2 node
					// whose underlying identifier differs from the
					// rendered title:
					//   - CallFunction / CallParentFunction → targetFunction
					//   - VariableGet / VariableSet → variableName
					//   - Event / CustomEvent → eventName
					const TCHAR* ExtrasKeys[] = {
						TEXT("targetFunction"), TEXT("function_name"),
						TEXT("variableName"),   TEXT("variable_name"),
						TEXT("eventName"),      TEXT("event_name"),
					};
					for (const TCHAR* Key : ExtrasKeys)
					{
						if (const FString* V = N.Extras.Find(Key))
						{
							if (V->ToLower().Contains(LowerQuery))
							{
								bExtrasMatch = true;
								break;
							}
						}
					}
				}
				if (!bClassMatch && !bTitleMatch && !bExtrasMatch) continue;
			}
			// find_node spans every graph in the blueprint, so each hit
			// needs to carry the graph it came from — otherwise the
			// agent has no way to call get_node / delete_node / wire_pins
			// (which all require -Graph=) on the result (issue #6).
			TSharedRef<FJsonObject> NodeObj = NodeToJson(N);
			NodeObj->SetStringField(TEXT("graph_name"), Graph.Name);
			NodeObj->SetStringField(TEXT("graph_type"), Graph.WireType);
			Out.Add(MakeShared<FJsonValueObject>(NodeObj));
		}
	}
}

TSharedRef<FJsonObject> FBlueprintReaderWireJson::MetadataToJson(const FBlueprintInfo& Info)
{
	auto Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("asset_path"), ToPackagePath(Info.Path));
	Obj->SetStringField(TEXT("name"), Info.Name);
	Obj->SetStringField(TEXT("parent_class"), Info.ParentClassPath);

	TArray<TSharedPtr<FJsonValue>> Interfaces;
	for (const FBPInterfaceInfo& I : Info.Interfaces)
	{
		Interfaces.Add(MakeShared<FJsonValueString>(I.InterfacePath));
	}
	Obj->SetArrayField(TEXT("interfaces"), Interfaces);

	TArray<TSharedPtr<FJsonValue>> Variables;
	for (const FBPVariableInfo& V : Info.Variables)
	{
		Variables.Add(MakeShared<FJsonValueObject>(VariableToJson(V)));
	}
	Obj->SetArrayField(TEXT("variables"), Variables);

	TArray<TSharedPtr<FJsonValue>> Functions;
	// Function summaries only — no signature/locals at the metadata level.
	for (const FBPGraphInfo& G : Info.FunctionGraphs)
	{
		// Skip ConstructionScript here — it's reported separately under graphs.
		if (G.WireType == TEXT("Construction")) continue;
		auto FnObj = MakeShared<FJsonObject>();
		FnObj->SetStringField(TEXT("name"), G.Name);
		Functions.Add(MakeShared<FJsonValueObject>(FnObj));
	}
	Obj->SetArrayField(TEXT("functions"), Functions);

	TArray<TSharedPtr<FJsonValue>> Macros;
	for (const FBPGraphInfo& G : Info.MacroGraphs)
	{
		Macros.Add(MakeShared<FJsonValueString>(G.Name));
	}
	Obj->SetArrayField(TEXT("macros"), Macros);

	TArray<TSharedPtr<FJsonValue>> Graphs;
	auto AddGraphSummary = [&](const FBPGraphInfo& G)
	{
		auto S = MakeShared<FJsonObject>();
		S->SetStringField(TEXT("name"), G.Name);
		S->SetStringField(TEXT("type"), G.WireType);
		Graphs.Add(MakeShared<FJsonValueObject>(S));
	};
	for (const FBPGraphInfo& G : Info.EventGraphs)             AddGraphSummary(G);
	for (const FBPGraphInfo& G : Info.FunctionGraphs)
	{
		// Surface ConstructionScript as a graph summary (matches the wire fixture shape).
		if (G.WireType == TEXT("Construction")) AddGraphSummary(G);
	}
	Obj->SetArrayField(TEXT("graphs"), Graphs);
	return Obj;
}

TSharedPtr<FJsonObject> FBlueprintReaderWireJson::GraphToJson(const FBlueprintInfo& Info, const FString& GraphName)
{
	const FBPGraphInfo* G = FindGraphByName(Info, GraphName);
	if (!G) return nullptr;
	return GraphInfoToJson(*G);
}

TSharedPtr<FJsonObject> FBlueprintReaderWireJson::FunctionToJson(const FBlueprintInfo& Info, const FString& FunctionName)
{
	for (const FBPGraphInfo& G : Info.FunctionGraphs)
	{
		if (!G.Name.Equals(FunctionName, ESearchCase::IgnoreCase)) continue;

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), G.Name);

		TArray<FBPVariableInfo> Inputs;
		TArray<FBPVariableInfo> Outputs;
		BuildSignature(G, Inputs, Outputs);

		auto VarsToJson = [&](const TArray<FBPVariableInfo>& Vs)
		{
			TArray<TSharedPtr<FJsonValue>> Out;
			Out.Reserve(Vs.Num());
			for (const FBPVariableInfo& V : Vs)
			{
				Out.Add(MakeShared<FJsonValueObject>(VariableToJson(V)));
			}
			return Out;
		};
		Obj->SetArrayField(TEXT("inputs"),  VarsToJson(Inputs));
		Obj->SetArrayField(TEXT("outputs"), VarsToJson(Outputs));
		Obj->SetArrayField(TEXT("locals"),  VarsToJson(G.LocalVariables));

		// The function's graph is a Function-typed wire BPGraph.
		FBPGraphInfo GraphCopy = G;
		GraphCopy.WireType = TEXT("Function");
		Obj->SetObjectField(TEXT("graph"), GraphInfoToJson(GraphCopy));
		return Obj;
	}
	return nullptr;
}

TArray<TSharedPtr<FJsonValue>> FBlueprintReaderWireJson::VariablesToJson(const FBlueprintInfo& Info)
{
	TArray<TSharedPtr<FJsonValue>> Out;
	Out.Reserve(Info.Variables.Num());
	for (const FBPVariableInfo& V : Info.Variables)
	{
		Out.Add(MakeShared<FJsonValueObject>(VariableToJson(V)));
	}
	return Out;
}

TArray<TSharedPtr<FJsonValue>> FBlueprintReaderWireJson::ComponentsToJson(const FBlueprintInfo& Info)
{
	TArray<TSharedPtr<FJsonValue>> Out;
	Out.Reserve(Info.Components.Num());
	for (const FBPComponentInfo& C : Info.Components)
	{
		auto Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), C.Name);
		Obj->SetStringField(TEXT("class"), C.ClassPath);
		SetStringOrNull(Obj, TEXT("parent"), C.ParentName);
		Obj->SetBoolField(TEXT("is_root"), C.bIsRoot);
		Out.Add(MakeShared<FJsonValueObject>(Obj));
	}
	return Out;
}

TArray<TSharedPtr<FJsonValue>> FBlueprintReaderWireJson::FindNodesAsJson(const FBlueprintInfo& Info,
                                                                         const FString& Query,
                                                                         const FString& Kind)
{
	const FString LowerQuery = Query.ToLower();
	const FString LowerKind  = Kind.ToLower();
	TArray<TSharedPtr<FJsonValue>> Out;
	for (const FBPGraphInfo& G : Info.EventGraphs)             GatherNodesMatching(G, LowerQuery, LowerKind, Out);
	for (const FBPGraphInfo& G : Info.FunctionGraphs)          GatherNodesMatching(G, LowerQuery, LowerKind, Out);
	for (const FBPGraphInfo& G : Info.MacroGraphs)             GatherNodesMatching(G, LowerQuery, LowerKind, Out);
	for (const FBPGraphInfo& G : Info.DelegateSignatureGraphs) GatherNodesMatching(G, LowerQuery, LowerKind, Out);
	return Out;
}

TSharedRef<FJsonObject> FBlueprintReaderWireJson::SummaryToJson(const FBlueprintInfo& Info, const FString& ModifiedIso)
{
	return SummaryToJson(Info.Path, Info.Name, Info.ParentClassPath, ModifiedIso);
}

TSharedRef<FJsonObject> FBlueprintReaderWireJson::SummaryToJson(const FString& AssetPath,
                                                                const FString& Name,
                                                                const FString& ParentClassPath,
                                                                const FString& ModifiedIso)
{
	auto Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("asset_path"), ToPackagePath(AssetPath));
	Obj->SetStringField(TEXT("name"), Name);
	Obj->SetStringField(TEXT("parent_class"), ParentClassPath);
	Obj->SetStringField(TEXT("modified_iso"), ModifiedIso);
	return Obj;
}

FString FBlueprintReaderWireJson::WriteString(const TSharedRef<FJsonObject>& Object, bool bPretty)
{
	FString Out;
	if (bPretty)
	{
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Object, Writer);
	}
	else
	{
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Object, Writer);
	}
	return Out;
}

bool FBlueprintReaderWireJson::ParseWirePinType(const TSharedPtr<FJsonObject>& Json, FEdGraphPinType& OutType)
{
	if (!Json.IsValid()) return false;

	FString Category;
	if (!Json->TryGetStringField(TEXT("category"), Category) || Category.IsEmpty())
	{
		return false;
	}
	OutType.PinCategory = FName(*Category);

	FString SubCategory;
	if (Json->TryGetStringField(TEXT("sub_category"), SubCategory) && !SubCategory.IsEmpty())
	{
		OutType.PinSubCategory = FName(*SubCategory);
	}

	FString SubObjectPath;
	if (Json->TryGetStringField(TEXT("sub_category_object"), SubObjectPath) && !SubObjectPath.IsEmpty())
	{
		// Try to resolve the path as a UClass first (most common for object/class pins).
		// Fall back to UScriptStruct / UEnum for struct/enum pins.
		UObject* Resolved = StaticLoadObject(UClass::StaticClass(), nullptr, *SubObjectPath);
		if (!Resolved)
		{
			Resolved = StaticLoadObject(UScriptStruct::StaticClass(), nullptr, *SubObjectPath);
		}
		if (!Resolved)
		{
			Resolved = StaticLoadObject(UEnum::StaticClass(), nullptr, *SubObjectPath);
		}
		if (!Resolved)
		{
			Resolved = StaticLoadObject(UObject::StaticClass(), nullptr, *SubObjectPath);
		}
		if (Resolved)
		{
			OutType.PinSubCategoryObject = Resolved;
		}
	}

	bool bArr = false, bSet = false, bMap = false;
	Json->TryGetBoolField(TEXT("is_array"), bArr);
	Json->TryGetBoolField(TEXT("is_set"),   bSet);
	Json->TryGetBoolField(TEXT("is_map"),   bMap);
	if (bArr)      OutType.ContainerType = EPinContainerType::Array;
	else if (bSet) OutType.ContainerType = EPinContainerType::Set;
	else if (bMap) OutType.ContainerType = EPinContainerType::Map;
	else           OutType.ContainerType = EPinContainerType::None;

	return true;
}

FString FBlueprintReaderWireJson::WriteArrayString(const TArray<TSharedPtr<FJsonValue>>& Array, bool bPretty)
{
	FString Out;
	if (bPretty)
	{
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Array, Writer);
	}
	else
	{
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Array, Writer);
	}
	return Out;
}
