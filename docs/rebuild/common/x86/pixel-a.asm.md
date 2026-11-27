# `common/x86/pixel-a.asm` — x86 pixel comparison / distortion metrics

**Dirty-side behavioural specification.** Source path:
`common/x86/pixel-a.asm`. Role: the main x86 SIMD implementation of the pixel
metrics used by motion estimation, rate–distortion decision, intra prediction,
adaptive quantisation, and SSIM.

---

## 1. Header

Large assembly source (over 5000 lines in the reference) built for both 8-bit
and high-bit-depth x86 (32- and 64-bit). It includes `x86inc.asm` and
`x86util.asm`, declares an extensive `.rodata` section with shuffle/sign/mask
tables and SSIM constants, and instantiates each kernel family across many ISA
levels via macros. SAD kernels are *not* here (they live in `sad-a.asm` /
`sad16-a.asm`); this file covers SSD, variance, SATD, SA8D, their fused forms,
intra-mode evaluation, SSIM, ASD, and ADS.

## 2. Purpose

Motion estimation and mode decision call these kernels millions of times per
frame, so each is highly tuned. The file provides multiple ISA variants so the
dispatcher can select the fastest available implementation, with careful
attention to data layout (byte vs word, interleaved U/V planes), rounding, and
overflow.

## 3. Public interface

All symbols are declared in `pixel.h.md`. Families present here:

### 3.1 `pixel_ssd_WxH` — sum of squared differences
* `(int pixel_ssd_WxH(pixel *pix1, intptr_t stride1, pixel *pix2, intptr_t stride2))`.
* 8-bit: sizes 4x4, 4x8, 4x16, 8x4, 8x8, 8x16, 16x8, 16x16; ISAs MMX,
  SSE2 (`sse2`, `sse2slow`), SSSE3, AVX, XOP, AVX2.
* High-bit-depth: same sizes; ISAs MMX2, SSE2, AVX2.
* Return: `int` SSD (sum over all pixels of `(pix1-pix2)^2`).

### 3.2 `pixel_ssd_nv12_core`
* `(void pixel_ssd_nv12_core(pixel *pixuv1, intptr_t stride1,
  pixel *pixuv2, intptr_t stride2, int width, int height,
  uint64_t *ssd_u, uint64_t *ssd_v))`.
* SSE2, AVX, XOP, AVX2. Interleaved UV plane: U and V are deinterleaved and
  accumulated separately. Width is in UV-sample pairs; height in rows.
  Maximum width before 32-bit accumulation overflow is documented (~32832 for
  10-bit XMM, ~11008 for 8-bit MMX).

### 3.3 `pixel_var_WxH` — sum and sum-of-squares
* `(uint64_t pixel_var_WxH(pixel *pix, intptr_t stride))`.
* Sizes 8x8, 8x16, 16x8, 16x16; ISAs SSE2, AVX, AVX2, AVX-512.
* Returns a packed 64-bit value: low 32 bits = sum of all samples, high 32 bits
  = sum of squared samples (this is the x264 convention; the caller computes
  variance as `sumsq - sum*sum/N`).

### 3.4 `pixel_var2_8x8` / `pixel_var2_8x16` — dual-plane variance
* `(int pixel_var2_8xH(pixel *fenc, pixel *fdec, int ssd[2]))`.
* SSE2, SSSE3, AVX2, AVX-512.
* Computes per-plane (U and V of an interleaved 4:2:0 chroma block) `d = fenc -
  fdec`, accumulation of `d` and `d^2`. Returns `sum(variance_u +
  variance_v)`; stores the raw squared sums in `ssd[0..1]`.

### 3.5 `pixel_satd_WxH` — sum of absolute transformed differences
* `(int pixel_satd_WxH(pixel*, intptr_t, pixel*, intptr_t))`.
* All eight sizes; ISAs MMX2, SSE2, SSSE3 (`ssse3` and `ssse3_atom`), SSE4,
  AVX, XOP, AVX2, AVX-512.
* Computes `sum |Hadamard(pix1-pix2)|` over 4x4 or 8x8 blocks.

### 3.6 `pixel_sa8d_WxH` — 8x8 SATD
* `(int pixel_sa8d_8x8/16x16(pixel*, intptr_t, pixel*, intptr_t))`.
* MMX2, SSE2, SSSE3 (`ssse3`, `ssse3_atom`), SSE4, AVX, XOP, AVX2, AVX-512.
* Single 8x8 Hadamard; result is `(sum|coeff| + 1) >> 1` (half-scale SATD).

