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

        // Version stamp (INSTALL-1): mirror BlueprintReader.uplugin's
        // VersionName + the git short-hash so --version / doctor / the MCP
        // initialize handshake report exactly which build is running. The
        // CMake fallback build sets the same defines for the installed-engine
        // path. PUBLIC so they reach main.cpp + Diagnostics.cpp.
        string pluginRoot = Path.GetFullPath(Path.Combine(ModuleDirectory, "..", ".."));
        string bprVersion = "0.0.0";
        try
        {
            string uplugin = Path.Combine(pluginRoot, "BlueprintReader.uplugin");
            if (File.Exists(uplugin))
            {
                var m = System.Text.RegularExpressions.Regex.Match(
                    File.ReadAllText(uplugin), "\"VersionName\"\\s*:\\s*\"([^\"]+)\"");
                if (m.Success) { bprVersion = m.Groups[1].Value; }
            }
        }
        catch { /* best-effort — fall back to 0.0.0 */ }
        string bprGitHash = "unknown";
        try
        {
            var psi = new System.Diagnostics.ProcessStartInfo("git",
                "-C \"" + pluginRoot + "\" rev-parse --short HEAD")
            {
                RedirectStandardOutput = true,
                UseShellExecute = false,
                CreateNoWindow = true,
            };
            using (var proc = System.Diagnostics.Process.Start(psi))
            {
                string outStr = proc.StandardOutput.ReadToEnd().Trim();
                proc.WaitForExit();
                if (proc.ExitCode == 0 && outStr.Length > 0) { bprGitHash = outStr; }
            }
        }
        catch { /* git not available / not a repo — leave "unknown" */ }
        PublicDefinitions.Add("BPR_VERSION=\"" + bprVersion + "\"");
        PublicDefinitions.Add("BPR_GIT_HASH=\"" + bprGitHash + "\"");

        // Win32 socket API used by SocketBlueprintReader and the daemon
        // probe. Bring Ws2_32 in here so every consuming target gets it.
        if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            PublicSystemLibraries.Add("Ws2_32.lib");
        }
    }
}
