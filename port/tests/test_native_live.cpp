// SPDX-License-Identifier: GPL-2.0-or-later
#undef NDEBUG
#include "../src/iss_native_live.h"
#include "../src/iss_karma.h"
#include <cassert>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <atomic>
#ifdef _WIN32
#include <windows.h>
#endif
static unsigned calls;static uintptr_t object;static int observed_frame_index;
// Player camera hook absent: the cutscene framing never applies here (it calls game code).
extern "C" int runtime_mouse_aim_camera_live(void){return -1;}
extern "C" unsigned runtime_mouse_aim_camera_calls(void){return 0;}
static void __attribute__((ms_abi)) observe(const void* p,int frame,uint64_t){calls++;object=(uintptr_t)p;observed_frame_index=frame;}
static void __attribute__((ms_abi)) observeObject(const void* p){object=(uintptr_t)p;calls++;}
int main(){
 assert(iss_native_live_option(nullptr)&&iss_native_live_option("1"));
 assert(!iss_native_live_option("0")&&!iss_native_live_option("")&&!iss_native_live_option("bad"));
 std::vector<unsigned char> image(0x580500);
 memcpy(image.data()+ISS_NATIVE_FRAME_SITE,iss_native_frame_signature,sizeof(iss_native_frame_signature));
 memcpy(image.data()+ISS_NATIVE_TARGET_SITE,iss_native_target_signature,sizeof(iss_native_target_signature));
 memcpy(image.data()+ISS_NATIVE_SAFE_SITE,iss_native_safe_signature,sizeof(iss_native_safe_signature));
 memcpy(image.data()+ISS_NATIVE_DIM_SITE,iss_native_dim_signature,sizeof(iss_native_dim_signature));
 assert(iss_native_live_signature(image.data(),image.size(),"CUSA00223","01.07"));
 for(auto site:{ISS_NATIVE_FRAME_SITE,ISS_NATIVE_TARGET_SITE,ISS_NATIVE_SAFE_SITE,ISS_NATIVE_DIM_SITE}){
  unsigned size=site==ISS_NATIVE_FRAME_SITE?ISS_NATIVE_FRAME_STOLEN:site==ISS_NATIVE_TARGET_SITE?ISS_NATIVE_TARGET_STOLEN:site==ISS_NATIVE_SAFE_SITE?ISS_NATIVE_SAFE_STOLEN:ISS_NATIVE_DIM_STOLEN;
  for(unsigned i=0;i<size;i++){image[site+i]^=1;assert(!iss_native_live_signature(image.data(),image.size(),"CUSA00223","01.07"));image[site+i]^=1;}
 }
 assert(!iss_native_live_signature(image.data(),image.size(),"CUSA00000","01.07"));
 assert(!iss_native_live_signature(image.data(),image.size(),"CUSA00223","01.00"));
 assert(!iss_native_live_signature(image.data(),ISS_NATIVE_FRAME_SITE+19,"CUSA00223","01.07"));
 assert(!iss_native_live_signature(image.data(),ISS_NATIVE_SAFE_SITE+14,"CUSA00223","01.07"));
 // Only the verified object and an explicit queued request can invalidate
 // its mode cache. A complete game-owned rebuild must be observed to finish.
 std::vector<unsigned char> renderer(0xdf0f09);auto* p=renderer.data();
 uint32_t width=3440,height=1440;memcpy(p,&width,4);memcpy(p+4,&height,4);memcpy(p+12,&width,4);memcpy(p+16,&height,4);
 uintptr_t base=(uintptr_t)p-0xf1bcd0;runtime_native_live_attach(base);
 IssNativePlan initial;assert(iss_native_plan({3,100,0,1},&initial));runtime_native_publish(1,"test",&initial);
#ifdef _WIN32
 _putenv_s("ISS_NATIVE_LIVE_REBUILD_TEST","0");assert(!runtime_native_live_rebuild_request());
 _putenv_s("ISS_NATIVE_LIVE_REBUILD_TEST","1");
#else
 setenv("ISS_NATIVE_LIVE_REBUILD_TEST","0",1);assert(!runtime_native_live_rebuild_request());
 setenv("ISS_NATIVE_LIVE_REBUILD_TEST","1",1);
#endif
 assert(runtime_native_live_rebuild_request()&&!runtime_native_live_rebuild_request());
 runtime_native_safe_resize(p+1);assert(p[0xdf0f08]==0);
 runtime_native_safe_resize(p);assert(p[0xdf0f08]==2&&runtime_native_live_rebuild_state()==2);
 p[0xdf0f08]=0;runtime_native_target_observer((void*)(base+0xe682b0),0);runtime_native_safe_resize(p);assert(runtime_native_live_rebuild_state()==3);
 assert(!memcmp(p,&width,4)&&!memcmp(p+4,&height,4));
 assert(runtime_native_live_rebuild_request());runtime_native_safe_resize(p);
 for(int i=0;i<4;i++)runtime_native_frame_observer(p,0);
 runtime_native_safe_resize(p);assert(runtime_native_live_rebuild_state()==-1&&p[0xdf0f08]==0);
 IssNativeSite sites[ISS_NATIVE_SITE_COUNT];iss_native_changes(&initial,sites);
 for(unsigned i=16;i<18;i++)memcpy(image.data()+sites[i].offset,sites[i].after,7);
 memcpy(image.data()+0x58030c+7,iss_native_projection+7,sizeof(iss_native_projection)-7);
 auto before=image;assert(!iss_native_live_projection_redirect(image.data(),image.size(),&initial,UINTPTR_MAX));assert(image==before);
 image[0x58030c+9]^=1;before=image;assert(!iss_native_live_projection_redirect(image.data(),image.size(),&initial,(uintptr_t)image.data()+0x580400));assert(image==before);image[0x58030c+9]^=1;
 assert(iss_native_live_projection_redirect(image.data(),image.size(),&initial,(uintptr_t)image.data()+0x580400));
 alignas(8) uint32_t cells[2]{};runtime_native_live_enable(cells,&initial);assert(runtime_native_live_enabled());
 uint32_t capacity_w,capacity_h,capacity_lights;
 assert(runtime_native_live_capacity(&capacity_w,&capacity_h,&capacity_lights));
 assert(capacity_w==3440&&capacity_h==1440&&capacity_lights==initial.light_entries);
 uint32_t purge[4]{};assert(runtime_native_live_purge_plan(purge)==0);
 assert(!runtime_native_live_request_config({2,100,0,1}));
 assert(runtime_native_live_request_config({3,75,0,1}));
 // A queued same-shape request is already covered below; complete it before
 // testing the independently guarded cross-aspect path.
 runtime_native_safe_resize(p);p[0xdf0f08]=0;
 uint32_t scaled_w=2580,scaled_h=1080;
 memcpy(p,&scaled_w,4);memcpy(p+4,&scaled_h,4);memcpy(p+12,&scaled_w,4);memcpy(p+16,&scaled_h,4);
 runtime_native_target_observer((void*)(base+0xe682b0),0);runtime_native_safe_resize(p);
 runtime_native_publish(1,"test",&initial);memcpy(p,&width,4);memcpy(p+4,&height,4);memcpy(p+12,&width,4);memcpy(p+16,&height,4);
 runtime_native_live_aspect_enable();
 assert(!runtime_native_live_request_config({2,100,0,1}));
 // An enabled hook alone is insufficient: require a verified active camera.
 std::vector<unsigned char> camera_image(0x7f2d2b8u+8),camera(0x700);
 uintptr_t camera_base=(uintptr_t)camera_image.data(),camera_pointer=(uintptr_t)camera.data();
 memcpy(camera_image.data()+0x7f2d2b8u,&camera_pointer,8);
 uintptr_t vtable=camera_base+0xd77e80u,projection=camera_base+0x581c90u,frustum=camera_base+0x5821b0u;
 memcpy(camera.data(),&vtable,8);memcpy((void*)(vtable+0x120),&projection,8);memcpy((void*)(vtable+0x128),&frustum,8);
 float camera_aspect=3440.f/1440.f,tan_y=0.5f,tan_x=tan_y*camera_aspect;
 memcpy(camera.data()+0x59c,&camera_aspect,4);memcpy(camera.data()+0x5a0,&tan_x,4);memcpy(camera.data()+0x5a4,&tan_y,4);
 runtime_native_live_attach(camera_base);runtime_native_live_aspect_camera(camera.data(),0);runtime_native_live_attach(base);
 assert(!runtime_native_live_request_config({4,100,0,1}));assert(!runtime_native_live_request_config({0,100,0,1}));
 // A narrower but taller surface can fit the pixel budget while exceeding
 // the original height. Likewise a wider/shorter scene exceeds width.
 assert(!runtime_native_live_request_config({4,75,0,1}));
 assert(!runtime_native_live_request_config({5,70,0,1}));
 assert(runtime_native_live_request_config({2,100,0,1}));assert(!runtime_native_live_request_config({2,75,0,1}));
 runtime_native_safe_resize(p);assert(runtime_native_live_rebuild_state()==2);
 uint64_t saved[16];for(unsigned i=0;i<16;i++)saved[i]=1000+i;saved[6]=1;
 auto original_saved=std::vector<uint64_t>(saved,saved+16);runtime_native_dimension_resize(p,saved);
 assert(saved[11]==2560&&saved[13]==1440&&saved[14]==2560&&saved[12]==1440&&saved[9]==2560&&saved[8]==1440);
 for(unsigned i:{0u,1u,2u,3u,4u,5u,6u,7u,10u,15u})assert(saved[i]==original_saved[i]);
 // Emulate only the four original stores; no invented resource descriptors.
 for(unsigned i=0;i<4;i++){uint32_t value=(uint32_t)saved[i==0?11:i==1?13:i==2?14:12];memcpy(p+(i<2?i*4:i*4+4),&value,4);}
 p[0xdf0f08]=0;runtime_native_target_observer((void*)(base+0xe682b0),0);runtime_native_safe_resize(p);
 assert(runtime_native_live_rebuild_state()==3&&cells[0]==2560&&cells[1]==1440&&runtime_native_plan()->width==2560);
 assert(runtime_native_live_purge_plan(purge)==2);
 assert(purge[0]==3440&&purge[1]==1440&&purge[2]==2560&&purge[3]==1440);
#ifdef _WIN32
 auto* code=(unsigned char*)VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);assert(code);
 for(int type=0;type<2;type++){
  auto sig=type?iss_native_target_signature:iss_native_frame_signature;
  unsigned stolen=type?ISS_NATIVE_TARGET_STOLEN:ISS_NATIVE_FRAME_STOLEN;
  unsigned n=iss_karma_bridge_ex(code,(uintptr_t)observe,sig,(uintptr_t)code+2048,stolen,1);assert(n<2048);
  const unsigned char frame_end[]={0x48,0x81,0xc4,0x88,0,0,0,0x5b,0x41,0x5c,0x41,0x5d,0x41,0x5e,0x41,0x5f,0x5d,0xc3};
  const unsigned char target_end[]={0x5b,0x41,0x5e,0x5d,0xc3};
  memcpy(code+2048,type?target_end:frame_end,type?sizeof(target_end):sizeof(frame_end));
  DWORD old;assert(VirtualProtect(code,4096,PAGE_EXECUTE_READ,&old));FlushInstructionCache(GetCurrentProcess(),code,4096);
  using Fn=void(__attribute__((sysv_abi)) *)(void*,int,uint64_t);
  ((Fn)code)((void*)0x12345678,42,0);assert(object==0x12345678&&observed_frame_index==42&&calls==unsigned(type+1));
  assert(VirtualProtect(code,4096,PAGE_READWRITE,&old));
 }
 // New safe-point bridge passes the original R13, without disturbing callers.
 iss_karma_bridge_ex(code,(uintptr_t)observeObject,iss_native_frame_signature,(uintptr_t)code+2048,ISS_NATIVE_FRAME_STOLEN,5);
 const unsigned char end[]={0x48,0x81,0xc4,0x88,0,0,0,0x5b,0x41,0x5c,0x41,0x5d,0x41,0x5e,0x41,0x5f,0x5d,0xc3};memcpy(code+2048,end,sizeof(end));
 const unsigned char wrapper[]={0x41,0x55,0x49,0x89,0xfd,0x48,0xb8};memcpy(code+3000,wrapper,sizeof(wrapper));
 uintptr_t destination=(uintptr_t)code;memcpy(code+3000+sizeof(wrapper),&destination,8);
 const unsigned char tail[]={0xff,0xd0,0x41,0x5d,0xc3};memcpy(code+3000+sizeof(wrapper)+8,tail,sizeof(tail));
 DWORD old;assert(VirtualProtect(code,4096,PAGE_EXECUTE_READ,&old));FlushInstructionCache(GetCurrentProcess(),code,4096);
 using ObjFn=void(__attribute__((sysv_abi)) *)(void*);((ObjFn)(code+3000))((void*)0xabcdef);assert(object==0xabcdef&&calls==3);
 // Execute the actual mid-function dimension stores with editable saved GPRs.
 assert(runtime_native_live_request_config({3,100,0,1}));runtime_native_safe_resize(p);
 assert(VirtualProtect(code,4096,PAGE_READWRITE,&old));
 iss_karma_bridge_ex(code,(uintptr_t)runtime_native_dimension_resize,iss_native_dim_signature,(uintptr_t)code+2048,ISS_NATIVE_DIM_STOLEN,6);code[2048]=0xc3;
 const unsigned char dim_wrapper[]={0x53,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x49,0x89,0xfd,0x41,0xb9,1,0,0,0,0x48,0xb8};
 memcpy(code+3000,dim_wrapper,sizeof(dim_wrapper));memcpy(code+3000+sizeof(dim_wrapper),&destination,8);
 const unsigned char dim_tail[]={0xff,0xd0,0x41,0x5f,0x41,0x5e,0x41,0x5d,0x41,0x5c,0x5b,0xc3};memcpy(code+3000+sizeof(dim_wrapper)+8,dim_tail,sizeof(dim_tail));
 assert(VirtualProtect(code,4096,PAGE_EXECUTE_READ,&old));FlushInstructionCache(GetCurrentProcess(),code,4096);
 using DimFn=uint64_t(__attribute__((sysv_abi)) *)(void*);assert(((DimFn)(code+3000))(p)==3440);
 uint32_t stored;memcpy(&stored,p,4);assert(stored==3440);p[0xdf0f08]=0;
 runtime_native_target_observer((void*)(base+0xe682b0),0);runtime_native_safe_resize(p);assert(cells[0]==3440&&runtime_native_live_rebuild_state()==3);
 VirtualFree(code,0,MEM_RELEASE);
