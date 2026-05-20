# Scripts/setup.ps1
# Restore Lyra assets that are not tracked in this repo. Run after a
# fresh clone, or any time the asset working tree was cleaned.
#
# Default flow:
#   1. Try to locate a Lyra Starter Game install via Epic Games
#      Launcher's LauncherInstalled.dat.
#   2. If found, robocopy /MIR Content/ and Plugins/*/Content/ from
#      that install into this repo.
#   3. Otherwise, download the bundle from this repo's GitHub
#      Release tag (default: lyra-assets-v1), verify SHA-256, extract.
#   4. Validate every restored file against the committed manifest
#      (Scripts/lyra-assets-manifest.json). Warn on hash mismatches
#      (UE auto-upgrades older asset versions on load).

[CmdletBinding()]
param(
    [ValidateSet('auto','local','release')]
    [string] $Source = 'auto',
    [string] $ReleaseTag = 'lyra-assets-v1',
    [string] $RepoOwner  = 'defessler',
    [string] $RepoName   = 'Unreal-Engine-5-MCP',
    [switch] $Force,
    [switch] $DryRun,
    [switch] $VerifyOnly,
    [switch] $Clean,
    [switch] $Repair
)

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'

. "$PSScriptRoot/LyraAssetCommon.ps1"

$RepoRoot     = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$ManifestPath = Join-Path $PSScriptRoot 'lyra-assets-manifest.json'

function Write-Step ([string] $Msg) { Write-Host "==> $Msg" -ForegroundColor Cyan }
function Write-Warn ([string] $Msg) { Write-Host "    warning: $Msg" -ForegroundColor Yellow }

Write-Step "Repo root: $RepoRoot"

if (-not (Test-Path -LiteralPath $ManifestPath)) {
    throw "Manifest missing at $ManifestPath. This script must be run from a clone of the repo, after the manifest has been committed."
}
$manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json

# --- Verify-only mode: just check current working tree against manifest ---
if ($VerifyOnly) {
    Write-Step 'Verifying current working tree against manifest'
    $r = Test-Manifest -RepoRoot $RepoRoot -Manifest $manifest
    if ($r.Ok) {
        Write-Host "  $($manifest.total_files) files present and hashes match." -ForegroundColor Green
        exit 0
    }
    Write-Warn "$($r.Missing.Count) missing, $($r.Mismatch.Count) mismatched"
    foreach ($m in ($r.Missing  | Select-Object -First 5)) { Write-Host "    missing:  $m" }
    foreach ($m in ($r.Mismatch | Select-Object -First 5)) { Write-Host "    mismatch: $m" }
    exit 1
}

# --- Clean mode: inverse of restore. Delete every manifest file from the
# working tree, leaving exemptions / tracked test BPs in place. Useful for
# checking what a fresh-clone-without-setup state looks like, or before
# packaging the project elsewhere.
if ($Clean) {
    $totalMb = [math]::Round($manifest.total_bytes / 1MB, 1)
    Write-Step "Cleaning $($manifest.total_files) manifest files (~$totalMb MB) from $RepoRoot"
    if ($DryRun) {
        Write-Host "    [dry-run] would Remove-Item every path in the manifest, then sweep empty dirs"
        exit 0
    }
    $r = Invoke-LyraAssetCleanup -RepoRoot $RepoRoot -Manifest $manifest
    Write-Host ''
    Write-Step "Deleted $($r.Deleted.Count) files; $($r.NotPresent.Count) already absent."
    exit 0
}

# --- Source resolution (shared by restore + repair) ---
$selected = $Source
$lyraRoot = $null

if ($Source -in @('auto','local')) {
    $datPath = Join-Path $env:ProgramData 'Epic\UnrealEngineLauncher\LauncherInstalled.dat'
    if (Test-Path -LiteralPath $datPath) {
        try {
            $installs = @(Find-LyraInstallPaths -LauncherDatPath $datPath)
            if ($installs.Count -gt 0) {
                $lyraRoot = $installs[0]
                $selected = 'local'
                Write-Step "Found local Lyra install: $lyraRoot"
            }
        } catch { Write-Warn "Could not parse LauncherInstalled.dat: $($_.Exception.Message)" }
    } else {
        Write-Warn "LauncherInstalled.dat not found at $datPath"
    }
    if (-not $lyraRoot -and $Source -eq 'local') {
        throw 'No local Lyra install found. Re-run with -Source release.'
    }
    if (-not $lyraRoot) { $selected = 'release' }
}

