#requires -Version 7
# Update-Plugin.ps1 - refresh an installed plugin from GitHub WITHOUT building.
#
# Downloads the latest plugin from the GitHub repo (a ZIP archive over HTTPS - no
# git required) into a temp dir, redeploys it over the existing install (preserving
# the built Binaries - this script never compiles anything), then re-runs the
# configure steps (MCP client config + Claude/AGENTS assets + doctor) via
# Install-Plugin.ps1 -SkipBuild.
#
# Self-updating: if the update changed THIS script (or any of the configure
# scripts), the work is performed by the freshly-downloaded copies, so an in-flight
# behaviour change in the update takes effect on this same run. When the downloaded
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
    # Path to the freshly-downloaded <extract>/Plugins/BlueprintReader; when set we
    # skip the download and deploy/configure straight from it.
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
        # ---- Phase 1: download the latest plugin from GitHub as a ZIP ----------
        # No git dependency - just HTTPS. The codeload archive resolves $Ref as a
        # branch, tag, or full SHA and unpacks to a single <repo>-<ref> top folder.
        $tmpClone = Join-Path ([System.IO.Path]::GetTempPath()) ("bpr-update-" + [guid]::NewGuid().ToString('N').Substring(0,12))
        New-Item -ItemType Directory -Force $tmpClone | Out-Null
        $base   = ($Repo -replace '\.git/?$', '').TrimEnd('/')
        $zipUrl = "$base/archive/$Ref.zip"
        $zipPath = Join-Path $tmpClone 'plugin.zip'
        Write-Host "$tag Downloading $zipUrl ..."
        try {
            $oldPref = $ProgressPreference; $ProgressPreference = 'SilentlyContinue'
            Invoke-WebRequest -Uri $zipUrl -OutFile $zipPath -MaximumRedirection 5
            $ProgressPreference = $oldPref
        } catch {
            throw "$tag download failed for $zipUrl : $($_.Exception.Message)"
        }
        Write-Host "$tag Extracting ..."
        Expand-Archive -LiteralPath $zipPath -DestinationPath $tmpClone -Force
        Remove-Item -LiteralPath $zipPath -ErrorAction SilentlyContinue

        # The archive unpacks to one top-level <repo>-<ref> dir; find the one that
        # actually contains the plugin (don't guess the folder name - $Ref may
        # contain '/', which GitHub rewrites to '-').
        $extracted = Get-ChildItem -LiteralPath $tmpClone -Directory |
            Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'Plugins\BlueprintReader\BlueprintReader.uplugin') } |
            Select-Object -First 1
        if (-not $extracted) {
            throw "$tag the downloaded archive doesn't contain Plugins/BlueprintReader - wrong repo/ref ($Ref)?"
        }
        $clonePlugin = Join-Path $extracted.FullName 'Plugins\BlueprintReader'
        $cloneUpdate = Join-Path $clonePlugin 'Scripts\Update-Plugin.ps1'
        if (-not (Test-Path -LiteralPath $cloneUpdate)) {
            throw "$tag the archive is missing Plugins/BlueprintReader/Scripts/Update-Plugin.ps1 - wrong repo/ref?"
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
        Write-Host "$tag Updater unchanged - proceeding with the downloaded plugin."
        $SourceDir = $clonePlugin
    }

    if (-not $SourceDir -or -not (Test-Path -LiteralPath $SourceDir)) {
        throw "$tag internal: no source plugin dir to deploy from."
    }

    # ---- Phase 2: deploy + configure (NO build) ----------------------------
    # Install-Plugin.ps1 -SkipBuild does the mount (two-pass robocopy: a /MIR
    # mirror that holds Binaries\Win64 out of the purge, then an additive copy of
    # the precompiled server payload only when the source carries one). The source
    # archive downloaded in Phase 1 has no Binaries/ (gitignored), so the existing
    # installed BlueprintReaderMcp.exe + fixtures are preserved across the update.
    # Phase 3 below then refreshes the exe from a matching prebuilt release if one
    # exists. It also writes the client config, deploys Claude/AGENTS assets, and
    # runs doctor. Running the downloaded copy means the latest logic is used too.
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

    # ---- Phase 3: prebuilt-exe download + accurate rebuild detection --------
    # Determine the deployed source VersionName (the newly-downloaded .uplugin).
    $destPlugin  = Join-Path $projectDir 'Plugins\BlueprintReader'
    $upluginPath = Join-Path $destPlugin 'BlueprintReader.uplugin'
    $srcVersionName = $null
    if (Test-Path -LiteralPath $upluginPath) {
        try { $srcVersionName = (Get-Content -Raw -LiteralPath $upluginPath | ConvertFrom-Json).VersionName } catch {}
    }

    # Get the running exe's stamped version.
    $exePath   = Join-Path $destPlugin 'Binaries\Win64\BlueprintReaderMcp.exe'
    $exeVersion = $null
    if (Test-Path -LiteralPath $exePath) {
        try {
            $vl = & $exePath --version 2>$null | Select-Object -First 1
            if ($vl -match 'bp-reader-mcp\s+(\S+)') { $exeVersion = $Matches[1] }
        } catch {}
    }

    # Check GitHub releases API for a prebuilt asset matching the new source version.
    $prebuiltSwapped = $false
    if ($srcVersionName) {
        $relTag = "v$srcVersionName"
        try {
            $base     = ($Repo -replace '\.git/?$', '').TrimEnd('/')
            $apiUrl   = "https://api.github.com/repos/$($base -replace 'https://github.com/','')/releases/tags/$relTag"
            # Try direct tag lookup; fall back to /releases/latest (e.g. a -Ref
            # main update where no v<version> tag exists yet).
            try {
                $oldPref = $ProgressPreference; $ProgressPreference = 'SilentlyContinue'
                $resp  = Invoke-WebRequest -Uri $apiUrl -UseBasicParsing `
                             -Headers @{'User-Agent'='bp-reader-update/1.0'} `
                             -MaximumRedirection 3 -TimeoutSec 8 -ErrorAction Stop
                $ProgressPreference = $oldPref
                $release = $resp.Content | ConvertFrom-Json
            } catch {
                try {
                    $latestUrl = "https://api.github.com/repos/$($base -replace 'https://github.com/','')/releases/latest"
                    $oldPref = $ProgressPreference; $ProgressPreference = 'SilentlyContinue'
                    $resp = Invoke-WebRequest -Uri $latestUrl -UseBasicParsing `
                                -Headers @{'User-Agent'='bp-reader-update/1.0'} `
                                -MaximumRedirection 3 -TimeoutSec 8 -ErrorAction Stop
                    $ProgressPreference = $oldPref
                    $release = $resp.Content | ConvertFrom-Json
                } catch {
                    $release = $null
                }
            }
            if ($release -and $release.assets) {
                # The published plugin bundle (BlueprintReader-<tag>-plugin.zip)
                # carries Binaries/Win64/BlueprintReaderMcp.exe; the recursive
                # search below extracts it. Also accept a future server-only
                # *win64*.zip if one is ever published.
                $asset = $release.assets | Where-Object { $_.name -like '*-plugin.zip' -or $_.name -like '*win64*.zip' } | Select-Object -First 1
                if ($asset) {
                    Write-Host "[BlueprintReader/Update] Downloading prebuilt exe from $relTag ..."
                    $zipTmp = Join-Path ([System.IO.Path]::GetTempPath()) "bpr-prebuilt-$([guid]::NewGuid().ToString('N').Substring(0,8)).zip"
                    $binDir = Join-Path $destPlugin 'Binaries\Win64'
                    $oldPref = $ProgressPreference; $ProgressPreference = 'SilentlyContinue'
                    Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $zipTmp `
                        -UseBasicParsing -MaximumRedirection 5 -TimeoutSec 120 -ErrorAction Stop
                    $ProgressPreference = $oldPref
                    # REL-11: verify against the release's SHA256SUMS when it
                    # ships one (releases from 2026-06-12 on). A mismatch means
                    # a corrupted or substituted asset - refuse it. Older
                    # releases without sums skip the check (logged).
                    $sumsAsset = $release.assets | Where-Object { $_.name -eq 'SHA256SUMS' } | Select-Object -First 1
                    if ($sumsAsset) {
                        $sumsTmp = "$zipTmp.sums"
                        Invoke-WebRequest -Uri $sumsAsset.browser_download_url -OutFile $sumsTmp `
                            -UseBasicParsing -MaximumRedirection 5 -TimeoutSec 60 -ErrorAction Stop
                        $expected = $null
                        foreach ($line in (Get-Content -LiteralPath $sumsTmp)) {
                            if ($line -match '^([0-9a-fA-F]{64})\s+(.+)$' -and $Matches[2].Trim() -ieq $asset.name) {
                                $expected = $Matches[1].ToLower(); break
                            }
                        }
                        Remove-Item -LiteralPath $sumsTmp -ErrorAction SilentlyContinue
                        if ($expected) {
                            $actual = (Get-FileHash -LiteralPath $zipTmp -Algorithm SHA256).Hash.ToLower()
                            if ($actual -ne $expected) {
                                Remove-Item -LiteralPath $zipTmp -ErrorAction SilentlyContinue
                                throw "$tag SHA256 mismatch for $($asset.name) (expected $expected, got $actual) - refusing the download."
                            }
                            Write-Host "$tag SHA256 verified for $($asset.name)."
                        } else {
                            Write-Host "$tag SHA256SUMS present but has no entry for $($asset.name); skipping verification."
                        }
                    } else {
                        Write-Host "$tag Release ships no SHA256SUMS (pre-2026-06-12) - skipping checksum verification."
                    }
                    # Extract to a temp dir, verify --version, then move.
                    $unzipTmp = Join-Path ([System.IO.Path]::GetTempPath()) "bpr-prebuilt-$([guid]::NewGuid().ToString('N').Substring(0,8))"
                    Expand-Archive -LiteralPath $zipTmp -DestinationPath $unzipTmp -Force
                    Remove-Item -LiteralPath $zipTmp -ErrorAction SilentlyContinue
                    $candidateExe = Get-ChildItem -Recurse -LiteralPath $unzipTmp -Filter 'BlueprintReaderMcp.exe' | Select-Object -First 1
                    if ($candidateExe) {
                        $newVer = $null
                        try { $vl = & $candidateExe.FullName --version 2>$null | Select-Object -First 1; if ($vl -match 'bp-reader-mcp\s+(\S+)') { $newVer = $Matches[1] } } catch {}
                        if ($newVer) {
                            # REL-12: stop ONLY the server(s) running THIS project's exe —
                            # an unscoped kill took down live MCP sessions of every other
                            # project on the machine. Path-match against the install dir.
                            $targetExe = (Join-Path $binDir 'BlueprintReaderMcp.exe')
                            $running = Get-Process BlueprintReaderMcp -ErrorAction SilentlyContinue |
                                Where-Object { $_.Path -and ($_.Path -ieq $targetExe) }
                            if ($running) {
                                Write-Host "$tag Stopping $(@($running).Count) running server instance(s) for this project to swap the exe."
                                $running | Stop-Process -Force
                                Start-Sleep -Seconds 1
                            }
                            New-Item -ItemType Directory -Force $binDir | Out-Null
                            Copy-Item -LiteralPath $candidateExe.FullName -Destination (Join-Path $binDir 'BlueprintReaderMcp.exe') -Force
                            # Copy fixtures alongside the exe if present.
                            $fixturesSrc = Join-Path (Split-Path -Parent $candidateExe.FullName) 'fixtures'
                            if (Test-Path -LiteralPath $fixturesSrc) {
                                Copy-Item -LiteralPath $fixturesSrc -Destination $binDir -Recurse -Force
                            }
                            $exeVersion      = $newVer
                            $prebuiltSwapped = $true
                            Write-Host "[BlueprintReader/Update] Prebuilt exe installed: $newVer"
                        }
                    }
                    Remove-Item -Recurse -Force -LiteralPath $unzipTmp -ErrorAction SilentlyContinue
                }
            }
        } catch {
            Write-Warning "[BlueprintReader/Update] Could not download prebuilt exe (network/release error): $($_.Exception.Message)"
        }
    }

    # ---- Accurate rebuild detection ----------------------------------------
    if ($prebuiltSwapped) {
        Write-Host "$tag Update complete. Server exe is current ($exeVersion) - no rebuild needed."
    } elseif (-not $exeVersion -or -not (Test-Path -LiteralPath $exePath)) {
        Write-Host "$tag Update complete. Server exe not found - build it with Scripts\Build-MCPServer.bat."
    } else {
        # Compare deployed source VersionName to running exe stamp (strip git hash).
        $exeBase  = ($exeVersion -split '\+')[0] -replace '^v', ''
        $srcBase  = if ($srcVersionName) { $srcVersionName -replace '^v', '' } else { '' }
        if ($srcBase -and $exeBase -ne $srcBase) {
            Write-Host "$tag Update complete. Source changed ($srcBase) vs running exe ($exeBase) - rebuild with Scripts\Build-MCPServer.bat."
        } else {
            Write-Host "$tag Update complete. Server exe is current ($exeVersion)."
        }
    }
    Write-Host "$tag Restart your MCP client (Claude Code / Cursor / VS Code) or reload its MCP servers so it launches the updated server."
    exit 0
}
finally {
    if ($tmpClone -and (Test-Path -LiteralPath $tmpClone)) {
        Remove-Item -Recurse -Force -LiteralPath $tmpClone -ErrorAction SilentlyContinue
    }
}
