# common/x86/mc-a.asm — behavioural specification

**Role:** x86 SIMD kernels for block averaging, explicit and implicit weighted prediction, offset addition/subtraction, plain block copy, prefetching, and chroma motion compensation (all interpolation cases).
**Marker:** dirty-side behavioural specification.

---

## 1. Header

- Source path: `common/x86/mc-a.asm`
- Role: the "first half" of x86 MC; `mc-a2.asm` supplies the half-pel filters and plane-copy/integral/mbtree helpers, and `mc-c.c` supplies the C-level dispatch and qpel composition.
- Contains both 8-bit and high-depth code paths, selected by `HIGH_BIT_DEPTH` at assembly time.

---

## 2. Purpose

Motion compensation in H.264 needs: copying integer-pel blocks; averaging two references (biprediction); applying per-reference weights and offsets (weighted prediction); and chroma interpolation with fractional offsets. This file implements all of those as SIMD kernels, with ABI-aware argument marshalling and ISA-specific fast paths (SSSE3 `pmaddubsw`/`pmulhrsw`, AVX2 3-operand forms, AVX-512 masked broadcasts), plus cacheline-split-safe averaging and prefetch helpers.

---

## 3. Public interface

Signatures (all private-prefixed, CPU-suffixed):

- `void pixel_avg_WxH( pixel *dst, intptr_t dst_stride, pixel *src1, intptr_t src1_stride, pixel *src2, intptr_t src2_stride, int weight )`
  - Averages two equally sized blocks. `weight` selects the rounding variant: 32 means "rounding average" (`pavgb`/`pavgw`), 0 means "truncating average" (used by qpel interpolation). Dispatches to the width-specific `pixel_avg_wN` when `weight==32`, else to `pixel_avg_weight_wN`.
- `void pixel_avg_wN( pixel *dst, intptr_t, pixel *src1, intptr_t, pixel *src2, intptr_t, int height )` — two-row average, width N∈{4,8,16} (8-bit also 12/20 via aliases).
- `void pixel_avg_weight_wN( pixel *dst, intptr_t, pixel *src1, intptr_t, pixel *src2, intptr_t, int i_weight )` — implicit weighted biprediction: `dst = (src1*w + src2*(64−w) + 32) >> 6`, `log2_denom=5`, `offset=0`, `w1+w2=64`. Height is taken from a fixed context in the caller; the `i_weight` argument is the weight byte.
- `void mc_weight_wN( pixel *dst, intptr_t i_dst, pixel *src, intptr_t i_src, const x264_weight_t *weight, int h )` — explicit weighted prediction, `dst = clip((src*scale + offset*2^denom + 2^(denom−1)) >> denom)`, with a fast path when the shift can be folded into the scale factor.
- `void mc_offsetadd_wN` / `mc_offsetsub_wN( pixel *src, intptr_t i_src, pixel *dst, intptr_t i_dst, const x264_weight_t *w, int h )` — add/subtract `abs(offset)` with saturation when `scale == 1<<denom`.
- `void mc_copy_wN( pixel *dst, intptr_t i_dst, pixel *src, intptr_t i_src, int h )` — plain copy, widths 4/8/16(20/18 high-depth aliases).
- `void prefetch_fenc_{400,420,422}( pixel *pix_y, intptr_t stride_y, pixel *pix_uv, intptr_t stride_uv, int mb_x )`, `void prefetch_ref( pixel *pix, intptr_t stride, int parity )` — issue `prefetcht0` for future macroblock/reference data.
- `void mc_chroma( pixel *dstu, pixel *dstv, intptr_t i_dst, pixel *src, intptr_t i_src, int dx, int dy, int i_width, int i_height )` — chroma interpolation for dx,dy ∈ 0..7, widths 4 or 8, writing U and V planes. May write up to two bytes of garbage past `dst` (documented in `mc.h`), so callers must run left-to-right.

---

## 4. State and data structures

