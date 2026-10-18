[CmdletBinding()]
param([string]$Compiler = 'g++.exe')
$ErrorActionPreference = 'Stop'
$taskCompiler = (Get-Command $Compiler -ErrorAction Stop).Source
$taskOutput = Join-Path (Split-Path $PSScriptRoot -Parent) 'SessionMonitor.exe'
& $taskCompiler -std=c++17 -O2 -static -s -D_WIN32_WINNT=0x0602 (Join-Path $PSScriptRoot 'monitor.cpp') -o $taskOutput -lwinhttp
if ($LASTEXITCODE -ne 0) { throw 'Native monitor compilation failed.' }
Write-Output $taskOutput
$taskResourceCompiler = Join-Path (Split-Path $taskCompiler -Parent) 'windres.exe'
$taskResource = Join-Path $PSScriptRoot 'window-resource.o'
Push-Location $PSScriptRoot
try {
    & $taskResourceCompiler window.rc -o $taskResource
    if ($LASTEXITCODE -ne 0) { throw 'Icon resource compilation failed.' }
} finally { Pop-Location }
$taskPanel = Join-Path (Split-Path $PSScriptRoot -Parent) 'SessionStats.exe'
& $taskCompiler -std=c++17 -O2 -static -s -municode -mwindows -D_WIN32_WINNT=0x0602 (Join-Path $PSScriptRoot 'window.cpp') $taskResource -o $taskPanel -luser32 -lgdi32 -lshell32 -lcomctl32
if ($LASTEXITCODE -ne 0) { throw 'Native panel compilation failed.' }
Write-Output $taskPanel
