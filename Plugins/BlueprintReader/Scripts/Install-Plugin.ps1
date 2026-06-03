#requires -Version 7
# Install-Plugin.ps1 — one-shot installer (INSTALL-M1).
#
# Collapses the ~8-step manual setup into a single command: mount the plugin
# into a UE project, build the MCP server (Build-MCPServer.ps1 auto-picks UBT
# for a source engine or the CMake fallback for an installed one, INSTALL-M2),
# write the MCP client config, deploy the Claude/AGENTS assets, and finish with
# `doctor`. All sub-steps are existing scripts — this is the glue.
#
# Usage:
#   pwsh Plugins/BlueprintReader/Scripts/Install-Plugin.ps1 `
#     -EngineDir "D:\Path\To\UE_5.8" -ProjectFile "D:\Path\To\MyGame.uproject"
#
#   Options:
#     -Client <ClaudeCode|Cursor|VSCode|Gemini|Codex|All>   (default ClaudeCode)
#     -Symlink              symlink the plugin instead of copying it
#     -ApplyEnginePatches   run Patch-Engine.ps1 -Apply (source engines only)
#     -SkipBuild            skip the server build (config/assets/doctor only)
#     -Force                replace an existing mounted plugin / config

param(
    [Parameter(Mandatory=$true)][string]$EngineDir,
    [Parameter(Mandatory=$true)][string]$ProjectFile,
    [ValidateSet('ClaudeCode','Cursor','VSCode','Gemini','Codex','All')]
    [string]$Client = 'ClaudeCode',
    [switch]$Symlink,
    [switch]$ApplyEnginePatches,
    [switch]$SkipBuild,
    [switch]$Force
)
$ErrorActionPreference = 'Stop'
$tag = '[BlueprintReader/Install]'

$scriptsDir = Split-Path -Parent $PSCommandPath
$pluginSrc  = (Resolve-Path (Split-Path -Parent $scriptsDir)).Path   # <...>/BlueprintReader
if (-not (Test-Path $ProjectFile)) { throw "$tag .uproject not found: $ProjectFile" }
if (-not (Test-Path $EngineDir))   { throw "$tag engine dir not found: $EngineDir" }
$projectDir = (Resolve-Path (Split-Path -Parent $ProjectFile)).Path

# ---- 1. Mount the plugin into <Project>/Plugins/BlueprintReader -------------
$dest = Join-Path $projectDir 'Plugins\BlueprintReader'
$destResolved = if (Test-Path $dest) { (Resolve-Path $dest).Path } else { $dest }
if ($pluginSrc -ieq $destResolved) {
    Write-Host "$tag Plugin already mounted at $dest (source == target) — skipping copy."
} elseif ($Symlink) {
    if (Test-Path $dest) {
        if (-not $Force) { throw "$tag $dest exists; pass -Force to replace it with a symlink." }
        Remove-Item $dest -Recurse -Force
    }
    New-Item -ItemType Directory -Force (Split-Path -Parent $dest) | Out-Null
    New-Item -ItemType SymbolicLink -Path $dest -Target $pluginSrc | Out-Null
    Write-Host "$tag Symlinked plugin -> $dest"
} else {
    New-Item -ItemType Directory -Force $dest | Out-Null
    # Mirror the plugin minus build artifacts / VCS. robocopy exit codes 0-7
    # are success; 8+ is a real failure.
    & robocopy $pluginSrc $dest /MIR /XD `
        (Join-Path $pluginSrc 'Binaries') (Join-Path $pluginSrc 'Intermediate') (Join-Path $pluginSrc '.git') `
        /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "$tag robocopy failed (exit $LASTEXITCODE)" }
    $global:LASTEXITCODE = 0
    Write-Host "$tag Copied plugin -> $dest"
}

# ---- 2. (optional) engine patches — source engines only --------------------
if ($ApplyEnginePatches) {
    Write-Host "$tag Applying engine patches (Patch-Engine.ps1 -Apply)..."
    & (Join-Path $scriptsDir 'Patch-Engine.ps1') -EngineDir $EngineDir -Apply
}

# ---- 3. Build the MCP server (UBT or CMake fallback, auto-detected) --------
if (-not $SkipBuild) {
    Write-Host "$tag Building the MCP server..."
    & (Join-Path $scriptsDir 'Build-MCPServer.ps1') -EngineDir $EngineDir -ProjectFile $ProjectFile
}

# ---- 4. Write the MCP client config ----------------------------------------
Write-Host "$tag Writing MCP client config ($Client)..."
& (Join-Path $scriptsDir 'Generate-ClientConfig.ps1') -Client $Client -ProjectDir $projectDir

# ---- 5. Deploy Claude / AGENTS assets --------------------------------------
Write-Host "$tag Deploying Claude / AGENTS assets..."
& (Join-Path $scriptsDir 'Install-ClaudeAssets.ps1') -ProjectRoot $projectDir

# ---- 6. doctor -------------------------------------------------------------
$exe = Join-Path $projectDir 'Binaries\Win64\BlueprintReaderMcp.exe'
if (Test-Path $exe) {
    Write-Host "$tag Running doctor..."
    & $exe doctor
} else {
    Write-Host "$tag NOTE: server exe not found at $exe — run '$exe doctor' after building."
}
Write-Host "$tag Install complete."
