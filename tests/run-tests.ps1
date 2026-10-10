[CmdletBinding()]
param([string]$Compiler = 'g++.exe')
$ErrorActionPreference = 'Stop'
$taskCompiler = (Get-Command $Compiler -ErrorAction Stop).Source
$taskRoot = Split-Path $PSScriptRoot -Parent
$taskTest = Join-Path $PSScriptRoot 'native-stats-test.exe'
& $taskCompiler -std=c++17 -O2 -static -s (Join-Path $PSScriptRoot 'native-stats-test.cpp') -o $taskTest
if ($LASTEXITCODE -ne 0) { throw 'Native reader test compilation failed.' }
& $taskTest
if ($LASTEXITCODE -ne 0) { throw 'Native reader tests failed.' }
& node.exe (Join-Path $PSScriptRoot 'session-ui.test.mjs')
if ($LASTEXITCODE -ne 0) { throw 'Session UI tests failed.' }
$taskScripts = @('Start-SessionStats.ps1','Stop-SessionStats.ps1','Build-Release.ps1','native/build.ps1','tests/run-tests.ps1')
foreach ($taskScript in $taskScripts) {
    $taskTokens = $null
    $taskErrors = $null
    [void][Management.Automation.Language.Parser]::ParseFile((Join-Path $taskRoot $taskScript), [ref]$taskTokens, [ref]$taskErrors)
    if ($taskErrors.Count) { throw "PowerShell syntax errors in $taskScript" }
}
Write-Output 'All native, UI, and PowerShell checks passed.'
