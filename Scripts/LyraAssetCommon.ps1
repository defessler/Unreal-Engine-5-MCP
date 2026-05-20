# Scripts/LyraAssetCommon.ps1
# Shared pure functions for Lyra asset manifest building, install detection,
# and verification. Dot-sourced by setup.ps1 and Publish-LyraAssetsRelease.ps1.

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'

# Bumped when manifest schema changes. setup.ps1 refuses to operate on a
# manifest with a different schema_version.
$script:ManifestSchemaVersion = 1

# Globs (relative to RepoRoot) whose .uasset/.umap contents are considered
# "Lyra assets" and removed by setup.bat / restored by setup.bat. Two
# exemptions: Content/AI/BP_TestEnemy.uasset and BP_TestPickup.uasset stay
# tracked because UBPRSeedCommandlet produces them and live tests assert on
# /Game/AI/BP_TestEnemy and /Game/AI/BP_TestPickup by literal path.
$script:LyraAssetGlobs = @(
    'Content',
    'Plugins/LyraExampleContent/Content',
    'Plugins/LyraExtTool/Content',
    'Plugins/PocketWorlds/Content',
    'Plugins/GameFeatures/ShooterCore/Content',
    'Plugins/GameFeatures/ShooterTests/Content',
    'Plugins/GameFeatures/TopDownArena/Content',
    'Plugins/GameFeatures/ShooterMaps/Content',
    'Plugins/GameFeatures/ShooterExplorer/Content'
)

$script:LyraAssetExemptions = @(
    'Content/AI/BP_TestEnemy.uasset',
    'Content/AI/BP_TestPickup.uasset'
)

function Get-LyraAssetPaths {
    [CmdletBinding()]
    param([Parameter(Mandatory)] [string] $RepoRoot)

    $RepoRoot = (Resolve-Path -LiteralPath $RepoRoot).Path
    $results  = [System.Collections.Generic.List[string]]::new()
    $exempt   = [System.Collections.Generic.HashSet[string]]::new(
        [string[]]$script:LyraAssetExemptions,
        [System.StringComparer]::OrdinalIgnoreCase)

    foreach ($glob in $script:LyraAssetGlobs) {
        $absDir = Join-Path $RepoRoot $glob
        if (-not (Test-Path -LiteralPath $absDir)) { continue }
        # Filter by extension after the fact — Get-ChildItem -Include + -Recurse is
        # slower than a single -Recurse + Where-Object on large trees.
        Get-ChildItem -LiteralPath $absDir -Recurse -File -ErrorAction SilentlyContinue |
            Where-Object { $_.Extension -in '.uasset','.umap' } |
            ForEach-Object {
                $rel = $_.FullName.Substring($RepoRoot.Length).TrimStart('\','/').Replace('\','/')
                if (-not $exempt.Contains($rel)) { $results.Add($rel) }
            }
    }

    return $results.ToArray()
}

function Get-FileManifestEntries {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)] [string]   $RepoRoot,
        [Parameter(Mandatory)] [string[]] $RelativePaths
    )

    $RepoRoot = (Resolve-Path -LiteralPath $RepoRoot).Path
    foreach ($rel in $RelativePaths) {
        $abs  = Join-Path $RepoRoot $rel
        $info = Get-Item -LiteralPath $abs
        $hash = (Get-FileHash -LiteralPath $abs -Algorithm SHA256).Hash.ToLowerInvariant()
        [pscustomobject][ordered]@{
            path   = $rel.Replace('\','/')
            size   = [int64]$info.Length
            sha256 = $hash
        }
    }
}

function Build-Manifest {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)] [string] $RepoRoot,
        [Parameter(Mandatory)] [string] $Tag
    )

    $paths   = Get-LyraAssetPaths -RepoRoot $RepoRoot
    $sorted  = @($paths | Sort-Object -CaseSensitive:$false)
    $entries = @()
    if ($sorted.Count -gt 0) {
        $entries = @(Get-FileManifestEntries -RepoRoot $RepoRoot -RelativePaths $sorted)
    }
    # Manual sum: Measure-Object -Sum on empty pipeline returns a result with
    # no Sum property under StrictMode 3.0.
    $totalBytes = [int64]0
    foreach ($e in $entries) { $totalBytes += [int64]$e.size }

    [pscustomobject][ordered]@{
        schema_version = $script:ManifestSchemaVersion
        tag            = $Tag
        generated_at   = (Get-Date).ToUniversalTime().ToString('o')
        total_files    = $entries.Count
        total_bytes    = [int64]$totalBytes
        files          = $entries
    }
}

function Test-Manifest {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)] [string]         $RepoRoot,
        [Parameter(Mandatory)] [PSCustomObject] $Manifest
    )

    if ($Manifest.schema_version -ne $script:ManifestSchemaVersion) {
        throw "Manifest schema_version $($Manifest.schema_version) does not match expected $($script:ManifestSchemaVersion)"
    }

    $missing  = [System.Collections.Generic.List[string]]::new()
    $mismatch = [System.Collections.Generic.List[string]]::new()
    $RepoRoot = (Resolve-Path -LiteralPath $RepoRoot).Path

    foreach ($entry in $Manifest.files) {
        $abs = Join-Path $RepoRoot $entry.path
        if (-not (Test-Path -LiteralPath $abs)) {
            $missing.Add($entry.path)
            continue
        }
        $actual = (Get-FileHash -LiteralPath $abs -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actual -ne $entry.sha256) {
            $mismatch.Add($entry.path)
        }
    }

    [pscustomobject][ordered]@{
        Ok       = ($missing.Count -eq 0 -and $mismatch.Count -eq 0)
        Missing  = $missing.ToArray()
        Mismatch = $mismatch.ToArray()
    }
}

function Find-LyraInstallPaths {
    [CmdletBinding()]
    param([Parameter(Mandatory)] [string] $LauncherDatPath)

    if (-not (Test-Path -LiteralPath $LauncherDatPath)) {
        throw "LauncherInstalled.dat not found at: $LauncherDatPath"
    }

    $dat = Get-Content -LiteralPath $LauncherDatPath -Raw | ConvertFrom-Json
    $hits = [System.Collections.Generic.List[string]]::new()

    foreach ($entry in @($dat.InstallationList)) {
        if (-not $entry.PSObject.Properties['InstallLocation']) { continue }
        $loc = $entry.InstallLocation
        if (-not $loc) { continue }
        $uproject = Join-Path $loc 'LyraStarterGame.uproject'
        if (Test-Path -LiteralPath $uproject) {
            $hits.Add($loc)
        }
    }

    return $hits.ToArray()
}

function Get-RestorePathMap {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)] [string] $LyraInstallRoot,
        [Parameter(Mandatory)] [string] $RepoRoot
    )

    $map = [ordered]@{}
    foreach ($glob in $script:LyraAssetGlobs) {
        $map[$glob]         = (Join-Path $LyraInstallRoot $glob).Replace('\','/')
        $map["${glob}_dst"] = (Join-Path $RepoRoot        $glob).Replace('\','/')
    }
    return $map
}
