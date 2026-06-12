#requires -Version 7
# Check-Update.ps1 - check whether a newer version of the BlueprintReader plugin
# is available on GitHub and write a cache so doctor / the server can surface the
# notice without making a network call on every startup.
#
# Compares the running server exe's reported version (bp-reader-mcp --version)
# against the latest GitHub release tag.  Falls back to the .uplugin VersionName
# when the exe is not available.  Silent on network failure (offline = no-op).
#
# Cache: <ProjectDir>/Saved/bp-reader-update.json
#   {checked_iso, latest_tag, current, update_available}
#
# Usage (zero-arg from in-project location):
#   pwsh Plugins/BlueprintReader/Scripts/Check-Update.ps1
#   Check-Update.bat
#
# Exit codes:
#   0  - check succeeded (update_available may be true or false)
#   1  - unrecoverable local error (missing project dir, bad args)
#   2  - update available (set when -ExitCodeOnUpdate is passed)

[CmdletBinding()]
param(
    # Path to the server exe; inferred from the plugin Binaries if omitted.
    [string]$ExePath,
    # The .uproject file; inferred from the canonical in-project layout if omitted.
    [string]$ProjectFile,
    # GitHub repo slug (owner/name). Override to track a fork.
    [string]$Repo = 'defessler/Unreal-Engine-5-MCP',
    # Return exit code 2 when an update is available (useful for CI / scripted checks).
    [switch]$ExitCodeOnUpdate,
    # Print the cache file path and exit (useful for doctor to find the cache).
    [switch]$PrintCachePath
)
$ErrorActionPreference = 'Stop'
$tag = '[BlueprintReader/CheckUpdate]'
$scriptsDir = Split-Path -Parent $PSCommandPath
. (Join-Path $scriptsDir '_Common.ps1')

# Resolve the project file so we can locate the Saved/ dir and the .uplugin.
if (-not $ProjectFile) {
    $ProjectFile = Resolve-BprProjectFile (Resolve-BprProjectDir $scriptsDir)
}
$projectDir = if ($ProjectFile -and (Test-Path -LiteralPath $ProjectFile)) {
    (Resolve-Path (Split-Path -Parent $ProjectFile)).Path
} else {
    # Fallback: use the directory three levels up (Scripts->BlueprintReader->Plugins->Project)
    Resolve-BprProjectDir $scriptsDir
}

$savedDir   = Join-Path $projectDir 'Saved'
$cacheFile  = Join-Path $savedDir   'bp-reader-update.json'
$pluginRoot = Split-Path -Parent $scriptsDir

if ($PrintCachePath) { Write-Output $cacheFile; exit 0 }

# ---- Determine the current version ----------------------------------------
$currentVersion = $null

# Prefer the running exe's self-reported version.
if (-not $ExePath) {
    $ExePath = Join-Path $pluginRoot 'Binaries\Win64\BlueprintReaderMcp.exe'
}
if (Test-Path -LiteralPath $ExePath) {
    try {
        $verLine = & $ExePath --version 2>$null | Select-Object -First 1
        # Format: "bp-reader-mcp 0.1.0+abc1234"
        if ($verLine -match 'bp-reader-mcp\s+(\S+)') {
            $currentVersion = $Matches[1]
        }
    } catch {}
}

# Fall back to the .uplugin VersionName.
if (-not $currentVersion) {
    $upluginPath = Join-Path $pluginRoot 'BlueprintReader.uplugin'
    if (Test-Path -LiteralPath $upluginPath) {
        try {
            $vn = (Get-Content -Raw -LiteralPath $upluginPath | ConvertFrom-Json).VersionName
            if ($vn) { $currentVersion = $vn }
        } catch {}
    }
}
if (-not $currentVersion) { $currentVersion = 'unknown' }

# ---- Throttle: skip if we checked within the last hour --------------------
if (Test-Path -LiteralPath $cacheFile) {
    try {
        $cache = Get-Content -Raw -LiteralPath $cacheFile | ConvertFrom-Json
        if ($cache.checked_iso) {
            $checkedAt  = [datetime]::Parse($cache.checked_iso)
            $hoursSince = ([datetime]::UtcNow - $checkedAt).TotalHours
            if ($hoursSince -lt 1) {
                # Fresh enough: print the cached verdict and exit.
                if ($cache.update_available) {
                    Write-Host "$tag Update available: $($cache.latest_tag) (current: $currentVersion) — run Setup-Plugin.bat"
                    if ($ExitCodeOnUpdate) { exit 2 }
                }
                exit 0
            }
        }
    } catch {}
}

# ---- Query GitHub releases API ---------------------------------------------
$latestTag = $null
$updateAvailable = $false
try {
    $apiUrl   = "https://api.github.com/repos/$Repo/releases/latest"
    $oldPref  = $ProgressPreference; $ProgressPreference = 'SilentlyContinue'
    $response = Invoke-WebRequest -Uri $apiUrl -UseBasicParsing `
                    -Headers @{ 'User-Agent' = 'bp-reader-check-update/1.0' } `
                    -MaximumRedirection 3 -TimeoutSec 8 -ErrorAction Stop
    $ProgressPreference = $oldPref
    $data = $response.Content | ConvertFrom-Json
    $latestTag = $data.tag_name
    if ($latestTag) {
        # Strip leading 'v' for comparison.
        $latestVer  = $latestTag -replace '^v', ''
        $currentVer = ($currentVersion -split '\+')[0] -replace '^v', ''
        # Version comparison: try as [version], fall back to string.
        try {
            $updateAvailable = [version]$latestVer -gt [version]$currentVer
        } catch {
            $updateAvailable = $latestVer -ne $currentVer -and $latestVer -gt $currentVer
        }
    }
} catch {
    # Network failure, rate-limit, etc. — silent, just don't update the cache.
    exit 0
}

# ---- Write the cache -------------------------------------------------------
if (-not (Test-Path -LiteralPath $savedDir)) {
    New-Item -ItemType Directory -Force $savedDir | Out-Null
}
# REL-3: temp + rename so a crash mid-write can't leave a truncated cache
# (doctor reads this file verbatim).
$cacheTmp = "$cacheFile.tmp.$PID"
@{
    checked_iso      = [datetime]::UtcNow.ToString('o')
    latest_tag       = $latestTag
    current          = $currentVersion
    update_available = $updateAvailable
} | ConvertTo-Json -Compress | Set-Content -LiteralPath $cacheTmp -Encoding UTF8
Move-Item -LiteralPath $cacheTmp -Destination $cacheFile -Force

# ---- Report ----------------------------------------------------------------
if ($updateAvailable) {
    Write-Host "$tag Update available: $latestTag (current: $currentVersion) — run Setup-Plugin.bat to update"
    if ($ExitCodeOnUpdate) { exit 2 }
} else {
    Write-Host "$tag Plugin is up to date ($currentVersion)"
}
exit 0
