using UnrealBuildTool;
using System.Collections.Generic;

public class UE5_MCPTarget : TargetRules
{
	public UE5_MCPTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.V6;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("UE5_MCP");
	}
}
