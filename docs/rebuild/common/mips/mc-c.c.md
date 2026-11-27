# `common/mips/mc-c.c` — MSA motion compensation, interpolation and plane helpers

**Dirty-side behavioural specification.** Source path: `E:\_x264_dirty_stash\x264-source\common\mips\mc-c.c`. Role: MSA-128 implementations of luma/chroma motion-compensated interpolation, weighted prediction, block averaging/copying, half-pel filter generation, plane interleave/deinterleave, low-resolution frame generation and aligned memory helpers; also the architecture dispatcher `x264_mc_init_mips`. Compiled only for 8-bit builds.

## 1. Header

The title line above gives the source path and the file's role in one sentence; the adjacent bold marker identifies this document as a **dirty-side behavioural specification** written from the reference source and public literature. No reference source or near-verbatim pseudocode is reproduced.

## 2. Purpose

Motion compensation (MC) is the single most bandwidth-heavy stage for inter frames: every macroblock fetches reference samples, interpolates to quarter-pel luma / eighth-pel chroma precision, and averages up to two predictions with optional weighted prediction. This file replaces all of those paths with MSA vector code, plus the plane-copy and memory utilities that surround them. It is the largest MIPS source file because it covers nine block sizes and both "normal" and "no-denominator" weighted-prediction variants.

## 3. Public interface

Only one symbol is exported: `void x264_mc_init_mips(uint32_t cpu, x264_mc_functions_t *pf)`. It installs the following implementations into `pf` when `cpu & X264_CPU_MSA`:

- `mc_luma = mc_luma_msa`, `mc_chroma = mc_chroma_msa`, `get_ref = get_ref_msa`
- `avg[PIXEL_16x16 .. PIXEL_4x2]` (nine entries) to `pixel_avg_*_msa`
- `weight`, `offsetadd`, `offsetsub` all to the six-entry `mc_weight_wtab_msa` table
- `copy_16x16_unaligned` and `copy[16x16,8x8,4x4]` to the `mc_copy_w*_msa` routines
- `store_interleave_chroma`, `load_deinterleave_chroma_fenc/fdec`
- `plane_copy_interleave`, `plane_copy_deinterleave`, `plane_copy_deinterleave_rgb`
- `hpel_filter`, `memcpy_aligned` (plain C `memcpy`), `memzero_aligned`, `frame_init_lowres_core`

All internal helpers are `static`; their signatures follow the generic `x264_mc_functions_t` typedefs. `weight_fn_t` is `void(*)(uint8_t*, intptr_t, uint8_t*, intptr_t, const x264_weight_t*, int)`.

## 4. State and data structures

- **Half-pel reference planes:** four pointers `src[0..3]` (luma) selected by `x264_hpel_ref0`/`x264_hpel_ref1` tables indexed by `qpel_idx = ((mvy&3)<<2) + (mvx&3)`. Integer part is `offset = (mvy>>2)*stride + (mvx>>2)`. For qpel `3` positions an extra stride/byte is added to point at the next filtered plane.
- **Masks:** `pu_luma_mask_arr[16*8]` — three 16-byte `vshf` masks for the 8-wide and 4-wide luma horizontal 6-tap filter (indices into a pair of 8-byte vectors), and a second set for 4-wide cases that packs two rows into one vector. `pu_chroma_mask_arr[16*5]` — five masks for 8/4/2-wide chroma bilinear filtering.
- **Weight descriptors:** `x264_weight_t` with `i_scale`, `i_offset`, `i_denom`; the MSA code reads these directly.
- **Working vectors:** up to 16 vector registers per kernel; row loop counts are `height/4` or `height%4`, with scalar tails.

## 5. Algorithm / behaviour

### Luma horizontal 6-tap filter (`avc_luma_hz_16w_msa`)

