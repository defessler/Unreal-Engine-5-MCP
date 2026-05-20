# Scripts/Test-LyraAssetCommon.ps1
# Lightweight test runner for LyraAssetCommon.ps1.
# Each test is a script block that throws on failure.
# Exit code: 0 if all pass, 1 if any fail. No external test framework.

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'

. "$PSScriptRoot/LyraAssetCommon.ps1"

$script:Tests   = [System.Collections.Generic.List[object]]::new()
$script:Passed  = 0
$script:Failed  = [System.Collections.Generic.List[object]]::new()

function Test ([string] $Name, [scriptblock] $Body) {
    $script:Tests.Add([pscustomobject]@{ Name = $Name; Body = $Body })
}

function AssertEqual ($Expected, $Actual, [string] $Message = '') {
    # Deep-equal via JSON serialization. Works for primitives, arrays, hashtables, PSCustomObjects.
    $e = $Expected | ConvertTo-Json -Depth 20 -Compress
    $a = $Actual   | ConvertTo-Json -Depth 20 -Compress
    if ($e -ne $a) {
        $suffix = if ($Message) { ": $Message" } else { '' }
        throw "AssertEqual failed${suffix}: expected $e, got $a"
    }
}

function AssertTrue ($Condition, [string] $Message = '') {
    if (-not $Condition) { throw "AssertTrue failed: $Message" }
}

function AssertThrows ([scriptblock] $Body, [string] $MatchPattern = '') {
    try   { & $Body | Out-Null }
    catch {
        if ($MatchPattern -and ($_.Exception.Message -notmatch $MatchPattern)) {
            throw "AssertThrows: expected match '$MatchPattern', got '$($_.Exception.Message)'"
        }
        return
    }
    throw 'AssertThrows: expected exception but none thrown'
}

# --- TESTS GO HERE (added in subsequent tasks) ---

Test 'Get-LyraAssetPaths: returns array of strings' {
    $tmp = New-Item -ItemType Directory -Path (Join-Path $env:TEMP "lyra-test-$([guid]::NewGuid())") -Force
    try {
        New-Item -ItemType Directory -Path "$($tmp.FullName)/Content/Foo" -Force | Out-Null
        Set-Content -Path "$($tmp.FullName)/Content/Foo/Bar.uasset" -Value 'x'
        $paths = @(Get-LyraAssetPaths -RepoRoot $tmp.FullName)
        AssertTrue ($paths -contains 'Content/Foo/Bar.uasset') "expected Content/Foo/Bar.uasset, got: $($paths -join ', ')"
    } finally { Remove-Item -Recurse -Force $tmp.FullName }
}

Test 'Get-LyraAssetPaths: excludes BP_TestEnemy and BP_TestPickup' {
    $tmp = New-Item -ItemType Directory -Path (Join-Path $env:TEMP "lyra-test-$([guid]::NewGuid())") -Force
    try {
        New-Item -ItemType Directory -Path "$($tmp.FullName)/Content/AI" -Force | Out-Null
        Set-Content -Path "$($tmp.FullName)/Content/AI/BP_TestEnemy.uasset"  -Value 'x'
        Set-Content -Path "$($tmp.FullName)/Content/AI/BP_TestPickup.uasset" -Value 'x'
        Set-Content -Path "$($tmp.FullName)/Content/AI/BP_OtherLyra.uasset"  -Value 'x'
        $paths = @(Get-LyraAssetPaths -RepoRoot $tmp.FullName)
        AssertTrue ($paths -notcontains 'Content/AI/BP_TestEnemy.uasset')  'should exclude BP_TestEnemy'
        AssertTrue ($paths -notcontains 'Content/AI/BP_TestPickup.uasset') 'should exclude BP_TestPickup'
        AssertTrue ($paths -contains    'Content/AI/BP_OtherLyra.uasset')  'should include other Lyra BPs'
    } finally { Remove-Item -Recurse -Force $tmp.FullName }
}

