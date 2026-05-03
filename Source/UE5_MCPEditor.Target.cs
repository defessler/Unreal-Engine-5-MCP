using UnrealBuildTool;
using System.Collections.Generic;

public class UE5_MCPEditorTarget : TargetRules
{
	public UE5_MCPEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.V6;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		BuildEnvironment = TargetBuildEnvironment.Shared;
		ExtraModuleNames.Add("UE5_MCP");
	}
}
