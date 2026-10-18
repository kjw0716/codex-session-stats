[CmdletBinding()]
param([switch]$Quiet)
$ErrorActionPreference = 'Stop'
$taskPanel = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'SessionStats.exe'))
$taskOwned = @(Get-Process -Name SessionStats -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $taskPanel })
if ($taskOwned.Count) {
    $taskQuit = Start-Process -FilePath $taskPanel -ArgumentList '--quit' -PassThru
    [void]$taskQuit.WaitForExit(5000)
    foreach ($taskProcess in $taskOwned) { if (-not $taskProcess.WaitForExit(5000)) { throw 'Session Stats is still closing. Try again shortly.' } }
}
& (Join-Path $PSScriptRoot 'Stop-EmbeddedStats.ps1') -Quiet:$Quiet
