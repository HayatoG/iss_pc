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
#include "iss_native_exposure.h"

/* 6 and 7 (2026-11-05): the 1080-line ultrawides, 2560x1080 (21:9) and 3840x1080 (32:9). Added at
 * the end so a saved iss_native_output keeps its meaning. */
enum { ISS_NATIVE_PRESETS = 8, ISS_NATIVE_SITE_COUNT = 18 };
static const uint32_t iss_native_widths[] = {0,1920,2560,3440,3840,5120,2560,3840};
static const uint32_t iss_native_heights[] = {0,1080,1440,1440,2160,1440,1080,1080};
typedef struct IssNativeConfig { int output, percent, hud_edges;
#ifdef __cplusplus
    int ui_16_9=0;
    int temporal_mode=0;
#else
    int ui_16_9;
    int temporal_mode;
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
/* Temporal upscaling modes (iss_temporal_mode): the scene is the output divided by the FSR 3.1 ratio.
 * 0 keeps iss_native_percent as the player set it (100% = temporal anti-aliasing at the output
 * size); 1 Quality 1.5x, 2 Balanced 1.7x, 3 Performance 2.0x, 4 Ultra performance 3.0x. */
enum { ISS_TEMPORAL_MODES = 5 };
static const int iss_temporal_mode_percent[ISS_TEMPORAL_MODES] = {0, 67, 59, 50, 33};
static inline int iss_native_integer(const char *s,int min,int max,int *out) {
    if (!s || !*s) return 0;
    char *end; errno=0; long n=strtol(s,&end,10);
    if (errno || *end || n<min || n>max) return 0;
    *out=(int)n; return 1;
}
/* Strict parsing: malformed native settings reject the request, not a silently
 * clamped substitute resolution. Missing keys default to off/100%, the UI canvas of
 * the screen's own shape and the HUD at the screen edges (2026-11-07: menus laid out
 * by the game for wide screens, checked in play at 3440x1440). Explicit existing
 * choices remain respected. */
static inline int iss_native_config(FILE *f, IssNativeConfig *c) {
    *c=(IssNativeConfig){0,100,1,0,0};
    if (!f) return 1;
    char line[1024]; int valid=1;
    while (fgets(line,sizeof(line),f)) {
        size_t n=strlen(line);
        while(n && (line[n-1]==' '||line[n-1]=='\r'||line[n-1]=='\n'||line[n-1]=='\t'))line[--n]=0;
        if (!strncmp(line,"iss_native_output=",18)) valid &= iss_native_integer(line+18,0,ISS_NATIVE_PRESETS-1,&c->output);
        if (!strncmp(line,"iss_native_percent=",19)) valid &= iss_native_integer(line+19,25,200,&c->percent);
        if (!strncmp(line,"iss_native_hud_edges=",21)) valid &= iss_native_integer(line+21,0,1,&c->hud_edges);
        if (!strncmp(line,"iss_native_ui_16_9=",19)) valid &= iss_native_integer(line+19,0,2,&c->ui_16_9);
        if (!strncmp(line,"iss_temporal_mode=",18)) valid &= iss_native_integer(line+18,0,ISS_TEMPORAL_MODES-1,&c->temporal_mode);
    }
    return valid;
}
/* Folds the temporal mode into the config: the percent the mode stands for replaces iss_native_percent.
 * Returns 1 when it did, 0 when nothing changed (mode 0, or the native output is off: the scene size
 * is the game's own then and there is no render size to choose). */
static inline int iss_native_apply_temporal_mode(IssNativeConfig *c,int mode) {
    if (mode<=0 || mode>=ISS_TEMPORAL_MODES || c->output<=0) return 0;
    c->temporal_mode=mode; c->percent=iss_temporal_mode_percent[mode]; return 1;
}
/* One output dimension scaled by `percent`, rounded to an even pixel count. */
static inline uint32_t iss_native_scale(uint32_t dimension,int percent) {
    return (uint32_t)(((uint64_t)dimension*percent+100)/200)*2;
}
static inline int iss_native_plan(IssNativeConfig c, IssNativePlan *p) {
    memset(p,0,sizeof(*p));
    if(c.output<=0 || c.output>=ISS_NATIVE_PRESETS || c.percent<25 || c.percent>200 ||
       (c.hud_edges!=0 && c.hud_edges!=1)||c.ui_16_9<0||c.ui_16_9>2)return 0;
    p->output_width=iss_native_widths[c.output]; p->output_height=iss_native_heights[c.output];
    /* At 100% these are EXACT output pixels, never an upscale-only 16:9 image.
     * Other percentages round to even pixels; Vulkan tiling alignment is handled
     * by the game's surface allocator, not by changing the visible dimensions. */
    p->width=iss_native_scale(p->output_width,c.percent);
    p->height=iss_native_scale(p->output_height,c.percent);
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
/* ---- Internal resolution as a height (shared by the launchers and the in-game menu) ----
 * The ini keeps only the output preset and the percent; the player thinks in "render 720p,
 * show 1440p". iss_native_pick() turns a wanted scene height into the percent to store (the
 * closest the integer percent allows, lowest percent on a tie), and reports an inexact result and
 * a refused request instead of hiding them. port/launcher/native_resolution.py is the same
 * arithmetic and tests/native_resolution_cases.txt feeds both. */
enum { ISS_NATIVE_STEPS = 6 };
static const uint32_t iss_native_step_heights[ISS_NATIVE_STEPS] = {540,720,900,1080,1440,2160};
enum { ISS_NATIVE_PICK_OK = 1, ISS_NATIVE_PICK_BUDGET = 0, ISS_NATIVE_PICK_RANGE = -1, ISS_NATIVE_PICK_OUTPUT = -2 };
typedef struct { int percent; uint32_t width, height; int32_t error; } IssNativePick;
static inline int iss_native_pick(int output,uint32_t target_height,IssNativePick *r) {
    memset(r,0,sizeof(*r));
    if (output<=0 || output>=ISS_NATIVE_PRESETS) return ISS_NATIVE_PICK_OUTPUT;
    const uint32_t out_h=iss_native_heights[output];
    if ((uint64_t)target_height*100<(uint64_t)out_h*25 || (uint64_t)target_height*100>(uint64_t)out_h*200)
        return ISS_NATIVE_PICK_RANGE;
    int best=25; int64_t best_error=-1;
    for (int percent=25;percent<=200;percent++) {
        int64_t error=(int64_t)iss_native_scale(out_h,percent)-(int64_t)target_height;
        if (error<0) error=-error;
        if (best_error<0 || error<best_error) {best=percent;best_error=error;}
    }
    r->percent=best;
    r->width=iss_native_scale(iss_native_widths[output],best);
    r->height=iss_native_scale(out_h,best);
    r->error=(int32_t)r->height-(int32_t)target_height;
    IssNativePlan plan;
    return iss_native_plan((IssNativeConfig){output,best,1,0,0},&plan) ? ISS_NATIVE_PICK_OK : ISS_NATIVE_PICK_BUDGET;
}
/* The step (index into iss_native_step_heights) that a stored percent stands for on this output,
 * or -1 when the percent matches none (a hand-edited ini). */
static inline int iss_native_step_for_percent(int output,int percent) {
    IssNativePick pick;
    for (int i=0;i<ISS_NATIVE_STEPS;i++) {
        const int status=iss_native_pick(output,iss_native_step_heights[i],&pick);
        if ((status==ISS_NATIVE_PICK_OK || status==ISS_NATIVE_PICK_BUDGET) && pick.percent==percent) return i;
    }
    return -1;
}
/* The percent to store when the output changes, so that "render 720p" stays 720p: the step the
 * old percent stood for is picked again on the new output. A percent that is no step, an unusable
 * new output or a refused result leaves the percent as it was. */
static inline int iss_native_retarget(int old_output,int percent,int new_output) {
    const int step=iss_native_step_for_percent(old_output,percent);
    IssNativePick pick;
    if (step<0 || iss_native_pick(new_output,iss_native_step_heights[step],&pick)!=ISS_NATIVE_PICK_OK) return percent;
    return pick.percent;
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
    const uint32_t exposure_entries=iss_native_exposure_entries(plan.width,plan.height);
    if(!exposure_entries || (exposure_entries>ISS_EXPOSURE_ORIGINAL_ENTRIES&&!iss_native_exposure_guard(image,size)))return 0;
    if(size<0x58030cu+sizeof(iss_native_projection) ||
       memcmp(image+0x58030c,iss_native_projection,sizeof(iss_native_projection)) ||
       memcmp(image+0x5802e6,iss_native_aspect_width,sizeof(iss_native_aspect_width)))return 0;
    IssNativeSite sites[ISS_NATIVE_SITE_COUNT];iss_native_changes(&plan,sites);
    for(unsigned i=0;i<ISS_NATIVE_SITE_COUNT;i++) {
        const IssNativeSite *s=sites+i;
        if(s->offset>size || s->length>size-s->offset || memcmp(image+s->offset,s->before,s->length))return 0;
    }
    for(unsigned i=0;i<ISS_NATIVE_SITE_COUNT;i++)memcpy(image+sites[i].offset,sites[i].after,sites[i].length);
    if(exposure_entries>ISS_EXPOSURE_ORIGINAL_ENTRIES)iss_native_exposure_write(image,exposure_entries);
    return 1;
}
/* Private capacity-only control: increase the two already-guarded lighting
 * immediates after native startup patching. Keep dimensions and UI unchanged.
 * Preflight both entire instructions before either write. */
static inline int iss_native_raise_light_capacity(unsigned char *image,size_t size,const IssNativePlan *plan,uint32_t entries){
    if(!image||!plan||entries<plan->light_entries||entries<0x320000u||entries>0x400000u||(entries&65535u))return 0;
    IssNativeSite sites[ISS_NATIVE_SITE_COUNT];iss_native_changes(plan,sites);
    for(unsigned i=14;i<16;i++)if(sites[i].offset>size||sites[i].length>size-sites[i].offset||
        memcmp(image+sites[i].offset,sites[i].after,sites[i].length))return 0;
    iss_native_write32(image+sites[14].offset+3,entries);
    iss_native_write32(image+sites[15].offset+2,entries);return 1;
}
#ifdef __cplusplus
extern "C" {
#endif
/* Published snapshots are copied under a short lock. The returned plan is a
 * thread-local snapshot, valid until that thread's next plan query. */
void runtime_native_publish(int status,const char *reason,const IssNativePlan *plan);
int runtime_native_status(void);
const char *runtime_native_reason(void);
const IssNativePlan *runtime_native_plan(void);
float runtime_native_safe_area(void);
#ifdef __cplusplus
}
#endif
#endif
