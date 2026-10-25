/* The guest code (eboot.bin, libc.prx) is compiled for the PS4's Jaguar CPU and runs on the host
 * CPU as it is. A host without one of Jaguar's instruction sets stops with an illegal
 * instruction somewhere inside the game (#33, #66); this check names the missing sets at start. */
#ifndef TLG_CPU_CHECK_H
#define TLG_CPU_CHECK_H
#include <stddef.h>
#include <stdint.h>

/* CPUID words: leaf 1 ECX, leaf 7 subleaf 0 EBX, leaf 0x80000001 ECX. */
typedef struct { uint32_t leaf1_ecx, leaf7_ebx, ext1_ecx; } CpuFeatureWords;

CpuFeatureWords cpu_feature_words(void);
enum { CPU_SET_MOVBE = 1u << 0, CPU_SET_BMI1 = 1u << 1, CPU_SET_LZCNT = 1u << 2, CPU_SET_F16C = 1u << 3, CPU_SET_SSE4A = 1u << 4 };
/* Fatal sets the host lacks (SSE4.2, POPCNT, AVX), comma separated in out; returns how many. */
int cpu_missing_fatal(CpuFeatureWords words, char *out, size_t size);
/* Rewritable sets the host lacks (MOVBE, F16C, BMI1, LZCNT, SSE4A): CPU_SET_* mask; names in out. */
unsigned cpu_missing_rewritable(CpuFeatureWords words, char *out, size_t size);
/* BMI2 (leaf 7 EBX bit 8, Intel since Haswell): the ISA rewrite's inline EXTRQ (SHRX/SHLX). */
int cpu_has_bmi2(CpuFeatureWords words);

/* CPU_SET_* of a TLG_ISA_REWRITE value: "1" = every emulated set (MOVBE, BMI1, LZCNT, F16C, SSE4A),
 * otherwise a comma separated list of names ("sse4a", "f16c", "movbe,bmi1", case-insensitive); 0 for "0",
 * empty, or a list with an unknown name (*valid = 0 then). */
unsigned cpu_sets_named(const char *list, int *valid);

#endif
