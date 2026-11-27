# `common/arm/dct.h` — ARM transform and zigzag declarations

## 1. Header

- **Source path:** `common/arm/dct.h`.
- **Role in one sentence:** declares the 8-bit ARM/NEON implementations of the H.264 transforms and the 4x4 frame zigzag scan.
- **Marker:** Dirty-side behavioural specification; all descriptions are behavioural and written independently of the reference source.


## 2. Purpose

This header collects the prototypes for the NEON transform kernels used by the encoder's transform-and-quantization pipeline. It lets the generic `x264_dct_init` dispatcher install optimized routines via function pointers without including architecture-specific code in the generic path.

## 3. Public interface

All functions use `dctcoef` (`int16_t` in 8-bit builds) and `uint8_t` sample pointers. Source strides are fixed: `pix1` uses `FENC_STRIDE` (16), `pix2` and `p_dst` use `FDEC_STRIDE` (32). The declared routines are:

- `void x264_dct4x4dc_neon( int16_t d[16] )` — in-place 4x4 Hadamard-like transform of a DC block.
- `void x264_idct4x4dc_neon( int16_t d[16] )` — in-place inverse of the above.
- `void x264_sub4x4_dct_neon( int16_t dct[16], uint8_t *pix1, uint8_t *pix2 )` — 4x4 forward transform of the difference of two blocks.
- `void x264_sub8x8_dct_neon( int16_t dct[4][16], uint8_t *pix1, uint8_t *pix2 )` — four 4x4 transforms covering an 8x8 region.
- `void x264_sub16x16_dct_neon( int16_t dct[16][16], uint8_t *pix1, uint8_t *pix2 )` — sixteen 4x4 transforms.
- `void x264_add4x4_idct_neon( uint8_t *p_dst, int16_t dct[16] )` — inverse 4x4 transform plus saturation to the destination.
- `void x264_add8x8_idct_neon( uint8_t *p_dst, int16_t dct[4][16] )`, `void x264_add16x16_idct_neon( uint8_t *p_dst, int16_t dct[16][16] )` — tiled versions.
- DC variants: `x264_add8x8_idct_dc_neon( uint8_t*, int16_t[4] )`, `x264_add16x16_idct_dc_neon( uint8_t*, int16_t[16] )`, `x264_sub8x8_dct_dc_neon( int16_t[4], ... )`, `x264_sub8x16_dct_dc_neon( int16_t[8], ... )`.
- 8x8 transform: `x264_sub8x8_dct8_neon( int16_t[64], ... )`, `x264_sub16x16_dct8_neon( int16_t[4][64], ... )`, `x264_add8x8_idct8_neon( uint8_t*, int16_t[64] )`, `x264_add16x16_idct8_neon( uint8_t*, int16_t[4][64] )`.
- `void x264_zigzag_scan_4x4_frame_neon( int16_t level[16], int16_t dct[16] )` — reorders coefficients into progressive scan order.

All prototypes are mangled with `x264_template` and are only meaningful in builds where 8-bit NEON transforms are compiled.

## 4. State and data structures

Every function is stateless apart from its explicit input/output buffers. The 4x4 transform operates on a 4x4 array stored in raster order. The 8x8 DC-only routines operate on a 2x2 (or 2x4) array of per-4x4 DC coefficients that has already been Hadamard-transformed. The destination pixel buffer is always assumed to be at least `FDEC_STRIDE` rows tall; the routine writes exactly the block dimensions requested.

## 5. Algorithm / behaviour

The mathematical semantics match the generic C implementations in `common/dct.c`:

- Forward 4x4 transform: a two-dimensional butterfly with coefficient weights 1 and 2; no rounding or clipping is applied to the output coefficients.
- Inverse 4x4 transform: the transposed butterfly, with an arithmetic right shift by 6 with rounding (adding 32 before the shift), followed by addition to the destination and clipping to the valid sample range (0..255 for 8-bit).
- DC block transform: a 4x4 Hadamard with a final right shift by one with rounding on the forward path; the inverse path performs the same butterflies without the shift.
- 8x8 transform: an unnormalized Hadamard-like transform using shifts by 1 and 2 with truncation toward negative infinity on intermediate stages. Forward transforms produce coefficients without clipping; inverse transforms add a DC rounding offset before a final shift by 6 and clip the added results to the sample range.
- The zigzag routine reorders 4x4 coefficients according to the standard progressive (frame) scan table; it is a pure permutation of the input.

The NEON implementations compute these results with 16-bit vector lanes, using widening byte differences on input and saturating narrowing instructions on output.

## 6. Dependencies

- Consumed by `common/dct.c` through `x264_dct_init` and `x264_zigzag_init`.
- Implemented in `common/arm/dct-a.S`.
- Requires `FENC_STRIDE`/`FDEC_STRIDE` from `common/common.h` and the `x264_template` macro from the build configuration.

## 7. Constants and tables

- `FENC_STRIDE = 16`, `FDEC_STRIDE = 32`.
- Transform scaling: final inverse shift of 6 with a rounding constant of 32; 8x8 inverse also adds 32 to the DC coefficient before transformation.
- The 4x4 frame zigzag table is a 16-byte permutation stored in the assembly file. It is an implementation detail; rebuilders should generate it from the standard scan order.

## 8. Performance notes

Transforms are among the most heavily used kernels in all presets. The 4x4 path typically operates on 16 coefficients per call, so vectorization amortizes poorly unless multiple blocks are processed together; the NEON code uses 8x4 helpers to double throughput. Cache pressure is low because operands are small and reused across blocks. When rebuilding, prioritize (1) exact arithmetic equivalence on extreme coefficient values, (2) saturating output conversion, and (3) minimizing load/store instructions by processing two or four blocks per call. Public optimization literature on Hadamard transforms and SIMD butterfly networks applies directly.

## 9. References

- ITU-T H.264, integer transform and scanning specifications.
- `common/dct.c` for the canonical reference behaviour.
- ARM NEON Programmer's Guide for widening differences and saturating arithmetic.

## 10. Verification checklist

- Forward/inverse round-trip on a block of constant input yields the same block after inverse transform within the documented rounding tolerance.
- All-zero input produces all-zero coefficients.
- Extreme coefficient values (`-32768`, `32767`) saturate correctly on inverse paths.
- Zigzag output is a permutation: every input coefficient appears exactly once.
- Output matches the generic C implementation for randomized inputs over many trials.
- Destination rows outside the block are untouched.

## 11. Open questions

- `UNKNOWN`: whether all declared routines are actually installed by `x264_dct_init` in every ARM build; the generic dispatcher wraps them in compile-time feature guards.
- `UNKNOWN`: whether high-bit-depth builds expose any of these symbols at all; the declarations are unconditional but the assembly is guarded by 8-bit-only compilation.
