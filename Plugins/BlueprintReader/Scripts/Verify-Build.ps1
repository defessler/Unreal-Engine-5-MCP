# Verify-Build.ps1
# Diagnostic: confirm both halves of the BlueprintReader plugin are present
# and ready to use:
#
#   1. BlueprintReaderMcp.exe                  (built by UBT -- `Build.bat
#                                               BlueprintReaderMcp ...`,
#                                               post-PR-#75)
#   2. UnrealEditor-BlueprintReaderEditor.dll  (built by UBT during an
#                                               editor-target build)
#
# When the daemon "exits before reaching READY" with no obvious Error: line
# in the tail, a missing plugin DLL is the usual cause -- UE finishes plugin
# discovery, doesn't find the BlueprintReader commandlet class, and exits.
#
# Usage:
#   pwsh -File <thisScript>                    # auto-discover Project/Plugin from script location
#   pwsh -File <thisScript> -ProjectDir D:\YourGame\MyProject
#
# Exits 0 if both outputs exist, 1 otherwise -- usable in CI.

param(
    [string]$ProjectDir,
    [string]$PluginDir
)

$ErrorActionPreference = 'Continue'
$tag = '[bp-reader/verify]'

# Resolve defaults from script location.
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $PluginDir)  { $PluginDir  = (Resolve-Path (Join-Path $scriptDir '..')).Path }
if (-not $ProjectDir) { $ProjectDir = (Resolve-Path (Join-Path $PluginDir '..\..')).Path }

function Check {
    param(
        [string]$Label,
        [string]$Path
    )
    $exists = Test-Path -LiteralPath $Path
    if ($exists) {
        Write-Host ('[ OK ] ' + $Label) -ForegroundColor Green
    } else {
        Write-Host ('[MISS] ' + $Label) -ForegroundColor Red
    }
    Write-Host ('       ' + $Path) -ForegroundColor DarkGray
    return $exists
}

Write-Host ''
Write-Host "$tag Verifying BlueprintReader build state" -ForegroundColor Cyan
Write-Host "  ProjectDir : $ProjectDir"
Write-Host "  PluginDir  : $PluginDir"
Write-Host ''

# ---------------------------------------------------------------------------
# Source-side checks (these should always pass on a fresh checkout)
# ---------------------------------------------------------------------------
Write-Host 'Source files:' -ForegroundColor White
$srcOk  = $true
$srcOk  = (Check 'BlueprintReader.uplugin manifest'              (Join-Path $PluginDir 'BlueprintReader.uplugin')) -and $srcOk
$srcOk  = (Check 'BlueprintReaderEditor module Build.cs'          (Join-Path $PluginDir 'Source\BlueprintReaderEditor\BlueprintReaderEditor.Build.cs')) -and $srcOk
$srcOk  = (Check 'MCP server Target.cs (BlueprintReaderMcp)'      (Join-Path $PluginDir 'Tests\BlueprintReaderMcp\BlueprintReaderMcp.Target.cs')) -and $srcOk
$srcOk  = (Check 'MCP core Build.cs (BlueprintReaderMcpCore)'     (Join-Path $PluginDir 'Tests\BlueprintReaderMcpCore\BlueprintReaderMcpCore.Build.cs')) -and $srcOk
$srcOk  = (Check 'Build wrapper script (Build-MCPServer.ps1)'     (Join-Path $PluginDir 'Scripts\Build-MCPServer.ps1')) -and $srcOk
$srcOk  = (Check 'PreBuildSteps hook script (PreBuildHook.ps1)'   (Join-Path $PluginDir 'Scripts\PreBuildHook.ps1')) -and $srcOk
Write-Host ''

# ---------------------------------------------------------------------------
# Freshness checks for in-place plugin copies. Editor-target builds trigger
# the MCP-server build through three pieces that arrived in PR #97 + #98;
# if a user copied the plugin folder from an older version, the editor
# would compile fine but the MCP server would never get built. Flag that
# specifically so the diagnostic is "your plugin copy is stale" not
# "<undifferentiated> failure".
# ---------------------------------------------------------------------------
Write-Host 'Plugin freshness (matters when copying the plugin folder into another project):' -ForegroundColor White
$freshOk = $true

