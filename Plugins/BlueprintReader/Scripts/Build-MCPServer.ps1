# Build the bp-reader MCP server + tests via UBT.
#
# Convenience wrapper around `Build.bat BlueprintReaderMcp` and
# `BlueprintReaderMcpTests`. The MCP server is a UE Program target
# (Plugins/BlueprintReader/Tests/BlueprintReaderMcp/) so it builds with
# the rest of the unreal pipeline (UBA, ninja, etc.).
#
# The MCP server is engine-independent, so it is NOT coupled to the editor
# build. Release bundles ship it precompiled (built engine-free via CMake in
# CI) and the Toolbox / Install-Plugin.ps1 invoke this script on demand only
# when the user opts to (re)build from source. Run it standalone when iterating
# on server-only changes. On an installed / Launcher engine UBT refuses Program
# targets - this script auto-falls back to the CMake build in that case.
# (Older plugin versions auto-ran this via a .uplugin PreBuildSteps hook; that
# hook + PreBuildHook.ps1 were removed. A stale UBT PreBuild-N.bat from a
# hook-era build invokes the DELETED PreBuildHook.ps1 directly, not this script,
# so the legacy no-op path below does NOT cover it — Install-Plugin.ps1 clears
# those stale caches instead, or regenerate project files once after upgrading.)
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
# Legacy-call no-op path: older plugin versions declared a .uplugin
# PreBuildSteps hook that invoked this script with -ProjectDir / -PluginDir
# args, cached by UBT in Intermediate/Build/<TargetCfg>/PreBuild-N.bat. That
# hook has been removed, but a user upgrading from such a version can still
# have a stale cached PreBuild-N.bat that calls this with the old args until
# UBT regenerates the cache on the next solution refresh. Failing there with
# "unknown parameter" would block the editor build for no good reason; accept
# + no-op those calls so the build isn't blocked.

[CmdletBinding()]
param(
    [string]$EngineDir,
    [string]$ProjectFile,
    [ValidateSet("Debug","DebugGame","Development","Test","Shipping")]
    [string]$Config = "Development",
    [ValidateSet("All","Mcp","Tests")]
    [string]$Targets = "All",
    [string]$ExtraArgs = "",
    # Skip the up-to-date gate and rebuild unconditionally. By default the
    # script only invokes the toolchain when a Tests/ source is newer than the
    # built exe - so running it against an unchanged tree (e.g. the live MCP
    # server is holding the exe) is a no-op instead of a failed relink.
    [switch]$Force,
    # Legacy parameters from the old .uplugin PreBuildStep contract (now
    # removed). A user upgrading from a version that still had the hook can
    # have a cached UBT PreBuild-N.bat that invokes this with these old args
    # until UBT regenerates the cache. Accepted as no-op; see the header.
    [string]$ProjectDir,
    [string]$PluginDir,
    # Catches any other legacy/extra params without erroring.
    [Parameter(ValueFromRemainingArguments=$true)]
    [string[]]$RemainingArgs
)

$ErrorActionPreference = "Stop"
$tag = "[BlueprintReader/MCP]"

# Infer -ProjectFile / -EngineDir from the canonical in-project layout (plugin at
# <Project>/Plugins/BlueprintReader) when omitted, so the launcher works with no
# args. Skipped for the legacy PreBuildStep call (-ProjectDir), which no-ops below.
$common = Join-Path $PSScriptRoot '_Common.ps1'
if ((Test-Path $common) -and -not $ProjectDir) {
    . $common
    if (-not $ProjectFile) {
        $ProjectFile = Resolve-BprProjectFile (Resolve-BprProjectDir $PSScriptRoot)
        if ($ProjectFile) { Write-Host "$tag Inferred -ProjectFile: $ProjectFile" }
    }
    if (-not $EngineDir) {
        $EngineDir = Resolve-BprEngineDir $ProjectFile
        if ($EngineDir) { Write-Host "$tag Inferred -EngineDir: $EngineDir" }
    }
}

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

# ---- Up-to-date gate -------------------------------------------------------
# Only invoke the build toolchain when a source is actually newer than the
# built exe. The MCP server + tests are engine-independent and depend ONLY on
# Tests/ sources, so "is a build needed?" is a pure Tests/ vs Binaries/ mtime
# compare. This matters most when the server is RUNNING: the live MCP process
# holds an exclusive lock on the exe, so a needless relink fails with LNK1104
# and forces a terminal restart for nothing. Skipping the build entirely when
# nothing changed avoids that. -Force overrides the gate.
$pluginBinDir = Join-Path (Split-Path -Parent $PSScriptRoot) 'Binaries\Win64'
$projBinDir   = Join-Path (Split-Path -Parent $ProjectFile) 'Binaries\Win64'
$testsSrcDir  = Join-Path (Split-Path -Parent $PSScriptRoot) 'Tests'

$targetExes = @()
if ($Targets -in @("All","Mcp"))   { $targetExes += "BlueprintReaderMcp" }
if ($Targets -in @("All","Tests")) { $targetExes += "BlueprintReaderMcpTests" }

