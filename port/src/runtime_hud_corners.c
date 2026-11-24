#include "iss_hud_corners.h"
#include "iss_native_resolution.h"
#include "iss_menu_aspect.h"
#include <stdlib.h>
#include <time.h>
#include <stdatomic.h>
#include <stdio.h>
static uintptr_t image_base;
static atomic_uint announced;
static atomic_uint inspected;
static atomic_uint calls;
void runtime_hud_corner_attach(uintptr_t base){image_base=base;}
static uint64_t hud64(const unsigned char *p){uint64_t v;memcpy(&v,p,8);return v;}
static uint32_t hud32(const unsigned char *p){uint32_t v;memcpy(&v,p,4);return v;}
/* Pause menu in its own 16:9 shape without black bars (experimental, ISS_PAUSE_MENU_FIT=1, 2026-11-06):
 * the game stretches its 16:9 menu canvas across a wide frame; instead of squeezing the whole picture
 * (the presenter's centred 16:9 view, with bars), the menu's own elements are drawn at 16:9 in the
 * centre while the paused game world keeps filling the screen. Menu elements are recognised by their
 * owner chain reaching the verified 1870x1020 menu root (iss_menu_member). */
static int hud_read_menu_node(uintptr_t at,IssMenuNode *out,void *user){
    (void)user;
    if(at<0x1000000000ull || at>=0x10000000000ull)return 0;
    const unsigned char *p=(const unsigned char*)at;
    out->vtable=(uintptr_t)hud64(p)-image_base;out->owner=(uintptr_t)hud64(p+0x38);
    out->width=hud32(p+0x4ec);out->height=hud32(p+0x4f0);out->flags=hud32(p+0x4f4);
    return 1;
}
/* On by default since the 2026-11-06 play test (pause menu: right shape, no bars); ISS_PAUSE_MENU_FIT=0
 * returns to the presenter's centred 16:9 view of the whole picture. Known gaps (2026-11-07): the Powers
 * and Map pages have pieces placed outside this hook (partly fitted), Options uses another root (boxed). */
int runtime_pause_menu_fit_enabled(void){
    static int on=-1;if(on<0){const char *e=getenv("ISS_PAUSE_MENU_FIT");on=!(e&&e[0]=='0');}
    return on;
}
static atomic_uint menu_roots,menu_members,menu_logged;
static _Atomic uint64_t menu_fitted_ms;
/* 1 while recognised pause-menu elements are being drawn at 16:9 (one within the last 250 ms): only then
 * does the presenter skip its 16:9 view; other paused screens (not recognised) keep it. */
