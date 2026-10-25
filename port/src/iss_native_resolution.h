/* SPDX-License-Identifier: GPL-2.0-or-later
 * Native startup render dimensions, CUSA00223 01.07 only.
 * Game mechanisms researched by axylpup/shadPS4: 55fdf965, 5afba802,
 * 160de347, 63692218. Independently checked against the US executable.
 * No game files, camera speed, controller input or progression are changed.
 */
#ifndef ISS_NATIVE_RESOLUTION_H
#define ISS_NATIVE_RESOLUTION_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

enum { ISS_NATIVE_PRESETS = 6, ISS_NATIVE_SITE_COUNT = 18 };
static const uint32_t iss_native_widths[] = {0,1920,2560,3440,3840,5120};
static const uint32_t iss_native_heights[] = {0,1080,1440,1440,2160,1440};
typedef struct IssNativeConfig { int output, percent, hud_edges;
#ifdef __cplusplus
    int ui_16_9=0;
#else
    int ui_16_9;
#endif
} IssNativeConfig;
typedef struct {
    uint32_t output_width, output_height, width, height, ui_width, ui_height, light_entries;
    int percent, hud_edges, ui_16_9;
} IssNativePlan;
typedef struct {uint32_t offset; unsigned length; unsigned char before[17], after[17];} IssNativeSite;
#include "iss_native_sites.inc"
/* Logical screen width/height are used by the camera's aspect division. Guard
 * the actual US instructions too, rather than trusting a serial or EU offsets. */
static const unsigned char iss_native_projection[] = {
  0x48,0x8d,0x05,0xb1,0xc9,0x78,0x01,0xc5,0xf8,0x57,0xc0,
  0xc5,0xfa,0x2a,0x01,0xc5,0xfa,0x2a,0x08,0xb0,0x01,0xc5,0xfa,0x5e,0xc1
};
static const unsigned char iss_native_aspect_width[]={0x48,0x8d,0x0d,0xd3,0xc9,0x78,0x01};
static inline int iss_native_integer(const char *s,int min,int max,int *out) {
    if (!s || !*s) return 0;
    char *end; errno=0; long n=strtol(s,&end,10);
    if (errno || *end || n<min || n>max) return 0;
    *out=(int)n; return 1;
}
/* Strict parsing: malformed native settings reject the request, not a silently
 * clamped substitute resolution. Missing keys default to off/100%/edge HUD. */
