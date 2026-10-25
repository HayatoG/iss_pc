# CPU/GPU stall diagnostics

This is optional instrumentation, not a performance patch. It does not disable particles,
change readback accuracy or replace the existing frame, regional and readback statistics.

## Build and enable

Build the Windows runtime with MSYS2 CLANG64 as described in
`port/packaging/windows/README.md`. Keep it separate from a playable release.
Set `TLG_PERF_TRACE=1` in the environment of the diagnostic game process. Use a separate
`ISS_USER_DIR` for experiments and preserve the original saves and caches. Never clear a
player's cache automatically. The direct `--play` launcher now prepares the first-start
shader job list just like the graphical launcher; cancellation prevents launch, generator
failure remains visible and playable. A job list existing on disk does not prove precompilation
succeeded: check runtime startup output as well.

For paired runs, `TLG_PERF_TRACE=0 TLG_PERF_FRAMES=1` records only the common frame
series, with detailed hooks disabled. Both variants include this minimal frame recorder;
the comparison measures the additional cost of detailed tracing, not the cost of all
instrumentation against a stock executable. The PowerShell helper accepts `-NoTrace` for
this variant and `-Python <interpreter>` for an isolated launcher environment. Normal
window closure finalizes output before the runtime's fast exit; crash/forced termination
can still leave an incomplete file.

When tracing is on, **Insert > Performance / Desempenho** provides categories and a
**Mark performance event / Marcar evento de desempenho** button. A marker records the user's
observation. It does not automatically recognize a smoke effect and may be later than the event
if opening the menu takes time. Mark before and after the action. No new gameplay key is intercepted.

## Outputs

Files are written below `<GPU user data>/perf-traces/session-<unique-id>/`:

- `frames.csv`: low-volume series of presented intervals, including startup. Its final comment
  records losses/truncation. Missing terminal comment means the session may have crashed or
  been terminated; never treat it as a complete benchmark.
- `capture-*.jsonl`: up to ten detailed captures, normally triggered by an interval >=100 ms,
  a marker, or absence of a presented frame for 100 ms after frames have begun.
  The last condition uses the consumer thread, so a stalled presentation thread need not
  resume before data can be saved. Pre-first-frame boot hangs remain visible only in legacy
  freeze diagnostics; they are not detected by this frame-based trigger.

The history aims for five seconds before and two after a trigger; bursts can reduce coverage.
Overlapping triggers extend a capture only to a bounded seven-second post-open deadline.
Producer rings are preallocated (128 producers x 4,096 events x 64 bytes, 32 MiB); the consumer
keeps up to 65,536 historical events (4 MiB), plus bounded drain/serialization working memory.
A full ring drops and counts events instead of blocking a game/fault thread. Producer slots
are session lifetime registrations, not an unbounded registry. Overflow and registration
failure are reported in the terminal capture status. Files use unique session directories;
no earlier capture is overwritten or deleted.
Detailed capture footers and the `frames.csv` terminal comment also break down losses into `dropped_ring`,
`dropped_unregistered` and `dropped_reentrant`; older captures have only the total.

Session data is capped at 128 MiB, including a frame series capped at 16 MiB. Quotas are
conservative diagnostics defaults; after a capture limit is reached a marker can still enter
the in-memory event stream but a new detailed capture is not promised. A consumer notice
reports the limit; restart a new controlled session if more detailed captures are required.
Hard termination can leave the final file incomplete. File-system errors stop the affected
output, not gameplay. Do not enable other profilers indiscriminately; measure their overhead.

## Report

No Python dependencies are required:

```text
python scripts/perf_trace_report.py capture-1-1.jsonl --out report --frames frames.csv
python scripts/perf_trace_report.py --frames frames.csv --out frames-only-report
```

This writes `report.json` and an interactive `report.html` (expand each stall to inspect its
supporting events). Use `--start-frame N --end-frame M` to calculate series metrics for a
controlled route, rather than menus/boot. Frame p99 uses nearest rank of positive recorded
intervals. Missing rows, losses, truncation or a missing terminal marker mark the series
incomplete. A p99 from a detailed capture is explicitly a biased capture-only statistic,
never a whole-session p99 or a global 1% low.

## Interpretation

