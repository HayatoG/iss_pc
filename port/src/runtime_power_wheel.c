#include "iss_power_wheel.h"
#include "power_wheel_transaction.h"
#include "power_wheel_signatures.h"
#include "power_wheel_gameplay.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

static unsigned char *pw_image;
static atomic_int pw_supported,pw_ready,pw_active,pw_mask,pw_error,pw_requested;
static atomic_int pw_visual_open,pw_visual_choice;
static atomic_uint pw_generation;
static unsigned pw_seen_generation;
static uintptr_t pw_hero;
/* Unlimited missiles mod on: the game always holds 5 missiles, so the wheel does not track or
 * restore them (it reads them as 0 and writes none); energy is tracked as usual. */
static int pw_missiles_fixed;
static PwBank pw_bank;
static _Thread_local int pw_inside;
static atomic_flag pw_working=ATOMIC_FLAG_INIT;
extern int tlggpu_overlay_captures_input(void);
static uint32_t pw_u32(const void *p){uint32_t n;memcpy(&n,p,4);return n;}
static uintptr_t pw_pointer(const void *p){uintptr_t n;memcpy(&n,p,sizeof(n));return n;}
static int pw_guest(uintptr_t p){return p>=0x1000000000ull&&p<0x10000000000ull;}
static int pw_read_game(void *context,PwSample *out){
    (void)context;
    uintptr_t hero=pw_pointer(pw_image+0x1d0cfe0);
    if(!pw_guest(hero)||hero!=pw_hero)return 0;
    unsigned char *a=pw_image+0xefacf8,*h=(unsigned char*)hero;
    out->native=(int)pw_u32(a+0xe4);out->missiles=pw_missiles_fixed?0:(int)pw_u32(a+0xdc);
    out->capacity=(int)pw_u32(h+0x1ee4c);
    memcpy(&out->energy,h+0x1a1e8,4);memcpy(&out->maximum,h+0x1a1f0,4);
    for(int i=0;i<PW_COUNT;i++)out->unlocked[i]=pw_u32(a+0xe8+pw_family_id[i]*4);
    return pw_sample_layout_valid(out);
}
typedef void (__attribute__((sysv_abi)) *PwChange)(void *,int);
typedef void (__attribute__((sysv_abi)) *PwAmmo)(void *,int);
static int pw_change_game(void *context,int native){
    (void)context;
    if(pw_pointer(pw_image+0x1d0cfe0)!=pw_hero)return 0;
    ((PwChange)(pw_image+0x2143d0))((void*)pw_hero,native);
    return 1;
}
static int pw_write_game(void *context,float energy,int missiles){
    PwSample current;
    if(!pw_read_game(context,&current)||!isfinite(energy)||energy<0||energy>current.maximum||
       missiles<0||missiles>current.capacity)return 0;
    memcpy((void*)(pw_hero+0x1a1e8),&energy,4);
    if(!pw_missiles_fixed)((PwAmmo)(pw_image+0x1e35a0))(pw_image+0xefacf8,missiles);
    return 1;
}
void runtime_power_wheel_attach(unsigned char *image,size_t size){
    const char *opt=getenv("ISS_POWER_WHEEL_TEST");
    if(!opt||strcmp(opt,"1"))return;
    if(!pw_signatures(image,size)||!pw_pause_signature(image,size)){
        puts("Power wheel: disabled (game-function or pause-state signatures differ)");return;
    }
    pw_image=image;pw_reset(&pw_bank,0);pw_hero=0;
    pw_missiles_fixed=pw_missile_cheat(image,size);
    if(pw_missiles_fixed)puts("Power wheel: Unlimited missiles mod found; missiles stay at 5 and are not tracked per power");
    atomic_store(&pw_requested,0);atomic_store(&pw_ready,0);atomic_store(&pw_error,0);
    atomic_store(&pw_active,-1);atomic_store(&pw_mask,0);atomic_store(&pw_supported,1);
    puts("Power wheel: experimental game-thread switching enabled; current resources carry across every switch");
}
void runtime_power_wheel_load_begin(void){
    atomic_store(&pw_visual_open,0);
    atomic_store(&pw_ready,0);atomic_store(&pw_requested,0);atomic_store(&pw_mask,0);
    atomic_fetch_add(&pw_generation,1);
}
void runtime_power_wheel_request(int index){
    if(index<0){atomic_store(&pw_requested,0);atomic_store(&pw_visual_open,0);return;}
    if(index>=0&&index<PW_COUNT&&runtime_power_wheel_gameplay()&&atomic_load(&pw_ready)&&!atomic_load(&pw_error))
        atomic_store(&pw_requested,index+1);
}
extern int runtime_mouse_aim_camera_live(void);
static atomic_int pw_cutscene,pw_cutscene_reports;
/* Diagnostic (ISS_CUTSCENE_SHOT=1): game-only screenshots 1.5 s and 4 s into the first 10 cutscenes. */
#ifdef _WIN32
extern void tlggpu_request_screenshot(void);
#else
/* The Linux probe can also link against renderer 1, which has no screenshot hook. */
extern void tlggpu_request_screenshot(void) __attribute__((weak));
#endif
static atomic_llong pw_shot_start;static atomic_int pw_shot_cutscenes,pw_shot_taken;
static long long pw_now_ms(void){struct timespec t;timespec_get(&t,TIME_UTC);return (long long)t.tv_sec*1000+t.tv_nsec/1000000;}
static int pw_shots_enabled(void){static int on=-1;if(on<0){const char *e=getenv("ISS_CUTSCENE_SHOT");on=e&&e[0]=='1';}return on;}
static void pw_cutscene_shots_start(void){
#ifndef _WIN32
    if(!tlggpu_request_screenshot)return;
#endif
    if(!pw_shots_enabled()||atomic_fetch_add(&pw_shot_cutscenes,1)>=10)return;
    atomic_store(&pw_shot_taken,0);atomic_store(&pw_shot_start,pw_now_ms());
}
static void pw_cutscene_shots_tick(void){
    long long start=atomic_load(&pw_shot_start);if(!start)return;
    long long age=pw_now_ms()-start;int taken=atomic_load(&pw_shot_taken);
    if((taken==0&&age>=1500)||(taken==1&&age>=4000)){
        if(atomic_compare_exchange_strong(&pw_shot_taken,&taken,taken+1)){
            tlggpu_request_screenshot();printf("Power wheel: cutscene screenshot %d requested\n",taken+1);
            if(taken+1==2)atomic_store(&pw_shot_start,0);
        }
    }
}
int runtime_power_wheel_gameplay(void){
    /* The guest's writable segments are not mapped yet during renderer startup.
     * Only the camera-thread's validated hero/resource sample publishes ready. */
    int verified=atomic_load(&pw_supported)&&atomic_load(&pw_ready);
    uint32_t paused=verified?*(const volatile uint32_t*)(pw_image+PW_PAUSE_COUNTER):1;
    /* Cutscenes: the game stops updating the player camera (no hook: unchanged). */
    int cutscene=verified&&!paused&&runtime_mouse_aim_camera_live()==0;
    if(verified&&!paused&&atomic_exchange(&pw_cutscene,cutscene)!=cutscene){
        if(atomic_fetch_add(&pw_cutscene_reports,1)<400)
            puts(cutscene?"Power wheel: hidden, player camera stopped (cutscene)":"Power wheel: player camera running again");
        if(cutscene)pw_cutscene_shots_start();
    }
    if(cutscene)pw_cutscene_shots_tick();
    if(!pw_gameplay_allowed(verified,paused)||cutscene){
        atomic_store(&pw_requested,0);atomic_store(&pw_visual_open,0);return 0;
    }
    return 1;
}
int runtime_power_wheel_unpaused(void){
    int verified=atomic_load(&pw_supported)&&atomic_load(&pw_ready);
    return verified&&*(const volatile uint32_t*)(pw_image+PW_PAUSE_COUNTER)==0;
}
void runtime_power_wheel_status(IssPowerWheelStatus *out){
    *out=(IssPowerWheelStatus){atomic_load(&pw_supported),atomic_load(&pw_ready),
        atomic_load(&pw_active),atomic_load(&pw_mask),atomic_load(&pw_error),atomic_load(&pw_requested)};
}
void runtime_power_wheel_visual(int open,int choice){
    atomic_store(&pw_visual_choice,choice);atomic_store(&pw_visual_open,open&&runtime_power_wheel_gameplay());
}
void runtime_power_wheel_visual_status(int *open,int *choice){
    *open=runtime_power_wheel_gameplay()&&atomic_load(&pw_visual_open);*choice=atomic_load(&pw_visual_choice);
}
/* The existing guarded update bridge calls this independently of Karma state.
 * The overlay and input thread only enqueue; they never call guest code. */