static inline int iss_native_config(FILE *f, IssNativeConfig *c) {
    *c=(IssNativeConfig){0,100,1,0};
    if (!f) return 1;
    char line[1024]; int valid=1;
    while (fgets(line,sizeof(line),f)) {
        size_t n=strlen(line);
        while(n && (line[n-1]==' '||line[n-1]=='\r'||line[n-1]=='\n'||line[n-1]=='\t'))line[--n]=0;
        if (!strncmp(line,"iss_native_output=",18)) valid &= iss_native_integer(line+18,0,ISS_NATIVE_PRESETS-1,&c->output);
        if (!strncmp(line,"iss_native_percent=",19)) valid &= iss_native_integer(line+19,25,200,&c->percent);
        if (!strncmp(line,"iss_native_hud_edges=",21)) valid &= iss_native_integer(line+21,0,1,&c->hud_edges);
        if (!strncmp(line,"iss_native_ui_16_9=",19)) valid &= iss_native_integer(line+19,0,2,&c->ui_16_9);
    }
    return valid;
}
static inline int iss_native_plan(IssNativeConfig c, IssNativePlan *p) {
    memset(p,0,sizeof(*p));
    if(c.output<=0 || c.output>=ISS_NATIVE_PRESETS || c.percent<25 || c.percent>200 ||
       (c.hud_edges!=0 && c.hud_edges!=1)||c.ui_16_9<0||c.ui_16_9>2)return 0;
    p->output_width=iss_native_widths[c.output]; p->output_height=iss_native_heights[c.output];
    /* At 100% these are EXACT output pixels, never an upscale-only 16:9 image.
     * Other percentages round to even pixels; Vulkan tiling alignment is handled
     * by the game's surface allocator, not by changing the visible dimensions. */
    p->width=(uint32_t)(((uint64_t)p->output_width*c.percent+100)/200)*2;
    p->height=(uint32_t)(((uint64_t)p->output_height*c.percent+100)/200)*2;
    if (!p->width || !p->height || (uint64_t)p->width*p->height>3840u*2160u) {
        memset(p,0,sizeof(*p)); return 0;
    }
    p->ui_height=1080;
    p->ui_width=(p->output_width*1080u+p->output_height/2)/p->output_height;
    if(c.ui_16_9)p->ui_width=1920;
    // Diagnostic: retain a 16:9 menu aspect while giving fixed-size HUD
    // artwork the wide display's logical width. Integer 16:9 units keep
    // the canvas proportional; never alter scene/output dimensions here.
    if(c.ui_16_9==2 && (uint64_t)p->output_width*9u>(uint64_t)p->output_height*16u){
        uint32_t wide=(p->output_width*1080u+p->output_height/2)/p->output_height;
        uint32_t units=(wide+15u)/16u;
        p->ui_width=units*16u;p->ui_height=units*9u;
    }
    uint64_t entries=(uint64_t)((p->width+15)/16)*((p->height+15)/16)*128;
    entries=(entries+65535)&~(uint64_t)65535;
    p->light_entries=(uint32_t)(entries<0x320000 ? 0x320000 : entries);
    p->percent=c.percent; p->hud_edges=c.hud_edges;p->ui_16_9=c.ui_16_9;
    return 1;
}
static inline void iss_native_write32(unsigned char *p,uint32_t x) {memcpy(p,&x,4);}
static inline void iss_native_changes(const IssNativePlan *p,IssNativeSite sites[ISS_NATIVE_SITE_COUNT]) {
    memcpy(sites,iss_native_sites,sizeof(iss_native_sites));
    const uint32_t values[]={p->width,p->height,p->width,p->height,p->ui_width,p->ui_height,p->width,p->height};
    const unsigned imm[]={3,3,2,3,4,3,1,1};
    for(unsigned i=0;i<8;i++){
        memcpy(sites[i].after,sites[i].before,sites[i].length);
        iss_native_write32(sites[i].after+imm[i],values[i]);
    }
    for(unsigned i=14;i<16;i++){
        memcpy(sites[i].after,sites[i].before,sites[i].length);
        iss_native_write32(sites[i].after+(i==14?3:2),p->light_entries);
    }
    // Experimental separation: retain the game's original UI canvas while
    // the world camera reads the already-guarded render-dimension immediates.
    // No borrowed data cave, unverified offset, or live memory poke is used.
    for(unsigned i=16;i<18;i++){
        memcpy(sites[i].after,sites[i].before,sites[i].length);
        if(p->ui_16_9){
            const uint32_t target=i==16?0x1eff4a:0x1eff51;
            const int32_t displacement=(int32_t)((int64_t)target-(sites[i].offset+7));
            memcpy(sites[i].after+3,&displacement,4);
        }
    }
}
/* Atomic preflight: a single incompatible byte, short image, wrong region or
 * other render patch refuses ALL writes, including lighting capacity. */
static inline int iss_native_apply(unsigned char *image,uint64_t size,const char *serial,
                                   const char *version,IssNativeConfig config) {
    IssNativePlan plan;
    if(!image || strcmp(serial,"CUSA00223") || strcmp(version,"01.07") || !iss_native_plan(config,&plan))return 0;
    if(size<0x58030cu+sizeof(iss_native_projection) ||
       memcmp(image+0x58030c,iss_native_projection,sizeof(iss_native_projection)) ||
       memcmp(image+0x5802e6,iss_native_aspect_width,sizeof(iss_native_aspect_width)))return 0;
    IssNativeSite sites[ISS_NATIVE_SITE_COUNT];iss_native_changes(&plan,sites);
    for(unsigned i=0;i<ISS_NATIVE_SITE_COUNT;i++) {
        const IssNativeSite *s=sites+i;
        if(s->offset>size || s->length>size-s->offset || memcmp(image+s->offset,s->before,s->length))return 0;
    }
    for(unsigned i=0;i<ISS_NATIVE_SITE_COUNT;i++)memcpy(image+sites[i].offset,sites[i].after,sites[i].length);
    return 1;
}
#ifdef __cplusplus
extern "C" {
#endif
/* Immutable after startup, safe for the render thread to query. */
void runtime_native_publish(int status,const char *reason,const IssNativePlan *plan);
int runtime_native_status(void);
const char *runtime_native_reason(void);
const IssNativePlan *runtime_native_plan(void);
float runtime_native_safe_area(void);
#ifdef __cplusplus
}
#endif
#endif