For 16-wide blocks, each of two adjacent 8-byte row vectors is sign-flipped (XOR 128), split into three shifted copies via `vshf` using masks `{0,5,1,6,2,7,3,8,...}`, `{1,4,2,5,...}` and `{2,3,3,4,...}`, and accumulated as `hadd(signed byte) + (−5)·dpadd(mask1) + 20·dpadd(mask2)`. Each output byte is therefore the H.264 6-tap `[1,-5,20,20,-5,1]` filter applied to six horizontally adjacent samples. The result is rounded right by **5** (`srari_h`), saturated to 7-bit signed (`sat_sh(7)`), sign-flipped back and packed to bytes. Four rows are processed per iteration; a `height%4` tail loop handles the remainder.

### Luma vertical 6-tap filter (`avc_luma_vt_16w_msa`)

Uses three constants packed as halfwords: `0xfb01` (i.e. −5 in one lane, 1 in another), `0x1414` (20,20), `0x01fb` (1,−5). Five source rows are loaded and sign-flipped; pairs are widened to halfword vectors (left/right halves). Four output rows per iteration are computed as three-term dot products `dpadd_sh3` of the widened row triplets with those constants, then `>>5` with rounding, saturation to 7 bits, pack and sign-flip. Row state shifts down by four each iteration; scalar tail loop for `height%4`.

### Luma center (6x6) filter (`avc_luma_mid_8w_msa`)

First applies the horizontal 6-tap filter to five source rows (producing halfword vectors `hz_out0..4`), then for each output row applies the vertical 6-tap combination via `AVC_CALC_DPADD_H_6PIX_2COEFF_SH`: two `hadd` pairs of `(in5,in0)` and three pairwise dot products with constants −5 and 20, accumulated in word lanes, rounded right by **10**, and saturated to 7 bits (`sat_sw(7)`). Output bytes are packed and stored 8x4 per iteration. Tail loop handles remaining rows. The 16-wide version calls the 8-wide version twice at offsets 0 and 8.

### Chroma bilinear interpolation

The eight coefficients are `hor0 = mvx&7`, `hor1 = 8−hor0`, `ver0 = mvy&7`, `ver1 = 8−ver0`. For interleaved Cb/Cr source data, byte-shuffle masks gather even/odd sample pairs, unsigned dot products multiply by the horizontal weights (packed into one byte vector via `ilvr_b` of two weight vectors), then the vertical weights multiply the four partial sums. The total is rounded right by **6** and saturated to 8 bits (`sat_uh(7)`). Results are packed and written as halfwords to the separate U and V destinations. Specialized variants exist for widths 2, 4 and 8 and heights 2, 4, 8, 16; the 4-wide/multiple-height case loops in groups of four rows. The `i_width=8` kernel uses a different mask and processes U and V in two passes.

### Block copying and averaging

- `copy_width4/8/16_msa`: load 4/8/16 bytes per row with the given source stride and store with the destination stride. The 8- and 16-wide versions dispatch on `height % 12`, `% 8`, `% 4` (and `%2` for width 8) to process 12/8/4/2 rows per iteration, using 128-bit loads/stores.
- `avg_src_width4/8/16_msa`: pairwise rounding average `(a+b+1)>>1` of two sources (`aver_u_b`), four rows at a time.
- `pixel_avg_*_msa` wrappers: if `weight == 32`, use the plain rounding average; if `weight < 0 || weight > 63`, use the "no-denominator" weighted variant; otherwise use the normal weighted variant. Luma weights are `(i_weight, 64−i_weight)` with log2 denominator **5**.

### Weighted prediction

Two families exist:

- **Normal** (`avc_wgt_opscale_*`): `offset_in <<= log2_denom`, plus `(1<<(log2_denom−1))` when the denominator is nonzero; each source byte is widened, multiplied by the signed weight, added with the saturated offset, clamped to ≥0, shifted right logically by `log2_denom`, and saturated to 8 bits.
- **Bi-weighted normal** (`avc_biwgt_opscale_*`): `offset = ((offset+1)|1) << log2_denom`; weights are interleaved into a byte vector; `dpadd_u_h` accumulates `w0·s0 + w1·s1 + offset` per pair; right shift by `log2_denom+1`, clamp and saturate.
- **Bi-weighted no-denominator** (`*_nw`): offset is ignored; `denom = log2_denom + 1`; signed word products of `src1*w1 + src2*w2`, rounded right shift (`srar_h`), then clip to `[0,255]`.

