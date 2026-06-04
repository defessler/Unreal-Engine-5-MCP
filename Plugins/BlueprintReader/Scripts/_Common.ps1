# _Common.ps1 - shared path inference for the BlueprintReader helper scripts.
#
# Assumes the canonical layout: the plugin lives at <Project>/Plugins/BlueprintReader,
# so these scripts are at <Project>/Plugins/BlueprintReader/Scripts. Dot-source this
# and call the Resolve-Bpr* helpers to fill omitted -ProjectFile / -EngineDir args, so
# the launchers work with zero arguments from their in-project location.
#
#   . (Join-Path $PSScriptRoot '_Common.ps1')
#   if (-not $ProjectFile) { $ProjectFile = Resolve-BprProjectFile (Resolve-BprProjectDir $PSScriptRoot) }
#   if (-not $EngineDir)   { $EngineDir   = Resolve-BprEngineDir $ProjectFile }

# <Project> is three levels up from the Scripts dir: Scripts -> BlueprintReader -> Plugins -> <Project>.
function Resolve-BprProjectDir {
    param([Parameter(Mandatory=$true)][string]$ScriptsDir)
    return (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $ScriptsDir)))
}

# The single *.uproject in the project dir (the first one if somehow several).
function Resolve-BprProjectFile {
    param([Parameter(Mandatory=$true)][string]$ProjectDir)
    if (-not (Test-Path -LiteralPath $ProjectDir)) { return $null }
    $up = Get-ChildItem -LiteralPath $ProjectDir -Filter *.uproject -ErrorAction SilentlyContinue |
          Select-Object -First 1
    if ($up) { return $up.FullName }
    return $null
}

# Enumerate the UE engines installed on this machine, highest version first.
# Binary/Launcher installs (HKLM InstalledDirectory) + registered source builds
# (HKCU Builds) + a scan of the common Launcher dirs. Used as the fallback when a
# project's EngineAssociation can't be resolved on its own. Each entry is a
# hashtable @{ Version = '5.8'; Path = '...' }; source builds sort last (tagged 0.0).
function Get-BprInstalledEngines {
    $found = [System.Collections.Generic.List[object]]::new()

    foreach ($base in @('HKLM:\SOFTWARE\EpicGames\Unreal Engine',
                        'HKLM:\SOFTWARE\WOW6432Node\EpicGames\Unreal Engine')) {
        try {
            Get-ChildItem -LiteralPath $base -ErrorAction Stop | ForEach-Object {
                $dir = (Get-ItemProperty -LiteralPath $_.PSPath -Name InstalledDirectory -ErrorAction SilentlyContinue).InstalledDirectory
                if ($dir -and (Test-Path -LiteralPath $dir)) { $found.Add(@{ Version = $_.PSChildName; Path = $dir }) }
            }
        } catch {}
    }
    $builds = 'HKCU:\Software\Epic Games\Unreal Engine\Builds'
    if (Test-Path $builds) {
        foreach ($name in (Get-Item $builds).Property) {
            $p = (Get-ItemProperty -LiteralPath $builds -Name $name -ErrorAction SilentlyContinue).$name
            if ($p -and (Test-Path -LiteralPath $p)) { $found.Add(@{ Version = '0.0'; Path = $p }) }
        }
    }
    foreach ($root in @($env:ProgramFiles, ${env:ProgramFiles(x86)}, 'C:\Program Files', 'D:\Epic Games', 'D:\Games\Epic Games')) {
        if (-not $root) { continue }
        foreach ($pat in @('Epic Games\UE_*', 'UE_*')) {
            Get-ChildItem -Path (Join-Path $root $pat) -Directory -ErrorAction SilentlyContinue | ForEach-Object {
                if (Test-Path -LiteralPath (Join-Path $_.FullName 'Engine')) {
                    $found.Add(@{ Version = ($_.Name -replace '^UE_',''); Path = $_.FullName })
                }
            }
        }
    }
    # De-dup by path, then highest version first.
    $seen = @{}
    $uniq = foreach ($e in $found) { if (-not $seen.ContainsKey($e.Path)) { $seen[$e.Path] = $true; $e } }
    return @($uniq | Sort-Object { try { [version]$_.Version } catch { [version]'0.0' } } -Descending)
}

