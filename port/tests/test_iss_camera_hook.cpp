#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif
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
    float scale;
    for (const char *s : {"nan","inf","0","-1","4.01","1.5junk","1e99"})
        assert(!iss_camera_scale(s,&scale));
    assert(iss_camera_scale(nullptr,&scale) && scale==1);
    assert(iss_camera_scale("1.5",&scale) && scale==1.5f);
    assert(argc==1 || argc==2);
    std::vector<unsigned char> img(0x14dc50+16);
    if (argc==2) {
        // Optional local audit against the researched ELF; never needed by CI.
        FILE *f=fopen(argv[1],"rb"); assert(f);
        std::vector<unsigned char> elf(17075867);
        assert(fread(elf.data(),1,elf.size(),f)==elf.size()); fclose(f);
        memcpy(img.data(),elf.data()+0x4000,img.size());
    } else {
        // Minimal instruction fixtures, not a game image. Exercise all four guards.
        const unsigned char entry[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xe4};
        const unsigned char demand[]={0xc4,0xc1,0x7a,0x59,0x54,0x24,0x04,0xc5,0xe2,0x59,0xc1,0xc4,0xc1,0x7a,0x59,0x44,0x24,0x08};
        const unsigned char angles[]={0xc5,0xfa,0x7f,0x00,0xc5,0xf8,0x28,0x84,0x24,0x80,0x00,0x00,0x00};
        memcpy(img.data()+0x14d6f0,entry,sizeof(entry));
        memcpy(img.data()+ISS_CAMERA_SITE,iss_camera_original,ISS_CAMERA_LENGTH);
        memcpy(img.data()+0x14b01e,demand,sizeof(demand));
        memcpy(img.data()+0x14dc43,angles,sizeof(angles));
    }
    assert(iss_camera_signature_match(img.data(),img.size()));
    img[ISS_CAMERA_SITE]^=1; assert(!iss_camera_signature_match(img.data(),img.size()));
    img[ISS_CAMERA_SITE]^=1; assert(!iss_camera_signature_match(img.data(),ISS_CAMERA_SITE+33));
    for (unsigned site : {0x14d6f0u,0x14b01eu,0x14dc43u}) {
        img[site]^=1; assert(!iss_camera_signature_match(img.data(),img.size())); img[site]^=1;
    }
    unsigned char jump[ISS_CAMERA_LENGTH];
    uintptr_t address=0x1234567887654321ull, decoded=0;
    iss_camera_build_jump(jump,address);
    assert(jump[0]==0xff && jump[1]==0x25);
    memcpy(&decoded,jump+6,8); assert(decoded==address);
    for (unsigned i=14;i<sizeof(jump);++i) assert(jump[i]==0x90);
    if (!__builtin_cpu_supports("avx")) {
        puts("SKIP executable replay: AVX unavailable; scale, jump and signature tests passed.");
        return 0;
    }
    float cfg[0x500/4]={}; cfg[0x40c/4]=1.25f;
    for (auto pair : {std::pair<float,float>{1,1},{1.5f,1.5f},{2,1},{1,2}}) {
#ifdef _WIN32
        unsigned char *code=(unsigned char*)VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE); assert(code);
#else
        unsigned char *code=(unsigned char*)mmap(nullptr,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        assert(code!=MAP_FAILED);
#endif
        code[256]=0xc3;
        assert(iss_camera_build_stub(code,(uintptr_t)(code+256),pair.first,pair.second)==ISS_CAMERA_STUB_SIZE);
#ifdef _WIN32
        DWORD old; assert(VirtualProtect(code,4096,PAGE_EXECUTE_READ,&old));
        FlushInstructionCache(GetCurrentProcess(),code,4096);
#else
        assert(mprotect(code,4096,PROT_READ|PROT_EXEC)==0);
        __builtin___clear_cache((char*)code,(char*)code+4096);
#endif
        float out[2]={}; run_stub(code,cfg,out,1.4f,0.6f);
        assert(fabsf(out[0]-1.4f*pair.first)<1e-6f);
        assert(fabsf(out[1]-0.75f*pair.second)<1e-6f);
        printf("PASS executed replay: horizontal %.3f, vertical %.3f -> %.3f %.3f\n",pair.first,pair.second,out[0],out[1]);
#ifdef _WIN32
        VirtualFree(code,0,MEM_RELEASE);
#else
        assert(munmap(code,4096)==0);
#endif
    }
    puts("PASS rejected invalid scales, wrong signatures and truncated image; executed generated machine code.");
}
