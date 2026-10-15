// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: region trace (TLG_REGION_TRACE=1). inFAMOUS streams the city in `.xpps` packages; which
// package the game opens is the best hint of where the player is going. This writes one
// tab-separated line per event to `region_trace.tsv` in the GPU user folder, so an offline
// pass can tell which pipelines each region needs:
//     O <ms> <package>            the game opened an .xpps package
//     P <ms> <g|c> <hash> <r|m>   the first request of a graphics or compute pipeline; r = it was
//                                 ready (cache or an earlier build), m = missing: the draw waits or goes
// <ms> is a monotonic clock shared by both kinds of line. The first step of the per-region
// pre-warm (DESIGN_PREWARM_POR_MAPA.md): measure before building anything on top.
#pragma once
#include <cstdint>

namespace TlgRegion {

/// True when TLG_REGION_TRACE=1; every call below is a no-op otherwise.
bool Enabled();

/// The game opened `guest_path`; only `.xpps` packages are recorded.
void NoteOpen(const char* guest_path);

/// The game asked for a pipeline (`kind` 'g' graphics, 'c' compute); `ready`: it already exists.
/// Logged once per hash, with the state of that first request.
void NotePipeline(char kind, std::uint64_t hash, bool ready);

} // namespace TlgRegion
