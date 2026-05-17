#include "BlueprintStructuralDiff.h"

#include "Engine/Blueprint.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Kismet2/BlueprintEditorUtils.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace BlueprintStructuralDiff {

namespace
{
	void Add(FResult& R, FString Path, FString Kind,
	         FString ValueA = {}, FString ValueB = {})
	{
		R.Differences.Add({ MoveTemp(Path), MoveTemp(Kind),
		                    MoveTemp(ValueA), MoveTemp(ValueB) });
	}

	FString PinTypeStr(const FEdGraphPinType& T)
	{
		return FString::Printf(TEXT("%s/%s/%s%s%s%s"),
			*T.PinCategory.ToString(),
			*T.PinSubCategory.ToString(),
			T.PinSubCategoryObject.IsValid() ? *T.PinSubCategoryObject->GetPathName() : TEXT(""),
			T.IsArray() ? TEXT("[]") : TEXT(""),
			T.IsSet()   ? TEXT("(set)") : TEXT(""),
			T.IsMap()   ? TEXT("(map)") : TEXT(""));
	}

	FString NodeSignature(UEdGraphNode* N)
	{
		FString Sig = N->GetClass()->GetName() + TEXT("|") +
		              N->GetNodeTitle(ENodeTitleType::ListView).ToString();
		for (UEdGraphPin* P : N->Pins)
		{
			Sig += FString::Printf(TEXT("|%s:%s:%s"),
				*P->PinName.ToString(),
				P->Direction == EGPD_Input ? TEXT("In") : TEXT("Out"),
				*PinTypeStr(P->PinType));
		}
		return Sig;
	}

	void CompareVariables(UBlueprint* A, UBlueprint* B, FResult& R)
	{
		TMap<FName, const FBPVariableDescription*> MapA;
		TMap<FName, const FBPVariableDescription*> MapB;
		for (const FBPVariableDescription& V : A->NewVariables)
		{
			MapA.Add(V.VarName, &V);
		}
		for (const FBPVariableDescription& V : B->NewVariables)
		{
			MapB.Add(V.VarName, &V);
		}

		for (const auto& Pair : MapA)
		{
			const FBPVariableDescription* const* Found = MapB.Find(Pair.Key);
			if (!Found)
			{
				Add(R, FString::Printf(TEXT("variables.%s"), *Pair.Key.ToString()),
					TEXT("missing"), TEXT("present"), TEXT("absent"));
				continue;
			}
			const FBPVariableDescription* VA = Pair.Value;
			const FBPVariableDescription* VB = *Found;
			const FString TA = PinTypeStr(VA->VarType);
			const FString TB = PinTypeStr(VB->VarType);
			if (TA != TB)
			{
				Add(R, FString::Printf(TEXT("variables.%s.type"), *Pair.Key.ToString()),
					TEXT("type_mismatch"), TA, TB);
			}
			if (VA->DefaultValue != VB->DefaultValue)
			{
				Add(R, FString::Printf(TEXT("variables.%s.default"), *Pair.Key.ToString()),
					TEXT("value_mismatch"), VA->DefaultValue, VB->DefaultValue);
			}
		}
		for (const auto& Pair : MapB)
		{
			if (!MapA.Contains(Pair.Key))
			{
				Add(R, FString::Printf(TEXT("variables.%s"), *Pair.Key.ToString()),
					TEXT("extra"), TEXT("absent"), TEXT("present"));
			}
		}
	}

	void CompareComponents(UBlueprint* A, UBlueprint* B, FResult& R)
	{
		if (!A->SimpleConstructionScript || !B->SimpleConstructionScript)
		{
			return;
		}
		TMap<FName, USCS_Node*> NodesA;
		TMap<FName, USCS_Node*> NodesB;
		for (USCS_Node* N : A->SimpleConstructionScript->GetAllNodes())
		{
			if (N)
			{
				NodesA.Add(N->GetVariableName(), N);
			}
		}
		for (USCS_Node* N : B->SimpleConstructionScript->GetAllNodes())
		{
			if (N)
			{
				NodesB.Add(N->GetVariableName(), N);
			}
		}

		for (const auto& Pair : NodesA)
		{
			USCS_Node** Found = NodesB.Find(Pair.Key);
			if (!Found)
			{
				Add(R, FString::Printf(TEXT("components.%s"), *Pair.Key.ToString()),
					TEXT("missing"));
				continue;
			}
			USCS_Node* NA = Pair.Value;
			USCS_Node* NB = *Found;
			const FString CA = NA->ComponentClass ? NA->ComponentClass->GetPathName() : TEXT("");
			const FString CB = NB->ComponentClass ? NB->ComponentClass->GetPathName() : TEXT("");
			if (CA != CB)
			{
				Add(R, FString::Printf(TEXT("components.%s.class"), *Pair.Key.ToString()),
					TEXT("type_mismatch"), CA, CB);
			}
		}
		for (const auto& Pair : NodesB)
		{
			if (!NodesA.Contains(Pair.Key))
			{
				Add(R, FString::Printf(TEXT("components.%s"), *Pair.Key.ToString()),
					TEXT("extra"));
			}
		}
	}