Test 'Get-LyraAssetPaths: ignores non-asset extensions' {
    $tmp = New-Item -ItemType Directory -Path (Join-Path $env:TEMP "lyra-test-$([guid]::NewGuid())") -Force
    try {
        New-Item -ItemType Directory -Path "$($tmp.FullName)/Content" -Force | Out-Null
        Set-Content -Path "$($tmp.FullName)/Content/A.uasset" -Value 'x'
        Set-Content -Path "$($tmp.FullName)/Content/B.umap"   -Value 'x'
        Set-Content -Path "$($tmp.FullName)/Content/C.txt"    -Value 'x'
        Set-Content -Path "$($tmp.FullName)/Content/D.cpp"    -Value 'x'
        $paths = @(Get-LyraAssetPaths -RepoRoot $tmp.FullName) | Sort-Object
        AssertEqual @('Content/A.uasset', 'Content/B.umap') $paths
    } finally { Remove-Item -Recurse -Force $tmp.FullName }
}

Test 'Get-LyraAssetPaths: excludes Content/Recreated subtree (regenerable test output)' {
    $tmp = New-Item -ItemType Directory -Path (Join-Path $env:TEMP "lyra-test-$([guid]::NewGuid())") -Force
    try {
        New-Item -ItemType Directory -Path "$($tmp.FullName)/Content/Recreated" -Force | Out-Null
        New-Item -ItemType Directory -Path "$($tmp.FullName)/Content/Real"      -Force | Out-Null
        Set-Content -Path "$($tmp.FullName)/Content/Recreated/BPIR_Foo.uasset" -Value 'x'
        Set-Content -Path "$($tmp.FullName)/Content/Real/Bar.uasset"           -Value 'x'
        $paths = @(Get-LyraAssetPaths -RepoRoot $tmp.FullName)
        AssertTrue ($paths -notcontains 'Content/Recreated/BPIR_Foo.uasset') 'should exclude Recreated/'
        AssertTrue ($paths -contains    'Content/Real/Bar.uasset')           'should include real Lyra content'
    } finally { Remove-Item -Recurse -Force $tmp.FullName }
}

Test 'Get-LyraAssetPaths: walks plugin Content/ dirs' {
    $tmp = New-Item -ItemType Directory -Path (Join-Path $env:TEMP "lyra-test-$([guid]::NewGuid())") -Force
    try {
        $p = "$($tmp.FullName)/Plugins/GameFeatures/ShooterCore/Content"
        New-Item -ItemType Directory -Path $p -Force | Out-Null
        Set-Content -Path "$p/Pawn.uasset" -Value 'x'
        $paths = @(Get-LyraAssetPaths -RepoRoot $tmp.FullName)
        AssertTrue ($paths -contains 'Plugins/GameFeatures/ShooterCore/Content/Pawn.uasset') "got: $($paths -join ', ')"
    } finally { Remove-Item -Recurse -Force $tmp.FullName }
}

Test 'Get-FileManifestEntries: computes SHA-256 and size' {
    $tmp = New-Item -ItemType Directory -Path (Join-Path $env:TEMP "lyra-test-$([guid]::NewGuid())") -Force
    try {
        # SHA-256 of 'hello world' (no trailing newline):
        # b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9
        $bytes = [System.Text.Encoding]::ASCII.GetBytes('hello world')
        [System.IO.File]::WriteAllBytes("$($tmp.FullName)/a.txt", $bytes)
        $entries = @(Get-FileManifestEntries -RepoRoot $tmp.FullName -RelativePaths @('a.txt'))
        AssertEqual 1 $entries.Count
        AssertEqual 'a.txt'                                                              $entries[0].path
        AssertEqual 11                                                                   $entries[0].size
        AssertEqual 'b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9'   $entries[0].sha256
    } finally { Remove-Item -Recurse -Force $tmp.FullName }
}

