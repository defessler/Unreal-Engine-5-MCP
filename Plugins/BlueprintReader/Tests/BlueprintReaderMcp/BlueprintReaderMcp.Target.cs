// Copyright contributors to the UE5_MCP project.
//
// UE Program target for the bp-reader MCP server. Pure C++20 console
// app — no engine link, no UObject, no FEngineLoop. Built via UBT so
// it participates in the same build pipeline (and UBA, when enabled)
// as the rest of the plugin instead of running a separate CMake step.
//
// Pattern lifted from `Engine/Source/Programs/BreakpadSymbolEncoder/`
// — the closest in-tree precedent for a stdlib-only Program. Notable
// flags:
//   * bCompileAgainstEngine = false        : no UObject / FEngineLoop
//   * bCompileAgainstCoreUObject = false   : ditto
//   * bIsBuildingConsoleApplication = true : main(), not WinMain()
//   * bAddDefaultIncludePaths = false      : in the Build.cs — keeps
//     UE's prelude from leaking into mcp-server sources that use
//     stdlib only.
//
// Layout note: the Programs live under `<Plugin>/Tests/` because
// UBT's auto-discovery only scans plugin Source/ for *Module* rules,
// not Target rules — plugin-hosted Programs need to live in the
// `Tests/` directory (the one path UBT does scan for plugin Target
// rules; see Engine/Source/Programs/UnrealBuildTool/System/
// RulesCompiler.cs `FindTestRulesForPlugins`). The dir name is a
// historical UBT artifact; the BlueprintReaderMcp target is a
// production executable, not a test. The actual doctest suite lives
// in the sibling `BlueprintReaderMcpTests` target.

using UnrealBuildTool;
using System.Collections.Generic;

[SupportedPlatforms("Win64")]
public class BlueprintReaderMcpTarget : TargetRules
{
    public BlueprintReaderMcpTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Program;
        LinkType = TargetLinkType.Monolithic;
        LaunchModuleName = "BlueprintReaderMcp";

        IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
        CppStandard = CppStandardVersion.Cpp20;

        // Standalone — no engine modules linked.
        bCompileAgainstEngine = false;
        bCompileAgainstCoreUObject = false;
        bCompileAgainstApplicationCore = false;
        bCompileICU = false;
        bBuildDeveloperTools = false;
        bUseLoggingInShipping = true;

        // Plain int main() with argv[].
        bIsBuildingConsoleApplication = true;

        // Exceptions are on. The mcp-server uses nlohmann::json which
        // throws on parse failure; the CMake build set /EHsc globally.
        // UE Programs disable exceptions by default to match engine
        // settings — flip this back for parity with the CMake build.
        bForceEnableExceptions = true;

        // mcp-server sources don't share headers across TU boundaries
        // in unity-incompatible ways, but each TU pulls in heavy stdlib
        // headers (filesystem, variant, optional, regex). Unity build
        // bloats those PCH-style compiles unnecessarily; turning it off
        // here matches the CMake build's per-TU compile model.
        bUseUnityBuild = false;
    }
}
