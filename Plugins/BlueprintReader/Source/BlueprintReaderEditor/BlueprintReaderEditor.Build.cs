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
			"AssetRegistry"
		});
	}
}
