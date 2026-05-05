# Build the bp-reader-mcp standalone MCP server as a pre-build step of the
# BlueprintReader plugin's editor module.
#
# Wired up in BlueprintReader.uplugin's `PreBuildSteps.Win64`. UnrealBuildTool
# substitutes $(ProjectDir) etc. before invoking us. We do the cmake configure
# (only on first build, when build/ doesn't exist) and the cmake --build, and
# we skip both when the produced .exe is newer than every source under src/
# plus CMakeLists.txt — so incremental UE builds don't pay for a no-op MCP
# rebuild.
#
# Looks for mcp-server/ in two places, in this order:
#   1. $(ProjectDir)/mcp-server    — current layout (server is a sibling of Plugins/)
#   2. $(PluginDir)/mcp-server     — alternate layout if the server ever moves
#                                    inside the plugin
# If neither exists, we log and exit 0 so the plugin can still be dropped into
# a project that doesn't ship the MCP server source.
#
# Usage (as invoked by UE):
#   powershell.exe -ExecutionPolicy Bypass -File <ThisScript>
#       -ProjectDir "$(ProjectDir)" -PluginDir "$(PluginDir)"

param(
    [Parameter(Mandatory=$true)] [string]$ProjectDir,
    [Parameter(Mandatory=$true)] [string]$PluginDir,
    [string]$Generator = "Visual Studio 17 2022",
    [string]$Config    = "Release"
)

$ErrorActionPreference = "Stop"
$tag = "[BlueprintReader/MCP]"

function Resolve-McpDir {
    foreach ($candidate in @(
        (Join-Path $ProjectDir "mcp-server"),
        (Join-Path $PluginDir  "mcp-server")
    )) {
        if (Test-Path (Join-Path $candidate "CMakeLists.txt")) {
            return (Resolve-Path $candidate).Path
        }
    }
    return $null
}

$mcpDir = Resolve-McpDir
if ($null -eq $mcpDir) {
    Write-Host "$tag mcp-server/ not found under ProjectDir or PluginDir; skipping (plugin is being used standalone)."
    exit 0
}

$buildDir = Join-Path $mcpDir "build"
$exePath  = Join-Path $buildDir "$Config\bp-reader-mcp.exe"

# Skip path: exe exists and is newer than every src/ file + CMakeLists.txt.
if (Test-Path $exePath) {
    $exeTime = (Get-Item $exePath).LastWriteTimeUtc
    $sources = @()
    $sources += Get-Item -Path (Join-Path $mcpDir "CMakeLists.txt") -ErrorAction SilentlyContinue
    $sources += Get-ChildItem -Recurse -File -Path (Join-Path $mcpDir "src") `
                                            -Include *.cpp, *.h, *.hpp `
                                            -ErrorAction SilentlyContinue
    $newest = $sources | Where-Object { $_ -ne $null } |
              Sort-Object LastWriteTimeUtc -Descending |
              Select-Object -First 1
    if ($newest -and $newest.LastWriteTimeUtc -le $exeTime) {
        Write-Host "$tag bp-reader-mcp.exe up to date (newest src: $($newest.Name) @ $($newest.LastWriteTimeUtc))."
        exit 0
    }
}

# Verify cmake is on PATH. We don't try to vswhere our way to a bundled copy —
# if the developer has VS but not cmake, the friendlier action is to error
# loudly so they `winget install Kitware.CMake` once.
$cmake = Get-Command cmake -ErrorAction SilentlyContinue
if ($null -eq $cmake) {
    Write-Error "$tag cmake.exe not found on PATH. Install CMake (winget install Kitware.CMake) and retry."
    exit 1
}

# Note: third-party deps (nlohmann_json, fmt, doctest) are vendored under
# mcp-server/third_party/, so the configure step needs no git, no network,
# and no vcpkg. CMake itself is the only external tool required.

Write-Host "$tag Building MCP server at $mcpDir ..."

if (-not (Test-Path $buildDir)) {
    Write-Host "$tag Configuring CMake (first build)..."
    & cmake -S $mcpDir -B $buildDir -G $Generator -A x64
    if ($LASTEXITCODE -ne 0) {
        Write-Error "$tag cmake configure failed (exit $LASTEXITCODE)."
        exit $LASTEXITCODE
    }
}

& cmake --build $buildDir --config $Config
if ($LASTEXITCODE -ne 0) {
    Write-Error "$tag cmake --build failed (exit $LASTEXITCODE)."
    exit $LASTEXITCODE
}

Write-Host "$tag Built $exePath."
exit 0
