# common/x86/util.h — behavioural specification

**Role:** C header exposing a handful of hand-written MMX inline-asm helpers and an SSE union type used by the x86 motion-estimation and CABAC code.
**Marker:** dirty-side behavioural specification.

---

## 1. Header

- Source path: `common/x86/util.h`
- Role: architecture-specific utility header; defines `M128_ZERO`, `x264_union128_t`, an optional GCC vector type, and four `ALWAYS_INLINE` MMX2 functions selected by macro redirection when `HAVE_X86_INLINE_ASM && HAVE_MMX`.
- Guarded by `X264_X86_UTIL_H`.

---

## 2. Purpose

Some operations are used so frequently in the hot motion-estimation loop (median of motion vectors, predictor-difference cost, CABAC MVD sum, predictor clipping/rounding/clipping) that calling an out-of-line function would cost more than the work itself. On x86 with MMX2 available, this header redirects the generic C names to inline MMX implementations that process several values in parallel. It also supplies a union type for type-punning between SSE/64-bit/32-bit/16-bit/8-bit views of a 128-bit value, used by the inline-asm macros in `mc-c.c`.

---

## 3. Public interface

### 3.1 Types and constants (SSE builds)
- `M128_ZERO` — a compound literal zero `__m128` (`{0,0,0,0}`), redefined after undefining any prior definition.
- `x264_union128_t` = `x264_union128_sse_t`: a `MAY_ALIAS` union with members `__m128 i; uint64_t q[2]; uint32_t d[4]; uint16_t w[8]; uint8_t b[16];`.
- When `HAVE_VECTOREXT`: `v4si` — a GCC vector extension type of four `uint32_t` with 16-byte size.

### 3.2 Inline functions (only when `HAVE_X86_INLINE_ASM && HAVE_MMX`)
Each is exposed by defining the generic name to the suffixed one:

- `x264_median_mv( int16_t *dst, int16_t *a, int16_t *b, int16_t *c )` → `x264_median_mv_mmx2`
  - Computes the component-wise median of three MV pairs into `dst`. Contract: exactly two int16 values are written (a 32-bit store); the three inputs are each 32-bit memory operands.
- `x264_predictor_difference( int16_t (*mvc)[2], intptr_t i_mvc )` → `x264_predictor_difference_mmx2`
  - Returns the sum over the predictor list of `abs(a−b)` folded to a scalar: for each of the `i_mvc` 2-component predictors it computes the absolute difference of its components, accumulates the unsigned-saturated word sums, multiplies by an all-ones word vector, and horizontally adds to an `int`. The odd-count case (first element handled separately) is explicit.
- `x264_cabac_mvd_sum( uint8_t *mvdleft, uint8_t *mvdtop )` → `x264_cabac_mvd_sum_mmx2`
  - Returns a 16-bit cost derived from two 4-byte MVD vectors: sums corresponding bytes with unsigned saturation, clamps to 0x21, compares against thresholds 2 and 0x20 to form a bitmask of "large MVD" flags, and returns that mask as `uint16_t`.
- `x264_predictor_clip( int16_t (*dst)[2], int16_t (*mvc)[2], int i_mvc, int16_t mv_limit[2][2], uint32_t pmv )` → `x264_predictor_clip_mmx2`
  - Clips each predictor MV to the per-frame vertical/horizontal limits (the limit array is read as a 64-bit value and shifted left 2 to convert to subpel units), drops predictors equal to the predicted MV `pmv` or equal to zero, writes the surviving MVs to `dst`, and returns the new count. The last element is handled by a separate tail path.
- `x264_predictor_roundclip( ... same signature ... )` → `x264_predictor_roundclip_mmx2`
  - Same as above but first rounds each incoming MV with `(mv + 2) >> 2` (word add of 2, arithmetic shift right 2) before the equality/clip tests.

All five use GCC-style extended asm with explicit clobber lists (MMX registers and `cc`, or `mm0..mm7` and `cc`).

---

## 4. State and data structures

- No persistent runtime state; all functions are inline leaf code.
- Constants embedded in each function:
  - `pw_1 = 0x0001000100010001` (word all-ones, for horizontal folding of predictor differences).
  - `pb_2 = 0x0202020202020202`, `pb_32 = 0x2020202020202020`, `pb_33 = 0x2121212121212121` (thresholds for MVD cost).
  - `pd_32 = 0x20` (32, used as a mask selector for skipped predictors).
  - `pw_2 = 0x0002000200020002` (rounding addend for roundclip).
- Memory contracts: `mvc` is an array of 16-bit MV pairs (4 bytes each); `dst` receives 4-byte pairs; `mv_limit` is a 2x2 int16 array (8 bytes) with vertical and horizontal limits; `pmv` packs an MV pair into a 32-bit integer.

---

## 5. Algorithm / behaviour

### 5.1 Median of three MVs
Load the three 32-bit MV pairs into MMX registers. Compute per-component maxima and minima: the median of three values is `max(min(a,b), min(max(a,b), c))` — implemented as `pmaxsw` of `pminsw(a,b)` and `pminsw(pmaxsw(a,b),c)`. Store the 32-bit result. This is the classic SIMD median-of-three.

