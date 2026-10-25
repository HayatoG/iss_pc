# SPDX-License-Identifier: GPL-2.0-or-later

<#
.SYNOPSIS
Automated, repeatable A/B benchmark of inFAMOUS Second Son on Windows (renderer v2).

.DESCRIPTION
  record   Launches the game once to record a route (-Seconds, default 90). Recording starts by
           itself when gameplay is detected and the game closes a few seconds after it ends.
           The whole savedata folder of the player's data folder is copied into the bench folder
           first: every later run starts from that same copy, never from the player's saves (the
           game's Continue loads the newest save in it).
  run      Replays the route: up to -WarmupRuns (2) warm-up runs per variant (discarded; they stop
           early once a run compiled no shaders; none with -NoWarmup; skipped for a variant whose
           cache slot's last run had the same build and settings and compiled nothing, unless
           -AlwaysWarmup), then rounds, each variant once per round, the order reversed every
           other round (A B, B A, A B): -Rounds N fixed, else adaptive, -MinRounds (3) to
           -MaxRounds (5), stopping early once every variant's paired avg_fps diff vs the control
           is clearly outside the noise or its 90% CI within +-EquivPct (1%). Each run gets its own
           data folder (a copy of the save) and its own launcher settings; the shader/pipeline
           caches are linked (junctions) to a cache copy of the bench (a persistent slot by
           default), so runs stay warm. Exit code 3 when a measured run did not end cleanly.
  compare  Table of the session: per variant mean +- stdev of avg_fps, low1_fps, p99_ms,
           over33..., % diff vs the control variant and '~' when the diff is within 2x the
           control's stdev (noise; only when that stdev is above 0). Uses
           scripts\round_analyze.py when Python is present, which also checks scene drift: a run
           whose mean draws/frame is more than -DriftPct (5) off the median of all runs rendered
           another scene (the replay diverged); its variant's diffs are marked unreliable ('!'),
           -ExcludeDrift leaves such runs out.

Caches: by default the benchmark never writes into the player's caches. Each variant (and the
recording, as '_record') has its own copy under <BenchDir>\cache\<key>\, seeded on first use
by copying the player's caches (cache, gpu-0..gpu-19, user\cache; read only, nothing is moved)
and reused by later runs and sessions. The key is the variant's slot by position (-CacheMode
slot, the default: _slot0 for the control, _slot1, ... ; a variant that sets a shader-profile
ISS_TUNING key gets _slotN-p<hash>), its name (-CacheMode variant, or the variant key
'bench:cache' = 'variant'), or a name of its own ('bench:cache' = '<name>'). A slot keeps the
entries of every variant that used it: give a variant that generates other shaders or
pipelines its own name. -ResetBenchCache deletes and seeds again the copies this command uses.
-SharePlayerCache links the runs to the player's real caches instead (warm without a copy, but
every run writes there); it refuses variants that change the shader profile (ISS_TUNING keys
lds_barriers_multi_wave, wave_memory_barriers: the game would move the player's cache aside to
<serial>.incompatible and drop the previous one) or move the cache (ISS_PIPELINE_CACHE_ROOT).

Layout of -BenchDir (default %USERPROFILE%\iss-bench):
  route.txt                    the recorded route (TLG_PAD_REPLAY format)
  save\                        savedata, tlg.ini and launcher settings.json the route starts from
  cache\<key>\                 a cache copy: _slotN or a variant/bench:cache name (seed.json: seeded
                               from where, when; slot.json: its last run, build/settings hash, compiles)
  cache\_record\               the recording's own caches
  cache\_shared\               -SharePlayerCache: the caches the player's folder does not have
  runs\<session>\variants.json the variants of the session, control first, and 'build': the git
                               commit/branch/dirty state of this tree and the probe exe (time, SHA256)
  runs\<session>\NN-<variant>\ bench.json, game.log, launcher-*.txt, env.txt, run.json
  bench.lock                   held while record/run is active (one session per bench folder)

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\bench.ps1 record -Exe 'C:\Games\inFAMOUS\inFAMOUS Second Son.exe'
.EXAMPLE
  .\scripts\bench.ps1 run -Exe '...\inFAMOUS Second Son.exe' -Variants ([ordered]@{ A = @{}; B = @{ ISS_TUNING = 'x=1' } })
.EXAMPLE
  .\scripts\bench.ps1 run -VariantFile variants.json -Rounds 5 -DryRun
.EXAMPLE
  .\scripts\bench.ps1 compare
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)][string]$Mode,
    [string]$BenchDir = (Join-Path $env:USERPROFILE 'iss-bench'),
    # The launcher ("inFAMOUS Second Son.exe", run with --play) or port\launcher\infamous_run.py.
    [string]$Exe,
    # The player's data folder the save is copied from (record) and the caches are seeded from.
    # Default: the launcher's rules (ISS_USER_DIR, %APPDATA%\inFAMOUS-PC\launcher.json, <exe>\user).
    [string]$UserDir,
    # The game folder (ISS_GAME_DIR for every run). Default: ISS_GAME_DIR, else the launcher's settings.
    [string]$GameDir,
    # Source tree (port\launcher\infamous_run.py): the MSYS2 folder with the probe's DLLs, put in
    # front of PATH for this process when SDL3.dll is not on PATH yet.
    [string]$MsysBin = 'C:\msys64\clang64\bin',
    [int]$Seconds = 90,
    # Fixed number of rounds. Without it the rounds are adaptive: -MinRounds to -MaxRounds.
    [int]$Rounds = 3,
    # Adaptive rounds: after each round from -MinRounds on, stop once every test variant's paired
    # avg_fps diff vs the control is clearly outside the noise or clearly within +-EquivPct.
    [int]$MinRounds = 3,
    [int]$MaxRounds = 5,
    # Adaptive: "no change" when the 90% CI of the paired diff lies within +-this %.
    [double]$EquivPct = 1.0,
    # Adaptive: the per-run noise (CV of avg_fps, %) assumed at least (A/A sessions: about 1.2%);
    # a "clear difference" must exceed -NoiseK x max(this, the control's own CV).
    [double]$NoiseCvPct = 1.2,
    # Adaptive: the stdev of the per-round paired diff (%) assumed at least (a few lucky rounds that
    # agree closely must not end the session).
    [double]$PairSdFloorPct = 0.5,
    # Caches: 'slot' (default) maps variants by position onto persistent slots reused by every
    # session (<BenchDir>\cache\_slot0 = control, _slot1, ...); 'variant' = one copy per variant name.
    # A variant can pick its own with the key 'bench:cache' = 'variant' or '<name>'.
    [ValidateSet('slot', 'variant')][string]$CacheMode = 'slot',
    # run: never skip the warm-up of a variant whose cache slot is known to be warm.
    [switch]$AlwaysWarmup,
    # Variant name -> @{ VAR = 'value' }; '' or $null unsets VAR. Keys 'ini:<key>' set tlg.ini keys.
    [System.Collections.IDictionary]$Variants,
    # The same as JSON: { "A": {}, "B": { "ISS_TUNING": "x=1" } } (file order kept).
    [string]$VariantFile,
    # The variant the others are compared with (default: the first one).
    [string]$Control,
    [switch]$NoWarmup,
    # Warm-up runs per variant at most; they stop early once a run compiled no shaders.
    [int]$WarmupRuns = 2,
    [int]$TimeoutSec = 600,
    # run: the session folder name under <BenchDir>\runs (default: a timestamp).
    # compare: a session name or folder (default: the newest session).
    [string]$Session,
    # tlg.ini keys for every run (record included), merged over the defaults vsync=0, fps_limit=0
    # (no VSync, no FPS limit); '' or $null drops a key (the save's tlg.ini value applies).
    [System.Collections.IDictionary]$Ini,
    # ISS_BENCH_SKIP_MS (the game's default is 1000 ms).
    [int]$SkipMs = -1,
    # Also write SHADPS4_FRAME_CSV (renderer v2 per-frame CSV) into each run folder.
    [switch]$FrameCsv,
    # record: keep the game open after the route is recorded (close it yourself).
    [switch]$KeepOpen,
    # record: replace an existing route and save (the old ones move to <BenchDir>\old-<time>).
    [switch]$Force,
    [string]$Python,
    # compare: also print round_analyze.py's per-run 'compare' table for each variant.
    [switch]$Detail,
    [double]$NoiseK = 2.0,
    # compare: a run whose mean draws/frame is more than this % off the median of all runs drifted.
    [double]$DriftPct = 5.0,
    # compare: leave the drift runs out of the table (round_analyze.py --exclude-drift).
    [switch]$ExcludeDrift,
    # record/run: link the runs to the player's real caches instead of the bench's own copies.
    [switch]$SharePlayerCache,
    # record/run: delete the bench's cache copies this command uses and seed them again.
    [switch]$ResetBenchCache,
    [switch]$DryRun
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
$Invariant = [Globalization.CultureInfo]::InvariantCulture
$Utf8 = New-Object Text.UTF8Encoding $false   # no BOM: the launcher's json.loads rejects one
$ExeName = 'inFAMOUS Second Son.exe'
$GraceSec = 30
# Set by this script for every run; a variant may not set them.
$Reserved = @('ISS_PAD_REPLAY', 'ISS_PAD_REPLAY_AUTO', 'ISS_PAD_RECORD', 'ISS_PAD_RECORD_SECONDS', 'ISS_PAD_FILE',
    'ISS_BENCH_JSON', 'ISS_BENCH_LABEL', 'ISS_BENCH_EXIT', 'ISS_BENCH_SKIP_MS', 'ISS_STOP_FILE', 'ISS_FRAME_STATS',
    'ISS_USER_DIR', 'ISS_GPU_USER_DIR', 'SHADPS4_FRAME_CSV', 'APPDATA')
$Metrics = @('avg_fps', 'low1_fps', 'p99_ms', 'worst_ms', 'over33', 'over50', 'compiles', 'cpu_ms_per_frame')
$IniDefaults = [ordered]@{ vsync = '0'; fps_limit = '0' }
$LauncherWaitMs = 60000            # the launcher's exit after the game closed (then it is killed)
$script:Junctions = New-Object Collections.Generic.List[string]   # every junction this process made
$script:FailedRuns = 0
$script:Lock = $null

function Fail([string]$message) {
    $e = New-Object System.Exception $message
    $e.Data['bench'] = $true   # an expected error: the message alone, no stack trace
    throw $e
}
function Info([string]$message) { Write-Host $message }
function Note([string]$message) { Write-Host $message -ForegroundColor Yellow }
function Dry([string]$message) { Write-Host "[dry-run] $message" -ForegroundColor Cyan }
function Fmt([string]$format, $value) { [string]::Format($Invariant, $format, $value) }

