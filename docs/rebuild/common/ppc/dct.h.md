# `common/ppc/dct.h` — PowerPC transform and zigzag declarations

## 1. Header

- **Source path:** `common/ppc/dct.h`.
- **Role in one sentence:** declares the AltiVec implementations of transforms, DC transforms, and scan interleaving used on PowerPC.
- **Marker:** Dirty-side behavioural specification; all descriptions are behavioural and written independently of the reference source.


## 2. Purpose

This header provides the prototypes for the PowerPC back-end transform kernels. It allows `common/dct.c` to install optimized pointers through the generic `x264_dct_function_t` table while keeping AltiVec code out of the generic path.

## 3. Public interface

Declared functions:

- Forward 4x4/8x8/16x16 transforms: `x264_sub4x4_dct_altivec`, `x264_sub8x8_dct_altivec`, `x264_sub16x16_dct_altivec`.
- Inverse 4x4/8x8/16x16 transforms: `x264_add4x4_idct_altivec`, `x264_add8x8_idct_altivec`, `x264_add16x16_idct_altivec`.
- 8x8 transform variants: `x264_sub8x8_dct8_altivec`, `x264_sub16x16_dct8_altivec`, `x264_add8x8_idct8_altivec`, `x264_add16x16_idct8_altivec`.
- DC-only paths: `x264_sub8x8_dct_dc_altivec`, `x264_add8x8_idct_dc_altivec`, `x264_add16x16_idct_dc_altivec`.
- Zigzag: `x264_zigzag_scan_4x4_frame_altivec`, `x264_zigzag_scan_4x4_field_altivec`, `x264_zigzag_scan_8x8_frame_altivec`.
- CAVLC interleaving: `x264_zigzag_interleave_8x8_cavlc_altivec`.

Source strides follow the same convention as other architectures: `pix1` uses `FENC_STRIDE = 16`, `pix2` and `p_dst` use `FDEC_STRIDE = 32`.

## 4. State and data structures

All routines are stateless. Coefficient buffers use `int16_t` lanes because the PowerPC back end is compiled for 8-bit samples. The 8x8 frame zigzag routine writes 64 coefficients into a flat array. The CAVLC interleave routine additionally updates an 8-byte `nnz` array.

## 5. Algorithm / behaviour

The declared functions implement the same transforms as their ARM counterparts (see `common/arm/dct.h.md`): separable integer transforms with fixed shifts, rounded inverse transforms, and saturation to byte range on reconstruction. The AltiVec implementations use 16-bit vector lanes throughout, with widening byte differences on input and saturating pack instructions on output.

The field zigzag scan for 4x4 blocks uses a different coefficient order than the frame scan, reflecting interlaced coding. The 8x8 CAVLC interleave routine writes the four 4x4 blocks of an 8x8 region in CAVLC scan order and computes a nonzero bitmap from the coefficients.

## 6. Dependencies

- Implemented by `common/ppc/dct.c`.
- Installed by `x264_dct_init` and `x264_zigzag_init` in `common/dct.c`.
- Uses vector macros from `common/ppc/ppccommon.h`.

## 7. Constants and tables

- `FENC_STRIDE = 16`, `FDEC_STRIDE = 32`.
- Final inverse-transform shift is 6 with rounding; 8x8 butterflies use intermediate shifts of 1 and 2.
- Scan orders are defined by the H.264 standard and encoded as vector selector masks in the implementation.

## 8. Performance notes

AltiVec's 16-byte vectors map naturally to 8x8 blocks of 16-bit coefficients, making the 8x8 transform particularly efficient. The 4x4 transforms pack two rows per vector and use `VEC_TRANSPOSE_4`. CAVLC interleaving is memory-bound but benefits from pack/permute instructions. Clean implementations should focus on lane ordering, which differs between big- and little-endian PowerPC.

## 9. References

- ITU-T H.264 transform and scanning specification.
- `common/dct.c` for reference arithmetic.
- AltiVec Programming Interface Manual.

## 10. Verification checklist

- All transforms match the generic C implementations on randomized inputs.
- Frame and field 4x4 scans produce the standard coefficient orders.
- 8x8 scan is a permutation of its input.
- CAVLC interleave produces correct coefficient order and `nnz` bitmap.
- Reconstructed pixels are saturated to 0..255.

## 11. Open questions

- `UNKNOWN`: whether all declared functions are installed in both 32-bit and 64-bit PowerPC builds.
- `UNKNOWN`: whether high-bit-depth builds use any of these symbols.
