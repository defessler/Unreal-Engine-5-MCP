#include "BlueprintReaderCommandlet.h"

#include "BlueprintIntrospector.h"
#include "BlueprintReaderJson.h"
#include "BlueprintReaderWireJson.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Blueprint.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include "Windows/HideWindowsPlatformTypes.h"

DEFINE_LOG_CATEGORY_STATIC(LogBlueprintReader, Log, All);

UBlueprintReaderCommandlet::UBlueprintReaderCommandlet()
{
	IsClient = false;
	IsServer = false;
	IsEditor = true;
	LogToConsole = true;
	ShowErrorCount = false;
}

namespace
{
	enum class EOp : uint8
	{
		Legacy,    // No -Op specified — emit the rich plugin shape (backward compat).
		List,
		Read,
		Graph,
		Function,
		Variables,
		Find,
		// Write ops (Phase 1.5):
		AddVariable,
		SetNodePosition,
		DeleteNode,
	};

	bool ParseOp(const FString& Params, EOp& OutOp)
	{
		FString OpStr;
		if (!FParse::Value(*Params, TEXT("Op="), OpStr))
		{
			OutOp = EOp::Legacy;
			return true;
		}
		if (OpStr.Equals(TEXT("List"), ESearchCase::IgnoreCase))             { OutOp = EOp::List; return true; }
		if (OpStr.Equals(TEXT("Read"), ESearchCase::IgnoreCase))             { OutOp = EOp::Read; return true; }
		if (OpStr.Equals(TEXT("Graph"), ESearchCase::IgnoreCase))            { OutOp = EOp::Graph; return true; }
		if (OpStr.Equals(TEXT("Function"), ESearchCase::IgnoreCase))         { OutOp = EOp::Function; return true; }
		if (OpStr.Equals(TEXT("Variables"), ESearchCase::IgnoreCase))        { OutOp = EOp::Variables; return true; }
		if (OpStr.Equals(TEXT("Find"), ESearchCase::IgnoreCase))             { OutOp = EOp::Find; return true; }
		if (OpStr.Equals(TEXT("AddVariable"), ESearchCase::IgnoreCase))      { OutOp = EOp::AddVariable; return true; }
		if (OpStr.Equals(TEXT("SetNodePosition"), ESearchCase::IgnoreCase))  { OutOp = EOp::SetNodePosition; return true; }
		if (OpStr.Equals(TEXT("DeleteNode"), ESearchCase::IgnoreCase))       { OutOp = EOp::DeleteNode; return true; }
		UE_LOG(LogBlueprintReader, Error, TEXT("Unknown -Op=%s"), *OpStr);
		return false;
	}

	FString ResolveAssetPath(const FString& Params)
	{
		FString Asset;
		if (FParse::Value(*Params, TEXT("Asset="), Asset)) return Asset;
		FParse::Value(*Params, TEXT("Path="), Asset);
		return Asset;
	}

	FString ResolveOutputPath(const FString& Params)
	{
		FString Out;
		if (FParse::Value(*Params, TEXT("Out="), Out)) return Out;
		FParse::Value(*Params, TEXT("Output="), Out);
		return Out;
	}

