# `common/arm/quant.h` — ARM quantization declarations

**Dirty-side behavioural specification.** Source path: `common/arm/quant.h`. Role: declares NEON quantization, dequantization, coefficient analysis, and DCT denoising kernels.

## 1. Purpose

Quantization and its companion coefficient scans run for every transformed block and are among the hottest parts of the encoder. This header exposes the ARM-optimized kernels so the generic quantizer dispatcher can install them, and documents the fixed-point conventions that the assembly relies on.

## 2. Public interface

Quantization (each returns a nonzero flag or a 4-bit mask indicating which of four 4x4 blocks produced nonzero coefficients):
- `int x264_quant_2x2_dc_armv6( int16_t dct[4], int mf, int bias )`
- `int x264_quant_2x2_dc_neon( int16_t dct[4], int mf, int bias )`
- `int x264_quant_4x4_dc_neon( int16_t dct[16], int mf, int bias )`
- `int x264_quant_4x4_neon( int16_t dct[16], uint16_t mf[16], uint16_t bias[16] )`
- `int x264_quant_4x4x4_neon( int16_t dct[4][16], uint16_t mf[16], uint16_t bias[16] )`
- `int x264_quant_8x8_neon( int16_t dct[64], uint16_t mf[64], uint16_t bias[64] )`

Dequantization:
- `void x264_dequant_4x4_dc_neon( int16_t dct[16], int dequant_mf[6][16], int i_qp )`
- `void x264_dequant_4x4_neon( int16_t dct[16], int dequant_mf[6][16], int i_qp )`
- `void x264_dequant_8x8_neon( int16_t dct[64], int dequant_mf[6][64], int i_qp )`

Coefficient analysis:
- `int x264_decimate_score15_neon( int16_t * )`, `int x264_decimate_score16_neon( int16_t * )`, `int x264_decimate_score64_neon( int16_t * )`
- `int x264_coeff_last4_arm( int16_t * )`, `int x264_coeff_last8_arm( int16_t * )`
- `int x264_coeff_last15_neon`, `int x264_coeff_last16_neon`, `int x264_coeff_last64_neon` (same single-pointer signature)
- `void x264_denoise_dct_neon( dctcoef *, uint32_t *, udctcoef *, int )`

All symbols are mangled with `x264_template`.

## 3. State and data structures

`mf` and `bias` are caller-supplied per-coefficient tables in 16.16 fixed point. `dequant_mf` is a 6xN table selected by `i_qp % 6`, with a shift count derived from `i_qp / 6`. `denoise_dct` accumulates per-coefficient magnitudes into a caller-owned `sum` array and subtracts per-coefficient offsets, writing the cleaned coefficients back in place.

## 4. Algorithm / behaviour

The quantization equation is: for a coefficient `c` with multiplier `m` and bias `f`, compute `q = ((f + |c|) * m) >> 16`, then restore the original sign. The functions OR all quantized coefficients to derive the "any nonzero" result. The 4x4x4 variant returns a 4-bit mask with one bit per 4x4 block.

Dequantization uses `i_qbits = i_qp/6 - 4` for 4x4 blocks and `i_qp/6 - 6` for 8x8 blocks. When the shift count is non-negative, coefficients are multiplied by the dequantization factor and shifted left; when negative, a rounding constant `1 << (-i_qbits - 1)` is added before an arithmetic right shift by `-i_qbits`. The DC-only 4x4 variant uses only entry zero of the factor table and a shift count of `i_qp/6 - 6`.

`coeff_lastN` returns the highest index with a nonzero coefficient, or -1 if all are zero. `decimate_scoreN` walks the scan from the end, returns 9 immediately if any coefficient has magnitude greater than 1, and otherwise sums table values indexed by the length of each zero run. The 15-coefficient variants skip the DC coefficient by shifting the input pointer. `denoise_dct` accumulates magnitudes, subtracts a per-coefficient offset, and zeros negative results while restoring the sign.

## 5. Dependencies

- Installed by `x264_quant_init` in `common/quant.c` when ARMv6/NEON features are present.
- Implemented in `common/arm/quant-a.S`.
- Uses `dctcoef`/`udctcoef` typedefs from `common/common.h` and the decimation tables from `common/tables.c`.

## 6. Constants and tables

- Fixed-point multiplier shift: 16.
- `x264_decimate_table4` has values `3,2,2,1,1,1,0,...`; `x264_decimate_table8` uses `3,3,3,3,2,...,1,...,0`.
- Decimation score saturates at 9.
- 4x4 dequantization base offset is 4; 8x8 and DC-only base offset is 6.

## 7. Performance notes

Quantization is used for every block, so even small savings multiply across the frame. The NEON kernels process 16 coefficients per iteration with widening multiplies and narrowing stores, avoiding per-coefficient branches. The coefficient-last routines use leading-zero / count-leading-zero instructions for fast scan termination. The decimation kernels convert coefficient magnitudes to bit masks and use CLZ loops over run lengths. Clean rebuilds should first implement the equations exactly, then introduce SIMD; subtle sign handling in quantization is a common source of mismatch.

## 8. References

- `common/quant.c` for canonical equations and decimation semantics.
- `common/tables.c` for decimation table values.
- JVT-B118 for the rationale behind coefficient decimation.
- ARM NEON Programmer's Guide for widening multiply and narrowing store instructions.

## 9. Verification checklist

- Quantized coefficients match the scalar equation for positive, negative, and zero inputs.
- Returned nonzero mask is correct for each of the four sub-blocks in 4x4x4 mode.
- Dequantization matches scalar results for QP values below, at, and above the shift-count crossover.
- `coeff_lastN` returns -1 for all-zero input and the correct last index otherwise.
- Decimation score matches the scalar implementation, including the early-exit value 9.
- Denoising accumulates the original magnitudes and preserves signs of retained coefficients.

## 10. Open questions

- `UNKNOWN`: whether `x264_quant_4x4x4_neon` can return bit patterns other than 0..15; the reference returns a 4-bit mask.
- `UNKNOWN`: exact overflow behaviour of the 16-bit dequantization path for extreme QP and coefficient values.
