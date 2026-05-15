# Build the bp-reader MCP server + tests via UBT.
#
# Convenience wrapper around `Build.bat BlueprintReaderMcp` and
# `BlueprintReaderMcpTests`. Replaces the prior CMake-based script that
# ran as a PreBuildStep -- the MCP server is now a UE Program target
# (Plugins/BlueprintReader/Tests/BlueprintReaderMcp/) so it builds with
# the rest of the unreal pipeline (UBA, ninja, etc.) instead of running
# its own toolchain. PreBuildStep is gone; this script is opt-in.
#
# Usage:
#   .\Build-MCPServer.ps1 -EngineDir "D:\Path\To\UnrealEngine"
#                         -ProjectFile "D:\Path\To\MyGame.uproject"
#                         [-Config Development]   # Debug | DebugGame | Development | Test | Shipping
#                         [-Targets All]          # All | Mcp | Tests
#                         [-ExtraArgs "-NoUba -MaxParallelActions=4"]
#
# Exits non-zero on any UBT failure.
#
# Legacy-call no-op path: if invoked with the old -ProjectDir / -PluginDir
# parameters (the pre-UBT PreBuildStep contract), the script logs a
# deprecation notice and exits 0. UBT caches PreBuild invocations in
# Intermediate/Build/<TargetCfg>/PreBuild-N.bat -- when a user updates
# the plugin without regenerating project files, those cached scripts
# still call this with old args. Failing there with "unknown parameter"
# blocks the editor build for no good reason; accepting + no-op'ing
# clears the way until UBT regenerates the cache on the next solution
# refresh. The .uplugin no longer declares a PreBuildStep, so once the
# cache regenerates this path stops being hit.

[CmdletBinding()]
param(
    [string]$EngineDir,
    [string]$ProjectFile,
    [ValidateSet("Debug","DebugGame","Development","Test","Shipping")]
    [string]$Config = "Development",
    [ValidateSet("All","Mcp","Tests")]
    [string]$Targets = "All",
    [string]$ExtraArgs = "",
    # Legacy parameters from the pre-UBT PreBuildStep contract (PR #75
    # removed the PreBuildStep itself, but cached UBT PreBuild-N.bat
    # scripts can still invoke this with these args until UBT
    # regenerates them). Accepted as no-op; see the header comment.
    [string]$ProjectDir,
    [string]$PluginDir,
    # Catches any other legacy/extra params without erroring.
    [Parameter(ValueFromRemainingArguments=$true)]
    [string[]]$RemainingArgs
)

$ErrorActionPreference = "Stop"
$tag = "[BlueprintReader/MCP]"

# Legacy invocation: PR-#75-era cached PreBuild-N.bat calling us with
# -ProjectDir / -PluginDir. Log + exit 0 so the editor build proceeds.
# The new build path is invoked explicitly via -EngineDir / -ProjectFile.
if ($ProjectDir -or $PluginDir -or -not $EngineDir -or -not $ProjectFile) {
    if ($ProjectDir -or $PluginDir) {
        Write-Host "$tag Legacy PreBuildStep invocation detected (-ProjectDir / -PluginDir)."
        Write-Host "$tag The MCP server is now its own UBT Program target -- no longer built"
        Write-Host "$tag as part of the editor build. Build it explicitly with:"
        Write-Host "$tag   Build.bat BlueprintReaderMcp Win64 Development -project=<.uproject>"
        Write-Host "$tag (or via this script with -EngineDir / -ProjectFile)."
        Write-Host "$tag To stop this notice from appearing on every build, regenerate your"
        Write-Host "$tag UE project files (right-click the .uproject -> 'Generate Visual Studio"
        Write-Host "$tag project files'). UBT will then re-read the .uplugin (which no longer"
        Write-Host "$tag declares a PreBuildStep) and stop calling this script."
        exit 0
    }
    # No legacy args either; user invoked the new API without required params.
    throw "$tag -EngineDir and -ProjectFile are required for normal invocation."
}

$BuildBat = Join-Path $EngineDir "Engine\Build\BatchFiles\Build.bat"
if (-not (Test-Path $BuildBat)) {
    throw "$tag Build.bat not found at $BuildBat (check -EngineDir)"
}
if (-not (Test-Path $ProjectFile)) {
    throw "$tag .uproject not found at $ProjectFile (check -ProjectFile)"
}

$wanted = @()
if ($Targets -in @("All","Mcp"))   { $wanted += "BlueprintReaderMcp" }
if ($Targets -in @("All","Tests")) { $wanted += "BlueprintReaderMcpTests" }

foreach ($target in $wanted) {
    Write-Host "$tag Building $target ($Config) via UBT..."
    $argv = @($target, "Win64", $Config, "-project=$ProjectFile", "-waitmutex")
    if ($ExtraArgs) { $argv += $ExtraArgs.Split(" ") }
    & $BuildBat @argv
    if ($LASTEXITCODE -ne 0) {
        throw "$tag $target build failed (exit $LASTEXITCODE)"
    }
}

Write-Host "$tag Done."
