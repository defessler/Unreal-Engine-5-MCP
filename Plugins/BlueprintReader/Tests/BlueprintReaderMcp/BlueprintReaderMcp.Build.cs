// Copyright contributors to the UE5_MCP project.
//
// Module rules for the bp-reader MCP Program. Just main.cpp here —
// everything else lives in the sibling BlueprintReaderMcpCore module
// that this target links statically.

using UnrealBuildTool;

public class BlueprintReaderMcp : ModuleRules
{
    public BlueprintReaderMcp(ReadOnlyTargetRules Target) : base(Target)
    {
        bAddDefaultIncludePaths = false;
        PCHUsage = PCHUsageMode.NoPCHs;
        PublicIncludePathModuleNames.Add("Launch");

        // main.cpp dynamic_casts the IBlueprintReader to MockBlueprintReader*
        // to log fixture count at startup. Needs RTTI — UE Programs
        // disable it by default; CMake build's MSVC has /GR on. McpCore
        // already enables this for the same reason; mirroring here.
        bUseRTTI = true;

        // All the heavy lifting (jsonrpc, tools, backends, parsers,
        // codegen) lives here. main.cpp just wires it up.
        PrivateDependencyModuleNames.Add("BlueprintReaderMcpCore");
    }
}
