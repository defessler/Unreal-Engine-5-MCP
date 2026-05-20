# PreBuildHook.ps1
# Invoked by BlueprintReader.uplugin's PreBuildSteps for every target
# that enables the plugin. Verifies that every co-resident Program
# target (the MCP server + its tests) is up-to-date with the plugin's
# current source, and stages fixture files into the runtime dir so the
# test exe can find them.
#
# What gets verified each parent build:
#   1. BlueprintReaderMcp.exe       -- the standalone MCP server
#   2. BlueprintReaderMcpTests.exe  -- the doctest suite
#   3. <Binaries>/<Platform>/fixtures/*.json staged from
#      Plugins/BlueprintReader/Tests/BlueprintReaderMcpTests/fixtures/
#
# UBT's own incremental build keeps "no-op rebuild" fast (sub-second
# for an already-up-to-date target), so the per-build cost of verifying
# everything is small in steady state. Stale targets fall back to a
# full UBT rebuild for that target only.
#
# Recursion guard: the BlueprintReaderMcp and BlueprintReaderMcpTests
# targets live inside this same plugin, so UBT runs the plugin's
# PreBuildSteps when building them too. Without the guard we'd loop
# forever -- when the parent target IS the MCP server (or its tests),
# this script no-ops.
#
# All inputs come from UBT's PreBuildSteps macro expansion:
#   $(EngineDir), $(ProjectFile), $(TargetName), $(TargetConfiguration)
#
# Env opt-outs (set any to '1' to suppress that part of the
# verification):
#   BP_READER_SKIP_PREBUILD     -- whole script no-ops
#   BP_READER_SKIP_TESTS_BUILD  -- skip BlueprintReaderMcpTests
#                                  (preserves the pre-#171 behavior
#                                   for users who want the lighter
#                                   parent-target builds)
#   BP_READER_SKIP_FIXTURES     -- skip staging fixture .json files

[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$EngineDir,
    [Parameter(Mandatory=$true)][string]$ProjectFile,
    [Parameter(Mandatory=$true)][string]$TargetName,
    [string]$Configuration = "Development",

    # Default = the script's own plugin dir. The macro $(PluginDir) is
    # also passed by the .uplugin so this stays robust to copies.
    [string]$PluginDir
)

$ErrorActionPreference = "Stop"
$tag = "[BlueprintReader/PreBuild]"

# 1. Env opt-out -- any non-empty value disables the auto-build.
if ($env:BP_READER_SKIP_PREBUILD) {
    Write-Host "$tag BP_READER_SKIP_PREBUILD is set; skipping MCP server build for $TargetName."
    exit 0
}

