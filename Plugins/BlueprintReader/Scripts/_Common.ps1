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

# Resolve the engine install dir from the .uproject's EngineAssociation, the way UE
# itself does: env override first, then a GUID -> HKCU source-build registration, a
# version ("5.8") -> HKLM Launcher InstalledDirectory, an explicit path, then a couple
# of common Launcher locations. Returns $null if nothing resolves (caller asks for
# -EngineDir).
function Resolve-BprEngineDir {
    param([string]$ProjectFile)

    if ($env:BP_READER_ENGINE_DIR -and (Test-Path -LiteralPath $env:BP_READER_ENGINE_DIR)) {
        return $env:BP_READER_ENGINE_DIR
    }
    if (-not $ProjectFile -or -not (Test-Path -LiteralPath $ProjectFile)) { return $null }

    $assoc = $null
    try { $assoc = (Get-Content -Raw -LiteralPath $ProjectFile | ConvertFrom-Json).EngineAssociation } catch {}
    if ([string]::IsNullOrWhiteSpace($assoc)) { return $null }

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
        return $null
    }

    # A version association ("5.8") -> the Launcher's recorded InstalledDirectory.
    foreach ($base in @('HKLM:\SOFTWARE\EpicGames\Unreal Engine',
                        'HKLM:\SOFTWARE\WOW6432Node\EpicGames\Unreal Engine')) {
        try {
            $p = (Get-ItemProperty -Path (Join-Path $base $assoc) -Name 'InstalledDirectory' -ErrorAction Stop).InstalledDirectory
            if ($p -and (Test-Path -LiteralPath $p)) { return $p }
        } catch {}
    }
    # Common Launcher install locations as a last resort.
    foreach ($root in @($env:ProgramFiles, ${env:ProgramFiles(x86)}, 'C:\Program Files', 'D:\Epic Games', 'D:\Games\Epic Games')) {
        if (-not $root) { continue }
        $cand = Join-Path $root ("Epic Games\UE_" + $assoc)
        if (Test-Path -LiteralPath $cand) { return $cand }
        $cand2 = Join-Path $root ("UE_" + $assoc)
        if (Test-Path -LiteralPath $cand2) { return $cand2 }
    }
    return $null
}