Widths 4/8/16/20 are handled; `w20` is `w16 + w4`.

### `mc_luma_msa`, `mc_chroma_msa`, `get_ref_msa`

These are the dispatchers: compute `qpel_idx`/`offset`, pick the two half-pel source pointers; if `qpel_idx & 5` (any half-pel component) they average the two planes with `avg_src_width*` and optionally apply weighting in place; otherwise they apply weighting directly or copy the integer-pel plane. `get_ref_msa` additionally handles widths 12 and 20 and arbitrary `height%4` tails with scalar helpers, and returns either `p_dst` (after averaging/weighting with `*dst_stride` unchanged) or the source pointer with `*dst_stride = src_stride` (zero-copy case).

### Plane helpers

- `core_plane_copy_interleave_msa`: merges two planar U/V sources into interleaved NV12-style output, 4 rows per iteration, with 16-wide vector path, 8-wide path and scalar tail.
- `core_plane_copy_deinterleave_msa`: splits interleaved U/V into two planes via `pckev`/`pckod`, 8 rows per iteration, with 8-wide/4-wide/scalar tails.
- `core_plane_copy_deinterleave_rgb_msa` and `_rgba_msa`: split RGB/RGBA triples/quads into three planes using `vshf_b` with masks `{0,3,6,...}`, `{1,4,7,...}`, `{2,5,8,...}` (or `pckev_h`/`pckod_h` for RGBA), with 4-row vector loops and scalar tails.
- `core_frame_init_lowres_core_msa`: generates four half-resolution planes from a full-resolution luma plane by averaging 2x2 neighborhoods (`pckev`/`pckod` + `aver_u_b`), with 16-wide and 8-wide paths.
- `memzero_aligned_msa`: vector zero stores of 16 bytes plus a `memset` tail.
- `hpel_filter_msa`: for each 16-wide column group, calls the vertical, center and horizontal filters to produce the vertical-only, center and horizontal-only half-pel planes.
- `core_store_interleave_chroma_msa`, `load_deinterleave_chroma_fenc/fdec_msa`: frame-stride-specific wrappers around the deinterleave/interleave cores for FDEC/FENC strides.

## 6. Dependencies

- **Calls:** `mips/macros.h` (`LD_SB*`, `LD_UB*`, `ST_UB*`, `LD4`, `LW`, `SW`, `SD`, `SD4`, `XORI_B*`, `VSHF_B2`, `HADD_SB*`, `DPADD_SB*`, `SRARI_H*`, `SAT_SH*`, `PCKEV_*`, `PCKOD_*`, `ILVR_*`, `ILVL_*`, `AVER_UB*`, `INSERT_W4`, `ST8x4_UB`, `ST4x4_UB`, `AVC_HORZ_FILTER_SH`, `AVC_CALC_DPADD_H_6PIX_2COEFF_SH`) and MSA intrinsics.
- **Called by:** `x264_mc_init()` in `common/mc.c`.
- **Shared tables:** `x264_hpel_ref0[16]`, `x264_hpel_ref1[16]` from `common/mc.c`; `x264_weight_t` from `common/frame.h`; `FDEC_STRIDE`/`FENC_STRIDE`.

## 7. Constants and tables

