# `common/arm/pixel-a.S` — ARM/NEON pixel-metric kernels

**Dirty-side behavioural specification.** Source path: `common/arm/pixel-a.S`. Role: assembly implementations of SAD, SATD, SA8D, SSD, variance, Hadamard-AC, SSIM, and related pixel comparisons.

## 1. Purpose

Pixel comparison kernels are the innermost loops of motion estimation and mode decision. This file contains the hand-optimized ARM implementations, including an ARMv6-only 4-wide SAD routine and a large family of NEON routines for all standard block sizes and for multiple reference candidates at once.

## 2. Public interface

Exported symbols are listed in `arm/pixel.h`. Major families:

- `x264_pixel_sad_4x4_armv6`, `x264_pixel_sad_4x8_armv6`.
- NEON SAD for sizes 4x4, 4x8, 8x4, 8x8, 8x16, 16x8, 16x16, plus aligned and `_neon_dual` variants.
- `x264_pixel_sad_x3_*` and `x264_pixel_sad_x4_*` for sizes 4x4, 4x8, 8x8, 8x16, 16x8, 16x16.
- NEON SATD for all seven standard block sizes and x3/x4 variants.
- `x264_pixel_sa8d_8x8_neon`, `x264_pixel_sa8d_16x16_neon`, `x264_pixel_sa8d_satd_16x16_neon`.
- NEON SSD for all standard sizes and `x264_pixel_ssd_nv12_core_neon`.
- Variance: `x264_pixel_var_8x8_neon`, `var_8x16`, `var_16x16`, `var2_8x8`, `var2_8x16`.
- Auxiliary: `x264_pixel_vsad_neon`, `x264_pixel_asd8_neon`, `x264_pixel_hadamard_ac_*_neon`, `x264_pixel_ssim_4x4x2_core_neon`, `x264_pixel_ssim_end4_neon`.

ABI follows the generic typedefs: single-reference metrics take two pointers and two strides in `r0..r3`; x3/x4 variants take the encoded block pointer in `r0`, candidate pointers in `r1..r4`, a stride, and a score array on the stack.

## 3. State and data structures

Constant tables in read-only sections:
- `mask_array` provides 16 bytes of `0xFF` followed by 16 zero bytes for SSIM/averaging masks.
- `mask_ac4` and `mask_ac8` select AC positions for the Hadamard-AC metric.
- `pw_0to15`-style index vectors are synthesized with `vmov` where needed.

Internal helpers such as `satd_8x8_neon`, `sa8d_8x8_neon`, `hadamard_ac_8x8_neon`, and the `*_end_neon` tail routines are not exported but are shared by multiple public kernels.

## 4. Algorithm / behaviour

**SAD.** ARMv6 4-wide SAD uses `usad8`/`usada8` to accumulate four byte pairs per word with no vector setup. NEON SAD uses `vabdl.u8` for the first row and `vabal.u8` for subsequent rows, accumulating absolute differences into 16-bit lanes. Results are reduced with pairwise adds (`vpadd`) to a 32-bit integer. Aligned variants use aligned load hints; `_dual` variants use two independent accumulators to hide dependency latency.

**Multi-reference SAD.** x3/x4 variants load the encoded block once and compare it against three or four candidate blocks in the same loop, accumulating separate sums. This amortizes the source loads and is a major speedup in motion search.

**SATD.** Differences are widened to 16 bits, then a two-dimensional Hadamard transform is applied using `SUMSUB_AB` and the `HADAMARD` macro from `asm.S`. The absolute-value reduction uses the identity that `abs(a)+abs(b)` equals `2*max(|a+b|/2, |a-b|/2)` for aligned data, letting the code use `vmax.u16` and pairwise adds instead of per-lane absolute values. The final result is right-shifted by one.

**SA8D.** Similar to SATD but uses an 8-point one-dimensional transform with half-word shifts and a final `(sum + 2) >> 2` scaling. The combined `sa8d_satd` helper computes both metrics in one pass.

**SSD.** Byte differences are widened and squared with `vmull.s16`/`vmlal.s16`, accumulating in 32-bit lanes before a final horizontal reduction.

**Variance.** Sums and squared sums are accumulated separately with `vpadal` and `vmlal`, then packed into one 64-bit return value: low 32 bits hold the sample sum, high 32 bits the square sum.

**Hadamard-AC.** A 2D Hadamard transform is applied to 8x8 blocks; separate AC-masked accumulations yield two sums. The wrapper shifts them right by one and two respectively for the final return value.

**SSIM.** The core routine accumulates four sums per 4x4 window (first image, second image, squares of both, and cross-product) for two windows at once. The end helper combines accumulated sums from multiple windows into the final SSIM value.

## 5. Dependencies

- Includes `common/arm/asm.S`.
- Declared in `arm/pixel.h` and installed by the generic pixel-initialization path.
- Uses typedefs and the pixel-size enum from `common/pixel.h`.

## 6. Constants and tables

- SATD final divide by 2; SA8D final `(sum+2)>>2`.
- Weighted-average denominator 64 with rounding 32 (used by helper routines in `mc-a.S`, not by the metrics here).
- Mask tables for AC/DC separation in Hadamard-AC.

## 7. Performance notes

The kernels rely on three key ideas: widening absolute-difference instructions, multi-accumulator scheduling to hide VABAL latency, and the `abs`/`max` trick for Hadamard reduction. The 4x4 ARMv6 SAD demonstrates that vector setup is not always worthwhile for small blocks. For a clean implementation, start with scalar definitions of every metric and verify them; then optimize the hottest SAD/SATD paths. Cache behaviour is streaming with good locality across rows.

## 8. References

- `common/pixel.c` for canonical metric definitions.
- ITU-T H.264 reference documentation for Hadamard-based distortion measures.
- ARM NEON Programmer's Guide for `vabdl`, `vabal`, `vpadd`, `vmlal`, and pairwise widening.

## 9. Verification checklist

- All metrics match the generic C implementations on randomized inputs for every block size.
- x3/x4 variants agree with repeated single-reference calls.
- SATD and SA8D agree with the generic Hadamard implementations including final shifts.
- Variance and Hadamard-AC respect the packed 64-bit return convention.
- SSIM core sums match scalar accumulation.
- Extreme pixel values (0 and 255) do not overflow accumulators.

## 10. Open questions

- `UNKNOWN`: which `_neon_dual` routines are actually selected on a given core, since the dual-issue comment implies core-dependent dispatch.
- `UNKNOWN`: exact behaviour of `pixel_ssim_end4_neon` for non-standard width arguments.
