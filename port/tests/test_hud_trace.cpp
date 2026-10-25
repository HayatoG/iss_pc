#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include "../src/iss_karma.h"
#include "../src/iss_hud_trace.h"
#include "../src/iss_hud_corners.h"
static const void *seen_widget,*seen_matrix;
static uintptr_t seen_caller;
static void __attribute__((ms_abi)) observe(const void *widget,const void *matrix,uintptr_t caller){
    seen_widget=widget;seen_matrix=matrix;seen_caller=caller;
}
int main(){
    std::vector<unsigned char> image(0x5651a4);
    memcpy(image.data()+ISS_HUD_TRACE_SITE,iss_hud_trace_entry,sizeof(iss_hud_trace_entry));
    const unsigned char scale[]={0xc5,0xfa,0x10,0xab,0xc0,4,0,0};
    const unsigned char layout[]={0x48,0x8b,0x8b,0xf0,4,0,0,0xc5,0xfa,0x2a,0x93,0xec,4,0,0,0xc5,0xfa,0x2a,0xc9,0x48,0xc1,0xe9,0x20};
    memcpy(image.data()+0x564f99,scale,sizeof(scale));memcpy(image.data()+0x564d49,layout,sizeof(layout));
    assert(iss_hud_trace_signature(image.data(),image.size()));
    for(unsigned i=0;i<sizeof(iss_hud_trace_entry);i++){
        image[ISS_HUD_TRACE_SITE+i]^=1;assert(!iss_hud_trace_signature(image.data(),image.size()));image[ISS_HUD_TRACE_SITE+i]^=1;
    }
    assert(!iss_hud_trace_signature(image.data(),16));
#ifdef _WIN32
    auto* code=(unsigned char*)VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
#else
    auto* code=(unsigned char*)mmap(nullptr,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(code!=MAP_FAILED);
#endif
    assert(code);
    // Replay the verified stack prologue and execute a synthetic balanced tail.
    const unsigned char tail[]={0x48,0x89,0xf8,0x48,0x81,0xc4,0x88,0,0,0,0x5b,0x41,0x5e,0x41,0x5f,0x5d,0xc3};
    memcpy(code+2048,tail,sizeof(tail));
    unsigned n=iss_karma_bridge_ex(code,(uintptr_t)observe,iss_hud_trace_entry,(uintptr_t)(code+2048),16,2);assert(n<2048);
#ifdef _WIN32
    DWORD old;assert(VirtualProtect(code,4096,PAGE_EXECUTE_READ,&old));FlushInstructionCache(GetCurrentProcess(),code,4096);
#else
    assert(!mprotect(code,4096,PROT_READ|PROT_EXEC));
#endif
    char widget[16]={0},matrix[64]={0};
    auto fn=(void*(__attribute__((sysv_abi)) *)(void*,void*))code;
    assert(fn(matrix,widget)==matrix);
    assert(seen_widget==widget&&seen_matrix==matrix&&seen_caller);
    for(char x:widget)assert(!x);
    for(char x:matrix)assert(!x);
#ifdef _WIN32
    VirtualFree(code,0,MEM_RELEASE);
#else
    munmap(code,4096);
#endif
    assert(iss_hud_corner_kind(0xd74668,0xcdb7d0,140,200,5)==1);
    assert(iss_hud_corner_kind(0xd74668,0xcdb7d0,424,280,9)==2);
    assert(iss_hud_corner_kind(0xd795d0,0,330,430,10)==3);
    assert(!iss_hud_corner_kind(0xd74668,0,140,200,5));
    assert(!iss_hud_corner_kind(0xd795d0,0,1920,1080,10));
    assert(!iss_hud_corner_kind(0xd75d98,0xcdb7d0,1220,600,10));
    for(unsigned width:{3440u,5120u})for(int kind:{1,2,3}){
        float m[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
        const float anchor=kind==3?960.0f:-960.0f;
        m[12]=anchor+(kind==3?-60.0f:60.0f);m[13]=kind==1?420.0f:-420.0f;
        float previous_y=m[13],old_distance=fabsf(m[12]-anchor);
        assert(iss_hud_corner_transform(m,width,1440,kind));
        assert(fabsf((float)width/1920*m[0]-1440.0f/1080)<0.0001f);
        assert(fabsf((float)width/1920*fabsf(m[12]-anchor)-1440.0f/1080*old_distance)<0.001f);
        assert(m[5]==1&&m[10]==1&&m[13]==previous_y);
    }
    float unchanged[16]={1,0,0,0,0,1,0,0,0,0,1,0,30,40,0,1},copy[16];memcpy(copy,unchanged,sizeof(copy));
    // Actual captured HUD matrix convention: unused final W=0.
    float game_matrix[16]={1,0,0,0,0,1,0,0,0,0,1,0,779,-309,0,0};
    assert(iss_hud_corner_transform(game_matrix,3440,1440,3));assert(game_matrix[15]==0&&game_matrix[13]==-309);
    assert(!iss_hud_corner_transform(unchanged,2560,1440,1));assert(!memcmp(copy,unchanged,sizeof(copy)));
    assert(!iss_hud_corner_transform(unchanged,3440,1440,0));assert(!memcmp(copy,unchanged,sizeof(copy)));
#ifdef _WIN32
    code=(unsigned char*)VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
#else
    code=(unsigned char*)mmap(nullptr,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(code!=MAP_FAILED);
#endif
    assert(code);
    // Enter with a real widget/matrix, then intercept the verified completed-layout tail.
    memcpy(code+2048,iss_hud_trace_entry,sizeof(iss_hud_trace_entry));
    const unsigned char setup[]={0x48,0x89,0xf3,0x49,0x89,0xfe};
    memcpy(code+2064,setup,sizeof(setup));iss_karma_jump(code+2070,(uintptr_t)code);
    n=iss_karma_bridge_ex(code,(uintptr_t)observe,iss_hud_corner_tail,(uintptr_t)(code+3000),sizeof(iss_hud_corner_tail),3);assert(n<2048);
#ifdef _WIN32
    assert(VirtualProtect(code,4096,PAGE_EXECUTE_READ,&old));FlushInstructionCache(GetCurrentProcess(),code,4096);
#else
    assert(!mprotect(code,4096,PROT_READ|PROT_EXEC));
#endif
    fn=(void*(__attribute__((sysv_abi)) *)(void*,void*))(code+2048);
    seen_widget=nullptr;seen_matrix=nullptr;
    assert(fn(matrix,widget)==matrix);assert(seen_widget==widget&&seen_matrix==matrix);
#ifdef _WIN32
    VirtualFree(code,0,MEM_RELEASE);
#else
    munmap(code,4096);
#endif
    puts("HUD observer: entry guards, bridge arguments, stack replay and no data writes passed.");
    puts("HUD corner candidate: exact root filtering, 16:9/21:9/32:9 geometry, stable corner margins and exit bridge passed.");
}