function Show-Usage {
    Info @'
Usage: scripts\bench.ps1 <record|run|compare> [options]   (Get-Help scripts\bench.ps1 -Detailed)
  record  [-Exe <launcher>] [-UserDir <data folder>] [-Seconds 90] [-Force] [-KeepOpen]
  run     [-Exe <launcher>] [-Variants @{...} | -VariantFile <json>] [-NoWarmup] [-WarmupRuns 2]
          [-AlwaysWarmup] [-TimeoutSec 600] [-Session <name>] [-FrameCsv] [-SkipMs <ms>]
          rounds: [-Rounds N] (fixed) | [-MinRounds 3] [-MaxRounds 5] [-EquivPct 1]
          [-NoiseCvPct 1.2] [-PairSdFloorPct 0.5] (adaptive, the default)
          [-CacheMode slot|variant] (variant key 'bench:cache' = 'variant' | '<name>')
  compare [-Session <name|folder>] [-Control <variant>] [-Detail] [-Python <exe>] [-NoiseK 2]
          [-DriftPct 5] [-ExcludeDrift]
  caches  (record/run) [-ResetBenchCache] (delete and seed again the bench's own copies)
          [-SharePlayerCache] (use the player's real caches; refuses profile-changing variants)
  source  (record/run with port\launcher\infamous_run.py) [-GameDir <folder with eboot.bin>]
          [-MsysBin C:\msys64\clang64\bin] (added to PATH when SDL3.dll is not on it)
  common  [-BenchDir %USERPROFILE%\iss-bench] [-Ini @{ key='value' }] [-DryRun]
          (-Ini is merged over vsync=0 fps_limit=0; '' drops a key)
  exit    0 ok, 1 error, 2 usage, 3 a measured run did not end cleanly
'@
}

# ------------------------------------------------------------------------------------- helpers
function Write-Text([string]$path, [string]$text) { [IO.File]::WriteAllText($path, $text, $Utf8) }

function Write-Json([string]$path, $value) { Write-Text $path (($value | ConvertTo-Json -Depth 6) + "`n") }

function Read-Json([string]$path) {
    try { return (Get-Content -LiteralPath $path -Raw -Encoding UTF8 | ConvertFrom-Json) }
    catch { Fail "cannot read JSON $path`: $($_.Exception.Message)" }
}

# (New-Item -ItemType Directory -Force silently does nothing when a parent is a file; this throws.)
function New-Dir([string]$path) { [void][IO.Directory]::CreateDirectory($path) }

# PSCustomObject (ConvertFrom-Json) or any dictionary -> ordered dictionary, keys in their order.
function ConvertTo-Ordered($value, [string]$what) {
    $out = [ordered]@{}
    if ($null -eq $value) { return $out }
    if ($value -is [System.Collections.IDictionary]) {
        $keys = @($value.Keys)
        if (-not ($value -is [System.Collections.Specialized.OrderedDictionary])) { $keys = @($keys | Sort-Object) }
        foreach ($k in $keys) { $out[[string]$k] = $value[$k] }
    } elseif ($value -is [System.Management.Automation.PSCustomObject]) {
        foreach ($p in $value.PSObject.Properties) { $out[$p.Name] = $p.Value }
    } else {
        Fail "$what must be a hashtable / JSON object, not '$value'"
    }
    return $out
}

# -Ini over the defaults; '' / $null removes a key (then the save's tlg.ini value stays).
function Merge-Ini($base, $given, [string]$what) {
    $out = [ordered]@{}
    foreach ($k in $base.Keys) { $out[$k] = $base[$k] }
    $g = ConvertTo-Ordered $given $what
    foreach ($k in $g.Keys) {
        if ($null -eq $g[$k] -or "$($g[$k])" -eq '') { $out.Remove($k) } else { $out[$k] = $g[$k] }
    }
    return $out
}

# True when $path is $parent or inside it (full paths, case-insensitive); $false if unresolvable.
function Test-Inside([string]$path, [string]$parent) {
    try {
        $a = [IO.Path]::GetFullPath($path).TrimEnd('\') + '\'
        $b = [IO.Path]::GetFullPath($parent).TrimEnd('\') + '\'
        return $a.StartsWith($b, [StringComparison]::OrdinalIgnoreCase)
    } catch { return $false }
}

function Set-IniValues([string]$path, $values) {
    if ($values.Count -eq 0) { return }
    $lines = New-Object Collections.Generic.List[string]
    if (Test-Path -LiteralPath $path) {
        foreach ($l in [IO.File]::ReadAllLines($path, $Utf8)) { $lines.Add($l) }
    } else { $lines.Add('# tlg settings') }
    $seen = @{}
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $key = $lines[$i].Split('=')[0].Trim()
        if ($values.Contains($key)) { $lines[$i] = "$key=$($values[$key])"; $seen[$key] = 1 }
    }
    foreach ($k in $values.Keys) { if (-not $seen.ContainsKey($k)) { $lines.Add("$k=$($values[$k])") } }
    Write-Text $path (($lines -join "`n") + "`n")
}

# ------------------------------------------------------------------------- launcher and folders
function Find-Python {
    $candidates = @()
    if ($Python) { $candidates += , @($Python) } else { $candidates += , @('py', '-3'); $candidates += , @('python'); $candidates += , @('python3') }
    $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'   # native stderr is not an error
    try {
        foreach ($c in $candidates) {
            $cmd = Get-Command $c[0] -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
            if (-not $cmd) { continue }
            $pre = @(); if ($c.Count -gt 1) { $pre = @($c[1..($c.Count - 1)]) }
            try {
                $v = & $cmd.Source @pre -c 'import sys; print(sys.version_info[0])' 2>$null
                if ($LASTEXITCODE -eq 0 -and "$v".Trim() -eq '3') { return @{ Exe = $cmd.Source; Pre = $pre } }
            } catch { }
        }
    } finally { $ErrorActionPreference = $old }
    if ($Python) { Fail "-Python '$Python' is not a working Python 3" }
    return $null
}

function Resolve-Launcher {
    $root = Split-Path $PSScriptRoot -Parent
    $path = $Exe
    if (-not $path) {
        $tries = @((Join-Path $PSScriptRoot $ExeName), (Join-Path $root $ExeName))
        $stage = Join-Path $root 'port\out\stage'
        if (Test-Path -LiteralPath $stage) {
            $tries += @(Get-ChildItem -LiteralPath $stage -Directory | Sort-Object LastWriteTime -Descending |
                    ForEach-Object { Join-Path $_.FullName $ExeName })
        }
        $out = Join-Path $root 'port\out'
        if ((Test-Path (Join-Path $out 'tlg-probe-v2.exe')) -or (Test-Path (Join-Path $out 'tlg-probe.exe'))) {
            $tries += (Join-Path $root 'port\launcher\infamous_run.py')
        }
        $path = $tries | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
        if (-not $path) {
            $msg = "the game launcher was not found next to this script or in port\out. Pass -Exe " +
                "'<game folder>\$ExeName' (the script starts it with --play) or -Exe port\launcher\infamous_run.py."
            if ($DryRun) { Note "warning: $msg"; return @{ Exe = "<$ExeName>"; Kind = 'exe'; PortDir = '<game folder>'; Missing = $true } }
            Fail $msg
        }
    }
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { Fail "-Exe '$path' does not exist" }
    $full = (Resolve-Path -LiteralPath $path).Path
    if ($full -like '*.py') {
        # Source tree: port\launcher\infamous_run.py, PORT_DIR = port\.
        return @{ Exe = $full; Kind = 'py'; PortDir = (Split-Path (Split-Path $full -Parent) -Parent); Missing = $false }
    }
    return @{ Exe = $full; Kind = 'exe'; PortDir = (Split-Path $full -Parent); Missing = $false }
}

# The launcher's own rules (port/launcher/userdata.py user_dir), read before this script changes them.
function Resolve-SourceUserDir($launcher) {
    if ($UserDir) { return [IO.Path]::GetFullPath($UserDir) }
    foreach ($name in 'ISS_USER_DIR', 'TLG_USER_DIR') {
        $v = [Environment]::GetEnvironmentVariable($name, 'Process')
        if ($v) { return [IO.Path]::GetFullPath($v) }
    }
    $pointer = Join-Path $env:APPDATA 'inFAMOUS-PC\launcher.json'
    if (Test-Path -LiteralPath $pointer) {
        try {
            $p = Get-Content -LiteralPath $pointer -Raw -Encoding UTF8 | ConvertFrom-Json
            if ($p.PSObject.Properties['user_dir'] -and "$($p.user_dir)".Trim()) { return [IO.Path]::GetFullPath("$($p.user_dir)".Trim()) }
        } catch { Note "warning: unreadable $pointer, using the default data folder" }
    }
    return (Join-Path $launcher.PortDir 'user')
}

function Get-LauncherSettingsPath { Join-Path $env:APPDATA 'infamous-launcher\settings.json' }

# PATH and ISS_GAME_DIR as they were before Initialize-LaunchEnv (restored at the very end: a
# script run from a prompt shares that prompt's environment). Not part of Save-Env/Restore-Env,
# which reset to this baseline before every run.
$script:LaunchEnvSaved = $null
function Restore-LaunchEnv {
    if (-not $script:LaunchEnvSaved) { return }
    foreach ($k in @($script:LaunchEnvSaved.Keys)) { [Environment]::SetEnvironmentVariable($k, $script:LaunchEnvSaved[$k], 'Process') }
    $script:LaunchEnvSaved = $null
}

# The game folder and (source tree) the probe's DLLs, so a run starts without any manual setup:
# -GameDir, else ISS_GAME_DIR, else the launcher's settings game_dir, else <repo>\game\CUSA00223.
function Initialize-LaunchEnv($launcher) {
    $script:LaunchEnvSaved = @{ PATH = $env:PATH; ISS_GAME_DIR = $env:ISS_GAME_DIR }
    $game = $GameDir; $from = '-GameDir'
    if (-not $game -and $env:ISS_GAME_DIR) { $game = $env:ISS_GAME_DIR; $from = 'ISS_GAME_DIR' }
    if (-not $game) {
        $settings = Get-LauncherSettingsPath
        if (Test-Path -LiteralPath $settings) {
            try { $game = "$((Read-Json $settings).game_dir)"; $from = $settings } catch { }
        }
    }
    if (-not $game -and $launcher.Kind -eq 'py') {
        $tree = Join-Path (Split-Path $launcher.PortDir -Parent) 'game\CUSA00223'
        if (Test-Path -LiteralPath (Join-Path $tree 'eboot.bin')) { $game = $tree; $from = 'source tree default' }
    }
    if (-not $game) {
        $msg = "no game folder: pass -GameDir '<folder with eboot.bin>' or set ISS_GAME_DIR (or pick it once in the launcher)"
        if ($DryRun) { Note "warning: $msg" } else { Fail $msg }
    } else {
        if (-not (Test-Path -LiteralPath (Join-Path $game 'eboot.bin'))) {
            $msg = "the game folder $game ($from) has no eboot.bin"
            if ($DryRun) { Note "warning: $msg" } else { Fail $msg }
        }
        $env:ISS_GAME_DIR = $game
        $script:GameDirUsed = "$game ($from)"
    }
    if ($launcher.Kind -ne 'py') { return }
    $onPath = $false
    foreach ($d in "$env:PATH".Split(';')) { if ($d -and (Test-Path -LiteralPath (Join-Path $d 'SDL3.dll'))) { $onPath = $true; break } }
    if ($onPath) { return }
    if (Test-Path -LiteralPath (Join-Path $MsysBin 'SDL3.dll')) {
        $env:PATH = "$MsysBin;$env:PATH"
        Info "  PATH     : $MsysBin added in front (the probe's DLLs; this process only)"
    } else {
        $msg = "SDL3.dll is neither on PATH nor in ${MsysBin}: the probe would not start. Pass -MsysBin <msys2>\clang64\bin."
        if ($DryRun) { Note "warning: $msg" } else { Fail $msg }
    }
}
$script:GameDirUsed = "(not set)"

# What is being measured: the git state of this tree (commit, branch, uncommitted changes other
# than the submodules under port/gpu/third_party) and the probe executables (time, SHA256).
function Get-BuildInfo($launcher) {
    $root = Split-Path $PSScriptRoot -Parent
    $b = [ordered]@{ commit = $null; branch = $null; dirty = $null; dirty_files = @(); probes = @() }
    $git = Get-Command git -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($git) {
        $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
        try {
            $c = & $git.Source -C $root rev-parse HEAD 2>$null
            if ($LASTEXITCODE -eq 0 -and $c) {
                $b.commit = "$c".Trim()
                $b.branch = "$(& $git.Source -C $root rev-parse --abbrev-ref HEAD 2>$null)".Trim()
                $changes = @(& $git.Source -C $root status --porcelain 2>$null | Where-Object { $_ } |
                        Where-Object { $_.Substring(3).Trim('"') -notlike 'port/gpu/third_party/*' })
                $b.dirty = ($changes.Count -gt 0)
                $b.dirty_files = @($changes | Select-Object -First 20 | ForEach-Object { $_.Substring(3) })
            }
        } catch { } finally { $ErrorActionPreference = $old }
    }
    if ($launcher.Missing) { return $b }
    foreach ($n in 'tlg-probe-v2.exe', 'tlg-probe.exe') {
        $p = Join-Path $launcher.PortDir "out\$n"
        if (-not (Test-Path -LiteralPath $p -PathType Leaf)) { continue }
        $f = Get-Item -LiteralPath $p
        $hash = $null; try { $hash = (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash.ToLower() } catch { }
        $b.probes += , ([ordered]@{ path = $f.FullName; modified = $f.LastWriteTime.ToString('s'); bytes = $f.Length; sha256 = $hash })
    }
    return $b
}

function Format-BuildInfo($b) {
    $lines = @()
    if ($b.commit) {
        $state = if ($b.dirty) { "DIRTY ($(@($b.dirty_files).Count) changed: $((@($b.dirty_files) | Select-Object -First 5) -join ', '))" } else { 'clean' }
        $lines += "  commit   : $($b.commit) ($($b.branch)), tree $state"
    } else { $lines += '  commit   : unknown (not a git checkout, or git not found)' }
    foreach ($p in @($b.probes)) {
        $sha = if ($p.sha256) { $p.sha256.Substring(0, 16) } else { '?' }
        $lines += "  probe    : $($p.path) ($($p.modified), sha256 $sha...)"
    }
    return ($lines -join "`n")
}

# save\: the player's savedata, tlg.ini and launcher settings, as the route starts from them.
function New-SaveSnapshot([string]$source, [string]$dest) {
    $saves = Join-Path $source 'savedata'
    if (-not (Test-Path -LiteralPath $saves -PathType Container) -or
        -not (Get-ChildItem -LiteralPath $saves -Recurse -File -ErrorAction SilentlyContinue | Select-Object -First 1)) {
        Fail "no saves in $saves. Play to a quiet spot with the launcher first (or pass -UserDir <data folder>)."
    }
    New-Dir $dest
    Copy-Item -LiteralPath $saves -Destination (Join-Path $dest 'savedata') -Recurse -Force
    $ini = Join-Path $source 'tlg.ini'
    if (Test-Path -LiteralPath $ini) { Copy-Item -LiteralPath $ini -Destination (Join-Path $dest 'tlg.ini') -Force }
    $settings = Get-LauncherSettingsPath
    if (Test-Path -LiteralPath $settings) { Copy-Item -LiteralPath $settings -Destination (Join-Path $dest 'settings.json') -Force }
    Write-Json (Join-Path $dest 'snapshot.json') ([ordered]@{ source = $source; time = (Get-Date).ToString('s'); settings = $settings })
}

# Caches the game may create in its data folder: port renderer (cache\) and renderer v2 (gpu-0..gpu-19,
# one per shadPS4 PathType, gpu_v2/gpu/bridge.cpp). Shared even when the player's folder has none yet.
$CacheNames = @('cache') + @(0..19 | ForEach-Object { "gpu-$_" })
# The old Windows cache location (port/launcher/shader_cache.py cache_dirs): only when it exists.
$OldCacheName = 'user\cache'

# ISS_TUNING keys that are part of Shader::Profile (gpu_v2/gpu/overrides/vk_pipeline_cache.cpp):
# a cache made with another profile is moved aside by DataBase::Reset (cache_storage.cpp), which
# first deletes the previous <serial>.incompatible. Variables that move the cache folder elsewhere.
$ProfileTuningKeys = @('lds_barriers_multi_wave', 'wave_memory_barriers')
$CacheRootVars = @('ISS_PIPELINE_CACHE_ROOT', 'TLG_PIPELINE_CACHE_ROOT')

# The only folders a run links (as NTFS junctions): the shader/pipeline caches. By default they
# point into <BenchDir>\cache\<key>\ (the bench's own copies, Initialize-BenchCache); with
# -SharePlayerCache into the player's folder (else <BenchDir>\cache\_shared\). Everything else in
# the player's folder (saves and their backups, mods, dlc, screenshots...) is never linked:
# savedata is a copy, mods is read in place through ISS_MODS_DIR (Get-ModsVars), dlc gets a
# header-only copy (Copy-DlcHeaders), small top-level files are copied.
function Plan-Links([string]$source, [string]$key) {
    $links = [ordered]@{}
    if ($SharePlayerCache) {
        foreach ($n in $CacheNames) {
            $own = Join-Path $source $n
            if ([IO.Directory]::Exists($own)) { $links[$n] = $own } else { $links[$n] = Join-Path $BenchDir "cache\_shared\$n" }
        }
        $old = Join-Path $source $OldCacheName
        if ([IO.Directory]::Exists($old)) { $links[$OldCacheName] = $old }
        return $links
    }
    $root = Get-BenchCacheRoot $key
    foreach ($n in $CacheNames) { $links[$n] = Join-Path $root $n }
    if ([IO.Directory]::Exists((Join-Path $root $OldCacheName)) -or [IO.Directory]::Exists((Join-Path $source $OldCacheName))) {
        $links[$OldCacheName] = Join-Path $root $OldCacheName
    }
    return $links
}

function Get-BenchCacheRoot([string]$key) { Join-Path $BenchDir "cache\$key" }

# A variable as a run of this variant sees it: the variant's value ('' = unset), else the shell's
# (a TLG_ twin of an ISS_ variable the variant sets is unset, Set-RunEnv).
function Get-RunValue($envs, [string]$name) {
    if ($envs -and $envs.Contains($name)) { return "$($envs[$name])" }
    if ($name -like 'TLG_*' -and $envs -and $envs.Contains('ISS_' + $name.Substring(4))) { return '' }
    return "$([Environment]::GetEnvironmentVariable($name, 'Process'))"
}

# What in a run's environment changes the shader profile or the cache folder (-SharePlayerCache
# refuses these: they would reset or bypass the player's cache).
function Get-CacheRisks($envs) {
    $risks = @()
    foreach ($name in 'ISS_TUNING', 'TLG_TUNING') {
        $v = Get-RunValue $envs $name
        if (-not $v) { continue }
        foreach ($item in $v.Split(',')) {
            $k = $item.Split('=')[0].Trim()
            if ($ProfileTuningKeys -contains $k) { $risks += "$name sets $k (part of the shader profile: the game would move the player's cache aside)" }
        }
    }
    foreach ($name in $CacheRootVars) {
        $v = Get-RunValue $envs $name
        if ($v) { $risks += "$name=$v (moves the pipeline cache out of the run's data folder)" }
    }
    return $risks
}

# Checks the variants (or the recording, $set = @{ '_record' = @{ Env = @{} } }) against the cache mode.
function Assert-CacheMode($set, [string]$source) {
    if ($SharePlayerCache -and $ResetBenchCache) {
        Fail "-ResetBenchCache resets the bench's own cache copies; with -SharePlayerCache the runs use the player's caches (clear those with the launcher's 'Clear shader cache')"
    }
    foreach ($name in $set.Keys) {
        $envs = $set[$name].Env
        if ($SharePlayerCache) {
            $risks = @(Get-CacheRisks $envs)
            if ($risks.Count) {
                $who = if ($name -eq '_record') { 'the recording' } else { "variant '$name'" }
                Fail ("-SharePlayerCache: $who would change the player's real caches: " + ($risks -join '; ') +
                    ". Run without -SharePlayerCache (the bench then uses cache copies of its own).")
            }
            continue
        }
        foreach ($var in $CacheRootVars) {
            $v = Get-RunValue $envs $var
            if (-not $v) { continue }
            if ([IO.Path]::IsPathRooted($v) -and ((Test-Inside $v $source) -or (Test-Inside $source $v))) {
                Fail "'$name': $var=$v is the player's data folder or inside it; point it at a folder of its own"
            }
            $rel = if ([IO.Path]::IsPathRooted($v)) { '' } else { '; a relative path is resolved by the game against its own working folder' }
            Note "note: '$name': $var=$v keeps the pipeline cache there (not a bench copy, not seeded$rel)"
        }
    }
    if ($SharePlayerCache) {
        Note "warning: -SharePlayerCache: every run writes into the shader/pipeline caches of $source. A run killed on timeout can leave a half-written entry there; if the game then crashes while the cache loads, use the launcher's 'Clear shader cache'."
    }
}

# Bytes and files under a folder, reparse points not followed.
function Get-TreeSize([string]$path) {
    $bytes = 0L; $files = 0
    if (-not [IO.Directory]::Exists($path)) { return @{ Bytes = 0L; Files = 0 } }
    $stack = New-Object Collections.Generic.Stack[string]
    $stack.Push($path)
    while ($stack.Count) {
        $dir = New-Object IO.DirectoryInfo $stack.Pop()
        foreach ($e in $dir.GetFileSystemInfos()) {
            if (($e.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { continue }
            if (($e.Attributes -band [IO.FileAttributes]::Directory) -ne 0) { $stack.Push($e.FullName) }
            else { $bytes += $e.Length; $files++ }
        }
    }
    return @{ Bytes = $bytes; Files = $files }
}

function Format-Size([long]$bytes) {
    if ($bytes -ge 1GB) { return Fmt '{0:F2} GB' ($bytes / 1GB) }
    if ($bytes -ge 1MB) { return Fmt '{0:F1} MB' ($bytes / 1MB) }
    return Fmt '{0:F0} KB' ($bytes / 1KB)
}

# Deletes a folder of the bench (never anything outside -BenchDir) without ever going through a
# junction: it refuses when the folder is, or contains, a reparse point (junction, symbolic link),
# because Remove-Item -Recurse in Windows PowerShell 5.1 follows junctions into their targets.
function Remove-BenchTree([string]$path) {
    $full = [IO.Path]::GetFullPath($path).TrimEnd('\')
    if (-not (Test-Inside $full $BenchDir) -or $full -ieq $BenchDir.TrimEnd('\')) { Fail "refusing to delete ${full}: not inside -BenchDir $BenchDir" }
    if (-not [IO.Directory]::Exists($full)) {
        if ([IO.File]::Exists($full)) { Fail "refusing to delete ${full}: a file, not a folder" }
        return
    }
    if (Test-ReparsePoint $full) { Fail "refusing to delete ${full}: it is a junction or link; remove only the link with: cmd /c rmdir `"$full`"" }
    $links = @()
    $stack = New-Object Collections.Generic.Stack[string]
    $stack.Push($full)
    while ($stack.Count) {
        $dir = New-Object IO.DirectoryInfo $stack.Pop()
        foreach ($e in $dir.GetFileSystemInfos()) {
            if (($e.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { $links += $e.FullName; continue }
            if (($e.Attributes -band [IO.FileAttributes]::Directory) -ne 0) { $stack.Push($e.FullName) }
            elseif (($e.Attributes -band [IO.FileAttributes]::ReadOnly) -ne 0) { $e.Attributes = $e.Attributes -band (-bnot [IO.FileAttributes]::ReadOnly) }
        }
    }
    if ($links.Count) {
        Fail ("refusing to delete ${full}: it contains $($links.Count) junction(s)/link(s), which must not be followed: " +
            (($links | Select-Object -First 5) -join ', ') + ". Remove each link with cmd /c rmdir `"<link>`" first.")
    }
    [IO.Directory]::Delete($full, $true)
}

# The bench's own copy of the caches for one variant (or '_record'): reused when seed.json says
# it was seeded; otherwise (new, or a seeding that was interrupted) seeded by copying the
# player's cache folders (robocopy: read only on the source, junctions not followed, the
# <serial>.incompatible leftovers not copied). Nothing is ever written into the player's folder.
function Initialize-BenchCache([string]$key, [string]$source) {
    $root = Get-BenchCacheRoot $key
    $marker = Join-Path $root 'seed.json'
    $have = @(@($CacheNames) + @($OldCacheName) | Where-Object { [IO.Directory]::Exists((Join-Path $source $_)) })
    if ($ResetBenchCache -and [IO.Directory]::Exists($root)) {
        if ($DryRun) { Dry "cache '$key': -ResetBenchCache: delete $root" }
        else { Info "  cache '$key': -ResetBenchCache: deleting $root"; Remove-BenchTree $root }
    } elseif ([IO.File]::Exists($marker)) {
        $when = ''
        try { $when = " (seeded $((Read-Json $marker).time))" } catch { }
        if ($DryRun) { Dry "cache '$key': reuse $root$when" }
        return
    } elseif ([IO.Directory]::Exists($root)) {
        if ($DryRun) { Dry "cache '$key': $root has no seed.json (interrupted seeding): delete it and seed again" }
        else { Note "  cache '$key': $root has no seed.json (interrupted seeding?): deleting it and seeding again"; Remove-BenchTree $root }
    }
    $size = 0L; foreach ($n in $have) { $size += (Get-TreeSize (Join-Path $source $n)).Bytes }
    $what = if ($have.Count) { "copy $($have -join ', ') from $source ($(Format-Size $size))" } else { "start empty (the player's folder has no cache yet; the warm-up builds it)" }
    if ($DryRun) { Dry "cache '$key': seed ${root}: $what"; return }
    Info "  cache '$key': $what -> $root"
    New-Dir $root
    foreach ($n in $have) {
        $src = Join-Path $source $n; $dst = Join-Path $root $n
        New-Dir $dst
        $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
        try {
            $out = & robocopy.exe $src $dst /E /XJ /XD '*.incompatible' /COPY:DAT /DCOPY:T /R:1 /W:1 /MT:8 /NFL /NDL /NJH /NJS /NP 2>&1
            $code = $LASTEXITCODE
        } finally { $ErrorActionPreference = $old }
        if ($code -ge 8) { Fail "seeding the cache copy failed (robocopy exit $code): $src -> $dst`n$(@($out) -join "`n")" }
    }
    Write-Json $marker ([ordered]@{ source = $source; time = (Get-Date).ToString('s'); copied = $have; bytes = $size })
}

# The launcher reads mod XML files from ISS_MODS_DIR (port/launcher/infamous_run.py mods_dir, read
# only): the player's mods folder in place, unless this shell already sets ISS_MODS_DIR.
function Get-ModsVars([string]$source) {
    $v = [ordered]@{}
    $mods = Join-Path $source 'mods'
    if (-not $env:ISS_MODS_DIR -and [IO.Directory]::Exists($mods)) { $v['ISS_MODS_DIR'] = $mods }
    return $v
}

# <data folder>\dlc only gives the launcher the add-on labels (port/launcher/addons.py: the first bytes
# of each .pkg and sce_sys\param.sfo, depth <= 4); the same files, .pkg cut to their header, give the
# run the same ISS_ADDCONT_LABELS without copying or linking gigabytes.
function Copy-DlcHeaders([string]$src, [string]$dst, [int]$depth = 0) {
    if (-not [IO.Directory]::Exists($src)) { return }
    $sfo = Join-Path $src 'sce_sys\param.sfo'
    if ([IO.File]::Exists($sfo) -and (Get-Item -LiteralPath $sfo -Force).Length -le 1MB) {
        New-Dir (Join-Path $dst 'sce_sys')
        Copy-Item -LiteralPath $sfo -Destination (Join-Path $dst 'sce_sys\param.sfo') -Force
    }
    foreach ($e in @(Get-ChildItem -LiteralPath $src -Force -ErrorAction SilentlyContinue)) {
        if (-not $e.PSIsContainer -and $e.Extension -eq '.pkg') {
            New-Dir $dst
            $fs = [IO.File]::OpenRead($e.FullName)
            try { $buf = New-Object byte[] 4096; $n = $fs.Read($buf, 0, $buf.Length) } finally { $fs.Dispose() }
            $head = New-Object byte[] $n
            [Array]::Copy($buf, $head, $n)
            [IO.File]::WriteAllBytes((Join-Path $dst $e.Name), $head)
        } elseif ($e.PSIsContainer -and $depth -lt 4 -and $e.Name -ne 'sce_sys') {
            Copy-DlcHeaders $e.FullName (Join-Path $dst $e.Name) ($depth + 1)
        }
    }
}

function Test-ReparsePoint([string]$path) {
    try { return (([IO.File]::GetAttributes($path) -band [IO.FileAttributes]::ReparsePoint) -ne 0) }
    catch { return $false }   # missing
}

# Every junction is registered in $script:Junctions before it is made, so a throw or Ctrl+C at any
# point leaves nothing untracked: the per-run cleanup and the script's final cleanup remove them.
function New-Junction([string]$link, [string]$target) {
    $script:Junctions.Add($link)
    $ok = $false
    try { New-Item -ItemType Junction -Path $link -Value $target -ErrorAction Stop | Out-Null; $ok = $true } catch { }
    if (-not $ok) {
        $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
        try { & cmd.exe /c mklink /J "$link" "$target" > $null 2> $null; $ok = ($LASTEXITCODE -eq 0) }
        catch { $ok = $false } finally { $ErrorActionPreference = $old }
    }
    if (-not $ok -and -not (Test-ReparsePoint $link)) { [void]$script:Junctions.Remove($link) }
    return $ok
}

# Removes the tracked junctions without touching their targets (Directory.Delete, not
# Remove-Item -Recurse, which follows junctions in Windows PowerShell 5.1). A path that is not a
# reparse point (anymore) is dropped from the list and never deleted.
function Remove-Junctions {
    foreach ($j in @($script:Junctions)) {
        $done = $true
        try { if (Test-ReparsePoint $j) { [IO.Directory]::Delete($j, $false) } }
        catch {
            $done = $false
            Note "warning: could not remove the junction $j ($($_.Exception.Message)); delete it with: cmd /c rmdir `"$j`""
        }
        if ($done) { [void]$script:Junctions.Remove($j) }
    }
}

# Junctions left by a bench.ps1 that was killed (no cleanup ran): <BenchDir>\record-*\user\* and
# <BenchDir>\runs\<session>\<run>\user\* (and user\user\* for the old cache location).
function Find-StaleJunctions {
    $users = @()
    foreach ($d in @(Get-ChildItem -LiteralPath $BenchDir -Directory -Force -Filter 'record-*' -ErrorAction SilentlyContinue)) {
        $users += Join-Path $d.FullName 'user'
    }
    $runs = Join-Path $BenchDir 'runs'
    foreach ($s in @(Get-ChildItem -LiteralPath $runs -Directory -Force -ErrorAction SilentlyContinue)) {
        if (Test-ReparsePoint $s.FullName) { continue }
        foreach ($d in @(Get-ChildItem -LiteralPath $s.FullName -Directory -Force -ErrorAction SilentlyContinue)) {
            $users += Join-Path $d.FullName 'user'
        }
    }
    $found = @()
    foreach ($u in $users) {
        foreach ($base in @($u, (Join-Path $u 'user'))) {
            if (-not [IO.Directory]::Exists($base) -or (Test-ReparsePoint $base)) { continue }
            foreach ($e in [IO.Directory]::GetDirectories($base)) { if (Test-ReparsePoint $e) { $found += $e } }
        }
    }
    return $found
}

function Clear-StaleJunctions {
    $stale = @(Find-StaleJunctions)
    if (-not $stale.Count) { return }
    if ($DryRun) { foreach ($j in $stale) { Dry "remove the junction left by an interrupted session: $j" }; return }
    Note "warning: $($stale.Count) junction(s) left by an interrupted bench.ps1 session; removing them (their targets are kept):"
    foreach ($j in $stale) {
        try { [IO.Directory]::Delete($j, $false); Note "  removed $j" }
        catch { Note "  could not remove $j ($($_.Exception.Message)); delete it with: cmd /c rmdir `"$j`"" }
    }
}

# One record/run per bench folder at a time (the stale-junction cleanup must not hit a live session).
# The lock is released when this process ends, however it ends.
function Lock-BenchDir {
    New-Dir $BenchDir
    $p = Join-Path $BenchDir 'bench.lock'
    try { $script:Lock = [IO.File]::Open($p, [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None) }
    catch { Fail "another bench.ps1 record/run is using $BenchDir (locked $p); wait for it or use another -BenchDir" }
}

# The bench folder and the player's data folder must not contain each other: runs copy, link and
# clean up inside the bench folder.
function Assert-Separate([string]$source) {
    if ((Test-Inside $BenchDir $source) -or (Test-Inside $source $BenchDir)) {
        Fail "-BenchDir $BenchDir and the player's data folder $source must not be inside each other; pick another -BenchDir"
    }
}

# <run>\user (save copy + tlg.ini + cache junctions, $links from Plan-Links) and <run>\appdata
# (launcher settings, no extra backup folder: the launcher would mirror these runs' save backups
# there and prune the player's own).
function New-RunData([string]$runDir, [string]$snapshot, [string]$source, $iniValues, $links) {
    $user = Join-Path $runDir 'user'
    $appdata = Join-Path $runDir 'appdata'
    New-Dir $user
    Copy-Item -LiteralPath (Join-Path $snapshot 'savedata') -Destination (Join-Path $user 'savedata') -Recurse -Force
    $snapIni = Join-Path $snapshot 'tlg.ini'
    if (Test-Path -LiteralPath $snapIni) { Copy-Item -LiteralPath $snapIni -Destination (Join-Path $user 'tlg.ini') -Force }
    Set-IniValues (Join-Path $user 'tlg.ini') $iniValues
    if (Test-Path -LiteralPath $source) {   # small top-level files (mods.json...): copies
        foreach ($f in Get-ChildItem -LiteralPath $source -Force -File) {
            if ($f.Name -eq 'tlg.ini' -or $f.Name -like 'savedata*') { continue }
            if ($f.Length -gt 256MB) { Note "warning: not copied (over 256 MB): $($f.FullName)"; continue }
            Copy-Item -LiteralPath $f.FullName -Destination (Join-Path $user $f.Name) -Force
        }
    }
    Copy-DlcHeaders (Join-Path $source 'dlc') (Join-Path $user 'dlc')
    foreach ($name in $links.Keys) {
        $target = $links[$name]
        if (-not $SharePlayerCache -and -not (Test-Inside $target (Join-Path $BenchDir 'cache'))) {
            Fail "internal: a cache link outside $BenchDir\cache without -SharePlayerCache: $target"
        }
        if ([IO.File]::Exists($target)) { Note "warning: not linked: $target is a file, not a folder"; continue }
        New-Dir $target
        $link = Join-Path $user $name
        New-Dir (Split-Path $link -Parent)
        if (-not (New-Junction $link $target)) { Note "warning: could not link $link -> $target (not shared; NTFS needed)" }
    }
    New-Dir (Join-Path $appdata 'infamous-launcher')
    New-Dir (Join-Path $appdata 'inFAMOUS-PC')
    $settings = [ordered]@{}
    $snapSettings = Join-Path $snapshot 'settings.json'
    if (Test-Path -LiteralPath $snapSettings) { $settings = ConvertTo-Ordered (Read-Json $snapSettings) 'settings.json' }
    if ($iniValues.Contains('vsync')) { $settings['vsync'] = ("$($iniValues['vsync'])" -ne '0') }   # launch() writes it into tlg.ini
    Write-Json (Join-Path $appdata 'infamous-launcher\settings.json') $settings
    Write-Json (Join-Path $appdata 'inFAMOUS-PC\launcher.json') ([ordered]@{ user_dir = $user })
    return @{ User = $user; AppData = $appdata }
}

# ---------------------------------------------------------------------------------- environment
$script:SavedEnv = @{}
function Save-Env([string[]]$keys) {
    foreach ($k in $keys) {
        if (-not $script:SavedEnv.ContainsKey($k)) { $script:SavedEnv[$k] = [Environment]::GetEnvironmentVariable($k, 'Process') }
    }
}
function Restore-Env {
    foreach ($k in @($script:SavedEnv.Keys)) { [Environment]::SetEnvironmentVariable($k, $script:SavedEnv[$k], 'Process') }
}
# Applies one run's variables over the saved (original) environment; '' / $null unset.
function Set-RunEnv($vars) {
    $keys = @($vars.Keys)
    $twins = @($keys | Where-Object { $_ -like 'ISS_*' } | ForEach-Object { 'TLG_' + $_.Substring(4) })
    Save-Env ($keys + $twins)
    Restore-Env
    foreach ($t in $twins) { [Environment]::SetEnvironmentVariable($t, $null, 'Process') }   # the ISS_ name wins
    foreach ($k in $keys) {
        $v = $vars[$k]
        if ($null -eq $v -or "$v" -eq '') { [Environment]::SetEnvironmentVariable($k, $null, 'Process') }
        else { [Environment]::SetEnvironmentVariable($k, "$v", 'Process') }
    }
}

function Format-Env($vars) {   # the variables set; Format-Unset lists the ones removed
    (@($vars.Keys | Where-Object { "$($vars[$_])" -ne '' } | ForEach-Object { "$_=$($vars[$_])" })) -join "`n"
}
function Format-Unset($vars) { (@($vars.Keys | Where-Object { "$($vars[$_])" -eq '' })) -join ' ' }

function Format-Links([string]$source, [string]$key) {
    $links = Plan-Links $source $key
    $own = @($links.Keys | Where-Object { Test-Inside $links[$_] $source })
    $bench = @($links.Keys | Where-Object { $own -notcontains $_ })
    $mods = Get-ModsVars $source
    $extra = if ($mods.Count) { "mods read in place (ISS_MODS_DIR=$($mods['ISS_MODS_DIR']))" } else { 'mods: not shared' }
    $short = {   # gpu-0, gpu-1, ... gpu-19 -> gpu-0..gpu-19
        param($names)
        $gpu = @($names | Where-Object { $_ -like 'gpu-*' })
        if ($gpu.Count -lt 20) { return ($names -join ', ') }
        $out = @(); $put = $false
        foreach ($n in $names) { if ($n -like 'gpu-*') { if (-not $put) { $out += 'gpu-0..gpu-19'; $put = $true } } else { $out += $n } }
        return ($out -join ', ')
    }
    $parts = @()
    if ($own.Count) { $parts += "player's real caches (the runs write there): $(& $short $own)" }
    if ($bench.Count) {
        $where = if ($SharePlayerCache) { Join-Path $BenchDir 'cache\_shared' } else { "$(Get-BenchCacheRoot $key) (the bench's copy)" }
        $parts += "$where`: $(& $short $bench)"
    }
    "caches only - $($parts -join '; '); $extra; dlc: header-only copy; nothing else is linked"
}

# ------------------------------------------------------------------------------------- the game
function Get-LogDir($launcher) { Join-Path $launcher.PortDir 'out\logs' }

# tlg-probe*.exe processes of this game folder (<PortDir>\out\...). -AnyPath also counts the ones
# whose path cannot be read (the "already running" check; never used to pick processes to kill).
function Get-Probes($launcher, [switch]$AnyPath) {
    $out = (Join-Path $launcher.PortDir 'out').TrimEnd('\') + '\'
    @(Get-Process -Name 'tlg-probe-v2', 'tlg-probe' -ErrorAction SilentlyContinue | Where-Object {
            $p = $null; try { $p = $_.Path } catch { }
            if (-not $p) { $AnyPath.IsPresent } else { $p.StartsWith($out, [StringComparison]::OrdinalIgnoreCase) } })
}

# Adds the probes of this game folder started since the run began to $run.Probes (by id). Only
# these are ever stopped: a probe that was already running, or one of another folder, is not.
function Update-Probes($run) {
    foreach ($p in Get-Probes $run.Launcher) {
        if ($run.Probes.ContainsKey($p.Id)) { continue }
        $t = $null; try { $t = $p.StartTime } catch { }
        if ($t -and $t -ge $run.Start.AddSeconds(-1)) {
            try { $null = $p.Handle } catch { }   # pins the process (no id reuse) and its exit state
            $run.Probes[$p.Id] = $p
        }
    }
}

function Get-LiveProbes($run) {
    @($run.Probes.Values | Where-Object { $alive = $false; try { $alive = -not $_.HasExited } catch { }; $alive })
}

function Read-NewLines($tail) {
    if (-not $tail.Path) { return @() }
    try {
        $fs = [IO.File]::Open($tail.Path, [IO.FileMode]::Open, [IO.FileAccess]::Read,
            [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
        try {
            if ($fs.Length -lt $tail.Pos) { $tail.Pos = 0 }
            if ($fs.Length -eq $tail.Pos) { return @() }
            [void]$fs.Seek($tail.Pos, [IO.SeekOrigin]::Begin)
            $buf = New-Object byte[] ([int][Math]::Min($fs.Length - $tail.Pos, 64MB))
            $n = $fs.Read($buf, 0, $buf.Length)
            $tail.Pos += $n
            $text = $tail.Rest + [Text.Encoding]::UTF8.GetString($buf, 0, $n)
        } finally { $fs.Dispose() }
    } catch { return @() }
    $parts = $text -split "`r?`n"
    $tail.Rest = $parts[-1]
    if ($parts.Count -le 1) { return @() }
    return $parts[0..($parts.Count - 2)]
}

# The newest run-*.log that did not exist before the start (the game's log of this run).
function Find-RunLog($logDir, $before, $tail, $r) {
    if ($tail.Path -or -not (Test-Path -LiteralPath $logDir)) { return }
    $new = Get-ChildItem -LiteralPath $logDir -Filter 'run-*.log' | Where-Object { -not $before.ContainsKey($_.Name) } |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($new) { $tail.Path = $new.FullName; $r.log = $new.FullName; Info "  log: $($new.FullName)" }
}

function Read-GameLog($tail, $r, $marks, [datetime]$now) {
    foreach ($line in Read-NewLines $tail) {
        if ($line -match 'Runtime: gameplay detected') { $r.gameplay = $true; Info '  gameplay detected' }
        elseif ($line -match 'Runtime: pad recording started') { Info "  recording the route ($Seconds s)..." }
        elseif ($line -match 'Runtime: pad recording stopped') { $marks.recorded = $now; Info '  route recorded' }
        elseif ($line -match 'Runtime: pad replay file empty or missing') {
            $r.status = 'empty-route'; $marks.empty = $now; Note "  the route is empty or unreadable ($line)"
        }
        elseif ($line -match 'Runtime: pad replay of (\d+) samples') {
            $r.replay_samples = [int]$Matches[1]; Info "  replaying $($Matches[1]) route samples"
            if ($r.replay_samples -eq 0) { $r.status = 'empty-route'; $marks.empty = $now; Note '  the route is empty or unreadable (0 samples)' }
        }
        elseif ($line -match 'Runtime: pad replay finished') { $marks.replayed = $now; Info '  route finished' }
        elseif ($line -match '(Bench: frames=.*)$') { $r.bench_line = $Matches[1]; $marks.bench = $now; Info "  $($Matches[1])" }
        elseif ($line -match '(Bench: .*)$') { Info "  $($Matches[1])" }   # window started, cut, cannot write...
    }
}

# @{ Process; Stop; Launcher; Start; Probes } while a game runs (Ctrl+C / a throw: Stop-Running).
$script:Running = $null

# The stop file, then up to $GraceSec for the launcher and the probes it started to exit, then kills
# whatever of them is left. Probes started meanwhile are picked up while waiting.
function Stop-Game($run) {
    $proc = $run.Process
    if ($run.Stop -and -not (Test-Path -LiteralPath $run.Stop)) {
        try { Write-Text $run.Stop "stop`n" } catch { Note "  could not write the stop file $($run.Stop): $($_.Exception.Message)" }
    }
    $deadline = (Get-Date).AddSeconds($GraceSec)
    while ($true) {
        Update-Probes $run
        if ($proc.HasExited -and -not @(Get-LiveProbes $run).Count) { return }
        if ((Get-Date) -ge $deadline) { break }
        Start-Sleep -Milliseconds 500
    }
    # Kill first, report after: the report itself may fail (closed console) during a cleanup.
    $killed = @()
    if (-not $proc.HasExited) { try { $proc.Kill() } catch { }; $killed += 'the launcher' }
    foreach ($p in Get-LiveProbes $run) { try { $p.Kill() } catch { }; $killed += "$($p.ProcessName) (pid $($p.Id))" }
    if ($killed.Count) { Note "  killed after $GraceSec s without closing on the stop file: $($killed -join ', ')" }
}

function Stop-Running {
    $run = $script:Running
    if ($run) {
        $alive = $false
        try { $alive = -not $run.Process.HasExited } catch { }
        try { Update-Probes $run } catch { }
        if ($alive -or @(Get-LiveProbes $run).Count) {
            Note 'interrupted: stopping the game'
            Stop-Game $run
        }
    }
    $script:Running = $null
}

# Cleanup after a run, whatever happened: each step runs even when an earlier one throws.
function Invoke-RunCleanup {
    try { Stop-Running }
    finally {
        try { Restore-Env } finally { Remove-Junctions }
    }
}

# Starts the launcher with --play under the current environment and follows the game log.
# $mode 'record' closes the game after 'pad recording stopped'; 'run' after the Bench: line if the
# game did not exit by itself (ISS_BENCH_EXIT), or 30 s after 'pad replay finished' without one.
function Invoke-Game($launcher, [string]$runDir, [string]$stopFile, [int]$timeout, [string]$mode) {
    $logDir = Get-LogDir $launcher
    $before = @{}
    if (Test-Path -LiteralPath $logDir) { Get-ChildItem -LiteralPath $logDir -Filter 'run-*.log' | ForEach-Object { $before[$_.Name] = 1 } }
    $out = Join-Path $runDir 'launcher-out.txt'; $err = Join-Path $runDir 'launcher-err.txt'
    if ($launcher.Kind -eq 'py') {
        $py = Find-Python
        if (-not $py) { Fail 'Python 3 is needed to run port\launcher\infamous_run.py (pass -Python)' }
        $file = $py.Exe; $argList = (@($py.Pre) + @("`"$($launcher.Exe)`"")) -join ' '
    } else { $file = $launcher.Exe; $argList = '--play' }
    $start = Get-Date
    $proc = Start-Process -FilePath $file -ArgumentList $argList -WorkingDirectory (Split-Path $launcher.Exe -Parent) `
        -RedirectStandardOutput $out -RedirectStandardError $err -PassThru
    $null = $proc.Handle   # keeps the exit code readable after the exit
    $run = @{ Process = $proc; Stop = $stopFile; Launcher = $launcher; Start = $start; Probes = @{} }
    $script:Running = $run
    $tail = @{ Path = $null; Pos = 0L; Rest = '' }
    $r = [ordered]@{ status = 'ok'; exit_code = $null; seconds = 0; log = $null; bench_line = $null;
        gameplay = $false; replay_samples = $null; renderer = $null; stopped_by = $null }
    $marks = @{}
    while (-not $proc.HasExited) {
        Start-Sleep -Milliseconds 1000
        $now = Get-Date
        Update-Probes $run
        Find-RunLog $logDir $before $tail $r
        Read-GameLog $tail $r $marks $now
        $elapsed = ($now - $start).TotalSeconds
        $why = $null
        if ($mode -eq 'run' -and $marks.ContainsKey('empty')) { $why = 'empty route' }   # nothing to measure
        elseif ($elapsed -gt $timeout) { $why = 'timeout'; $r.status = 'timeout' }
        elseif ($mode -eq 'record' -and -not $KeepOpen -and $marks.ContainsKey('recorded') -and ($now - $marks.recorded).TotalSeconds -gt 3) { $why = 'recorded' }
        elseif ($mode -eq 'run' -and $marks.ContainsKey('bench') -and ($now - $marks.bench).TotalSeconds -gt $GraceSec) { $why = 'bench line, no exit' }
        elseif ($mode -eq 'run' -and $marks.ContainsKey('replayed') -and -not $marks.ContainsKey('bench') -and ($now - $marks.replayed).TotalSeconds -gt $GraceSec) {
            $why = 'replay finished, no Bench line'; $r.status = 'no-bench-line'
        }
        if ($why) {
            $r.stopped_by = $why
            if ($why -eq 'timeout') { Note "  timeout ($timeout s): stopping the game" }
            elseif ($why -eq 'empty route') { Note '  stopping the game (empty route: nothing to replay)' }
            else { Info "  closing the game ($why)" }
            Stop-Game $run
            break
        }
    }
    if (-not $proc.WaitForExit($LauncherWaitMs)) {
        Note "  the launcher did not exit within $($LauncherWaitMs / 1000) s: killing it"
        try { $proc.Kill() } catch { }
        [void]$proc.WaitForExit(10000)
        if ($r.status -eq 'ok') { $r.status = 'launcher hung' }
    }
    if ($proc.HasExited) { $r.exit_code = $proc.ExitCode }
    $r.seconds = [Math]::Round(((Get-Date) - $start).TotalSeconds, 1)
    Update-Probes $run
    if (@(Get-LiveProbes $run).Count) { Note '  the game was still running after the launcher exited: stopping it'; Stop-Game $run }
    $script:Running = $null
    # A run that failed fast may have exited before the first look at the log folder.
    Find-RunLog $logDir $before $tail $r
    Read-GameLog $tail $r $marks (Get-Date)
    if (Test-Path -LiteralPath $out) {
        $o = Get-Content -LiteralPath $out -Raw -ErrorAction SilentlyContinue
        if ($o -match 'Renderer v2') { $r.renderer = 'v2' } elseif ($o) { $r.renderer = 'port (v1)' }
    }
    if (-not $r.log -and $r.status -eq 'ok') { $r.status = 'no-log' }
    if ($r.status -eq 'ok' -and $r.exit_code -ne 0 -and -not $r.stopped_by) { $r.status = "exit $($r.exit_code)" }
    if ($r.status -ne 'ok') {
        Note "  status: $($r.status) (exit code $($r.exit_code))"
        if (Test-Path -LiteralPath $err) {
            $e = @(Get-Content -LiteralPath $err -Tail 8 -ErrorAction SilentlyContinue)
            if ($e.Count) { Note ('  launcher said: ' + ($e -join "`n    ")) }
        }
    }
    return $r
}

# 'Bench: frames=... label=...' -> ordered dictionary (bench JSON keys); $null if malformed.
function ConvertFrom-BenchLine([string]$line) {
    if (-not $line -or $line -notmatch '^Bench: (.*?)(?:\s+label=(.*?))?\s*$') { return $null }
    $label = $Matches[2]
    $d = [ordered]@{}
    foreach ($tok in ($Matches[1] -split '\s+')) {
        $kv = $tok.Split('=', 2)
        if ($kv.Count -ne 2) { continue }
        $num = 0.0
        if ([double]::TryParse($kv[1], [Globalization.NumberStyles]::Float, $Invariant, [ref]$num)) { $d[$kv[0]] = $num } else { $d[$kv[0]] = $kv[1] }
    }
    if (-not $d.Contains('avg_fps')) { return $null }
    if ($null -eq $label -or $label -eq '-') { $label = '' }   # '-': the game's "no ISS_BENCH_LABEL"
    $d['label'] = $label
    $d['source'] = 'log_bench'
    return $d
}

# -------------------------------------------------------------------------------------- variants
function Get-VariantSet {
    if ($Variants -and $VariantFile) { Fail 'pass -Variants or -VariantFile, not both' }
    if ($VariantFile) {
        if (-not (Test-Path -LiteralPath $VariantFile)) { Fail "-VariantFile '$VariantFile' does not exist" }
        $raw = ConvertTo-Ordered (Read-Json $VariantFile) '-VariantFile'
    } elseif ($Variants) {
        if (-not ($Variants -is [System.Collections.Specialized.OrderedDictionary]) -and $Variants.Count -gt 1) {
            Note 'note: a @{...} hashtable has no order; variants sorted by name. Use [ordered]@{...} or -Control.'
        }
        $raw = ConvertTo-Ordered $Variants '-Variants'
    } else {
        Note 'note: no -Variants / -VariantFile: an A/A run of the same settings (measures the noise).'
        $raw = [ordered]@{ A = @{}; A2 = @{} }
    }
    if ($raw.Count -eq 0) { Fail 'no variants' }
    $set = [ordered]@{}
    foreach ($name in $raw.Keys) {
        if ($name -notmatch '^[A-Za-z0-9][A-Za-z0-9_.-]{0,40}$') { Fail "variant name '$name': use letters, digits, _ . - (it names folders and ISS_BENCH_LABEL)" }
        $vars = ConvertTo-Ordered $raw[$name] "variant '$name'"
        $envs = [ordered]@{}; $ini = [ordered]@{}; $cache = $null
        foreach ($k in $vars.Keys) {
            $v = $vars[$k]
            if ($null -ne $v -and -not ($v -is [string] -or $v -is [ValueType])) { Fail "variant '$name': $k must be a string or number" }
            if ($k -like 'ini:*') { $ini[$k.Substring(4)] = $v; continue }
            if ($k -like 'bench:*') {
                if ($k -ne 'bench:cache') { Fail "variant '$name': unknown key '$k' (bench:cache is the only bench: key)" }
                $cache = "$v".Trim()
                if ($cache -and $cache -ne 'variant' -and $cache -notmatch '^[A-Za-z0-9][A-Za-z0-9_.-]{0,40}$') {
                    Fail "variant '$name': bench:cache '$cache': 'variant' (a copy named after the variant) or a name of letters, digits, _ . -"
                }
                continue
            }
            if ($k -notmatch '^[A-Za-z_][A-Za-z0-9_]*$') { Fail "variant '$name': '$k' is not an environment variable name" }
            $twin = $k -replace '^TLG_', 'ISS_'
            if ($Reserved -contains $k.ToUpper() -or $Reserved -contains $twin.ToUpper() -or $k -match '^(ISS|TLG)_USER_DIR$') {
                Fail "variant '$name': $k is set by bench.ps1 itself"
            }
            $envs[$k] = $v
        }
        $set[$name] = @{ Env = $envs; Ini = $ini; Cache = $cache }
    }
    if ($Control -and -not $set.Contains($Control)) { Fail "-Control '$Control' is not one of: $($set.Keys -join ', ')" }
    if ($Control) {
        $ordered = [ordered]@{}; $ordered[$Control] = $set[$Control]
        foreach ($k in $set.Keys) { if ($k -ne $Control) { $ordered[$k] = $set[$k] } }
        $set = $ordered
    }
    $noted = @{}
    foreach ($name in $set.Keys) {
        foreach ($k in $set[$name].Env.Keys) {
            if ($noted.ContainsKey($k)) { continue }
            $noted[$k] = 1
            $shell = [Environment]::GetEnvironmentVariable($k, 'Process')
            if ($shell) { Note "note: $k=$shell is set in this shell; variants that do not set $k inherit it" }
            if ($k -like 'ISS_*') {
                $t = 'TLG_' + $k.Substring(4)
                if ([Environment]::GetEnvironmentVariable($t, 'Process')) { Note "note: $t is set in this shell; it is unset for the runs so $k applies" }
            }
        }
    }
    return $set
}

# ------------------------------------------------------------------------------------- record
function Invoke-Record {
    if ($Seconds -lt 10) { Fail '-Seconds must be at least 10' }
    $launcher = Resolve-Launcher
    $source = Resolve-SourceUserDir $launcher
    Initialize-LaunchEnv $launcher
    $build = Get-BuildInfo $launcher
    $route = Join-Path $BenchDir 'route.txt'
    $save = Join-Path $BenchDir 'save'
    if (((Test-Path -LiteralPath $route) -or (Test-Path -LiteralPath $save)) -and -not $Force) {
        Fail "$BenchDir already has a route. Pass -Force to record a new one (the old route and save move to old-<time>\), or use another -BenchDir."
    }
    $timeout = $TimeoutSec
    if (-not $script:Bound.ContainsKey('TimeoutSec')) { $timeout = [Math]::Max(1800, $Seconds + 900) }
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $recDir = Join-Path $BenchDir "record-$stamp"
    $vars = [ordered]@{
        ISS_PAD_RECORD = (Join-Path $recDir 'route.txt'); ISS_PAD_RECORD_SECONDS = "$Seconds"
        ISS_FRAME_STATS = '1'; ISS_STOP_FILE = (Join-Path $recDir 'stop')
        ISS_USER_DIR = (Join-Path $recDir 'user'); APPDATA = (Join-Path $recDir 'appdata')
        ISS_PAD_REPLAY = $null; ISS_PAD_REPLAY_AUTO = $null; ISS_PAD_FILE = $null
        ISS_BENCH_JSON = $null; ISS_BENCH_LABEL = $null; ISS_BENCH_EXIT = $null; ISS_GPU_USER_DIR = $null
        TLG_USER_DIR = $null; TLG_GPU_USER_DIR = $null
    }
    $mods = Get-ModsVars $source
    foreach ($k in $mods.Keys) { $vars[$k] = $mods[$k] }
    if ($launcher.Kind -eq 'py') { $vars['PYTHONUSERBASE'] = (Join-Path $env:APPDATA 'Python') }
    Assert-Separate $source
    Assert-CacheMode @{ '_record' = @{ Env = $vars } } $source
    $cacheLine = if ($SharePlayerCache) { "shared with $source (-SharePlayerCache)" } else { "own copy in $(Get-BenchCacheRoot '_record') (seeded from $source)" }
    Info @"
Recording a $Seconds s benchmark route.
  launcher : $($launcher.Exe)
  game     : $script:GameDirUsed
$(Format-BuildInfo $build)
  saves of : $source
  bench    : $BenchDir
  caches   : $cacheLine
Before you start: the route begins where the newest save puts you (the whole savedata folder is copied). Play to a quiet, repeatable
spot first and let the game save there (then quit), because every run will press Cross through
the menus and take Continue. Then, in the game window:
  1. On the title screen choose Continue (press only Cross in the menus).
  2. Recording starts by itself when gameplay is detected; play the route for $Seconds s
     (walk/fly/fight the same way you would like to measure). Avoid pausing and menus.
  3. 'Route recorded' appears on screen; the game closes a few seconds later$(if ($KeepOpen) { ' (-KeepOpen: close it yourself)' } else { '' }).
Your own saves and settings are not touched: the game runs on a copy in $recDir.
"@
    if ($DryRun) {
        Clear-StaleJunctions
        if (-not $SharePlayerCache) { Initialize-BenchCache '_record' $source }
        Dry "copy the savedata folder, tlg.ini and launcher settings: $source -> $BenchDir\save.new"
        Dry "data folder for the game: $recDir\user; junctions: $(Format-Links $source '_record')"
        Dry "tlg.ini keys: $((Format-Env $Ini) -replace "`n", ', ')"
        Dry "environment:`n    $((Format-Env $vars) -replace "`n", "`n    ")"
        Dry "unset: $(Format-Unset $vars)"
        Dry "start: `"$($launcher.Exe)`" $(if ($launcher.Kind -eq 'exe') { '--play' } else { '(run with Python 3)' }) (timeout $timeout s)"
        Dry "then: $recDir\route.txt -> $route, save.new -> save"
        return
    }
    Lock-BenchDir
    if (@(Get-Probes $launcher -AnyPath).Count) { Fail 'the game is already running; close it first' }
    Clear-StaleJunctions
    if (-not $SharePlayerCache) { Initialize-BenchCache '_record' $source }
    New-Dir $recDir
    $snapNew = Join-Path $BenchDir 'save.new'
    Remove-BenchTree $snapNew   # a leftover of an interrupted record (a link under it is refused, never followed)
    New-SaveSnapshot $source $snapNew
    try {
        $null = New-RunData $recDir $snapNew $source $Ini (Plan-Links $source '_record')
        Set-RunEnv $vars
        Write-Text (Join-Path $recDir 'env.txt') ((Format-Env $vars) + "`n")
        Info 'Starting the game...'
        $r = Invoke-Game $launcher $recDir $vars.ISS_STOP_FILE $timeout 'record'
    } finally { Invoke-RunCleanup }
    if ($r.log) { Copy-Item -LiteralPath $r.log -Destination (Join-Path $recDir 'game.log') -Force -ErrorAction SilentlyContinue }
    $r['build'] = $build
    Write-Json (Join-Path $recDir 'run.json') $r
    $newRoute = $vars.ISS_PAD_RECORD
    if (-not (Test-Path -LiteralPath $newRoute) -or (Get-Item -LiteralPath $newRoute).Length -eq 0) {
        Fail "no route was recorded ($newRoute missing or empty; status $($r.status)). Was gameplay reached? See $recDir\game.log."
    }
    if ((Test-Path -LiteralPath $route) -or (Test-Path -LiteralPath $save)) {
        $old = Join-Path $BenchDir "old-$stamp"
        New-Dir $old
        foreach ($p in $route, $save) { if (Test-Path -LiteralPath $p) { Move-Item -LiteralPath $p -Destination $old } }
        Info "previous route and save moved to $old"
    }
    Copy-Item -LiteralPath $newRoute -Destination $route
    Move-Item -LiteralPath $snapNew -Destination $save
    $samples = @(Get-Content -LiteralPath $route).Count
    Info "Route saved: $route ($samples samples). Next: scripts\bench.ps1 run -Variants ..."
}

# ------------------------------------------------------------------------- cache slots, warm state
function Get-ShortHash([string]$text, [int]$len = 8) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { $h = $sha.ComputeHash($Utf8.GetBytes($text)) } finally { $sha.Dispose() }
    return (-join ($h | ForEach-Object { $_.ToString('x2') })).Substring(0, $len)
}

# The shader-profile items (ISS_TUNING keys of $ProfileTuningKeys) a run of this variant sees, sorted.
function Get-ProfileItems($envs) {
    $items = @()
    foreach ($name in 'ISS_TUNING', 'TLG_TUNING') {
        $v = Get-RunValue $envs $name
        if (-not $v) { continue }
        foreach ($item in $v.Split(',')) {
            $k = $item.Split('=')[0].Trim()
            if ($ProfileTuningKeys -contains $k) { $items += $item.Trim() }
        }
    }
    return @($items | Sort-Object -Unique)
}

# Variant -> @{ Key = <folder under cache\>; How = <why> }. Slots go by position (control first):
# the same position reuses the same warm copy in every session. A variant that sets shader-profile
# keys gets a slot of that profile (_slotN-p<hash>): another profile would make the game move the
# cache aside. 'bench:cache' = 'variant' / '<name>' and -CacheMode variant name the copy instead.
function Get-CacheKeys($set) {
    $out = [ordered]@{}; $i = 0; $owner = @{}
    foreach ($v in $set.Keys) {
        $c = $set[$v].Cache
        if ($c -eq 'variant' -or (-not $c -and $CacheMode -eq 'variant')) { $key = $v; $how = 'named after the variant' }
        elseif ($c) { $key = $c; $how = 'bench:cache' }
        else {
            $key = "_slot$i"; $how = "slot $i"
            $prof = @(Get-ProfileItems $set[$v].Env)
            if ($prof.Count) { $key += '-p' + (Get-ShortHash ($prof -join ',')); $how += " of shader profile $($prof -join ',')" }
        }
        if ($owner.ContainsKey($key)) {
            Fail "variants '$($owner[$key])' and '$v' would share the cache copy cache\$key; give one its own with 'bench:cache' = '<name>'"
        }
        $owner[$key] = $v
        $out[$v] = @{ Key = $key; How = $how }
        $i++
    }
    return $out
}

# A hash of what decides which shaders/pipelines a run builds: the probe executables (SHA256), the
# launcher, the ISS_/TLG_/SHADPS4_ variables the run sees (the shell's, mods, the variant's) and its
# tlg.ini keys. $null without a probe hash (the build is unknown: never treated as warm).
function Get-RunSignature($def, $runIni, $build, $launcher, $mods) {
    $probes = @(@($build.probes) | Where-Object { $_ -and $_.sha256 } | ForEach-Object { $_.sha256 })
    if (-not $probes.Count) { return $null }
    $envs = @{}
    $all = [Environment]::GetEnvironmentVariables('Process')
    foreach ($k in $all.Keys) {
        $n = "$k".ToUpper()
        if ($n -notmatch '^(ISS|TLG|SHADPS4)_') { continue }
        if ($Reserved -contains ($n -replace '^TLG_', 'ISS_') -or $n -match '^(ISS|TLG)_(USER_DIR|GPU_USER_DIR)$') { continue }
        $envs[$n] = "$($all[$k])"
    }
    foreach ($k in $mods.Keys) { $envs[$k.ToUpper()] = "$($mods[$k])" }
    foreach ($k in $def.Env.Keys) {
        $u = $k.ToUpper()
        if ($u -like 'ISS_*') { $envs.Remove('TLG_' + $u.Substring(4)) }
        $v = $def.Env[$k]
        if ($null -eq $v -or "$v" -eq '') { $envs.Remove($u) } else { $envs[$u] = "$v" }
    }
    $lines = @("exe=$($launcher.Exe)") + @($probes | ForEach-Object { "probe=$_" }) +
        @($envs.Keys | Sort-Object | ForEach-Object { "env:$_=$($envs[$_])" }) +
        @(@($runIni.Keys) | Sort-Object | ForEach-Object { "ini:$_=$($runIni[$_])" })
    return Get-ShortHash ($lines -join "`n") 16
}

# <cache copy>\slot.json: the last run that used the copy (written after every run of a session).
function Read-SlotState([string]$key) {
    $p = Join-Path (Get-BenchCacheRoot $key) 'slot.json'
    if (-not [IO.File]::Exists($p)) { return $null }
    try { $s = Get-Content -LiteralPath $p -Raw -Encoding UTF8 | ConvertFrom-Json } catch { return $null }
    if (-not $s -or -not $s.PSObject.Properties['last']) { return $null }
    return $s.last
}

function Write-SlotState([string]$key, $last) {
    $root = Get-BenchCacheRoot $key
    if (-not [IO.Directory]::Exists($root)) { return }
    try { Write-Json (Join-Path $root 'slot.json') ([ordered]@{ last = $last }) }
    catch { Note "  warning: could not write $root\slot.json: $($_.Exception.Message)" }
}

# ---------------------------------------------------------------------------------- adaptive rounds
# Student t quantiles by degrees of freedom (index; 0 unused): two-sided 95% (0.975) and 90% (0.95).
# Above 20 the df-20 value is used (slightly conservative).
$T975 = @(0, 12.706, 4.303, 3.182, 2.776, 2.571, 2.447, 2.365, 2.306, 2.262, 2.228, 2.201, 2.179, 2.160, 2.145, 2.131, 2.120, 2.110, 2.101, 2.093, 2.086)
$T95 = @(0, 6.314, 2.920, 2.353, 2.132, 2.015, 1.943, 1.895, 1.860, 1.833, 1.812, 1.796, 1.782, 1.771, 1.761, 1.753, 1.746, 1.740, 1.734, 1.729, 1.725)
function Get-TQuantile([int]$df, [double[]]$table) {
    if ($df -lt 1) { return [double]::PositiveInfinity }
    if ($df -lt $table.Count) { return $table[$df] }
    return $table[$table.Count - 1]
}

function F([string]$format, [object[]]$values) { [string]::Format($Invariant, $format, $values) }

# After a round: for each test variant, the per-round paired diff d = 100 (test - control) / control
# of avg_fps (both runs of the round ok; the pairing cancels slow drift, which the alternating order
# spreads over both sides), its mean and stdev (at least -PairSdFloorPct). Decided when
#   clear difference: the 95% CI excludes 0 and |mean| > -NoiseK x max(-NoiseCvPct, control CV)
#                     (the noise test of round_analyze.py bench, '~', with a floor), or
#   no change:        the 90% CI lies within +-EquivPct (an equivalence test at 5% per side).
# @{ Stop = all decided; Why; Lines; Verdicts }.
function Test-AdaptiveStop($measured, $names, [int]$round) {
    $ctrl = $names[0]
    if ($names.Count -lt 2) { return @{ Stop = $true; Why = 'only the control: nothing to compare'; Lines = @(); Verdicts = @() } }
    $cs = Get-Stats @($measured | Where-Object { $_.Variant -eq $ctrl -and $_.Ok } | ForEach-Object { $_.Fps })
    $cv = $null; if ($null -ne $cs.Sd -and $cs.Mean) { $cv = 100.0 * $cs.Sd / $cs.Mean }
    $noise = $NoiseCvPct; if ($null -ne $cv -and $cv -gt $noise) { $noise = $cv }
    $lines = @(); $open = @(); $done = @(); $verdicts = @()
    foreach ($g in @($names | Select-Object -Skip 1)) {
        $d = @()
        for ($rr = 1; $rr -le $round; $rr++) {
            $c = @($measured | Where-Object { $_.Variant -eq $ctrl -and $_.Round -eq $rr -and $_.Ok }) | Select-Object -First 1
            $t = @($measured | Where-Object { $_.Variant -eq $g -and $_.Round -eq $rr -and $_.Ok }) | Select-Object -First 1
            if ($c -and $t -and $c.Fps -gt 0) { $d += 100.0 * ($t.Fps - $c.Fps) / $c.Fps }
        }
        $n = $d.Count
        if ($n -lt 2) {
            $lines += "$g vs ${ctrl}: $n complete round(s), undecided"; $open += "$g (too few complete rounds)"
            $verdicts += [ordered]@{ variant = $g; rounds = $n; verdict = 'undecided' }
            continue
        }
        $s = Get-Stats $d
        $sd = [Math]::Max([double]$s.Sd, $PairSdFloorPct)
        $h95 = (Get-TQuantile ($n - 1) $T975) * $sd / [Math]::Sqrt($n)
        $h90 = (Get-TQuantile ($n - 1) $T95) * $sd / [Math]::Sqrt($n)
        $m = $s.Mean
        $verdict = 'undecided'
        if (($m - $h95 -gt 0 -or $m + $h95 -lt 0) -and [Math]::Abs($m) -gt $NoiseK * $noise) { $verdict = 'clear difference' }
        elseif ([Math]::Abs($m) + $h90 -le $EquivPct) { $verdict = (F 'no change (within +-{0:0.0#}%)' @($EquivPct)) }
        $lines += F ('{0} vs {1}: {2:+0.00;-0.00;+0.00}% over {3} rounds (paired sd {4:0.00}%, used {5:0.00}%), ' +
            '95% CI [{6:+0.00;-0.00;+0.00}, {7:+0.00;-0.00;+0.00}], 90% CI [{8:+0.00;-0.00;+0.00}, {9:+0.00;-0.00;+0.00}], ' +
            'noise bar {10:0.00}%: {11}') @($g, $ctrl, $m, $n, [double]$s.Sd, $sd, ($m - $h95), ($m + $h95), ($m - $h90), ($m + $h90), ($NoiseK * $noise), $verdict)
        $tag = F '{0} {1:+0.0;-0.0;+0.0}%' @($g, $m)
        if ($verdict -eq 'undecided') { $open += $tag } else { $done += "$tag $verdict" }
        $verdicts += [ordered]@{ variant = $g; rounds = $n; diff_pct = [Math]::Round($m, 3); pair_sd_pct = [Math]::Round([double]$s.Sd, 3);
            ci95 = @([Math]::Round($m - $h95, 3), [Math]::Round($m + $h95, 3)); ci90 = @([Math]::Round($m - $h90, 3), [Math]::Round($m + $h90, 3));
            noise_bar_pct = [Math]::Round($NoiseK * $noise, 3); verdict = $verdict }
    }
    if ($open.Count) { return @{ Stop = $false; Why = "undecided: $($open -join ', ')"; Lines = $lines; Verdicts = $verdicts } }
    return @{ Stop = $true; Why = "decided: $($done -join '; ')"; Lines = $lines; Verdicts = $verdicts }
}

# Before an early stop: round_analyze.py bench's scene-drift check over the measured runs so far
# (a drift run measured another scene, so the diffs are not trusted). The reason not to stop, or
# $null (no drift, or no Python / no usable result: then the check is skipped with a note).
function Get-DriftBlock([string]$sessionDir, $measured, [string]$ctrl) {
    $analyzer = Join-Path $PSScriptRoot 'round_analyze.py'
    if (-not (Test-Path -LiteralPath $analyzer)) { return $null }
    if ($null -eq $script:AdaptivePy) { $script:AdaptivePy = Find-Python; if (-not $script:AdaptivePy) { $script:AdaptivePy = $false } }
    if (-not $script:AdaptivePy) { Note '  adaptive: Python 3 not found: no scene-drift check before stopping'; return $null }
    $py = $script:AdaptivePy
    $files = @($measured | Where-Object { $_.Ok -and $_.File } | ForEach-Object { $_.File })
    $out = Join-Path $sessionDir 'adaptive-check.json'
    $pyArgs = @('-I', $analyzer, 'bench') + $files + @('--control', $ctrl, '--noise-k', (Fmt '{0}' $NoiseK), '--drift-pct', (Fmt '{0}' $DriftPct), '--json', $out)
    $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    try { $null = & $py.Exe @($py.Pre) @pyArgs 2>&1; $code = $LASTEXITCODE } finally { $ErrorActionPreference = $old }
    if ($code -ne 0 -or -not (Test-Path -LiteralPath $out)) { Note "  adaptive: round_analyze.py bench failed (exit $code): no scene-drift check"; return $null }
    try { $j = Get-Content -LiteralPath $out -Raw -Encoding UTF8 | ConvertFrom-Json } catch { $j = $null }
    if (-not $j -or -not $j.PSObject.Properties['drift'] -or -not $j.drift) { Note '  adaptive: no drift block in round_analyze.py output: no scene-drift check'; return $null }
    if ($j.drift.PSObject.Properties['state'] -and "$($j.drift.state)" -eq 'inconclusive') { return 'scene-drift check inconclusive' }
    $flagged = @()
    if ($j.drift.PSObject.Properties['runs']) { $flagged = @(@($j.drift.runs) | Where-Object { $_.PSObject.Properties['drift'] -and $_.drift -eq $true } | ForEach-Object { "$($_.run)" }) }
    if ($flagged.Count) { return "scene drift in $($flagged -join ', ')" }
    return $null
}
$script:AdaptivePy = $null

# ---------------------------------------------------------------------------------------- run
function Invoke-Run {
    # -Rounds N: fixed. Otherwise adaptive, -MinRounds to -MaxRounds (Test-AdaptiveStop).
    $fixed = $script:Bound.ContainsKey('Rounds')
    if ($fixed -and ($script:Bound.ContainsKey('MinRounds') -or $script:Bound.ContainsKey('MaxRounds'))) {
        Fail 'pass -Rounds N (fixed rounds) or -MinRounds/-MaxRounds (adaptive), not both'
    }
    if ($fixed) {
        if ($Rounds -lt 1) { Fail '-Rounds must be at least 1' }
        $minR = $Rounds; $maxR = $Rounds
    } else {
        if ($MinRounds -lt 2) { Fail '-MinRounds must be at least 2 (the noise of the paired diff needs two rounds)' }
        if ($MaxRounds -lt $MinRounds) { Fail '-MaxRounds must be at least -MinRounds' }
        if ($EquivPct -le 0 -or $NoiseCvPct -lt 0 -or $PairSdFloorPct -lt 0) { Fail '-EquivPct must be above 0, -NoiseCvPct and -PairSdFloorPct at least 0' }
        $minR = $MinRounds; $maxR = $MaxRounds
    }
    $adaptive = $maxR -gt $minR
    if (-not $NoWarmup -and $WarmupRuns -lt 1) { Fail '-WarmupRuns must be at least 1 (or pass -NoWarmup)' }
    $route = Join-Path $BenchDir 'route.txt'
    $snap = Join-Path $BenchDir 'save'
    if (-not (Test-Path -LiteralPath $route) -or -not (Test-Path -LiteralPath (Join-Path $snap 'savedata'))) {
        $msg = "no recorded route in $BenchDir (route.txt and save\). Run: scripts\bench.ps1 record"
        if ($DryRun) { Note "warning: $msg" } else { Fail $msg }
    }
    $launcher = Resolve-Launcher
    $source = Resolve-SourceUserDir $launcher
    Assert-Separate $source
    $mods = Get-ModsVars $source
    Initialize-LaunchEnv $launcher
    $build = Get-BuildInfo $launcher
    $set = Get-VariantSet
    $names = @($set.Keys)
    Assert-CacheMode $set $source
    $name = $Session; if (-not $name) { $name = Get-Date -Format 'yyyyMMdd-HHmmss' }
    if ($name -notmatch '^[A-Za-z0-9][A-Za-z0-9_.-]*$') { Fail "-Session '$name': use letters, digits, _ . -" }
    $sessionDir = Join-Path $BenchDir "runs\$name"
    if ((Test-Path -LiteralPath $sessionDir) -and -not $DryRun) { Fail "session $sessionDir already exists; pick another -Session" }

    # Cache copies (slots by default), each variant's tlg.ini and build/settings signature.
    $caches = [ordered]@{}
    if (-not $SharePlayerCache) { $caches = Get-CacheKeys $set }
    $runInis = @{}; $sigs = @{}; $relocated = @{}
    foreach ($v in $names) {
        # (-Ini, then the variant's ini: keys; '' drops a key.)
        $runInis[$v] = Merge-Ini $Ini $set[$v].Ini "variant '$v' ini"
        $sigs[$v] = Get-RunSignature $set[$v] $runInis[$v] $build $launcher $mods
        $relocated[$v] = (@($CacheRootVars | Where-Object { Get-RunValue $set[$v].Env $_ }).Count -gt 0)
    }
    # Warm slots: a variant whose copy's last run (slot.json) had the same signature, ended ok and
    # compiled nothing needs no warm-up. If that holds for every variant, the control keeps one
    # warm-up anyway (the session's first run also warms the disk cache and the GPU clocks). A
    # measured run of a skipped variant that still compiles becomes a warm-up and is repeated.
    $warmSkip = @{}
    if (-not $NoWarmup -and -not $AlwaysWarmup -and -not $SharePlayerCache -and -not $ResetBenchCache) {
        foreach ($v in $names) {
            if (-not $sigs[$v] -or $relocated[$v]) { continue }
            $st = Read-SlotState $caches[$v].Key
            if (-not $st -or -not $st.PSObject.Properties['signature'] -or "$($st.signature)" -ne $sigs[$v]) { continue }
            if ("$($st.status)" -ne 'ok' -or $null -eq $st.compiles -or [double]$st.compiles -ne 0) { continue }
            $warmSkip[$v] = "cache\$($caches[$v].Key) is warm: its last run ($($st.session)\$($st.run), $($st.time)) had the same build and settings and compiled nothing"
        }
        if ($warmSkip.Count -eq $names.Count) {
            $warmSkip.Remove($names[0])
            Info "  warm-up  : every cache slot is warm; the control '$($names[0])' keeps one warm-up run (disk cache, GPU clocks)"
        }
    }

    # The plan: warm-ups, then rounds alternating the order (A B / B A / A B ...); adaptive
    # sessions plan -MaxRounds and skip the rest once decided.
    $plan = New-Object Collections.Generic.List[object]
    if (-not $NoWarmup) {
        foreach ($v in $names) { for ($i = 1; $i -le $WarmupRuns; $i++) { $plan.Add(@{ Variant = $v; Warmup = $i; Round = 0; Dir = "warmup-$v-$i"; Skip = $warmSkip.ContainsKey($v) }) } }
    }
    $n = 0
    for ($round = 1; $round -le $maxR; $round++) {
        $order = $names
        if ($round % 2 -eq 0) { $order = @($names[($names.Count - 1)..0]) }
        foreach ($v in $order) { $n++; $plan.Add(@{ Variant = $v; Warmup = 0; Round = $round; Dir = ('{0:D2}-{1}' -f $n, $v); Skip = $false }) }
    }
    $measured = @($plan | Where-Object { -not $_.Warmup }).Count
    $warmups = @($plan | Where-Object { $_.Warmup -and -not $_.Skip }).Count
    Info "Benchmark session $sessionDir"
    Info "  launcher : $($launcher.Exe)"
    Info "  game     : $script:GameDirUsed"
    Info (Format-BuildInfo $build)
    if ($build.dirty) { Note '  note: uncommitted changes: this session is not tied to the commit alone (dirty_files in variants.json)' }
    Info "  route    : $route"
    if ($SharePlayerCache) { Info "  caches   : SHARED with $source (-SharePlayerCache: the runs write into the player's caches)" }
    else {
        Info "  caches   : bench copies in $BenchDir\cache (seeded from $source on first use)"
        foreach ($v in $names) { Info ("    {0,-10} cache\{1} ({2})" -f $v, $caches[$v].Key, $caches[$v].How) }
    }
    foreach ($v in $names) { if ($warmSkip.ContainsKey($v)) { Info "  warm-up  : none for ${v}: $($warmSkip[$v])" } }
    Info "  variants : $($names -join ', ') (control: $($names[0]))"
    if ($adaptive) {
        Info ("  rounds   : adaptive, $minR to $maxR (" + (F 'stop when every variant is a clear difference: 95% CI excludes 0 and |diff| > {0:0.##}x max({1:0.##}%, control CV); or no change: 90% CI within +-{2:0.##}%' @($NoiseK, $NoiseCvPct, $EquivPct)) + ')')
        Info "  runs     : $($minR * $names.Count) to $measured measured + up to $warmups warm-up, timeout $TimeoutSec s each"
    } else {
        Info "  rounds   : $maxR (fixed)"
        Info "  runs     : $measured measured + up to $warmups warm-up, timeout $TimeoutSec s each"
    }
    foreach ($v in $names) {
        $desc = @(); foreach ($k in $set[$v].Env.Keys) { $desc += "$k=$($set[$v].Env[$k])" }
        foreach ($k in $set[$v].Ini.Keys) { $desc += "ini:$k=$($set[$v].Ini[$k])" }
        if (-not $desc) { $desc = @('(no changes)') }
        Info ("    {0,-10} {1}" -f $v, ($desc -join ' '))
    }
    $info = [ordered]@{ variants = $names; control = $names[0]; rounds = $maxR; route = $route; launcher = $launcher.Exe;
        source_user_dir = $source; ini = $Ini; started = (Get-Date).ToString('s'); env = [ordered]@{}; ini_variants = [ordered]@{}
        cache_mode = $(if ($SharePlayerCache) { 'shared' } else { 'bench' }); cache_keys = [ordered]@{}; caches = [ordered]@{};
        round_mode = $(if ($adaptive) { 'adaptive' } else { 'fixed' }); min_rounds = $minR; max_rounds = $maxR;
        warmup_skipped = @($names | Where-Object { $warmSkip.ContainsKey($_) });
        game_dir = $env:ISS_GAME_DIR; build = $build }
    if ($adaptive) { $info['adaptive'] = [ordered]@{ equiv_pct = $EquivPct; noise_cv_pct = $NoiseCvPct; noise_k = $NoiseK; pair_sd_floor_pct = $PairSdFloorPct } }
    foreach ($v in $names) {
        $info.env[$v] = $set[$v].Env; $info.ini_variants[$v] = $set[$v].Ini
        if ($SharePlayerCache) { $info.caches[$v] = $source }
        else { $info.cache_keys[$v] = $caches[$v].Key; $info.caches[$v] = Get-BenchCacheRoot $caches[$v].Key }
    }

    if ($DryRun) {
        Clear-StaleJunctions
        if (-not $SharePlayerCache) { foreach ($v in $names) { Initialize-BenchCache $caches[$v].Key $source } }
    } else {
        Lock-BenchDir
        if (@(Get-Probes $launcher -AnyPath).Count) { Fail 'the game is already running; close it first' }
        Clear-StaleJunctions
        if (-not $SharePlayerCache) { foreach ($v in $names) { Initialize-BenchCache $caches[$v].Key $source } }
        New-Dir $sessionDir
        Write-Json (Join-Path $sessionDir 'variants.json') $info
    }
    $results = New-Object Collections.Generic.List[object]
    $runsDone = New-Object Collections.Generic.List[object]   # measured: Variant, Round, Fps, Ok, File
    $checks = New-Object Collections.Generic.List[object]
    $stopReason = if ($adaptive) { $null } else { "fixed: $maxR round(s)" }
    $lastRound = 0
    $shown = @{}
    for ($k = 1; $k -le $plan.Count; $k++) {
        $step = $plan[$k - 1]
        if ($step.Skip) { continue }   # warm-ups after one that compiled nothing; rounds after an adaptive stop
        $v = $step.Variant
        $runDir = Join-Path $sessionDir $step.Dir
        $vars = [ordered]@{
            ISS_PAD_REPLAY = $route; ISS_PAD_REPLAY_AUTO = '1'
            ISS_BENCH_JSON = (Join-Path $runDir 'bench.json'); ISS_BENCH_LABEL = $v; ISS_BENCH_EXIT = '1'
            ISS_STOP_FILE = (Join-Path $runDir 'stop'); ISS_FRAME_STATS = '1'
            ISS_USER_DIR = (Join-Path $runDir 'user'); APPDATA = (Join-Path $runDir 'appdata')
            ISS_PAD_RECORD = $null; ISS_PAD_RECORD_SECONDS = $null; ISS_PAD_FILE = $null
            ISS_GPU_USER_DIR = $null; TLG_USER_DIR = $null; TLG_GPU_USER_DIR = $null
            SHADPS4_FRAME_CSV = $null; ISS_BENCH_SKIP_MS = $null
        }
        if ($FrameCsv) { $vars.SHADPS4_FRAME_CSV = (Join-Path $runDir 'frames.csv') }
        if ($SkipMs -ge 0) { $vars.ISS_BENCH_SKIP_MS = "$SkipMs" }
        if ($launcher.Kind -eq 'py') { $vars['PYTHONUSERBASE'] = (Join-Path $env:APPDATA 'Python') }
        foreach ($e in $mods.Keys) { $vars[$e] = $mods[$e] }
        foreach ($e in $set[$v].Env.Keys) { $vars[$e] = $set[$v].Env[$e] }
        $runIni = $runInis[$v]
        $cacheKey = if ($SharePlayerCache) { $v } else { $caches[$v].Key }
        $what = if ($step.Warmup) { "warm-up $($step.Warmup) of $v" } else { "round $($step.Round), $v" }
        if ($DryRun) {
            Dry "[$k/$($plan.Count)] $what -> $runDir"
            if (-not $shown.ContainsKey($v)) {
                $shown[$v] = 1
                Dry "  data folder: $runDir\user = copy of $snap\savedata + tlg.ini; junctions: $(Format-Links $source $cacheKey)"
            }
            if ($k -eq 1) {
                Dry "  unset for every run (unless a variant sets them): $(Format-Unset $vars)"
                Dry "  launcher settings: $runDir\appdata (copy of $snap\settings.json, no extra backup folder)"
                Dry "  start: `"$($launcher.Exe)`" $(if ($launcher.Kind -eq 'exe') { '--play' } else { '(run with Python 3)' }); wait <= $TimeoutSec s, then the stop file, then kill"
            }
            Dry "  tlg.ini: $((Format-Env $runIni) -replace "`n", ', ')"
            Dry "  env: $((Format-Env $vars) -replace "`n", '; ')"
            if ($adaptive -and -not $step.Warmup -and $step.Round -ge $minR -and $step.Round -lt $maxR -and $step.Variant -eq @($plan | Where-Object { $_.Round -eq $step.Round })[-1].Variant) {
                Dry "  adaptive: after round $($step.Round): stop here when every variant is decided (else round $($step.Round + 1))"
            }
            continue
        }
        Info "[$k/$($plan.Count)] $what"
        New-Dir $runDir
        $r = $null
        try {
            $null = New-RunData $runDir $snap $source $runIni (Plan-Links $source $cacheKey)
            Set-RunEnv $vars
            Write-Text (Join-Path $runDir 'env.txt') ((Format-Env $vars) + "`n")
            $r = Invoke-Game $launcher $runDir $vars.ISS_STOP_FILE $TimeoutSec 'run'
        } finally { Invoke-RunCleanup }
        if ($r.log) { Copy-Item -LiteralPath $r.log -Destination (Join-Path $runDir 'game.log') -Force -ErrorAction SilentlyContinue }
        $benchJson = Join-Path $runDir 'bench.json'
        if (-not (Test-Path -LiteralPath $benchJson) -and $r.status -ne 'empty-route') {
            $parsed = ConvertFrom-BenchLine $r.bench_line
            if ($parsed) { Write-Json $benchJson $parsed; Note '  bench.json missing: written from the Bench: log line' }
            else { $r.status = "$($r.status), no bench result" }
        }
        $r['variant'] = $v; $r['round'] = $step.Round; $r['warmup'] = $step.Warmup; $r['cache'] = $cacheKey
        Write-Json (Join-Path $runDir 'run.json') $r
        if ($r.status -eq 'empty-route') {
            # Every run replays the same file: the other runs would end the same way.
            $script:FailedRuns++
            Fail "the route $route is empty or unreadable for the game (status empty-route, see $runDir); session stopped. Record it again: scripts\bench.ps1 record -Force"
        }
        if ($r.renderer -and $r.renderer -ne 'v2') { Note "  warning: this run used the $($r.renderer) renderer, not v2" }
        $b = $null
        if (Test-Path -LiteralPath $benchJson) {
            try { $b = Read-Json $benchJson } catch { Note "  warning: $($_.Exception.Message)"; $r.status = "$($r.status), unreadable bench.json"; Write-Json (Join-Path $runDir 'run.json') $r }
        }
        $compiles = $null; $fps = $null
        if ($b -and $b.PSObject.Properties['compiles']) { $compiles = [double]$b.compiles }
        if ($b -and $b.PSObject.Properties['avg_fps']) { $fps = [double]$b.avg_fps }
        if (-not $SharePlayerCache -and -not $relocated[$v]) {
            Write-SlotState $cacheKey ([ordered]@{ session = $name; variant = $v; run = $step.Dir; time = (Get-Date).ToString('s');
                    signature = $sigs[$v]; commit = $build.commit; status = "$($r.status)"; compiles = $compiles; warmup = [bool]$step.Warmup })
        }
        if ($b) {
            if ($step.Warmup -and $null -ne $compiles -and $compiles -eq 0 -and $step.Warmup -lt $WarmupRuns) {
                Info "  warm: no compiles, skipping the remaining warm-ups of $v"
                foreach ($s in $plan) { if ($s.Variant -eq $v -and $s.Warmup -gt $step.Warmup) { $s.Skip = $true } }
            }
            if ((-not $step.Warmup) -and $warmSkip.ContainsKey($v)) {
                $warmSkip.Remove($v)   # only the variant's first measured run is checked
                if ($null -ne $compiles -and $compiles -gt 0) {
                    # The slot was not as warm as slot.json said: this run counts as its warm-up.
                    $dest = Join-Path $sessionDir "warmup-$v-auto"
                    $r['warmup'] = 'auto'
                    Write-Json (Join-Path $runDir 'run.json') $r
                    [IO.Directory]::Move($runDir, $dest)
                    Note "  warning: $compiles compiles although cache\$cacheKey was warm: this run is now the warm-up ($dest); repeating it"
                    $results.Add($r)
                    $plan.Insert($k, @{ Variant = $v; Warmup = 0; Round = $step.Round; Dir = $step.Dir; Skip = $false })
                    continue
                }
            }
            if ((-not $step.Warmup) -and $null -ne $compiles -and $compiles -gt 0) {
                Note "  warning: $($b.compiles) shader/pipeline compiles during the measurement (more warm-up: -WarmupRuns $($WarmupRuns + 1))"
            }
        }
        $results.Add($r)
        if ($step.Warmup) { continue }
        $runsDone.Add(@{ Variant = $v; Round = $step.Round; Fps = $fps; Ok = ($r.status -eq 'ok' -and $null -ne $fps); File = $(if ($b) { $benchJson } else { $null }) })
        $lastRound = $step.Round
        # Adaptive: after the last run of a round from -MinRounds on.
        if (-not $adaptive -or $step.Round -lt $minR) { continue }
        $more = $false
        for ($j = $k; $j -lt $plan.Count; $j++) { if (-not $plan[$j].Skip -and -not $plan[$j].Warmup -and $plan[$j].Round -eq $step.Round) { $more = $true; break } }
        if ($more) { continue }
        $t = Test-AdaptiveStop $runsDone $names $step.Round
        foreach ($l in $t.Lines) { Info "  adaptive: $l" }
        $stop = $t.Stop; $why = $t.Why
        if ($stop -and $names.Count -gt 1 -and $step.Round -lt $maxR) {
            $block = Get-DriftBlock $sessionDir $runsDone $names[0]
            if ($block) { $stop = $false; $why = "$why, but $block" }
        }
        $checks.Add([ordered]@{ round = $step.Round; stop = $stop; why = $why; verdicts = $t.Verdicts })
        if ($step.Round -ge $maxR) {
            $stopReason = "reached -MaxRounds $maxR ($why)"
            Info "  adaptive: $stopReason"
        } elseif ($stop) {
            $stopReason = "stopped after round $($step.Round) of at most ${maxR}: $why"
            Info "  adaptive: $stopReason"
            for ($j = $k; $j -lt $plan.Count; $j++) { $plan[$j].Skip = $true }
        } else {
            Info "  adaptive: round $($step.Round + 1) next ($why)"
        }
    }
    if ($DryRun) { Dry "then: scripts\bench.ps1 compare -Session $name"; return }
    $bad = @($results | Where-Object { $_.status -ne 'ok' -and -not $_.warmup })
    $script:FailedRuns = $bad.Count
    if ($bad.Count) { Note "$($bad.Count) measured run(s) did not end cleanly (exit code 3); see run.json in $sessionDir" }
    $info['rounds_run'] = $lastRound
    $info['round_stop'] = $stopReason
    $info['adaptive_checks'] = [object[]]$checks.ToArray()
    Write-Json (Join-Path $sessionDir 'variants.json') $info
    $warmRan = @($results | Where-Object { $_.warmup }).Count
    Info ''
    Info "Rounds: $lastRound ($stopReason); runs: $(@($results | Where-Object { -not $_.warmup }).Count) measured, $warmRan warm-up"
    Info ''
    Invoke-Compare $sessionDir
}

# -------------------------------------------------------------------------------------- compare
function Resolve-Session {
    $runs = Join-Path $BenchDir 'runs'
    if ($Session) {
        if (Test-Path -LiteralPath $Session -PathType Container) { return (Resolve-Path -LiteralPath $Session).Path }
        $p = Join-Path $runs $Session
        if (Test-Path -LiteralPath $p -PathType Container) { return $p }
        Fail "session '$Session' not found (neither a folder nor $p)"
    }
    if (-not (Test-Path -LiteralPath $runs)) { Fail "no sessions in $runs yet. Run: scripts\bench.ps1 run" }
    $last = Get-ChildItem -LiteralPath $runs -Directory | Where-Object { Test-Path (Join-Path $_.FullName 'variants.json') } |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $last) { Fail "no sessions in $runs yet. Run: scripts\bench.ps1 run" }
    return $last.FullName
}

function Get-Stats($values) {
    $v = @($values | Where-Object { $null -ne $_ } | ForEach-Object { [double]$_ })
    if ($v.Count -eq 0) { return @{ Mean = $null; Sd = $null; N = 0 } }
    $m = ($v | Measure-Object -Average).Average
    $sd = $null
    if ($v.Count -gt 1) { $acc = 0.0; foreach ($x in $v) { $acc += ($x - $m) * ($x - $m) }; $sd = [Math]::Sqrt($acc / ($v.Count - 1)) }
    return @{ Mean = $m; Sd = $sd; N = $v.Count }
}

# Same table as round_analyze.py bench, for machines without Python.
function Show-InternalTable($runs, $order, [string]$ctrl) {
    $cols = @($Metrics | Where-Object { $m = $_; @($runs | Where-Object { $_.Data.PSObject.Properties[$m] }).Count -gt 0 })
    $stats = @{}
    foreach ($g in $order) {
        $stats[$g] = @{}
        foreach ($m in $cols) {
            $stats[$g][$m] = Get-Stats @($runs | Where-Object { $_.Variant -eq $g -and $_.Data.PSObject.Properties[$m] } | ForEach-Object { $_.Data.$m })
        }
    }
    $rows = New-Object Collections.Generic.List[object]
    $rows.Add(@('variant', 'runs') + $cols)
    foreach ($g in $order) {
        $label = $g; if ($g -eq $ctrl) { $label = "$g (control)" }
        $row = @($label, "$(@($runs | Where-Object { $_.Variant -eq $g }).Count)")
        foreach ($m in $cols) {
            $s = $stats[$g][$m]; $c = $stats[$ctrl][$m]
            if ($null -eq $s.Mean) { $row += '-'; continue }
            $nd = 1; if ($m -in 'over33', 'over50', 'compiles') { $nd = 0 } elseif ($m -eq 'cpu_ms_per_frame') { $nd = 2 }
            $cell = Fmt "{0:F$nd}" $s.Mean
            if ($null -ne $s.Sd) { $cell += Fmt " +-{0:F$([Math]::Max($nd,1))}" $s.Sd }
            if ($g -ne $ctrl -and $null -ne $c.Mean) {
                if ($c.Mean -ne 0) {
                    $d = 100.0 * ($s.Mean - $c.Mean) / $c.Mean
                    $noise = ''
                    # (a stdev of 0 is no noise estimate: a 0% diff is not marked)
                    if ($null -ne $c.Sd -and $c.Sd -gt 0 -and [Math]::Abs($d) -le $NoiseK * 100.0 * $c.Sd / [Math]::Abs($c.Mean)) { $noise = ' ~' }
                    $cell += (Fmt ' ({0:+0.0;-0.0;+0.0}%' $d) + "$noise)"
                } else { $cell += Fmt ' ({0:+0.###;-0.###;+0})' $s.Mean }
            }
            $row += $cell
        }
        $rows.Add($row)
    }
    $widths = @(); for ($i = 0; $i -lt $rows[0].Count; $i++) { $widths += (@($rows | ForEach-Object { "$($_[$i])".Length }) | Measure-Object -Maximum).Maximum }
    $line = { param($r) (@(for ($i = 0; $i -lt $r.Count; $i++) { "$($r[$i])".PadRight($widths[$i]) }) -join '  ').TrimEnd() }
    Info (& $line $rows[0])
    Info (& $line @($widths | ForEach-Object { '-' * $_ }))
    for ($i = 1; $i -lt $rows.Count; $i++) { Info (& $line $rows[$i]) }
    $single = ''; if ($stats[$ctrl][$cols[0]].N -lt 2) { $single = ' One control run only: no noise estimate.' }
    Info ''
    Info ("mean +-stdev per variant; % = diff vs '$ctrl'; '~' = within $NoiseK" + "x the control's stdev when above 0 (likely noise).$single")
}

function Invoke-Compare([string]$sessionDir) {
    if (-not $sessionDir) { $sessionDir = Resolve-Session }
    $info = Read-Json (Join-Path $sessionDir 'variants.json')
    $order = @($info.variants)
    $ctrl = $order[0]
    if ($Control) {
        if ($order -notcontains $Control) { Fail "-Control '$Control' is not one of: $($order -join ', ')" }
        $ctrl = $Control
        $order = @($ctrl) + @($order | Where-Object { $_ -ne $ctrl })
    }
    $runs = New-Object Collections.Generic.List[object]
    $missing = @()
    foreach ($d in Get-ChildItem -LiteralPath $sessionDir -Directory | Where-Object { $_.Name -match '^\d+-' } | Sort-Object Name) {
        $meta = Join-Path $d.FullName 'run.json'
        $variant = $d.Name -replace '^\d+-', ''
        $status = 'ok'
        if (Test-Path -LiteralPath $meta) {
            $m = Read-Json $meta
            if ($m.PSObject.Properties['variant']) { $variant = $m.variant }
            if ($m.PSObject.Properties['status']) { $status = "$($m.status)" }
        }
        $bj = Join-Path $d.FullName 'bench.json'
        $log = Join-Path $d.FullName 'game.log'
        $csv = Join-Path $d.FullName 'frames.csv'
        if (Test-Path -LiteralPath $bj) { $runs.Add(@{ Variant = $variant; File = $bj; Data = (Read-Json $bj); Status = $status }) }
        elseif (Test-Path -LiteralPath $csv) { $runs.Add(@{ Variant = $variant; File = $csv; Data = $null }) }
        elseif (Test-Path -LiteralPath $log) { $runs.Add(@{ Variant = $variant; File = $log; Data = $null }) }
        else { $missing += $d.Name }
    }
    Info "Session $sessionDir"
    if ($info.PSObject.Properties['build'] -and $info.build) { Info (Format-BuildInfo $info.build) }
    else { Info '  commit   : not recorded (session made before bench.ps1 recorded the build)' }
    if ($missing.Count) { Note "no result in: $($missing -join ', ')" }
    $present = @($order | Where-Object { $v = $_; @($runs | Where-Object { $_.Variant -eq $v }).Count -gt 0 })
    if ($present -notcontains $ctrl) { Fail "the control variant '$ctrl' has no results in $sessionDir" }
    $order = $present
    $analyzer = Join-Path $PSScriptRoot 'round_analyze.py'
    $py = $null
    if (Test-Path -LiteralPath $analyzer) { $py = Find-Python }
    # round_analyze.py groups by label; files without one (a log, a CSV) by folder name minus 'NN-'.
    $files = @(); foreach ($g in $order) { $files += @($runs | Where-Object { $_.Variant -eq $g } | ForEach-Object { $_.File }) }
    $common = @('--control', $ctrl, '--noise-k', (Fmt '{0}' $NoiseK), '--drift-pct', (Fmt '{0}' $DriftPct))
    if ($ExcludeDrift) { $common += '--exclude-drift' }
    if ($SkipMs -ge 0) { $common += @('--skip-ms', "$SkipMs") }
    $pyArgs = @('-I', $analyzer, 'bench') + $files + $common + @('--csv', (Join-Path $sessionDir 'compare.csv'), '--json', (Join-Path $sessionDir 'compare.json'))
    if ($DryRun) {
        if ($py) { Dry "$($py.Exe) $((@($py.Pre) + $pyArgs) -join ' ')" } else { Dry 'Python not found: the internal table would be printed' }
        return
    }
    if ($py) {
        & $py.Exe @($py.Pre) @pyArgs
        if ($LASTEXITCODE -ne 0) { Fail "round_analyze.py bench failed (exit $LASTEXITCODE)" }
        if ($Detail) {
            if ($ExcludeDrift) { Note 'note: -ExcludeDrift is ignored by the -Detail per-run tables (every run is listed)' }
            $ctrlFiles = @($runs | Where-Object { $_.Variant -eq $ctrl -and $_.File -like '*.json' } | ForEach-Object { $_.File })
            foreach ($g in $order) {
                if ($g -eq $ctrl) { continue }
                $vf = @($runs | Where-Object { $_.Variant -eq $g -and $_.File -like '*.json' } | ForEach-Object { $_.File })
                if (-not $ctrlFiles.Count -or -not $vf.Count) { continue }
                Info "`n--- per run: $ctrl vs $g"
                & $py.Exe @($py.Pre) -I $analyzer compare --control @ctrlFiles --variant @vf --noise-k (Fmt '{0}' $NoiseK)
            }
        }
    } else {
        if (Test-Path -LiteralPath $analyzer) { Note 'Python 3 not found: internal table (bench.json files only, no scene drift check).' }
        if ($ExcludeDrift) { Note 'note: -ExcludeDrift is ignored without Python (no drift check): every run is in the table' }
        # a failed run (run.json status not ok) is left out, as round_analyze.py does
        foreach ($r in @($runs | Where-Object { $null -ne $_.Data -and $_.Status -ne 'ok' })) { Note "warning: $($r.File): run failed (status '$($r.Status)'), left out of the table" }
        $withData = @($runs | Where-Object { $null -ne $_.Data -and $_.Status -eq 'ok' })
        if (-not @($withData | Where-Object { $_.Variant -eq $ctrl }).Count) { Fail "no bench.json for the control '$ctrl' (Python is needed to read logs / CSVs)" }
        Show-InternalTable $withData @($order | Where-Object { $v = $_; @($withData | Where-Object { $_.Variant -eq $v }).Count }) $ctrl
        foreach ($r in $withData) {
            if ($r.Data.PSObject.Properties['compiles'] -and [double]$r.Data.compiles -gt 0) { Note "warning: $($r.File): $($r.Data.compiles) compiles in the window (cache not warm)" }
        }
    }
}

# ----------------------------------------------------------------------------------------- main
$script:Bound = $PSBoundParameters   # (inside a function $PSBoundParameters is that function's)
try {
    if (-not $Mode -or $Mode -in 'help', '/?') { Show-Usage; exit $(if ($Mode) { 0 } else { 2 }) }
    if ($Mode -notin 'record', 'run', 'compare') { Show-Usage; Fail "unknown mode '$Mode' (record, run or compare)" }
    if (-not $env:USERPROFILE -and -not $PSBoundParameters.ContainsKey('BenchDir')) { Fail 'USERPROFILE is not set; pass -BenchDir' }
    if (-not $env:APPDATA) { Fail 'APPDATA is not set (the launcher keeps its settings there)' }
    $BenchDir = [IO.Path]::GetFullPath($BenchDir)
    $Ini = Merge-Ini $IniDefaults $Ini '-Ini'
    if ($Mode -eq 'compare' -and ($SharePlayerCache -or $ResetBenchCache)) { Note 'note: -SharePlayerCache / -ResetBenchCache apply to record and run only' }
    switch ($Mode) {
        'record' { Invoke-Record }
        'run' { Invoke-Run }
        'compare' { Invoke-Compare $null }
    }
    if ($script:FailedRuns -gt 0) { exit 3 }
    exit 0
} catch {
    if ($_.Exception.Data['bench']) {
        Write-Host "bench.ps1: error: $($_.Exception.Message)" -ForegroundColor Red
    } else {
        Write-Host "bench.ps1: unexpected error: $($_.Exception.Message)" -ForegroundColor Red
        Write-Host $_.ScriptStackTrace
    }
    exit 1
} finally {
    # Normally a no-op (each run cleans up after itself); after Ctrl+C or a throw between two
    # cleanups: the game, the environment and every junction this process made.
    try { Invoke-RunCleanup }
    catch { try { Write-Host "bench.ps1: cleanup error: $($_.Exception.Message)" -ForegroundColor Red } catch { } }
    finally {
        try { Restore-LaunchEnv } catch { }
        if ($script:Lock) { $script:Lock.Dispose(); $script:Lock = $null }
    }
}