### 3.7 `pixel_sa8d_satd_16x16`
* `(uint64_t pixel_sa8d_satd_16x16(pixel*, intptr_t, pixel*, intptr_t))`.
* SSE2, SSSE3, SSE4, AVX, XOP, AVX2 (x86-64 only). Returns both metrics packed:
  low 32 bits = half-scale SA8D, high 32 bits = SATD.

### 3.8 Intra-mode kernels (x3 and x9)
* `void intra_satd_x3_4x4(uint8_t *fenc, uint8_t *fdec, int *res)`;
  `void intra_sa8d_x3_8x8(uint8_t*, uint8_t edge[36], int *res)` (MMX2/SSE2).
* `int intra_sad_x9_4x4(uint8_t *fenc, uint8_t *fdec, uint16_t *bitcosts)`;
  `int intra_satd_x9_4x4(...)`; `int intra_sad_x9_8x8(..., uint8_t edge[36],
  uint16_t *bitcosts, uint16_t *satds)`;
  `int intra_sa8d_x9_8x8(...)`. SSSE3, SSE4, AVX, XOP (4x4 only for XOP);
  also MMX2 for 32-bit.
* Evaluate the 3 or 9 H.264 intra prediction modes, combine each distortion with
  the mode's bit cost, return `(cost<<16)|mode_index` (or fill `res[]` for x3),
  and write the best-mode predicted samples into `fdec`.

### 3.9 `pixel_hadamard_ac_WxH` — Hadamard SATD+SA8D for AQ
* `(uint64_t pixel_hadamard_ac_WxH(pixel*, intptr_t))`.
* MMX2, SSE2, SSSE3 (`ssse3`, `ssse3_atom`), SSE4, AVX, XOP, AVX2.
* Returns packed `(satd_normalized << 32) | (sa8d >> 1)` (exact shifts depend on
  block size / vector width; see section 5.9).

### 3.10 `pixel_ssim_4x4x2_core`, `pixel_ssim_end4`
* `void pixel_ssim_4x4x2_core(const pixel *pix1, intptr_t stride1,
  const pixel *pix2, intptr_t stride2, int sums[2][4])` — SSE2, AVX; also
  an MMX2 32-bit variant.
* `float pixel_ssim_end4(int sum0[5][4], int sum1[5][4], int width)` — SSE2, AVX.
* Core accumulates `sum1`, `sum2`, `sumSq1`, `sumSq2`, `sumCross` for two 4x4
  blocks; end computes the SSIM formula with constants C1/C2.

### 3.11 `pixel_asd8`, `pixel_ads{1,2,4}`
* `int pixel_asd8(pixel*, intptr_t, pixel*, intptr_t, int height)` — SSE2, SSSE3,
  XOP. Sum of absolute differences of 8-wide column sums.
* `int pixel_adsN(int enc_dc[N], uint16_t *sums, int delta, uint16_t *cost_mvx,
  int16_t *mvs, int width, int thresh)` — MMX2/SSE2/SSSE3/AVX/AVX2.
  Successive-elimination early termination for motion search; returns a packed
  16-bit mask of per-candidate decisions.

## 4. State and data structures

* Inputs are two pixel planes with independent strides (`FIX_STRIDES` macro
  handles `intptr_t` vs 32-bit stack arguments).
* Interleaved chroma planes store U/V in adjacent bytes/words; deinterleaving
  uses `SBUTTERFLY`, `DEINTB`, `punpck`, or `vpermt2d`.
* Many kernels use temporary stack space for transposed blocks and pred buffers:
  e.g. `INTRA_X9` reserves `0xc0`/`0xb0` bytes, `INTRA8_X9` reserves `0x240`,
  `hadamard_ac` reserves `mmsize*3 + ...`.
* MMX variants call `SAVE_MM_PERMUTATION` / `RESET_MM_PERMUTATION` to keep the
  assembler's logical register numbering valid.

## 5. Algorithm / behaviour

### 5.1 SSD (8-bit)
For each pair of 8-byte rows:
1. Compute byte-wise absolute difference using two unsigned subtracts and OR:
   `d = (a-b unsigned) | (b-a unsigned)` (`psubusb` twice + `por`). This avoids
   the signed-saturation problem of `psubw`.
