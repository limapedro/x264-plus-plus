# `common/ppc/quant.c` — PowerPC quantization implementation

**Dirty-side behavioural specification.** Source path: `common/ppc/quant.c`. Role: AltiVec implementations of quantization and dequantization for 4x4, 4x4x4, 8x8, 4x4-DC and 2x2-DC blocks.

## 1. Purpose

This file implements the fixed-point quantization arithmetic used after the forward transform and its inverse. The AltiVec code processes 16 coefficients per iteration using widening 32-bit multiplies and vector sign reconstruction, with no per-coefficient branches.

## 2. Public interface

Exported functions declared in `ppc/quant.h`:

- `x264_quant_4x4_altivec`, `x264_quant_4x4x4_altivec`, `x264_quant_8x8_altivec`, `x264_quant_4x4_dc_altivec`, `x264_quant_2x2_dc_altivec`
- `x264_dequant_4x4_altivec`, `x264_dequant_8x8_altivec`

Quantizers return a nonzero flag or a 4-bit mask for 4x4x4; dequantizers return nothing.

## 3. State and data structures

All routines are stateless. Multiplier and bias tables are caller-owned 16-bit arrays. The dequantization factor table has six rows selected by `i_qp % 6`. Vector lanes hold eight 16-bit coefficients each, so a 4x4 block requires two vectors.

## 4. Algorithm / behaviour

**Quantization.** The `QUANT_16_U` macro loads two 8-lane coefficient vectors and their multiplier/bias tables, records sign masks, takes absolute values, adds bias with saturating addition, and multiplies with `vec_mule` and `vec_mulo` to produce 32-bit even/odd products. The products are shifted right by 16, packed back to 16-bit lanes, and reordered into coefficient order with doubleword permutations and merges. The sign masks are applied with XOR followed by addition, which negates negative results. The OR of all outputs forms the nonzero flag. The 4x4x4 routine interleaves four blocks to hide multiply latency and returns a bit per block. The 8x8 routine loops four times.

The DC variants use scalar multiplier and bias values splatted into vectors. The 2x2 variant operates on one vector and uses a selector to replace only the first four lanes, returning nonzero only for those lanes.

**Dequantization.** The shift count and factor index are computed as `qp/6 - base` and `qp%6`, with base 4 for 4x4 and 6 for 8x8. For a non-negative shift, coefficients are multiplied by the factor and shifted left. For a negative shift, a rounding constant `1 << (-shift-1)` is added before an arithmetic right shift. Because AltiVec's multiply-even/odd instructions have endian-dependent semantics, the code defines `VEC_MULE`/`VEC_MULO` to swap them on little-endian targets. The 32-bit intermediates are combined with `vec_sl` by 16 and added, then shifted and packed to 16-bit lanes.

## 5. Dependencies

- Includes `common/common.h`, `ppccommon.h`, local `quant.h`.
- Installed by `x264_quant_init` when `X264_CPU_ALTIVEC` is set.
- Uses `xxpermdi` from `ppccommon.h` for lane repacking.

## 6. Constants and tables

- Fixed-point multiplier shift 16.
- Dequantization base offsets 4 (4x4) and 6 (8x8).
- Rounding constant `1 << (-shift-1)` for right-shift dequantization.
- Endian-dependent selection of even/odd multiply.

## 7. Performance notes

The 4x4x4 routine is the most optimized: it loads all four blocks first, then interleaves multiplies and packs so the pipeline stays full. Sign reconstruction with XOR/add avoids branches. The main portability risk is lane ordering after `vec_mule`/`vec_mulo` and `xxpermdi`; these must be validated separately on big- and little-endian builds.

## 8. References

- `common/quant.c` for canonical equations.
- `common/ppc/quant.c` for implementation.
- AltiVec Programming Interface Manual.

## 9. Verification checklist

- Quantized coefficients match the reference equation for positive, negative, and zero inputs.
- Nonzero flags and 4-bit masks are correct.
- Dequantization matches scalar results across the shift-count crossover.
- Big- and little-endian builds produce identical logical results.
- Results do not overflow 16-bit lanes for supported QP ranges.

## 10. Open questions

- `UNKNOWN`: whether high-bit-depth builds use any of these routines.
- `UNKNOWN`: exact overflow behaviour for extreme QP and coefficient values.
