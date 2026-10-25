#include "iss_hud_trace.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
static uintptr_t image_base;
static const char *trigger;
static atomic_flag trace_lock=ATOMIC_FLAG_INIT;
static atomic_uint calls;
static uintptr_t seen[512];
static unsigned seen_count,epoch;
void runtime_hud_trace_attach(uintptr_t base){image_base=base;trigger=getenv("ISS_HUD_LAYOUT_TRACE_TRIGGER");}
static uint64_t read64(const unsigned char *p){uint64_t v;memcpy(&v,p,8);return v;}
static uint32_t read32(const unsigned char *p){uint32_t v;memcpy(&v,p,4);return v;}
static float read_float(const unsigned char *p){float v;memcpy(&v,p,4);return v;}
void __attribute__((ms_abi)) runtime_hud_trace_node(const void *widget,const void *matrix,uintptr_t caller){
    if(!trigger || !*trigger || atomic_flag_test_and_set_explicit(&trace_lock,memory_order_acquire))return;
    unsigned call=atomic_fetch_add_explicit(&calls,1,memory_order_relaxed);
    if(!(call&1023u)){
        FILE *file=fopen(trigger,"rb");
        if(file){
            char label[32]={0};size_t n=fread(label,1,31,file);fclose(file);
            for(size_t i=0;i<n;i++)if(label[i]<32 || label[i]>126)label[i]='_';
            if(!remove(trigger)){seen_count=0;++epoch;fprintf(stderr,"HUD_LAYOUT epoch=%u label=%s budget=512 unique widgets; observer only\n",epoch,label);}
        }
    }
    if(!epoch || seen_count==512)goto done;
    for(unsigned i=0;i<seen_count;i++)if(seen[i]==(uintptr_t)widget)goto done;
    seen[seen_count++]=(uintptr_t)widget;
    const unsigned char *p=widget;
    fprintf(stderr,"HUD_NODE epoch=%u node=0x%llx vt=0x%llx owner=0x%llx matrix=0x%llx caller=+0x%llx size=%ux%u flags=0x%x scale=%.6g pos=%.6g,%.6g attr=0x%llx,0x%llx\n",
        epoch,(unsigned long long)(uintptr_t)p,(unsigned long long)read64(p),
        (unsigned long long)read64(p+0x38),(unsigned long long)(uintptr_t)matrix,
        (unsigned long long)(caller-image_base),read32(p+0x4ec),read32(p+0x4f0),read32(p+0x4f4),
        (double)read_float(p+0x4c0),(double)read_float(p+0x120),(double)read_float(p+0x124),
        (unsigned long long)read64(p+8),(unsigned long long)read64(p+16));
done:
    atomic_flag_clear_explicit(&trace_lock,memory_order_release);
}