function Get-NewestSourceTimeUtc {
    param([string]$Dir)
    $exts = '.cpp','.cc','.cxx','.c','.h','.hpp','.inl','.cs','.txt'
    $newest = [DateTime]::MinValue
    if (-not (Test-Path $Dir)) { return $newest }
    Get-ChildItem -LiteralPath $Dir -Recurse -File -ErrorAction SilentlyContinue |
        Where-Object {
            ($exts -contains $_.Extension.ToLower()) -and
            ($_.FullName -notmatch '\\(Binaries|Intermediate)\\')
        } | ForEach-Object {
            if ($_.LastWriteTimeUtc -gt $newest) { $newest = $_.LastWriteTimeUtc }
        }
    return $newest
}

function Test-TargetStale {
    param([string]$Target, [DateTime]$NewestSrcUtc)
    # Built-artifact time = newest of the plugin-mirrored exe and the UBT
    # project-Binaries exe (either is a valid "last good build").
    $exeTimes = @()
    foreach ($dir in @($pluginBinDir, $projBinDir)) {
        $exe = Join-Path $dir "$Target.exe"
        if (Test-Path $exe) { $exeTimes += (Get-Item $exe).LastWriteTimeUtc }
    }
    if ($exeTimes.Count -eq 0) { return $true }          # never built
    $builtUtc = ($exeTimes | Sort-Object)[-1]
    return ($NewestSrcUtc -gt $builtUtc)
}

function Test-ExeLocked {
    param([string]$Path)
    if (-not (Test-Path $Path)) { return $false }
    try {
        $fs = [System.IO.File]::Open($Path, 'Open', 'ReadWrite', 'None')
        $fs.Close(); $fs.Dispose()
        return $false
    } catch { return $true }
}

function Stop-LockingServer {
    param([string]$ExePath)
    # Force-kill any process running THIS exact exe (it would block the relink /
    # the mirror copy). Match on the full image path so we only ever kill the
    # server built from this tree, never an unrelated same-named process. Then
    # poll for the OS to release the handle. Returns $true once the file is free.
    $name = [System.IO.Path]::GetFileNameWithoutExtension($ExePath)
    Get-Process -Name $name -ErrorAction SilentlyContinue | Where-Object {
        try { $_.Path -and ($_.Path -ieq $ExePath) } catch { $false }
    } | ForEach-Object {
        Write-Host "$tag Force-closing running MCP server (PID $($_.Id)) holding $name.exe..."
        try { Stop-Process -Id $_.Id -Force -ErrorAction Stop } catch {
            Write-Host "$tag   could not kill PID $($_.Id): $($_.Exception.Message)"
        }
    }
    for ($i = 0; $i -lt 20; $i++) {           # up to ~5s for the handle to drop
        if (-not (Test-ExeLocked $ExePath)) { return $true }
        Start-Sleep -Milliseconds 250
    }
    return -not (Test-ExeLocked $ExePath)
}

if (-not $Force) {
    $newestSrc = Get-NewestSourceTimeUtc -Dir $testsSrcDir
    $stale = @($targetExes | Where-Object { Test-TargetStale -Target $_ -NewestSrcUtc $newestSrc })
    if ($stale.Count -eq 0) {
        Write-Host "$tag MCP server is up to date (no Tests/ source newer than the built exe) - skipping build."
        Write-Host "$tag Pass -Force to rebuild anyway."
        return
    }
    if ($stale.Count -lt $targetExes.Count) {
        $skipped = $targetExes | Where-Object { $stale -notcontains $_ }
        Write-Host "$tag Up to date, skipping: $($skipped -join ', ')"
    }
    Write-Host "$tag Build needed for: $($stale -join ', ')"
    # Narrow the build to just the stale target(s) for the UBT path below.
    $Targets = if (($stale -contains "BlueprintReaderMcp") -and ($stale -contains "BlueprintReaderMcpTests")) { "All" }
               elseif ($stale -contains "BlueprintReaderMcp") { "Mcp" } else { "Tests" }
}

# A rebuild is needed - if the running MCP server is holding the exe, the relink
# (UBT to project Binaries, or the mirror Copy-Item to plugin Binaries) would die
# with LNK1104 / "file in use". Force-close the running server and proceed rather
# than failing the build. (We only reach here when a source is genuinely newer
# than the exe, so killing the server to relink is the intended trade.)
foreach ($t in $targetExes) {
    foreach ($dir in @($pluginBinDir, $projBinDir)) {
        $exe = Join-Path $dir "$t.exe"
        if (Test-ExeLocked $exe) {
            if (-not (Stop-LockingServer -ExePath $exe)) {
                throw "$tag $t.exe is still locked after force-closing the MCP server - " +
                      "another process (debugger, AV scan, or a client respawning it) is holding it. Free it and retry."
            }
            Write-Host "$tag Freed $t.exe - continuing the build."
        }
    }
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
