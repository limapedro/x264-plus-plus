# `common/ppc/quant.h` — PowerPC quantization declarations

## 1. Header

- **Source path:** `common/ppc/quant.h`.
- **Role in one sentence:** declares the AltiVec quantization and dequantization kernels available on PowerPC.
- **Marker:** Dirty-side behavioural specification; all descriptions are behavioural and written independently of the reference source.


## 2. Purpose

This header exposes the PowerPC quantization kernels to the generic dispatcher. The set is smaller than ARM's: five quantizers and two dequantizers, with no coefficient-analysis or denoising routines.

## 3. Public interface

- `int x264_quant_4x4x4_altivec( int16_t dct[4][16], uint16_t mf[16], uint16_t bias[16] )`
- `int x264_quant_4x4_altivec( int16_t dct[16], uint16_t mf[16], uint16_t bias[16] )`
- `int x264_quant_8x8_altivec( int16_t dct[64], uint16_t mf[64], uint16_t bias[64] )`
- `int x264_quant_4x4_dc_altivec( int16_t dct[16], int mf, int bias )`
- `int x264_quant_2x2_dc_altivec( int16_t dct[4], int mf, int bias )`
- `void x264_dequant_4x4_altivec( int16_t dct[16], int dequant_mf[6][16], int i_qp )`
- `void x264_dequant_8x8_altivec( int16_t dct[64], int dequant_mf[6][64], int i_qp )`

All symbols use the `x264_template` naming convention. Quantizers return a nonzero flag (4x4x4 returns a 4-bit mask); dequantizers return nothing.

## 4. State and data structures

`mf` and `bias` are 16-bit fixed-point tables in the caller's memory. The 4x4x4 function processes four coefficient blocks sharing the same tables. Dequantization uses the six-entry factor table selected by `i_qp % 6` and a shift count derived from `i_qp / 6`.

## 5. Algorithm / behaviour

Quantization computes, for each coefficient, `((bias + |coef|) * mf) >> 16` with the sign restored. Resulting coefficients are written back in place and OR-ed to derive the nonzero result. The 4x4x4 variant produces one bit per block.

Dequantization multiplies coefficients by the selected factor and applies either a left shift by `qp/6 - 4` (4x4) or `qp/6 - 6` (8x8) when non-negative, or adds `1 << (-shift-1)` and shifts right when negative. The PowerPC implementation uses vector multiply-even/odd instructions and doubleword permutations to pack 32-bit intermediates back to 16-bit lanes.

## 6. Dependencies

- Implemented in `common/ppc/quant.c`.
- Installed by `x264_quant_init` when `X264_CPU_ALTIVEC` is set.
- Uses vector helpers from `ppccommon.h` and typedefs from `common/common.h`.

## 7. Constants and tables

- Fixed-point shift 16.
- Dequantization base offsets 4 and 6.
- Endian-dependent multiply-even/odd selection in `ppccommon.h`.

## 8. Performance notes

The Altivec quantizers process 16 coefficients per iteration with widening multiplies, avoiding scalar branches. The 4x4x4 variant interleaves four blocks to hide multiply latency. Byte-order handling for `vec_mule`/`vec_mulo` is endian-dependent and must be preserved exactly.

## 9. References

- `common/quant.c` for canonical equations.
- `common/ppc/quant.c` for implementation specifics.
- AltiVec Programming Interface Manual.

## 10. Verification checklist

- Quantized coefficients match the reference equation for positive and negative inputs.
- Nonzero masks are correct for all four blocks.
- Dequantization matches scalar results across the shift-count crossover.
- Results are identical on big- and little-endian PowerPC builds.

## 11. Open questions

- `UNKNOWN`: whether high-bit-depth builds install any of these routines.
