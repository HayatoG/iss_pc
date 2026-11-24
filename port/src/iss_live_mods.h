// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef ISS_LIVE_MODS_H
#define ISS_LIVE_MODS_H
#include <stdint.h>
#include <string.h>
typedef struct {
    uint64_t *camera_data;
    unsigned char *blur_flag;
    int camera_ready, pitch_ready, camera_enabled, blur_enabled;
    unsigned char blur_original;
    unsigned char *gameplay_flag[4];
    int gameplay_ready[4];
    uint32_t *dash_table;
    uint32_t dash_off[12],dash_on[12];
} IssLiveMods;
static inline uint64_t iss_live_camera_pack(float pan,float tilt){
    uint32_t a,b;memcpy(&a,&pan,4);memcpy(&b,&tilt,4);return (uint64_t)a|((uint64_t)b<<32);
}
static inline int iss_live_mod_available(const IssLiveMods *s,int id){
    if(id>=2&&id<6)return s->gameplay_ready[id-2]&&s->gameplay_flag[id-2];
    return id==0 ? s->camera_data&&s->camera_ready&&s->pitch_ready : id==1 ? s->blur_flag!=0 : 0;
}
static inline int iss_live_mod_get(const IssLiveMods*s,int id){
    if(id>=2&&id<6)return iss_live_mod_available(s,id)?__atomic_load_n(s->gameplay_flag[id-2],__ATOMIC_ACQUIRE):0;
    return id==0?__atomic_load_n(&s->camera_enabled,__ATOMIC_ACQUIRE):id==1?__atomic_load_n(&s->blur_enabled,__ATOMIC_ACQUIRE):0;
}
static inline int iss_live_mod_set(IssLiveMods*s,int id,int enabled){
    if(!iss_live_mod_available(s,id)||(enabled!=0&&enabled!=1))return 0;
    if(id>=2&&id<6){
        if(id==5&&enabled)for(unsigned i=0;i<12;i++)__atomic_store_n(s->dash_table+i,s->dash_on[i],__ATOMIC_RELEASE);
        __atomic_store_n(s->gameplay_flag[id-2],(unsigned char)enabled,__ATOMIC_RELEASE);
        if(id==5&&!enabled)for(unsigned i=0;i<12;i++)__atomic_store_n(s->dash_table+i,s->dash_off[i],__ATOMIC_RELEASE);
        return 1;
    }
    if(id==0){
        __atomic_store_n(s->camera_data,iss_live_camera_pack(enabled?1.5f:1.f,enabled?1.5f:1.f),__ATOMIC_RELEASE);
        __atomic_store_n(&s->camera_enabled,enabled,__ATOMIC_RELEASE);
    }else{
        __atomic_store_n(s->blur_flag,enabled?1:s->blur_original,__ATOMIC_RELEASE);
        __atomic_store_n(&s->blur_enabled,enabled,__ATOMIC_RELEASE);
    }
    return 1;
}
/* Resolve only the exact known blur-off branch; no running code is rewritten. */
static inline int iss_live_blur_find(const unsigned char*image,uint64_t size,uint64_t*site,uint64_t*flag){
    static const unsigned char before[]={0x0f,0x85,0x8a,0x04,0,0,0x48,0x8d,0x05,0x18,0x36,0xcf,0,0x80,0x38,0,0x0f,0x85,0x7a,0x04,0,0};
    static const unsigned char enabled[]={0xe9,0x8b,0x04,0,0,0x90};
    uint64_t found=0,at=0;
    for(uint64_t i=0;i+sizeof(before)<=size;i++){
        if(image[i+6]!=0x48||memcmp(image+i+6,before+6,sizeof(before)-6))continue;
        if(memcmp(image+i,before,6)&&memcmp(image+i,enabled,6))continue;
        at=i;if(++found>1)return 0;
    }
    if(found!=1)return 0;
    int32_t displacement;memcpy(&displacement,image+at+9,4);
    int64_t target=(int64_t)at+13+displacement;
    if(target<0||(uint64_t)target>=size||image[target]>1)return 0;
    *site=at;*flag=(uint64_t)target;return 1;
}
#endif