	int32 EmitJson(const FString& Json, const FString& OutputPath)
	{
		if (!OutputPath.IsEmpty())
		{
			if (!FFileHelper::SaveStringToFile(Json, *OutputPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				UE_LOG(LogBlueprintReader, Error, TEXT("Failed to write output to: %s"), *OutputPath);
				return 3;
			}
			UE_LOG(LogBlueprintReader, Display, TEXT("Wrote %d bytes to %s"), Json.Len(), *OutputPath);
		}
		else
		{
			FPlatformMisc::LocalPrint(*Json);
			FPlatformMisc::LocalPrint(TEXT("\n"));
		}
		return 0;
	}

	FString IsoDateForFile(const FString& Filename)
	{
		IFileManager& FM = IFileManager::Get();
		const FDateTime DT = FM.GetTimeStamp(*Filename);
		if (DT == FDateTime::MinValue()) return FString();
		return DT.ToIso8601();
	}

	// ----- Shared helpers for write ops -------------------------------------

	UBlueprint* LoadMutableBlueprint(const FString& AssetPath)
	{
		FString Resolved = AssetPath;
		if (!Resolved.Contains(TEXT(".")))
		{
			FString Leaf;
			if (Resolved.Split(TEXT("/"), nullptr, &Leaf, ESearchCase::IgnoreCase, ESearchDir::FromEnd))
			{
				Resolved = Resolved + TEXT(".") + Leaf;
			}
		}
		return LoadObject<UBlueprint>(nullptr, *Resolved);
	}

	UEdGraph* FindGraphByName(UBlueprint* BP, const FString& Name)
	{
		auto Search = [&](const TArray<UEdGraph*>& Graphs) -> UEdGraph*
		{
			for (UEdGraph* G : Graphs)
			{
				if (G && G->GetFName().ToString().Equals(Name, ESearchCase::IgnoreCase))
				{
					return G;
				}
			}
			return nullptr;
		};
		if (UEdGraph* G = Search(BP->UbergraphPages)) return G;
		if (UEdGraph* G = Search(BP->FunctionGraphs)) return G;
		if (UEdGraph* G = Search(BP->MacroGraphs))    return G;
		return nullptr;
	}

	UEdGraphNode* FindNodeByGuid(UEdGraph* Graph, const FString& Guid)
	{
		FGuid Parsed;
		if (!FGuid::Parse(Guid, Parsed)) return nullptr;
		for (UEdGraphNode* N : Graph->Nodes)
		{
			if (N && N->NodeGuid == Parsed) return N;
		}
		return nullptr;
	}

	bool CompileAndSaveBlueprint(UBlueprint* BP)
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
		FKismetEditorUtilities::CompileBlueprint(BP);

		UPackage* Package = BP->GetOutermost();
		if (!Package) return false;

		const FString FileName = FPackageName::LongPackageNameToFilename(
			Package->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		Args.Error = GError;
		const bool bOk = UPackage::SavePackage(Package, BP, *FileName, Args);
		if (!bOk)
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("SavePackage failed: %s"), *FileName);
		}
		return bOk;
	}

	// Emit a small ack JSON blob for a successful write op.
	int32 EmitOk(const FString& OutputPath, bool bPretty)
	{
		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	int32 RunAddVariableOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		const FString AssetPath = ResolveAssetPath(Params);
		FString VarName, DefaultValue, Category;
		FParse::Value(*Params, TEXT("Name="),     VarName);
		FParse::Value(*Params, TEXT("Default="),  DefaultValue);
		FParse::Value(*Params, TEXT("Category="), Category);
		const bool bReplicated = FParse::Param(*Params, TEXT("Replicated"));
		const bool bEditable   = FParse::Param(*Params, TEXT("Editable"));

		// Build BPPinType from the individual -TypeFoo= flags.
		FString TypeCategory, TypeSubCategory, TypeSubObject;
		FParse::Value(*Params, TEXT("TypeCategory="),         TypeCategory);
		FParse::Value(*Params, TEXT("TypeSubCategory="),      TypeSubCategory);
		FParse::Value(*Params, TEXT("TypeSubCategoryObject="), TypeSubObject);

		if (AssetPath.IsEmpty() || VarName.IsEmpty() || TypeCategory.IsEmpty())
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("AddVariable requires -Asset= -Name= -TypeCategory= [-TypeSubCategory=...] [-TypeSubCategoryObject=...]"));
			return 1;
		}

		auto TypeJson = MakeShared<FJsonObject>();
		TypeJson->SetStringField(TEXT("category"), TypeCategory);
		if (!TypeSubCategory.IsEmpty()) TypeJson->SetStringField(TEXT("sub_category"), TypeSubCategory);
		if (!TypeSubObject.IsEmpty())   TypeJson->SetStringField(TEXT("sub_category_object"), TypeSubObject);
		TypeJson->SetBoolField(TEXT("is_array"), FParse::Param(*Params, TEXT("TypeIsArray")));
		TypeJson->SetBoolField(TEXT("is_set"),   FParse::Param(*Params, TEXT("TypeIsSet")));
		TypeJson->SetBoolField(TEXT("is_map"),   FParse::Param(*Params, TEXT("TypeIsMap")));

		FEdGraphPinType PinType;
		if (!FBlueprintReaderWireJson::ParseWirePinType(TypeJson, PinType))
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("AddVariable: failed to build pin type"));
			return 1;
		}

		UBlueprint* BP = LoadMutableBlueprint(AssetPath);
		if (!BP)
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("AddVariable: failed to load %s"), *AssetPath);
			return 4;
		}

		const FName NewName(*VarName);
		if (FBlueprintEditorUtils::FindNewVariableIndex(BP, NewName) != INDEX_NONE)
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("AddVariable: variable %s already exists on %s"),
				*VarName, *AssetPath);
			return 1;
		}
		FBlueprintEditorUtils::AddMemberVariable(BP, NewName, PinType, DefaultValue);
		const int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(BP, NewName);
		if (Index == INDEX_NONE)
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("AddVariable: AddMemberVariable failed"));
			return 1;
		}
		FBPVariableDescription& Var = BP->NewVariables[Index];
		if (!Category.IsEmpty()) Var.Category = FText::FromString(Category);
		if (bEditable)   Var.PropertyFlags |= CPF_Edit | CPF_BlueprintVisible;
		if (bReplicated) { Var.PropertyFlags |= CPF_Net; Var.ReplicationCondition = COND_None; }

		if (!CompileAndSaveBlueprint(BP)) return 5;
		return EmitOk(OutputPath, bPretty);
	}

	int32 RunSetNodePositionOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		const FString AssetPath = ResolveAssetPath(Params);
		FString GraphName, NodeId;
		int32 X = 0, Y = 0;
		FParse::Value(*Params, TEXT("Graph="), GraphName);
		FParse::Value(*Params, TEXT("Node="),  NodeId);
		FParse::Value(*Params, TEXT("X="),     X);
		FParse::Value(*Params, TEXT("Y="),     Y);

		if (AssetPath.IsEmpty() || GraphName.IsEmpty() || NodeId.IsEmpty())
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("SetNodePosition requires -Asset= -Graph= -Node= -X= -Y="));
			return 1;
		}

		UBlueprint* BP = LoadMutableBlueprint(AssetPath);
		if (!BP) return 4;

		UEdGraph* Graph = FindGraphByName(BP, GraphName);
		if (!Graph)
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("SetNodePosition: graph %s not found"), *GraphName);
			return 4;
		}
		UEdGraphNode* Node = FindNodeByGuid(Graph, NodeId);
		if (!Node)
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("SetNodePosition: node %s not found in %s"), *NodeId, *GraphName);
			return 4;
		}
		Node->Modify();
		Node->NodePosX = X;
		Node->NodePosY = Y;

		if (!CompileAndSaveBlueprint(BP)) return 5;
		return EmitOk(OutputPath, bPretty);
	}

	int32 RunDeleteNodeOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		const FString AssetPath = ResolveAssetPath(Params);
		FString GraphName, NodeId;
		FParse::Value(*Params, TEXT("Graph="), GraphName);
		FParse::Value(*Params, TEXT("Node="),  NodeId);

		if (AssetPath.IsEmpty() || GraphName.IsEmpty() || NodeId.IsEmpty())
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("DeleteNode requires -Asset= -Graph= -Node="));
			return 1;
		}

		UBlueprint* BP = LoadMutableBlueprint(AssetPath);
		if (!BP) return 4;
		UEdGraph* Graph = FindGraphByName(BP, GraphName);
		if (!Graph) return 4;
		UEdGraphNode* Node = FindNodeByGuid(Graph, NodeId);
		if (!Node)
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("DeleteNode: node %s not found in %s"), *NodeId, *GraphName);
			return 4;
		}

		// Break links + remove from graph. FBlueprintEditorUtils::RemoveNode
		// handles both, plus refreshes any dependent UI state.
		FBlueprintEditorUtils::RemoveNode(BP, Node, /*bDontRecompile=*/true);

		if (!CompileAndSaveBlueprint(BP)) return 5;
		return EmitOk(OutputPath, bPretty);
	}

	// List op via the asset registry. Faster than walking Content/ on disk +
	// LoadObject'ing every .uasset, and gets `parent_class` from asset tags
	// without paying the full BP load cost. Falls back to a disk walk if the
	// asset registry isn't available (e.g. very early commandlet startup
	// before module init is complete).
	int32 RunListOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString PathFilter;
		FParse::Value(*Params, TEXT("Path="), PathFilter);
		if (PathFilter.IsEmpty())
		{
			PathFilter = TEXT("/Game");
		}

		TArray<TSharedPtr<FJsonValue>> Out;

		FAssetRegistryModule& AssetRegistryModule =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		IAssetRegistry& AR = AssetRegistryModule.Get();
		AR.SearchAllAssets(/*bSynchronousSearch=*/true);

		FARFilter Filter;
		Filter.bRecursivePaths = true;
		Filter.PackagePaths.Add(*PathFilter);
		Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());

		TArray<FAssetData> Assets;
		AR.GetAssets(Filter, Assets);

		Out.Reserve(Assets.Num());
		for (const FAssetData& Data : Assets)
		{
			FString ParentClass;
			Data.GetTagValue(TEXT("ParentClass"), ParentClass);
			if (ParentClass.IsEmpty())
			{
				// Older asset tags use NativeParentClass; try that as a fallback.
				Data.GetTagValue(TEXT("NativeParentClass"), ParentClass);
			}

			const FString PackagePath = Data.PackageName.ToString();
			const FString FileOnDisk = FPackageName::LongPackageNameToFilename(
				PackagePath, FPackageName::GetAssetPackageExtension());
			const FString Modified = IsoDateForFile(FileOnDisk);

			TSharedRef<FJsonObject> Summary = FBlueprintReaderWireJson::SummaryToJson(
				PackagePath, Data.AssetName.ToString(), ParentClass, Modified);
			Out.Add(MakeShared<FJsonValueObject>(Summary));
		}

		Out.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
		{
			return A->AsObject()->GetStringField(TEXT("asset_path")) <
			       B->AsObject()->GetStringField(TEXT("asset_path"));
		});

		const FString Json = FBlueprintReaderWireJson::WriteArrayString(Out, bPretty);
		return EmitJson(Json, OutputPath);
	}
}

