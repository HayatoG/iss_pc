// SPDX-License-Identifier: GPL-2.0-or-later
// Experimental guest <-> command processor lockstep toggles (renderer v2).
//
// ISS_FRAME_END_MARK=1 (default 0): the command processor runs its end of frame work (rasterizer
// OnSubmit and Flush) once per guest frame end, as soon as everything submitted before that frame
// end has been processed (a G4 ledger mark taken in Liverpool::SubmitDone), instead of once when
// it runs out of work. Off: exactly the old behaviour.
#pragma once

#include <cstdlib>

namespace iss::submit_gate {

inline bool FrameEndMarks() {
    static const bool enabled = [] {
        const char* marks = std::getenv("ISS_FRAME_END_MARK");
        return marks && marks[0] == '1';
    }();
    return enabled;
}

} // namespace iss::submit_gate
