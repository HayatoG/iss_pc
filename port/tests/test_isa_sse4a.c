/* SSE4A EXTRQ/INSERTQ rewrite (isa_emulate_sse4a in src/isa_emulate.c, the SSE4A stubs and the int3
 * path of src/isa_rewrite.c) against the real instructions. On a host with SSE4A (AMD) the CPU is
 * the oracle: every field length and bit index, random operands, every immediate pair, and whole
 * rewritten sites run next to the original code with all GPRs, flags, YMM registers and the red zone
 * compared. Without SSE4A (Intel) the AMD results recorded below and the model stand in for it. */
#define _GNU_SOURCE   /* MAP_ANONYMOUS and REG_RIP under -std=c11 */
#include <assert.h>
#include <immintrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#include <sys/mman.h>
#include <ucontext.h>
#endif
#include "isa_rewrite.h"

enum { PAGE = 4096, SITE_AT = 0x100 };
static int native;                      /* host executes SSE4A */
static unsigned char *image, *stubs, *pristine, *jit;
static uint64_t rng = 0x9e3779b97f4a7c15ull;

static uint64_t next(void) {             /* xorshift64* */
    rng ^= rng >> 12;
    rng ^= rng << 25;
    rng ^= rng >> 27;
    return rng * 0x2545f4914f6cdd1dull;
}

/* Operands with structure: all ones, zero, single bits and random words, so fields at the edges show. */
static uint64_t operand(void) {
    switch (next() % 6) {
    case 0: return ~0ull;
    case 1: return 0;
    case 2: return 1ull << (next() % 64);
    default: return next();
    }
}

static IsaSite sse4a_site(uint8_t kind, uint8_t dst, uint8_t src, unsigned length, unsigned index) {
    IsaSite s;
    memset(&s, 0, sizeof s);
    s.kind = kind;
    s.dst = dst;
    s.src1 = kind == ISA_EXTRQ_IMM ? ISA_NO_REG : src;
    s.src2 = s.base = s.index = ISA_NO_REG;
    s.disp = (int64_t)(length | index << 8);
    return s;
}

/* The model: xmm[0..1] destination, xmm[2..3] source. */
static void model(uint8_t kind, uint64_t xmm[4], unsigned length, unsigned index) {
    IsaSite s = sse4a_site(kind, 0, 1, length, index);
    isa_emulate_sse4a(&s, xmm);
}

/* An independent reference, bit by bit (AMD APM vol. 4): a field of length (0 = 64) bits at index,
 * bits past 63 cut; the high qword zeroed as AMD hardware does. Register forms take the control
 * from the source (EXTRQ: low qword, INSERTQ: high qword). */
static void reference(uint8_t kind, uint64_t xmm[4], unsigned length, unsigned index) {
    if (kind == ISA_EXTRQ || kind == ISA_INSERTQ) {
        const uint64_t control = kind == ISA_EXTRQ ? xmm[2] : xmm[3];
        length = control & 0xff;
        index = control >> 8 & 0xff;
    }
    const unsigned bits = (length & 63) ? (length & 63) : 64;
    index &= 63;
    const int extract = kind == ISA_EXTRQ || kind == ISA_EXTRQ_IMM;
    uint64_t r = extract ? 0 : xmm[0];
    for (unsigned b = 0; b < bits && index + b < 64; b++) {
        if (extract) r |= (xmm[0] >> (index + b) & 1) << b;
        else r = (r & ~(1ull << (index + b))) | (xmm[2] >> b & 1) << (index + b);
    }
    xmm[0] = r;
    xmm[1] = 0;
}

/* ---- the real instructions ---- */

typedef void (ABI *Jit)(uint64_t *xmm);

#ifdef __SSE4A__
static void native_reg(uint8_t kind, uint64_t xmm[4]) {
    __m128i d, s;
    memcpy(&d, xmm, 16);
    memcpy(&s, xmm + 2, 16);
    if (kind == ISA_EXTRQ) __asm__("extrq %1, %0" : "+x"(d) : "x"(s));
    else __asm__("insertq %1, %0" : "+x"(d) : "x"(s));
    memcpy(xmm, &d, 16);
}

