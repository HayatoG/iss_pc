# ISA rewrite: instructions the host CPU lacks

The PS4's Jaguar CPU has MOVBE, F16C, BMI1, LZCNT and AMD's SSE4A. The guest code runs on the host
CPU as it is, so a host without one of these sets would stop on an illegal instruction. At start
(`launcher/infamous_run.py` → `scripts/isa_rewrite.py`) the port lists every such instruction in
the linked image (`out/isa-rewrite.bin`, format G3ISA1), and the probe (`src/isa_rewrite.c`) patches
the sites of the sets the CPU lacks with a jump to a stub that emulates the instruction
(`src/isa_emulate.c`, built with `-mgeneral-regs-only` and no libc: it runs inside guest code and
leaves every XMM/YMM register alone). SSE4.2, POPCNT and AVX are still required.

| Set | Hosts without it | inFAMOUS 1.07 sites | Stub |
|---|---|---|---|
| MOVBE, BMI1, LZCNT | Sandy/Ivy Bridge | 27, 1615, 164 | GPRs in an `IsaRegs`, `isa_emulate` |
| SSE4A (EXTRQ/INSERTQ) | every Intel CPU | 96 | XMM operands copied to a buffer, `isa_emulate_sse4a` |
| F16C (VCVTPH2PS/VCVTPS2PH) | Sandy Bridge (#51) | 13 | `IsaRegs` + YMM buffer, `isa_emulate_f16c` |

`ISS_ISA_REWRITE` chooses: unset = the sets the CPU lacks, `0` = never, `1` = every emulated set,
or a list (`sse4a`, `f16c`, `f16c,movbe`, ...) forced on top of what the CPU lacks (testing on a
complete CPU).

## SSE4A inline stubs (`ISS_EXTRQ_INLINE=1`, default 0)

All 96 SSE4A sites of the 1.07 eboot are `extrq xmmD, xmmS` with a register control (73 four-byte
sites relocated, 23 REX five-byte jmp sites, D != S, no INSERTQ, no immediate forms), in one routine
group (0x890a77-0x89475c). They are hot: with `ISS_RUNTIME_COUNTERS=1` the top ten each ran about
3.2 million times in a 60 s bench run (`ISA: most executed sites:` at exit). The default stub saves
11 GPRs and rflags, stores two XMM registers, calls `isa_emulate_sse4a` and reloads (popfq alone is
about 20 cycles on Intel).

With `ISS_EXTRQ_INLINE=1` (`TLG_EXTRQ_INLINE`) the sites an SSE2 sequence computes get an inline
stub instead: EXTRQ with a register control and distinct registers, EXTRQ imm, INSERTQ imm. The
register INSERTQ form and `extrq xmmN, xmmN` keep the C stub. For `extrq d, s`:

```
lea rsp, [rsp-0x90]      ; below the red zone
movdqu [rsp], t          ; scratch: lowest XMM other than d and s
movdqa t, s ; psrlq t, 8 ; pand t, [63]                  ; t = index
psrlq d, t
movdqa t, s ; pandn t, [63] ; paddq t, [1] ; pand t, [63] ; t = (64 - length) & 63
psllq d, t ; psrlq d, t                                   ; keep length bits (0 = 64)
movq d, d                ; high qword = 0, as AMD hardware and isa_emulate_sse4a
movdqu t, [rsp] ; lea rsp, [rsp+0x90] ; (relocated instruction) ; jmp back
```

No flags or GPRs change (with `ISS_RUNTIME_COUNTERS=1` the hit count saves rflags and rax around
its inc); legacy SSE keeps bits 255:128 of every YMM register. The constants sit at the end of the
16-aligned stub slot. `ISA: SSE4A inline stubs (TLG_EXTRQ_INLINE=1): N sites` in the log says how
many were inlined. `tests/test_isa_sse4a.c` runs every register pair and form, every immediate
pair and 20,000 random controls per game form through the inline stubs with the whole state
compared (GPRs, flags, 16 YMM, red zone), against the CPU on AMD and the model elsewhere; the model
is checked against a bitwise reference on every round.

## F16C (iss_pc #51)

### The sites

All 13 are in one routine group of the 1.07 eboot (0x612da8-0x61300e), VEX.128, register or `[rdi]`:

| Form | Sites |
|---|---|
| `vcvtps2ph xmm0, xmm0, 8` | 0x612da8, 0x612f79, 0x612fd6 |
| `vcvtps2ph xmm1, xmm1, 8` | 0x612f4a, 0x612f5a, 0x612f73, 0x612fba, 0x612fd0 |
| `vcvtph2ps xmm0, xmm0` | 0x612de3, 0x612e07, 0x61300e |
| `vcvtph2ps xmm0, [rdi]` | 0x612df0, 0x612fa0 |

Immediate 8 = round to nearest even (bits 1:0 = 0), MXCSR.RC not used (bit 2 = 0), precision
exception masked (bit 3). They are 5 and 6 bytes long, so every one is a plain `jmp` site.

The emulation covers more than these: every VEX form (xmm and ymm, register or memory operand,
RIP-relative and rsp-based addresses, any immediate). EVEX (AVX-512) forms, segment prefixes and
32-bit addresses stay UNSUPPORTED. The record keeps the XMM/YMM numbers in `dst`/`src1` (`NO_REG` for
the memory operand), the vector size in bytes in `width` (16 or 32) and the VCVTPS2PH immediate in
`imm` (a byte taken from the record's padding).

### The stub

```
lea rsp, [rsp-0x80]              ; step over the guest's red zone (Windows writes exception frames below rsp)
push r15 .. rax ; pushfq         ; an IsaRegs; gpr[4] then set to the guest rsp ([rsp+x] operands)
cld ; lea rsp, [rsp-0x40]        ; 64-byte buffer: [0, 32) destination, [32, 64) source
vmovdqu [rsp+0x20], ymm<src>     ; when the source is a register
inc hits ; align ; call isa_emulate_f16c(site, regs, buffer)
vmovdqu ymm<dst>, [rsp]          ; when the destination is a register: all 256 bits (VEX zeroes the rest)
popfq ; pop rax .. r15 ; lea rsp, [rsp+0x80] ; jmp back
```

About 165 bytes (the stub slot is 256). The host has AVX, so `vmovdqu ymm` saves and loads the
registers; no other XMM/YMM register is touched, every GPR and the flags come back as they were.
The C side reads and writes memory operands itself, 256+ bytes below the guest rsp, so a fault on a
GPU-tracked page there cannot hurt the red zone. F16C sites are never `int3` sites (a trap frame has
no YMM state to update).

Red-zone protection (`scripts/red_zone.py`, Windows) runs after the rewrite: an F16C site is an ISA
site for the analysis, so a red-zone group that holds it applies when the CPU has F16C and is
skipped (its bytes changed) when the site is emulated, as for the other sets.

### The conversion (software, no SSE)

- `isa_half_to_float`: exact. Denormal halves become normal floats; MXCSR.DAZ does not apply.
  Inf stays Inf; a NaN keeps sign and payload (shifted left 13) and is quieted.
- `isa_float_to_half`: the value is cut at the half's unit (2^-24 for denormals, 2^(E-10) for
  normals) with round and sticky bits, then rounded: nearest even, down, up or toward zero.
  `imm8[1:0]` picks the mode, or MXCSR.RC when `imm8[2]` is set. A carry out of the mantissa raises
  the exponent; past 65504 the result is Inf or 0x7bff/0xfbff as the mode says. MXCSR.DAZ makes a
  denormal input zero (checked on Zen 3); MXCSR.FTZ is ignored (tiny results stay denormal). A NaN
  keeps sign and the top 10 payload bits and is quieted.
- The MXCSR exception flags (PE, UE, OE, IE) are not updated: no guest code reads them after a
  conversion, and an unmasked-exception guest does not exist on the PS4.

### Tests (`tests/test_isa_f16c.c`, `build.sh --test`)

With F16C on the host the CPU is the oracle:
- every one of the 65,536 halves through VCVTPH2PS, under four MXCSR values;
- VCVTPS2PH for every sign, exponent and low-14-bit mantissa pattern in the four immediate modes,
  then all 16 immediates against all 16 MXCSR combinations of RC, DAZ and FTZ, then random words
  (about 79 million values);
- whole rewritten sites: every register pair, every base register, rsp-based operands inside the
  red zone and RIP-relative ones, xmm and ymm, random immediates and MXCSR, run next to the original
  instruction with all GPRs, flags, the 16 YMM registers, memory and the callee's red zone compared
  (2,036 sites), and the four forms of the game 20,000 times each.

Without F16C the test checks the recorded values only.

### In game (Ryzen 7 5700X3D, 150 s from the save, 14/10/2026)

| Run | Sites rewritten / skipped | Faults | Frame stats (mean of the 5 s windows after loading) |
|---|---|---|---|
| normal (no rewrite) | 0 | none | 26.2 FPS |
| `ISS_ISA_REWRITE=f16c` | 13 (13 jmp) / 0 | none | 26.0 FPS |
| `ISS_ISA_REWRITE=1` | 1915 (1764 jmp, 148 relocated, 3 int3) / 0 | none | 24.6 FPS |

The F16C stubs ran about 2.6 million times in 150 s (0x612e07 and 0x61300e 439k each, the
`vcvtps2ph` sites 121k-256k each); the cost does not show in the frame times.
