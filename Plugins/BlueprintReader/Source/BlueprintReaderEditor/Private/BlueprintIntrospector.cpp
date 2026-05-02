#include "BlueprintIntrospector.h"

#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Class.h"
#include "UObject/ObjectMacros.h"

#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_MacroInstance.h"
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

	void ExtractK2Extras(UEdGraphNode* Node, TMap<FString, FString>& Extras)
	{
		if (UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
		{
			Extras.Add(TEXT("kind"), TEXT("CallFunction"));
			Extras.Add(TEXT("targetFunction"), Call->FunctionReference.GetMemberName().ToString());
			if (UClass* C = Call->FunctionReference.GetMemberParentClass())
			{
				Extras.Add(TEXT("targetClass"), C->GetPathName());
			}
			Extras.Add(TEXT("isSelfContext"), BoolToString(Call->FunctionReference.IsSelfContext()));
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
	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *Resolved);
	if (!Blueprint)
	{
		return TOptional<FBlueprintInfo>();
	}
	return Read(Blueprint);
}

TOptional<FBlueprintInfo> FBlueprintIntrospector::Read(UBlueprint* Blueprint)
{
	if (!Blueprint)
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

	for (const FBPVariableDescription& Var : Blueprint->NewVariables)
	{
		Info.Variables.Add(VariableDescToInfo(Var));
	}

	if (USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript)
	{
		const TArray<USCS_Node*>& RootSet = SCS->GetRootNodes();
		for (USCS_Node* Node : SCS->GetAllNodes())
		{
			if (!Node) continue;
			FBPComponentInfo C;
			C.Name = Node->GetVariableName().ToString();
			C.ClassPath = Node->ComponentClass ? Node->ComponentClass->GetPathName() : FString();
			if (USCS_Node* Parent = SCS->FindParentNode(Node))
			{
				C.ParentName = Parent->GetVariableName().ToString();
			}
			C.bIsRoot = RootSet.Contains(Node);
			Info.Components.Add(MoveTemp(C));
		}
	}

	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (!Graph) continue;
		FBPGraphInfo G = ReadGraph(Graph);
		const bool bIsConstruction =
			G.Name.Equals(TEXT("ConstructionScript"), ESearchCase::IgnoreCase) ||
			G.Name.Equals(TEXT("UserConstructionScript"), ESearchCase::IgnoreCase);
		G.WireType = bIsConstruction ? TEXT("Construction") : TEXT("Function");
		Info.FunctionGraphs.Add(MoveTemp(G));
	}
	for (UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		if (!Graph) continue;
		FBPGraphInfo G = ReadGraph(Graph);
		G.WireType = TEXT("EventGraph");
		Info.EventGraphs.Add(MoveTemp(G));
	}
	for (UEdGraph* Graph : Blueprint->MacroGraphs)
	{
		if (!Graph) continue;
		FBPGraphInfo G = ReadGraph(Graph);
		G.WireType = TEXT("Macro");
		Info.MacroGraphs.Add(MoveTemp(G));
	}
	for (UEdGraph* Graph : Blueprint->DelegateSignatureGraphs)
	{
		if (!Graph) continue;
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
		if (!Node) continue;

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
			if (!Pin) continue;

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
				if (!Linked) continue;
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
