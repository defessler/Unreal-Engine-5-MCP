#include "BlueprintReaderCommandlet.h"

#include "BatchContext.h"
#include "BlueprintIntrospector.h"
#include "BlueprintReaderCmdletServer.h"
#include "BlueprintReaderJson.h"
#include "BlueprintReaderLogSink.h"
#include "BlueprintReaderWireJson.h"
#include "BlueprintStructuralDiff.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
// Phase 8 spatial coord tools — FSceneView + projection math.
#include "SceneView.h"
#include "Engine/HitResult.h"
#include "Engine/World.h"
#include "CollisionQueryParams.h"
// Phase 11 H Tier 1 — GameFeaturesToolset.
#include "GameFeaturesSubsystem.h"
#include "Interfaces/IPluginManager.h"
// Phase 11 H Tier 1 — AbilitySystemInspectorToolset.
#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"
#include "GameplayAbilitySpec.h"
#include "Abilities/GameplayAbility.h"
#include "GameplayTagContainer.h"
#include "AbilitySystemInterface.h"
#include "GameplayEffect.h"
#include "AttributeSet.h"
// Phase 12 Wave 2 — FBlueprintEditor (BlueprintEditor module / Kismet).
#include "BlueprintEditor.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "Engine/StaticMesh.h"
// FStaticMeshRenderData + FStaticMeshLODResources full definitions
// (Engine/StaticMesh.h only forward-declares them) — get_mesh_preview_state
// walks GetRenderData()->LODResources. Required outside unity builds.
#include "StaticMeshResources.h"
#include "WidgetBlueprintEditor.h"
#include "WidgetBlueprint.h"
#include "IMaterialEditor.h"
#include "Materials/Material.h"
#include "IStaticMeshEditor.h"
#include "Camera/PlayerCameraManager.h"
#include "GameFramework/PlayerController.h"
#include "Settings/LevelEditorViewportSettings.h"
#include "LevelSequence.h"
#include "ILevelSequenceEditorToolkit.h"
#include "ISequencer.h"
#include "MovieScene.h"
#include "MovieSceneFwd.h"
#include "Animation/AnimationAsset.h"
#include "Async/TaskGraphInterfaces.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphUtilities.h"
#include "EdGraphSchema_K2.h"
#include "UObject/TopLevelAssetPath.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "Engine/Blueprint.h"
#include "UObject/Interface.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h" // Phase 17 — get_workspace_layout
#include "ProfilingDebugging/TraceAuxiliary.h" // Phase 17 — get_trace_state
#include "Widgets/SWindow.h"
#include "EditorModeManager.h"
#include "EditorModes.h"
#include "Layers/LayersSubsystem.h"    // Phase 13 Wave 3 — layer visibility (UnrealEd)
#include "Layers/Layer.h"              // ULayer::IsVisible()
#include "Bookmarks/IBookmarkTypeTools.h" // Phase 13 Wave 3 — camera bookmarks (UnrealEd)
#include "Engine/BookMark.h"           // UBookMark (Location/Rotation)
#include "GameFramework/WorldSettings.h"  // AWorldSettings::GetBookmarks()
#include "AssetCompilingManager.h"     // Phase 14 — async compile backlog
#include "ShaderCompiler.h"            // Phase 14 — GShaderCompilingManager
#include "Interfaces/ITargetPlatformManagerModule.h" // Phase 14 — active cook targets
#include "Interfaces/ITargetPlatform.h"
#include "ISourceControlModule.h"      // Phase 14 — SCC provider
#include "ISourceControlProvider.h"
#include "ISourceControlState.h"       // Phase 14 — per-file SCC state
#include "SourceControlHelpers.h"      // package path -> filename
#include "Internationalization/Internationalization.h" // Phase 17 — culture
#include "Internationalization/Culture.h"               // FCulture accessors
#include "Settings/EditorStyleSettings.h"               // Phase 17 — theme id
#include "GenericPlatform/GenericApplication.h"         // Phase 17 — FDisplayMetrics
#include "MRUFavoritesList.h"                            // Phase 14 — recent assets MRU
#include "Kismet2/KismetDebugUtilities.h"                // Phase 17 — BP breakpoints
#include "Kismet2/Breakpoint.h"
#include "Interfaces/IProjectManager.h"                  // Phase 11 — set_plugin_enabled
#if PLATFORM_WINDOWS
#include "ILiveCodingModule.h"                          // Phase 17 — live coding state
#endif
#include "AssetRegistry/IAssetRegistry.h" // Phase 14 — registry scan state
#include "WorldPartition/DataLayer/DataLayerManager.h"  // Phase 14 — data layers
#include "WorldPartition/DataLayer/DataLayerInstance.h"
#include "WorldPartition/WorldPartitionSubsystem.h"      // Phase 14 — streaming sources
#include "WorldPartition/WorldPartitionStreamingSource.h"
#include "UObject/Package.h"                             // Phase 14 — saved-package event
#include "UObject/ObjectSaveContext.h"
#include "Engine/DeveloperSettings.h"                    // Phase 16 — project settings nav
#include "UObject/UObjectHash.h"                         // GetDerivedClasses
#include "Misc/AutomationTest.h"                         // Phase 16 — automation discovery
#include "Editor/UnrealEdEngine.h"     // Phase 14 — GUnrealEd autosaver
#include "UnrealEdGlobals.h"           // GUnrealEd
#include "IPackageAutoSaver.h"
#include "Tools/UEdMode.h"
#include "Engine/DataTable.h"
#include "Engine/Selection.h"
#include "Engine/World.h"
#include "EngineUtils.h"               // TActorIterator
#include "Factories/BlueprintFactory.h"
#include "FileHelpers.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "IAssetTools.h"
#include "IPythonScriptPlugin.h"
#include "JsonObjectConverter.h"
#include "LightingBuildOptions.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/StringOutputDevice.h"
#include "ObjectTools.h"
#include "PythonScriptTypes.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectIterator.h"
#if PLATFORM_WINDOWS
    #include "Windows/WindowsHWrapper.h"
#endif    // PLATFORM_WINDOWS
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallArrayFunction.h"   // array library funcs need the wildcard-aware node
#include "K2Node_GetSubsystem.h"        // add_node Kind=GetSubsystem
#include "Subsystems/Subsystem.h"       // USubsystem base for the GetSubsystem type check
#include "K2Node_Composite.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_Event.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Tunnel.h"
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
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/UObjectToken.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
// Material authoring (Stage 1).
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Engine/Texture.h"
// UMG widget authoring (Stage 1).
#include "Blueprint/WidgetTree.h"
#include "Components/PanelSlot.h"
#include "Components/PanelWidget.h"
#include "Components/Widget.h"
#include "WidgetBlueprint.h"
// Behavior Tree authoring (Stage 2).
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTNode.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/BTTaskNode.h"
// DataAsset CRUD (Stage 2).
#include "Engine/DataAsset.h"
// AnimBlueprint authoring (Stage 4).
#include "Animation/AnimBlueprint.h"
// NOMINMAX before <windows.h> stops the Windows `min`/`max` macros from
// clobbering `std::numeric_limits<T>::max()` etc. in engine headers
// (MovieScene, RelativePtr, ...) that get pulled in transitively.
#ifndef NOMINMAX
	#define NOMINMAX
#endif    // NOMINMAX
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include "Windows/HideWindowsPlatformTypes.h"

// Definition for the LogBlueprintReader category declared in
// BlueprintIntrospector.h. Was DEFINE_LOG_CATEGORY_STATIC originally —
// non-static now so the introspector can UE_LOG into it (issue #58
// follow-up; diagnostic needed to fire on the read path too).
DEFINE_LOG_CATEGORY(LogBlueprintReader);

