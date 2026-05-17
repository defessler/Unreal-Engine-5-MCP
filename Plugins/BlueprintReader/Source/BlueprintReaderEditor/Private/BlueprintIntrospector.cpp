#include "BlueprintIntrospector.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "UObject/Class.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectGlobals.h"

#include "K2Node_BaseAsyncTask.h"
#include "K2Node_BaseMCDelegate.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallParentFunction.h"
#include "K2Node_Composite.h"
#include "K2Node_CreateDelegate.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_Event.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_FormatText.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_Knot.h"
#include "K2Node_Literal.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_MakeArray.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_Self.h"
#include "K2Node_Switch.h"
#include "K2Node_Timeline.h"
#include "K2Node_Tunnel.h"
#include "K2Node_Variable.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"

namespace
{
	FString BoolToString(bool b) { return b ? TEXT("true") : TEXT("false"); }

	FString TerminalToString(const FName& Category, const FName& SubCategory, const TWeakObjectPtr<UObject>& SubObject)
	{
		FString Out = Category.ToString();
		if (!SubCategory.IsNone())
		{
			Out += TEXT(":") + SubCategory.ToString();
		}
		if (UObject* Resolved = SubObject.Get())
		{
			Out += TEXT("(") + Resolved->GetPathName() + TEXT(")");
		}
		return Out;
	}

	FBPVariableInfo VariableDescToInfo(const FBPVariableDescription& Var)
	{
		FBPVariableInfo V;
		V.Name = Var.VarName.ToString();
		V.FriendlyName = Var.FriendlyName;
		V.Category = Var.Category.ToString();
		V.Type = FBlueprintIntrospector::FormatPinType(Var.VarType);
		V.StructuredType = FBlueprintIntrospector::MakeStructuredPinType(Var.VarType);
		V.DefaultValue = Var.DefaultValue;
		V.bIsReplicated        = (Var.PropertyFlags & CPF_Net) != 0;
		V.bIsTransient         = (Var.PropertyFlags & CPF_Transient) != 0;
		V.bIsEditable          = (Var.PropertyFlags & CPF_Edit) != 0;
		V.bIsBlueprintReadOnly = (Var.PropertyFlags & CPF_BlueprintReadOnly) != 0;
		V.bIsExposeOnSpawn     = Var.HasMetaData(TEXT("ExposeOnSpawn"));
		return V;
	}

	// Read delegate-signature params off a BP's generated class for a
	// multicast-delegate variable. Populates V.DelegateParams when the
	// property is one of FMulticastInlineDelegateProperty /
	// FMulticastSparseDelegateProperty (BP's typical mcdelegate kinds).
	// No-op when the type is anything else. Used by codegen to emit the
	// matching _NParams DECLARE variant.
	void PopulateDelegateParams(FBPVariableInfo& V, UClass* GeneratedClass)
	{
		if (!GeneratedClass)
		{
			return;
		}
		FProperty* Prop = GeneratedClass->FindPropertyByName(*V.Name);
		if (!Prop)
		{
			return;
		}
		FMulticastDelegateProperty* DelProp = CastField<FMulticastDelegateProperty>(Prop);
		if (!DelProp || !DelProp->SignatureFunction)
		{
			return;
		}
		const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
		if (!IsValid(K2Schema))
		{
			return;
		}
		for (TFieldIterator<FProperty> It(DelProp->SignatureFunction); It; ++It)
		{
			FProperty* Param = *It;
			if (!Param || !Param->HasAnyPropertyFlags(CPF_Parm))
			{
				continue;
			}
			if (Param->HasAnyPropertyFlags(CPF_ReturnParm))
			{
				continue;
			}
			FEdGraphPinType PinType;
			if (!K2Schema->ConvertPropertyToPinType(Param, PinType))
			{
				continue;
			}
			FBPDelegateParam DP;
			DP.Name = Param->GetName();
			DP.Type = FBlueprintIntrospector::FormatPinType(PinType);
			V.DelegateParams.Add(MoveTemp(DP));
		}
	}