	void CompareGraph(const FString& GraphPath, UEdGraph* GA, UEdGraph* GB,
	                  const FCompareOptions& Opt, FResult& R)
	{
		if (!GA && !GB)
		{
			return;
		}
		if (!GA)
		{
			Add(R, GraphPath, TEXT("extra"));
			return;
		}
		if (!GB)
		{
			Add(R, GraphPath, TEXT("missing"));
			return;
		}

		auto ShouldSkipNode = [&Opt](UEdGraphNode* N) -> bool
		{
			if (!N)
			{
				return true;
			}
			if (Opt.bIgnoreCommentNodes)
			{
				// Comment nodes are UEdGraphNode_Comment; classify by class name to
				// avoid an extra include just for this check. The standard class
				// name is "EdGraphNode_Comment".
				const FString ClassName = N->GetClass()->GetName();
				if (ClassName.Contains(TEXT("Comment")))
				{
					return true;
				}
			}
			return false;
		};

		TMap<FString, int32> SigCountA;
		TMap<FString, int32> SigCountB;
		for (UEdGraphNode* N : GA->Nodes)
		{
			if (ShouldSkipNode(N))
			{
				continue;
			}
			SigCountA.FindOrAdd(NodeSignature(N))++;
		}
		for (UEdGraphNode* N : GB->Nodes)
		{
			if (ShouldSkipNode(N))
			{
				continue;
			}
			SigCountB.FindOrAdd(NodeSignature(N))++;
		}

		for (const auto& Pair : SigCountA)
		{
			const int32* BVal = SigCountB.Find(Pair.Key);
			const int32 BCount = BVal ? *BVal : 0;
			if (BCount != Pair.Value)
			{
				Add(R, FString::Printf(TEXT("%s.node_count[%s]"), *GraphPath, *Pair.Key),
					TEXT("value_mismatch"),
					FString::FromInt(Pair.Value), FString::FromInt(BCount));
			}
		}
		for (const auto& Pair : SigCountB)
		{
			if (!SigCountA.Contains(Pair.Key))
			{
				Add(R, FString::Printf(TEXT("%s.node_count[%s]"), *GraphPath, *Pair.Key),
					TEXT("value_mismatch"), TEXT("0"), FString::FromInt(Pair.Value));
			}
		}

		// Connection-count comparison (cheap, catches wiring regressions).
		int32 LinksA = 0;
		int32 LinksB = 0;
		for (UEdGraphNode* N : GA->Nodes)
		{
			if (ShouldSkipNode(N))
			{
				continue;
			}
			for (UEdGraphPin* P : N->Pins)
			{
				LinksA += P->LinkedTo.Num();
			}
		}
		for (UEdGraphNode* N : GB->Nodes)
		{
			if (ShouldSkipNode(N))
			{
				continue;
			}
			for (UEdGraphPin* P : N->Pins)
			{
				LinksB += P->LinkedTo.Num();
			}
		}
		// Each link counted twice (both ends); both sides see same factor.
		if (LinksA != LinksB)
		{
			Add(R, FString::Printf(TEXT("%s.link_count"), *GraphPath),
				TEXT("value_mismatch"),
				FString::FromInt(LinksA / 2), FString::FromInt(LinksB / 2));
		}
	}

	void CompareGraphs(UBlueprint* A, UBlueprint* B,
	                   const FCompareOptions& Opt, FResult& R)
	{
		TArray<UEdGraph*> GraphsA = A->FunctionGraphs;
		TArray<UEdGraph*> GraphsB = B->FunctionGraphs;
		GraphsA.Append(A->MacroGraphs);
		GraphsB.Append(B->MacroGraphs);
		GraphsA.Append(A->UbergraphPages);
		GraphsB.Append(B->UbergraphPages);

		TMap<FName, UEdGraph*> MapA;
		TMap<FName, UEdGraph*> MapB;
		for (UEdGraph* G : GraphsA)
		{
			if (G)
			{
				MapA.Add(G->GetFName(), G);
			}
		}
		for (UEdGraph* G : GraphsB)
		{
			if (G)
			{
				MapB.Add(G->GetFName(), G);
			}
		}

		for (const auto& Pair : MapA)
		{
			UEdGraph** Found = MapB.Find(Pair.Key);
			const FString Path = FString::Printf(TEXT("graphs.%s"), *Pair.Key.ToString());
			CompareGraph(Path, Pair.Value, Found ? *Found : nullptr, Opt, R);
		}
		for (const auto& Pair : MapB)
		{
			if (!MapA.Contains(Pair.Key))
			{
				Add(R, FString::Printf(TEXT("graphs.%s"), *Pair.Key.ToString()),
					TEXT("extra"));
			}
		}
	}
}    // namespace

FResult Compare(UBlueprint* A, UBlueprint* B, const FCompareOptions& Options)
{
	FResult R;
	if (!A || !B)
	{
		Add(R, TEXT("root"), TEXT("missing"));
		return R;
	}

	// Parent class
	if (A->ParentClass != B->ParentClass)
	{
		Add(R, TEXT("parent_class"), TEXT("value_mismatch"),
			A->ParentClass ? A->ParentClass->GetPathName() : TEXT(""),
			B->ParentClass ? B->ParentClass->GetPathName() : TEXT(""));
	}

	CompareVariables(A, B, R);
	CompareComponents(A, B, R);
	CompareGraphs(A, B, Options, R);

	R.bEqual = R.Differences.IsEmpty();
	return R;
}

TSharedRef<FJsonObject> FResult::ToJson() const
{
	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetBoolField(TEXT("ok"), bEqual);
	TArray<TSharedPtr<FJsonValue>> Arr;
	for (const FDifference& D : Differences)
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("path"), D.Path);
		O->SetStringField(TEXT("kind"), D.Kind);
		O->SetStringField(TEXT("a"),    D.ValueA);
		O->SetStringField(TEXT("b"),    D.ValueB);
		Arr.Add(MakeShared<FJsonValueObject>(O));
	}
	Obj->SetArrayField(TEXT("differences"), Arr);
	return Obj;
}

}    // namespace BlueprintStructuralDiff
