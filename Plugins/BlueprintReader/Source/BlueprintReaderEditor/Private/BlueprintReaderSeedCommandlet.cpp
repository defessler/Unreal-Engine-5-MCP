#include "BlueprintReaderSeedCommandlet.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Factories/BlueprintFactory.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_VariableGet.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "HAL/FileManager.h"   // REL-22: read existing .uasset bytes (no load)
#include "Misc/CommandLine.h"  // REL-22: -Force overwrite check
#include "Misc/FileHelper.h"   // REL-22: LoadFileToArray
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

DEFINE_LOG_CATEGORY_STATIC(LogBlueprintReaderSeed, Log, All);

UBPRSeedCommandlet::UBPRSeedCommandlet()
{
	IsClient = false;
	IsServer = false;
	IsEditor = true;
	LogToConsole = true;
	ShowErrorCount = false;
}

namespace
{
	struct FNewBlueprint
	{
		UBlueprint* Blueprint = nullptr;
		UPackage* Package = nullptr;
		FString PackageName;
	};

	bool CreateBlueprint(const FString& PackageName, const FString& AssetName,
	                     UClass* ParentClass, FNewBlueprint& Out)
	{
		// REL-22: the seeder writes at FIXED /Game/AI paths. If a USER asset
		// already lives at one of them, FactoryCreateNew would silently
		// replace it. A prior SEED is fine to overwrite (re-seeding is the
		// documented workflow) — we recognize our own output by the marker
		// variable every seed BP carries.
		//
		// CRUCIAL: do NOT LoadObject the existing asset to inspect it. Loading
		// pulls it into memory under this exact name, and FactoryCreateNew
		// below then fatal-errors ("renaming on top of an existing object")
		// when the BP reinstancer tries to move the old generated class aside.
		// Instead, read the marker variable's NAME straight out of the
		// existing .uasset's name table on disk (a UBlueprint stores every
		// member-variable name verbatim there) — lock-free, load-free, and it
		// never perturbs the object graph the factory is about to build.
		const FString ExistingFile = FPackageName::LongPackageNameToFilename(
			PackageName, FPackageName::GetAssetPackageExtension());
		if (IFileManager::Get().FileExists(*ExistingFile))
		{
			bool bLooksLikeOurSeed = false;
			TArray<uint8> Bytes;
			if (FFileHelper::LoadFileToArray(Bytes, *ExistingFile))
			{
				static const char* kMarker = "BPRSeedMarker";
				const int32 NeedleLen = 13;  // strlen("BPRSeedMarker")
				for (int32 i = 0; i + NeedleLen <= Bytes.Num(); ++i)
				{
					if (FMemory::Memcmp(&Bytes[i], kMarker, NeedleLen) == 0)
					{
						bLooksLikeOurSeed = true;
						break;
					}
				}
			}
			const bool bForce = FParse::Param(FCommandLine::Get(), TEXT("Force"));
			if (!bLooksLikeOurSeed && !bForce)
			{
				UE_LOG(LogBlueprintReaderSeed, Error,
					TEXT("REFUSING to overwrite %s — an asset already exists there "
						 "and does not look like a previous seed (no BPRSeedMarker "
						 "variable). Move your asset or pass -Force to overwrite."),
					*ExistingFile);
				return false;
			}
			// Proceeding to overwrite our own prior seed (or -Force). The seed
			// RECREATES the asset wholesale, and FactoryCreateNew fatal-asserts
			// if a BP of this name already exists in the package — so give it a
			// clean slate: delete the on-disk file. (A fresh commandlet has
			// nothing of /Game/AI loaded into memory, so the package name is
			// free once the file is gone. This is also why we never LoadObject
			// the existing asset above — loading is exactly what would make the
			// name un-free and crash the factory.)
			IFileManager::Get().Delete(*ExistingFile, /*RequireExists=*/false,
				/*EvenReadOnly=*/true);
		}

		UPackage* Package = CreatePackage(*PackageName);
		if (!IsValid(Package))
		{
			UE_LOG(LogBlueprintReaderSeed, Error, TEXT("CreatePackage failed: %s"), *PackageName);
			return false;
		}
		Package->FullyLoad();

		UBlueprintFactory* Factory = NewObject<UBlueprintFactory>();
		Factory->ParentClass = ParentClass;

		UBlueprint* BP = CastChecked<UBlueprint>(Factory->FactoryCreateNew(
			UBlueprint::StaticClass(), Package, *AssetName, RF_Public | RF_Standalone, nullptr,
			GWarn));
		if (!IsValid(BP))
		{
			UE_LOG(LogBlueprintReaderSeed, Error, TEXT("FactoryCreateNew failed: %s"), *PackageName);
			return false;
		}
		BP->MarkPackageDirty();

		// REL-22: stamp every seed BP with a marker variable so a future
		// re-seed can tell "previous seed output (safe to overwrite)" from
		// "user asset that happens to share the name" (refused above).
		{
			FEdGraphPinType MarkerType;
			MarkerType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
			FBlueprintEditorUtils::AddMemberVariable(
				BP, TEXT("BPRSeedMarker"), MarkerType, TEXT("true"));
		}

		Out.Blueprint = BP;
		Out.Package = Package;
		Out.PackageName = PackageName;
		return true;
	}

