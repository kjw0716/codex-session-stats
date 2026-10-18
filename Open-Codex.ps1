[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$taskPreferences = Join-Path $env:LOCALAPPDATA 'CodexSessionMonitor/preferences.json'
$taskAutoStart = $false
if (Test-Path -LiteralPath $taskPreferences) {
    try { $taskAutoStart = (Get-Content -LiteralPath $taskPreferences -Raw | ConvertFrom-Json).autoStart -eq $true } catch { }
}
& (Join-Path $PSScriptRoot 'Start-EmbeddedStats.ps1') -AppOnly:(-not $taskAutoStart)