	// For K2Node_BaseAsyncTask nodes, walk the proxy class's multicast
	// delegate UPROPERTYs and surface each delegate's parameter list as
	// a JSON-encoded string under the node's "delegate_params" extra.
	// This unblocks the MCP-side async-task auto-lowering — without per-
	// delegate-signature param info, generated UFUNCTION() callbacks
	// can't carry the matching `(const FXxxPayload&)` signature.
	//
	// Wire format: a JSON object keyed by delegate name (which matches
	// the node's output exec pin name), each value being an array of
	// `{name, type}` objects where `type` is in the BPIR-shorthand form.
	//
	// Encoded as a string field on `Extras` because `Extras` is a
	// `TMap<FString,FString>` — nested-shape data must be JSON-stringified.
	// The MCP-side decompile parses it back.
	void ExtractAsyncTaskDelegateParams(UK2Node_BaseAsyncTask* AsyncNode,
										TMap<FString, FString>& Extras)
	{
		if (!IsValid(AsyncNode))
		{
			return;
		}
		// ProxyClass is a protected member of UK2Node_BaseAsyncTask, so
		// derive it from the factory function's return type instead.
		// Factories conventionally return `<ProxyClass>*`.
		UFunction* Factory = AsyncNode->GetFactoryFunction();
		if (!IsValid(Factory))
		{
			return;
		}
		FProperty* RetProp = Factory->GetReturnProperty();
		FObjectProperty* ObjRetProp = CastField<FObjectProperty>(RetProp);
		if (!ObjRetProp)
		{
			return;
		}
		UClass* ProxyClass = ObjRetProp->PropertyClass;
		if (!IsValid(ProxyClass))
		{
			return;
		}
		const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
		if (!IsValid(K2Schema))
		{
			return;
		}

		// Build the nested structure manually as a JSON-stringified
		// payload. Format: {"OnSuccess":[{"name":"Result","type":"object:Pawn"},...]}.
		FString Json = TEXT("{");
		bool bAnyDelegate = false;
		for (TFieldIterator<FMulticastDelegateProperty> PropIt(ProxyClass); PropIt; ++PropIt)
		{
			FMulticastDelegateProperty* DelProp = *PropIt;
			if (!DelProp || !DelProp->SignatureFunction)
			{
				continue;
			}
			// Pin name == delegate name on the BaseAsyncTask node.
			const FString PinName = DelProp->GetName();

			FString ParamsArr = TEXT("[");
			bool bAnyParam = false;
			for (TFieldIterator<FProperty> ParamIt(DelProp->SignatureFunction); ParamIt; ++ParamIt)
			{
				FProperty* Param = *ParamIt;
				if (!Param || !Param->HasAnyPropertyFlags(CPF_Parm))
				{
					continue;
				}
				if (Param->HasAnyPropertyFlags(CPF_ReturnParm))
				{
					continue;
				}
				FEdGraphPinType PinType;
				if (!K2Schema->ConvertPropertyToPinType(Param, PinType))
				{
					continue;
				}
				if (bAnyParam)
				{
					ParamsArr += TEXT(",");
				}
				bAnyParam = true;
				// Escape param name in case it contains special chars (rare
				// for delegate signature params).
				FString EscapedName = Param->GetName().Replace(TEXT("\""), TEXT("\\\""));
				FString TypeStr = FBlueprintIntrospector::FormatPinType(PinType);
				FString EscapedType = TypeStr.Replace(TEXT("\""), TEXT("\\\""));
				ParamsArr += FString::Printf(TEXT("{\"name\":\"%s\",\"type\":\"%s\"}"),
											 *EscapedName, *EscapedType);
			}
			ParamsArr += TEXT("]");

			if (bAnyDelegate)
			{
				Json += TEXT(",");
			}
			bAnyDelegate = true;
			FString EscapedPin = PinName.Replace(TEXT("\""), TEXT("\\\""));
			Json += FString::Printf(TEXT("\"%s\":%s"), *EscapedPin, *ParamsArr);
		}
		Json += TEXT("}");
		if (bAnyDelegate)
		{
			Extras.Add(TEXT("delegate_params"), Json);
		}
	}