/* movdqu xmm0,[rdi]; movdqu xmm1,[rdi+16]; <extrq xmm0,imm,imm | insertq xmm0,xmm1,imm,imm>; movdqu [rdi],xmm0; ret */
static void native_imm(uint8_t kind, uint64_t xmm[4], unsigned length, unsigned index) {
    unsigned char *p = jit;
    static const uint8_t load[] = {0xf3, 0x0f, 0x6f, 0x07, 0xf3, 0x0f, 0x6f, 0x4f, 0x10};
    static const uint8_t store[] = {0xf3, 0x0f, 0x7f, 0x07, 0xc3};
    memcpy(p, load, sizeof load);
    p += sizeof load;
    *p++ = kind == ISA_EXTRQ_IMM ? 0x66 : 0xf2;
    *p++ = 0x0f;
    *p++ = 0x78;
    *p++ = kind == ISA_EXTRQ_IMM ? 0xc0 : 0xc1;
    *p++ = (uint8_t)length;
    *p++ = (uint8_t)index;
    memcpy(p, store, sizeof store);
    ((Jit)(void *)jit)(xmm);
}
#endif

/* ---- 1: AMD results (Zen 3) that every host checks ---- */

static void test_recorded_amd_results(void) {
    static const struct { uint8_t kind; uint64_t dst, src_lo, src_hi; unsigned length, index; uint64_t lo; } v[] = {
        {ISA_EXTRQ_IMM, 0x0123456789abcdefull, 0, 0, 8, 16, 0xab},
        {ISA_EXTRQ_IMM, 0x0123456789abcdefull, 0, 0, 0, 0, 0x0123456789abcdefull},       /* length 0 = 64 */
        {ISA_EXTRQ_IMM, 0x0123456789abcdefull, 0, 0, 0, 7, 0x0002468acf13579bull},       /* 7 + 64 > 64 */
        {ISA_EXTRQ_IMM, 0x0123456789abcdefull, 0, 0, 54, 14, 0x0000048d159e26afull},     /* 14 + 54 > 64 */
        {ISA_EXTRQ_IMM, 0x0123456789abcdefull, 0, 0, 27, 49, 0x91},                      /* 49 + 27 > 64 */
        {ISA_EXTRQ_IMM, 0x0123456789abcdefull, 0, 0, 0xc8, 0xd0, 0xab},                  /* bits 7:6 ignored */
        {ISA_INSERTQ_IMM, 0x0123456789abcdefull, 0xff, 0, 4, 8, 0x0123456789abcfefull},
        {ISA_INSERTQ_IMM, 0x0123456789abcdefull, 0x1122334455667788ull, 0, 0, 0, 0x1122334455667788ull},
        {ISA_INSERTQ_IMM, 0x0123456789abcdefull, 0x1122334455667788ull, 0, 0, 7, 0x9119a22ab33bc46full},
        {ISA_INSERTQ_IMM, 0x0123456789abcdefull, 0x1122334455667788ull, 0, 45, 21, 0x688aaccef10bcdefull},
        {ISA_INSERTQ_IMM, 0x0123456789abcdefull, 0x1122334455667788ull, 0, 9, 7, 0x0123456789abc46full},
    };
    for (size_t i = 0; i < sizeof v / sizeof *v; i++) {
        uint64_t xmm[4] = {v[i].dst, 0xfedcba9876543210ull, v[i].src_lo, v[i].src_hi};
        model(v[i].kind, xmm, v[i].length, v[i].index);
        assert(xmm[0] == v[i].lo && xmm[1] == 0);
        /* The register forms read the same fields from the source register. */
        uint64_t reg[4] = {v[i].dst, 0xfedcba9876543210ull, v[i].src_lo, v[i].src_hi};
        const uint64_t control = 0xdead0000c0c0c0c0ull | (v[i].length & 63) | (uint64_t)(v[i].index & 63) << 8;
        IsaSite s = sse4a_site(v[i].kind == ISA_EXTRQ_IMM ? ISA_EXTRQ : ISA_INSERTQ, 0, 1, 0, 0);
        if (s.kind == ISA_EXTRQ) reg[2] = control;
        else reg[3] = control;
        isa_emulate_sse4a(&s, reg);
        assert(reg[0] == v[i].lo && reg[1] == 0);
    }
}

