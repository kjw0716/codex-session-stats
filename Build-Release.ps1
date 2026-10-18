[CmdletBinding()]
param([string]$Version = '0.2.0', [string]$Compiler = 'g++.exe')
$ErrorActionPreference = 'Stop'
if ($Version -notmatch '^\d+\.\d+\.\d+(?:-[a-zA-Z0-9.-]+)?$') { throw 'Use a semantic version such as 0.1.0.' }
& (Join-Path $PSScriptRoot 'native/build.ps1') -Compiler $Compiler | Out-Null
$taskDistribution = Join-Path $PSScriptRoot 'dist'
$taskPackage = Join-Path $taskDistribution ('codex-session-stats-v' + $Version + '-windows-x64')
New-Item -ItemType Directory -Path $taskPackage -Force | Out-Null
$taskFiles = @('SessionStats.exe','SessionMonitor.exe','session-ui.js','Start-SessionStats.ps1','Stop-SessionStats.ps1','Start-EmbeddedStats.ps1','Stop-EmbeddedStats.ps1','Install-Shortcuts.ps1','Open-Codex.ps1','Start-SessionStats.cmd','Stop-SessionStats.cmd','README.md','CHANGELOG.md','LICENSE','NOTICE.md')
foreach ($taskFile in $taskFiles) { Copy-Item -LiteralPath (Join-Path $PSScriptRoot $taskFile) -Destination $taskPackage -Force }
New-Item -ItemType Directory -Path (Join-Path $taskPackage 'assets'),(Join-Path $taskPackage 'docs'),(Join-Path $taskPackage 'native/vendor') -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'assets/screenshot.png') -Destination (Join-Path $taskPackage 'assets/screenshot.png') -Force
foreach ($taskImage in @('floating-bar.png','floating-bar-narrow.png')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot ('assets/' + $taskImage)) -Destination (Join-Path $taskPackage ('assets/' + $taskImage)) -Force
}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'assets/session-stats.ico') -Destination (Join-Path $taskPackage 'assets/session-stats.ico') -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'docs/benchmark.json') -Destination (Join-Path $taskPackage 'docs/benchmark.json') -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'docs/panel-benchmark.json') -Destination (Join-Path $taskPackage 'docs/panel-benchmark.json') -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'native/vendor/LICENSE.MIT') -Destination (Join-Path $taskPackage 'native/vendor/LICENSE.MIT') -Force
$taskZip = $taskPackage + '.zip'
Compress-Archive -LiteralPath $taskPackage -DestinationPath $taskZip -Force
$taskHash = (Get-FileHash -LiteralPath $taskZip -Algorithm SHA256).Hash.ToLowerInvariant()
Set-Content -LiteralPath ($taskZip + '.sha256') -Value ($taskHash + '  ' + [IO.Path]::GetFileName($taskZip)) -Encoding ascii
Write-Output $taskZip
