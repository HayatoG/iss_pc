#include <cassert>
#include <cstdio>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif
#include "../src/iss_karma.h"
#include "../src/iss_mouse_aim.h"
static uint64_t clock_us=1000000;
extern "C" uint64_t mouse_aim_test_now(void){return clock_us;}
static const void *bridge_data,*bridge_context;
static void __attribute__((ms_abi)) edit_angles(float *a,const void *data,const void *context){
    assert(data==bridge_data && context==bridge_context);
    a[0]+=0.25f;a[1]=0.5f;
}
int main(){
    std::vector<unsigned char> image(0xe16c41);
    memcpy(image.data()+0x14ebc5,iss_mouse_expected,sizeof(iss_mouse_expected));
    const unsigned char follow[]={0x48,0x8d,0x05,0xcb,0xd8,0xcc,0};
    memcpy(image.data()+0x14936e,follow,sizeof(follow));
    memcpy(image.data()+0x14dfb2,iss_mouse_data,sizeof(iss_mouse_data));
    memcpy(image.data()+0x14ea75,iss_mouse_context,sizeof(iss_mouse_context));
    memcpy(image.data()+0x14e38f,iss_mouse_limits,sizeof(iss_mouse_limits));
    assert(iss_mouse_signature(image.data(),image.size()));
    for(unsigned i=0;i<sizeof(iss_mouse_expected);i++){
        image[0x14ebc5+i]^=1;assert(!iss_mouse_signature(image.data(),image.size()));image[0x14ebc5+i]^=1;
    }
    image[0x14936e]^=1;assert(!iss_mouse_signature(image.data(),image.size()));
    assert(!iss_mouse_signature(image.data(),18));
    float a[4]={1,0,7,9};
    assert(iss_mouse_apply(a,0.1f,0.2f,-1,1,-0.5f,0.5f));
    assert(fabsf(a[0]-1.1f)<0.00001f && a[1]==0.2f && a[2]==7 && a[3]==9);
    assert(iss_mouse_apply(a,0,2,-1,1,-0.5f,0.5f));assert(a[1]==0.5f);
    a[1]=1.2f;assert(iss_mouse_apply(a,0,1,-1,1,-0.5f,0.5f));assert(a[1]==1.2f);
    float copy[4];memcpy(copy,a,sizeof(a));
    assert(!iss_mouse_apply(a,NAN,0,-1,1,-1,1));assert(!memcmp(copy,a,sizeof(a)));
    assert(!iss_mouse_apply(a,0,0,1,-1,-1,1));assert(!memcmp(copy,a,sizeof(a)));
    assert(!iss_mouse_apply(a,0,0,-1,-0.5f,0.5f,1));
    float live[4]={-2.2772f,0.025f,0,0};
    assert(iss_mouse_apply(live,0.1f,0.2f,-1.45f,1.525f,-10,10));
    assert(fabsf(live[0]+2.1772f)<0.00001f && fabsf(live[1]-0.225f)<0.00001f);
    assert(iss_mouse_apply(live,0,2,-1.45f,1.525f,-10,10));assert(live[1]==1.525f);
    assert(!iss_mouse_apply(live,0,0,-10,10,-10,10));
    // Equal total mouse distance must produce equal rotation at different rates.
    float slow[4]={0},fast[4]={0};
    for(int i=0;i<30;i++)assert(iss_mouse_apply(slow,0.01f,0,-1,1,-1,1));
    for(int i=0;i<120;i++)assert(iss_mouse_apply(fast,0.0025f,0,-1,1,-1,1));
    assert(fabsf(slow[0]-fast[0])<0.00001f);
    assert(!runtime_mouse_aim_motion(1,1,1,0,1)); // Hook absent: fallback.
    runtime_mouse_aim_attach((uintptr_t)image.data());
    unsigned char data[0x1c8]={0},ctx[0x1c8]={0};
    float limits[2]={-1,1};memcpy(data+0x1c0,limits,8);memcpy(ctx+0x1c0,limits,8);
    float angles[4]={0,0,7,9};
    runtime_mouse_aim_camera(angles,data,ctx);
    assert(runtime_mouse_aim_motion(100,100,1,0,1));clock_us+=16000;
    runtime_mouse_aim_camera(angles,data,ctx);
    const float turn=5.12f*0.017453292519943295f;
    assert(fabsf(angles[0]+turn)<0.00001f && fabsf(angles[1]-turn)<0.00001f);
    assert(angles[2]==7 && angles[3]==9 && image[0xe16c40]==1);
    float saved[4];memcpy(saved,angles,16);clock_us+=16000;runtime_mouse_aim_camera(angles,data,ctx);
    assert(!memcmp(saved,angles,16)); // Each delta consumed once.
    runtime_mouse_aim_motion(100,100,1,0,1);runtime_mouse_aim_motion(0,0,1,0,0);
    clock_us+=16000;runtime_mouse_aim_camera(angles,data,ctx);
    assert(!memcmp(saved,angles,16) && !image[0xe16c40]); // Capture loss clears pending/follow.
    runtime_mouse_aim_motion(100,100,1,0,1);clock_us+=1000000;
    runtime_mouse_aim_camera(angles,data,ctx);assert(!memcmp(saved,angles,16)); // Pause: no resumed jump.
    runtime_mouse_aim_motion(0,100,1,1,1);clock_us+=16000;runtime_mouse_aim_camera(angles,data,ctx);
    assert(fabsf(angles[1])<0.00001f); // Overlay invert setting.
    runtime_mouse_aim_motion(NAN,1,1,0,1);clock_us+=16000;runtime_mouse_aim_camera(angles,data,ctx);
    assert(isfinite(angles[0]) && isfinite(angles[1]));
    puts("Direct mouse: signature rejection, angle integration, pitch limits and frame-rate independence passed.");
    puts("Mailbox: fallback, sensitivity, single consumption, capture loss, pause expiry and invert passed.");
#ifdef _WIN32
    auto *code=(unsigned char*)VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);assert(code);
    const unsigned char harness[]={0x41,0x54,0x41,0x55,0x48,0x83,0xec,0x68,
        0x49,0x89,0xfd,0x49,0x89,0xf4,0x48,0x89,0x54,0x24,0x50,
        0xc4,0xc1,0x78,0x10,0x85,0x30,3,0,0,
        0xc4,0xc1,0x7a,0x6f,0x8d,0x40,3,0,0,0xff,0xe1};
    const unsigned char resume[]={0x48,0x83,0xc4,0x68,0x41,0x5d,0x41,0x5c,0xc3};
    memcpy(code+2048,harness,sizeof(harness));memcpy(code+3000,resume,sizeof(resume));
    unsigned n=iss_karma_bridge_ex(code,(uintptr_t)edit_angles,iss_mouse_expected+9,(uintptr_t)(code+3000),18,4);
    assert(n<2048);DWORD old;assert(VirtualProtect(code,4096,PAGE_EXECUTE_READ,&old));
    FlushInstructionCache(GetCurrentProcess(),code,4096);
    alignas(32) unsigned char state[0x350]={0};
    float input[8]={1,2,3,4,5,6,7,8};memcpy(state+0x330,input,32);
    bridge_data=data;bridge_context=ctx;
    auto fn=(void(__attribute__((sysv_abi)) *)(void*,const void*,const void*,void*))(code+2048);
    fn(state,data,ctx,code);
    float result[8];memcpy(result,state+0x330,32);
    assert(result[0]==1.25f && result[1]==0.5f);
    for(int i=2;i<8;i++)assert(result[i]==input[i]);
    VirtualFree(code,0,MEM_RELEASE);
    puts("Executed camera bridge: saved angles editable, context/data arguments valid, original stores and XMM1 preserved.");
#endif
}
