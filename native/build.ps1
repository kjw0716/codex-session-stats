[CmdletBinding()]
param([string]$Compiler = 'g++.exe')
$ErrorActionPreference = 'Stop'
$taskCompiler = (Get-Command $Compiler -ErrorAction Stop).Source
$taskOutput = Join-Path (Split-Path $PSScriptRoot -Parent) 'SessionMonitor.exe'
& $taskCompiler -std=c++17 -O2 -static -s -D_WIN32_WINNT=0x0602 (Join-Path $PSScriptRoot 'monitor.cpp') -o $taskOutput -lwinhttp
if ($LASTEXITCODE -ne 0) { throw 'Native monitor compilation failed.' }
Write-Output $taskOutput
