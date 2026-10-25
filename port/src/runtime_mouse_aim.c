#include "iss_mouse_aim.h"
#include <stdatomic.h>
#ifdef ISS_MOUSE_AIM_TEST_CLOCK
extern uint64_t mouse_aim_test_now(void);
#else
#include <SDL3/SDL.h>
static uint64_t mouse_aim_test_now(void){return SDL_GetTicksNS()/1000u;}
#endif
#include <stdio.h>
static atomic_uintptr_t attached;
static atomic_uint_fast64_t pending,last_camera,last_motion;
static unsigned char *no_follow;
static unsigned char follow_before;
static int follow_owned;
static atomic_uint motion_reports;
static unsigned camera_reports;
static float read_float(const void *p,unsigned offset){float v;memcpy(&v,(const char*)p+offset,4);return v;}
static void release_follow(void){
    if(follow_owned){if(*no_follow==1)*no_follow=follow_before;follow_owned=0;}
}
void runtime_mouse_aim_attach(uintptr_t base){
    no_follow=(unsigned char*)(base+0xe16c40u);
    atomic_store(&attached,base);
}
int runtime_mouse_aim_installed(void){return atomic_load(&attached)!=0;}
/* Called by the existing pad sampler, including capture loss. No double
 * application through the mouse-to-stick path once this hook is installed. */
int runtime_mouse_aim_motion(float dx,float dy,float sensitivity,int invert_y,int active){
    if(!atomic_load(&attached))return 0;
    uint64_t now=mouse_aim_test_now(),last=atomic_load(&last_camera);
    if((dx!=0 || dy!=0) && atomic_fetch_add(&motion_reports,1)<8)
        fprintf(stderr,"Direct mouse input dx=%.3g dy=%.3g active=%d camera_age_us=%llu\n",
            (double)dx,(double)dy,active,(unsigned long long)(last?now-last:UINT64_MAX));
    if(!active || !last || now-last>250000u){
        atomic_store(&pending,0);atomic_store(&last_motion,0);return 1;
    }
    if(!isfinite(dx)||!isfinite(dy)||!isfinite(sensitivity))return 1;
    float k=iss_mouse_bound(sensitivity,0.05f,10.0f)*5.12f*0.017453292519943295f/100.0f;
    float yaw=-dx*k,pitch=dy*k*(invert_y?-1.0f:1.0f);
    if(yaw==0 && pitch==0)return 1;
    atomic_store(&last_motion,now);
    uint_fast64_t old=atomic_load(&pending),next;
    do{
        float f[2];memcpy(f,&old,8);
        f[0]=iss_mouse_bound(f[0]+yaw,-0.75f,0.75f);
        f[1]=iss_mouse_bound(f[1]+pitch,-0.75f,0.75f);
        uint64_t bits;memcpy(&bits,f,8);next=bits;
    }while(!atomic_compare_exchange_weak(&pending,&old,next));
    return 1;
}
void __attribute__((ms_abi)) runtime_mouse_aim_camera(float *angles,const void *data,const void *context){
    uint64_t now=mouse_aim_test_now();
    uint64_t previous=atomic_exchange(&last_camera,now);
    uint64_t motion=atomic_load(&last_motion);
    uint64_t bits=atomic_exchange(&pending,0);
    if(!previous)fputs("Direct mouse: verified camera hook is running\n",stderr);
    if(!motion || now-motion>5000000u)release_follow();
    if(!previous || now-previous>250000u || !motion || now-motion>250000u || !bits)return;
    if(!data || !context)return;
    float delta[2];memcpy(delta,&bits,8);
    float dl=read_float(data,0x1c0),du=read_float(data,0x1c4);
    float cl=read_float(context,0x1c0),cu=read_float(context,0x1c4);
    int applied=iss_mouse_apply(angles,delta[0],delta[1],dl,du,cl,cu);
    if(camera_reports++<8)fprintf(stderr,
        "Direct mouse consume applied=%d delta=%.5g,%.5g limits=%.5g,%.5g/%.5g,%.5g angles=%.5g,%.5g\n",
        applied,(double)delta[0],(double)delta[1],(double)dl,(double)du,(double)cl,(double)cu,
        (double)angles[0],(double)angles[1]);
    if(!applied)return;
    if(!follow_owned){follow_before=*no_follow;follow_owned=1;}
    *no_follow=1;
}
