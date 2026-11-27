# `common/arm/predict-a.S` — ARM/NEON intra-prediction kernels

## 1. Header

- **Source path:** `common/arm/predict-a.S`.
- **Role in one sentence:** assembly implementations of H.264 intra prediction for 4x4, 8x8, 8x16-chroma, and 16x16 blocks on ARMv6 and NEON.
- **Marker:** Dirty-side behavioural specification; all descriptions are behavioural and written independently of the reference source.


## 2. Purpose

Intra prediction is invoked for every candidate mode during intra analysis and once per block during reconstruction. This file provides the optimized kernels that fill a block from neighboring samples, including the directional interpolation modes that are most expensive in scalar code.

## 3. Public interface

Exported symbols match `arm/predict.h`:

- ARMv6 4x4: `x264_predict_4x4_h_armv6`, `x264_predict_4x4_v_armv6`, `x264_predict_4x4_dc_armv6`, `x264_predict_4x4_ddr_armv6`.
- NEON 4x4: `x264_predict_4x4_dc_top_neon`, `x264_predict_4x4_ddl_neon`.
- NEON 8x8 luma: `x264_predict_8x8_{dc,h,v,ddl,ddr,vl,vr,hd,hu}_neon`, each taking `(uint8_t *dst, uint8_t edge[36])`.
- NEON 8x8 chroma: `x264_predict_8x8c_{dc,dc_top,dc_left,h,v,p}_neon`.
- NEON 8x16 chroma: `x264_predict_8x16c_{dc_top,h,p}_neon`.
- NEON 16x16: `x264_predict_16x16_{dc,dc_top,dc_left,h,v,p}_neon`.

The 4x4 routines take only a destination pointer. The 8x8 luma routines additionally take the filtered edge array produced by the generic edge filter.

ABI: `r0` is the destination block, `r1` the edge array where applicable, and `ip`/`r12` are used as `FDEC_STRIDE` scratch. Callee-saved registers are pushed where needed.

## 4. State and data structures

The edge array layout mirrors the generic `predict_8x8_filter_c` producer: left neighbors, top neighbors, and corner extensions are packed into a 36-byte buffer. The 16x16 plane-mode kernel uses a constant vector `p16weight` containing the values 1 through 8 to generate horizontal gradients across two halves of the block. Helper macros `ldcol.8`, `ldcol.16`, and `add16x8` load neighbor columns and reduce them with pairwise adds.

## 5. Algorithm / behaviour

- **Horizontal / vertical modes** replicate the left column or copy the top row across every block row using splat and vector-store instructions.
- **DC modes** sum the available neighbors with the correct rounding: 4x4 DC uses `(sum + 4) >> 3` for the combined case and `(sum + 2) >> 2` for top- or left-only; 8x8 chroma DC computes four quadrant values from top and left sums.
- **Plane modes** compute horizontal and vertical gradients from neighbors, derive the `a`, `b`, `c` plane parameters with the fixed-point formulas used in `common/predict.c`, and generate each row with a vector of successive column indices added to a row base. Output is saturated to byte range. The 8x8 chroma variant uses `(17H+16)>>5`; the 16x16 variant uses `(5H+32)>>6`.
- **Diagonal modes** apply one- or three-tap filters along diagonals. The three-tap filter is `(a + 2b + c + 2) >> 2`; the two-tap average is `(a+b+1) >> 1`. The ARMv6 DDR kernel uses a dedicated low-pass macro that forms these values with `uhadd8`/`uadd8` and parity-correction tricks. The NEON DDL kernel uses `vhadd.u8` and `vrhadd.u8` to the same effect.
- **VL/VR/HD/HU modes** combine the two filter widths along their respective diagonal directions, with replicated edge samples at block boundaries.

All kernels write exactly the block dimensions they are named for, leaving the rest of the reconstructed frame untouched.

## 6. Dependencies

- Includes `common/arm/asm.S`.
- Declared in `arm/predict.h` and installed by `arm/predict-c.c`.
- Depends on the generic edge-array producer in `common/predict.c` for 8x8 luma modes.

## 7. Constants and tables

- `FDEC_STRIDE = 32`.
- Plane coefficients: 5/64 for 16x16, 17/32 for 8x8 chroma; rounding constants 32 and 16.
- Low-pass filter weights (1,2,1) with rounding 2; two-tap average rounding 1.
- `p16weight` vector: 1..8.

## 8. Performance notes

The 4x4 ARMv6 routines use scalar word operations with the free shift in `add` instructions because vector setup would cost more than the 16-byte block is worth. NEON becomes advantageous at 8x8 and 16x16, where a single vector store fills a row. The plane kernels avoid per-pixel multiplication by maintaining a running column-index vector. A clean implementation should prioritize exact neighbor selection and rounding; SIMD details can follow.

## 9. References

- ITU-T H.264, intra prediction specification.
- `common/predict.c` for canonical formulas and edge-array layout.
- ARM NEON Programmer's Guide for `vhadd`, `vrhadd`, `vext`, and `vdup`.

## 10. Verification checklist

- Every mode matches the generic C prediction byte for byte on random neighbors.
- Edge cases with missing top/left/corner neighbors match the generic substitution rules.
- Plane-mode output is clipped to 0..255.
- DC accumulator does not overflow for maximum neighbor values.
- Destination rows outside the block are unchanged.
- The edge array is read-only from the kernel's perspective.

## 11. Open questions

- `UNKNOWN`: exact mapping of the 36-byte edge array indices for every 8x8 luma mode; this is inherited from the generic filter producer.
- `UNKNOWN`: whether the NEON 4x4 DC-top and DDL kernels are measurably faster than C on all target cores.
