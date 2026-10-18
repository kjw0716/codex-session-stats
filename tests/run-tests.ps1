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
$taskFloatingTest = Join-Path $PSScriptRoot 'floating-ui-test.exe'
& $taskCompiler -std=c++17 -O2 -static -s (Join-Path $PSScriptRoot 'floating-ui-test.cpp') -o $taskFloatingTest -lgdi32 -luser32
if ($LASTEXITCODE -ne 0) { throw 'Floating layout test compilation failed.' }
& $taskFloatingTest
if ($LASTEXITCODE -ne 0) { throw 'Floating layout tests failed.' }
& node.exe (Join-Path $PSScriptRoot 'session-ui.test.mjs')
if ($LASTEXITCODE -ne 0) { throw 'Session UI tests failed.' }
if (Test-Path -LiteralPath (Join-Path $taskRoot 'SessionStats.exe')) {
    & node.exe (Join-Path $PSScriptRoot 'panel.test.mjs')
    if ($LASTEXITCODE -ne 0) { throw 'Native panel integration tests failed.' }
} else { throw 'Build the native executables before running the integration tests.' }
$taskScripts = @('Start-SessionStats.ps1','Stop-SessionStats.ps1','Start-EmbeddedStats.ps1','Stop-EmbeddedStats.ps1','Install-Shortcuts.ps1','Open-Codex.ps1','Build-Release.ps1','native/build.ps1','tests/run-tests.ps1','tools/New-Icon.ps1')
foreach ($taskScript in $taskScripts) {
    $taskTokens = $null
    $taskErrors = $null
    [void][Management.Automation.Language.Parser]::ParseFile((Join-Path $taskRoot $taskScript), [ref]$taskTokens, [ref]$taskErrors)
    if ($taskErrors.Count) { throw "PowerShell syntax errors in $taskScript" }
}
Write-Output 'All native, UI, and PowerShell checks passed.'