$uplugin = Join-Path $PluginDir 'BlueprintReader.uplugin'
$upluginText = if (Test-Path -LiteralPath $uplugin) { Get-Content -Raw -LiteralPath $uplugin } else { '' }
if ($upluginText -match '"PreBuildSteps"') {
    Write-Host '[ OK ] BlueprintReader.uplugin contains PreBuildSteps block (PR #97)' -ForegroundColor Green
} else {
    Write-Host '[MISS] BlueprintReader.uplugin is missing the PreBuildSteps block (PR #97)' -ForegroundColor Red
    Write-Host '       This is the only mechanism that builds BlueprintReaderMcp.exe as part of' -ForegroundColor DarkGray
    Write-Host '       the editor build. Without it, the editor compiles but the MCP server is never built.' -ForegroundColor DarkGray
    $freshOk = $false
}

$targetCs = Join-Path $PluginDir 'Tests\BlueprintReaderMcp\BlueprintReaderMcp.Target.cs'
$targetCsText = if (Test-Path -LiteralPath $targetCs) { Get-Content -Raw -LiteralPath $targetCs } else { '' }
if ($targetCsText -match 'DefaultBuildSettings\s*=\s*BuildSettingsVersion\.V6') {
    Write-Host '[ OK ] BlueprintReaderMcp.Target.cs sets DefaultBuildSettings = V6 (commit 8e37b72)' -ForegroundColor Green
} else {
    Write-Host '[MISS] BlueprintReaderMcp.Target.cs does not set DefaultBuildSettings = V6' -ForegroundColor Red
    Write-Host '       (commit 8e37b72). Build will print a noisy [Upgrade] block — usually a sign that' -ForegroundColor DarkGray
    Write-Host '       the plugin copy is older than PR #98.' -ForegroundColor DarkGray
    $freshOk = $false
}
Write-Host ''

# ---------------------------------------------------------------------------
# Build-output checks (these are what users actually need)
# ---------------------------------------------------------------------------
Write-Host 'Build outputs:' -ForegroundColor White
# Post-PR-#75: UBT builds BlueprintReaderMcp as a Program target with
# default output at <ProjectDir>/Binaries/Win64/BlueprintReaderMcp.exe.
$mcpExe = Join-Path $ProjectDir 'Binaries\Win64\BlueprintReaderMcp.exe'
$mcpOk  = Check 'BlueprintReaderMcp.exe (MCP server)' $mcpExe

# UE's binary naming: Development is suffix-less, every other config gets
# "-Win64-<Config>" appended. Find ALL variants so we can tell the user
# which configs they've built (and warn if Development is missing -- that's
# the one the daemon defaults to launching).
$binDir = Join-Path $PluginDir 'Binaries\Win64'
$dllVariants = Get-ChildItem -LiteralPath $binDir `
                             -Filter 'UnrealEditor-BlueprintReaderEditor*.dll' `
                             -ErrorAction SilentlyContinue
$dllOk = $false
$haveDevelopment = $false
$haveOtherConfigs = @()
foreach ($v in $dllVariants) {
    $dllOk = $true
    if ($v.Name -eq 'UnrealEditor-BlueprintReaderEditor.dll') {
        $haveDevelopment = $true
        Write-Host ('[ OK ] UnrealEditor-BlueprintReaderEditor.dll (Development)') -ForegroundColor Green
        Write-Host ('       ' + $v.FullName) -ForegroundColor DarkGray
    } else {
        # e.g. UnrealEditor-BlueprintReaderEditor-Win64-DebugGame.dll
        if ($v.Name -match '^UnrealEditor-BlueprintReaderEditor-Win64-(.+)\.dll$') {
            $haveOtherConfigs += $Matches[1]
        }
        Write-Host ('[ OK ] ' + $v.Name) -ForegroundColor Green
        Write-Host ('       ' + $v.FullName) -ForegroundColor DarkGray
    }
}
if (-not $dllOk) {
    Write-Host '[MISS] UnrealEditor-BlueprintReaderEditor*.dll (UE plugin module -- any config)' -ForegroundColor Red
    Write-Host ('       searched: ' + (Join-Path $binDir 'UnrealEditor-BlueprintReaderEditor*.dll')) -ForegroundColor DarkGray
}
Write-Host ''

# Warn if the user only has non-Development variants. The daemon defaults to
# launching UnrealEditor-Cmd.exe (Development) unless BP_READER_EDITOR_CONFIG
# is set; loading would silently skip a non-matching plugin DLL.
if ($dllOk -and -not $haveDevelopment -and $haveOtherConfigs.Count -gt 0) {
    $cfg = $haveOtherConfigs[0]
    Write-Host 'Note: only non-Development DLL variants are present.' -ForegroundColor Yellow
    Write-Host ('  Found: ' + ($haveOtherConfigs -join ', '))
    Write-Host '  The MCP daemon defaults to launching UnrealEditor-Cmd.exe (Development).'
    Write-Host '  Either:'
    Write-Host '    (a) build BlueprintReader in Development, or'
    Write-Host ('    (b) set BP_READER_EDITOR_CONFIG="' + $cfg + '" in your MCP config so the daemon')
    Write-Host ('        launches UnrealEditor-Win64-' + $cfg + '-Cmd.exe instead.')
    Write-Host ''
}