/* ---- 2: the emulation against the CPU ---- */

#ifdef __SSE4A__
static void compare_reg(uint8_t kind, const uint64_t in[4]) {
    uint64_t want[4], got[4];
    memcpy(want, in, sizeof want);
    memcpy(got, in, sizeof got);
    native_reg(kind, want);
    IsaSite s = sse4a_site(kind, 0, 1, 0, 0);
    isa_emulate_sse4a(&s, got);
    if (memcmp(want, got, 16) != 0) {
        fprintf(stderr, "%s dst %016llx:%016llx src %016llx:%016llx: cpu %016llx:%016llx, emulated %016llx:%016llx\n",
                isa_kind_name(kind), (unsigned long long)in[1], (unsigned long long)in[0], (unsigned long long)in[3],
                (unsigned long long)in[2], (unsigned long long)want[1], (unsigned long long)want[0],
                (unsigned long long)got[1], (unsigned long long)got[0]);
        abort();
    }
}
#endif

static unsigned long long compared, reference_checked;

static void test_against_cpu(void) {
#ifdef __SSE4A__
    if (!native) return;
    static const uint8_t kinds[] = {ISA_EXTRQ, ISA_INSERTQ};
    for (int k = 0; k < 2; k++) {
        const int control_hi = kinds[k] == ISA_INSERTQ;   /* control in xmm2[63:0] or xmm2[127:64] */
        /* Every field length and bit index (including index + length > 64 and length 0), with junk
         * in the control word's other bits. */
        for (unsigned length = 0; length < 64; length++)
            for (unsigned index = 0; index < 64; index++)
                for (int n = 0; n < 24; n++) {
                    uint64_t in[4] = {operand(), operand(), operand(), operand()};
                    in[2 + control_hi] = (next() & ~0x3f3full) | length | (uint64_t)index << 8;
                    if (n < 4) in[2 + control_hi] &= 0x3f3f;
                    compare_reg(kinds[k], in);
                    compared++;
                }
        for (int n = 0; n < 1000000; n++) {          /* fully random operands */
            uint64_t in[4] = {next(), next(), next(), next()};
            compare_reg(kinds[k], in);
            compared++;
        }
    }
    /* Immediate forms: every pair of immediate bytes (bits 7:6 included). */
    static const uint8_t imm_kinds[] = {ISA_EXTRQ_IMM, ISA_INSERTQ_IMM};
    for (int k = 0; k < 2; k++)
        for (unsigned length = 0; length < 256; length++)
            for (unsigned index = 0; index < 256; index++)
                for (int n = 0; n < 3; n++) {
                    uint64_t want[4] = {operand(), operand(), operand(), operand()}, got[4];
                    memcpy(got, want, sizeof got);
                    native_imm(imm_kinds[k], want, length, index);
                    model(imm_kinds[k], got, length, index);
                    if (memcmp(want, got, 16) != 0) {
                        fprintf(stderr, "%s length %u index %u: cpu %016llx:%016llx, emulated %016llx:%016llx\n",
                                isa_kind_name(imm_kinds[k]), length, index, (unsigned long long)want[1],
                                (unsigned long long)want[0], (unsigned long long)got[1], (unsigned long long)got[0]);
                        abort();
                    }
                    compared++;
                }
#endif
}

/* ---- 3: whole rewritten sites ---- */

/* Registers around a call (run_state below): gpr in x86 numbering (rsp not used), all 16 YMM, and
 * the callee's red zone, filled with 0x5a before the call. */
typedef struct {
    uint64_t gpr[16];
    uint64_t rflags, pad;
    uint8_t ymm[16][32];
    uint64_t red_zone[16];
} State;
_Static_assert(sizeof(State) == 784, "run_state offsets");
void ABI run_state(void *code, State *state);

