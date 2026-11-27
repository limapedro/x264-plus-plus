# `common/ppc/dct.c` — PowerPC transform and zigzag implementation

**Dirty-side behavioural specification.** Source path: `common/ppc/dct.c`. Role: AltiVec implementations of the 4x4/8x8 forward and inverse transforms, DC transforms, frame/field zigzag scans, and CAVLC coefficient interleaving.

## 1. Purpose

This file contains the PowerPC transform kernels. All routines are compiled only for 8-bit builds and use 16-bit vector lanes to operate on blocks of coefficients in parallel. The code is structured around two one-dimensional transform macros that are applied along rows and then along columns after a vector transpose.

## 2. Public interface

All functions are declared in `common/ppc/dct.h` and described there in detail. The exported set covers:

- 4x4 forward and inverse transforms at 4x4, 8x8 and 16x16 granularity.
- 8x8 forward and inverse transforms at 8x8 and 16x16 granularity.
- 4x4 DC Hadamard and inverse plus 8x8/8x16 DC-only forward transforms and 8x8/16x16 DC inverse transforms.
- 4x4 frame and field zigzag, 8x8 frame zigzag, and 8x8 CAVLC interleave.

ABI: arguments follow the C signatures in the header, with `r3`-style PowerPC register conventions handled by the compiler because these are ordinary C functions with AltiVec intrinsics.

## 3. State and data structures

All transforms are stateless. The only persistent data are constant vector selectors embedded in the functions, such as the byte-permutation masks `permHigh` and `permLow` used to repack 16-bit transform results into coefficient arrays, the 14-element mask array used by the 8x8 frame scan, the three mask vectors used by the CAVLC interleave, and the `inverse_bridge_shuffle` masks used by related code in `mc.c`. Intermediate coefficients are held in `vec_s16_t` variables.

## 4. Algorithm / behaviour

**4x4 forward transform.** `VEC_DCT` implements a one-dimensional butterfly: it forms sums and differences of the four source lanes, then produces four outputs where the odd coefficients have an extra factor of two. `x264_sub4x4_dct_altivec` loads four 8-byte rows using the 8-byte-aligned difference macro, widens them to signed 16-bit, applies `VEC_DCT` along rows, transposes the 4x4 matrix with `VEC_TRANSPOSE_4`, then applies `VEC_DCT` along columns. Two byte-permutes select the high or low halves of the 16-bit lanes and store the 16 coefficients in raster order. The 8x8 and 16x16 variants call the 4x4 routine on four or sixteen sub-blocks.

**8x8 forward transform.** `DCT8_1D_ALTIVEC` implements the unnormalized 8-point transform. It forms symmetric sums `s07`, `s16`, `s25`, `s34`, combines them into `a0..a3`, forms differences `d07..d34`, and derives `a4..a7` using a factor of 1.5 implemented as `d + (d>>1)`. Outputs are `a0+a1`, `a4+(a7>>2)`, `a2+(a3>>1)`, `a5+(a6>>2)`, and the symmetric complements. The 8x8 routine applies this along rows, transposes with `VEC_TRANSPOSE_8`, and applies it along columns. The 16x16 variant calls it on four sub-blocks.

**DC transforms.** `x264_sub8x8_dct_dc_altivec` accumulates byte differences over 4x4 quadrants, uses `vec_sum4s`/`vec_packs` to reduce to four DC sums per 8x8 block, then applies a 2x2 Hadamard network using doubleword permutations and merge operations. The result is written into the first four 16-bit positions of the destination. `x264_add8x8_idct_dc_altivec` divides each DC coefficient by 64 with rounding (implemented as adding 32 and shifting right by 6), broadcasts it across a quadrant with merge operations, and adds it to destination bytes with `vec_adds` followed by `vec_packsu`. The 16x16 variant repeats for four 8x8 regions.

**Inverse transforms.** `IDCT_1D_ALTIVEC` computes the 4-point inverse: `a0 = s0+s2`, `a1 = s0-s2`, `a2 = (s1>>1)-s3`, `a3 = (s3>>1)+s1`, producing outputs `a0+a3`, `a1+a2`, `a1-a2`, `a0-a3`. `x264_add4x4_idct_altivec` adds 32 to the DC coefficient for final rounding, applies the 1D inverse along rows, transposes, applies it along columns, shifts right by 6, adds to the destination, and packs with saturation. `IDCT8_1D_ALTIVEC` implements the 8-point inverse with a 5/20 structure and shifts by 1 and 2, followed by the same rounding, addition, and saturation.

**Zigzag.** The 4x4 frame scan uses two byte-selector vectors with `vec_perm` to place the sixteen coefficients in progressive order. The field scan uses a different selector for the lower half. The 8x8 frame scan uses a sequence of merges and fourteen permutation masks to gather coefficients along the standard zigzag path; masks contain byte offsets into the eight loaded vectors.

**CAVLC interleave.** The routine loads eight 16-bit vectors, merges adjacent pairs, permutes them into interleaved order, and writes eight output vectors. It also ORs all source vectors, packs the result to bytes, compares against zero, and updates the eight-byte `nnz` bitmap using a selector mask.

## 5. Dependencies

- Includes `common/common.h`, `common/ppc/ppccommon.h`, and local `dct.h`.
- Installed through `common/dct.c` and `common/zigzag` dispatch.
- Uses vector helpers and transpose macros from `ppccommon.h`.

## 6. Constants and tables

- `FENC_STRIDE = 16`, `FDEC_STRIDE = 32`.
- Inverse rounding constant 32, final shift 6.
- 8x8 transform constants 1.5 (`d + (d>>1)`), shifts 1 and 2.
- Two byte-permutation vectors for 4x4 output repacking, fourteen masks for the 8x8 zigzag, three masks for CAVLC interleave.

## 7. Performance notes

All kernels keep intermediates in vector registers and use `VEC_TRANSPOSE_8`/`VEC_TRANSPOSE_4` instead of memory transposes. The 8x8 path processes eight columns per vector, which is a natural fit for AltiVec. The zigzag routines are permutation-heavy and benefit from few loads. Clean implementations should pay special attention to endian-dependent packs and permutes: `vec_mergeh`/`vec_mergel` and `vec_perm` lane ordering differ between big- and little-endian PowerPC, and `ppccommon.h` provides explicit helpers for the common widening cases.

## 8. References

- ITU-T H.264 integer transform and scanning specification.
- `common/dct.c` for reference arithmetic.
- AltiVec Technology Programming Interface Manual.

## 9. Verification checklist

- Every transform matches the generic C implementation on randomized inputs.
- Frame and field 4x4 zigzag orders match the standard tables.
- 8x8 zigzag output is a permutation of its input.
- CAVLC interleave produces correct ordering and nonzero bitmap.
- Reconstructed pixels are saturated to 0..255.
- Big- and little-endian builds produce identical logical results.

## 10. Open questions

- `UNKNOWN`: whether all declared functions are installed on VSX builds where different intrinsics might be preferable.
- `UNKNOWN`: exact overflow behaviour of the 8x8 transform on extreme coefficient values.
