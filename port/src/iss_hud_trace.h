/* Local UI research only. No widget values are changed. */
#ifndef ISS_HUD_TRACE_H
#define ISS_HUD_TRACE_H
#include <stdint.h>
#include <string.h>
#define ISS_HUD_TRACE_SITE 0x564cf0u
#define ISS_HUD_TRACE_STOLEN 16u
static const unsigned char iss_hud_trace_entry[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x53,0x48,0x81,0xec,0x88,0,0,0};
static inline int iss_hud_trace_signature(const unsigned char *image,uint64_t size){
    static const unsigned char scale[]={0xc5,0xfa,0x10,0xab,0xc0,0x04,0,0};
    static const unsigned char layout[]={0x48,0x8b,0x8b,0xf0,0x04,0,0,0xc5,0xfa,0x2a,0x93,0xec,0x04,0,0,0xc5,0xfa,0x2a,0xc9,0x48,0xc1,0xe9,0x20};
    return size>=0x5651a4u && !memcmp(image+ISS_HUD_TRACE_SITE,iss_hud_trace_entry,sizeof(iss_hud_trace_entry)) &&
        !memcmp(image+0x564f99,scale,sizeof(scale)) && !memcmp(image+0x564d49,layout,sizeof(layout));
}
void runtime_hud_trace_attach(uintptr_t base);
void __attribute__((ms_abi)) runtime_hud_trace_node(const void *widget,const void *matrix,uintptr_t caller);
#endif
