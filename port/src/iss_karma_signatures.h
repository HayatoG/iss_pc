#ifndef ISS_KARMA_SIGNATURES_H
#define ISS_KARMA_SIGNATURES_H
#include <stdint.h>
static inline uint32_t karma_hash(const unsigned char *p,unsigned n){uint32_t h=2166136261u;for(unsigned i=0;i<n;++i)h=(h^p[i])*16777619u;return h;}
typedef struct {uint32_t offset,length,expected;} IssKarmaCodeCheck;
static const IssKarmaCodeCheck iss_karma_code_checks[]={
    {0x14d6f0,0x11,0xbc9fa80e},{0x1e4040,0x491,0x634aa4bc},
    {0x1e2f10,0x1bd,0x0a1a1de6},{0x230780,0x6c,0xfaf95892},
    {0x1e2270,0x19d,0x10c4f299},{0x2e5b70,9,0xa3e5cb88},
    {0x231350,0x6f,0x65f2b8ce}
};
static inline int iss_karma_signatures(const unsigned char *image,uint64_t size){
    for(unsigned i=0;i<sizeof(iss_karma_code_checks)/sizeof(iss_karma_code_checks[0]);++i){
        const IssKarmaCodeCheck *c=&iss_karma_code_checks[i];
        if(c->offset>size || c->length>size-c->offset || karma_hash(image+c->offset,c->length)!=c->expected)return 0;
    }
    return 1;
}
#endif
