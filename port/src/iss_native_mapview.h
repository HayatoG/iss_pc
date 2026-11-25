/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
/* Pause map with the wide UI canvas (2026-11-20). The map view sets its size and clamps its zoom with
 * the HUD canvas (zoom <= canvas height * (view w/h) / canvas width, then canvas width * zoom). On a
 * wide canvas (3440x1440 -> 2580x1080) that zoom came out too small, so the map picture did not fill
 * its frame and the edge rows/columns were stretched into blurry bands at the top, bottom and sides.
 * Found by pointing groups of the game's 103 canvas readers at a fixed 1920x1080 pair: only these
 * three (one function plus the one after it) removed the bands, and nothing else changed.
 * Each site is a "lea rax,[canvas cell]"; all three are checked before any is changed. */
typedef struct { uint32_t lea_at; uint32_t cell; } IssMapviewSite; /* cell 0 width, 1 height */
static const IssMapviewSite iss_mapview_ss[3]={{0x5a7e40,0},{0x5a7e86,1},{0x5a7f30,0}};
static const IssMapviewSite iss_mapview_fl[3]={{0x6896d0,0},{0x689716,1},{0x6897c0,0}};
static inline const IssMapviewSite *iss_mapview_sites(const char *title,const char *version,uint32_t *cells){
 if(!title||!version)return NULL;
 if(!strcmp(title,"CUSA00223")&&!strcmp(version,"01.07")){*cells=0x1d0ccc0;return iss_mapview_ss;}
 if(!strcmp(title,"CUSA00575")&&!strcmp(version,"01.04")){*cells=0x1f272c0;return iss_mapview_fl;}
 return NULL;
}
/* Points the three reads at fixed[0] (width) and fixed[1] (height), set to 1920x1080.
 * Returns 1 when patched, 0 (and no writes) when the game code is not as expected. */
static inline int iss_mapview_fit(unsigned char *image,size_t size,const char *title,const char *version,uint32_t fixed[2]){
 uint32_t cells=0;const IssMapviewSite *s=iss_mapview_sites(title,version,&cells);
 if(!s||!image||!fixed)return 0;
 int32_t nd[3];
 for(unsigned i=0;i<3;i++){
  if(size<(size_t)s[i].lea_at+7)return 0;
  const unsigned char *p=image+s[i].lea_at;
  if(p[0]!=0x48||p[1]!=0x8d||p[2]!=0x05)return 0;
  int32_t d;memcpy(&d,p+3,4);
  if((int64_t)s[i].lea_at+7+d!=(int64_t)cells+s[i].cell*4)return 0;
  intptr_t n=(intptr_t)&fixed[s[i].cell]-(intptr_t)(p+7);
  if(n<INT32_MIN||n>INT32_MAX)return 0;
  nd[i]=(int32_t)n;
 }
 fixed[0]=1920;fixed[1]=1080;
 for(unsigned i=0;i<3;i++)memcpy(image+s[i].lea_at+3,&nd[i],4);
 return 1;
}