#define RS_IRP16 ".irp i,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15\n"
__asm__(".text\n"
        ".globl run_state\n"
#ifndef _WIN32
        ".type run_state, @function\n"
#endif
        "run_state:\n"
        "push %rbx\n push %rbp\n push %r12\n push %r13\n push %r14\n push %r15\n"
        "push %rsi\n push %rdi\n"                     /* [rsp] = code, [rsp+8] = state */
        "movabs $0x5a5a5a5a5a5a5a5a, %rax\n"
        RS_IRP16 "mov %rax, -16-8*\\i(%rsp)\n .endr\n" /* callee red zone: [rsp-136, rsp-8) */
        RS_IRP16 "vmovdqu 144+32*\\i(%rsi), %ymm\\i\n .endr\n"
        "pushq 128(%rsi)\n popfq\n"
        "mov 0(%rsi), %rax\n mov 8(%rsi), %rcx\n mov 16(%rsi), %rdx\n mov 24(%rsi), %rbx\n"
        "mov 40(%rsi), %rbp\n mov 56(%rsi), %rdi\n mov 64(%rsi), %r8\n mov 72(%rsi), %r9\n"
        "mov 80(%rsi), %r10\n mov 88(%rsi), %r11\n mov 96(%rsi), %r12\n mov 104(%rsi), %r13\n"
        "mov 112(%rsi), %r14\n mov 120(%rsi), %r15\n mov 48(%rsi), %rsi\n"
        "call *(%rsp)\n"
        "pushfq\n"
        "xchg %rsi, 16(%rsp)\n"                       /* rsi = state, slot = the guest's rsi */
        "mov %rax, 0(%rsi)\n mov %rcx, 8(%rsi)\n mov %rdx, 16(%rsi)\n mov %rbx, 24(%rsi)\n"
        "mov %rbp, 40(%rsi)\n mov %rdi, 56(%rsi)\n mov %r8, 64(%rsi)\n mov %r9, 72(%rsi)\n"
        "mov %r10, 80(%rsi)\n mov %r11, 88(%rsi)\n mov %r12, 96(%rsi)\n mov %r13, 104(%rsi)\n"
        "mov %r14, 112(%rsi)\n mov %r15, 120(%rsi)\n"
        "mov 16(%rsp), %rax\n mov %rax, 48(%rsi)\n"
        "pop %rax\n mov %rax, 128(%rsi)\n"
        "cld\n"  /* guest DF is captured above; the C caller requires DF clear */
        RS_IRP16 "vmovdqu %ymm\\i, 144+32*\\i(%rsi)\n .endr\n"
        RS_IRP16 "mov -16-8*\\i(%rsp), %rax\n mov %rax, 656+8*\\i(%rsi)\n .endr\n"
        "vzeroupper\n"
        "add $16, %rsp\n"
        "pop %r15\n pop %r14\n pop %r13\n pop %r12\n pop %rbp\n pop %rbx\n"
        "ret\n");

/* int3 sites: the probe's handler in miniature, XMM registers included. */
static IsaTable *trap_table;
#ifdef _WIN32
static LONG WINAPI trap_handler(EXCEPTION_POINTERS *info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT *c = info->ContextRecord;
    IsaRegs regs;
    memset(&regs, 0, sizeof regs);
    uintptr_t rip;
    if (!isa_rewrite_trap(trap_table, (uintptr_t)info->ExceptionRecord->ExceptionAddress, &regs,
                          &c->FltSave.XmmRegisters[0], &rip))
        return EXCEPTION_CONTINUE_SEARCH;
    c->Rip = rip;
    return EXCEPTION_CONTINUE_EXECUTION;
}
static void install_trap_handler(void) { assert(AddVectoredExceptionHandler(1, trap_handler) != NULL); }
#else
static void trap_handler(int sig, siginfo_t *info, void *context) {
    (void)sig;
    (void)info;
    ucontext_t *uc = context;
    greg_t *g = uc->uc_mcontext.gregs;
    unsigned char *fpu = (unsigned char *)uc->uc_mcontext.fpregs;
    IsaRegs regs;
    memset(&regs, 0, sizeof regs);
    uintptr_t rip;
    if (!isa_rewrite_trap(trap_table, (uintptr_t)g[REG_RIP] - 1, &regs, fpu + 160, &rip)) abort();
    uint32_t magic;
    memcpy(&magic, fpu + 464, 4);
    if (magic == 0x46505853u) fpu[512] |= 2;   /* as in probe.c: XSTATE_BV.SSE */
    g[REG_RIP] = (greg_t)rip;
}
static void install_trap_handler(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = trap_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    assert(sigaction(SIGTRAP, &sa, NULL) == 0);
}
#endif