- `x264_weight_t` (defined in `common/mc.h`): 16-byte aligned struct with `int16_t cachea[8]` (scale or offset), `int16_t cacheb[8]` (offset/rounding), `int32_t i_denom/i_scale/i_offset`, and `weight_fn_t *weightfn`. The assembly reads `cachea/cacheb` as packed word vectors from `r4`.
- `AVG_START`/`AVG_END`: ABI-specific argument marshalling. Win64 declares 7 registers and aliases t0..t5 to r0..r5; Unix64 declares 9 and uses t6/t7 as scratch; 32-bit loads all six args from stack slots into t0..t5. `AVG_END` advances the row pointers by `2*stride*SIZEOF_PIXEL` and loops `eax` rows down.
- `DECLARE_REG_TMP` aliases used for weight/chroma functions differ per ABI.
- Constants: `ch_shuf`, `ch_shuf_adj`, `sq_1` in local rodata; `cextern` of `pb_0`, `pw_1`, `pw_4`, `pw_8`, `pw_32`, `pw_64`, `pw_512`, `pw_00ff`, `pw_pixel_max`, `sw_64`, `pd_32`, `deinterleave_shufd`.

---

## 5. Algorithm / behaviour

### 5.1 Implicit weighted biprediction (`pixel_avg_weight_wN`)
- 8-bit MMX: `BIWEIGHT_START_MMX` loads weight `w` (byte at `r6m`) into a splatted word `m2`, computes `64−w` in `m3`, and sets `m4=32` (rounding). `BIWEIGHT_MMX` loads 8 bytes from each source, unpacks to words, multiplies by the two weights (`pmullw`), adds, adds 32, shifts right 6 (`psraw`), packs back to bytes with unsigned saturation. SSSE3 replaces this with `pmaddubsw` on interleaved bytes and `pmulhrsw` with a 512-scaled rounding vector (`pw_512`), which performs `(x+32)>>6` in one instruction.
- High-depth MMX/SSE2: pairs of words are unpacked to dwords and multiplied by a packed `pmaddwd` factor `[w<<16 | (64−w)]`, then `+32`, `>>6`, packed back with signed clamping to `[0, PIXEL_MAX]`.
- AVX2 (8-bit, w16): loads two rows of each source into YMM via `movu`+`vinserti128`, interleaves bytes with `SBUTTERFLY bw`, applies `pmaddubsw`/`pmulhrsw`, packs and writes with `mova`+`vextracti128`.
- AVX-512: a w8 ZMM variant uses mask `k1` and masked `vpbroadcastq` to place two rows' pixels into lanes; a w16 ZMM variant assembles four rows with `vinserti128`/`vinserti32x4` and uses `SBUTTERFLY bw`, `pmaddubsw`, `pmulhrsw`, extracting rows with `vextracti128/32x4`.

### 5.2 Explicit weighted prediction (`mc_weight_wN`)
- High depth: `WEIGHT_START` loads `1<<denom` into `m0` (multiplier), `m3` from `r4+16` (second scale), `m2 = denom+1` (`sq_1` added). `WEIGHT` unpacks two words to dwords and multiplies by the cached scale with `pmaddwd`, shifts by `m2`, packs with `packssdw` and clips via `CLIPW` to `[pb_0, pw_pixel_max]`. Two rows are processed per loop iteration.
- 8-bit SSSE3: `pmulhrsw` with a pre-shifted scale (`<<7`) implements the rounding multiply when `cacheb[0]==0` (the `.fast` path chosen by `cmp byte [r4+1],0`), otherwise `pmullw`+`paddsw`+`psraw m5` (shift amount from `r4+32`) is used. The "fast" check tests whether `(scale<<7)` fits in int16.
- Width tail handling: full `mmsize` chunks use `WEIGHT_ROWx2`; remaining bytes use `WEIGHT_COL`, which loads one row from each of two source rows (`movh` or `vinserti128`), unpacks, multiplies, packs, and writes with `movq`/`movd` depending on width (4/8/20). Width 20 is treated as 16 in the AVX2 column path to avoid overreading.

