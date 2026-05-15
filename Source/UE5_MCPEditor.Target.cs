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

		// MCP server is auto-built by the BlueprintReader plugin's own
		// `PreBuildSteps` block (in BlueprintReader.uplugin) — nothing
		// to wire from the project side. Drop the plugin into any
		// project and the editor build pulls the MCP server with it.
	}
}