/* Encodes the instruction; returns its length. */
static size_t encode(uint8_t *out, uint8_t kind, int dst, int src, unsigned length, unsigned index) {
    size_t n = 0;
    out[n++] = kind == ISA_EXTRQ || kind == ISA_EXTRQ_IMM ? 0x66 : 0xf2;
    const int rm = kind == ISA_EXTRQ_IMM ? dst : src, reg = kind == ISA_EXTRQ_IMM ? 0 : dst;
    if (reg >= 8 || rm >= 8) out[n++] = (uint8_t)(0x40 | (reg >= 8) << 2 | (rm >= 8));
    out[n++] = 0x0f;
    out[n++] = kind == ISA_EXTRQ || kind == ISA_INSERTQ ? 0x79 : 0x78;
    out[n++] = (uint8_t)(0xc0 | (reg & 7) << 3 | (rm & 7));
    if (kind == ISA_EXTRQ_IMM || kind == ISA_INSERTQ_IMM) {
        out[n++] = (uint8_t)length;
        out[n++] = (uint8_t)index;
    }
    return n;
}

static void random_state(State *s) {
    memset(s, 0, sizeof *s);
    for (int i = 0; i < 16; i++) s->gpr[i] = i == 4 ? 0 : next();
    s->rflags = 0x202 | (next() & 0x8d5) | (next() & 1 ? 0x400 : 0);   /* CF PF AF ZF SF OF, DF */
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 4; j++) {
            uint64_t v = operand();
            memcpy(&s->ymm[i][8 * j], &v, 8);
        }
}

/* Expected state when the CPU cannot run the original: the model on dst's low 128 bits; the
 * relocated lea adds 1 to rax. */
static void expected_by_model(State *s, uint8_t kind, int dst, int src, unsigned length, unsigned index,
                              int relocated) {
    uint64_t xmm[4];
    memcpy(xmm, s->ymm[dst], 16);
    if (src >= 0) memcpy(xmm + 2, s->ymm[src], 16);
    uint64_t ref[4];
    memcpy(ref, xmm, sizeof ref);
    model(kind, xmm, length, index);
    reference(kind, ref, length, index);
    assert(memcmp(ref, xmm, 16) == 0);   /* the model and the bitwise reference agree */
    reference_checked++;
    memcpy(s->ymm[dst], xmm, 16);
    if (relocated) s->gpr[0]++;
    for (int i = 0; i < 16; i++) s->red_zone[i] = 0x5a5a5a5a5a5a5a5aull;
}

static unsigned sites_run[4];   /* by mode */
/* run_site options: inline_on sets IsaTable.inline_sse4a (TLG_EXTRQ_INLINE=1), counters_off sets
 * no_hits (the default in game: ISS_RUNTIME_COUNTERS=0). random_control: every round gets a control
 * word with any length and index (index + length > 64 and length 0 included). */
static int inline_on, counters_off, random_control;
static unsigned inline_sites;

