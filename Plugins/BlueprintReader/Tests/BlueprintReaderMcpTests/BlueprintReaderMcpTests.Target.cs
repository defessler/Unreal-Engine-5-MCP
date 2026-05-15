// Copyright contributors to the UE5_MCP project.
//
// UE Program target for the bp-reader MCP server's doctest suite.
// Twin of BlueprintReaderMcp.Target.cs — same flags, different
// LaunchModule. Provides `bp-reader-tests.exe`.

using UnrealBuildTool;
using System.Collections.Generic;

[SupportedPlatforms("Win64")]
public class BlueprintReaderMcpTestsTarget : TargetRules
{
    public BlueprintReaderMcpTestsTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Program;
        LinkType = TargetLinkType.Monolithic;
        LaunchModuleName = "BlueprintReaderMcpTests";

        // V6 defaults -- same rationale as BlueprintReaderMcp.Target.cs.
        DefaultBuildSettings = BuildSettingsVersion.V6;

        IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
        CppStandard = CppStandardVersion.Cpp20;

        bCompileAgainstEngine = false;
        bCompileAgainstCoreUObject = false;
        bCompileAgainstApplicationCore = false;
        bCompileICU = false;
        bBuildDeveloperTools = false;
        bUseLoggingInShipping = true;
        bIsBuildingConsoleApplication = true;
        bUseUnityBuild = false;

        // doctest's REQUIRE/CHECK macros need exceptions; same rationale
        // as BlueprintReaderMcp.Target.cs (the CMake build set /EHsc).
        bForceEnableExceptions = true;
    }
}