2. Deinterleave even/odd bytes to separate word lanes (`DEINTB` on SSE2,
   `punpcklbw/hbw` on MMX, `pmaddubsw` with `[1,-1,...]` on SSSE3).
3. Square via `pmaddwd` of the word differences with themselves, accumulate
   dwords.
4. Horizontally sum dwords (`HADDD`) and return the low 32 bits.
For high-bit-depth, step 1 is a plain `psubw`, then `pmaddwd` squares.

### 5.2 SSD NV12
Process U and V halves separately with `psadbw`-like squaring; deinterleave with
`SBUTTERFLY wd` (SSE) or `pshufb` with `ssd_nv12_shuf` (AVX2). Accumulate in
64-bit `paddq` accumulators, then store `ssd_u`/`ssd_v` as two 64-bit values.
AVX2 may overread by up to 32 bytes; the tail is corrected by subtracting the
extra squared terms.

### 5.3 Variance
1. Accumulate `sum += pix` in word lanes (`paddw`) and `sumsq += pix*pix` in
   dword lanes (`pmaddwd`).
2. At the end, `pmaddwd sum,[pw_1]` reduces word pairs to dwords;
   `SBUTTERFLY dq` interleaves sum/sumsq lanes; horizontal adds produce two
   dwords `[sum_total, sqr_total]` returned as one 64-bit value.
8-bit paths first zero-extend bytes to words (`DEINTB` or `pmovzxbw`).
AVX-512 uses `vbroadcasti64x4`/`vinserti32x4` with opmasks to assemble a whole
block in one 512-bit register.

### 5.4 var2
Compute `d = fenc - fdec` per plane (U/V deinterleaved), accumulate `sum_d` and
`sum_d2`, then `VAR2_END` computes `sum_d2 - (sum_d*sum_d >> shift)` with
`shift=6` for 8x8 / `7` for 8x16, stores the raw squared sums to `ssd[2]`, and
returns the sum of the two variances.

### 5.5 SATD
1. Load residual `r = pix1-pix2` (byte paths use `pmaddubsw` with `hmul_8p`
   or `hmul_16p` to form sum/difference pairs in one instruction; word paths
   use `psubw`).
2. Apply a 2D Hadamard: `HADAMARD4_2D` or `HADAMARD8_2D` (butterflies along
   rows then columns), with the `amax` variant taking absolute values at
   intermediate stages where legal (Hadamard of absolute values is ≤ true
   SATD for the chosen sign pattern — the reference notes this trick).
3. Sum absolute coefficients with `HADDW`/`HADDUW`, mask to 16 bits for 8-bit.
The "vertical" vs "horizontal" mode flag selects whether the first butterfly
is applied via `hmul` on bytes (SSSE3 fast path) or via explicit word
butterflies (SSE2/high-bit-depth path).

### 5.6 SA8D
An 8x8 Hadamard of the residual, absolute values summed, then `(sum+1)>>1`.
The 16x16 form computes four 8x8 blocks and saturating-adds their results
(`paddusw`) before the final shift.

### 5.7 SA8D_SATD
Computes both metrics in one pass: the 8x8 Hadamard network yields both the
SA8D coefficient magnitudes and the 4x4-subblock SATD contributions. At the end
the two sums are separated and packed into a 64-bit return value.

### 5.8 Intra x3 / x9
1. Compute the 4x4 or 8x8 Hadamard of the fencoded block (for SATD/SA8D modes)
   or use SAD directly (for SAD modes).
2. Construct the 3 or 9 predicted blocks from the edge samples using shuffle
   tables (`intrax9a_*`/`intrax9b_*` for 4x4, `intra8x9_*` for 8x8) and the
   `PRED4x4_LOWPASS` filter that implements `(a + 2b + c + 2)>>2`.
3. Compute each mode's distortion against fenc, add the corresponding
   `bitcosts[mode]`, and find the minimum. SSE4 uses `phminposuw`; older ISAs
   pack the cost and a 3-bit mode index into a 16-bit word and use `pminsw`.
4. Return `(cost<<16)|(mode+1)` and write the winning prediction into `fdec`.
   `intra_sad_x9_8x8` also fills `satds[mode]` for all nine modes.
The x3 kernels are retained only for pre-SSSE3 CPUs; x9 supersedes them.

