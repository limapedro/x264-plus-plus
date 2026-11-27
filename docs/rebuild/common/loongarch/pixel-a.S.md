# `common/loongarch/pixel-a.S` — LoongArch LSX/LASX SATD, SSD, SA8D, variance and Hadamard assembly

**Dirty-side behavioural specification.** Source path: `E:\_x264_dirty_stash\x264-source\common\loongarch\pixel-a.S`. Role: assembly implementations of Hadamard/AC, SATD, SSD, SA8D and variance metrics (SAD lives in `sad-a.S`), with LSX and LASX variants. Compiled only for 8-bit builds.

## 1. Header

The title line above gives the source path and the file's role in one sentence; the adjacent bold marker identifies this document as a **dirty-side behavioural specification** written from the reference source and public literature. No reference source or near-verbatim pseudocode is reproduced.

## 2. Purpose

Motion estimation and RD mode decision rely on Hadamard-transform-based costs (SATD/SA8D) and squared-error/variance metrics. This file provides wide-vector versions with reusable Hadamard butterfly macros, a shared 16x8 Hadamard core used by all Hadamard-AC block sizes, and specialized 4x4/8x8 kernels for small blocks where LASX offers little benefit.

## 3. Public interface

Exports (prefixed with `ASM_REF`), declared in `loongarch/pixel.h.md`:

- Hadamard cores: `hadamard_ac_8x8_lsx`, `hadamard_ac_16x8_lasx`; macro-generated `pixel_hadamard_ac_8x8/16x8_lasx` and `pixel_hadamard_ac_8x8_lsx`.
- SATD: `pixel_satd_4x4_lsx` and `pixel_satd_{4x8,4x16,8x4,8x8,8x16,16x8,16x16}_{lsx,lasx}` (4x4 has no LASX variant).
- SSD: `pixel_ssd_{4x4,4x8,4x16,8x4,8x8,8x16,16x8,16x16}_lsx` and `pixel_ssd_{8x8,8x16,16x8,16x16}_lasx`.
- SA8D: `pixel_sa8d_8x8/16x16_lsx` and `_lasx`.
- Variance: `pixel_var_8x8/8x16/16x16_lsx`, `pixel_var2_8x8/8x16_lsx` and `_lasx` variants.

All metric functions have signature `(uint8_t *a, intptr_t stride_a, uint8_t *b, intptr_t stride_b)` → `int32_t` (or `uint64_t` for var/hadamard).

## 4. State and data structures

No globals. Read-only constant tables:

- `hmul_8p` (32 bytes): byte pattern `{1×8, 1,−1,1,−1,1,−1,1,−1}` repeated twice — the alternating sign vector used by Hadamard row operations.
- `mask_ac4b` (16 halfwords): `{0,−1,0,−1,−1,−1,−1,−1}` × 2 — selects which lanes contribute to AC sums for 4x4 sub-blocks.
- `mask_ac8` (16 halfwords): `{0,−1,−1,−1,−1,−1,−1,−1}` × 2 — the 8x8 analogue.

Working vectors are `vr*`/`xr*`; Hadamard butterflies use `SUMSUB_BADC`, `HADAMARD4_V`, `HADAMARD_1`, `HADAMARD_2` macros defined locally.

## 5. Algorithm / behaviour

### Hadamard butterfly macros

- `SUMSUB_BADC(a,b,c,d)`: computes `(a+b, b−b, c+d, d−d)` style pairwise sum/difference with doubling of b/d; used as the building block of the 4-point Hadamard.
- `HADAMARD4_V(a,b,c,d)`: two applications of `SUMSUB_BADC` with rotated operands, producing the 4x4 Hadamard of four row vectors.
- `HADAMARD_1/2`: pack/shuffle and absolute-value/max accumulation steps used by the Hadamard-AC kernel.

### Hadamard-AC (`pixel_hadamard_ac_*`)

The macro `HADAMARD_AC_WXH_LASX(w,h)` generates `pixel_hadamard_ac_WxH_lasx`. It calls the shared `hadamard_ac_16x8_lasx` core (twice if `h == 16`), accumulates word-lane sums for the two 4x4/8x8 sub-block categories with `xvhaddw` chains, extracts to GPRs, and returns a `uint64_t` with the raw sums: one shifted right by **2** and placed in the high half, the other shifted right by **1** and placed in the low half. The C wrappers in `pixel-c.c` then apply the final `((sum>>34)<<32)+((uint32)sum>>1)` packing. The LSX `hadamard_ac_8x8_lsx` core uses `hmul_8p`, `mask_ac4b`, `mask_ac8` and the LSX butterfly macros to produce the same raw sums for one 8x8 block.

### SATD

