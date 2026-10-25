#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <cassert>
#include <cstdio>
#include <vector>
#include <cmath>
#include "../src/iss_camera_pitch_hook.h"

extern "C" __attribute__((naked,sysv_abi)) void run_pitch_stub(void*,void*,void*,void*,float) {
    __asm__ volatile("push %rbp\n push %rbx\n push %r13\n sub $8,%rsp\n"
        "mov %rsi,%r13\n mov %rdx,%rbx\n mov %rcx,%rbp\n call *%rdi\n"
        "add $8,%rsp\n pop %r13\n pop %rbx\n pop %rbp\n ret\n");
}
static void put(void* p,int off,float v) { memcpy((unsigned char*)p+off,&v,4); }
int main(int argc,char**argv) {
    std::vector<unsigned char> image(0x14b100);
    if (argc==2) {
        FILE*f=fopen(argv[1],"rb");assert(f);fseek(f,0,SEEK_END);long len=ftell(f);rewind(f);
        std::vector<unsigned char> elf(len);assert(fread(elf.data(),1,len,f)==(size_t)len);fclose(f);
        assert(elf.size()>=image.size()+0x4000);
        memcpy(image.data(),elf.data()+0x4000,image.size());
    } else {
        // Synthetic instruction fixtures exercise guards without proprietary game files.
        memcpy(image.data()+ISS_PITCH_SITE,iss_pitch_original,16);
        const unsigned char projection[]={0xe8,0x45,0xe5,0x4c,0x00};
        const unsigned char product[]={0xc5,0xfa,0x59,0x9d,0xf0,0xfe,0xff,0xff};
        memcpy(image.data()+0x14ae46,projection,sizeof(projection));
        memcpy(image.data()+0x14aff1,product,sizeof(product));
    }
    assert(iss_pitch_signature(image.data(),image.size()));
    image[ISS_PITCH_SITE]^=1;assert(!iss_pitch_signature(image.data(),image.size()));image[ISS_PITCH_SITE]^=1;
    assert(!iss_pitch_signature(image.data(),ISS_PITCH_SITE+15));
    alignas(16) unsigned char state[0x800]={},cfg[0x800]={},frame_mem[0x800]={};
    void*frame=frame_mem+0x400;
    put(state,0x334,-1.4502f);put(state,0x360,0);put(state,0x4d4,.76f);
    *(uint32_t*)(state+0x470)=2;
    put(cfg,0x1c0,-1.45f);put(cfg,0x1c4,1.525f);put(cfg,0x334,.9f);
    put(frame,-0x1b0,.976f);put(frame,-0x190,.568f);
    put(frame,-0x210,-1.56f);put(frame,-0x260,0);
    const float cap=.16f;
    assert(iss_pitch_cap(cap,state,cfg,frame)==.76f);
    #ifdef _WIN32
    auto*code=(unsigned char*)VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);assert(code);
    #else
    auto*code=(unsigned char*)mmap(nullptr,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(code!=MAP_FAILED);
    #endif
    code[1024]=0xc3;
    unsigned n=iss_pitch_build_stub(code,(uintptr_t)&iss_pitch_cap,(uintptr_t)(code+1024));assert(n<=ISS_PITCH_MAX_STUB);
    #ifdef _WIN32
    DWORD old;assert(VirtualProtect(code,4096,PAGE_EXECUTE_READ,&old));FlushInstructionCache(GetCurrentProcess(),code,4096);
    #else
    assert(mprotect(code,4096,PROT_READ|PROT_EXEC)==0);
    #endif
    auto check=[&](float expected){put(frame,-0x110,-99);run_pitch_stub(code,state,cfg,frame,cap);assert(iss_pitch_f(frame,-0x110)==expected);};
    check(.76f);
    put(state,0x360,.5f);check(cap);put(state,0x360,0);
    put(state,0x334,0);check(cap);put(state,0x334,-1.4502f);
    put(frame,-0x190,0);check(cap);put(frame,-0x190,.568f);
    put(frame,-0x260,-.2f);check(cap);put(frame,-0x260,0);
    put(frame,-0x210,1.56f);check(cap);put(frame,-0x210,-1.56f);
    put(frame,-0x1b0,.1f);check(cap);put(frame,-0x1b0,.976f);
    put(frame,-0x190,-.568f);check(cap);
    put(state,0x334,1.525f);check(.76f);
    put(frame,-0x1b0,-.976f);put(frame,-0x210,1.56f);check(.76f);
    put(state,0x4d4,.10f);check(cap);put(state,0x4d4,.76f);
    put(state,0x360,NAN);check(cap);put(state,0x360,0);
    *(uint32_t*)(state+0x470)=0;check(cap);
    #ifdef _WIN32
    VirtualFree(code,0,MEM_RELEASE);
    #else
    munmap(code,4096);
    #endif
    puts("PASS: generated bridge executed; both pitch limits and pan signs; zoom, center, neutral, reversal, small input, moving pitch, invalid values and signature guards.");
}
