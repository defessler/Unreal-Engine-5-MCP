// Copyright contributors to the UE5_MCP project.
//
// Shared static-library module containing the bp-reader MCP server's
// implementation. The actual program (BlueprintReaderMcp) and the
// doctest suite (BlueprintReaderMcpTests) each link this module —
// avoids compiling the same ~25 TUs twice while keeping each Program
// target small (just main.cpp / test_*.cpp + the link step).
//
// This module has no .Target.cs and is never built as an exe on its
// own; UBT compiles it on demand for any consuming Program target.
//
// Pattern caveat: this lives in `<Plugin>/Tests/` because UBT only
// auto-discovers Module rules in `<Plugin>/Source/` (plugin modules,
// loaded at runtime) and `<Plugin>/Tests/` (plugin test programs).
// We're neither — but Tests/ is the closest first-class fit and the
// only path that lets a plugin host its own Program-flavored modules.

using UnrealBuildTool;
using System.IO;

public class BlueprintReaderMcpCore : ModuleRules
{
    public BlueprintReaderMcpCore(ReadOnlyTargetRules Target) : base(Target)
    {
        // Don't auto-include UE's prelude. Our TUs use stdlib only —
        // dragging FString / FCoreDelegates / etc. in via CoreMinimal
        // both bloats compile and risks macro collisions with our
        // namespace types.
        bAddDefaultIncludePaths = false;
        PCHUsage = PCHUsageMode.NoPCHs;
        PublicIncludePathModuleNames.Add("Launch");

        // mcp-server uses dynamic_cast on the decorator chain
        // (WrapWithCache, ReadOnly wrappers etc.) and through doctest
        // patterns. UE Programs disable RTTI by default — the CMake
        // build had it on via MSVC's default /GR. Re-enable here.
        bUseRTTI = true;

        // Header-only vendored deps — shared with the sibling Programs.
        string ThirdParty = Path.Combine(ModuleDirectory, "..", "ThirdParty");
        PublicIncludePaths.Add(Path.Combine(ThirdParty, "nlohmann_json"));
        PublicIncludePaths.Add(Path.Combine(ThirdParty, "fmt"));

        // Our own headers reference like `#include "tools/Foo.h"`.
        // Make Private/ public so the consuming Program targets'
        // main.cpp / test_*.cpp can `#include "backends/..."`.
        PublicIncludePaths.Add(Path.Combine(ModuleDirectory, "Private"));

        PublicDefinitions.Add("FMT_HEADER_ONLY=1");
        PrivateDefinitions.Add("NOMINMAX");
        PrivateDefinitions.Add("_CRT_SECURE_NO_WARNINGS");

        // Win32 socket API used by SocketBlueprintReader and the daemon
        // probe. Bring Ws2_32 in here so every consuming target gets it.
        if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            PublicSystemLibraries.Add("Ws2_32.lib");
        }
    }
}
