// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: TLG_ASYNC_FREE=1 — objects whose destruction is costly (a draw-preparation submission:
// thousands of prepared draws, several MB) are released on a background thread instead of the
// GPU command thread. In the city that thread spent ~18% of its time in the heap freeing them
// (TLG_SAMPLE, RX 9060 XT).
#pragma once

#include <memory>

namespace TlgReclaim {
/// True when TLG_ASYNC_FREE=1.
bool Enabled();
/// Drops `object` on the reclaim thread (or here when disabled).
void Later(std::shared_ptr<const void> object);
} // namespace TlgReclaim
