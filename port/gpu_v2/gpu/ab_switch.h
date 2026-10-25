// SPDX-License-Identifier: GPL-2.0-or-later
// A/B tests inside one session, against the ±3 FPS that separate two sessions of the same build.
// - `iss_ab.env` next to the executable (absent for players): KEY=VALUE lines put into the
//   environment when the renderer starts, over the launcher's values (the A/B script writes and
//   removes it, so the player's launcher and settings stay untouched).
// - ISS_AB_PERIOD_SEC=N: every N seconds of presented frames the next mode of ISS_AB_MODES
//   (default 2): mode 0 is the feature off (Off()), 1 on, 2 and up variants the feature defines.
//   Each finished phase prints one AB_PHASE line (frame rate, CPU of the command processor and of
//   the draw pipe worker). ISS_AB_START=off starts with mode 0, else with mode 1.
#pragma once

#include <atomic>

namespace iss::ab {

enum Slot : unsigned { CommandProcessor, DrawWorker, NumSlots };

inline std::atomic<unsigned> g_mode{1};
inline std::atomic<unsigned> g_switches{0};

/// The phase's mode; 1 without ISS_AB_PERIOD_SEC.
inline unsigned Mode() noexcept {
    return g_mode.load(std::memory_order_relaxed);
}
/// The feature under test stays off in this phase.
inline bool Off() noexcept {
    return Mode() == 0;
}
/// Changes at every flip: state another thread kept while the feature was off must be redone.
inline unsigned Switches() noexcept {
    return g_switches.load(std::memory_order_acquire);
}

/// Before the renderer reads its settings.
/// ISS_STALL_DUMP_SEC=N (Windows): once frames stop for N seconds, the stack of every thread of
/// the process goes to the log (STALL_DUMP lines, `exe+0x...` offsets for llvm-symbolizer on the
/// unstripped build); again after frames resume and stop again.
void ApplyEnvFile();
/// Extra state printed with a stall dump (one function; the last one set wins).
void SetStallNote(void (*note)());
/// One more line after each AB_PHASE line (the feature's own counters for that phase).
void SetPhaseNote(void (*note)(unsigned phase, unsigned mode));
/// Frames presented so far (counted by OnFrame).
unsigned long long Frames();
/// Called once on the thread itself: its CPU time is reported per phase.
void NoteThread(Slot slot);
/// Once per presented frame (present thread).
void OnFrame();

} // namespace iss::ab
