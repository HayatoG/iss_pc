#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <vector>
#include "../src/iss_camera_hook.h"

extern "C" __attribute__((naked,sysv_abi)) void run_stub(void *, void *, float *, float, float) {
    __asm__ volatile(
        "push %r14\n push %rbx\n sub $0x208,%rsp\n"
        "mov %rdx,%rbx\n mov %rsi,%r14\n xor %eax,%eax\n"
        "vmovss %xmm1,0x28(%rsp)\n call *%rdi\n"
        "vmovss 0xfc(%rsp),%xmm0\n vmovss %xmm0,(%rbx)\n"
        "vmovss 0x100(%rsp),%xmm0\n vmovss %xmm0,4(%rbx)\n"
        "add $0x208,%rsp\n pop %rbx\n pop %r14\n ret\n");
}
int main(int argc,char **argv) {
    assert(iss_camera_title_supported("CUSA00223","01.07"));
    assert(iss_camera_title_supported("CUSA00004","01.07"));
    assert(iss_camera_title_supported("CUSA00004","01.00"));
    assert(!iss_camera_title_supported("CUSA00223","01.00"));
    assert(!iss_camera_title_supported("CUSA99999","01.07"));
    assert(!iss_camera_title_supported("CUSA00004","01.06"));
    float scale;
    for (const char *s : {"nan","inf","0","-1","4.01","1.5junk","1e99"})
        assert(!iss_camera_scale(s,&scale));
    assert(iss_camera_scale(nullptr,&scale) && scale==1);
    assert(iss_camera_scale("1.5",&scale) && scale==1.5f);
    assert(argc==2);
    FILE *f=fopen(argv[1],"rb"); assert(f);
    std::vector<unsigned char> elf(17075867); assert(fread(elf.data(),1,elf.size(),f)==elf.size()); fclose(f);
    std::vector<unsigned char> img(0x14dc50+16);
    memcpy(img.data(),elf.data()+0x4000,img.size());
    assert(iss_camera_signature_match(img.data(),img.size()));
    img[ISS_CAMERA_SITE]^=1; assert(!iss_camera_signature_match(img.data(),img.size()));
    img[ISS_CAMERA_SITE]^=1; assert(!iss_camera_signature_match(img.data(),ISS_CAMERA_SITE+33));
    float cfg[0x500/4]={}; cfg[0x40c/4]=1.25f;
    for (auto pair : {std::pair<float,float>{1,1},{1.5f,1.5f},{2,1},{1,2}}) {
        unsigned char *code=(unsigned char*)VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE); assert(code);
        code[256]=0xc3;
        assert(iss_camera_build_stub(code,(uintptr_t)(code+256),pair.first,pair.second)==ISS_CAMERA_STUB_SIZE);
        DWORD old; assert(VirtualProtect(code,4096,PAGE_EXECUTE_READ,&old));
        FlushInstructionCache(GetCurrentProcess(),code,4096);
        float out[2]={}; run_stub(code,cfg,out,1.4f,0.6f);
        assert(fabsf(out[0]-1.4f*pair.first)<1e-6f);
        assert(fabsf(out[1]-0.75f*pair.second)<1e-6f);
        printf("PASS executed replay: horizontal %.3f, vertical %.3f -> %.3f %.3f\n",pair.first,pair.second,out[0],out[1]);
        VirtualFree(code,0,MEM_RELEASE);
    }
    puts("PASS rejected invalid scales, wrong signatures and truncated image; executed generated machine code.");
}