Test 'Get-FileManifestEntries: handles multiple files in input order' {
    $tmp = New-Item -ItemType Directory -Path (Join-Path $env:TEMP "lyra-test-$([guid]::NewGuid())") -Force
    try {
        [System.IO.File]::WriteAllBytes("$($tmp.FullName)/b.txt", [byte[]](98))
        [System.IO.File]::WriteAllBytes("$($tmp.FullName)/a.txt", [byte[]](97))
        $entries = @(Get-FileManifestEntries -RepoRoot $tmp.FullName -RelativePaths @('b.txt','a.txt'))
        AssertEqual 'b.txt' $entries[0].path
        AssertEqual 'a.txt' $entries[1].path
    } finally { Remove-Item -Recurse -Force $tmp.FullName }
}

Test 'Build-Manifest: schema_version, tag, sorted files' {
    $tmp = New-Item -ItemType Directory -Path (Join-Path $env:TEMP "lyra-test-$([guid]::NewGuid())") -Force
    try {
        New-Item -ItemType Directory -Path "$($tmp.FullName)/Content" -Force | Out-Null
        [System.IO.File]::WriteAllBytes("$($tmp.FullName)/Content/Z.uasset", [byte[]](122))
        [System.IO.File]::WriteAllBytes("$($tmp.FullName)/Content/A.uasset", [byte[]](97))
        $m = Build-Manifest -RepoRoot $tmp.FullName -Tag 'lyra-assets-v1'
        AssertEqual 1                  $m.schema_version
        AssertEqual 'lyra-assets-v1'   $m.tag
        AssertEqual 2                  $m.total_files
        AssertEqual 2                  $m.total_bytes
        AssertEqual 'Content/A.uasset' $m.files[0].path
        AssertEqual 'Content/Z.uasset' $m.files[1].path
    } finally { Remove-Item -Recurse -Force $tmp.FullName }
}

Test 'Build-Manifest: empty asset set produces valid manifest' {
    $tmp = New-Item -ItemType Directory -Path (Join-Path $env:TEMP "lyra-test-$([guid]::NewGuid())") -Force
    try {
        $m = Build-Manifest -RepoRoot $tmp.FullName -Tag 'lyra-assets-v1'
        AssertEqual 0 $m.total_files
        AssertEqual 0 $m.total_bytes
    } finally { Remove-Item -Recurse -Force $tmp.FullName }
}

Test 'Test-Manifest: Ok when all files present and hashes match' {
    $tmp = New-Item -ItemType Directory -Path (Join-Path $env:TEMP "lyra-test-$([guid]::NewGuid())") -Force
    try {
        New-Item -ItemType Directory -Path "$($tmp.FullName)/Content" -Force | Out-Null
        [System.IO.File]::WriteAllBytes("$($tmp.FullName)/Content/A.uasset", [byte[]](97))
        $m = Build-Manifest -RepoRoot $tmp.FullName -Tag 'lyra-assets-v1'
        $r = Test-Manifest  -RepoRoot $tmp.FullName -Manifest $m
        AssertTrue $r.Ok 'expected Ok'
        AssertEqual 0 $r.Missing.Count
        AssertEqual 0 $r.Mismatch.Count
    } finally { Remove-Item -Recurse -Force $tmp.FullName }
}

Test 'Test-Manifest: detects missing files' {
    $tmp = New-Item -ItemType Directory -Path (Join-Path $env:TEMP "lyra-test-$([guid]::NewGuid())") -Force
    try {
        New-Item -ItemType Directory -Path "$($tmp.FullName)/Content" -Force | Out-Null
        [System.IO.File]::WriteAllBytes("$($tmp.FullName)/Content/A.uasset", [byte[]](97))
        $m = Build-Manifest -RepoRoot $tmp.FullName -Tag 'lyra-assets-v1'
        Remove-Item "$($tmp.FullName)/Content/A.uasset"
        $r = Test-Manifest -RepoRoot $tmp.FullName -Manifest $m
        AssertTrue (-not $r.Ok)
        AssertEqual @('Content/A.uasset') $r.Missing
    } finally { Remove-Item -Recurse -Force $tmp.FullName }
}

