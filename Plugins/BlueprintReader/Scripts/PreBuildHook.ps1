# PreBuildHook.ps1
# Invoked by BlueprintReader.uplugin's PreBuildSteps for every target
# that enables the plugin. Builds the MCP server (BlueprintReaderMcp
# Program target) so editor builds don't leave Binaries/Win64/
# BlueprintReaderMcp.exe stale.
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
# Skip the auto-build entirely by setting BP_READER_SKIP_PREBUILD=1 in
# the build environment.

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

# 2. Recursion guard -- when the parent target IS one of the plugin's
# Program targets, building it again would loop. Skip.
$selfTargets = @('BlueprintReaderMcp', 'BlueprintReaderMcpTests')
if ($selfTargets -contains $TargetName) {
    # Quiet -- printing on every recursive entry spams the build log.
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

# 5. Delegate to the wrapper. Build only the MCP server (not the test
# exe) -- the tests are heavier and not needed for editor work.
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
Write-Host "$tag building BlueprintReaderMcp ($Configuration) before $TargetName..."
& $wrapper -EngineDir $EngineDir -ProjectFile $ProjectFile -Config $Configuration -Targets Mcp -ExtraArgs $extra
exit $LASTEXITCODE
