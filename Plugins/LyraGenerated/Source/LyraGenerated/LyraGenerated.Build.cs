using UnrealBuildTool;

public class LyraGenerated : ModuleRules
{
	public LyraGenerated(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"GameplayTags",
			"GameplayAbilities",
			"EnhancedInput",
			"UMG",
			"Slate",
			"SlateCore",
			"Niagara",       // NiagaraComponent
		});
	}
}
