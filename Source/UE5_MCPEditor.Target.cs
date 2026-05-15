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

		// Auto-build the MCP server alongside the editor. The MCP server
		// is its own UBT Program target under Plugins/BlueprintReader/
		// Tests/BlueprintReaderMcp/ — without this dependency, an editor
		// build would leave Binaries/Win64/BlueprintReaderMcp.exe stale.
		//
		// Win64-only: BlueprintReaderMcp.Target.cs declares
		// [SupportedPlatforms("Win64")]; UBT errors if we declare it as
		// a PreBuildTarget on a non-Win64 build.
		//
		// Skip via `Build.bat -SkipPreBuildTargets` if you only want
		// the editor module rebuilt and not the server. The standalone
		// `Build.bat BlueprintReaderMcp Win64 Development -project=...`
		// path still works for explicit server builds.
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PreBuildTargets.Add(new TargetInfo(
				"BlueprintReaderMcp",
				Target.Platform,
				Target.Configuration,
				Target.Architectures,
				Target.ProjectFile,
				Target.Arguments));
		}
	}
}
