[CmdletBinding()]
param([switch]$Quiet)
$ErrorActionPreference = 'Stop'
$taskNativePath = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'SessionMonitor.exe'))
$taskLegacyPath = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'monitor.mjs'))
$taskMonitorPort = 9335
function Get-TaskOwnedDaemons {
    foreach ($taskProcess in @(Get-CimInstance Win32_Process -Filter "Name='SessionMonitor.exe' OR Name='node.exe'" -ErrorAction Stop)) {
        if (-not $taskProcess.ExecutablePath -or -not $taskProcess.CommandLine) { continue }
        try { $taskExecutable = [IO.Path]::GetFullPath($taskProcess.ExecutablePath) } catch { continue }
        if ($taskExecutable -eq $taskNativePath) {
            $taskCommand = $taskProcess.CommandLine.Trim()
            if ($taskCommand -eq $taskNativePath -or $taskCommand -eq ('"' + $taskNativePath + '"')) {
                [pscustomobject]@{ ProcessId = [int]$taskProcess.ProcessId; Backend = 'native'; ExecutablePath = $taskExecutable }
            }
            continue
        }
        if ($taskProcess.Name -ne 'node.exe' -or [IO.Path]::GetFileName($taskExecutable) -ne 'node.exe') { continue }
        $taskArguments = [regex]::Match($taskProcess.CommandLine, '^\s*(?:"(?<exe>[^"]+)"|(?<exe>\S+))\s+(?:"(?<script>[^"]+)"|(?<script>\S+))\s*$')
        if (-not $taskArguments.Success) { continue }
        $taskExeArgument = $taskArguments.Groups['exe'].Value
        $taskScriptArgument = $taskArguments.Groups['script'].Value
        if (-not [IO.Path]::IsPathRooted($taskExeArgument) -or -not [IO.Path]::IsPathRooted($taskScriptArgument)) { continue }
        try {
            if ([IO.Path]::GetFullPath($taskExeArgument) -eq $taskExecutable -and [IO.Path]::GetFullPath($taskScriptArgument) -eq $taskLegacyPath) {
                [pscustomobject]@{ ProcessId = [int]$taskProcess.ProcessId; Backend = 'node'; ExecutablePath = $taskExecutable }
            }
        } catch { }
    }
}
function Test-TaskCdp {
    $taskListeners = @(Get-NetTCPConnection -State Listen -LocalPort $taskMonitorPort -ErrorAction SilentlyContinue)
    if (-not $taskListeners.Count) { return $false }
    if (@($taskListeners | Where-Object { $_.LocalAddress -ne '127.0.0.1' }).Count) { throw 'The statistics port is not bound exclusively to 127.0.0.1. UI cleanup was skipped.' }
    $taskPackage = Get-AppxPackage -Name OpenAI.Codex | Sort-Object Version -Descending | Select-Object -First 1
    if (-not $taskPackage) { throw 'The installed Codex desktop app was not found. UI cleanup was skipped.' }
    $taskExpectedPath = Join-Path $taskPackage.InstallLocation 'app\ChatGPT.exe'
    foreach ($taskListener in $taskListeners) {
        $taskOwner = Get-Process -Id $taskListener.OwningProcess -ErrorAction SilentlyContinue
        if (-not $taskOwner -or $taskOwner.ProcessName -ne 'ChatGPT' -or $taskOwner.Path -ne $taskExpectedPath) { throw 'The statistics port does not belong to the installed Codex desktop app. UI cleanup was skipped.' }
    }
    return $true
}
$taskMutex = [Threading.Mutex]::new($false, 'Local\CodexSessionMonitorLauncher')
$taskAcquired = $false
try {
    try { $taskAcquired = $taskMutex.WaitOne([TimeSpan]::FromSeconds(8)) } catch [Threading.AbandonedMutexException] { $taskAcquired = $true }
    if (-not $taskAcquired) { throw 'Session Stats startup is still in progress. Try Stop-SessionStats.ps1 again shortly.' }
    foreach ($taskDaemon in @(Get-TaskOwnedDaemons)) {
        $taskVerified = @(Get-TaskOwnedDaemons | Where-Object { $_.ProcessId -eq $taskDaemon.ProcessId -and $_.ExecutablePath -eq $taskDaemon.ExecutablePath -and $_.Backend -eq $taskDaemon.Backend })
        if (-not $taskVerified.Count) { continue }
        Stop-Process -Id $taskDaemon.ProcessId -ErrorAction Stop
        Wait-Process -Id $taskDaemon.ProcessId -Timeout 5 -ErrorAction SilentlyContinue
    }
    if (Test-TaskCdp) {
        if (-not (Test-Path -LiteralPath $taskNativePath -PathType Leaf)) { throw 'The statistics background process has stopped, but SessionMonitor.exe is missing. Quit Codex/ChatGPT normally to remove the statistics bar.' }
        $taskCleanup = Start-Process -FilePath $taskNativePath -ArgumentList '--remove' -WindowStyle Hidden -PassThru
        if (-not $taskCleanup.WaitForExit(8000)) { throw 'The statistics background process has stopped, but UI cleanup has not completed.' }
        if ($taskCleanup.ExitCode -ne 0) { throw 'The statistics background process has stopped, but UI cleanup failed. Quit Codex/ChatGPT normally to remove the statistics bar.' }
    }
    if (-not $Quiet) { Write-Host 'Session Stats is closed. The desktop app remains running.' }
} catch {
    if (-not $Quiet) {
        Add-Type -AssemblyName System.Windows.Forms
        [void][System.Windows.Forms.MessageBox]::Show($_.Exception.Message, 'Session Stats')
    }
    Write-Error $_.Exception.Message
} finally {
    if ($taskAcquired) { $taskMutex.ReleaseMutex() }
    $taskMutex.Dispose()
}