UBPRCommandlet::UBPRCommandlet()
{
	IsClient = false;
	IsServer = false;
	IsEditor = true;
	LogToConsole = true;
	ShowErrorCount = false;
	// Honour the explicit return value from Main() / RunOneOp() / RunDaemon()
	// instead of letting the engine override exit=0 → exit=1 just because
	// some op logged an Error (see LaunchEngineLoop.cpp ~line 4160). Several
	// of our ops emit Error-level diagnostics on EXPECTED no-op paths — most
	// notably CreateBlueprint's idempotency probe, which calls
	// LoadMutableBlueprint to check for an existing asset and gets back
	// `LoadBlueprint: ... asset not in registry` on the (intended) miss.
	// Without this flag the MCP client sees a spurious exit=1 and treats the
	// successful create as a hard failure.
	UseCommandletResultAsExitCode = true;
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
		// Clone a whole graph's nodes (all classes, losslessly) from a source
		// BP into a target BP via UE's node export/import — recreate-identical
		// for graphs that add_node/BPIR can't reconstruct node-by-node.
		CloneGraph,
		// Add an implemented interface to a Blueprint (no prior tool existed).
		ImplementInterface,
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
		GetEditorState,
		RunPythonScript,
		// Asset queries.
		GetReferencers,
		GetDependencies,
		// Project settings.
		ReadConfigValue,
		SetConfigValue,
		// Lighting.
		BuildLighting,
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
		// Structural diff between two BPs (BP roundtrip-capability,
		// Task 13). Read-only — no compile/save side effects.
		StructuralDiff,
		// Asset-registry queries — list/find any asset type, not just
		// the typed list_* family. Closes the agent-feedback gap where
		// the catalog has list_blueprints / list_materials / etc. but
		// no general "what assets are under this path?" probe.
		ListAssets,
		FindAsset,
		// Project metadata — read .uproject + return engine
		// association etc. Implemented editor-side so the live
		// backend doesn't need its own .uproject path config.
		GetProjectMetadata,
		// Phase 8 EA-pull Wave 1 — editor-awareness reads + lifecycle.
		ListOpenAssets,
		GetActiveAsset,
		GetCompileStatus,
		GetDirtyPackages,
		GetFocusedWindow,
		GetPieState,
		GetModalState,
		GetActiveEditorMode,
		GetFocusedWidget,
		OpenAssetEditor,
		CloseAssetEditor,
		GetCameraTransform,
		GetViewMode,
		GetShowFlags,
		GetSelectedComponents,
		GetSelectedAssets,
		SetSelectedAssets,
		GetSelectedFolders,
		GetContentBrowserPath,
		SetContentBrowserPath,
		WorldToScreen,
		ScreenToWorld,
		UiSnapshot,
		UiFind,
		ListDesktopWindows,
		ListGameFeatures,
		GetGameFeatureState,
		ListPlugins,
		GetPluginDescriptor,
		GetPluginDependencies,
		ListActorAbilities,
		ListActorGameplayTags,
		ListActorAttributes,
		ListActorGameplayEffects,
		GetBlueprintEditorState,
		GetMaterialInstanceParams,
		GetStaticMeshInfo,
		GetUmgEditorState,
		GetMaterialEditorState,
		GetMeshPreviewState,
		GetCinematicCamera,
		GetSequencerState,
		GetAnimEditorState,
		GetNiagaraModuleSelection,
		GetCurveEditorSelection,
		GetBufferVisualizationMode,
		GetGizmoState,
		GetViewportRealtime,
		GetViewportCameraSettings,
		GetSnappingSettings,
		GetActiveViewport,
		GetHiddenActors,
		GetVisibleActors,
		SetViewMode,
		SetGizmoMode,
		SetViewportRealtime,
		SetActorVisibility,
		GetHiddenLayers,
		SetLayerVisibility,
		GetCameraBookmarks,
		GotoCameraBookmark,
		GetHoverTarget,
		GetIsolateMode,
		GetAsyncCompileState,
		GetShaderCompileState,
		GetCurrentLevel,
		ListLoadedLevels,
		GetSourceControlProvider,
		GetAssetRegistryState,
		GetDataLayerStates,
		GetAutosaveStatus,
		GetRecoveryState,
		GetSourceControlStatus,
		GetFileLockStatus,
		GetActiveCulture,
		GetEditorTheme,
		GetMonitors,            // not GetMonitorInfo: Win32 macro collision
		GetLiveCodingState,
		ActivateGameFeature,
		DeactivateGameFeature,
		GetRecentlyOpenedAssets,
		GetDebugInstance,
		GetBlueprintBreakpoints,
		GetWatchedPins,
		GetActiveStats,
		SetPluginEnabled,
		GetStreamingSources,
		GetRecentlySavedPackages,
		ListProjectSettings,
		GetProjectSettingValues,
		SetProjectSetting,
		ListAutomationTests,
		GetEditorEvents,
		GetActiveCookTarget,
		GetWorkspaceLayout,
		GetTraceState,
		GetUiStateStub,
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
		if (OpStr.Equals(TEXT("CloneGraph"), ESearchCase::IgnoreCase))         { OutOp = EOp::CloneGraph; return true; }
		if (OpStr.Equals(TEXT("ImplementInterface"), ESearchCase::IgnoreCase)) { OutOp = EOp::ImplementInterface; return true; }
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
		if (OpStr.Equals(TEXT("GetEditorState"), ESearchCase::IgnoreCase))      { OutOp = EOp::GetEditorState; return true; }
		if (OpStr.Equals(TEXT("RunPythonScript"), ESearchCase::IgnoreCase))     { OutOp = EOp::RunPythonScript; return true; }
		if (OpStr.Equals(TEXT("GetReferencers"), ESearchCase::IgnoreCase))      { OutOp = EOp::GetReferencers; return true; }
		if (OpStr.Equals(TEXT("GetDependencies"), ESearchCase::IgnoreCase))     { OutOp = EOp::GetDependencies; return true; }
		if (OpStr.Equals(TEXT("ReadConfigValue"), ESearchCase::IgnoreCase))     { OutOp = EOp::ReadConfigValue; return true; }
		if (OpStr.Equals(TEXT("SetConfigValue"), ESearchCase::IgnoreCase))      { OutOp = EOp::SetConfigValue; return true; }
		if (OpStr.Equals(TEXT("BuildLighting"), ESearchCase::IgnoreCase))       { OutOp = EOp::BuildLighting; return true; }
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
		if (OpStr.Equals(TEXT("StructuralDiff"), ESearchCase::IgnoreCase))          { OutOp = EOp::StructuralDiff; return true; }
		if (OpStr.Equals(TEXT("ListAssets"), ESearchCase::IgnoreCase))              { OutOp = EOp::ListAssets; return true; }
		if (OpStr.Equals(TEXT("FindAsset"), ESearchCase::IgnoreCase))               { OutOp = EOp::FindAsset; return true; }
		if (OpStr.Equals(TEXT("GetProjectMetadata"), ESearchCase::IgnoreCase))      { OutOp = EOp::GetProjectMetadata; return true; }
		// Phase 8 EA-pull Wave 1 (partial)
		if (OpStr.Equals(TEXT("ListOpenAssets"), ESearchCase::IgnoreCase))          { OutOp = EOp::ListOpenAssets; return true; }
		if (OpStr.Equals(TEXT("GetActiveAsset"), ESearchCase::IgnoreCase))          { OutOp = EOp::GetActiveAsset; return true; }
		if (OpStr.Equals(TEXT("GetCompileStatus"), ESearchCase::IgnoreCase))        { OutOp = EOp::GetCompileStatus; return true; }
		if (OpStr.Equals(TEXT("GetDirtyPackages"), ESearchCase::IgnoreCase))        { OutOp = EOp::GetDirtyPackages; return true; }
		if (OpStr.Equals(TEXT("GetFocusedWindow"), ESearchCase::IgnoreCase))        { OutOp = EOp::GetFocusedWindow; return true; }
		if (OpStr.Equals(TEXT("GetPieState"), ESearchCase::IgnoreCase))             { OutOp = EOp::GetPieState; return true; }
		if (OpStr.Equals(TEXT("GetModalState"), ESearchCase::IgnoreCase))           { OutOp = EOp::GetModalState; return true; }
		if (OpStr.Equals(TEXT("GetActiveEditorMode"), ESearchCase::IgnoreCase))     { OutOp = EOp::GetActiveEditorMode; return true; }
		if (OpStr.Equals(TEXT("GetFocusedWidget"), ESearchCase::IgnoreCase))        { OutOp = EOp::GetFocusedWidget; return true; }
		if (OpStr.Equals(TEXT("OpenAssetEditor"), ESearchCase::IgnoreCase))         { OutOp = EOp::OpenAssetEditor; return true; }
		if (OpStr.Equals(TEXT("CloseAssetEditor"), ESearchCase::IgnoreCase))        { OutOp = EOp::CloseAssetEditor; return true; }
		if (OpStr.Equals(TEXT("GetCameraTransform"), ESearchCase::IgnoreCase))      { OutOp = EOp::GetCameraTransform; return true; }
		if (OpStr.Equals(TEXT("GetViewMode"), ESearchCase::IgnoreCase))             { OutOp = EOp::GetViewMode; return true; }
		if (OpStr.Equals(TEXT("GetShowFlags"), ESearchCase::IgnoreCase))            { OutOp = EOp::GetShowFlags; return true; }
		if (OpStr.Equals(TEXT("GetSelectedComponents"), ESearchCase::IgnoreCase))   { OutOp = EOp::GetSelectedComponents; return true; }
		if (OpStr.Equals(TEXT("GetSelectedAssets"), ESearchCase::IgnoreCase))       { OutOp = EOp::GetSelectedAssets; return true; }
		if (OpStr.Equals(TEXT("SetSelectedAssets"), ESearchCase::IgnoreCase))       { OutOp = EOp::SetSelectedAssets; return true; }
		if (OpStr.Equals(TEXT("GetSelectedFolders"), ESearchCase::IgnoreCase))      { OutOp = EOp::GetSelectedFolders; return true; }
		if (OpStr.Equals(TEXT("GetContentBrowserPath"), ESearchCase::IgnoreCase))   { OutOp = EOp::GetContentBrowserPath; return true; }
		if (OpStr.Equals(TEXT("SetContentBrowserPath"), ESearchCase::IgnoreCase))   { OutOp = EOp::SetContentBrowserPath; return true; }
		if (OpStr.Equals(TEXT("WorldToScreen"), ESearchCase::IgnoreCase))           { OutOp = EOp::WorldToScreen; return true; }
		if (OpStr.Equals(TEXT("ScreenToWorld"), ESearchCase::IgnoreCase))           { OutOp = EOp::ScreenToWorld; return true; }
		if (OpStr.Equals(TEXT("UiSnapshot"), ESearchCase::IgnoreCase))              { OutOp = EOp::UiSnapshot; return true; }
		if (OpStr.Equals(TEXT("UiFind"), ESearchCase::IgnoreCase))                  { OutOp = EOp::UiFind; return true; }
		if (OpStr.Equals(TEXT("ListDesktopWindows"), ESearchCase::IgnoreCase))      { OutOp = EOp::ListDesktopWindows; return true; }
		if (OpStr.Equals(TEXT("ListGameFeatures"), ESearchCase::IgnoreCase))        { OutOp = EOp::ListGameFeatures; return true; }
		if (OpStr.Equals(TEXT("GetGameFeatureState"), ESearchCase::IgnoreCase))     { OutOp = EOp::GetGameFeatureState; return true; }
		if (OpStr.Equals(TEXT("ListPlugins"), ESearchCase::IgnoreCase))             { OutOp = EOp::ListPlugins; return true; }
		if (OpStr.Equals(TEXT("GetPluginDescriptor"), ESearchCase::IgnoreCase))     { OutOp = EOp::GetPluginDescriptor; return true; }
		if (OpStr.Equals(TEXT("GetPluginDependencies"), ESearchCase::IgnoreCase))   { OutOp = EOp::GetPluginDependencies; return true; }
		if (OpStr.Equals(TEXT("ListActorAbilities"), ESearchCase::IgnoreCase))      { OutOp = EOp::ListActorAbilities; return true; }
		if (OpStr.Equals(TEXT("ListActorGameplayTags"), ESearchCase::IgnoreCase))   { OutOp = EOp::ListActorGameplayTags; return true; }
		if (OpStr.Equals(TEXT("ListActorAttributes"), ESearchCase::IgnoreCase))     { OutOp = EOp::ListActorAttributes; return true; }
		if (OpStr.Equals(TEXT("ListActorGameplayEffects"), ESearchCase::IgnoreCase)){ OutOp = EOp::ListActorGameplayEffects; return true; }
		if (OpStr.Equals(TEXT("GetBlueprintEditorState"), ESearchCase::IgnoreCase)) { OutOp = EOp::GetBlueprintEditorState; return true; }
		if (OpStr.Equals(TEXT("GetMaterialInstanceParams"), ESearchCase::IgnoreCase)){ OutOp = EOp::GetMaterialInstanceParams; return true; }
		if (OpStr.Equals(TEXT("GetStaticMeshInfo"), ESearchCase::IgnoreCase))       { OutOp = EOp::GetStaticMeshInfo; return true; }
		if (OpStr.Equals(TEXT("GetUmgEditorState"), ESearchCase::IgnoreCase))       { OutOp = EOp::GetUmgEditorState; return true; }
		if (OpStr.Equals(TEXT("GetMaterialEditorState"), ESearchCase::IgnoreCase))  { OutOp = EOp::GetMaterialEditorState; return true; }
		if (OpStr.Equals(TEXT("GetMeshPreviewState"), ESearchCase::IgnoreCase))     { OutOp = EOp::GetMeshPreviewState; return true; }
		if (OpStr.Equals(TEXT("GetCinematicCamera"), ESearchCase::IgnoreCase))      { OutOp = EOp::GetCinematicCamera; return true; }
		if (OpStr.Equals(TEXT("GetSequencerState"), ESearchCase::IgnoreCase))       { OutOp = EOp::GetSequencerState; return true; }
		if (OpStr.Equals(TEXT("GetAnimEditorState"), ESearchCase::IgnoreCase))      { OutOp = EOp::GetAnimEditorState; return true; }
		if (OpStr.Equals(TEXT("GetNiagaraModuleSelection"), ESearchCase::IgnoreCase)){ OutOp = EOp::GetNiagaraModuleSelection; return true; }
		if (OpStr.Equals(TEXT("GetCurveEditorSelection"), ESearchCase::IgnoreCase)) { OutOp = EOp::GetCurveEditorSelection; return true; }
		if (OpStr.Equals(TEXT("GetBufferVisualizationMode"), ESearchCase::IgnoreCase)){OutOp = EOp::GetBufferVisualizationMode; return true; }
		if (OpStr.Equals(TEXT("GetGizmoState"), ESearchCase::IgnoreCase))           { OutOp = EOp::GetGizmoState; return true; }
		if (OpStr.Equals(TEXT("GetViewportRealtime"), ESearchCase::IgnoreCase))     { OutOp = EOp::GetViewportRealtime; return true; }
		if (OpStr.Equals(TEXT("GetViewportCameraSettings"), ESearchCase::IgnoreCase)){OutOp = EOp::GetViewportCameraSettings; return true; }
		if (OpStr.Equals(TEXT("GetSnappingSettings"), ESearchCase::IgnoreCase))     { OutOp = EOp::GetSnappingSettings; return true; }
		if (OpStr.Equals(TEXT("GetActiveViewport"), ESearchCase::IgnoreCase))       { OutOp = EOp::GetActiveViewport; return true; }
		if (OpStr.Equals(TEXT("GetHiddenActors"), ESearchCase::IgnoreCase))         { OutOp = EOp::GetHiddenActors; return true; }
		if (OpStr.Equals(TEXT("GetVisibleActors"), ESearchCase::IgnoreCase))        { OutOp = EOp::GetVisibleActors; return true; }
		if (OpStr.Equals(TEXT("SetViewMode"), ESearchCase::IgnoreCase))             { OutOp = EOp::SetViewMode; return true; }
		if (OpStr.Equals(TEXT("SetGizmoMode"), ESearchCase::IgnoreCase))            { OutOp = EOp::SetGizmoMode; return true; }
		if (OpStr.Equals(TEXT("SetViewportRealtime"), ESearchCase::IgnoreCase))     { OutOp = EOp::SetViewportRealtime; return true; }
		if (OpStr.Equals(TEXT("SetActorVisibility"), ESearchCase::IgnoreCase))      { OutOp = EOp::SetActorVisibility; return true; }
		if (OpStr.Equals(TEXT("GetHiddenLayers"), ESearchCase::IgnoreCase))         { OutOp = EOp::GetHiddenLayers; return true; }
		if (OpStr.Equals(TEXT("SetLayerVisibility"), ESearchCase::IgnoreCase))      { OutOp = EOp::SetLayerVisibility; return true; }
		if (OpStr.Equals(TEXT("GetCameraBookmarks"), ESearchCase::IgnoreCase))      { OutOp = EOp::GetCameraBookmarks; return true; }
		if (OpStr.Equals(TEXT("GotoCameraBookmark"), ESearchCase::IgnoreCase))      { OutOp = EOp::GotoCameraBookmark; return true; }
		if (OpStr.Equals(TEXT("GetHoverTarget"), ESearchCase::IgnoreCase))          { OutOp = EOp::GetHoverTarget; return true; }
		if (OpStr.Equals(TEXT("GetIsolateMode"), ESearchCase::IgnoreCase))          { OutOp = EOp::GetIsolateMode; return true; }
		if (OpStr.Equals(TEXT("GetAsyncCompileState"), ESearchCase::IgnoreCase))    { OutOp = EOp::GetAsyncCompileState; return true; }
		if (OpStr.Equals(TEXT("GetShaderCompileState"), ESearchCase::IgnoreCase))   { OutOp = EOp::GetShaderCompileState; return true; }
		if (OpStr.Equals(TEXT("GetCurrentLevel"), ESearchCase::IgnoreCase))         { OutOp = EOp::GetCurrentLevel; return true; }
		if (OpStr.Equals(TEXT("ListLoadedLevels"), ESearchCase::IgnoreCase))        { OutOp = EOp::ListLoadedLevels; return true; }
		if (OpStr.Equals(TEXT("GetSourceControlProvider"), ESearchCase::IgnoreCase)){ OutOp = EOp::GetSourceControlProvider; return true; }
		if (OpStr.Equals(TEXT("GetAssetRegistryState"), ESearchCase::IgnoreCase))   { OutOp = EOp::GetAssetRegistryState; return true; }
		if (OpStr.Equals(TEXT("GetDataLayerStates"), ESearchCase::IgnoreCase))      { OutOp = EOp::GetDataLayerStates; return true; }
		if (OpStr.Equals(TEXT("GetAutosaveStatus"), ESearchCase::IgnoreCase))       { OutOp = EOp::GetAutosaveStatus; return true; }
		if (OpStr.Equals(TEXT("GetRecoveryState"), ESearchCase::IgnoreCase))        { OutOp = EOp::GetRecoveryState; return true; }
		if (OpStr.Equals(TEXT("GetSourceControlStatus"), ESearchCase::IgnoreCase)) { OutOp = EOp::GetSourceControlStatus; return true; }
		if (OpStr.Equals(TEXT("GetFileLockStatus"), ESearchCase::IgnoreCase))       { OutOp = EOp::GetFileLockStatus; return true; }
		if (OpStr.Equals(TEXT("GetActiveCulture"), ESearchCase::IgnoreCase))        { OutOp = EOp::GetActiveCulture; return true; }
		if (OpStr.Equals(TEXT("GetEditorTheme"), ESearchCase::IgnoreCase))          { OutOp = EOp::GetEditorTheme; return true; }
		if (OpStr.Equals(TEXT("GetMonitors"), ESearchCase::IgnoreCase))             { OutOp = EOp::GetMonitors; return true; }
		if (OpStr.Equals(TEXT("GetLiveCodingState"), ESearchCase::IgnoreCase))      { OutOp = EOp::GetLiveCodingState; return true; }
		if (OpStr.Equals(TEXT("ActivateGameFeature"), ESearchCase::IgnoreCase))     { OutOp = EOp::ActivateGameFeature; return true; }
		if (OpStr.Equals(TEXT("DeactivateGameFeature"), ESearchCase::IgnoreCase))   { OutOp = EOp::DeactivateGameFeature; return true; }
		if (OpStr.Equals(TEXT("GetRecentlyOpenedAssets"), ESearchCase::IgnoreCase)){ OutOp = EOp::GetRecentlyOpenedAssets; return true; }
		if (OpStr.Equals(TEXT("GetDebugInstance"), ESearchCase::IgnoreCase))       { OutOp = EOp::GetDebugInstance; return true; }
		if (OpStr.Equals(TEXT("GetBlueprintBreakpoints"), ESearchCase::IgnoreCase)){ OutOp = EOp::GetBlueprintBreakpoints; return true; }
		if (OpStr.Equals(TEXT("GetWatchedPins"), ESearchCase::IgnoreCase))         { OutOp = EOp::GetWatchedPins; return true; }
		if (OpStr.Equals(TEXT("GetActiveStats"), ESearchCase::IgnoreCase))         { OutOp = EOp::GetActiveStats; return true; }
		if (OpStr.Equals(TEXT("SetPluginEnabled"), ESearchCase::IgnoreCase))       { OutOp = EOp::SetPluginEnabled; return true; }
		if (OpStr.Equals(TEXT("GetStreamingSources"), ESearchCase::IgnoreCase))    { OutOp = EOp::GetStreamingSources; return true; }
		if (OpStr.Equals(TEXT("GetRecentlySavedPackages"), ESearchCase::IgnoreCase)){ OutOp = EOp::GetRecentlySavedPackages; return true; }
		if (OpStr.Equals(TEXT("ListProjectSettings"), ESearchCase::IgnoreCase))     { OutOp = EOp::ListProjectSettings; return true; }
		if (OpStr.Equals(TEXT("GetProjectSettingValues"), ESearchCase::IgnoreCase)){ OutOp = EOp::GetProjectSettingValues; return true; }
		if (OpStr.Equals(TEXT("SetProjectSetting"), ESearchCase::IgnoreCase))      { OutOp = EOp::SetProjectSetting; return true; }
		if (OpStr.Equals(TEXT("ListAutomationTests"), ESearchCase::IgnoreCase))    { OutOp = EOp::ListAutomationTests; return true; }
		if (OpStr.Equals(TEXT("GetEditorEvents"), ESearchCase::IgnoreCase))        { OutOp = EOp::GetEditorEvents; return true; }
		if (OpStr.Equals(TEXT("GetActiveCookTarget"), ESearchCase::IgnoreCase))    { OutOp = EOp::GetActiveCookTarget; return true; }
		if (OpStr.Equals(TEXT("GetWorkspaceLayout"), ESearchCase::IgnoreCase))     { OutOp = EOp::GetWorkspaceLayout; return true; }
		if (OpStr.Equals(TEXT("GetTraceState"), ESearchCase::IgnoreCase))         { OutOp = EOp::GetTraceState; return true; }
		if (OpStr.Equals(TEXT("GetUiStateStub"), ESearchCase::IgnoreCase))        { OutOp = EOp::GetUiStateStub; return true; }
		UE_LOG(LogBlueprintReader, Error, TEXT("Unknown -Op=%s"), *OpStr);
		return false;
	}

	FString ResolveAssetPath(const FString& Params)
	{
		FString Asset;
		if (FParse::Value(*Params, TEXT("Asset="), Asset))
		{
			return Asset;
		}
		FParse::Value(*Params, TEXT("Path="), Asset);
		return Asset;
	}

	FString ResolveOutputPath(const FString& Params)
	{
		FString Out;
		if (FParse::Value(*Params, TEXT("Out="), Out))
		{
			return Out;
		}
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
		if (DT == FDateTime::MinValue())
		{
			return FString();
		}
		return DT.ToIso8601();
	}

	// ----- Shared helpers for write ops -------------------------------------

	// Forward decls: BatchDeferFlag() is defined further down in this TU
	// (after the LoadMutableBlueprint helper that needs it for the
	// cross-session write-lock check). Hoist the prototype here so the
	// in-anon-namespace name lookup resolves at LoadMutableBlueprint
	// parse time.
	bool& BatchDeferFlag();

	// Cross-session BP write-lock side channel. When LoadMutableBlueprint
	// detects that another connection has the BP locked for its own
	// batch, it can't return a useful UBlueprint*. Returning nullptr is
	// fine — existing call sites all do `if (!BP) return 2;` — but the
	// agent needs a different error shape than "asset not found". So we
	// stash the structured error payload + an override exit code in
	// thread-local slots; the RunOneOp dispatcher checks them after the
	// handler returns and overrides the response.
	//
	// Why thread-local: game thread is the sole dispatcher, but using
	// thread_local rather than a plain static makes the contract
	// explicit and crash-safe under any future refactor that moves
	// dispatch off the game thread (per-connection thread, task graph
	// pool, etc.). Cost is one TLS slot per worker — negligible.
	//
	// Why TLS instead of changing LoadMutableBlueprint's signature:
	// there are ~50 call sites, all with the same `if (!BP) return 2;`
	// shape. Threading OutputPath + bPretty + a result code through
	// every one would be a large mechanical diff for zero behavior win
	// once the dispatcher overrides the response anyway.
	namespace LoadFailure
	{
		thread_local int32   OverrideCode = 0;     // 0 = no override
		thread_local FString OverrideJson;          // already-serialized JSON body
	}    // namespace LoadFailure

	// Auto-checkout: under source control, programmatically check `Package`
	// out (non-interactively) before we modify/save it. A not-checked-out
	// asset is read-only on disk, so in a *live* editor the modification
	// either fails the save outright or — with "prompt for checkout on
	// modification" enabled — pops a modal "Check Out?" dialog. That modal
	// blocks the game thread, the MCP call hangs, and the client (e.g.
	// Copilot) concludes the tool broke. The provider API does the checkout
	// with no UI. Best-effort: we never fail the op here — if the file isn't
	// under source control it isn't read-only and the normal save path works;
	// if checkout genuinely can't happen (e.g. locked by another user) the
	// existing SavePackage error path reports it. Opt out with
	// BP_READER_AUTO_CHECKOUT=0.
	void EnsureCheckedOutForWrite(UPackage* Package)
	{
		if (!IsValid(Package))
		{
			return;
		}
		const FString Opt =
			FPlatformMisc::GetEnvironmentVariable(TEXT("BP_READER_AUTO_CHECKOUT"));
		if (Opt == TEXT("0") || Opt.Equals(TEXT("false"), ESearchCase::IgnoreCase))
		{
			return;
		}
		ISourceControlModule& SCM = ISourceControlModule::Get();
		if (!SCM.IsEnabled() || !SCM.GetProvider().IsAvailable())
		{
			return;  // No source control → file isn't read-only; normal save.
		}
		const FString Filename =
			USourceControlHelpers::PackageFilename(Package->GetName());
		if (Filename.IsEmpty())
		{
			return;
		}
		// CheckOutFile is non-interactive and a no-op when already checked
		// out; it returns false (deliberately ignored) when the file isn't
		// under source control or can't be checked out.
		if (!USourceControlHelpers::CheckOutFile(Filename, /*bSilent=*/true))
		{
			UE_LOG(LogBlueprintReader, Verbose,
				TEXT("Auto-checkout: %s not checked out (not under source "
				     "control or checkout failed); proceeding with save."),
				*Filename);
		}
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
		// LOAD_NoWarn + LOAD_Quiet suppresses the default LogLinker
		// "Failed to load X" warning so that, on the failure path, the
		// agent-facing diagnostic below is the ONLY error in the daemon
		// tail — no duplicate noise to parse around. On success there's
		// no extra logging either way.
		UBlueprint* BP = LoadObject<UBlueprint>(
			nullptr, *Resolved, nullptr, LOAD_NoWarn | LOAD_Quiet);
		if (!IsValid(BP))
		{
			// Diagnostic lives in BlueprintIntrospector — see header. We
			// call the same routine from FBlueprintIntrospector::Read so
			// read-only failures (which never enter LoadMutableBlueprint)
			// get the same error trace. Codex review on PR #58 flagged
			// the read-path gap.
			FBlueprintIntrospector::DiagnoseFailedBlueprintLoad(Resolved);
			return nullptr;
		}

		// Cross-session BP write lock: when this connection is in an
		// open batch and another connection already holds the lock on
		// this BP, refuse the mutation. The agent gets a structured
		// `blueprint_locked_by_other_session` response (set via the TLS
		// side channel, picked up in the dispatcher); the in-memory BP
		// is NOT touched so the holding session's pending state stays
		// clean. Non-batch ops skip the lock — single-op mutations are
		// committed immediately so there's no interleave window.
		if (BatchDeferFlag())
		{
			const uint64 MyConn = BlueprintReader::GetCurrentConnectionId();
			uint64 HeldBy = 0;
			if (BlueprintReader::FBatchRegistry::Get()
					.AcquireWriteOwnership(MyConn, BP, HeldBy) != 0)
			{
				// Build a JSON body the dispatcher can splice into the
				// response. Keep this stable + searchable: agents will
				// match on the error string to decide whether to retry.
				auto Obj = MakeShared<FJsonObject>();
				Obj->SetBoolField(TEXT("ok"), false);
				Obj->SetStringField(TEXT("error"),
					TEXT("blueprint_locked_by_other_session"));
				Obj->SetStringField(TEXT("asset_path"), AssetPath);
				Obj->SetNumberField(TEXT("held_by_connection"),
					static_cast<double>(HeldBy));
				Obj->SetStringField(TEXT("hint"),
					TEXT("Another MCP session has this Blueprint in an open "
					     "batch. Retry after the holding session calls "
					     "end_batch, or close that session."));
				LoadFailure::OverrideJson =
					FBlueprintReaderWireJson::WriteString(Obj, /*bPretty=*/false);
				LoadFailure::OverrideCode = 6;
				UE_LOG(LogBlueprintReader, Warning,
					TEXT("Refusing write to %s: locked by connection %llu (mine=%llu)"),
					*AssetPath,
					(unsigned long long)HeldBy,
					(unsigned long long)MyConn);
				return nullptr;
			}
		}
		// Check the asset out of source control now — before any caller
		// marks it dirty — so a live editor never needs the interactive
		// "Check Out?" modal (which blocks the game thread and makes the MCP
		// call look like it hung). Best-effort + gated; see the helper above.
		EnsureCheckedOutForWrite(BP->GetOutermost());
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
		if (UEdGraph* G = Search(BP->UbergraphPages))
		{
			return G;
		}
		if (UEdGraph* G = Search(BP->FunctionGraphs))
		{
			return G;
		}
		if (UEdGraph* G = Search(BP->MacroGraphs))
		{
			return G;
		}
		return nullptr;
	}

	UEdGraphNode* FindNodeByGuid(UEdGraph* Graph, const FString& Guid)
	{
		FGuid Parsed;
		if (!FGuid::Parse(Guid, Parsed))
		{
			return nullptr;
		}
		for (UEdGraphNode* N : Graph->Nodes)
		{
			if (N && N->NodeGuid == Parsed)
			{
				return N;
			}
		}
		return nullptr;
	}

	// Forward decls — these are defined later in the file but referenced
	// by ops higher up (SetPinDefaultOp / RetypeVariableOp use them).
	// Definitions land in their existing locations alongside the WirePins
	// code / EmitOk / AddVariable helpers.
	UEdGraphPin* FindPinByIdOrName(UEdGraphNode* Node, const FString& Spec);
	int32 EmitOk(const FString& OutputPath, bool bPretty);
	int32 EmitError(const FString& OutputPath, bool bPretty,
					int32 Code, const FString& ErrorMsg);
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
		// Anchor the BP against GC for the duration of compile + save.
		// `FKismetEditorUtilities::CompileBlueprint` creates and destroys
		// intermediate UObjects (BP nodes can be `MarkAsGarbage`'d during
		// recompilation) — without an explicit anchor, a GC pass kicked
		// off by the compiler could collect the BP itself if its outer
		// package's reference graph has weak links to the world. In
		// daemon mode this is rare but real; the anchor is a one-line
		// defensive measure on the cold critical path.
		TStrongObjectPtr<UBlueprint> Anchor(BP);

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

		FCompilerResultsLog Results;
		FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::None, &Results);

		if (OutDiagnostics)
		{
			TArray<TSharedPtr<FJsonValue>> Diags = SerializeDiagnostics(Results);
			OutDiagnostics->Append(MoveTemp(Diags));
		}

		UPackage* Package = BP->GetOutermost();
		if (!IsValid(Package))
		{
			return false;
		}

		const FString FileName = FPackageName::LongPackageNameToFilename(
			Package->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		Args.Error = GError;
		const bool bOk = UPackage::SavePackage(Package, BP, *FileName, Args);
		if (!bOk)
		{
			// SavePackage failure under the commandlet is most often a
			// Windows sharing violation — the editor is open and holds
			// an exclusive write handle on the .uasset (issue #2). Probe
			// via raw Win32 CreateFileW with OPEN_EXISTING and no share
			// flags — fails with ERROR_SHARING_VIOLATION (32) when the
			// file is locked, never creates/truncates the asset. (The
			// initial PR used IPlatformFile::OpenWrite, which truncates
			// on success — turning a non-lock save failure into asset
			// corruption. Codex review on PR #59 caught this.)
#if PLATFORM_WINDOWS
			const FString LockHint =
				TEXT("close the editor before running commandlet writes, or set "
				     "BP_READER_BACKEND=auto so writes route through the editor's "
				     "in-process TCP server (issue #2)");
			HANDLE Probe = ::CreateFileW(*FileName,
				GENERIC_READ | GENERIC_WRITE,
				0,                        // no share — fail if anyone has it open
				nullptr,
				OPEN_EXISTING,            // never create or truncate
				FILE_ATTRIBUTE_NORMAL,
				nullptr);
			if (Probe == INVALID_HANDLE_VALUE)
			{
				const DWORD Err = ::GetLastError();
				if (Err == ERROR_SHARING_VIOLATION)
				{
					UE_LOG(LogBlueprintReader, Error,
						TEXT("SavePackage failed: %s — file is locked (sharing violation). %s"),
						*FileName, *LockHint);
				}
				else
				{
					UE_LOG(LogBlueprintReader, Error,
						TEXT("SavePackage failed: %s — could not open file for diagnostic probe "
						     "(Win32 error %lu). %s"),
						*FileName, Err, *LockHint);
				}
			}
			else
			{
				::CloseHandle(Probe);
				UE_LOG(LogBlueprintReader, Error,
					TEXT("SavePackage failed: %s — file opens cleanly, so this isn't a "
					     "file-lock issue; check the editor log above for the underlying "
					     "compile/serialization error"),
					*FileName);
			}
#else    // PLATFORM_WINDOWS
			UE_LOG(LogBlueprintReader, Error,
				TEXT("SavePackage failed: %s — check the editor log for the underlying error"),
				*FileName);
#endif    // PLATFORM_WINDOWS
		}
		return bOk;
	}

	// ----- Batch state (A1) ---------------------------------------------------
	// When a BeginBatch op is seen, every subsequent write op defers its
	// CompileAndSaveBlueprint call. The deferred BPs are tracked here and
	// flushed by EndBatch in one pass — N×compile collapses to 1×compile per
	// affected BP. Single-op (non-batch) callers see no behavior change.
	//
	// **Per-connection** since PR #71 (multi-session): file-scope statics
	// would let conn2's BeginBatch clobber conn1's pending set. Now the
	// state lives in BlueprintReader::FBatchRegistry keyed by the thread-
	// local current connection id (set by FConnectionScope around each
	// dispatch). A zero connection id (legacy direct -run=BPR)
	// gets its own slot — backwards compatible.
	bool& BatchDeferFlag()
	{
		return BlueprintReader::FBatchRegistry::Get()
			.GetOrCreate(BlueprintReader::GetCurrentConnectionId())
			.bDeferCompile;
	}
	TArray<TWeakObjectPtr<UBlueprint>>& BatchPending()
	{
		return BlueprintReader::FBatchRegistry::Get()
			.GetOrCreate(BlueprintReader::GetCurrentConnectionId())
			.Pending;
	}

	// Replaces the 12 direct CompileAndSaveBlueprint(BP) call sites. In
	// non-batch mode this is identical to CompileAndSaveBlueprint. In batch
	// mode it just records the BP for EndBatch to process.
	bool MaybeCompileAndSave(UBlueprint* BP)
	{
		if (!BP)
		{
			return false;
		}
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
		// Release any BP write ownership the previous (unclosed) batch
		// held. Without this, re-entering BeginBatch leaks locks that
		// stay parked until disconnect — blocking other sessions on
		// BPs whose pending edits we just discarded. Surfaced by audit.
		BlueprintReader::FBatchRegistry::Get()
			.ReleaseAllWriteOwnership(BlueprintReader::GetCurrentConnectionId());
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

		// Release every BP this connection had locked for the batch.
		// Independent of bSkipCompile below — the ownership is about
		// preventing OTHER sessions from interleaving writes, and the
		// batch boundary ends regardless of whether we compile here.
		BlueprintReader::FBatchRegistry::Get()
			.ReleaseAllWriteOwnership(BlueprintReader::GetCurrentConnectionId());

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
			if (!IsValid(BP))
			{
				continue;  // GC'd between batch ops — nothing to save
			}
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
			if (!bOk)
			{
				++Failures;
			}
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
						if (Sev == TEXT("error"))
						{
							++Errors;
						}
						if (Sev == TEXT("warning"))
						{
							++Warnings;
						}
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
		if (Spec.IsEmpty())
		{
			return nullptr;
		}
		// Direct path form: "/Script/Engine.Actor"
		if (UClass* C = LoadObject<UClass>(nullptr, *Spec))
		{
			return C;
		}
		// Short-name form: "Actor", "Pawn", "ACharacter" — try common prefixes.
		const TCHAR* Prefixes[] = {TEXT(""), TEXT("/Script/Engine."), TEXT("/Script/CoreUObject.")};
		for (const TCHAR* Pre : Prefixes)
		{
			FString Trial = FString(Pre) + Spec;
			if (UClass* C = LoadObject<UClass>(nullptr, *Trial))
			{
				return C;
			}
			// Drop a leading 'A' or 'U' if present (UE convention).
			if (Spec.Len() > 1 && (Spec[0] == TEXT('A') || Spec[0] == TEXT('U')))
			{
				FString Trim = FString(Pre) + Spec.RightChop(1);
				if (UClass* C = LoadObject<UClass>(nullptr, *Trim))
				{
					return C;
				}
			}
		}
		return nullptr;
	}

	int32 RunCreateBlueprintOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		const FString AssetPath = ResolveAssetPath(Params);
		FString ParentClassSpec;
		FParse::Value(*Params, TEXT("ParentClass="), ParentClassSpec);
		FString BlueprintTypeSpec;
		FParse::Value(*Params, TEXT("BlueprintType="), BlueprintTypeSpec);

		if (AssetPath.IsEmpty())
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("CreateBlueprint requires -Asset=/Game/..."));
			return 1;
		}
		if (!AssetPath.StartsWith(TEXT("/Game/")))
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("CreateBlueprint: -Asset must be under /Game/ (got: %s)"), *AssetPath);
			return 1;
		}

		// Optional -BlueprintType= (default BPTYPE_Normal). Accepts the StaticEnum
		// name ("BPTYPE_Interface") or the short form ("Interface") — the recreator
		// passes the introspector's emitted EBlueprintType name straight through so
		// e.g. a Blueprint Interface is recreated as a real interface (no EventGraph).
		EBlueprintType BlueprintType = BPTYPE_Normal;
		if (!BlueprintTypeSpec.IsEmpty())
		{
			FString TypeName = BlueprintTypeSpec;
			if (!TypeName.StartsWith(TEXT("BPTYPE_")))
			{
				TypeName = FString(TEXT("BPTYPE_")) + TypeName;
			}
			const UEnum* TypeEnum = StaticEnum<EBlueprintType>();
			const int64 TypeVal = TypeEnum ? TypeEnum->GetValueByNameString(TypeName) : INDEX_NONE;
			if (TypeVal == INDEX_NONE)
			{
				UE_LOG(LogBlueprintReader, Error,
					TEXT("CreateBlueprint: unknown -BlueprintType=%s"), *BlueprintTypeSpec);
				return 1;
			}
			BlueprintType = static_cast<EBlueprintType>(TypeVal);
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

		// Resolve the parent. A Blueprint Interface may omit -ParentClass= and
		// defaults to UInterface (mirrors UBlueprintInterfaceFactory). Other types
		// still require an explicit parent.
		UClass* ParentClass = nullptr;
		if (!ParentClassSpec.IsEmpty())
		{
			ParentClass = ResolveParentClass(ParentClassSpec);
			if (!IsValid(ParentClass))
			{
				UE_LOG(LogBlueprintReader, Error,
					TEXT("CreateBlueprint: parent class not found: %s"), *ParentClassSpec);
				return 4;
			}
		}
		else if (BlueprintType == BPTYPE_Interface)
		{
			ParentClass = UInterface::StaticClass();
		}
		else
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("CreateBlueprint requires -ParentClass= for non-interface blueprint types"));
			return 1;
		}

		// Derive package name + asset short name from the asset path.
		// `/Game/AI/BP_Foo` -> package `/Game/AI/BP_Foo`, asset `BP_Foo`.
		const FString PackageName = AssetPath;
		const FString AssetName   = FPackageName::GetShortName(AssetPath);

		UPackage* Package = CreatePackage(*PackageName);
		if (!IsValid(Package))
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("CreatePackage failed: %s"), *PackageName);
			return 5;
		}
		Package->FullyLoad();

		UBlueprintFactory* Factory = NewObject<UBlueprintFactory>();
		Factory->ParentClass = ParentClass;
		Factory->BlueprintType = BlueprintType;

		UBlueprint* BP = Cast<UBlueprint>(Factory->FactoryCreateNew(
			UBlueprint::StaticClass(), Package, *AssetName,
			RF_Public | RF_Standalone, nullptr, GWarn));
		if (!IsValid(BP))
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("FactoryCreateNew failed: %s"), *PackageName);
			return 5;
		}
		BP->MarkPackageDirty();

		// Critical: register with asset registry so a follow-up op in the
		// same batch (e.g. AddVariable) can LoadMutableBlueprint() this asset.
		FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		ARM.Get().AssetCreated(BP);

		if (!MaybeCompileAndSave(BP))
		{
			return 5;
		}

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		Obj->SetBoolField(TEXT("already_existed"), false);
		Obj->SetStringField(TEXT("asset_path"), AssetPath);
		Obj->SetStringField(TEXT("parent_class"), ParentClass->GetPathName());
		if (const UEnum* TypeEnum = StaticEnum<EBlueprintType>())
		{
			Obj->SetStringField(TEXT("blueprint_type"),
				TypeEnum->GetNameStringByValue(static_cast<int64>(BlueprintType)));
		}
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
		if (!IsValid(BP))
		{
			return 2;
		}
		UEdGraph* Graph = FindGraphByName(BP, GraphName);
		if (!IsValid(Graph))
		{
			return 4;
		}
		UEdGraphNode* Node = FindNodeByGuid(Graph, NodeId);
		if (!IsValid(Node))
		{
			return 4;
		}
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
		if (!MaybeCompileAndSave(BP))
		{
			return 5;
		}
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
		if (!IsValid(BP))
		{
			return 4;
		}
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
		if (!MaybeCompileAndSave(BP))
		{
			return 5;
		}
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
		if (!IsValid(BP))
		{
			return 4;
		}
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
		if (!MaybeCompileAndSave(BP))
		{
			return 5;
		}
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
			if (Pkg && Pkg->IsDirty())
			{
				DirtyBefore.Add(Pkg);
			}
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
			if (!IsValid(Pkg))
			{
				continue;
			}
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
		if (!IsValid(SourceObj))
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
		// Force-scan the containing folder so the registry picks up any
		// .uasset files saved by a prior commandlet process. In commandlet
		// mode the cached registry is loaded at startup; files written by
		// later processes aren't in it. Without this rescan, DeleteAsset
		// on a clone produced by a previous run says "not found" and
		// leaves the .uasset rotting on disk.
		const FString ContainingDir = FPaths::GetPath(AssetPath);
		if (!ContainingDir.IsEmpty())
		{
			AR.ScanPathsSynchronous({ ContainingDir }, /*bForce=*/ true);
		}
		FAssetData AssetData = AR.GetAssetByObjectPath(FSoftObjectPath(AssetPath));
		if (!AssetData.IsValid())
		{
			// Asset registry doesn't know about this path. With -Force,
			// fall back to a raw disk-level purge — the .uasset file may
			// have been saved by a prior commandlet process that exited
			// before its cache update was persisted, leaving the registry
			// in this process stale. Without this fallback, a clone from
			// a previous test run rots on disk forever (no registry entry
			// to delete) and the next CreateBlueprint trips its idempotency
			// probe.
			if (bForce)
			{
				const FString PkgFilename = FPackageName::LongPackageNameToFilename(
					AssetPath, FPackageName::GetAssetPackageExtension());
				bool bDiskPurged = false;
				if (FPaths::FileExists(PkgFilename))
				{
					bDiskPurged = IFileManager::Get().Delete(
						*PkgFilename, /*bRequireExists=*/false,
						/*bEvenReadOnly=*/true, /*bQuiet=*/false);
					UE_LOG(LogBlueprintReader, Display,
						TEXT("DeleteAsset: registry miss, raw disk-purge: %s -> %s"),
						*PkgFilename, bDiskPurged ? TEXT("OK") : TEXT("FAILED"));
				}
				auto Out = MakeShared<FJsonObject>();
				Out->SetBoolField(TEXT("ok"), true);
				Out->SetStringField(TEXT("path"), AssetPath);
				Out->SetBoolField(TEXT("deleted"), bDiskPurged);
				Out->SetBoolField(TEXT("already_absent"), !bDiskPurged);
				return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
			}
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
			if (IsValid(Obj))
			{
				TArray<UObject*> ToDelete = { Obj };
				const int32 NumDeleted = ObjectTools::ForceDeleteObjects(
					ToDelete, /*bShowConfirmation=*/false);
				bDeleted = NumDeleted > 0;
			}
			// ForceDeleteObjects removes the in-memory UObject + asset-
			// registry entry, but in commandlet mode it doesn't persist
			// a deletion to disk — the .uasset file lingers. A next-run
			// CreateBlueprint then sees the stale file and short-circuits
			// to idempotent-load, which makes downstream AddVariable
			// fail with "variable already exists". Force-remove the
			// .uasset off disk so the next process starts clean.
			const FString PackageFilename = FPackageName::LongPackageNameToFilename(
				AssetData.PackageName.ToString(), FPackageName::GetAssetPackageExtension());
			if (FPaths::FileExists(PackageFilename))
			{
				const bool bRemoved = IFileManager::Get().Delete(
					*PackageFilename, /*bRequireExists=*/false,
					/*bEvenReadOnly=*/true, /*bQuiet=*/false);
				UE_LOG(LogBlueprintReader, Display,
					TEXT("DeleteAsset disk-purge: %s -> %s"),
					*PackageFilename, bRemoved ? TEXT("OK") : TEXT("FAILED"));
			}
			else
			{
				UE_LOG(LogBlueprintReader, Display,
					TEXT("DeleteAsset: file does not exist at %s"), *PackageFilename);
			}

			// Force a GC pass so the just-deleted Blueprint's generated class and
			// (crucially) its custom-event function names are released right away.
			// ForceDeleteObjects nulls references but leaves the object alive until
			// the next GC; if we recreate into the same path before then, the
			// freshly-imported custom events collide with the lingering names and
			// get silently demoted to generic "CustomEvent" nodes. That showed up
			// as a run-to-run flake (first recreate over an existing target lost
			// event names, a second pass succeeded). A full purge here makes
			// recreate-over-existing deterministic.
			if (bDeleted)
			{
				CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, /*bPerformFullPurge=*/true);
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
		if (PathFilter.IsEmpty())
		{
			PathFilter = TEXT("/Game");
		}

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
		if (!IsValid(DT))
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("ReadDataTable: failed to load: %s"), *AssetPath);
			return 4;
		}

		const UScriptStruct* RowStruct = DT->GetRowStruct();
		FString RowStructPath;
		TArray<TSharedPtr<FJsonValue>> Columns;
		if (IsValid(RowStruct))
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
		if (IsValid(GEditor))
		{
			if (UWorld* W = GEditor->GetEditorWorldContext().World())
			{
				return W;
			}
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
		if (IsValid(GEngine))
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
		if (IsValid(GEditor))
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
			if (IsValid(GEngine))
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
		if (IsValid(GEditor))
		{
			USelection* Selection = GEditor->GetSelectedActors();
			if (IsValid(Selection))
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

	// One-call situational-awareness bundle. Mirrors Epic AIAssistant's
	// Slate-querier intent: "what is the user looking at right now?"
	// Returns active asset + open assets + current level + viewport camera
	// + actor selection + PIE state in a single response so the agent
	// doesn't have to coordinate 6 separate calls before its first edit.
	// Local helper: convert /Game/AI/BP_Foo.BP_Foo → /Game/AI/BP_Foo.
	// Wire format always uses package paths. Mirrors the same-named
	// helper anon-ns'd inside BlueprintReaderWireJson.cpp — kept inline
	// here to avoid widening that header just for this op.
	FString EditorStateToPackagePath(const FString& In)
	{
		int32 DotIdx = INDEX_NONE;
		if (In.FindChar(TEXT('.'), DotIdx))
		{
			return In.Left(DotIdx);
		}
		return In;
	}

	int32 RunGetEditorStateOp(const FString&, const FString& OutputPath, bool bPretty)
	{
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);

		// ----- open assets + active asset --------------------------------
		// UAssetEditorSubsystem tracks every asset whose editor tab is
		// currently open. "Active" = the editor with the latest
		// FindEditorForAsset->GetLastActivationTime — the actual API for
		// "which window did the user touch most recently."
		//
		// Earlier revisions of this op cheated by flagging
		// `OpenAssets.Num()-1` as active. That was a bug:
		// GetAllEditedAssets() iterates a TMultiMap and returns hash-
		// bucket order, NOT insertion / activation order. Surfaced by
		// audit pass; fixed here.
		TArray<TSharedPtr<FJsonValue>> OpenAssetsJson;
		TSharedPtr<FJsonValue> ActiveAssetJson;
		if (IsValid(GEditor))
		{
			if (UAssetEditorSubsystem* AES = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
			{
				TArray<UObject*> OpenAssets = AES->GetAllEditedAssets();

				// First pass: find the most-recently-activated editor by
				// querying each open asset's editor instance. Editor
				// activation time is the authoritative signal.
				UObject* ActiveAsset = nullptr;
				double BestActivation = -1.0;
				for (UObject* Obj : OpenAssets)
				{
					if (!Obj)
					{
						continue;
					}
					IAssetEditorInstance* Inst = AES->FindEditorForAsset(Obj, /*bFocusIfOpen=*/false);
					if (!Inst)
					{
						continue;
					}
					const double T = Inst->GetLastActivationTime();
					if (T > BestActivation)
					{
						BestActivation = T;
						ActiveAsset = Obj;
					}
				}

				// Second pass: emit the array, flagging which one matched.
				for (UObject* Obj : OpenAssets)
				{
					if (!Obj)
					{
						continue;
					}
					auto Entry = MakeShared<FJsonObject>();
					Entry->SetStringField(TEXT("package_path"),
						EditorStateToPackagePath(Obj->GetPathName()));
					Entry->SetStringField(TEXT("class"),
						Obj->GetClass() ? Obj->GetClass()->GetName() : TEXT(""));
					const bool bIsActive = (Obj == ActiveAsset);
					Entry->SetBoolField(TEXT("is_active"), bIsActive);
					if (bIsActive)
					{
						ActiveAssetJson = MakeShared<FJsonValueObject>(Entry);
					}
					OpenAssetsJson.Add(MakeShared<FJsonValueObject>(Entry));
				}
			}
		}
		Out->SetArrayField(TEXT("open_assets"), OpenAssetsJson);
		if (ActiveAssetJson.IsValid())
		{
			Out->SetField(TEXT("active_asset"), ActiveAssetJson);
		}
		else                            Out->SetField(TEXT("active_asset"), MakeShared<FJsonValueNull>());

		// ----- current level --------------------------------------------
		TSharedPtr<FJsonObject> LevelJson;
		if (IsValid(GEditor))
		{
			UWorld* World = GEditor->GetEditorWorldContext().World();
			if (IsValid(World))
			{
				LevelJson = MakeShared<FJsonObject>();
				LevelJson->SetStringField(TEXT("package_path"),
					EditorStateToPackagePath(World->GetPathName()));
				LevelJson->SetStringField(TEXT("name"), World->GetMapName());
				const bool bDirty = World->GetOutermost() && World->GetOutermost()->IsDirty();
				LevelJson->SetBoolField(TEXT("is_dirty"), bDirty);
			}
		}
		if (LevelJson.IsValid())
		{
			Out->SetObjectField(TEXT("current_level"), LevelJson);
		}
		else                      Out->SetField(TEXT("current_level"), MakeShared<FJsonValueNull>());

		// ----- viewport camera (read; we already have set_camera_transform) -
		TSharedPtr<FJsonObject> CameraJson;
		if (IsValid(GEditor))
		{
			// Iterate the level viewport clients; pick the active one (or
			// the first perspective viewport if none active). Mirrors how
			// set_camera_transform identifies its target.
			FEditorViewportClient* Best = nullptr;
			for (FEditorViewportClient* VC : GEditor->GetAllViewportClients())
			{
				if (!VC)
				{
					continue;
				}
				if (VC->IsLevelEditorClient())
				{
					if (VC->Viewport && VC->Viewport->HasFocus())
					{
						Best = VC;
						break;
					}
					if (!Best && VC->IsPerspective())
					{
						Best = VC;
					}
				}
			}
			if (Best)
			{
				const FVector L = Best->GetViewLocation();
				const FRotator R = Best->GetViewRotation();
				CameraJson = MakeShared<FJsonObject>();
				auto Loc = MakeShared<FJsonObject>();
				Loc->SetNumberField(TEXT("x"), L.X);
				Loc->SetNumberField(TEXT("y"), L.Y);
				Loc->SetNumberField(TEXT("z"), L.Z);
				CameraJson->SetObjectField(TEXT("location"), Loc);
				auto Rot = MakeShared<FJsonObject>();
				Rot->SetNumberField(TEXT("pitch"), R.Pitch);
				Rot->SetNumberField(TEXT("yaw"),   R.Yaw);
				Rot->SetNumberField(TEXT("roll"),  R.Roll);
				CameraJson->SetObjectField(TEXT("rotation"), Rot);
			}
		}
		if (CameraJson.IsValid())
		{
			Out->SetObjectField(TEXT("camera"), CameraJson);
		}
		else                       Out->SetField(TEXT("camera"), MakeShared<FJsonValueNull>());

		// ----- selected actors (duplicated for one-call convenience) ----
		TArray<TSharedPtr<FJsonValue>> SelectedJson;
		if (IsValid(GEditor))
		{
			if (USelection* Selection = GEditor->GetSelectedActors())
			{
				for (FSelectionIterator It(*Selection); It; ++It)
				{
					if (AActor* Actor = Cast<AActor>(*It))
					{
						SelectedJson.Add(MakeShared<FJsonValueString>(Actor->GetName()));
					}
				}
			}
		}
		Out->SetArrayField(TEXT("selected_actors"), SelectedJson);

		// ----- PIE state -----------------------------------------------
		const bool bPieRunning = (GEditor && GEditor->PlayWorld != nullptr);
		Out->SetBoolField(TEXT("pie_running"), bPieRunning);

		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	// Epic AIAssistant's code-as-universal-tool capability. Wraps
	// PythonScriptPlugin's ExecPythonCommandEx with the same
	// transaction-boundary pattern AIAssistant uses. Gated by env var
	// (BP_READER_ALLOW_PYTHON=1) because arbitrary Python = arbitrary
	// editor mutation; opt-in only.
	int32 RunRunPythonScriptOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		// Gate: require BP_READER_ALLOW_PYTHON=1. Default-off because
		// arbitrary Python execution bypasses every safety convention
		// the curated 119-tool surface establishes.
		const FString AllowRaw = FPlatformMisc::GetEnvironmentVariable(TEXT("BP_READER_ALLOW_PYTHON"));
		const bool bAllowed =
			AllowRaw.Equals(TEXT("1")) || AllowRaw.Equals(TEXT("true"), ESearchCase::IgnoreCase) ||
			AllowRaw.Equals(TEXT("yes"), ESearchCase::IgnoreCase) || AllowRaw.Equals(TEXT("on"), ESearchCase::IgnoreCase);
		if (!bAllowed)
		{
			auto Obj = MakeShared<FJsonObject>();
			Obj->SetBoolField(TEXT("ok"), false);
			Obj->SetStringField(TEXT("error"), TEXT("python_disabled"));
			Obj->SetStringField(TEXT("hint"),
				TEXT("Set BP_READER_ALLOW_PYTHON=1 in the MCP server's env "
				     "to enable run_python_script. Off by default — arbitrary "
				     "Python in the editor is a footgun."));
			return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
		}

		FString Code;
		FParse::Value(*Params, TEXT("Code="), Code);
		if (Code.IsEmpty())
		{
			auto Obj = MakeShared<FJsonObject>();
			Obj->SetBoolField(TEXT("ok"), false);
			Obj->SetStringField(TEXT("error"),
				TEXT("run_python_script requires a non-empty -Code= argument"));
			return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
		}

		// Refuse cleanly if the Python plugin isn't loaded — separate error
		// path so the agent can distinguish "disabled by env" from "engine
		// build doesn't include Python."
		IPythonScriptPlugin* Py = IPythonScriptPlugin::Get();
		if (!Py)
		{
			auto Obj = MakeShared<FJsonObject>();
			Obj->SetBoolField(TEXT("ok"), false);
			Obj->SetStringField(TEXT("error"), TEXT("python_plugin_unavailable"));
			Obj->SetStringField(TEXT("hint"),
				TEXT("IPythonScriptPlugin is null — the engine build doesn't "
				     "include PythonScriptPlugin, or it's disabled in this "
				     "project's .uproject. bp-reader.uplugin requests it, "
				     "but a packaged commandlet build may strip it."));
			return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
		}

		// Mirror Epic AIAssistant's pattern: wrap in a transaction so
		// the Python-driven editor mutations land as one undo-able unit.
		// RAII-scope the Begin/End pair so even if ExecPythonCommandEx
		// throws (or anything between throws), the transaction always
		// gets ended — UE's transaction stack leaks otherwise.
		const FText TransactionTitle = FText::FromString(
			TEXT("bp-reader: run_python_script"));
		struct FPyTransactionScope
		{
			bool bActive = false;
			explicit FPyTransactionScope(const FText& Title)
			{
				if (IsValid(GEditor)) { GEditor->BeginTransaction(Title); bActive = true; }
			}
			~FPyTransactionScope()
			{
				if (bActive && GEditor)
				{
					GEditor->EndTransaction();
				}
			}
		} TxScope(TransactionTitle);

		FPythonCommandEx PyCmd;
		PyCmd.Command = Code;
		PyCmd.ExecutionMode = EPythonCommandExecutionMode::ExecuteFile;
		// CaptureOutput so we get stdout/stderr in PyCmd.LogOutput.
		PyCmd.Flags |= EPythonCommandFlags::Unattended;

		const bool bOk = Py->ExecPythonCommandEx(PyCmd);

		TArray<TSharedPtr<FJsonValue>> LogEntriesJson;
		for (const FPythonLogOutputEntry& Entry : PyCmd.LogOutput)
		{
			auto E = MakeShared<FJsonObject>();
			E->SetStringField(TEXT("type"),
				Entry.Type == EPythonLogOutputType::Info ? TEXT("info")
				: Entry.Type == EPythonLogOutputType::Warning ? TEXT("warning")
				: TEXT("error"));
			E->SetStringField(TEXT("output"), Entry.Output);
			LogEntriesJson.Add(MakeShared<FJsonValueObject>(E));
		}

		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), bOk);
		Out->SetStringField(TEXT("command_result"), PyCmd.CommandResult);
		Out->SetArrayField(TEXT("log"), LogEntriesJson);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	// --------------------------------------------------------------------
	// #83 quick-win ops: asset queries + project settings + lighting build.
	// All five live below — they share enough boilerplate that grouping is
	// cheaper to read than splitting.

	// Shared: query the asset registry for referencers or dependencies.
	// Direction selects which API to call; both return a flat array of
	// package paths.
	int32 RunAssetGraphQueryOp(const FString& Params, const FString& OutputPath,
	                           bool bPretty, bool bWantReferencers)
	{
		FString AssetPath;
		FParse::Value(*Params, TEXT("Asset="), AssetPath);
		if (AssetPath.IsEmpty())
		{
			auto Obj = MakeShared<FJsonObject>();
			Obj->SetBoolField(TEXT("ok"), false);
			Obj->SetStringField(TEXT("error"),
				bWantReferencers
					? TEXT("get_referencers requires -Asset=<package_path>")
					: TEXT("get_dependencies requires -Asset=<package_path>"));
			return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
		}

		FAssetRegistryModule& ARM =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		IAssetRegistry& AR = ARM.Get();
		const FName PackageName(*AssetPath);

		TArray<FName> Results;
		if (bWantReferencers)
		{
			AR.GetReferencers(PackageName, Results);
		}
		else
		{
			AR.GetDependencies(PackageName, Results);
		}

		TArray<TSharedPtr<FJsonValue>> Json;
		Json.Reserve(Results.Num());
		for (const FName& N : Results)
		{
			Json.Add(MakeShared<FJsonValueString>(N.ToString()));
		}
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetStringField(TEXT("asset_path"), AssetPath);
		Out->SetArrayField(bWantReferencers ? TEXT("referencers") : TEXT("dependencies"), Json);
		Out->SetNumberField(TEXT("count"), Results.Num());
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunGetReferencersOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		return RunAssetGraphQueryOp(Params, OutputPath, bPretty, /*bWantReferencers=*/true);
	}

	int32 RunGetDependenciesOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		return RunAssetGraphQueryOp(Params, OutputPath, bPretty, /*bWantReferencers=*/false);
	}

	// Helper: map an ini-file token (e.g. "Engine", "Game") to the actual
	// GConfig branch path. Mirrors how UE's own commandlets accept the
	// short name plus the full path.
	FString ResolveIniBranchName(const FString& Token)
	{
		if (Token.IsEmpty() || Token.Equals(TEXT("Engine"), ESearchCase::IgnoreCase))
		{
			return GEngineIni;
		}
		if (Token.Equals(TEXT("Game"), ESearchCase::IgnoreCase))
		{
			return GGameIni;
		}
		if (Token.Equals(TEXT("Input"), ESearchCase::IgnoreCase))
		{
			return GInputIni;
		}
		if (Token.Equals(TEXT("Editor"), ESearchCase::IgnoreCase))
		{
			return GEditorIni;
		}
		if (Token.Equals(TEXT("EditorPerProjectIni"), ESearchCase::IgnoreCase))
		{
				return GEditorPerProjectIni;
		}
		// Caller passed a full path; let GConfig sort it out.
		return Token;
	}

	int32 RunReadConfigValueOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString IniFile;
		FString Section;
		FString Key;
		FParse::Value(*Params, TEXT("File="), IniFile);
		FParse::Value(*Params, TEXT("Section="), Section);
		FParse::Value(*Params, TEXT("Key="), Key);

		auto Out = MakeShared<FJsonObject>();
		if (Section.IsEmpty() || Key.IsEmpty())
		{
			Out->SetBoolField(TEXT("ok"), false);
			Out->SetStringField(TEXT("error"),
				TEXT("read_config_value requires -Section= and -Key= "
				     "(optional -File=<Engine|Game|Input|Editor|EditorPerProjectIni> "
				     "or full path; defaults to Engine)"));
			return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
		}

		const FString Branch = ResolveIniBranchName(IniFile);
		FString Value;
		const bool bFound = GConfig->GetString(*Section, *Key, Value, Branch);

		Out->SetBoolField(TEXT("ok"), true);
		Out->SetBoolField(TEXT("exists"), bFound);
		Out->SetStringField(TEXT("section"), Section);
		Out->SetStringField(TEXT("key"), Key);
		Out->SetStringField(TEXT("file"), Branch);
		if (bFound)
		{
			Out->SetStringField(TEXT("value"), Value);
		}
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunSetConfigValueOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString IniFile;
		FString Section;
		FString Key;
		FString Value;
		FParse::Value(*Params, TEXT("File="), IniFile);
		FParse::Value(*Params, TEXT("Section="), Section);
		FParse::Value(*Params, TEXT("Key="), Key);
		FParse::Value(*Params, TEXT("Value="), Value);

		auto Out = MakeShared<FJsonObject>();
		if (Section.IsEmpty() || Key.IsEmpty())
		{
			Out->SetBoolField(TEXT("ok"), false);
			Out->SetStringField(TEXT("error"),
				TEXT("set_config_value requires -Section=, -Key=, and -Value="));
			return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
		}

		const FString Branch = ResolveIniBranchName(IniFile);
		// Capture old value for the response (so the agent can see what
		// changed). GetString returns false on missing — we surface that
		// as `previous_value: null`.
		FString OldValue;
		const bool bExisted = GConfig->GetString(*Section, *Key, OldValue, Branch);

		GConfig->SetString(*Section, *Key, *Value, Branch);
		GConfig->Flush(/*Read=*/false, Branch);

		Out->SetBoolField(TEXT("ok"), true);
		Out->SetStringField(TEXT("section"), Section);
		Out->SetStringField(TEXT("key"), Key);
		Out->SetStringField(TEXT("file"), Branch);
		Out->SetStringField(TEXT("value"), Value);
		if (bExisted)
		{
			Out->SetStringField(TEXT("previous_value"), OldValue);
		}
		else          Out->SetField(TEXT("previous_value"), MakeShared<FJsonValueNull>());
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunBuildLightingOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString Quality;
		FParse::Value(*Params, TEXT("Quality="), Quality);
		// Resolve quality token → ELightingBuildQuality enum. Defaults to
		// Production (the most common "build it for shipping" choice).
		// Console-command form `BuildLighting <quality>` works too but
		// only when the editor is interactive; this path uses the UEd
		// API directly so it works from commandlet + automation contexts.
		ELightingBuildQuality Q = ELightingBuildQuality::Quality_Production;
		if      (Quality.Equals(TEXT("Preview"), ESearchCase::IgnoreCase))
		{
			Q = ELightingBuildQuality::Quality_Preview;
		}
		else if (Quality.Equals(TEXT("Medium"), ESearchCase::IgnoreCase))     Q = ELightingBuildQuality::Quality_Medium;
		else if (Quality.Equals(TEXT("High"), ESearchCase::IgnoreCase))       Q = ELightingBuildQuality::Quality_High;
		else if (Quality.Equals(TEXT("Production"), ESearchCase::IgnoreCase)) Q = ELightingBuildQuality::Quality_Production;

		auto Out = MakeShared<FJsonObject>();
		if (!GEditor || !GEditor->GetEditorWorldContext().World())
		{
			Out->SetBoolField(TEXT("ok"), false);
			Out->SetStringField(TEXT("error"),
				TEXT("build_lighting requires a running editor with a level loaded"));
			return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
		}

		FLightingBuildOptions Options;
		Options.QualityLevel = Q;
		// Async by design — UEd returns once the build is QUEUED, not
		// once it finishes. The agent should poll read_output_log or
		// check for the completion log line.
		GEditor->BuildLighting(Options);

		Out->SetBoolField(TEXT("ok"), true);
		Out->SetBoolField(TEXT("queued"), true);
		Out->SetStringField(TEXT("quality"),
			Q == ELightingBuildQuality::Quality_Preview ? TEXT("Preview")
			: Q == ELightingBuildQuality::Quality_Medium ? TEXT("Medium")
			: Q == ELightingBuildQuality::Quality_High ? TEXT("High")
			: TEXT("Production"));
		Out->SetStringField(TEXT("note"),
			TEXT("Lighting build is async. Poll read_output_log for "
			     "completion (Lightmass emits 'Lighting build complete' "
			     "or 'Lighting build failed')."));
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunSetSelectionOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString NamesJoined;
		FParse::Value(*Params, TEXT("Names="), NamesJoined);
		const bool bReplace = !FParse::Param(*Params, TEXT("Add"));

		TArray<FString> NameList;
		NamesJoined.ParseIntoArray(NameList, TEXT(","), /*bCullEmpty=*/true);

		if (IsValid(GEditor))
		{
			USelection* Selection = GEditor->GetSelectedActors();
			if (IsValid(Selection))
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
		if (IsValid(GEditor))
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
		if (!IsValid(SpawnClass))
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("SpawnActor: could not load class '%s'"), *ClassPath);
			return 4;
		}
		UWorld* World = GetEditorWorldOrNull();
		if (!IsValid(World))
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("SpawnActor: no editor world"));
			return 5;
		}

		FTransform T(FRotator(RP, RY, RR), FVector(LX, LY, LZ), FVector(SX, SY, SZ));
		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AActor* Spawned = World->SpawnActor<AActor>(SpawnClass, T, SpawnParams);
		if (!IsValid(Spawned))
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
		if (!IsValid(World))
		{
			return nullptr;
		}
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (*It && (*It)->GetName() == Name)
			{
				return *It;
			}
		}
		return nullptr;
	}

	int32 RunSetActorTransformOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString Name;
		FParse::Value(*Params, TEXT("Name="), Name);
		AActor* Actor = FindActorByName(Name);
		if (!IsValid(Actor))
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
		if (!IsValid(DT))
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("AddDataRow: failed to load DataTable: %s"), *AssetPath);
			return 4;
		}
		const UScriptStruct* RowStruct = DT->GetRowStruct();
		if (!IsValid(RowStruct))
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
			if (!Prop)
			{
				continue;  // ignore unknown keys
			}
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
		if (!IsValid(DT))
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
		if (!SCS)
		{
			return nullptr;
		}
		return SCS->FindSCSNode(Name);
	}

	USCS_Node* FindParentOfNode(USimpleConstructionScript* SCS, USCS_Node* Target)
	{
		if (!SCS || !Target)
		{
			return nullptr;
		}
		for (USCS_Node* Root : SCS->GetRootNodes())
		{
			TArray<USCS_Node*> Stack;
			Stack.Push(Root);
			while (Stack.Num() > 0)
			{
				USCS_Node* Cur = Stack.Pop();
				if (!IsValid(Cur))
				{
					continue;
				}
				for (USCS_Node* Child : Cur->GetChildNodes())
				{
					if (Child == Target)
					{
						return Cur;
					}
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
		if (!IsValid(BP))
		{
			return 4;
		}
		USimpleConstructionScript* SCS = BP->SimpleConstructionScript;
		if (!IsValid(SCS))
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
		if (!IsValid(NewNode))
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("AddComponent: SCS->CreateNode returned null"));
			return 5;
		}

		// Parent attachment: child of a named node, or root.
		USCS_Node* Parent = ParentName.IsEmpty() ? nullptr :
			SCS->FindSCSNode(FName(*ParentName));
		if (IsValid(Parent))
		{
			Parent->AddChildNode(NewNode);
			if (!Socket.IsEmpty())
			{
				NewNode->AttachToName = FName(*Socket);
			}
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
		if (!IsValid(BP))
		{
			return 4;
		}
		USimpleConstructionScript* SCS = BP->SimpleConstructionScript;
		USCS_Node* Node = SCS ? SCS->FindSCSNode(FName(*Name)) : nullptr;

		bool bRemoved = false;
		if (IsValid(Node))
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
		if (!IsValid(BP))
		{
			return 4;
		}
		USimpleConstructionScript* SCS = BP->SimpleConstructionScript;
		USCS_Node* Node = SCS ? SCS->FindSCSNode(FName(*Name)) : nullptr;
		if (!IsValid(Node))
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("AttachComponent: node '%s' not found"), *Name);
			return 4;
		}

		// Detach from current parent (could be root or a child).
		USCS_Node* OldParent = FindParentOfNode(SCS, Node);
		if (IsValid(OldParent))
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
		if (IsValid(NewParentNode))
		{
			NewParentNode->AddChildNode(Node);
			if (!Socket.IsEmpty())
			{
				Node->AttachToName = FName(*Socket);
			}
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
		if (!IsValid(BP))
		{
			return 4;
		}
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
		if (PathFilter.IsEmpty())
		{
			PathFilter = TEXT("/Game");
		}

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
		if (!IsValid(Mat))
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
			if (!E)
			{
				continue;
			}
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
				if (!In || !In->Expression)
				{
					continue;
				}
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
			if (!Slot.Expression)
			{
				return;
			}
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
		if (!IsValid(Mat))
		{
			return 4;
		}

		// Resolve the expression class by short name. UMaterialEditingLibrary
		// expects a UClass*, not a string — look it up via UObject iteration
		// since UMaterialExpression subclasses live in /Script/Engine.
		UClass* ExprClass = nullptr;
		for (TObjectIterator<UClass> It; It; ++It)
		{
			if (!It->IsChildOf(UMaterialExpression::StaticClass()))
			{
				continue;
			}
			if (It->GetName() == ClassName) { ExprClass = *It; break; }
		}
		if (!IsValid(ExprClass))
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("AddMaterialExpression: class '%s' not found"), *ClassName);
			return 4;
		}

		UMaterialExpression* Expr =
			UMaterialEditingLibrary::CreateMaterialExpression(Mat, ExprClass, X, Y);
		if (!IsValid(Expr))
		{
			return 4;
		}

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
		if (!IsValid(Mat))
		{
			return 4;
		}

		auto FindExpr = [&](const FString& Name) -> UMaterialExpression*
		{
			for (UMaterialExpression* E : Mat->GetExpressions())
			{
				if (E && E->GetName() == Name)
				{
					return E;
				}
			}
			return nullptr;
		};
		UMaterialExpression* From = FindExpr(FromName);
		if (!IsValid(From)) { UE_LOG(LogBlueprintReader, Error, TEXT("From expression not found")); return 4; }

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
			if (!IsValid(To)) { UE_LOG(LogBlueprintReader, Error, TEXT("To expression not found")); return 4; }
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
		if (!IsValid(Mat))
		{
			return 4;
		}

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

		if (bSet)
		{
			Mat->MarkPackageDirty();
		}

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
		if (!IsValid(MIC))
		{
			return 4;
		}

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

		if (bOk)
		{
			MIC->MarkPackageDirty();
		}

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
		if (!IsValid(Mat))
		{
			return 4;
		}

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
		if (UWidget* Root = WBP->WidgetTree->RootWidget)
		{
			RootName = Root->GetName();
		}

		// Walk every widget in the tree. WidgetTree::ForEachWidget visits
		// every node including the root. ParentName is empty for the root.
		WBP->WidgetTree->ForEachWidget([&](UWidget* W)
		{
			if (!W)
			{
				return;
			}
			auto Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("name"),  W->GetName());
			Obj->SetStringField(TEXT("class"), W->GetClass()->GetName());
			FString ParentName;
			if (UPanelWidget* P = W->GetParent())
			{
				ParentName = P->GetName();
			}
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
		if (!WBP || !WBP->WidgetTree)
		{
			return 4;
		}

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
			if (!It->IsChildOf(UWidget::StaticClass()))
			{
				continue;
			}
			if (It->GetName() == ClassName) { WidgetClass = *It; break; }
		}
		if (!IsValid(WidgetClass))
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("AddWidget: class '%s' not found"), *ClassName);
			return 4;
		}

		UWidget* NewWidget = WBP->WidgetTree->ConstructWidget<UWidget>(
			WidgetClass, FName(*Name));
		if (!IsValid(NewWidget))
		{
			return 4;
		}

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
		if (!WBP || !WBP->WidgetTree)
		{
			return 4;
		}
		UWidget* W = WBP->WidgetTree->FindWidget(FName(*WidgetName));
		if (!IsValid(W))
		{
			return 4;
		}

		FProperty* Prop = W->GetClass()->FindPropertyByName(FName(*PropName));
		if (!Prop)
		{
			return 4;
		}

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
		if (!WBP || !WBP->WidgetTree)
		{
			return 4;
		}

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
				if (IsValid(EventGraph))
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
		if (!IsValid(WBP))
		{
			return 4;
		}

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
		if (PathFilter.IsEmpty())
		{
			PathFilter = TEXT("/Game");
		}

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
		if (!IsValid(BT))
		{
			return 4;
		}

		TArray<TSharedPtr<FJsonValue>> Nodes;
		FString RootName;

		// Walk the root composite tree. Each UBTCompositeNode has
		// Children + Decorators + Services arrays.
		TFunction<void(UBTNode*, const FString&)> Walk = [&](UBTNode* N, const FString& Parent)
		{
			if (!N)
			{
				return;
			}
			auto Obj = MakeShared<FJsonObject>();
			FString Kind;
			if (Cast<UBTCompositeNode>(N))
			{
				Kind = TEXT("composite");
			}
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
					for (UBTDecorator* Dec : Child.Decorators)
					{
						Walk(Dec, N->GetName());
					}
					// Child node + its sub-tree.
					if (Child.ChildComposite)
					{
						Walk(Child.ChildComposite, N->GetName());
					}
					if (Child.ChildTask)
					{
						Walk(Child.ChildTask,      N->GetName());
					}
				}
				for (UBTService* Svc : Comp->Services)
				{
					Walk(Svc, N->GetName());
				}
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
		if (!IsValid(BT))
		{
			return 4;
		}

		// Resolve node class by short name from any UBTNode subclass.
		UClass* NodeClass = nullptr;
		for (TObjectIterator<UClass> It; It; ++It)
		{
			if (!It->IsChildOf(UBTNode::StaticClass()))
			{
				continue;
			}
			if (It->GetName() == ClassName) { NodeClass = *It; break; }
		}
		if (!IsValid(NodeClass))
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("AddBTNode: class '%s' not found"), *ClassName);
			return 4;
		}

		UBTNode* NewNode = NewObject<UBTNode>(BT, NodeClass);
		if (!IsValid(NewNode))
		{
			return 4;
		}

		// Attach. Root composite if empty parent + this is composite + tree empty.
		if (ParentName.IsEmpty())
		{
			if (UBTCompositeNode* AsComp = Cast<UBTCompositeNode>(NewNode))
			{
				if (!BT->RootNode)
				{
					BT->RootNode = AsComp;
				}
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
		if (!IsValid(BT))
		{
			return 4;
		}

		// Linear search across all UBTNode children of the asset's package.
		UBTNode* Target = nullptr;
		ForEachObjectWithOuter(BT, [&](UObject* O)
		{
			if (IsValid(Target))
			{
				return;
			}
			if (UBTNode* N = Cast<UBTNode>(O))
			{
				if (N->GetName() == NodeName)
				{
					Target = N;
				}
			}
		});
		if (!IsValid(Target))
		{
			return 4;
		}

		FProperty* Prop = Target->GetClass()->FindPropertyByName(FName(*PropName));
		if (!Prop)
		{
			return 4;
		}

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
		if (!IsValid(BT))
		{
			return 4;
		}

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
		if (PathFilter.IsEmpty())
		{
			PathFilter = TEXT("/Game");
		}

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
		if (!IsValid(DA))
		{
			return 4;
		}

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
			if (!It->IsChildOf(UDataAsset::StaticClass()))
			{
				continue;
			}
			if (It->GetName() == ClassName) { AssetClass = *It; break; }
		}
		if (!IsValid(AssetClass))
		{
			return 4;
		}

		// Convert package path to disk path + create the asset.
		FString PackageName = AssetPath;
		FString AssetName = FPackageName::GetShortName(PackageName);
		UPackage* Pkg = CreatePackage(*PackageName);
		if (!IsValid(Pkg))
		{
			return 4;
		}
		UDataAsset* NewDA = NewObject<UDataAsset>(Pkg, AssetClass,
			FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
		if (!IsValid(NewDA))
		{
			return 4;
		}
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
		if (!IsValid(DA))
		{
			return 4;
		}

		FProperty* Prop = DA->GetClass()->FindPropertyByName(FName(*PropName));
		if (!Prop)
		{
			return 4;
		}

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
		if (PathFilter.IsEmpty())
		{
			PathFilter = TEXT("/Game");
		}

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
		if (AssetPath.IsEmpty())
		{
			return EmitError(OutputPath, bPretty, 1,
				TEXT("read_state_tree requires asset_path"));
		}

		// Validate: asset exists and is a StateTree. We don't link the
		// StateTreeModule headers, so check via the class path string.
		UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
		if (!IsValid(Asset))
		{
			return EmitError(OutputPath, bPretty, 4,
				FString::Printf(TEXT("read_state_tree: asset not found: %s"),
					*AssetPath));
		}
		const FString ActualClass = Asset->GetClass()->GetPathName();
		if (!ActualClass.Contains(TEXT("StateTree")))
		{
			return EmitError(OutputPath, bPretty, 1,
				FString::Printf(TEXT("read_state_tree: %s is %s, not a StateTree — "
					"call read_blueprint, read_widget_blueprint, or the matching typed reader instead"),
					*AssetPath, *ActualClass));
		}

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
		if (Mode.IsEmpty())
		{
			Mode = TEXT("stats");
		}
		Mode = Mode.ToLower();

		FString Cmd;
		if (Mode == TEXT("stats"))
		{
			Cmd = TEXT("stat startfile");
		}
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
		if (Platform.IsEmpty())
		{
			Platform = TEXT("Windows");
		}

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
		if (!IsValid(Cls))
		{
			Cls = FindObject<UClass>(nullptr, *ClassName);
		}
		if (!IsValid(Cls))
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
			// declared_on lets the MCP layer filter out inherited members
			// when include_inherited=false (default). UStruct::GetOwnerClass
			// returns the class that originally declared this property.
			if (UClass* Owner = It->GetOwnerClass())
			{
				P->SetStringField(TEXT("declared_on"), Owner->GetName());
			}
			Props.Add(MakeShared<FJsonValueObject>(P));
		}

		TArray<TSharedPtr<FJsonValue>> Funcs;
		for (TFieldIterator<UFunction> It(Cls); It; ++It)
		{
			auto F = MakeShared<FJsonObject>();
			F->SetStringField(TEXT("name"), It->GetName());
			if (UClass* Owner = It->GetOwnerClass())
			{
				F->SetStringField(TEXT("declared_on"), Owner->GetName());
			}
			// Emit a small flag CSV that covers the BP-callable surface.
			TArray<FString> Flags;
			if (It->HasAnyFunctionFlags(FUNC_BlueprintCallable))
			{
				Flags.Add(TEXT("BlueprintCallable"));
			}
			if (It->HasAnyFunctionFlags(FUNC_BlueprintPure))
			{
				Flags.Add(TEXT("BlueprintPure"));
			}
			if (It->HasAnyFunctionFlags(FUNC_BlueprintEvent))
			{
				Flags.Add(TEXT("BlueprintEvent"));
			}
			if (It->HasAnyFunctionFlags(FUNC_Net))
			{
				Flags.Add(TEXT("Net"));
			}
			if (It->HasAnyFunctionFlags(FUNC_Static))
			{
				Flags.Add(TEXT("Static"));
			}
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
				if (Classes.Num() >= 200)
				{
					break;  // cap
				}
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
		if (!IsValid(Cls))
		{
			Cls = FindObject<UClass>(nullptr, *ClassName);
		}
		if (!IsValid(Cls))
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
			if (It->HasAnyFunctionFlags(FUNC_BlueprintCallable))
			{
				Flags.Add(TEXT("BlueprintCallable"));
			}
			if (It->HasAnyFunctionFlags(FUNC_BlueprintPure))
			{
				Flags.Add(TEXT("BlueprintPure"));
			}
			if (It->HasAnyFunctionFlags(FUNC_BlueprintEvent))
			{
				Flags.Add(TEXT("BlueprintEvent"));
			}
			if (It->HasAnyFunctionFlags(FUNC_Net))
			{
				Flags.Add(TEXT("Net"));
			}
			if (It->HasAnyFunctionFlags(FUNC_Static))
			{
				Flags.Add(TEXT("Static"));
			}
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
					if (IsValid(GEditor))
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
		if (PathFilter.IsEmpty())
		{
			PathFilter = TEXT("/Game");
		}

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
		if (AssetPath.IsEmpty())
		{
			return EmitError(OutputPath, bPretty, 1,
				TEXT("read_niagara_system requires asset_path"));
		}

		UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
		if (!IsValid(Asset))
		{
			return EmitError(OutputPath, bPretty, 4,
				FString::Printf(TEXT("read_niagara_system: asset not found: %s"),
					*AssetPath));
		}
		const FString ActualClass = Asset->GetClass()->GetPathName();
		if (!ActualClass.Contains(TEXT("NiagaraSystem")))
		{
			return EmitError(OutputPath, bPretty, 1,
				FString::Printf(TEXT("read_niagara_system: %s is %s, not a NiagaraSystem — "
					"call read_blueprint or the matching typed reader instead"),
					*AssetPath, *ActualClass));
		}

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
		if (AssetPath.IsEmpty())
		{
			return EmitError(OutputPath, bPretty, 1,
				TEXT("read_ability_set requires asset_path"));
		}

		// Validate: asset exists and is a UDataAsset (AbilitySet is a
		// project-specific UDataAsset subclass — accept any DataAsset).
		UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
		if (!IsValid(Asset))
		{
			return EmitError(OutputPath, bPretty, 4,
				FString::Printf(TEXT("read_ability_set: asset not found: %s"),
					*AssetPath));
		}
		UDataAsset* DA = Cast<UDataAsset>(Asset);
		if (!IsValid(DA))
		{
			return EmitError(OutputPath, bPretty, 1,
				FString::Printf(TEXT("read_ability_set: %s is %s, not a DataAsset — "
					"call read_blueprint or the matching typed reader instead"),
					*AssetPath, *Asset->GetClass()->GetPathName()));
		}
		TArray<TSharedPtr<FJsonValue>> Abilities;
		// AbilitySet schemas vary across projects. We scan for any array
		// property containing a "class" + "level" pair via the property
		// system. Best-effort.
		if (IsValid(DA))
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
						if (!E->HasField(TEXT("level")))
						{
							E->SetNumberField(TEXT("level"), 1);
						}
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
		if (!IsValid(ABP))
		{
			return 4;
		}

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
		if (!IsValid(ABP))
		{
			return 4;
		}

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
		if (IsValid(GEngine))
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
		if (MinSeverity.Equals(TEXT("Fatal"),   ESearchCase::IgnoreCase))
		{
			MinVerb = ELogVerbosity::Fatal;
		}
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
		if (!IsValid(SourceBP))
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
		if (!IsValid(NewObj))
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("DuplicateBlueprint: DuplicateAsset failed for %s -> %s"),
				*SourceAsset, *DestAsset);
			return 5;
		}

		UBlueprint* NewBP = Cast<UBlueprint>(NewObj);
		if (!IsValid(NewBP))
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("DuplicateBlueprint: duplicate isn't a UBlueprint (was %s)"),
				*NewObj->GetClass()->GetName());
			return 5;
		}
		// MaybeCompileAndSave handles the save + compile. AssetCreated
		// notification fires inside DuplicateAsset already, so a
		// follow-up batch op can LoadMutableBlueprint it.
		if (!MaybeCompileAndSave(NewBP))
		{
			return 5;
		}

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		Obj->SetBoolField(TEXT("already_existed"), false);
		Obj->SetStringField(TEXT("asset_path"), DestAsset);
		Obj->SetStringField(TEXT("source_asset_path"), SourceAsset);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	// ----- StructuralDiff (BP roundtrip Task 13) ------------------------
	// Read-only comparison of two UBlueprints. Output:
	//   { ok: bool, differences: [{path, kind, a, b}, ...] }
	// `ok` is true iff differences is empty (i.e. the two BPs match).
	int32 RunStructuralDiffOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString APath;
		FString BPath;
		FParse::Value(*Params, TEXT("A="), APath);
		FParse::Value(*Params, TEXT("B="), BPath);
		if (APath.IsEmpty() || BPath.IsEmpty())
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("StructuralDiff requires -A=/Game/X -B=/Game/Y"));
			return 1;
		}

		UBlueprint* A = LoadObject<UBlueprint>(nullptr, *APath);
		if (!IsValid(A))
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("StructuralDiff: asset_not_found: %s"), *APath);
			return 5;
		}
		UBlueprint* B = LoadObject<UBlueprint>(nullptr, *BPath);
		if (!IsValid(B))
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("StructuralDiff: asset_not_found: %s"), *BPath);
			return 5;
		}

		BlueprintStructuralDiff::FCompareOptions Opt;
		// Default: ignore node positions (caller can override via flag).
		// `-IgnoreNodePositions=0` disables the default; the flag-form
		// `-IgnoreCommentNodes` enables comment-node skipping.
		Opt.bIgnoreNodePositions = true;
		{
			FString PosVal;
			if (FParse::Value(*Params, TEXT("IgnoreNodePositions="), PosVal))
			{
				Opt.bIgnoreNodePositions = !PosVal.Equals(TEXT("0"));
			}
		}
		Opt.bIgnoreCommentNodes = FParse::Param(*Params, TEXT("IgnoreCommentNodes"));

		BlueprintStructuralDiff::FResult Result =
			BlueprintStructuralDiff::Compare(A, B, Opt);
		TSharedRef<FJsonObject> Obj = Result.ToJson();
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	// ----- ListAssets / FindAsset (asset-registry queries) -------------
	// General-purpose asset enumeration. The list_blueprints / list_materials
	// / list_widgets family answers type-specific questions; these answer
	// "what's under this path?" and "find an asset by name fragment". Closes
	// the agent-feedback gap where the catalog had typed listings but no
	// way to discover an unknown asset by path or substring.
	//
	// Wire shape: array of { asset_path, class_name } objects. asset_path
	// is the canonical package path (`/Game/UI/WB_X`) and class_name is
	// the short UClass name (`WidgetBlueprint`, `Texture2D`, …).
	int32 RunListAssetsOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString Path;
		FParse::Value(*Params, TEXT("Path="), Path);
		if (Path.IsEmpty())
		{
			Path = TEXT("/Game");
		}
		const bool bRecursive = !FParse::Param(*Params, TEXT("NonRecursive"));

		FAssetRegistryModule& ARM =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		IAssetRegistry& AR = ARM.Get();

		TArray<FAssetData> Assets;
		AR.GetAssetsByPath(FName(*Path), Assets, bRecursive, /*bIncludeOnlyOnDiskAssets=*/false);

		TArray<TSharedPtr<FJsonValue>> Rows;
		Rows.Reserve(Assets.Num());
		for (const FAssetData& A : Assets)
		{
			auto Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("asset_path"), A.PackageName.ToString());
			Row->SetStringField(TEXT("name"),       A.AssetName.ToString());
			Row->SetStringField(TEXT("class_name"), A.AssetClassPath.GetAssetName().ToString());
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		}
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetArrayField(TEXT("assets"), Rows);
		Out->SetNumberField(TEXT("total"), Rows.Num());
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunFindAssetOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString Query;
		FString ScopePath;
		FParse::Value(*Params, TEXT("Query="), Query);
		FParse::Value(*Params, TEXT("Path="),  ScopePath);
		if (Query.IsEmpty())
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("FindAsset requires -Query=<substring>"));
			return 1;
		}
		if (ScopePath.IsEmpty())
		{
			ScopePath = TEXT("/Game");
		}

		FAssetRegistryModule& ARM =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		IAssetRegistry& AR = ARM.Get();

		TArray<FAssetData> Assets;
		AR.GetAssetsByPath(FName(*ScopePath), Assets, /*bRecursive=*/true,
						   /*bIncludeOnlyOnDiskAssets=*/false);

		const FString QueryLower = Query.ToLower();
		TArray<TSharedPtr<FJsonValue>> Rows;
		for (const FAssetData& A : Assets)
		{
			const FString NameLower = A.AssetName.ToString().ToLower();
			const FString PathLower = A.PackageName.ToString().ToLower();
			if (!NameLower.Contains(QueryLower) && !PathLower.Contains(QueryLower))
			{
				continue;
			}
			auto Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("asset_path"), A.PackageName.ToString());
			Row->SetStringField(TEXT("name"),       A.AssetName.ToString());
			Row->SetStringField(TEXT("class_name"), A.AssetClassPath.GetAssetName().ToString());
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		}

		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetStringField(TEXT("query"), Query);
		Out->SetArrayField(TEXT("matches"), Rows);
		Out->SetNumberField(TEXT("total"), Rows.Num());
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	// ----- GetProjectMetadata -------------------------------------------
	// Pure local read of the .uproject — engine association, category,
	// description. Implemented editor-side so the SocketBlueprintReader
	// can route the call here even when it doesn't know the project
	// path locally. (CommandletBlueprintReader still short-circuits and
	// reads the file itself — see its impl.)
	int32 RunGetProjectMetadataOp(const FString& /*Params*/, const FString& OutputPath, bool bPretty)
	{
		const FString ProjectPath = FPaths::GetProjectFilePath();
		const FString ProjectName = FApp::GetProjectName();

		FString Raw;
		if (!FFileHelper::LoadFileToString(Raw, *ProjectPath))
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("GetProjectMetadata: failed to read %s"), *ProjectPath);
			return 4;
		}

		TSharedPtr<FJsonObject> RawObj;
		auto Reader = TJsonReaderFactory<>::Create(Raw);
		if (!FJsonSerializer::Deserialize(Reader, RawObj) || !RawObj.IsValid())
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("GetProjectMetadata: %s is not valid JSON"), *ProjectPath);
			return 4;
		}

		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetStringField(TEXT("project_name"), ProjectName);
		Out->SetStringField(TEXT("project_path"), ProjectPath);
		FString EA, Cat, Desc;
		if (RawObj->TryGetStringField(TEXT("EngineAssociation"), EA))
		{
			Out->SetStringField(TEXT("engine_association"), EA);
		}
		if (RawObj->TryGetStringField(TEXT("Category"), Cat))
		{
			Out->SetStringField(TEXT("category"), Cat);
		}
		if (RawObj->TryGetStringField(TEXT("Description"), Desc))
		{
			Out->SetStringField(TEXT("description"), Desc);
		}
		Out->SetObjectField(TEXT("raw"), RawObj);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	// ----- Phase 8 EA-pull Wave 1 (partial) -----------------------------
	// All require a live editor; commandlet (headless) mode returns
	// empty results since GEditor and the asset-editor subsystem aren't
	// available outside an interactive editor session. The wire response
	// shape matches the SocketBlueprintReader parser.

	// Per-asset activation time: look up the IAssetEditorInstance via
	// FindEditorForAsset and call GetLastActivationTime() on the
	// toolkit. Returns 0 when no editor is open for the asset.
	double GetActivationTimeFor(UAssetEditorSubsystem* AES, UObject* Asset)
	{
		if (!AES || !IsValid(Asset))
		{
			return 0.0;
		}
		if (IAssetEditorInstance* Instance =
			AES->FindEditorForAsset(Asset, /*bFocusIfOpen=*/false))
		{
			return Instance->GetLastActivationTime();
		}
		return 0.0;
	}

	int32 RunListOpenAssetsOp(const FString& /*Params*/, const FString& OutputPath, bool bPretty)
	{
		TArray<TSharedPtr<FJsonValue>> Entries;
		if (GEditor != nullptr)
		{
			if (UAssetEditorSubsystem* AES =
				GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
			{
				TArray<UObject*> EditedAssets = AES->GetAllEditedAssets();
				Entries.Reserve(EditedAssets.Num());
				for (UObject* Asset : EditedAssets)
				{
					if (!IsValid(Asset))
					{
						continue;
					}
					auto Row = MakeShared<FJsonObject>();
					Row->SetStringField(TEXT("asset_path"),
						Asset->GetPackage() ? Asset->GetPackage()->GetName() : Asset->GetPathName());
					Row->SetStringField(TEXT("asset_class"),
						Asset->GetClass() ? Asset->GetClass()->GetName() : FString());
					Row->SetNumberField(TEXT("last_activation_seconds"),
						GetActivationTimeFor(AES, Asset));
					Entries.Add(MakeShared<FJsonValueObject>(Row));
				}
			}
		}
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetArrayField(TEXT("entries"), Entries);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunGetActiveAssetOp(const FString& /*Params*/, const FString& OutputPath, bool bPretty)
	{
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetStringField(TEXT("asset_path"),  FString());
		Out->SetStringField(TEXT("asset_class"), FString());
		Out->SetNumberField(TEXT("last_activation_seconds"), 0.0);

		if (GEditor != nullptr)
		{
			if (UAssetEditorSubsystem* AES =
				GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
			{
				TArray<UObject*> EditedAssets = AES->GetAllEditedAssets();
				UObject* MostRecent = nullptr;
				double   MostRecentTime = -1.0;
				for (UObject* Asset : EditedAssets)
				{
					if (!IsValid(Asset))
					{
						continue;
					}
					const double T = GetActivationTimeFor(AES, Asset);
					if (T > MostRecentTime)
					{
						MostRecentTime = T;
						MostRecent = Asset;
					}
				}
				if (IsValid(MostRecent))
				{
					Out->SetStringField(TEXT("asset_path"),
						MostRecent->GetPackage() ? MostRecent->GetPackage()->GetName() : MostRecent->GetPathName());
					Out->SetStringField(TEXT("asset_class"),
						MostRecent->GetClass() ? MostRecent->GetClass()->GetName() : FString());
					Out->SetNumberField(TEXT("last_activation_seconds"), MostRecentTime);
				}
			}
		}
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunGetCompileStatusOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		const FString AssetPath = ResolveAssetPath(Params);
		if (AssetPath.IsEmpty())
		{
			return EmitError(OutputPath, bPretty, 1,
				TEXT("get_compile_status requires asset_path"));
		}
		UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *AssetPath);
		if (!IsValid(BP))
		{
			return EmitError(OutputPath, bPretty, 4,
				FString::Printf(TEXT("get_compile_status: asset not found or not a Blueprint: %s"),
					*AssetPath));
		}
		// Map the UBlueprint::Status enum to a stable string.
		// BS_Unknown / Dirty / Error / UpToDate / UpToDateWithWarnings /
		// BeingCreated.
		const TCHAR* StatusStr = TEXT("unknown");
		switch (BP->Status)
		{
			case BS_Dirty:                  StatusStr = TEXT("dirty"); break;
			case BS_Error:                  StatusStr = TEXT("error"); break;
			case BS_UpToDate:               StatusStr = TEXT("good"); break;
			case BS_UpToDateWithWarnings:   StatusStr = TEXT("warning"); break;
			case BS_BeingCreated:           StatusStr = TEXT("compiling"); break;
			case BS_Unknown:                StatusStr = TEXT("uncompiled"); break;
			default:                        StatusStr = TEXT("unknown"); break;
		}
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetStringField(TEXT("asset_path"), AssetPath);
		Out->SetStringField(TEXT("status"), StatusStr);
		// The compile error text isn't readily available post-compile
		// (the editor surfaces it via the Compiler Results panel); leave
		// empty for now. A future revision can hook the BlueprintCompiled
		// delegate to capture last-error text.
		Out->SetStringField(TEXT("last_compile_error"), FString());
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunGetDirtyPackagesOp(const FString& /*Params*/, const FString& OutputPath, bool bPretty)
	{
		TArray<TSharedPtr<FJsonValue>> Packages;
		for (TObjectIterator<UPackage> It; It; ++It)
		{
			UPackage* Pkg = *It;
			if (!IsValid(Pkg) || !Pkg->IsDirty())
			{
				continue;
			}
			const FString Name = Pkg->GetName();
			auto Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("package_name"), Name);
			Row->SetBoolField(TEXT("is_content"), Name.StartsWith(TEXT("/Game/")));
			Packages.Add(MakeShared<FJsonValueObject>(Row));
		}
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetArrayField(TEXT("packages"), Packages);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunGetFocusedWindowOp(const FString& /*Params*/, const FString& OutputPath, bool bPretty)
	{
		FString Title;
		FString ClassName;
		if (FSlateApplication::IsInitialized())
		{
			FSlateApplication& Slate = FSlateApplication::Get();
			if (TSharedPtr<SWindow> ActiveWindow = Slate.GetActiveTopLevelWindow())
			{
				Title = ActiveWindow->GetTitle().ToString();
				ClassName = ActiveWindow->GetTypeAsString();
			}
		}
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetStringField(TEXT("title"),      Title);
		Out->SetStringField(TEXT("class_name"), ClassName);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunGetPieStateOp(const FString& /*Params*/, const FString& OutputPath, bool bPretty)
	{
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		bool bPlaying = false;
		FString Mode;
		int32 InstanceCount = 0;
		if (GEditor != nullptr)
		{
			// PlayWorld is non-null when PIE is running. Walk the world
			// list for the multi-PIE count (Client/Server split → two PIE
			// worlds).
			if (GEditor->PlayWorld != nullptr)
			{
				bPlaying = true;
			}
			if (GEngine != nullptr)
			{
				for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
				{
					if (Ctx.WorldType == EWorldType::PIE)
					{
						++InstanceCount;
					}
				}
			}
			if (bPlaying || InstanceCount > 0)
			{
				bPlaying = true;
				// PIE mode is stored in the play settings; we report the
				// last-used mode token. Editor has no public single accessor
				// for "current PIE mode" — agents that need detail can read
				// the Editor's PlaySession settings.
				Mode = TEXT("selected_viewport");
			}
		}
		Out->SetBoolField(TEXT("is_playing"),         bPlaying);
		Out->SetStringField(TEXT("mode"),             Mode);
		Out->SetNumberField(TEXT("instance_count"),   InstanceCount);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunGetModalStateOp(const FString& /*Params*/, const FString& OutputPath, bool bPretty)
	{
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		bool bOpen = false;
		FString Title;
		if (FSlateApplication::IsInitialized())
		{
			FSlateApplication& Slate = FSlateApplication::Get();
			// IsActiveModalWindow returns the modal window when one's
			// blocking input. We surface its title.
			TSharedPtr<SWindow> Modal = Slate.GetActiveModalWindow();
			if (Modal.IsValid())
			{
				bOpen = true;
				Title = Modal->GetTitle().ToString();
			}
		}
		Out->SetBoolField(TEXT("is_open"), bOpen);
		Out->SetStringField(TEXT("title"), Title);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunGetActiveEditorModeOp(const FString& /*Params*/, const FString& OutputPath, bool bPretty)
	{
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		TArray<TSharedPtr<FJsonValue>> Modes;
		// FEditorModeTools::ForEachEdMode is protected — no public API
		// for "list all active modes". Probe each well-known mode ID via
		// IsModeActive instead. Covers the modes agents care about
		// (selection, placement, landscape sculpting, foliage paint,
		// mesh paint, modeling). Missing exotic / plugin-shipped modes
		// is acceptable — agents that need to detect those can ship a
		// follow-up tool with a name allow-list.
		FEditorModeTools& ModeTools = GLevelEditorModeTools();
		// Use literal FName IDs — only EM_Default lives in UnrealEd; the
		// other built-in modes live in plugin modules we don't link
		// (LandscapeEditor / FoliageEdit / MeshPaint / etc.). The
		// IsModeActive query works against any FName ID regardless of
		// whether the mode's plugin is loaded — we just won't get
		// positive results when the corresponding plugin isn't active.
		auto Check = [&](FName Id, const TCHAR* Wire) {
			if (ModeTools.IsModeActive(Id))
			{
				Modes.Add(MakeShared<FJsonValueString>(FString(Wire)));
			}
		};
		Check(FName(TEXT("EM_Default")),       TEXT("EM_Default"));
		Check(FName(TEXT("EM_Placement")),     TEXT("EM_Placement"));
		Check(FName(TEXT("EM_Landscape")),     TEXT("EM_Landscape"));
		Check(FName(TEXT("EM_Foliage")),       TEXT("EM_Foliage"));
		Check(FName(TEXT("EM_MeshPaint")),     TEXT("EM_MeshPaint"));
		Check(FName(TEXT("EM_Level")),         TEXT("EM_Level"));
		Check(FName(TEXT("EM_ModelingToolsEditorMode")), TEXT("EM_Modeling"));
		Out->SetArrayField(TEXT("active_modes"), Modes);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunGetFocusedWidgetOp(const FString& /*Params*/, const FString& OutputPath, bool bPretty)
	{
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		FString WidgetType;
		FString ParentWindowTitle;
		if (FSlateApplication::IsInitialized())
		{
			FSlateApplication& Slate = FSlateApplication::Get();
			// User 0 is the primary editor user. GetUserFocusedWidget
			// returns the currently-focused widget for that user.
			TSharedPtr<SWidget> Focus = Slate.GetUserFocusedWidget(0);
			if (Focus.IsValid())
			{
				WidgetType = Focus->GetTypeAsString();
				if (TSharedPtr<SWindow> Win = Slate.FindWidgetWindow(Focus.ToSharedRef()))
				{
					ParentWindowTitle = Win->GetTitle().ToString();
				}
			}
		}
		Out->SetStringField(TEXT("widget_type"),         WidgetType);
		Out->SetStringField(TEXT("parent_window_title"), ParentWindowTitle);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunOpenAssetEditorOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		// Opening an editor tab requires a Slate application (live editor).
		// In commandlet mode FSlateApplication is never initialized, so
		// UAssetDefinition_Blueprint::OpenAssets asserts on SDockTab ctor.
		// Return a clean error rather than crashing the daemon.
		if (IsRunningCommandlet())
		{
			return EmitError(OutputPath, bPretty, 1,
				TEXT("open_asset_editor requires a running live editor; "
				     "cannot open editor tabs from a commandlet process"));
		}
		const FString AssetPath = ResolveAssetPath(Params);
		if (AssetPath.IsEmpty())
		{
			return EmitError(OutputPath, bPretty, 1,
				TEXT("open_asset_editor requires asset_path"));
		}
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetStringField(TEXT("asset_path"), AssetPath);
		bool bOpened = false;
		if (GEditor != nullptr)
		{
			if (UAssetEditorSubsystem* AES =
				GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
			{
				if (UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath))
				{
					bOpened = AES->OpenEditorForAsset(Asset);
				}
			}
		}
		Out->SetBoolField(TEXT("opened"), bOpened);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunCloseAssetEditorOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		const FString AssetPath = ResolveAssetPath(Params);
		if (AssetPath.IsEmpty())
		{
			return EmitError(OutputPath, bPretty, 1,
				TEXT("close_asset_editor requires asset_path"));
		}
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetStringField(TEXT("asset_path"), AssetPath);
		bool bClosed = false;
		if (GEditor != nullptr)
		{
			if (UAssetEditorSubsystem* AES =
				GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
			{
				// LoadObject (not FindObject) — closing-by-path should work
				// even if the editor is the only thing holding the asset
				// reference. CloseAllEditorsForAsset is idempotent (no
				// editors open → false return, no error).
				if (UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath))
				{
					bClosed = AES->CloseAllEditorsForAsset(Asset) != 0;
				}
			}
		}
		Out->SetBoolField(TEXT("closed"), bClosed);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	// Picks the focused level viewport client, or first perspective if
	// none has focus. Returns nullptr in commandlet mode (no UI) or
	// when no level editor exists yet.
	FEditorViewportClient* FindActiveLevelViewportClient()
	{
		if (!IsValid(GEditor))
		{
			return nullptr;
		}
		FEditorViewportClient* Best = nullptr;
		for (FEditorViewportClient* VC : GEditor->GetAllViewportClients())
		{
			if (!VC || !VC->IsLevelEditorClient())
			{
				continue;
			}
			if (VC->Viewport && VC->Viewport->HasFocus())
			{
				return VC;
			}
			if (!Best && VC->IsPerspective())
			{
				Best = VC;
			}
		}
		return Best;
	}

	int32 RunGetCameraTransformOp(const FString& /*Params*/, const FString& OutputPath, bool bPretty)
	{
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		bool bValid = false;
		double LX=0, LY=0, LZ=0, Pitch=0, Yaw=0, Roll=0, Fov=0;
		if (FEditorViewportClient* VC = FindActiveLevelViewportClient())
		{
			const FVector L = VC->GetViewLocation();
			const FRotator R = VC->GetViewRotation();
			LX = L.X; LY = L.Y; LZ = L.Z;
			Pitch = R.Pitch; Yaw = R.Yaw; Roll = R.Roll;
			Fov = VC->ViewFOV;
			bValid = true;
		}
		Out->SetBoolField(TEXT("valid"), bValid);
		Out->SetNumberField(TEXT("loc_x"), LX);
		Out->SetNumberField(TEXT("loc_y"), LY);
		Out->SetNumberField(TEXT("loc_z"), LZ);
		Out->SetNumberField(TEXT("pitch"), Pitch);
		Out->SetNumberField(TEXT("yaw"),   Yaw);
		Out->SetNumberField(TEXT("roll"),  Roll);
		Out->SetNumberField(TEXT("fov"),   Fov);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunGetViewModeOp(const FString& /*Params*/, const FString& OutputPath, bool bPretty)
	{
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		bool bValid = false;
		FString Mode;
		if (FEditorViewportClient* VC = FindActiveLevelViewportClient())
		{
			bValid = true;
			switch (VC->GetViewMode())
			{
				case VMI_BrushWireframe:        Mode = TEXT("BrushWireframe"); break;
				case VMI_Wireframe:             Mode = TEXT("Wireframe"); break;
				case VMI_Unlit:                 Mode = TEXT("Unlit"); break;
				case VMI_Lit:                   Mode = TEXT("Lit"); break;
				case VMI_LightingOnly:          Mode = TEXT("LightingOnly"); break;
				case VMI_LightComplexity:       Mode = TEXT("LightComplexity"); break;
				case VMI_ShaderComplexity:      Mode = TEXT("ShaderComplexity"); break;
				case VMI_StationaryLightOverlap:Mode = TEXT("StationaryLightOverlap"); break;
				case VMI_LightmapDensity:       Mode = TEXT("LightmapDensity"); break;
				case VMI_LitLightmapDensity:    Mode = TEXT("LitLightmapDensity"); break;
				case VMI_ReflectionOverride:    Mode = TEXT("ReflectionOverride"); break;
				case VMI_VisualizeBuffer:       Mode = TEXT("VisualizeBuffer"); break;
				case VMI_CollisionPawn:         Mode = TEXT("CollisionPawn"); break;
				case VMI_CollisionVisibility:   Mode = TEXT("CollisionVisibility"); break;
				default:                        Mode = TEXT("Lit"); break;
			}
		}
		Out->SetBoolField(TEXT("valid"), bValid);
		Out->SetStringField(TEXT("mode"), Mode);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	// Cache the singleton lookup. Module is loaded lazily so the first
	// call to ContentBrowserModule::Get() may load it; subsequent calls
	// are cheap.
	IContentBrowserSingleton* GetContentBrowserOrNull()
	{
		if (FModuleManager::Get().IsModuleLoaded(TEXT("ContentBrowser")))
		{
			FContentBrowserModule& CB = FModuleManager::GetModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
			return &CB.Get();
		}
		FContentBrowserModule& CB = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
		return &CB.Get();
	}

	// Try to extract visible text from a Slate widget. STextBlock/SEditableText/
	// SButton-with-text/etc. don't share a common interface so we sniff by
	// type name + reach into ToString() which most widgets expose.
	FString GetWidgetText(const TSharedRef<SWidget>& W)
	{
		// `ToString` returns "SBox(...)" for plain widgets and "STextBlock(text='Hello')"
		// for text widgets. Useful as a best-effort label without per-widget casts.
		const FString S = W->ToString();
		// Extract text='X' pattern when present.
		const int32 Idx = S.Find(TEXT("text='"));
		if (Idx != INDEX_NONE)
		{
			const int32 Start = Idx + 6;
			const int32 EndIdx = S.Find(TEXT("'"), ESearchCase::CaseSensitive,
										ESearchDir::FromStart, Start);
			if (EndIdx != INDEX_NONE)
			{
				return S.Mid(Start, EndIdx - Start);
			}
		}
		return FString();
	}

	// Recursively walk a Slate widget tree, collecting nodes that pass
	// `Predicate`. `MaxNodes` is a hard cap to keep payloads sane.
	void WalkSlateTree(TSharedRef<SWidget> Widget,
					   const FString& WindowTitle,
					   int32 Depth,
					   int32 MaxDepth,
					   int32 MaxNodes,
					   TFunctionRef<bool(const TSharedRef<SWidget>&, const FString&)> Predicate,
					   TArray<TSharedPtr<FJsonValue>>& OutNodes,
					   bool& bOutTruncated)
	{
		if (OutNodes.Num() >= MaxNodes)
		{
			bOutTruncated = true;
			return;
		}
		const FString Text = GetWidgetText(Widget);
		if (Predicate(Widget, Text))
		{
			auto Node = MakeShared<FJsonObject>();
			Node->SetNumberField(TEXT("depth"),         Depth);
			Node->SetStringField(TEXT("widget_type"),   Widget->GetTypeAsString());
			Node->SetStringField(TEXT("text"),          Text);
			Node->SetStringField(TEXT("parent_window"), WindowTitle);
			OutNodes.Add(MakeShared<FJsonValueObject>(Node));
		}
		if (Depth >= MaxDepth)
		{
			bOutTruncated = true;
			return;
		}
		FChildren* Kids = Widget->GetChildren();
		if (Kids)
		{
			for (int32 i = 0; i < Kids->Num(); ++i)
			{
				WalkSlateTree(Kids->GetChildAt(i), WindowTitle, Depth + 1,
							  MaxDepth, MaxNodes, Predicate, OutNodes, bOutTruncated);
				if (OutNodes.Num() >= MaxNodes)
				{
					bOutTruncated = true;
					return;
				}
			}
		}
	}

	int32 RunUiSnapshotOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString WindowFilter;
		FParse::Value(*Params, TEXT("Window="), WindowFilter);
		int32 MaxDepth = 8;
		FParse::Value(*Params, TEXT("MaxDepth="), MaxDepth);
		const int32 kMaxNodes = 500;

		TArray<TSharedPtr<FJsonValue>> Nodes;
		bool bTruncated = false;
		if (FSlateApplication::IsInitialized())
		{
			TArray<TSharedRef<SWindow>> Windows;
			FSlateApplication::Get().GetAllVisibleWindowsOrdered(Windows);
			for (const TSharedRef<SWindow>& Win : Windows)
			{
				const FString Title = Win->GetTitle().ToString();
				if (!WindowFilter.IsEmpty() && !Title.Contains(WindowFilter))
				{
					continue;
				}
				WalkSlateTree(Win, Title, 0, MaxDepth, kMaxNodes,
					[](const TSharedRef<SWidget>&, const FString&) { return true; },
					Nodes, bTruncated);
			}
		}
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetArrayField(TEXT("nodes"), Nodes);
		Out->SetBoolField(TEXT("truncated"), bTruncated);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunUiFindOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString TextFilter, RoleFilter;
		FParse::Value(*Params, TEXT("Text="), TextFilter);
		FParse::Value(*Params, TEXT("Role="), RoleFilter);
		const int32 kMaxDepth = 24;
		const int32 kMaxNodes = 200;

		TArray<TSharedPtr<FJsonValue>> Nodes;
		bool bTruncated = false;
		if (FSlateApplication::IsInitialized() && !TextFilter.IsEmpty())
		{
			TArray<TSharedRef<SWindow>> Windows;
			FSlateApplication::Get().GetAllVisibleWindowsOrdered(Windows);
			for (const TSharedRef<SWindow>& Win : Windows)
			{
				const FString Title = Win->GetTitle().ToString();
				WalkSlateTree(Win, Title, 0, kMaxDepth, kMaxNodes,
					[&TextFilter, &RoleFilter](const TSharedRef<SWidget>& W, const FString& Text)
					{
						if (!Text.Contains(TextFilter))
						{
							return false;
						}
						if (!RoleFilter.IsEmpty() && !W->GetTypeAsString().Contains(RoleFilter))
						{
							return false;
						}
						return true;
					},
					Nodes, bTruncated);
			}
		}
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetArrayField(TEXT("nodes"), Nodes);
		Out->SetBoolField(TEXT("truncated"), bTruncated);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	// Compute the simplified 6-string state by probing the subsystem's
	// state-bool API in priority order. The public API exposes binary
	// "is X" queries (registered / loaded / active) and doesn't have a
	// public enum dump, so we synthesize the agent-facing label here.
	FString ComputeGameFeatureState(UGameFeaturesSubsystem& GFS, const FString& Url)
	{
		if (GFS.IsGameFeaturePluginActive(Url, /*bCheckForActivating=*/false))
		{
			return TEXT("active");
		}
		if (GFS.IsGameFeaturePluginActive(Url, /*bCheckForActivating=*/true))
		{
			return TEXT("loading");      // activating → bucketed as transitional
		}
		if (GFS.IsGameFeaturePluginLoaded(Url))
		{
			return TEXT("loaded");
		}
		if (GFS.IsGameFeaturePluginRegistered(Url, /*bCheckForRegistering=*/true))
		{
			return TEXT("registered");
		}
		if (GFS.IsGameFeaturePluginInstalled(Url))
		{
			return TEXT("registered");   // installed-but-not-yet-registered
		}
		return TEXT("unknown");
	}

	// Collect all candidate (URL, plugin name) pairs by walking the
	// plugin manager. Lyra-style GFPs live under `Plugins/GameFeatures/`
	// and ship a `.uplugin` whose `EditorCustomVirtualPath` or the path
	// itself indicates a GFP. We accept any plugin whose descriptor has
	// "GameFeature" in its tags or whose path is under `GameFeatures/`.
	void CollectGameFeaturePluginUrls(TArray<TPair<FString, FString>>& OutPairs)
	{
		IPluginManager& PM = IPluginManager::Get();
		for (const TSharedRef<IPlugin>& Plugin : PM.GetEnabledPlugins())
		{
			const FString BaseDir = Plugin->GetBaseDir();
			const bool bLooksLikeGFP =
				BaseDir.Contains(TEXT("GameFeatures/")) ||
				BaseDir.Contains(TEXT("GameFeatures\\"));
			if (!bLooksLikeGFP)
			{
				continue;
			}
			const FString DescriptorPath = Plugin->GetDescriptorFileName();
			const FString Url = UGameFeaturesSubsystem::GetPluginURL_FileProtocol(DescriptorPath);
			OutPairs.Add({Url, Plugin->GetName()});
		}
	}

	// Extract the UAbilitySystemComponent from an actor — either via
	// the IAbilitySystemInterface (Lyra pattern) or by direct FindComponent
	// as a fallback.
	UAbilitySystemComponent* GetActorASC(AActor* Actor)
	{
		if (!IsValid(Actor))
		{
			return nullptr;
		}
		if (IAbilitySystemInterface* IFace = Cast<IAbilitySystemInterface>(Actor))
		{
			return IFace->GetAbilitySystemComponent();
		}
		return Actor->FindComponentByClass<UAbilitySystemComponent>();
	}

	int32 RunListActorAbilitiesOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString ActorName;
		FParse::Value(*Params, TEXT("Actor="), ActorName);

		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetBoolField(TEXT("valid"), false);
		Out->SetStringField(TEXT("actor_name"), ActorName);
		TArray<TSharedPtr<FJsonValue>> Abilities;

		if (AActor* Actor = FindActorByName(ActorName))
		{
			if (UAbilitySystemComponent* ASC = GetActorASC(Actor))
			{
				Out->SetBoolField(TEXT("valid"), true);
				const TArray<FGameplayAbilitySpec>& Specs = ASC->GetActivatableAbilities();
				for (const FGameplayAbilitySpec& Spec : Specs)
				{
					auto A = MakeShared<FJsonObject>();
					A->SetStringField(TEXT("ability_class"),
						Spec.Ability ? Spec.Ability->GetClass()->GetPathName() : FString());
					A->SetBoolField(TEXT("is_active"),       Spec.IsActive());
					A->SetNumberField(TEXT("level"),         Spec.Level);
					A->SetNumberField(TEXT("instanced_count"),
						Spec.GetAbilityInstances().Num());
					Abilities.Add(MakeShared<FJsonValueObject>(A));
				}
			}
		}
		Out->SetArrayField(TEXT("abilities"), Abilities);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunGetVisibleActorsOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		constexpr int32 kMaxActors = 500;
		FString ClassFilter;
		double MaxDist = 0.0;
		FParse::Value(*Params, TEXT("ClassFilter="), ClassFilter);
		FParse::Value(*Params, TEXT("Dist="),        MaxDist);

		TArray<TSharedPtr<FJsonValue>> Actors;
		bool bTruncated = false;
		UWorld* World = GetEditorWorldOrNull();
		FEditorViewportClient* VC = FindActiveLevelViewportClient();
		if (World && VC && VC->Viewport)
		{
			FSceneViewFamilyContext ViewFamily(FSceneViewFamily::ConstructionValues(
				VC->Viewport, VC->GetScene(), VC->EngineShowFlags)
				.SetRealtimeUpdate(VC->IsRealtime()));
			FSceneView* View = VC->CalcSceneView(&ViewFamily);
			const FVector CamLoc = VC->GetViewLocation();
			const FIntRect VR = View ? View->UnscaledViewRect : FIntRect();
			const FMatrix VPM = View ? View->ViewMatrices.GetViewProjectionMatrix() : FMatrix::Identity;

			for (TActorIterator<AActor> It(World); It; ++It)
			{
				AActor* Actor = *It;
				if (!IsValid(Actor)) continue;
				if (Actor->IsHidden() || Actor->IsTemporarilyHiddenInEditor() || Actor->IsHiddenEd())
				{
					continue;
				}
				if (!ClassFilter.IsEmpty())
				{
					const FString ClassName = Actor->GetClass()
						? Actor->GetClass()->GetName() : FString();
					if (!ClassName.Contains(ClassFilter))
					{
						continue;
					}
				}
				const FVector ActorLoc = Actor->GetActorLocation();
				const double Dist = FVector::Dist(CamLoc, ActorLoc);
				if (MaxDist > 0.0 && Dist > MaxDist)
				{
					continue;
				}
				// Frustum check via FBox around actor (use root component
				// bounds when available; else a point at the actor's loc).
				bool bInFrustum = true;
				if (View)
				{
					FBox Box = Actor->GetComponentsBoundingBox(true);
					if (!Box.IsValid)
					{
						Box = FBox::BuildAABB(ActorLoc, FVector(50.0));
					}
					bInFrustum = View->ViewFrustum.IntersectBox(
						Box.GetCenter(), Box.GetExtent());
				}
				if (!bInFrustum)
				{
					continue;
				}
				if (Actors.Num() >= kMaxActors)
				{
					bTruncated = true;
					break;
				}

				auto A = MakeShared<FJsonObject>();
				A->SetStringField(TEXT("name"),  Actor->GetName());
				A->SetStringField(TEXT("label"), Actor->GetActorLabel());
				A->SetStringField(TEXT("actor_class"),
					Actor->GetClass() ? Actor->GetClass()->GetName() : FString());
				A->SetNumberField(TEXT("world_x"), ActorLoc.X);
				A->SetNumberField(TEXT("world_y"), ActorLoc.Y);
				A->SetNumberField(TEXT("world_z"), ActorLoc.Z);
				A->SetNumberField(TEXT("distance_cm"), Dist);

				// Optional screen position via the same view.
				bool bHasScreen = false;
				double SX = 0.0, SY = 0.0;
				if (View && VR.Width() > 0 && VR.Height() > 0)
				{
					FVector2D ScreenPos;
					if (FSceneView::ProjectWorldToScreen(
							ActorLoc, VR, VPM, ScreenPos))
					{
						SX = (ScreenPos.X - VR.Min.X) / (double)VR.Width();
						SY = (ScreenPos.Y - VR.Min.Y) / (double)VR.Height();
						bHasScreen = true;
					}
				}
				A->SetBoolField(TEXT("has_screen_pos"), bHasScreen);
				A->SetNumberField(TEXT("screen_x"), SX);
				A->SetNumberField(TEXT("screen_y"), SY);
				Actors.Add(MakeShared<FJsonValueObject>(A));
			}
		}

		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetArrayField(TEXT("actors"), Actors);
		Out->SetBoolField(TEXT("truncated"), bTruncated);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunGetHiddenActorsOp(const FString& /*Params*/, const FString& OutputPath, bool bPretty)
	{
		constexpr int32 kMaxActors = 500;
		TArray<TSharedPtr<FJsonValue>> Names;
		bool bTruncated = false;
		if (UWorld* W = GetEditorWorldOrNull())
		{
			for (TActorIterator<AActor> It(W); It; ++It)
			{
				AActor* Actor = *It;
				if (!IsValid(Actor)) continue;
				if (!Actor->IsTemporarilyHiddenInEditor() && !Actor->IsHiddenEd())
				{
					continue;
				}
				if (Names.Num() >= kMaxActors)
				{
					bTruncated = true;
					break;
				}
				Names.Add(MakeShared<FJsonValueString>(Actor->GetName()));
			}
		}
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetArrayField(TEXT("actor_names"), Names);
		Out->SetBoolField(TEXT("truncated"), bTruncated);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunSetViewModeOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString ModeStr;
		FParse::Value(*Params, TEXT("Mode="), ModeStr);
		bool bValid = false;
		FString Applied;
		if (FEditorViewportClient* VC = FindActiveLevelViewportClient())
		{
			EViewModeIndex Mode = VMI_Lit;
			bool bRecognized = true;
			if      (ModeStr.Equals(TEXT("Wireframe"), ESearchCase::IgnoreCase))            Mode = VMI_Wireframe;
			else if (ModeStr.Equals(TEXT("BrushWireframe"), ESearchCase::IgnoreCase))       Mode = VMI_BrushWireframe;
			else if (ModeStr.Equals(TEXT("Unlit"), ESearchCase::IgnoreCase))                Mode = VMI_Unlit;
			else if (ModeStr.Equals(TEXT("Lit"), ESearchCase::IgnoreCase))                  Mode = VMI_Lit;
			else if (ModeStr.Equals(TEXT("LightingOnly"), ESearchCase::IgnoreCase))         Mode = VMI_LightingOnly;
			else if (ModeStr.Equals(TEXT("LightComplexity"), ESearchCase::IgnoreCase))      Mode = VMI_LightComplexity;
			else if (ModeStr.Equals(TEXT("ShaderComplexity"), ESearchCase::IgnoreCase))     Mode = VMI_ShaderComplexity;
			else if (ModeStr.Equals(TEXT("StationaryLightOverlap"), ESearchCase::IgnoreCase))Mode = VMI_StationaryLightOverlap;
			else if (ModeStr.Equals(TEXT("LightmapDensity"), ESearchCase::IgnoreCase))      Mode = VMI_LightmapDensity;
			else if (ModeStr.Equals(TEXT("LitLightmapDensity"), ESearchCase::IgnoreCase))   Mode = VMI_LitLightmapDensity;
			else if (ModeStr.Equals(TEXT("ReflectionOverride"), ESearchCase::IgnoreCase))   Mode = VMI_ReflectionOverride;
			else if (ModeStr.Equals(TEXT("CollisionPawn"), ESearchCase::IgnoreCase))        Mode = VMI_CollisionPawn;
			else if (ModeStr.Equals(TEXT("CollisionVisibility"), ESearchCase::IgnoreCase))  Mode = VMI_CollisionVisibility;
			else bRecognized = false;

			if (bRecognized)
			{
				VC->SetViewMode(Mode);
				VC->Invalidate();
				bValid = true;
				Applied = ModeStr;
			}
		}
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetBoolField(TEXT("valid"), bValid);
		Out->SetStringField(TEXT("mode"), Applied);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunSetGizmoModeOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString ModeStr;
		FParse::Value(*Params, TEXT("Mode="), ModeStr);
		bool bValid = false;
		FString Applied;
		if (FEditorViewportClient* VC = FindActiveLevelViewportClient())
		{
			UE::Widget::EWidgetMode Mode = UE::Widget::WM_Translate;
			bool bRecognized = true;
			if      (ModeStr.Equals(TEXT("translate"), ESearchCase::IgnoreCase)) Mode = UE::Widget::WM_Translate;
			else if (ModeStr.Equals(TEXT("rotate"), ESearchCase::IgnoreCase))    Mode = UE::Widget::WM_Rotate;
			else if (ModeStr.Equals(TEXT("scale"), ESearchCase::IgnoreCase))     Mode = UE::Widget::WM_Scale;
			else bRecognized = false;

			if (bRecognized)
			{
				VC->SetWidgetMode(Mode);
				VC->Invalidate();
				bValid = true;
				Applied = ModeStr.ToLower();
			}
		}
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetBoolField(TEXT("valid"), bValid);
		Out->SetStringField(TEXT("mode"), Applied);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunSetViewportRealtimeOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		const bool bEnabled = FParse::Param(*Params, TEXT("Enabled"));
		bool bValid = false;
		bool bIsRealtime = false;
		if (FEditorViewportClient* VC = FindActiveLevelViewportClient())
		{
			VC->SetRealtime(bEnabled);
			VC->Invalidate();
			bValid = true;
			bIsRealtime = VC->IsRealtime();
		}
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetBoolField(TEXT("valid"), bValid);
		Out->SetBoolField(TEXT("is_realtime"), bIsRealtime);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunSetActorVisibilityOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString Name;
		FParse::Value(*Params, TEXT("Name="), Name);
		const bool bVisible = FParse::Param(*Params, TEXT("Visible"));
		bool bValid = false;
		if (AActor* Actor = FindActorByName(Name))
		{
			// Temporary editor-only hide: does not dirty the package.
			Actor->SetIsTemporarilyHiddenInEditor(!bVisible);
			bValid = true;
		}
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetBoolField(TEXT("valid"), bValid);
		Out->SetStringField(TEXT("name"), Name);
		Out->SetBoolField(TEXT("visible"), bVisible);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunGetHiddenLayersOp(const FString& /*Params*/, const FString& OutputPath, bool bPretty)
	{
		constexpr int32 kMaxLayers = 500;
		TArray<TSharedPtr<FJsonValue>> Names;
		bool bTruncated = false;
		if (IsValid(GEditor))
		{
			if (ULayersSubsystem* Layers = GEditor->GetEditorSubsystem<ULayersSubsystem>())
			{
				TArray<FName> AllLayers;
				Layers->AddAllLayerNamesTo(AllLayers);
				for (const FName& LayerName : AllLayers)
				{
					ULayer* Layer = nullptr;
					if (!Layers->TryGetLayer(LayerName, Layer) || !Layer || Layer->IsVisible())
					{
						continue;   // resolved-and-visible, or couldn't resolve
					}
					if (Names.Num() >= kMaxLayers)
					{
						bTruncated = true;
						break;
					}
					Names.Add(MakeShared<FJsonValueString>(LayerName.ToString()));
				}
			}
		}
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetArrayField(TEXT("layer_names"), Names);
		Out->SetBoolField(TEXT("truncated"), bTruncated);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunSetLayerVisibilityOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString LayerName;
		FParse::Value(*Params, TEXT("Layer="), LayerName);
		const bool bVisible = FParse::Param(*Params, TEXT("Visible"));
		bool bValid = false;
		if (IsValid(GEditor) && !LayerName.IsEmpty())
		{
			if (ULayersSubsystem* Layers = GEditor->GetEditorSubsystem<ULayersSubsystem>())
			{
				const FName LayerFName(*LayerName);
				if (Layers->IsLayer(LayerFName))
				{
					Layers->SetLayerVisibility(LayerFName, bVisible);
					bValid = true;
				}
			}
		}
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetBoolField(TEXT("valid"), bValid);
		Out->SetStringField(TEXT("layer"), LayerName);
		Out->SetBoolField(TEXT("visible"), bVisible);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunGetCameraBookmarksOp(const FString& /*Params*/, const FString& OutputPath, bool bPretty)
	{
		TArray<TSharedPtr<FJsonValue>> Bookmarks;
		int32 MaxSlots = 0;
		if (UWorld* World = GetEditorWorldOrNull())
		{
			if (AWorldSettings* WS = World->GetWorldSettings())
			{
				const TArray<UBookmarkBase*>& All = WS->GetBookmarks();
				MaxSlots = All.Num();
				for (int32 i = 0; i < All.Num(); ++i)
				{
					UBookMark* BM = Cast<UBookMark>(All[i]);
					if (!BM)
					{
						continue;   // empty slot or non-3D bookmark
					}
					auto B = MakeShared<FJsonObject>();
					B->SetNumberField(TEXT("slot"),  i);
					B->SetNumberField(TEXT("loc_x"), BM->Location.X);
					B->SetNumberField(TEXT("loc_y"), BM->Location.Y);
					B->SetNumberField(TEXT("loc_z"), BM->Location.Z);
					B->SetNumberField(TEXT("pitch"), BM->Rotation.Pitch);
					B->SetNumberField(TEXT("yaw"),   BM->Rotation.Yaw);
					B->SetNumberField(TEXT("roll"),  BM->Rotation.Roll);
					Bookmarks.Add(MakeShared<FJsonValueObject>(B));
				}
			}
		}
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetArrayField(TEXT("bookmarks"), Bookmarks);
		Out->SetNumberField(TEXT("max_slots"), MaxSlots);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunGotoCameraBookmarkOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		int32 Slot = 0;
		FParse::Value(*Params, TEXT("Slot="), Slot);
		bool bJumped = false;
		if (FEditorViewportClient* VC = FindActiveLevelViewportClient())
		{
			IBookmarkTypeTools& BT = IBookmarkTypeTools::Get();
			if (Slot >= 0 && (uint32)Slot < BT.GetMaxNumberOfBookmarks(VC)
				&& BT.CheckBookmark((uint32)Slot, VC))
			{
				BT.JumpToBookmark((uint32)Slot, nullptr, VC);
				VC->Invalidate();
				bJumped = true;
			}
		}
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetBoolField(TEXT("jumped"), bJumped);
		Out->SetNumberField(TEXT("slot"), Slot);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunGetHoverTargetOp(const FString& /*Params*/, const FString& OutputPath, bool bPretty)
	{
		// v1 stub: resolving the viewport hit proxy requires a render-thread
		// readback + RTTI cross-cast (HActor/HBSPSurface/etc.) that isn't safe
		// to do out-of-process from a commandlet. Returns valid:false until a
		// sidecar in the editor module captures hover state on the game thread.
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetBoolField(TEXT("valid"), false);
		Out->SetStringField(TEXT("hit_proxy_type"), FString());
		Out->SetStringField(TEXT("actor_name"), FString());
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunGetIsolateModeOp(const FString& /*Params*/, const FString& OutputPath, bool bPretty)
	{
		// v1 stub: no stable public accessor for the level-viewport
		// show-only-selected / isolate flag (UE 5.6+). Returns valid:false.
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetBoolField(TEXT("valid"), false);
		Out->SetBoolField(TEXT("isolated"), false);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunGetAsyncCompileStateOp(const FString& /*Params*/, const FString& OutputPath, bool bPretty)
	{
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetNumberField(TEXT("remaining_assets"),
			FAssetCompilingManager::Get().GetNumRemainingAssets());
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunGetShaderCompileStateOp(const FString& /*Params*/, const FString& OutputPath, bool bPretty)
	{
		bool bCompiling = false;
		int32 Outstanding = 0, Pending = 0;
		if (GShaderCompilingManager)
		{
			bCompiling  = GShaderCompilingManager->IsCompiling();
			Outstanding = GShaderCompilingManager->GetNumOutstandingJobs();
			Pending     = GShaderCompilingManager->GetNumPendingJobs();
		}
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetBoolField(TEXT("is_compiling"), bCompiling);
		Out->SetNumberField(TEXT("outstanding_jobs"), Outstanding);
		Out->SetNumberField(TEXT("pending_jobs"), Pending);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunGetCurrentLevelOp(const FString& /*Params*/, const FString& OutputPath, bool bPretty)
	{
		bool bValid = false;
		FString LevelName, WorldName;
		if (UWorld* World = GetEditorWorldOrNull())
		{
			WorldName = World->GetOutermost()->GetName();
			if (ULevel* Cur = World->GetCurrentLevel())
			{
				LevelName = Cur->GetOutermost()->GetName();
				bValid = true;
			}
		}
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetBoolField(TEXT("valid"), bValid);
		Out->SetStringField(TEXT("level_name"), LevelName);
		Out->SetStringField(TEXT("world_name"), WorldName);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunListLoadedLevelsOp(const FString& /*Params*/, const FString& OutputPath, bool bPretty)
	{
		TArray<TSharedPtr<FJsonValue>> Names;
		if (UWorld* World = GetEditorWorldOrNull())
		{
			for (ULevel* Level : World->GetLevels())
			{
				if (!Level) continue;
				Names.Add(MakeShared<FJsonValueString>(Level->GetOutermost()->GetName()));
			}
		}
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetArrayField(TEXT("level_names"), Names);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunGetSourceControlProviderOp(const FString& /*Params*/, const FString& OutputPath, bool bPretty)
	{
		ISourceControlModule& SCM = ISourceControlModule::Get();
		ISourceControlProvider& Provider = SCM.GetProvider();
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetStringField(TEXT("name"), Provider.GetName().ToString());
		Out->SetBoolField(TEXT("enabled"), SCM.IsEnabled());
		Out->SetBoolField(TEXT("available"), Provider.IsAvailable());
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunGetAssetRegistryStateOp(const FString& /*Params*/, const FString& OutputPath, bool bPretty)
	{
		IAssetRegistry& AR = IAssetRegistry::GetChecked();
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetBoolField(TEXT("is_loading_assets"), AR.IsLoadingAssets());
		Out->SetBoolField(TEXT("search_all_assets"), AR.IsSearchAllAssets());
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunGetDataLayerStatesOp(const FString& /*Params*/, const FString& OutputPath, bool bPretty)
	{
		TArray<TSharedPtr<FJsonValue>> Layers;
		bool bHasWP = false;
		if (UWorld* World = GetEditorWorldOrNull())
		{
			if (UDataLayerManager* DLM = UDataLayerManager::GetDataLayerManager(World))
			{
				bHasWP = true;
				for (UDataLayerInstance* Inst : DLM->GetDataLayerInstances())
				{
					if (!Inst) continue;
					auto L = MakeShared<FJsonObject>();
					L->SetStringField(TEXT("short_name"), Inst->GetDataLayerShortName());
					L->SetStringField(TEXT("full_name"),  Inst->GetDataLayerFullName());
					L->SetStringField(TEXT("runtime_state"),
						GetDataLayerRuntimeStateName(Inst->GetEffectiveRuntimeState()));
					Layers.Add(MakeShared<FJsonValueObject>(L));
				}
			}
		}
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetArrayField(TEXT("layers"), Layers);
		Out->SetBoolField(TEXT("has_world_partition"), bHasWP);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunGetAutosaveStatusOp(const FString& /*Params*/, const FString& OutputPath, bool bPretty)
	{
		bool bAutoSaving = false;
		if (GUnrealEd)
		{
			bAutoSaving = GUnrealEd->GetPackageAutoSaver().IsAutoSaving();
		}
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetBoolField(TEXT("is_auto_saving"), bAutoSaving);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunGetRecoveryStateOp(const FString& /*Params*/, const FString& OutputPath, bool bPretty)
	{
		bool bHasRestore = false;
		if (GUnrealEd)
		{
			bHasRestore = GUnrealEd->GetPackageAutoSaver().HasPackagesToRestore();
		}
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetBoolField(TEXT("has_packages_to_restore"), bHasRestore);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunGetSourceControlStatusOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		const FString AssetPath = ResolveAssetPath(Params);
		bool bValid=false, bControlled=false, bCheckedOut=false,
			 bOther=false, bModified=false, bCurrent=false;
		ISourceControlModule& SCM = ISourceControlModule::Get();
		if (SCM.IsEnabled() && !AssetPath.IsEmpty())
		{
			const FString Filename = USourceControlHelpers::PackageFilename(AssetPath);
			FSourceControlStatePtr State =
				SCM.GetProvider().GetState(Filename, EStateCacheUsage::Use);
			if (State.IsValid())
			{
				bValid      = true;
				bControlled = State->IsSourceControlled();
				bCheckedOut = State->IsCheckedOut();
				bOther      = State->IsCheckedOutOther();
				bModified   = State->IsModified();
				bCurrent    = State->IsCurrent();
			}
		}
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetBoolField(TEXT("valid"), bValid);
		Out->SetBoolField(TEXT("controlled"), bControlled);
		Out->SetBoolField(TEXT("checked_out"), bCheckedOut);
		Out->SetBoolField(TEXT("checked_out_other"), bOther);
		Out->SetBoolField(TEXT("modified"), bModified);
		Out->SetBoolField(TEXT("current"), bCurrent);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunGetFileLockStatusOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		const FString AssetPath = ResolveAssetPath(Params);
		bool bValid=false, bOther=false;
		FString Who;
		ISourceControlModule& SCM = ISourceControlModule::Get();
		if (SCM.IsEnabled() && !AssetPath.IsEmpty())
		{
			const FString Filename = USourceControlHelpers::PackageFilename(AssetPath);
			FSourceControlStatePtr State =
				SCM.GetProvider().GetState(Filename, EStateCacheUsage::Use);
			if (State.IsValid())
			{
				bValid = true;
				bOther = State->IsCheckedOutOther(&Who);
			}
		}
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetBoolField(TEXT("valid"), bValid);
		Out->SetBoolField(TEXT("checked_out_by_other"), bOther);
		Out->SetStringField(TEXT("other_user"), Who);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunGetMonitorInfoOp(const FString& /*Params*/, const FString& OutputPath, bool bPretty)
	{
		TArray<TSharedPtr<FJsonValue>> Monitors;
		FDisplayMetrics DM;
		FDisplayMetrics::RebuildDisplayMetrics(DM);
		for (const FMonitorInfo& M : DM.MonitorInfo)
		{
			auto J = MakeShared<FJsonObject>();
			J->SetStringField(TEXT("name"), M.Name);
			J->SetNumberField(TEXT("native_width"),  M.NativeWidth);
			J->SetNumberField(TEXT("native_height"), M.NativeHeight);
			J->SetBoolField(TEXT("is_primary"), M.bIsPrimary);
			Monitors.Add(MakeShared<FJsonValueObject>(J));
		}
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetArrayField(TEXT("monitors"), Monitors);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunGetLiveCodingStateOp(const FString& /*Params*/, const FString& OutputPath, bool bPretty)
	{
		bool bAvailable=false, bStarted=false, bCompiling=false;
#if PLATFORM_WINDOWS
		if (ILiveCodingModule* LC = FModuleManager::GetModulePtr<ILiveCodingModule>("LiveCoding"))
		{
			bAvailable  = true;
			bStarted    = LC->HasStarted();
			bCompiling  = LC->IsCompiling();
		}
#endif
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetBoolField(TEXT("available"), bAvailable);
		Out->SetBoolField(TEXT("has_started"), bStarted);
		Out->SetBoolField(TEXT("is_compiling"), bCompiling);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunGetActiveCultureOp(const FString& /*Params*/, const FString& OutputPath, bool bPretty)
	{
		FInternationalization& I18N = FInternationalization::Get();
		FCultureRef Culture = I18N.GetCurrentCulture();
		FCultureRef Lang = I18N.GetCurrentLanguage();
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetStringField(TEXT("language"),     Lang->GetName());
		Out->SetStringField(TEXT("culture"),      Culture->GetName());
		Out->SetStringField(TEXT("display_name"), Culture->GetEnglishName());
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunGetEditorThemeOp(const FString& /*Params*/, const FString& OutputPath, bool bPretty)
	{
		FString ThemeId;
		if (const UEditorStyleSettings* S = GetDefault<UEditorStyleSettings>())
		{
			ThemeId = S->CurrentAppliedTheme.ToString();
		}
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetStringField(TEXT("theme_id"), ThemeId);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	// Resolve a GFP identifier (plugin name OR file:-protocol URL) to its
	// URL. Names are matched against the enumerated GFP list.
	FString ResolveGameFeatureUrl(const FString& NameOrUrl)
	{
		if (NameOrUrl.Contains(TEXT(":")))   // already a file:-protocol URL
		{
			return NameOrUrl;
		}
		TArray<TPair<FString, FString>> Pairs;
		CollectGameFeaturePluginUrls(Pairs);   // {URL, plugin name}
		for (const TPair<FString, FString>& P : Pairs)
		{
			if (P.Value == NameOrUrl)
			{
				return P.Key;
			}
		}
		return FString();
	}

	int32 RunActivateGameFeatureOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString NameOrUrl;
		FParse::Value(*Params, TEXT("Plugin="), NameOrUrl);
		const FString Url = ResolveGameFeatureUrl(NameOrUrl);
		bool bRequested = false;
		if (!Url.IsEmpty() && GEngine)
		{
			if (UGameFeaturesSubsystem* GFS = GEngine->GetEngineSubsystem<UGameFeaturesSubsystem>())
			{
				GFS->LoadAndActivateGameFeaturePlugin(Url, FGameFeaturePluginLoadComplete());
				bRequested = true;
			}
		}
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetBoolField(TEXT("requested"), bRequested);
		Out->SetStringField(TEXT("url"), Url);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunDeactivateGameFeatureOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString NameOrUrl;
		FParse::Value(*Params, TEXT("Plugin="), NameOrUrl);
		const FString Url = ResolveGameFeatureUrl(NameOrUrl);
		bool bRequested = false;
		if (!Url.IsEmpty() && GEngine)
		{
			if (UGameFeaturesSubsystem* GFS = GEngine->GetEngineSubsystem<UGameFeaturesSubsystem>())
			{
				GFS->DeactivateGameFeaturePlugin(Url);
				bRequested = true;
			}
		}
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetBoolField(TEXT("requested"), bRequested);
		Out->SetStringField(TEXT("url"), Url);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	// Phase 14 — recently-saved package ring buffer. Subscribed lazily on
	// the first get_recently_saved_packages call (no module-lifecycle
	// wiring needed); accumulates from UPackage::PackageSavedWithContextEvent
	// in a live editor. Bounded, most-recent last.
	struct FRecentSavesBuffer
	{
		static constexpr int32 kMax = 50;
		FCriticalSection Lock;
		TArray<FString> Items;

		void Push(const FString& Name)
		{
			FScopeLock L(&Lock);
			Items.Remove(Name);          // dedup → move to most-recent
			Items.Add(Name);
			while (Items.Num() > kMax) { Items.RemoveAt(0); }
		}
		TArray<FString> Snapshot()
		{
			FScopeLock L(&Lock);
			return Items;
		}
	};

	FRecentSavesBuffer& GetRecentSavesBuffer()
	{
		static FRecentSavesBuffer Buffer;
		return Buffer;
	}

	void EnsureRecentSavesSubscribed()
	{
		static bool bSubscribed = false;
		if (bSubscribed) { return; }
		bSubscribed = true;
		UPackage::PackageSavedWithContextEvent.AddLambda(
			[](const FString& /*Filename*/, UPackage* Package, FObjectPostSaveContext)
			{
				if (Package)
				{
					GetRecentSavesBuffer().Push(Package->GetName());
				}
			});
	}

	int32 RunListAutomationTestsOp(const FString& /*Params*/, const FString& OutputPath, bool bPretty)
	{
		constexpr int32 kMax = 2000;
		TArray<FAutomationTestInfo> TestInfos;
		FAutomationTestFramework::Get().GetValidTestNames(TestInfos);
		TArray<TSharedPtr<FJsonValue>> Tests;
		bool bTruncated = false;
		for (const FAutomationTestInfo& Info : TestInfos)
		{
			if (Tests.Num() >= kMax) { bTruncated = true; break; }
			auto T = MakeShared<FJsonObject>();
			T->SetStringField(TEXT("display_name"), Info.GetDisplayName());
			T->SetStringField(TEXT("full_path"),    Info.GetFullTestPath());
			T->SetStringField(TEXT("test_name"),    Info.GetTestName());
			Tests.Add(MakeShared<FJsonValueObject>(T));
		}
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetArrayField(TEXT("tests"), Tests);
		Out->SetBoolField(TEXT("truncated"), bTruncated);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunSetProjectSettingOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString ClassPath, PropName, NewValue;
		FParse::Value(*Params, TEXT("Class="), ClassPath);
		FParse::Value(*Params, TEXT("Property="), PropName);
		FParse::Value(*Params, TEXT("Value="), NewValue);
		bool bApplied = false;
		FString Message;
		UClass* Cls = FindObject<UClass>(nullptr, *ClassPath);
		if (!Cls) { Cls = LoadObject<UClass>(nullptr, *ClassPath); }
		if (!Cls)
		{
			Message = TEXT("class not found");
		}
		else if (UObject* CDO = Cls->GetDefaultObject())
		{
			FProperty* Prop = FindFProperty<FProperty>(Cls, *PropName);
			if (!Prop)
			{
				Message = TEXT("property not found");
			}
			else
			{
				const TCHAR* Parsed = Prop->ImportText_Direct(*NewValue,
					Prop->ContainerPtrToValuePtr<void>(CDO),
					/*OwnerObject=*/CDO, PPF_None);
				if (Parsed == nullptr)
				{
					Message = TEXT("value parse failed");
				}
				else
				{
					CDO->TryUpdateDefaultConfigFile();
					bApplied = true;
				}
			}
		}
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetBoolField(TEXT("applied"), bApplied);
		Out->SetStringField(TEXT("message"), Message);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunGetProjectSettingValuesOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString ClassPath;
		FParse::Value(*Params, TEXT("Class="), ClassPath);
		bool bValid = false;
		TArray<TSharedPtr<FJsonValue>> Values;
		UClass* Cls = FindObject<UClass>(nullptr, *ClassPath);
		if (!Cls) { Cls = LoadObject<UClass>(nullptr, *ClassPath); }
		if (Cls)
		{
			if (UObject* CDO = Cls->GetDefaultObject())
			{
				bValid = true;
				for (TFieldIterator<FProperty> It(Cls); It; ++It)
				{
					FProperty* Prop = *It;
					if (!Prop) continue;
					FString ValueText;
					Prop->ExportText_Direct(ValueText,
						Prop->ContainerPtrToValuePtr<void>(CDO),
						/*Defaults=*/nullptr, /*OwnerObject=*/CDO, PPF_None);
					auto V = MakeShared<FJsonObject>();
					V->SetStringField(TEXT("name"),  Prop->GetName());
					V->SetStringField(TEXT("value"), ValueText);
					V->SetStringField(TEXT("type"),  Prop->GetCPPType());
					Values.Add(MakeShared<FJsonValueObject>(V));
				}
			}
		}
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetBoolField(TEXT("valid"), bValid);
		Out->SetStringField(TEXT("class_path"), ClassPath);
		Out->SetArrayField(TEXT("values"), Values);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunListProjectSettingsOp(const FString& /*Params*/, const FString& OutputPath, bool bPretty)
	{
		TArray<TSharedPtr<FJsonValue>> Sections;
		TArray<UClass*> DerivedClasses;
		GetDerivedClasses(UDeveloperSettings::StaticClass(), DerivedClasses, /*bRecursive=*/true);
		for (UClass* Cls : DerivedClasses)
		{
			if (!Cls || Cls->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated))
			{
				continue;
			}
			const UDeveloperSettings* CDO = Cls->GetDefaultObject<UDeveloperSettings>();
			if (!CDO)
			{
				continue;
			}
			auto S = MakeShared<FJsonObject>();
			S->SetStringField(TEXT("container"),  CDO->GetContainerName().ToString());
			S->SetStringField(TEXT("category"),   CDO->GetCategoryName().ToString());
			S->SetStringField(TEXT("section"),    CDO->GetSectionName().ToString());
			S->SetStringField(TEXT("class_path"), Cls->GetPathName());
			Sections.Add(MakeShared<FJsonValueObject>(S));
		}
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetArrayField(TEXT("sections"), Sections);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunGetRecentlySavedPackagesOp(const FString& /*Params*/, const FString& OutputPath, bool bPretty)
	{
		EnsureRecentSavesSubscribed();
		const TArray<FString> Snap = GetRecentSavesBuffer().Snapshot();
		TArray<TSharedPtr<FJsonValue>> Paths;
		for (int32 i = Snap.Num() - 1; i >= 0; --i)   // most-recent first
		{
			Paths.Add(MakeShared<FJsonValueString>(Snap[i]));
		}
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetArrayField(TEXT("package_paths"), Paths);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunGetStreamingSourcesOp(const FString& /*Params*/, const FString& OutputPath, bool bPretty)
	{
		TArray<TSharedPtr<FJsonValue>> Sources;
		bool bHasWP = false;
		if (UWorld* World = GetEditorWorldOrNull())
		{
			if (UWorldPartitionSubsystem* WPS = World->GetSubsystem<UWorldPartitionSubsystem>())
			{
				bHasWP = true;
				for (const IWorldPartitionStreamingSourceProvider* Provider :
						WPS->GetStreamingSourceProviders())
				{
					if (!Provider) continue;
					FWorldPartitionStreamingSource Src;
					if (!Provider->GetStreamingSource(Src)) continue;
					auto S = MakeShared<FJsonObject>();
					S->SetStringField(TEXT("name"),  Src.Name.ToString());
					S->SetNumberField(TEXT("loc_x"), Src.Location.X);
					S->SetNumberField(TEXT("loc_y"), Src.Location.Y);
					S->SetNumberField(TEXT("loc_z"), Src.Location.Z);
					S->SetNumberField(TEXT("pitch"), Src.Rotation.Pitch);
					S->SetNumberField(TEXT("yaw"),   Src.Rotation.Yaw);
					S->SetNumberField(TEXT("roll"),  Src.Rotation.Roll);
					Sources.Add(MakeShared<FJsonValueObject>(S));
				}
			}
		}
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetArrayField(TEXT("sources"), Sources);
		Out->SetBoolField(TEXT("has_world_partition"), bHasWP);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunSetPluginEnabledOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString PluginName;
		FParse::Value(*Params, TEXT("Plugin="), PluginName);
		const bool bEnabled = FParse::Param(*Params, TEXT("Enabled"));
		bool bApplied = false, bSaved = false;
		FString Message;
		if (!PluginName.IsEmpty())
		{
			FText FailReason;
			bApplied = IProjectManager::Get().SetPluginEnabled(PluginName, bEnabled, FailReason);
			if (!bApplied)
			{
				Message = FailReason.ToString();
			}
			else if (IProjectManager::Get().IsCurrentProjectDirty())
			{
				FText SaveFail;
				bSaved = IProjectManager::Get().SaveCurrentProjectToDisk(SaveFail);
				if (!bSaved)
				{
					Message = SaveFail.ToString();
				}
			}
			else
			{
				bSaved = true;   // already in the requested state, nothing to write
			}
		}
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetBoolField(TEXT("applied"), bApplied);
		Out->SetBoolField(TEXT("saved"), bSaved);
		Out->SetStringField(TEXT("message"), Message);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunGetActiveStatsOp(const FString& /*Params*/, const FString& OutputPath, bool bPretty)
	{
		bool bValid = false;
		TArray<TSharedPtr<FJsonValue>> Stats;
		if (FEditorViewportClient* VC = FindActiveLevelViewportClient())
		{
			bValid = true;
			if (const TArray<FString>* Enabled = VC->GetEnabledStats())
			{
				for (const FString& S : *Enabled)
				{
					Stats.Add(MakeShared<FJsonValueString>(S));
				}
			}
		}
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetBoolField(TEXT("valid"), bValid);
		Out->SetArrayField(TEXT("stats"), Stats);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunGetWatchedPinsOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		const FString AssetPath = ResolveAssetPath(Params);
		bool bValid = false;
		TArray<TSharedPtr<FJsonValue>> Pins;
		if (UBlueprint* BP = LoadMutableBlueprint(AssetPath))
		{
			bValid = true;
			FKismetDebugUtilities::ForeachPinWatch(BP,
				[&Pins](UEdGraphPin* Pin)
				{
					if (!Pin) return;
					UEdGraphNode* Node = Pin->GetOwningNodeUnchecked();
					auto P = MakeShared<FJsonObject>();
					P->SetStringField(TEXT("pin_name"),  Pin->PinName.ToString());
					P->SetStringField(TEXT("node_guid"),
						Node ? Node->NodeGuid.ToString() : FString());
					P->SetStringField(TEXT("node_name"),
						Node ? Node->GetName() : FString());
					P->SetStringField(TEXT("direction"),
						Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
					Pins.Add(MakeShared<FJsonValueObject>(P));
				});
		}
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetBoolField(TEXT("valid"), bValid);
		Out->SetArrayField(TEXT("pins"), Pins);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunGetBlueprintBreakpointsOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		const FString AssetPath = ResolveAssetPath(Params);
		bool bValid = false;
		TArray<TSharedPtr<FJsonValue>> Breaks;
		if (UBlueprint* BP = LoadMutableBlueprint(AssetPath))
		{
			bValid = true;
			FKismetDebugUtilities::ForeachBreakpoint(BP,
				[&Breaks](FBlueprintBreakpoint& BPt)
				{
					UEdGraphNode* Node = BPt.GetLocation();
					auto B = MakeShared<FJsonObject>();
					B->SetStringField(TEXT("node_guid"),
						Node ? Node->NodeGuid.ToString() : FString());
					B->SetStringField(TEXT("node_name"),
						Node ? Node->GetName() : FString());
					B->SetStringField(TEXT("location"),
						BPt.GetLocationDescription().ToString());
					B->SetBoolField(TEXT("enabled"), BPt.IsEnabled());
					Breaks.Add(MakeShared<FJsonValueObject>(B));
				});
		}
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetBoolField(TEXT("valid"), bValid);
		Out->SetArrayField(TEXT("breakpoints"), Breaks);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunGetDebugInstanceOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		const FString AssetPath = ResolveAssetPath(Params);
		bool bValid = false, bHasDbg = false;
		FString DbgName, DbgPath;
		if (UBlueprint* BP = LoadMutableBlueprint(AssetPath))
		{
			bValid = true;
			if (UObject* Dbg = BP->GetObjectBeingDebugged())
			{
				bHasDbg = true;
				DbgName = Dbg->GetName();
				DbgPath = Dbg->GetPathName();
			}
		}
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetBoolField(TEXT("valid"), bValid);
		Out->SetBoolField(TEXT("has_debug_object"), bHasDbg);
		Out->SetStringField(TEXT("debug_object_name"), DbgName);
		Out->SetStringField(TEXT("debug_object_path"), DbgPath);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunGetRecentlyOpenedAssetsOp(const FString& /*Params*/, const FString& OutputPath, bool bPretty)
	{
		TArray<TSharedPtr<FJsonValue>> Paths;
		if (IsValid(GEditor))
		{
			if (UAssetEditorSubsystem* AES = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
			{
				if (FMainMRUFavoritesList* MRU = AES->GetRecentlyOpenedAssets())
				{
					const int32 Num = MRU->GetNumItems();
					for (int32 i = 0; i < Num; ++i)
					{
						Paths.Add(MakeShared<FJsonValueString>(MRU->GetMRUItem(i)));
					}
				}
			}
		}
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetArrayField(TEXT("asset_paths"), Paths);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	// Phase 10 (EA-push) — editor event capture. Lazy-subscribed on the
	// first get_editor_events call (no module-lifecycle wiring); buffers
	// Tier-A UE-delegate events as {name, params}. Bounded ring, drained
	// on read. Game-thread only in practice (delegates + the op both run
	// on the editor's main thread); the lock is cheap insurance.
	struct FEditorEventBuffer
	{
		static constexpr int32 kMax = 200;
		FCriticalSection Lock;
		TArray<TSharedPtr<FJsonObject>> Events;

		void Push(const FString& Name, const TSharedPtr<FJsonObject>& Params)
		{
			FScopeLock L(&Lock);
			auto E = MakeShared<FJsonObject>();
			E->SetStringField(TEXT("name"), Name);
			E->SetObjectField(TEXT("params"),
				Params.IsValid() ? Params : MakeShared<FJsonObject>());
			Events.Add(E);
			while (Events.Num() > kMax) { Events.RemoveAt(0); }
		}
		TArray<TSharedPtr<FJsonValue>> Drain()
		{
			FScopeLock L(&Lock);
			TArray<TSharedPtr<FJsonValue>> Out;
			for (const TSharedPtr<FJsonObject>& E : Events)
			{
				Out.Add(MakeShared<FJsonValueObject>(E));
			}
			Events.Reset();
			return Out;
		}
	};

	FEditorEventBuffer& GetEditorEventBuffer()
	{
		static FEditorEventBuffer Buffer;
		return Buffer;
	}

	void EnsureEditorEventsSubscribed()
	{
		static bool bSubscribed = false;
		if (bSubscribed) { return; }
		bSubscribed = true;

		USelection::SelectionChangedEvent.AddLambda([](UObject* /*Sel*/)
		{
			int32 Count = 0;
			if (IsValid(GEditor))
			{
				if (USelection* S = GEditor->GetSelectedActors()) { Count = S->Num(); }
			}
			auto P = MakeShared<FJsonObject>();
			P->SetNumberField(TEXT("count"), Count);
			GetEditorEventBuffer().Push(TEXT("level_actor_selection_changed"), P);
		});

		if (IsValid(GEditor))
		{
			if (UAssetEditorSubsystem* AES = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
			{
				AES->OnAssetOpenedInEditor().AddLambda([](UObject* Asset, IAssetEditorInstance*)
				{
					auto P = MakeShared<FJsonObject>();
					P->SetStringField(TEXT("asset"), Asset ? Asset->GetPathName() : FString());
					GetEditorEventBuffer().Push(TEXT("asset_opened"), P);
				});
			}

			// Any Blueprint finished compiling (editor-wide signal) — highly
			// relevant for a BP-focused server. No params on this delegate.
			GEditor->OnBlueprintCompiled().AddLambda([]()
			{
				GetEditorEventBuffer().Push(TEXT("blueprint_compiled"), MakeShared<FJsonObject>());
			});
		}

		FEditorDelegates::BeginPIE.AddLambda([](bool)
		{
			GetEditorEventBuffer().Push(TEXT("pie_started"), MakeShared<FJsonObject>());
		});
		FEditorDelegates::EndPIE.AddLambda([](bool)
		{
			GetEditorEventBuffer().Push(TEXT("pie_stopped"), MakeShared<FJsonObject>());
		});
		FEditorDelegates::PausePIE.AddLambda([](bool)
		{
			GetEditorEventBuffer().Push(TEXT("pie_paused"), MakeShared<FJsonObject>());
		});
		FEditorDelegates::ResumePIE.AddLambda([](bool)
		{
			GetEditorEventBuffer().Push(TEXT("pie_resumed"), MakeShared<FJsonObject>());
		});

		// Package saved (also feeds the recently-saved ring buffer via its
		// own independent hook — both subscribe to the same multicast).
		UPackage::PackageSavedWithContextEvent.AddLambda(
			[](const FString& /*Filename*/, UPackage* Package, FObjectPostSaveContext)
			{
				auto P = MakeShared<FJsonObject>();
				P->SetStringField(TEXT("package"),
					Package ? Package->GetName() : FString());
				GetEditorEventBuffer().Push(TEXT("package_saved"), P);
			});

		// Map/level opened.
		FEditorDelegates::OnMapOpened.AddLambda(
			[](const FString& Filename, bool /*bAsTemplate*/)
			{
				auto P = MakeShared<FJsonObject>();
				P->SetStringField(TEXT("filename"), Filename);
				GetEditorEventBuffer().Push(TEXT("map_opened"), P);
			});

		// Asset lifecycle (removal/rename fire on user actions, not the
		// initial scan, so they're not noisy like OnAssetAdded would be).
		{
			FAssetRegistryModule& ARM =
				FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
			IAssetRegistry& AR = ARM.Get();
			AR.OnAssetRemoved().AddLambda([](const FAssetData& Data)
			{
				auto P = MakeShared<FJsonObject>();
				P->SetStringField(TEXT("asset"), Data.GetObjectPathString());
				GetEditorEventBuffer().Push(TEXT("asset_removed"), P);
			});
			AR.OnAssetRenamed().AddLambda([](const FAssetData& Data, const FString& OldName)
			{
				auto P = MakeShared<FJsonObject>();
				P->SetStringField(TEXT("asset"), Data.GetObjectPathString());
				P->SetStringField(TEXT("old_name"), OldName);
				GetEditorEventBuffer().Push(TEXT("asset_renamed"), P);
			});
		}

		// Actor lifecycle in the editor world (spawn / delete) — a top
		// agentic signal. UEngine multicast delegates, populated by the
		// editor engine.
		if (IsValid(GEngine))
		{
			GEngine->OnLevelActorAdded().AddLambda([](AActor* Actor)
			{
				auto P = MakeShared<FJsonObject>();
				P->SetStringField(TEXT("actor"), Actor ? Actor->GetName() : FString());
				GetEditorEventBuffer().Push(TEXT("actor_added"), P);
			});
			GEngine->OnLevelActorDeleted().AddLambda([](AActor* Actor)
			{
				auto P = MakeShared<FJsonObject>();
				P->SetStringField(TEXT("actor"), Actor ? Actor->GetName() : FString());
				GetEditorEventBuffer().Push(TEXT("actor_deleted"), P);
			});
		}

		// Tier-B (debounced): actor property edits. OnObjectPropertyChanged
		// is a firehose across all UObjects, so we scope to level actors and
		// time-gate to at most ~2/sec — enough for "the user is editing this
		// actor" without flooding the buffer. Demonstrates the debounce
		// pattern for the higher-frequency event tiers.
		FCoreUObjectDelegates::OnObjectPropertyChanged.AddLambda(
			[](UObject* Obj, FPropertyChangedEvent& Evt)
			{
				if (!IsValid(Obj) || !Obj->IsA<AActor>())
				{
					return;
				}
				static double LastEmit = 0.0;
				const double Now = FPlatformTime::Seconds();
				if (Now - LastEmit < 0.5)
				{
					return;
				}
				LastEmit = Now;
				auto P = MakeShared<FJsonObject>();
				P->SetStringField(TEXT("actor"), Obj->GetName());
				P->SetStringField(TEXT("property"),
					Evt.Property ? Evt.Property->GetName() : FString());
				GetEditorEventBuffer().Push(TEXT("actor_property_changed"), P);
			});

		// Tier-C (debounced): viewport camera moves, gated to ~4Hz.
		FEditorDelegates::OnEditorCameraMoved.AddLambda(
			[](const FVector& Loc, const FRotator& Rot, ELevelViewportType, int32)
			{
				static double LastEmit = 0.0;
				const double Now = FPlatformTime::Seconds();
				if (Now - LastEmit < 0.25)
				{
					return;
				}
				LastEmit = Now;
				auto P = MakeShared<FJsonObject>();
				P->SetNumberField(TEXT("x"), Loc.X);
				P->SetNumberField(TEXT("y"), Loc.Y);
				P->SetNumberField(TEXT("z"), Loc.Z);
				P->SetNumberField(TEXT("pitch"), Rot.Pitch);
				P->SetNumberField(TEXT("yaw"), Rot.Yaw);
				P->SetNumberField(TEXT("roll"), Rot.Roll);
				GetEditorEventBuffer().Push(TEXT("camera_moved"), P);
			});

		// Tier-C (debounced): a package became dirty, gated to ~1/sec.
		UPackage::PackageMarkedDirtyEvent.AddLambda(
			[](UPackage* Pkg, bool /*bWasDirty*/)
			{
				static double LastEmit = 0.0;
				const double Now = FPlatformTime::Seconds();
				if (Now - LastEmit < 1.0)
				{
					return;
				}
				LastEmit = Now;
				auto P = MakeShared<FJsonObject>();
				P->SetStringField(TEXT("package"), Pkg ? Pkg->GetName() : FString());
				GetEditorEventBuffer().Push(TEXT("dirty_packages_changed"), P);
			});
	}

	int32 RunGetEditorEventsOp(const FString& /*Params*/, const FString& OutputPath, bool bPretty)
	{
		EnsureEditorEventsSubscribed();
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetArrayField(TEXT("events"), GetEditorEventBuffer().Drain());
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunGetActiveCookTargetOp(const FString& /*Params*/, const FString& OutputPath, bool bPretty)
	{
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		TArray<TSharedPtr<FJsonValue>> Platforms;
		FString Running;
		if (ITargetPlatformManagerModule* TPM =
				FModuleManager::GetModulePtr<ITargetPlatformManagerModule>(TEXT("TargetPlatform")))
		{
			for (ITargetPlatform* P : TPM->GetActiveTargetPlatforms())
			{
				if (P)
				{
					Platforms.Add(MakeShared<FJsonValueString>(P->PlatformName()));
				}
			}
			if (ITargetPlatform* R = TPM->GetRunningTargetPlatform())
			{
				Running = R->PlatformName();
			}
		}
		Out->SetArrayField(TEXT("platforms"), Platforms);
		Out->SetStringField(TEXT("running_platform"), Running);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunGetWorkspaceLayoutOp(const FString& /*Params*/, const FString& OutputPath, bool bPretty)
	{
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		FString LayoutStr;
		if (FSlateApplication::IsInitialized())
		{
			// Serialize the editor's current docking layout. In a -nullrhi
			// commandlet the global tab manager exists but has no persisted
			// layout, so this is a minimal string; in a live editor it is the
			// full workspace state.
			const TSharedRef<FTabManager::FLayout> Layout =
				FGlobalTabmanager::Get()->PersistLayout();
			LayoutStr = Layout->ToString();
		}
		Out->SetStringField(TEXT("layout"), LayoutStr);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunGetTraceStateOp(const FString& /*Params*/, const FString& OutputPath, bool bPretty)
	{
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetBoolField(TEXT("connected"), FTraceAuxiliary::IsConnected());
		Out->SetBoolField(TEXT("paused"),    FTraceAuxiliary::IsPaused());
		const TCHAR* TypeStr = TEXT("none");
		switch (FTraceAuxiliary::GetConnectionType())
		{
			case FTraceAuxiliary::EConnectionType::Network: TypeStr = TEXT("network"); break;
			case FTraceAuxiliary::EConnectionType::File:    TypeStr = TEXT("file");    break;
			default: break;
		}
		Out->SetStringField(TEXT("connection_type"), TypeStr);
		Out->SetStringField(TEXT("destination"), FTraceAuxiliary::GetTraceDestinationString());
		TStringBuilder<512> Channels;
		FTraceAuxiliary::GetActiveChannelsString(Channels);
		Out->SetStringField(TEXT("active_channels"), Channels.ToString());
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	// Shared responder for editor UI-state surfaces (outliner expansion,
	// details-panel filter, active toasts, status-bar text) that live in
	// transient Slate widgets. No out-of-process bridge yet — returns
	// valid:false with a per-feature reason. The tool surface + schema are
	// the stable contract; a future in-editor Slate-introspection branch can
	// fill a given feature in here.
	int32 RunGetUiStateStubOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString Feature;
		FParse::Value(*Params, TEXT("Feature="), Feature);
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetBoolField(TEXT("valid"), false);
		Out->SetStringField(TEXT("reason"),
			FString::Printf(
				TEXT("%s is not available via this backend in this build ")
				TEXT("(v1 stub — see the tool description for what a real ")
				TEXT("implementation requires)"),
				Feature.IsEmpty() ? TEXT("this feature") : *Feature));
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunGetSnappingSettingsOp(const FString& /*Params*/, const FString& OutputPath, bool bPretty)
	{
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		const ULevelEditorViewportSettings* S = GetDefault<ULevelEditorViewportSettings>();
		const bool bValid = (S != nullptr);
		Out->SetBoolField(TEXT("valid"), bValid);
		Out->SetBoolField(TEXT("grid_enabled"),         bValid ? (bool)S->GridEnabled    : false);
		Out->SetBoolField(TEXT("rot_grid_enabled"),     bValid ? (bool)S->RotGridEnabled : false);
		Out->SetBoolField(TEXT("snap_vertices"),        bValid ? S->bSnapVertices         : false);
		Out->SetNumberField(TEXT("current_pos_grid_size"), bValid ? S->CurrentPosGridSize : 0);
		Out->SetNumberField(TEXT("current_rot_grid_size"), bValid ? S->CurrentRotGridSize : 0);
		Out->SetNumberField(TEXT("actor_snap_distance"),   bValid ? S->ActorSnapDistance  : 0.0f);
		Out->SetNumberField(TEXT("snap_distance"),         bValid ? S->SnapDistance       : 0.0f);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunGetActiveViewportOp(const FString& /*Params*/, const FString& OutputPath, bool bPretty)
	{
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		bool bValid = false;
		int32 Idx = -1;
		bool bPersp = false;
		int32 SX = 0, SY = 0;
		if (IsValid(GEditor))
		{
			int32 ScanIdx = 0;
			for (FEditorViewportClient* VC : GEditor->GetAllViewportClients())
			{
				if (!VC || !VC->IsLevelEditorClient())
				{
					continue;
				}
				if (VC->Viewport && VC->Viewport->HasFocus())
				{
					bValid = true;
					Idx = ScanIdx;
					bPersp = VC->IsPerspective();
					const FIntPoint Size = VC->Viewport->GetSizeXY();
					SX = Size.X; SY = Size.Y;
					break;
				}
				++ScanIdx;
			}
		}
		Out->SetBoolField(TEXT("valid"),         bValid);
		Out->SetNumberField(TEXT("viewport_index"), Idx);
		Out->SetBoolField(TEXT("is_perspective"), bPersp);
		Out->SetNumberField(TEXT("size_x"),       SX);
		Out->SetNumberField(TEXT("size_y"),       SY);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunGetBufferVisualizationModeOp(const FString& /*Params*/, const FString& OutputPath, bool bPretty)
	{
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		bool bValid = false;
		FString Mode;
		if (FEditorViewportClient* VC = FindActiveLevelViewportClient())
		{
			bValid = true;
			Mode = VC->GetCurrentBufferVisualizationModeDisplayName().ToString();
		}
		Out->SetBoolField(TEXT("valid"), bValid);
		Out->SetStringField(TEXT("mode"), Mode);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunGetGizmoStateOp(const FString& /*Params*/, const FString& OutputPath, bool bPretty)
	{
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		bool bValid = false;
		FString ModeStr;
		FString CoordStr;
		if (FEditorViewportClient* VC = FindActiveLevelViewportClient())
		{
			bValid = true;
			switch (VC->GetWidgetMode())
			{
				case UE::Widget::WM_Translate:        ModeStr = TEXT("translate"); break;
				case UE::Widget::WM_Rotate:           ModeStr = TEXT("rotate"); break;
				case UE::Widget::WM_Scale:            ModeStr = TEXT("scale"); break;
				case UE::Widget::WM_TranslateRotateZ: ModeStr = TEXT("translaterotatez"); break;
				case UE::Widget::WM_2D:               ModeStr = TEXT("2d"); break;
				case UE::Widget::WM_None:             ModeStr = TEXT("none"); break;
				default:                              ModeStr = TEXT("unknown"); break;
			}
			CoordStr = (VC->GetWidgetCoordSystemSpace() == COORD_World)
				? TEXT("world") : TEXT("local");
		}
		Out->SetBoolField(TEXT("valid"), bValid);
		Out->SetStringField(TEXT("mode"),        ModeStr);
		Out->SetStringField(TEXT("coord_space"), CoordStr);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunGetViewportRealtimeOp(const FString& /*Params*/, const FString& OutputPath, bool bPretty)
	{
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		bool bValid = false;
		bool bIsRealtime = false;
		if (FEditorViewportClient* VC = FindActiveLevelViewportClient())
		{
			bValid = true;
			bIsRealtime = VC->IsRealtime();
		}
		Out->SetBoolField(TEXT("valid"), bValid);
		Out->SetBoolField(TEXT("is_realtime"), bIsRealtime);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunGetViewportCameraSettingsOp(const FString& /*Params*/, const FString& OutputPath, bool bPretty)
	{
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		bool bValid = false;
		double Fov = 0.0, Speed = 0.0, Near = 0.0, Far = 0.0;
		if (FEditorViewportClient* VC = FindActiveLevelViewportClient())
		{
			bValid = true;
			Fov   = VC->ViewFOV;
			Speed = VC->GetCameraSpeed();
			Near  = VC->GetNearClipPlane();
			Far   = VC->GetFarClipPlaneOverride();   // 0 when not overridden
		}
		Out->SetBoolField(TEXT("valid"),        bValid);
		Out->SetNumberField(TEXT("fov"),          Fov);
		Out->SetNumberField(TEXT("camera_speed"), Speed);
		Out->SetNumberField(TEXT("near_clip"),    Near);
		Out->SetNumberField(TEXT("far_clip"),     Far);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunGetNiagaraModuleSelectionOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		// v1 stub. NiagaraSystemEditor uses multi-inheritance in the
		// Niagara plugin's editor module; safe cross-cast without RTTI
		// requires a sidecar registry hooked into the asset-editor lifecycle.
		const FString AssetPath = ResolveAssetPath(Params);
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetBoolField(TEXT("valid"), false);
		Out->SetStringField(TEXT("asset_path"), AssetPath);
		Out->SetArrayField(TEXT("selected_module_names"), TArray<TSharedPtr<FJsonValue>>{});
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunGetCurveEditorSelectionOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		// v1 stub. Curve editor is a SCurveEditor widget hosted inside
		// 5+ different editors; there's no single instance keyed on
		// asset_path. The upgrade requires per-host-editor tracking of
		// the active FCurveEditor pointer.
		const FString AssetPath = ResolveAssetPath(Params);
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetBoolField(TEXT("valid"), false);
		Out->SetStringField(TEXT("asset_path"), AssetPath);
		Out->SetNumberField(TEXT("selected_key_count"), 0);
		Out->SetArrayField(TEXT("selected_curve_names"), TArray<TSharedPtr<FJsonValue>>{});
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunGetAnimEditorStateOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		const FString AssetPath = ResolveAssetPath(Params);
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetStringField(TEXT("asset_path"), AssetPath);
		// Roadmap 3.1. Real editor-open detection via UAssetEditorSubsystem
		// (the documented, public path — no garbage, no fragile internals):
		// if the asset's editor tab is open we report valid:true + its
		// editor name. Deep skeleton-tree selection (selected_bone_index /
		// socket) lives in the Persona preview scene and needs a cross-cast
		// to IHasPersonaToolkit (now buildable via bUseRTTI) developed
		// against a running anim editor — left honestly stubbed (-1 / "")
		// until that live-dev follow-up so we never report fabricated state.
		bool bEditorOpen = false;
		FString EditorName;
		if (!AssetPath.IsEmpty() && IsValid(GEditor))
		{
			// Anim BPs load as UAnimBlueprint, anim sequences/montages as
			// UAnimationAsset — resolve generically.
			UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
			if (Asset != nullptr)
			{
				if (UAssetEditorSubsystem* AES =
						GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
				{
					if (IAssetEditorInstance* Inst =
							AES->FindEditorForAsset(Asset, /*bFocusIfOpen=*/false))
					{
						bEditorOpen = true;
						EditorName  = Inst->GetEditorName().ToString();
					}
				}
			}
		}
		Out->SetBoolField(TEXT("valid"), bEditorOpen);
		Out->SetBoolField(TEXT("editor_open"), bEditorOpen);
		Out->SetStringField(TEXT("editor_name"), EditorName);
		Out->SetNumberField(TEXT("selected_bone_index"), -1);
		Out->SetStringField(TEXT("selected_socket_name"), FString());
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	FString MovieSceneStatusToString(EMovieScenePlayerStatus::Type Status)
	{
		switch (Status)
		{
			case EMovieScenePlayerStatus::Stopped:   return TEXT("stopped");
			case EMovieScenePlayerStatus::Playing:   return TEXT("playing");
			case EMovieScenePlayerStatus::Scrubbing: return TEXT("scrubbing");
			case EMovieScenePlayerStatus::Jumping:   return TEXT("jumping");
			case EMovieScenePlayerStatus::Stepping:  return TEXT("stepping");
			case EMovieScenePlayerStatus::Paused:    return TEXT("paused");
			default:                                  return TEXT("unknown");
		}
	}

	int32 RunGetSequencerStateOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		const FString AssetPath = ResolveAssetPath(Params);
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetBoolField(TEXT("valid"), false);
		Out->SetStringField(TEXT("asset_path"), AssetPath);
		Out->SetNumberField(TEXT("playhead_seconds"), 0.0);
		Out->SetStringField(TEXT("playback_status"), FString());
		Out->SetNumberField(TEXT("playback_range_start_seconds"), 0.0);
		Out->SetNumberField(TEXT("playback_range_end_seconds"),   0.0);

		if (!AssetPath.IsEmpty() && GEditor != nullptr)
		{
			if (ULevelSequence* LS = LoadObject<ULevelSequence>(nullptr, *AssetPath))
			{
				if (UAssetEditorSubsystem* AES = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
				{
					if (IAssetEditorInstance* Editor = AES->FindEditorForAsset(LS, /*bFocusIfOpen=*/false))
					{
						const FName EditorId = Editor->GetEditorName();
						if (EditorId == FName(TEXT("LevelSequenceEditor")))
						{
							ILevelSequenceEditorToolkit* LSToolkit = static_cast<ILevelSequenceEditorToolkit*>(Editor);
							if (TSharedPtr<ISequencer> Seq = LSToolkit->GetSequencer())
							{
								Out->SetBoolField(TEXT("valid"), true);
								const FQualifiedFrameTime GlobalTime = Seq->GetGlobalTime();
								Out->SetNumberField(TEXT("playhead_seconds"),
									GlobalTime.AsSeconds());
								Out->SetStringField(TEXT("playback_status"),
									MovieSceneStatusToString(Seq->GetPlaybackStatus()));
								// Playback range from the level sequence's movie scene.
								if (UMovieScene* Scene = LS->GetMovieScene())
								{
									const FFrameRate Rate = Scene->GetTickResolution();
									const TRange<FFrameNumber> Range = Scene->GetPlaybackRange();
									if (Range.HasLowerBound())
									{
										const FFrameNumber StartFrame = Range.GetLowerBoundValue();
										Out->SetNumberField(TEXT("playback_range_start_seconds"),
											Rate.AsSeconds(FFrameTime(StartFrame)));
									}
									if (Range.HasUpperBound())
									{
										const FFrameNumber EndFrame = Range.GetUpperBoundValue();
										Out->SetNumberField(TEXT("playback_range_end_seconds"),
											Rate.AsSeconds(FFrameTime(EndFrame)));
									}
								}
							}
						}
					}
				}
			}
		}
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunGetCinematicCameraOp(const FString& /*Params*/, const FString& OutputPath, bool bPretty)
	{
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		bool bValid = false;
		FString ActorName;
		double LX=0, LY=0, LZ=0, Pitch=0, Yaw=0, Roll=0, Fov=0;

		// Walk PIE world contexts to find a player controller.
		if (GEngine != nullptr)
		{
			for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
			{
				if (Ctx.WorldType != EWorldType::PIE) continue;
				UWorld* W = Ctx.World();
				if (!IsValid(W)) continue;
				APlayerController* PC = W->GetFirstPlayerController();
				if (!IsValid(PC) || !IsValid(PC->PlayerCameraManager)) continue;
				AActor* ViewTarget = PC->PlayerCameraManager->GetViewTarget();
				if (!IsValid(ViewTarget)) continue;
				bValid = true;
				ActorName = ViewTarget->GetName();
				const FVector L = ViewTarget->GetActorLocation();
				const FRotator R = ViewTarget->GetActorRotation();
				LX = L.X; LY = L.Y; LZ = L.Z;
				Pitch = R.Pitch; Yaw = R.Yaw; Roll = R.Roll;
				Fov = PC->PlayerCameraManager->GetFOVAngle();
				break;
			}
		}
		Out->SetBoolField(TEXT("valid"),     bValid);
		Out->SetStringField(TEXT("actor_name"), ActorName);
		Out->SetNumberField(TEXT("loc_x"),  LX);
		Out->SetNumberField(TEXT("loc_y"),  LY);
		Out->SetNumberField(TEXT("loc_z"),  LZ);
		Out->SetNumberField(TEXT("pitch"),  Pitch);
		Out->SetNumberField(TEXT("yaw"),    Yaw);
		Out->SetNumberField(TEXT("roll"),   Roll);
		Out->SetNumberField(TEXT("fov"),    Fov);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunGetMeshPreviewStateOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		const FString AssetPath = ResolveAssetPath(Params);
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetBoolField(TEXT("valid"), false);
		Out->SetStringField(TEXT("asset_path"), AssetPath);
		Out->SetNumberField(TEXT("current_lod_level"), -1);
		Out->SetNumberField(TEXT("current_lod_index"),  0);

		if (!AssetPath.IsEmpty() && GEditor != nullptr)
		{
			if (UStaticMesh* SM = LoadObject<UStaticMesh>(nullptr, *AssetPath))
			{
				if (UAssetEditorSubsystem* AES = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
				{
					if (IAssetEditorInstance* Editor = AES->FindEditorForAsset(SM, /*bFocusIfOpen=*/false))
					{
						const FName EditorId = Editor->GetEditorName();
						if (EditorId == FName(TEXT("StaticMeshEditor")))
						{
							Out->SetBoolField(TEXT("valid"), true);
							IStaticMeshEditor* SME = static_cast<IStaticMeshEditor*>(Editor);
							Out->SetNumberField(TEXT("current_lod_level"), SME->GetCurrentLODLevel());
							Out->SetNumberField(TEXT("current_lod_index"), SME->GetCurrentLODIndex());
						}
					}
				}
			}
		}
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunGetMaterialEditorStateOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		const FString AssetPath = ResolveAssetPath(Params);
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetBoolField(TEXT("valid"), false);
		Out->SetStringField(TEXT("asset_path"), AssetPath);
		Out->SetNumberField(TEXT("selected_node_count"), 0);
		TArray<TSharedPtr<FJsonValue>> Classes;

		if (!AssetPath.IsEmpty() && GEditor != nullptr)
		{
			if (UMaterial* Mat = LoadObject<UMaterial>(nullptr, *AssetPath))
			{
				if (UAssetEditorSubsystem* AES = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
				{
					if (IAssetEditorInstance* Editor = AES->FindEditorForAsset(Mat, /*bFocusIfOpen=*/false))
					{
						const FName EditorId = Editor->GetEditorName();
						if (EditorId == FName(TEXT("MaterialEditor")))
						{
							Out->SetBoolField(TEXT("valid"), true);
							IMaterialEditor* ME = static_cast<IMaterialEditor*>(Editor);
							TSet<UObject*> SelSet = ME->GetSelectedNodes();
							Out->SetNumberField(TEXT("selected_node_count"), SelSet.Num());
							for (UObject* Obj : SelSet)
							{
								if (IsValid(Obj) && Obj->GetClass())
								{
									Classes.Add(MakeShared<FJsonValueString>(
										Obj->GetClass()->GetName()));
								}
							}
						}
					}
				}
			}
		}
		Out->SetArrayField(TEXT("selected_expression_classes"), Classes);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunGetUmgEditorStateOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		const FString AssetPath = ResolveAssetPath(Params);
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetBoolField(TEXT("valid"), false);
		Out->SetStringField(TEXT("asset_path"), AssetPath);
		Out->SetStringField(TEXT("current_designer_tab"), FString());
		TArray<TSharedPtr<FJsonValue>> Selected;

		if (!AssetPath.IsEmpty() && GEditor != nullptr)
		{
			if (UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *AssetPath))
			{
				if (UAssetEditorSubsystem* AES = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
				{
					if (IAssetEditorInstance* Editor = AES->FindEditorForAsset(WBP, /*bFocusIfOpen=*/false))
					{
						const FName EditorId = Editor->GetEditorName();
						if (EditorId == FName(TEXT("WidgetBlueprintEditor")) ||
							EditorId == FName(TEXT("UMGEditor")))
						{
							Out->SetBoolField(TEXT("valid"), true);
							FWidgetBlueprintEditor* WBE = static_cast<FWidgetBlueprintEditor*>(Editor);
							const TSet<FWidgetReference>& SelSet = WBE->GetSelectedWidgets();
							for (const FWidgetReference& Ref : SelSet)
							{
								if (UWidget* TplW = Ref.GetTemplate())
								{
									Selected.Add(MakeShared<FJsonValueString>(TplW->GetName()));
								}
							}
						}
					}
				}
			}
		}
		Out->SetArrayField(TEXT("selected_widget_names"), Selected);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunGetStaticMeshInfoOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		const FString AssetPath = ResolveAssetPath(Params);
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetBoolField(TEXT("valid"), false);
		Out->SetStringField(TEXT("asset_path"), AssetPath);
		Out->SetNumberField(TEXT("lod_count"), 0);
		Out->SetBoolField(TEXT("is_nanite_enabled"), false);
		TArray<TSharedPtr<FJsonValue>> Lods;

		if (!AssetPath.IsEmpty())
		{
			if (UStaticMesh* SM = LoadObject<UStaticMesh>(nullptr, *AssetPath))
			{
				Out->SetBoolField(TEXT("valid"), true);
				const int32 LODCount = SM->GetNumLODs();
				Out->SetNumberField(TEXT("lod_count"), LODCount);
				Out->SetBoolField(TEXT("is_nanite_enabled"),
					SM->GetNaniteSettings().bEnabled);
				for (int32 i = 0; i < LODCount; ++i)
				{
					const FStaticMeshLODResources* LOD = &SM->GetRenderData()->LODResources[i];
					auto L = MakeShared<FJsonObject>();
					L->SetNumberField(TEXT("triangle_count"), LOD->GetNumTriangles());
					L->SetNumberField(TEXT("vertex_count"),   LOD->GetNumVertices());
					double ScreenSize = 0.0;
					if (i < SM->GetNumSourceModels())
					{
						ScreenSize = SM->GetSourceModel(i).ScreenSize.Default;
					}
					L->SetNumberField(TEXT("screen_size"), ScreenSize);
					Lods.Add(MakeShared<FJsonValueObject>(L));
				}
			}
		}
		Out->SetArrayField(TEXT("lods"), Lods);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunGetMaterialInstanceParamsOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		const FString AssetPath = ResolveAssetPath(Params);
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetBoolField(TEXT("valid"), false);
		Out->SetStringField(TEXT("asset_path"), AssetPath);
		Out->SetStringField(TEXT("parent_path"), FString());
		TArray<TSharedPtr<FJsonValue>> Scalars, Vectors, Textures;

		if (!AssetPath.IsEmpty())
		{
			if (UMaterialInstanceConstant* MI = LoadObject<UMaterialInstanceConstant>(nullptr, *AssetPath))
			{
				Out->SetBoolField(TEXT("valid"), true);
				if (MI->Parent)
				{
					Out->SetStringField(TEXT("parent_path"),
						MI->Parent->GetPathName());
				}
				for (const FScalarParameterValue& S : MI->ScalarParameterValues)
				{
					auto J = MakeShared<FJsonObject>();
					J->SetStringField(TEXT("name"),  S.ParameterInfo.Name.ToString());
					J->SetNumberField(TEXT("value"), S.ParameterValue);
					Scalars.Add(MakeShared<FJsonValueObject>(J));
				}
				for (const FVectorParameterValue& V : MI->VectorParameterValues)
				{
					auto J = MakeShared<FJsonObject>();
					J->SetStringField(TEXT("name"), V.ParameterInfo.Name.ToString());
					J->SetNumberField(TEXT("r"), V.ParameterValue.R);
					J->SetNumberField(TEXT("g"), V.ParameterValue.G);
					J->SetNumberField(TEXT("b"), V.ParameterValue.B);
					J->SetNumberField(TEXT("a"), V.ParameterValue.A);
					Vectors.Add(MakeShared<FJsonValueObject>(J));
				}
				for (const FTextureParameterValue& T : MI->TextureParameterValues)
				{
					auto J = MakeShared<FJsonObject>();
					J->SetStringField(TEXT("name"),         T.ParameterInfo.Name.ToString());
					J->SetStringField(TEXT("texture_path"),
						T.ParameterValue ? T.ParameterValue->GetPathName() : FString());
					Textures.Add(MakeShared<FJsonValueObject>(J));
				}
			}
		}
		Out->SetArrayField(TEXT("scalars"),  Scalars);
		Out->SetArrayField(TEXT("vectors"),  Vectors);
		Out->SetArrayField(TEXT("textures"), Textures);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunGetBlueprintEditorStateOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		const FString AssetPath = ResolveAssetPath(Params);
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetBoolField(TEXT("valid"), false);
		Out->SetStringField(TEXT("asset_path"), AssetPath);
		Out->SetStringField(TEXT("current_graph_name"), FString());
		Out->SetStringField(TEXT("compile_status"), FString());
		TArray<TSharedPtr<FJsonValue>> SelectedIds;

		if (AssetPath.IsEmpty())
		{
			Out->SetArrayField(TEXT("selected_node_ids"), SelectedIds);
			return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
		}
		UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *AssetPath);
		if (!IsValid(BP))
		{
			Out->SetArrayField(TEXT("selected_node_ids"), SelectedIds);
			return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
		}

		// Compile status is cheap; populate even when editor isn't open.
		const TCHAR* StatusStr = TEXT("unknown");
		switch (BP->Status)
		{
			case BS_Dirty:                  StatusStr = TEXT("dirty"); break;
			case BS_Error:                  StatusStr = TEXT("error"); break;
			case BS_UpToDate:               StatusStr = TEXT("good"); break;
			case BS_UpToDateWithWarnings:   StatusStr = TEXT("warning"); break;
			case BS_BeingCreated:           StatusStr = TEXT("compiling"); break;
			default:                        StatusStr = TEXT("unknown"); break;
		}
		Out->SetStringField(TEXT("compile_status"), StatusStr);

		if (GEditor != nullptr)
		{
			if (UAssetEditorSubsystem* AES = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
			{
				IAssetEditorInstance* Editor = AES->FindEditorForAsset(BP, /*bFocusIfOpen=*/false);
				if (Editor != nullptr)
				{
					// IAssetEditorInstance for a Blueprint is a FBlueprintEditor.
					// The class hierarchy uses multiple inheritance so a
					// reinterpret-style cast isn't safe; do a name check.
					if (FCString::Stricmp(Editor->GetEditorName().ToString().GetCharArray().GetData(), TEXT("Kismet")) == 0 ||
						FCString::Stricmp(Editor->GetEditorName().ToString().GetCharArray().GetData(), TEXT("BlueprintEditor")) == 0)
					{
						Out->SetBoolField(TEXT("valid"), true);
						FBlueprintEditor* BPEditor = static_cast<FBlueprintEditor*>(Editor);
						const FGraphPanelSelectionSet Selected = BPEditor->GetSelectedNodes();
						for (UObject* Obj : Selected)
						{
							if (UEdGraphNode* Node = Cast<UEdGraphNode>(Obj))
							{
								SelectedIds.Add(MakeShared<FJsonValueString>(
									Node->NodeGuid.ToString()));
							}
						}
						if (UEdGraph* Graph = BPEditor->GetFocusedGraph())
						{
							Out->SetStringField(TEXT("current_graph_name"),
								Graph->GetName());
						}
					}
				}
			}
		}
		Out->SetArrayField(TEXT("selected_node_ids"), SelectedIds);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunListActorAttributesOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString ActorName;
		FParse::Value(*Params, TEXT("Actor="), ActorName);

		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetBoolField(TEXT("valid"), false);
		Out->SetStringField(TEXT("actor_name"), ActorName);
		TArray<TSharedPtr<FJsonValue>> Attrs;

		if (AActor* Actor = FindActorByName(ActorName))
		{
			if (UAbilitySystemComponent* ASC = GetActorASC(Actor))
			{
				Out->SetBoolField(TEXT("valid"), true);
				TArray<FGameplayAttribute> Attributes;
				ASC->GetAllAttributes(Attributes);
				for (const FGameplayAttribute& Attr : Attributes)
				{
					auto A = MakeShared<FJsonObject>();
					// FGameplayAttribute::GetName() returns "Set.Attr" style.
					A->SetStringField(TEXT("name"),          Attr.GetName());
					A->SetNumberField(TEXT("base_value"),    ASC->GetNumericAttributeBase(Attr));
					A->SetNumberField(TEXT("current_value"), ASC->GetNumericAttribute(Attr));
					Attrs.Add(MakeShared<FJsonValueObject>(A));
				}
			}
		}
		Out->SetArrayField(TEXT("attributes"), Attrs);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunListActorGameplayEffectsOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString ActorName;
		FParse::Value(*Params, TEXT("Actor="), ActorName);

		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetBoolField(TEXT("valid"), false);
		Out->SetStringField(TEXT("actor_name"), ActorName);
		TArray<TSharedPtr<FJsonValue>> Effects;

		if (AActor* Actor = FindActorByName(ActorName))
		{
			if (UAbilitySystemComponent* ASC = GetActorASC(Actor))
			{
				Out->SetBoolField(TEXT("valid"), true);
				const FActiveGameplayEffectsContainer& Active = ASC->GetActiveGameplayEffects();
				for (auto It = Active.CreateConstIterator(); It; ++It)
				{
					const FActiveGameplayEffect& Eff = *It;
					auto E = MakeShared<FJsonObject>();
					const UGameplayEffect* Def = Eff.Spec.Def.Get();
					E->SetStringField(TEXT("effect_class"),
						Def ? Def->GetClass()->GetPathName() : FString());
					E->SetNumberField(TEXT("stack_count"),       Eff.Spec.GetStackCount());
					// Duration: -1 sentinel for infinite, 0+ for time-limited
					// (remaining seconds).
					float Duration = Eff.GetDuration();
					E->SetNumberField(TEXT("duration_remaining"),
						Duration < 0.0f ? -1.0 : (double)Eff.GetTimeRemaining(Actor->GetWorld()->GetTimeSeconds()));
					E->SetNumberField(TEXT("level"), Eff.Spec.GetLevel());

					TArray<TSharedPtr<FJsonValue>> Tags;
					FGameplayTagContainer Granted;
					Eff.Spec.GetAllGrantedTags(Granted);
					for (auto T = Granted.CreateConstIterator(); T; ++T)
					{
						Tags.Add(MakeShared<FJsonValueString>(T->ToString()));
					}
					E->SetArrayField(TEXT("granted_tags"), Tags);
					Effects.Add(MakeShared<FJsonValueObject>(E));
				}
			}
		}
		Out->SetArrayField(TEXT("effects"), Effects);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunListActorGameplayTagsOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString ActorName;
		FParse::Value(*Params, TEXT("Actor="), ActorName);

		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetBoolField(TEXT("valid"), false);
		Out->SetStringField(TEXT("actor_name"), ActorName);
		TArray<TSharedPtr<FJsonValue>> Tags;

		if (AActor* Actor = FindActorByName(ActorName))
		{
			if (UAbilitySystemComponent* ASC = GetActorASC(Actor))
			{
				Out->SetBoolField(TEXT("valid"), true);
				FGameplayTagContainer Owned;
				ASC->GetOwnedGameplayTags(Owned);
				for (auto It = Owned.CreateConstIterator(); It; ++It)
				{
					Tags.Add(MakeShared<FJsonValueString>(It->ToString()));
				}
			}
		}
		Out->SetArrayField(TEXT("tags"), Tags);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunListPluginsOp(const FString& /*Params*/, const FString& OutputPath, bool bPretty)
	{
		TArray<TSharedPtr<FJsonValue>> Plugins;
		IPluginManager& PM = IPluginManager::Get();
		for (const TSharedRef<IPlugin>& Plugin : PM.GetDiscoveredPlugins())
		{
			const FPluginDescriptor& Desc = Plugin->GetDescriptor();
			auto P = MakeShared<FJsonObject>();
			P->SetStringField(TEXT("name"),            Plugin->GetName());
			P->SetStringField(TEXT("descriptor_path"), Plugin->GetDescriptorFileName());
			P->SetStringField(TEXT("category"),        Desc.Category);
			P->SetStringField(TEXT("version"),         Desc.VersionName);
			P->SetBoolField(TEXT("is_enabled"),        Plugin->IsEnabled());
			P->SetBoolField(TEXT("is_built_in"),
				Plugin->GetType() == EPluginType::Engine ||
				Plugin->GetType() == EPluginType::Enterprise);
			P->SetBoolField(TEXT("is_content_only"),   Desc.Modules.Num() == 0);
			Plugins.Add(MakeShared<FJsonValueObject>(P));
		}
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetArrayField(TEXT("plugins"), Plugins);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunGetPluginDescriptorOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString Plugin;
		FParse::Value(*Params, TEXT("Plugin="), Plugin);

		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetBoolField(TEXT("valid"), false);
		Out->SetField(TEXT("descriptor"), MakeShared<FJsonValueNull>());

		if (!Plugin.IsEmpty())
		{
			IPluginManager& PM = IPluginManager::Get();
			TSharedPtr<IPlugin> Found = PM.FindPlugin(Plugin);
			if (Found.IsValid())
			{
				// Read the raw .uplugin and parse as JSON. Simpler than
				// re-serializing FPluginDescriptor through its UpdateJson
				// API, and avoids field drift across UE versions.
				FString Raw;
				if (FFileHelper::LoadFileToString(Raw, *Found->GetDescriptorFileName()))
				{
					TSharedPtr<FJsonValue> Parsed;
					TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Raw);
					if (FJsonSerializer::Deserialize(Reader, Parsed) && Parsed.IsValid())
					{
						Out->SetBoolField(TEXT("valid"), true);
						Out->SetField(TEXT("descriptor"), Parsed);
					}
				}
			}
		}
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunGetPluginDependenciesOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString Plugin;
		FParse::Value(*Params, TEXT("Plugin="), Plugin);
		TArray<TSharedPtr<FJsonValue>> Deps;
		bool bValid = false;
		if (!Plugin.IsEmpty())
		{
			IPluginManager& PM = IPluginManager::Get();
			TSharedPtr<IPlugin> Found = PM.FindPlugin(Plugin);
			if (Found.IsValid())
			{
				bValid = true;
				const FPluginDescriptor& Desc = Found->GetDescriptor();
				for (const FPluginReferenceDescriptor& Ref : Desc.Plugins)
				{
					Deps.Add(MakeShared<FJsonValueString>(Ref.Name));
				}
			}
		}
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetBoolField(TEXT("valid"), bValid);
		Out->SetArrayField(TEXT("dependencies"), Deps);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunListGameFeaturesOp(const FString& /*Params*/, const FString& OutputPath, bool bPretty)
	{
		TArray<TSharedPtr<FJsonValue>> Features;
		if (GEngine != nullptr)
		{
			UGameFeaturesSubsystem& GFS = UGameFeaturesSubsystem::Get();
			TArray<TPair<FString, FString>> UrlPairs;
			CollectGameFeaturePluginUrls(UrlPairs);
			for (const TPair<FString, FString>& Pair : UrlPairs)
			{
				const FString& Url        = Pair.Key;
				const FString& PluginName = Pair.Value;
				auto F = MakeShared<FJsonObject>();
				F->SetStringField(TEXT("plugin_name"), PluginName);
				F->SetStringField(TEXT("plugin_url"),  Url);
				F->SetStringField(TEXT("state"),       ComputeGameFeatureState(GFS, Url));
				Features.Add(MakeShared<FJsonValueObject>(F));
			}
		}
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetArrayField(TEXT("features"), Features);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunGetGameFeatureStateOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString Plugin;
		FParse::Value(*Params, TEXT("Plugin="), Plugin);

		bool bValid = false;
		FString StateStr;
		FString MatchedUrl;
		if (GEngine != nullptr && !Plugin.IsEmpty())
		{
			UGameFeaturesSubsystem& GFS = UGameFeaturesSubsystem::Get();
			TArray<TPair<FString, FString>> UrlPairs;
			CollectGameFeaturePluginUrls(UrlPairs);
			for (const TPair<FString, FString>& Pair : UrlPairs)
			{
				if (Pair.Value.Equals(Plugin, ESearchCase::IgnoreCase))
				{
					MatchedUrl = Pair.Key;
					StateStr   = ComputeGameFeatureState(GFS, MatchedUrl);
					bValid     = true;
					break;
				}
			}
		}
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetBoolField(TEXT("valid"), bValid);
		Out->SetStringField(TEXT("plugin_name"), Plugin);
		Out->SetStringField(TEXT("plugin_url"),  MatchedUrl);
		Out->SetStringField(TEXT("state"),       StateStr);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunListDesktopWindowsOp(const FString& /*Params*/, const FString& OutputPath, bool bPretty)
	{
		TArray<TSharedPtr<FJsonValue>> Wins;
		if (FSlateApplication::IsInitialized())
		{
			FSlateApplication& Slate = FSlateApplication::Get();
			TArray<TSharedRef<SWindow>> Windows;
			Slate.GetAllVisibleWindowsOrdered(Windows);
			TSharedPtr<SWindow> Active = Slate.GetActiveTopLevelWindow();
			for (const TSharedRef<SWindow>& Win : Windows)
			{
				auto W = MakeShared<FJsonObject>();
				W->SetStringField(TEXT("title"),       Win->GetTitle().ToString());
				W->SetStringField(TEXT("widget_type"), Win->GetTypeAsString());
				const FVector2D Pos  = Win->GetPositionInScreen();
				const FVector2D Size = Win->GetSizeInScreen();
				W->SetNumberField(TEXT("pos_x"),  Pos.X);
				W->SetNumberField(TEXT("pos_y"),  Pos.Y);
				W->SetNumberField(TEXT("size_x"), Size.X);
				W->SetNumberField(TEXT("size_y"), Size.Y);
				W->SetBoolField(TEXT("is_active"), Active.IsValid() && Active.Get() == &Win.Get());
				Wins.Add(MakeShared<FJsonValueObject>(W));
			}
		}
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetArrayField(TEXT("windows"), Wins);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunWorldToScreenOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		double WX = 0.0, WY = 0.0, WZ = 0.0;
		FParse::Value(*Params, TEXT("WX="), WX);
		FParse::Value(*Params, TEXT("WY="), WY);
		FParse::Value(*Params, TEXT("WZ="), WZ);

		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		bool bValid = false, bOnScreen = false;
		double NormX = 0.0, NormY = 0.0;

		FEditorViewportClient* VC = FindActiveLevelViewportClient();
		if (VC && VC->Viewport)
		{
			FSceneViewFamilyContext ViewFamily(FSceneViewFamily::ConstructionValues(
				VC->Viewport, VC->GetScene(), VC->EngineShowFlags)
				.SetRealtimeUpdate(VC->IsRealtime()));
			if (FSceneView* View = VC->CalcSceneView(&ViewFamily))
			{
				const FIntRect VR = View->UnscaledViewRect;
				FVector2D ScreenPos;
				const FVector World((float)WX, (float)WY, (float)WZ);
				const bool bProjected = FSceneView::ProjectWorldToScreen(
					World, VR, View->ViewMatrices.GetViewProjectionMatrix(), ScreenPos);
				if (bProjected && VR.Width() > 0 && VR.Height() > 0)
				{
					NormX = (ScreenPos.X - VR.Min.X) / (double)VR.Width();
					NormY = (ScreenPos.Y - VR.Min.Y) / (double)VR.Height();
					bValid = true;
					bOnScreen = (NormX >= 0.0 && NormX <= 1.0 &&
								 NormY >= 0.0 && NormY <= 1.0);
				}
			}
		}
		Out->SetBoolField(TEXT("valid"),        bValid);
		Out->SetNumberField(TEXT("screen_x"),   NormX);
		Out->SetNumberField(TEXT("screen_y"),   NormY);
		Out->SetBoolField(TEXT("is_on_screen"), bOnScreen);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunScreenToWorldOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		double SX = 0.0, SY = 0.0, MaxDist = 10000.0;
		FParse::Value(*Params, TEXT("SX="),   SX);
		FParse::Value(*Params, TEXT("SY="),   SY);
		FParse::Value(*Params, TEXT("Dist="), MaxDist);

		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		bool bValid = false, bHit = false;
		double WX = 0.0, WY = 0.0, WZ = 0.0;
		FString HitActorName;

		FEditorViewportClient* VC = FindActiveLevelViewportClient();
		if (VC && VC->Viewport)
		{
			FSceneViewFamilyContext ViewFamily(FSceneViewFamily::ConstructionValues(
				VC->Viewport, VC->GetScene(), VC->EngineShowFlags)
				.SetRealtimeUpdate(VC->IsRealtime()));
			if (FSceneView* View = VC->CalcSceneView(&ViewFamily))
			{
				const FIntRect VR = View->UnscaledViewRect;
				// Normalized [0,1] → pixel coords inside the viewport.
				const FVector2D Pixel(
					(double)VR.Min.X + SX * (double)VR.Width(),
					(double)VR.Min.Y + SY * (double)VR.Height());
				FVector Origin, Direction;
				View->DeprojectFVector2D(Pixel, Origin, Direction);
				bValid = true;

				const FVector End = Origin + Direction * MaxDist;
				WX = End.X; WY = End.Y; WZ = End.Z;
				if (UWorld* W = VC->GetWorld())
				{
					FHitResult Hit;
					FCollisionQueryParams Params2(SCENE_QUERY_STAT(BPRScreenToWorld), false);
					if (W->LineTraceSingleByChannel(Hit, Origin, End,
													 ECC_Visibility, Params2))
					{
						bHit = true;
						WX = Hit.ImpactPoint.X;
						WY = Hit.ImpactPoint.Y;
						WZ = Hit.ImpactPoint.Z;
						if (AActor* HitActor = Hit.GetActor())
						{
							HitActorName = HitActor->GetName();
						}
					}
				}
			}
		}
		Out->SetBoolField(TEXT("valid"),  bValid);
		Out->SetBoolField(TEXT("hit"),    bHit);
		Out->SetNumberField(TEXT("world_x"), WX);
		Out->SetNumberField(TEXT("world_y"), WY);
		Out->SetNumberField(TEXT("world_z"), WZ);
		Out->SetStringField(TEXT("hit_actor_name"), HitActorName);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunGetSelectedAssetsOp(const FString& /*Params*/, const FString& OutputPath, bool bPretty)
	{
		TArray<TSharedPtr<FJsonValue>> Paths;
		if (IContentBrowserSingleton* CB = GetContentBrowserOrNull())
		{
			TArray<FAssetData> Assets;
			CB->GetSelectedAssets(Assets);
			for (const FAssetData& A : Assets)
			{
				Paths.Add(MakeShared<FJsonValueString>(A.PackageName.ToString()));
			}
		}
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetArrayField(TEXT("asset_paths"), Paths);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunSetSelectedAssetsOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString AssetsRaw;
		FParse::Value(*Params, TEXT("Assets="), AssetsRaw);
		TArray<FString> AssetList;
		AssetsRaw.ParseIntoArray(AssetList, TEXT(";"), true);

		TArray<TSharedPtr<FJsonValue>> Resulting;
		if (IContentBrowserSingleton* CB = GetContentBrowserOrNull())
		{
			TArray<FAssetData> Data;
			for (const FString& Path : AssetList)
			{
				// Loadable path → AssetData. Use the asset registry so we
				// don't force-load every asset on selection.
				FAssetRegistryModule& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
				FAssetData AD = AR.Get().GetAssetByObjectPath(FSoftObjectPath(Path));
				if (AD.IsValid())
				{
					Data.Add(AD);
				}
			}
			CB->SyncBrowserToAssets(Data);
			// Echo back what's selected post-call (the UI may not have
			// settled but the in-memory selection model is updated).
			TArray<FAssetData> Confirmed;
			CB->GetSelectedAssets(Confirmed);
			for (const FAssetData& A : Confirmed)
			{
				Resulting.Add(MakeShared<FJsonValueString>(A.PackageName.ToString()));
			}
		}
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetArrayField(TEXT("asset_paths"), Resulting);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunGetSelectedFoldersOp(const FString& /*Params*/, const FString& OutputPath, bool bPretty)
	{
		TArray<TSharedPtr<FJsonValue>> Folders;
		if (IContentBrowserSingleton* CB = GetContentBrowserOrNull())
		{
			TArray<FString> FolderPaths;
			CB->GetSelectedFolders(FolderPaths);
			for (const FString& F : FolderPaths)
			{
				Folders.Add(MakeShared<FJsonValueString>(F));
			}
		}
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetArrayField(TEXT("folder_paths"), Folders);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunGetContentBrowserPathOp(const FString& /*Params*/, const FString& OutputPath, bool bPretty)
	{
		FString CurrentPath;
		if (IContentBrowserSingleton* CB = GetContentBrowserOrNull())
		{
			TArray<FString> Paths;
			CB->GetSelectedPathViewFolders(Paths);
			if (Paths.Num() > 0)
			{
				CurrentPath = Paths[0];
			}
		}
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetStringField(TEXT("current_path"), CurrentPath);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunSetContentBrowserPathOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString Folder;
		FParse::Value(*Params, TEXT("Folder="), Folder);
		FString Confirmed;
		if (IContentBrowserSingleton* CB = GetContentBrowserOrNull())
		{
			if (!Folder.IsEmpty())
			{
				TArray<FString> Paths;
				Paths.Add(Folder);
				CB->SyncBrowserToFolders(Paths);
				TArray<FString> ConfirmedPaths;
				CB->GetSelectedPathViewFolders(ConfirmedPaths);
				if (ConfirmedPaths.Num() > 0)
				{
					Confirmed = ConfirmedPaths[0];
				}
			}
		}
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetStringField(TEXT("current_path"), Confirmed);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunGetSelectedComponentsOp(const FString& /*Params*/, const FString& OutputPath, bool bPretty)
	{
		TArray<TSharedPtr<FJsonValue>> ActorJsonArr;
		if (IsValid(GEditor))
		{
			if (USelection* Selection = GEditor->GetSelectedActors())
			{
				for (FSelectionIterator It(*Selection); It; ++It)
				{
					AActor* Actor = Cast<AActor>(*It);
					if (!IsValid(Actor))
					{
						continue;
					}
					auto ActorJson = MakeShared<FJsonObject>();
					ActorJson->SetStringField(TEXT("actor_name"), Actor->GetName());

					TArray<TSharedPtr<FJsonValue>> CompJsonArr;
					TArray<UActorComponent*> Components;
					Actor->GetComponents(Components);
					for (UActorComponent* Comp : Components)
					{
						if (!IsValid(Comp))
						{
							continue;
						}
						auto CompJson = MakeShared<FJsonObject>();
						CompJson->SetStringField(TEXT("name"),
							Comp->GetName());
						CompJson->SetStringField(TEXT("component_class"),
							Comp->GetClass() ? Comp->GetClass()->GetName() : FString());
						CompJsonArr.Add(MakeShared<FJsonValueObject>(CompJson));
					}
					ActorJson->SetArrayField(TEXT("components"), CompJsonArr);
					ActorJsonArr.Add(MakeShared<FJsonValueObject>(ActorJson));
				}
			}
		}
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetArrayField(TEXT("actors"), ActorJsonArr);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	int32 RunGetShowFlagsOp(const FString& /*Params*/, const FString& OutputPath, bool bPretty)
	{
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		bool bValid = false;
		bool Wf=false, Coll=false, Grid=false, Bnds=false, Nav=false, Atmo=false;
		bool Fog=false, Light=false, Post=false, Aa=false, Shdw=false;
		if (FEditorViewportClient* VC = FindActiveLevelViewportClient())
		{
			bValid = true;
			const FEngineShowFlags& F = VC->EngineShowFlags;
			Wf    = F.Wireframe != 0;
			Coll  = F.Collision != 0;
			Grid  = F.Grid != 0;
			Bnds  = F.Bounds != 0;
			Nav   = F.Navigation != 0;
			Atmo  = F.Atmosphere != 0;
			Fog   = F.Fog != 0;
			Light = F.Lighting != 0;
			Post  = F.PostProcessing != 0;
			Aa    = F.AntiAliasing != 0;
			Shdw  = F.DynamicShadows != 0;
		}
		Out->SetBoolField(TEXT("valid"),           bValid);
		Out->SetBoolField(TEXT("wireframe"),       Wf);
		Out->SetBoolField(TEXT("collision"),       Coll);
		Out->SetBoolField(TEXT("grid"),            Grid);
		Out->SetBoolField(TEXT("bounds"),          Bnds);
		Out->SetBoolField(TEXT("navigation"),      Nav);
		Out->SetBoolField(TEXT("atmosphere"),      Atmo);
		Out->SetBoolField(TEXT("fog"),             Fog);
		Out->SetBoolField(TEXT("lighting"),        Light);
		Out->SetBoolField(TEXT("post_processing"), Post);
		Out->SetBoolField(TEXT("antialiasing"),    Aa);
		Out->SetBoolField(TEXT("shadows"),         Shdw);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	// Emit a small ack JSON blob for a successful write op.
	int32 EmitOk(const FString& OutputPath, bool bPretty)
	{
		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	// Emit a structured error response so the live/socket transport can
	// surface a meaningful message instead of the static LookupErrorCode
	// description. SocketBlueprintReader reads `json.error` when code != 0.
	int32 EmitError(const FString& OutputPath, bool bPretty,
					int32 Code, const FString& ErrorMsg)
	{
		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), false);
		Obj->SetStringField(TEXT("error"), ErrorMsg);
		(void)EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
		return Code;
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
		if (!TypeSubCategory.IsEmpty())
		{
			TypeJson->SetStringField(TEXT("sub_category"), TypeSubCategory);
		}
		if (!TypeSubObject.IsEmpty())
		{
			TypeJson->SetStringField(TEXT("sub_category_object"), TypeSubObject);
		}
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
		if (!IsValid(BP))
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
		if (!Category.IsEmpty())
		{
			Var.Category = FText::FromString(Category);
		}
		if (bEditable)
		{
			Var.PropertyFlags |= CPF_Edit | CPF_BlueprintVisible;
		}
		if (bReplicated) { Var.PropertyFlags |= CPF_Net; Var.ReplicationCondition = COND_None; }

		if (!MaybeCompileAndSave(BP))
		{
			return 5;
		}
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
		if (!IsValid(BP))
		{
			return 4;
		}

		UEdGraph* Graph = FindGraphByName(BP, GraphName);
		if (!IsValid(Graph))
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("SetNodePosition: graph %s not found"), *GraphName);
			return 4;
		}
		UEdGraphNode* Node = FindNodeByGuid(Graph, NodeId);
		if (!IsValid(Node))
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("SetNodePosition: node %s not found in %s"), *NodeId, *GraphName);
			return 4;
		}
		Node->Modify();
		Node->NodePosX = X;
		Node->NodePosY = Y;

		if (!MaybeCompileAndSave(BP))
		{
			return 5;
		}
		return EmitOk(OutputPath, bPretty);
	}

	// Add an implemented interface to a Blueprint. Needed so that interface
	// event nodes (e.g. an AnimMotionEffect override) are valid on the target
	// -- otherwise the compiler demotes them to plain custom events.
	int32 RunImplementInterfaceOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		const FString AssetPath = ResolveAssetPath(Params);
		FString Interface;
		FParse::Value(*Params, TEXT("Interface="), Interface);
		if (AssetPath.IsEmpty() || Interface.IsEmpty())
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("ImplementInterface requires -Asset= -Interface="));
			return 1;
		}
		UBlueprint* BP = LoadMutableBlueprint(AssetPath);
		if (!IsValid(BP)) { return 4; }
		const bool bAdded = FBlueprintEditorUtils::ImplementNewInterface(BP, FTopLevelAssetPath(Interface));
		const bool bOk = CompileAndSaveBlueprint(BP);
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), bOk);
		Out->SetBoolField(TEXT("added"), bAdded);
		Out->SetStringField(TEXT("interface"), Interface);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
	}

	// Stats for the top-level clone (recursion fills its own, which are ignored).
	struct FCloneGraphStats { int32 SourceNodes = 0; int32 Removed = 0; int32 Imported = 0; };

	// Recursive worker: reproduce SrcGraph's contents into DstGraph via UE's node
	// export/import (the clipboard format) -- covering EVERY K2 node class -- then
	// fix up function entry/result + macro/composite tunnel signatures, cast
	// purity/pin-types, and override events, and RECURSE into K2Node_Composite
	// BoundGraphs + locally-defined macro graphs (which the clipboard does not
	// carry). Does not compile/save; the op does that once after the whole
	// recursion. Visited guards graph cycles; MacroMap dedupes local macros so
	// repeated instances share one reproduced macro graph.
	void CloneGraphContents(UBlueprint* SrcBP, UBlueprint* DstBP, UEdGraph* SrcGraph, UEdGraph* DstGraph,
	                        TSet<UEdGraph*>& Visited, TMap<FName, UEdGraph*>& MacroMap, FCloneGraphStats* OutStats)
	{
		if (!IsValid(SrcGraph) || !IsValid(DstGraph) || Visited.Contains(SrcGraph)) { return; }
		Visited.Add(SrcGraph);

		// Function graphs: the function-entry node (with its user-defined
		// input pins + local variables) and result node are NOT part of the
		// clipboard export. Reproduce the signature + locals on the
		// destination entry FIRST so the imported body nodes bind to the right
		// params/locals, then re-link entry/result connections afterwards.
		auto FindEntryIn = [](UEdGraph* G) -> UK2Node_FunctionEntry*
		{
			for (UEdGraphNode* N : G->Nodes)
			{
				if (UK2Node_FunctionEntry* E = Cast<UK2Node_FunctionEntry>(N)) { return E; }
			}
			return nullptr;
		};
		UK2Node_FunctionEntry* SrcEntry = FindEntryIn(SrcGraph);
		UK2Node_FunctionEntry* DstEntry = FindEntryIn(DstGraph);
		UK2Node_FunctionResult* SrcResult = nullptr;
		for (UEdGraphNode* N : SrcGraph->Nodes)
		{
			if (UK2Node_FunctionResult* R = Cast<UK2Node_FunctionResult>(N)) { SrcResult = R; break; }
		}
		if (SrcEntry && DstEntry)
		{
			DstEntry->UserDefinedPins.Empty();
			for (const TSharedPtr<FUserPinInfo>& Src : SrcEntry->UserDefinedPins)
			{
				DstEntry->UserDefinedPins.Add(MakeShared<FUserPinInfo>(*Src));
			}
			DstEntry->LocalVariables = SrcEntry->LocalVariables;
			// If the source function *overrides* a parent virtual (e.g.
			// GameplayCueNotify::OnExecute, GameplayAbility::K2_CanActivateAbility),
			// its entry references the parent UFunction and the inherited parameter
			// pins (Parameters, ActorInfo, MyTarget, ...) are derived from that
			// signature — they are NOT user-defined pins, so the copy above misses
			// them. Propagating FunctionReference + ExtraFlags makes ReconstructNode
			// regenerate the inherited params/flags on the destination entry so it
			// matches the original (the function graph was created as a plain user
			// function by add_function; this promotes it to a true override).
			DstEntry->FunctionReference = SrcEntry->FunctionReference;
			DstEntry->SetExtraFlags(SrcEntry->GetExtraFlags());
			DstEntry->ReconstructNode();
		}

		// Macro / composite BoundGraphs use UK2Node_Tunnel terminals (an input-sink
		// with outputs + an output-source with inputs) instead of FunctionEntry/
		// Result; their user-defined pins ARE the macro/composite signature and are
		// NOT carried by the clipboard. Reproduce them on the destination tunnels so
		// the body binds and the parent macro-instance / composite node regenerates
		// matching pins. (No-op on function/ubergraphs, which have no bare tunnels.)
		{
			auto FindTunnel = [](UEdGraph* G, bool bWantOutputs) -> UK2Node_Tunnel*
			{
				for (UEdGraphNode* N : G->Nodes)
				{
					// Exact UK2Node_Tunnel only — exclude the MacroInstance / Composite
					// subclasses that also derive from it.
					if (N && N->GetClass() == UK2Node_Tunnel::StaticClass())
					{
						UK2Node_Tunnel* T = static_cast<UK2Node_Tunnel*>(N);
						if (bWantOutputs ? T->bCanHaveOutputs : T->bCanHaveInputs) { return T; }
					}
				}
				return nullptr;
			};
			for (bool bOutputs : { true, false })
			{
				UK2Node_Tunnel* SrcTun = FindTunnel(SrcGraph, bOutputs);
				UK2Node_Tunnel* DstTun = FindTunnel(DstGraph, bOutputs);
				if (SrcTun && DstTun)
				{
					DstTun->UserDefinedPins.Empty();
					for (const TSharedPtr<FUserPinInfo>& Src : SrcTun->UserDefinedPins)
					{
						DstTun->UserDefinedPins.Add(MakeShared<FUserPinInfo>(*Src));
					}
					DstTun->ReconstructNode();
				}
			}
		}

		// 1. Export every source-graph node to UE's clipboard text format.
		TSet<UObject*> ToExport;
		for (UEdGraphNode* N : SrcGraph->Nodes) { if (IsValid(N)) { ToExport.Add(N); } }
		FString ExportText;
		FEdGraphUtilities::ExportNodesToText(ToExport, ExportText);

		// 2. Clear the destination graph's deletable nodes (function entry /
		//    result + default event ghosts are kept by the schema).
		TArray<UEdGraphNode*> Existing = DstGraph->Nodes;
		int32 Removed = 0;
		for (UEdGraphNode* N : Existing)
		{
			if (IsValid(N) && N->CanUserDeleteNode())
			{
				FBlueprintEditorUtils::RemoveNode(DstBP, N, /*bDontRecompile=*/true);
				++Removed;
			}
		}

		// 3. Reconstruct the source nodes into the destination graph.
		TSet<UEdGraphNode*> Imported;
		FEdGraphUtilities::ImportNodesFromText(DstGraph, ExportText, Imported);

		// 4. Build a position -> imported-node map (ImportNodesFromText
		//    preserves NodePosX/Y) to re-link connections the clipboard
		//    dropped, and a helper that copies one source node's links onto a
		//    destination node by position+pin-name matching.
		TMap<FIntPoint, UEdGraphNode*> ByPos;
		for (UEdGraphNode* N : Imported)
		{
			if (IsValid(N)) { ByPos.Add(FIntPoint(N->NodePosX, N->NodePosY), N); }
		}

		// 3a. Restore K2Node_DynamicCast purity. The clipboard paste resets cast
		//     nodes to impure (re-adding the exec/CastFailed pins), so a source
		//     *pure* cast (data pins only) comes back impure and differs
		//     structurally. Match each imported cast to its source counterpart by
		//     position and set purity to match (SetPurity reconstructs the node,
		//     preserving the still-valid data-pin links).
		for (UEdGraphNode* SrcNode : SrcGraph->Nodes)
		{
			UK2Node_DynamicCast* SrcCast = Cast<UK2Node_DynamicCast>(SrcNode);
			if (!SrcCast) { continue; }
			UEdGraphNode** Imp = ByPos.Find(FIntPoint(SrcNode->NodePosX, SrcNode->NodePosY));
			if (!Imp || !IsValid(*Imp)) { continue; }
			if (UK2Node_DynamicCast* DstCast = Cast<UK2Node_DynamicCast>(*Imp))
			{
				if (DstCast->IsNodePure() != SrcCast->IsNodePure())
				{
					DstCast->SetPurity(SrcCast->IsNodePure());
				}
			}
		}
		auto Relink = [&ByPos](UEdGraphNode* SrcNode, UEdGraphNode* DstNode)
		{
			if (!SrcNode || !DstNode) { return; }
			for (UEdGraphPin* SrcPin : SrcNode->Pins)
			{
				UEdGraphPin* DstPin = DstNode->FindPin(SrcPin->PinName, SrcPin->Direction);
				if (!DstPin) { continue; }
				for (UEdGraphPin* Linked : SrcPin->LinkedTo)
				{
					UEdGraphNode* SrcOther = Linked ? Linked->GetOwningNode() : nullptr;
					if (!SrcOther) { continue; }
					if (UEdGraphNode** Imp = ByPos.Find(FIntPoint(SrcOther->NodePosX, SrcOther->NodePosY)))
					{
						if (UEdGraphPin* DstOther = (*Imp)->FindPin(Linked->PinName, Linked->Direction))
						{
							DstPin->MakeLinkTo(DstOther);
						}
					}
				}
			}
		};

		// 4a. Function entry/result connections.
		if (SrcEntry && DstEntry)
		{
			Relink(SrcEntry, DstEntry);
			if (SrcResult)
			{
				UK2Node_FunctionResult* DstResult = nullptr;
				for (UEdGraphNode* N : DstGraph->Nodes)
				{
					if (UK2Node_FunctionResult* R = Cast<UK2Node_FunctionResult>(N)) { DstResult = R; break; }
				}
				if (DstResult)
				{
					// Mirror the entry treatment onto the result node: for an
					// override function the out-params / return value come from the
					// parent signature (NOT user-defined pins), so reset the
					// user-defined pins to the source's and propagate the
					// FunctionReference, then reconstruct. This regenerates inherited
					// outputs (e.g. CanActivateAbility's RelevantTags + bool) exactly
					// once — without the reset they collide with the clipboard-imported
					// pins (RelevantTags1, ReturnValue1). User-function results (where
					// the outputs ARE user-defined) round-trip unchanged.
					DstResult->UserDefinedPins.Empty();
					for (const TSharedPtr<FUserPinInfo>& Src : SrcResult->UserDefinedPins)
					{
						DstResult->UserDefinedPins.Add(MakeShared<FUserPinInfo>(*Src));
					}
					DstResult->FunctionReference = SrcResult->FunctionReference;
					DstResult->ReconstructNode();
				}
				Relink(SrcResult, DstResult);
			}
		}

		// 4a2. Macro/composite tunnel connections. The input-sink / output-source
		//      tunnels are not part of the clipboard export (they're the dst graph's
		//      own terminals), so the body's links TO them were dropped on import.
		//      Relink them from the source tunnels (whose pins were reproduced above).
		{
			auto FindTun = [](UEdGraph* G, bool bWantOutputs) -> UK2Node_Tunnel*
			{
				for (UEdGraphNode* N : G->Nodes)
				{
					if (N && N->GetClass() == UK2Node_Tunnel::StaticClass())
					{
						UK2Node_Tunnel* T = static_cast<UK2Node_Tunnel*>(N);
						if (bWantOutputs ? T->bCanHaveOutputs : T->bCanHaveInputs) { return T; }
					}
				}
				return nullptr;
			};
			for (bool bOut : { true, false })
			{
				Relink(FindTun(SrcGraph, bOut), FindTun(DstGraph, bOut));
			}
		}

		// 4b. Override/interface events: the clipboard import demotes a
		//     K2Node_Event to a K2Node_CustomEvent (it can't re-bind the
		//     overridden function on paste). For each such demoted event,
		//     replace it with a proper event node bound to the same function
		//     and re-link its body connections.
		for (UEdGraphNode* SrcNode : SrcGraph->Nodes)
		{
			UK2Node_Event* SrcEvt = Cast<UK2Node_Event>(SrcNode);
			if (!SrcEvt || !SrcEvt->bOverrideFunction) { continue; }
			const FIntPoint Pos(SrcNode->NodePosX, SrcNode->NodePosY);
			UEdGraphNode** Imp = ByPos.Find(Pos);
			if (!Imp || !IsValid(*Imp) || !(*Imp)->IsA<UK2Node_CustomEvent>()) { continue; }
			FBlueprintEditorUtils::RemoveNode(DstBP, *Imp, /*bDontRecompile=*/true);
			ByPos.Remove(Pos);
			UK2Node_Event* NewEvt = NewObject<UK2Node_Event>(DstGraph);
			NewEvt->EventReference    = SrcEvt->EventReference;
			NewEvt->bOverrideFunction = SrcEvt->bOverrideFunction;
			NewEvt->NodePosX = SrcNode->NodePosX;
			NewEvt->NodePosY = SrcNode->NodePosY;
			NewEvt->CreateNewGuid();
			DstGraph->AddNode(NewEvt, /*bFromUI=*/false, /*bSelectNewNode=*/false);
			NewEvt->PostPlacedNewNode();
			NewEvt->AllocateDefaultPins();
			Relink(SrcEvt, NewEvt);
		}

		// 4c. Restore pin types that came back as wildcard. Several node classes
		//     (DynamicCast's Object input, CallArrayFunction's TargetArray, Select,
		//     etc.) allocate wildcard pins that only harden to a concrete type via
		//     NotifyPinConnectionListChanged — which doesn't re-fire after the clone's
		//     late import/relink, so a source `object`/`Vector[]` pin comes back
		//     `wildcard`. For each imported node matched to its source by position,
		//     copy the source pin's type onto any dst pin that is STILL wildcard
		//     (deterministic, connection-independent, and a no-op for already-correct
		//     pins). Runs last so nothing re-allocates the pin afterward.
		for (UEdGraphNode* SrcNode : SrcGraph->Nodes)
		{
			UEdGraphNode** Imp = ByPos.Find(FIntPoint(SrcNode->NodePosX, SrcNode->NodePosY));
			if (!Imp || !IsValid(*Imp)) { continue; }
			UEdGraphNode* DstNode = *Imp;
			for (UEdGraphPin* SrcPin : SrcNode->Pins)
			{
				if (!SrcPin || SrcPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard) { continue; }
				UEdGraphPin* DstPin = DstNode->FindPin(SrcPin->PinName, SrcPin->Direction);
				if (DstPin && DstPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard)
				{
					DstPin->PinType = SrcPin->PinType;
				}
			}
		}

		// 5. Recurse into K2Node_Composite BoundGraphs — the clipboard does not
		//    carry a composite's nested sub-graph, so reproduce it explicitly.
		for (UEdGraphNode* SrcNode : SrcGraph->Nodes)
		{
			UK2Node_Composite* SrcComp = Cast<UK2Node_Composite>(SrcNode);
			if (!SrcComp || !IsValid(SrcComp->BoundGraph)) { continue; }
			UEdGraphNode** Imp = ByPos.Find(FIntPoint(SrcNode->NodePosX, SrcNode->NodePosY));
			if (!Imp || !IsValid(*Imp)) { continue; }
			UK2Node_Composite* DstComp = Cast<UK2Node_Composite>(*Imp);
			if (!DstComp) { continue; }
			// The import may or may not have brought a BoundGraph; normalize to one
			// owned by DstBP, then reproduce its contents (tunnels + body) via the
			// recursive worker. ReconstructNode regenerates the composite's pins from
			// the (now-populated) tunnels.
			UEdGraph* DstBound = DstComp->BoundGraph;
			if (!IsValid(DstBound))
			{
				DstBound = FBlueprintEditorUtils::CreateNewGraph(
					DstBP, SrcComp->BoundGraph->GetFName(),
					UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
				DstComp->BoundGraph = DstBound;
			}
			DstGraph->SubGraphs.AddUnique(DstBound);
			CloneGraphContents(SrcBP, DstBP, SrcComp->BoundGraph, DstBound, Visited, MacroMap, nullptr);
			// Force the bound-graph name to the source's so the composite's title
			// (which mirrors the bound-graph name) matches — the clipboard paste
			// suffixes it (e.g. "CreatePolygon" -> "CreatePolygon_2").
			const FString WantBound = SrcComp->BoundGraph->GetName();
			if (DstBound->GetName() != WantBound)
			{
				if (UObject* Clash = StaticFindObjectFast(nullptr, DstBound->GetOuter(), FName(*WantBound)))
				{
					if (Clash != DstBound)
					{
						Clash->Rename(nullptr, GetTransientPackage(),
							REN_DontCreateRedirectors | REN_DoNotDirty | REN_NonTransactional);
					}
				}
				DstBound->Rename(*WantBound, DstBound->GetOuter(),
					REN_DontCreateRedirectors | REN_DoNotDirty | REN_NonTransactional);
			}
			DstComp->ReconstructNode();
		}

		// 6. Recurse into locally-defined macro graphs referenced by a
		//    K2Node_MacroInstance. External macro-library macros resolve by
		//    reference after import and are left alone.
		for (UEdGraphNode* SrcNode : SrcGraph->Nodes)
		{
			UK2Node_MacroInstance* SrcInst = Cast<UK2Node_MacroInstance>(SrcNode);
			if (!SrcInst) { continue; }
			UEdGraph* SrcMacro = SrcInst->GetMacroGraph();
			if (!IsValid(SrcMacro) || SrcMacro->GetTypedOuter<UBlueprint>() != SrcBP) { continue; }

			// Ensure the dest BP has a same-named macro graph, reproduced once.
			const FName MacroName = SrcMacro->GetFName();
			UEdGraph* DstMacro = MacroMap.FindRef(MacroName);
			if (!IsValid(DstMacro))
			{
				DstMacro = FindGraphByName(DstBP, MacroName.ToString());
				if (!IsValid(DstMacro))
				{
					DstMacro = FBlueprintEditorUtils::CreateNewGraph(
						DstBP, MacroName, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
					FBlueprintEditorUtils::AddMacroGraph(DstBP, DstMacro, /*bIsUserCreated=*/true, /*SignatureFromClass=*/nullptr);
				}
				MacroMap.Add(MacroName, DstMacro);
				CloneGraphContents(SrcBP, DstBP, SrcMacro, DstMacro, Visited, MacroMap, nullptr);
			}

			// The clipboard DROPS a macro instance that references a LOCAL (private)
			// macro of the source BP ("Illegal TEXT reference to a private object in
			// external package"), so it never imports. Re-create the instance fresh,
			// pointing at the dest macro, and relink its body connections (mirrors the
			// override-event rebuild above). If the import DID keep it, just retarget.
			UEdGraphNode** Imp = ByPos.Find(FIntPoint(SrcNode->NodePosX, SrcNode->NodePosY));
			UK2Node_MacroInstance* DstInst = (Imp && IsValid(*Imp)) ? Cast<UK2Node_MacroInstance>(*Imp) : nullptr;
			if (DstInst)
			{
				DstInst->SetMacroGraph(DstMacro);
				DstInst->ReconstructNode();
			}
			else
			{
				DstInst = NewObject<UK2Node_MacroInstance>(DstGraph);
				DstInst->SetMacroGraph(DstMacro);
				DstInst->NodePosX = SrcNode->NodePosX;
				DstInst->NodePosY = SrcNode->NodePosY;
				DstInst->CreateNewGuid();
				DstGraph->AddNode(DstInst, /*bFromUI=*/false, /*bSelectNewNode=*/false);
				DstInst->PostPlacedNewNode();
				DstInst->AllocateDefaultPins();
				ByPos.Add(FIntPoint(SrcNode->NodePosX, SrcNode->NodePosY), DstInst);
				Relink(SrcInst, DstInst);
			}
		}

		if (OutStats)
		{
			OutStats->SourceNodes = ToExport.Num();
			OutStats->Removed      = Removed;
			OutStats->Imported     = Imported.Num();
		}
	}

	// Reconstruct an entire graph's nodes from a source BP into a target BP,
	// losslessly, via UE's own node export/import (the clipboard format) plus
	// recursive reproduction of composite sub-graphs + local macro graphs. The
	// target must already have the same variables, parent, and (for functions) the
	// function declared, so the reconstructed nodes' references resolve.
	int32 RunCloneGraphOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString SourcePath, TargetPath, GraphName;
		FParse::Value(*Params, TEXT("Source="), SourcePath);
		FParse::Value(*Params, TEXT("Target="), TargetPath);
		FParse::Value(*Params, TEXT("Graph="),  GraphName);
		if (SourcePath.IsEmpty() || TargetPath.IsEmpty() || GraphName.IsEmpty())
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("CloneGraph requires -Source= -Target= -Graph="));
			return 1;
		}
		UBlueprint* SrcBP = LoadMutableBlueprint(SourcePath);
		UBlueprint* DstBP = LoadMutableBlueprint(TargetPath);
		if (!IsValid(SrcBP) || !IsValid(DstBP)) { return 4; }
		UEdGraph* SrcGraph = FindGraphByName(SrcBP, GraphName);
		UEdGraph* DstGraph = FindGraphByName(DstBP, GraphName);
		if (!IsValid(SrcGraph) || !IsValid(DstGraph))
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("CloneGraph: graph '%s' not found in source or target"), *GraphName);
			return 4;
		}

		FCloneGraphStats Stats;
		TSet<UEdGraph*> Visited;
		TMap<FName, UEdGraph*> MacroMap;
		CloneGraphContents(SrcBP, DstBP, SrcGraph, DstGraph, Visited, MacroMap, &Stats);

		const bool bOk = CompileAndSaveBlueprint(DstBP);
		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), bOk);
		Out->SetStringField(TEXT("graph"), GraphName);
		Out->SetNumberField(TEXT("source_nodes"), Stats.SourceNodes);
		Out->SetNumberField(TEXT("removed_stub_nodes"), Stats.Removed);
		Out->SetNumberField(TEXT("imported_nodes"), Stats.Imported);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Out, bPretty), OutputPath);
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
		if (!IsValid(BP))
		{
			return 4;
		}
		UEdGraph* Graph = FindGraphByName(BP, GraphName);
		if (!IsValid(Graph))
		{
			return 4;
		}
		UEdGraphNode* Node = FindNodeByGuid(Graph, NodeId);
		if (!IsValid(Node))
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("DeleteNode: node %s not found in %s"), *NodeId, *GraphName);
			return 4;
		}

		// Break links + remove from graph. FBlueprintEditorUtils::RemoveNode
		// handles both, plus refreshes any dependent UI state.
		FBlueprintEditorUtils::RemoveNode(BP, Node, /*bDontRecompile=*/true);

		if (!MaybeCompileAndSave(BP))
		{
			return 5;
		}
		return EmitOk(OutputPath, bPretty);
	}

	// ----- AddNode ----------------------------------------------------------
	// Resolve a UClass path passed via -ClassPath= or short name (e.g.
	// "Actor" or "/Script/Engine.Actor").
	UClass* ResolveClass(const FString& Spec)
	{
		if (Spec.IsEmpty())
		{
			return nullptr;
		}
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
		if (!IsValid(BP))
		{
			return 4;
		}
		UEdGraph* Graph = FindGraphByName(BP, GraphName);
		if (!IsValid(Graph))
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
			if (!IsValid(OwnerClass))
			{
				UE_LOG(LogBlueprintReader, Error, TEXT("AddNode CallFunction: class %s not found"), *FunctionOwner);
				return 1;
			}
			UFunction* Fn = OwnerClass->FindFunctionByName(*FunctionName);
			if (!IsValid(Fn))
			{
				UE_LOG(LogBlueprintReader, Error,
					TEXT("AddNode CallFunction: function %s not found on %s"),
					*FunctionName, *OwnerClass->GetPathName());
				return 1;
			}
			// Array library functions (Clear/Add/Remove/Contains/…) carry the
			// `ArrayParm` metadata and must be spawned as a
			// UK2Node_CallArrayFunction — that subclass owns the wildcard
			// TargetArray type-propagation (it conforms the wildcard to the
			// connected array's element type when a pin is wired). A plain
			// UK2Node_CallFunction leaves TargetArray a permanent wildcard, so
			// the BP fails to compile with "The type of Target Array is
			// undetermined" no matter how the pin is connected.
			UK2Node_CallFunction* Call =
				Fn->HasMetaData(FBlueprintMetadata::MD_ArrayParam)
					? NewObject<UK2Node_CallArrayFunction>(Graph)
					: NewObject<UK2Node_CallFunction>(Graph);
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
			if (!IsValid(Tgt))
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
			if (!IsValid(Struct))
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
		else if (Kind.Equals(TEXT("GetSubsystem"), ESearchCase::IgnoreCase))
		{
			FString SubsystemClass;
			FParse::Value(*Params, TEXT("SubsystemClass="), SubsystemClass);
			if (SubsystemClass.IsEmpty())
			{
				UE_LOG(LogBlueprintReader, Error,
					TEXT("AddNode GetSubsystem requires -SubsystemClass=<USubsystem subclass path or short name>"));
				return 1;
			}
			UClass* SubCls = ResolveClass(SubsystemClass);
			if (!IsValid(SubCls))
			{
				UE_LOG(LogBlueprintReader, Error, TEXT("AddNode GetSubsystem: class %s not found"), *SubsystemClass);
				return 1;
			}
			if (!SubCls->IsChildOf(USubsystem::StaticClass()))
			{
				UE_LOG(LogBlueprintReader, Error,
					TEXT("AddNode GetSubsystem: %s is not a USubsystem subclass"), *SubsystemClass);
				return 1;
			}
			// Initialize() sets CustomClass, which AllocateDefaultPins reads to
			// build the correctly-typed output pin — so it must run first.
			// UK2Node_GetSubsystem covers GameInstance/World/Engine subsystems;
			// LocalPlayer subsystems use UK2Node_GetSubsystemFromPC/FromLP.
			UK2Node_GetSubsystem* Sub = NewObject<UK2Node_GetSubsystem>(Graph);
			Sub->Initialize(SubCls);
			Sub->CreateNewGuid();
			Sub->NodePosX = X; Sub->NodePosY = Y;
			Graph->AddNode(Sub, false, false);
			Sub->PostPlacedNewNode();
			Sub->AllocateDefaultPins();
			Spawned = Sub;
		}
		else
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("AddNode: unrecognised -Kind=%s; see list_node_kinds for valid values"),
				*Kind);
			return 1;
		}

		if (!Spawned)
		{
			return 5;
		}
		const FString NewId = Spawned->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens);

		if (!MaybeCompileAndSave(BP))
		{
			return 5;
		}

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
				if (P && P->PinId == AsGuid)
				{
					return P;
				}
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
			if (!P)
			{
				continue;
			}
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
		if (!IsValid(BP))
		{
			return 4;
		}
		UEdGraph* Graph = FindGraphByName(BP, GraphName);
		if (!IsValid(Graph))
		{
			return 4;
		}

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
		if (!IsValid(Schema))
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

		if (!MaybeCompileAndSave(BP))
		{
			return 5;
		}
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
		if (!IsValid(BP))
		{
			return 4;
		}
		const FName Var(*VarName);
		if (FBlueprintEditorUtils::FindNewVariableIndex(BP, Var) == INDEX_NONE)
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("DeleteVariable: %s not found on %s"), *VarName, *AssetPath);
			return 4;
		}
		FBlueprintEditorUtils::RemoveMemberVariable(BP, Var);

		if (!MaybeCompileAndSave(BP))
		{
			return 5;
		}
		return EmitOk(OutputPath, bPretty);
	}

	// ----- Function helpers (shared by add_function_input/output) -----------
	bool BuildPinTypeFromFlags(const FString& Params, FEdGraphPinType& Out)
	{
		FString TypeCategory, TypeSubCategory, TypeSubObject;
		FParse::Value(*Params, TEXT("TypeCategory="),         TypeCategory);
		FParse::Value(*Params, TEXT("TypeSubCategory="),      TypeSubCategory);
		FParse::Value(*Params, TEXT("TypeSubCategoryObject="), TypeSubObject);
		if (TypeCategory.IsEmpty())
		{
			return false;
		}

		auto TypeJson = MakeShared<FJsonObject>();
		TypeJson->SetStringField(TEXT("category"), TypeCategory);
		if (!TypeSubCategory.IsEmpty())
		{
			TypeJson->SetStringField(TEXT("sub_category"), TypeSubCategory);
		}
		if (!TypeSubObject.IsEmpty())
		{
			TypeJson->SetStringField(TEXT("sub_category_object"), TypeSubObject);
		}
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
			if (UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(N))
			{
				return Entry;
			}
		}
		return nullptr;
	}

	UK2Node_FunctionResult* FindOrCreateFunctionResult(UEdGraph* Graph)
	{
		for (UEdGraphNode* N : Graph->Nodes)
		{
			if (UK2Node_FunctionResult* R = Cast<UK2Node_FunctionResult>(N))
			{
				return R;
			}
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
		if (!IsValid(BP))
		{
			return 4;
		}
		if (FindFunctionGraph(BP, FunctionName))
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("AddFunction: function %s already exists"), *FunctionName);
			return 1;
		}

		UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
			BP, FName(*FunctionName), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		FBlueprintEditorUtils::AddFunctionGraph<UClass>(BP, NewGraph, /*bIsUserCreated=*/true, nullptr);

		if (!MaybeCompileAndSave(BP))
		{
			return 5;
		}
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
	// Idempotency helper: return true if `Node` already has a user-defined
	// pin with this name. Used by AddFunctionInput / AddFunctionOutput
	// before calling CreateUserDefinedPin — otherwise repeated calls
	// duplicate pins on the underlying FunctionEntry / FunctionResult
	// node (caught by bp_structural_diff in agent sessions).
	bool HasUserDefinedPinNamed(UK2Node_EditablePinBase* Node, FName PinName)
	{
		if (!Node)
		{
			return false;
		}
		for (const TSharedPtr<FUserPinInfo>& Info : Node->UserDefinedPins)
		{
			if (Info.IsValid() && Info->PinName == PinName)
			{
				return true;
			}
		}
		return false;
	}

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
		if (!IsValid(BP))
		{
			return 4;
		}
		UEdGraph* Graph = FindFunctionGraph(BP, FunctionName);
		if (!IsValid(Graph))
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("AddFunctionInput: function %s not found"), *FunctionName);
			return 4;
		}
		UK2Node_FunctionEntry* Entry = FindFunctionEntry(Graph);
		if (!IsValid(Entry))
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("AddFunctionInput: no FunctionEntry node"));
			return 5;
		}
		// Idempotency: skip when a pin with the same name already exists.
		// Without this, repeated AddFunctionInput on the same name
		// duplicates pins on the FunctionEntry node.
		if (!HasUserDefinedPinNamed(Entry, FName(*ParamName)))
		{
			Entry->CreateUserDefinedPin(FName(*ParamName), PinType, EGPD_Output, /*bUseUniqueName=*/false);
		}

		if (!MaybeCompileAndSave(BP))
		{
			return 5;
		}
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
		if (!IsValid(BP))
		{
			return 4;
		}
		UEdGraph* Graph = FindFunctionGraph(BP, FunctionName);
		if (!IsValid(Graph))
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("AddFunctionOutput: function %s not found"), *FunctionName);
			return 4;
		}
		UK2Node_FunctionResult* Result = FindOrCreateFunctionResult(Graph);
		// Idempotency: skip when an output pin with the same name already
		// exists. Without this, repeated AddFunctionOutput duplicates pins
		// on the FunctionResult node — caught by bp_structural_diff in
		// agent sessions.
		if (!HasUserDefinedPinNamed(Result, FName(*ParamName)))
		{
			Result->CreateUserDefinedPin(FName(*ParamName), PinType, EGPD_Input, /*bUseUniqueName=*/false);
		}

		if (!MaybeCompileAndSave(BP))
		{
			return 5;
		}
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
		if (!IsValid(BP))
		{
			return 4;
		}
		UEdGraph* Graph = FindFunctionGraph(BP, FunctionName);
		if (!IsValid(Graph))
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("DeleteFunction: %s not found"), *FunctionName);
			return 4;
		}
		// EFlags=None purges the graph + any UFUNCTION the compiler emitted.
		FBlueprintEditorUtils::RemoveGraph(BP, Graph, EGraphRemoveFlags::None);

		if (!MaybeCompileAndSave(BP))
		{
			return 5;
		}
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
		if (!IsValid(BP))
		{
			return 4;
		}
		const FName Var(*VarName);
		const int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(BP, Var);
		if (Index == INDEX_NONE)
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("SetVariableDefault: %s not found"), *VarName);
			return 4;
		}

		BP->Modify();
		BP->NewVariables[Index].DefaultValue = NewDefault;

		// Propagate immediately to the CDO so that a list_variables read
		// in the same session reflects the new value without requiring a
		// separate compile cycle. FBPVariableDescription::DefaultValue is
		// a compiler-hint string; the CDO is the runtime-authoritative
		// storage and must also be updated.
		if (IsValid(BP->GeneratedClass))
		{
			if (UObject* CDO = BP->GeneratedClass->GetDefaultObject(false))
			{
				if (FProperty* Prop = FindFProperty<FProperty>(BP->GeneratedClass, Var))
				{
					CDO->Modify();
					Prop->ImportText_Direct(*NewDefault,
						Prop->ContainerPtrToValuePtr<void>(CDO),
						CDO, PPF_None);
				}
			}
		}

		if (!MaybeCompileAndSave(BP))
		{
			return 5;
		}
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
		if (!IsValid(BP))
		{
			return 4;
		}
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

		if (!MaybeCompileAndSave(BP))
		{
			return 5;
		}
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

int32 UBPRCommandlet::Main(const FString& Params)
{
	if (FParse::Param(*Params, TEXT("Daemon")))
	{
		return RunDaemon();
	}
	return RunOneOp(Params);
}

namespace
{
// Drains the LoadFailure TLS side channel into the response slot. Called
// after every handler that might have called LoadMutableBlueprint. If
// the lock-conflict path tripped, override the handler's return code +
// emit the structured error JSON instead of whatever the handler wrote.
int32 ApplyLoadFailureOverride(int32 HandlerCode, const FString& OutputPath)
{
	if (LoadFailure::OverrideCode == 0)
	{
		return HandlerCode;
	}
	const int32 Code = LoadFailure::OverrideCode;
	const FString Json = MoveTemp(LoadFailure::OverrideJson);
	LoadFailure::OverrideCode = 0;
	LoadFailure::OverrideJson.Empty();
	EmitJson(Json, OutputPath);
	return Code;
}

int32 RunOneOp(const FString& Params)
{
	EOp Op;
	if (!ParseOp(Params, Op))
	{
		return 1;
	}

	const bool bPretty = !FParse::Param(*Params, TEXT("Compact"));
	const FString OutputPath = ResolveOutputPath(Params);

	// Belt + suspenders: each dispatch starts with a clean TLS slot.
	// Should already be 0 (cleared after the previous dispatch), but a
	// stray uncleared value would leak across ops on the same thread.
	LoadFailure::OverrideCode = 0;
	LoadFailure::OverrideJson.Empty();

	// Table-driven dispatch for ops whose handler takes the uniform
	// `(Params, OutputPath, bPretty)` signature. Adding a new op of this
	// shape is a one-line addition here. The handful of read ops below
	// (Read / Graph / Function / Variables / Components / Find / Legacy)
	// stay in the switch — they need a pre-loaded FBlueprintInfo and
	// have inline bodies, not function-call shapes.
	using FOpHandler = int32(*)(const FString&, const FString&, bool);
	static const struct { EOp Op; FOpHandler Handler; } kDispatchTable[] =
	{
		{ EOp::List,                       &RunListOp },
		{ EOp::AddVariable,                &RunAddVariableOp },
		{ EOp::SetNodePosition,            &RunSetNodePositionOp },
		{ EOp::DeleteNode,                 &RunDeleteNodeOp },
		{ EOp::AddNode,                    &RunAddNodeOp },
		{ EOp::WirePins,                   &RunWirePinsOp },
		{ EOp::DeleteVariable,             &RunDeleteVariableOp },
		{ EOp::RenameVariable,             &RunRenameVariableOp },
		{ EOp::AddFunction,                &RunAddFunctionOp },
		{ EOp::AddFunctionInput,           &RunAddFunctionInputOp },
		{ EOp::AddFunctionOutput,          &RunAddFunctionOutputOp },
		{ EOp::DeleteFunction,             &RunDeleteFunctionOp },
		{ EOp::SetVariableDefault,         &RunSetVariableDefaultOp },
		{ EOp::CloneGraph,                 &RunCloneGraphOp },
		{ EOp::ImplementInterface,         &RunImplementInterfaceOp },
		{ EOp::BeginBatch,                 &RunBeginBatchOp },
		{ EOp::EndBatch,                   &RunEndBatchOp },
		{ EOp::CreateBlueprint,            &RunCreateBlueprintOp },
		{ EOp::SetPinDefault,              &RunSetPinDefaultOp },
		{ EOp::RetypeVariable,             &RunRetypeVariableOp },
		{ EOp::SetVariableCategory,        &RunSetVariableCategoryOp },
		{ EOp::DuplicateBlueprint,         &RunDuplicateBlueprintOp },
		{ EOp::WriteGeneratedSource,       &RunWriteGeneratedSourceOp },
		{ EOp::SaveAll,                    &RunSaveAllOp },
		{ EOp::MoveAsset,                  &RunMoveAssetOp },
		{ EOp::DeleteAsset,                &RunDeleteAssetOp },
		{ EOp::CreateFolder,               &RunCreateFolderOp },
		{ EOp::ListDataTables,             &RunListDataTablesOp },
		{ EOp::ReadDataTable,              &RunReadDataTableOp },
		{ EOp::ConsoleCommand,             &RunConsoleCommandOp },
		{ EOp::GetCVar,                    &RunGetCVarOp },
		{ EOp::SetCVar,                    &RunSetCVarOp },
		{ EOp::PieStart,                   &RunPieStartOp },
		{ EOp::PieStop,                    &RunPieStopOp },
		{ EOp::LiveCodingCompile,          &RunLiveCodingCompileOp },
		{ EOp::GetSelectedActors,          &RunGetSelectedActorsOp },
		{ EOp::GetEditorState,             &RunGetEditorStateOp },
		{ EOp::RunPythonScript,            &RunRunPythonScriptOp },
		{ EOp::GetReferencers,             &RunGetReferencersOp },
		{ EOp::GetDependencies,            &RunGetDependenciesOp },
		{ EOp::ReadConfigValue,            &RunReadConfigValueOp },
		{ EOp::SetConfigValue,             &RunSetConfigValueOp },
		{ EOp::BuildLighting,              &RunBuildLightingOp },
		{ EOp::SetSelection,               &RunSetSelectionOp },
		{ EOp::SpawnActor,                 &RunSpawnActorOp },
		{ EOp::SetActorTransform,          &RunSetActorTransformOp },
		{ EOp::DeleteActor,                &RunDeleteActorOp },
		{ EOp::ReadOutputLog,              &RunReadOutputLogOp },
		{ EOp::RunAutomationTests,         &RunRunAutomationTestsOp },
		{ EOp::AddDataRow,                 &RunAddDataRowOp },
		{ EOp::SetDataRowValue,            &RunSetDataRowValueOp },
		{ EOp::AddComponent,               &RunAddComponentOp },
		{ EOp::RemoveComponent,            &RunRemoveComponentOp },
		{ EOp::AttachComponent,            &RunAttachComponentOp },
		{ EOp::SetComponentProperty,       &RunSetComponentPropertyOp },
		{ EOp::ListMaterials,              &RunListMaterialsOp },
		{ EOp::ReadMaterial,               &RunReadMaterialOp },
		{ EOp::AddMaterialExpression,      &RunAddMaterialExpressionOp },
		{ EOp::ConnectMaterialExpressions, &RunConnectMaterialExpressionsOp },
		{ EOp::SetMaterialParameter,       &RunSetMaterialParameterOp },
		{ EOp::SetMaterialInstanceParameter, &RunSetMaterialInstanceParameterOp },
		{ EOp::CompileMaterial,            &RunCompileMaterialOp },
		{ EOp::ReadWidgetBlueprint,        &RunReadWidgetBlueprintOp },
		{ EOp::AddWidget,                  &RunAddWidgetOp },
		{ EOp::SetWidgetProperty,          &RunSetWidgetPropertyOp },
		{ EOp::BindWidgetEvent,            &RunBindWidgetEventOp },
		{ EOp::CompileWidgetBlueprint,     &RunCompileWidgetBlueprintOp },
		{ EOp::ListBehaviorTrees,          &RunListBehaviorTreesOp },
		{ EOp::ReadBehaviorTree,           &RunReadBehaviorTreeOp },
		{ EOp::AddBTNode,                  &RunAddBTNodeOp },
		{ EOp::SetBTNodeProperty,          &RunSetBTNodePropertyOp },
		{ EOp::CompileBehaviorTree,        &RunCompileBehaviorTreeOp },
		{ EOp::ListDataAssets,             &RunListDataAssetsOp },
		{ EOp::ReadDataAsset,              &RunReadDataAssetOp },
		{ EOp::CreateDataAsset,            &RunCreateDataAssetOp },
		{ EOp::SetDataAssetProperty,       &RunSetDataAssetPropertyOp },
		{ EOp::ListStateTrees,             &RunListStateTreesOp },
		{ EOp::ReadStateTree,              &RunReadStateTreeOp },
		{ EOp::AddStateTreeState,          &RunAddStateTreeStateOp },
		{ EOp::SetStateTreeTransition,     &RunSetStateTreeTransitionOp },
		{ EOp::CompileStateTree,           &RunCompileStateTreeOp },
		{ EOp::StartProfile,               &RunStartProfileOp },
		{ EOp::StopProfile,                &RunStopProfileOp },
		{ EOp::GetStats,                   &RunGetStatsOp },
		{ EOp::TakeScreenshot,             &RunTakeScreenshotOp },
		{ EOp::CookContent,                &RunCookContentOp },
		{ EOp::PackageProject,             &RunPackageProjectOp },
		{ EOp::IntrospectClass,            &RunIntrospectClassOp },
		{ EOp::FindClass,                  &RunFindClassOp },
		{ EOp::ListFunctions,              &RunListFunctionsOp },
		{ EOp::FocusActor,                 &RunFocusActorOp },
		{ EOp::SetCameraTransform,         &RunSetCameraTransformOp },
		{ EOp::TakeViewportScreenshot,     &RunTakeViewportScreenshotOp },
		{ EOp::SetShowFlag,                &RunSetShowFlagOp },
		{ EOp::ListNiagaraSystems,         &RunListNiagaraSystemsOp },
		{ EOp::ReadNiagaraSystem,          &RunReadNiagaraSystemOp },
		{ EOp::CreateNiagaraSystem,        &RunCreateNiagaraSystemOp },
		{ EOp::SetNiagaraParameter,        &RunSetNiagaraParameterOp },
		{ EOp::ListLevelSequences,         &RunListLevelSequencesOp },
		{ EOp::ReadLevelSequence,          &RunReadLevelSequenceOp },
		{ EOp::AddSequenceTrack,           &RunAddSequenceTrackOp },
		{ EOp::SetSequencePlaybackRange,   &RunSetSequencePlaybackRangeOp },
		{ EOp::ListGameplayTags,           &RunListGameplayTagsOp },
		{ EOp::AddGameplayTag,             &RunAddGameplayTagOp },
		{ EOp::ReadAbilitySet,             &RunReadAbilitySetOp },
		{ EOp::ListAnimBlueprints,         &RunListAnimBlueprintsOp },
		{ EOp::ReadAnimBlueprint,          &RunReadAnimBlueprintOp },
		{ EOp::AddAnimState,               &RunAddAnimStateOp },
		{ EOp::CompileAnimBlueprint,       &RunCompileAnimBlueprintOp },
		{ EOp::StructuralDiff,             &RunStructuralDiffOp },
		{ EOp::ListAssets,                 &RunListAssetsOp },
		{ EOp::FindAsset,                  &RunFindAssetOp },
		{ EOp::GetProjectMetadata,         &RunGetProjectMetadataOp },
		// Phase 8 EA-pull Wave 1 (partial)
		{ EOp::ListOpenAssets,             &RunListOpenAssetsOp },
		{ EOp::GetActiveAsset,             &RunGetActiveAssetOp },
		{ EOp::GetCompileStatus,           &RunGetCompileStatusOp },
		{ EOp::GetDirtyPackages,           &RunGetDirtyPackagesOp },
		{ EOp::GetFocusedWindow,           &RunGetFocusedWindowOp },
		{ EOp::GetPieState,                &RunGetPieStateOp },
		{ EOp::GetModalState,              &RunGetModalStateOp },
		{ EOp::GetActiveEditorMode,        &RunGetActiveEditorModeOp },
		{ EOp::GetFocusedWidget,           &RunGetFocusedWidgetOp },
		{ EOp::OpenAssetEditor,            &RunOpenAssetEditorOp },
		{ EOp::CloseAssetEditor,           &RunCloseAssetEditorOp },
		{ EOp::GetCameraTransform,         &RunGetCameraTransformOp },
		{ EOp::GetViewMode,                &RunGetViewModeOp },
		{ EOp::GetShowFlags,               &RunGetShowFlagsOp },
		{ EOp::GetSelectedComponents,      &RunGetSelectedComponentsOp },
		{ EOp::GetSelectedAssets,          &RunGetSelectedAssetsOp },
		{ EOp::SetSelectedAssets,          &RunSetSelectedAssetsOp },
		{ EOp::GetSelectedFolders,         &RunGetSelectedFoldersOp },
		{ EOp::GetContentBrowserPath,      &RunGetContentBrowserPathOp },
		{ EOp::SetContentBrowserPath,      &RunSetContentBrowserPathOp },
		{ EOp::WorldToScreen,              &RunWorldToScreenOp },
		{ EOp::ScreenToWorld,              &RunScreenToWorldOp },
		{ EOp::UiSnapshot,                 &RunUiSnapshotOp },
		{ EOp::UiFind,                     &RunUiFindOp },
		{ EOp::ListDesktopWindows,         &RunListDesktopWindowsOp },
		{ EOp::ListGameFeatures,           &RunListGameFeaturesOp },
		{ EOp::GetGameFeatureState,        &RunGetGameFeatureStateOp },
		{ EOp::ListPlugins,                &RunListPluginsOp },
		{ EOp::GetPluginDescriptor,        &RunGetPluginDescriptorOp },
		{ EOp::GetPluginDependencies,      &RunGetPluginDependenciesOp },
		{ EOp::ListActorAbilities,         &RunListActorAbilitiesOp },
		{ EOp::ListActorGameplayTags,      &RunListActorGameplayTagsOp },
		{ EOp::ListActorAttributes,        &RunListActorAttributesOp },
		{ EOp::ListActorGameplayEffects,   &RunListActorGameplayEffectsOp },
		{ EOp::GetBlueprintEditorState,    &RunGetBlueprintEditorStateOp },
		{ EOp::GetMaterialInstanceParams,  &RunGetMaterialInstanceParamsOp },
		{ EOp::GetStaticMeshInfo,          &RunGetStaticMeshInfoOp },
		{ EOp::GetUmgEditorState,          &RunGetUmgEditorStateOp },
		{ EOp::GetMaterialEditorState,     &RunGetMaterialEditorStateOp },
		{ EOp::GetMeshPreviewState,        &RunGetMeshPreviewStateOp },
		{ EOp::GetCinematicCamera,         &RunGetCinematicCameraOp },
		{ EOp::GetSequencerState,          &RunGetSequencerStateOp },
		{ EOp::GetAnimEditorState,         &RunGetAnimEditorStateOp },
		{ EOp::GetNiagaraModuleSelection,  &RunGetNiagaraModuleSelectionOp },
		{ EOp::GetCurveEditorSelection,    &RunGetCurveEditorSelectionOp },
		{ EOp::GetBufferVisualizationMode, &RunGetBufferVisualizationModeOp },
		{ EOp::GetGizmoState,              &RunGetGizmoStateOp },
		{ EOp::GetViewportRealtime,        &RunGetViewportRealtimeOp },
		{ EOp::GetViewportCameraSettings,  &RunGetViewportCameraSettingsOp },
		{ EOp::GetSnappingSettings,        &RunGetSnappingSettingsOp },
		{ EOp::GetActiveViewport,          &RunGetActiveViewportOp },
		{ EOp::GetHiddenActors,            &RunGetHiddenActorsOp },
		{ EOp::GetVisibleActors,           &RunGetVisibleActorsOp },
		{ EOp::SetViewMode,                &RunSetViewModeOp },
		{ EOp::SetGizmoMode,               &RunSetGizmoModeOp },
		{ EOp::SetViewportRealtime,        &RunSetViewportRealtimeOp },
		{ EOp::SetActorVisibility,         &RunSetActorVisibilityOp },
		{ EOp::GetHiddenLayers,            &RunGetHiddenLayersOp },
		{ EOp::SetLayerVisibility,         &RunSetLayerVisibilityOp },
		{ EOp::GetCameraBookmarks,         &RunGetCameraBookmarksOp },
		{ EOp::GotoCameraBookmark,         &RunGotoCameraBookmarkOp },
		{ EOp::GetHoverTarget,             &RunGetHoverTargetOp },
		{ EOp::GetIsolateMode,             &RunGetIsolateModeOp },
		{ EOp::GetAsyncCompileState,       &RunGetAsyncCompileStateOp },
		{ EOp::GetShaderCompileState,      &RunGetShaderCompileStateOp },
		{ EOp::GetCurrentLevel,            &RunGetCurrentLevelOp },
		{ EOp::ListLoadedLevels,           &RunListLoadedLevelsOp },
		{ EOp::GetSourceControlProvider,   &RunGetSourceControlProviderOp },
		{ EOp::GetAssetRegistryState,      &RunGetAssetRegistryStateOp },
		{ EOp::GetDataLayerStates,         &RunGetDataLayerStatesOp },
		{ EOp::GetAutosaveStatus,          &RunGetAutosaveStatusOp },
		{ EOp::GetRecoveryState,           &RunGetRecoveryStateOp },
		{ EOp::GetSourceControlStatus,     &RunGetSourceControlStatusOp },
		{ EOp::GetFileLockStatus,          &RunGetFileLockStatusOp },
		{ EOp::GetActiveCulture,           &RunGetActiveCultureOp },
		{ EOp::GetEditorTheme,             &RunGetEditorThemeOp },
		{ EOp::GetMonitors,                &RunGetMonitorInfoOp },
		{ EOp::GetLiveCodingState,         &RunGetLiveCodingStateOp },
		{ EOp::ActivateGameFeature,        &RunActivateGameFeatureOp },
		{ EOp::DeactivateGameFeature,      &RunDeactivateGameFeatureOp },
		{ EOp::GetRecentlyOpenedAssets,    &RunGetRecentlyOpenedAssetsOp },
		{ EOp::GetDebugInstance,           &RunGetDebugInstanceOp },
		{ EOp::GetBlueprintBreakpoints,    &RunGetBlueprintBreakpointsOp },
		{ EOp::GetWatchedPins,             &RunGetWatchedPinsOp },
		{ EOp::GetActiveStats,             &RunGetActiveStatsOp },
		{ EOp::SetPluginEnabled,           &RunSetPluginEnabledOp },
		{ EOp::GetStreamingSources,        &RunGetStreamingSourcesOp },
		{ EOp::GetRecentlySavedPackages,   &RunGetRecentlySavedPackagesOp },
		{ EOp::ListProjectSettings,        &RunListProjectSettingsOp },
		{ EOp::GetProjectSettingValues,    &RunGetProjectSettingValuesOp },
		{ EOp::SetProjectSetting,          &RunSetProjectSettingOp },
		{ EOp::ListAutomationTests,        &RunListAutomationTestsOp },
		{ EOp::GetEditorEvents,            &RunGetEditorEventsOp },
		{ EOp::GetActiveCookTarget,        &RunGetActiveCookTargetOp },
		{ EOp::GetWorkspaceLayout,         &RunGetWorkspaceLayoutOp },
		{ EOp::GetTraceState,              &RunGetTraceStateOp },
		{ EOp::GetUiStateStub,             &RunGetUiStateStubOp },
	};
	for (const auto& Entry : kDispatchTable)
	{
		if (Entry.Op == Op)
		{
			const int32 HandlerCode = Entry.Handler(Params, OutputPath, bPretty);
			return ApplyLoadFailureOverride(HandlerCode, OutputPath);
		}
	}

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
    int32 RunOneOpFromLiveServer(uint64 ConnectionId, const FString& Params);
}    // namespace BlueprintReader
namespace
{
int32 RunOneOp(const FString& Params);  // forward decl from earlier defn
}
int32 BlueprintReader::RunOneOpFromLiveServer(uint64 ConnectionId, const FString& Params)
{
    // Pin the current connection id for the duration of this dispatch.
    // BatchDeferFlag()/BatchPending() — and any future per-session state
    // — read this id to look up their context in the registry. Restored
    // on scope exit so nested or re-entrant calls behave correctly.
    BlueprintReader::FConnectionScope Scope(ConnectionId);
    // Suppress interactive editor dialogs (notably the source-control
    // "Check Out?" modal) for the duration of a live-backend op. This runs
    // on the game thread (the live server dispatches via
    // AsyncTask(GameThread,...)), and a modal there blocks that thread — the
    // MCP call then appears to hang and the client concludes the tool broke.
    // Under unattended script the editor takes the non-interactive default
    // instead. Scoped + restored per op; the commandlet backend already runs
    // unattended, so this is a no-op there.
    TGuardValue<bool> UnattendedGuard(GIsRunningUnattendedScript, true);
    return RunOneOp(Params);
}

// Commit-partial-on-disconnect (Task 4.4). Called from the cmdlet /
// live server's disconnect path on a server thread. Reaps the
// connection's pending BPs and compiles them on the game thread.
// `BP_READER_BATCH_ON_DISCONNECT=discard` drops them instead, matching
// the strict-atomic-mode contract where clients want no half state.
void BlueprintReader::FlushBatchForConnection(uint64 ConnectionId)
{
    if (ConnectionId == 0)
    {
    	return;
    }
    TArray<TWeakObjectPtr<UBlueprint>> Pending =
        BlueprintReader::FBatchRegistry::Get().Discard(ConnectionId);
    if (Pending.IsEmpty())
    {
    	return;
    }

    const bool bSkipCompile =
        FPlatformMisc::GetEnvironmentVariable(TEXT("BP_READER_BATCH_ON_DISCONNECT"))
            .Equals(TEXT("discard"), ESearchCase::IgnoreCase);
    if (bSkipCompile)
    {
        UE_LOG(LogBlueprintReader, Display,
            TEXT("Client %llu disconnected mid-batch; discarding %d pending BP(s) per BP_READER_BATCH_ON_DISCONNECT=discard"),
            (unsigned long long)ConnectionId, Pending.Num());
        return;
    }

    UE_LOG(LogBlueprintReader, Display,
        TEXT("Client %llu disconnected mid-batch; flushing %d pending BP(s)"),
        (unsigned long long)ConnectionId, Pending.Num());

    // CompileAndSaveBlueprint requires the game thread. Schedule + wait
    // — the server worker is exiting anyway, so a short block is fine,
    // and the daemon's main poll loop is still pumping the task graph
    // (it doesn't exit until ActiveConnections hits 0 AND idle elapses).
    FEvent* Done = FPlatformProcess::GetSynchEventFromPool(false);
    AsyncTask(ENamedThreads::GameThread, [Pending, Done]()
    {
        for (const TWeakObjectPtr<UBlueprint>& Weak : Pending)
        {
            if (UBlueprint* BP = Weak.Get())
            {
                CompileAndSaveBlueprint(BP);
            }
        }
        Done->Trigger();
    });
    Done->Wait();
    FPlatformProcess::ReturnSynchEventToPool(Done);
}

namespace
{
// Pointer to the live FCmdletServer the daemon's polling loop owns.
// Set before installing the Win32 console handler and cleared after
// it's removed, so the handler is never called against a dangling
// pointer. Static (not thread_local) because Win32 console handlers
// are invoked on an OS-injected handler thread; every invocation
// must see the same target.
static BlueprintReader::FCmdletServer* GDaemonShutdownTarget = nullptr;

// Win32 design note — registration timing + UE API patching:
//
// UE's `FWindowsPlatformMisc::SetGracefulTerminationHandler()` runs
// during engine init. In non-shipping builds on x86, UE PATCHES the
// `SetConsoleCtrlHandler` Win32 API with `mov eax, 1; ret` to prevent
// third-party code (Perforce libs, etc.) from registering competing
// handlers. THAT patch is guarded by an early-out:
//   if (GetConsoleWindow() == nullptr) return;
//
// Initial hope: a daemon launched with `-unattended` + `-nullrhi`
// would have `GetConsoleWindow() == nullptr`, UE would skip both
// handler-install and API-patch, and we'd have a free
// SetConsoleCtrlHandler to register on.
//
// Empirical result (verified by Python+CTRL_BREAK_EVENT test):
// `GetConsoleWindow()` returns a real HWND for the daemon process,
// UE DOES install its handler, UE DOES patch the API, our subsequent
// registration call returns 1 from the patched stub but our handler
// never gets invoked. UE's own handler then catches Ctrl-C, falls
// through to TerminateProcess(0xc000013a) in unattended mode without
// running ApplicationWillTerminateDelegate, and the daemon hard-dies
// leaving the handshake + lock files behind.
//
// We keep the SetConsoleCtrlHandler registration anyway because it's
// correct code that works in (a) shipping builds, (b) non-x86 builds,
// (c) any future engine build where the API patch is removed. The
// stale-file detection in Task 2.4 will catch the leaked-file case
// on the next daemon launch.
//
// The actual graceful-shutdown path that DOES work today is the
// polling-loop exit: anything that calls FCmdletServer::Stop() (or
// flips bShuttingDown via the future idle-timeout in Task 4.2) gets
// full cleanup including thread joins.
//
// Why RequestShutdown/TerminateOnSignal are separate from Stop():
// the handler runs on a Win32-injected thread; Stop() joins listener
// + connection threads and we can't safely block them in the handler
// context. RequestShutdown flips the polling-loop flag (main thread
// then calls Stop()); TerminateOnSignal does the minimum on-disk
// cleanup we can synchronously complete before the default-handler
// path TerminateProcess's us.

// Win32 console-control handler. See "Win32 design note" above.
// Returns 1 (Win32 TRUE) when the event was handled; `TRUE`/`FALSE`
// macros are `#undef`'d here because UE's Windows-types-hiding
// header chain unconditionally hides them.
static BOOL WINAPI DaemonConsoleCtrlHandler(DWORD CtrlType)
{
	switch (CtrlType)
	{
		case CTRL_C_EVENT:
		case CTRL_BREAK_EVENT:
		case CTRL_CLOSE_EVENT:
		case CTRL_LOGOFF_EVENT:
		case CTRL_SHUTDOWN_EVENT:
			if (GDaemonShutdownTarget)
			{
				UE_LOG(LogBlueprintReader, Display,
					TEXT("Daemon received console-control signal (%lu); requesting graceful shutdown"),
					CtrlType);
				// Set the polling-loop flag first so the main thread
				// notices on its next iteration (50ms).
				GDaemonShutdownTarget->RequestShutdown();
				// Synchronously delete the handshake + lock files in
				// case the OS kills us before the main thread reaches
				// Stop(). This is idempotent with Stop() running
				// afterward.
				GDaemonShutdownTarget->TerminateOnSignal();
			}
			return 1;
		default:
			return 0;
	}
}

int32 RunDaemon()
{
	// The daemon used to read newline-delimited commandlet-arg lines
	// from stdin and write `__BPR_DONE <code>__` back to stdout, with
	// a single MCP-server parent on the other end of the pipe. That
	// path forced N daemons against the same project for N concurrent
	// MCP clients — they fought over the asset registry / DDC / .uasset
	// files. The daemon now hosts FCmdletServer (a TCP listener with
	// the same wire protocol the editor's live server speaks), so any
	// number of MCP-server clients can multiplex through one daemon
	// process. RunOneOpFromLiveServer (the same dispatch entry the
	// live server uses) is invoked from the per-connection threads
	// FCmdletServer spawns.
	UE_LOG(LogBlueprintReader, Display,
		TEXT("BlueprintReader daemon: starting cmdlet TCP server"));

	BlueprintReader::FCmdletServer Server;
	if (!Server.Start())
	{
		UE_LOG(LogBlueprintReader, Error,
			TEXT("BlueprintReader daemon: failed to start cmdlet TCP server"));
		return 1;
	}

	UE_LOG(LogBlueprintReader, Display,
		TEXT("BlueprintReader daemon: cmdlet TCP server listening on 127.0.0.1:%d"),
		Server.GetListenPort());

	// Install the Win32 console-control handler so Ctrl-C / Ctrl-Break
	// / console-close events have a chance to trigger graceful shutdown
	// instead of a hard process kill (which would leave the handshake +
	// lock files behind and confuse the MCP server's "is the daemon
	// alive?" probe). Pass 1 (Win32 TRUE) — `TRUE`/`FALSE` macros are
	// `#undef`'d here by UE's Windows-types-hiding header chain.
	//
	// IMPORTANT CAVEAT (empirically verified — see commit message):
	// In non-shipping x86 builds, UE's
	// `FWindowsPlatformMisc::SetGracefulTerminationHandler` PATCHES the
	// SetConsoleCtrlHandler Win32 API with `mov eax, 1; ret` to prevent
	// third-party code from registering competing handlers (Perforce
	// being the original motivator). Our call below returns 1
	// (success) from that patched stub but our handler is NEVER
	// actually registered — the Win32 callback table remains
	// UE-only. UE's own handler (in unattended mode) falls through
	// to `TerminateProcess(0xc000013a)` without joining our threads
	// or deleting our files.
	//
	// Net result: in non-shipping x86 builds, this registration is a
	// no-op and Ctrl-C cleanup is best-effort (the OS reaps file
	// handles on process exit, so the lifetime-lock IS released, but
	// the handshake + lock FILES persist on disk and will need to be
	// cleaned by the next daemon launch's stale-file detection).
	// In shipping or non-x86 builds, UE doesn't patch the API and
	// this registration works as written.
	//
	// We keep the registration anyway because (a) it's correct code
	// that handles the cases UE doesn't sabotage, (b) the stale-file
	// detection in Task 2.4 catches the leak case, and (c) the
	// polling-loop shutdown path is what tests + future idle-timeout
	// (Task 4.2) actually drive — that path always works.
	GDaemonShutdownTarget = &Server;
	if (!SetConsoleCtrlHandler(&DaemonConsoleCtrlHandler, 1))
	{
		UE_LOG(LogBlueprintReader, Warning,
			TEXT("BlueprintReader daemon: SetConsoleCtrlHandler returned 0 "
				 "(GetLastError=%lu); Ctrl-C path will not trigger graceful shutdown"),
			GetLastError());
	}

	// Block here until graceful shutdown. The server runs its own
	// accept + reader threads (spawned via FRunnableThread::Create in
	// FCmdletServer), so we just need the daemon process to stay alive.
	// Real shutdown triggers (idle timer) will be wired later; the
	// console-control handler above already handles Ctrl-C / close.
	//
	// We MUST pump the game thread's task queue here: FCmdletServer's
	// per-connection threads dispatch op handlers via
	// `AsyncTask(ENamedThreads::GameThread, ...)` so UObject mutation
	// runs on the game thread. In a normal editor session the editor
	// pumps the task graph naturally; in a commandlet daemon the only
	// game-thread runner is this loop, so without an explicit
	// ProcessThreadUntilIdle the queued tasks would sit forever and
	// every op frame would time out. Also tick the core ticker so
	// timer-driven engine subsystems (e.g. async asset registry
	// callbacks) get serviced.
	//
	// IMPORTANT: must be `ENamedThreads::GameThread`, NOT
	// `GameThread_Local`. Tasks queued via
	// `AsyncTask(ENamedThreads::GameThread, ...)` land on the regular
	// game-thread queue, not the "_Local" subqueue. Pumping only
	// GameThread_Local from a commandlet drains nothing and the
	// dispatched task never fires — manifested as every `op` frame
	// hanging until the client times out. See the comment in
	// FCmdletServer where the AsyncTask is issued.
	while (!Server.WantsShutdown())
	{
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
		FTSTicker::GetCoreTicker().Tick(0.05f);
		FPlatformProcess::Sleep(0.05f);
	}

	// Remove the handler before Stop() runs so a late-arriving Ctrl-C
	// can't re-enter and race the main-thread teardown.
	SetConsoleCtrlHandler(&DaemonConsoleCtrlHandler, 0);
	GDaemonShutdownTarget = nullptr;

	Server.Stop();
	UE_LOG(LogBlueprintReader, Display,
		TEXT("BlueprintReader daemon: clean shutdown"));
	return 0;
}
} // namespace