### 5.3 Offset add/sub (`mc_offset{add,sub}_wN`)
`OFFSET_OP` loads two rows, applies `paddusb`/`psubusb` (8-bit) or `paddusw`/`psubusw` (high depth, with `pminsw` to `pw_pixel_max` after adds), and stores. The offset vector `m2` is loaded once from `r4->cachea`. Full-vector chunks use aligned/native loads; the tail uses half-width forms (`movh` for high depth, `movd` for 8-bit). Instantiated for widths 4/8/12/16/20 (MMX2) and 8/12/16/20 (SSE2), plus high-depth 8.

### 5.4 Averaging (`pixel_avg_wN`, `pixel_avg_WxH`)
`AVG_FUNC` loads two rows from src1, averages with src2 using `pavgb`/`pavgw` (rounding by default), and stores two rows. `AVGH` is a dispatcher: if `r6m != 32` tail-jump to `pixel_avg_weight_wN`; for width 16 with AVX2 jump to the unaligned-optimized `pixel_avg_w16_avx2`; with SSE2 and 16-byte-aligned src1 jump to the aligned variant; otherwise use the MMX variant. Widths 4/8/16 and heights 2/4/8/16 (8-bit also via SSSE3/AVX-512 aliases). High depth uses word averaging (`pavgw`) and separate `movq` loads.

### 5.5 `pixel_avg2_wN` (two-source average without weight argument)
`AVG2_W_ONE`/`AVG2_W_TWO` compute `dst = avg(src1, src2)` for two consecutive rows per iteration where `src2 = src1 + r4` (r4 is pre-subtracted from r2 so it becomes the delta). High-depth widths 4/8/10/16/18 (MMX2/SSE2/AVX2), 8-bit widths 4/8/12/16/20 (MMX2/SSE2/AVX2). Specialised functions handle w10/w18 tails with `movd`/`movhps`. Cacheline-split variants (MMX2 only for simplicity, since src1/src2 can have different alignments) use `INIT_SHIFT` to compute complementary shifts (`64−offset`) and realign with `psrlq`/`psllq`/`por` before `pavgb`. The SSSE3 w16 cache64 path uses a computed jump over 16×16=256 specialised 48-byte loops built by `AVG16_CACHELINE_LOOP_SSSE3` with `palignr` for both sources.

### 5.6 Block copy (`mc_copy_wN`)
`COPY1`/`COPY2`/`COPY4` load four rows (`movu`) and store with `mova`. `mc_copy_w4_mmx` handles height 4 specially (copies 8 rows via two `COPY1` calls for larger heights). `MC_COPY` computes `w*SIZEOF_PIXEL/mmsize` and emits the corresponding `COPY` macro; instantiated for MMX/SSE/aligned-SSE and (high depth) AVX/aligned-AVX.

### 5.7 Prefetch
`prefetch_fenc_400/420/422`: compute a 64-byte offset from `mb_x&3` times the luma stride, issue four `prefetcht0`s for luma rows, then two (420) or four (422) for chroma; 400 has no chroma. 32-bit path loads arguments from the stack. `prefetch_ref`: computes `((parity−1) & stride)*8 + 64` and prefetches eight rows spanning two parity groups. All assume 64-byte cache lines; the source notes a FIXME for high-depth/4:4:4 coverage.