Test 'Test-Manifest: detects mismatched hashes' {
    $tmp = New-Item -ItemType Directory -Path (Join-Path $env:TEMP "lyra-test-$([guid]::NewGuid())") -Force
    try {
        New-Item -ItemType Directory -Path "$($tmp.FullName)/Content" -Force | Out-Null
        [System.IO.File]::WriteAllBytes("$($tmp.FullName)/Content/A.uasset", [byte[]](97))
        $m = Build-Manifest -RepoRoot $tmp.FullName -Tag 'lyra-assets-v1'
        [System.IO.File]::WriteAllBytes("$($tmp.FullName)/Content/A.uasset", [byte[]](98))
        $r = Test-Manifest -RepoRoot $tmp.FullName -Manifest $m
        AssertTrue (-not $r.Ok)
        AssertEqual @('Content/A.uasset') $r.Mismatch
    } finally { Remove-Item -Recurse -Force $tmp.FullName }
}

Test 'Find-LyraInstallPaths: returns matching InstallLocation dirs' {
    $tmp = New-Item -ItemType Directory -Path (Join-Path $env:TEMP "lyra-test-$([guid]::NewGuid())") -Force
    try {
        $datDir = Join-Path $tmp.FullName 'launcher'
        New-Item -ItemType Directory -Path $datDir -Force | Out-Null
        $datPath = Join-Path $datDir 'LauncherInstalled.dat'

        $lyraDir  = Join-Path $tmp.FullName 'EpicGames/Lyra'
        $otherDir = Join-Path $tmp.FullName 'EpicGames/Fortnite'
        New-Item -ItemType Directory -Path $lyraDir  -Force | Out-Null
        New-Item -ItemType Directory -Path $otherDir -Force | Out-Null
        Set-Content -Path (Join-Path $lyraDir 'LyraStarterGame.uproject') -Value '{}'

        $dat = [pscustomobject]@{
            InstallationList = @(
                [pscustomobject]@{ InstallLocation = $lyraDir;  AppName = 'UE_Lyra'; AppVersion = '5.7.4' }
                [pscustomobject]@{ InstallLocation = $otherDir; AppName = 'Fortnite'; AppVersion = '1.0' }
            )
        }
        $dat | ConvertTo-Json -Depth 10 | Set-Content -Path $datPath -Encoding utf8

        $found = @(Find-LyraInstallPaths -LauncherDatPath $datPath)
        AssertEqual 1 $found.Count
        AssertEqual $lyraDir $found[0]
    } finally { Remove-Item -Recurse -Force $tmp.FullName }
}

Test 'Find-LyraInstallPaths: returns empty when no Lyra entry' {
    $tmp = New-Item -ItemType Directory -Path (Join-Path $env:TEMP "lyra-test-$([guid]::NewGuid())") -Force
    try {
        $datPath = Join-Path $tmp.FullName 'LauncherInstalled.dat'
        ([pscustomobject]@{ InstallationList = @() }) | ConvertTo-Json | Set-Content -Path $datPath -Encoding utf8
        $found = @(Find-LyraInstallPaths -LauncherDatPath $datPath)
        AssertEqual 0 $found.Count
    } finally { Remove-Item -Recurse -Force $tmp.FullName }
}

Test 'Find-LyraInstallPaths: throws when dat file missing' {
    AssertThrows { Find-LyraInstallPaths -LauncherDatPath 'X:\does\not\exist.dat' } 'not found'
}