	// Return the bare C++ class name for an asset-ref property, with
	// the correct U/A prefix. Empty for non-asset-ref properties.
	// Codegen uses this to fill in `T` in `ConstructorHelpers::FObjectFinder<T>`.
	FString GetAssetRefPropertyClassName(FProperty* Prop)
	{
		if (!Prop)
		{
			return FString();
		}
		UClass* AssetClass = nullptr;
		if (FObjectProperty* OP = CastField<FObjectProperty>(Prop))
		{
			AssetClass = OP->PropertyClass;
		}
		else if (FClassProperty* CP = CastField<FClassProperty>(Prop))
		{
			AssetClass = CP->MetaClass;
		}
		else if (FSoftObjectProperty* SOP = CastField<FSoftObjectProperty>(Prop))
		{
			AssetClass = SOP->PropertyClass;
		}
		else if (FSoftClassProperty* SCP = CastField<FSoftClassProperty>(Prop))
		{
			AssetClass = SCP->MetaClass;
		}
		if (!IsValid(AssetClass))
		{
			return FString();
		}
		// AssetClass->GetName() returns the bare name without prefix
		// (e.g. "SkeletalMesh"). UE convention: U-prefix unless the
		// class is Actor-derived (then A-prefix). UClass::IsChildOf
		// gives us the test.
		const FString Bare = AssetClass->GetName();
		if (AssetClass->IsChildOf(AActor::StaticClass()))
		{
			return FString::Printf(TEXT("A%s"), *Bare);
		}
		return FString::Printf(TEXT("U%s"), *Bare);
	}