int runtime_pause_menu_fitted_now(void){
    const uint64_t last=atomic_load(&menu_fitted_ms);
    struct timespec t;timespec_get(&t,TIME_UTC);const uint64_t now=(uint64_t)t.tv_sec*1000u+(uint64_t)t.tv_nsec/1000000u;
    return last && now>=last && now-last<250;
}
void __attribute__((ms_abi)) runtime_hud_corner_node(const void *widget,void *output){
    if(!image_base || runtime_native_status()!=1)return;
    const IssNativePlan *plan=runtime_native_plan();
    if(atomic_fetch_add(&calls,1)==0)fprintf(stderr,"HUD_CORNER_CALL ui=%d canvas=%ux%u widget=%p matrix=%p\n",plan->ui_16_9,plan->ui_width,plan->ui_height,widget,output);
    /* Wide UI canvas (2026-11-07): the game places its HUD once, from the safe area it reads at
     * startup. A live "Place HUD at screen edges" change then shifts the three corner groups by the
     * gap between the two placements, (canvas width - 1920) / 2 units, until the next restart. */
    const int wide=plan->ui_16_9==0 && plan->ui_height==1080 && plan->ui_width>1920 && plan->ui_width<=3840;
    if(!wide && (plan->ui_16_9!=1 || plan->ui_width!=1920 || plan->ui_height!=1080))return;
    const unsigned char *p=widget;
    uintptr_t vt=(uintptr_t)hud64(p),owner=(uintptr_t)hud64(p+0x38),owner_vt=0;
    if(wide){
        static int startup_edges=-1;if(startup_edges<0)startup_edges=plan->hud_edges;
        if(plan->hud_edges==startup_edges)return;
        if(vt==image_base+0xd74668u){
            if(!owner)return;
            owner_vt=(uintptr_t)hud64((const unsigned char*)owner)-image_base;
        }else if(vt!=image_base+0xd795d0u || owner)return;
        const int kind=iss_hud_corner_kind(vt-image_base,owner_vt,hud32(p+0x4ec),hud32(p+0x4f0),hud32(p+0x4f4));
        if(!kind)return;
        float matrix[16];memcpy(matrix,output,sizeof(matrix));
        const float gap=(float)(plan->ui_width-1920u)/2.0f*(plan->hud_edges?1.0f:-1.0f);
        if(!isfinite(matrix[12]))return;
        matrix[12]+=kind==3?gap:-gap;
        memcpy(output,matrix,sizeof(matrix));
        static atomic_uint shifted;
        if(!(atomic_fetch_or(&shifted,1u<<kind)&(1u<<kind)))fprintf(stderr,"HUD_CORNERS kind=%d live shift %.0f (HUD %s)\n",
            kind,(double)(kind==3?gap:-gap),plan->hud_edges?"at screen edges":"centred 16:9");
        return;
    }
    if(runtime_pause_menu_fit_enabled() && iss_menu_member((uintptr_t)widget,hud_read_menu_node,NULL)){
        const int root=vt==image_base+0xcdfc10u && !owner;
        const unsigned n=atomic_fetch_add(root?&menu_roots:&menu_members,1);
        float matrix[16];memcpy(matrix,output,sizeof(matrix));
        if(atomic_fetch_add(&menu_logged,1)<12)fprintf(stderr,
            "PAUSE_MENU_FIT %s #%u vt=+%llx matrix x=[%.4g %.4g %.4g] t=[%.4g %.4g] w=%.4g\n",root?"root":"member",n,
            (unsigned long long)(vt-image_base),(double)matrix[0],(double)matrix[4],(double)matrix[8],
            (double)matrix[12],(double)matrix[13],(double)matrix[15]);
        /* Every menu element: squeezing only the root left the menu stretched (2026-11-06 test), so each
         * element's matrix is squeezed about the canvas centre on its own. */
        if(iss_menu_transform(matrix,plan->output_width,plan->output_height)){
            memcpy(output,matrix,sizeof(matrix));
            struct timespec t;timespec_get(&t,TIME_UTC);
            atomic_store(&menu_fitted_ms,(uint64_t)t.tv_sec*1000u+(uint64_t)t.tv_nsec/1000000u);
        }
        return;
    }
    // Inspect the parent only for the ordinary HUD group type. Other UI types
    // (including menu/map widgets) leave before touching parent data.
    if(vt==image_base+0xd74668u){
        if(!owner)return;
        owner_vt=(uintptr_t)hud64((const unsigned char*)owner)-image_base;
    }else if(vt!=image_base+0xd795d0u || owner)return;
    int kind=iss_hud_corner_kind(vt-image_base,owner_vt,hud32(p+0x4ec),hud32(p+0x4f0),hud32(p+0x4f4));
    if(!kind)return;
    float matrix[16];memcpy(matrix,output,sizeof(matrix));
    unsigned inspect_bit=1u<<(unsigned)kind;
    if(!(atomic_fetch_or(&inspected,inspect_bit)&inspect_bit))fprintf(stderr,
        "HUD_CORNER_MATRIX kind=%d rows=[%.6g %.6g %.6g %.6g][%.6g %.6g %.6g %.6g][%.6g %.6g %.6g %.6g][%.6g %.6g %.6g %.6g]\n",
        kind,(double)matrix[0],(double)matrix[1],(double)matrix[2],(double)matrix[3],
        (double)matrix[4],(double)matrix[5],(double)matrix[6],(double)matrix[7],
        (double)matrix[8],(double)matrix[9],(double)matrix[10],(double)matrix[11],
        (double)matrix[12],(double)matrix[13],(double)matrix[14],(double)matrix[15]);
    if(!iss_hud_corner_transform(matrix,plan->output_width,plan->output_height,kind))return;
    memcpy(output,matrix,sizeof(matrix));
    unsigned bit=1u<<(unsigned)kind;
    if(!(atomic_fetch_or(&announced,bit)&bit))fprintf(stderr,"HUD_CORNERS kind=%d matched: original 16:9 proportions, corner anchored; menu canvas unchanged\n",kind);
}