- `pu_luma_mask_arr`: 8 rows × 16 bytes giving the three `vshf` masks for 8-wide and 4-wide luma horizontal filtering. Values are byte indices into neighbouring 8-byte vectors (e.g. `{0,5,1,6,2,7,3,8,4,9,5,10,6,11,7,12}`).
- `pu_chroma_mask_arr`: 5 rows × 16 bytes for chroma bilinear gather (`{0,1,1,2,2,3,3,4,...}` for 8-wide, plus 4- and 2-wide variants).
- Filter constants: `−5`, `20`, halfword patterns `0xfb01`, `0x1414`, `0x1fb`.
- Shift/saturation: `>>5` + `sat(7)` for luma filters, `>>10` + `sat_sw(7)` for center filter vertical pass, `>>6` + `sat_uh(7)` for chroma.
- Luma weights `(w, 64−w)` with denominator 5; RGB masks `{0,3,6,9,12,15,18,21}`, `{1,4,7,10,13,16,19,22}`, `{2,5,8,11,14,17,20,23}`.

**Why:** the 6-tap coefficients and shifts are fixed by H.264 §8.4.2.2.1; the chroma `>>6` comes from 1/8-pel precision; the weight denominator 5 yields 1/32 steps for B-frame weighted prediction.

## 8. Performance notes

- **Hotness:** highest of all MIPS kernels for inter frames; every macroblock calls MC one to four times.
- **SIMD status:** fully MSA-128 except the `memcpy_aligned` slot (uses libc `memcpy`) and scalar tails for height/width not divisible by block factors.
- **Branch behaviour:** dispatch on block size and qpel pattern is call-site-uniform; per-row loops are counted. Weighted-predicate `weight==32` check is uniform.
- **Optimization tips:** (a) the half-pel filter kernels are the main cost — keep the mask arrays in read-only cache and avoid reloading them per call; (b) align destination pointers to 16 bytes so vector stores are aligned; (c) for `get_ref_msa`, the zero-copy path avoids a full block copy — preserve it; (d) fuse the two half-pel averages with the weighting step when both are needed to avoid a round-trip through the destination; (e) the RGB deinterleave kernels benefit from unrolling four rows at a time (already done).
## 9. References

ITU-T H.264 §8.4.2 (sample interpolation) and §8.4.4 (weighted sample prediction); x264 `common/mc.c`; MSA programmer's guide.

## 10. Verification checklist

1. For all qpel positions (0..3 in x and y), compare `mc_luma_msa` output with the scalar reference for a deterministic reference plane; check the `qpel_idx & 5` path specifically.
2. Chroma: test all 8x8 fractional offsets for widths 2/4/8 and heights 2/4/8/16 against the scalar bilinear reference.
3. Weighted prediction: test `weight` values 0, 1, 31, 32, 33, 63 and out-of-range values (triggering the `_nw` path); verify rounding and saturation at 0/255 boundaries.
4. `get_ref_msa`: verify zero-copy path returns a pointer with the source stride and that the destination is untouched; verify width 12/20 tails.
5. Plane interleave/deinterleave: round-trip a random YUV 4:2:0 plane through `plane_copy_interleave` → `plane_copy_deinterleave` and compare to the original.
6. RGB/RGBA deinterleave: verify byte positions for a known 3- or 4-byte-per-pixel pattern.
7. `frame_init_lowres_core`: compare four downsampled planes against the scalar 2x2 average.
8. ASan with odd strides and non-multiple-of-4 heights to catch tail-handling errors.
9. Full-encode bitstream equivalence between MSA and C builds.

## 11. Open questions

- UNKNOWN: exact source-pointer arithmetic for qpel positions with `mvx&3 == 3` / `mvy&3 == 3` (extra byte/stride selection via `x264_hpel_ref1`); validate against scalar MC for all 16 qpel combinations.
- UNKNOWN: whether `memcpy_aligned` being plain libc `memcpy` is intentional (it is in the source) or a placeholder for a MSA vector copy.
- UNKNOWN: the `height%12` special case in `copy_width8/16_msa` — 12-row groups are unusual; confirm the reason (likely 4:2:0 chroma height 12 for unusual block sizes) and that tails are correct.
- UNKNOWN: behaviour of the `*_nw` weighted path for `log2_denom == 0` — the `denom = log2_denom+1` shift may be off by one relative to the generic no-denominator formula; verify numerically.
