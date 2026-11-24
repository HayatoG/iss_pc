/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "iss_native_resolution.h"
#include "iss_native_aspect.h"
/* Enabled by default; explicit zero or malformed overrides fail closed. */
static inline int iss_native_live_option(const char *value){return !value||!strcmp(value,"1");}
enum { ISS_NATIVE_FRAME_SITE=0x1f0be0,ISS_NATIVE_FRAME_STOLEN=20,
       ISS_NATIVE_TARGET_SITE=0x18e210,ISS_NATIVE_TARGET_STOLEN=17,
       ISS_NATIVE_SAFE_SITE=0x1f0c67,ISS_NATIVE_SAFE_STOLEN=15,
       ISS_NATIVE_DIM_SITE=0x1f0d6e,ISS_NATIVE_DIM_STOLEN=16 };
static const unsigned char iss_native_frame_signature[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0x88,0,0,0};
static const unsigned char iss_native_target_signature[]={0x55,0x48,0x89,0xe5,0x41,0x56,0x53,0x48,0x89,0xfb,0x48,0x8d,0xbb,0xdc,0x1e,1,0};
static const unsigned char iss_native_safe_signature[]={0x4d,0x89,0xa5,0xa0,0xdc,1,0,0x41,0xc6,0x85,4,0x0f,0xdf,0,0};
static const unsigned char iss_native_dim_signature[]={0x41,0x89,0x5d,0,0x41,0x89,0x4d,4,0x41,0x89,0x45,0x0c,0x41,0x89,0x55,0x10};
/* inFAMOUS First Light 1.04 (2026-11-12): the same four hooks (frame, safe point and dimension stores are
 * byte-identical; the target rebuild function starts with another field) on its own frame object
 * (0x11362d0, same field offsets) and render-target object (0xf84740). */
static const unsigned char iss_native_target_signature_fl[]={0x55,0x48,0x89,0xe5,0x41,0x56,0x53,0x48,0x89,0xfb,0x48,0x8d,0xbb,0x68,0x26,1,0};
typedef struct {
 uint32_t frame_site,target_site,safe_site,dim_site;
 const unsigned char *target_signature;
 uint32_t frame_object,target_object;
 const IssNativeLayout *native;
 const IssNativeAspectLayout *aspect;
} IssNativeLiveLayout;
static const IssNativeLiveLayout iss_native_live_layout_ss={ISS_NATIVE_FRAME_SITE,ISS_NATIVE_TARGET_SITE,ISS_NATIVE_SAFE_SITE,ISS_NATIVE_DIM_SITE,
 iss_native_target_signature,0xf1bcd0,0xe682b0,&iss_native_layout_ss,&iss_native_aspect_layout_ss};
static const IssNativeLiveLayout iss_native_live_layout_fl={0x237670,0x1bc1d0,0x2376f7,0x2377fe,
 iss_native_target_signature_fl,0x11362d0,0xf84740,&iss_native_layout_fl,&iss_native_aspect_layout_fl};
/* The verified layout for this game's code, or NULL (wrong title/version or any byte different). */
static inline const IssNativeLiveLayout *iss_native_live_layout_for(const unsigned char *image,size_t size,const char *title,const char *version){
 const IssNativeLiveLayout *L=NULL;
 if(!image||!title||!version)return NULL;
 if(!strcmp(title,"CUSA00223")&&!strcmp(version,"01.07"))L=&iss_native_live_layout_ss;
 else if(!strcmp(title,"CUSA00575")&&!strcmp(version,"01.04"))L=&iss_native_live_layout_fl;
 if(!L)return NULL;
 if(size<L->frame_site+sizeof(iss_native_frame_signature)||size<L->target_site+sizeof(iss_native_target_signature)||
    size<L->safe_site+sizeof(iss_native_safe_signature)||size<L->dim_site+sizeof(iss_native_dim_signature))return NULL;
 if(memcmp(image+L->frame_site,iss_native_frame_signature,sizeof(iss_native_frame_signature))||
    memcmp(image+L->target_site,L->target_signature,sizeof(iss_native_target_signature))||
    memcmp(image+L->safe_site,iss_native_safe_signature,sizeof(iss_native_safe_signature))||
    memcmp(image+L->dim_site,iss_native_dim_signature,sizeof(iss_native_dim_signature)))return NULL;
 return L;
}
static inline int iss_native_live_signature(const unsigned char *image,size_t size,const char *title,const char *version){
 return iss_native_live_layout_for(image,size,title,version)!=NULL;
}
/* Saved GPR slots in the bridge: R15,R14,R13,R12,R11,R10,R9,R8,
 * RDI,RSI,RBP,RBX,RDX,RCX,RAX,RFLAGS. Only dimension-producing GPRs change. */