### 5.8 Chroma MC (`mc_chroma`)
- `MC_CHROMA_START` computes the source offset `(dx>>3) + (dy>>3)*src_stride` and adds it to `src`; dx/dy are then masked to 0..7.
- Generic path (`MC_CHROMA`): builds the four filter coefficients `(x<<24)+((8−x)<<16)+(y<<8)+(8−y)` in a dword and splats to words. For each output pixel it loads a 2×2 source neighbourhood, splits into even/odd bytes (`pand [pw_00ff]`, `psrlw 8`), applies `pmaddwd` with the coefficient vector, then combines the vertical partial sums with `pmullw` by the horizontal weights, adds rounding `[pw_32]`, shifts right 6, packs to bytes and writes U/V. It has separate `.loop2` (width 4), `.width4`, `.width8` and `.loop4` paths, plus a `.mc1d` fast path when only dx or only dy is nonzero (weights are just 8 and the fractional value, with `+4` rounding). High-depth variants use `UNPACK_UNALIGNED` on 16-bit pixels and `SBUTTERFLY wd` to organise pairs.
- SSSE3 path (`MC_CHROMA_SSSE3`): replaces the byte splitting with `pshufb` (`ch_shuf`, optionally adjusted by `ch_shuf_adj` for cache64 CPUs), and the multiply/round with `pmaddubsw`+`pmulhrsw [pw_512]`. The four coefficients are formed as `(x*255+8)*y` and `(x*255+8)*(8−y)` and splatted. Widths 4/8 have separate loops; sse4 uses `pextrd` to store the four interleaved U/V results. AVX2 uses `vpbroadcastw` and `vinserti128` to process two rows per iteration and `vpermd` with `deinterleave_shufd` to separate U/V.

### 5.9 Dispatch
`mc-c.c: x264_mc_init_mmx` installs these based on CPU flags: MMX2 baseline for avg/weight/copy/prefetch/chroma; SSE for memcpy/memzero/plane copy; SSE2 for fast paths and high-depth; SSSE3 for `pmaddubsw` weight and chroma; AVX for chroma; AVX2 for w16 weight, w16 avg, chroma; AVX-512 for w8/w16 avg. `STACK_MOD4` disables SSE2/SSSE3 chroma because it needs 16-byte stack alignment.

---

## 6. Dependencies

- Includes `x86inc.asm`, `x86util.asm`; uses `SBUTTERFLY`, `FIX_STRIDES`, `movnta`, `DECLARE_REG_TMP`, `psadbw`-free vector arithmetic.
- `cextern` constants from `const-a.asm`; local rodata `ch_shuf`, `ch_shuf_adj`, `sq_1`.
- Called from `common/x86/mc-c.c` through `x264_mc_functions_t`; `mc-c.c` also composes qpel interpolation from these avg/copy functions.
- `mc-a2.asm` provides the half-pel filter generation used by `mc_luma`.

---

## 7. Constants and tables

- `ch_shuf`: `db 0,2,2,4,4,6,6,8,1,3,3,5,5,7,7,9` (×2) — gathers source neighbours for chroma `pshufb`.
- `ch_shuf_adj`: eight 0s then eight 2s — offset adjustment applied with `paddb` when `cache64` is set.
- `sq_1`: one dword 1 — added to `denom` for the high-depth weight shift count.
- `pw_512` (512 = 2^9) is the `pmulhrsw` rounding multiplier giving `(x+32)>>6`; `pw_32` is the rounding add for the generic shift path; `pw_64` provides `64−w`; `pw_pixel_max` clips high-depth results.

---

## 8. Performance notes

- Biprediction is extremely hot in B-frames; `pmaddubsw`+`pmulhrsw` (SSSE3) replaces a dozen MMX instructions and is the single most important optimisation here.
- Averaging wide blocks benefits from AVX2/AVX-512 because one instruction covers 32/64 bytes; the width-16 AVX2 avg path avoids cacheline checks that older CPUs needed.
- The cacheline-split avg paths are only selected on flagged CPUs; they trade a branch for avoiding multi-cycle split loads.
- Chroma MC has complex data movement; the SSSE3/AVX2 shuffle-based paths are much faster than the generic byte-splitting path.
- Non-temporal prefetches use `prefetcht0`/`prefetchnta` judiciously; excessive prefetch distance hurts (source comments note 4 iterations ahead is arbitrary).

