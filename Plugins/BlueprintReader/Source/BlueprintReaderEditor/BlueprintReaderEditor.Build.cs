using System.IO;
using UnrealBuildTool;

public class BlueprintReaderEditor : ModuleRules
{
	public BlueprintReaderEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		// Tie "is the editor module up to date?" to "have any MCP server
		// source files changed?" via ExternalDependencies. The .uplugin's
		// PreBuildSteps hook only runs when UBT decides the editor target
		// has work to do; if a user only edits MCP server source (under
		// Plugins/BlueprintReader/Tests/), the editor target would
		// otherwise stay clean and the hook never fires. Adding those
		// files as ExternalDependencies makes UBT invalidate this module's
		// makefile when MCP source changes, which triggers the editor
		// rebuild path, which fires the hook, which rebuilds the MCP
		// server. End result: editing MCP source then building the editor
		// always picks up the new server, no extra invocation needed.
		//
		// Scope: source + headers + Build.cs/Target.cs under both Program
		// targets. Skip ThirdParty/ — vendored deps change rarely and a
		// bulk file enumeration there isn't worth the discovery cost.
		string PluginDir = Path.GetFullPath(Path.Combine(ModuleDirectory, "..", ".."));
		string TestsDir = Path.Combine(PluginDir, "Tests");
		if (Directory.Exists(TestsDir))
		{
			foreach (string SubdirName in new[] { "BlueprintReaderMcp", "BlueprintReaderMcpCore", "BlueprintReaderMcpTests" })
			{
				string SubdirFull = Path.Combine(TestsDir, SubdirName);
				if (!Directory.Exists(SubdirFull)) { continue; }
				foreach (string Pattern in new[] { "*.cpp", "*.h", "*.hpp", "*.cs" })
				{
					foreach (string FilePath in Directory.EnumerateFiles(SubdirFull, Pattern, SearchOption.AllDirectories))
					{
						ExternalDependencies.Add(FilePath);
					}
				}
			}
		}

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
			"MovieScene"
		});
	}
}