namespace
{
	// Shared dispatch — used by both the one-shot Main path and the daemon
	// loop. Treats `Params` as a commandlet-arg-style string and runs a
	// single op against it.
	int32 RunOneOp(const FString& Params);

	// Daemon mode: read newline-terminated commandlet-arg strings from stdin,
	// run each through RunOneOp, print a sentinel after each so the MCP
	// backend knows the result file is ready.
	int32 RunDaemon();
}

int32 UBlueprintReaderCommandlet::Main(const FString& Params)
{
	if (FParse::Param(*Params, TEXT("Daemon")))
	{
		return RunDaemon();
	}
	return RunOneOp(Params);
}

namespace
{
int32 RunOneOp(const FString& Params)
{
	EOp Op;
	if (!ParseOp(Params, Op)) return 1;

	const bool bPretty = !FParse::Param(*Params, TEXT("Compact"));
	const FString OutputPath = ResolveOutputPath(Params);

	if (Op == EOp::List)
	{
		return RunListOp(Params, OutputPath, bPretty);
	}
	if (Op == EOp::AddVariable)     return RunAddVariableOp(Params, OutputPath, bPretty);
	if (Op == EOp::SetNodePosition) return RunSetNodePositionOp(Params, OutputPath, bPretty);
	if (Op == EOp::DeleteNode)      return RunDeleteNodeOp(Params, OutputPath, bPretty);

	const FString AssetPath = ResolveAssetPath(Params);
	if (AssetPath.IsEmpty())
	{
		UE_LOG(LogBlueprintReader, Error, TEXT("Missing required argument: -Asset=<asset path> (or -Path=)"));
		return 1;
	}

	if (Op == EOp::Legacy)
	{
		// Backward-compatible original behavior — emit the rich
		// FBlueprintReaderJson shape (camelCase, K2 extras).
		const TOptional<FBlueprintInfo> Info = FBlueprintIntrospector::Read(AssetPath);
		if (!Info.IsSet())
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("Failed to load blueprint at: %s"), *AssetPath);
			return 2;
		}
		const FString Json = FBlueprintReaderJson::ToString(*Info, bPretty);
		return EmitJson(Json, OutputPath);
	}

	const TOptional<FBlueprintInfo> Info = FBlueprintIntrospector::Read(AssetPath);
	if (!Info.IsSet())
	{
		UE_LOG(LogBlueprintReader, Error, TEXT("Failed to load blueprint at: %s"), *AssetPath);
		return 2;
	}

	switch (Op)
	{
	case EOp::Read:
	{
		auto Obj = FBlueprintReaderWireJson::MetadataToJson(*Info);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}
	case EOp::Graph:
	{
		FString GraphName;
		FParse::Value(*Params, TEXT("Graph="), GraphName);
		if (GraphName.IsEmpty())
		{
			GraphName = TEXT("EventGraph");
		}
		auto Obj = FBlueprintReaderWireJson::GraphToJson(*Info, GraphName);
		if (!Obj.IsValid())
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("Graph '%s' not found in %s"), *GraphName, *AssetPath);
			return 4;
		}
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj.ToSharedRef(), bPretty), OutputPath);
	}
	case EOp::Function:
	{
		FString FunctionName;
		FParse::Value(*Params, TEXT("Function="), FunctionName);
		if (FunctionName.IsEmpty())
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("Missing required argument: -Function=<name>"));
			return 1;
		}
		auto Obj = FBlueprintReaderWireJson::FunctionToJson(*Info, FunctionName);
		if (!Obj.IsValid())
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("Function '%s' not found in %s"), *FunctionName, *AssetPath);
			return 4;
		}
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj.ToSharedRef(), bPretty), OutputPath);
	}
	case EOp::Variables:
	{
		auto Vars = FBlueprintReaderWireJson::VariablesToJson(*Info);
		return EmitJson(FBlueprintReaderWireJson::WriteArrayString(Vars, bPretty), OutputPath);
	}
	case EOp::Find:
	{
		FString Query;
		FParse::Value(*Params, TEXT("Query="), Query);
		FString Kind;
		FParse::Value(*Params, TEXT("Kind="), Kind);
		// Permit empty Query when Kind is set (kind-only filter).
		if (Query.IsEmpty() && Kind.IsEmpty())
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("find_node requires -Query=<substring> and/or -Kind=<extra>"));
			return 1;
		}
		auto Nodes = FBlueprintReaderWireJson::FindNodesAsJson(*Info, Query, Kind);
		return EmitJson(FBlueprintReaderWireJson::WriteArrayString(Nodes, bPretty), OutputPath);
	}
	default:
		UE_LOG(LogBlueprintReader, Error, TEXT("Unsupported op"));
		return 1;
	}
}