static void run_site(uint8_t kind, int dst, int src, unsigned length, unsigned index, uint8_t mode, int rounds) {
    static const uint8_t lea_rax[] = {0x48, 0x8d, 0x40, 0x01};   /* lea rax, [rax+1]: plain, 4 bytes */
    uint8_t code[32];
    size_t n = encode(code, kind, dst, src, length, index);
    const int relocated = mode == ISA_MODE_JMP_RELOCATE;
    assert(mode == ISA_MODE_JMP ? n >= 5 : n == 4);
    size_t total = n;
    if (relocated) {
        memcpy(code + total, lea_rax, sizeof lea_rax);
        total += sizeof lea_rax;
    }
    code[total++] = 0xc3;
    memset(pristine, 0xcc, PAGE);
    memcpy(pristine + SITE_AT, code, total);
    memcpy(image, pristine, PAGE);
    memset(stubs, 0, PAGE);

    static IsaSite s;   /* the stub and trap_table keep pointers to these */
    static IsaTable t;
    static uint64_t hits;
    s = sse4a_site(kind, (uint8_t)dst, (uint8_t)src, length, index);
    s.offset = SITE_AT;
    s.length = (uint8_t)n;
    s.mode = mode;
    memcpy(s.original, code, n);
    if (relocated) {
        s.relocated_length = sizeof lea_rax;
        memcpy(s.relocated, lea_rax, sizeof lea_rax);
    }
    memset(&t, 0, sizeof t);
    hits = 0;
    t.sites = &s;
    t.hits = &hits;
    t.count = 1;
    t.image_size = PAGE;
    t.inline_sse4a = inline_on;
    t.no_hits = counters_off;
    IsaReport r = isa_rewrite_apply(&t, image, stubs, CPU_SET_SSE4A);
    assert(r.rewritten == 1 && r.skipped == 0);
    /* Inline: every jmp/relocated EXTRQ imm, INSERTQ imm and EXTRQ xmm, xmm (distinct registers). */
    const int inlinable = inline_on && mode != ISA_MODE_INT3 &&
                          (kind == ISA_EXTRQ_IMM || kind == ISA_INSERTQ_IMM || (kind == ISA_EXTRQ && dst != src));
    assert(r.inlined == (uint32_t)inlinable);
    inline_sites += r.inlined;
    trap_table = &t;
    for (int round = 0; round < rounds; round++) {
        State in, want, got;
        random_state(&in);
        if (kind == ISA_EXTRQ || kind == ISA_INSERTQ) {
            /* Half the time a valid field (index + length <= 64) in the control word. */
            const unsigned l = (unsigned)(next() % 64), i = l ? (unsigned)(next() % (65 - l)) : 0;
            uint64_t control = (next() & ~0x3f3full) | l | (uint64_t)i << 8;
            if (random_control) control = (next() & ~0x3f3full) | next() % 64 | (next() % 64) << 8;
            if ((round & 1 || random_control) && src != dst)
                memcpy(&in.ymm[src][kind == ISA_INSERTQ ? 8 : 0], &control, 8);
        }
        memcpy(&want, &in, sizeof want);
        memcpy(&got, &in, sizeof got);
#ifdef __SSE4A__
        if (native) run_state(pristine + SITE_AT, &want);
#endif
        if (!native) expected_by_model(&want, kind, dst, kind == ISA_EXTRQ_IMM ? -1 : src, length, index, relocated);
        run_state(image + SITE_AT, &got);
#ifdef _WIN32
        /* Windows writes its exception frame below rsp: an int3 site cannot keep the red zone
         * (docs/red_zone.md: int3 ISA sites are not protected). */
        if (mode == ISA_MODE_INT3) memcpy(got.red_zone, want.red_zone, sizeof got.red_zone);
#endif
        if (memcmp(&want, &got, sizeof want) != 0) {
            fprintf(stderr, "%s xmm%d, xmm%d (length %u, index %u) mode %u: state differs\n", isa_kind_name(kind), dst,
                    src, length, index, mode);
            for (int i = 0; i < 16; i++)
                if (want.gpr[i] != got.gpr[i]) fprintf(stderr, "  gpr%d %016llx %016llx\n", i,
                                                       (unsigned long long)want.gpr[i], (unsigned long long)got.gpr[i]);
            if (want.rflags != got.rflags) fprintf(stderr, "  rflags %llx %llx\n", (unsigned long long)want.rflags,
                                                   (unsigned long long)got.rflags);
            for (int i = 0; i < 16; i++)
                if (memcmp(want.ymm[i], got.ymm[i], 32)) fprintf(stderr, "  ymm%d\n", i);
            if (memcmp(want.red_zone, got.red_zone, sizeof want.red_zone)) fprintf(stderr, "  red zone\n");
            abort();
        }
        if (native) {   /* the CPU agrees with the model too */
            State model_state = in;
            expected_by_model(&model_state, kind, dst, kind == ISA_EXTRQ_IMM ? -1 : src, length, index, relocated);
            assert(memcmp(&model_state, &want, sizeof want) == 0);
        }
    }
    assert(hits == (counters_off ? 0 : (uint64_t)rounds));
    sites_run[mode]++;
}

