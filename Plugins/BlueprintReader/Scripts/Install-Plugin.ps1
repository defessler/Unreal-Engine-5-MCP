#requires -Version 7
# Install-Plugin.ps1 - one-shot installer (INSTALL-M1).
#
# Collapses the ~8-step manual setup into a single command: mount the plugin
# into a UE project, build the MCP server (Build-MCPServer.ps1 auto-picks UBT
# for a source engine or the CMake fallback for an installed one, INSTALL-M2),
# write the MCP client config, deploy the Claude/AGENTS assets, and finish with
# `doctor`. All sub-steps are existing scripts - this is the glue.
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
    # Both inferred from the canonical in-project layout (plugin at
    # <Project>/Plugins/BlueprintReader) when omitted -- see _Common.ps1.
    [string]$EngineDir,
    [string]$ProjectFile,
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

# Infer omitted paths from the in-project layout (plugin at <Project>/Plugins/BlueprintReader).
. (Join-Path $scriptsDir '_Common.ps1')
if (-not $ProjectFile) {
    $ProjectFile = Resolve-BprProjectFile (Resolve-BprProjectDir $scriptsDir)
    if ($ProjectFile) { Write-Host "$tag Inferred -ProjectFile: $ProjectFile" }
    else { throw "$tag could not infer a .uproject - pass -ProjectFile (or place the plugin under <Project>/Plugins/)." }
}
if (-not $EngineDir) {
    $EngineDir = Resolve-BprEngineDir $ProjectFile
    if ($EngineDir) { Write-Host "$tag Inferred -EngineDir: $EngineDir" }
    else { throw "$tag no UE engine found (checked the .uproject EngineAssociation, the HKLM/HKCU registry, and the common Launcher dirs) - pass -EngineDir `"<...>\UE_5.8`" or set BP_READER_ENGINE_DIR." }
}

if (-not (Test-Path $ProjectFile)) { throw "$tag .uproject not found: $ProjectFile" }
if (-not (Test-Path $EngineDir))   { throw "$tag engine dir not found: $EngineDir" }
$projectDir = (Resolve-Path (Split-Path -Parent $ProjectFile)).Path

# ---- 1. Mount the plugin into <Project>/Plugins/BlueprintReader -------------
$dest = Join-Path $projectDir 'Plugins\BlueprintReader'
$destResolved = if (Test-Path $dest) { (Resolve-Path $dest).Path } else { $dest }
if ($pluginSrc -ieq $destResolved) {
    Write-Host "$tag Plugin already mounted at $dest (source == target) - skipping copy."
} elseif ($Symlink) {
    if (Test-Path $dest) {
        if (-not $Force) { throw "$tag $dest exists; pass -Force to replace it with a symlink." }
        Remove-Item $dest -Recurse -Force
    }
    New-Item -ItemType Directory -Force (Split-Path -Parent $dest) | Out-Null
    New-Item -ItemType SymbolicLink -Path $dest -Target $pluginSrc | Out-Null
    Write-Host "$tag Symlinked plugin -> $dest"
} else {
    # REL-10: a plain (non-junction) git checkout living at the destination
    # would be silently MIRRORED OVER by the /MIR below — uncommitted local
    # changes gone. Junction installs are guarded upstream (Update-Plugin
    # skips redeploy); guard the copied-checkout case here the same way.
    if ((Test-Path (Join-Path $dest '.git')) -and
        -not ((Get-Item -LiteralPath $dest -ErrorAction SilentlyContinue).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw ("$tag $dest is a git checkout (.git present) - refusing to mirror over it " +
               "(uncommitted changes would be lost). 'git pull' that checkout instead, " +
               "or remove .git if it is not a working copy.")
    }
    New-Item -ItemType Directory -Force $dest | Out-Null
    # Two-pass mount so a /MIR mirror never PURGES an already-installed
    # precompiled server when the SOURCE lacks it. (The Update/Setup-from-source
    # path downloads the GitHub *source* archive, which has no Binaries/ — it's
    # gitignored — so a single /MIR that included Binaries\Win64 would delete the
    # user's working BlueprintReaderMcp.exe + fixtures and defeat the no-build
    # default.) robocopy exit codes 0-7 are success; 8+ is a real failure.
    #
    # Pass 1: mirror the tree but leave the whole Binaries tree alone. We exclude
    #   it by BOTH paths on purpose: the SOURCE path stops Pass 1 from copying in
    #   the source's Binaries (so source editor DLLs / the test exe never leak in
    #   — Pass 2 copies just the server payload), and the DEST path stops /MIR
    #   from PURGING the dest's existing Binaries when the source lacks one (the
    #   Update-from-source case: the GitHub source archive has no Binaries/ — it's
    #   gitignored — so without the dest-path exclusion /MIR would delete the
    #   user's working exe + fixtures + locally-built editor DLLs). robocopy /XD
    #   only protects a dest dir from purge when matched by its DEST path.
    & robocopy $pluginSrc $dest /MIR `
        /XD (Join-Path $pluginSrc 'Binaries') (Join-Path $dest 'Binaries') `
            (Join-Path $pluginSrc 'Intermediate') (Join-Path $pluginSrc '.git') `
        /R:2 /W:2 /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "$tag robocopy failed (exit $LASTEXITCODE)" }
    # Pass 2: additively copy the precompiled server payload from source IF it
    #   carries one (a release plugin ZIP does; a source archive does not — then
    #   the dest's existing exe is left untouched). NO /MIR = no purge. Exclude
    #   engine/machine-specific editor binaries, the dev-only test exe, and the
    #   transpile-dump scratch dir so the install stays lean.
    $srcBin = Join-Path $pluginSrc 'Binaries\Win64'
    if (Test-Path -LiteralPath $srcBin) {
        # Free a running server before overwriting its exe so the copy can't block
        # (and a live client picks up the new build on its next launch). Scope the
        # kill to THIS project's exe path only — never other projects' sessions.
        $targetExe = Join-Path $dest 'Binaries\Win64\BlueprintReaderMcp.exe'
        $holding = Get-Process BlueprintReaderMcp -ErrorAction SilentlyContinue |
            Where-Object { $_.Path -and ($_.Path -ieq $targetExe) }
        if ($holding) {
            Write-Host "$tag Stopping $(@($holding).Count) running server instance(s) for this project to swap the exe."
            $holding | Stop-Process -Force
            Start-Sleep -Seconds 1
        }
        & robocopy $srcBin (Join-Path $dest 'Binaries\Win64') /E `
            /XD "transpile-dump" `
            /XF "UnrealEditor-*.dll" "BlueprintReaderMcpTests.exe" "*.pdb" "*.modules" "*.lib" "*.exp" "*.ilk" `
            /R:2 /W:2 /NFL /NDL /NJH /NJS /NP | Out-Null
        if ($LASTEXITCODE -ge 8) { throw "$tag robocopy (server payload) failed (exit $LASTEXITCODE). If BlueprintReaderMcp.exe is in use, stop your MCP client (or the Toolbox) and retry." }
    }
    $global:LASTEXITCODE = 0
    Write-Host "$tag Copied plugin -> $dest"
}

# ---- 1b. Clear stale hook-era UBT PreBuild caches ---------------------------
# Older plugin versions shipped a .uplugin PreBuildSteps hook (PreBuildHook.ps1,
# now removed) that UBT cached as Intermediate/Build/**/PreBuild-N.bat. A stale
# one still invokes the deleted hook and fails the next editor build (exit 127).
# Drop any that reference the old hook so UBT re-emits a hook-free action graph.
$imBuild = Join-Path $projectDir 'Intermediate\Build'
if (Test-Path $imBuild) {
    Get-ChildItem -LiteralPath $imBuild -Recurse -Filter 'PreBuild-*.bat' -ErrorAction SilentlyContinue |
        Where-Object { (Get-Content -Raw -LiteralPath $_.FullName -ErrorAction SilentlyContinue) -match 'PreBuildHook' } |
        ForEach-Object {
            Write-Host "$tag Removing stale hook-era PreBuild cache: $($_.FullName)"
            Remove-Item -LiteralPath $_.FullName -Force -ErrorAction SilentlyContinue
        }
}

# ---- 2. (optional) engine patches - source engines only --------------------
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
$exe = Join-Path $projectDir 'Plugins\BlueprintReader\Binaries\Win64\BlueprintReaderMcp.exe'
if (-not (Test-Path $exe)) {
    $legacyExe = Join-Path $projectDir 'Binaries\Win64\BlueprintReaderMcp.exe'  # pre-plugin-Binaries location
    if (Test-Path $legacyExe) { $exe = $legacyExe }
}
if (Test-Path $exe) {
    Write-Host "$tag Running doctor (advisory health check)..."
    & $exe doctor
    # doctor is informational: it flags project-side setup nits (engine not
    # registered, plugin not yet enabled in the .uproject, etc.) that are NOT
    # install failures. The mount/config/asset steps above throw on real errors.
    # Don't let doctor's exit code become this script's exit code.
} else {
    Write-Host "$tag NOTE: server exe not found at $exe - run '$exe doctor' after building."
}
Write-Host "$tag Install complete."
exit 0
