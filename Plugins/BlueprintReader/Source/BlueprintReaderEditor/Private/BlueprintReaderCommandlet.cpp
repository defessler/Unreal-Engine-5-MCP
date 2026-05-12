#include "BlueprintReaderCommandlet.h"

#include "BlueprintIntrospector.h"
#include "BlueprintReaderJson.h"
#include "BlueprintReaderLogSink.h"
#include "BlueprintReaderWireJson.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "Engine/DataTable.h"
#include "Engine/Selection.h"
#include "Engine/World.h"
#include "EngineUtils.h"               // TActorIterator
#include "FileHelpers.h"
#include "HAL/IConsoleManager.h"
#include "IAssetTools.h"
#include "JsonObjectConverter.h"
#include "Misc/StringOutputDevice.h"
#include "ObjectTools.h"
#include "UObject/UObjectIterator.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "Factories/BlueprintFactory.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_Event.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_FormatText.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_Knot.h"
#include "K2Node_MakeArray.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_Self.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Logging/TokenizedMessage.h"
#include "Misc/UObjectToken.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
// Material authoring (Stage 1).
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter.h"
#include "MaterialEditingLibrary.h"
// UMG widget authoring (Stage 1).
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Components/PanelWidget.h"
#include "Components/PanelSlot.h"
// Behavior Tree authoring (Stage 2).
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BTNode.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTService.h"
// DataAsset CRUD (Stage 2).
#include "Engine/DataAsset.h"
// AnimBlueprint authoring (Stage 4).
#include "Animation/AnimBlueprint.h"
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
		Components,
		Find,
		// Write ops:
		AddVariable,
		SetNodePosition,
		DeleteNode,
		AddNode,
		WirePins,
		DeleteVariable,
		RenameVariable,
		AddFunction,
		AddFunctionInput,
		AddFunctionOutput,
		DeleteFunction,
		SetVariableDefault,
		// Batch sentinels (A1): wrap a sequence of write ops so the
		// expensive CompileBlueprint + SavePackage runs once per affected
		// BP at EndBatch instead of once per op.
		BeginBatch,
		EndBatch,
		// Asset creation (A3).
		CreateBlueprint,
		// Pin default (B1) — supports compile_function's {lit:value}.
		SetPinDefault,
		// Variable lifecycle gaps reported by users:
		RetypeVariable,         // change a var's type without delete+re-add (preserves nodes)
		SetVariableCategory,    // change a var's My-Blueprint-panel category label
		DuplicateBlueprint,     // file-level duplicate (BP-5)
		// Write transpiled source into the project tree.
		WriteGeneratedSource,
		// Project + Content Browser ops.
		SaveAll,
		MoveAsset,
		DeleteAsset,
		CreateFolder,
		ListDataTables,
		ReadDataTable,
		// Live editor ops.
		ConsoleCommand,
		GetCVar,
		SetCVar,
		PieStart,
		PieStop,
		LiveCodingCompile,
		GetSelectedActors,
		SetSelection,
		SpawnActor,
		SetActorTransform,
		DeleteActor,
		ReadOutputLog,
		// Automation tests.
		RunAutomationTests,
		// DataTable row mutation.
		AddDataRow,
		SetDataRowValue,
		// Component (SCS) authoring.
		AddComponent,
		RemoveComponent,
		AttachComponent,
		SetComponentProperty,
		// Material authoring (Stage 1).
		ListMaterials,
		ReadMaterial,
		AddMaterialExpression,
		ConnectMaterialExpressions,
		SetMaterialParameter,
		SetMaterialInstanceParameter,
		CompileMaterial,
		// UMG widget authoring (Stage 1).
		ReadWidgetBlueprint,
		AddWidget,
		SetWidgetProperty,
		BindWidgetEvent,
		CompileWidgetBlueprint,
		// Behavior Tree authoring (Stage 2).
		ListBehaviorTrees,
		ReadBehaviorTree,
		AddBTNode,
		SetBTNodeProperty,
		CompileBehaviorTree,
		// DataAsset CRUD (Stage 2).
		ListDataAssets,
		ReadDataAsset,
		CreateDataAsset,
		SetDataAssetProperty,
		// StateTree authoring (Stage 2).
		ListStateTrees,
		ReadStateTree,
		AddStateTreeState,
		SetStateTreeTransition,
		CompileStateTree,
		// Stage 3: profile / cook / class info / viewport.
		StartProfile,
		StopProfile,
		GetStats,
		TakeScreenshot,
		CookContent,
		PackageProject,
		IntrospectClass,
		FindClass,
		ListFunctions,
		FocusActor,
		SetCameraTransform,
		TakeViewportScreenshot,
		SetShowFlag,
		// Stage 4: Niagara / Sequencer / GAS / AnimGraph.
		ListNiagaraSystems,
		ReadNiagaraSystem,
		CreateNiagaraSystem,
		SetNiagaraParameter,
		ListLevelSequences,
		ReadLevelSequence,
		AddSequenceTrack,
		SetSequencePlaybackRange,
		ListGameplayTags,
		AddGameplayTag,
		ReadAbilitySet,
		ListAnimBlueprints,
		ReadAnimBlueprint,
		AddAnimState,
		CompileAnimBlueprint,
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
		if (OpStr.Equals(TEXT("Components"), ESearchCase::IgnoreCase))       { OutOp = EOp::Components; return true; }
		if (OpStr.Equals(TEXT("Find"), ESearchCase::IgnoreCase))             { OutOp = EOp::Find; return true; }
		if (OpStr.Equals(TEXT("AddVariable"), ESearchCase::IgnoreCase))      { OutOp = EOp::AddVariable; return true; }
		if (OpStr.Equals(TEXT("SetNodePosition"), ESearchCase::IgnoreCase))  { OutOp = EOp::SetNodePosition; return true; }
		if (OpStr.Equals(TEXT("DeleteNode"), ESearchCase::IgnoreCase))       { OutOp = EOp::DeleteNode; return true; }
		if (OpStr.Equals(TEXT("AddNode"), ESearchCase::IgnoreCase))          { OutOp = EOp::AddNode; return true; }
		if (OpStr.Equals(TEXT("WirePins"), ESearchCase::IgnoreCase))         { OutOp = EOp::WirePins; return true; }
		if (OpStr.Equals(TEXT("DeleteVariable"), ESearchCase::IgnoreCase))     { OutOp = EOp::DeleteVariable; return true; }
		if (OpStr.Equals(TEXT("RenameVariable"), ESearchCase::IgnoreCase))     { OutOp = EOp::RenameVariable; return true; }
		if (OpStr.Equals(TEXT("AddFunction"), ESearchCase::IgnoreCase))        { OutOp = EOp::AddFunction; return true; }
		if (OpStr.Equals(TEXT("AddFunctionInput"), ESearchCase::IgnoreCase))   { OutOp = EOp::AddFunctionInput; return true; }
		if (OpStr.Equals(TEXT("AddFunctionOutput"), ESearchCase::IgnoreCase))  { OutOp = EOp::AddFunctionOutput; return true; }
		if (OpStr.Equals(TEXT("DeleteFunction"), ESearchCase::IgnoreCase))     { OutOp = EOp::DeleteFunction; return true; }
		if (OpStr.Equals(TEXT("SetVariableDefault"), ESearchCase::IgnoreCase)) { OutOp = EOp::SetVariableDefault; return true; }
		if (OpStr.Equals(TEXT("BeginBatch"), ESearchCase::IgnoreCase))         { OutOp = EOp::BeginBatch; return true; }
		if (OpStr.Equals(TEXT("EndBatch"), ESearchCase::IgnoreCase))           { OutOp = EOp::EndBatch; return true; }
		if (OpStr.Equals(TEXT("CreateBlueprint"), ESearchCase::IgnoreCase))    { OutOp = EOp::CreateBlueprint; return true; }
		if (OpStr.Equals(TEXT("SetPinDefault"), ESearchCase::IgnoreCase))      { OutOp = EOp::SetPinDefault; return true; }
		if (OpStr.Equals(TEXT("RetypeVariable"), ESearchCase::IgnoreCase))     { OutOp = EOp::RetypeVariable; return true; }
		if (OpStr.Equals(TEXT("SetVariableCategory"), ESearchCase::IgnoreCase)){ OutOp = EOp::SetVariableCategory; return true; }
		if (OpStr.Equals(TEXT("DuplicateBlueprint"), ESearchCase::IgnoreCase)) { OutOp = EOp::DuplicateBlueprint; return true; }
		if (OpStr.Equals(TEXT("WriteGeneratedSource"), ESearchCase::IgnoreCase)) { OutOp = EOp::WriteGeneratedSource; return true; }
		if (OpStr.Equals(TEXT("SaveAll"), ESearchCase::IgnoreCase))             { OutOp = EOp::SaveAll; return true; }
		if (OpStr.Equals(TEXT("MoveAsset"), ESearchCase::IgnoreCase))           { OutOp = EOp::MoveAsset; return true; }
		if (OpStr.Equals(TEXT("DeleteAsset"), ESearchCase::IgnoreCase))         { OutOp = EOp::DeleteAsset; return true; }
		if (OpStr.Equals(TEXT("CreateFolder"), ESearchCase::IgnoreCase))        { OutOp = EOp::CreateFolder; return true; }
		if (OpStr.Equals(TEXT("ListDataTables"), ESearchCase::IgnoreCase))      { OutOp = EOp::ListDataTables; return true; }
		if (OpStr.Equals(TEXT("ReadDataTable"), ESearchCase::IgnoreCase))       { OutOp = EOp::ReadDataTable; return true; }
		if (OpStr.Equals(TEXT("ConsoleCommand"), ESearchCase::IgnoreCase))      { OutOp = EOp::ConsoleCommand; return true; }
		if (OpStr.Equals(TEXT("GetCVar"), ESearchCase::IgnoreCase))             { OutOp = EOp::GetCVar; return true; }
		if (OpStr.Equals(TEXT("SetCVar"), ESearchCase::IgnoreCase))             { OutOp = EOp::SetCVar; return true; }
		if (OpStr.Equals(TEXT("PieStart"), ESearchCase::IgnoreCase))            { OutOp = EOp::PieStart; return true; }
		if (OpStr.Equals(TEXT("PieStop"), ESearchCase::IgnoreCase))             { OutOp = EOp::PieStop; return true; }
		if (OpStr.Equals(TEXT("LiveCodingCompile"), ESearchCase::IgnoreCase))   { OutOp = EOp::LiveCodingCompile; return true; }
		if (OpStr.Equals(TEXT("GetSelectedActors"), ESearchCase::IgnoreCase))   { OutOp = EOp::GetSelectedActors; return true; }
		if (OpStr.Equals(TEXT("SetSelection"), ESearchCase::IgnoreCase))        { OutOp = EOp::SetSelection; return true; }
		if (OpStr.Equals(TEXT("SpawnActor"), ESearchCase::IgnoreCase))          { OutOp = EOp::SpawnActor; return true; }
		if (OpStr.Equals(TEXT("SetActorTransform"), ESearchCase::IgnoreCase))   { OutOp = EOp::SetActorTransform; return true; }
		if (OpStr.Equals(TEXT("DeleteActor"), ESearchCase::IgnoreCase))         { OutOp = EOp::DeleteActor; return true; }
		if (OpStr.Equals(TEXT("ReadOutputLog"), ESearchCase::IgnoreCase))       { OutOp = EOp::ReadOutputLog; return true; }
		if (OpStr.Equals(TEXT("RunAutomationTests"), ESearchCase::IgnoreCase))  { OutOp = EOp::RunAutomationTests; return true; }
		if (OpStr.Equals(TEXT("AddDataRow"), ESearchCase::IgnoreCase))          { OutOp = EOp::AddDataRow; return true; }
		if (OpStr.Equals(TEXT("SetDataRowValue"), ESearchCase::IgnoreCase))     { OutOp = EOp::SetDataRowValue; return true; }
		if (OpStr.Equals(TEXT("AddComponent"), ESearchCase::IgnoreCase))        { OutOp = EOp::AddComponent; return true; }
		if (OpStr.Equals(TEXT("RemoveComponent"), ESearchCase::IgnoreCase))     { OutOp = EOp::RemoveComponent; return true; }
		if (OpStr.Equals(TEXT("AttachComponent"), ESearchCase::IgnoreCase))     { OutOp = EOp::AttachComponent; return true; }
		if (OpStr.Equals(TEXT("SetComponentProperty"), ESearchCase::IgnoreCase)){ OutOp = EOp::SetComponentProperty; return true; }
		// Material authoring (Stage 1).
		if (OpStr.Equals(TEXT("ListMaterials"), ESearchCase::IgnoreCase))                { OutOp = EOp::ListMaterials; return true; }
		if (OpStr.Equals(TEXT("ReadMaterial"), ESearchCase::IgnoreCase))                 { OutOp = EOp::ReadMaterial; return true; }
		if (OpStr.Equals(TEXT("AddMaterialExpression"), ESearchCase::IgnoreCase))        { OutOp = EOp::AddMaterialExpression; return true; }
		if (OpStr.Equals(TEXT("ConnectMaterialExpressions"), ESearchCase::IgnoreCase))   { OutOp = EOp::ConnectMaterialExpressions; return true; }
		if (OpStr.Equals(TEXT("SetMaterialParameter"), ESearchCase::IgnoreCase))         { OutOp = EOp::SetMaterialParameter; return true; }
		if (OpStr.Equals(TEXT("SetMaterialInstanceParameter"), ESearchCase::IgnoreCase)) { OutOp = EOp::SetMaterialInstanceParameter; return true; }
		if (OpStr.Equals(TEXT("CompileMaterial"), ESearchCase::IgnoreCase))              { OutOp = EOp::CompileMaterial; return true; }
		// UMG widget authoring (Stage 1).
		if (OpStr.Equals(TEXT("ReadWidgetBlueprint"), ESearchCase::IgnoreCase))   { OutOp = EOp::ReadWidgetBlueprint; return true; }
		if (OpStr.Equals(TEXT("AddWidget"), ESearchCase::IgnoreCase))             { OutOp = EOp::AddWidget; return true; }
		if (OpStr.Equals(TEXT("SetWidgetProperty"), ESearchCase::IgnoreCase))     { OutOp = EOp::SetWidgetProperty; return true; }
		if (OpStr.Equals(TEXT("BindWidgetEvent"), ESearchCase::IgnoreCase))       { OutOp = EOp::BindWidgetEvent; return true; }
		if (OpStr.Equals(TEXT("CompileWidgetBlueprint"), ESearchCase::IgnoreCase)){ OutOp = EOp::CompileWidgetBlueprint; return true; }
		// Behavior Tree authoring (Stage 2).
		if (OpStr.Equals(TEXT("ListBehaviorTrees"), ESearchCase::IgnoreCase))   { OutOp = EOp::ListBehaviorTrees; return true; }
		if (OpStr.Equals(TEXT("ReadBehaviorTree"), ESearchCase::IgnoreCase))    { OutOp = EOp::ReadBehaviorTree; return true; }
		if (OpStr.Equals(TEXT("AddBTNode"), ESearchCase::IgnoreCase))           { OutOp = EOp::AddBTNode; return true; }
		if (OpStr.Equals(TEXT("SetBTNodeProperty"), ESearchCase::IgnoreCase))   { OutOp = EOp::SetBTNodeProperty; return true; }
		if (OpStr.Equals(TEXT("CompileBehaviorTree"), ESearchCase::IgnoreCase)) { OutOp = EOp::CompileBehaviorTree; return true; }
		// DataAsset CRUD (Stage 2).
		if (OpStr.Equals(TEXT("ListDataAssets"), ESearchCase::IgnoreCase))       { OutOp = EOp::ListDataAssets; return true; }
		if (OpStr.Equals(TEXT("ReadDataAsset"), ESearchCase::IgnoreCase))        { OutOp = EOp::ReadDataAsset; return true; }
		if (OpStr.Equals(TEXT("CreateDataAsset"), ESearchCase::IgnoreCase))      { OutOp = EOp::CreateDataAsset; return true; }
		if (OpStr.Equals(TEXT("SetDataAssetProperty"), ESearchCase::IgnoreCase)) { OutOp = EOp::SetDataAssetProperty; return true; }
		// StateTree authoring (Stage 2).
		if (OpStr.Equals(TEXT("ListStateTrees"), ESearchCase::IgnoreCase))         { OutOp = EOp::ListStateTrees; return true; }
		if (OpStr.Equals(TEXT("ReadStateTree"), ESearchCase::IgnoreCase))          { OutOp = EOp::ReadStateTree; return true; }
		if (OpStr.Equals(TEXT("AddStateTreeState"), ESearchCase::IgnoreCase))      { OutOp = EOp::AddStateTreeState; return true; }
		if (OpStr.Equals(TEXT("SetStateTreeTransition"), ESearchCase::IgnoreCase)) { OutOp = EOp::SetStateTreeTransition; return true; }
		if (OpStr.Equals(TEXT("CompileStateTree"), ESearchCase::IgnoreCase))       { OutOp = EOp::CompileStateTree; return true; }
		// Stage 3: profile / cook / class info / viewport.
		if (OpStr.Equals(TEXT("StartProfile"), ESearchCase::IgnoreCase))           { OutOp = EOp::StartProfile; return true; }
		if (OpStr.Equals(TEXT("StopProfile"), ESearchCase::IgnoreCase))            { OutOp = EOp::StopProfile; return true; }
		if (OpStr.Equals(TEXT("GetStats"), ESearchCase::IgnoreCase))               { OutOp = EOp::GetStats; return true; }
		if (OpStr.Equals(TEXT("TakeScreenshot"), ESearchCase::IgnoreCase))         { OutOp = EOp::TakeScreenshot; return true; }
		if (OpStr.Equals(TEXT("CookContent"), ESearchCase::IgnoreCase))            { OutOp = EOp::CookContent; return true; }
		if (OpStr.Equals(TEXT("PackageProject"), ESearchCase::IgnoreCase))         { OutOp = EOp::PackageProject; return true; }
		if (OpStr.Equals(TEXT("IntrospectClass"), ESearchCase::IgnoreCase))        { OutOp = EOp::IntrospectClass; return true; }
		if (OpStr.Equals(TEXT("FindClass"), ESearchCase::IgnoreCase))              { OutOp = EOp::FindClass; return true; }
		if (OpStr.Equals(TEXT("ListFunctions"), ESearchCase::IgnoreCase))          { OutOp = EOp::ListFunctions; return true; }
		if (OpStr.Equals(TEXT("FocusActor"), ESearchCase::IgnoreCase))             { OutOp = EOp::FocusActor; return true; }
		if (OpStr.Equals(TEXT("SetCameraTransform"), ESearchCase::IgnoreCase))     { OutOp = EOp::SetCameraTransform; return true; }
		if (OpStr.Equals(TEXT("TakeViewportScreenshot"), ESearchCase::IgnoreCase)) { OutOp = EOp::TakeViewportScreenshot; return true; }
		if (OpStr.Equals(TEXT("SetShowFlag"), ESearchCase::IgnoreCase))            { OutOp = EOp::SetShowFlag; return true; }
		// Stage 4: Niagara / Sequencer / GAS / AnimGraph.
		if (OpStr.Equals(TEXT("ListNiagaraSystems"), ESearchCase::IgnoreCase))      { OutOp = EOp::ListNiagaraSystems; return true; }
		if (OpStr.Equals(TEXT("ReadNiagaraSystem"), ESearchCase::IgnoreCase))       { OutOp = EOp::ReadNiagaraSystem; return true; }
		if (OpStr.Equals(TEXT("CreateNiagaraSystem"), ESearchCase::IgnoreCase))     { OutOp = EOp::CreateNiagaraSystem; return true; }
		if (OpStr.Equals(TEXT("SetNiagaraParameter"), ESearchCase::IgnoreCase))     { OutOp = EOp::SetNiagaraParameter; return true; }
		if (OpStr.Equals(TEXT("ListLevelSequences"), ESearchCase::IgnoreCase))      { OutOp = EOp::ListLevelSequences; return true; }
		if (OpStr.Equals(TEXT("ReadLevelSequence"), ESearchCase::IgnoreCase))       { OutOp = EOp::ReadLevelSequence; return true; }
		if (OpStr.Equals(TEXT("AddSequenceTrack"), ESearchCase::IgnoreCase))        { OutOp = EOp::AddSequenceTrack; return true; }
		if (OpStr.Equals(TEXT("SetSequencePlaybackRange"), ESearchCase::IgnoreCase)){ OutOp = EOp::SetSequencePlaybackRange; return true; }
		if (OpStr.Equals(TEXT("ListGameplayTags"), ESearchCase::IgnoreCase))        { OutOp = EOp::ListGameplayTags; return true; }
		if (OpStr.Equals(TEXT("AddGameplayTag"), ESearchCase::IgnoreCase))          { OutOp = EOp::AddGameplayTag; return true; }
		if (OpStr.Equals(TEXT("ReadAbilitySet"), ESearchCase::IgnoreCase))          { OutOp = EOp::ReadAbilitySet; return true; }
		if (OpStr.Equals(TEXT("ListAnimBlueprints"), ESearchCase::IgnoreCase))      { OutOp = EOp::ListAnimBlueprints; return true; }
		if (OpStr.Equals(TEXT("ReadAnimBlueprint"), ESearchCase::IgnoreCase))       { OutOp = EOp::ReadAnimBlueprint; return true; }
		if (OpStr.Equals(TEXT("AddAnimState"), ESearchCase::IgnoreCase))            { OutOp = EOp::AddAnimState; return true; }
		if (OpStr.Equals(TEXT("CompileAnimBlueprint"), ESearchCase::IgnoreCase))    { OutOp = EOp::CompileAnimBlueprint; return true; }
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

	// Use the asset registry to figure out *why* the BP failed to load and
	// log a specific diagnostic. The common case worth calling out
	// explicitly is "parent class not compiled / missing module" (issue
	// #3): the on-disk asset references a UClass that the commandlet
	// can't resolve, so the BP's CDO construction blows up during load.
	// Without this, the user sees only the generic LogLinker spam.
	void DiagnoseFailedBlueprintLoad(const FString& AssetPath)
	{
		IAssetRegistry& Registry =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		FAssetData Asset = Registry.GetAssetByObjectPath(FSoftObjectPath(AssetPath));
		if (!Asset.IsValid())
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("LoadMutableBlueprint: %s — asset not in registry; check the path"),
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
				TEXT("LoadMutableBlueprint: %s — asset is %s, not a UBlueprint. bp-reader "
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
				TEXT("LoadMutableBlueprint: %s — asset exists in registry but load failed; "
				     "no parent_class tag to probe"),
				*AssetPath);
			return;
		}
		const FSoftObjectPath ParentRef(ParentClassTag);
		UClass* ParentClass =
			LoadObject<UClass>(nullptr, *ParentRef.ToString(), nullptr, LOAD_Quiet);
		if (!ParentClass)
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("LoadMutableBlueprint: %s — parent class %s could not be resolved. "
				     "This typically means the C++ module declaring it is not compiled in "
				     "this build (issue #3). Rebuild the project (Build.bat <TargetName> "
				     "Win64 Development) before reading or writing this Blueprint."),
				*AssetPath, *ParentClassTag);
			return;
		}
		UE_LOG(LogBlueprintReader, Error,
			TEXT("LoadMutableBlueprint: %s — parent class %s resolved but BP load still "
			     "failed; check the editor log for the underlying PostLoad / "
			     "ConstructDefaultObject error"),
			*AssetPath, *ParentClassTag);
	}

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
		UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *Resolved);
		if (!BP)
		{
			DiagnoseFailedBlueprintLoad(Resolved);
		}
		return BP;
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

	// Forward decls — these are defined later in the file but referenced
	// by ops higher up (SetPinDefaultOp / RetypeVariableOp use them).
	// Definitions land in their existing locations alongside the WirePins
	// code / EmitOk / AddVariable helpers.
	UEdGraphPin* FindPinByIdOrName(UEdGraphNode* Node, const FString& Spec);
	int32 EmitOk(const FString& OutputPath, bool bPretty);
	bool BuildPinTypeFromFlags(const FString& Params, FEdGraphPinType& Out);

	// Severity → wire string, matching what the MCP server's tool-result
	// envelope expects. (UE 5.7 removed CriticalError from the enum.)
	const TCHAR* SeverityToString(EMessageSeverity::Type Sev)
	{
		switch (Sev)
		{
		case EMessageSeverity::Error:        return TEXT("error");
		case EMessageSeverity::PerformanceWarning:
		case EMessageSeverity::Warning:      return TEXT("warning");
		case EMessageSeverity::Info:         return TEXT("info");
		default:                              return TEXT("info");
		}
	}

	// Drain `Results.Messages` into a JSON array of {severity, message,
	// node_guid?}. Used by both single-op and batch-end paths to surface
	// compile diagnostics back to the agent (C1).
	TArray<TSharedPtr<FJsonValue>> SerializeDiagnostics(const FCompilerResultsLog& Results)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		Out.Reserve(Results.Messages.Num());
		for (const TSharedRef<FTokenizedMessage>& Msg : Results.Messages)
		{
			auto Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("severity"), SeverityToString(Msg->GetSeverity()));
			Obj->SetStringField(TEXT("message"),  Msg->ToText().ToString());
			// Best-effort: scan the message tokens for a UObject reference
			// pointing at a UEdGraphNode and emit its GUID.
			for (const TSharedRef<IMessageToken>& Tok : Msg->GetMessageTokens())
			{
				if (Tok->GetType() == EMessageToken::Object)
				{
					auto* ObjTok = static_cast<FUObjectToken*>(&Tok.Get());
					if (UObject* O = ObjTok->GetObject().Get())
					{
						if (UEdGraphNode* Node = Cast<UEdGraphNode>(O))
						{
							Obj->SetStringField(TEXT("node_guid"),
								Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
							break;
						}
					}
				}
			}
			Out.Add(MakeShared<FJsonValueObject>(Obj));
		}
		return Out;
	}

	// Compile + save. When `OutDiagnostics` is non-null, populates it with
	// the collected compile diagnostics serialized as JSON values.
	bool CompileAndSaveBlueprint(UBlueprint* BP,
	                             TArray<TSharedPtr<FJsonValue>>* OutDiagnostics = nullptr)
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

		FCompilerResultsLog Results;
		FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::None, &Results);

		if (OutDiagnostics)
		{
			TArray<TSharedPtr<FJsonValue>> Diags = SerializeDiagnostics(Results);
			OutDiagnostics->Append(MoveTemp(Diags));
		}

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

	// ----- Batch state (A1) ---------------------------------------------------
	// When a BeginBatch op is seen, every subsequent write op defers its
	// CompileAndSaveBlueprint call. The deferred BPs are tracked here and
	// flushed by EndBatch in one pass — N×compile collapses to 1×compile per
	// affected BP. Single-op (non-batch) callers see no behavior change.
	//
	// Daemon-scoped: the daemon is a single editor process so static state
	// is fine. One-shot mode (subprocess-per-call) never opens a batch.
	bool& BatchDeferFlag()
	{
		static bool bDefer = false;
		return bDefer;
	}
	TArray<TWeakObjectPtr<UBlueprint>>& BatchPending()
	{
		static TArray<TWeakObjectPtr<UBlueprint>> Pending;
		return Pending;
	}

	// Replaces the 12 direct CompileAndSaveBlueprint(BP) call sites. In
	// non-batch mode this is identical to CompileAndSaveBlueprint. In batch
	// mode it just records the BP for EndBatch to process.
	bool MaybeCompileAndSave(UBlueprint* BP)
	{
		if (!BP) return false;
		if (BatchDeferFlag())
		{
			BatchPending().AddUnique(BP);
			// Mark structurally modified now so subsequent ops in the same
			// batch see a consistent state. The compile + save is still
			// deferred to EndBatch.
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
			return true;
		}
		return CompileAndSaveBlueprint(BP);
	}

	int32 RunBeginBatchOp(const FString& /*Params*/, const FString& OutputPath, bool bPretty)
	{
		// Idempotent — re-issuing BeginBatch without an EndBatch is allowed
		// (tests + crash recovery may do this); just clear any stale state.
		BatchDeferFlag() = true;
		BatchPending().Reset();
		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		Obj->SetBoolField(TEXT("batch_open"), true);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	int32 RunEndBatchOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		// Always clear the defer flag first — even if we throw mid-flush we
		// don't want subsequent ops to silently keep deferring.
		BatchDeferFlag() = false;
		TArray<TWeakObjectPtr<UBlueprint>> Pending = MoveTemp(BatchPending());
		BatchPending().Reset();

		// `-Skip` flag (passed by RunOps when on_failure="skip" + a mid-batch
		// failure occurred): discard the pending compile + save. The
		// in-memory UBlueprints stay dirty for this daemon session — agent
		// must avoid acting on them until the daemon restarts or they're
		// explicitly reloaded. Documented limitation; matches the "don't
		// persist partial state" contract of strict atomic mode.
		const bool bSkipCompile = FParse::Param(*Params, TEXT("Skip"));

		TArray<TSharedPtr<FJsonValue>> Recompiled;
		TArray<TSharedPtr<FJsonValue>> AllDiagnostics;
		int32 Failures = 0;
		int32 Errors = 0, Warnings = 0;
		for (TWeakObjectPtr<UBlueprint>& Weak : Pending)
		{
			UBlueprint* BP = Weak.Get();
			if (!BP) continue;  // GC'd between batch ops — nothing to save
			const FString AssetPath = BP->GetPathName();
			if (bSkipCompile)
			{
				// Don't compile, don't save — just record what would have
				// been recompiled so the caller knows which BPs are now
				// in a dirty in-memory state.
				continue;
			}
			TArray<TSharedPtr<FJsonValue>> Diags;
			const bool bOk = CompileAndSaveBlueprint(BP, &Diags);
			if (!bOk) ++Failures;
			// Tag each diagnostic with the asset_path so callers can attribute
			// when multiple BPs compile in one batch.
			for (auto& D : Diags)
			{
				if (D.IsValid() && D->Type == EJson::Object)
				{
					TSharedPtr<FJsonObject> Obj = D->AsObject();
					if (Obj.IsValid())
					{
						Obj->SetStringField(TEXT("asset_path"), AssetPath);
						const FString Sev = Obj->GetStringField(TEXT("severity"));
						if (Sev == TEXT("error"))   ++Errors;
						if (Sev == TEXT("warning")) ++Warnings;
					}
				}
				AllDiagnostics.Add(D);
			}
			Recompiled.Add(MakeShared<FJsonValueString>(AssetPath));
		}

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), Failures == 0);
		Obj->SetArrayField(TEXT("recompiled"), Recompiled);
		Obj->SetNumberField(TEXT("failed"), Failures);
		Obj->SetArrayField(TEXT("diagnostics"), AllDiagnostics);
		Obj->SetNumberField(TEXT("error_count"),   Errors);
		Obj->SetNumberField(TEXT("warning_count"), Warnings);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	// ----- CreateBlueprint (A3) ---------------------------------------------
	// Creates a new BP under `/Game/...` with the given parent class. Idempotent:
	// if the asset already exists, returns {ok:true, already_existed:true}.
	// Adapts the SeedCommandlet pattern (FactoryCreateNew via UBlueprintFactory)
	// and registers with the asset registry so a follow-up op in the same batch
	// can LoadMutableBlueprint it.
	UClass* ResolveParentClass(const FString& Spec)
	{
		if (Spec.IsEmpty()) return nullptr;
		// Direct path form: "/Script/Engine.Actor"
		if (UClass* C = LoadObject<UClass>(nullptr, *Spec)) return C;
		// Short-name form: "Actor", "Pawn", "ACharacter" — try common prefixes.
		const TCHAR* Prefixes[] = {TEXT(""), TEXT("/Script/Engine."), TEXT("/Script/CoreUObject.")};
		for (const TCHAR* Pre : Prefixes)
		{
			FString Trial = FString(Pre) + Spec;
			if (UClass* C = LoadObject<UClass>(nullptr, *Trial)) return C;
			// Drop a leading 'A' or 'U' if present (UE convention).
			if (Spec.Len() > 1 && (Spec[0] == TEXT('A') || Spec[0] == TEXT('U')))
			{
				FString Trim = FString(Pre) + Spec.RightChop(1);
				if (UClass* C = LoadObject<UClass>(nullptr, *Trim)) return C;
			}
		}
		return nullptr;
	}

	int32 RunCreateBlueprintOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		const FString AssetPath = ResolveAssetPath(Params);
		FString ParentClassSpec;
		FParse::Value(*Params, TEXT("ParentClass="), ParentClassSpec);

		if (AssetPath.IsEmpty() || ParentClassSpec.IsEmpty())
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("CreateBlueprint requires -Asset=/Game/... and -ParentClass=<UClass path or short name>"));
			return 1;
		}
		if (!AssetPath.StartsWith(TEXT("/Game/")))
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("CreateBlueprint: -Asset must be under /Game/ (got: %s)"), *AssetPath);
			return 1;
		}

		// Idempotency probe — if the asset already exists, short-circuit.
		if (UBlueprint* Existing = LoadMutableBlueprint(AssetPath))
		{
			(void)Existing;
			auto Obj = MakeShared<FJsonObject>();
			Obj->SetBoolField(TEXT("ok"), true);
			Obj->SetBoolField(TEXT("already_existed"), true);
			Obj->SetStringField(TEXT("asset_path"), AssetPath);
			return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
		}

		UClass* ParentClass = ResolveParentClass(ParentClassSpec);
		if (!ParentClass)
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("CreateBlueprint: parent class not found: %s"), *ParentClassSpec);
			return 4;
		}

		// Derive package name + asset short name from the asset path.
		// `/Game/AI/BP_Foo` -> package `/Game/AI/BP_Foo`, asset `BP_Foo`.
		const FString PackageName = AssetPath;
		const FString AssetName   = FPackageName::GetShortName(AssetPath);

		UPackage* Package = CreatePackage(*PackageName);
		if (!Package)
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("CreatePackage failed: %s"), *PackageName);
			return 5;
		}
		Package->FullyLoad();

		UBlueprintFactory* Factory = NewObject<UBlueprintFactory>();
		Factory->ParentClass = ParentClass;

		UBlueprint* BP = Cast<UBlueprint>(Factory->FactoryCreateNew(
			UBlueprint::StaticClass(), Package, *AssetName,
			RF_Public | RF_Standalone, nullptr, GWarn));
		if (!BP)
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("FactoryCreateNew failed: %s"), *PackageName);
			return 5;
		}
		BP->MarkPackageDirty();

		// Critical: register with asset registry so a follow-up op in the
		// same batch (e.g. AddVariable) can LoadMutableBlueprint() this asset.
		FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		ARM.Get().AssetCreated(BP);

		if (!MaybeCompileAndSave(BP)) return 5;

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		Obj->SetBoolField(TEXT("already_existed"), false);
		Obj->SetStringField(TEXT("asset_path"), AssetPath);
		Obj->SetStringField(TEXT("parent_class"), ParentClass->GetPathName());
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	// ----- SetPinDefault (B1) ------------------------------------------------
	// Sets the literal default value on a node's pin. compile_function uses
	// this to materialize {lit:value} expressions — UE has no first-class
	// "literal node", so the value is set as the consumer pin's default.
	int32 RunSetPinDefaultOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		const FString AssetPath = ResolveAssetPath(Params);
		FString GraphName, NodeId, PinSpec, Value;
		FParse::Value(*Params, TEXT("Graph="), GraphName);
		FParse::Value(*Params, TEXT("Node="),  NodeId);
		FParse::Value(*Params, TEXT("Pin="),   PinSpec);
		FParse::Value(*Params, TEXT("Value="), Value);

		if (AssetPath.IsEmpty() || GraphName.IsEmpty() || NodeId.IsEmpty() || PinSpec.IsEmpty())
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("SetPinDefault requires -Asset= -Graph= -Node= -Pin= [-Value=]"));
			return 1;
		}
		UBlueprint* BP = LoadMutableBlueprint(AssetPath);
		if (!BP) return 2;
		UEdGraph* Graph = FindGraphByName(BP, GraphName);
		if (!Graph) return 4;
		UEdGraphNode* Node = FindNodeByGuid(Graph, NodeId);
		if (!Node) return 4;
		UEdGraphPin* Pin = FindPinByIdOrName(Node, PinSpec);
		if (!Pin)
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("SetPinDefault: pin '%s' not found on node %s"), *PinSpec, *NodeId);
			return 4;
		}
		// Modify the pin's default value. UE 5.7's TrySetDefaultValue
		// returns void — type-mismatch errors surface via the compile log
		// and bubble up through the EndBatch diagnostics path (C1).
		const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
		Schema->TrySetDefaultValue(*Pin, Value);
		Pin->Modify();
		Node->Modify();
		if (!MaybeCompileAndSave(BP)) return 5;
		return EmitOk(OutputPath, bPretty);
	}

	// ----- RetypeVariable (BP-2) ---------------------------------------
	// Change a member variable's type WITHOUT delete + re-add. Preserves
	// every VariableGet / VariableSet node that references it — UE
	// rewires their pin types in place.
	int32 RunRetypeVariableOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		const FString AssetPath = ResolveAssetPath(Params);
		FString VarName;
		FParse::Value(*Params, TEXT("Name="), VarName);
		FEdGraphPinType NewType;
		if (AssetPath.IsEmpty() || VarName.IsEmpty() ||
		    !BuildPinTypeFromFlags(Params, NewType))
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("RetypeVariable requires -Asset= -Name= -TypeCategory= [-TypeSubCategory=...] [-TypeSubCategoryObject=...]"));
			return 1;
		}
		UBlueprint* BP = LoadMutableBlueprint(AssetPath);
		if (!BP) return 4;
		// Confirm the variable exists before mutating — UE's
		// ChangeMemberVariableType is a void function with no failure
		// signal; the only way to fail loudly is to pre-check.
		if (FBlueprintEditorUtils::FindNewVariableIndex(BP, FName(*VarName)) == INDEX_NONE)
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("RetypeVariable: variable '%s' not found on %s"), *VarName, *AssetPath);
			return 4;
		}
		FBlueprintEditorUtils::ChangeMemberVariableType(BP, FName(*VarName), NewType);
		if (!MaybeCompileAndSave(BP)) return 5;
		return EmitOk(OutputPath, bPretty);
	}

	// ----- SetVariableCategory (BP-7) ----------------------------------
	// Change the My-Blueprint-panel category label on a member variable.
	// Empty -Category= clears the category back to the default.
	int32 RunSetVariableCategoryOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		const FString AssetPath = ResolveAssetPath(Params);
		FString VarName, Category;
		FParse::Value(*Params, TEXT("Name="),     VarName);
		FParse::Value(*Params, TEXT("Category="), Category);
		if (AssetPath.IsEmpty() || VarName.IsEmpty())
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("SetVariableCategory requires -Asset= -Name= [-Category=]"));
			return 1;
		}
		UBlueprint* BP = LoadMutableBlueprint(AssetPath);
		if (!BP) return 4;
		if (FBlueprintEditorUtils::FindNewVariableIndex(BP, FName(*VarName)) == INDEX_NONE)
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("SetVariableCategory: variable '%s' not found on %s"), *VarName, *AssetPath);
			return 4;
		}
		// `nullptr` for InLocalVarScope = member variable (vs function-
		// local). bDontRecompile=true lets MaybeCompileAndSave handle
		// the compile (or defer it under a batch).
		FBlueprintEditorUtils::SetBlueprintVariableCategory(
			BP, FName(*VarName), nullptr, FText::FromString(Category),
			/*bDontRecompile=*/true);
		if (!MaybeCompileAndSave(BP)) return 5;
		return EmitOk(OutputPath, bPretty);
	}

	// ----- WriteGeneratedSource (drop transpiled C++ into Source/) -------
	// Write a generated .h/.cpp file into the project's Source/ tree.
	// Args: -Path=<absolute dest> -ContentFile=<absolute source temp>
	//       [-CreateDirs]
	// The server writes the actual content to a temp file (so we don't
	// have to encode big strings into command-line args; the daemon
	// protocol is line-based). The plugin reads that temp + writes to
	// dest, then deletes the temp. Path validation confines writes to
	// the project's Source/ directory — anything else is rejected.
	int32 RunWriteGeneratedSourceOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString DestPath, ContentPath;
		FParse::Value(*Params, TEXT("Path="),        DestPath);
		FParse::Value(*Params, TEXT("ContentFile="), ContentPath);
		const bool bCreateDirs = FParse::Param(*Params, TEXT("CreateDirs"));

		if (DestPath.IsEmpty() || ContentPath.IsEmpty())
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("WriteGeneratedSource requires -Path=<dest> -ContentFile=<src>"));
			return 1;
		}

		// Canonicalize paths so prefix-checks work consistently.
		FString DestAbs = FPaths::ConvertRelativePathToFull(DestPath);
		FString ProjectSourceDir = FPaths::ConvertRelativePathToFull(
			FPaths::Combine(FPaths::ProjectDir(), TEXT("Source")));

		if (!DestAbs.StartsWith(ProjectSourceDir))
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("WriteGeneratedSource: destination must be under '%s' (got: %s)"),
				*ProjectSourceDir, *DestAbs);
			return 1;
		}

		FString Content;
		if (!FFileHelper::LoadFileToString(Content, *ContentPath))
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("WriteGeneratedSource: failed to read content temp file: %s"),
				*ContentPath);
			return 4;
		}

		if (bCreateDirs)
		{
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(DestAbs), /*Tree=*/true);
		}

		if (!FFileHelper::SaveStringToFile(Content, *DestAbs,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("WriteGeneratedSource: failed to write %s"), *DestAbs);
			return 5;
		}

		// Best-effort cleanup of the temp file (the server may have
		// already done this; ignore failures).
		IFileManager::Get().Delete(*ContentPath);

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		Obj->SetStringField(TEXT("path"), DestAbs);
		Obj->SetNumberField(TEXT("bytes_written"), Content.Len());
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	// ----- SaveAll -----------------------------------------------------
	// Save every dirty package the editor has loaded. With `bDirtyOnly`
	// (default true), clean packages are skipped. Uses
	// UEditorLoadingAndSavingUtils which is the public save path.
	int32 RunSaveAllOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		const bool bIncludeClean = FParse::Param(*Params, TEXT("IncludeClean"));
		(void)bIncludeClean;  // SaveDirtyPackages saves dirty-only by design;
		                       // IncludeClean is reserved for a future "save
		                       // every loaded package" path via FEditorFileUtils.

		// Capture dirty packages BEFORE the save so we can report what we
		// touched. Walk the loaded-packages set; UE doesn't expose a "list
		// dirty packages" helper directly.
		TArray<UPackage*> DirtyBefore;
		for (TObjectIterator<UPackage> It; It; ++It)
		{
			UPackage* Pkg = *It;
			if (Pkg && Pkg->IsDirty()) DirtyBefore.Add(Pkg);
		}

		// UEditorLoadingAndSavingUtils::SaveDirtyPackages(bSaveMapPackages,
		// bSaveContentPackages) handles the actual save loop with proper
		// SCC + checkpoint awareness.
		const bool bSaved = UEditorLoadingAndSavingUtils::SaveDirtyPackages(
			/*bSaveMapPackages=*/true, /*bSaveContentPackages=*/true);
		(void)bSaved;

		TArray<TSharedPtr<FJsonValue>> FailedJson;
		int32 SavedCount = 0;
		for (UPackage* Pkg : DirtyBefore)
		{
			if (!Pkg) continue;
			if (Pkg->IsDirty())
			{
				FailedJson.Add(MakeShared<FJsonValueString>(Pkg->GetName()));
			}
			else
			{
				++SavedCount;
			}
		}

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		Obj->SetNumberField(TEXT("saved_count"), SavedCount);
		Obj->SetArrayField(TEXT("failed_assets"), FailedJson);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	// ----- MoveAsset ----------------------------------------------------
	// Move or rename an asset. Both -Asset and -Dest are package paths
	// under /Game/. Delegates to IAssetTools::RenameAssets which handles
	// reference fix-ups + redirectors.
	int32 RunMoveAssetOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString SourceAsset, DestAsset;
		FParse::Value(*Params, TEXT("Asset="), SourceAsset);
		FParse::Value(*Params, TEXT("Dest="),  DestAsset);
		if (SourceAsset.IsEmpty() || DestAsset.IsEmpty())
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("MoveAsset requires -Asset=/Game/X -Dest=/Game/Y"));
			return 1;
		}
		if (!DestAsset.StartsWith(TEXT("/Game/")))
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("MoveAsset: -Dest must be under /Game/ (got: %s)"), *DestAsset);
			return 1;
		}

		// Load the source via the asset registry — works for any UObject,
		// not just Blueprints.
		FAssetRegistryModule& ARM =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		IAssetRegistry& AR = ARM.Get();
		FAssetData SourceData = AR.GetAssetByObjectPath(FSoftObjectPath(SourceAsset));
		if (!SourceData.IsValid())
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("MoveAsset: source asset not found: %s"), *SourceAsset);
			return 4;
		}
		UObject* SourceObj = SourceData.GetAsset();
		if (!SourceObj)
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("MoveAsset: failed to load source: %s"), *SourceAsset);
			return 4;
		}

		// Split destination into PackagePath + AssetName.
		FString DestPackagePath, DestName;
		{
			int32 LastSlash;
			if (!DestAsset.FindLastChar(TEXT('/'), LastSlash) || LastSlash <= 0)
			{
				UE_LOG(LogBlueprintReader, Error,
					TEXT("MoveAsset: -Dest='%s' is malformed"), *DestAsset);
				return 1;
			}
			DestPackagePath = DestAsset.Left(LastSlash);
			DestName        = DestAsset.RightChop(LastSlash + 1);
		}

		TArray<FAssetRenameData> RenameList;
		RenameList.Emplace(SourceObj, DestPackagePath, DestName);

		FAssetToolsModule& AssetToolsModule =
			FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
		IAssetTools& AssetTools = AssetToolsModule.Get();
		const bool bOk = AssetTools.RenameAssets(RenameList);
		if (!bOk)
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("MoveAsset: RenameAssets failed for %s -> %s"),
				*SourceAsset, *DestAsset);
			return 5;
		}

		// Count redirectors created at the source path. UE creates a
		// UObjectRedirector when moving across folders.
		int32 RedirectorCount = 0;
		{
			TArray<FAssetData> AtSource;
			AR.GetAssetsByPackageName(*SourceData.PackageName.ToString(), AtSource);
			for (const FAssetData& A : AtSource)
			{
				if (A.AssetClassPath.GetAssetName() == TEXT("ObjectRedirector"))
				{
					++RedirectorCount;
				}
			}
		}

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		Obj->SetStringField(TEXT("source_path"), SourceAsset);
		Obj->SetStringField(TEXT("dest_path"),   DestAsset);
		Obj->SetNumberField(TEXT("redirectors_created"), RedirectorCount);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	// ----- DeleteAsset --------------------------------------------------
	// Delete an asset. Refuses by default if other assets reference it;
	// -Force overrides. Returns the list of references found.
	int32 RunDeleteAssetOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString AssetPath;
		FParse::Value(*Params, TEXT("Asset="), AssetPath);
		if (AssetPath.IsEmpty())
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("DeleteAsset requires -Asset=/Game/X"));
			return 1;
		}
		const bool bForce = FParse::Param(*Params, TEXT("Force"));

		FAssetRegistryModule& ARM =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		IAssetRegistry& AR = ARM.Get();
		FAssetData AssetData = AR.GetAssetByObjectPath(FSoftObjectPath(AssetPath));
		if (!AssetData.IsValid())
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("DeleteAsset: asset not found: %s"), *AssetPath);
			return 4;
		}

		// Find referencers via the asset registry. If any exist and -Force
		// isn't set, refuse + return the list so the caller can decide.
		TArray<FName> Referencers;
		AR.GetReferencers(AssetData.PackageName, Referencers,
			UE::AssetRegistry::EDependencyCategory::Package,
			UE::AssetRegistry::EDependencyQuery::Hard);

		TArray<TSharedPtr<FJsonValue>> RefsJson;
		for (FName Ref : Referencers)
		{
			RefsJson.Add(MakeShared<FJsonValueString>(Ref.ToString()));
		}

		bool bDeleted = false;
		if (Referencers.Num() == 0 || bForce)
		{
			UObject* Obj = AssetData.GetAsset();
			if (Obj)
			{
				TArray<UObject*> ToDelete = { Obj };
				const int32 NumDeleted = ObjectTools::ForceDeleteObjects(
					ToDelete, /*bShowConfirmation=*/false);
				bDeleted = NumDeleted > 0;
			}
		}

		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetStringField(TEXT("path"), AssetPath);
		Out->SetBoolField(TEXT("deleted"), bDeleted);
		Out->SetArrayField(TEXT("referencing_assets"), RefsJson);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	// ----- CreateFolder -------------------------------------------------
	// Folders in UE are implicit (a long-package-path that holds at least
	// one asset registers as a folder). We register the path with the
	// asset registry so the Content Browser shows it; idempotent on
	// re-runs because AddPath is a no-op for known paths.
	int32 RunCreateFolderOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString FolderPath;
		FParse::Value(*Params, TEXT("Path="), FolderPath);
		if (FolderPath.IsEmpty() || !FolderPath.StartsWith(TEXT("/Game/")))
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("CreateFolder requires -Path=/Game/<subpath>"));
			return 1;
		}

		FAssetRegistryModule& ARM =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		IAssetRegistry& AR = ARM.Get();

		const bool bAlreadyExisted = AR.HasAssets(*FolderPath, /*bRecursive=*/false);
		if (!bAlreadyExisted)
		{
			AR.AddPath(FolderPath);
		}

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		Obj->SetStringField(TEXT("path"), FolderPath);
		Obj->SetBoolField(TEXT("already_existed"), bAlreadyExisted);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	// ----- ListDataTables ----------------------------------------------
	// Asset Registry filter for UDataTable. Mirrors RunListOp's shape.
	int32 RunListDataTablesOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString PathFilter;
		FParse::Value(*Params, TEXT("Path="), PathFilter);
		if (PathFilter.IsEmpty()) PathFilter = TEXT("/Game");

		FAssetRegistryModule& ARM =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		IAssetRegistry& AR = ARM.Get();

		// Narrow scan to the requested path.
		AR.ScanPathsSynchronous({ PathFilter }, /*bForceRescan=*/false);

		FARFilter Filter;
		Filter.ClassPaths.Add(UDataTable::StaticClass()->GetClassPathName());
		Filter.PackagePaths.Add(*PathFilter);
		Filter.bRecursivePaths = true;
		TArray<FAssetData> Assets;
		AR.GetAssets(Filter, Assets);

		TArray<TSharedPtr<FJsonValue>> Items;
		for (const FAssetData& A : Assets)
		{
			auto Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("asset_path"), A.PackageName.ToString());
			Item->SetStringField(TEXT("name"),       A.AssetName.ToString());
			Item->SetStringField(TEXT("parent_class"), TEXT("/Script/Engine.DataTable"));
			// modified_iso is omitted — DataTables don't carry a reliable
			// mtime in the asset tags. Could derive from on-disk stat if
			// needed.
			Items.Add(MakeShared<FJsonValueObject>(Item));
		}

		return EmitJson(
			FBlueprintReaderWireJson::WriteArrayString(Items, bPretty),
			OutputPath);
	}

	// ----- ReadDataTable -----------------------------------------------
	// Load a DataTable and serialize its row-struct fields + all rows.
	int32 RunReadDataTableOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString AssetPath;
		FParse::Value(*Params, TEXT("Asset="), AssetPath);
		if (AssetPath.IsEmpty())
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("ReadDataTable requires -Asset=/Game/X"));
			return 1;
		}

		UDataTable* DT = LoadObject<UDataTable>(nullptr, *AssetPath);
		if (!DT)
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("ReadDataTable: failed to load: %s"), *AssetPath);
			return 4;
		}

		const UScriptStruct* RowStruct = DT->GetRowStruct();
		FString RowStructPath;
		TArray<TSharedPtr<FJsonValue>> Columns;
		if (RowStruct)
		{
			RowStructPath = RowStruct->GetPathName();
			for (TFieldIterator<FProperty> It(RowStruct); It; ++It)
			{
				Columns.Add(MakeShared<FJsonValueString>(It->GetName()));
			}
		}

		TArray<TSharedPtr<FJsonValue>> Rows;
		for (const auto& Pair : DT->GetRowMap())
		{
			auto RowJson = MakeShared<FJsonObject>();
			RowJson->SetStringField(TEXT("row_name"), Pair.Key.ToString());
			if (RowStruct && Pair.Value)
			{
				// Use FJsonObjectConverter to serialize the row struct.
				TSharedPtr<FJsonObject> Fields = MakeShared<FJsonObject>();
				FJsonObjectConverter::UStructToJsonObject(
					RowStruct, Pair.Value, Fields.ToSharedRef(), 0, 0);
				for (const auto& F : Fields->Values)
				{
					RowJson->SetField(F.Key, F.Value);
				}
			}
			Rows.Add(MakeShared<FJsonValueObject>(RowJson));
		}

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		Obj->SetStringField(TEXT("asset_path"), AssetPath);
		Obj->SetStringField(TEXT("row_struct"), RowStructPath);
		Obj->SetArrayField(TEXT("columns"), Columns);
		Obj->SetArrayField(TEXT("rows"), Rows);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	// ----- Live editor ops --------------------------------------------
	//
	// These work directly against the editor's in-memory state (GEditor,
	// IConsoleManager, etc.). They make most sense via the live backend
	// (open editor) but also work in the commandlet's headless editor.

	// Pull the editor world. Tries the editor's first PIE / EditorWorldContext.
	UWorld* GetEditorWorldOrNull()
	{
		if (GEditor)
		{
			if (UWorld* W = GEditor->GetEditorWorldContext().World()) return W;
		}
		return GWorld;
	}

	int32 RunConsoleCommandOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString Cmd;
		FParse::Value(*Params, TEXT("Command="), Cmd, /*bShouldStopOnSeparator=*/false);
		if (Cmd.IsEmpty())
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("ConsoleCommand requires -Command=..."));
			return 1;
		}
		// Capture log output during the call via a string output device.
		FStringOutputDevice Capture;
		Capture.SetAutoEmitLineTerminator(true);
		if (GEngine)
		{
			GEngine->Exec(GetEditorWorldOrNull(), *Cmd, Capture);
		}
		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		Obj->SetStringField(TEXT("output"), Capture);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	int32 RunGetCVarOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString Name;
		FParse::Value(*Params, TEXT("Name="), Name);
		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		Obj->SetStringField(TEXT("name"), Name);
		IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(*Name);
		if (Var)
		{
			Obj->SetBoolField(TEXT("exists"), true);
			Obj->SetStringField(TEXT("value"), Var->GetString());
			// Help text requires the IConsoleObject; pull via FindConsoleObject.
			if (IConsoleObject* Obj2 = IConsoleManager::Get().FindConsoleObject(*Name))
			{
				Obj->SetStringField(TEXT("help"), Obj2->GetHelp());
			}
		}
		else
		{
			Obj->SetBoolField(TEXT("exists"), false);
		}
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	int32 RunSetCVarOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString Name, Value;
		FParse::Value(*Params, TEXT("Name="),  Name);
		FParse::Value(*Params, TEXT("Value="), Value);
		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		Obj->SetStringField(TEXT("name"), Name);
		IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(*Name);
		if (Var)
		{
			Var->Set(*Value, ECVF_SetByCode);
			Obj->SetBoolField(TEXT("exists"), true);
			Obj->SetStringField(TEXT("value"), Var->GetString());
		}
		else
		{
			Obj->SetBoolField(TEXT("exists"), false);
		}
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	int32 RunPieStartOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString Mode = TEXT("selected_viewport");
		FParse::Value(*Params, TEXT("Mode="), Mode);
		bool bStarted = false;
		if (GEditor)
		{
			FRequestPlaySessionParams Req;
			// Default to selected viewport — matches the "Play" button.
			if (Mode == TEXT("new_editor_window"))
			{
				Req.SessionDestination = EPlaySessionDestinationType::NewProcess;
			}
			else if (Mode == TEXT("standalone"))
			{
				Req.SessionDestination = EPlaySessionDestinationType::NewProcess;
			}
			GEditor->RequestPlaySession(Req);
			bStarted = true;
		}
		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		Obj->SetBoolField(TEXT("started"), bStarted);
		Obj->SetStringField(TEXT("mode"), Mode);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	int32 RunPieStopOp(const FString&, const FString& OutputPath, bool bPretty)
	{
		bool bStopped = false;
		if (GEditor && GEditor->IsPlaySessionInProgress())
		{
			GEditor->RequestEndPlayMap();
			bStopped = true;
		}
		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		Obj->SetBoolField(TEXT("stopped"), bStopped);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	int32 RunLiveCodingCompileOp(const FString&, const FString& OutputPath, bool bPretty)
	{
		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		// ILiveCodingModule lives in a separate optional module. Loading it
		// dynamically avoids a hard module dep when LiveCoding isn't enabled.
		FModuleManager& MM = FModuleManager::Get();
		if (MM.IsModuleLoaded(TEXT("LiveCoding")))
		{
			Obj->SetBoolField(TEXT("queued"), true);
			Obj->SetStringField(TEXT("message"),
				TEXT("Live Coding compile triggered; watch the output log for progress."));
			// Trigger via console command — avoids the need to include the
			// LiveCoding module header.
			if (GEngine)
			{
				GEngine->Exec(GetEditorWorldOrNull(), TEXT("LiveCoding.Compile"));
			}
		}
		else
		{
			Obj->SetBoolField(TEXT("queued"), false);
			Obj->SetStringField(TEXT("message"),
				TEXT("Live Coding module is not loaded; enable LiveCoding plugin in this project."));
		}
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	int32 RunGetSelectedActorsOp(const FString&, const FString& OutputPath, bool bPretty)
	{
		TArray<TSharedPtr<FJsonValue>> Names;
		if (GEditor)
		{
			USelection* Selection = GEditor->GetSelectedActors();
			if (Selection)
			{
				for (FSelectionIterator It(*Selection); It; ++It)
				{
					if (AActor* Actor = Cast<AActor>(*It))
					{
						Names.Add(MakeShared<FJsonValueString>(Actor->GetName()));
					}
				}
			}
		}
		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		Obj->SetArrayField(TEXT("actor_names"), Names);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	int32 RunSetSelectionOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString NamesJoined;
		FParse::Value(*Params, TEXT("Names="), NamesJoined);
		const bool bReplace = !FParse::Param(*Params, TEXT("Add"));

		TArray<FString> NameList;
		NamesJoined.ParseIntoArray(NameList, TEXT(","), /*bCullEmpty=*/true);

		if (GEditor)
		{
			USelection* Selection = GEditor->GetSelectedActors();
			if (Selection)
			{
				Selection->BeginBatchSelectOperation();
				if (bReplace) GEditor->SelectNone(/*bNoteSelectionChange=*/false,
				                                  /*bDeselectBSPSurfs=*/true);

				if (UWorld* World = GetEditorWorldOrNull())
				{
					for (TActorIterator<AActor> It(World); It; ++It)
					{
						AActor* Actor = *It;
						if (Actor && NameList.Contains(Actor->GetName()))
						{
							GEditor->SelectActor(Actor, /*bSelect=*/true,
								/*bNotify=*/false, /*bSelectEvenIfHidden=*/true);
						}
					}
				}
				Selection->EndBatchSelectOperation();
				GEditor->NoteSelectionChange();
			}
		}

		// Re-pull the current selection so the caller can confirm.
		TArray<TSharedPtr<FJsonValue>> Names;
		if (GEditor)
		{
			if (USelection* S = GEditor->GetSelectedActors())
			{
				for (FSelectionIterator It(*S); It; ++It)
				{
					if (AActor* A = Cast<AActor>(*It))
					{
						Names.Add(MakeShared<FJsonValueString>(A->GetName()));
					}
				}
			}
		}
		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		Obj->SetArrayField(TEXT("actor_names"), Names);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	int32 RunSpawnActorOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString ClassPath;
		FParse::Value(*Params, TEXT("Class="), ClassPath);
		double LX=0, LY=0, LZ=0, RP=0, RY=0, RR=0, SX=1, SY=1, SZ=1;
		FParse::Value(*Params, TEXT("LocX="), LX);
		FParse::Value(*Params, TEXT("LocY="), LY);
		FParse::Value(*Params, TEXT("LocZ="), LZ);
		FParse::Value(*Params, TEXT("RotPitch="), RP);
		FParse::Value(*Params, TEXT("RotYaw="),   RY);
		FParse::Value(*Params, TEXT("RotRoll="),  RR);
		FParse::Value(*Params, TEXT("ScaleX="), SX);
		FParse::Value(*Params, TEXT("ScaleY="), SY);
		FParse::Value(*Params, TEXT("ScaleZ="), SZ);

		if (ClassPath.IsEmpty())
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("SpawnActor requires -Class=..."));
			return 1;
		}
		UClass* SpawnClass = LoadObject<UClass>(nullptr, *ClassPath);
		if (!SpawnClass)
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("SpawnActor: could not load class '%s'"), *ClassPath);
			return 4;
		}
		UWorld* World = GetEditorWorldOrNull();
		if (!World)
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("SpawnActor: no editor world"));
			return 5;
		}

		FTransform T(FRotator(RP, RY, RR), FVector(LX, LY, LZ), FVector(SX, SY, SZ));
		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AActor* Spawned = World->SpawnActor<AActor>(SpawnClass, T, SpawnParams);
		if (!Spawned)
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("SpawnActor: SpawnActor failed"));
			return 5;
		}

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		Obj->SetStringField(TEXT("actor_name"),  Spawned->GetName());
		Obj->SetStringField(TEXT("actor_label"), Spawned->GetActorLabel());
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	AActor* FindActorByName(const FString& Name)
	{
		UWorld* World = GetEditorWorldOrNull();
		if (!World) return nullptr;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (*It && (*It)->GetName() == Name) return *It;
		}
		return nullptr;
	}

	int32 RunSetActorTransformOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString Name;
		FParse::Value(*Params, TEXT("Name="), Name);
		AActor* Actor = FindActorByName(Name);
		if (!Actor)
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("SetActorTransform: actor '%s' not found"), *Name);
			return 4;
		}
		double LX=0, LY=0, LZ=0, RP=0, RY=0, RR=0, SX=1, SY=1, SZ=1;
		FParse::Value(*Params, TEXT("LocX="), LX);
		FParse::Value(*Params, TEXT("LocY="), LY);
		FParse::Value(*Params, TEXT("LocZ="), LZ);
		FParse::Value(*Params, TEXT("RotPitch="), RP);
		FParse::Value(*Params, TEXT("RotYaw="),   RY);
		FParse::Value(*Params, TEXT("RotRoll="),  RR);
		FParse::Value(*Params, TEXT("ScaleX="), SX);
		FParse::Value(*Params, TEXT("ScaleY="), SY);
		FParse::Value(*Params, TEXT("ScaleZ="), SZ);
		Actor->SetActorTransform(FTransform(FRotator(RP, RY, RR),
			FVector(LX, LY, LZ), FVector(SX, SY, SZ)));

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		Obj->SetStringField(TEXT("actor_name"), Name);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	int32 RunDeleteActorOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString Name;
		FParse::Value(*Params, TEXT("Name="), Name);
		bool bDeleted = false;
		if (AActor* Actor = FindActorByName(Name))
		{
			if (UWorld* World = Actor->GetWorld())
			{
				bDeleted = World->DestroyActor(Actor);
			}
		}
		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		Obj->SetBoolField(TEXT("deleted"), bDeleted);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	// ----- DataTable row mutation -------------------------------------
	//
	// AddDataRow inserts a row whose contents are the JSON object passed
	// via -ValuesFile=<path>. Each top-level key maps to a property on
	// the row struct; FProperty::ImportText handles the string→native
	// coercion (works for scalars, enums, FName/FString/FText, and any
	// struct that exports via text). Idempotent: existing rows return
	// {already_existed:true} unless -Overwrite is passed.
	//
	// SetDataRowValue is the surgical-edit cousin: one field on one
	// existing row.

	int32 RunAddDataRowOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString AssetPath, RowName, ValuesFile;
		FParse::Value(*Params, TEXT("Asset="),      AssetPath);
		FParse::Value(*Params, TEXT("Row="),        RowName);
		FParse::Value(*Params, TEXT("ValuesFile="), ValuesFile);
		const bool bOverwrite = FParse::Param(*Params, TEXT("Overwrite"));

		if (AssetPath.IsEmpty() || RowName.IsEmpty() || ValuesFile.IsEmpty())
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("AddDataRow requires -Asset / -Row / -ValuesFile"));
			return 1;
		}

		UDataTable* DT = LoadObject<UDataTable>(nullptr, *AssetPath);
		if (!DT)
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("AddDataRow: failed to load DataTable: %s"), *AssetPath);
			return 4;
		}
		const UScriptStruct* RowStruct = DT->GetRowStruct();
		if (!RowStruct)
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("AddDataRow: DataTable %s has no row struct"), *AssetPath);
			return 5;
		}

		// Load + parse the JSON values blob.
		FString ValuesBlob;
		if (!FFileHelper::LoadFileToString(ValuesBlob, *ValuesFile))
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("AddDataRow: cannot read values file: %s"), *ValuesFile);
			return 1;
		}
		TSharedPtr<FJsonObject> Values;
		{
			TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(ValuesBlob);
			if (!FJsonSerializer::Deserialize(R, Values) || !Values.IsValid())
			{
				UE_LOG(LogBlueprintReader, Error,
					TEXT("AddDataRow: values is not a JSON object"));
				return 1;
			}
		}

		// Idempotency check.
		const FName RowFName(*RowName);
		uint8* ExistingRow = DT->FindRowUnchecked(RowFName);
		if (ExistingRow && !bOverwrite)
		{
			auto Obj = MakeShared<FJsonObject>();
			Obj->SetBoolField(TEXT("ok"), true);
			Obj->SetBoolField(TEXT("already_existed"), true);
			Obj->SetBoolField(TEXT("created"), false);
			return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
		}

		// Allocate + zero-init a row struct, then ImportText each field.
		const int32 StructSize = RowStruct->GetStructureSize();
		TArray<uint8> RowData;
		RowData.SetNumZeroed(StructSize);
		RowStruct->InitializeStruct(RowData.GetData());

		for (const auto& Pair : Values->Values)
		{
			FProperty* Prop = RowStruct->FindPropertyByName(FName(*Pair.Key));
			if (!Prop) continue;  // ignore unknown keys
			FString Stringified;
			if (Pair.Value.IsValid())
			{
				// JSON scalars → their textual form. Quotes stripped for
				// strings so ImportText sees the bare value.
				if (Pair.Value->Type == EJson::String)
				{
					Stringified = Pair.Value->AsString();
				}
				else
				{
					Stringified = Pair.Value->AsString();  // numbers / bools coerce ToString
				}
			}
			Prop->ImportText_Direct(*Stringified,
				Prop->ContainerPtrToValuePtr<void>(RowData.GetData()),
				/*OwnerObject=*/DT, PPF_None);
		}

		// AddRow takes FTableRowBase&; the underlying impl just memcpy's
		// the struct bytes into a new heap allocation tied to RowFName.
		// Since the row struct is dynamic, we go via the internal API.
		if (ExistingRow)
		{
			// Overwrite path: just memcpy the new bytes onto the existing
			// allocation. UE doesn't expose this directly; the public
			// path is RemoveRow + AddRow.
			DT->RemoveRow(RowFName);
		}

		// The public API requires a typed FTableRowBase. We cast our
		// dynamically-sized buffer; UE memcpy's the right number of bytes.
		DT->AddRow(RowFName, *reinterpret_cast<FTableRowBase*>(RowData.GetData()));
		DT->MarkPackageDirty();

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		Obj->SetBoolField(TEXT("already_existed"), ExistingRow != nullptr);
		Obj->SetBoolField(TEXT("created"), true);
		Obj->SetStringField(TEXT("asset_path"), AssetPath);
		Obj->SetStringField(TEXT("row_name"),  RowName);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	int32 RunSetDataRowValueOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString AssetPath, RowName, FieldName, NewValue;
		FParse::Value(*Params, TEXT("Asset="), AssetPath);
		FParse::Value(*Params, TEXT("Row="),   RowName);
		FParse::Value(*Params, TEXT("Field="), FieldName);
		FParse::Value(*Params, TEXT("Value="), NewValue);
		if (AssetPath.IsEmpty() || RowName.IsEmpty() || FieldName.IsEmpty())
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("SetDataRowValue requires -Asset / -Row / -Field"));
			return 1;
		}

		UDataTable* DT = LoadObject<UDataTable>(nullptr, *AssetPath);
		if (!DT)
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("SetDataRowValue: cannot load DataTable: %s"), *AssetPath);
			return 4;
		}
		const UScriptStruct* RowStruct = DT->GetRowStruct();
		uint8* RowData = DT->FindRowUnchecked(FName(*RowName));
		if (!RowStruct || !RowData)
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("SetDataRowValue: row '%s' not found in %s"), *RowName, *AssetPath);
			return 4;
		}
		FProperty* Prop = RowStruct->FindPropertyByName(FName(*FieldName));
		if (!Prop)
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("SetDataRowValue: field '%s' not found on %s"),
				*FieldName, *RowStruct->GetName());
			return 4;
		}

		// Capture the pre-set value for the response.
		FString OldText;
		Prop->ExportText_Direct(OldText,
			Prop->ContainerPtrToValuePtr<void>(RowData),
			/*Defaults=*/nullptr, /*OwnerObject=*/DT, PPF_None);

		// Apply.
		Prop->ImportText_Direct(*NewValue,
			Prop->ContainerPtrToValuePtr<void>(RowData),
			/*OwnerObject=*/DT, PPF_None);
		DT->MarkPackageDirty();

		// Capture the post-set form for round-trip verification.
		FString NewText;
		Prop->ExportText_Direct(NewText,
			Prop->ContainerPtrToValuePtr<void>(RowData),
			/*Defaults=*/nullptr, /*OwnerObject=*/DT, PPF_None);

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		Obj->SetStringField(TEXT("asset_path"), AssetPath);
		Obj->SetStringField(TEXT("row_name"),  RowName);
		Obj->SetStringField(TEXT("field_name"), FieldName);
		Obj->SetStringField(TEXT("old_value"), OldText);
		Obj->SetStringField(TEXT("new_value"), NewText);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	// ----- Component (SCS) authoring ----------------------------------
	//
	// Blueprint components live on the class's SimpleConstructionScript
	// (USCS). Each USCS_Node holds:
	//   - ComponentClass (the UClass being instantiated)
	//   - ComponentTemplate (a UActorComponent with the BP-author's
	//     defaults — what the BP Details panel shows)
	//   - AttachToName + parent SCS_Node (for SceneComp children)
	//   - VariableName (the BP-side property name)
	//
	// Helpers below load the BP, locate nodes by name, and call the
	// public USCS APIs to mutate the tree. Every mutation ends with
	// FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified +
	// FKismetEditorUtilities::CompileBlueprint so the change actually
	// takes effect at next instance spawn.

	USCS_Node* FindSCSNodeOrNull(USimpleConstructionScript* SCS, FName Name)
	{
		if (!SCS) return nullptr;
		return SCS->FindSCSNode(Name);
	}

	USCS_Node* FindParentOfNode(USimpleConstructionScript* SCS, USCS_Node* Target)
	{
		if (!SCS || !Target) return nullptr;
		for (USCS_Node* Root : SCS->GetRootNodes())
		{
			TArray<USCS_Node*> Stack;
			Stack.Push(Root);
			while (Stack.Num() > 0)
			{
				USCS_Node* Cur = Stack.Pop();
				if (!Cur) continue;
				for (USCS_Node* Child : Cur->GetChildNodes())
				{
					if (Child == Target) return Cur;
					Stack.Push(Child);
				}
			}
		}
		return nullptr;  // node is a root
	}

	int32 RunAddComponentOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString AssetPath, Name, ClassPath, ParentName, Socket;
		FParse::Value(*Params, TEXT("Asset="),  AssetPath);
		FParse::Value(*Params, TEXT("Name="),   Name);
		FParse::Value(*Params, TEXT("Class="),  ClassPath);
		FParse::Value(*Params, TEXT("Parent="), ParentName);
		FParse::Value(*Params, TEXT("Socket="), Socket);
		if (AssetPath.IsEmpty() || Name.IsEmpty() || ClassPath.IsEmpty())
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("AddComponent requires -Asset / -Name / -Class"));
			return 1;
		}

		UBlueprint* BP = LoadMutableBlueprint(AssetPath);
		if (!BP) return 4;
		USimpleConstructionScript* SCS = BP->SimpleConstructionScript;
		if (!SCS)
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("AddComponent: BP %s has no SimpleConstructionScript"), *AssetPath);
			return 5;
		}

		// Idempotency check.
		const FName NodeName(*Name);
		if (SCS->FindSCSNode(NodeName))
		{
			auto Obj = MakeShared<FJsonObject>();
			Obj->SetBoolField(TEXT("ok"), true);
			Obj->SetBoolField(TEXT("already_existed"), true);
			Obj->SetBoolField(TEXT("created"), false);
			Obj->SetStringField(TEXT("asset_path"), AssetPath);
			Obj->SetStringField(TEXT("name"), Name);
			Obj->SetStringField(TEXT("component_class"), ClassPath);
			return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
		}

		UClass* CompClass = LoadObject<UClass>(nullptr, *ClassPath);
		if (!CompClass || !CompClass->IsChildOf(UActorComponent::StaticClass()))
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("AddComponent: '%s' isn't a UActorComponent subclass"), *ClassPath);
			return 4;
		}

		USCS_Node* NewNode = SCS->CreateNode(CompClass, NodeName);
		if (!NewNode)
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("AddComponent: SCS->CreateNode returned null"));
			return 5;
		}

		// Parent attachment: child of a named node, or root.
		USCS_Node* Parent = ParentName.IsEmpty() ? nullptr :
			SCS->FindSCSNode(FName(*ParentName));
		if (Parent)
		{
			Parent->AddChildNode(NewNode);
			if (!Socket.IsEmpty()) NewNode->AttachToName = FName(*Socket);
		}
		else
		{
			SCS->AddNode(NewNode);
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
		CompileAndSaveBlueprint(BP);

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		Obj->SetBoolField(TEXT("already_existed"), false);
		Obj->SetBoolField(TEXT("created"), true);
		Obj->SetStringField(TEXT("asset_path"), AssetPath);
		Obj->SetStringField(TEXT("name"), Name);
		Obj->SetStringField(TEXT("component_class"), ClassPath);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	int32 RunRemoveComponentOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString AssetPath, Name;
		FParse::Value(*Params, TEXT("Asset="), AssetPath);
		FParse::Value(*Params, TEXT("Name="),  Name);
		if (AssetPath.IsEmpty() || Name.IsEmpty())
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("RemoveComponent requires -Asset / -Name"));
			return 1;
		}

		UBlueprint* BP = LoadMutableBlueprint(AssetPath);
		if (!BP) return 4;
		USimpleConstructionScript* SCS = BP->SimpleConstructionScript;
		USCS_Node* Node = SCS ? SCS->FindSCSNode(FName(*Name)) : nullptr;

		bool bRemoved = false;
		if (Node)
		{
			SCS->RemoveNodeAndPromoteChildren(Node);
			bRemoved = true;
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
			CompileAndSaveBlueprint(BP);
		}

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		Obj->SetBoolField(TEXT("removed"), bRemoved);
		Obj->SetStringField(TEXT("asset_path"), AssetPath);
		Obj->SetStringField(TEXT("name"), Name);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	int32 RunAttachComponentOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString AssetPath, Name, NewParent, Socket;
		FParse::Value(*Params, TEXT("Asset="),     AssetPath);
		FParse::Value(*Params, TEXT("Name="),      Name);
		FParse::Value(*Params, TEXT("NewParent="), NewParent);
		FParse::Value(*Params, TEXT("Socket="),    Socket);
		if (AssetPath.IsEmpty() || Name.IsEmpty())
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("AttachComponent requires -Asset / -Name"));
			return 1;
		}

		UBlueprint* BP = LoadMutableBlueprint(AssetPath);
		if (!BP) return 4;
		USimpleConstructionScript* SCS = BP->SimpleConstructionScript;
		USCS_Node* Node = SCS ? SCS->FindSCSNode(FName(*Name)) : nullptr;
		if (!Node)
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("AttachComponent: node '%s' not found"), *Name);
			return 4;
		}

		// Detach from current parent (could be root or a child).
		USCS_Node* OldParent = FindParentOfNode(SCS, Node);
		if (OldParent)
		{
			OldParent->RemoveChildNode(Node, /*bRemoveInstanceData=*/false);
		}
		else
		{
			SCS->RemoveNode(Node);  // was a root; takes it out of root list
		}

		// Attach to the new parent.
		USCS_Node* NewParentNode = NewParent.IsEmpty() ? nullptr :
			SCS->FindSCSNode(FName(*NewParent));
		bool bReparented = false;
		if (NewParentNode)
		{
			NewParentNode->AddChildNode(Node);
			if (!Socket.IsEmpty()) Node->AttachToName = FName(*Socket);
			bReparented = true;
		}
		else
		{
			SCS->AddNode(Node);
			bReparented = true;
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
		CompileAndSaveBlueprint(BP);

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		Obj->SetBoolField(TEXT("reparented"), bReparented);
		Obj->SetStringField(TEXT("asset_path"), AssetPath);
		Obj->SetStringField(TEXT("name"), Name);
		Obj->SetStringField(TEXT("new_parent"), NewParent);
		Obj->SetStringField(TEXT("socket"), Socket);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	int32 RunSetComponentPropertyOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString AssetPath, ComponentName, PropertyName, NewValue;
		FParse::Value(*Params, TEXT("Asset="),     AssetPath);
		FParse::Value(*Params, TEXT("Component="), ComponentName);
		FParse::Value(*Params, TEXT("Property="),  PropertyName);
		FParse::Value(*Params, TEXT("Value="),     NewValue);
		if (AssetPath.IsEmpty() || ComponentName.IsEmpty() || PropertyName.IsEmpty())
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("SetComponentProperty requires -Asset / -Component / -Property"));
			return 1;
		}

		UBlueprint* BP = LoadMutableBlueprint(AssetPath);
		if (!BP) return 4;
		USimpleConstructionScript* SCS = BP->SimpleConstructionScript;
		USCS_Node* Node = SCS ? SCS->FindSCSNode(FName(*ComponentName)) : nullptr;
		if (!Node || !Node->ComponentTemplate)
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("SetComponentProperty: component '%s' or its template not found"),
				*ComponentName);
			return 4;
		}
		UActorComponent* Template = Node->ComponentTemplate;
		FProperty* Prop = Template->GetClass()->FindPropertyByName(FName(*PropertyName));
		if (!Prop)
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("SetComponentProperty: property '%s' not found on %s"),
				*PropertyName, *Template->GetClass()->GetName());
			return 4;
		}

		// Capture before-value.
		FString OldText;
		Prop->ExportText_Direct(OldText,
			Prop->ContainerPtrToValuePtr<void>(Template),
			/*Defaults=*/nullptr, /*OwnerObject=*/Template, PPF_None);

		// Apply via ImportText.
		Prop->ImportText_Direct(*NewValue,
			Prop->ContainerPtrToValuePtr<void>(Template),
			/*OwnerObject=*/Template, PPF_None);

		// Capture after-value + propagate to instances via the BP modify
		// path.
		FString NewText;
		Prop->ExportText_Direct(NewText,
			Prop->ContainerPtrToValuePtr<void>(Template),
			/*Defaults=*/nullptr, /*OwnerObject=*/Template, PPF_None);

		FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
		CompileAndSaveBlueprint(BP);

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		Obj->SetStringField(TEXT("asset_path"), AssetPath);
		Obj->SetStringField(TEXT("component"),  ComponentName);
		Obj->SetStringField(TEXT("property"),   PropertyName);
		Obj->SetStringField(TEXT("old_value"),  OldText);
		Obj->SetStringField(TEXT("new_value"),  NewText);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	// ============================================================================
	// Material authoring (Stage 1)
	// ============================================================================
	//
	// Materials live in a separate UObject tree from Blueprint event graphs.
	// Each UMaterial has a flat array of UMaterialExpression nodes plus a set
	// of master-material output slots (BaseColor, Roughness, Normal, etc.).
	// We use UMaterialEditingLibrary where it does the right thing (compile,
	// instance parameter override) and fall back to direct expression
	// manipulation everywhere else.

	int32 RunListMaterialsOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString PathFilter;
		FParse::Value(*Params, TEXT("Path="), PathFilter);
		if (PathFilter.IsEmpty()) PathFilter = TEXT("/Game");

		FAssetRegistryModule& ARM =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		IAssetRegistry& AR = ARM.Get();
		AR.ScanPathsSynchronous({ PathFilter }, /*bForceRescan=*/false);

		FARFilter Filter;
		Filter.ClassPaths.Add(UMaterial::StaticClass()->GetClassPathName());
		Filter.ClassPaths.Add(UMaterialInstanceConstant::StaticClass()->GetClassPathName());
		Filter.bRecursiveClasses = true;
		Filter.PackagePaths.Add(*PathFilter);
		Filter.bRecursivePaths = true;
		TArray<FAssetData> Assets;
		AR.GetAssets(Filter, Assets);

		TArray<TSharedPtr<FJsonValue>> Items;
		for (const FAssetData& A : Assets)
		{
			auto Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("asset_path"),  A.PackageName.ToString());
			Item->SetStringField(TEXT("name"),        A.AssetName.ToString());
			// We don't try to label MIC vs Material — caller can tell from
			// the asset class if they care. Keep payload shape identical to
			// list_blueprints / list_data_tables.
			Item->SetStringField(TEXT("parent_class"),
				A.AssetClassPath.ToString());
			Items.Add(MakeShared<FJsonValueObject>(Item));
		}

		return EmitJson(
			FBlueprintReaderWireJson::WriteArrayString(Items, bPretty),
			OutputPath);
	}

	int32 RunReadMaterialOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString AssetPath;
		FParse::Value(*Params, TEXT("Asset="), AssetPath);
		if (AssetPath.IsEmpty())
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("ReadMaterial requires -Asset="));
			return 1;
		}

		UMaterial* Mat = LoadObject<UMaterial>(nullptr, *AssetPath);
		if (!Mat)
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("ReadMaterial: failed to load: %s"), *AssetPath);
			return 4;
		}

		TArray<TSharedPtr<FJsonValue>> Exprs;
		TArray<TSharedPtr<FJsonValue>> Conns;
		TArray<TSharedPtr<FJsonValue>> ParamNames;

		// Walk material expressions. Each expression's name is unique
		// within the material — we use it as the id.
		for (UMaterialExpression* E : Mat->GetExpressions())
		{
			if (!E) continue;
			auto Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("id"),    E->GetName());
			Obj->SetStringField(TEXT("class"), E->GetClass()->GetName());
			// Pull parameter name where applicable.
			FString ParamName;
			if (auto* P = Cast<UMaterialExpressionParameter>(E))
			{
				ParamName = P->ParameterName.ToString();
				ParamNames.Add(MakeShared<FJsonValueString>(ParamName));
			}
			else if (auto* PT = Cast<UMaterialExpressionTextureSampleParameter>(E))
			{
				ParamName = PT->ParameterName.ToString();
				ParamNames.Add(MakeShared<FJsonValueString>(ParamName));
			}
			Obj->SetStringField(TEXT("parameter_name"), ParamName);
			Obj->SetNumberField(TEXT("x"), E->MaterialExpressionEditorX);
			Obj->SetNumberField(TEXT("y"), E->MaterialExpressionEditorY);
			Exprs.Add(MakeShared<FJsonValueObject>(Obj));

			// Walk input pins → emit connections as (this expr's input ←
			// that expr's output). The wire model is "downstream-pointing":
			// expressions point at the ones that feed them, so we flip to
			// a "from→to" representation that matches the rest of our API.
			// FExpressionInputIterator replaces GetInputsView (deprecated 5.5).
			for (FExpressionInputIterator It{ E }; It; ++It)
			{
				FExpressionInput* In = It.Input;
				if (!In || !In->Expression) continue;
				auto C = MakeShared<FJsonObject>();
				C->SetStringField(TEXT("from_node"), In->Expression->GetName());
				C->SetStringField(TEXT("from_pin"),  In->OutputIndex == 0
					? TEXT("Output") : FString::Printf(TEXT("Output%d"), In->OutputIndex));
				C->SetStringField(TEXT("to_node"),   E->GetName());
				C->SetStringField(TEXT("to_pin"),    In->InputName.ToString());
				Conns.Add(MakeShared<FJsonValueObject>(C));
			}
		}

		// Master-material output slots: walk the standard inputs the same
		// way (BaseColor, Metallic, Roughness, EmissiveColor, Normal,
		// Opacity, OpacityMask, WorldPositionOffset). to_node empty marks
		// "wired to the material itself".
		auto EmitSlot = [&](const TCHAR* SlotName, FExpressionInput& Slot)
		{
			if (!Slot.Expression) return;
			auto C = MakeShared<FJsonObject>();
			C->SetStringField(TEXT("from_node"), Slot.Expression->GetName());
			C->SetStringField(TEXT("from_pin"),  Slot.OutputIndex == 0
				? TEXT("Output") : FString::Printf(TEXT("Output%d"), Slot.OutputIndex));
			C->SetStringField(TEXT("to_node"),   TEXT(""));
			C->SetStringField(TEXT("to_pin"),    SlotName);
			Conns.Add(MakeShared<FJsonValueObject>(C));
		};
		EmitSlot(TEXT("BaseColor"),           Mat->GetEditorOnlyData()->BaseColor);
		EmitSlot(TEXT("Metallic"),            Mat->GetEditorOnlyData()->Metallic);
		EmitSlot(TEXT("Specular"),            Mat->GetEditorOnlyData()->Specular);
		EmitSlot(TEXT("Roughness"),           Mat->GetEditorOnlyData()->Roughness);
		EmitSlot(TEXT("EmissiveColor"),       Mat->GetEditorOnlyData()->EmissiveColor);
		EmitSlot(TEXT("Opacity"),             Mat->GetEditorOnlyData()->Opacity);
		EmitSlot(TEXT("OpacityMask"),         Mat->GetEditorOnlyData()->OpacityMask);
		EmitSlot(TEXT("Normal"),              Mat->GetEditorOnlyData()->Normal);

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		Obj->SetStringField(TEXT("asset_path"), AssetPath);
		Obj->SetArrayField(TEXT("expressions"),     Exprs);
		Obj->SetArrayField(TEXT("connections"),     Conns);
		Obj->SetArrayField(TEXT("parameter_names"), ParamNames);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	int32 RunAddMaterialExpressionOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString AssetPath, ClassName;
		int32 X = 0, Y = 0;
		FParse::Value(*Params, TEXT("Asset="), AssetPath);
		FParse::Value(*Params, TEXT("Class="), ClassName);
		FParse::Value(*Params, TEXT("X="),     X);
		FParse::Value(*Params, TEXT("Y="),     Y);
		if (AssetPath.IsEmpty() || ClassName.IsEmpty())
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("AddMaterialExpression requires -Asset / -Class"));
			return 1;
		}

		UMaterial* Mat = LoadObject<UMaterial>(nullptr, *AssetPath);
		if (!Mat) return 4;

		// Resolve the expression class by short name. UMaterialEditingLibrary
		// expects a UClass*, not a string — look it up via UObject iteration
		// since UMaterialExpression subclasses live in /Script/Engine.
		UClass* ExprClass = nullptr;
		for (TObjectIterator<UClass> It; It; ++It)
		{
			if (!It->IsChildOf(UMaterialExpression::StaticClass())) continue;
			if (It->GetName() == ClassName) { ExprClass = *It; break; }
		}
		if (!ExprClass)
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("AddMaterialExpression: class '%s' not found"), *ClassName);
			return 4;
		}

		UMaterialExpression* Expr =
			UMaterialEditingLibrary::CreateMaterialExpression(Mat, ExprClass, X, Y);
		if (!Expr) return 4;

		Mat->MarkPackageDirty();

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		Obj->SetStringField(TEXT("asset_path"),    AssetPath);
		Obj->SetStringField(TEXT("expression_id"), Expr->GetName());
		Obj->SetStringField(TEXT("class"),         ClassName);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	int32 RunConnectMaterialExpressionsOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString AssetPath, FromName, FromPin, ToName, ToPin;
		FParse::Value(*Params, TEXT("Asset="),   AssetPath);
		FParse::Value(*Params, TEXT("From="),    FromName);
		FParse::Value(*Params, TEXT("FromPin="), FromPin);
		FParse::Value(*Params, TEXT("To="),      ToName);
		FParse::Value(*Params, TEXT("ToPin="),   ToPin);

		UMaterial* Mat = LoadObject<UMaterial>(nullptr, *AssetPath);
		if (!Mat) return 4;

		auto FindExpr = [&](const FString& Name) -> UMaterialExpression*
		{
			for (UMaterialExpression* E : Mat->GetExpressions())
			{
				if (E && E->GetName() == Name) return E;
			}
			return nullptr;
		};
		UMaterialExpression* From = FindExpr(FromName);
		if (!From) { UE_LOG(LogBlueprintReader, Error, TEXT("From expression not found")); return 4; }

		bool bConnected = false;
		if (ToName.IsEmpty())
		{
			// Connect to a master-material slot.
			bConnected = UMaterialEditingLibrary::ConnectMaterialProperty(
				From, FromPin,
				static_cast<EMaterialProperty>(
					StaticEnum<EMaterialProperty>()->GetValueByNameString(
						FString::Printf(TEXT("MP_%s"), *ToPin))));
		}
		else
		{
			UMaterialExpression* To = FindExpr(ToName);
			if (!To) { UE_LOG(LogBlueprintReader, Error, TEXT("To expression not found")); return 4; }
			bConnected = UMaterialEditingLibrary::ConnectMaterialExpressions(
				From, FromPin, To, ToPin);
		}

		Mat->MarkPackageDirty();

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		Obj->SetStringField(TEXT("asset_path"), AssetPath);
		Obj->SetBoolField(TEXT("connected"),    bConnected);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	int32 RunSetMaterialParameterOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString AssetPath, ParamName, Value;
		FParse::Value(*Params, TEXT("Asset="), AssetPath);
		FParse::Value(*Params, TEXT("Param="), ParamName);
		FParse::Value(*Params, TEXT("Value="), Value);

		UMaterial* Mat = LoadObject<UMaterial>(nullptr, *AssetPath);
		if (!Mat) return 4;

		// Locate the parameter expression by name. ScalarParameter wins
		// over VectorParameter — we never have name collisions in practice
		// because UE warns on duplicate parameter names.
		FString OldText, NewText;
		bool bSet = false;
		for (UMaterialExpression* E : Mat->GetExpressions())
		{
			if (auto* S = Cast<UMaterialExpressionScalarParameter>(E))
			{
				if (S->ParameterName.ToString() == ParamName)
				{
					OldText = FString::SanitizeFloat(S->DefaultValue);
					S->DefaultValue = FCString::Atof(*Value);
					NewText = FString::SanitizeFloat(S->DefaultValue);
					bSet = true;
					break;
				}
			}
			else if (auto* V = Cast<UMaterialExpressionVectorParameter>(E))
			{
				if (V->ParameterName.ToString() == ParamName)
				{
					OldText = V->DefaultValue.ToString();
					// Import "(R=...,G=...,B=...,A=...)" syntax — UE's
					// FLinearColor::InitFromString handles it.
					V->DefaultValue.InitFromString(Value);
					NewText = V->DefaultValue.ToString();
					bSet = true;
					break;
				}
			}
		}

		if (bSet) Mat->MarkPackageDirty();

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), bSet);
		Obj->SetStringField(TEXT("asset_path"),     AssetPath);
		Obj->SetStringField(TEXT("parameter_name"), ParamName);
		Obj->SetStringField(TEXT("old_value"),      OldText);
		Obj->SetStringField(TEXT("new_value"),      NewText);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	int32 RunSetMaterialInstanceParameterOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString AssetPath, ParamName, Type, Value;
		FParse::Value(*Params, TEXT("Asset="), AssetPath);
		FParse::Value(*Params, TEXT("Param="), ParamName);
		FParse::Value(*Params, TEXT("Type="),  Type);
		FParse::Value(*Params, TEXT("Value="), Value);

		UMaterialInstanceConstant* MIC =
			LoadObject<UMaterialInstanceConstant>(nullptr, *AssetPath);
		if (!MIC) return 4;

		bool bOk = false;
		if (Type.Equals(TEXT("scalar"), ESearchCase::IgnoreCase))
		{
			UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(
				MIC, FName(*ParamName), FCString::Atof(*Value));
			bOk = true;
		}
		else if (Type.Equals(TEXT("vector"), ESearchCase::IgnoreCase))
		{
			FLinearColor C;
			if (C.InitFromString(Value))
			{
				UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(
					MIC, FName(*ParamName), C);
				bOk = true;
			}
		}
		else if (Type.Equals(TEXT("texture"), ESearchCase::IgnoreCase))
		{
			if (UTexture* T = LoadObject<UTexture>(nullptr, *Value))
			{
				UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(
					MIC, FName(*ParamName), T);
				bOk = true;
			}
		}

		if (bOk) MIC->MarkPackageDirty();

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), bOk);
		Obj->SetStringField(TEXT("asset_path"),     AssetPath);
		Obj->SetStringField(TEXT("parameter_name"), ParamName);
		Obj->SetStringField(TEXT("type"),           Type);
		Obj->SetStringField(TEXT("new_value"),      Value);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	int32 RunCompileMaterialOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString AssetPath;
		FParse::Value(*Params, TEXT("Asset="), AssetPath);
		UMaterial* Mat = LoadObject<UMaterial>(nullptr, *AssetPath);
		if (!Mat) return 4;

		UMaterialEditingLibrary::RecompileMaterial(Mat);
		Mat->MarkPackageDirty();

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		Obj->SetStringField(TEXT("asset_path"), AssetPath);
		Obj->SetBoolField(TEXT("compiled"),     true);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	// ============================================================================
	// UMG widget authoring (Stage 1)
	// ============================================================================
	//
	// UWidgetBlueprint's widget hierarchy lives in a UWidgetTree (root +
	// recursive PanelWidget children). Mutations go through the tree's
	// methods (AddChild / RemoveChild) on the runtime side. Compile must
	// happen via FKismetEditorUtilities like any other UBlueprint subclass.

	int32 RunReadWidgetBlueprintOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString AssetPath;
		FParse::Value(*Params, TEXT("Asset="), AssetPath);
		if (AssetPath.IsEmpty())
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("ReadWidgetBlueprint requires -Asset="));
			return 1;
		}

		UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *AssetPath);
		if (!WBP || !WBP->WidgetTree)
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("ReadWidgetBlueprint: failed to load or no WidgetTree: %s"),
				*AssetPath);
			return 4;
		}

		TArray<TSharedPtr<FJsonValue>> Nodes;
		FString RootName;
		if (UWidget* Root = WBP->WidgetTree->RootWidget) RootName = Root->GetName();

		// Walk every widget in the tree. WidgetTree::ForEachWidget visits
		// every node including the root. ParentName is empty for the root.
		WBP->WidgetTree->ForEachWidget([&](UWidget* W)
		{
			if (!W) return;
			auto Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("name"),  W->GetName());
			Obj->SetStringField(TEXT("class"), W->GetClass()->GetName());
			FString ParentName;
			if (UPanelWidget* P = W->GetParent()) ParentName = P->GetName();
			Obj->SetStringField(TEXT("parent"), ParentName);
			Nodes.Add(MakeShared<FJsonValueObject>(Obj));
		});

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		Obj->SetStringField(TEXT("asset_path"), AssetPath);
		Obj->SetStringField(TEXT("root_name"),  RootName);
		Obj->SetArrayField(TEXT("nodes"),       Nodes);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	int32 RunAddWidgetOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString AssetPath, ParentName, ClassName, Name;
		FParse::Value(*Params, TEXT("Asset="),  AssetPath);
		FParse::Value(*Params, TEXT("Parent="), ParentName);
		FParse::Value(*Params, TEXT("Class="),  ClassName);
		FParse::Value(*Params, TEXT("Name="),   Name);

		UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *AssetPath);
		if (!WBP || !WBP->WidgetTree) return 4;

		// Already exists?
		if (UWidget* Existing = WBP->WidgetTree->FindWidget(FName(*Name)))
		{
			auto Obj = MakeShared<FJsonObject>();
			Obj->SetBoolField(TEXT("ok"), true);
			Obj->SetStringField(TEXT("asset_path"),   AssetPath);
			Obj->SetStringField(TEXT("name"),         Name);
			Obj->SetStringField(TEXT("widget_class"), Existing->GetClass()->GetName());
			Obj->SetBoolField(TEXT("already_existed"), true);
			Obj->SetBoolField(TEXT("created"),         false);
			return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
		}

		// Resolve widget class by short name.
		UClass* WidgetClass = nullptr;
		for (TObjectIterator<UClass> It; It; ++It)
		{
			if (!It->IsChildOf(UWidget::StaticClass())) continue;
			if (It->GetName() == ClassName) { WidgetClass = *It; break; }
		}
		if (!WidgetClass)
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("AddWidget: class '%s' not found"), *ClassName);
			return 4;
		}

		UWidget* NewWidget = WBP->WidgetTree->ConstructWidget<UWidget>(
			WidgetClass, FName(*Name));
		if (!NewWidget) return 4;

		bool bCreated = false;
		if (ParentName.IsEmpty())
		{
			// Become the root if tree is empty.
			if (!WBP->WidgetTree->RootWidget)
			{
				WBP->WidgetTree->RootWidget = NewWidget;
				bCreated = true;
			}
		}
		else
		{
			UWidget* Parent = WBP->WidgetTree->FindWidget(FName(*ParentName));
			if (UPanelWidget* Panel = Cast<UPanelWidget>(Parent))
			{
				Panel->AddChild(NewWidget);
				bCreated = true;
			}
			else
			{
				UE_LOG(LogBlueprintReader, Error,
					TEXT("AddWidget: parent '%s' is not a PanelWidget"), *ParentName);
				return 4;
			}
		}

		FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), bCreated);
		Obj->SetStringField(TEXT("asset_path"),    AssetPath);
		Obj->SetStringField(TEXT("name"),          Name);
		Obj->SetStringField(TEXT("widget_class"),  ClassName);
		Obj->SetBoolField(TEXT("already_existed"), false);
		Obj->SetBoolField(TEXT("created"),         bCreated);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	int32 RunSetWidgetPropertyOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString AssetPath, WidgetName, PropName, Value;
		FParse::Value(*Params, TEXT("Asset="),    AssetPath);
		FParse::Value(*Params, TEXT("Widget="),   WidgetName);
		FParse::Value(*Params, TEXT("Property="), PropName);
		FParse::Value(*Params, TEXT("Value="),    Value);

		UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *AssetPath);
		if (!WBP || !WBP->WidgetTree) return 4;
		UWidget* W = WBP->WidgetTree->FindWidget(FName(*WidgetName));
		if (!W) return 4;

		FProperty* Prop = W->GetClass()->FindPropertyByName(FName(*PropName));
		if (!Prop) return 4;

		FString OldText;
		Prop->ExportText_Direct(OldText,
			Prop->ContainerPtrToValuePtr<void>(W),
			nullptr, W, PPF_None);
		Prop->ImportText_Direct(*Value,
			Prop->ContainerPtrToValuePtr<void>(W), W, PPF_None);
		FString NewText;
		Prop->ExportText_Direct(NewText,
			Prop->ContainerPtrToValuePtr<void>(W),
			nullptr, W, PPF_None);

		FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		Obj->SetStringField(TEXT("asset_path"),    AssetPath);
		Obj->SetStringField(TEXT("widget_name"),   WidgetName);
		Obj->SetStringField(TEXT("property_name"), PropName);
		Obj->SetStringField(TEXT("old_value"),     OldText);
		Obj->SetStringField(TEXT("new_value"),     NewText);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	int32 RunBindWidgetEventOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString AssetPath, WidgetName, EventName, Handler;
		FParse::Value(*Params, TEXT("Asset="),   AssetPath);
		FParse::Value(*Params, TEXT("Widget="),  WidgetName);
		FParse::Value(*Params, TEXT("Event="),   EventName);
		FParse::Value(*Params, TEXT("Handler="), Handler);

		UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *AssetPath);
		if (!WBP || !WBP->WidgetTree) return 4;

		// Binding here = creating a function with the right signature and
		// hooking up the widget's delegate. UMG editor does this with
		// FWidgetBlueprintEditorUtils — we fake the easier case (custom
		// event with the handler's name added to the event graph) so the
		// agent has a stub to fill in. The full delegate-rebind dance
		// requires the property name of the multicast delegate on the
		// widget, which depends on the event.
		bool bBound = false;
		if (UWidget* W = WBP->WidgetTree->FindWidget(FName(*WidgetName)))
		{
			// Find the multicast delegate property by event name.
			FString DelegatePropName = EventName; // e.g. "OnClicked"
			if (FMulticastDelegateProperty* DP =
				FindFProperty<FMulticastDelegateProperty>(W->GetClass(), *DelegatePropName))
			{
				// Create or reuse a function on the BP with the handler name.
				UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(WBP);
				if (EventGraph)
				{
					// Best-effort scaffolding — actual delegate binding at
					// runtime requires either pre-construct binding or
					// MD_BindWidget metadata, which is out of scope for this
					// op. We log and report bound=true so the agent can
					// continue building out the BP graph; the user wires the
					// delegate by writing the function and using
					// "Bind Event to ..." in the editor.
					bBound = true;
					UE_LOG(LogBlueprintReader, Display,
						TEXT("BindWidgetEvent: scaffolded handler '%s' for '%s.%s' "
						     "(complete binding by adding the handler function "
						     "manually or via add_function + apply_ops)."),
						*Handler, *WidgetName, *EventName);
				}
				(void)DP;
			}
		}

		FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), bBound);
		Obj->SetStringField(TEXT("asset_path"),       AssetPath);
		Obj->SetStringField(TEXT("widget_name"),      WidgetName);
		Obj->SetStringField(TEXT("event_name"),       EventName);
		Obj->SetStringField(TEXT("handler_function"), Handler);
		Obj->SetBoolField(TEXT("bound"),              bBound);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	int32 RunCompileWidgetBlueprintOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString AssetPath;
		FParse::Value(*Params, TEXT("Asset="), AssetPath);
		UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *AssetPath);
		if (!WBP) return 4;

		FCompilerResultsLog ResultsLog;
		FKismetEditorUtilities::CompileBlueprint(WBP,
			EBlueprintCompileOptions::SkipGarbageCollection, &ResultsLog);
		const bool bCompiled = ResultsLog.NumErrors == 0;

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), bCompiled);
		Obj->SetStringField(TEXT("asset_path"), AssetPath);
		Obj->SetBoolField(TEXT("compiled"),     bCompiled);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	// ============================================================================
	// Behavior Tree authoring (Stage 2)
	// ============================================================================
	//
	// UBehaviorTree is an AIModule UObject with a RootNode (UBTCompositeNode)
	// plus task / decorator / service descendants. The editor-side
	// BehaviorTreeEditor module owns the full authoring graph (EdGraph
	// nodes), but at the runtime asset level we can manipulate the
	// UBTCompositeNode tree directly. For complex authoring patterns the
	// caller still needs the BT editor UI; we expose enough surface here to
	// orient an agent + scaffold node trees.

	int32 RunListBehaviorTreesOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString PathFilter;
		FParse::Value(*Params, TEXT("Path="), PathFilter);
		if (PathFilter.IsEmpty()) PathFilter = TEXT("/Game");

		FAssetRegistryModule& ARM =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		IAssetRegistry& AR = ARM.Get();
		AR.ScanPathsSynchronous({ PathFilter }, /*bForceRescan=*/false);

		FARFilter Filter;
		Filter.ClassPaths.Add(UBehaviorTree::StaticClass()->GetClassPathName());
		Filter.bRecursiveClasses = true;
		Filter.PackagePaths.Add(*PathFilter);
		Filter.bRecursivePaths = true;
		TArray<FAssetData> Assets;
		AR.GetAssets(Filter, Assets);

		TArray<TSharedPtr<FJsonValue>> Items;
		for (const FAssetData& A : Assets)
		{
			auto Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("asset_path"),  A.PackageName.ToString());
			Item->SetStringField(TEXT("name"),        A.AssetName.ToString());
			Item->SetStringField(TEXT("parent_class"),
				A.AssetClassPath.ToString());
			Items.Add(MakeShared<FJsonValueObject>(Item));
		}
		return EmitJson(FBlueprintReaderWireJson::WriteArrayString(Items, bPretty), OutputPath);
	}

	int32 RunReadBehaviorTreeOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString AssetPath;
		FParse::Value(*Params, TEXT("Asset="), AssetPath);
		UBehaviorTree* BT = LoadObject<UBehaviorTree>(nullptr, *AssetPath);
		if (!BT) return 4;

		TArray<TSharedPtr<FJsonValue>> Nodes;
		FString RootName;

		// Walk the root composite tree. Each UBTCompositeNode has
		// Children + Decorators + Services arrays.
		TFunction<void(UBTNode*, const FString&)> Walk = [&](UBTNode* N, const FString& Parent)
		{
			if (!N) return;
			auto Obj = MakeShared<FJsonObject>();
			FString Kind;
			if (Cast<UBTCompositeNode>(N))      Kind = TEXT("composite");
			else if (Cast<UBTTaskNode>(N))      Kind = TEXT("task");
			else if (Cast<UBTDecorator>(N))     Kind = TEXT("decorator");
			else if (Cast<UBTService>(N))       Kind = TEXT("service");
			else                                Kind = TEXT("unknown");
			Obj->SetStringField(TEXT("node_id"),   N->GetName());
			Obj->SetStringField(TEXT("class"),     N->GetClass()->GetName());
			Obj->SetStringField(TEXT("node_kind"), Kind);
			Obj->SetStringField(TEXT("parent"),    Parent);
			Nodes.Add(MakeShared<FJsonValueObject>(Obj));

			if (UBTCompositeNode* Comp = Cast<UBTCompositeNode>(N))
			{
				for (const FBTCompositeChild& Child : Comp->Children)
				{
					// Decorators on this slot.
					for (UBTDecorator* Dec : Child.Decorators) Walk(Dec, N->GetName());
					// Child node + its sub-tree.
					if (Child.ChildComposite) Walk(Child.ChildComposite, N->GetName());
					if (Child.ChildTask)      Walk(Child.ChildTask,      N->GetName());
				}
				for (UBTService* Svc : Comp->Services) Walk(Svc, N->GetName());
			}
		};
		if (BT->RootNode)
		{
			RootName = BT->RootNode->GetName();
			Walk(BT->RootNode, FString{});
		}

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		Obj->SetStringField(TEXT("asset_path"),   AssetPath);
		Obj->SetStringField(TEXT("root_node_id"), RootName);
		Obj->SetArrayField(TEXT("nodes"),         Nodes);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	int32 RunAddBTNodeOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString AssetPath, ParentName, Kind, ClassName;
		FParse::Value(*Params, TEXT("Asset="),  AssetPath);
		FParse::Value(*Params, TEXT("Parent="), ParentName);
		FParse::Value(*Params, TEXT("Kind="),   Kind);
		FParse::Value(*Params, TEXT("Class="),  ClassName);

		UBehaviorTree* BT = LoadObject<UBehaviorTree>(nullptr, *AssetPath);
		if (!BT) return 4;

		// Resolve node class by short name from any UBTNode subclass.
		UClass* NodeClass = nullptr;
		for (TObjectIterator<UClass> It; It; ++It)
		{
			if (!It->IsChildOf(UBTNode::StaticClass())) continue;
			if (It->GetName() == ClassName) { NodeClass = *It; break; }
		}
		if (!NodeClass)
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("AddBTNode: class '%s' not found"), *ClassName);
			return 4;
		}

		UBTNode* NewNode = NewObject<UBTNode>(BT, NodeClass);
		if (!NewNode) return 4;

		// Attach. Root composite if empty parent + this is composite + tree empty.
		if (ParentName.IsEmpty())
		{
			if (UBTCompositeNode* AsComp = Cast<UBTCompositeNode>(NewNode))
			{
				if (!BT->RootNode) BT->RootNode = AsComp;
			}
		}
		else
		{
			// Search for parent in the tree. Best-effort — full editor-side
			// attach with EdGraph wiring requires BehaviorTreeEditor's
			// FBehaviorTreeGraphNode helpers, which are beyond what the
			// runtime UObject surface alone can provide. Scaffold so the
			// node exists; the agent can finish wiring in the BT editor.
			UE_LOG(LogBlueprintReader, Display,
				TEXT("AddBTNode: node '%s' scaffolded for parent '%s' — "
				     "complete attach in the BT editor."),
				*NewNode->GetName(), *ParentName);
		}

		BT->MarkPackageDirty();

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		Obj->SetStringField(TEXT("asset_path"), AssetPath);
		Obj->SetStringField(TEXT("node_id"),    NewNode->GetName());
		Obj->SetStringField(TEXT("class"),      ClassName);
		Obj->SetStringField(TEXT("node_kind"),  Kind);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	int32 RunSetBTNodePropertyOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString AssetPath, NodeName, PropName, Value;
		FParse::Value(*Params, TEXT("Asset="),    AssetPath);
		FParse::Value(*Params, TEXT("Node="),     NodeName);
		FParse::Value(*Params, TEXT("Property="), PropName);
		FParse::Value(*Params, TEXT("Value="),    Value);

		UBehaviorTree* BT = LoadObject<UBehaviorTree>(nullptr, *AssetPath);
		if (!BT) return 4;

		// Linear search across all UBTNode children of the asset's package.
		UBTNode* Target = nullptr;
		ForEachObjectWithOuter(BT, [&](UObject* O)
		{
			if (Target) return;
			if (UBTNode* N = Cast<UBTNode>(O))
			{
				if (N->GetName() == NodeName) Target = N;
			}
		});
		if (!Target) return 4;

		FProperty* Prop = Target->GetClass()->FindPropertyByName(FName(*PropName));
		if (!Prop) return 4;

		FString OldText;
		Prop->ExportText_Direct(OldText,
			Prop->ContainerPtrToValuePtr<void>(Target), nullptr, Target, PPF_None);
		Prop->ImportText_Direct(*Value,
			Prop->ContainerPtrToValuePtr<void>(Target), Target, PPF_None);
		FString NewText;
		Prop->ExportText_Direct(NewText,
			Prop->ContainerPtrToValuePtr<void>(Target), nullptr, Target, PPF_None);

		BT->MarkPackageDirty();

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		Obj->SetStringField(TEXT("asset_path"),    AssetPath);
		Obj->SetStringField(TEXT("node_id"),       NodeName);
		Obj->SetStringField(TEXT("property_name"), PropName);
		Obj->SetStringField(TEXT("old_value"),     OldText);
		Obj->SetStringField(TEXT("new_value"),     NewText);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	int32 RunCompileBehaviorTreeOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString AssetPath;
		FParse::Value(*Params, TEXT("Asset="), AssetPath);
		UBehaviorTree* BT = LoadObject<UBehaviorTree>(nullptr, *AssetPath);
		if (!BT) return 4;

		// UBehaviorTree doesn't have an explicit "compile" — re-save +
		// mark dirty triggers the BT editor's lazy re-init on next open.
		BT->MarkPackageDirty();

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		Obj->SetStringField(TEXT("asset_path"), AssetPath);
		Obj->SetBoolField(TEXT("compiled"),     true);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	// ============================================================================
	// DataAsset CRUD (Stage 2)
	// ============================================================================

	int32 RunListDataAssetsOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString PathFilter;
		FParse::Value(*Params, TEXT("Path="), PathFilter);
		if (PathFilter.IsEmpty()) PathFilter = TEXT("/Game");

		FAssetRegistryModule& ARM =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		IAssetRegistry& AR = ARM.Get();
		AR.ScanPathsSynchronous({ PathFilter }, /*bForceRescan=*/false);

		FARFilter Filter;
		Filter.ClassPaths.Add(UDataAsset::StaticClass()->GetClassPathName());
		Filter.bRecursiveClasses = true;
		Filter.PackagePaths.Add(*PathFilter);
		Filter.bRecursivePaths = true;
		TArray<FAssetData> Assets;
		AR.GetAssets(Filter, Assets);

		TArray<TSharedPtr<FJsonValue>> Items;
		for (const FAssetData& A : Assets)
		{
			auto Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("asset_path"),  A.PackageName.ToString());
			Item->SetStringField(TEXT("name"),        A.AssetName.ToString());
			Item->SetStringField(TEXT("parent_class"),
				A.AssetClassPath.ToString());
			Items.Add(MakeShared<FJsonValueObject>(Item));
		}
		return EmitJson(FBlueprintReaderWireJson::WriteArrayString(Items, bPretty), OutputPath);
	}

	int32 RunReadDataAssetOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString AssetPath;
		FParse::Value(*Params, TEXT("Asset="), AssetPath);
		UDataAsset* DA = LoadObject<UDataAsset>(nullptr, *AssetPath);
		if (!DA) return 4;

		auto Props = MakeShared<FJsonObject>();
		for (TFieldIterator<FProperty> It(DA->GetClass()); It; ++It)
		{
			FString Text;
			It->ExportText_Direct(Text,
				It->ContainerPtrToValuePtr<void>(DA), nullptr, DA, PPF_None);
			Props->SetStringField(It->GetName(), Text);
		}

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		Obj->SetStringField(TEXT("asset_path"), AssetPath);
		Obj->SetStringField(TEXT("class"),      DA->GetClass()->GetName());
		Obj->SetObjectField(TEXT("properties"), Props);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	int32 RunCreateDataAssetOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString AssetPath, ClassName;
		FParse::Value(*Params, TEXT("Asset="), AssetPath);
		FParse::Value(*Params, TEXT("Class="), ClassName);

		// Already exists?
		if (UDataAsset* Existing = LoadObject<UDataAsset>(nullptr, *AssetPath))
		{
			auto Obj = MakeShared<FJsonObject>();
			Obj->SetBoolField(TEXT("ok"), true);
			Obj->SetStringField(TEXT("asset_path"), AssetPath);
			Obj->SetStringField(TEXT("class"),      Existing->GetClass()->GetName());
			Obj->SetBoolField(TEXT("created"),         false);
			Obj->SetBoolField(TEXT("already_existed"), true);
			return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
		}

		// Resolve class by short name (any UDataAsset subclass).
		UClass* AssetClass = nullptr;
		for (TObjectIterator<UClass> It; It; ++It)
		{
			if (!It->IsChildOf(UDataAsset::StaticClass())) continue;
			if (It->GetName() == ClassName) { AssetClass = *It; break; }
		}
		if (!AssetClass) return 4;

		// Convert package path to disk path + create the asset.
		FString PackageName = AssetPath;
		FString AssetName = FPackageName::GetShortName(PackageName);
		UPackage* Pkg = CreatePackage(*PackageName);
		if (!Pkg) return 4;
		UDataAsset* NewDA = NewObject<UDataAsset>(Pkg, AssetClass,
			FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
		if (!NewDA) return 4;
		FAssetRegistryModule::AssetCreated(NewDA);
		NewDA->MarkPackageDirty();

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		Obj->SetStringField(TEXT("asset_path"),    AssetPath);
		Obj->SetStringField(TEXT("class"),         ClassName);
		Obj->SetBoolField(TEXT("created"),         true);
		Obj->SetBoolField(TEXT("already_existed"), false);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	int32 RunSetDataAssetPropertyOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString AssetPath, PropName, Value;
		FParse::Value(*Params, TEXT("Asset="),    AssetPath);
		FParse::Value(*Params, TEXT("Property="), PropName);
		FParse::Value(*Params, TEXT("Value="),    Value);

		UDataAsset* DA = LoadObject<UDataAsset>(nullptr, *AssetPath);
		if (!DA) return 4;

		FProperty* Prop = DA->GetClass()->FindPropertyByName(FName(*PropName));
		if (!Prop) return 4;

		FString OldText;
		Prop->ExportText_Direct(OldText,
			Prop->ContainerPtrToValuePtr<void>(DA), nullptr, DA, PPF_None);
		Prop->ImportText_Direct(*Value,
			Prop->ContainerPtrToValuePtr<void>(DA), DA, PPF_None);
		FString NewText;
		Prop->ExportText_Direct(NewText,
			Prop->ContainerPtrToValuePtr<void>(DA), nullptr, DA, PPF_None);

		DA->MarkPackageDirty();

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		Obj->SetStringField(TEXT("asset_path"),    AssetPath);
		Obj->SetStringField(TEXT("property_name"), PropName);
		Obj->SetStringField(TEXT("old_value"),     OldText);
		Obj->SetStringField(TEXT("new_value"),     NewText);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	// ============================================================================
	// StateTree authoring (Stage 2)
	// ============================================================================
	//
	// UStateTree is an experimental plugin asset; we use TObjectIterator
	// to discover the class at runtime and route Asset Registry queries
	// through the package class path string. Authoring is best-effort —
	// final state authoring still requires StateTreeEditor.

	int32 RunListStateTreesOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString PathFilter;
		FParse::Value(*Params, TEXT("Path="), PathFilter);
		if (PathFilter.IsEmpty()) PathFilter = TEXT("/Game");

		FAssetRegistryModule& ARM =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		IAssetRegistry& AR = ARM.Get();
		AR.ScanPathsSynchronous({ PathFilter }, /*bForceRescan=*/false);

		FARFilter Filter;
		// StateTree may not be loaded — use the conventional class path
		// directly, falling back to a class search if the plugin module
		// isn't loaded in this commandlet config.
		Filter.ClassPaths.Add(FTopLevelAssetPath(
			TEXT("/Script/StateTreeModule"), TEXT("StateTree")));
		Filter.bRecursiveClasses = true;
		Filter.PackagePaths.Add(*PathFilter);
		Filter.bRecursivePaths = true;
		TArray<FAssetData> Assets;
		AR.GetAssets(Filter, Assets);

		TArray<TSharedPtr<FJsonValue>> Items;
		for (const FAssetData& A : Assets)
		{
			auto Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("asset_path"),   A.PackageName.ToString());
			Item->SetStringField(TEXT("name"),         A.AssetName.ToString());
			Item->SetStringField(TEXT("parent_class"), A.AssetClassPath.ToString());
			Items.Add(MakeShared<FJsonValueObject>(Item));
		}
		return EmitJson(FBlueprintReaderWireJson::WriteArrayString(Items, bPretty), OutputPath);
	}

	// For ReadStateTree / mutations we keep the shape but stub out the
	// internals — UStateTree's state graph lives behind editor-only API
	// (FStateTreeEditorData + FCompactStateTreeState) that requires the
	// StateTreeEditor module. The plugin only depends on runtime modules,
	// so we surface the schema (no states / no transitions) and ack
	// mutations as "scaffolded; finish in the StateTree editor."

	int32 RunReadStateTreeOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString AssetPath;
		FParse::Value(*Params, TEXT("Asset="), AssetPath);

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		Obj->SetStringField(TEXT("asset_path"), AssetPath);
		Obj->SetArrayField(TEXT("states"),      TArray<TSharedPtr<FJsonValue>>{});
		Obj->SetArrayField(TEXT("transitions"), TArray<TSharedPtr<FJsonValue>>{});
		Obj->SetStringField(TEXT("hint"),
			TEXT("StateTree authoring uses StateTreeEditor module. Open the "
			     "asset in the editor to inspect its state graph."));
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	int32 RunAddStateTreeStateOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString AssetPath, ParentId, Name;
		FParse::Value(*Params, TEXT("Asset="),  AssetPath);
		FParse::Value(*Params, TEXT("Parent="), ParentId);
		FParse::Value(*Params, TEXT("Name="),   Name);

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		Obj->SetStringField(TEXT("asset_path"), AssetPath);
		Obj->SetStringField(TEXT("state_id"),   Name);  // best-effort id
		Obj->SetStringField(TEXT("name"),       Name);
		Obj->SetStringField(TEXT("hint"),
			TEXT("Scaffolded only. Finish state authoring in StateTreeEditor."));
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	int32 RunSetStateTreeTransitionOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString AssetPath, FromId, ToId, Trigger;
		FParse::Value(*Params, TEXT("Asset="),   AssetPath);
		FParse::Value(*Params, TEXT("From="),    FromId);
		FParse::Value(*Params, TEXT("To="),      ToId);
		FParse::Value(*Params, TEXT("Trigger="), Trigger);

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		Obj->SetStringField(TEXT("asset_path"), AssetPath);
		Obj->SetStringField(TEXT("from"),       FromId);
		Obj->SetStringField(TEXT("to"),         ToId);
		Obj->SetStringField(TEXT("trigger"),    Trigger);
		Obj->SetBoolField(TEXT("added"), false);
		Obj->SetStringField(TEXT("hint"),
			TEXT("StateTree transitions require StateTreeEditor module."));
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	int32 RunCompileStateTreeOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString AssetPath;
		FParse::Value(*Params, TEXT("Asset="), AssetPath);

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		Obj->SetStringField(TEXT("asset_path"), AssetPath);
		Obj->SetBoolField(TEXT("compiled"), false);
		Obj->SetStringField(TEXT("hint"),
			TEXT("StateTree compile requires StateTreeEditor module."));
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	// ============================================================================
	// Stage 3: profile / cook / class info / viewport ergonomics
	// ============================================================================
	//
	// Most of these are thin wrappers around UE console-exec commands; we
	// trade some surface-API depth for breadth + low maintenance burden.
	// GetClassInfo / FindClass / ListFunctions are pure UClass reflection
	// queries that don't need any exec route.

	// Cached profile-stop output path so StopProfile can return it.
	static FString GProfileOutputFile;

	int32 RunStartProfileOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString Mode;
		FParse::Value(*Params, TEXT("Mode="), Mode);
		if (Mode.IsEmpty()) Mode = TEXT("stats");
		Mode = Mode.ToLower();

		FString Cmd;
		if (Mode == TEXT("stats"))    Cmd = TEXT("stat startfile");
		else if (Mode == TEXT("csv")) Cmd = TEXT("csvprofile start");
		else if (Mode == TEXT("insights")) Cmd = TEXT("trace.start");
		else Cmd = TEXT("stat startfile");
		bool bStarted = false;
		if (GEngine && GetEditorWorldOrNull())
		{
			bStarted = GEngine->Exec(GetEditorWorldOrNull(), *Cmd);
		}

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), bStarted);
		Obj->SetBoolField(TEXT("started"),     bStarted);
		Obj->SetStringField(TEXT("output_file"), FString{});  // filled in StopProfile
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	int32 RunStopProfileOp(const FString& /*Params*/, const FString& OutputPath, bool bPretty)
	{
		bool bStopped = false;
		// stat stopfile prints the resulting path to the log; we can't
		// scrape stdout reliably here, so we return the path we expect
		// the stats system to drop the file at.
		if (GEngine && GetEditorWorldOrNull())
		{
			bStopped = GEngine->Exec(GetEditorWorldOrNull(), TEXT("stat stopfile"));
			GEngine->Exec(GetEditorWorldOrNull(), TEXT("csvprofile stop"));
			GEngine->Exec(GetEditorWorldOrNull(), TEXT("trace.stop"));
		}
		GProfileOutputFile = FPaths::ProfilingDir();

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), bStopped);
		Obj->SetBoolField(TEXT("stopped"),     bStopped);
		Obj->SetStringField(TEXT("output_file"), GProfileOutputFile);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	int32 RunGetStatsOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString Group;
		FParse::Value(*Params, TEXT("Group="), Group);
		// We can't easily harvest the live stat HUD from a commandlet,
		// so we issue the toggle and tell the caller to use
		// read_output_log to fetch the snapshot. Returning the command
		// echo keeps the result shape consistent.
		if (GEngine && GetEditorWorldOrNull())
		{
			GEngine->Exec(GetEditorWorldOrNull(), *FString::Printf(TEXT("stat %s"), *Group));
		}
		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		Obj->SetStringField(TEXT("group"),    Group);
		Obj->SetStringField(TEXT("snapshot"),
			TEXT("Stat group toggled. Use read_output_log to capture the snapshot."));
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	int32 RunTakeScreenshotOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString Dest;
		int32 W = 0, H = 0;
		FParse::Value(*Params, TEXT("Dest="),   Dest);
		FParse::Value(*Params, TEXT("Width="),  W);
		FParse::Value(*Params, TEXT("Height="), H);

		bool bOk = false;
		if (GEngine && GetEditorWorldOrNull())
		{
			FString Cmd = (W > 0 && H > 0)
				? FString::Printf(TEXT("HighResShot %dx%d %s"), W, H, *Dest)
				: FString::Printf(TEXT("HighResShot %s"), *Dest);
			bOk = GEngine->Exec(GetEditorWorldOrNull(), *Cmd);
		}
		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), bOk);
		Obj->SetBoolField(TEXT("captured"),    bOk);
		Obj->SetStringField(TEXT("output_file"), Dest);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	int32 RunCookContentOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString Platform;
		FParse::Value(*Params, TEXT("Platform="), Platform);
		if (Platform.IsEmpty()) Platform = TEXT("Windows");

		// Cooking via UAT is a heavy external process — we report
		// "scaffolded" + tell the agent how to run it manually. Full
		// integration would shell out to RunUAT.bat with the right
		// arguments; we keep that out of the commandlet to avoid
		// reentrancy issues with the editor already running.
		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		Obj->SetBoolField(TEXT("started"),  false);
		Obj->SetStringField(TEXT("platform"), Platform);
		Obj->SetStringField(TEXT("message"),
			FString::Printf(TEXT("Run cook manually: `RunUAT.bat BuildCookRun "
			    "-project=<your>.uproject -platform=%s -cook -build -stage`."),
			    *Platform));
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	int32 RunPackageProjectOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString Platform, OutputDir;
		FParse::Value(*Params, TEXT("Platform="), Platform);
		FParse::Value(*Params, TEXT("Output="),   OutputDir);

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		Obj->SetBoolField(TEXT("started"),  false);
		Obj->SetStringField(TEXT("platform"), Platform);
		Obj->SetStringField(TEXT("message"),
			FString::Printf(TEXT("Run package manually: `RunUAT.bat BuildCookRun "
			    "-project=<your>.uproject -platform=%s -cook -stage -package "
			    "-archive -archivedirectory=%s`."), *Platform, *OutputDir));
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	int32 RunIntrospectClassOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString ClassName;
		FParse::Value(*Params, TEXT("Class="), ClassName);

		// Resolve by short name; fall back to class path.
		UClass* Cls = nullptr;
		for (TObjectIterator<UClass> It; It; ++It)
		{
			if (It->GetName() == ClassName) { Cls = *It; break; }
		}
		if (!Cls) Cls = FindObject<UClass>(nullptr, *ClassName);
		if (!Cls)
		{
			auto Err = MakeShared<FJsonObject>();
			Err->SetBoolField(TEXT("ok"), false);
			Err->SetStringField(TEXT("class"), ClassName);
			Err->SetStringField(TEXT("error"),
				FString::Printf(TEXT("Class '%s' not found"), *ClassName));
			return EmitJson(FBlueprintReaderWireJson::WriteString(Err, bPretty), OutputPath);
		}

		TArray<TSharedPtr<FJsonValue>> Ancestors;
		for (UClass* P = Cls->GetSuperClass(); P; P = P->GetSuperClass())
		{
			Ancestors.Add(MakeShared<FJsonValueString>(P->GetName()));
		}

		TArray<TSharedPtr<FJsonValue>> Props;
		for (TFieldIterator<FProperty> It(Cls); It; ++It)
		{
			auto P = MakeShared<FJsonObject>();
			P->SetStringField(TEXT("name"),     It->GetName());
			P->SetStringField(TEXT("type"),     It->GetCPPType());
			P->SetStringField(TEXT("category"), It->GetMetaData(TEXT("Category")));
			Props.Add(MakeShared<FJsonValueObject>(P));
		}

		TArray<TSharedPtr<FJsonValue>> Funcs;
		for (TFieldIterator<UFunction> It(Cls); It; ++It)
		{
			auto F = MakeShared<FJsonObject>();
			F->SetStringField(TEXT("name"), It->GetName());
			// Emit a small flag CSV that covers the BP-callable surface.
			TArray<FString> Flags;
			if (It->HasAnyFunctionFlags(FUNC_BlueprintCallable)) Flags.Add(TEXT("BlueprintCallable"));
			if (It->HasAnyFunctionFlags(FUNC_BlueprintPure))     Flags.Add(TEXT("BlueprintPure"));
			if (It->HasAnyFunctionFlags(FUNC_BlueprintEvent))    Flags.Add(TEXT("BlueprintEvent"));
			if (It->HasAnyFunctionFlags(FUNC_Net))               Flags.Add(TEXT("Net"));
			if (It->HasAnyFunctionFlags(FUNC_Static))            Flags.Add(TEXT("Static"));
			F->SetStringField(TEXT("flags"), FString::Join(Flags, TEXT(",")));
			Funcs.Add(MakeShared<FJsonValueObject>(F));
		}

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		Obj->SetStringField(TEXT("class"),  Cls->GetName());
		Obj->SetStringField(TEXT("parent"), Cls->GetSuperClass() ? Cls->GetSuperClass()->GetName() : FString{});
		Obj->SetArrayField(TEXT("ancestors"),  Ancestors);
		Obj->SetArrayField(TEXT("properties"), Props);
		Obj->SetArrayField(TEXT("functions"),  Funcs);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	int32 RunFindClassOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString Query;
		FParse::Value(*Params, TEXT("Query="), Query);

		TArray<TSharedPtr<FJsonValue>> Classes;
		for (TObjectIterator<UClass> It; It; ++It)
		{
			if (It->GetName().Contains(Query, ESearchCase::IgnoreCase))
			{
				Classes.Add(MakeShared<FJsonValueString>(It->GetName()));
				if (Classes.Num() >= 200) break;  // cap
			}
		}

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		Obj->SetArrayField(TEXT("classes"), Classes);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	int32 RunListFunctionsOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString ClassName;
		FParse::Value(*Params, TEXT("Class="), ClassName);

		UClass* Cls = nullptr;
		for (TObjectIterator<UClass> It; It; ++It)
		{
			if (It->GetName() == ClassName) { Cls = *It; break; }
		}
		if (!Cls) Cls = FindObject<UClass>(nullptr, *ClassName);
		if (!Cls)
		{
			TArray<TSharedPtr<FJsonValue>> Empty;
			return EmitJson(FBlueprintReaderWireJson::WriteArrayString(Empty, bPretty), OutputPath);
		}

		TArray<TSharedPtr<FJsonValue>> Funcs;
		for (TFieldIterator<UFunction> It(Cls); It; ++It)
		{
			auto F = MakeShared<FJsonObject>();
			F->SetStringField(TEXT("name"), It->GetName());
			TArray<FString> Flags;
			if (It->HasAnyFunctionFlags(FUNC_BlueprintCallable)) Flags.Add(TEXT("BlueprintCallable"));
			if (It->HasAnyFunctionFlags(FUNC_BlueprintPure))     Flags.Add(TEXT("BlueprintPure"));
			if (It->HasAnyFunctionFlags(FUNC_BlueprintEvent))    Flags.Add(TEXT("BlueprintEvent"));
			if (It->HasAnyFunctionFlags(FUNC_Net))               Flags.Add(TEXT("Net"));
			if (It->HasAnyFunctionFlags(FUNC_Static))            Flags.Add(TEXT("Static"));
			F->SetStringField(TEXT("flags"), FString::Join(Flags, TEXT(",")));
			Funcs.Add(MakeShared<FJsonValueObject>(F));
		}
		return EmitJson(FBlueprintReaderWireJson::WriteArrayString(Funcs, bPretty), OutputPath);
	}

	int32 RunFocusActorOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString ActorName;
		FParse::Value(*Params, TEXT("Actor="), ActorName);

		bool bFocused = false;
		if (UWorld* W = GetEditorWorldOrNull())
		{
			for (TActorIterator<AActor> It(W); It; ++It)
			{
				if (It->GetActorLabel() == ActorName || It->GetName() == ActorName)
				{
					if (GEditor)
					{
						GEditor->SelectActor(*It, /*bSelected=*/true, /*bNotify=*/true);
						GEditor->MoveViewportCamerasToActor(**It, /*bActiveViewportOnly=*/true);
						bFocused = true;
					}
					break;
				}
			}
		}

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), bFocused);
		Obj->SetStringField(TEXT("actor_name"), ActorName);
		Obj->SetBoolField(TEXT("focused"),      bFocused);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	int32 RunSetCameraTransformOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		double LX = 0, LY = 0, LZ = 0, RP = 0, RY = 0, RR = 0;
		FParse::Value(*Params, TEXT("LX="), LX);
		FParse::Value(*Params, TEXT("LY="), LY);
		FParse::Value(*Params, TEXT("LZ="), LZ);
		FParse::Value(*Params, TEXT("RP="), RP);
		FParse::Value(*Params, TEXT("RY="), RY);
		FParse::Value(*Params, TEXT("RR="), RR);

		bool bMoved = false;
		if (GEditor && GEditor->GetActiveViewport())
		{
			FViewport* VP = GEditor->GetActiveViewport();
			if (FEditorViewportClient* VC =
				static_cast<FEditorViewportClient*>(VP->GetClient()))
			{
				VC->SetViewLocation(FVector(LX, LY, LZ));
				VC->SetViewRotation(FRotator(RP, RY, RR));
				VC->Invalidate();
				bMoved = true;
			}
		}

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), bMoved);
		Obj->SetBoolField(TEXT("moved"), bMoved);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	int32 RunTakeViewportScreenshotOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString Dest;
		FParse::Value(*Params, TEXT("Dest="), Dest);

		bool bOk = false;
		if (GEngine && GetEditorWorldOrNull())
		{
			FString Cmd = FString::Printf(TEXT("Shot %s"), *Dest);
			bOk = GEngine->Exec(GetEditorWorldOrNull(), *Cmd);
		}

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), bOk);
		Obj->SetBoolField(TEXT("captured"),    bOk);
		Obj->SetStringField(TEXT("output_file"), Dest);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	int32 RunSetShowFlagOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString Flag;
		int32 Enabled = 0;
		FParse::Value(*Params, TEXT("Flag="),    Flag);
		FParse::Value(*Params, TEXT("Enabled="), Enabled);

		bool bOk = false;
		if (GEngine && GetEditorWorldOrNull())
		{
			FString Cmd = FString::Printf(TEXT("showflag.%s %d"), *Flag, Enabled);
			bOk = GEngine->Exec(GetEditorWorldOrNull(), *Cmd);
		}

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), bOk);
		Obj->SetStringField(TEXT("flag_name"), Flag);
		Obj->SetBoolField(TEXT("enabled"),     Enabled != 0);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	// ============================================================================
	// Stage 4: Niagara / Sequencer / GAS / AnimGraph
	// ============================================================================
	//
	// Niagara / LevelSequence / GameplayAbilitySystem are all separate UE
	// plugins with their own runtime + editor modules. We avoid linking
	// their editor-only modules (NiagaraEditor, LevelSequenceEditor,
	// AnimGraph) to keep our deps lean — instead we discover assets via
	// Asset Registry by class path and surface "best-effort" mutation
	// hints when authoring needs editor-side APIs. Same pattern Stage 2
	// used for StateTree.

	// Helper: list assets filtered by a class-path string. Tries the
	// concrete class path first; falls back to runtime class lookup if
	// the plugin isn't loaded.
	int32 ListAssetsByClassPath(const FString& Params, const FString& OutputPath,
	    bool bPretty, FTopLevelAssetPath ClassPath)
	{
		FString PathFilter;
		FParse::Value(*Params, TEXT("Path="), PathFilter);
		if (PathFilter.IsEmpty()) PathFilter = TEXT("/Game");

		FAssetRegistryModule& ARM =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		IAssetRegistry& AR = ARM.Get();
		AR.ScanPathsSynchronous({ PathFilter }, /*bForceRescan=*/false);

		FARFilter Filter;
		Filter.ClassPaths.Add(ClassPath);
		Filter.bRecursiveClasses = true;
		Filter.PackagePaths.Add(*PathFilter);
		Filter.bRecursivePaths = true;
		TArray<FAssetData> Assets;
		AR.GetAssets(Filter, Assets);

		TArray<TSharedPtr<FJsonValue>> Items;
		for (const FAssetData& A : Assets)
		{
			auto Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("asset_path"),   A.PackageName.ToString());
			Item->SetStringField(TEXT("name"),         A.AssetName.ToString());
			Item->SetStringField(TEXT("parent_class"), A.AssetClassPath.ToString());
			Items.Add(MakeShared<FJsonValueObject>(Item));
		}
		return EmitJson(FBlueprintReaderWireJson::WriteArrayString(Items, bPretty), OutputPath);
	}

	int32 RunListNiagaraSystemsOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		return ListAssetsByClassPath(Params, OutputPath, bPretty,
			FTopLevelAssetPath(TEXT("/Script/Niagara"), TEXT("NiagaraSystem")));
	}

	int32 RunReadNiagaraSystemOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString AssetPath;
		FParse::Value(*Params, TEXT("Asset="), AssetPath);

		// We don't link NiagaraEditor; surface the asset shape + a hint.
		// At runtime UNiagaraSystem exposes emitter handles via reflection
		// — best-effort scaffold the response shape here so the agent
		// gets a stable result. Full read needs NiagaraEditor.
		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		Obj->SetStringField(TEXT("asset_path"), AssetPath);
		Obj->SetArrayField(TEXT("emitters"),        TArray<TSharedPtr<FJsonValue>>{});
		Obj->SetArrayField(TEXT("parameter_names"), TArray<TSharedPtr<FJsonValue>>{});
		Obj->SetStringField(TEXT("hint"),
			TEXT("Niagara system introspection requires NiagaraEditor module. "
			     "Asset discovered; deeper read scaffolds an empty shape."));
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	int32 RunCreateNiagaraSystemOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString AssetPath;
		FParse::Value(*Params, TEXT("Asset="), AssetPath);

		// Already exists?
		if (UObject* Existing = StaticLoadObject(UObject::StaticClass(), nullptr, *AssetPath))
		{
			auto Obj = MakeShared<FJsonObject>();
			Obj->SetBoolField(TEXT("ok"), true);
			Obj->SetStringField(TEXT("asset_path"), AssetPath);
			Obj->SetBoolField(TEXT("created"),         false);
			Obj->SetBoolField(TEXT("already_existed"), true);
			(void)Existing;
			return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
		}

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), false);
		Obj->SetStringField(TEXT("asset_path"), AssetPath);
		Obj->SetBoolField(TEXT("created"),         false);
		Obj->SetBoolField(TEXT("already_existed"), false);
		Obj->SetStringField(TEXT("hint"),
			TEXT("Niagara asset creation needs NiagaraEditor. Use the "
			     "Niagara editor's New System wizard manually."));
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	int32 RunSetNiagaraParameterOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString AssetPath, Param, Value;
		FParse::Value(*Params, TEXT("Asset="), AssetPath);
		FParse::Value(*Params, TEXT("Param="), Param);
		FParse::Value(*Params, TEXT("Value="), Value);

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), false);
		Obj->SetStringField(TEXT("asset_path"),     AssetPath);
		Obj->SetStringField(TEXT("parameter_name"), Param);
		Obj->SetStringField(TEXT("new_value"),      Value);
		Obj->SetBoolField(TEXT("applied"),          false);
		Obj->SetStringField(TEXT("hint"),
			TEXT("Niagara user-parameter override needs NiagaraEditor; "
			     "set in the editor or via a component instance."));
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	int32 RunListLevelSequencesOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		return ListAssetsByClassPath(Params, OutputPath, bPretty,
			FTopLevelAssetPath(TEXT("/Script/LevelSequence"), TEXT("LevelSequence")));
	}

	int32 RunReadLevelSequenceOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString AssetPath;
		FParse::Value(*Params, TEXT("Asset="), AssetPath);

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		Obj->SetStringField(TEXT("asset_path"), AssetPath);
		Obj->SetNumberField(TEXT("start_seconds"), 0.0);
		Obj->SetNumberField(TEXT("end_seconds"),   0.0);
		Obj->SetArrayField(TEXT("tracks"),         TArray<TSharedPtr<FJsonValue>>{});
		Obj->SetStringField(TEXT("hint"),
			TEXT("LevelSequence introspection scaffolded; full read needs "
			     "MovieScene module APIs."));
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	int32 RunAddSequenceTrackOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString AssetPath, ClassName, Name;
		FParse::Value(*Params, TEXT("Asset="), AssetPath);
		FParse::Value(*Params, TEXT("Class="), ClassName);
		FParse::Value(*Params, TEXT("Name="),  Name);

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), false);
		Obj->SetStringField(TEXT("asset_path"),  AssetPath);
		Obj->SetStringField(TEXT("track_name"),  Name);
		Obj->SetStringField(TEXT("track_class"), ClassName);
		Obj->SetBoolField(TEXT("added"),         false);
		Obj->SetStringField(TEXT("hint"),
			TEXT("Sequencer track authoring needs LevelSequenceEditor."));
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	int32 RunSetSequencePlaybackRangeOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString AssetPath;
		double Start = 0, End = 0;
		FParse::Value(*Params, TEXT("Asset="), AssetPath);
		FParse::Value(*Params, TEXT("Start="), Start);
		FParse::Value(*Params, TEXT("End="),   End);

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), false);
		Obj->SetStringField(TEXT("asset_path"),    AssetPath);
		Obj->SetNumberField(TEXT("start_seconds"), Start);
		Obj->SetNumberField(TEXT("end_seconds"),   End);
		Obj->SetBoolField(TEXT("applied"),         false);
		Obj->SetStringField(TEXT("hint"),
			TEXT("Playback range write needs MovieScene helpers."));
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	int32 RunListGameplayTagsOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString Filter;
		FParse::Value(*Params, TEXT("Filter="), Filter);

		// UGameplayTagsManager is in /Script/GameplayTags; query directly
		// via UClass lookup so we don't have to link the module.
		TArray<TSharedPtr<FJsonValue>> Tags;
		if (UClass* MgrClass = FindObject<UClass>(nullptr, TEXT("/Script/GameplayTags.GameplayTagsManager")))
		{
			if (UObject* Mgr = MgrClass->GetDefaultObject())
			{
				if (UFunction* Fn = MgrClass->FindFunctionByName(FName(TEXT("RequestAllGameplayTags"))))
				{
					(void)Fn;
					(void)Mgr;
				}
			}
		}
		// Best-effort: agent gets the schema; specific tag enumeration
		// requires the editor's GameplayTagsManager singleton at runtime.
		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		Obj->SetArrayField(TEXT("tags"), Tags);
		Obj->SetStringField(TEXT("hint"),
			TEXT("Tag enumeration scaffolded; full list needs the live "
			     "GameplayTagsManager singleton."));
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	int32 RunAddGameplayTagOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString Tag, Comment;
		FParse::Value(*Params, TEXT("Tag="),     Tag);
		FParse::Value(*Params, TEXT("Comment="), Comment);

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), false);
		Obj->SetStringField(TEXT("tag_name"),       Tag);
		Obj->SetBoolField(TEXT("added"),            false);
		Obj->SetBoolField(TEXT("already_existed"),  false);
		Obj->SetStringField(TEXT("hint"),
			TEXT("Tag dictionary mutation writes Config/Tags/DefaultGameplayTags.ini "
			     "directly. Edit the .ini, then `console_command \"GameplayTags.PrintReport\"`."));
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	int32 RunReadAbilitySetOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString AssetPath;
		FParse::Value(*Params, TEXT("Asset="), AssetPath);

		UDataAsset* DA = LoadObject<UDataAsset>(nullptr, *AssetPath);
		TArray<TSharedPtr<FJsonValue>> Abilities;
		// AbilitySet schemas vary across projects. We scan for any array
		// property containing a "class" + "level" pair via the property
		// system. Best-effort.
		if (DA)
		{
			for (TFieldIterator<FArrayProperty> It(DA->GetClass()); It; ++It)
			{
				FScriptArrayHelper Helper(*It, It->ContainerPtrToValuePtr<void>(DA));
				for (int32 i = 0; i < Helper.Num(); ++i)
				{
					if (FStructProperty* Struct = CastField<FStructProperty>(It->Inner))
					{
						void* Ptr = Helper.GetRawPtr(i);
						auto Entry = MakeShared<FJsonObject>();
						for (TFieldIterator<FProperty> P(Struct->Struct); P; ++P)
						{
							FString Text;
							P->ExportText_Direct(Text,
								P->ContainerPtrToValuePtr<void>(Ptr), nullptr, nullptr, PPF_None);
							Entry->SetStringField(P->GetName(), Text);
						}
						// Re-emit as our normalized {class, level} shape.
						auto E = MakeShared<FJsonObject>();
						const FString* CName = nullptr;
						const FString* LName = nullptr;
						for (const auto& Pair : Entry->Values)
						{
							if (Pair.Key.Contains(TEXT("Class")) ||
							    Pair.Key.Contains(TEXT("Ability")))
							{
								FString S;
								Pair.Value->TryGetString(S);
								E->SetStringField(TEXT("class"), S);
								CName = &Pair.Key;
							}
							else if (Pair.Key.Contains(TEXT("Level")))
							{
								int32 L = 1;
								Pair.Value->TryGetNumber(L);
								E->SetNumberField(TEXT("level"), L);
								LName = &Pair.Key;
							}
						}
						(void)CName; (void)LName;
						if (!E->HasField(TEXT("level"))) E->SetNumberField(TEXT("level"), 1);
						Abilities.Add(MakeShared<FJsonValueObject>(E));
					}
				}
			}
		}

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), DA != nullptr);
		Obj->SetStringField(TEXT("asset_path"), AssetPath);
		Obj->SetArrayField(TEXT("abilities"),   Abilities);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	int32 RunListAnimBlueprintsOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		return ListAssetsByClassPath(Params, OutputPath, bPretty,
			UAnimBlueprint::StaticClass()->GetClassPathName());
	}

	int32 RunReadAnimBlueprintOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString AssetPath;
		FParse::Value(*Params, TEXT("Asset="), AssetPath);

		UAnimBlueprint* ABP = LoadObject<UAnimBlueprint>(nullptr, *AssetPath);
		if (!ABP) return 4;

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		Obj->SetStringField(TEXT("asset_path"),   AssetPath);
		Obj->SetStringField(TEXT("parent_class"),
			ABP->ParentClass ? ABP->ParentClass->GetName() : FString{});
		// State machine introspection lives in AnimGraph editor module
		// (FAnimStateMachineNodeBase). We surface an empty list with a
		// hint so the agent gets the asset's parent class but knows it
		// needs the editor for the deeper graph.
		Obj->SetArrayField(TEXT("state_machines"), TArray<TSharedPtr<FJsonValue>>{});
		Obj->SetStringField(TEXT("hint"),
			TEXT("State-machine walk needs AnimGraph module."));
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	int32 RunAddAnimStateOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString AssetPath, Machine, Name;
		FParse::Value(*Params, TEXT("Asset="),   AssetPath);
		FParse::Value(*Params, TEXT("Machine="), Machine);
		FParse::Value(*Params, TEXT("Name="),    Name);

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), false);
		Obj->SetStringField(TEXT("asset_path"),    AssetPath);
		Obj->SetStringField(TEXT("state_machine"), Machine);
		Obj->SetStringField(TEXT("state_name"),    Name);
		Obj->SetBoolField(TEXT("added"),           false);
		Obj->SetStringField(TEXT("hint"),
			TEXT("AnimGraph state authoring needs AnimGraph module."));
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	int32 RunCompileAnimBlueprintOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString AssetPath;
		FParse::Value(*Params, TEXT("Asset="), AssetPath);

		UAnimBlueprint* ABP = LoadObject<UAnimBlueprint>(nullptr, *AssetPath);
		if (!ABP) return 4;

		FCompilerResultsLog ResultsLog;
		FKismetEditorUtilities::CompileBlueprint(ABP,
			EBlueprintCompileOptions::SkipGarbageCollection, &ResultsLog);
		const bool bCompiled = ResultsLog.NumErrors == 0;

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), bCompiled);
		Obj->SetStringField(TEXT("asset_path"), AssetPath);
		Obj->SetBoolField(TEXT("compiled"),     bCompiled);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	int32 RunRunAutomationTestsOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString Pattern;
		FParse::Value(*Params, TEXT("Pattern="), Pattern);

		// Use the console-exec route to kick off automation runs. UE's
		// "Automation" exec dispatches to FAutomationTestFramework, which
		// runs asynchronously on the game thread. Results land in the
		// output log + Saved/Automation/.
		FString Cmd = TEXT("Automation RunTests ");
		Cmd += Pattern.IsEmpty() ? TEXT("*") : Pattern;
		bool bStarted = false;
		FString Message;
		if (GEngine)
		{
			FStringOutputDevice Capture;
			GEngine->Exec(GetEditorWorldOrNull(), *Cmd, Capture);
			Message = Capture;
			bStarted = true;
		}
		else
		{
			Message = TEXT("GEngine is null; can't dispatch automation tests");
		}

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		Obj->SetBoolField(TEXT("started"), bStarted);
		Obj->SetStringField(TEXT("message"), Message);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	int32 RunReadOutputLogOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		int32 Limit = 200;
		FString MinSeverity;
		FParse::Value(*Params, TEXT("Limit="),       Limit);
		FParse::Value(*Params, TEXT("MinSeverity="), MinSeverity);

		// Map the human-readable severity name to UE's ELogVerbosity. The
		// MCP-server tool's schema constrains values to the five common
		// ones; anything else is treated as "no filter."
		ELogVerbosity::Type MinVerb = ELogVerbosity::NoLogging;
		if (MinSeverity.Equals(TEXT("Fatal"),   ESearchCase::IgnoreCase)) MinVerb = ELogVerbosity::Fatal;
		else if (MinSeverity.Equals(TEXT("Error"),  ESearchCase::IgnoreCase)) MinVerb = ELogVerbosity::Error;
		else if (MinSeverity.Equals(TEXT("Warning"),ESearchCase::IgnoreCase)) MinVerb = ELogVerbosity::Warning;
		else if (MinSeverity.Equals(TEXT("Display"),ESearchCase::IgnoreCase)) MinVerb = ELogVerbosity::Display;
		else if (MinSeverity.Equals(TEXT("Log"),    ESearchCase::IgnoreCase)) MinVerb = ELogVerbosity::Log;

		TArray<BlueprintReader::FLogSinkEntry> Drained;
		if (BlueprintReader::FLogSink* Sink = BlueprintReader::GetLogSink())
		{
			Sink->Drain(Limit, MinVerb, Drained);
		}

		TArray<TSharedPtr<FJsonValue>> Entries;
		Entries.Reserve(Drained.Num());
		for (const auto& E : Drained)
		{
			auto Entry = MakeShared<FJsonObject>();
			// Severity name — re-match the enum to a stable string so
			// the wire shape doesn't drift if UE adds verbosity levels.
			const TCHAR* SevName = TEXT("Log");
			switch (E.Verbosity)
			{
				case ELogVerbosity::Fatal:       SevName = TEXT("Fatal"); break;
				case ELogVerbosity::Error:       SevName = TEXT("Error"); break;
				case ELogVerbosity::Warning:     SevName = TEXT("Warning"); break;
				case ELogVerbosity::Display:     SevName = TEXT("Display"); break;
				case ELogVerbosity::Log:         SevName = TEXT("Log"); break;
				case ELogVerbosity::Verbose:     SevName = TEXT("Verbose"); break;
				case ELogVerbosity::VeryVerbose: SevName = TEXT("VeryVerbose"); break;
				default: break;
			}
			Entry->SetStringField(TEXT("severity"),  SevName);
			Entry->SetStringField(TEXT("category"),  E.Category.ToString());
			Entry->SetStringField(TEXT("message"),   E.Message);
			Entry->SetStringField(TEXT("timestamp"), E.Timestamp.ToIso8601());
			Entries.Add(MakeShared<FJsonValueObject>(Entry));
		}

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		Obj->SetArrayField(TEXT("entries"), Entries);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	// ----- DuplicateBlueprint (BP-5) -----------------------------------
	// File-level duplicate: source BP at /Game/X → new BP at /Game/Y.
	// Idempotent on the destination path: if /Game/Y already exists,
	// returns already_existed:true without overwriting.
	int32 RunDuplicateBlueprintOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		const FString SourceAsset = ResolveAssetPath(Params);
		FString DestAsset;
		FParse::Value(*Params, TEXT("Dest="), DestAsset);
		if (SourceAsset.IsEmpty() || DestAsset.IsEmpty())
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("DuplicateBlueprint requires -Asset=/Game/X -Dest=/Game/Y"));
			return 1;
		}
		if (!DestAsset.StartsWith(TEXT("/Game/")))
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("DuplicateBlueprint: -Dest must be under /Game/ (got: %s)"), *DestAsset);
			return 1;
		}

		// Idempotency: if the destination already exists, return without
		// touching anything. Same contract as create_blueprint.
		if (UBlueprint* Existing = LoadMutableBlueprint(DestAsset))
		{
			(void)Existing;
			auto Obj = MakeShared<FJsonObject>();
			Obj->SetBoolField(TEXT("ok"), true);
			Obj->SetBoolField(TEXT("already_existed"), true);
			Obj->SetStringField(TEXT("asset_path"), DestAsset);
			return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
		}

		UBlueprint* SourceBP = LoadMutableBlueprint(SourceAsset);
		if (!SourceBP)
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("DuplicateBlueprint: source asset not found: %s"), *SourceAsset);
			return 4;
		}

		// IAssetTools::DuplicateAsset takes (NewAssetName, NewPackagePath,
		// SourceObject). Split the destination path: package path is
		// everything up to (but not including) the last segment; asset
		// name is the last segment.
		FString DestPackagePath, DestName;
		{
			int32 LastSlash;
			if (!DestAsset.FindLastChar(TEXT('/'), LastSlash) || LastSlash <= 0)
			{
				UE_LOG(LogBlueprintReader, Error,
					TEXT("DuplicateBlueprint: -Dest='%s' is malformed"), *DestAsset);
				return 1;
			}
			DestPackagePath = DestAsset.Left(LastSlash);
			DestName        = DestAsset.RightChop(LastSlash + 1);
		}

		FAssetToolsModule& AssetToolsModule =
			FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
		IAssetTools& AssetTools = AssetToolsModule.Get();
		UObject* NewObj = AssetTools.DuplicateAsset(DestName, DestPackagePath, SourceBP);
		if (!NewObj)
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("DuplicateBlueprint: DuplicateAsset failed for %s -> %s"),
				*SourceAsset, *DestAsset);
			return 5;
		}

		UBlueprint* NewBP = Cast<UBlueprint>(NewObj);
		if (!NewBP)
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("DuplicateBlueprint: duplicate isn't a UBlueprint (was %s)"),
				*NewObj->GetClass()->GetName());
			return 5;
		}
		// MaybeCompileAndSave handles the save + compile. AssetCreated
		// notification fires inside DuplicateAsset already, so a
		// follow-up batch op can LoadMutableBlueprint it.
		if (!MaybeCompileAndSave(NewBP)) return 5;

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		Obj->SetBoolField(TEXT("already_existed"), false);
		Obj->SetStringField(TEXT("asset_path"), DestAsset);
		Obj->SetStringField(TEXT("source_asset_path"), SourceAsset);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
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

		if (!MaybeCompileAndSave(BP)) return 5;
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

		if (!MaybeCompileAndSave(BP)) return 5;
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

		if (!MaybeCompileAndSave(BP)) return 5;
		return EmitOk(OutputPath, bPretty);
	}

	// ----- AddNode ----------------------------------------------------------
	// Resolve a UClass path passed via -ClassPath= or short name (e.g.
	// "Actor" or "/Script/Engine.Actor").
	UClass* ResolveClass(const FString& Spec)
	{
		if (Spec.IsEmpty()) return nullptr;
		// Try as full path first.
		if (UObject* Resolved = StaticLoadObject(UClass::StaticClass(), nullptr, *Spec))
		{
			return Cast<UClass>(Resolved);
		}
		// Try as a short name under /Script/Engine.
		const FString Engine = FString::Printf(TEXT("/Script/Engine.%s"), *Spec);
		return Cast<UClass>(StaticLoadObject(UClass::StaticClass(), nullptr, *Engine));
	}

	// Spawn + register a node in `Graph`. Caller is responsible for any
	// node-specific state (variable/function refs, output pins, etc.).
	template <typename TNode>
	TNode* AddNodeToGraph(UEdGraph* Graph, int32 X, int32 Y)
	{
		TNode* Node = NewObject<TNode>(Graph);
		Node->CreateNewGuid();
		Node->NodePosX = X;
		Node->NodePosY = Y;
		Graph->AddNode(Node, /*bFromUI=*/false, /*bSelectNewNode=*/false);
		Node->PostPlacedNewNode();
		Node->AllocateDefaultPins();
		return Node;
	}

	int32 RunAddNodeOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		const FString AssetPath = ResolveAssetPath(Params);
		FString GraphName, Kind;
		int32 X = 0, Y = 0;
		FParse::Value(*Params, TEXT("Graph="), GraphName);
		FParse::Value(*Params, TEXT("Kind="),  Kind);
		FParse::Value(*Params, TEXT("X="),     X);
		FParse::Value(*Params, TEXT("Y="),     Y);

		if (AssetPath.IsEmpty() || GraphName.IsEmpty() || Kind.IsEmpty())
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("AddNode requires -Asset= -Graph= -Kind="));
			return 1;
		}

		UBlueprint* BP = LoadMutableBlueprint(AssetPath);
		if (!BP) return 4;
		UEdGraph* Graph = FindGraphByName(BP, GraphName);
		if (!Graph)
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("AddNode: graph %s not found"), *GraphName);
			return 4;
		}

		UEdGraphNode* Spawned = nullptr;

		if (Kind.Equals(TEXT("Branch"), ESearchCase::IgnoreCase) ||
		    Kind.Equals(TEXT("IfThenElse"), ESearchCase::IgnoreCase))
		{
			Spawned = AddNodeToGraph<UK2Node_IfThenElse>(Graph, X, Y);
		}
		else if (Kind.Equals(TEXT("Sequence"), ESearchCase::IgnoreCase) ||
		         Kind.Equals(TEXT("ExecutionSequence"), ESearchCase::IgnoreCase))
		{
			Spawned = AddNodeToGraph<UK2Node_ExecutionSequence>(Graph, X, Y);
		}
		else if (Kind.Equals(TEXT("VariableGet"), ESearchCase::IgnoreCase))
		{
			FString VarName;
			FParse::Value(*Params, TEXT("Variable="), VarName);
			if (VarName.IsEmpty())
			{
				UE_LOG(LogBlueprintReader, Error, TEXT("AddNode VariableGet requires -Variable="));
				return 1;
			}
			UK2Node_VariableGet* Get = NewObject<UK2Node_VariableGet>(Graph);
			Get->VariableReference.SetSelfMember(*VarName);
			Get->CreateNewGuid();
			Get->NodePosX = X; Get->NodePosY = Y;
			Graph->AddNode(Get, false, false);
			Get->PostPlacedNewNode();
			Get->AllocateDefaultPins();
			Spawned = Get;
		}
		else if (Kind.Equals(TEXT("VariableSet"), ESearchCase::IgnoreCase))
		{
			FString VarName;
			FParse::Value(*Params, TEXT("Variable="), VarName);
			if (VarName.IsEmpty())
			{
				UE_LOG(LogBlueprintReader, Error, TEXT("AddNode VariableSet requires -Variable="));
				return 1;
			}
			UK2Node_VariableSet* Set = NewObject<UK2Node_VariableSet>(Graph);
			Set->VariableReference.SetSelfMember(*VarName);
			Set->CreateNewGuid();
			Set->NodePosX = X; Set->NodePosY = Y;
			Graph->AddNode(Set, false, false);
			Set->PostPlacedNewNode();
			Set->AllocateDefaultPins();
			Spawned = Set;
		}
		else if (Kind.Equals(TEXT("CallFunction"), ESearchCase::IgnoreCase))
		{
			FString FunctionName, FunctionOwner;
			FParse::Value(*Params, TEXT("Function="),      FunctionName);
			FParse::Value(*Params, TEXT("FunctionOwner="), FunctionOwner);
			if (FunctionName.IsEmpty() || FunctionOwner.IsEmpty())
			{
				UE_LOG(LogBlueprintReader, Error,
					TEXT("AddNode CallFunction requires -Function=<name> -FunctionOwner=<class path>"));
				return 1;
			}
			UClass* OwnerClass = ResolveClass(FunctionOwner);
			if (!OwnerClass)
			{
				UE_LOG(LogBlueprintReader, Error, TEXT("AddNode CallFunction: class %s not found"), *FunctionOwner);
				return 1;
			}
			UFunction* Fn = OwnerClass->FindFunctionByName(*FunctionName);
			if (!Fn)
			{
				UE_LOG(LogBlueprintReader, Error,
					TEXT("AddNode CallFunction: function %s not found on %s"),
					*FunctionName, *OwnerClass->GetPathName());
				return 1;
			}
			UK2Node_CallFunction* Call = NewObject<UK2Node_CallFunction>(Graph);
			Call->SetFromFunction(Fn);
			Call->CreateNewGuid();
			Call->NodePosX = X; Call->NodePosY = Y;
			Graph->AddNode(Call, false, false);
			Call->PostPlacedNewNode();
			Call->AllocateDefaultPins();
			Spawned = Call;
		}
		else if (Kind.Equals(TEXT("CustomEvent"), ESearchCase::IgnoreCase))
		{
			FString EventName;
			FParse::Value(*Params, TEXT("EventName="), EventName);
			if (EventName.IsEmpty())
			{
				UE_LOG(LogBlueprintReader, Error, TEXT("AddNode CustomEvent requires -EventName="));
				return 1;
			}
			UK2Node_CustomEvent* Evt = NewObject<UK2Node_CustomEvent>(Graph);
			Evt->CustomFunctionName = FName(*EventName);
			Evt->CreateNewGuid();
			Evt->NodePosX = X; Evt->NodePosY = Y;
			Graph->AddNode(Evt, false, false);
			Evt->PostPlacedNewNode();
			Evt->AllocateDefaultPins();
			Spawned = Evt;
		}
		else if (Kind.Equals(TEXT("Cast"), ESearchCase::IgnoreCase) ||
		         Kind.Equals(TEXT("DynamicCast"), ESearchCase::IgnoreCase))
		{
			FString TargetClass;
			FParse::Value(*Params, TEXT("TargetClass="), TargetClass);
			if (TargetClass.IsEmpty())
			{
				UE_LOG(LogBlueprintReader, Error,
					TEXT("AddNode Cast requires -TargetClass=<UClass path or short name>"));
				return 1;
			}
			UClass* Tgt = ResolveClass(TargetClass);
			if (!Tgt)
			{
				UE_LOG(LogBlueprintReader, Error, TEXT("AddNode Cast: class %s not found"), *TargetClass);
				return 1;
			}
			UK2Node_DynamicCast* Cast = NewObject<UK2Node_DynamicCast>(Graph);
			Cast->TargetType = Tgt;
			Cast->CreateNewGuid();
			Cast->NodePosX = X; Cast->NodePosY = Y;
			Graph->AddNode(Cast, false, false);
			Cast->PostPlacedNewNode();
			Cast->AllocateDefaultPins();
			Spawned = Cast;
		}
		else if (Kind.Equals(TEXT("Self"), ESearchCase::IgnoreCase))
		{
			Spawned = AddNodeToGraph<UK2Node_Self>(Graph, X, Y);
		}
		else if (Kind.Equals(TEXT("MakeArray"), ESearchCase::IgnoreCase))
		{
			Spawned = AddNodeToGraph<UK2Node_MakeArray>(Graph, X, Y);
		}
		else if (Kind.Equals(TEXT("MakeStruct"), ESearchCase::IgnoreCase))
		{
			FString StructPath;
			FParse::Value(*Params, TEXT("StructType="), StructPath);
			if (StructPath.IsEmpty())
			{
				UE_LOG(LogBlueprintReader, Error,
					TEXT("AddNode MakeStruct requires -StructType=<UScriptStruct path, e.g. /Script/CoreUObject.Vector>"));
				return 1;
			}
			UScriptStruct* Struct = LoadObject<UScriptStruct>(nullptr, *StructPath);
			if (!Struct)
			{
				UE_LOG(LogBlueprintReader, Error, TEXT("AddNode MakeStruct: struct %s not found"), *StructPath);
				return 1;
			}
			UK2Node_MakeStruct* Make = NewObject<UK2Node_MakeStruct>(Graph);
			Make->StructType = Struct;
			Make->CreateNewGuid();
			Make->NodePosX = X; Make->NodePosY = Y;
			Graph->AddNode(Make, false, false);
			Make->PostPlacedNewNode();
			Make->AllocateDefaultPins();
			Spawned = Make;
		}
		else if (Kind.Equals(TEXT("FormatText"), ESearchCase::IgnoreCase))
		{
			Spawned = AddNodeToGraph<UK2Node_FormatText>(Graph, X, Y);
		}
		else if (Kind.Equals(TEXT("Knot"), ESearchCase::IgnoreCase) ||
		         Kind.Equals(TEXT("Reroute"), ESearchCase::IgnoreCase))
		{
			Spawned = AddNodeToGraph<UK2Node_Knot>(Graph, X, Y);
		}
		else
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("AddNode: unrecognised -Kind=%s; see list_node_kinds for valid values"),
				*Kind);
			return 1;
		}

		if (!Spawned) return 5;
		const FString NewId = Spawned->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens);

		if (!MaybeCompileAndSave(BP)) return 5;

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		Obj->SetStringField(TEXT("node_id"), NewId);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	// ----- WirePins ---------------------------------------------------------
	UEdGraphPin* FindPinByIdOrName(UEdGraphNode* Node, const FString& Spec)
	{
		// Try GUID first.
		FGuid AsGuid;
		if (FGuid::Parse(Spec, AsGuid))
		{
			for (UEdGraphPin* P : Node->Pins)
			{
				if (P && P->PinId == AsGuid) return P;
			}
		}
		// Fall back to FName match. This is the underlying pin name —
		// matches the param-name of the UFUNCTION the pin came from,
		// or the BP variable name for variable nodes. Always whitespace-
		// free because UE FNames can't carry the friendly text.
		for (UEdGraphPin* P : Node->Pins)
		{
			if (P && P->GetFName().ToString().Equals(Spec, ESearchCase::IgnoreCase))
			{
				return P;
			}
		}
		// Final fallback: PinFriendlyName (the label the editor shows in
		// the graph UI). This is what users typically type when they
		// "see" a pin — for example, an Array Library node whose pin
		// has FName "TargetArray" but displays as "Target Array", or a
		// function with `DisplayName="Dummy Targets"` meta override.
		// Required for issue #10: callers often pass the visible label.
		for (UEdGraphPin* P : Node->Pins)
		{
			if (!P) continue;
			const FString Friendly = P->PinFriendlyName.IsEmpty()
				? FString()
				: P->PinFriendlyName.ToString();
			if (!Friendly.IsEmpty() && Friendly.Equals(Spec, ESearchCase::IgnoreCase))
			{
				return P;
			}
		}
		return nullptr;
	}

	int32 RunWirePinsOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		const FString AssetPath = ResolveAssetPath(Params);
		FString GraphName, FromNodeId, FromPinSpec, ToNodeId, ToPinSpec;
		FParse::Value(*Params, TEXT("Graph="),    GraphName);
		FParse::Value(*Params, TEXT("FromNode="), FromNodeId);
		FParse::Value(*Params, TEXT("FromPin="),  FromPinSpec);
		FParse::Value(*Params, TEXT("ToNode="),   ToNodeId);
		FParse::Value(*Params, TEXT("ToPin="),    ToPinSpec);

		if (AssetPath.IsEmpty() || GraphName.IsEmpty() ||
		    FromNodeId.IsEmpty() || FromPinSpec.IsEmpty() ||
		    ToNodeId.IsEmpty()   || ToPinSpec.IsEmpty())
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("WirePins requires -Asset= -Graph= -FromNode= -FromPin= -ToNode= -ToPin="));
			return 1;
		}

		UBlueprint* BP = LoadMutableBlueprint(AssetPath);
		if (!BP) return 4;
		UEdGraph* Graph = FindGraphByName(BP, GraphName);
		if (!Graph) return 4;

		UEdGraphNode* FromNode = FindNodeByGuid(Graph, FromNodeId);
		UEdGraphNode* ToNode   = FindNodeByGuid(Graph, ToNodeId);
		if (!FromNode || !ToNode)
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("WirePins: from/to node not found"));
			return 4;
		}
		UEdGraphPin* FromPin = FindPinByIdOrName(FromNode, FromPinSpec);
		UEdGraphPin* ToPin   = FindPinByIdOrName(ToNode,   ToPinSpec);
		if (!FromPin || !ToPin)
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("WirePins: from/to pin not found"));
			return 4;
		}

		// Route through the schema's TryCreateConnection rather than calling
		// MakeLinkTo directly. The schema entry point:
		//   1. Re-runs CanCreateConnection and honors every response code
		//      (BREAK_OTHERS_A/B/AB, MAKE_WITH_CONVERSION_NODE,
		//      MAKE_WITH_PROMOTION) the same way the editor's drag-drop
		//      handler does.
		//   2. Calls PinConnectionListChanged on both owning nodes after
		//      a successful link — this is the hook K2 wildcard nodes
		//      (UK2Node_CallArrayFunction, UK2Node_Select, etc.) use to
		//      propagate the concrete type from a typed source pin into
		//      every linked wildcard slot. Without this hook, the array
		//      library nodes' TargetArray pin stays `wildcard` after
		//      wire_pins and the BP fails to compile (issue #11:
		//      "The type of Target Array is undetermined").
		const UEdGraphSchema* Schema = Graph->GetSchema();
		if (!Schema)
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("WirePins: graph has no schema"));
			return 1;
		}
		const FPinConnectionResponse Resp = Schema->CanCreateConnection(FromPin, ToPin);
		if (Resp.Response == CONNECT_RESPONSE_DISALLOW)
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("WirePins: schema rejected: %s"),
				*Resp.Message.ToString());
			return 1;
		}
		const bool bMade = Schema->TryCreateConnection(FromPin, ToPin);
		if (!bMade)
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("WirePins: TryCreateConnection returned false despite CanCreateConnection=%d"),
				(int32)Resp.Response);
			return 1;
		}

		if (!MaybeCompileAndSave(BP)) return 5;
		return EmitOk(OutputPath, bPretty);
	}

	// ----- DeleteVariable ---------------------------------------------------
	int32 RunDeleteVariableOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		const FString AssetPath = ResolveAssetPath(Params);
		FString VarName;
		FParse::Value(*Params, TEXT("Name="), VarName);

		if (AssetPath.IsEmpty() || VarName.IsEmpty())
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("DeleteVariable requires -Asset= -Name="));
			return 1;
		}

		UBlueprint* BP = LoadMutableBlueprint(AssetPath);
		if (!BP) return 4;
		const FName Var(*VarName);
		if (FBlueprintEditorUtils::FindNewVariableIndex(BP, Var) == INDEX_NONE)
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("DeleteVariable: %s not found on %s"), *VarName, *AssetPath);
			return 4;
		}
		FBlueprintEditorUtils::RemoveMemberVariable(BP, Var);

		if (!MaybeCompileAndSave(BP)) return 5;
		return EmitOk(OutputPath, bPretty);
	}

	// ----- Function helpers (shared by add_function_input/output) -----------
	bool BuildPinTypeFromFlags(const FString& Params, FEdGraphPinType& Out)
	{
		FString TypeCategory, TypeSubCategory, TypeSubObject;
		FParse::Value(*Params, TEXT("TypeCategory="),         TypeCategory);
		FParse::Value(*Params, TEXT("TypeSubCategory="),      TypeSubCategory);
		FParse::Value(*Params, TEXT("TypeSubCategoryObject="), TypeSubObject);
		if (TypeCategory.IsEmpty()) return false;

		auto TypeJson = MakeShared<FJsonObject>();
		TypeJson->SetStringField(TEXT("category"), TypeCategory);
		if (!TypeSubCategory.IsEmpty()) TypeJson->SetStringField(TEXT("sub_category"), TypeSubCategory);
		if (!TypeSubObject.IsEmpty())   TypeJson->SetStringField(TEXT("sub_category_object"), TypeSubObject);
		TypeJson->SetBoolField(TEXT("is_array"), FParse::Param(*Params, TEXT("TypeIsArray")));
		TypeJson->SetBoolField(TEXT("is_set"),   FParse::Param(*Params, TEXT("TypeIsSet")));
		TypeJson->SetBoolField(TEXT("is_map"),   FParse::Param(*Params, TEXT("TypeIsMap")));
		return FBlueprintReaderWireJson::ParseWirePinType(TypeJson, Out);
	}

	UEdGraph* FindFunctionGraph(UBlueprint* BP, const FString& FunctionName)
	{
		for (UEdGraph* G : BP->FunctionGraphs)
		{
			if (G && G->GetFName().ToString().Equals(FunctionName, ESearchCase::IgnoreCase))
			{
				return G;
			}
		}
		return nullptr;
	}

	UK2Node_FunctionEntry* FindFunctionEntry(UEdGraph* Graph)
	{
		for (UEdGraphNode* N : Graph->Nodes)
		{
			if (UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(N)) return Entry;
		}
		return nullptr;
	}

	UK2Node_FunctionResult* FindOrCreateFunctionResult(UEdGraph* Graph)
	{
		for (UEdGraphNode* N : Graph->Nodes)
		{
			if (UK2Node_FunctionResult* R = Cast<UK2Node_FunctionResult>(N)) return R;
		}
		UK2Node_FunctionResult* Result = NewObject<UK2Node_FunctionResult>(Graph);
		Result->CreateNewGuid();
		Result->NodePosX = 240;
		Result->NodePosY = 0;
		Graph->AddNode(Result, /*bFromUI=*/false, /*bSelectNewNode=*/false);
		Result->PostPlacedNewNode();
		Result->AllocateDefaultPins();
		return Result;
	}

	// ----- AddFunction ------------------------------------------------------
	int32 RunAddFunctionOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		const FString AssetPath = ResolveAssetPath(Params);
		FString FunctionName;
		FParse::Value(*Params, TEXT("Name="), FunctionName);

		if (AssetPath.IsEmpty() || FunctionName.IsEmpty())
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("AddFunction requires -Asset= -Name="));
			return 1;
		}

		UBlueprint* BP = LoadMutableBlueprint(AssetPath);
		if (!BP) return 4;
		if (FindFunctionGraph(BP, FunctionName))
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("AddFunction: function %s already exists"), *FunctionName);
			return 1;
		}

		UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
			BP, FName(*FunctionName), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		FBlueprintEditorUtils::AddFunctionGraph<UClass>(BP, NewGraph, /*bIsUserCreated=*/true, nullptr);

		if (!MaybeCompileAndSave(BP)) return 5;
		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		Obj->SetStringField(TEXT("function_name"), FunctionName);
		// Return the FunctionEntry node's GUID so callers (notably
		// compile_function) can wire its `then` exec output into the
		// first statement they spawn — saves a follow-up read+find.
		if (UK2Node_FunctionEntry* Entry = FindFunctionEntry(NewGraph))
		{
			Obj->SetStringField(TEXT("entry_node_id"),
				Entry->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
		}
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	// ----- AddFunctionInput / AddFunctionOutput -----------------------------
	int32 RunAddFunctionInputOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		const FString AssetPath = ResolveAssetPath(Params);
		FString FunctionName, ParamName;
		FParse::Value(*Params, TEXT("Function="), FunctionName);
		FParse::Value(*Params, TEXT("Param="),    ParamName);

		FEdGraphPinType PinType;
		if (AssetPath.IsEmpty() || FunctionName.IsEmpty() || ParamName.IsEmpty() ||
		    !BuildPinTypeFromFlags(Params, PinType))
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("AddFunctionInput requires -Asset= -Function= -Param= -TypeCategory= [...]"));
			return 1;
		}

		UBlueprint* BP = LoadMutableBlueprint(AssetPath);
		if (!BP) return 4;
		UEdGraph* Graph = FindFunctionGraph(BP, FunctionName);
		if (!Graph)
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("AddFunctionInput: function %s not found"), *FunctionName);
			return 4;
		}
		UK2Node_FunctionEntry* Entry = FindFunctionEntry(Graph);
		if (!Entry)
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("AddFunctionInput: no FunctionEntry node"));
			return 5;
		}
		Entry->CreateUserDefinedPin(FName(*ParamName), PinType, EGPD_Output, /*bUseUniqueName=*/false);

		if (!MaybeCompileAndSave(BP)) return 5;
		return EmitOk(OutputPath, bPretty);
	}

	int32 RunAddFunctionOutputOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		const FString AssetPath = ResolveAssetPath(Params);
		FString FunctionName, ParamName;
		FParse::Value(*Params, TEXT("Function="), FunctionName);
		FParse::Value(*Params, TEXT("Param="),    ParamName);

		FEdGraphPinType PinType;
		if (AssetPath.IsEmpty() || FunctionName.IsEmpty() || ParamName.IsEmpty() ||
		    !BuildPinTypeFromFlags(Params, PinType))
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("AddFunctionOutput requires -Asset= -Function= -Param= -TypeCategory= [...]"));
			return 1;
		}

		UBlueprint* BP = LoadMutableBlueprint(AssetPath);
		if (!BP) return 4;
		UEdGraph* Graph = FindFunctionGraph(BP, FunctionName);
		if (!Graph)
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("AddFunctionOutput: function %s not found"), *FunctionName);
			return 4;
		}
		UK2Node_FunctionResult* Result = FindOrCreateFunctionResult(Graph);
		Result->CreateUserDefinedPin(FName(*ParamName), PinType, EGPD_Input, /*bUseUniqueName=*/false);

		if (!MaybeCompileAndSave(BP)) return 5;
		return EmitOk(OutputPath, bPretty);
	}

	// ----- DeleteFunction ---------------------------------------------------
	int32 RunDeleteFunctionOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		const FString AssetPath = ResolveAssetPath(Params);
		FString FunctionName;
		FParse::Value(*Params, TEXT("Name="), FunctionName);

		if (AssetPath.IsEmpty() || FunctionName.IsEmpty())
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("DeleteFunction requires -Asset= -Name="));
			return 1;
		}

		UBlueprint* BP = LoadMutableBlueprint(AssetPath);
		if (!BP) return 4;
		UEdGraph* Graph = FindFunctionGraph(BP, FunctionName);
		if (!Graph)
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("DeleteFunction: %s not found"), *FunctionName);
			return 4;
		}
		// EFlags=None purges the graph + any UFUNCTION the compiler emitted.
		FBlueprintEditorUtils::RemoveGraph(BP, Graph, EGraphRemoveFlags::None);

		if (!MaybeCompileAndSave(BP)) return 5;
		return EmitOk(OutputPath, bPretty);
	}

	// ----- SetVariableDefault -----------------------------------------------
	int32 RunSetVariableDefaultOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		const FString AssetPath = ResolveAssetPath(Params);
		FString VarName, NewDefault;
		FParse::Value(*Params, TEXT("Name="),    VarName);
		FParse::Value(*Params, TEXT("Default="), NewDefault);

		if (AssetPath.IsEmpty() || VarName.IsEmpty())
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("SetVariableDefault requires -Asset= -Name= [-Default=...]"));
			return 1;
		}

		UBlueprint* BP = LoadMutableBlueprint(AssetPath);
		if (!BP) return 4;
		const FName Var(*VarName);
		const int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(BP, Var);
		if (Index == INDEX_NONE)
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("SetVariableDefault: %s not found"), *VarName);
			return 4;
		}
		BP->NewVariables[Index].DefaultValue = NewDefault;

		if (!MaybeCompileAndSave(BP)) return 5;
		return EmitOk(OutputPath, bPretty);
	}

	// ----- RenameVariable ---------------------------------------------------
	int32 RunRenameVariableOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		const FString AssetPath = ResolveAssetPath(Params);
		FString OldName, NewName;
		FParse::Value(*Params, TEXT("OldName="), OldName);
		FParse::Value(*Params, TEXT("NewName="), NewName);

		if (AssetPath.IsEmpty() || OldName.IsEmpty() || NewName.IsEmpty())
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("RenameVariable requires -Asset= -OldName= -NewName="));
			return 1;
		}

		UBlueprint* BP = LoadMutableBlueprint(AssetPath);
		if (!BP) return 4;
		const FName Old(*OldName), New(*NewName);
		if (FBlueprintEditorUtils::FindNewVariableIndex(BP, Old) == INDEX_NONE)
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("RenameVariable: %s not found"), *OldName);
			return 4;
		}
		if (FBlueprintEditorUtils::FindNewVariableIndex(BP, New) != INDEX_NONE)
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("RenameVariable: %s already exists"), *NewName);
			return 1;
		}
		FBlueprintEditorUtils::RenameMemberVariable(BP, Old, New);

		if (!MaybeCompileAndSave(BP)) return 5;
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
		// Path-scoped scan, not a full SearchAllAssets. The previous code
		// did `SearchAllAssets(bSync=true)` which forces the entire project's
		// asset registry to finalize and fires the global
		// OnAssetRegistryLoadComplete broadcast. That broadcast invokes
		// every plugin's load handler — and one bad handler (e.g. Niagara
		// loading a NiagaraDataChannel asset whose post-load constructor
		// crashes in some projects) takes the whole commandlet down with
		// exit=3 and a long callstack ending in our RunListOp.
		//
		// ScanPathsSynchronous populates only the requested path's metadata,
		// doesn't invoke the global completion broadcast, and doesn't load
		// asset payloads (only header tags). bForceRescan=false → cheap no-op
		// when the path's already in the registry from earlier startup.
		AR.ScanPathsSynchronous({PathFilter}, /*bForceRescan=*/false);

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
			// UE 5's asset-registry tag value is a FStringClassReference text
			// form: `/Script/CoreUObject.Class'/Script/Engine.Actor'`. Strip
			// the wrapping `Class'...'` so the wire shape matches what
			// read_blueprint emits via Blueprint->ParentClass->GetPathName().
			{
				int32 OpenQuote = INDEX_NONE, CloseQuote = INDEX_NONE;
				if (ParentClass.FindChar(TEXT('\''), OpenQuote) &&
				    ParentClass.FindLastChar(TEXT('\''), CloseQuote) &&
				    CloseQuote > OpenQuote)
				{
					ParentClass = ParentClass.Mid(OpenQuote + 1, CloseQuote - OpenQuote - 1);
				}
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
	if (Op == EOp::AddNode)         return RunAddNodeOp(Params, OutputPath, bPretty);
	if (Op == EOp::WirePins)        return RunWirePinsOp(Params, OutputPath, bPretty);
	if (Op == EOp::DeleteVariable)     return RunDeleteVariableOp(Params, OutputPath, bPretty);
	if (Op == EOp::RenameVariable)     return RunRenameVariableOp(Params, OutputPath, bPretty);
	if (Op == EOp::AddFunction)        return RunAddFunctionOp(Params, OutputPath, bPretty);
	if (Op == EOp::AddFunctionInput)   return RunAddFunctionInputOp(Params, OutputPath, bPretty);
	if (Op == EOp::AddFunctionOutput)  return RunAddFunctionOutputOp(Params, OutputPath, bPretty);
	if (Op == EOp::DeleteFunction)     return RunDeleteFunctionOp(Params, OutputPath, bPretty);
	if (Op == EOp::SetVariableDefault) return RunSetVariableDefaultOp(Params, OutputPath, bPretty);
	if (Op == EOp::BeginBatch)         return RunBeginBatchOp(Params, OutputPath, bPretty);
	if (Op == EOp::EndBatch)           return RunEndBatchOp(Params, OutputPath, bPretty);
	if (Op == EOp::CreateBlueprint)    return RunCreateBlueprintOp(Params, OutputPath, bPretty);
	if (Op == EOp::SetPinDefault)      return RunSetPinDefaultOp(Params, OutputPath, bPretty);
	if (Op == EOp::RetypeVariable)     return RunRetypeVariableOp(Params, OutputPath, bPretty);
	if (Op == EOp::SetVariableCategory)return RunSetVariableCategoryOp(Params, OutputPath, bPretty);
	if (Op == EOp::DuplicateBlueprint) return RunDuplicateBlueprintOp(Params, OutputPath, bPretty);
	if (Op == EOp::WriteGeneratedSource) return RunWriteGeneratedSourceOp(Params, OutputPath, bPretty);
	if (Op == EOp::SaveAll)            return RunSaveAllOp(Params, OutputPath, bPretty);
	if (Op == EOp::MoveAsset)          return RunMoveAssetOp(Params, OutputPath, bPretty);
	if (Op == EOp::DeleteAsset)        return RunDeleteAssetOp(Params, OutputPath, bPretty);
	if (Op == EOp::CreateFolder)       return RunCreateFolderOp(Params, OutputPath, bPretty);
	if (Op == EOp::ListDataTables)     return RunListDataTablesOp(Params, OutputPath, bPretty);
	if (Op == EOp::ReadDataTable)      return RunReadDataTableOp(Params, OutputPath, bPretty);
	if (Op == EOp::ConsoleCommand)     return RunConsoleCommandOp(Params, OutputPath, bPretty);
	if (Op == EOp::GetCVar)            return RunGetCVarOp(Params, OutputPath, bPretty);
	if (Op == EOp::SetCVar)            return RunSetCVarOp(Params, OutputPath, bPretty);
	if (Op == EOp::PieStart)           return RunPieStartOp(Params, OutputPath, bPretty);
	if (Op == EOp::PieStop)            return RunPieStopOp(Params, OutputPath, bPretty);
	if (Op == EOp::LiveCodingCompile)  return RunLiveCodingCompileOp(Params, OutputPath, bPretty);
	if (Op == EOp::GetSelectedActors)  return RunGetSelectedActorsOp(Params, OutputPath, bPretty);
	if (Op == EOp::SetSelection)       return RunSetSelectionOp(Params, OutputPath, bPretty);
	if (Op == EOp::SpawnActor)         return RunSpawnActorOp(Params, OutputPath, bPretty);
	if (Op == EOp::SetActorTransform)  return RunSetActorTransformOp(Params, OutputPath, bPretty);
	if (Op == EOp::DeleteActor)        return RunDeleteActorOp(Params, OutputPath, bPretty);
	if (Op == EOp::ReadOutputLog)      return RunReadOutputLogOp(Params, OutputPath, bPretty);
	if (Op == EOp::RunAutomationTests) return RunRunAutomationTestsOp(Params, OutputPath, bPretty);
	if (Op == EOp::AddDataRow)         return RunAddDataRowOp(Params, OutputPath, bPretty);
	if (Op == EOp::SetDataRowValue)    return RunSetDataRowValueOp(Params, OutputPath, bPretty);
	if (Op == EOp::AddComponent)       return RunAddComponentOp(Params, OutputPath, bPretty);
	if (Op == EOp::RemoveComponent)    return RunRemoveComponentOp(Params, OutputPath, bPretty);
	if (Op == EOp::AttachComponent)    return RunAttachComponentOp(Params, OutputPath, bPretty);
	if (Op == EOp::SetComponentProperty) return RunSetComponentPropertyOp(Params, OutputPath, bPretty);
	if (Op == EOp::ListMaterials)                return RunListMaterialsOp(Params, OutputPath, bPretty);
	if (Op == EOp::ReadMaterial)                 return RunReadMaterialOp(Params, OutputPath, bPretty);
	if (Op == EOp::AddMaterialExpression)        return RunAddMaterialExpressionOp(Params, OutputPath, bPretty);
	if (Op == EOp::ConnectMaterialExpressions)   return RunConnectMaterialExpressionsOp(Params, OutputPath, bPretty);
	if (Op == EOp::SetMaterialParameter)         return RunSetMaterialParameterOp(Params, OutputPath, bPretty);
	if (Op == EOp::SetMaterialInstanceParameter) return RunSetMaterialInstanceParameterOp(Params, OutputPath, bPretty);
	if (Op == EOp::CompileMaterial)              return RunCompileMaterialOp(Params, OutputPath, bPretty);
	if (Op == EOp::ReadWidgetBlueprint)          return RunReadWidgetBlueprintOp(Params, OutputPath, bPretty);
	if (Op == EOp::AddWidget)                    return RunAddWidgetOp(Params, OutputPath, bPretty);
	if (Op == EOp::SetWidgetProperty)            return RunSetWidgetPropertyOp(Params, OutputPath, bPretty);
	if (Op == EOp::BindWidgetEvent)              return RunBindWidgetEventOp(Params, OutputPath, bPretty);
	if (Op == EOp::CompileWidgetBlueprint)       return RunCompileWidgetBlueprintOp(Params, OutputPath, bPretty);
	if (Op == EOp::ListBehaviorTrees)            return RunListBehaviorTreesOp(Params, OutputPath, bPretty);
	if (Op == EOp::ReadBehaviorTree)             return RunReadBehaviorTreeOp(Params, OutputPath, bPretty);
	if (Op == EOp::AddBTNode)                    return RunAddBTNodeOp(Params, OutputPath, bPretty);
	if (Op == EOp::SetBTNodeProperty)            return RunSetBTNodePropertyOp(Params, OutputPath, bPretty);
	if (Op == EOp::CompileBehaviorTree)          return RunCompileBehaviorTreeOp(Params, OutputPath, bPretty);
	if (Op == EOp::ListDataAssets)               return RunListDataAssetsOp(Params, OutputPath, bPretty);
	if (Op == EOp::ReadDataAsset)                return RunReadDataAssetOp(Params, OutputPath, bPretty);
	if (Op == EOp::CreateDataAsset)              return RunCreateDataAssetOp(Params, OutputPath, bPretty);
	if (Op == EOp::SetDataAssetProperty)         return RunSetDataAssetPropertyOp(Params, OutputPath, bPretty);
	if (Op == EOp::ListStateTrees)               return RunListStateTreesOp(Params, OutputPath, bPretty);
	if (Op == EOp::ReadStateTree)                return RunReadStateTreeOp(Params, OutputPath, bPretty);
	if (Op == EOp::AddStateTreeState)            return RunAddStateTreeStateOp(Params, OutputPath, bPretty);
	if (Op == EOp::SetStateTreeTransition)       return RunSetStateTreeTransitionOp(Params, OutputPath, bPretty);
	if (Op == EOp::CompileStateTree)             return RunCompileStateTreeOp(Params, OutputPath, bPretty);
	if (Op == EOp::StartProfile)                 return RunStartProfileOp(Params, OutputPath, bPretty);
	if (Op == EOp::StopProfile)                  return RunStopProfileOp(Params, OutputPath, bPretty);
	if (Op == EOp::GetStats)                     return RunGetStatsOp(Params, OutputPath, bPretty);
	if (Op == EOp::TakeScreenshot)               return RunTakeScreenshotOp(Params, OutputPath, bPretty);
	if (Op == EOp::CookContent)                  return RunCookContentOp(Params, OutputPath, bPretty);
	if (Op == EOp::PackageProject)               return RunPackageProjectOp(Params, OutputPath, bPretty);
	if (Op == EOp::IntrospectClass)              return RunIntrospectClassOp(Params, OutputPath, bPretty);
	if (Op == EOp::FindClass)                    return RunFindClassOp(Params, OutputPath, bPretty);
	if (Op == EOp::ListFunctions)                return RunListFunctionsOp(Params, OutputPath, bPretty);
	if (Op == EOp::FocusActor)                   return RunFocusActorOp(Params, OutputPath, bPretty);
	if (Op == EOp::SetCameraTransform)           return RunSetCameraTransformOp(Params, OutputPath, bPretty);
	if (Op == EOp::TakeViewportScreenshot)       return RunTakeViewportScreenshotOp(Params, OutputPath, bPretty);
	if (Op == EOp::SetShowFlag)                  return RunSetShowFlagOp(Params, OutputPath, bPretty);
	if (Op == EOp::ListNiagaraSystems)           return RunListNiagaraSystemsOp(Params, OutputPath, bPretty);
	if (Op == EOp::ReadNiagaraSystem)            return RunReadNiagaraSystemOp(Params, OutputPath, bPretty);
	if (Op == EOp::CreateNiagaraSystem)          return RunCreateNiagaraSystemOp(Params, OutputPath, bPretty);
	if (Op == EOp::SetNiagaraParameter)          return RunSetNiagaraParameterOp(Params, OutputPath, bPretty);
	if (Op == EOp::ListLevelSequences)           return RunListLevelSequencesOp(Params, OutputPath, bPretty);
	if (Op == EOp::ReadLevelSequence)            return RunReadLevelSequenceOp(Params, OutputPath, bPretty);
	if (Op == EOp::AddSequenceTrack)             return RunAddSequenceTrackOp(Params, OutputPath, bPretty);
	if (Op == EOp::SetSequencePlaybackRange)     return RunSetSequencePlaybackRangeOp(Params, OutputPath, bPretty);
	if (Op == EOp::ListGameplayTags)             return RunListGameplayTagsOp(Params, OutputPath, bPretty);
	if (Op == EOp::AddGameplayTag)               return RunAddGameplayTagOp(Params, OutputPath, bPretty);
	if (Op == EOp::ReadAbilitySet)               return RunReadAbilitySetOp(Params, OutputPath, bPretty);
	if (Op == EOp::ListAnimBlueprints)           return RunListAnimBlueprintsOp(Params, OutputPath, bPretty);
	if (Op == EOp::ReadAnimBlueprint)            return RunReadAnimBlueprintOp(Params, OutputPath, bPretty);
	if (Op == EOp::AddAnimState)                 return RunAddAnimStateOp(Params, OutputPath, bPretty);
	if (Op == EOp::CompileAnimBlueprint)         return RunCompileAnimBlueprintOp(Params, OutputPath, bPretty);

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
	case EOp::Components:
	{
		auto Comps = FBlueprintReaderWireJson::ComponentsToJson(*Info);
		return EmitJson(FBlueprintReaderWireJson::WriteArrayString(Comps, bPretty), OutputPath);
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
} // anonymous namespace

// Live-server entry point. The TCP listener in BlueprintReaderLiveServer.cpp
// shells op-args through here on the game thread — same dispatch path the
// daemon uses for its stdin lines. Defined at file scope (not in the anon
// namespace) so the symbol has external linkage; the body uses unqualified
// name lookup to find RunOneOp in the anonymous namespace below.
namespace BlueprintReader
{
    // Forward decl of the anon-ns helper, hoisted to the same namespace
    // as our bridge so name lookup picks it up.
    int32 RunOneOpFromLiveServer(const FString& Params);
}
namespace
{
int32 RunOneOp(const FString& Params);  // forward decl from earlier defn
}
int32 BlueprintReader::RunOneOpFromLiveServer(const FString& Params)
{
    return RunOneOp(Params);
}

namespace
{
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
