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
    [switch] $Clean
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

# --- Source resolution ---
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
        Write-Host "    [dry-run] curl -L $bundleUrl -> $bundlePath"
        Write-Host "    [dry-run] verify SHA-256, extract under $RepoRoot"
    } else {
        & curl --fail --location --silent --show-error --output $bundlePath $bundleUrl
        if ($LASTEXITCODE -ne 0) { throw "curl failed downloading bundle (exit $LASTEXITCODE)" }
        & curl --fail --location --silent --show-error --output $shaPath    $shaUrl
        if ($LASTEXITCODE -ne 0) { throw "curl failed downloading sha256 (exit $LASTEXITCODE)" }

        $expected = (Get-Content -LiteralPath $shaPath -Raw).Split()[0].ToLowerInvariant()
        $actual   = (Get-FileHash -LiteralPath $bundlePath -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($expected -ne $actual) {
            throw "Bundle SHA-256 mismatch. expected=$expected actual=$actual"
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
