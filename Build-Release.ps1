[CmdletBinding()]
param([string]$Version = '0.1.0', [string]$Compiler = 'g++.exe')
$ErrorActionPreference = 'Stop'
if ($Version -notmatch '^\d+\.\d+\.\d+(?:-[a-zA-Z0-9.-]+)?$') { throw 'Use a semantic version such as 0.1.0.' }
& (Join-Path $PSScriptRoot 'native/build.ps1') -Compiler $Compiler | Out-Null
$taskDistribution = Join-Path $PSScriptRoot 'dist'
$taskPackage = Join-Path $taskDistribution ('codex-session-stats-v' + $Version + '-windows-x64')
New-Item -ItemType Directory -Path $taskPackage -Force | Out-Null
$taskFiles = @('SessionMonitor.exe','session-ui.js','Start-SessionStats.ps1','Stop-SessionStats.ps1','Start-SessionStats.cmd','Stop-SessionStats.cmd','README.md','LICENSE','NOTICE.md')
foreach ($taskFile in $taskFiles) { Copy-Item -LiteralPath (Join-Path $PSScriptRoot $taskFile) -Destination $taskPackage -Force }
New-Item -ItemType Directory -Path (Join-Path $taskPackage 'assets'),(Join-Path $taskPackage 'docs'),(Join-Path $taskPackage 'native/vendor') -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'assets/screenshot.png') -Destination (Join-Path $taskPackage 'assets/screenshot.png') -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'docs/benchmark.json') -Destination (Join-Path $taskPackage 'docs/benchmark.json') -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'native/vendor/LICENSE.MIT') -Destination (Join-Path $taskPackage 'native/vendor/LICENSE.MIT') -Force
$taskZip = $taskPackage + '.zip'
Compress-Archive -LiteralPath $taskPackage -DestinationPath $taskZip -Force
$taskHash = (Get-FileHash -LiteralPath $taskZip -Algorithm SHA256).Hash.ToLowerInvariant()
Set-Content -LiteralPath ($taskZip + '.sha256') -Value ($taskHash + '  ' + [IO.Path]::GetFileName($taskZip)) -Encoding ascii
Write-Output $taskZip
