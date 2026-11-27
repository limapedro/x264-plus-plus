# `common/mips/pixel-c.c` — MSA pixel distortion metrics

**Dirty-side behavioural specification.** Source path: `E:\_x264_dirty_stash\x264-source\common\mips\pixel-c.c`. Role: MSA-128 implementations of SAD, SSD, SATD, SA8D, Hadamard/AC, SSIM, variance, and the intra-prediction-plus-metric combined helpers. Compiled only for 8-bit builds.

## 1. Header

The title line above gives the source path and the file's role in one sentence; the adjacent bold marker identifies this document as a **dirty-side behavioural specification** written from the reference source and public literature. No reference source or near-verbatim pseudocode is reproduced.

## 2. Purpose

Motion estimation and mode decision evaluate a large number of candidate blocks, so distortion metrics dominate encoder runtime at all presets. This file provides vectorized versions of every metric the generic `common/pixel.c` dispatcher can install, including multi-reference SAD variants that compute three or four candidate costs in one pass. It also implements the fast intra decision helpers that generate a few candidate predictions and score them immediately.

## 3. Public interface

Declared in `mips/pixel.h.md`; the full set includes:

- Single-reference `x264_pixel_sad_WxH_msa` for eight sizes (16x16, 16x8, 8x16, 8x8, 8x4, 4x16, 4x8, 4x4).
- Multi-reference `x264_pixel_sad_x3_WxH_msa` and `x264_pixel_sad_x4_WxH_msa` for seven sizes.
- `x264_pixel_ssd_WxH_msa` for eight sizes.
- `x264_pixel_satd_WxH_msa` for eight sizes; `x264_pixel_sa8d_8x8/16x16_msa`.
- `x264_pixel_hadamard_ac_WxH_msa` for 8x8, 8x16, 16x8, 16x16.
- `x264_ssim_4x4x2_core_msa`.
- `x264_pixel_var_16x16/8x16/8x8_msa`, `x264_pixel_var2_8x16/8x8_msa`.
- Intra combined helpers `x264_intra_sad_x3_*`, `x264_intra_satd_x3_*`, `x264_intra_sa8d_x3_8x8_msa`.

Return values: SAD/SSD/SATD/SA8D are non-negative `int32_t`; Hadamard-AC and variance are packed `uint64_t` (low 32 bits AC/sum, high 32 bits DC/sum-of-squares). Multi-reference helpers write their results into caller arrays.

## 4. State and data structures

No globals. Working state is in MSA vectors:

- SAD accumulators are eight halfword lanes (`v8u16`), so up to `8·255·height` can accumulate before overflow; loops process 2 or 4 rows per iteration and horizontally reduce at the end.
- SSE/SATD accumulators are four word lanes (`v4i32`); the maximum squared difference per byte is 65025, and 16·16·65025 ≈ 16.6M fits in 32 bits.
- Intra combined helpers use a 16-byte-aligned local scratch block `uint8_t pix[8*FDEC_STRIDE]` for 8x8 predictions; 4x4 and 16x16 helpers predict directly into `p_dec`.

## 5. Algorithm / behaviour

### SAD (single reference)

- **4-wide:** four 32-bit words per row from source and reference are inserted into two 16-byte vectors; `asub_u_b` gives absolute byte differences; `hadd_u_h` pairs them into halfwords; a `v8u16` accumulator sums across rows four at a time; final `HADD_UH_U32` reduces to one 32-bit integer.
- **8-wide:** four 16-byte vectors per side per four rows are packed down from 8-byte halves with `pckev_d`; `SAD_UB2_UH` computes two halfword SAD vectors per row pair; accumulate and reduce.
- **16-wide:** two 16-byte vectors per row; four rows per iteration, accumulating two SAD vectors at a time.

### Multi-reference SAD

- **x3:** source is loaded once per group of rows and reused against three reference pointers. For 4-wide, three separate `v8u16` accumulators are updated from the same source vector; for 8-wide, references are packed with `pckev_d2`; for 16-wide, two source vectors are compared to each of three reference vectors in a two-row loop. Results are reduced with `HADD_UH_U32` into `sad[0..2]`.
- **x4:** same pattern with four accumulators; the 4-wide case loads four 32-bit words per reference; the 8-wide case loads all four references before packing; the 16-wide case processes two rows per iteration. Results go to `sad[0..3]`.
- The source stride is always `FENC_STRIDE`; only the reference stride is passed in.

