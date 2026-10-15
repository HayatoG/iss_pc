# inFAMOUS Second Son on Windows

The port builds and runs natively on 64-bit Windows 10 (1903+) / 11 with the same sources as
on Linux. Players: see `README-player.md` (it ships in the package, pt-BR and en).

## Build

Install [MSYS2](https://www.msys2.org) (`winget install MSYS2.MSYS2`), open the
**MSYS2 CLANG64** shell and install the toolchain and libraries (the counterpart of
`shell.nix`):

```bash
pacman -S --needed git patch \
  mingw-w64-clang-x86_64-{toolchain,cmake,ninja,pkgconf,python} \
  mingw-w64-clang-x86_64-{vulkan-headers,vulkan-loader,vulkan-memory-allocator,glslang} \
  mingw-w64-clang-x86_64-{sdl3,fmt,boost,robin-map,xxhash,ffmpeg,zydis} \
  mingw-w64-clang-x86_64-{spirv-tools,spirv-headers,spirv-cross}
```

magic_enum, miniz and xbyak (not packaged by MSYS2) are submodules under `gpu/third_party/`.
Then, in the repository:

```bash
git submodule update --init --recursive
bash build.sh                      # out/tlg-probe.exe, out/tlg-gpu-capabilities.exe, out/tlg-play.exe
TLG_GAME_DIR=F:/path/to/CUSA00223 python run.py
python launcher/infamous_launcher_qt.py   # the Qt launcher (needs PySide6); infamous_launcher.py is the Tk one
bash packaging/windows/package.sh  # dist/infamous-windows-x86_64.zip: inFAMOUS Second Son.exe + DLLs
```

`run.py` is the Windows `run.sh` (same variables; works from a plain Windows Python too).
The package freezes the launchers with PyInstaller (a private venv in `out/pyenv`, from a
python.org Python) into `inFAMOUS Second Son.exe` (Qt) and `inFAMOUS Second Son (classic).exe` (Tk).
Both also run the preparation scripts (`--script`) and the game without the window (`--play`):
players need no Python.
`Play inFAMOUS Second Son.exe` (`packaging/windows/play.c`) is a small native program that runs
the launcher's `--play` from its folder and waits for it, for players who want to start the
game directly with the saved settings.
`scripts/patches.py` applies the built-in patches only to the game build they were checked against
(by `eboot.bin` sha256; `TLG_FORCE_PATCHES=1` overrides); other builds run unpatched.
From a source tree it adds `C:\msys64\clang64\bin` (or `$MSYS2_ROOT`) to `PATH` for the DLLs.

The shared core also carries optional upscaler code from other ports (a DLSS bridge built with
`packaging/windows/build_dlss.sh`, FSR 4 helpers in `tools/`). **inFAMOUS does not use them**: the
game's menu only has FSR 1 and sharpening (RCAS), and the launcher runs with the native image
(`ISS_UPSCALER=off`).

The launcher texts are in Portuguese (Brazil) and English (`launcher/qt/i18n/strings.json`, plus the
Tk launcher's own table). The icon (`launcher/infamous.ico`/`.png`) is original art drawn by
`packaging/windows/make_icon.py` and built into the executables and `tlg-probe.exe` (the game
window).

## What is different from Linux

| Linux | Windows |
|---|---|
| `mov rax, fs:[0]` rewritten to `gs:[0]`, GS base set with `arch_prctl` | GS is the TEB and cannot move. The loader rewrites the displacement to `gs:[0x1480 + 8*slot]` (the TEB's `TlsSlots[slot]`, same 9-byte instruction) and each guest thread keeps its TCB in that TLS slot. The slot is claimed first thing in `main` (Vulkan drivers take many). |
| One memfd for direct + flexible memory, `mmap(MAP_FIXED)` views | A pagefile-backed section; the PS4 range 64 GiB–1 TiB is reserved as one placeholder and views replace placeholders (`VirtualAlloc2`/`MapViewOfFile3`, as in shadPS4). Partial unmaps split views and restore page protections (GPU tracking). |
| `-no-pie`, heap below 1 TiB | `--disable-high-entropy-va`: heaps and thread stacks stay low and out of the PS4 range. |
| SIGSEGV handler, `sigsetjmp` recovery | Vectored exception handler; `tlg_setjmp`/`tlg_longjmp` (`src/compat_win.c`) restore all Win64 callee-saved registers without SEH unwinding (guest frames have no unwind data; clang's `__builtin_longjmp` restores a wrong frame pointer on Win64). |
| Guest thread stacks from `runtime_low_map` | Windows allocates them, fully committed (`pthread_attr_setstacksize`): guest code has no stack probes and would skip the guard page. The main thread stack is 16 MiB committed. |
| glibc `nanosleep`, monotonic condition variables | High-resolution waitable timers (`compat_sleep_ns`; winpthreads' sleep rounds to 15.6 ms); semaphore deadlines use `CLOCK_REALTIME` (winpthreads rejects monotonic condition variables). |
| `libtlggpu.so` | `libtlggpu.a`, linked into `tlg-probe.exe` by CMake (`gpu/CMakeLists.txt`), with a manifest for UTF-8 paths and long paths. |
| `execlp bash run.sh` (in-game restart) | `run.py` puts its command line in `TLG_RESTART_COMMAND`; the new launch waits for the old process (`--after PID`). The launcher keeps the game in a job object. |
| Mod overlay with symlinks | Symlinks when allowed (Developer Mode), else junctions for folders and hard links for files. |

Diagnostics added on the way: `TLG_FRAME_DUMP_TRIGGER=<file>` writes the guest display buffer
and the presented image of the next frame to `TLG_DUMP_DIR` (raw 32-bit pixels), which also
works for menus and movies.

Not ported: the watchdog/`--timeout` thread dumps (Linux signals), userfaultfd tracking
(`TLG_UFFD`), MangoHud, PGO (GCC flags) and
the `tests/` programs (several use Linux-only APIs).

## Launchers in the package (Qt default, Tk "classic")

`package.sh` now freezes two launchers into the same folder with `packaging/launcher.spec`:
`inFAMOUS Second Son.exe` is the Qt Quick launcher (PySide6-Essentials, pinned by `PORT_QT_PACKAGE`
in `packaging/port.env`) and `inFAMOUS Second Son (classic).exe` is the Tk one, kept as a fallback.
Both are windowed and share their DLLs. The Qt data (`qml`, `fonts`, `i18n`, icon) is added by the spec
under `qt\`; unused Qt modules, Controls styles, translations and platform plugins are trimmed
(see `docs/QT_PACKAGING.md`). `"inFAMOUS Second Son.exe" --selftest` opens the QML offscreen, renders one
frame and writes `selftest ok` to the file in `ISS_SELFTEST_OUT` (a windowed exe has no console); the
CI runs it after Package. Estimated size: the zip grows by 55-70 MB.

## Status on Windows

Players have tested the Windows package (the occlusion fix for headlights and light glare was confirmed on Windows).
Windows-specific points, all described in the top-level `README.md`:

- Saves and settings live in `user/` next to the game by default; the launcher can move them to a fixed folder
  (suggestion `Documents\inFAMOUS-PC`), remembered in `%APPDATA%\inFAMOUS-PC\launcher.json`. `ISS_USER_DIR` overrides it.
- Logs go to `out\logs\run-*.log`; the Support screen builds a report without personal paths.
- Red-zone protection (crash at `0x1cfaaf`, #29/#41) is on by default on Windows (`ISS_RED_ZONE_REWRITE=0` turns it off).
- AMD's Windows driver: tiny BC textures are decoded on the CPU (`ISS_BC_TINY`). RX 580 / Polaris still crashes in
  `amdvlk64.dll+0xebbbb` (#31, #49, #59, #65, #67, #81): cause under investigation; try `ISS_BP09=1` and the latest driver.
- The executables are not digitally signed: Windows shows "Windows protected your PC" (More info, Run anyway).