int32 RunDaemon()
{
	UE_LOG(LogBlueprintReader, Display,
		TEXT("BlueprintReader daemon started; awaiting commandlet-arg lines on stdin"));

	// Use Windows API directly for stdio so UE's runtime redirection (which
	// can route C stdio through its log/output system) doesn't intercept the
	// stream. We need raw bytes hitting the pipe in both directions.
	HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
	HANDLE hIn  = GetStdHandle(STD_INPUT_HANDLE);

	auto WriteAll = [hOut](const char* s, DWORD n)
	{
		while (n > 0)
		{
			DWORD wrote = 0;
			if (!WriteFile(hOut, s, n, &wrote, nullptr) || wrote == 0) return false;
			s += wrote;
			n -= wrote;
		}
		return true;
	};

	const char ready[] = "__BPR_READY__\n";
	WriteAll(ready, (DWORD)(sizeof(ready) - 1));

	// Read a single line (terminated by '\n') from hIn into Out. Returns
	// false on EOF / error.
	auto ReadLine = [hIn](FString& Out) -> bool
	{
		Out.Reset();
		char ch;
		while (true)
		{
			DWORD got = 0;
			if (!ReadFile(hIn, &ch, 1, &got, nullptr) || got == 0)
			{
				return !Out.IsEmpty();  // EOF: return what we have if any
			}
			if (ch == '\r') continue;
			if (ch == '\n') return true;
			Out.AppendChar(static_cast<TCHAR>(ch));
		}
	};

	while (true)
	{
		FString Line;
		if (!ReadLine(Line))
		{
			UE_LOG(LogBlueprintReader, Display, TEXT("Daemon: stdin closed; exiting"));
			return 0;
		}
		Line.TrimStartAndEndInline();
		if (Line.IsEmpty()) continue;
		if (Line.Equals(TEXT("QUIT"), ESearchCase::IgnoreCase))
		{
			UE_LOG(LogBlueprintReader, Display, TEXT("Daemon: QUIT received"));
			return 0;
		}

		UE_LOG(LogBlueprintReader, Display, TEXT("Daemon: received line: %s"), *Line);
		const int32 Code = RunOneOp(Line);
		const FString DoneStr = FString::Printf(TEXT("__BPR_DONE %d__\n"), Code);
		const auto DoneAnsi = StringCast<ANSICHAR>(*DoneStr);
		WriteAll(DoneAnsi.Get(), (DWORD)FCStringAnsi::Strlen(DoneAnsi.Get()));
	}
}
} // namespace
