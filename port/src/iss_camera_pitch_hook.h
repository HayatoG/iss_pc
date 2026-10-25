/* CUSA00223 01.07: optional prevention of boost loss at the vertical camera limit.
 * No input scaling or angle writes. The game still applies its acceleration and clamps.
 */
#ifndef ISS_CAMERA_PITCH_HOOK_H
#define ISS_CAMERA_PITCH_HOOK_H
#include <stdint.h>
#include <string.h>

#define ISS_PITCH_SITE 0x14ae69u
#define ISS_PITCH_LENGTH 16u
#define ISS_PITCH_MAX_STUB 512u
static const unsigned char iss_pitch_original[16] = {
    0xc5,0xfa,0x11,0x85,0xf0,0xfe,0xff,0xff,
    0x41,0x83,0xbd,0x70,0x04,0x00,0x00,0x02
};
typedef struct {
    uint64_t calls, changed;
    float last_x, last_y, last_pitch, last_zoom, last_cap, last_result;
} IssPitchStats;
static IssPitchStats iss_pitch_stats;
static inline float iss_pitch_f(const void *p, intptr_t offset) {
    return *(const float *)((const unsigned char *)p + offset);
}
static inline float iss_pitch_abs(float x) {
    union { float f; uint32_t u; } v = {x};
    v.u &= 0x7fffffffu;
    return v.f;
}
static inline int iss_pitch_finite(float x) {
    union { float f; uint32_t u; } v = {x};
    return (v.u & 0x7f800000u) != 0x7f800000u;
}
/* The bridge uses the Windows x64 ABI on both hosts. Disable AVX so this helper
 * cannot zero upper YMM lanes; the bridge preserves all XMM lanes and flags.
 * No calls, allocation, logging or changes to the game data occur here.
 */
static __attribute__((ms_abi,noinline,target("no-avx,no-avx2")))
float iss_pitch_cap(float cap, const void *state, const void *config, const void *frame) {
    const float x = iss_pitch_f(frame,-0x1b0), y = iss_pitch_f(frame,-0x190);
    const float pitch = iss_pitch_f(state,0x334), zoom = iss_pitch_f(state,0x360);
    const float pan_velocity = iss_pitch_f(frame,-0x210);
    const float tilt_velocity = iss_pitch_f(frame,-0x260);
    const float previous = iss_pitch_f(state,0x4d4);
    const float lower = iss_pitch_f(config,0x1c0), upper = iss_pitch_f(config,0x1c4);
    const float boost_max = iss_pitch_f(config,0x334);
    ++iss_pitch_stats.calls;
    iss_pitch_stats.last_x=x; iss_pitch_stats.last_y=y;
    iss_pitch_stats.last_pitch=pitch; iss_pitch_stats.last_zoom=zoom;
    iss_pitch_stats.last_cap=cap;
    float result=cap;
    if (iss_pitch_finite(cap) && iss_pitch_finite(previous) &&
        iss_pitch_finite(x) && iss_pitch_finite(y) && iss_pitch_finite(pitch) &&
        iss_pitch_finite(zoom) && iss_pitch_finite(pan_velocity) && iss_pitch_finite(tilt_velocity) &&
        iss_pitch_finite(lower) && iss_pitch_finite(upper) && iss_pitch_finite(boost_max) &&
        zoom>=0.0f && zoom<=0.001f &&
        *(const uint32_t *)((const unsigned char *)state+0x470)==2 &&
        iss_pitch_abs(x)>=0.65f && iss_pitch_abs(y)>=0.30f &&
        x*pan_velocity < -0.05f && iss_pitch_abs(tilt_velocity)<=0.02f &&
        lower>=-1.7f && upper<=1.7f && lower<upper &&
        boost_max>0.0f && boost_max<=4.0f && cap>=0.0f &&
        previous>cap && previous<=boost_max &&
        ((pitch<=lower+0.004f && y>0.0f) || (pitch>=upper-0.004f && y<0.0f))) {
        result=previous;
        ++iss_pitch_stats.changed;
    }
    iss_pitch_stats.last_result=result;
    return result;
}
static inline int iss_pitch_signature(const unsigned char *image, uint64_t size) {
    static const unsigned char projection[] = {0xe8,0x45,0xe5,0x4c,0x00};
    static const unsigned char product[] = {0xc5,0xfa,0x59,0x9d,0xf0,0xfe,0xff,0xff};
    return ISS_PITCH_SITE<=size && ISS_PITCH_LENGTH<=size-ISS_PITCH_SITE &&
        !memcmp(image+ISS_PITCH_SITE,iss_pitch_original,16) &&
        0x14aff9u<=size && !memcmp(image+0x14aff1,product,sizeof(product)) &&
        !memcmp(image+0x14ae46,projection,sizeof(projection));
}
/* Entry replaces a jump, so RSP retains its verified 16-byte alignment. Save
 * volatile GPRs, all XMM registers and flags; reserve Windows call shadow space.
 * Only the returned low float replaces XMM0. Replay both original instructions,
 * including CMP (the next conditional jump must see its flags), then tail-jump.
 */
static inline unsigned iss_pitch_build_stub(unsigned char *out, uintptr_t helper, uintptr_t resume) {
    unsigned n=0;
    static const unsigned char save[] = {0x9c,0x50,0x51,0x52,0x56,0x57,0x41,0x50,0x41,0x51,0x41,0x52,0x41,0x53,
                                       0x48,0x81,0xec,0x20,0x01,0x00,0x00};
    memcpy(out+n,save,sizeof(save)); n+=sizeof(save);
    for (unsigned i=0;i<16;++i) {
        out[n++]=0xf3; if(i>=8) out[n++]=0x44;
        out[n++]=0x0f;out[n++]=0x7f;out[n++]=(unsigned char)(0x84+8*(i%8));out[n++]=0x24;
        uint32_t disp=0x20+i*16;memcpy(out+n,&disp,4);n+=4;
    }
    static const unsigned char args[]={0x4c,0x89,0xea,0x49,0x89,0xd8,0x49,0x89,0xe9,0x48,0xb8};
    memcpy(out+n,args,sizeof(args));n+=sizeof(args);memcpy(out+n,&helper,8);n+=8;
    static const unsigned char call[]={0xff,0xd0,0xf3,0x0f,0x11,0x44,0x24,0x20};
    memcpy(out+n,call,sizeof(call));n+=sizeof(call);
    for (unsigned i=0;i<16;++i) {
        out[n++]=0xf3;if(i>=8)out[n++]=0x44;
        out[n++]=0x0f;out[n++]=0x6f;out[n++]=(unsigned char)(0x84+8*(i%8));out[n++]=0x24;
        uint32_t disp=0x20+i*16;memcpy(out+n,&disp,4);n+=4;
    }
    static const unsigned char restore[]={0x48,0x81,0xc4,0x20,0x01,0x00,0x00,
        0x41,0x5b,0x41,0x5a,0x41,0x59,0x41,0x58,0x5f,0x5e,0x5a,0x59,0x58,0x9d};
    memcpy(out+n,restore,sizeof(restore));n+=sizeof(restore);
    memcpy(out+n,iss_pitch_original,16);n+=16;
    static const unsigned char jump[]={0xff,0x25,0,0,0,0};
    memcpy(out+n,jump,6);n+=6;memcpy(out+n,&resume,8);n+=8;
    return n;
}
static inline void iss_pitch_build_jump(unsigned char *out, uintptr_t stub) {
    static const unsigned char jump[]={0xff,0x25,0,0,0,0};
    memcpy(out,jump,6);memcpy(out+6,&stub,8);out[14]=out[15]=0x90;
}
#endif
