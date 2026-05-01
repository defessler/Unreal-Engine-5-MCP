using UnrealBuildTool;
using System.Collections.Generic;

public class UE5_AI_BPEditorTarget : TargetRules
{
	public UE5_AI_BPEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.V6;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		BuildEnvironment = TargetBuildEnvironment.Shared;
		ExtraModuleNames.Add("UE5_AI_BP");
	}
}