Residual bytes are obtained by interleaving/widening source and reference bytes and taking pairwise differences into halfwords; a 2-D Hadamard (via `HADAMARD4_V`, `TRANSPOSE4x4_H`, `TRANSPOSE8x8_H` or LASX equivalents) is applied; absolute values are taken with `add_a_h`; lanes are summed horizontally. The result is shifted right by **1** (x264 SATD normalization). The 4x4 kernel (`pixel_satd_4x4_lsx`) uses a dedicated compact core; 8- and 16-wide kernels process four rows per iteration; 16x8/16x16 LASX variants use wider registers and fewer iterations.

### SSD

Uses widening dot-product helpers (`xvdp2.h.bu.b` via `LOAD_INC_8x4W`, `vdp2add.w.h`) to accumulate squared byte differences into word lanes; four rows at a time; final horizontal add. The LSX and LASX variants differ only in vector width. No clipping or rounding is applied; result fits in 32 bits for standard block sizes.

### SA8D

`pixel_sa8d_8x8_*` computes the full 8x8 Hadamard of the byte residual via `suB` butterflies and transposes, then forms `temp + |diff|` pairs from the transformed halfword lanes (using `add_a_h` and `asub_s_h`), sums them and horizontally reduces. The public value is `(sum + 2) >> 2`. `pixel_sa8d_16x16_*` sums four 8x8 cores at offsets 0, 8, `8*stride`, `8*stride+8` and applies the same scaling.

### Variance

`pixel_var_*_lsx` accumulates a byte sum and a byte-square sum from the block, returning a packed `uint64_t` with sum in the low word and sum-of-squares in the high word. `pixel_var2_*` additionally computes `Σ(src−ref)` and `Σ(src−ref)²`, applies `VARIANCE_WxH = sse − ((uint32)diff * diff) >> shift` with shift `log2(n)+1`, writes the SSD to the caller's array, and returns the variance. LSX uses `sse_diff_8width_lsx`; LASX uses `sse_diff_8width_lasx`.

## 6. Dependencies

- **Calls:** `loongson_asm.S` macros (`xvdp2.h.bu.b`, `vdp2add.w.h`, clip, transpose), `loongson_util.S` function macros; raw LSX/LASX instructions for `add_a_h`, `asub_s_h`, `haddw`, `pickev`, `max`, `min`.
- **Called by:** `x264_pixel_init()` via `loongarch/pixel.h`; the C wrappers in `pixel-c.c` call the Hadamard cores.
- **Shared config:** `HIGH_BIT_DEPTH == 0`, `BIT_DEPTH`.

## 7. Constants and tables

- `hmul_8p` alternating signs `{1,−1}` pattern for Hadamard sign flips.
- `mask_ac4b` and `mask_ac8` halfword masks for AC lane selection.
- Normalization shifts: SATD `>>1`, SA8D `(sum+2)>>2`, Hadamard-AC raw shifts `>>1`/`>>2` plus C-level `>>34` for DC.
- Variance correction shift `log2(W·H)+1` (e.g. 7 for 8x16, 6 for 8x8).
- Unroll factor 4 rows; 16/32-byte vector widths.

## 8. Performance notes

- **Hotness:** very high for motion search and intra RD; x4 SAD (in `sad-a.S`) is hotter, but SATD/SA8D dominate at slower presets.
- **SIMD status:** fully LSX/LASX; LASX variants halve loop counts for 8- and 16-wide blocks.
- **Optimization tips:** (a) reuse the shared `hadamard_ac_16x8_lasx` core across all block sizes (already done); (b) keep the masks in registers — they are constants and should be hoisted; (c) for 4x4 blocks, LSX is sufficient — avoid LASX overhead; (d) the Hadamard butterfly macros are only add/sub/shuffle — do not replace with loops; (e) align input pointers to 32 bytes for LASX loads.
## 9. References

x264 `common/pixel.c`; ITU-T H.264 §8.4.2.2; Hadamard transform literature; LoongArch ISA manuals.

## 10. Verification checklist

1. Compare all SATD sizes against the scalar reference, including the `>>1` normalization at boundary cases.
2. Compare SSD for an all-255 vs all-0 block (max energy) to confirm no 32-bit overflow.
3. SA8D: verify 8x8 and 16x16 outputs against scalar for random blocks.
4. Hadamard-AC: verify raw core output and the C-level packed result for all four block sizes.
5. Variance: verify packed layout and `ssd` output array, including the half-way rounding of the `diff² >> shift` correction.
6. Verify LSX/LASX variants of each metric agree.
7. ASan-style bounds check with odd strides and non-multiple-of-4 heights.

## 11. Open questions

- UNKNOWN: exact assignment of the two raw Hadamard sums to the low/high words of the `uint64_t` core result; validate with a constant block where DC dominates.
- UNKNOWN: whether `pixel_var2_*_lasx` writes one or two `ssd` entries (the header declares `int32_t ssd[2]`); inspect the assembly store instructions.
- UNKNOWN: 4x4 SATD/SA8D have no LASX variants — likely because 16 bytes fit an LSX register; confirm no dispatcher slot is left null on LASX-only builds.
