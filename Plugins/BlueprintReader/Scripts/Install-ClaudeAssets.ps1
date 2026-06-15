# Install-ClaudeAssets.ps1
#
# Copies the bp-reader plugin's AI-assistant assets into the parent project
# at the locations each assistant family auto-discovers. Idempotent -- running
# it again refreshes the targets from the current plugin contents.
#
# Source of truth (all inside the plugin, so they travel with it):
#   Plugins/BlueprintReader/Claude/skills + agents   (Claude Code)
#   Plugins/BlueprintReader/AGENTS.md                (cross-agent + Copilot)
# Deploy targets:
#   <project-root>/.claude/skills, .claude/agents    -> Claude Code
#   <project-root>/AGENTS.md                          -> Codex / Cursor / Aider / Jules
#   <project-root>/.github/copilot-instructions.md    -> GitHub Copilot
# The AGENTS.md / copilot targets are section-merged: our guidance lives
# between <!-- blueprintreader:start --> / <!-- blueprintreader:end -->
# delimiters, so any content the consumer adds to those files is preserved
# across re-runs -- only the delimited block is refreshed.
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
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'

# ---- Section-level merge for shared guidance files ------------------
# AGENTS.md and .github/copilot-instructions.md are shared with the
# consumer -- they may add their own content. Rather than overwrite
# (loses their edits) or skip (leaves our guidance stale), we wrap our
# content in HTML-comment delimiters and only ever touch what's between
# them:
#     <!-- blueprintreader:start -->
#     ...plugin guidance...
#     <!-- blueprintreader:end -->
# Cases:
#   * No file               -> create with just our block
#   * Has our delimiters     -> replace the inner block in place
#   * Pre-delimiter file we wrote (legacy marker present, no delimiters)
#                            -> replace wholesale (one-time migration)
#   * Consumer-owned file (no marker) -> append our block, keep theirs
# Honors $DryRun (read from the script scope).
#
# UTF-8 (no BOM) I/O. Windows PowerShell 5.1's default Get-Content/Set-Content
# read as ANSI and write a BOM, which silently corrupted the deployed AGENTS.md
# / copilot-instructions.md (em-dashes -> mojibake, leading BOM) when this script
# ran under 5.1 instead of pwsh 7. Go through .NET so the bytes are identical
# UTF-8-no-BOM on BOTH shells.
$script:Utf8NoBom = New-Object System.Text.UTF8Encoding($false)
function Read-BprText([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return $null }
    return [System.IO.File]::ReadAllText($Path)   # BOM-aware; no-BOM treated as UTF-8
}
function Write-BprText([string]$Path, [string]$Text) {
    [System.IO.File]::WriteAllText($Path, $Text, $script:Utf8NoBom)
}

function Merge-BprSection {
    param(
        [Parameter(Mandatory)] [string]$TargetPath,
        [Parameter(Mandatory)] [AllowEmptyString()] [string]$SourceContent,
        [string]$Kind = 'file',
        [string]$LegacyMarker = 'BlueprintReader MCP'
    )

    $startTag = '<!-- blueprintreader:start -->'
    $endTag   = '<!-- blueprintreader:end -->'
    $block    = "$startTag`n$SourceContent`n$endTag"

    if (-not (Test-Path $TargetPath)) {
        Write-Host ("  {0,-7} {1}: {2}" -f 'CREATE', $Kind, $TargetPath)
        if (-not $DryRun) {
            $parent = Split-Path -Parent $TargetPath
            if ($parent -and -not (Test-Path $parent)) {
                New-Item -ItemType Directory -Path $parent -Force | Out-Null
            }
            Write-BprText $TargetPath ($block + "`n")
        }
        return
    }

    $existing = Read-BprText $TargetPath
    if ($null -eq $existing) { $existing = '' }

    $startIdx = $existing.IndexOf($startTag)
    $endIdx   = $existing.IndexOf($endTag)

    if ($startIdx -ge 0 -and $endIdx -gt $startIdx) {
        # Delimited block present -- replace only the inner block (string
        # splice, not regex, so $ and \ in the content are never special).
        $before  = $existing.Substring(0, $startIdx)
        $after   = $existing.Substring($endIdx + $endTag.Length)
        $updated = $before + $block + $after
        $verb    = 'UPDATE'
    }
    elseif ($LegacyMarker -and $existing.Contains($LegacyMarker)) {
        # A file we wrote before delimiters existed -- replace wholesale so
        # we don't leave a stale undelimited copy beside the new block.
        $updated = $block
        $verb    = 'MIGRATE'
    }
    else {
        # Consumer-owned file -- append our block, leave their content intact.
        $updated = $existing.TrimEnd() + "`n`n" + $block + "`n"
        $verb    = 'APPEND'
    }

    if ($updated -eq $existing) {
        Write-Host ("  {0,-7} {1}: {2}" -f 'OK', $Kind, $TargetPath)
        return
    }
    Write-Host ("  {0,-7} {1}: {2}" -f $verb, $Kind, $TargetPath)
    if (-not $DryRun) {
        Write-BprText $TargetPath ($updated + "`n")
    }
}

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