### 5.9 hadamard_ac
Compute an 8x8 Hadamard, accumulate *all* coefficient magnitudes (SATD-like) and
a masked subset (SA8D-like, mask `mask_ac4`/`mask_ac8` selecting the low-frequency
coefficients). For 16x16, four 8x8 sub-blocks are combined with saturating
adds. Final scaling: for MMX path `satd >>=1`, `sa8d >>=1`; for SSE2 path
`satd >>= (2 - (W*H*16/mmsize >> 8))`, `sa8d >>=1`; packed into 64 bits as
`(satd<<32)|sa8d`.

### 5.10 SSIM
1. Core: for each of two adjacent 4x4 blocks, accumulate per-lane
   `sum1`, `sum2`, `sum1^2`, `sum2^2`, `sum1*sum2` into `sums[2][4]`.
2. End4: sum the five supplied `sum[5][4]` arrays, transpose, then compute
   `SSIM = (2*mu1*mu2 + C1)*(2*sigma12 + C2) / ((mu1^2+mu2^2+C1)*(sigma1^2+sigma2^2+C2))`
   using floating point (`BIT_DEPTH==10` path uses `cvtdq2ps` and pre-scaled
   constants; 8/9-bit uses integer arithmetic then `cvtdq2ps`).
   `C1=(0.01*MAX)^2*64`, `C2=(0.03*MAX)^2*64*63` (values in `.rodata`).
   When `width != 4`, a mask zeroes unused lanes.

### 5.11 ASD8
Sum 8-wide column sums from pix1 and pix2 over `height` rows, then return the
absolute difference of the two totals. High-bit-depth sums words; 8-bit uses
`psadbw` to sum bytes.

### 5.12 ADS (successive elimination)
For each candidate motion vector in `mvs[]`:
1. Compute `sums[i]` differences from the encoded DC values `enc_dc[i]`,
   take absolute values (`ABSW`/`ABSD`), add the motion-vector cost from
   `cost_mvx[delta+mvy]` and the precomputed `delta` bias.
2. Compute `threshold - cost` with unsigned saturation (`psubusw`), pack to
   bytes, and write one byte per candidate to `mvs`-adjacent storage.
3. The `.nz` path uses dword saturation when `thresh` exceeds 16 bits.
The `ads_mvs_shuffle` table (256 entries × 8) reorders results to match the
candidate MV ordering.

## 6. Dependencies

* Includes `x86inc.asm`, `x86util.asm`; heavy use of `INIT_*`, `cglobal`,
  `FIX_STRIDES`, `HADAMARD*`, `SBUTTERFLY`, `DEINTB`, `ACCUM`, `SAVE_MM_PERMUTATION`.
* Imports C constants via `cextern`: `pw_00ff`, `pw_ppppmmmm`,
  `pw_ppmmppmm`, `pw_pmpmpmpm`, `pw_pmmpzzzz`, `pd_1`, `hsub_mul`,
  `popcnt_table`.
* Called by `common/mc.c`, `common/me.c`, `common/analyse.c`, `common/quant.c`,
  `common/pixel.c`, `common/intra.c` through the dispatch layer.
* 32-bit-only variants are in `pixel-32.asm`.

## 7. Constants and tables

* `hmul_16p`, `hmul_8p`, `hmul_4p`: byte sign patterns `[+1,-1]` for
  `pmaddubsw` to compute Hadamard sum/difference pairs.
* `mask_ac4`, `mask_ac4b`, `mask_ac8`: masks selecting the low-frequency
  Hadamard coefficients for SA8D.
* `sw_f0`, `pd_f0`: sign/zero masks for DC rounding in intra kernels.
* `pw_s01234567`, `pw_s01234657`, `pw_s00112233`, `pw_s00001111`: packed
  cost+index tags for finding the minimum intra mode.
* `intrax9a_*`, `intrax9b_*`, `intra8x9_*`: byte-index shuffle tables that
  construct each intra prediction from edge samples.
* `intrax9_edge`, `intrax9a_lut`, `intrax9b_lut`: edge replication / mode-to-
  prediction-buffer lookup tables.
* `ssim_c1`, `ssim_c2`, `pf_64`, `pf_128`: SSIM constants, bit-depth specific.
* `var_shuf_avx512`, `ssd_nv12_shuf`, `deinterleave_shuf`, `ads_mvs_shuffle`:
  vector permutation tables.