# Resolve the engine install dir from the .uproject's EngineAssociation, the way UE
# itself does: env override first, then an explicit path / a GUID -> HKCU source-build
# registration / a version ("5.8") -> HKLM Launcher InstalledDirectory. When the
# association can't be resolved (empty, an unregistered GUID, an unknown version),
# fall back to whatever engine IS installed (highest version) so the launchers still
# work on a single-engine machine. Returns $null only if no engine can be found at all.
function Resolve-BprEngineDir {
    param([string]$ProjectFile)

    if ($env:BP_READER_ENGINE_DIR -and (Test-Path -LiteralPath $env:BP_READER_ENGINE_DIR)) {
        return $env:BP_READER_ENGINE_DIR
    }

    $assoc = $null
    if ($ProjectFile -and (Test-Path -LiteralPath $ProjectFile)) {
        try { $assoc = (Get-Content -Raw -LiteralPath $ProjectFile | ConvertFrom-Json).EngineAssociation } catch {}
    }

    # 1) Association-specific resolution (wins on a multi-engine machine).
    if (-not [string]::IsNullOrWhiteSpace($assoc)) {
        # An explicit path association.
        if (($assoc -match '[\\/]') -and (Test-Path -LiteralPath $assoc)) { return $assoc }

        # A GUID association -> a registered source/custom build under HKCU Builds.
        if ($assoc -match '^\{?[0-9A-Fa-f]{8}-([0-9A-Fa-f]{4}-){3}[0-9A-Fa-f]{12}\}?$') {
            $bare = $assoc.Trim('{', '}')
            foreach ($name in @($assoc, "{$bare}", $bare)) {
                try {
                    $p = (Get-ItemProperty -Path 'HKCU:\Software\Epic Games\Unreal Engine\Builds' -Name $name -ErrorAction Stop).$name
                    if ($p -and (Test-Path -LiteralPath $p)) { return $p }
                } catch {}
            }
        }
        else {
            # A version association ("5.8") -> the Launcher's recorded InstalledDirectory.
            foreach ($base in @('HKLM:\SOFTWARE\EpicGames\Unreal Engine',
                                'HKLM:\SOFTWARE\WOW6432Node\EpicGames\Unreal Engine')) {
                try {
                    $p = (Get-ItemProperty -Path (Join-Path $base $assoc) -Name 'InstalledDirectory' -ErrorAction Stop).InstalledDirectory
                    if ($p -and (Test-Path -LiteralPath $p)) { return $p }
                } catch {}
            }
            foreach ($root in @($env:ProgramFiles, ${env:ProgramFiles(x86)}, 'C:\Program Files', 'D:\Epic Games', 'D:\Games\Epic Games')) {
                if (-not $root) { continue }
                $cand = Join-Path $root ("Epic Games\UE_" + $assoc)
                if (Test-Path -LiteralPath $cand) { return $cand }
                $cand2 = Join-Path $root ("UE_" + $assoc)
                if (Test-Path -LiteralPath $cand2) { return $cand2 }
            }
        }
    }

    # 2) Fallback: the association didn't resolve. Use the installed engine (highest
    #    version). Correct for the common single-install machine; on a multi-engine
    #    host pass -EngineDir / set BP_READER_ENGINE_DIR to disambiguate.
    $engines = @(Get-BprInstalledEngines)   # @() so a single result stays an indexable array, not an unrolled hashtable
    if ($engines.Count -gt 0) {
        $pick = $engines[0].Path
        if ([string]::IsNullOrWhiteSpace($assoc)) {
            Write-Warning "No EngineAssociation in the .uproject; using the installed engine: $pick"
        } else {
            Write-Warning "EngineAssociation '$assoc' did not resolve; using the installed engine: $pick"
        }
        return $pick
    }
    return $null
}