static void pw_tick_impl(void){
    if(!atomic_load(&pw_supported)||!pw_image||pw_inside)return;
    unsigned generation=atomic_load(&pw_generation);
    uintptr_t hero=pw_pointer(pw_image+0x1d0cfe0);
    if(generation!=pw_seen_generation||hero!=pw_hero){
        pw_reset(&pw_bank,generation);pw_seen_generation=generation;pw_hero=hero;
        atomic_store(&pw_requested,0);atomic_store(&pw_error,0);
    }
    PwSample current;
    if(!pw_guest(hero)||!pw_read_game(NULL,&current)||!pw_sample_valid(&current)){
        atomic_store(&pw_ready,0);atomic_store(&pw_requested,0);return;
    }
    int active=pw_index(current.native),mask=0;
    for(int i=0;i<PW_COUNT;i++)if(pw_unlocked(current.unlocked[i]))mask|=1<<i;
    atomic_store(&pw_active,active);atomic_store(&pw_mask,mask);
    atomic_store(&pw_ready,!atomic_load(&pw_error));
    if(!runtime_power_wheel_gameplay())return;
    if(atomic_load(&pw_error))return;
    /* Natural use, drains and native source switches update only the active bank. */
    pw_capture(&pw_bank,active,current.energy,current.maximum,current.missiles);
    if(tlggpu_overlay_captures_input())return;
    int request=atomic_exchange(&pw_requested,0);
    if(!request)return;
    if(generation!=atomic_load(&pw_generation))return;
    PwGame game={NULL,pw_read_game,pw_change_game,pw_write_game};
    pw_inside=1;int result=pw_switch(&pw_bank,&game,request-1);pw_inside=0;
    printf("Power wheel: request=%d result=%d\n",request-1,result);
    if(result<0&&result!=PW_SWITCH_LOCKED){atomic_store(&pw_error,result);atomic_store(&pw_ready,0);}
}
void runtime_power_wheel_tick(void){
    if(pw_inside||atomic_flag_test_and_set(&pw_working))return;
    pw_tick_impl();
    atomic_flag_clear(&pw_working);
}
