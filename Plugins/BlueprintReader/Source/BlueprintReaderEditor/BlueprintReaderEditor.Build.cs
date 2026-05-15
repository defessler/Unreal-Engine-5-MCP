using UnrealBuildTool;

public class BlueprintReaderEditor : ModuleRules
{
	public BlueprintReaderEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		// Required Target.cs settings for the consuming project. Without
		// these, you get cryptic linker errors (missing UnrealEd internals,
		// PrivateIncludePath resolution failures from the engine patches we
		// rely on). Fail fast here with an actionable message instead.
		if (Target.Type != TargetType.Program &&
		    Target.BuildEnvironment != TargetBuildEnvironment.Shared)
		{
			throw new BuildException(
				"BlueprintReaderEditor requires `BuildEnvironment = " +
				"TargetBuildEnvironment.Shared;` in the consuming project's " +
				"<Project>Editor.Target.cs. Current value: " +
				Target.BuildEnvironment + ". " +
				"Also ensure `DefaultBuildSettings = BuildSettingsVersion.V6;` " +
				"is set. See the project's README under 'Build invariants' " +
				"for details.");
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
			"PythonScriptPlugin"
		});
	}
}
