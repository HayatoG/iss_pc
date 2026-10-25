/* Loader side of the ISA rewrite (#33, #66; SSE4A on Intel; F16C, iss_pc #51): reads a G3ISA1 table (scripts/isa_rewrite.py), builds
 * one machine-code stub per site in an area next to the image and patches each site with a jmp
 * into its stub (or an int3). The stub saves the guest's registers, calls isa_emulate and jumps
 * back. Runs at load time, before the image is protected; libc is fine here. */
#ifndef TLG_ISA_REWRITE_H
#define TLG_ISA_REWRITE_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "isa_emulate.h"
#include "cpu_check.h"

/* One applied red-zone patch group: its first protected instruction and its stub (crash reports). */
typedef struct { uint64_t site; uint32_t stub, stub_bytes; } IsaRedZoneStub;
enum { ISA_RZ_STATS = 11 };              /* scripts/red_zone.py STATS_FIELDS */
typedef struct {
    IsaSite *sites; uint64_t *hits;      /* hits[i] belongs to sites[i] */
    uint32_t count, unsupported;
    uint64_t image_size; uint8_t sha256[32];
    unsigned char *image, *stubs;        /* set by isa_rewrite_apply */
    /* Windows red-zone protection (scripts/red_zone.py, section G3RZONE1 after the records): stub
     * code for image + rz_code_offset and patch groups. rz is NULL when the table has no section. */
    unsigned char *rz; uint64_t rz_size, rz_code_offset;
    uint32_t rz_code_bytes, rz_groups, rz_group_bytes, rz_stats[ISA_RZ_STATS];
    IsaRedZoneStub *rz_applied; uint32_t rz_applied_count;
    unsigned char *rz_area;              /* set by isa_red_zone_apply */
    /* Set before isa_rewrite_apply: a JMP_RELOCATE stub runs its relocated instruction 128 bytes
     * below the guest rsp when that instruction can fault (IsaSite.mem_flags & ISA_MEM_RELOCATED_FAULTABLE). */
    int protect_red_zone;
    /* Set before isa_rewrite_apply (host has BMI2): EXTRQ stubs extract the field inline with
     * SHRX/SHLX, which leave rflags alone, instead of saving registers and rflags around a call. */
    int extrq_bmi2;
} IsaTable;
enum { ISA_MEM_RELOCATED_FAULTABLE = 2 };
typedef struct { uint32_t groups, protected_instructions, short_relays, skipped; } IsaRedZoneReport;
/* rewritten = jmp + relocated + int3; skipped and unsupported_needed are outside it. */
typedef struct { uint32_t rewritten, jmp, relocated, int3, skipped, unsupported_needed; } IsaReport;
enum { ISA_STUB_BYTES = 256 };

/* Parses a G3ISA1 file (spec 4.4). NULL and one stderr line ("ISA: <reason>") when invalid. */
IsaTable *isa_table_load(const char *path);
/* 0 when size and sha256 match the image. */
int isa_table_matches(const IsaTable *t, const unsigned char *image, uint64_t size);
size_t isa_stub_area_size(const IsaTable *t);           /* count * ISA_STUB_BYTES */
unsigned isa_site_set(const IsaSite *s);                 /* CPU_SET_* of the kind */
/* Rewrites the sites whose set is in `sets` (spec 5.4). Writes nothing and returns
 * unsupported_needed > 0 when a needed site is UNSUPPORTED. `stubs` has isa_stub_area_size bytes, RW.
 * A JMP_RELOCATE site whose next instruction no longer matches becomes INT3 (its mode in t changes);
 * each skipped or downgraded site gets one stderr line. */
IsaReport isa_rewrite_apply(IsaTable *t, unsigned char *image, unsigned char *stubs, unsigned sets);

/* int3 at `address` (the CC byte): 1 when it is a rewritten INT3 site; regs updated, *next_rip = site + its length (4).
 * 0 otherwise, with regs and *next_rip untouched. xmm: the trapped thread's XMM0-15, 16 bytes each
 * (signal frame or CONTEXT), updated for an SSE4A site; NULL leaves SSE4A sites unhandled. */
int isa_rewrite_trap(IsaTable *t, uintptr_t address, IsaRegs *regs, void *xmm, uintptr_t *next_rip);
/* Site index whose stub holds rip, or -1. */
int isa_rewrite_site_of_stub(const IsaTable *t, uintptr_t rip);
/* Bytes of the red-zone stub code (0 without a section); the loader maps them at image + rz_code_offset. */
size_t isa_red_zone_code_size(const IsaTable *t);
/* Copies the red-zone stub code to `area` (which must be image + rz_code_offset, RW) and writes each
 * patch group whose original bytes all still match the image; a changed group (game patch, hook) is
 * skipped whole, with one stderr line. Returns {0} with skipped = groups when area is misplaced. */
IsaRedZoneReport isa_red_zone_apply(IsaTable *t, unsigned char *image, unsigned char *area);
/* 1 and *site (image offset of the protected instruction) when rip is inside an applied red-zone stub. */
int isa_red_zone_site_of_stub(const IsaTable *t, uintptr_t rip, uint64_t *site);
/* "ISA: most executed sites:" then up to 10 lines "  0x<offset> <mnemonic> <hits>" (hits > 0 only),
 * descending hits, ties by ascending offset. */
void isa_rewrite_report(const IsaTable *t, FILE *out);
/* "movbe", "lzcnt", "tzcnt", "andn", "bextr", "blsi", "blsmsk", "blsr", "vcvtph2ps", "vcvtps2ph", "extrq",
 * "insertq", "movntss", "movntsd"; "?" otherwise. */
const char *isa_kind_name(uint8_t kind);

#endif
