/* Direct camera-angle input, inspired by axylpup/shadPS4 second-son-v1.1
 * src/core/mouse_aim.cpp (GPL-2.0-or-later). US offsets independently checked.
 * No changes to controller input or the existing angular-speed hooks. */
#ifndef ISS_MOUSE_AIM_H
#define ISS_MOUSE_AIM_H
#include <stdint.h>
#include <string.h>
#include <math.h>
#define ISS_MOUSE_SITE 0x14ebceu
#define ISS_MOUSE_STOLEN 18u
static const unsigned char iss_mouse_expected[]={
    0xc5,0xf8,0x11,0x84,0x24,0,1,0,0,
    0xc4,0xc1,0x78,0x11,0x85,0x30,3,0,0,
    0xc4,0xc1,0x7a,0x7f,0x8d,0x40,3,0,0};
static const unsigned char iss_mouse_data[]={0x4c,0x8b,0xa4,0x24,0xe8,3,0,0};
static const unsigned char iss_mouse_context[]={0x4c,0x89,0x7c,0x24,0x50};
static const unsigned char iss_mouse_limits[]={0xc4,0xc1,0x78,0x10,0x8f,0xc0,1,0,0,
    0xc4,0xc1,0x78,0x10,0x84,0x24,0xc0,1,0,0};
static inline int iss_mouse_signature(const unsigned char *p,uint64_t n){
    static const unsigned char follow[]={0x48,0x8d,0x05,0xcb,0xd8,0xcc,0};
    return n>0xe16c40u && !memcmp(p+0x14ebc5,iss_mouse_expected,sizeof(iss_mouse_expected)) &&
        !memcmp(p+0x14936e,follow,sizeof(follow)) &&
        !memcmp(p+0x14dfb2,iss_mouse_data,sizeof(iss_mouse_data)) &&
        !memcmp(p+0x14ea75,iss_mouse_context,sizeof(iss_mouse_context)) &&
        !memcmp(p+0x14e38f,iss_mouse_limits,sizeof(iss_mouse_limits));
}
/* inFAMOUS First Light CUSA00575 01.04 (2026-11-12): the same camera update, compiled apart
 * (player camera 0x171c00). The angle store is at 0x17228a with the camera in R15; the two pitch-limit
 * objects are RBX (limits at +0x1c0) and R12 (limits at +0x1d0), as the limits helper 0x173620 reads
 * them; the camera-follow switch is the byte at 0xf2b380 (function 0x16cf60). */
#define ISS_MOUSE_FL_SITE 0x17228au
#define ISS_MOUSE_FL_FOLLOW 0xf2b380u
static inline int iss_mouse_signature_first_light(const unsigned char *p,uint64_t n){
    static const unsigned char site[]={0xc5,0xf8,0x11,0x84,0x24,0x80,1,0,0,
        0xc4,0xc1,0x78,0x11,0x87,0x30,3,0,0,
        0xc5,0xf8,0x10,0x8c,0x24,0x70,1,0,0};
    static const unsigned char follow[]={0x48,0x8d,0x05,0x0b,0xe4,0xdb,0};
    static const unsigned char context[]={0x4d,0x89,0xec};
    static const unsigned char data[]={0x48,0x8b,0x9c,0x24,0x68,3,0,0};
    static const unsigned char limits[]={0xc5,0xf8,0x10,0x86,0xc0,1,0,0,0xc4,0xc1,0x78,0x10,0x8e,0xd0,1,0,0};
    return n>ISS_MOUSE_FL_FOLLOW && !memcmp(p+0x172281,site,sizeof(site)) &&
        !memcmp(p+0x16cf6e,follow,sizeof(follow)) &&
        !memcmp(p+0x172003,context,sizeof(context)) &&
        !memcmp(p+0x171e60,data,sizeof(data)) &&
        !memcmp(p+0x173689,limits,sizeof(limits));
}
static inline float iss_mouse_bound(float v,float lo,float hi){return fminf(hi,fmaxf(lo,v));}
/* Invalid limits reject the delta. A game-controlled tilt outside its normal
 * interval is preserved rather than snapped back. */
static inline int iss_mouse_apply(float *angles,float yaw,float pitch,float dl,float du,float cl,float cu){
    if(!isfinite(angles[0])||!isfinite(angles[1])||!isfinite(yaw)||!isfinite(pitch)||
       !isfinite(dl)||!isfinite(du)||!isfinite(cl)||!isfinite(cu)||
       dl>du || cl>cu)return 0;
    float lo=fmaxf(dl,cl),hi=fminf(du,cu);
    /* Live US 1.07 context uses [-10,10] as a permissive range. Validate
     * the effective intersection, not each input range independently. */
    if(lo>hi || lo < -3.2f || hi > 3.2f)return 0;
    angles[0]+=yaw;
    angles[1]=iss_mouse_bound(angles[1]+pitch,fminf(lo,angles[1]),fmaxf(hi,angles[1]));
    return 1;
}
#ifdef __cplusplus
extern "C" {
#endif
void runtime_mouse_aim_attach(uintptr_t base);
/* Another build of the game: where its camera-follow byte is and at which offsets the two
 * pitch-limit pairs sit in the data and context objects the hook passes. */
void runtime_mouse_aim_attach_at(uintptr_t follow,unsigned data_limits,unsigned context_limits);
int runtime_mouse_aim_installed(void);
int runtime_mouse_aim_camera_live(void);
int runtime_mouse_aim_motion(float dx,float dy,float sensitivity,int invert_y,int active);
void __attribute__((ms_abi)) runtime_mouse_aim_camera(float *angles,const void *data,const void *context);
#ifdef __cplusplus
}
#endif
#endif
