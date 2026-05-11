using UnrealBuildTool;

public class BlueprintReaderEditor : ModuleRules
{
	public BlueprintReaderEditor(ReadOnlyTargetRules Target) : base(Target)
	{
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
			"UMGEditor"
		});
	}
}