### Variance and SSD

- `avc_pixel_var16width_msa`: for each row, horizontally add unsigned bytes to halfwords, reduce to a 32-bit sum with `HADD_UH_U32`; widen bytes to halfwords with zero interleaving and accumulate `dpadd_u_w(pix,pix)` into a 4-word square accumulator; return `sum | (sqr<<32)`.
- `avc_pixel_var8width_msa`: same but only the right half of each 16-byte load is used (the 8-wide block sits in the low or high half depending on the helper).
- `sse_diff_8width_msa`: computes `Σ(src−ref)²` in a `v4i32` accumulator using `ILVRL_B2_UB`/`HSUB_UB2_SH`/`DPADD_SH2_SW`, while also accumulating the signed difference `Σ(src−ref)` in a `v8i16` vector. Returns the SSE and writes the difference sum; `VARIANCE_WxH(sse,diff,shift) = sse − ((uint32)diff² >> shift)` with shift 7 for 8x16 and 6 for 8x8.
- `sse_4/8/16width_msa`: pure squared-difference accumulators using the `CALC_MSE_B` macro (interleave, horizontal subtract, widening dot-product-add).

### SSIM 4x4x2 core

Loads four 8-byte rows from each image, reduces byte pairs to two sets of halfword sums (`Σx` for the two 4x4 quadrants of each image), widens to words and splits into even/odd lanes via `ilvev_w`/`ilvod_w` to obtain `sum[0][0],sum[1][0]` etc. Then widens each byte to halfwords and accumulates `Σx²` for both images (using `dotp_u_w`/`dpadd_u_w` over eight vectors) and the cross term `Σx·y`. Results are written into `sums[2][4]` where index 2 and 3 are the squared sums and cross sums respectively.

### SATD

The residual is obtained by interleaving source and reference bytes, taking pairwise unsigned differences into halfwords, then applying a Hadamard transform in both dimensions using `BUTTERFLY_4`/`BUTTERFLY_8` and the transpose macros, followed by `add_a_h(·,0)` (absolute value) and a horizontal reduction. The result is shifted right by **1** (`u_sum >> 1`) — x264's SATD normalization. 4x4 and 8-wide variants process four rows per iteration; 16-wide variants call the 8-wide routine twice at offsets 0 and 8.

### SA8D

`sa8d_8x8_msa` performs a full 8x8 Hadamard on the byte differences (using `TRANSPOSE8x8_SH_SH` and two passes of `BUTTERFLY_4`), then combines the four partial sums via `add_a_h` and `asub_s_h` into `temp + |diff|` terms, sums and reduces. `x264_pixel_sa8d_8x8_msa` returns `(sum+2)>>2`; the 16x16 version sums four 8x8 cores and applies the same `(sum+2)>>2`.

### Hadamard/AC

`pixel_hadamard_ac_8x8_msa` transforms an 8x8 block with two Hadamard passes; it saves the four DC-adjacent halfwords `diff0[0], diff0[4], diff4[0], diff4[4]`, computes the 4x4- and 8x8-subblock absolute sums (subtracting the single DC term from each), and returns `(u_sum8 << 32) + u_sum4` where each accumulator is the sum of absolute coefficients for the respective subblock size. The public wrapper converts this to x264's packed form with `((u_sum >> 34) << 32) + ((uint32_t)u_sum >> 1)`, i.e. AC cost (÷2) in the low word and DC cost (÷4) in the high word. Larger sizes sum multiple 8x8 cores with pointer offsets of 8 bytes and `8*stride`.

### Intra combined helpers

- SAD x3 (4x4/16x16): call `x264_intra_predict_vert/hor/dc_*_msa(p_dec)` in that order and score each with the matching `x264_pixel_sad_*_msa(p_dec, FDEC_STRIDE, p_enc, FENC_STRIDE)`; outputs go to `sad[0]` (V), `[1]` (H), `[2]` (DC).
- SAD x3 (8x8 / 8x8c): use a local aligned scratch block, call the `*_8x8_msa(pix, p_edge)` helpers (V, H, DC or DC-4blk, H, V) and score with `x264_pixel_sad_8x8_msa`.
- SATD x3 and SA8D x3 mirror the same pattern with the corresponding metric, using the same predictor order and output slots.

## 6. Dependencies

