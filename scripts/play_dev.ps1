# SPDX-License-Identifier: GPL-2.0-or-later

<#
.SYNOPSIS
Plays inFAMOUS Second Son from the source tree (port\launcher\infamous_run.py, the probe in port\out).

.DESCRIPTION
For a local MSYS2 build: the probe's DLLs (SDL3, FFmpeg, libc++...) stay in <MSYS2>\clang64\bin,
which this script puts in front of PATH for this process only. The game folder comes from
-GameDir, else ISS_GAME_DIR, else the launcher's saved settings (%APPDATA%\infamous-launcher\
settings.json game_dir), else <repo>\game\CUSA00223 when it exists. The data folder (saves, caches, tlg.ini) follows the launcher's rules
(ISS_USER_DIR, else port\user); logs go to port\out\logs. Other arguments are passed to the
launcher (e.g. a number of seconds after which the game closes). Close the game normally, or
create the file named by ISS_STOP_FILE.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\play_dev.ps1 -GameDir 'D:\infamous 1.07\infamoususa'
#>
param(
    [string]$GameDir,
    [string]$MsysBin = 'C:\msys64\clang64\bin',
    [string]$Python = 'py',
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$LauncherArgs
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$launcher = Join-Path $root 'port\launcher\infamous_run.py'
if (-not (Test-Path -LiteralPath $launcher)) { Write-Host "play_dev: $launcher not found" -ForegroundColor Red; exit 1 }

if (-not $GameDir) { $GameDir = $env:ISS_GAME_DIR }
if (-not $GameDir) {
    $settings = Join-Path $env:APPDATA 'infamous-launcher\settings.json'
    if (Test-Path -LiteralPath $settings) {
        try { $GameDir = "$((Get-Content -LiteralPath $settings -Raw -Encoding UTF8 | ConvertFrom-Json).game_dir)" } catch { }
    }
}
if (-not $GameDir -and (Test-Path -LiteralPath (Join-Path $root 'game\CUSA00223\eboot.bin'))) { $GameDir = Join-Path $root 'game\CUSA00223' }
if (-not $GameDir) {
    Write-Host "play_dev: no game folder. Pass -GameDir '<dump>\infamoususa' (the folder with eboot.bin) or set ISS_GAME_DIR." -ForegroundColor Red
    exit 2
}
if (-not (Test-Path -LiteralPath (Join-Path $GameDir 'eboot.bin'))) {
    Write-Host "play_dev: $GameDir has no eboot.bin (the game folder is the one with eboot.bin and sce_sys)" -ForegroundColor Red
    exit 2
}
$env:ISS_GAME_DIR = $GameDir

if (Test-Path -LiteralPath (Join-Path $MsysBin 'SDL3.dll')) { $env:PATH = "$MsysBin;$env:PATH" }
elseif (-not (Get-Command 'SDL3.dll' -ErrorAction SilentlyContinue)) {
    Write-Host "play_dev: warning: SDL3.dll is neither in $MsysBin nor on PATH (pass -MsysBin <msys2>\clang64\bin)" -ForegroundColor Yellow
}

$pre = @(); if ($Python -eq 'py') { $pre = @('-3') }
Write-Host "game folder: $GameDir"
Write-Host "launcher   : $launcher $($LauncherArgs -join ' ')"
Push-Location (Split-Path $launcher -Parent)
try { & $Python @pre $launcher @LauncherArgs; $code = $LASTEXITCODE } finally { Pop-Location }
exit $code