Schema 1 uses numeric events with a session-relative monotonic timestamp, observed presentation
frame ID (0 before the first), local thread ID, operation ID and object/generation context.
Frame context on an asynchronous worker does not prove exclusive ownership by that frame.
Complete BEGIN/END pairs establish the duration of the recorded scope. Same-thread nested spans
have a parent operation; compiler queue operation IDs connect enqueue, worker begin/end and nested work; buffer readback request/apply events also share a request ID across threads. The time to apply start includes queue progress, not GPU execution alone; other cross-thread parentage remains unknown when unavailable.
The report unions overlapping intervals rather than adding them as elapsed wall time. It
labels proximity as association, not proof of cause. Missing endpoints remain incomplete.

Pipeline requests distinguish absence in RAM, known pending graphics compilation and lazy
program loading mode. They do **not** establish absence from the offline jobs list or driver
cache. Shader translation events and disk-program loads are separate. Vulkan creation feedback
(core in required Vulkan 1.3) is attached only while diagnostics are enabled and interpreted
only with a valid result and VALID flag. `APPLICATION_PIPELINE_CACHE_HIT` means the application's
cache helped; lack of that bit says nothing conclusive about an internal driver cache.
Base-pipeline acceleration is not a cache hit. Compile scope durations include nested work;
use their union, not a sum, and inspect the actual identity and feedback.

Texture creation, removal, refresh and uploads carry lifetime generation and an opaque
session-salted origin ID. Recreated origin is a churn candidate, not proof of identical texture
content. GC removals distinguish pressure from ordinary collection; other removal reasons remain unknown at the shared unregister site. Reported upload bytes are
logical guest image bytes, not a measurement of actual transfer bandwidth. Memory pressure
is observed at the existing throttled reporting point.

Windows faults use the existing portable `Common::GetRip` abstraction; only an instruction
offset inside the guest range is recorded. No guest stack walking, game memory, raw address,
texture/buffer content or personal path is serialized. Fault producers use fixed numeric events,
lock-free atomics and a monotonic clock, with no file I/O, allocation or blocking locks. Existing
`TLG_FAULT_PROFILE` remains a Linux-only profiler; it has not been copied into a Windows handler.

Reuse `TLG_READBACK_STATS=1` and `TLG_PERF_DIAG=1` in separately measured diagnostic runs.
`TLG_GPU_PROFILE=1` already provides delayed GPU timestamps: a separate run can help distinguish
GPU pass cost from CPU wait time without introducing a GPU-wide wait.

## Automated benchmark (Windows / renderer v2)

`scripts/bench.ps1` (Windows PowerShell 5.1, no modules) repeats one recorded route under several
variants of environment variables and compares them. It drives the packaged launcher
(`"inFAMOUS Second Son.exe" --play`, or `port\launcher\infamous_run.py` from a source tree); pass
`-Exe` unless the script sits next to the executable or the tree has `port\out` built. Hashtable
arguments need a PowerShell prompt (`.\scripts\bench.ps1 ...`); with `powershell -File` use
`-VariantFile`. Every mode takes `-DryRun`, which prints the plan, folders and environment and
launches nothing. Exit codes: 0 ok, 1 error, 2 usage, 3 when a measured run did not end cleanly.

```text
.\scripts\bench.ps1 record -Exe 'D:\inFAMOUS\inFAMOUS Second Son.exe' -Seconds 90
.\scripts\bench.ps1 run -Exe 'D:\inFAMOUS\inFAMOUS Second Son.exe' -Rounds 3 `
    -Variants ([ordered]@{ A = @{}; B = @{ ISS_TUNING = 'x=1' } })
