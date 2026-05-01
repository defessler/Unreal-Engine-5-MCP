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

namespace
{
	FString ContainerTypeToString(EPinContainerType Type)
	{
		switch (Type)
		{
		case EPinContainerType::Array: return TEXT("Array");
		case EPinContainerType::Set:   return TEXT("Set");
		case EPinContainerType::Map:   return TEXT("Map");
		default:                       return FString();
		}
	}

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

TOptional<FBlueprintInfo> FBlueprintIntrospector::Read(const FString& AssetPath)
{
	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
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
	Info.ParentClassPath = Blueprint->ParentClass ? Blueprint->ParentClass->GetPathName() : FString();
	Info.GeneratedClassPath = Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetPathName() : FString();

	for (const FBPInterfaceDescription& Iface : Blueprint->ImplementedInterfaces)
	{
		FBPInterfaceInfo I;
		I.InterfacePath = Iface.Interface ? Iface.Interface->GetPathName() : FString();
		Info.Interfaces.Add(MoveTemp(I));
	}

	for (const FBPVariableDescription& Var : Blueprint->NewVariables)
	{
		FBPVariableInfo V;
		V.Name = Var.VarName.ToString();
		V.FriendlyName = Var.FriendlyName;
		V.Category = Var.Category.ToString();
		V.Type = FormatPinType(Var.VarType);
		V.DefaultValue = Var.DefaultValue;
		V.bIsReplicated      = (Var.PropertyFlags & CPF_Net) != 0;
		V.bIsTransient       = (Var.PropertyFlags & CPF_Transient) != 0;
		V.bIsEditable        = (Var.PropertyFlags & CPF_Edit) != 0;
		V.bIsBlueprintReadOnly = (Var.PropertyFlags & CPF_BlueprintReadOnly) != 0;
		V.bIsExposeOnSpawn   = Var.HasMetaData(TEXT("ExposeOnSpawn"));
		Info.Variables.Add(MoveTemp(V));
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
		if (Graph) Info.FunctionGraphs.Add(ReadGraph(Graph));
	}
	for (UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		if (Graph) Info.EventGraphs.Add(ReadGraph(Graph));
	}
	for (UEdGraph* Graph : Blueprint->MacroGraphs)
	{
		if (Graph) Info.MacroGraphs.Add(ReadGraph(Graph));
	}
	for (UEdGraph* Graph : Blueprint->DelegateSignatureGraphs)
	{
		if (Graph) Info.DelegateSignatureGraphs.Add(ReadGraph(Graph));
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

		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin) continue;

			FBPPinInfo P;
			P.Name = Pin->GetFName().ToString();
			P.Direction = (Pin->Direction == EGPD_Input) ? TEXT("Input") : TEXT("Output");
			P.Type = FormatPinType(Pin->PinType);
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
					P.LinkedTo.Add(MoveTemp(L));
				}
			}

			N.Pins.Add(MoveTemp(P));
		}

		G.Nodes.Add(MoveTemp(N));
	}

	return G;
}