# --- Repair mode: targeted restore of just missing/mismatched files.
# Cheaper than a full re-run after a partial failure or version-skew patch.
if ($Repair) {
    Write-Step 'Identifying broken files via manifest check'
    $check = Test-Manifest -RepoRoot $RepoRoot -Manifest $manifest
    if ($check.Ok) {
        Write-Host "  Nothing to repair — manifest verifies clean." -ForegroundColor Green
        exit 0
    }
    $broken = @() + $check.Missing + $check.Mismatch
    Write-Host "    $($check.Missing.Count) missing + $($check.Mismatch.Count) mismatched = $($broken.Count) files to repair"

    if ($DryRun) {
        Write-Host "    [dry-run] would repair from source=$selected; first 5 paths:"
        foreach ($p in ($broken | Select-Object -First 5)) { Write-Host "      $p" }
        exit 0
    }

    if ($selected -eq 'local') {
        Write-Step "Copying $($broken.Count) files from local Lyra install"
        $copied = 0; $skipped = 0
        foreach ($rel in $broken) {
            $src = Join-Path $lyraRoot $rel
            $dst = Join-Path $RepoRoot $rel
            if (-not (Test-Path -LiteralPath $src)) { $skipped++; continue }
            New-Item -ItemType Directory -Path (Split-Path $dst -Parent) -Force | Out-Null
            Copy-Item -LiteralPath $src -Destination $dst -Force
            $copied++
        }
        Write-Host "    copied: $copied, skipped (not in Lyra install): $skipped"
    } else {
        # Release path: download (or reuse cached) bundle, extract just the
        # broken paths via tar -T <file-list>. Avoids re-extracting 8,686
        # files when the broken set is small.
        $bundleName = "${ReleaseTag}.tar.zst"
        $bundleUrl  = "https://github.com/$RepoOwner/$RepoName/releases/download/$ReleaseTag/$bundleName"
        $shaUrl     = "$bundleUrl.sha256"
        $bundlePath = Join-Path $env:TEMP $bundleName
        $shaPath    = "$bundlePath.sha256"

        Write-Step "Resolving bundle for selective extract"
        & curl --fail --location --silent --show-error --output $shaPath $shaUrl
        if ($LASTEXITCODE -ne 0) { throw "curl failed downloading sha256 (exit $LASTEXITCODE)" }
        $expected = (Get-Content -LiteralPath $shaPath -Raw).Split()[0].ToLowerInvariant()

        $needsDownload = $true
        if (Test-Path -LiteralPath $bundlePath) {
            $h = (Get-FileHash -LiteralPath $bundlePath -Algorithm SHA256).Hash.ToLowerInvariant()
            if ($h -eq $expected) { $needsDownload = $false; Write-Host "    reusing cached bundle (hash matches)" }
        }
        if ($needsDownload) {
            & curl --fail --location --show-error --continue-at - --output $bundlePath $bundleUrl
            if ($LASTEXITCODE -ne 0) { throw "curl failed downloading bundle (exit $LASTEXITCODE)" }
            $actual = (Get-FileHash -LiteralPath $bundlePath -Algorithm SHA256).Hash.ToLowerInvariant()
            if ($expected -ne $actual) {
                Remove-Item -LiteralPath $bundlePath -ErrorAction SilentlyContinue
                throw "Bundle SHA-256 mismatch. Stale partial removed; re-run setup.bat -Repair."
            }
        }

        Write-Step "Extracting $($broken.Count) files from bundle"
        $listFile = Join-Path $env:TEMP "lyra-repair-$([guid]::NewGuid()).txt"
        $broken | Set-Content -LiteralPath $listFile -Encoding utf8
        try {
            & tar -x --zstd -f $bundlePath -C $RepoRoot -T $listFile
            if ($LASTEXITCODE -ne 0) { throw "tar extract failed (exit $LASTEXITCODE)" }
        } finally { Remove-Item -LiteralPath $listFile -ErrorAction SilentlyContinue }
    }

    Write-Step 'Re-verifying after repair'
    $after = Test-Manifest -RepoRoot $RepoRoot -Manifest $manifest
    if ($after.Ok) {
        Write-Host "  Repair complete — $($manifest.total_files) files present and hashes match." -ForegroundColor Green
        exit 0
    }
    Write-Warn "$($after.Missing.Count) still missing, $($after.Mismatch.Count) still mismatched after repair"
    foreach ($m in ($after.Missing  | Select-Object -First 5)) { Write-Host "    missing:  $m" }
    foreach ($m in ($after.Mismatch | Select-Object -First 5)) { Write-Host "    mismatch: $m" }
    exit 1
}

# --- Pre-flight: refuse if there are uncommitted asset changes ---
if (-not $Force -and -not $DryRun) {
    Push-Location $RepoRoot
    try {
        $dirty = git status --porcelain -- Content Plugins/LyraExampleContent/Content Plugins/LyraExtTool/Content Plugins/PocketWorlds/Content Plugins/GameFeatures 2>$null
        if ($dirty) {
            Write-Host "Uncommitted changes detected under restore-target paths:" -ForegroundColor Red
            Write-Host $dirty
            throw 'Refusing to overwrite local work. Re-run with -Force to bypass.'
        }
    } finally { Pop-Location }
}

