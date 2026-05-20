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
    throw 'not implemented'
}

function Get-FileManifestEntries {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)] [string]   $RepoRoot,
        [Parameter(Mandatory)] [string[]] $RelativePaths
    )
    throw 'not implemented'
}

function Build-Manifest {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)] [string] $RepoRoot,
        [Parameter(Mandatory)] [string] $Tag
    )
    throw 'not implemented'
}

function Test-Manifest {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)] [string]         $RepoRoot,
        [Parameter(Mandatory)] [PSCustomObject] $Manifest
    )
    throw 'not implemented'
}

function Find-LyraInstallPaths {
    [CmdletBinding()]
    param([Parameter(Mandatory)] [string] $LauncherDatPath)
    throw 'not implemented'
}

function Get-RestorePathMap {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)] [string] $LyraInstallRoot,
        [Parameter(Mandatory)] [string] $RepoRoot
    )
    throw 'not implemented'
}
