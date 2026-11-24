#include "../src/iss_menu_aspect.h"
#include "../src/iss_hud_corners.h"
#include <cassert>
#include <cstring>
#include <cstdio>
#include <initializer_list>
static IssMenuNode nodes[4];
static int reader(uintptr_t at,IssMenuNode*out,void*){if(at<1||at>4)return 0;*out=nodes[at-1];return 1;}
int main(){
 unsigned seen=0;
 assert(iss_menu_presentation_option(nullptr));assert(iss_menu_presentation_option("1"));
 assert(!iss_menu_presentation_option("0"));assert(!iss_menu_presentation_option(""));
 assert(!iss_menu_presentation_option("yes"));assert(!iss_menu_presentation_option("10"));
 assert(iss_menu_presentation_fit(5120,1440,1));
 assert(iss_menu_presentation_fit(3440,1440,1));
 assert(!iss_menu_presentation_fit(5120,1440,0));
 assert(!iss_menu_presentation_fit(2560,1440,1));
 assert(!iss_menu_presentation_fit(3840,2160,1));
 assert(!iss_menu_presentation_fit(0,1440,1));
 assert(!iss_menu_pause_gate(&seen,0,0,0));
 assert(!iss_menu_pause_gate(&seen,1,0,0));
 assert(!iss_menu_pause_gate(&seen,1,1,0)); // ready during loading isn't gameplay
 assert(!iss_menu_pause_gate(&seen,1,1,1)&&seen==1);
 assert(iss_menu_pause_gate(&seen,1,1,0)); // now a pause transition is eligible
 assert(!iss_menu_pause_gate(&seen,1,0,0)&&seen==0); // save-load boundary
 assert(!iss_menu_pause_gate(&seen,1,1,0));
 assert(!iss_menu_pause_gate(&seen,1,1,1));assert(iss_menu_pause_gate(&seen,1,1,0));
 nodes[0]={0xcdfc10,0,1870,1020,0};nodes[1]={0xd74668,1,0,0,0};nodes[2]={0xd795d0,2,0,0,0};
 assert(iss_menu_member(1,reader,nullptr));assert(iss_menu_member(3,reader,nullptr));
 auto original=nodes[0];for(unsigned f=0;f<4;f++){nodes[0]=original;if(f==0)nodes[0].vtable++;if(f==1)nodes[0].width++;if(f==2)nodes[0].height++;if(f==3)nodes[0].flags++;assert(!iss_menu_member(3,reader,nullptr));}nodes[0]=original;
 nodes[1].owner=3;assert(!iss_menu_member(3,reader,nullptr));nodes[1].owner=1;
 nodes[2].vtable=0xd76410;assert(!iss_menu_member(3,reader,nullptr));nodes[2].vtable=0xd795d0;
 nodes[2].owner=999;assert(!iss_menu_member(3,reader,nullptr));
 for(unsigned w:{3440u,5120u}){
  float m[16]={1,0,0,0,0,1,0,0,0,0,1,0,800,400,0,0};assert(iss_menu_transform(m,w,1440));
  assert(fabsf(m[0]*w/1920.0f-1440.0f/1080)<0.0001f);assert(m[5]==1&&m[13]==400&&m[15]==0);
  assert(fabsf(m[12]-800*m[0])<0.0001f);
 }
 float m[16]={1,0,0,0,0,1,0,0,0,0,1,0,800,400,0,1},copy[16];memcpy(copy,m,sizeof(m));
 assert(!iss_menu_transform(m,2560,1440));assert(!memcmp(copy,m,sizeof(m)));
 m[0]=NAN;memcpy(copy,m,sizeof(m));assert(!iss_menu_transform(m,5120,1440));assert(!memcmp(copy,m,sizeof(m)));
 assert(iss_hud_corner_kind(0xd74668,0xcdb7d0,140,200,5)==1);
 assert(!iss_hud_corner_kind(0xcdfc10,0,1870,1020,0));
 puts("Menu aspect: exact root, descendant filtering, cycles/unreadable/unknown rejection, centered 21:9/32:9 geometry and 16:9 no-op passed.");
}
