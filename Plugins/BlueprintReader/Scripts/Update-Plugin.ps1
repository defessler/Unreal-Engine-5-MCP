#requires -Version 7
# Update-Plugin.ps1 - refresh an installed plugin from GitHub WITHOUT building.
#
# Pulls the latest plugin from the GitHub repo into a temp clone, redeploys it
# over the existing install (preserving the built Binaries - this script never
# compiles anything), then re-runs the configure steps (MCP client config +
# Claude/AGENTS assets + doctor) via Install-Plugin.ps1 -SkipBuild.
#
# Self-updating: if the update changed THIS script (or any of the configure
# scripts), the work is performed by the freshly-cloned copies, so an in-flight
# behaviour change in the update takes effect on this same run. When the cloned
# Update-Plugin.ps1 differs from the running one, the run hands off to the new
# version (one re-exec, guarded so it can't loop).
#
# Building is intentionally out of scope. After updating, rebuild the server
# if its sources changed (Build-MCPServer.bat) or drop in a prebuilt release exe.
#
# Usage (zero-arg from the in-project location):
#   pwsh Plugins/BlueprintReader/Scripts/Update-Plugin.ps1
#   Update-Plugin.bat -Client All
#   Update-Plugin.bat -ProjectFile "D:\Game\MyGame.uproject" -Ref main

param(
    # Inferred from the canonical in-project layout (plugin at
    # <Project>/Plugins/BlueprintReader) when omitted -- see _Common.ps1.
    [string]$ProjectFile,
    # Git ref to pull (branch or tag).
    [string]$Ref = 'main',
    # Source repo. Defaults to the plugin's own OSS repo; override to track a fork.
    [string]$Repo = 'https://github.com/defessler/Unreal-Engine-5-MCP.git',
    [ValidateSet('ClaudeCode','Cursor','VSCode','Gemini','Codex','All')]
    [string]$Client = 'ClaudeCode',
    [switch]$Force,
    # --- internal (set on the self-update hand-off) ---
    # Path to the freshly-cloned <clone>/Plugins/BlueprintReader; when set we skip
    # the clone and deploy/configure straight from it.
    [string]$SourceDir,
    [switch]$PostUpdate
)
$ErrorActionPreference = 'Stop'
$tag = '[BlueprintReader/Update]'

$scriptsDir = Split-Path -Parent $PSCommandPath
. (Join-Path $scriptsDir '_Common.ps1')

# Resolve the target project (where the install lives) the same way the other
# launchers do. On the -PostUpdate hand-off it's passed in explicitly.
if (-not $ProjectFile) {
    $ProjectFile = Resolve-BprProjectFile (Resolve-BprProjectDir $scriptsDir)
    if ($ProjectFile) { Write-Host "$tag Inferred -ProjectFile: $ProjectFile" }
    else { throw "$tag could not infer a .uproject - pass -ProjectFile (or run from <Project>/Plugins/BlueprintReader/Scripts)." }
}
if (-not (Test-Path -LiteralPath $ProjectFile)) { throw "$tag .uproject not found: $ProjectFile" }

