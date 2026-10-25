#include "cpu_check.h"
#include <cpuid.h>
#include <stdio.h>
#include <string.h>

CpuFeatureWords cpu_feature_words(void) {
    CpuFeatureWords words = {0, 0, 0};
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (__get_cpuid(1, &a, &b, &c, &d)) words.leaf1_ecx = c;
    if (__get_cpuid_count(7, 0, &a, &b, &c, &d)) words.leaf7_ebx = b;
    if (__get_cpuid(0x80000001, &a, &b, &c, &d)) words.ext1_ecx = c;
    (void)a; (void)d;
    return words;
}

/* Jaguar's sets beyond the x86-64 baseline. A missing fatal set stops the game; the rewritable ones
 * can be emulated: MOVBE, F16C, BMI1 and LZCNT, which Sandy and Ivy Bridge lack (each arrived on
 * Intel with Haswell at the latest), and AMD's SSE4A, which no Intel CPU has. The guest code does use
 * SSE4A: inFAMOUS Second Son and Gravity Rush 2 each have 96 EXTRQ in eboot.bin. */
enum { LEAF1, LEAF7, EXT1 };
typedef struct { const char *name; int word; uint32_t bit; unsigned set; } CpuSet;
static const CpuSet fatal_sets[] = {
    {"SSE4.2", LEAF1, 1u << 20, 0}, {"POPCNT", LEAF1, 1u << 23, 0}, {"AVX", LEAF1, 1u << 28, 0},
};
static const CpuSet rewritable_sets[] = {
    {"MOVBE", LEAF1, 1u << 22, CPU_SET_MOVBE}, {"F16C", LEAF1, 1u << 29, CPU_SET_F16C},
    {"BMI1", LEAF7, 1u << 3, CPU_SET_BMI1},    {"LZCNT", EXT1, 1u << 5, CPU_SET_LZCNT},
    {"SSE4A", EXT1, 1u << 6, CPU_SET_SSE4A},
};

/* Names the missing sets, comma separated in out; returns how many, and their CPU_SET_* bits in *mask. */
static int missing_sets(CpuFeatureWords words, const CpuSet *sets, size_t count, char *out, size_t size,
                        unsigned *mask) {
    const uint32_t values[] = {words.leaf1_ecx, words.leaf7_ebx, words.ext1_ecx};
    int missing = 0;
    size_t used = 0;
    *mask = 0;
    if (size) out[0] = 0;
    for (size_t i = 0; i < count; ++i) {
        if (values[sets[i].word] & sets[i].bit) continue;
        if (used < size) {
            const int n = snprintf(out + used, size - used, "%s%s", missing ? ", " : "", sets[i].name);
            used = n < 0 ? size : used + (size_t)n;
            if (used >= size) used = size - 1;
        }
        *mask |= sets[i].set;
        ++missing;
    }
    return missing;
}

int cpu_missing_fatal(CpuFeatureWords words, char *out, size_t size) {
    unsigned mask;
    return missing_sets(words, fatal_sets, sizeof fatal_sets / sizeof *fatal_sets, out, size, &mask);
}

unsigned cpu_missing_rewritable(CpuFeatureWords words, char *out, size_t size) {
    unsigned mask;
    missing_sets(words, rewritable_sets, sizeof rewritable_sets / sizeof *rewritable_sets, out, size, &mask);
    return mask;
}

int cpu_has_bmi2(CpuFeatureWords words) { return (words.leaf7_ebx >> 8) & 1; }

/* The n bytes at text equal name (lower case), ignoring ASCII case. */
static int name_is(const char *text, size_t n, const char *name) {
    if (n != strlen(name)) return 0;
    for (size_t i = 0; i < n; ++i) {
        char c = text[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != name[i]) return 0;
    }
    return 1;
}

unsigned cpu_sets_named(const char *list, int *valid) {
    static const struct { const char *name; unsigned set; } names[] = {
        {"movbe", CPU_SET_MOVBE}, {"bmi1", CPU_SET_BMI1}, {"lzcnt", CPU_SET_LZCNT}, {"f16c", CPU_SET_F16C},
        {"sse4a", CPU_SET_SSE4A},
    };
    *valid = 1;
    if (!list || !*list || !strcmp(list, "0")) return 0;
    if (!strcmp(list, "1")) return CPU_SET_MOVBE | CPU_SET_BMI1 | CPU_SET_LZCNT | CPU_SET_F16C | CPU_SET_SSE4A;
    unsigned sets = 0;
    for (const char *at = list; *at;) {
        size_t n = strcspn(at, ",");
        unsigned found = 0;
        for (size_t i = 0; i < sizeof names / sizeof *names; ++i)
            if (name_is(at, n, names[i].name)) found = names[i].set;
        if (!found) { *valid = 0; return 0; }
        sets |= found;
        at += n;
        if (*at == ',') ++at;
    }
    return sets;
}
