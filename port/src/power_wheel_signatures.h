#ifndef ISS_POWER_WHEEL_SIGNATURES_H
#define ISS_POWER_WHEEL_SIGNATURES_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
/* hash: the original code; alt (0 = none): the same code with a known mod applied. */
typedef struct { uint32_t offset, length, hash, alt; } PwCodeCheck;
static const PwCodeCheck pw_code_checks[]={
    {0x2143d0,0x158,0x48eb0971u,0},
    {0x2064e0,0xcf,0x5473c4e4u,0},
    /* alt: the Unlimited missiles mod (Weysincha's GoldHEN cheat), see pw_missile_cheat. */
    {0x1e35a0,0x4c,0x8b83547eu,0x229f7ef7u},
    {0x1e35f0,0x7,0x87731340u,0},
    {0x1e3600,0x4c,0xa30b846eu,0},
};
/* The Unlimited missiles mod: the missile-count setter (0x1e35a0) stores 5 whatever it is given
 * (mov esi, 5 / mov [rdi+0xdc], esi at 0x1e35a8). */
static inline int pw_missile_cheat(const unsigned char *image,size_t size){
    static const unsigned char cheat[13]={0xbe,0x05,0x00,0x00,0x00,0x89,0xb7,0xdc,0x00,0x00,0x00,0x90,0x90};
    return image && size>0x1e35a8+sizeof(cheat) && !memcmp(image+0x1e35a8,cheat,sizeof(cheat));
}
static inline int pw_signatures(const unsigned char *image,size_t size){
    if(!image)return 0;
    for(size_t i=0;i<sizeof(pw_code_checks)/sizeof(pw_code_checks[0]);i++){
        const PwCodeCheck *p=&pw_code_checks[i];
        if(p->offset>size || p->length>size-p->offset)return 0;
        uint32_t hash=2166136261u;
        for(uint32_t j=0;j<p->length;j++)hash=(hash^image[p->offset+j])*16777619u;
        if(hash!=p->hash && (!p->alt || hash!=p->alt))return 0;
    }
    return 1;
}
#endif
