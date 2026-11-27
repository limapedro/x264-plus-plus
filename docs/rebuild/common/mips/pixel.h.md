# `common/mips/pixel.h` — MSA pixel-metric declarations

**Dirty-side behavioural specification.** Source path: `E:\_x264_dirty_stash\x264-source\common\mips\pixel.h`. Role: public interface for the MSA pixel-distortion metrics used by motion search, intra decision, rate-distortion, and SSIM.

## 1. Header

The title line above gives the source path and the file's role in one sentence; the adjacent bold marker identifies this document as a **dirty-side behavioural specification** written from the reference source and public literature. No reference source or near-verbatim pseudocode is reproduced.

## 2. Purpose

This header declares every distortion primitive that the generic `common/pixel.c` dispatcher can replace with an MSA implementation: SAD (single and multi-reference), SSD, SATD, SA8D, Hadamard/AC, SSIM core, variance, and the intra-prediction-plus-metric combined helpers. All declarations are guarded by `#if !HIGH_BIT_DEPTH` and name-mangled through `x264_template`.

## 3. Public interface

Signatures (all names carry the `_msa` suffix before mangling):

**Single-reference SAD** — `int32_t x264_pixel_sad_WxH_msa(uint8_t *src, intptr_t i_src_stride, uint8_t *ref, intptr_t i_ref_stride)` for sizes 16x16, 16x8, 8x16, 8x8, 8x4, 4x16, 4x8, 4x4. Returns `Σ|src−ref|` as a non-negative 32-bit integer.

**Four-reference SAD** — `void x264_pixel_sad_x4_WxH_msa(uint8_t *src, uint8_t *ref0, uint8_t *ref1, uint8_t *ref2, uint8_t *ref3, intptr_t i_ref_stride, int32_t sad[4])` for sizes 16x16, 16x8, 8x16, 8x8, 8x4, 4x8, 4x4. Source uses `FENC_STRIDE`; each output slot is the SAD against the corresponding reference.

**Three-reference SAD** — analogous `x264_pixel_sad_x3_WxH_msa(..., int32_t sad[3])` for the same sizes.

**SSD** — `int32_t x264_pixel_ssd_WxH_msa(uint8_t*, intptr_t, uint8_t*, intptr_t)` for the eight standard sizes; returns `Σ(src−ref)²` (sum of squared differences of 8-bit values, max about 16·16·65025 ≈ 16.6M, fits in 32 bits).

**SATD** — `int32_t x264_pixel_satd_WxH_msa(uint8_t*, intptr_t, uint8_t*, intptr_t)` for 4x4, 4x8, 4x16, 8x4, 8x8, 8x16, 16x8, 16x16. Returns `Σ|Hadamard(residual)| >> 1` (the x264 convention).

**SA8D** — `int32_t x264_pixel_sa8d_8x8_msa(...)`, `x264_pixel_sa8d_16x16_msa(...)`; 8x8 Hadamard cost with `(sum+2)>>2` scaling.

**Hadamard/AC** — `uint64_t x264_pixel_hadamard_ac_WxH_msa(uint8_t*, intptr_t)` for 8x8, 8x16, 16x8, 16x16. Packs the AC cost in the low 32 bits and the DC cost in the high 32 bits.

**SSIM** — `void x264_ssim_4x4x2_core_msa(const uint8_t *p1, intptr_t s1, const uint8_t *p2, intptr_t s2, int32_t sums[2][4])`. Fills sums `[0][*]` for the first image and `[1][*]` for the second: sums of pixels, sums of squares, and cross-sums split by quadrant.

**Variance** — `uint64_t x264_pixel_var_WxH_msa(uint8_t*, intptr_t)` for 16x16, 8x16, 8x8 (low word = sum, high word = sum of squares); `int32_t x264_pixel_var2_8x16_msa(p1, s1, p2, s2, int32_t *ssd)` and the 8x8 variant return a variance measure and write the SSD.

**Intra combined metrics** — `x264_intra_sad_x3_4x4_msa`, `..._16x16_msa`, `..._8x8_msa(p_enc, p_edge[36], sad[3])`, `..._8x8c_msa`, and the matching `x264_intra_satd_x3_*`, `x264_intra_sa8d_x3_8x8_msa`. These generate three candidate predictions (V/H/DC) into a scratch block and score each against the encoder block.

## 4. State and data structures

No state. Include guard `X264_MIPS_PIXEL_H`. The `p_edge[36]` array used by 8x8 helpers is the standard x264 edge buffer laid out as: indices 0–7 top-left/top rows, 8–15 top-right, 16–23 left column, 24–31 unused/filler, plus corner sample at index 35 in the generic convention. The implementations in `pixel-c.c` use offsets `+14`, `+16`, `+7` into this array for H/V/DC predictions.

Scratch prediction blocks use `ALIGNED_ARRAY_16(uint8_t, pix, [8*FDEC_STRIDE])` — a 16-byte-aligned local buffer of eight rows at reconstruction stride.

## 5. Algorithm / behaviour

Declarations only. The metric definitions and rounding are in `pixel-c.c.md`. Important ABI facts for a rebuild:

- All SAD/SSD/SATD functions take the source pointer first; x3/x4 helpers take the single source then the reference pointers, with the source stride implicitly `FENC_STRIDE`.
- The integer types of return values distinguish packed results (`uint64_t`) from scalar costs (`int32_t`).
- `p_edge` is read-only from the caller's perspective, but the intra helpers write a temporary prediction buffer, not the edge itself.

## 6. Dependencies

- **Included by:** `common/mips/pixel-c.c` and `common/pixel.c` (dispatcher).
- **Requires:** `x264_template`, `intptr_t`, `int32_t`, `uint64_t`, `uint8_t`, `ALIGNED_ARRAY_16`, and the `FENC_STRIDE`/`FDEC_STRIDE` macros from generic headers.
- **Cross-header:** `mips/predict.h` must be in scope because the intra-combined helpers call `x264_intra_predict_*_msa` functions; `pixel-c.c` includes it directly.

## 7. Constants and tables

None here. Numerical constants (the `>>1` SATD normalization, `(sum+2)>>2` SA8D, `>>34`/`>>1` Hadamard-AC packing) are documented in `pixel-c.c.md`.

## 8. Performance notes

No runtime cost. For rebuild, preserve the exact signature ordering — the multi-reference helpers are called in tight motion-search loops and mismatched pointer/stride order would produce wrong costs without a compiler error (all pointers are `uint8_t*`).

## 9. References

ITU-T H.264 standard; x264 project documentation; architecture SIMD programmer's guides.

## 10. Verification checklist

1. Include this header plus `mips/predict.h` in isolation under an 8-bit build; confirm no warnings.
2. Verify each declared symbol appears in `pixel-c.c`'s object file.
3. Check that the eight single-reference SAD sizes and the seven x4 SAD sizes listed above match the dispatch slots the generic `pixel.c` expects.
4. Confirm no high-bit-depth declaration leaks (rebuild with `HIGH_BIT_DEPTH=1`).

## 11. Open questions

- UNKNOWN: the precise edge-buffer layout for `p_edge[36]` should be cross-checked against `common/predict.c`'s `x264_predict_8x8_filter` output, since the MSA helpers index it with hardcoded offsets.
- UNKNOWN: whether `x264_pixel_var2_*_msa`'s `ssd[2]`-style signature in other architectures implies a second element that this MIPS version ignores; verify against the generic typedef.