- **Calls:** `mips/macros.h` (`LW4`, `INSERT_W4_UB`, `LD_UB2/4/8`, `PCKEV_D2/4_UB`, `SAD_UB2_UH`, `HADD_UB*`, `HSUB_UB2/4_SH`, `DPADD_SH2/4_SW`, `ILVRL_B2_UB`, `CLIP*`, `TRANSPOSE4x4_SH_SH`, `TRANSPOSE8X4/4X8_SH_SH`, `TRANSPOSE8x8_SH_SH`, `BUTTERFLY_4/8`, `HADD_SW_S32`, `HADD_UH_U32`) and MSA intrinsics (`asub_u_b`, `add_a_h/w`, `hadd_u_h`, `dotp_u_w`, `dpadd_u_w`, `nloc`, `asub_s_h`).
- **Called by:** `x264_pixel_init()` in `common/pixel.c`, and `x264_intra_*_init()` paths for the combined helpers.
- **Cross-file:** `mips/predict.h` declarations are required by the intra combined helpers; `FENC_STRIDE`/`FDEC_STRIDE` from `common/common.h`.

## 7. Constants and tables

- `VARIANCE_WxH` shifts: **7** for 8x16, **6** for 8x8 (the `log2(W·H)+1` correction).
- SATD normalization: `>>1`. SA8D normalization: `(sum+2)>>2`. Hadamard-AC packing: low word `sum>>1`, high word `sum>>34`.
- Butterfly-based Hadamard uses no scaling constants; only ±1 additions.
- No data tables are defined in this file.

**Why:** the `>>1`/`>>2` factors convert the Hadamard energy to a comparable scale with SAD for rate-distortion decisions; the variance shift is `log2(n)+1` from the identity `Var = E[x²] − E[x]²`.

## 8. Performance notes

- **Hotness:** highest for motion search; x3/x4 SAD variants are the inner loop of diamond/hex/umh search.
- **SIMD status:** fully MSA-128; the 4-wide path uses GPR word loads plus vector inserts because 4 bytes do not fill a vector.
- **Branch behaviour:** no data-dependent branches in the kernels; loop counts derive only from block height.
- **Optimization tips:** (a) reuse the loaded source vector across all references in x3/x4 (already done); (b) keep the `v8u16` accumulators in registers across the full block height to avoid partial reductions; (c) for SATD/SA8D, the transpose macros are the main cost — use the specialized 4x4/8x8 transposes; (d) align `p_enc`/`p_dec` to 16 bytes; (e) in motion search, prefer the x4 SAD variant over four single SAD calls to save source loads.
## 9. References

x264 `common/pixel.c`; ITU-T H.264 §8.4.2.2; literature on sum-of-absolute-Hadamard-transform distortion estimation; MSA programmer's guide.

## 10. Verification checklist

1. For all eight block sizes, compare single SAD with the scalar reference on random and pattern inputs (including max values 255).
2. Multi-reference SAD: verify each output slot matches an independent scalar SAD; test source stride `FENC_STRIDE` and arbitrary reference strides.
3. SSD: verify no 32-bit overflow for a block of all-255 vs all-0.
4. SATD: compare with scalar `x264_pixel_satd_c` for all sizes; check the `>>1` normalization at boundary cases.
5. SA8D/Hadamard-AC: verify the packed high/low words against the C reference for 8x8 and the four tiled 8x8 positions of 16x16.
6. SSIM core: verify `sums[0][k]` and `sums[1][k]` for a deterministic input, including the cross-term split between quadrants.
7. Variance/var2: verify the packed return layout and the SSD output pointer.
8. Intra helpers: compare each candidate SAD/SATD value with a manual predict-then-metric sequence using the scalar predictor.
9. ASan with 16-byte-aligned and deliberately unaligned pointers.

## 11. Open questions

- UNKNOWN: precise quadrant-to-index mapping in `ssim_4x4x2_core_msa` output (`sums[2][4]`) relative to the generic SSIM formula; validate with a known 4x4 pattern.
- UNKNOWN: whether the 8x8 Hadamard-AC `u_dc` subtraction removes exactly the true DC term for all four sub-block sizes; verify with a constant-valued block.
- UNKNOWN: overflow safety of `v8u16` SAD accumulators for unusual heights beyond the documented block sizes; the public API may be called with larger heights internally.
- UNKNOWN: exact predictor order for the 8x8c intra x3 helpers in this version (observed DC-4blk, H, V) — confirm against the generic `intra_satd_x3_8x8c` expectations.