Test 'Find-LyraInstallPaths: throws clean message on malformed JSON' {
    $tmp = New-Item -ItemType Directory -Path (Join-Path $env:TEMP "lyra-test-$([guid]::NewGuid())") -Force
    try {
        $datPath = Join-Path $tmp.FullName 'LauncherInstalled.dat'
        Set-Content -Path $datPath -Value '{ not valid json,'
        AssertThrows { Find-LyraInstallPaths -LauncherDatPath $datPath } 'parse'
    } finally { Remove-Item -Recurse -Force $tmp.FullName }
}

Test 'Find-LyraInstallPaths: tolerates dat with no InstallationList key' {
    $tmp = New-Item -ItemType Directory -Path (Join-Path $env:TEMP "lyra-test-$([guid]::NewGuid())") -Force
    try {
        $datPath = Join-Path $tmp.FullName 'LauncherInstalled.dat'
        '{}' | Set-Content -Path $datPath -Encoding utf8
        $found = @(Find-LyraInstallPaths -LauncherDatPath $datPath)
        AssertEqual 0 $found.Count
    } finally { Remove-Item -Recurse -Force $tmp.FullName }
}

Test 'Get-RestorePathMap: maps each glob root to identical relative dest' {
    $map = Get-RestorePathMap -LyraInstallRoot 'C:/L' -RepoRoot 'D:/R'
    AssertEqual 'C:/L/Content'                                  $map['Content']
    AssertEqual 'D:/R/Content'                                  $map['Content_dst']
    AssertTrue ($map.Contains('Plugins/GameFeatures/ShooterCore/Content'))
    AssertEqual 'C:/L/Plugins/GameFeatures/ShooterCore/Content' $map['Plugins/GameFeatures/ShooterCore/Content']
    AssertEqual 'D:/R/Plugins/GameFeatures/ShooterCore/Content' $map['Plugins/GameFeatures/ShooterCore/Content_dst']
}

# Behavioral guard for the contract setup.ps1's robocopy call relies on:
# /E /XO must copy src-only files in, overwrite shared files where src is newer,
# AND preserve dest-only files. This is the difference between /E and /MIR (which
# would PURGE the dest-only file — the bug that almost deleted the test BPs during
# the Phase B working-tree recovery).
Test 'Invoke-LyraAssetCleanup: deletes manifest entries, leaves non-manifest files alone' {
    $tmp = New-Item -ItemType Directory -Path (Join-Path $env:TEMP "lyra-test-$([guid]::NewGuid())") -Force
    try {
        $repo = $tmp.FullName
        New-Item -ItemType Directory -Path "$repo/Content/Characters" -Force | Out-Null
        New-Item -ItemType Directory -Path "$repo/Content/AI"         -Force | Out-Null
        # In-manifest Lyra files (must be deleted)
        Set-Content -Path "$repo/Content/Characters/Mesh.uasset" -NoNewline -Value 'mesh'
        Set-Content -Path "$repo/Content/Characters/Tex.uasset"  -NoNewline -Value 'tex'
        # Out-of-manifest test BP (must survive)
        Set-Content -Path "$repo/Content/AI/BP_TestEnemy.uasset" -NoNewline -Value 'seed'
        # Out-of-manifest README (must survive)
        Set-Content -Path "$repo/README.md" -NoNewline -Value 'readme'

        $m = Build-Manifest -RepoRoot $repo -Tag 'lyra-assets-v1'
        # Manifest will exclude BP_TestEnemy (exemption); confirm before cleanup
        AssertEqual 2 $m.total_files

        $r = Invoke-LyraAssetCleanup -RepoRoot $repo -Manifest $m
        AssertEqual 2 $r.Deleted.Count
        AssertEqual 0 $r.NotPresent.Count
        AssertTrue (-not (Test-Path "$repo/Content/Characters/Mesh.uasset")) 'manifest entry should be gone'
        AssertTrue (-not (Test-Path "$repo/Content/Characters/Tex.uasset"))  'manifest entry should be gone'
        AssertTrue (Test-Path "$repo/Content/AI/BP_TestEnemy.uasset")        'test BP must survive'
        AssertTrue (Test-Path "$repo/README.md")                             'README must survive'
        AssertTrue (-not (Test-Path "$repo/Content/Characters"))             'empty dir should be swept'
        AssertTrue (Test-Path "$repo/Content/AI")                            'non-empty dir must survive'
    } finally { Remove-Item -Recurse -Force $tmp.FullName }
}