	void AddVariable(UBlueprint* BP, FName Name, const FEdGraphPinType& Type,
	                 const FString& DefaultValue, FName Category, bool bReplicated, bool bEditable)
	{
		FBlueprintEditorUtils::AddMemberVariable(BP, Name, Type, DefaultValue);
		const int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(BP, Name);
		if (Index == INDEX_NONE)
		{
			return;
		}

		FBPVariableDescription& Var = BP->NewVariables[Index];
		if (!Category.IsNone())
		{
			Var.Category = FText::FromName(Category);
		}
		if (bEditable)
		{
			Var.PropertyFlags |= CPF_Edit | CPF_BlueprintVisible;
		}
		if (bReplicated)
		{
			Var.PropertyFlags |= CPF_Net;
			Var.ReplicationCondition = COND_None;
		}
	}

	UEdGraph* AddBlueprintFunction(UBlueprint* BP, FName FunctionName)
	{
		UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
			BP, FunctionName,
			UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		FBlueprintEditorUtils::AddFunctionGraph<UClass>(BP, NewGraph, /*bIsUserCreated=*/true, nullptr);
		return NewGraph;
	}

	void AddFunctionInput(UEdGraph* Graph, FName ParamName, const FEdGraphPinType& Type)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
			{
				Entry->CreateUserDefinedPin(ParamName, Type, EGPD_Output, /*bUseUniqueName=*/false);
				return;
			}
		}
	}

	void AddFunctionOutput(UEdGraph* Graph, FName ParamName, const FEdGraphPinType& Type)
	{
		UK2Node_FunctionResult* Result = nullptr;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (UK2Node_FunctionResult* R = Cast<UK2Node_FunctionResult>(Node))
			{
				Result = R;
				break;
			}
		}
		if (!IsValid(Result))
		{
			// No Result node yet — spawn one. Functions without outputs don't
			// auto-spawn a Result node, so we have to create it manually.
			Result = NewObject<UK2Node_FunctionResult>(Graph);
			Result->CreateNewGuid();
			Result->NodePosX = 240;
			Result->NodePosY = 0;
			Graph->AddNode(Result, /*bFromUI=*/false, /*bSelectNewNode=*/false);
			Result->PostPlacedNewNode();
			Result->AllocateDefaultPins();
		}
		Result->CreateUserDefinedPin(ParamName, Type, EGPD_Input, /*bUseUniqueName=*/false);
	}

	void AddLocalVariable(UEdGraph* Graph, FName Name, const FEdGraphPinType& Type)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
			{
				FBPVariableDescription Local;
				Local.VarName = Name;
				Local.VarGuid = FGuid::NewGuid();
				Local.VarType = Type;
				Entry->LocalVariables.Add(MoveTemp(Local));
				return;
			}
		}
	}

	void BuildEventGraph(UBlueprint* BP)
	{
		UEdGraph* Graph = FBlueprintEditorUtils::FindEventGraph(BP);
		if (!IsValid(Graph))
		{
			return;
		}

		// BeginPlay event (existing or new).
		UK2Node_Event* BeginPlay = nullptr;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (UK2Node_Event* Evt = Cast<UK2Node_Event>(Node))
			{
				if (Evt->EventReference.GetMemberName() == TEXT("ReceiveBeginPlay"))
				{
					BeginPlay = Evt;
					break;
				}
			}
		}
		if (!IsValid(BeginPlay))
		{
			BeginPlay = NewObject<UK2Node_Event>(Graph);
			BeginPlay->EventReference.SetExternalMember(TEXT("ReceiveBeginPlay"), AActor::StaticClass());
			BeginPlay->bOverrideFunction = true;
			BeginPlay->CreateNewGuid();
			BeginPlay->NodePosX = 0;
			BeginPlay->NodePosY = 0;
			Graph->AddNode(BeginPlay, /*bFromUI=*/false, /*bSelectNewNode=*/false);
			BeginPlay->PostPlacedNewNode();
			BeginPlay->AllocateDefaultPins();
		}

		// Get bIsAlive (VariableGet).
		UK2Node_VariableGet* GetIsAlive = NewObject<UK2Node_VariableGet>(Graph);
		GetIsAlive->VariableReference.SetSelfMember(TEXT("bIsAlive"));
		GetIsAlive->CreateNewGuid();
		GetIsAlive->NodePosX = -16;
		GetIsAlive->NodePosY = 160;
		Graph->AddNode(GetIsAlive, false, false);
		GetIsAlive->PostPlacedNewNode();
		GetIsAlive->AllocateDefaultPins();

		// Branch.
		UK2Node_IfThenElse* Branch = NewObject<UK2Node_IfThenElse>(Graph);
		Branch->CreateNewGuid();
		Branch->NodePosX = 240;
		Branch->NodePosY = 0;
		Graph->AddNode(Branch, false, false);
		Branch->PostPlacedNewNode();
		Branch->AllocateDefaultPins();

		// PrintString call (KismetSystemLibrary::PrintString).
		UK2Node_CallFunction* PrintNode = NewObject<UK2Node_CallFunction>(Graph);
		PrintNode->FunctionReference.SetExternalMember(
			GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString),
			UKismetSystemLibrary::StaticClass());
		PrintNode->CreateNewGuid();
		PrintNode->NodePosX = 480;
		PrintNode->NodePosY = 0;
		Graph->AddNode(PrintNode, false, false);
		PrintNode->PostPlacedNewNode();
		PrintNode->AllocateDefaultPins();

		// Wire it up.
		UEdGraphPin* PlayThen      = BeginPlay->FindPin(UEdGraphSchema_K2::PN_Then);
		UEdGraphPin* BranchExec    = Branch->GetExecPin();
		UEdGraphPin* IsAliveOut    = GetIsAlive->FindPin(TEXT("bIsAlive"));
		UEdGraphPin* BranchCond    = Branch->GetConditionPin();
		UEdGraphPin* BranchThen    = Branch->GetThenPin();
		UEdGraphPin* PrintExecIn   = PrintNode->GetExecPin();

		if (PlayThen    && BranchExec)
		{
			PlayThen->MakeLinkTo(BranchExec);
		}
		if (IsAliveOut  && BranchCond)
		{
			IsAliveOut->MakeLinkTo(BranchCond);
		}
		if (BranchThen  && PrintExecIn)
		{
			BranchThen->MakeLinkTo(PrintExecIn);
		}
	}

	bool SavePackageToDisk(UPackage* Package, UBlueprint* Asset, const FString& PackageName)
	{
		const FString FileName = FPackageName::LongPackageNameToFilename(
			PackageName, FPackageName::GetAssetPackageExtension());

		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		Args.Error = GError;

		const bool bOk = UPackage::SavePackage(Package, Asset, *FileName, Args);
		if (!bOk)
		{
			UE_LOG(LogBlueprintReaderSeed, Error, TEXT("Failed to save package: %s"), *FileName);
		}
		else
		{
			UE_LOG(LogBlueprintReaderSeed, Display, TEXT("Saved package: %s"), *FileName);
		}
		return bOk;
	}

	bool SeedBP_TestEnemy()
	{
		const FString PackageName = TEXT("/Game/AI/BP_TestEnemy");
		const FString AssetName = TEXT("BP_TestEnemy");

		FNewBlueprint NB;
		if (!CreateBlueprint(PackageName, AssetName, AActor::StaticClass(), NB))
		{
			return false;
		}
		UBlueprint* BP = NB.Blueprint;

		// Variables: replicated Health (float), MaxHealth (float, editable), AggroTarget (object Actor, replicated).
		FEdGraphPinType FloatType;
		FloatType.PinCategory = UEdGraphSchema_K2::PC_Real;
		FloatType.PinSubCategory = UEdGraphSchema_K2::PC_Float;

		FEdGraphPinType BoolType;
		BoolType.PinCategory = UEdGraphSchema_K2::PC_Boolean;

		FEdGraphPinType IntType;
		IntType.PinCategory = UEdGraphSchema_K2::PC_Int;

		FEdGraphPinType ActorObjType;
		ActorObjType.PinCategory = UEdGraphSchema_K2::PC_Object;
		ActorObjType.PinSubCategoryObject = AActor::StaticClass();

		AddVariable(BP, TEXT("Health"),       FloatType,    TEXT("100.0"), TEXT("Combat"), /*replicated=*/true,  /*editable=*/true);
		AddVariable(BP, TEXT("MaxHealth"),    FloatType,    TEXT("100.0"), TEXT("Combat"), false, true);
		AddVariable(BP, TEXT("AggroTarget"),  ActorObjType, FString(),     TEXT("AI"),     true,  false);
		AddVariable(BP, TEXT("bIsAlive"),     BoolType,     TEXT("true"),  TEXT("Combat"), false, true);

		// Custom function: TakeDamage(Damage:float) -> Killed:bool, with local NewHealth.
		UEdGraph* TakeDamage = AddBlueprintFunction(BP, TEXT("TakeDamage"));
		AddFunctionInput(TakeDamage,  TEXT("Damage"), FloatType);
		AddFunctionOutput(TakeDamage, TEXT("Killed"), BoolType);
		AddLocalVariable(TakeDamage,  TEXT("NewHealth"), FloatType);

		// Custom function: OnDeath() with no params.
		AddBlueprintFunction(BP, TEXT("OnDeath"));

		// Event graph: BeginPlay -> Branch (Get bIsAlive condition) -> PrintString.
		BuildEventGraph(BP);

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
		FKismetEditorUtilities::CompileBlueprint(BP);

		return SavePackageToDisk(NB.Package, BP, PackageName);
	}

	bool SeedBP_TestPickup()
	{
		const FString PackageName = TEXT("/Game/AI/BP_TestPickup");
		const FString AssetName = TEXT("BP_TestPickup");

		FNewBlueprint NB;
		if (!CreateBlueprint(PackageName, AssetName, AActor::StaticClass(), NB))
		{
			return false;
		}
		UBlueprint* BP = NB.Blueprint;

		FEdGraphPinType IntType;
		IntType.PinCategory = UEdGraphSchema_K2::PC_Int;

		FEdGraphPinType StringType;
		StringType.PinCategory = UEdGraphSchema_K2::PC_String;

		AddVariable(BP, TEXT("ScoreValue"), IntType,    TEXT("10"),     TEXT("Pickup"), false, true);
		AddVariable(BP, TEXT("PickupName"), StringType, TEXT("\"Coin\""), TEXT("Pickup"), false, true);

		// Custom function: Collect()
		AddBlueprintFunction(BP, TEXT("Collect"));

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
		FKismetEditorUtilities::CompileBlueprint(BP);

		return SavePackageToDisk(NB.Package, BP, PackageName);
	}
}

int32 UBPRSeedCommandlet::Main(const FString& /*Params*/)
{
	UE_LOG(LogBlueprintReaderSeed, Display, TEXT("Seeding test blueprints under /Game/AI..."));

	bool bAllOk = true;
	bAllOk &= SeedBP_TestEnemy();
	bAllOk &= SeedBP_TestPickup();

	if (bAllOk)
	{
		UE_LOG(LogBlueprintReaderSeed, Display, TEXT("Seeding complete."));
		return 0;
	}
	UE_LOG(LogBlueprintReaderSeed, Error, TEXT("Seeding had errors."));
	return 1;
}
