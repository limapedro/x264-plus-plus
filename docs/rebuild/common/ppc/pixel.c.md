# `common/ppc/pixel.c` — PowerPC pixel-metric implementation

## 1. Header

- **Source path:** `common/ppc/pixel.c`.
- **Role in one sentence:** AltiVec implementations of SAD, multi-reference SAD, SATD, SA8D, SSD, variance, Hadamard-AC, SSIM core, intra mode-comparison helpers, and the pixel function-table initializer.
- **Marker:** Dirty-side behavioural specification; all descriptions are behavioural and written independently of the reference source.


## 2. Purpose

This file provides the PowerPC pixel comparison kernels used by motion estimation and mode decision. It is compiled only for 8-bit builds and uses 16-byte AltiVec vectors with 16-bit or 32-bit lanes depending on the metric.

## 3. Public interface

The only exported symbol is:

- `void x264_pixel_init_altivec( x264_pixel_function_t *pixf )`

declared in `ppc/pixel.h`. All metric routines are file-local and installed through the function table. The installed families are:

- SAD at 16x16, 16x8, 8x16, 8x8; x3 and x4 variants at those sizes.
- SATD at all seven standard block sizes; x3 and x4 wrappers at all seven sizes.
- SSD at 16x16 and 8x8.
- SA8D at 8x8 and 16x16.
- Variance at 16x16 and 8x8.
- Hadamard-AC at 8x8, 8x16, 16x8, 16x16.
- SSIM 4x4x2 core.
- Intra mode-comparison helpers: `intra_sad_x3_8x8`, `intra_sad_x3_8x8c`, `intra_sad_x3_16x16`, `intra_satd_x3_4x4`, `intra_satd_x3_8x8c`, `intra_satd_x3_16x16`, `intra_sa8d_x3_8x8`.

## 4. State and data structures

Constant tables include `hadamard_permtab` with two byte-selector vectors for 16-bit widening depending on pointer alignment, and a `perm_tab` array used by the 8x8 variance routine. The `xxpermdi` helper from `ppccommon.h` is used to assemble 8-column data from two 16-byte loads for the narrower blocks.

## 5. Algorithm / behaviour

**SAD.** `PIXEL_SAD_ALTIVEC` loads one row from each block, computes absolute byte differences with `vec_absd`, and accumulates into 32-bit lanes with `vec_sum4s`. A final `vec_sums` reduces to a scalar; different splat indices (3 or 1) are used depending on how many 32-bit lanes contain meaningful data. The x3/x4 routines load the encoded block once per iteration and accumulate separate sums for each candidate, reusing `FENC_STRIDE = 16`.

**SATD.** The 1D Hadamard macro combines four values into sum/difference pairs. SATD applies it horizontally, transposes with `VEC_TRANSPOSE_4` or `VEC_TRANSPOSE_8`, applies it vertically, and accumulates absolute values with `vec_abs` followed by `vec_sum4s`. The final result is right-shifted by one. The 4x4 and 4x8 routines use a 4-lane transform; 8-wide routines use the 8-lane transform twice or on both halves of a vector. The 16x8 and 16x16 routines use `VEC_DIFF_HL` to split 16-byte rows into high and low halves and process each independently.

**SSD.** Byte differences are squared and accumulated with `vec_msum`. The 16x16 routine keeps two row vectors in flight to hide latency and performs sixteen rows plus a final partial accumulation. The 8x8 routine masks off unused lanes with a selector vector before the final reduction.

**Variance.** The 16x16 routine accumulates sample sums with `vec_sum4s` and squared sums with `vec_msum` over sixteen rows, then packs the two 32-bit results into a 64-bit return value. The 8x8 routine handles pointer alignment by selecting the appropriate 8-byte half of each 16-byte load with a permutation mask, processing four row pairs.

**Hadamard-AC.** A common core loads eight rows, widens bytes to 16-bit using an alignment-dependent permutation, applies two 1D Hadamard stages with a transpose between them, and accumulates absolute values. Two sums are produced: one over all coefficients and one over a permuted view. The DC term is subtracted from both. Wrappers for 8x8, 8x16, 16x8 and 16x16 combine one or more cores and apply final shifts of 1 and 2 to the two halves of the 64-bit result.

**SSIM core.** Four sums are accumulated per 4x4 window over four rows: first image, second image, squares of each, and the cross product. Results are split into two 32-bit lanes for the two windows and stored into `sums[2][4]`.

**Intra helpers.** These call one of the generic intra prediction routines to fill a temporary block, then run the corresponding metric against the encoded source. The 8x8 luma variants use the filtered edge array; the chroma and 16x16 variants use the reconstructed-buffer prediction functions.

## 6. Dependencies

- Includes `common/common.h`, `ppccommon.h`, local `pixel.h`, and `common/predict.h` for intra prediction helpers.
- Installed by the generic pixel-initialization path.
- Uses pixel-size enums and function-table typedefs from `common/pixel.h`.

## 7. Constants and tables

- SATD final divide by 2; SA8D final `(sum+2)>>2`.
- Hadamard-AC final shifts 1 and 2 on the two packed sums.
- `FENC_STRIDE = 16`, `FDEC_STRIDE = 32`.
- Alignment-dependent permutation tables for 8x8 variance and Hadamard-AC.

## 8. Performance notes

The x3/x4 variants are the most important because they amortize encoded-block loads across several candidates. AltiVec's `vec_sum4s` and `vec_msum` provide cheap horizontal reduction. The SATD reduction uses saturation-safe 16-bit intermediates followed by widening accumulation. A clean implementation should first match the scalar metrics, then optimize the reduction chains; lane-order mistakes on little-endian PowerPC are common.

## 9. References

- `common/pixel.c` for canonical metric definitions.
- `common/predict.c` for intra prediction helpers.
- AltiVec Programming Interface Manual.

## 10. Verification checklist

- All metrics match generic C results on randomized inputs for their supported sizes.
- x3/x4 variants agree with repeated single-reference calls.
- SATD and SA8D match generic Hadamard implementations including final rounding.
- Variance and Hadamard-AC respect the packed 64-bit return convention.
- SSIM core sums match scalar accumulation.
- Intra helpers produce the same three scores as predict-then-metric in the generic path.

## 11. Open questions

- `UNKNOWN`: exact lane assignments in `hadamard_permtab` for little-endian builds.
- `UNKNOWN`: whether the x3/x4 SATD wrappers are installed for all seven block sizes in all build configurations.