	void ExtractK2Extras(UEdGraphNode* Node, TMap<FString, FString>& Extras)
	{
		// CallParentFunction must come before CallFunction since it's a subclass.
		if (UK2Node_CallParentFunction* CallParent = Cast<UK2Node_CallParentFunction>(Node))
		{
			Extras.Add(TEXT("kind"), TEXT("CallParentFunction"));
			Extras.Add(TEXT("targetFunction"), CallParent->FunctionReference.GetMemberName().ToString());
			if (UClass* C = CallParent->FunctionReference.GetMemberParentClass())
			{
				Extras.Add(TEXT("targetClass"), C->GetPathName());
			}
			return;
		}
		if (UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
		{
			Extras.Add(TEXT("kind"), TEXT("CallFunction"));
			Extras.Add(TEXT("targetFunction"), Call->FunctionReference.GetMemberName().ToString());
			if (UClass* C = Call->FunctionReference.GetMemberParentClass())
			{
				Extras.Add(TEXT("targetClass"), C->GetPathName());
			}
			Extras.Add(TEXT("isSelfContext"), BoolToString(Call->FunctionReference.IsSelfContext()));
			// UK2Node_BaseAsyncTask is a UK2Node_CallFunction subclass.
			// Augment the extras with per-delegate parameter lists so the
			// MCP-side async-task auto-lowering can generate UFUNCTION
			// callbacks with matching `(const FXxxPayload&)` signatures
			// instead of parameter-less stubs.
			if (UK2Node_BaseAsyncTask* AsyncNode = Cast<UK2Node_BaseAsyncTask>(Node))
			{
				ExtractAsyncTaskDelegateParams(AsyncNode, Extras);
			}
			return;
		}
		if (UK2Node_VariableSet* VarSet = Cast<UK2Node_VariableSet>(Node))
		{
			Extras.Add(TEXT("kind"), TEXT("VariableSet"));
			Extras.Add(TEXT("variableName"), VarSet->VariableReference.GetMemberName().ToString());
			if (UClass* C = VarSet->VariableReference.GetMemberParentClass())
			{
				Extras.Add(TEXT("variableClass"), C->GetPathName());
			}
			return;
		}
		if (UK2Node_VariableGet* VarGet = Cast<UK2Node_VariableGet>(Node))
		{
			Extras.Add(TEXT("kind"), TEXT("VariableGet"));
			Extras.Add(TEXT("variableName"), VarGet->VariableReference.GetMemberName().ToString());
			if (UClass* C = VarGet->VariableReference.GetMemberParentClass())
			{
				Extras.Add(TEXT("variableClass"), C->GetPathName());
			}
			return;
		}
		if (UK2Node_CustomEvent* CustomEvt = Cast<UK2Node_CustomEvent>(Node))
		{
			Extras.Add(TEXT("kind"), TEXT("CustomEvent"));
			Extras.Add(TEXT("eventName"), CustomEvt->CustomFunctionName.ToString());
			return;
		}
		if (UK2Node_Event* Evt = Cast<UK2Node_Event>(Node))
		{
			Extras.Add(TEXT("kind"), TEXT("Event"));
			Extras.Add(TEXT("eventName"), Evt->EventReference.GetMemberName().ToString());
			if (UClass* C = Evt->EventReference.GetMemberParentClass())
			{
				Extras.Add(TEXT("eventClass"), C->GetPathName());
			}
			Extras.Add(TEXT("isOverride"), BoolToString(Evt->bOverrideFunction));
			return;
		}
		if (UK2Node_DynamicCast* CastNode = Cast<UK2Node_DynamicCast>(Node))
		{
			Extras.Add(TEXT("kind"), TEXT("DynamicCast"));
			if (UClass* T = CastNode->TargetType)
			{
				Extras.Add(TEXT("targetClass"), T->GetPathName());
			}
			else
			{
				// Broken cast — the target class was deleted, renamed, or
				// belongs to an uncompiled module. The node still serializes
				// (title becomes literally "Bad cast node") but is dead
				// code: the cast always fails at runtime, every downstream
				// branch is unreachable, and the output pin's type drops to
				// wildcard (sub_category_object: null). Surface a clear
				// signal so callers can detect this without title-string
				// matching (issue #7). The agent can search via
				//   find_node(kind="DynamicCast")
				// and then filter on meta.castBroken == "true".
				Extras.Add(TEXT("castBroken"), TEXT("true"));
			}
			Extras.Add(TEXT("isPureCast"), BoolToString(CastNode->IsNodePure()));
			return;
		}
		if (UK2Node_MacroInstance* Macro = Cast<UK2Node_MacroInstance>(Node))
		{
			Extras.Add(TEXT("kind"), TEXT("MacroInstance"));
			if (UEdGraph* G = Macro->GetMacroGraph())
			{
				Extras.Add(TEXT("macroGraph"), G->GetPathName());
				Extras.Add(TEXT("macroName"), G->GetFName().ToString());
			}
			return;
		}
		if (UK2Node_IfThenElse* Branch = Cast<UK2Node_IfThenElse>(Node))
		{
			(void)Branch;
			Extras.Add(TEXT("kind"), TEXT("Branch"));
			return;
		}
		if (UK2Node_ExecutionSequence* Seq = Cast<UK2Node_ExecutionSequence>(Node))
		{
			// Count output pins of category "exec" — those are the "Then 0/1/..."
			// outputs. UE doesn't expose a public GetNumOutputPins() on this class.
			int32 NumOutputs = 0;
			for (UEdGraphPin* P : Seq->Pins)
			{
				if (P && P->Direction == EGPD_Output && P->PinType.PinCategory == TEXT("exec"))
				{
					++NumOutputs;
				}
			}
			Extras.Add(TEXT("kind"), TEXT("Sequence"));
			Extras.Add(TEXT("numOutputs"), FString::FromInt(NumOutputs));
			return;
		}
		if (Node->IsA<UK2Node_Knot>())
		{
			Extras.Add(TEXT("kind"), TEXT("Knot"));
			return;
		}
		if (Node->IsA<UK2Node_Self>())
		{
			Extras.Add(TEXT("kind"), TEXT("Self"));
			return;
		}
		if (UK2Node_FormatText* Fmt = Cast<UK2Node_FormatText>(Node))
		{
			Extras.Add(TEXT("kind"), TEXT("FormatText"));
			Extras.Add(TEXT("numArgs"), FString::FromInt(Fmt->GetArgumentCount()));
			return;
		}
		if (UK2Node_MakeArray* MakeArr = Cast<UK2Node_MakeArray>(Node))
		{
			(void)MakeArr;
			Extras.Add(TEXT("kind"), TEXT("MakeArray"));
			return;
		}
		if (UK2Node_MakeStruct* MakeStruct = Cast<UK2Node_MakeStruct>(Node))
		{
			Extras.Add(TEXT("kind"), TEXT("MakeStruct"));
			if (UScriptStruct* SS = MakeStruct->StructType)
			{
				Extras.Add(TEXT("structType"), SS->GetPathName());
			}
			return;
		}
		if (UK2Node_Composite* Composite = Cast<UK2Node_Composite>(Node))
		{
			Extras.Add(TEXT("kind"), TEXT("Composite"));
			if (UEdGraph* G = Composite->BoundGraph)
			{
				Extras.Add(TEXT("subgraphName"), G->GetFName().ToString());
			}
			return;
		}
		if (Node->IsA<UK2Node_Tunnel>())
		{
			Extras.Add(TEXT("kind"), TEXT("Tunnel"));
			return;
		}
		if (UK2Node_Timeline* Timeline = Cast<UK2Node_Timeline>(Node))
		{
			Extras.Add(TEXT("kind"), TEXT("Timeline"));
			Extras.Add(TEXT("timelineName"), Timeline->TimelineName.ToString());
			return;
		}
		if (UK2Node_Switch* Switch = Cast<UK2Node_Switch>(Node))
		{
			(void)Switch;
			Extras.Add(TEXT("kind"), TEXT("Switch"));
			Extras.Add(TEXT("switchClass"), Node->GetClass()->GetName());
			return;
		}
		if (UK2Node_Literal* Lit = Cast<UK2Node_Literal>(Node))
		{
			Extras.Add(TEXT("kind"), TEXT("Literal"));
			if (UObject* Src = Lit->GetObjectRef())
			{
				Extras.Add(TEXT("literalObject"), Src->GetPathName());
			}
			return;
		}
		if (UK2Node_CreateDelegate* MakeDel = Cast<UK2Node_CreateDelegate>(Node))
		{
			Extras.Add(TEXT("kind"), TEXT("CreateDelegate"));
			Extras.Add(TEXT("delegateName"), MakeDel->GetFunctionName().ToString());
			return;
		}
		// K2Node_BaseMCDelegate is the base for Call/Add/Remove/Clear
		// delegate ops. The property name + owning class come off the
		// DelegateReference member; without surfacing them here, the
		// transpiler can't lower these nodes to Broadcast / AddDynamic
		// / RemoveDynamic / Clear because it has no way to recover the
		// property name from pin metadata alone.
		if (UK2Node_BaseMCDelegate* DelOp = Cast<UK2Node_BaseMCDelegate>(Node))
		{
			Extras.Add(TEXT("kind"), TEXT("DelegateOp"));
			Extras.Add(TEXT("delegateProperty"), DelOp->GetPropertyName().ToString());
			if (UClass* ScopeClass = DelOp->DelegateReference.GetMemberParentClass(nullptr))
			{
				Extras.Add(TEXT("delegateClass"), ScopeClass->GetName());
			}
			return;
		}
		if (Node->IsA<UK2Node_FunctionEntry>())
		{
			Extras.Add(TEXT("kind"), TEXT("FunctionEntry"));
			return;
		}
		if (Node->IsA<UK2Node_FunctionResult>())
		{
			Extras.Add(TEXT("kind"), TEXT("FunctionResult"));
			return;
		}
	}
}

FString FBlueprintIntrospector::FormatPinType(const FEdGraphPinType& T)
{
	const FString Terminal = TerminalToString(T.PinCategory, T.PinSubCategory, T.PinSubCategoryObject);

	switch (T.ContainerType)
	{
	case EPinContainerType::Array:
		return FString::Printf(TEXT("Array<%s>"), *Terminal);
	case EPinContainerType::Set:
		return FString::Printf(TEXT("Set<%s>"), *Terminal);
	case EPinContainerType::Map:
	{
		const FString Value = TerminalToString(
			T.PinValueType.TerminalCategory,
			T.PinValueType.TerminalSubCategory,
			T.PinValueType.TerminalSubCategoryObject);
		return FString::Printf(TEXT("Map<%s,%s>"), *Terminal, *Value);
	}
	default:
		return Terminal;
	}
}

FBPStructuredPinType FBlueprintIntrospector::MakeStructuredPinType(const FEdGraphPinType& T)
{
	FBPStructuredPinType Out;
	Out.Category = T.PinCategory.ToString();
	if (!T.PinSubCategory.IsNone())
	{
		Out.SubCategory = T.PinSubCategory.ToString();
	}
	if (UObject* Resolved = T.PinSubCategoryObject.Get())
	{
		Out.SubCategoryObject = Resolved->GetPathName();
	}
	Out.bIsArray = (T.ContainerType == EPinContainerType::Array);
	Out.bIsSet   = (T.ContainerType == EPinContainerType::Set);
	Out.bIsMap   = (T.ContainerType == EPinContainerType::Map);
	return Out;
}

TOptional<FBlueprintInfo> FBlueprintIntrospector::Read(const FString& AssetPath)
{
	// Accept both `/Game/AI/BP_Enemy` (package path) and
	// `/Game/AI/BP_Enemy.BP_Enemy` (object path).
	FString Resolved = AssetPath;
	if (!Resolved.Contains(TEXT(".")))
	{
		FString Leaf;
		if (Resolved.Split(TEXT("/"), nullptr, &Leaf, ESearchCase::IgnoreCase, ESearchDir::FromEnd))
		{
			Resolved = Resolved + TEXT(".") + Leaf;
		}
	}
	// Quiet the load — diagnostic below is the user-facing error on the
	// failure path; the default LogLinker warning would just duplicate it.
	UBlueprint* Blueprint = LoadObject<UBlueprint>(
		nullptr, *Resolved, nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!IsValid(Blueprint))
	{
		// `read_blueprint` is the most common entry point for issues #3
		// (uncompiled parent class) and #4 (non-Blueprint asset
		// misrouted to bp-reader), so the diagnostic needs to fire here
		// too — not just on the write path's LoadMutableBlueprint.
		// Codex review on PR #58 caught the original miss.
		DiagnoseFailedBlueprintLoad(Resolved);
		return TOptional<FBlueprintInfo>();
	}
	return Read(Blueprint);
}

void FBlueprintIntrospector::DiagnoseFailedBlueprintLoad(const FString& AssetPath)
{
	IAssetRegistry& Registry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FAssetData Asset = Registry.GetAssetByObjectPath(FSoftObjectPath(AssetPath));
	if (!Asset.IsValid())
	{
		UE_LOG(LogBlueprintReader, Error,
			TEXT("LoadBlueprint: %s — asset not in registry; check the path"),
			*AssetPath);
		return;
	}
	// Issue #4: bp-reader only handles UBlueprint / UWidgetBlueprint
	// assets. If the asset on disk is a UPrimaryDataAsset descendant
	// (DA_*.uasset) or any other non-Blueprint asset class, the
	// LoadObject<UBlueprint> cast returns null even though the asset
	// itself loaded fine. Detect that and emit a clear message
	// pointing the caller at the right tool.
	const UClass* AssetClass = Asset.GetClass();
	if (AssetClass && !AssetClass->IsChildOf(UBlueprint::StaticClass()))
	{
		UE_LOG(LogBlueprintReader, Error,
			TEXT("LoadBlueprint: %s — asset is %s, not a UBlueprint. bp-reader "
			     "only handles Blueprint assets (UBlueprint / UWidgetBlueprint). "
			     "Data Assets (UPrimaryDataAsset descendants), DataTables, Curves, "
			     "Materials, etc. are not supported here — inspect them via the "
			     "editor or raw asset serialization (issue #4)."),
			*AssetPath, *AssetClass->GetName());
		return;
	}
	const FString ParentClassTag =
		Asset.GetTagValueRef<FString>(FBlueprintTags::ParentClassPath);
	if (ParentClassTag.IsEmpty())
	{
		UE_LOG(LogBlueprintReader, Error,
			TEXT("LoadBlueprint: %s — asset exists in registry but load failed; "
			     "no parent_class tag to probe"),
			*AssetPath);
		return;
	}
	const FSoftObjectPath ParentRef(ParentClassTag);
	UClass* ParentClass =
		LoadObject<UClass>(nullptr, *ParentRef.ToString(), nullptr, LOAD_Quiet);
	if (!IsValid(ParentClass))
	{
		UE_LOG(LogBlueprintReader, Error,
			TEXT("LoadBlueprint: %s — parent class %s could not be resolved. "
			     "This typically means the C++ module declaring it is not compiled in "
			     "this build (issue #3). Rebuild the project (Build.bat <TargetName> "
			     "Win64 Development) before reading or writing this Blueprint."),
			*AssetPath, *ParentClassTag);
		return;
	}
	UE_LOG(LogBlueprintReader, Error,
		TEXT("LoadBlueprint: %s — parent class %s resolved but BP load still "
		     "failed; check the editor log for the underlying PostLoad / "
		     "ConstructDefaultObject error"),
		*AssetPath, *ParentClassTag);
}

TOptional<FBlueprintInfo> FBlueprintIntrospector::Read(UBlueprint* Blueprint)
{
	if (!IsValid(Blueprint))
	{
		return TOptional<FBlueprintInfo>();
	}

	FBlueprintInfo Info;
	Info.Path = Blueprint->GetPathName();
	Info.Name = Blueprint->GetName();

	if (UEnum* TypeEnum = StaticEnum<EBlueprintType>())
	{
		Info.BlueprintType = TypeEnum->GetNameStringByValue(static_cast<int64>(Blueprint->BlueprintType));
	}
	Info.ParentClassPath    = Blueprint->ParentClass    ? Blueprint->ParentClass->GetPathName()    : FString();
	Info.GeneratedClassPath = Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetPathName() : FString();

	for (const FBPInterfaceDescription& Iface : Blueprint->ImplementedInterfaces)
	{
		FBPInterfaceInfo I;
		I.InterfacePath = Iface.Interface ? Iface.Interface->GetPathName() : FString();
		Info.Interfaces.Add(MoveTemp(I));
	}

	{
		UClass* GenClass = Blueprint->GeneratedClass;
		if (!IsValid(GenClass))
		{
			GenClass = Blueprint->SkeletonGeneratedClass;
		}
		for (const FBPVariableDescription& Var : Blueprint->NewVariables)
		{
			FBPVariableInfo V = VariableDescToInfo(Var);
			// Multicast delegate variables: read signature params off
			// the generated class so codegen can emit the matching
			// DECLARE_DYNAMIC_MULTICAST_DELEGATE_<N>Params variant.
			PopulateDelegateParams(V, GenClass);
			Info.Variables.Add(MoveTemp(V));
		}
	}

	if (USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript)
	{
		const TArray<USCS_Node*>& RootSet = SCS->GetRootNodes();
		for (USCS_Node* Node : SCS->GetAllNodes())
		{
			if (!IsValid(Node))
			{
				continue;
			}
			FBPComponentInfo C;
			C.Name = Node->GetVariableName().ToString();
			C.ClassPath = Node->ComponentClass ? Node->ComponentClass->GetPathName() : FString();
			if (USCS_Node* Parent = SCS->FindParentNode(Node))
			{
				C.ParentName = Parent->GetVariableName().ToString();
			}
			C.bIsRoot = RootSet.Contains(Node);

			// Read property overrides: compare each FProperty on the
			// component template against the component class's CDO.
			// Properties whose value differs are what the BP author
			// edited in the Components panel (RelativeLocation, mesh
			// assets, etc.). Skip transient + non-edit-anywhere
			// properties -- those aren't real authored values.
			UActorComponent* Template = Node->ComponentTemplate;
			if (Template && Template->GetClass())
			{
				UObject* CDO = Template->GetClass()->GetDefaultObject();
				if (IsValid(CDO))
				{
					for (TFieldIterator<FProperty> It(Template->GetClass()); It; ++It)
					{
						FProperty* Prop = *It;
						if (!Prop)
						{
							continue;
						}
						// Skip non-authored properties (transient,
						// internal-only, computed-at-runtime).
						if (Prop->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient |
						                              CPF_NonPIEDuplicateTransient))
						{
							continue;
						}
						const void* TemplatePtr = Prop->ContainerPtrToValuePtr<void>(Template);
						const void* CDOPtr      = Prop->ContainerPtrToValuePtr<void>(CDO);
						if (Prop->Identical(TemplatePtr, CDOPtr, PPF_DeepComparison))
						{
							continue;
						}
						FBPComponentPropertyOverride Override;
						Override.Name = Prop->GetName();
						Override.Type = Prop->GetClass() ? Prop->GetClass()->GetName() : FString();
						FString ValueStr;
						Prop->ExportTextItem_Direct(ValueStr, TemplatePtr, CDOPtr, nullptr,
						                            PPF_None);
						Override.ValueText = MoveTemp(ValueStr);
						// Asset-ref properties: capture the property's
						// class so codegen can fill in the
						// ConstructorHelpers::FObjectFinder<T> template
						// arg instead of leaving T as a TODO.
						Override.PropertyClass = GetAssetRefPropertyClassName(Prop);
						C.Properties.Add(MoveTemp(Override));
					}
				}
			}

			Info.Components.Add(MoveTemp(C));
		}
	}

	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (!Graph)
		{
			continue;
		}
		FBPGraphInfo G = ReadGraph(Graph);
		const bool bIsConstruction =
			G.Name.Equals(TEXT("ConstructionScript"), ESearchCase::IgnoreCase) ||
			G.Name.Equals(TEXT("UserConstructionScript"), ESearchCase::IgnoreCase);
		G.WireType = bIsConstruction ? TEXT("Construction") : TEXT("Function");
		Info.FunctionGraphs.Add(MoveTemp(G));
	}
	for (UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		if (!Graph)
		{
			continue;
		}
		FBPGraphInfo G = ReadGraph(Graph);
		G.WireType = TEXT("EventGraph");
		Info.EventGraphs.Add(MoveTemp(G));
	}
	for (UEdGraph* Graph : Blueprint->MacroGraphs)
	{
		if (!Graph)
		{
			continue;
		}
		FBPGraphInfo G = ReadGraph(Graph);
		G.WireType = TEXT("Macro");
		Info.MacroGraphs.Add(MoveTemp(G));
	}
	for (UEdGraph* Graph : Blueprint->DelegateSignatureGraphs)
	{
		if (!Graph)
		{
			continue;
		}
		FBPGraphInfo G = ReadGraph(Graph);
		G.WireType = TEXT("DelegateSignature");
		Info.DelegateSignatureGraphs.Add(MoveTemp(G));
	}

	return Info;
}

