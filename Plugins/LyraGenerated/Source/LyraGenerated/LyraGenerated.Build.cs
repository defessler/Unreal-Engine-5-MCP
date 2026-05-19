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
			"Niagara",
			"AIModule",
			"NavigationSystem",
			"PhysicsCore",
			// LyraGame + plugins for stub parents that target project classes.
			"LyraGame",
			"CommonUI",
			"CommonInput",
			"CommonGame",
			"ModularGameplay",
			"ModularGameplayActors",
			"GameSettings",
			"GameFeatures",
			"GameplayMessageRuntime",
			"ShooterCoreRuntime",
			"TopDownArenaRuntime",
			"ShooterTestsRuntime",
		});
	}
}
