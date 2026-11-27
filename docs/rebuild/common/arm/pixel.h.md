# `common/arm/pixel.h` — ARM pixel-metric declarations

## 1. Header

- **Source path:** `common/arm/pixel.h`.
- **Role in one sentence:** declares the ARM/NEON pixel-distance, variance, Hadamard, and SSIM kernels and the helper macros used to declare them.
- **Marker:** Dirty-side behavioural specification; all descriptions are behavioural and written independently of the reference source.


## 2. Purpose

Pixel comparison functions are called millions of times per encode during motion estimation and mode decision. This header lists the ARM-optimized implementations and provides declaration macros that expand to the full family of block sizes, keeping the source list compact and consistent.

## 3. Public interface

The header declares four groups of functions:

- **SAD**: `x264_pixel_sad_4x4_armv6`, `x264_pixel_sad_4x8_armv6`; NEON SAD for 16x16, 16x8, 8x16, 8x8, 8x4, 4x8, 4x4; aligned SAD variants (with `_aligned_` in the name, including `_neon_dual`); three- and four-reference SAD (`sad_x3`, `sad_x4`) for all standard block sizes.
- **SATD and SA8D**: NEON SATD for all seven standard block sizes, three-/four-reference SATD variants, `x264_pixel_sa8d_8x8_neon`, `x264_pixel_sa8d_16x16_neon`, and the combined `x264_pixel_sa8d_satd_16x16_neon`.
- **SSD / variance / auxiliary metrics**: NEON SSD for all standard sizes, NV12 SSD core, variance (`var_8x8`, `var_8x16`, `var_16x16`, `var2_8x8`, `var2_8x16`), vertical SAD, ASD8, SA8D, Hadamard-AC for 8x8/8x16/16x8/16x16, and SSIM core/end helpers.
- **Averaging helpers**: NEON `pixel_avg2_w4/w8/w16/w20` and `pixel_avg_16x16`, `16x8`, `8x16`, `8x8`, `8x4`, `4x16`, `4x8`, `4x4`, `4x2`.

Two declaration macros, `DECL_PIXELS` and `DECL_X1`/`DECL_X4`, expand to the standard signature families. `DECL_PIXELS` takes a return type, a function-name stem, a suffix, and an argument list, then declares the seven standard sizes.

Typical signatures:
- Single-reference metrics: `int f( uint8_t *pix1, intptr_t stride1, uint8_t *pix2, intptr_t stride2 )`.
- Triple-reference SAD: `void f( uint8_t *fenc, uint8_t *p0, uint8_t *p1, uint8_t *p2, intptr_t stride, int scores[3] )`.
- Quadruple-reference SAD adds a fourth candidate and `int scores[4]`.
- Variance: `uint64_t f( uint8_t *pix, intptr_t stride )`, packing sum and sum-of-squares into one 64-bit value.
- Hadamard-AC: `uint64_t f( uint8_t *pix, intptr_t stride )`, packing two sums into 32-bit halves.
- SSIM core: `void f( const uint8_t *, intptr_t, const uint8_t *, intptr_t, int sums[2][4] )`.

## 4. State and data structures

All metrics are stateless. The `pixel_size` enum from `common/pixel.h` supplies array indices: `PIXEL_16x16 = 0` through `PIXEL_4x4 = 6`, plus subsampled-only sizes. The `x264_pixel_function_t` table in the generic code holds pointers to each routine.

The 64-bit variance return value packs the sum of samples in the low 32 bits and the sum of squared samples in the high 32 bits. The Hadamard-AC return packs two intermediate sums in the two halves and applies a final scaling in the wrapper.

## 5. Algorithm / behaviour

Definitions that the ARM implementations must match:

- **SAD**: sum of absolute byte differences.
- **SATD**: sum of absolute values of the 2D Hadamard transform of the difference block, divided by two. The transform is unnormalized; intermediate values are 16-bit.
- **SA8D**: sum of absolute values of the 8x8 Hadamard transform of differences, divided by four with rounding.
- **SSD**: sum of squared differences, returned as a 32-bit integer.
- **Variance**: returns `(sum of squares << 32) | sum`; the caller derives mean and variance.
- **Hadamard-AC**: returns two sums used by intra-mode decision: a four-fold SATD-like quantity and an eight-fold variant; wrappers shift them down by one and two bits respectively.
- **SSIM core**: accumulates, for two 4x4 windows, the sums of first image, second image, both squares, and cross-products, storing results in `sums[2][4]`.
- **Pixel average**: `(a + b + 1) >> 1` for plain averaging; weighted variants compute `(w*a + (64-w)*b + 32) >> 6` with saturation.

The x3/x4 variants compute several candidate scores in one pass, reusing the encoded source block from `FENC_STRIDE = 16`.

## 6. Dependencies

- Installed by `x264_pixel_init_altivec`-style dispatchers in the generic pixel initialization code when NEON is detected.
- Implemented in `common/arm/pixel-a.S`.
- Depends on `x264_pixel_cmp_t` typedefs and `x264_pixel_function_t` from `common/pixel.h`.

## 7. Constants and tables

- `FENC_STRIDE = 16`, `FDEC_STRIDE = 32`.
- SATD final shift: divide by 2. SA8D final shift: divide by 4 with rounding.
- Weighted average denominator: 64, with rounding constant 32 and saturation.
- Masks `mask_ac4` and `mask_ac8` select AC versus DC positions in Hadamard-AC computation.

## 8. Performance notes

These are the hottest functions in the encoder. The ARM implementations use `vabdl.u8`/`vabal.u8` accumulating absolute differences directly in 16-bit lanes, avoiding separate widening and absolute-value steps. SATD uses a clever `abs + max` formulation to compute sums of absolute Hadamard coefficients with vector max operations. The `*_dual` SAD variants use two accumulators to hide VABAL latency on cores capable of dual issue. Compile-time unrolling by block height is used throughout. A clean rebuild should prefer correctness and then focus on reducing dependency chains in the accumulator.

## 9. References

- `common/pixel.c` for canonical metric definitions.
- ITU-T H.264 reference software documentation for SATD/Hadamard metrics.
- ARM NEON Programmer's Guide for widening absolute-difference and pairwise-add instructions.

## 10. Verification checklist

- SAD/SATD/SSD match the generic C functions for all seven block sizes on randomized inputs.
- x3/x4 variants produce the same scores as three/four separate single-reference calls.
- SATD and SA8D agree with the C Hadamard implementation including final rounding.
- Variance and Hadamard-AC match the generic packing convention.
- SSIM core sums match scalar accumulation.
- Averaging functions saturate correctly at 0 and 255 for extreme weights.

## 11. Open questions

- `UNKNOWN`: precise index layout of `edge[36]` consumers is irrelevant here but the 8x8 chroma intra-metric helpers in the PPC counterpart use it; confirm no ARM equivalent needs it.
- `UNKNOWN`: whether all declared `_neon_dual` functions are actually installed on all NEON cores, given the dual-issue comment in the assembly.
