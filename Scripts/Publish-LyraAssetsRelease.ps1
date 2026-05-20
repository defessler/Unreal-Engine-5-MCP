# Scripts/Publish-LyraAssetsRelease.ps1
# Operator script — must run on a checkout that still has the Lyra
# assets present. Builds Scripts/lyra-assets-manifest.json, packs the
# bundle as <Tag>.tar.zst at repo root, writes a .sha256 sidecar, and
# (with -Upload) publishes them as a GitHub Release.

[CmdletBinding()]
param(
    [string] $Tag = 'lyra-assets-v1',
    [switch] $BuildManifestOnly,
    [switch] $BundleOnly,
    [switch] $Upload,
    [switch] $DryRun
)

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'

. "$PSScriptRoot/LyraAssetCommon.ps1"

$RepoRoot     = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$ManifestPath = Join-Path $PSScriptRoot 'lyra-assets-manifest.json'
$BundleName   = "${Tag}.tar.zst"
$BundlePath   = Join-Path $RepoRoot $BundleName
$ShaPath      = "$BundlePath.sha256"

function Write-Step ([string] $Msg) { Write-Host "==> $Msg" -ForegroundColor Cyan }

# --- 1. Manifest ---
Write-Step "Building manifest for tag $Tag"
$manifest = Build-Manifest -RepoRoot $RepoRoot -Tag $Tag
$totalMb  = [math]::Round($manifest.total_bytes / 1MB, 1)
Write-Host "    files: $($manifest.total_files)"
Write-Host "    bytes: $($manifest.total_bytes) ($totalMb MB)"

if ($manifest.total_files -lt 1000) {
    throw "Refusing to publish: only $($manifest.total_files) files found. Likely run on a stripped checkout."
}

if ($DryRun) {
    Write-Host "    [dry-run] would write $ManifestPath"
} else {
    $manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $ManifestPath -Encoding utf8
    Write-Host "    wrote: $ManifestPath"
}

if ($BuildManifestOnly) { exit 0 }

# --- 2. Bundle ---
Write-Step "Packing bundle $BundleName (tar --zstd -19)"
$listPath = Join-Path $env:TEMP "lyra-paths-$([guid]::NewGuid()).txt"
$manifest.files | ForEach-Object { $_.path } | Set-Content -LiteralPath $listPath -Encoding utf8

if ($DryRun) {
    Write-Host "    [dry-run] tar -c --zstd -f $BundlePath -T $listPath -C $RepoRoot"
} else {
    Push-Location $RepoRoot
    try {
        $env:ZSTD_CLEVEL = '19'
        & tar -c --zstd -f $BundlePath -T $listPath
        if ($LASTEXITCODE -ne 0) { throw "tar failed (exit $LASTEXITCODE)" }
    } finally {
        $env:ZSTD_CLEVEL = $null
        Pop-Location
        Remove-Item -LiteralPath $listPath -ErrorAction SilentlyContinue
    }
    $bundleMb = [math]::Round((Get-Item $BundlePath).Length / 1MB, 1)
    Write-Host "    bundle: $BundlePath ($bundleMb MB)"
}

# --- 3. Hash ---
Write-Step "Hashing bundle"
if ($DryRun) {
    Write-Host "    [dry-run] write $ShaPath"
} else {
    $hash = (Get-FileHash -LiteralPath $BundlePath -Algorithm SHA256).Hash.ToLowerInvariant()
    "$hash  $BundleName" | Set-Content -LiteralPath $ShaPath -Encoding ascii
    Write-Host "    sha256: $hash"
}

if ($BundleOnly) { exit 0 }

# --- 4. Upload ---
if (-not $Upload) {
    Write-Host ''
    Write-Step "Bundle ready. Re-run with -Upload to publish to GitHub Release '$Tag'."
    exit 0
}

Write-Step "Publishing GitHub Release $Tag"
$notes = "Lyra asset bundle. $($manifest.total_files) files, $totalMb MB. Restore with setup.bat."
if ($DryRun) {
    Write-Host "    [dry-run] gh release create $Tag --title '$Tag' --notes '$notes' $BundlePath $ShaPath $ManifestPath"
} else {
    & gh release create $Tag --title $Tag --notes $notes $BundlePath $ShaPath $ManifestPath
    if ($LASTEXITCODE -ne 0) { throw "gh release create failed (exit $LASTEXITCODE)" }
}

Write-Host ''
Write-Step "Done."
