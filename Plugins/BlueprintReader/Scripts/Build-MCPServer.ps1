# Build the bp-reader MCP server + tests via UBT.
#
# Convenience wrapper around `Build.bat BlueprintReaderMcp` and
# `BlueprintReaderMcpTests`. The MCP server is a UE Program target
# (Plugins/BlueprintReader/Tests/BlueprintReaderMcp/) so it builds with
# the rest of the unreal pipeline (UBA, ninja, etc.).
#
# The plugin's .uplugin DOES declare a PreBuildSteps hook (Scripts/PreBuildHook.ps1)
# that invokes this script during a consuming editor target's build, so the
# server is rebuilt alongside the editor module automatically. You can also
# run it standalone (opt-in) when iterating on server-only changes. On an
# installed / Launcher engine UBT refuses Program targets - use the CMake
# fallback (Saved/build-mcp-cmake.ps1) instead.
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
# refresh. The current PreBuildStep (PreBuildHook.ps1) calls this script
# with the new -EngineDir / -ProjectFile API, so once the cache regenerates
# this legacy path stops being hit.

[CmdletBinding()]
param(
    [string]$EngineDir,
    [string]$ProjectFile,
    [ValidateSet("Debug","DebugGame","Development","Test","Shipping")]
    [string]$Config = "Development",
    [ValidateSet("All","Mcp","Tests")]
    [string]$Targets = "All",
    [string]$ExtraArgs = "",
    # Legacy parameters from the pre-UBT PreBuildStep contract. The hook
    # was later re-added as PreBuildHook.ps1 (which calls this script with
    # the new -EngineDir / -ProjectFile API), but cached UBT PreBuild-N.bat
    # scripts can still invoke this with these old args until UBT
    # regenerates them. Accepted as no-op; see the header comment.
    [string]$ProjectDir,
    [string]$PluginDir,
    # Catches any other legacy/extra params without erroring.
    [Parameter(ValueFromRemainingArguments=$true)]
    [string[]]$RemainingArgs
)

$ErrorActionPreference = "Stop"
$tag = "[BlueprintReader/MCP]"

# INSTALL-M2: engine-free CMake/Ninja build of the server + tests, for an
# installed/Launcher engine where UBT refuses Program targets. Mirrors the
# documented fallback; CMakeLists lands the exes in the plugin's own
# Binaries/Win64 (Plugins/BlueprintReader/Binaries/Win64).
function Invoke-CMakeServerBuild {
    param([string]$ScriptsDir, [string]$BuildConfig)
    $testsDir = Join-Path (Split-Path -Parent $ScriptsDir) "Tests"
    if (-not (Test-Path (Join-Path $testsDir "CMakeLists.txt"))) {
        throw "$tag CMake fallback: Tests/CMakeLists.txt not found at $testsDir"
    }
    # Import the MSVC env (cl + ninja) from vcvars64.
    $vsw = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vsw)) {
        throw "$tag CMake fallback needs Visual Studio C++ tools (vswhere not found at $vsw)."
    }
    $ip = & $vsw -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $vcvars = Join-Path $ip "VC\Auxiliary\Build\vcvars64.bat"
    if (-not (Test-Path $vcvars)) { throw "$tag vcvars64.bat not found at $vcvars" }
    cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
    }
    $cmakeCfg = if ($BuildConfig -in @("Debug","DebugGame")) { "Debug" } else { "RelWithDebInfo" }
    $buildDir = Join-Path ([System.IO.Path]::GetTempPath()) "bpr-mcp-cmake-build"
    & cmake -S $testsDir -B $buildDir -G Ninja "-DCMAKE_BUILD_TYPE=$cmakeCfg"
    if ($LASTEXITCODE -ne 0) { throw "$tag cmake configure failed ($LASTEXITCODE)" }
    & cmake --build $buildDir
    if ($LASTEXITCODE -ne 0) { throw "$tag cmake build failed ($LASTEXITCODE)" }
}

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

# INSTALL-M2: an installed/Launcher engine rejects UE Program targets
# ("Program targets are not currently supported from this engine distribution").
# Detect it via the InstalledBuild.txt marker and transparently use the
# engine-free CMake/Ninja fallback instead - so the caller doesn't have to know
# which toolchain applies.
if (Test-Path (Join-Path $EngineDir "Engine\Build\InstalledBuild.txt")) {
    Write-Host "$tag Installed engine detected (InstalledBuild.txt) - UBT can't build"
    Write-Host "$tag Program targets here; using the engine-free CMake/Ninja fallback."
    Invoke-CMakeServerBuild -ScriptsDir $PSScriptRoot -BuildConfig $Config
    Write-Host "$tag Done (CMake fallback)."
    return
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

# UBT Program targets output to <Project>/Binaries/Win64. Mirror them into the
# plugin's own Binaries/Win64 so the server ships with the plugin (portable) and
# .mcp.json / the helper scripts have one canonical path across build toolchains.
$projBin   = Join-Path (Split-Path -Parent $ProjectFile) 'Binaries\Win64'
$pluginBin = Join-Path (Split-Path -Parent $PSScriptRoot) 'Binaries\Win64'
if ($projBin -ne $pluginBin) {
    New-Item -ItemType Directory -Force -Path $pluginBin | Out-Null
    foreach ($target in $wanted) {
        $src = Join-Path $projBin "$target.exe"
        if (Test-Path $src) {
            Copy-Item $src (Join-Path $pluginBin "$target.exe") -Force
            Write-Host "$tag Mirrored $target.exe -> plugin Binaries\Win64"
        }
    }
}

Write-Host "$tag Done."
