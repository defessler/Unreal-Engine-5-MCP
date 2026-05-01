using UnrealBuildTool;
using System.Collections.Generic;

public class UE5_AI_BPTarget : TargetRules
{
	public UE5_AI_BPTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.V6;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("UE5_AI_BP");
	}
}
