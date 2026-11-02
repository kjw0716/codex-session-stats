[CmdletBinding()]
param(
    [switch]$ConfigureCodexEntry,
    [ValidateSet(0,1)][int]$AutoStart = 0,
    [string]$InstallDirectory = (Join-Path $env:LOCALAPPDATA 'CodexSessionStats'),
    [string]$DesktopPath = [Environment]::GetFolderPath('Desktop')
)
$ErrorActionPreference = 'Stop'
$taskFiles = @('SessionStats.exe','SessionMonitor.exe','Open-ChatGPT.exe','session-ui.js','Start-SessionStats.ps1','Stop-SessionStats.ps1','Start-EmbeddedStats.ps1','Stop-EmbeddedStats.ps1','Open-Codex.ps1','Install-Shortcuts.ps1','Start-SessionStats.cmd','Stop-SessionStats.cmd','assets/session-stats.ico')
foreach ($taskFile in $taskFiles) {
    if (-not (Test-Path -LiteralPath (Join-Path $PSScriptRoot $taskFile) -PathType Leaf)) { throw "Missing installation file: $taskFile" }
}
New-Item -ItemType Directory -Path $InstallDirectory,(Join-Path $InstallDirectory 'assets'),$DesktopPath -Force | Out-Null
# Windows shortcuts resolve 8.3 aliases (for example RUNNER~1) to long paths.
# Compare canonical paths so reinstalling does not create a duplicate shortcut.
$InstallDirectory = (Get-Item -LiteralPath $InstallDirectory).FullName
if ($InstallDirectory.TrimEnd('\') -ne (Get-Item -LiteralPath $PSScriptRoot).FullName.TrimEnd('\')) {
    foreach ($taskFile in $taskFiles) { Copy-Item -LiteralPath (Join-Path $PSScriptRoot $taskFile) -Destination (Join-Path $InstallDirectory $taskFile) -Force }
}
$taskState = Join-Path $env:LOCALAPPDATA 'CodexSessionMonitor'
New-Item -ItemType Directory -Path $taskState -Force | Out-Null
$taskPrefsFile = Join-Path $taskState 'preferences.json'
$taskPrefs = @{}
if (Test-Path -LiteralPath $taskPrefsFile) {
    try { (Get-Content -LiteralPath $taskPrefsFile -Raw | ConvertFrom-Json).PSObject.Properties | ForEach-Object { $taskPrefs[$_.Name] = $_.Value } } catch { }
}
if ($PSBoundParameters.ContainsKey('AutoStart') -or -not $taskPrefs.ContainsKey('autoStart')) {
    $taskPrefs['autoStart'] = ($AutoStart -eq 1)
}
$taskPrefs | ConvertTo-Json | Set-Content -LiteralPath $taskPrefsFile -Encoding UTF8
$taskShell = New-Object -ComObject WScript.Shell
$taskBackup = Join-Path $taskState ('shortcut-backups/' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
function Backup-TaskShortcut([string]$Path) {
    if (Test-Path -LiteralPath $Path) {
        New-Item -ItemType Directory -Path $taskBackup -Force | Out-Null
        Copy-Item -LiteralPath $Path -Destination $taskBackup
    }
}
$taskStatsLink = Join-Path $DesktopPath 'Session Stats.lnk'
Backup-TaskShortcut $taskStatsLink
$taskLink = $taskShell.CreateShortcut($taskStatsLink)
$taskLink.TargetPath = Join-Path $InstallDirectory 'SessionStats.exe'
$taskLink.WorkingDirectory = $InstallDirectory
$taskLink.IconLocation = (Join-Path $InstallDirectory 'assets/session-stats.ico') + ',0'
$taskLink.Description = 'Lightweight local Codex session usage - embedded or floating'
$taskLink.Save()
# Remove only the obsolete monitor launcher, after preserving its exact shortcut.
$taskOldLink = Join-Path $DesktopPath ('ChatGPT ' + [char]0x672c + [char]0x4f1a + [char]0x8bdd + [char]0x7edf + [char]0x8ba1 + '.lnk')
if (Test-Path -LiteralPath $taskOldLink) {
    $taskOld = $taskShell.CreateShortcut($taskOldLink)
    if ($taskOld.Arguments -like '*session-monitor*' -and $taskOld.Arguments -like '*.ps1*') { Backup-TaskShortcut $taskOldLink; Remove-Item -LiteralPath $taskOldLink }
}
if ($ConfigureCodexEntry) {
    $taskPackage = Get-AppxPackage -Name OpenAI.Codex | Sort-Object Version -Descending | Select-Object -First 1
    if (-not $taskPackage) { throw 'Codex desktop app is not installed. Session Stats shortcut was created.' }
    $taskAppLinkPath = Join-Path $DesktopPath 'ChatGPT.lnk'
    $taskKnownLink = $false
    if (Test-Path -LiteralPath $taskAppLinkPath) {
        $taskExisting = $taskShell.CreateShortcut($taskAppLinkPath)
        $taskItem = (New-Object -ComObject Shell.Application).Namespace($DesktopPath).ParseName('ChatGPT.lnk')
        $taskAppId = $taskItem.ExtendedProperty('System.AppUserModel.ID')
        $taskTargetId = $taskItem.ExtendedProperty('System.Link.TargetParsingPath')
        $taskLegacyEntry = $false
        if ($taskExisting.Arguments -match '(?i)(?:^|\s)-File\s+(?:"(?<script>[^\"]+)"|(?<script>\S+))') {
            $taskLegacyScript = Get-Item -LiteralPath $Matches['script'] -ErrorAction SilentlyContinue
            $taskLegacyEntry = $taskLegacyScript -and $taskLegacyScript.FullName -eq (Join-Path $InstallDirectory 'Open-Codex.ps1')
        }
        $taskLauncherEntry = $false
        if (-not [string]::IsNullOrWhiteSpace($taskExisting.TargetPath)) {
            $taskTargetFile = Get-Item -LiteralPath $taskExisting.TargetPath -ErrorAction SilentlyContinue
            $taskLauncherEntry = $taskTargetFile -and $taskTargetFile.FullName -eq (Join-Path $InstallDirectory 'Open-ChatGPT.exe')
        }
        $taskKnownLink = $taskAppId -like ($taskPackage.PackageFamilyName + '!*') -or $taskTargetId -like ($taskPackage.PackageFamilyName + '!*') -or $taskLegacyEntry -or $taskLauncherEntry
    } else { $taskKnownLink = $true }
    if (-not $taskKnownLink) { $taskAppLinkPath = Join-Path $DesktopPath 'ChatGPT (Session Ready).lnk' }
    Backup-TaskShortcut $taskAppLinkPath
    # A fresh shortcut must replace a Store link; changing its target in place retains its app identity.
    if (Test-Path -LiteralPath $taskAppLinkPath) { Remove-Item -LiteralPath $taskAppLinkPath }
    $taskAppLink = $taskShell.CreateShortcut($taskAppLinkPath)
    $taskAppLink.TargetPath = Join-Path $InstallDirectory 'Open-ChatGPT.exe'
    $taskAppLink.Arguments = ''
    $taskAppLink.WorkingDirectory = $InstallDirectory
    $taskAppIcon = Join-Path $taskPackage.InstallLocation 'app/resources/chatgpt-app-light.ico'
    if (Test-Path -LiteralPath $taskAppIcon) {
        Copy-Item -LiteralPath $taskAppIcon -Destination (Join-Path $InstallDirectory 'assets/chatgpt.ico') -Force
        $taskAppLink.IconLocation = (Join-Path $InstallDirectory 'assets/chatgpt.ico') + ',0'
    }
    $taskAppLink.Description = 'Open ChatGPT with local session statistics support'
    $taskAppLink.Save()
}
Write-Output 'Session Stats desktop shortcut installed. ChatGPT was not closed.'