# Hash a file via .NET so the hook works even on machines where
# Microsoft.PowerShell.Utility (which ships Get-FileHash) isn't on
# $PSModulePath or otherwise fails to auto-load. UBT invokes this via
# `powershell.exe -NoProfile`, which has been seen on some Windows
# hosts to produce 'Get-FileHash : not recognized as the name of a
# cmdlet'. Calling [System.Security.Cryptography.SHA1] directly
# sidesteps the module-loading layer entirely.
function Get-Sha1Hex {
    param([Parameter(Mandatory)] [string] $Path)
    $sha = [System.Security.Cryptography.SHA1]::Create()
    try {
        $stream = [System.IO.File]::OpenRead($Path)
        try {
            return [System.BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-','').ToUpperInvariant()
        } finally { $stream.Dispose() }
    } finally { $sha.Dispose() }
}

# Function to stage fixture .json files into <ProjectDir>/Binaries/Win64/fixtures/.
# The mock backend reads from `<exe>/fixtures` (MockBlueprintReader ctor +
# test_helpers::FixturesDir), and UBT doesn't copy them automatically -- the
# fixtures aren't source files UBT tracks. Without this staging the test
# exe hits "fixture directory does not exist" on first run after a clean
# build. Idempotent: hash-compares src vs dst, only copies on mismatch.
# Skips when BP_READER_SKIP_FIXTURES=1 (occasionally useful for users
# hand-staging custom fixture sets).
function Stage-Fixtures {
    if ($env:BP_READER_SKIP_FIXTURES) { return }
    $fixturesSrc = Join-Path $PluginDir 'Tests\BlueprintReaderMcpTests\fixtures'
    if (-not (Test-Path -LiteralPath $fixturesSrc)) { return }
    $projectDir = Split-Path -Parent $ProjectFile
    $fixturesDst = Join-Path $projectDir 'Binaries\Win64\fixtures'
    New-Item -ItemType Directory -Path $fixturesDst -Force | Out-Null
    $srcFiles = Get-ChildItem -LiteralPath $fixturesSrc -Filter '*.json' -File -ErrorAction SilentlyContinue
    $staged = 0
    foreach ($f in $srcFiles) {
        $dstPath = Join-Path $fixturesDst $f.Name
        $needsCopy = $true
        if (Test-Path -LiteralPath $dstPath) {
            $srcHash = Get-Sha1Hex -Path $f.FullName
            $dstHash = Get-Sha1Hex -Path $dstPath
            if ($srcHash -eq $dstHash) { $needsCopy = $false }
        }
        if ($needsCopy) {
            Copy-Item -LiteralPath $f.FullName -Destination $dstPath -Force
            $staged++
        }
    }
    if ($staged -gt 0) {
        Write-Host "$tag staged $staged fixture(s) into $fixturesDst"
    }
}

# Resolve PluginDir + ProjectFile-derived paths up front so Stage-Fixtures
# (closure-style; reads outer-scope vars) works from both code paths below.
if (-not $PluginDir) {
    $scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
    $PluginDir = (Resolve-Path (Join-Path $scriptDir '..')).Path
}

# 2. Recursion guard -- when the parent target IS one of the plugin's
# Program targets, building it again would loop. We still stage fixtures
# (the test exe is being built RIGHT NOW; staging fixtures here means
# they're ready by the time the link finishes) but skip the nested UBT
# recursion.
$selfTargets = @('BlueprintReaderMcp', 'BlueprintReaderMcpTests')
if ($selfTargets -contains $TargetName) {
    Stage-Fixtures
    exit 0
}

# 3. Normalize -EngineDir. UBT's $(EngineDir) macro expands to the
# `<root>/Engine/` subdirectory (so Build.bat lives at
# $(EngineDir)/Build/BatchFiles/Build.bat). Build-MCPServer.ps1 expects
# the engine root (so it can join `Engine/Build/...` itself, matching
# the way users invoke the wrapper directly). Strip the trailing
# `Engine` segment if present.
$normEngine = $EngineDir.TrimEnd('\','/')
$leaf = Split-Path -Leaf $normEngine
if ($leaf -ieq 'Engine') {
    $EngineDir = Split-Path -Parent $normEngine
}

# 4. Resolve the wrapper script alongside this one.
if (-not $PluginDir) {
    $scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
    $PluginDir = (Resolve-Path (Join-Path $scriptDir '..')).Path
}
$wrapper = Join-Path $PluginDir 'Scripts\Build-MCPServer.ps1'
if (-not (Test-Path -LiteralPath $wrapper)) {
    Write-Error "$tag Build-MCPServer.ps1 not found at $wrapper"
    exit 1
}

# 5. Delegate to the wrapper. By default we build BOTH the MCP server
# AND its doctest suite so a parent editor build leaves every
# co-resident Program target up-to-date. The user can opt out of the
# tests-build via BP_READER_SKIP_TESTS_BUILD=1.
#
# Two flags are critical for nested UBT execution:
#   -NoMutex   : parent UBT holds the global UBT mutex throughout
#                PreBuildStep execution. Without -NoMutex the child
#                blocks on it forever (parent waits on us; we wait
#                on parent -> deadlock). Intermediate dirs don't
#                collide (parent builds the editor target's tree,
#                child builds BlueprintReaderMcp's), so concurrent
#                UBT is safe here.
#   -Log=<path>: parent UBT has the default Log.txt open exclusively.
#                Point the child at a sibling log file so it can write
#                its own trace without colliding.
# $env:TEMP path has no spaces on default Windows installs; if it did
# the Build-MCPServer wrapper's `.Split(" ")` would mangle the quoted
# form. Belt-and-braces: use the 8.3 short name to dodge any spaces.
$childLog = Join-Path $env:TEMP "UnrealBuildTool-BlueprintReaderMcp.log"
$extra = "-NoMutex -Log=$childLog"

# Pick the target set: Mcp-only if the user opted out of tests,
# otherwise All (Mcp + Tests).
$targetSet = if ($env:BP_READER_SKIP_TESTS_BUILD) { "Mcp" } else { "All" }
Write-Host "$tag verifying $targetSet ($Configuration) before $TargetName..."
& $wrapper -EngineDir $EngineDir -ProjectFile $ProjectFile -Config $Configuration -Targets $targetSet -ExtraArgs $extra
$buildExit = $LASTEXITCODE
if ($buildExit -ne 0) {
    Write-Error "$tag UBT child build failed (exit $buildExit) -- skipping fixture staging."
    exit $buildExit
}

# 6. Stage fixture .json files into the runtime fixtures dir so the
# test exe can find them. Implemented as Stage-Fixtures up top so the
# recursion-guard path can also call it.
Stage-Fixtures
exit 0