* `popcnt_table` imported for ADS.

## 8. Performance notes

The file is a catalogue of micro-optimisations; high-value opportunities:
* **Wider vectors:** AVX-512 variants process a whole 8x8/16x16 block in one
  ZMM register using `vpbroadcast*` with opmasks (`k1/k2/k3`), eliminating
  explicit transposes. AVX2 packs two rows via `vinserti128`. Prefer the widest
  ISA; the reference explicitly notes that some XOP/AVX variants are slower on
  certain CPUs (e.g. no XOP `INTRA8_X9` because it regressed on Bulldozer).
* **Transpose strategy:** `SBUTTERFLY` and `TRANSPOSE4x4W/D` are used instead of
  full 8x8 transposes where possible; the AVX-512 SATD path avoids transposes
  entirely by broadcasting rows into stride patterns.
* **Avoiding spills:** On x86-64 the 16x16 kernels keep four 8x8 results live in
  registers; `SA8D_SATD_ACCUM` spills only two 128-bit temporaries to
  `[rsp+0*mmsize]`/`[rsp+1*mmsize]` when register pressure peaks. The heavy
  `INTRA_X9` uses a stack `pred_buf` because it must keep all nine predictions.
* **Preprocessing constants:** All shuffle/mask tables are static `.rodata`.
  The `ads_mvs_shuffle` table is generated at assemble time (256×8 bytes) to
  avoid runtime reordering logic. Bit-depth-dependent SSIM constants are
  precomputed as floats.
* **Fast paths:** `phminposuw` (SSE4) replaces a chain of `pminsw` in intra mode
  selection; `pmulhrsw`/`pmaddubsw` fuse rounding and arithmetic; `psadbw`
  accelerates byte sums.
* **Store-forwarding:** A comment explicitly avoids storing intra8x9 HD result
  at `[r4+12]` because it is too close to a preceding load — a clean
  implementation should preserve such spacing or use register shuffles.
* **Branch behaviour:** Kernels are branch-free inside row loops; the only
  branches are loop counters and the `width==4` SSIM fast path.
* **Hotness:** SATD/SA8D dominate at fast presets; SSD/variance at slow presets;
  intra x9 at all presets when intra is enabled; ADS in motion search.

## 9. References

* ITU-T H.264 Hadamard transform and intra prediction definitions.
* "Fast block matching using successive elimination" (SES) literature for ADS.
* SSIM paper (Wang et al.) for the C1/C2 constants.
* Intel Intrinsics Guide for `pmaddubsw`, `phminposuw`, `vpbroadcast*`.
* Public x264 source history for the `amax` Hadamard shortcut rationale.

## 10. Verification checklist

* For every metric, compare against the portable C implementation over random
  blocks and edge cases (all-zero, all-255, alternating).
* SSD: verify return type range and no signed overflow for worst-case 16x16.
* SATD/SA8D: verify the `+1>>1` rounding of SA8D and the exact saturation
  behaviour of 16x16 accumulation.
* `sa8d_satd_16x16`: verify the packed 64-bit layout (low = sa8d, high = satd).
* `hadamard_ac`: verify the packed layout and the block-size-dependent shift.
* Intra x9: verify the returned `(cost<<16)|mode` encoding and that `fdec`
  contains the predicted samples for the chosen mode; compare `satds[]` arrays.
* SSIM: compare `pixel_ssim_end4` output to the C formula for random sums and
  for `width==2/4` (mask path).
* var2: verify `ssd[0..1]` store values and the returned combined variance.
* ADS: verify the packed decision mask for all 256 possible `enc_dc` patterns
  on a small candidate set.

## 11. Open questions

* `UNKNOWN`: the exact mathematical justification and error bounds of the
  `amax` (abs-before-Hadamard) shortcut; it is an approximation used for speed.
* `UNKNOWN`: precise bit layout of `pixel_hadamard_ac` for 8x8 with 32-byte
  vectors; the shift formula `2 - (W*H*16/mmsize >> 8)` should be validated
  empirically for each size/ISA combination.
* `UNKNOWN`: why some `*_ssse3_atom` variants exist separately (Atom CPU
  scheduling) — exact dispatch conditions are in `pixel.h`/dispatch code.
* `UNKNOWN`: overflow limits for ADS with very large `delta`/`thresh` values
  beyond the documented 16-bit threshold split.