FBPGraphInfo FBlueprintIntrospector::ReadGraph(UEdGraph* Graph)
{
	FBPGraphInfo G;
	G.Name = Graph->GetFName().ToString();
	G.SchemaPath = Graph->Schema ? Graph->Schema->GetPathName() : FString();

	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!IsValid(Node))
		{
			continue;
		}

		FBPNodeInfo N;
		N.Guid = Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens);
		N.ClassName = Node->GetClass()->GetName();
		N.Title = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
		N.Comment = Node->NodeComment;
		N.PosX = Node->NodePosX;
		N.PosY = Node->NodePosY;
		N.bEnabled = Node->IsNodeEnabled();

		ExtractK2Extras(Node, N.Extras);

		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin)
			{
				continue;
			}

			FBPPinInfo P;
			P.Name = Pin->GetFName().ToString();
			P.PinId = Pin->PinId.ToString(EGuidFormats::DigitsWithHyphens);
			P.Direction = (Pin->Direction == EGPD_Input) ? TEXT("Input") : TEXT("Output");
			P.Type = FormatPinType(Pin->PinType);
			P.StructuredType = MakeStructuredPinType(Pin->PinType);
			P.DefaultValue = Pin->DefaultValue;
			P.DefaultObjectPath = Pin->DefaultObject ? Pin->DefaultObject->GetPathName() : FString();
			P.DefaultText = Pin->DefaultTextValue.ToString();
			P.bIsHidden = Pin->bHidden;
			P.bIsReference = Pin->PinType.bIsReference;
			P.bIsConst = Pin->PinType.bIsConst;

			for (UEdGraphPin* Linked : Pin->LinkedTo)
			{
				if (!Linked)
				{
					continue;
				}
				if (UEdGraphNode* OwningNode = Linked->GetOwningNodeUnchecked())
				{
					FBPPinLinkInfo L;
					L.NodeGuid = OwningNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens);
					L.PinName = Linked->GetFName().ToString();
					L.PinId = Linked->PinId.ToString(EGuidFormats::DigitsWithHyphens);
					P.LinkedTo.Add(MoveTemp(L));
				}
			}

			N.Pins.Add(MoveTemp(P));
		}

		if (UK2Node_FunctionEntry* FE = Cast<UK2Node_FunctionEntry>(Node))
		{
			for (const FBPVariableDescription& Local : FE->LocalVariables)
			{
				G.LocalVariables.Add(VariableDescToInfo(Local));
			}
		}

		G.Nodes.Add(MoveTemp(N));
	}

	return G;
}
