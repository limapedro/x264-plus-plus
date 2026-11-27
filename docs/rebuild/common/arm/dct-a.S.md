# `common/arm/dct-a.S` — ARM/NEON transform kernels

**Dirty-side behavioural specification.** Source path: `common/arm/dct-a.S`. Role: NEON implementations of the H.264 4x4 and 8x8 forward/inverse transforms, DC-block transforms, and the 4x4 frame zigzag scan.

## 1. Purpose

This file contains the arithmetic-intensive transform kernels used by the encoder. It is written entirely in NEON assembly because the transforms operate on small 16-bit matrices where instruction ordering and register allocation dominate performance. All routines operate on 8-bit samples; the file is compiled only for 8-bit ARM builds.

The file complements `common/arm/dct.h`, which declares the symbols, and `common/dct.c`, which installs them into the generic transform table. Its existence is justified by three observations:

- Transforms are invoked for every residual block, so even a small per-call saving multiplies across the whole frame.
- A 4x4 block of 16-bit coefficients fits exactly in four NEON `d` registers, enabling register-only butterflies.
- Widening byte-difference instructions and saturating narrowing instructions let the kernels avoid explicit clamping code.

## 2. Public interface

Exported symbols, all mangled through `asm.S` and declared in `arm/dct.h`:

- `x264_dct4x4dc_neon( int16_t d[16] )` — in-place 4x4 Hadamard transform of a DC block.
- `x264_idct4x4dc_neon( int16_t d[16] )` — inverse of the above.
- `x264_sub4x4_dct_neon( int16_t dct[16], uint8_t *pix1, uint8_t *pix2 )`.
- `x264_sub8x8_dct_neon( int16_t dct[4][16], uint8_t *pix1, uint8_t *pix2 )`.
- `x264_sub16x16_dct_neon( int16_t dct[16][16], uint8_t *pix1, uint8_t *pix2 )`.
- `x264_add4x4_idct_neon( uint8_t *p_dst, int16_t dct[16] )`.
- `x264_add8x8_idct_neon( uint8_t *p_dst, int16_t dct[4][16] )`.
- `x264_add16x16_idct_neon( uint8_t *p_dst, int16_t dct[16][16] )`.
- `x264_add8x8_idct_dc_neon( uint8_t *p_dst, int16_t dct[4] )`.
- `x264_add16x16_idct_dc_neon( uint8_t *p_dst, int16_t dct[16] )`.
- `x264_sub8x8_dct_dc_neon( int16_t dct[4], uint8_t *pix1, uint8_t *pix2 )`.
- `x264_sub8x16_dct_dc_neon( int16_t dct[8], uint8_t *pix1, uint8_t *pix2 )`.
- `x264_sub8x8_dct8_neon( int16_t dct[64], uint8_t *pix1, uint8_t *pix2 )`.
- `x264_sub16x16_dct8_neon( int16_t dct[4][64], uint8_t *pix1, uint8_t *pix2 )`.
- `x264_add8x8_idct8_neon( uint8_t *p_dst, int16_t dct[64] )`.
- `x264_add16x16_idct8_neon( uint8_t *p_dst, int16_t dct[4][64] )`.
- `x264_zigzag_scan_4x4_frame_neon( int16_t level[16], int16_t dct[16] )`.

Internal (not exported) helpers: `sub8x4_dct_neon` and `add8x4_idct_neon` process an 8x4 region and are invoked repeatedly by the larger routines.

ABI details:

- Arguments follow the C order in `r0`, `r1`, `r2`; there are no stack-passed arguments in these kernels.
- `r3` and the `ip` register are loaded with the fixed strides inside the kernels, so callers must not rely on their values being preserved.
- All coefficients are 16-bit signed values; all pixel samples are unsigned bytes.
- `pix1` rows are `FENC_STRIDE = 16` bytes apart; `pix2` and `p_dst` rows are `FDEC_STRIDE = 32` bytes apart.

## 3. State and data structures

Every function is stateless: all intermediate values live in NEON registers (`d0..d31` / `q0..q15`) and no stack frames are used except for saving `lr` in the tiled variants.

The only data object is the read-only constant `scan4x4_frame`, a 16-byte permutation table used by the zigzag routine. It encodes byte offsets into the 16-bit coefficient array and is a pure implementation detail: a clean rebuild should generate it from the standard 4x4 progressive scan order rather than copying it.

The 8x4 helpers produce coefficient sub-blocks in raster order: the first helper call fills the top-left and top-right 4x4 blocks, the second fills the bottom pair. Pointer adjustments of eight rows and eight columns select the four quadrants of a macroblock.

## 4. Algorithm / behaviour

### 4.1 Forward 4x4 transform

1. Four rows of four bytes are loaded from `pix1` and `pix2` with `vld1.32` and widened into signed 16-bit differences using `vsubl.u8`.
2. A one-dimensional butterfly computes, for each column group, the sum and difference of outer and inner pairs. The odd outputs receive an additional factor of two, matching the unnormalized H.264 transform.
3. `TRANSPOSE4x4_16` transposes the 4x4 matrix of 16-bit lanes using `vtrn.16` and `vtrn.32` stages.
4. The same butterfly is applied along the second dimension.
5. The 16 coefficients are stored as four 64-bit vectors in raster order.