# --- Local restore ---
if ($selected -eq 'local') {
    Write-Step 'Mirroring asset dirs from local Lyra install'
    $map = Get-RestorePathMap -LyraInstallRoot $lyraRoot -RepoRoot $RepoRoot
    foreach ($glob in $script:LyraAssetGlobs) {
        $src = $map[$glob]
        $dst = $map["${glob}_dst"]
        if (-not (Test-Path -LiteralPath $src)) {
            Write-Warn "Source missing in Lyra install: $src (skipping)"
            continue
        }
        if ($DryRun) {
            Write-Host "    [dry-run] robocopy /MIR $src -> $dst"
            continue
        }
        New-Item -ItemType Directory -Path $dst -Force | Out-Null
        # /E = recursive copy (including empty dirs); /XO = skip if dest is newer.
        # NOT /MIR: that would /PURGE files in dest that aren't in src, which would
        # delete Content/AI/BP_TestEnemy.uasset (tracked, not in Lyra) and
        # Content/Recreated/.gitkeep (tracked test-output marker).
        & robocopy $src $dst /E /XO /NFL /NDL /NJH /NJS /NP /R:2 /W:5 /MT:8 | Out-Null
        # robocopy exit codes 0-7 = success (0=no changes, 1-3=copied, 4-7=warnings)
        if ($LASTEXITCODE -ge 8) {
            throw "robocopy failed copying $src -> $dst (exit $LASTEXITCODE)"
        }
    }
}

# --- Release restore ---
if ($selected -eq 'release') {
    Write-Step "Downloading bundle from GitHub Release: $ReleaseTag"
    $bundleName = "${ReleaseTag}.tar.zst"
    $bundleUrl  = "https://github.com/$RepoOwner/$RepoName/releases/download/$ReleaseTag/$bundleName"
    $shaUrl     = "$bundleUrl.sha256"
    $bundlePath = Join-Path $env:TEMP $bundleName
    $shaPath    = "$bundlePath.sha256"

    if ($DryRun) {
        Write-Host "    [dry-run] curl -L --continue-at - $bundleUrl -> $bundlePath"
        Write-Host "    [dry-run] verify SHA-256, extract under $RepoRoot"
    } else {
        # Fetch sha256 sidecar first (small, cheap). If a stale partial bundle
        # already exists in TEMP and its hash matches, skip the bundle download
        # entirely. Otherwise resume the bundle download (--continue-at -)
        # so a flaky network doesn't force a 1.7 GB restart.
        & curl --fail --location --silent --show-error --output $shaPath $shaUrl
        if ($LASTEXITCODE -ne 0) { throw "curl failed downloading sha256 (exit $LASTEXITCODE)" }
        $expected = (Get-Content -LiteralPath $shaPath -Raw).Split()[0].ToLowerInvariant()

        $skipDownload = $false
        if (Test-Path -LiteralPath $bundlePath) {
            $existingHash = (Get-FileHash -LiteralPath $bundlePath -Algorithm SHA256).Hash.ToLowerInvariant()
            if ($existingHash -eq $expected) {
                Write-Step "Reusing already-downloaded bundle at $bundlePath (hash matches)"
                $skipDownload = $true
            }
        }
        if (-not $skipDownload) {
            & curl --fail --location --show-error --continue-at - --output $bundlePath $bundleUrl
            if ($LASTEXITCODE -ne 0) { throw "curl failed downloading bundle (exit $LASTEXITCODE)" }
        }

        $actual = (Get-FileHash -LiteralPath $bundlePath -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($expected -ne $actual) {
            # Stale partial download from a different release? Delete and retry.
            Remove-Item -LiteralPath $bundlePath -ErrorAction SilentlyContinue
            throw "Bundle SHA-256 mismatch. expected=$expected actual=$actual. Stale partial download removed; re-run setup.bat."
        }
        Write-Step "Extracting bundle into $RepoRoot"
        & tar -x --zstd -f $bundlePath -C $RepoRoot
        if ($LASTEXITCODE -ne 0) { throw "tar extract failed (exit $LASTEXITCODE)" }
        Remove-Item -LiteralPath $bundlePath -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $shaPath    -ErrorAction SilentlyContinue
    }
}

# --- Verification (skipped in dry-run) ---
if (-not $DryRun) {
    Write-Step 'Verifying restored files against manifest'
    $r = Test-Manifest -RepoRoot $RepoRoot -Manifest $manifest
    if ($r.Missing.Count -gt 0) {
        Write-Warn "$($r.Missing.Count) files still missing after restore:"
        foreach ($m in ($r.Missing | Select-Object -First 5)) { Write-Host "    $m" }
        if ($r.Missing.Count -gt 5) { Write-Host "    ... and $($r.Missing.Count - 5) more" }
    }
    if ($r.Mismatch.Count -gt 0) {
        Write-Warn "$($r.Mismatch.Count) files have different hashes than the bundled manifest"
        Write-Host '    (this is normal when restoring from a local Lyra install with a different patch version)'
    }
    $totalMb = [math]::Round($manifest.total_bytes / 1MB, 1)
    Write-Host ''
    Write-Step "Restored $($manifest.total_files - $r.Missing.Count)/$($manifest.total_files) assets ($totalMb MB expected). Source: $selected."
}

exit 0
