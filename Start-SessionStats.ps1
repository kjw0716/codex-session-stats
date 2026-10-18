[CmdletBinding()]
param([switch]$Quiet)
$ErrorActionPreference = 'Stop'
$taskPanel = Join-Path $PSScriptRoot 'SessionStats.exe'
if (-not (Test-Path -LiteralPath $taskPanel)) { throw 'SessionStats.exe is missing. Build the project or use a release.' }
Start-Process -FilePath $taskPanel
