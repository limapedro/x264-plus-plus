# `common/x86/predict-c.c` — x86 intra prediction dispatch and plane-P setup

**Dirty-side behavioural specification.** Source path:
`common/x86/predict-c.c`. Role: builds the intra-prediction function tables for
x86 CPUs (CPU-feature dispatch) and computes the plane-mode (`I_PRED_*_P`)
parameters passed to the SIMD `*_p_core` kernels.

---

## 1. Header

C source included only in x86 builds. Includes `common/common.h`, `predict.h`,
and `pixel.h`. It defines:
* a macro `PREDICT_P_SUM(j,i)` for the horizontal/vertical gradient sums,
* optional x86 inline-asm accelerators for those sums (guarded by
  `HAVE_X86_INLINE_ASM`),
* macros to generate static `predict_*_p_<isa>` wrapper functions for each ISA,
* the five exported `x264_predict_*_init_mmx` dispatcher functions,
* one C fallback `predict_8x8c_dc_left` for 64-bit 8-bit builds.

## 2. Purpose

The SIMD kernels in `predict-a.asm` implement most modes directly, but the
plane-P mode needs a *setup* phase: the encoder must compute `i00`, `b`, `c`
from the edge samples before the SIMD core can generate the block. This file
computes those parameters (with optional inline-ASM acceleration) and then
fills the function-pointer arrays used by `common/predict.c` according to the
detected CPU features. It therefore defines the exact ISA dispatch precedence
for intra prediction.

## 3. Public interface

Five exported dispatch functions (declared in `predict.h.md`):

### 3.1 `void x264_predict_16x16_init_mmx(uint32_t cpu, x264_predict_t pf[7])`
Fills `pf[I_PRED_16x16_V/H/DC/DC_TOP/DC_LEFT/P]`.

### 3.2 `void x264_predict_8x8c_init_mmx(uint32_t cpu, x264_predict_t pf[7])`
Fills chroma 8x8 modes `V/H/DC/DC_TOP/DC_LEFT/P`.

### 3.3 `void x264_predict_8x16c_init_mmx(uint32_t cpu, x264_predict_t pf[7])`
Fills chroma 8x16 modes (`DC`, `DC_TOP`, `V`, `H`, `P`) — used for 4:2:2.

