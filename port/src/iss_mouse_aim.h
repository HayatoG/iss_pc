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
int runtime_mouse_aim_installed(void);
int runtime_mouse_aim_motion(float dx,float dy,float sensitivity,int invert_y,int active);
void __attribute__((ms_abi)) runtime_mouse_aim_camera(float *angles,const void *data,const void *context);
#ifdef __cplusplus
}
#endif
#endif