$tmpClone = $null
try {
    if (-not $PostUpdate) {
        # ---- Phase 1: fetch the latest plugin from GitHub into a temp clone ----
        if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
            throw "$tag git is required to pull the latest plugin but was not found on PATH."
        }
        $tmpClone = Join-Path ([System.IO.Path]::GetTempPath()) ("bpr-update-" + [guid]::NewGuid().ToString('N').Substring(0,12))
        Write-Host "$tag Cloning $Repo ($Ref) ..."
        & git clone --depth 1 --branch $Ref $Repo $tmpClone 2>&1 | ForEach-Object { Write-Host "  $_" }
        if ($LASTEXITCODE -ne 0) { throw "$tag git clone failed (exit $LASTEXITCODE)." }

        $clonePlugin = Join-Path $tmpClone 'Plugins\BlueprintReader'
        $cloneUpdate = Join-Path $clonePlugin 'Scripts\Update-Plugin.ps1'
        if (-not (Test-Path -LiteralPath $cloneUpdate)) {
            throw "$tag the clone is missing Plugins/BlueprintReader/Scripts/Update-Plugin.ps1 - wrong repo/ref?"
        }

        # ---- Self-update: if the updater itself changed, hand off to the new one ----
        # Compare line-ending-normalized content so a CRLF/LF difference (git
        # autocrlf, editor settings) doesn't trigger a spurious re-exec every run.
        $hashOf = {
            param($p)
            $text  = ([System.IO.File]::ReadAllText($p)) -replace "`r`n", "`n"
            $bytes = [System.Text.Encoding]::UTF8.GetBytes($text)
            [System.BitConverter]::ToString([System.Security.Cryptography.SHA256]::HashData($bytes))
        }
        $runHash   = & $hashOf $PSCommandPath
        $cloneHash = & $hashOf $cloneUpdate
        if ($runHash -ne $cloneHash) {
            Write-Host "$tag Update-Plugin.ps1 changed upstream - re-running the new version..."
            # Native-exe arg array: each element becomes one argv token, which the
            # re-exec'd script then parses as its own named params.
            $cli = @('-NoProfile','-ExecutionPolicy','Bypass','-File', $cloneUpdate,
                     '-PostUpdate','-SourceDir', $clonePlugin,
                     '-ProjectFile', $ProjectFile, '-Client', $Client)
            if ($Force) { $cli += '-Force' }
            & pwsh.exe @cli
            exit $LASTEXITCODE   # finally{} cleans up $tmpClone
        }
        Write-Host "$tag Updater unchanged - proceeding with the freshly-cloned plugin."
        $SourceDir = $clonePlugin
    }

    if (-not $SourceDir -or -not (Test-Path -LiteralPath $SourceDir)) {
        throw "$tag internal: no source plugin dir to deploy from."
    }

    # ---- Phase 2: deploy + configure (NO build) ----------------------------
    # Install-Plugin.ps1 -SkipBuild does the mount (robocopy /MIR, which excludes
    # Binaries/Intermediate/.git so the built server survives) then writes the
    # client config, deploys the Claude/AGENTS assets, and runs doctor. Running
    # the CLONE's copy means the latest configure logic is used too.
    $projectDir = (Resolve-Path (Split-Path -Parent $ProjectFile)).Path
    $destPlugin = Join-Path $projectDir 'Plugins\BlueprintReader'
    if ((Test-Path -LiteralPath $destPlugin) -and
        ((Get-Item -LiteralPath $destPlugin).Attributes -band [System.IO.FileAttributes]::ReparsePoint)) {
        Write-Warning "$tag $destPlugin is a symlink/junction (a -Symlink install). Skipping redeploy so your source checkout isn't clobbered - 'git pull' that checkout instead. Reconfiguring only."
        $installSrc = $destPlugin   # configure against the existing (symlinked) plugin
    } else {
        $installSrc = $SourceDir
    }

    $installScript = Join-Path $installSrc 'Scripts\Install-Plugin.ps1'
    if (-not (Test-Path -LiteralPath $installScript)) { throw "$tag missing $installScript" }
    Write-Host "$tag Deploying + configuring (no build) ..."
    # Explicit named params (array splatting binds positionally and mis-binds these).
    if ($Force) {
        & $installScript -SkipBuild -ProjectFile $ProjectFile -Client $Client -Force
    } else {
        & $installScript -SkipBuild -ProjectFile $ProjectFile -Client $Client
    }
    $code = $LASTEXITCODE
    if ($code -ne 0) { throw "$tag configure step failed (exit $code)." }
    Write-Host "$tag Update complete. If the server sources changed, rebuild with Build-MCPServer.bat or drop in a prebuilt release exe."
    exit 0
}
finally {
    if ($tmpClone -and (Test-Path -LiteralPath $tmpClone)) {
        Remove-Item -Recurse -Force -LiteralPath $tmpClone -ErrorAction SilentlyContinue
    }
}
