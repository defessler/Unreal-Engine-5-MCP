# Build the bp-reader MCP server + tests via UBT.
#
# Convenience wrapper around `Build.bat BlueprintReaderMcp` and
# `BlueprintReaderMcpTests`. Replaces the prior CMake-based script that
# ran as a PreBuildStep — the MCP server is now a UE Program target
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

[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)] [string]$EngineDir,
    [Parameter(Mandatory=$true)] [string]$ProjectFile,
    [ValidateSet("Debug","DebugGame","Development","Test","Shipping")]
    [string]$Config = "Development",
    [ValidateSet("All","Mcp","Tests")]
    [string]$Targets = "All",
    [string]$ExtraArgs = ""
)

$ErrorActionPreference = "Stop"
$tag = "[BlueprintReader/MCP]"

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
