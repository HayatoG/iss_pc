/* SPDX-License-Identifier: GPL-2.0-or-later */
/* inFAMOUS Second Son (CUSA00223 01.07 eboot): PAUSE in the engine's microsecond busy-waits.
 * Two identical functions (0x53b200, 0x53b2b0; image offsets) take a count of microseconds:
 * above 500 they tail-call sceKernelUsleep, otherwise they spin on RDTSC until
 * now + us * (tsc_frequency / 1e6) (signed compare). Their callers are polling loops: job workers
 * waiting 1 us for work (0x1f2e40, 0x243cec), waits on counters in 50 us steps (0x1690f0,
 * 0x1f2700), 1-3 us back-off after mutex unlocks. The 16-byte loop has no PAUSE, so on an SMT
 * CPU it starves the sibling hyperthread.
 * The 12-byte alignment nop before each loop becomes a 2-byte jmp over 8 dead bytes to the loop,
 * and a PAUSE right before it; the loop's jl goes back to the PAUSE. The first deadline check runs
 * without a pause, the next ones after one. PAUSE changes no register, flag or memory. */
#ifndef TLG_SPIN_HOOK_H
#define TLG_SPIN_HOOK_H
#include <stdint.h>
#include <string.h>

#define TLG_SPIN_SITE_LENGTH 28u
static const uint32_t tlg_spin_sites[2] = {0x53b284u, 0x53b334u}; /* the nop before each loop */
static const unsigned char tlg_spin_code[TLG_SPIN_SITE_LENGTH] = {
    0x66, 0x66, 0x66, 0x2e, 0x0f, 0x1f, 0x84, 0, 0, 0, 0, 0, /* data16 data16 cs nopw 0x0(%rax,%rax,1) */
    0x0f, 0x31,                                           /* 1: rdtsc */
    0x89, 0xc0,                                           /* mov %eax,%eax */
    0x48, 0xc1, 0xe2, 0x20,                               /* shl $32,%rdx */
    0x48, 0x09, 0xc2,                                     /* or %rax,%rdx */
    0x48, 0x39, 0xca,                                     /* cmp %rcx,%rdx */
    0x7c, 0xf0};                                          /* jl 1b */
static const unsigned char tlg_spin_paused[TLG_SPIN_SITE_LENGTH] = {
    0xeb, 0x0a,                                           /* jmp 1f */
    0x0f, 0x1f, 0x84, 0, 0, 0, 0, 0,                      /* nopl 0(%rax,%rax): never runs */
    0xf3, 0x90,                                           /* 2: pause */
    0x0f, 0x31,                                           /* 1: rdtsc */
    0x89, 0xc0,
    0x48, 0xc1, 0xe2, 0x20,
    0x48, 0x09, 0xc2,
    0x48, 0x39, 0xca,
    0x7c, 0xee};                                          /* jl 2b */

/* Both loops unchanged (this eboot build). */
static inline int tlg_spin_signature_match(const unsigned char *image, uint64_t size) {
    for (unsigned i = 0; i < 2; ++i)
        if (size < TLG_SPIN_SITE_LENGTH || tlg_spin_sites[i] > size - TLG_SPIN_SITE_LENGTH ||
            memcmp(image + tlg_spin_sites[i], tlg_spin_code, TLG_SPIN_SITE_LENGTH)) return 0;
    return 1;
}
#endif