### 3.4 `void x264_predict_8x8_init_mmx(uint32_t cpu, x264_predict8x8_t pf[12],
x264_predict_8x8_filter_t *predict_8x8_filter)`
Fills all twelve luma 8x8 modes and the edge-filter function pointer.

### 3.5 `void x264_predict_4x4_init_mmx(uint32_t cpu, x264_predict_t pf[12])`
Fills the twelve luma 4x4 modes.

All functions return early when the required base CPU feature is absent,
leaving the portable C pointers installed by the caller in place.

### 3.6 Types
* `x264_predict_t`: `void (*)(pixel *src)`.
* `x264_predict8x8_t`: `void (*)(pixel *src, pixel edge[36])`.
* `x264_predict_8x8_filter_t`: `void (*)(pixel *src, pixel edge[36], int i_neighbor, int i_filters)`.

## 4. State and data structures

* Static constant tables (only when `HAVE_X86_INLINE_ASM`):
  * high-bit-depth: `pw_12345678 = {1..8}`, `pw_m87654321 = {-8..-1}`,
    `pw_m32101234 = {-3..4}` as `int16_t[8]`, 16-byte aligned.
  * 8-bit: `pb_12345678`, `pb_m87654321`, `pb_m32101234` as `int8_t[8]`,
    8-byte aligned.
* Generated static functions `predict_16x16_p_<isa>`, `predict_8x16c_p_<isa>`,
  `predict_8x8c_p_<isa>` for ISA names `mmx2` (32-bit 8-bit only), `sse2`,
  `ssse3` (8-bit inline-asm only), `avx` (8-bit), `avx2`.
* The function-pointer arrays `pf[]` are owned by the caller and written in
  place; no dynamic allocation.

## 5. Algorithm / behaviour

### 5.1 Plane-P parameter computation (general)
For an `WxH` block, the plane predictor is
`pred[x,y] = clip((i00 + b*x + c*y + 16) >> 5, 0, MAX)`.
The H.264 spec defines:
* `a = 16*(left[H-1] + top[W-1])`
* `b = (5*H_sum + 32) >> 6` for 16x16 (where `H_sum = Σ_{i=1..8} i*(top[7+i]-top[7-i])`);
  `(17*H + 16) >> 5` for 8x8c/8x16c (where `H` uses a 4-tap sum).
* `c` analogous from the left column: `(5*V+32)>>6` for 16x16, `(17*V+16)>>5`
  for 8x8c, `(5*V+32)>>6` for 8x16c.
* `i00 = a - b*(W/2-1) - c*(H/2-1) + 16` (8-bit) or `a` directly
  (high-bit-depth, where the core applies the offsets differently).

### 5.2 `PREDICT_P_SUM(j,i)`
Macro expanding to two gradient accumulations:
`H += i * (src[j+i-FDEC_STRIDE] - src[j-i-FDEC_STRIDE])` (top row symmetric
pairs around position `j`), and
`V += i * (src[(j+i)*FDEC_STRIDE-1] - src[(j-i)*FDEC_STRIDE-1])` (left column
symmetric pairs around row `j`).

### 5.3 `PREDICT_16x16_P`
1. `PREDICT_16x16_P_CORE` calls `PREDICT_P_SUM(7,i)` for `i=1..8`, accumulating
   `H` and `V` in scalar `int`s.
2. `PREDICT_16x16_P_END`:
   * `a = 16*(src[15*FDEC_STRIDE-1] + src[15-FDEC_STRIDE])`
   * `b = (5*H + 32) >> 6`, `c = (5*V + 32) >> 6`
   * `i00 = a - 7*b - 7*c + 16`
   * If `BIT_DEPTH > 8` and `i00 > 0x7fff` or `|b|>1092` or `|c|>1092`, fall
     back to the portable C `x264_predict_16x16_p_c(src)` to avoid 16-bit
     overflow in the SIMD core; otherwise call
     `x264_predict_16x16_p_core_<isa>(src, i00, b, c)`.
3. Inline-ASM variants (`PREDICT_16x16_P_ASM`) accelerate `H` with
   `pmaddubsw`/`pmaddwd` against `[1..8]`/`[-8..-1]` tables and horizontal
   reduction; `V` remains scalar but is combined with `H` by the same END macro.
   The SSSE3 inline variant uses the byte table with `palignr` to assemble the
   symmetric pair window.

### 5.4 `PREDICT_8x16C_P`
* `H = Σ_{i=0..3} (i+1)*(src[4+i-FDEC_STRIDE] - src[2-i-FDEC_STRIDE])`
* `V = Σ_{i=0..7} (i+1)*(src[-1+(i+8)*FDEC_STRIDE] - src[-1+(6-i)*FDEC_STRIDE])`
* High-bit-depth: `a = 16*(src[-1+15*FDEC_STRIDE] + src[7-FDEC_STRIDE])`,
  `b = (17*H+16)>>5`, `c = (5*V+32)>>6`, and the core receives `a` directly.
* 8-bit: same but `i00 = a - 3*b - 7*c + 16`.

### 5.5 `PREDICT_8x8C_P`
* Uses `PREDICT_P_SUM(3,i)` for `i=1..4`.
* `a = 16*(src[7*FDEC_STRIDE-1] + src[7-FDEC_STRIDE])`,
  `b = c = (17*H/V + 16) >> 5`.
* 8-bit: `i00 = a - 3*b - 3*c + 16`; high-bit-depth passes `a`.
* Inline-ASM variant computes `H` with `pmaddubsw` against
  `pb_m32101234 = {-3,-2,-1,0,1,2,3,4}` and adds `-4*src[-FDEC_STRIDE-1]`
  (the corner term folded into the SIMD sum). `V` is scalar.

### 5.6 `predict_8x8c_dc_left` (C fallback, 64-bit 8-bit only)
1. Sum the four left samples of the top half (`s0`) and bottom half (`s1`).
2. `dc0 = ((s0+2)>>2)*0x0101010101010101ULL`, `dc1` similarly.
3. Write `dc0` to the first four rows and `dc1` to the last four rows with
   `M64(src)` (64-bit stores).

### 5.7 Dispatch functions
Each init function follows the same pattern:
1. Check the base feature (MMX2 for 16x16/4x4/8x8, MMX for chroma); return if
   absent.
2. Install the lowest-level SIMD functions for all modes available at that
   level.
3. Progressively check higher features (SSE, SSE2, SSSE3, AVX, AVX2,
   `SSE2_IS_SLOW`, `SLOW_PSHUFB`, `SLOW_PALIGNR`, `CACHELINE_64`) and overwrite
   the relevant pointers with faster variants.
4. `HAVE_X86_INLINE_ASM` gates the ssse3/avx `p` wrappers; high-bit-depth uses
   sse2/avx wrappers computed with `pmaddwd`.

Notable choices:
* 16x16 V uses `sse` then `avx`; H uses `mmx2`, then `sse2` (high-bit-depth)
  or `ssse3` (8-bit), then `avx2`.
* 16x16 DC/DC_TOP/DC_LEFT move to `avx2` last.
* The `p` mode for 16x16: `mmx2` (32-bit 8-bit), `sse2`, `ssse3` (8-bit inline),
  `avx` (8-bit), `avx2`.
* 8x8 luma: starts at `mmx2` for V/H/DC/DC_TOP/DC_LEFT/HD/VL and the filter;
  `ddl`/`ddr`/`vr`/`hu` require `ARCH_X86` in 8-bit. SSE2 adds ddl/vl/vr/ddr/
  hd/hu; SSSE3 conditionally overrides ddl/vr (`SLOW_PALIGNR`) and hu; AVX
  overrides the four directional modes and the filter. Cacheline-64 CPUs get
  `ddl_cache64_ssse3`/`ddr_cache64_ssse3`.
* 4x4: MMX2 installs DC/DDL/DDR/VL/HD/HU and VR (8-bit); SSSE3 overrides
  DDR/VR/HD (and `vr_cache64_ssse3` on cacheline-64); high-bit-depth also gets
  SSE2/SSSE3/AVX/AVX2 variants; `h_avx2` requires AVX2.

## 6. Dependencies

* Includes `common/common.h`, `predict.h`, `pixel.h`.
* Calls assembly kernels declared in `predict.h` and implemented in
  `predict-a.asm`.
* Falls back to `x264_predict_16x16_p_c` from `common/predict.c` in the
  high-bit-depth overflow case.
* Uses CPU feature bits `X264_CPU_MMX`, `MMX2`, `SSE`, `SSE2`, `SSE2_IS_SLOW`,
  `SSSE3`, `SLOW_PSHUFB`, `SLOW_PALIGNR`, `AVX`, `AVX2`, `CACHELINE_64`.

## 7. Constants and tables

* `pw_12345678`, `pw_m87654321`, `pw_m32101234` (high-bit-depth, int16_t).
* `pb_12345678`, `pb_m87654321`, `pb_m32101234` (8-bit, int8_t).
* Numeric algorithm constants: `16`, `5`, `17`, `32`, `7` (16x16 offsets),
  `3`, `7` (8x16c/8x8c offsets), `0x7fff`, `1092` (overflow thresholds),
  `0x0101010101010101ULL` (byte replication for DC fallback).

## 8. Performance notes

* **Inline ASM:** The `PREDICT_*_P_ASM` macros compute the horizontal gradient
  sum in 2–4 SIMD instructions plus a horizontal reduction, replacing an eight-
  iteration scalar loop. This runs once per plane-mode candidate, so its cost is
  small but measurable at high bit depths.
* **Wider vectors:** Dispatch prefers AVX2 for plane-P cores and the simple
  modes; AVX2 `p_core` processes 16/32 columns at once.
* **Transpose strategy:** Not applicable here (no transposes); the inline ASM
  assembles symmetric edge pairs with `palignr`/`punpck`.
* **Avoiding spills:** The generated wrappers keep `H`, `V`, `a`, `b`, `c` in
  scalar registers and pass three arguments to the SIMD core; no stack frames
  beyond the ABI prologue.
* **Preprocessing constants:** Sign/weight tables are `ALIGNED_16`/`ALIGNED_8`
  static constants; the dispatcher checks CPU flags once at init.
* **Branch behaviour:** Dispatch is one-time; the overflow fallback branch in
  `PREDICT_16x16_P_END` is extremely rare by design (the comment explicitly
  notes it is cheaper to branch away than to widen the SIMD core).
* **Hotness:** Plane-P setup is cold relative to the SIMD core; dispatch
  accuracy matters more than setup speed.

## 9. References

* ITU-T H.264 intra prediction specification (plane mode formulas).
* Intel Intrinsics Guide for `pmaddubsw`, `pmaddwd`, `palignr`.
* Public x264 source history for CPU-feature dispatch policy.

## 10. Verification checklist

* For each ISA level, verify `pf[]` contains the expected function pointer by
  calling the init function with a synthetic `cpu` bitmask and comparing to the
  declarations in `predict.h`.
* Verify plane-P outputs: call each generated `predict_*_p_<isa>` through its
  wrapper with random edge samples and compare to the portable C plane predictor.
* Verify the high-bit-depth overflow fallback: construct edge values that make
  `i00 > 0x7fff` or `|b|>1092` and confirm the C path is taken and produces
  clipped results.
* Verify `predict_8x8c_dc_left` writes exactly four rows of `dc0` and four of
  `dc1`, with rounding `(s+2)>>2`.
* With `HAVE_X86_INLINE_ASM` on/off, confirm identical outputs from the two
  builds for the same inputs.

## 11. Open questions

* `UNKNOWN`: exact values of the `x264_predict_t` enum indices
  (`I_PRED_16x16_*`, `I_PRED_CHROMA_*`, `I_PRED_8x8_*`) — defined in
  `common/predict.h`, not in this file.
* `UNKNOWN`: whether `PREDICT_8x16C_P_END` high-bit-depth passing `a` (rather
  than `i00`) is a semantic difference in `predict_8x16c_p_core_sse2` that
  must be mirrored exactly; verify against the assembly.
* `UNKNOWN`: precise CPU flag combinations that enable `ssse3_atom` or
  `SLOW_PSHUFB` paths for prediction (handled outside this file).
