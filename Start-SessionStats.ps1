[CmdletBinding()]
param([switch]$WaitForClose, [switch]$Quiet)
$ErrorActionPreference = 'Stop'
$taskMonitorPort = 9335
$taskStateRoot = Join-Path $env:LOCALAPPDATA 'CodexSessionMonitor'
New-Item -ItemType Directory -Path $taskStateRoot -Force | Out-Null
$taskLaunchLog = Join-Path $taskStateRoot 'launcher.log'
$taskNativePath = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'SessionMonitor.exe'))
$taskLegacyPath = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'monitor.mjs'))
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
function Write-TaskStatus([string]$Text) {
    Add-Content -LiteralPath $taskLaunchLog -Value ((Get-Date).ToString('o') + ' ' + $Text) -Encoding utf8
    if (-not $Quiet) { Write-Host $Text }
}
function Get-TaskCdp {
    $taskListeners = @(Get-NetTCPConnection -State Listen -LocalPort $taskMonitorPort -ErrorAction SilentlyContinue)
    if (-not $taskListeners.Count) { return $false }
    if (@($taskListeners | Where-Object { $_.LocalAddress -ne '127.0.0.1' }).Count) { throw 'The statistics port is not bound exclusively to 127.0.0.1. Startup stopped.' }
    foreach ($taskListener in $taskListeners) {
        $taskOwner = Get-Process -Id $taskListener.OwningProcess -ErrorAction SilentlyContinue
        if ($taskOwner.ProcessName -ne 'ChatGPT') { throw 'Another application is using the statistics port. Startup stopped.' }
        $taskInstalledPackage = Get-AppxPackage -Name OpenAI.Codex | Sort-Object Version -Descending | Select-Object -First 1
        if (-not $taskInstalledPackage -or $taskOwner.Path -ne (Join-Path $taskInstalledPackage.InstallLocation 'app\ChatGPT.exe')) { throw 'The statistics port does not belong to the installed Codex desktop app. Startup stopped.' }
    }
    try {
        $taskTargets = @(Invoke-RestMethod -Uri "http://127.0.0.1:$taskMonitorPort/json/list" -TimeoutSec 3)
        return @($taskTargets | Where-Object { $_.type -eq 'page' -and $_.url -like 'app:*/index.html*' }).Count -gt 0
    } catch { return $false }
}
$taskMutex = [Threading.Mutex]::new($false, 'Local\CodexSessionMonitorLauncher')
$taskAcquired = $false
try {
    try { $taskAcquired = $taskMutex.WaitOne(0) } catch [Threading.AbandonedMutexException] { $taskAcquired = $true }
    if (-not $taskAcquired) { Write-TaskStatus 'Session Stats startup is already in progress.'; return }
    if (-not (Test-Path -LiteralPath $taskNativePath -PathType Leaf)) { throw 'SessionMonitor.exe is missing. Build the project or use a release containing the executable.' }
    if (-not (Get-TaskCdp)) {
        if (@(Get-Process -Name ChatGPT -ErrorAction SilentlyContinue).Count) {
            if (-not $WaitForClose) {
                Write-TaskStatus 'Quit Codex/ChatGPT normally, then run Start-SessionStats.ps1.'
                if (-not $Quiet) {
                    Add-Type -AssemblyName System.Windows.Forms
                    [void][System.Windows.Forms.MessageBox]::Show('Quit Codex/ChatGPT normally, then run Start-SessionStats.ps1. Your saved conversations are retained.', 'Session Stats')
                }
                return
            }
            Write-TaskStatus 'waiting-for-normal-exit'
            while (@(Get-Process -Name ChatGPT -ErrorAction SilentlyContinue).Count) { Start-Sleep -Seconds 2 }
        }
        $taskPackage = Get-AppxPackage -Name OpenAI.Codex | Sort-Object Version -Descending | Select-Object -First 1
        if (-not $taskPackage) { throw 'The Microsoft Store Codex desktop app (OpenAI.Codex) was not found.' }
        [xml]$taskManifest = Get-Content -LiteralPath (Join-Path $taskPackage.InstallLocation 'AppxManifest.xml') -Raw
        $taskApplication = @($taskManifest.Package.Applications.Application | Where-Object { $_.Executable -like '*ChatGPT.exe' }) | Select-Object -First 1
        if (-not $taskApplication) { throw 'The desktop app launch entry was not found in its package manifest.' }
        $taskAumid = $taskPackage.PackageFamilyName + '!' + $taskApplication.Id
        if (-not ('SessionMonitorActivation' -as [type])) {
            Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
[ComImport, Guid("2e941141-7f97-4756-ba1d-9decde894a3d"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface ISessionMonitorActivation {
    [PreserveSig] int ActivateApplication([MarshalAs(UnmanagedType.LPWStr)] string appId, [MarshalAs(UnmanagedType.LPWStr)] string arguments, uint options, out uint processId);
    [PreserveSig] int ActivateForFile([MarshalAs(UnmanagedType.LPWStr)] string appId, IntPtr items, [MarshalAs(UnmanagedType.LPWStr)] string verb, out uint processId);
    [PreserveSig] int ActivateForProtocol([MarshalAs(UnmanagedType.LPWStr)] string appId, IntPtr items, out uint processId);
}
public static class SessionMonitorActivation {
    public static uint Open(string appId, string arguments) {
        var type = Type.GetTypeFromCLSID(new Guid("45BA127D-10A8-46EA-8AB7-56EA9078943C"));
        var instance = (ISessionMonitorActivation)Activator.CreateInstance(type);
        uint processId; int result = instance.ActivateApplication(appId, arguments, 2, out processId);
        Marshal.ReleaseComObject(instance);
        Marshal.ThrowExceptionForHR(result); return processId;
    }
}
'@
        }
        [void][SessionMonitorActivation]::Open($taskAumid, '--remote-debugging-port=9335 --remote-debugging-address=127.0.0.1')
        Write-TaskStatus 'Opened the desktop app. Checking the local statistics endpoint.'
        $taskDeadline = (Get-Date).AddSeconds(45)
        while (-not (Get-TaskCdp)) {
            if ((Get-Date) -gt $taskDeadline) { throw 'The desktop app did not open its local statistics endpoint within 45 seconds. Startup stopped.' }
            Start-Sleep -Milliseconds 600
        }
    }
    $taskOwnedDaemons = @(Get-TaskOwnedDaemons)
    if (@($taskOwnedDaemons | Where-Object { $_.Backend -eq 'native' }).Count) { Write-TaskStatus 'Session Stats is already running.'; return }
    if (@($taskOwnedDaemons | Where-Object { $_.Backend -eq 'node' }).Count) { throw 'Legacy session stats are still running. Run Stop-SessionStats.ps1, then start again.' }
    $taskPreviousState = Join-Path $taskStateRoot 'status.json'
    $taskLaunchStartedAt = [DateTime]::UtcNow
    $taskDaemon = Start-Process -FilePath $taskNativePath -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $taskStateRoot 'monitor.log') -RedirectStandardError (Join-Path $taskStateRoot 'monitor-error.log')
    $taskDaemonDeadline = (Get-Date).AddSeconds(8)
    $taskConfirmed = $false
    do {
        Start-Sleep -Milliseconds 200
        $taskDaemon.Refresh()
        if ($taskDaemon.HasExited) { throw 'The statistics background process failed to start. Check monitor-error.log.' }
        try {
            $taskStateItem = Get-Item -LiteralPath $taskPreviousState -ErrorAction Stop
            $taskNewState = Get-Content -LiteralPath $taskPreviousState -Raw -ErrorAction Stop | ConvertFrom-Json
            $taskConfirmed = $taskStateItem.LastWriteTimeUtc -ge $taskLaunchStartedAt -and $taskNewState.pid -eq $taskDaemon.Id -and $taskNewState.backend -eq 'native' -and $taskNewState.port -eq $taskMonitorPort -and -not [string]::IsNullOrWhiteSpace([string]$taskNewState.phase)
        } catch { $taskConfirmed = $false }
    } while (-not $taskConfirmed -and (Get-Date) -lt $taskDaemonDeadline)
    if (-not $taskConfirmed) { throw 'The statistics background process was not confirmed within 8 seconds. Check monitor-error.log.' }
    Write-TaskStatus ('Session Stats started. Process ID: ' + $taskDaemon.Id)
} catch {
    Write-TaskStatus ('error: ' + $_.Exception.Message)
    if (-not $Quiet) {
        Add-Type -AssemblyName System.Windows.Forms
        [void][System.Windows.Forms.MessageBox]::Show($_.Exception.Message, 'Session Stats')
    }
} finally {
    if ($taskAcquired) { $taskMutex.ReleaseMutex() }
    $taskMutex.Dispose()
}
