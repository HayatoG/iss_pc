/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
/* Minimap fit with the wide UI canvas (2026-11-12). The minimap picture is drawn with a correction of
 * (canvas height*16)/(canvas width*9): 1 on the 16:9 canvas the game was made for, 0.74 on 2580x1080,
 * which shrank the minimap and pushed its skyline under the frame line. Its two loads of the canvas
 * cells (height, then width 0x2c bytes later) are pointed at a fixed 1920x1080 pair, so the minimap
 * keeps its 16:9 shape while the rest of the HUD uses the wide canvas. Same code in both games. */
#define ISS_MINIMAP_SIGNATURE_SIZE 0x34
/* 0xff in the mask: the byte must match; 0: the rip displacement of the two cell loads. */
static const unsigned char iss_minimap_signature[ISS_MINIMAP_SIGNATURE_SIZE]={
 0x48,0x8d,0x05,0,0,0,0, 0xc7,0x45,0xa0,0,0,0,0, 0xb9,0x0e,0x0e,0,0, 0x4c,0x8d,0x45,0xa0,
 0xba,0xff,0xff,0xff,0xff, 0x41,0xb9,1,0,0,0, 0x4c,0x89,0xf7, 0x48,0x89,0xde, 0xc5,0xfa,0x2a,0x00,
 0x48,0x8d,0x05,0,0,0,0, 0xc5};
static inline int iss_minimap_wildcard(unsigned i){return (i>=3&&i<7)||(i>=0x2f&&i<0x33);}
/* Second Son 1.07: 0x45766f, cells 0x1d0ccc0; First Light 1.04: 0x4f9a9f, cells 0x1f272c0. */
static inline uint32_t iss_minimap_site_for(const char *title,const char *version,uint32_t *cells){
 if(!title||!version)return 0;
 if(!strcmp(title,"CUSA00223")&&!strcmp(version,"01.07")){*cells=0x1d0ccc0;return 0x45766f;}
 if(!strcmp(title,"CUSA00575")&&!strcmp(version,"01.04")){*cells=0x1f272c0;return 0x4f9a9f;}
 return 0;
}
static inline int32_t iss_minimap_disp(const unsigned char *p){int32_t d;memcpy(&d,p,4);return d;}
/* Verifies the code (including that both loads still read the game's own height and width cells)
 * and points them at fixed[1] (height) and fixed[0] (width). Returns 1 when patched. */
static inline int iss_minimap_fit(unsigned char *image,size_t size,const char *title,const char *version,uint32_t fixed[2]){
 uint32_t cells=0,site=iss_minimap_site_for(title,version,&cells);
 if(!site||!image||!fixed||size<site+ISS_MINIMAP_SIGNATURE_SIZE)return 0;
 unsigned char *p=image+site;
 for(unsigned i=0;i<ISS_MINIMAP_SIGNATURE_SIZE;i++)if(!iss_minimap_wildcard(i)&&p[i]!=iss_minimap_signature[i])return 0;
 if((int64_t)site+7+iss_minimap_disp(p+3)!=(int64_t)cells+4||(int64_t)site+0x33+iss_minimap_disp(p+0x2f)!=(int64_t)cells)return 0;
 intptr_t h=(intptr_t)&fixed[1]-(intptr_t)(p+7),w=(intptr_t)&fixed[0]-(intptr_t)(p+0x33);
 if(h<INT32_MIN||h>INT32_MAX||w<INT32_MIN||w>INT32_MAX)return 0;
 fixed[0]=1920;fixed[1]=1080;
 int32_t hd=(int32_t)h,wd=(int32_t)w;memcpy(p+3,&hd,4);memcpy(p+0x2f,&wd,4);
 return 1;
}