Test 'Invoke-LyraAssetCleanup: idempotent (NotPresent on second run)' {
    $tmp = New-Item -ItemType Directory -Path (Join-Path $env:TEMP "lyra-test-$([guid]::NewGuid())") -Force
    try {
        $repo = $tmp.FullName
        New-Item -ItemType Directory -Path "$repo/Content" -Force | Out-Null
        Set-Content -Path "$repo/Content/A.uasset" -NoNewline -Value 'a'
        $m = Build-Manifest -RepoRoot $repo -Tag 'lyra-assets-v1'
        Invoke-LyraAssetCleanup -RepoRoot $repo -Manifest $m | Out-Null
        $r = Invoke-LyraAssetCleanup -RepoRoot $repo -Manifest $m
        AssertEqual 0 $r.Deleted.Count
        AssertEqual 1 $r.NotPresent.Count
    } finally { Remove-Item -Recurse -Force $tmp.FullName }
}

Test 'robocopy /E /XO contract: copies src-only, preserves dest-only (regression for /MIR-vs-/E)' {
    $tmp = New-Item -ItemType Directory -Path (Join-Path $env:TEMP "lyra-test-$([guid]::NewGuid())") -Force
    try {
        $src = Join-Path $tmp.FullName 'src'
        $dst = Join-Path $tmp.FullName 'dst'
        New-Item -ItemType Directory -Path $src -Force | Out-Null
        New-Item -ItemType Directory -Path $dst -Force | Out-Null
        # src has a Lyra-style asset
        Set-Content -Path "$src/SKM_Manny.uasset"     -NoNewline -Value 'lyra-mesh'
        # dst has a tracked-but-not-in-Lyra file (the BP_TestEnemy scenario)
        Set-Content -Path "$dst/BP_TestEnemy.uasset"  -NoNewline -Value 'seed-bp'
        # Same flag set as Scripts/setup.ps1 uses
        & robocopy $src $dst /E /XO /NFL /NDL /NJH /NJS /NP /R:1 /W:1 /MT:2 | Out-Null
        AssertTrue (Test-Path "$dst/SKM_Manny.uasset")     'src-only file should be copied'
        AssertTrue (Test-Path "$dst/BP_TestEnemy.uasset")  'dst-only file must survive — the /MIR-vs-/E lesson'
        AssertEqual 'seed-bp' (Get-Content "$dst/BP_TestEnemy.uasset" -Raw)
        AssertEqual 'lyra-mesh' (Get-Content "$dst/SKM_Manny.uasset" -Raw)
    } finally { Remove-Item -Recurse -Force $tmp.FullName }
}

# --- RUN ---

foreach ($t in $script:Tests) {
    try {
        & $t.Body
        $script:Passed++
        Write-Host "  pass: $($t.Name)" -ForegroundColor Green
    } catch {
        $script:Failed.Add([pscustomobject]@{ Name = $t.Name; Error = $_.Exception.Message })
        Write-Host "  FAIL: $($t.Name)" -ForegroundColor Red
        Write-Host "        $($_.Exception.Message)" -ForegroundColor DarkRed
    }
}

Write-Host ''
$failColor = if ($script:Failed.Count -gt 0) { 'Red' } else { 'Green' }
Write-Host "Passed: $($script:Passed)  Failed: $($script:Failed.Count)" -ForegroundColor $failColor

if ($script:Failed.Count -gt 0) { exit 1 } else { exit 0 }
