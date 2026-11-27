# `common/aarch64/quant.h` — AArch64 quantization declarations

> Dirty-side behavioural specification (reverse-engineered from the reference encoder; describes behaviour, not code).

## 1. Header

- Source path: `common/aarch64/quant.h`
- Role: declares the AArch64 quantization, dequantization, decimation, last-coefficient, run-level, and DCT-denoising kernels.
- This is a **dirty-side behavioural specification** artifact for the clean-room reimplementation.

## 2. Purpose

The generic quantizer dispatcher (`common/quant.c`) replaces selected C functions with AArch64 versions based on CPU features and bit depth. This header fixes the signatures, including the distinction between plain ARMv8 kernels (usable without NEON) and NEON kernels.

## 3. Public interface

Quantization:
- `int x264_quant_2x2_dc_aarch64( int16_t dct[4], int mf, int bias )`
- `int x264_quant_2x2_dc_neon( dctcoef dct[4], int mf, int bias )`
- `int x264_quant_4x4_dc_neon( dctcoef dct[16], int mf, int bias )`
- `int x264_quant_4x4_neon( dctcoef dct[16], udctcoef mf[16], udctcoef bias[16] )`
- `int x264_quant_4x4x4_neon( dctcoef dct[4][16], udctcoef mf[16], udctcoef bias[16] )`
- `int x264_quant_8x8_neon( dctcoef dct[64], udctcoef mf[64], udctcoef bias[64] )`

Dequantization:
- `void x264_dequant_4x4_dc_neon( dctcoef dct[16], int dequant_mf[6][16], int i_qp )`
- `void x264_dequant_4x4_neon( dctcoef dct[16], int dequant_mf[6][16], int i_qp )`
- `void x264_dequant_8x8_neon( dctcoef dct[64], int dequant_mf[6][64], int i_qp )`

Decimation:
- `int x264_decimate_score{15,16,64}_neon( dctcoef * )`

Last coefficient:
- 8-bit: `int x264_coeff_last4_aarch64( dctcoef * )`, `x264_coeff_last8_aarch64( dctcoef * )`
- 10-bit: `int x264_coeff_last4_neon( ... )`, `x264_coeff_last8_neon( ... )`
- Both: `int x264_coeff_last{15,16,64}_neon( ... )`

Run-level:
- 8-bit: `int x264_coeff_level_run4_aarch64( dctcoef *, x264_run_level_t * )`
- 10-bit: `int x264_coeff_level_run4_neon( ... )`
- Both: `int x264_coeff_level_run{8,15,16}_neon( ... )`

Denoise:
- `void x264_denoise_dct_neon( dctcoef *, uint32_t *, udctcoef *, int )`

All quantizer functions return 1 when any resulting coefficient is non-zero (or a 4-bit mask for `quant_4x4x4`); decimate returns a score 0..9; last/run-level return indices/counts.

## 4. State and data structures

- `dctcoef` = signed 16-bit (8-bit build) or signed 32-bit (10-bit build).
- `udctcoef` = unsigned counterpart for multiplication factors/biases.
- `x264_run_level_t`: contains `last`, `level[]`, and `mask[]` arrays; the assembly assumes a fixed offset for `mask`.
- `dequant_mf[6][N]`: six dequantization factor sets indexed by `i_qp % 6`.

## 5. Algorithm / behaviour

Declaration only; all semantics are documented in `quant-a.S.md`. Notable interface-level facts:
- The `_aarch64` suffix denotes scalar ARMv8 kernels usable without NEON (`coeff_last4/8`, `coeff_level_run4`, and the declared `quant_2x2_dc_aarch64`).
- The `_neon` suffix denotes SIMD kernels.
- The bit-depth comment markers in the header indicate which functions are dispatched in which build; the dispatcher in `common/quant.c` makes the final decision.

## 6. Dependencies

- Included by `common/quant.c`.
- Uses common typedefs and `x264_template`.
- Implementations in `quant-a.S`.

## 7. Constants and tables

None in the header. Quantization constants and the run-level layout offset are documented in `quant-a.S.md`.

## 8. Performance notes

Declaration-only. SVE/SVE2 tip: no SVE quant kernels are declared; a clean-room port adding them must preserve the exact nonzero flags and decimate scores because they drive bitstream decisions. Public references: x264 quantizer dispatcher design; ARM NEON quantization intrinsics.

## 9. References

- ITU-T H.264 / ISO/IEC 14496-10, Clauses 8.5–8.6.
- x264 `common/quant.c`.
- `docs/rebuild/common/aarch64/quant-a.S.md`.

## 10. Verification checklist

- Every declared symbol exists in `quant-a.S` (except possibly `quant_2x2_dc_aarch64` — resolve the UNKNOWN).
- Signatures match the generic function-pointer types in `common/quant.c`.
- Return-value conventions (nonzero flag, 4-bit mask, decimate score, last index, level count) match the C implementations.

## 11. Open questions

- UNKNOWN: whether `x264_quant_2x2_dc_aarch64` is implemented in a different source file or is a stale declaration.
- UNKNOWN: exact `x264_run_level_t` layout used by the assembly.