**AVX2/AVX-512 vectorization opportunities and bottlenecks (with references):**
- AVX-512's masked `vpbroadcastq/32` lets `pixel_avg_weight_w8` process a full 8-pixel row in one ZMM with no tail loop, a direct advantage over AVX2 (Intel Intrinsics Guide, masked broadcasts).
- Embedded broadcasts such as `vpbroadcastw m3, t7d` remove a register load in chroma MC; a rebuild should use them where the scalar weight is loop-invariant.
- `pshufb` on ZMM requires a 64-byte mask (`vbroadcasti32x8` of the 16-byte `ch_shuf`); the AVX-512 chroma path is not instantiated here — UNKNOWN whether it would be profitable. The AVX2 path (`vpermd`) is the widest provided.
- Bottleneck: `pmaddubsw` on 512-bit lanes doubles work but its result is 16-bit, so `pmulhrsw` must be applied before packing; the sequence is fixed by data dependencies (Intel SDM).
- Bottleneck: chroma MC's `vpermd`/`pshufb` cross-lane shuffles cost multiple uops on AVX2; AVX-512's `vpermt2w` can combine two shuffles (as in `mc-a2.asm` v210) — a concrete optimisation for a rebuild (Intel Optimization Reference Manual).

---

## 9. References

- ITU-T H.264 (AVC) standard, clauses on weighted prediction, biprediction and chroma sample interpolation (8-tap/linear filters).
- Intel SDM Vol. 2: `pavgb`, `pavgw`, `pmaddubsw`, `pmulhrsw`, `pshufb`, `vpbroadcast*`, `vpermd`.
- Intel Intrinsics Guide for weighted-average emulation (`_mm256_maddubs_epi16`, `_mm256_mulhrs_epi16`).
- Agner Fog, *Optimizing subroutines in assembly language* (cacheline split penalties, shuffle throughput).

---

## 10. Verification checklist

1. `pixel_avg_weight_wN`: for random src1/src2 and weights 0..64, verify against scalar `(s1*w + s2*(64−w) + 32)>>6` clipped to pixel range; test w=32 (rounding), w=0/64 (copy).
2. `mc_weight_wN`: verify against a scalar weighted-prediction reference for random `i_scale/i_denom/i_offset`, including the offset-only fast path (`scale==1<<denom`) and negative offsets for `offsetsub`.
3. `mc_offset{add,sub}_wN`: verify clipping at 0 and `PIXEL_MAX` for high depth and saturation at byte bounds for 8-bit.
4. `pixel_avg2_wN`: verify against scalar rounding average for all instantiated widths including the 10/18 high-depth tails.
5. `mc_copy_wN`: verify exact byte-for-byte copy for all heights and widths, including the `mc_copy_w4` height-4 special case.
6. `mc_chroma`: exhaustively verify all 64 (dx,dy) pairs for widths 4/8 and several heights against the H.264 scalar chroma interpolation formula; check U and V separately and confirm the ≤2-byte overrun past `dst` is harmless (guard pages in the harness).
7. Prefetch functions: issue them with varied `mb_x` and confirm no faults; observable correctness is limited to cache behaviour, so verify addresses by instrumenting/disassembly.
8. Run all variants under `checkasm` with CPU flags forcing each ISA.

---

## 11. Open questions

- `pixel_avg_weight_wN` takes `i_weight` as an argument but the implicit formula in the file comment fixes `log2_denom=5, offset=0, w1+w2=64`; UNKNOWN whether the argument is ever anything other than the byte-encoded weight and how the high byte is interpreted.
- The exact chroma rounding (H.264 requires `(a+...+32)>>6` for quarter-pel cases) is implemented with `[pw_512]`/`[pw_32]`; UNKNOWN whether all dx/dy combinations match the standard's rounding without executing the exhaustive test in §6.
- No AVX-512 chroma MC variant is defined here; UNKNOWN if the reference relies solely on the AVX2 variant on AVX-512 machines.