# ---------------------------------------------------------------------------
# Find the .uproject in ProjectDir to give the user concrete fix commands
# ---------------------------------------------------------------------------
$uproject = $null
$editorTarget = '<YourProjectEditor>'
$uprojects = Get-ChildItem -LiteralPath $ProjectDir -Filter '*.uproject' -ErrorAction SilentlyContinue
if ($uprojects -and $uprojects.Count -gt 0) {
    $uproject = $uprojects[0].FullName
    $editorTarget = ($uprojects[0].BaseName) + 'Editor'
}

# ---------------------------------------------------------------------------
# Concrete next steps if anything is missing
# ---------------------------------------------------------------------------
$problems = @()

if (-not $srcOk) {
    Write-Host 'Fix for missing source files:' -ForegroundColor Yellow
    Write-Host '  This means the plugin folder is incomplete. Re-clone or re-copy'
    Write-Host '  the entire Plugins\BlueprintReader\ tree from the upstream repo.'
    Write-Host ''
    $problems += 'source incomplete'
}

if (-not $freshOk) {
    Write-Host 'Fix for stale plugin copy:' -ForegroundColor Yellow
    Write-Host '  The .uplugin and/or Target.cs is older than the current upstream.'
    Write-Host '  Building the editor will succeed, but BlueprintReaderMcp.exe will'
    Write-Host '  not be produced as a side effect (the auto-build hook is missing).'
    Write-Host ''
    Write-Host '  Re-copy the entire Plugins\BlueprintReader\ folder from the upstream'
    Write-Host '  repo, then regenerate project files (right-click the .uproject ->'
    Write-Host '  ''Generate Visual Studio project files'') so UBT picks up the new'
    Write-Host '  PreBuildSteps and re-emits its cached PreBuild-N.bat files.'
    Write-Host ''
    $problems += 'plugin copy stale'
}

if (-not $mcpOk) {
    Write-Host 'Fix for missing BlueprintReaderMcp.exe:' -ForegroundColor Yellow
    Write-Host '  The MCP server is its own UBT Program target (no longer a'
    Write-Host '  PreBuildStep of the editor build). Build it explicitly:'
    Write-Host ''
    Write-Host '    & "<EngineDir>\Engine\Build\BatchFiles\Build.bat" `'
    Write-Host '        BlueprintReaderMcp Win64 Development `'
    if ($uproject) {
        Write-Host ('        -project="{0}"' -f $uproject)
    } else {
        Write-Host '        -project="<path to your .uproject>"'
    }
    Write-Host ''
    Write-Host '  Or use the wrapper script (builds Mcp + Tests):'
    Write-Host ('    & "{0}\Scripts\Build-MCPServer.ps1" -EngineDir "<engine>" -ProjectFile "<your.uproject>"' -f $PluginDir)
    Write-Host ''
    $problems += 'MCP server not built'
}

if (-not $dllOk) {
    Write-Host 'Fix for missing UnrealEditor-BlueprintReaderEditor.dll:' -ForegroundColor Yellow
    Write-Host '  Build the editor target so UBT compiles the plugin module.'
    Write-Host '  From your engine directory:'
    Write-Host ''
    Write-Host '    & "<EngineDir>\Engine\Build\BatchFiles\Build.bat" `'
    Write-Host ('        {0} Win64 Development `' -f $editorTarget)
    if ($uproject) {
        Write-Host ('        -project="{0}" `' -f $uproject)
    } else {
        Write-Host '        -project="<path to your .uproject>" `'
    }
    Write-Host '        -waitmutex'
    Write-Host ''
    Write-Host '  Add -NoUba -MaxParallelActions=4 if your Windows page file is small.'
    Write-Host ''
    Write-Host '  After UBT finishes, re-run this verifier.'
    Write-Host ''
    $problems += 'UE plugin DLL not built'
}

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
if ($problems.Count -eq 0) {
    Write-Host "$tag All checks passed -- plugin is ready to use." -ForegroundColor Green
    exit 0
}

Write-Host ("$tag FAIL -- " + ($problems -join '; ')) -ForegroundColor Red
exit 1
