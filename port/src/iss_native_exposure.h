/* SPDX-License-Identifier: GPL-2.0-or-later
 * US 01.07 exposure metering: ceil(W/128)*ceil(H/72) float results.
 * The original ping-pong buffers hold only 25*25 floats. Native 4K needs
 * 30*30; both allocations AND both descriptors must grow before startup. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
enum { ISS_EXPOSURE_ORIGINAL_ENTRIES=625,ISS_EXPOSURE_SITE_COUNT=8 };
typedef struct {uint32_t offset;unsigned char before[5];} IssExposureSite;
static const IssExposureSite iss_exposure_sites[]={
 {0x17132a,{0xbe,0xc4,9,0,0}}, {0x17134e,{0xb9,0x71,2,0,0}},
 {0x171374,{0xbe,0xc4,9,0,0}}, {0x171398,{0xb9,0x71,2,0,0}},
 /* Inlined startup allocator at 0x172320: the active initial pair. */
 {0x172340,{0xbe,0xc4,9,0,0}}, {0x172363,{0xb9,0x71,2,0,0}},
 {0x172387,{0xbe,0xc4,9,0,0}}, {0x1723aa,{0xb9,0x71,2,0,0}}
};
static const unsigned char iss_exposure_allocator[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x54,0x53,0x49,0x89,0xfc};
/* inFAMOUS First Light CUSA00575 01.04 (2026-11-12): the same allocator (0x197660) and sites, same bytes. */
enum { ISS_EXPOSURE_ALLOCATOR=0x171310, ISS_EXPOSURE_ALLOCATOR_FL=0x197660 };
static const IssExposureSite iss_exposure_sites_fl[]={
 {0x19767a,{0xbe,0xc4,9,0,0}}, {0x19769e,{0xb9,0x71,2,0,0}},
 {0x1976c4,{0xbe,0xc4,9,0,0}}, {0x1976e8,{0xb9,0x71,2,0,0}},
 {0x198680,{0xbe,0xc4,9,0,0}}, {0x1986a3,{0xb9,0x71,2,0,0}},
 {0x1986c7,{0xbe,0xc4,9,0,0}}, {0x1986ea,{0xb9,0x71,2,0,0}}
};
static inline uint32_t iss_native_exposure_entries(uint32_t width,uint32_t height){
 if(!width||!height||(uint64_t)width*height>3840u*2160u)return 0;
 uint64_t entries=(((uint64_t)width+127)/128)*(((uint64_t)height+71)/72);
 if(entries<=ISS_EXPOSURE_ORIGINAL_ENTRIES)return ISS_EXPOSURE_ORIGINAL_ENTRIES;
 entries=(entries+15)&~(uint64_t)15;
 return entries<=UINT32_MAX/4?(uint32_t)entries:0;
}
static inline int iss_native_exposure_guard_at(const unsigned char *image,size_t size,uint32_t allocator,const IssExposureSite *sites){
 if(!image||size<allocator+sizeof(iss_exposure_allocator)||
    memcmp(image+allocator,iss_exposure_allocator,sizeof(iss_exposure_allocator)))return 0;
 for(unsigned i=0;i<ISS_EXPOSURE_SITE_COUNT;i++){
  uint32_t at=sites[i].offset;
  if(at>size||5>size-at||memcmp(image+at,sites[i].before,5))return 0;
 }
 return 1;
}
/* Call only after complete startup preflight, before guest threads run. */
static inline void iss_native_exposure_write_at(unsigned char *image,uint32_t entries,const IssExposureSite *sites){
 for(unsigned i=0;i<ISS_EXPOSURE_SITE_COUNT;i++){
  uint32_t value=(i&1)?entries:entries*4;
  memcpy(image+sites[i].offset+1,&value,4);
 }
}
static inline int iss_native_exposure_guard(const unsigned char *image,size_t size){
 return iss_native_exposure_guard_at(image,size,ISS_EXPOSURE_ALLOCATOR,iss_exposure_sites);
}
static inline void iss_native_exposure_write(unsigned char *image,uint32_t entries){
 iss_native_exposure_write_at(image,entries,iss_exposure_sites);
}
