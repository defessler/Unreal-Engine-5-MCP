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

# Subtrees that look like Lyra content paths but are regenerable test output —
# excluded from the bundle and from manifest validation. Forward-slash, no
# trailing slash. Anything under these paths is filtered out.
$script:LyraAssetExcludeDirs = @(
    'Content/Recreated'
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
                if ($exempt.Contains($rel)) { return }
                foreach ($ex in $script:LyraAssetExcludeDirs) {
                    if ($rel -like "$ex/*") { return }
                }
                $results.Add($rel)
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

    $raw = Get-Content -LiteralPath $LauncherDatPath -Raw
    try {
        $dat = $raw | ConvertFrom-Json
    } catch {
        throw "Failed to parse LauncherInstalled.dat at $($LauncherDatPath): $($_.Exception.Message)"
    }

    # EGL writes the file as {InstallationList: [...]} but older / unrelated
    # JSON might not have the key. Treat absent / empty as "no installs".
    if (-not $dat.PSObject.Properties['InstallationList']) {
        return @()
    }

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

# Targeted local restore: given a known-good Lyra install root and a list of
# broken (missing or hash-mismatched) repo-relative paths, copy each broken
# file from the install into the repo. Skips entries that don't exist in
# LyraInstallRoot (common when EGL has a partial install or a different Lyra
# version). Returns {Copied, Skipped}.
function Repair-FromLocalInstall {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)] [string]   $LyraInstallRoot,
        [Parameter(Mandatory)] [string]   $RepoRoot,
        [Parameter(Mandatory)] [string[]] $BrokenPaths
    )

    $LyraInstallRoot = (Resolve-Path -LiteralPath $LyraInstallRoot).Path
    $RepoRoot        = (Resolve-Path -LiteralPath $RepoRoot).Path

    $copied  = [System.Collections.Generic.List[string]]::new()
    $skipped = [System.Collections.Generic.List[string]]::new()

    foreach ($rel in $BrokenPaths) {
        $src = Join-Path $LyraInstallRoot $rel
        $dst = Join-Path $RepoRoot $rel
        if (-not (Test-Path -LiteralPath $src)) { $skipped.Add($rel); continue }
        New-Item -ItemType Directory -Path (Split-Path $dst -Parent) -Force | Out-Null
        Copy-Item -LiteralPath $src -Destination $dst -Force
        $copied.Add($rel)
    }

    [pscustomobject][ordered]@{
        Copied  = $copied.ToArray()
        Skipped = $skipped.ToArray()
    }
}

# Inverse of restore — deletes every file listed in the manifest from RepoRoot,
# along with any empty parent directories under the configured glob roots.
# Skips files that don't exist (idempotent). Returns {Deleted, NotPresent}.
function Invoke-LyraAssetCleanup {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)] [string]         $RepoRoot,
        [Parameter(Mandatory)] [PSCustomObject] $Manifest
    )

    if ($Manifest.schema_version -ne $script:ManifestSchemaVersion) {
        throw "Manifest schema_version $($Manifest.schema_version) does not match expected $($script:ManifestSchemaVersion)"
    }

    $RepoRoot   = (Resolve-Path -LiteralPath $RepoRoot).Path
    $deleted    = [System.Collections.Generic.List[string]]::new()
    $notPresent = [System.Collections.Generic.List[string]]::new()

    foreach ($entry in $Manifest.files) {
        $abs = Join-Path $RepoRoot $entry.path
        if (Test-Path -LiteralPath $abs) {
            Remove-Item -LiteralPath $abs -Force
            $deleted.Add($entry.path)
        } else {
            $notPresent.Add($entry.path)
        }
    }

    # Sweep empty dirs under the glob roots, deepest-first.
    foreach ($glob in $script:LyraAssetGlobs) {
        $root = Join-Path $RepoRoot $glob
        if (-not (Test-Path -LiteralPath $root)) { continue }
        Get-ChildItem -LiteralPath $root -Recurse -Directory -ErrorAction SilentlyContinue |
            Sort-Object FullName -Descending |
            ForEach-Object {
                if (-not (Get-ChildItem -LiteralPath $_.FullName -Force -ErrorAction SilentlyContinue)) {
                    Remove-Item -LiteralPath $_.FullName -Force -ErrorAction SilentlyContinue
                }
            }
    }

    [pscustomobject][ordered]@{
        Deleted    = $deleted.ToArray()
        NotPresent = $notPresent.ToArray()
    }
}