static void test_sites(void) {
    install_trap_handler();
    static const uint8_t kinds[] = {ISA_EXTRQ, ISA_INSERTQ, ISA_EXTRQ_IMM, ISA_INSERTQ_IMM};
    for (int k = 0; k < 4; k++)
        for (int dst = 0; dst < 16; dst++)
            for (int src = 0; src < 16; src++) {
                if (kinds[k] == ISA_EXTRQ_IMM && src) continue;
                const unsigned length = (unsigned)(next() % 64), index = (unsigned)(next() % 64);
                uint8_t probe[16];
                const size_t n = encode(probe, kinds[k], dst, src, length, index);
                if (n == 4) {
                    run_site(kinds[k], dst, src, length, index, ISA_MODE_INT3, 8);
                    run_site(kinds[k], dst, src, length, index, ISA_MODE_JMP_RELOCATE, 8);
                } else {
                    run_site(kinds[k], dst, src, length, index, ISA_MODE_JMP, 8);
                }
            }
    /* The site of the Windows crash (inFAMOUS 1.07, 0x8938f1): extrq xmm4, xmm7. */
    run_site(ISA_EXTRQ, 4, 7, 0, 0, ISA_MODE_INT3, 2000);
    run_site(ISA_EXTRQ, 4, 7, 0, 0, ISA_MODE_JMP_RELOCATE, 2000);
}

/* TLG_EXTRQ_INLINE=1: the inline stubs against the CPU (SSE4A host) or the model, which is itself
 * checked against the bitwise reference on every round. Whole state compared as above: GPRs, flags,
 * all 16 YMM registers (the scratch one included) and the red zone. */
static void test_inline_sites(void) {
    static const uint8_t kinds[] = {ISA_EXTRQ, ISA_INSERTQ, ISA_EXTRQ_IMM, ISA_INSERTQ_IMM};
    inline_on = 1;
    for (int counters = 0; counters < 2; counters++) {
        counters_off = !counters;
        /* Every register pair and form (the INSERTQ register form and EXTRQ xmmN, xmmN keep the C
         * stub: run_site checks that too). */
        for (int k = 0; k < 4; k++)
            for (int dst = 0; dst < 16; dst++)
                for (int src = 0; src < 16; src++) {
                    if (kinds[k] == ISA_EXTRQ_IMM && src) continue;
                    const unsigned length = (unsigned)(next() % 64), index = (unsigned)(next() % 64);
                    uint8_t probe[16];
                    const size_t n = encode(probe, kinds[k], dst, src, length, index);
                    run_site(kinds[k], dst, src, length, index, n == 4 ? ISA_MODE_JMP_RELOCATE : ISA_MODE_JMP, 16);
                }
        /* Immediate forms: every length and index, including length 0 (= 64) and index + length > 64. */
        for (unsigned length = 0; length < 64; length++)
            for (unsigned index = 0; index < 64; index++) {
                run_site(ISA_EXTRQ_IMM, (int)(next() % 16), 0, length, index, ISA_MODE_JMP, 4);
                run_site(ISA_INSERTQ_IMM, (int)(next() % 16), (int)(next() % 16), length, index, ISA_MODE_JMP, 4);
            }
        /* EXTRQ xmm, xmm (every inFAMOUS 1.07 SSE4A site): random controls over many rounds, the
         * game's forms (4-byte relocated and REX 5-byte) and scratch registers past xmm0. */
        random_control = 1;
        static const int pairs[][2] = {{4, 7}, {0, 1}, {1, 0}, {0, 15}, {12, 14}, {8, 2}, {15, 0}, {3, 10}};
        for (size_t i = 0; i < sizeof pairs / sizeof *pairs; i++) {
            uint8_t probe[16];
            const size_t n = encode(probe, ISA_EXTRQ, pairs[i][0], pairs[i][1], 0, 0);
            run_site(ISA_EXTRQ, pairs[i][0], pairs[i][1], 0, 0, n == 4 ? ISA_MODE_JMP_RELOCATE : ISA_MODE_JMP, 20000);
        }
        random_control = 0;
    }
    inline_on = counters_off = 0;
}

