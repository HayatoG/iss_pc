# Native ultrawide candidate — implementation and validation record

Status: **experimental Windows Renderer v2 candidate**, not installed over the normal launcher.
Target source: HayatoG/iss_pc `developer`, pinned at `c8915d95f48421cf489708a35fa4e58e7f93c7f1`.
Target game: **CUSA00223 / APP_VER 01.07**. Other regions are refused even if some offsets match.
Local ELF SHA-256: `b3afd173a379f83e1e408e71fdef22b78f93092d332b79decd48a18a08bfc605`.

## Source research, rather than release-note assumptions

The reference is [second-son-v1.1](https://github.com/axylpup/shadPS4/releases/tag/second-son-v1.1), pinned to
[`143619c6fd81b16fd013f742f7b4336e092129ba`](https://github.com/axylpup/shadPS4/tree/143619c6fd81b16fd013f742f7b4336e092129ba).
The moving `second-son` branch is not used as an unversioned specification.
The actual game patches are in [`src/common/memory_patcher.cpp`](https://github.com/axylpup/shadPS4/blob/143619c6fd81b16fd013f742f7b4336e092129ba/src/common/memory_patcher.cpp), not a downloadable ultrawide XML patch.
`memory_patcher.h` exposes settings/status, and `src/core/libraries/gnmdriver/gnmdriver.cpp` invokes the game-thread resize handler.

| Reference change | Actual mechanism | Treatment in this port |
|---|---|---|
| [55fdf965](https://github.com/axylpup/shadPS4/commit/55fdf965fe04b847f363da2ace793c164f246068) | Eight immediate operands in the game's renderer initialization and mode-reset functions set render buffers, video output and logical screen dimensions. The camera consumes the logical ratio. | Reused as full-instruction, US-verified startup patches. Changes actual game scene dimensions. |
| [5afba802](https://github.com/axylpup/shadPS4/commit/5afba8028feead6927ad9947d32689242b74cb4e) | Three copies of HUD margin code have their 0.85 safe-area floor removed, and vertical scaling is separated from horizontal safe area. | Reused with exact US instruction bytes. Runtime safe-area service supplies full-width or centered 16:9 HUD. |
| [a053075a](https://github.com/axylpup/shadPS4/commit/a053075aae3d9f2a6f4f335d34afa20ab9ec3dfd) | Centered HUD default because a full-width minimap was reported clipped. | Both placements selectable. Edge placement is experimental at each aspect; centered is the fallback choice. |
| [160de347](https://github.com/axylpup/shadPS4/commit/160de34701e1859b5e52e377ee98333e289fc78c) | Keep UI coordinates at 1080 logical lines, independent of physical render height, avoiding oversized UI. | Reused. Logical width is the exact selected aspect times 1080: 2580 for 3440×1440; 3840 for 5120×1440. |
| [850d128a](https://github.com/axylpup/shadPS4/commit/850d128a63f8a1878edb6b5d8120e425f2b9b870) | Redirect projection builder's 2.0 constant to 2/k for displays narrower than 16:9. | Not needed for the offered 16:9–32:9 presets. No narrow-screen or camera-FOV constant patch imported. |
| [6ec09476](https://github.com/axylpup/shadPS4/commit/6ec09476c83bffc2d3b198d9b0d20fb57252e728) | Screen percentage derives scene pixels from the monitor reference, saved in render-size.ini. | Adapted to selected output presets and tlg.ini. 100% is exact output pixels. Other percentages round to even pixels. Requests above the 4K pixel budget are refused, never silently reduced. |
| [63692218](https://github.com/axylpup/shadPS4/commit/6369221821def1f88bd3bc64a869e8616bc1ee68) | Expand both allocation and descriptor record count of the game's per-tile light list. Each 16×16 tile needs 128 uint16 indices. Original 0x320000 entries cover only 25,600 tiles. Unwritten end markers can hang forward-light shaders. | Reused, independently disassembled in US 1.07. Both capacity sites are mandatory parts of the same all-or-nothing transaction as resolution/HUD changes. |
| [54c1b399](https://github.com/axylpup/shadPS4/commit/54c1b39910b250275214bac1e16f183a1250b0ac) | Live resize via sceGnmSubmitDone, game mode-toggle option byte, render-size confirmation table, boot allocation headroom and retries. | Not imported. Scale selection requires restart, so allocations are created once at the selected dimensions. |
| [cc569952](https://github.com/axylpup/shadPS4/commit/cc56995255d1b61a2dc41c2c27a47246ad5b0ed6) | Texture-cache purge after first frame at a new live size prevents stale lighting/history/photo-mode shadows when returning to an old size. Files: texture_cache.cpp/.h, memory_patcher.cpp/.h. | Emulator/cache-specific. No live resizing offered, so no broad cache-purge policy transplanted into Renderer v2. |
| [84b6b48c](https://github.com/axylpup/shadPS4/commit/84b6b48c87198444066e93d68cb1a311fc3b5a4d) | Scale DLSS-reported jitter phases by actual render dimensions / 1920×1080 in vk_rasterizer.cpp. | Emulator DLSS integration, not a general game-camera fix. This Renderer v2 path uses its existing spatial presenter and original game post-processing. No DLSS code or unverified jitter writes imported. |

## Architecture and verified US sites

The port maps the native game image, resolves its service calls, and translates GPU commands in Renderer v2. It does not use shadPS4's guest process loader or its global MemoryPatcher lifecycle. The reusable pieces are changes to the **game's own renderer setup**, not the emulator's host window size.

`iss_native_resolution.h` computes a plan and preflights every instruction before writing. `probe.c` additionally verifies executable segment coverage and game metadata, after external patches and CPU rewriting. CPU-dependent rewrites or another mod may cause a safe refusal; no signature guard is bypassed.

| US image offset | Verified role |
|---|---|
| 0x1eff47, 0x1eff4e | Scene render width / height stores |
| 0x1eff55, 0x1eff5b | Game output buffer width / height stores |
| 0x1f03ff, 0x1f03f8 | Logical screen width / height stores |
| 0x1f0d3d, 0x1f0d4c | Normal-mode dimension reapplication |
| 0x1f042f, 0x1f2aae, 0x1f2bae | HUD safe-area floor |
| 0x1f0471, 0x1f2afb, 0x1f2bfb | Independent vertical HUD margin |
| 0x16be04 | `imul esi,r15d,0x320000`: light-list allocation element count |
| 0x16be1d | `mov r8d,0x320000`: light-list descriptor record count |
| 0x5802e6, 0x58030c | Read-only guards on logical width/height addressing and camera aspect division |

The logical screen globals are 0x1d0ccc0 / 0x1d0ccc4. The camera divides width by height; for these wide presets the existing projection gives Hor+ behavior. Vertical FOV and existing speed/diagonal hooks are not modified.

Light capacity is `max(0x320000, align_up(ceil(width/16) * ceil(height/16) * 128, 65536))`.

| Scene at 100% | Tile count | Allocated list entries |
|---|---:|---:|
| 3440×1440 | 19,350 | 3,276,800 (original capacity sufficient) |
| 5120×1440 | 28,800 | 3,735,552 |
| 3840×2160 | 32,400 | 4,194,304 |

The game subsequently creates its scene/depth/post-processing surfaces from these startup dimensions. Renderer v2's existing texture descriptors and render-state dimensions follow the actual game surfaces. Its presenter uses the guest image's dimensions and aspect, rather than forcing 16:9. Host window dimensions alone are not counted as proof of native rendering.

The candidate logs `NATIVE_SCENE_TARGETS color+depth=... viewport=...` when an actual matching scene color/depth pass is encountered, and `NATIVE_GUEST_OUTPUT ...` before host FSR/post-processing. A color/depth log still needs gameplay/frame inspection to identify the intended world pass; a final-output log alone is insufficient proof.

## Controls and limits

Insert → **Native rendering** is visible without advanced mode. Select output resolution, 25–200% render scale, and edge/centered HUD. Close and relaunch the game to apply. No live resize or dynamic-resolution algorithm is added.

Presets: original/off, 1920×1080, 2560×1440, 3440×1440, 3840×2160, 5120×1440. Maximum scene pixel count is 8,294,400, following the reference's final native-4K limit. For example 5120×1440 at 200% is refused; 100% remains exactly 5120×1440. Aspect names such as “21:9” are not substituted for the panel's actual 43:18 ratio.

Settings in the active user folder's tlg.ini (ISS_CONFIG/TLG_CONFIG):

```ini
iss_native_output=3
iss_native_percent=100
iss_native_hud_edges=1
```

Output index 3 is 3440×1440; 5 is 5120×1440; 4 is native 4K; 0 disables the feature. Use a matching desktop resolution and Borderless for pixel-matched display output. A smaller host window will still downsample the native scene; the menu reports both guest and window sizes. External resolution XML patches must be disabled; conflicts refuse the native transaction.

Camera speed and pitch hooks remain the developer versions byte-for-byte. The existing tested Karma Switcher, shard accounting, persistent original state, assisted-purchase rollback and backup UI have been ported forward. Experimental all-powers grants are excluded. Existing overlay scaling/cursor fixes and borderless Fullscreen compatibility behavior are retained. No controller mappings, stick processing, story progression or collectibles are changed by native rendering.

## Automated validation

```powershell
cmake -S port/gpu_v2 -B ../native-build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build ../native-build --target tlg-probe-v2 test_native_resolution test_karma_regression
ctest --test-dir ../native-build --output-on-failure
# Optional: validate every native plan against a legally supplied local US ELF.
../native-build/test_native_resolution.exe C:/path/to/eboot.elf
```

The native test covers exact 100% dimensions, every offered percentage and preset, pixel-budget refusal, Hor+ geometry, logical aspect, both lighting counts, strict config parsing, US/EU/version rejection, truncated images, every corrupted signature byte, all-or-nothing writes, unchanged bytes outside the patch plan, safe-area and refusal fallback. It does not assert that a mathematical identity proves the running game's camera behavior.

Also run locally: existing generated-machine-code camera speed and diagonal safeguard tests; Karma regression including full YMM/flags preservation; persistent store corruption rejection; complete save/metadata backup tests. Actual US ELF patch plans accepted. Full Windows Renderer v2 executable builds successfully; existing upstream compiler warnings remain. Linux has not been compiled or tested for this candidate.

Isolated Windows startup runs also produced these independent renderer reports:

| Selected resolution | Scene color/depth pass | Viewport | Guest output before host scaling | Lighting entries |
|---|---|---|---|---:|
| 3440×1440, 100% | 3440×1440 | 3440×1440 | 3440×1440 | 3,276,800 |
| 5120×1440, 100% | 5120×1440 | 5120×1440 | 5120×1440 | 3,735,552 |

Both runs loaded the camera 1.5×, diagonal safeguard and Karma Switcher hooks. A raw pre-host-scaling PNG was captured at 5120×1440 during the Sony startup sequence. This confirms buffer dimensions, not final gameplay composition or correct authored-movie aspect. Both tests used hidden windows and copied saves/mod state; their audio was audible. They were deliberately stopped after the startup capture (Windows ignores the probe's --timeout option, so the test wrapper enforces a stop). All 21 protected normal save/mod-state/settings files were unchanged. No stability-duration claim follows from a controlled stop.

## Required real-game validation and remaining research

### Exclusive fullscreen candidate

The user requested actual exclusive fullscreen in addition to the working Borderless mode. The reference fork v1.1 contains two directly relevant commits: [5e96c249](https://github.com/axylpup/shadPS4/commit/5e96c249c45c0ffd876e4db67227214623740018) and [f6f32614](https://github.com/axylpup/shadPS4/commit/f6f326149ae7ca3ecbe3d1ac56856dbce89e5fb6). Its original fix avoids an unsynchronized whole-device idle wait during swapchain rebuilding. The follow-up submits empty fenced markers under the queue-submit mutex and waits outside it, so later dependency-signaling submissions can still proceed. The out-of-date rebuild after presentation also moves outside the submit lock. Its ImGui texture deletion waits for its own synchronized queue instead of every device queue. These mechanisms are adapted here, always enabled for this candidate.

The additional option `display_mode=exclusive` retains the existing keys and indices: Windowed=0, Borderless=1, legacy Fullscreen(borderless)=2, Exclusive=3. Renderer v2 enables `VK_EXT_full_screen_exclusive` when available, queries support for the actual Win32 surface/monitor, and requests application-controlled exclusivity. Native rendering's menu reports **acquired** only after `vkAcquireFullScreenExclusiveModeEXT` succeeds. A failed support query or exclusive swapchain creation returns to Borderless and logs the reason; a pending acquisition is reported as pending, not relabeled as exclusive.

The first explicit-exclusive test was unsuccessful: the driver repeatedly returned `VK_ERROR_INITIALIZATION_FAILED` (-3), and presentation of the unacquired application-controlled swapchain returned exclusive-mode-lost. The world/audio continued; Alt+Enter back to Windowed immediately restored the picture. This is not a successful exclusive validation. The follow-up prevents presentation before ownership is acquired, permits at most three initialization-failure attempts one second apart, then returns to Borderless without claiming success. It uses a monitor-sized SDL window with no simultaneous SDL display-mode change, leaving exclusive ownership to Vulkan; the active window is topmost only while focused, following the window preparation used by the Khronos sample. Whether this changes the driver's refusal still requires a visible test. Failure logs include client/monitor geometry and focus/topmost state. No Windows settings or driver settings are changed.

The presenter releases ownership when exclusive focus is lost, skips presentation while minimized/changing modes, treats `VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT` as recoverable, and retries acquisition. Image-acquire timeouts skip frames without submitting unsignaled semaphores or resetting the frame's presentation fence. Device-loss errors remain failures rather than being mistaken for ordinary Alt-Tab loss. Queue-drain dependency and WSI result handling have automated tests; actual acquisition, repeated Alt-Tab, mode changes, cursor alignment and long-run stability require the next visible Windows test. The reference fork's SDL fullscreen implementation alone does not prove application-controlled Vulkan ownership or guarantee no driver issues on this PC.

### Streaming freeze found during gameplay

The first visible native test froze after approximately five minutes while traversing rooftops and using a floating power. Audio continued. The saved log reported no new frames; the last GPU memory usage was about 4 GB, without a logged device-loss error. An offline minidump (no live debugger attachment) identified this lock cycle:

- Asset reader: `full_io` → guest write fault → `TextureCache::InvalidateMemory`, holding texture-cache mutex → speculative streak `MarkAsMaybeDirty` (`texture_cache.cpp:446` in that build) → `XXH3_64bits` touches a GPU-protected page → nested read fault → `BufferCache::ReadMemory` → `Liverpool::SendCommand<true>` waits for the command processor.
- Command processor: `NativeComputeExecute` → `TextureCache::FindImage` waits for that texture-cache mutex.

The follow-up candidate defaults `ISS_TEX_FAULT_STREAK_KB` to **0** and the local test launcher explicitly sets 0. Normal per-page invalidation remains enabled; speculative look-ahead hashing is disabled. This targets the exact captured path while preserving native dimensions, lighting capacity, all six menu tabs, camera and Karma features. It is a workaround for this optimization, not a proof that every hash/invalidation path is deadlock-free. The performance benefit of streak handling is forfeited; streaming may have more small fault-related stalls. The same traversal needs at least 15–20 minutes of retesting before calling the candidate stable. No reduction to 16:9 or render scale was used.

Evidence: `work/native-freeze-20261022.log`, `work/native-freeze-20261022.dmp`, `work/native-freeze-offline-stacks.txt`. Dumps contain process data and are kept locally, outside the candidate package.

The implementation is a compiled candidate, **not a verified stable ultrawide release**. These remain required:

1. Visible 3440×1440 and 5120×1440 **gameplay** captures showing matching scene color/depth targets, viewport and guest output. Startup target/viewport/output dimensions have been confirmed above; gameplay composition is still unverified. A real 32:9 display/tester is needed for the latter's presentation/HUD check.
2. Same-position 16:9 versus ultrawide comparison: identical vertical framing, additional world at sides, normal character proportions. Compare menus/minimap/health at both HUD placements.
3. Bright/dark scenes with many lights, Smoke/Neon/Video/Concrete effects, reflections, transparency, bloom, depth of field, temporal edges, photo mode and transitions. Verify lighting-list expansion prevents the reference failure on this renderer/GPU.
4. Startup logos/movie composition: prior scaled-ultrawide builds cropped the Sony/Sucker Punch logos. This candidate does not claim to fix them. Correct letterboxing of authored movie quads may require additional game-code/resource research; do not solve it by cropping/stretching gameplay.
5. Audit the game's fixed 1080p jitter constants if native temporal effects show artifacts. The reference's DLSS jitter reporting fix is not evidence that changing game jitter is safe here.
6. Stress Alt-Tab, resizes, Insert layout/cursor, loading screens and startup time. Check camera/diagonal behavior and Karma switch/reset on copied saves.

No additional game dump is needed to construct the verified US startup patch. Additional runtime/game-function research may be needed for authored movie quads, fixed-resolution post-processing assumptions, different game regions, or future live resizing. Untested effects are not labeled corrected merely because resolution guards and unit tests pass.

Standalone Win32 Vulkan isolation on RTX 3080 Ti: FIFO exclusive acquisition succeeds; Mailbox fails with -3. Format RGBA8, three images, and transfer usage each succeed with FIFO, including combined settings. Exclusive candidate now forces FIFO only in explicit exclusive mode. Actual game acquisition and Alt-Tab validation remain pending. Probe source/logs are in workspace work/exclusive_probe.c and exclusive-probe-*.log.

## Cross-save Karma incident — October 23, 2026
Reset applied the early-story profile baseline (330, Good rank 1, 11 shards) after a different 100% save was loaded. Both disk storage and runtime caching are profile-wide, not save-bound. An autosave recorded the wrong karma. User2 manual Light (True Hero) 100% matches the earlier backup and was preserved separately. The quarantine candidate blocks snapshot load/save and disables Karma action buttons; it does not implement save-specific Karma tracking. A production-storage regression verifies legacy stores remain unchanged and fresh profiles cannot create unbound baselines. Five CTest checks pass. Native ultrawide and exclusive FIFO presentation are retained. Normal installation and published ZIPs have NOT been updated and still require the cross-save fix before further Karma use. Full save-lineage tracking, adoption of previously modded saves, and in-game validation remain outstanding.

## Save-specific Karma candidate
The next candidate uses complete 131072-byte dis.dat transfers and SHA-256 content identities under karma-saves-v2/CUSA00223-01.07. Every load stops the active hold and clears cached baseline, shard and purchase state before reading. Completed loads are bound only when the observed version-123 header matches live score, side, rank and shards. Transfer coverage and generation checks refuse partial or superseded loads. Complete saves receive immutable version records pairing the original snapshot, shard ledger and purchase journal. Old manual saves therefore retain their own earlier records instead of inheriting a newer autosave journal. Conflicting or interrupted records fail closed. The original profile-wide store is preserved but never auto-adopted; unrecognized saves in a legacy profile are blocked. Seven automated checks cover storage isolation, restarts, assisted autosaves, old manual versions, corruption, conflicts, partial and stale transfers. Real gameplay validation is pending. Before a first-ever boost, changes to the loaded karma, shards or captured upgrade flags require saving and reloading first; this prevents attaching a later baseline to older save bytes. No save-file fields are written by the mod. Save header offsets are read guards observed against both copied saves and their runtime states; they are not treated as cross-version constants. The normal installation remains unchanged.

### In-game validation completed October 23
Using fresh copied 13% Protector and 100% True Hero saves: the 100% save initially exposed no early-save Reset target; Infamous then Reset restored its own Good rank 5 / 4300 and 0 shards. Switching to the 13% save, boosting and Reset restored Good rank 1 / 330 and 11 shards. Reloading the 100% save retained its own baseline. First save-and-quit check exposed deferred metadata persistence while the pause menu stopped camera ticks; fixed by sealing matched completed saves at file close and adding a no-post-save-Poll regression. Repeat produced a verified boosted manual save with 74 shards, 63 unused bonus shards, one assisted purchase and original score 330. After a real restart, Reset restored Good rank 1, 11 shards and Smoke Shot rank 1. Reloading the 100% save after that restart confirmed rank 5, 0 shards, original 4300 and zero assisted purchases. Normal protected files were verified unchanged by launcher exits. Legacy-ledger migration remains deliberately blocked; the normal launcher and previously published/shareable binaries are not updated. Map-menu distortion and the independently reproduced dark road patches remain unresolved.


## Named reset profiles
Added optional labels stored per exact save identity in karma-profile-names; labels are never used to select/reset values. Rename preserves baseline bytes. Capture current progression explicitly accepts the currently loaded values, including any existing mod effects, and creates an empty bonus/purchase ledger only after verified backups. Legacy-unknown saves remain blocked until this explicit capture or recovery. Archive and replace preserves the old record, exact save and label in karma-profile-archive; normal automatic sealing remains immutable. Capturing requires the saved point to match current runtime guards; save and reload first when progression has changed. Recovery lists only earlier save files with verified content-matched tracking and restores them to unused manual slots with a Recovered label, preserving all existing slots. It does not silently transplant another save's baseline or adopt legacy data. Backups now include saves, original ledger, v2 tracking, names and archives. Worker requests are save-generation checked. Names trim surrounding spaces in the UI and reject empty/control/overlong labels. Eight automated checks pass. In-game: renamed 100% profile without changing values; archived old 100% legitimate name and record then created 100% new reset point; captured Early story baseline for an unmatched 13% save at score330/rank1/11 shards; boosted then Reset restored it; names survived restart; recovery produced new User3/User4 slots retaining correct Level1/11 and profile name. A startup NVIDIA-driver access violation occurred after exclusive acquisition during cold shader compilation; causality with Alt-Tab is unproven; Borderless retry succeeded. Native rendering/camera code was not changed by named-profile work. Normal install update pending.


### Map/UI separation comparison (2026-10-23)

Controlled copied-profile results: original 1920x1080 and native 2560x1440
16:9 both show correct logos and smooth, undistorted maps. Native 3440x1440
with a widened logical UI has stretched map edges; centering the HUD removes
the observed menu slowdown but does not fix the distortion.

Experimental `iss_native_ui_16_9=1` retains a 1920x1080 logical UI while two
verified camera LEA instructions read the native render-dimension immediates.
The user reports that startup logos are now visible and maps look better with
no slowdown, but gameplay power indicators and minimap appear enlarged. This
is not ready for installation: HUD sizing/proportions still require correction.
All eight automated tests pass, including actual US ELF signature preflight.

A temporary picture freeze with continued audio recovered during this session.
A bounded detailed GPU frame capture was triggered shortly before the report;
its logging overhead is a possible confounder, not a confirmed explanation.
The process remained responsive afterward and render counters kept advancing.
Do not classify this as resolved stability or a confirmed deadlock recurrence.

### HUD size diagnostic, not a completed per-layer fix

User confirmed the minimap/power HUD appears larger in both dimensions. Local
inspection reproduced the enlarged HUD. Diagnostic mode `iss_native_ui_16_9=2`
keeps the menu canvas exactly 16:9 but expands its logical extent to at least
the wide display's 1080-line logical width (multiples of 16x9). For 3440x1440
this is 2592x1458; for 5120x1440 it is 3840x2160. Scene and output dimensions,
world-camera aspect, camera controls and Karma hooks are unchanged.

This is a shared-layout sizing experiment. It does NOT yet implement independent
HUD/menu render layers or prove that all UI elements use a 1080p design scale.
Visual checks must confirm HUD proportions, map/logo behavior and text legibility.
Do not ship or describe this diagnostic as the completed requested HUD fix.

Also fixed source settings persistence: `iss_native_ui_16_9` must survive overlay
settings saves. Earlier installed/package binaries predate that fix. GitHub work
was stopped at the user's request; no PR or release was created. Local HUD changes
are not uploaded to GitHub.

HUD diagnostic mode 2 FAILED the user visual check: minimap/power artwork was
slightly smaller, but HUD elements moved toward the center and pause-map edge
distortion returned. Screenshot: codex-clipboard-8bf7e7da-1d83-4da7-aad2-9d593b91ed02.png.
Do not install/package this mode. The default runtime safe-area calculation also
centers a 1920-wide region inside the expanded diagnostic canvas, explaining the
inward anchors, but changing that alone cannot fix the map regression. The last
map-correct baseline remains mode 1 (1920x1080 menu canvas) with enlarged gameplay
HUD. The requested independent HUD transform remains unimplemented; do not label
this shared-canvas diagnostic as a per-layer fix.

HUD-layout observer first launch crashed in Borderless before a capture request:
read of address 0x50 at nvoglv64.dll+0xdac501, thread 43536, process exit 3.
There were no HUD_LAYOUT/HUD_NODE records. This identifies the fault location,
not its cause; observer involvement is not excluded. Normal protected-file hash
checks passed. Preserve work/hud-layout-diagnostic-crash.log and compare the same
binary with observer installation disabled before attempting further captures.

### Gameplay HUD corner candidate (local only)

The bounded layout observer reached gameplay after appending the experimental
settings field to preserve established renderer field offsets. This is not proof
that field layout caused the earlier driver crashes. Logged three candidate roots:
- karma emblem: vtable +0xd74668, owner vtable +0xcdb7d0, 140x200, flags 5;
- power meter: same types, 424x280, flags 9;
- minimap: vtable +0xd795d0, no owner, 330x430, flags 10.

The candidate intercepts the completed-layout tail at US +0x565193 (17 verified
bytes), guarding the layout entry/scale instructions and relocated vtable methods
before installing. It filters exact types, ownership, dimensions and flags, then
corrects horizontal excess stretch around +/-960 logical corner anchors. It keeps
the original 1080-line vertical scale and leaves the shared menu canvas at
1920x1080. Unmatched widgets and non-affine/nonfinite transforms pass unchanged.
No widget asset, progression or input data is edited. The game uses affine 3x4
matrices with unused W=0, which was captured and added to fixtures alongside W=1.

Nine tests pass, including executable entry/exit bridge tests, target filtering,
16:9 identity and 21:9/32:9 pixel proportions and corner-margin calculations.
The initial transform candidate rejected the observed W=0 convention and made no
HUD changes. The corrected local run logs matches for all three intended groups;
the user confirms the HUD looks good, smaller, and texture quality remains high.
Expanded-map/preview visual and navigation regression checks are pending.

Current local run uses --no-driver-cache (BB_PIPELINE_CACHE=0), keeping shader
translation caches and saves intact. Repeated startup failures were reads of 0x50
at nvoglv64.dll+0xdac501, also reproduced with the observer disabled. One successful
cache-disabled startup is not proof of a cache cause or a general crash fix.
Normal installation and public ZIP still contain the previous map/menu version;
this HUD candidate has not been installed, packaged or uploaded. GitHub work is
stopped by user instruction.
