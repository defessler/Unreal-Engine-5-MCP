# Patch-Engine.ps1
#
# Applies the three engine `.Build.cs` patches the BlueprintReader plugin
# depends on. The 5.7.4 GitHub engine source ships three modules whose
# `PrivateIncludePaths` is declared relative to `Engine/Source/` instead
# of relative to the module dir. That breaks project-target builds with
# `fatal error C1083: Cannot open include file`. The fix in each case is:
#
#   1. Add `using System.IO;` near the top (if missing).
#   2. Replace the relative-string `PrivateIncludePaths.Add(...)` with a
#      `Path.Combine(ModuleDirectory, ...)` form.
#
# The three files:
#   - Engine/Source/Developer/Windows/LiveCoding/LiveCoding.Build.cs
#   - Engine/Source/Developer/IOS/TVOSTargetPlatformSettings/TVOSTargetPlatformSettings.Build.cs
#   - Engine/Platforms/VisionOS/Source/Developer/VisionOSTargetPlatformSettings/VisionOSTargetPlatformSettings.Build.cs
#
# Usage:
#   # Dry-run: report findings, change nothing.
#   .\Patch-Engine.ps1 -EngineDir "D:\Projects\Unreal Engine 5"
#
#   # Apply the patches:
#   .\Patch-Engine.ps1 -EngineDir "D:\Projects\Unreal Engine 5" -Apply
#
# Idempotent -- re-running on already-patched files is a no-op. Each patch
# is detected by presence of the relative-string form; if `Path.Combine`
# is already there, the file is left alone.
#
# Exit codes:
#   0 -- no patches needed OR all patches applied successfully
#   1 -- engine dir invalid / files missing
#   2 -- patches needed but -Apply was not passed (dry-run with pending work)
#   3 -- patch application failed (file write error, unexpected file shape)

[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)] [string]$EngineDir,
    [switch]$Apply
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $EngineDir -PathType Container)) {
    Write-Error "Engine dir not found: $EngineDir"
    exit 1
}

# Each patch entry:
#   File: path relative to $EngineDir
#   Find: regex pattern indicating the UNPATCHED form
#   Replace: replacement substring (Path.Combine form)
#   NeedsUsingSystemIO: true if `using System.IO;` insertion is also needed
$patches = @(
    @{
        Name    = "LiveCoding"
        File    = "Engine\Source\Developer\Windows\LiveCoding\LiveCoding.Build.cs"
        Find    = 'PrivateIncludePaths\.Add\(\s*"Developer/Windows/LiveCodingServer/Private/External"\s*\)'
        Replace = 'PrivateIncludePaths.Add(Path.Combine(ModuleDirectory, "..", "LiveCodingServer", "Private", "External"))'
    },
    @{
        Name    = "TVOSTargetPlatformSettings"
        File    = "Engine\Source\Developer\IOS\TVOSTargetPlatformSettings\TVOSTargetPlatformSettings.Build.cs"
        Find    = '"Developer/IOS/IOSTargetPlatformSettings/Private"'
        Replace = 'Path.Combine(ModuleDirectory, "..", "IOSTargetPlatformSettings", "Private")'
    },
    @{
        Name    = "VisionOSTargetPlatformSettings"
        File    = "Engine\Platforms\VisionOS\Source\Developer\VisionOSTargetPlatformSettings\VisionOSTargetPlatformSettings.Build.cs"
        Find    = '"Developer/IOS/IOSTargetPlatformSettings/Private"'
        Replace = 'Path.Combine(ModuleDirectory, "..", "..", "..", "..", "Source", "Developer", "IOS", "IOSTargetPlatformSettings", "Private")'
    }
)

$pendingPatches = 0
$appliedPatches = 0
$errors = 0

foreach ($p in $patches) {
    $full = Join-Path $EngineDir $p.File
    Write-Host "`n--- $($p.Name) ---"
    Write-Host "  File: $full"

    if (-not (Test-Path -LiteralPath $full)) {
        Write-Host "  SKIP: file not present (may not apply to your engine version)" -ForegroundColor Yellow
        continue
    }

    $content = Get-Content -LiteralPath $full -Raw

    $needsUsing = -not ($content -match '(?m)^\s*using\s+System\.IO\s*;')
    $needsReplace = $content -match $p.Find

    if (-not $needsReplace -and -not $needsUsing) {
        Write-Host "  OK: already patched (or didn't need patching)" -ForegroundColor Green
        continue
    }

    $pendingPatches++

    if ($needsUsing) {
        Write-Host "  NEEDS: insert 'using System.IO;'"
    }
    if ($needsReplace) {
        Write-Host "  NEEDS: replace relative path with Path.Combine form"
    }

    if (-not $Apply) {
        continue
    }

    try {
        # 1. Insert `using System.IO;` after the first `using ...;` line that
        #    isn't already System.IO. Conservative: don't touch line endings.
        if ($needsUsing) {
            $content = $content -replace '(?m)^(\s*using\s+UnrealBuildTool\s*;\s*\r?\n)',
                                        '$1using System.IO;
'
        }

        # 2. Replace the unpatched PrivateIncludePaths line.
        if ($needsReplace) {
            $content = $content -replace $p.Find, $p.Replace
        }

        Set-Content -LiteralPath $full -Value $content -NoNewline
        Write-Host "  APPLIED" -ForegroundColor Green
        $appliedPatches++
    } catch {
        Write-Host "  FAILED: $_" -ForegroundColor Red
        $errors++
    }
}

Write-Host "`n=== Summary ==="
Write-Host "  Patches needed:  $pendingPatches"
Write-Host "  Patches applied: $appliedPatches"
Write-Host "  Errors:          $errors"

if ($errors -gt 0) { exit 3 }
if ($pendingPatches -gt 0 -and -not $Apply) {
    Write-Host "`nRun again with -Apply to apply these patches." -ForegroundColor Yellow
    exit 2
}
exit 0
