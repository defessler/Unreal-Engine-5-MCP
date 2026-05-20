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