### 5.2 Predictor difference
Zero an accumulator. If `i_mvc` is odd, handle the first predictor by subtracting its second component from its first (a `movd` of each 4-byte pair, then `psubw`), then enter the main loop. For each remaining pair of predictors, subtract component 1 from component 0, take absolute value (`psubw` from zero then `pmaxsw`), and add the saturated word sum into the accumulator (`paddusw`). After the loop, multiply the word lanes by one (`pmaddwd` with `pw_1`), fold the two dwords (`pshufw` + `paddd`), and return the low 32 bits. The result is the total subpel-domain absolute difference across the predictor list.

### 5.3 CABAC MVD sum
Load 16 bits from each of the two MVD arrays, add with `paddusb`, and clamp with `pminub 0x21`. Compare byte-wise against thresholds 2 and 0x20 using signed `pcmpgtb`; subtract the comparison masks from zero to form flags; combine and return the low 16 bits. The returned value encodes which of the four (left/top × x/y) MVD magnitudes are "large", used to bias CABAC context selection.

### 5.4 Predictor clipping
Interpret the limit pair as two 16-bit values, shift left by 2 (subpel conversion) and duplicate across word lanes; duplicate `pmv` similarly. For each MV in the list, compute equality masks against `pmv` and against zero, combine them, and use `pmovmskb` to test per-MV validity, then clamp component-wise to `[−limit, +limit]` with `pmaxsw`/`pminsw`. Depending on validity, the MV is shifted in the packed register stream (skipped entries are removed by a `psrlq` with a mask-derived count) and stored to `dst`; the returned count is incremented for each valid MV. A specialized tail handles the final odd MV.

### 5.5 Predictor roundclip
Identical structure to 5.4 but inserts `paddw 2` and `psraw 2` on each MV before the equality/clip logic, implementing `(mv+2)>>2` rounding.

---

## 6. Dependencies

- Included by `common/me.c` / `common/analyse.c` (median, predictor difference/clip) and `common/cabac.c` (MVD sum) on x86 builds.
- Depends on `common/common.h` for `MAY_ALIAS`, `ALWAYS_INLINE`, `M32/M16/MEM_DYN` macros and the basic integer types; on `<xmmintrin.h>` when `__SSE__` is defined.
- The functions are selected by generic-name macros, so the generic C implementations in the corresponding `.c` files serve as the fallback when inline asm is unavailable.

---

## 7. Constants and tables

All constants are inline 64-bit immediates defined in section 4; there are no external table dependencies. They are chosen so that repeated byte/word patterns fit one MMX register.

---

## 8. Performance notes

Inlining removes call overhead and lets the compiler keep values in registers between ME/CABAC stages. The MMX paths process two MV components or four MVD bytes per instruction; the original C fallbacks do these serially. Because MMX state must be cleared before floating-point code runs, the caller must invoke `x264_emms()` appropriately (see `cpu-a.asm` spec) — a classic MMX pitfall.

**AVX2/AVX-512 vectorization opportunities and bottlenecks (with references):**
- These helpers operate on only two to eight small integers; a modern rebuild could use SSE2/AVX2 scalar-vector instructions (`pextrw`, `pmovmskb`) to avoid MMX state entirely and remove EMMS transitions — Intel SDM warns that leaving MMX state dirty corrupts x87/SSE code.
- `x264_predictor_difference` accumulates at most a handful of terms; wider vectors do not help unless the predictor list is long, so the bottleneck is the horizontal reduction, not throughput. A rebuild on AVX-512 could keep the list in one ZMM and use a mask-register reduction (`vpsadbw`, `vpcompressd`).
- `x264_cabac_mvd_sum`'s threshold comparisons map naturally to byte-wide masked compare (`vpcmpub` on AVX-512) plus `vpmovmskb`, replacing three MMX instructions with two.
- Bottleneck: MMX `pmovmskb`→GPR round trips have ~2–3 cycle latency on modern CPUs; AVX-512 mask registers remove the GPR round trip (Intel Optimization Reference Manual).

---

## 9. References

- Intel SDM Vol. 2: `paddusw`, `pmaddwd`, `pmovmskb`, `pminub`, `pcmpgtb`, `pshufw`.
- AMD64 / System V ABI: inline-asm clobber conventions.
- Agner Fog, *Optimizing subroutines in assembly language* (MMX/SSE transition costs).

---

## 10. Verification checklist

1. For random triples of MV pairs, `x264_median_mv` must equal the component-wise median of the C implementation for all 16-bit values including sign boundaries.
2. For random predictor lists of lengths 1..16, `x264_predictor_difference` must equal the scalar sum of per-predictor absolute differences.
3. For random MVD arrays, `x264_cabac_mvd_sum` must match the C fallback bit-for-bit in its 16-bit result.
4. For random MVs and limits, `x264_predictor_clip` and `x264_predictor_roundclip` must produce exactly the same surviving list and count as the C versions, including the `pmv == 0` and limit-clamping edge cases.
5. Verify no x87/SSE corruption: call an inline helper then execute floating-point code without an intervening EMMS in the test harness (MMX users must call `x264_emms()`).
6. Compile with `HAVE_X86_INLINE_ASM=0` and confirm the generic C paths produce identical results.

---

## 11. Open questions

- The exact semantics of `x264_cabac_mvd_sum`'s returned bit pattern (which bit corresponds to which MVD) is only described by the instruction sequence; UNKNOWN which CABAC context it maps to without reading `cabac.c`.
- `HAVE_VECTOREXT` `v4si` type usage sites are not in this header; UNKNOWN which kernels consume it.
- Whether `x264_predictor_roundclip` is still reachable in current encoder versions is UNKNOWN (no call site in the assigned files).
