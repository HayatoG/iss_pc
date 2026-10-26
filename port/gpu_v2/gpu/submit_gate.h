// SPDX-License-Identifier: GPL-2.0-or-later
// Experimental guest <-> command processor lockstep toggles (renderer v2).
//
// sceGnmSubmitDone sets a submission lock while the command processor is still busy; the guest's
// next graphics submission and DingDongs then wait until it has fully drained the frame (GpuIdle).
//
// ISS_FRAME_END_MARK=1 (default 0): the command processor runs its end of frame work (rasterizer
// OnSubmit and Flush) once per guest frame end, as soon as everything submitted before that frame
// end has been processed (a G4 ledger mark taken in Liverpool::SubmitDone), instead of once when
// it runs out of work. Off: exactly the old behaviour.
//
// ISS_SUBMIT_GATE=idle|gfx|off (default idle):
//   idle: the old lockstep.
//   gfx:  asynchronous compute DingDongs no longer wait for the lock; graphics submissions and
//         SubmitDone still do.
//   off:  nothing waits for the lock and sceGnmAreSubmitsAllowed always returns 1; frame end
//         marks are forced on. The game's own frame end label wait bounds the lag (and
//         ISS_TUNING max_gpu_lag_frames, optionally).
// Forced to idle with BB_COPY_GPU_BUFFERS (SubmitDone resets the copy buffer offsets) and with
// ISS_CP_SPLIT=1.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "core/emulator_settings.h"

namespace iss::submit_gate {

enum class Mode { Idle, Gfx, Off };

struct Config {
    Mode mode{Mode::Idle};
    bool frame_end_marks{};
    const char* forced{}; ///< why the requested mode was replaced by idle (nullptr: it was not)
};

inline const Config& Get() {
    static const Config config = [] {
        Config c{};
        if (const char* p = std::getenv("ISS_SUBMIT_GATE"); p && *p) {
            if (!std::strcmp(p, "gfx")) {
                c.mode = Mode::Gfx;
            } else if (!std::strcmp(p, "off")) {
                c.mode = Mode::Off;
            } else if (std::strcmp(p, "idle") != 0) {
                std::fprintf(stderr, "PORT_TUNING submit_gate: unknown value '%s', using idle\n",
                             p);
            }
        }
        const char* marks = std::getenv("ISS_FRAME_END_MARK");
        c.frame_end_marks = marks && marks[0] == '1';
        if (c.mode != Mode::Idle) {
            const char* split = std::getenv("ISS_CP_SPLIT");
            if (EmulatorSettings.IsCopyGpuBuffers()) {
                c.mode = Mode::Idle;
                c.forced = "BB_COPY_GPU_BUFFERS";
            } else if (split && split[0] == '1') {
                c.mode = Mode::Idle;
                c.forced = "ISS_CP_SPLIT=1";
            }
        }
        if (c.mode == Mode::Off) {
            c.frame_end_marks = true;
        }
        return c;
    }();
    return config;
}

inline const char* Name(Mode mode) {
    return mode == Mode::Off ? "off" : mode == Mode::Gfx ? "gfx" : "idle";
}

inline void Log() {
    const auto& c = Get();
    if (c.forced) {
        std::fprintf(stderr, "PORT_TUNING submit_gate=%s frame_end_mark=%d (forced idle: %s)\n",
                     Name(c.mode), c.frame_end_marks ? 1 : 0, c.forced);
    } else {
        std::fprintf(stderr, "PORT_TUNING submit_gate=%s frame_end_mark=%d\n", Name(c.mode),
                     c.frame_end_marks ? 1 : 0);
    }
}

/// DingDong (asynchronous compute) waits for the submission lock.
inline bool AscWaits() {
    return Get().mode == Mode::Idle;
}

/// Graphics submissions and SubmitDone wait for the submission lock (and SubmitDone sets it).
inline bool GfxWaits() {
    return Get().mode != Mode::Off;
}

inline bool FrameEndMarks() {
    return Get().frame_end_marks;
}

} // namespace iss::submit_gate
