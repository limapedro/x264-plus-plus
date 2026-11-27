# `common/arm/quant-a.S` — ARM/NEON quantization kernels

## 1. Header

- **Source path:** `common/arm/quant-a.S`.
- **Role in one sentence:** NEON implementations of quantization, dequantization, coefficient scanning, decimation scoring, and DCT denoising.
- **Marker:** Dirty-side behavioural specification; all descriptions are behavioural and written independently of the reference source.


## 2. Purpose

This file implements the fixed-point arithmetic and coefficient analysis that follow the forward transform. These routines are invoked for every residual block, so they are among the most performance-sensitive kernels in the encoder. The assembly uses widening multiplies, bit masks, and leading-zero instructions to replace scalar branches.

## 3. Public interface

Exported symbols (declared in `arm/quant.h`):

- `x264_quant_2x2_dc_neon`, `x264_quant_4x4_dc_neon`, `x264_quant_4x4_neon`, `x264_quant_4x4x4_neon`, `x264_quant_8x8_neon`
- `x264_dequant_4x4_neon`, `x264_dequant_8x8_neon`, `x264_dequant_4x4_dc_neon`
- `x264_decimate_score15_neon`, `x264_decimate_score16_neon`, `x264_decimate_score64_neon`
- `x264_coeff_last4_arm`, `x264_coeff_last8_arm`, `x264_coeff_last15_neon`, `x264_coeff_last16_neon`, `x264_coeff_last64_neon`
- `x264_denoise_dct_neon`

ABI: first argument is the coefficient buffer in `r0`; subsequent pointers and scalars follow the EABI order, and the function returns an integer in `r0`. `r1`..`r3` and `ip` are used freely as scratch within the routines.

## 4. State and data structures

Constant tables are stored in read-only sections:
- `pmovmskb_byte` maps byte lanes to bits, used by the 64-coefficient last-index routine.
- `mask_2bit` and `mask_1bit` select run-length positions for 16- and 64-coefficient decimation.
- The decimation score tables `x264_decimate_table4` and `x264_decimate_table8` live in `common/tables.c` and are accessed via PIC-aware relocations.

## 5. Algorithm / behaviour

**Quantization.** The `QUANT_TWO` macro processes 16 coefficients per iteration. It takes absolute values, adds the bias, multiplies by the multiplier with widening unsigned multiplies, shifts right by 16, then restores the sign with an XOR/subtract pair derived from the original sign bit. The OR of all resulting coefficients forms the nonzero flag. The 4x4x4 variant repeats this for four blocks and returns a 4-bit mask; the 8x8 variant loops four times over 16 coefficients.

**Dequantization.** The `DEQUANT` macro family computes `i_qbits = qp/6 - offset` and `i_mf = qp%6` using a multiply-by-43/shift-by-8 trick. When the shift is non-negative, coefficients are multiplied by the dequantization factor and shifted left; otherwise a rounding constant `1 << (-i_qbits - 1)` is added before an arithmetic right shift. The 8x8 variant repeats the operation four times with a loop counter. The DC-only variant loads a single factor and broadcasts it across all 16 coefficients.

**Decimation score.** Coefficients are narrowed to signed bytes; an early test detects any coefficient with magnitude greater than 1 and returns 9 immediately. Zero positions are converted to a bit mask, inverted, and scanned with `clz`/`rbit` loops. Each zero-run length indexes the appropriate decimation table and its value is added to the score. The 15-coefficient version shifts the input by 2 bytes to skip the DC term.

**Coefficient last.** The 4- and 8-coefficient variants use scalar word loads and explicit sign tests. The 15/16-coefficient NEON variants test 16 lanes for nonzero, narrow the result to a 32-bit mask, and use `clz` to find the highest nonzero position. The 64-coefficient version narrows 64 lanes to a 64-bit mask using `vqmovn` and the byte-to-bit table, then scans both 32-bit halves.

**Denoising.** The routine accumulates magnitudes into the caller's sum array, subtracts per-coefficient offsets, and writes the sign-restored result, or zero when the adjusted magnitude is negative.

## 6. Dependencies

- Includes `common/arm/asm.S`.
- Declared in `arm/quant.h` and installed by `x264_quant_init` in `common/quant.c`.
- Uses decimation tables from `common/tables.c` and typedefs from `common/common.h`.

## 7. Constants and tables

- Fixed-point shift 16 for quantization.
- Dequantization base offsets 4 (4x4) and 6 (8x8 and DC-only).
- Decimation table values: 4-wide `{3,2,2,1,1,1,0,...}`, 8-wide `{3,3,3,3,2,...,1,...,0,...}`.
- Early-exit decimation score 9.

## 8. Performance notes

All quantization paths use 16-bit vector lanes with widening 32-bit intermediates, avoiding overflow for typical coefficient ranges. Sign restoration via XOR/subtract is cheaper than branching per coefficient. The decimation and last-index routines replace scalar loops with CLZ scans over bit masks, which is the key speedup. Cache pressure is minimal because blocks are 32 to 128 bytes. A clean implementation should reproduce the exact fixed-point equations first; the SIMD formulation can be layered on afterward.

## 9. References

- `common/quant.c` for canonical quantization and decimation semantics.
- `common/tables.c` for decimation table contents.
- JVT-B118 for the purpose of coefficient decimation.
- ARM NEON Programmer's Guide for `vmull`, `vmlal`, `vqmovn`, and `vclz`.

## 10. Verification checklist

- Quantized coefficients match the reference equation for all sign combinations and zero.
- Returned nonzero flags/masks are correct for all four sub-blocks.
- Dequantization matches scalar results across QP values below and above the shift crossover.
- Decimation score matches the scalar implementation, including early exit at 9.
- Last-index routines return -1 for all-zero input and the correct index otherwise.
- Denoising accumulates original magnitudes and preserves signs correctly.

## 11. Open questions

- `UNKNOWN`: exact overflow behaviour when dequantization factors and coefficients combine to exceed 16 bits.
- `UNKNOWN`: whether the 2x2 DC ARMv6 routine is still installed on NEON-only cores where the NEON version supersedes it.
