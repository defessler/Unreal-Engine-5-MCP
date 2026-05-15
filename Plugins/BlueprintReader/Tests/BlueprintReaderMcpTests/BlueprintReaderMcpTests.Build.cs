// Copyright contributors to the UE5_MCP project.
//
// Module rules for the bp-reader doctest suite. The test files
// directly #include doctest headers and exercise the core library.

using UnrealBuildTool;
using System.IO;

public class BlueprintReaderMcpTests : ModuleRules
{
    public BlueprintReaderMcpTests(ReadOnlyTargetRules Target) : base(Target)
    {
        bAddDefaultIncludePaths = false;
        PCHUsage = PCHUsageMode.NoPCHs;
        PublicIncludePathModuleNames.Add("Launch");
        // dynamic_cast through the IBlueprintReader hierarchy — see
        // BlueprintReaderMcpCore.Build.cs for the same reason.
        bUseRTTI = true;

        // doctest header — only this target needs it (production code
        // doesn't pull in doctest), so kept here rather than in McpCore.
        string ThirdParty = Path.Combine(ModuleDirectory, "..", "ThirdParty");
        PrivateIncludePaths.Add(Path.Combine(ThirdParty, "doctest"));

        PrivateDependencyModuleNames.Add("BlueprintReaderMcpCore");
    }
}