#endif
 // Failed confirmation queues a game-owned rollback rather than publishing
 // unverified dimensions. Emulate its target recreation and confirm recovery.
 auto old_plan=*runtime_native_plan();assert(runtime_native_live_request_config({1,100,0,1}));runtime_native_safe_resize(p);
 runtime_native_dimension_resize(p,saved);
 for(unsigned i=0;i<4;i++){uint32_t v=(uint32_t)saved[i==0?11:i==1?13:i==2?14:12];memcpy(p+(i<2?i*4:i*4+4),&v,4);}
 for(int i=0;i<4;i++)runtime_native_frame_observer(p,0);
 runtime_native_safe_resize(p);assert(runtime_native_live_rebuild_state()==1);runtime_native_safe_resize(p);runtime_native_dimension_resize(p,saved);
 for(unsigned i=0;i<4;i++){uint32_t v=(uint32_t)saved[i==0?11:i==1?13:i==2?14:12];memcpy(p+(i<2?i*4:i*4+4),&v,4);}
 p[0xdf0f08]=0;runtime_native_target_observer((void*)(base+0xe682b0),0);runtime_native_safe_resize(p);
 assert(runtime_native_live_rebuild_state()==-1&&runtime_native_plan()->width==old_plan.width&&cells[0]==old_plan.width);
 IssNativePlan coherent{};coherent.width=100;coherent.height=200;runtime_native_publish(1,"test",&coherent);
 std::atomic<bool> done=false;std::thread writer([&]{for(unsigned i=1;i<2000;i++){auto next=coherent;next.width=i;next.height=i*2;runtime_native_publish(1,"test",&next);}done=true;});
 while(!done){auto snapshot=*runtime_native_plan();assert(snapshot.height==snapshot.width*2);}
 writer.join();
 // Exact user report: a run allocated at 2560x1440 cannot apply 3440x1440
 // at 100%. Refusal must leave the active plan untouched, even with the
 // projection refresh installed. An ultrawide scene must fit both axes too.
 IssNativePlan smaller;assert(iss_native_plan({2,100,0,1},&smaller));
 runtime_native_publish(1,"test",&smaller);runtime_native_live_enable(cells,&smaller);
 assert(!runtime_native_live_request_config({3,100,0,1}));
 assert(strstr(runtime_native_live_reason(),"Larger render allocation"));
 assert(runtime_native_plan()->width==2560 && runtime_native_plan()->height==1440);
 assert(!runtime_native_live_request_config({3,75,0,1})); // 2580 exceeds 2560
 assert(runtime_native_live_request_config({3,74,0,1})); // 2546 x 1066 fits
 // Wide UI canvas (2026-11-07): live resize is available, the HUD placement applies at once without a
 // rebuild, lower resolutions queue the normal rebuild, the UI canvas keeps its startup size, and
 // switching the UI layout itself still needs a restart.
 runtime_native_safe_resize(p);assert(runtime_native_live_rebuild_state()==2);
 runtime_native_dimension_resize(p,saved);
 for(unsigned i=0;i<4;i++){uint32_t v=(uint32_t)saved[i==0?11:i==1?13:i==2?14:12];memcpy(p+(i<2?i*4:i*4+4),&v,4);}
 p[0xdf0f08]=0;runtime_native_target_observer((void*)(base+0xe682b0),0);runtime_native_safe_resize(p);
 assert(runtime_native_live_rebuild_state()==3);
 IssNativePlan wide;assert(iss_native_plan({3,100,0,0},&wide)&&wide.ui_width==2580&&wide.ui_height==1080);
 runtime_native_publish(1,"test",&wide);runtime_native_live_enable(cells,&wide);assert(runtime_native_live_enabled());
 const int state_before=runtime_native_live_rebuild_state();
 assert(runtime_native_live_request_config({3,100,1,0}));
 assert(runtime_native_plan()->hud_edges==1&&runtime_native_plan()->width==3440&&runtime_native_live_rebuild_state()==state_before);
 assert(strstr(runtime_native_live_reason(),"HUD placement"));
 assert(!runtime_native_live_request_config({3,100,1,1})&&strstr(runtime_native_live_reason(),"UI layout"));
 assert(runtime_native_live_request_config({3,75,1,0})&&runtime_native_live_rebuild_state()==1);
 puts("Wide UI canvas: live HUD placement, live lower resolution, fixed UI canvas, layout change refused");
 puts("Native observer signatures, all-byte mismatch refusal, original prologues and executed ABI bridges passed");
}
