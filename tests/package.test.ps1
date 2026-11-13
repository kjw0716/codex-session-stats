[CmdletBinding()]
param([string]$Version = '0.2.2')
$ErrorActionPreference = 'Stop'
if ($Version -notmatch '^\d+\.\d+\.\d+(?:-[a-zA-Z0-9.-]+)?$') { throw 'Invalid release version.' }
$taskRoot = Split-Path $PSScriptRoot -Parent
$taskName = 'codex-session-stats-v' + $Version + '-windows-x64'
$taskZip = Join-Path $taskRoot ('dist/' + $taskName + '.zip')
$taskExpectedHash = ((Get-Content -LiteralPath ($taskZip + '.sha256') -Raw).Trim() -split '\s+')[0]
if ((Get-FileHash -LiteralPath $taskZip -Algorithm SHA256).Hash -ne $taskExpectedHash) { throw 'Release ZIP checksum mismatch.' }
$taskFixture = Join-Path ([IO.Path]::GetTempPath()) ('session-stats-package-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $taskFixture | Out-Null
$taskPreviousRoot = $env:SESSION_STATS_PACKAGE_ROOT
try {
    Expand-Archive -LiteralPath $taskZip -DestinationPath $taskFixture
    $taskExtracted = Join-Path $taskFixture $taskName
    foreach ($taskFile in @('SessionStats.exe','SessionMonitor.exe','Open-ChatGPT.exe','Install-Shortcuts.ps1','Open-Codex.ps1','Start-EmbeddedStats.ps1','README.md','CHANGELOG.md','LICENSE','NOTICE.md')) {
        if (-not (Test-Path -LiteralPath (Join-Path $taskExtracted $taskFile) -PathType Leaf)) { throw "Release is missing $taskFile" }
    }
    $env:SESSION_STATS_PACKAGE_ROOT = $taskExtracted
    & node.exe (Join-Path $PSScriptRoot 'launcher.test.mjs')
    if ($LASTEXITCODE -ne 0) { throw 'Extracted release launcher/install checks failed.' }
    Write-Output 'PASS release ZIP SHA-256, required files, extracted installation and console-free startup'
} finally {
    $env:SESSION_STATS_PACKAGE_ROOT = $taskPreviousRoot
    $taskTempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    $taskResolvedFixture = [IO.Path]::GetFullPath($taskFixture)
    if (-not $taskResolvedFixture.StartsWith($taskTempRoot, [StringComparison]::OrdinalIgnoreCase) -or [IO.Path]::GetFileName($taskResolvedFixture) -notlike 'session-stats-package-*') { throw 'Refusing unexpected cleanup path.' }
    Remove-Item -LiteralPath $taskResolvedFixture -Recurse -Force
}
