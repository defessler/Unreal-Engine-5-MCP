// BPRoundtripModule — UBT-compiled landing zone for BP→C++ code that
// the BPIR-roundtrip pipeline emits at test time. The module ships
// empty (just the IMPLEMENT_MODULE entry point); each test run drops
// generated .h/.cpp pairs into Private/Generated/, then invokes UBT to
// rebuild this module so we know the emitted source survives a real
// compile. Generated/ is .gitignored — only the module skeleton is
// versioned.
//
// Dependencies match what the codegen layer (CppClassEmit) actually
// emits today: Core / CoreUObject / Engine for UCLASS / UPROPERTY /
// UFUNCTION + the common Actor / Pawn / Character bases, and
// GameplayTags for FGameplayTag property types (a frequent BP variable
// in roundtrip fixtures). Adding new emitted node kinds that pull in
// other modules (UMG, AIModule, etc.) requires extending this list.

using UnrealBuildTool;

public class BPRoundtripModule : ModuleRules
{
	public BPRoundtripModule(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"GameplayTags",
			"EnhancedInput",     // FInputActionValue / UInputAction (TPC EIA decompile)
		});
	}
}
