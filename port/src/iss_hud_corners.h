/* Gameplay-only root transforms observed in CUSA00223 1.07. */
#ifndef ISS_HUD_CORNERS_H
#define ISS_HUD_CORNERS_H
#include <stdint.h>
#include <math.h>
#include <string.h>
#define ISS_HUD_CORNER_SITE 0x565193u
static const unsigned char iss_hud_corner_tail[]={0x4c,0x89,0xf0,0x48,0x81,0xc4,0x88,0,0,0,0x5b,0x41,0x5e,0x41,0x5f,0x5d,0xc3};
static inline int iss_hud_corner_kind(uintptr_t vt,uintptr_t owner_vt,uint32_t w,uint32_t h,uint32_t flags){
    if(vt==0xd74668u && owner_vt==0xcdb7d0u){
        if(w==140 && h==200 && flags==5)return 1;
        if(w==424 && h==280 && flags==9)return 2;
    }
    if(vt==0xd795d0u && !owner_vt && w==330 && h==430 && flags==10)return 3;
    return 0;
}
/* UI coordinates retain 1080 logical lines. Correct only the excess horizontal
 * output stretch, keeping the same 16:9 height scale and corner margin. */
static inline int iss_hud_corner_transform(float matrix[16],uint32_t width,uint32_t height,int kind){
    if(kind<1 || kind>3 || !width || !height || (uint64_t)width*9u<=(uint64_t)height*16u)return 0;
    float ratio=(16.0f*(float)height)/(9.0f*(float)width);
    if(!isfinite(ratio) || ratio<0.2f || ratio>=1.0f)return 0;
    for(unsigned i=0;i<16;i++)if(!isfinite(matrix[i]))return 0;
    // The observed game stores a 3x4 affine transform with an unused zero W;
    // synthetic/conventional affine fixtures may use W=1. Preserve either.
    if(fabsf(matrix[3])>0.0001f || fabsf(matrix[7])>0.0001f || fabsf(matrix[11])>0.0001f ||
       (fabsf(matrix[15])>0.0001f && fabsf(matrix[15]-1.0f)>0.0001f))return 0;
    const float anchor=kind==3?960.0f:-960.0f;
    matrix[0]*=ratio;matrix[4]*=ratio;matrix[8]*=ratio;
    matrix[12]=anchor+ratio*(matrix[12]-anchor);
    return 1;
}
void runtime_hud_corner_attach(uintptr_t image);
void __attribute__((ms_abi)) runtime_hud_corner_node(const void *widget,void *matrix);
#endif