static inline void iss_native_live_dimension_registers(uint64_t *saved,const IssNativePlan *p){
 saved[11]=p->width;saved[13]=p->height;saved[14]=p->width;saved[12]=p->height;
 saved[9]=(saved[6]&1)?p->width:0;saved[8]=(saved[6]&1)?p->height:0;
}
static inline int iss_native_live_projection_redirect_for(const IssNativeLayout *L,unsigned char *image,size_t size,const IssNativePlan *plan,uintptr_t cells){
 if(!L||!image||!plan||!cells||size<L->projection_at+sizeof(iss_native_projection))return 0;
 IssNativeSite sites[ISS_NATIVE_SITE_COUNT];iss_native_changes_for(L,plan,sites);
 if(memcmp(image+sites[16].offset,sites[16].after,7)||memcmp(image+sites[17].offset,sites[17].after,7)||
    memcmp(image+L->projection_at+7,L->projection+7,sizeof(iss_native_projection)-7))return 0;
 int32_t displacements[2];
 for(unsigned i=0;i<2;i++){
  intptr_t distance=(intptr_t)(cells+i*4)-(intptr_t)(image+sites[16+i].offset+7);
  if(distance<INT32_MIN||distance>INT32_MAX)return 0;
  displacements[i]=(int32_t)distance;
 }
 for(unsigned i=0;i<2;i++)memcpy(image+sites[16+i].offset+3,&displacements[i],4);
 return 1;
}
static inline int iss_native_live_projection_redirect(unsigned char *image,size_t size,const IssNativePlan *plan,uintptr_t cells){
 return iss_native_live_projection_redirect_for(&iss_native_layout_ss,image,size,plan,cells);
}
#ifdef __cplusplus
extern "C" {
#endif
void runtime_native_live_attach(uintptr_t base);
/* The same with another game's objects (First Light); runtime_native_live_attach uses Second Son's. */
void runtime_native_live_attach_layout(uintptr_t base,const IssNativeLiveLayout *layout);
void __attribute__((ms_abi)) runtime_native_frame_observer(const void *object,int frame);
void __attribute__((ms_abi)) runtime_native_target_observer(const void *object,int unused);
void __attribute__((ms_abi)) runtime_native_safe_resize(void *object);
int runtime_native_live_rebuild_request(void);
int runtime_native_live_rebuild_state(void);
void runtime_native_live_enable(uint32_t *aspect_cells,const IssNativePlan *startup);
int runtime_native_live_enabled(void);
int runtime_native_live_capacity(uint32_t *width,uint32_t *height,uint32_t *lights);
/* Diagnostic resize serial; zero until enabled. No GPU state is touched here. */
uint32_t runtime_native_live_generation(void);
/* CP requests a coherent confirmed old/new size pair for deferred retirement. */
uint32_t runtime_native_live_purge_plan(uint32_t sizes[4]);
void runtime_native_live_aspect_enable(void);
void __attribute__((ms_abi)) runtime_native_live_aspect_camera(void *object,int unused);
int runtime_native_live_request_config(IssNativeConfig config);
const char *runtime_native_live_reason(void);
void __attribute__((ms_abi)) runtime_native_dimension_resize(void *object,uint64_t *saved);
#ifdef __cplusplus
}
#endif