No rounding or clipping is applied; coefficients may be negative and are held as signed 16-bit values.

### 4.2 8x4 and tiled forward transforms

`sub8x4_dct_neon` performs the same computation on eight columns, using `q` registers and doubling the number of outputs. `sub8x8_dct_neon` calls it twice, `sub16x16_dct_neon` calls it eight times with pointer arithmetic that advances eight columns or eight rows between groups.

### 4.3 8x8 forward transform

`DCT8_1D` implements the unnormalized 8-point transform using the standard factorisation: symmetric sums and differences are formed, a 1.5 factor is obtained as `x + (x>>1)`, and odd outputs are shifted by 1 or 2. The macro is applied to eight rows, the matrix is transposed with `VEC_TRANSPOSE_8`-style `vswp`/`vtrn` sequences, and the macro is applied again. No clipping is performed.

### 4.4 Inverse 4x4 transform

1. The 16 coefficients are loaded into four registers.
2. For each column, `a0 = c0+c2`, `a1 = c0-c2`, `a2 = (c1>>1)-c3`, `a3 = (c3>>1)+c1`, with arithmetic shifts on negative values.
3. The 4x4 matrix is transposed and the same network is applied along the other dimension.
4. Each result is right-shifted by 6 with rounding (`vrshr.s16`), which is equivalent to adding 32 before an arithmetic shift.
5. The shifted values are added to the destination bytes with widening unsigned addition and narrowed back with `vqmovun.s16`, giving saturation to 0..255.

### 4.5 Inverse 8x8 transform

The 8-point inverse uses a 5/20 factor structure with intermediate arithmetic shifts by 1 and 2, followed by a rounded shift by 6 and saturating addition to the destination. The 16x16 variant applies the 8x8 routine to four quadrants.

### 4.6 DC transformations

- `dct4x4dc_neon` performs a two-dimensional butterfly on a 4x4 DC block. The forward path divides by two with round-to-nearest using `vrhadd.s16`/`vhsub.s16`; the inverse path omits the division.
- `add8x8_idct_dc_neon` divides each DC coefficient by 64 with rounding, broadcasts it across a 4x4 quadrant, and adds or subtracts it with saturating byte operations. Negative values are handled by constructing the complement vector.
- `sub8x8_dct_dc_neon` widens eight rows of byte differences, accumulates four per-4x4 sums, applies a 2x2 Hadamard network, and writes four coefficients.
- `sub8x16_dct_dc_neon` repeats the accumulation for sixteen rows and applies a 2x4 Hadamard network.

### 4.7 Zigzag

The 4x4 frame scan loads two 64-bit halves and uses `vtbl.8` with the stored permutation to reorder coefficients in one step.

## 5. Dependencies

- Includes `common/arm/asm.S` for `function`, `const`, `movrel`, stride constants, and SIMD helper macros.
- Installed through `x264_dct_init` and the zigzag dispatcher in `common/dct.c`.
- Declared in `common/arm/dct.h`.
- Uses `FENC_STRIDE = 16` and `FDEC_STRIDE = 32`.

## 6. Constants and tables

- `FENC_STRIDE = 16`, `FDEC_STRIDE = 32`.
- Inverse transform final shift: 6, with a rounding constant of 32.
- 8x8 inverse adds 32 to the DC coefficient before transformation.
- 8x8 butterflies use shifts of 1 and 2 and the weights 5 and 20.
- One 16-byte zigzag permutation table.

## 7. Performance notes

- Forward transforms dominate residual computation; inverse transforms dominate reconstruction.
- The 8x4 helper doubles throughput by processing two 4x4 blocks per call.
- Intermediates stay in registers; stack traffic is limited to link-register saves in tiled helpers.
- Transposes use `vtrn`/`vswp` sequences rather than memory.
- Critical correctness points: arithmetic right shifts on negative intermediates and exact saturation on reconstruction.
- Relevant public material: Hadamard/integer-transform papers, SIMD butterfly network literature, and the ARM NEON Programmer's Guide.

## 8. References

- ITU-T H.264, integer transform and scanning clauses.
- `common/dct.c` for canonical reference arithmetic.
- ARM NEON Programmer's Guide for widening differences, `vtrn`, `vswp`, and saturating narrowing.

## 9. Verification checklist

- Forward transforms produce coefficients identical to the generic C code for random and extreme inputs.
- All-zero input gives all-zero coefficients.
- Inverse transforms match generic output after clipping, including negative overflow cases.
- Extreme coefficients (`-32768`, `32767`) saturate correctly on reconstruction.
- DC-only transforms match generic DC routines for 8x8 and 8x16.
- Zigzag output is a permutation of the input and matches the standard frame order.
- Rows outside the addressed block are never written.
- Callee-saved registers are preserved.

## 10. Open questions

- `UNKNOWN`: whether `x264_dct4x4dc_neon` and `x264_idct4x4dc_neon` are installed in all ARM build configurations.
- `UNKNOWN`: exact overflow behaviour of the 8x8 butterfly near the 16-bit limit.
- `UNKNOWN`: whether high-bit-depth builds expose any of these symbols.