.\scripts\bench.ps1 compare
```

**record.** First play to a quiet, repeatable spot and let the game save there: every replay
presses Cross through the title menus and takes Continue, so the route starts where the newest
save puts the player. `record` copies the whole `savedata` folder (not only the newest save;
`savedata*` backup folders are left out), `tlg.ini` and the launcher's `settings.json` into the
bench folder (`-BenchDir`, default `%USERPROFILE%\iss-bench`, as `save\`), starts the game on a
copy, and records `-Seconds` of pad input into `route.txt` from the moment gameplay is
detected. The game closes a few seconds after "Route recorded" (`-KeepOpen` leaves it open).
An existing route is kept unless `-Force` (then it moves to `old-<time>\`).

**run.** Up to two warm-up runs per variant first (discarded; `-WarmupRuns N`, default 2, stops
early once a run compiled nothing; `-NoWarmup` skips them: after one warm-up measured runs still
compiled 2 to 21 shaders), then `-Rounds` rounds with each variant once,
the order reversed every other round (A B, B A, A B) so slow drift does not favour one side.
Variants are `-Variants` (name -> `@{ VAR = 'value' }`; use `[ordered]@{}` or `-Control` to pick
the control, a plain hashtable is sorted by name) or `-VariantFile` with the same JSON
(`{"A": {}, "B": {"ISS_TUNING": "x=1"}}`, file order kept). An empty value unsets a variable;
a key `ini:<key>` sets that `tlg.ini` key for the variant. Without variants it runs an A/A pair,
which measures the noise. Every run gets:

- its own data folder (`ISS_USER_DIR`): a copy of `save\savedata` and `tlg.ini`, and copies of
  the small top-level files of the player's folder (`mods.json`...). `-Ini` is merged over the
  defaults `vsync=0`, `fps_limit=0` (`-Ini @{ fps_limit = '60' }` keeps `vsync=0`; a value `''`
  drops a key so the save's `tlg.ini` value stays; the launcher's vsync setting follows), then a
  variant's `ini:` keys the same way;
- NTFS junctions for the shader/pipeline caches only, so they stay warm from run to run:
  `cache\` (port renderer), `gpu-0\` to `gpu-19\` (renderer v2) and the old Windows location
  `user\cache\`. They point into the variant's own cache copy (see *Caches* below). Nothing else
  is linked: `mods\` is read in place through `ISS_MODS_DIR` (unless the shell sets it), `dlc\`
  gets a header-only copy (each `.pkg` cut to 4 KB and each `sce_sys\param.sfo`: all the launcher
  reads for `ISS_ADDCONT_LABELS`), and other folders (screenshots, save backups...) are not
  visible to the run. Every junction is tracked as it is made and removed after the run, also
  after an error or Ctrl+C. If PowerShell itself was killed, the next `record`/`run` on the same
  bench folder finds the leftover junctions under `record-*\user\` and `runs\*\*\user\` and
  removes them (only the links; a warning lists them). To delete such a folder by hand use
  `cmd /c rmdir /s /q`, never `Remove-Item -Recurse` (Windows PowerShell 5.1 follows junctions
  into their targets); the script's own deletes (`save.new\`, `-ResetBenchCache`) refuse a
  folder that contains a junction or link. The bench folder and the player's data folder may not
  contain each other, and `bench.lock` keeps a second `record`/`run` off the same bench folder;
- its own `APPDATA` with a copy of the launcher settings and a pointer file without the extra
  backup folder: the launcher backs saves up before each start and would otherwise mirror (and
  prune) the copies into the player's extra backup folder;
- the variables below, the variant's variables, and the other variables of the shell. A `TLG_`
  twin of a variable the script or a variant sets is unset for the run, because the game copies
  `ISS_X` to `TLG_X` only when `TLG_X` is unset.

**Caches.** By default the benchmark never writes into the player's caches. Each variant has its
own copy in `<BenchDir>\cache\<variant>\` (the recording uses `cache\_record\`). On its first use
the copy is seeded by copying the player's `cache\`, `gpu-0\`...`gpu-19\` and `user\cache\`
folders (robocopy, read only on the player's side; nothing is moved, junctions are not followed,
`<serial>.incompatible` leftovers are skipped); `seed.json` records where from and when, and later
runs and sessions reuse the copy. Without a player cache the copy starts empty and the warm-up
builds it. Seeding copies the whole cache once per variant, so it needs that much disk space; a
copy whose seeding was interrupted (no `seed.json`) is deleted and seeded again. This matters
because the pipeline cache belongs to one shader profile: the ISS_TUNING keys
`lds_barriers_multi_wave` and `wave_memory_barriers` are part of it, and a run with another profile
moves the cache aside to `<serial>.incompatible` after deleting the previous one, so an A/B/A on a
shared cache would lose it. With per-variant copies that only happens to the bench's copy.
`-ResetBenchCache` deletes the copies of the variants of this `run` (or `cache\_record\` for
`record`) and seeds them again from the player's current caches.

`-SharePlayerCache` keeps the earlier behaviour: the junctions point into the player's folder
(else into `<BenchDir>\cache\_shared\`), so no copy is made, but every run writes into the
player's real shader and pipeline caches (a warning says so). It refuses, before anything starts,
a variant (or the shell, for variants that do not override it) that sets one of those profile
keys in `ISS_TUNING` or sets `ISS_PIPELINE_CACHE_ROOT`. Without it, a variant with its own
`ISS_PIPELINE_CACHE_ROOT` keeps its pipeline cache there (not seeded; it may not point into the
player's folder).

A run killed on timeout can leave a cache file half written. The Vulkan driver cache
(`<serial>.vkcache`) carries a checksum and a damaged one is discarded on load (its periodic
checkpoints are also replaced atomically), but a truncated shader or pipeline entry (`.key`,
`.meta`, `.spv`, written in place) is not detected and can make the next start crash while the
cache loads. By default the benchmark uses its own cache
copies under `<BenchDir>\cache`, so this never affects your normal game; use `-ResetBenchCache`
to rebuild them. With `-SharePlayerCache`, use the launcher's **Clear shader cache** (Graphics
page, or `--clear-shader-cache`) if that happens.

Each run waits up to `-TimeoutSec` (default 600). When the game reports that the route file is
empty or unreadable (`Runtime: pad replay file empty or missing`, or a replay of 0 samples), the
run is stopped at once with status `empty-route` and the session ends with an error (every run
replays the same file: record it again). On timeout, or when the game does not exit
30 s after its summary or after the route ended without one, the script creates the stop file,
waits 30 s for the launcher and the `tlg-probe*.exe` it started, then kills what is left. Only
probes in `<game folder>\out\` started after the run began are ever stopped; a launcher that
does not exit 60 s after the game closed is killed too (`launcher hung`). A run folder
`<BenchDir>\runs\<session>\NN-<variant>\` keeps `bench.json`, `game.log` (copied from
`<game folder>\out\logs\`, which keeps the last 30 logs: benchmark runs push older ones out),
`launcher-out.txt`/`launcher-err.txt`, `env.txt` and `run.json` (status, exit code, renderer,
time). A missing `bench.json` is rebuilt from the `Bench:` log line. The environment of the
calling shell is restored afterwards, also after Ctrl+C.

**compare.** For the newest session (or `-Session <name|folder>`) it runs
`python -I scripts\round_analyze.py bench` on the runs and prints, per variant, the mean and
sample standard deviation of `avg_fps`, `low1_fps`, `p99_ms`, `worst_ms`, `over33`, `over50`,
`compiles`, `cpu_ms/f` and `cpu_game_ms/f`, and the difference from the control in percent.
On a GPU-bound machine a CPU-side change does not move the FPS, but it shows in the last two
columns (lower is better). `cpu_ms/f` is the process's CPU time (user + kernel, all threads,
`GetProcessTimes`) per frame of the window. Windows charges that time by sampling: at each
15.625 ms clock tick the thread running on a core is charged a whole tick, so it comes in whole
ticks, and work that a timer wakes (the vblank ticks, frame pacing) mostly runs between two ticks
and is not charged. How much is missed depends on the phase of that work against the tick, which
changes from run to run: identical runs gave 3.4 to 16.5 ms/frame at the same FPS. Use
`cpu_game_ms/f` instead: guest + command processor + GPU helper threads (see `cpu_<category>`
below), from each thread's cycle time (`QueryThreadCycleTime`, exact), without the background
shader/pipeline compiles. In an A/A session (6 runs, same build) `cpu_ms/f` read 9.6 to 12.0
(CV 8.8%) while the cycle time was about 120 ms/frame (about 7 cores busy: `cpu_ms/f` misses
over 90% of it; the command processor alone: 11 ms/frame in cycles, 0.01-0.04 in ticks) and
`cpu_game_ms/f` had a CV of 2.1%. The background compiles were 0.00-0.01 ms/frame there: they are
not what made `cpu_ms/f` swing. `~` marks a difference within twice
the control's standard deviation (`-NoiseK`): probably noise. With a single control run, or a
standard deviation of 0 (identical control runs), there is no noise estimate and no `~`.
`-Detail` adds the per-run `compare` table. The table is also written to `compare.csv` and
`compare.json` in the session. Without Python the script prints the same table itself from the
`bench.json` files. A warning flags a measured run that still compiled shaders.

**Failed runs.** A measured run whose `run.json` status is not `ok` (crash, timeout, exit code 3),
or that has no bench summary or window (no `bench.json`, no `Bench:` line, so only its whole
`game.log` could be read: loading screens at hundreds of FPS, not the route), is left out of the
table and the drift check. A WARNING block lists each one with the reason; `compare.json` has
`failed_runs` (run, variant, reason), `compare.csv` a `failed_runs` count per variant and
`compare_runs.csv` a `failed` column. A variant whose runs all failed is listed as excluded, and
if every control run failed `compare` stops. A game log passed to `round_analyze.py bench` by hand,
outside a session folder (no `run.json`), is still read from its `Frame stats` lines.

**Scene drift.** The replay is time-based pad input, so past ~20 s a run can diverge (the camera
or the player ends up elsewhere) and render a lighter or heavier scene; its FPS then differs for
reasons that have nothing to do with the variant. `compare` therefore fingerprints each run by
the `draws/frame` of the `Frame stats` lines (every ~5 s) inside the benchmark window of its
`game.log` (from `Bench: window started` to `Runtime: pad replay finished`; a `frames.csv` with
the `bench` column is the fallback; a window that never ended, after a crash or timeout, counts as
unknown) and adds a `draws/frame` column (mean +- stdev per variant) and a `drift` column (drift
runs / runs, `?N` for N runs without draws, `unchecked` with fewer than 3 runs with draws). A run whose mean draws/frame is more than `-DriftPct`
(default 5, `--drift-pct`) off the median of **all** runs of the session (every variant; at least
3 runs with draws) is a drift run: a WARNING block lists it with its deviation, and the diffs of
its variant (of every variant when it is a control run) get `!` (unreliable, whatever `~` says).
The default comes from an A/A session of identical builds: the good runs stayed within 2.8% of
the median, the two drift runs were 14% and 22% below it, so 5% sits well clear of both. A run
without `Frame stats` lines in the window is reported as unknown and not checked. `-ExcludeDrift`
(`--exclude-drift`) recomputes the table without the drift runs and says how many remain (a
variant whose runs all drifted is listed as excluded, no comparison; `-Detail` and the no-Python
table ignore it). When half the runs or more are off the median, the median sits between two
scenes and means nothing: a WARNING says the check is inconclusive and no run is flagged. A
variant whose runs are all off the control's on the same side but within 5% of each other (and the
control's runs as tight) draws differently by itself (culling, LOD...): a note says so, without
`!`, since that is the change under test, but its FPS deltas are then not like-for-like.
`compare.json` (`drift`: threshold, median, state, each run's series, mean, deviation, flag;
`run_files` lists every input, `kept_run_files` those in the table; `excluded_variants`) and
`compare.csv` (its earlier columns, then `draws/frame`, `drift`, `unreliable`, `failed_runs`,
`cpu_ms/f`, `cpu_game_ms/f`; `-` for runs of a build without them) carry the same
information, and `compare_runs.csv` has one line per run (with its CPU values: `cpu_ms_per_frame`,
`cpu_game_ms_per_frame` and each `cpu_<category>_ms_per_frame`; `compare.json` compares each
category per variant too, rows with `split: true`, and lists them per run under `drift.runs[].cpu`). Excluding
is a stopgap (fewer runs, more noise). Frequent drift means the route is too long or too
dependent on timing: re-record a shorter, simpler one (stand still or walk straight, fewer camera
swings and no combat or traversal that depends on physics) and run the session again.

**Source tree (local MSYS2 build).** `scripts\play_dev.ps1 [-GameDir <folder with eboot.bin>]`
plays the game from the tree (`py -3 port\launcher\infamous_run.py`; extra arguments go to the
launcher): it puts `-MsysBin` (default `C:\msys64\clang64\bin`, the probe's SDL3/FFmpeg/libc++
DLLs) in front of `PATH` for that process and sets `ISS_GAME_DIR` from `-GameDir`, else
`ISS_GAME_DIR`, else the launcher's saved `game_dir`, else `game\CUSA00223`. Saves and caches go
to `port\user` (unless `ISS_USER_DIR`), logs to `port\out\logs`. The launcher builds
`out\isa-rewrite.bin` with `iced-x86` (`py -3 -m pip install --user iced-x86==1.21.0`), which
every Intel CPU needs (no SSE4A). `bench.ps1 record`/`run` do the same when they fall back to the
source-tree launcher (`-GameDir`, `-MsysBin`; PATH only when `SDL3.dll` is not on it yet) and
restore both variables when they end.

**Commits.** Every session records in `variants.json` (`build`), and `record` in its `run.json`,
the commit (`git rev-parse HEAD`), branch, whether the tree had uncommitted changes (`dirty`,
`dirty_files`; changes inside the `port/gpu/third_party/*` submodules are ignored) and each probe
exe (`tlg-probe-v2.exe`, `tlg-probe.exe`: path, time, size, SHA256). `run` prints it at the start
and `compare` in its header. To compare commits, run one session per build with the variant
named after the commit, from the same route and save:

```text
git checkout <commit A>; <build>; .\scripts\bench.ps1 run -Session c-A -Variants ([ordered]@{ A = @{} })
git checkout <commit B>; <build>; .\scripts\bench.ps1 run -Session c-B -Variants ([ordered]@{ B = @{} })
py -3 -I scripts\round_analyze.py bench (Get-ChildItem $env:USERPROFILE\iss-bench\runs\c-A\[0-9]*\bench.json,
    $env:USERPROFILE\iss-bench\runs\c-B\[0-9]*\bench.json).FullName --control A
```

`compare` reads one session only; `round_analyze.py bench` takes run files from any number of
sessions and groups them by label (the variant name), so give each commit its own variant name.
Each variant name gets its own cache copy (`cache\<variant>\`, seeded from the player's caches on
first use), so keep the warm-up: a build that changes shaders compiles them again there. Runs of different sessions are not interleaved, so drift
between them (temperature, background load) is not cancelled out: repeat the first session last
(A B A) to see it.

`round_analyze.py bench` reads bench JSON, `summary.json` from `summarize`, a game log (its last
`Bench:` line, else its `Frame stats`/`Frame pacing` lines) or a `SHADPS4_FRAME_CSV` file, and groups
runs by label, else by folder name without the `NN-` prefix. `compare --control ... --variant ...`
also accepts bench JSON files; its columns name the run folders (`01-A`, `03-A`, or
`label (folder)`). A `SHADPS4_FRAME_CSV` with the `bench` column is cut to the benchmark window:
from the `bench_start` row (or the first `1`) up to before `bench_end`, or to the end of the file,
because with `ISS_BENCH_EXIT` the game may exit before writing the `bench_end` row. `bench_start`
marks the window's start before `ISS_BENCH_SKIP_MS`, so `--skip-ms` is applied from there. The
CSV uses the game's p99 and 1% low definitions (`low1_fps = 1000 / p99_ms`). A CSV without the
column, and the log fallback, cover the whole session, menus and loading included: treat them as
diagnostics (the "cache not warm" warning is only given for the game's own summary).

Variables of the route and the summary (the game copies `ISS_X` to `TLG_X` at start, only when
`TLG_X` is unset):

| Variable | Meaning |
|---|---|
| `ISS_PAD_RECORD=<file>` | Records pad input (lines `ms buttons lx ly rx ry l2 r2`). Without `ISS_PAD_RECORD_SECONDS`, F9 starts and stops. |
| `ISS_PAD_RECORD_SECONDS=N` | Records N seconds, starting when gameplay is detected (over 1000 draws per frame twice in a row). |
| `ISS_PAD_REPLAY=<file>` | The route to replay. Started by `replay` in `ISS_PAD_FILE`, or by `ISS_PAD_REPLAY_AUTO`. |
| `ISS_PAD_REPLAY_AUTO=1` | Taps Cross every 2 s until gameplay is detected, then replays the route (wall-clock timed). |
| `ISS_PAD_FILE=<txt>` | Manual alternative: held button names, stick values, or `replay`. |
| `ISS_FRAME_STATS=1` | `Frame stats` / `Frame pacing` log lines every 5 s (the launcher's default). |
| `ISS_BENCH_JSON=<path>` | When the replay ends, writes `frames, seconds, avg_fps, low1_fps, p99_ms, worst_ms, over33, over50, compiles, cpu_ms_per_frame, cpu_s, cpu_util, label`. The log always gets `Bench: frames=... label=...`. `cpu_s` is the process CPU time (user + kernel, all threads; `GetProcessTimes`, Linux `CLOCK_PROCESS_CPUTIME_ID`) over the counted window, `cpu_ms_per_frame` = `cpu_s` per frame, `cpu_util` = `cpu_s` / `seconds` (CPU cores busy on average; on Windows tick-sampled, see **compare**). Windows also adds the window's CPU time per thread category, from cycle times (`QueryThreadCycleTime`, TSC cycles, converted with the TSC rate measured over the window; threads listed with Toolhelp32 and named by `GetThreadDescription` at both ends of the window): `cpu_guest_ms_per_frame` (threads in the runtime's guest thread registry, `runtime_guest_thread_ids`: the game's own threads and `main`), `cpu_cp_ms_per_frame` (`GpuCommandProcessor`), `cpu_gpu_helpers_ms_per_frame` (`GpuCommandRecorder`, `NativePasses*`, `Scout`, `GpuUploadWorker`, `PresentThread`, `DrawStageB`, `AscCP`, `GpuSchedPriorityPendingOpsRunner`), `cpu_compile_ms_per_frame` (`Precompile*`, `AotCompute*`, `SpecTranslate*`, `PipelineCompiler*`, `PipelineCacheIO`, `ShaderWarmup`), `cpu_other_ms_per_frame` (everything else: the window and loading-screen threads, the Vulkan driver's threads, and the threads that exited during the window, whose time is only in the process total) and `cpu_cycles_ms_per_frame` (the process's cycle time, `QueryProcessCycleTime`: the categories add up to it), plus `cpu_threads_exited`. The log line `Bench CPU threads:` lists the 12 busiest threads (cycle ms/frame, category, and the tick-sampled `GetThreadTimes` value next to it). Linux: no categories (thread names are cut at 15 characters there, which merges the command processor and the recorder). |
| `ISS_BENCH_LABEL=<text>` | `label` of the summary (`bench.ps1`: the variant name). |
| `ISS_BENCH_SKIP_MS=<ms>` | Start of the replay window left out of the summary (default 1000; `bench.ps1 -SkipMs`). |
| `ISS_BENCH_EXIT=1` | The game exits cleanly after the summary. |
| `ISS_STOP_FILE=<path>` | The game exits cleanly (caches checkpointed) when the file appears. |
| `SHADPS4_FRAME_CSV=<path>` | Renderer v2: one CSV row per presented frame from the start of the process (`frame, present_ms, draws, dispatches, shader_compile_ms, ..., shader_compiles, ..., gpu_profile_enabled, bench`; `bench` is `0` outside the benchmark window, `1` inside, `bench_start` on its first row, `bench_end` on the first row after it); its folder also receives the GPU black-box dumps. `bench.ps1 -FrameCsv` sets it per run. `perf_trace_report.py` reads the port renderer's `frames.csv`, not this file. |

Measure with VSync and the FPS limit off (the `-Ini` defaults), the same window mode and resolution,
nothing else running, and the laptop on mains power. A difference smaller than the noise of three
rounds needs more rounds, not a conclusion. For a cold-cache variant, point
`ISS_PIPELINE_CACHE_ROOT` at an empty folder in that variant only (not with `-SharePlayerCache`).

## Tests and rollout

The standalone recorder test does not need Vulkan or MSYS2:

```text
cmake -S port/tests/perf -B <build-dir>
cmake --build <build-dir> --config Debug
ctest --test-dir <build-dir> -C Debug --output-on-failure --timeout 15
python -m unittest discover -s port/tests -p test_cli_precompile.py -v
python -m unittest discover -s port/tests -p test_perf_trace_report.py -v
python -m unittest discover -s port/tests -p test_perf_build_revision.py -v
python -m unittest discover -s port/tests -p test_round_analyze.py -v
```

Tests use temporary synthetic data, not the game. Never publish local captures or game assets
with the contribution. Before using results to choose an optimization, run three paired
2–3-minute routes on the same instrumented build with tracing off/on, preserving resolution,
VSync, save, action sequence, driver, memory budget and warmed cache. Initial overhead goals
are <=2% FPS and <=5% frame p99; these are acceptance goals, not measured achievements.
Only then separately test a larger memory budget or different warmup policy. Compare visuals.

Coverage intentionally remains unknown for jobs-list membership, non-compiler cross-thread queue
relationships and some resource removal reasons; do not claim those were implemented. Gameplay
cost, overhead goals and improved FPS require measured runs and cannot be certified by unit tests.
