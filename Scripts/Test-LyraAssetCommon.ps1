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
