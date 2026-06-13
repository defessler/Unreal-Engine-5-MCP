using UnrealBuildTool;

public class BlueprintReaderEditor : ModuleRules
{
	public BlueprintReaderEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		// NOTE: this module used to register every MCP server source file as an
		// ExternalDependency so editing server source would invalidate the editor
		// makefile and let the .uplugin PreBuildSteps hook rebuild the server. That
		// hook was removed (the MCP server is engine-independent and ships
		// precompiled / is built on demand), so the coupling was deleted — it only
		// forced spurious full editor-module recompiles when iterating on server code.

		// Required Target.cs settings for the consuming project. Without
		// these, you get cryptic linker errors (missing UnrealEd internals,
		// PrivateIncludePath resolution failures from the engine patches we
		// rely on). Fail fast here with an actionable message instead.
		// BlueprintReaderEditor historically required Shared build
		// environment due to PrivateIncludePath issues with engine patches.
		// UE5.7 supports Unique env too (e.g. Lyra needs Unique to override
		// warning settings). The check is retained as a soft warning.
		if (Target.Type != TargetType.Program &&
		    Target.BuildEnvironment != TargetBuildEnvironment.Shared &&
		    Target.BuildEnvironment != TargetBuildEnvironment.Unique)
		{
			throw new BuildException(
				"BlueprintReaderEditor requires `BuildEnvironment = " +
				"TargetBuildEnvironment.Shared` or `Unique` in the " +
				"consuming project's <Project>Editor.Target.cs. " +
				"Current value: " + Target.BuildEnvironment + ".");
		}

		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Enable RTTI so per-asset-editor selection tools can cross-cast an
		// IAssetEditorInstance* to the public toolkit interfaces (e.g.
		// IHasPersonaToolkit / IAnimationBlueprintEditor) for deep editor
		// state. Roadmap 3.1: open-detection (FindEditorForAsset) lands now;
		// the skeleton-selection read via the cross-cast is the live-dev
		// follow-up this flag unblocks. Pattern matches BlueprintReaderMcpCore.
		bUseRTTI = true;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"UnrealEd",
			"BlueprintGraph",
			"Json",
			"JsonUtilities",
			"AssetRegistry",
			// IAssetTools::DuplicateAsset (BP-5: duplicate_blueprint).
			"AssetTools",
			// Live-mode TCP listener (BlueprintReaderLiveServer.cpp).
			"Sockets",
			"Networking",
			// Material authoring (Stage 1) — UMaterialEditingLibrary lives
			// in MaterialEditor module; the parameter expression types and
			// FLinearColor are in Engine, but we still need the editor
			// helpers (CreateMaterialExpression / ConnectMaterialExpressions
			// / RecompileMaterial / SetMaterialInstance* setters).
			"MaterialEditor",
			// UMG widget authoring (Stage 1) — UWidgetBlueprint lives in
			// UMGEditor, runtime UWidget/UPanelWidget in UMG.
			"UMG",
			"UMGEditor",
			// Behavior Tree authoring (Stage 2) — runtime UBehaviorTree +
			// UBTNode classes. We don't link BehaviorTreeEditor; full
			// authoring still needs that, but the runtime asset can be
			// inspected + scaffolded from here.
			"AIModule",
			// Python executor used by run_python_script (BP_READER_ALLOW_PYTHON
			// gated). Mirrors Epic AIAssistant's
			// Engine/Plugins/Experimental/AIAssistant which depends on the
			// same plugin for its ExecPythonCommandEx path.
			"PythonScriptPlugin",
			// UEditorAssetLibrary (Duplicate/Save/DoesAssetExist) — used by
			// the BPRoundtripSeed commandlet to import the engine's
			// ThirdPerson template into /Game/Imported/ThirdPerson/.
			"EditorScriptingUtilities",
			// UDeveloperSettings for the Project Settings → BlueprintReader
			// MCP page (port, autostart, allow/blocklist patterns, tool
			// search toggle). Persists to EditorPerProjectUserSettings.ini.
			"DeveloperSettings",
			// Phase 8 — get_focused_window queries Slate's active top-level
			// window via FSlateApplication + SWindow::GetTypeAsString.
			"Slate",
			"SlateCore",
			// Phase 8 — content browser selection tools
			// (get/set_selected_assets, get_selected_folders, etc.) hit
			// IContentBrowserSingleton via FContentBrowserModule.
			"ContentBrowser",
			// Phase 11 H Tier 1 — GameFeaturesToolset reads via
			// UGameFeaturesSubsystem. Loads via the GameFeatures plugin
			// when present (always present in Lyra). Optional at runtime;
			// the toolset surfaces empty state when the subsystem is null.
			"GameFeatures",
			// IPluginManager — used to enumerate GFPs by scanning the
			// enabled plugin list (subsystem doesn't expose a public URL
			// dump in 5.7).
			"Projects",
			// Phase 11 H Tier 1 — AbilitySystemInspectorToolset.
			// UAbilitySystemComponent + UGameplayAbility live in the
			// GameplayAbilities plugin. Lyra always has it; non-Lyra
			// projects with the plugin disabled will load the editor
			// fine but the GAS tools return empties / "not available".
			"GameplayAbilities",
			"GameplayTags",
			// Phase 12 Wave 2 — FBlueprintEditor for get_blueprint_editor_state.
			// `Kismet` is the module name (despite the legacy class
			// naming); IBlueprintEditor lives here.
			"Kismet",
			// Phase 12 Wave 2 — IStaticMeshEditor for get_mesh_preview_state.
			"StaticMeshEditor",
			// Phase 12 Wave 2 — get_sequencer_state. ILevelSequenceEditorToolkit
			// lives in the LevelSequenceEditor plugin module; ISequencer in
			// the Sequencer editor module; ULevelSequence runtime asset in
			// the LevelSequence plugin module.
			"LevelSequence",
			"LevelSequenceEditor",
			"Sequencer",
			// MovieScene for EMovieScenePlayerStatus + FQualifiedFrameTime
			// helpers (asSeconds resolves frame rate).
			"MovieScene",
			// Phase 13 Wave 3 note: ULayersSubsystem (get_hidden_layers /
			// set_layer_visibility) lives in UnrealEd (Layers/LayersSubsystem.h),
			// already a dependency — no extra module needed.
			// Phase 14 — get_source_control_provider reads ISourceControlModule.
			"SourceControl",
			// Phase 17 — get_monitor_info links FDisplayMetrics::RebuildDisplayMetrics.
			"ApplicationCore",
			// EDIT-1: AnimBlueprint state machine introspection.
			// UAnimStateMachineGraph / UAnimStateNode / UAnimStateTransitionNode
			// live in AnimGraph (editor-only). Required to walk state machine
			// graphs in read_anim_blueprint / add_anim_state.
			"AnimGraph",
			// TEST-2 P1b: ui_invoke_menu drives editor menu commands through
			// UToolMenus (GenerateMenu + ConvertUIAction). The level editor's
			// command list (FToolMenuContext source) comes from FLevelEditorModule.
			"ToolMenus",
			"LevelEditor"
		});

		// Phase 14 — get_active_cook_target queries ITargetPlatformManagerModule
		// via FModuleManager. All calls are header-declared pure virtuals
		// (GetActiveTargetPlatforms / GetRunningTargetPlatform / PlatformName),
		// resolved through the vtable — include-only, no link dependency.
		PrivateIncludePathModuleNames.Add("TargetPlatform");

		// Phase 17 — get_live_coding_state queries ILiveCodingModule via
		// FModuleManager (include-only, no link). Windows-only module.
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PrivateIncludePathModuleNames.Add("LiveCoding");
		}
	}
}
