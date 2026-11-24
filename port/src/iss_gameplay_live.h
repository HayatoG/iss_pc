// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "iss_gameplay_specs.h"
#include <limits.h>
#include <string.h>
static inline int iss_mod_relative(unsigned char *out,uintptr_t next,uintptr_t target){
    int64_t delta=(int64_t)target-(int64_t)next;if(delta<INT32_MIN||delta>INT32_MAX)return 0;
    int32_t d=(int32_t)delta;memcpy(out,&d,4);return 1;
}
static inline int iss_mod_block(unsigned char *out,const unsigned char *code,unsigned length,const IssModFix *fix,unsigned count,uintptr_t base){
    memcpy(out,code,length);
    for(unsigned i=0;i<count;i++){
        if((unsigned)fix[i].pos+4u>length||(unsigned)fix[i].next>length||!iss_mod_relative(out+fix[i].pos,(uintptr_t)out+fix[i].next,base+fix[i].target))return 0;
    }
    return 1;
}
/* Preserve every register and FLAGS, including the SysV red zone. Only the
 * writable flag changes at run time; both alternatives are fixed RX code. */
static inline int iss_mod_gate(unsigned char *out,uintptr_t flag,uintptr_t off,uintptr_t on){
    static const unsigned char code[]={
        0x48,0x8d,0xa4,0x24,0x80,0xff,0xff,0xff,0x9c,
        0x80,0x3d,0,0,0,0,0,0x74,0x0e,
        0x9d,0x48,0x8d,0xa4,0x24,0x80,0,0,0,0xe9,0,0,0,0,
        0x9d,0x48,0x8d,0xa4,0x24,0x80,0,0,0,0xe9,0,0,0,0};
    memcpy(out,code,sizeof(code));
    return iss_mod_relative(out+11,(uintptr_t)out+16,flag)&&iss_mod_relative(out+28,(uintptr_t)out+32,on)&&iss_mod_relative(out+42,(uintptr_t)out+46,off);
}
