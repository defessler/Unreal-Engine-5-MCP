# Install-ClaudeAssets.ps1
#
# Copies the bp-reader plugin's Claude skills + agents into the parent
# project's `.claude/` directory so Claude Code (and compatible
# clients) discover them. Idempotent -- running it again overwrites the
# target with the current plugin contents.
#
# Source of truth: Plugins/BlueprintReader/Claude/
# Deploy target:   <project-root>/.claude/
#
# Typical workflow:
#   1. Drop / update Plugins/BlueprintReader/ in your UE project.
#   2. Run this script once: refreshes <project>/.claude/skills and
#      <project>/.claude/agents from the plugin.
#   3. Restart Claude Code to pick up the new skill manifests.
#
# Usage:
#   pwsh Plugins/BlueprintReader/Scripts/Install-ClaudeAssets.ps1
#   pwsh Plugins/BlueprintReader/Scripts/Install-ClaudeAssets.ps1 -DryRun
#   pwsh Plugins/BlueprintReader/Scripts/Install-ClaudeAssets.ps1 -ProjectRoot C:\Path\To\Project

[CmdletBinding()]
param(
    # Override the auto-detected project root. By default the script
    # walks up from its own location until it finds a *.uproject (or
    # the project's existing .claude/) and installs there.
    [string]$ProjectRoot,

    # Print what would copy without doing anything.
    [switch]$DryRun,

    # Force overwrite even if .claude/ contains files the script
    # wouldn't normally touch. (We only manage bp-* skills + the
    # bp-audit agent -- anything else in .claude/ is preserved by
    # default.)
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

# ---- Locate source + target -----------------------------------------

$pluginRoot = Split-Path -Parent $PSScriptRoot
$claudeSrc  = Join-Path $pluginRoot 'Claude'
if (-not (Test-Path $claudeSrc -PathType Container)) {
    throw "Plugin Claude/ folder not found at $claudeSrc. Are you running this from a valid bp-reader plugin install?"
}

if (-not $ProjectRoot) {
    # Walk up from $pluginRoot looking for a sibling *.uproject. Plugin
    # layout: <project>/Plugins/BlueprintReader/, so two levels up
    # should hit the project root.
    $candidate = $pluginRoot
    for ($i = 0; $i -lt 5; $i++) {
        $candidate = Split-Path -Parent $candidate
        if (-not $candidate) { break }
        $uproject = Get-ChildItem -Path $candidate -Filter '*.uproject' -File -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($uproject) {
            $ProjectRoot = $candidate
            break
        }
    }
    if (-not $ProjectRoot) {
        throw 'Could not auto-detect project root (no .uproject found walking up from the plugin). Pass -ProjectRoot <path> explicitly.'
    }
}

if (-not (Test-Path $ProjectRoot -PathType Container)) {
    throw "Project root '$ProjectRoot' does not exist."
}

$claudeDst = Join-Path $ProjectRoot '.claude'

Write-Host "bp-reader Claude assets installer" -ForegroundColor Cyan
Write-Host "  Source:  $claudeSrc"
Write-Host "  Target:  $claudeDst"
if ($DryRun) { Write-Host '  DryRun:  yes (no files will change)' -ForegroundColor Yellow }

# ---- Plan the copy --------------------------------------------------

# Each entry: { source, target, kind }. We only manage skills/bp-* and
# agents/bp-* -- leaving any other content under .claude/ alone.
$plan = New-Object System.Collections.Generic.List[object]

$skillSrc = Join-Path $claudeSrc 'skills'
if (Test-Path $skillSrc) {
    Get-ChildItem -Path $skillSrc -Directory | ForEach-Object {
        $plan.Add([pscustomobject]@{
            Source = $_.FullName
            Target = Join-Path (Join-Path $claudeDst 'skills') $_.Name
            Kind   = 'skill'
        })
    }
}

$agentSrc = Join-Path $claudeSrc 'agents'
if (Test-Path $agentSrc) {
    Get-ChildItem -Path $agentSrc -File -Filter '*.md' | ForEach-Object {
        $plan.Add([pscustomobject]@{
            Source = $_.FullName
            Target = Join-Path (Join-Path $claudeDst 'agents') $_.Name
            Kind   = 'agent'
        })
    }
}

if ($plan.Count -eq 0) {
    Write-Warning 'Nothing to install -- Claude/ folder is empty.'
    return
}

# ---- Execute --------------------------------------------------------

foreach ($item in $plan) {
    $action = if (Test-Path $item.Target) { 'UPDATE' } else { 'CREATE' }
    Write-Host ("  {0,-6} {1}: {2}" -f $action, $item.Kind, $item.Target)

    if ($DryRun) { continue }

    # Ensure parent directory exists.
    $parent = Split-Path -Parent $item.Target
    if (-not (Test-Path $parent)) {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }

    if ($item.Kind -eq 'skill') {
        # Directory copy -- wipe target dir first so removed source
        # files don't linger.
        if (Test-Path $item.Target) {
            Remove-Item $item.Target -Recurse -Force
        }
        Copy-Item -Path $item.Source -Destination $item.Target -Recurse
    }
    else {
        # File copy.
        Copy-Item -Path $item.Source -Destination $item.Target -Force
    }
}

if ($DryRun) {
    Write-Host "Dry run complete. Re-run without -DryRun to apply." -ForegroundColor Yellow
}
else {
    Write-Host "Installed $($plan.Count) item(s). Restart Claude Code to pick up the new skill manifests." -ForegroundColor Green
}
