/* SPDX-License-Identifier: GPL-2.0-or-later */
/* inFAMOUS Second Son (CUSA00223 01.07 eboot): Havok hk_2013.2.0-r1 hkCriticalSection spin counts.
 * hkCriticalSection(int spinCount) (0x63ff90; image offsets) creates a recursive pthread mutex
 * (+0) and stores the count at +8 (0x63ffac: mov [rbx+8],esi). The ~84 inlined enter() loops
 * try-lock up to spinCount times, with 10 PAUSEs between attempts, before pthread_mutex_lock:
 * about 1,500 cycles an attempt on Raptor Lake, so 4000 spins for about 1.3 ms before blocking.
 * The 11 constructor calls with a constant count pass it as `mov esi,imm32; call 0x63ff90`;
 * the others pass 0 (xor esi,esi) or a parameter and are left alone. Each site below is the
 * mov's address; the patch rewrites only its imm32 (site + 1). */
#ifndef TLG_HAVOK_SPIN_H
#define TLG_HAVOK_SPIN_H
#include <stdint.h>
#include <string.h>

#define TLG_HAVOK_SPIN_SITES 11u
#define TLG_HAVOK_SPIN_SITE_LENGTH 10u /* BE imm32, E8 rel32 */
#define TLG_HAVOK_SPIN_CTOR 0x63ff90u

typedef struct {
    uint32_t site;  /* the mov esi,imm32 */
    uint32_t count; /* the original imm32 */
} TlgHavokSpinSite;

static const TlgHavokSpinSite tlg_havok_spin_sites[TLG_HAVOK_SPIN_SITES] = {
    {0x6400ebu, 4000u}, {0x6424b7u, 4000u}, {0x6425d5u, 4000u}, {0x642ef2u, 1000u},
    {0x657277u, 4000u}, {0x657596u, 4000u}, {0x66dfafu, 1000u}, {0x92c492u, 1000u},
    {0x92f767u, 1000u}, {0x92f833u, 2000u}, {0x930098u, 2000u}};

/* The site's bytes: mov esi,count; call hkCriticalSection::hkCriticalSection. */
static inline void tlg_havok_spin_signature(const TlgHavokSpinSite *s, uint32_t count,
                                            unsigned char out[TLG_HAVOK_SPIN_SITE_LENGTH]) {
    const uint32_t rel = TLG_HAVOK_SPIN_CTOR - (s->site + TLG_HAVOK_SPIN_SITE_LENGTH);
    out[0] = 0xbe;
    memcpy(out + 1, &count, 4);
    out[5] = 0xe8;
    memcpy(out + 6, &rel, 4);
}

/* Every site unchanged (this eboot build). */
static inline int tlg_havok_spin_signature_match(const unsigned char *image, uint64_t size) {
    unsigned char expected[TLG_HAVOK_SPIN_SITE_LENGTH];
    for (unsigned i = 0; i < TLG_HAVOK_SPIN_SITES; ++i) {
        const TlgHavokSpinSite *s = &tlg_havok_spin_sites[i];
        if (size < TLG_HAVOK_SPIN_SITE_LENGTH || s->site > size - TLG_HAVOK_SPIN_SITE_LENGTH) return 0;
        tlg_havok_spin_signature(s, s->count, expected);
        if (memcmp(image + s->site, expected, TLG_HAVOK_SPIN_SITE_LENGTH)) return 0;
    }
    return 1;
}
#endif
