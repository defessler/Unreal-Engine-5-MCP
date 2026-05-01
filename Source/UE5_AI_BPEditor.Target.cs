using UnrealBuildTool;
using System.Collections.Generic;

public class UE5_AI_BPEditorTarget : TargetRules
{
	public UE5_AI_BPEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.V5;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("UE5_AI_BP");
	}
}