static void test_table_records(void) {
    IsaSite s = sse4a_site(ISA_EXTRQ, 4, 7, 0, 0);
    assert(isa_site_set(&s) == CPU_SET_SSE4A && isa_kind_is_sse4a(ISA_INSERTQ_IMM) && !isa_kind_is_sse4a(ISA_BLSR));
    assert(!strcmp(isa_kind_name(ISA_EXTRQ_IMM), "extrq") && !strcmp(isa_kind_name(ISA_INSERTQ), "insertq") &&
           !strcmp(isa_kind_name(ISA_MOVNTSD), "movntsd"));
    /* A host lacking only MOVBE leaves SSE4A sites alone. */
    memcpy(pristine + SITE_AT, "\x66\x41\x0f\x79\xc7\xc3", 6);   /* extrq xmm0, xmm15; ret */
    memcpy(image, pristine, PAGE);
    s = sse4a_site(ISA_EXTRQ, 0, 15, 0, 0);
    s.offset = SITE_AT;
    s.length = 5;
    memcpy(s.original, pristine + SITE_AT, 5);
    uint64_t hits = 0;
    IsaTable t = {.sites = &s, .hits = &hits, .count = 1, .image_size = PAGE};
    assert(isa_rewrite_apply(&t, image, stubs, CPU_SET_MOVBE).rewritten == 0 && memcmp(image, pristine, PAGE) == 0);
    /* An unsupported SSE4A record (MOVNTSS) stops the rewrite only when SSE4A is rewritten. */
    s.kind = ISA_MOVNTSS;
    s.mode = ISA_MODE_UNSUPPORTED;
    assert(isa_rewrite_apply(&t, image, stubs, CPU_SET_MOVBE).unsupported_needed == 0);
    assert(isa_rewrite_apply(&t, image, stubs, CPU_SET_SSE4A).unsupported_needed == 1);
}

int main(void) {
#ifdef _WIN32
    unsigned char *region = VirtualAlloc(NULL, 4 * PAGE, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    assert(region != NULL);
#else
    unsigned char *region = mmap(NULL, 4 * PAGE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(region != MAP_FAILED);
#endif
    image = region;
    stubs = region + PAGE;
    pristine = region + 2 * PAGE;
    jit = region + 3 * PAGE;
    __builtin_cpu_init();
    /* run_state saves and compares YMM registers: a host without AVX cannot run the game either. */
    if (!__builtin_cpu_supports("avx")) {
        puts("isa-sse4a-test SKIP (host without AVX)");
        return 0;
    }
    native = __builtin_cpu_supports("sse4a");
#ifndef __SSE4A__
    native = 0;
#endif
    test_recorded_amd_results();
    test_against_cpu();
    test_table_records();
    test_sites();
    test_inline_sites();
    printf("isa-sse4a-test ok (%s: %llu operand sets against the CPU; sites: %u jmp, %u relocated, %u int3; "
           "%u inline stubs; %llu model results checked against the bitwise reference)\n",
           native ? "SSE4A host, CPU as oracle" : "no SSE4A, recorded AMD results and model", compared,
           sites_run[ISA_MODE_JMP], sites_run[ISA_MODE_JMP_RELOCATE], sites_run[ISA_MODE_INT3], inline_sites,
           reference_checked);
    return 0;
}