# Cross-agent + Copilot guidance: ONE portable source (the plugin's AGENTS.md)
# deployed to the locations each assistant family auto-discovers:
#   * <root>/AGENTS.md                      -> Codex / Cursor / Aider / Jules
#   * <root>/.github/copilot-instructions.md -> GitHub Copilot
# Same content, single source, no drift. (Claude reads the richer skills under
# .claude/.) The plugin's own AGENTS.md is ALSO natively discovered by agents
# working inside Plugins/BlueprintReader/ - no deploy needed for subtree work.
# These are section-merged (Kind='merge'): a consumer's own content in the
# file is preserved; only our delimited block is created/refreshed. See
# Merge-BprSection.
$agentsSrc = Join-Path $pluginRoot 'AGENTS.md'
if (Test-Path $agentsSrc) {
    $plan.Add([pscustomobject]@{
        Source = $agentsSrc
        Target = Join-Path $ProjectRoot 'AGENTS.md'
        Kind   = 'merge'
    })
    $plan.Add([pscustomobject]@{
        Source = $agentsSrc
        Target = Join-Path (Join-Path $ProjectRoot '.github') 'copilot-instructions.md'
        Kind   = 'merge'
    })
}

if ($plan.Count -eq 0) {
    Write-Warning 'Nothing to install -- Claude/ folder is empty.'
    return
}

# ---- Execute --------------------------------------------------------

foreach ($item in $plan) {
    # Shared guidance files are section-merged so consumer edits survive.
    if ($item.Kind -eq 'merge') {
        $sourceContent = (Read-BprText $item.Source).TrimEnd()
        Merge-BprSection -TargetPath $item.Target -SourceContent $sourceContent -Kind $item.Kind
        continue
    }

    $action = if (Test-Path $item.Target) { 'UPDATE' } else { 'CREATE' }
    Write-Host ("  {0,-7} {1}: {2}" -f $action, $item.Kind, $item.Target)

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

# ---- Reconcile: prune stale plugin-managed assets -------------------
# The plugin owns the bp-* namespace under .claude/. If an update removed a
# skill or agent, drop the deployed copy so the project mirrors the current
# plugin. Bounded to bp-* so consumer-authored .claude/ content is untouched.
$srcSkillNames = @()
if (Test-Path $skillSrc) { $srcSkillNames = @((Get-ChildItem -Path $skillSrc -Directory).Name) }
$srcAgentNames = @()
if (Test-Path $agentSrc) { $srcAgentNames = @((Get-ChildItem -Path $agentSrc -File -Filter '*.md').Name) }

$deployedSkills = Join-Path $claudeDst 'skills'
if (Test-Path $deployedSkills) {
    Get-ChildItem -Path $deployedSkills -Directory -Filter 'bp-*' |
        Where-Object { $_.Name -notin $srcSkillNames } | ForEach-Object {
            Write-Host ("  PRUNE  skill: {0} (no longer in the plugin)" -f $_.FullName) -ForegroundColor Yellow
            if (-not $DryRun) { Remove-Item -LiteralPath $_.FullName -Recurse -Force }
        }
}
$deployedAgents = Join-Path $claudeDst 'agents'
if (Test-Path $deployedAgents) {
    Get-ChildItem -Path $deployedAgents -File -Filter 'bp-*.md' |
        Where-Object { $_.Name -notin $srcAgentNames } | ForEach-Object {
            Write-Host ("  PRUNE  agent: {0} (no longer in the plugin)" -f $_.FullName) -ForegroundColor Yellow
            if (-not $DryRun) { Remove-Item -LiteralPath $_.FullName -Force }
        }
}

if ($DryRun) {
    Write-Host "Dry run complete. Re-run without -DryRun to apply." -ForegroundColor Yellow
}
else {
    Write-Host "Installed $($plan.Count) item(s). Restart Claude Code to pick up the new skill manifests." -ForegroundColor Green
}
