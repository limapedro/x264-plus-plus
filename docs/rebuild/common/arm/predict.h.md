# `common/arm/predict.h` — ARM intra-prediction declarations

## 1. Header

- **Source path:** `common/arm/predict.h`.
- **Role in one sentence:** declares the ARM/NEON intra-prediction kernels and their dispatch functions.
- **Marker:** Dirty-side behavioural specification; all descriptions are behavioural and written independently of the reference source.


## 2. Purpose

Intra prediction is invoked once per candidate mode per block during analysis and once per block during reconstruction, making it a significant fraction of intra-heavy encodes. This header exposes the ARM implementations for 4x4, 8x8, 8x8-chroma, 8x16-chroma and 16x16 blocks, together with the five initializer functions the generic code calls to install them.

## 3. Public interface

Prediction kernels (all write to the current block at `src`, using `FDEC_STRIDE = 32` as the row stride):

- 4x4 luma: `x264_predict_4x4_dc_armv6`, `x264_predict_4x4_v_armv6`, `x264_predict_4x4_h_armv6`, `x264_predict_4x4_ddr_armv6`, `x264_predict_4x4_dc_top_neon`, `x264_predict_4x4_ddl_neon`. Each takes a single `uint8_t *src`.
- 8x8 luma: nine functions `x264_predict_8x8_{dc,h,v,ddl,ddr,vl,vr,hd,hu}_neon`, each taking `(uint8_t *src, uint8_t edge[36])`.
- 8x8 chroma: `x264_predict_8x8c_{dc,dc_top,dc_left,h,v,p}_neon`, each taking `uint8_t *src`.
- 8x16 chroma: `x264_predict_8x16c_{dc_top,h,p}_neon`.
- 16x16 luma: `x264_predict_16x16_{dc,dc_top,dc_left,h,v,p}_neon`.

Initializer functions:
- `void x264_predict_4x4_init_arm( uint32_t cpu, x264_predict_t pf[12] )`
- `void x264_predict_8x8_init_arm( uint32_t cpu, x264_predict8x8_t pf[12], x264_predict_8x8_filter_t *predict_filter )`
- `void x264_predict_8x8c_init_arm( uint32_t cpu, x264_predict_t pf[7] )`
- `void x264_predict_8x16c_init_arm( uint32_t cpu, x264_predict_t pf[7] )`
- `void x264_predict_16x16_init_arm( uint32_t cpu, x264_predict_t pf[7] )`

All symbols are mangled through `x264_template`.

## 4. State and data structures

The `edge[36]` parameter used by 8x8 luma prediction is a precomputed, filtered neighbor array. Its layout is fixed: indices 0..7 hold the top row, 8..15 the top-right extension, 16..23 the left column, and 24..35 the bottom-left/top-left extension, depending on the exact filter path. The NEON 8x8 functions consume the same edge array as the generic `x264_predict_8x8_filter_t` producer, so the layout is an ABI between producer and consumer.

The `pf` arrays are indexed by the prediction-mode enumerations (`I_PRED_4x4_*`, `I_PRED_8x8_*`, `I_PRED_16x16_*`, `I_PRED_CHROMA_*`) declared in `common/predict.h`.

## 5. Algorithm / behaviour

The prediction modes follow the H.264 specification and the generic C implementations in `common/predict.c`:

- **Vertical / horizontal**: copy the row above / column to the left into every row of the block.
- **DC**: average available neighbors with rounding. For 4x4 DC the eight neighbors are summed and shifted right by 3 with a rounding constant of 4; top-only and left-only variants shift by 2 with rounding 2.
- **Plane (P)**: compute horizontal and vertical gradients (`H`, `V`) from neighbor samples, derive `a`, `b`, `c` using fixed-point coefficients, then fill each pixel with a clipped, rounded plane value. For 16x16 the coefficients are `b = (5H+32)>>6`, `c = (5V+32)>>6`; the 8x8-chroma variant uses `(17H+16)>>5` and `(17V+16)>>5`.
- **Diagonal down-left / down-right (DDL/DDR)**: apply a 3-tap low-pass filter `(a + 2b + c + 2) >> 2` along diagonals, using replicated edge samples when neighbors are unavailable.
- **VL/VR/HD/HU**: combinations of one-tap `(a+b+1)>>1` and three-tap filters along the corresponding diagonal directions.

The ARM implementations use ARMv6 SIMD for 4x4 modes and NEON byte-vector shuffles for larger blocks. They must produce bit-identical results to the C reference.

## 6. Dependencies

- Consumed by `x264_predict_*_init` dispatchers in `common/predict.c`.
- Implemented in `common/arm/predict-a.S` and `common/arm/predict-c.c`.
- For 8x8 luma prediction, the edge array is built by the generic edge-filter routine; the ARM initializer does not override that producer.

## 7. Constants and tables

- `FDEC_STRIDE = 32`.
- Plane-mode weights: 5/64 scaling for 16x16, 17/32 for 8x8 chroma, with rounding constants 32 and 16.
- 3-tap low-pass weights `(1,2,1)` with rounding constant 2; 2-tap average uses rounding constant 1.
- A 16-entry weight vector `1..8` is used by the 16x16 plane NEON kernel to generate row gradients in parallel.

## 8. Performance notes

Intra prediction is memory-light and compute-light; its cost is dominated by branch setup and small stores. ARMv6 integer instructions are sufficient for 4x4 modes because the block is only 16 bytes. NEON helps most for 16x16 and 8x8 blocks where a single vector store can fill a whole row. The 8x8 initializer deliberately overrides only a subset of modes because measurements showed the remaining NEON versions were not faster than C on Cortex A8/A9. A clean implementation should focus on correctness of neighbor replication and rounding first.

## 9. References

- ITU-T H.264, clause on intra prediction.
- `common/predict.c` for canonical filter formulas and edge layout.
- ARM NEON Programmer's Guide for byte shuffles and splat operations.

## 10. Verification checklist

- Each mode matches the generic C function byte for byte on random neighbor samples.
- Missing-neighbor cases (block at picture or slice boundary) use the same default substitution as the generic code.
- DC modes never overflow the 8-bit accumulator for maximum neighbor values.
- Plane output is clipped to 0..255.
- The 8x8 NEON routines consume the edge array without modifying it.
- Installed function pointers correspond only to modes the ARM initializer is documented to override.

## 11. Open questions

- `UNKNOWN`: exact edge-array index mapping for every 8x8 prediction mode; the layout is inherited from the generic filter producer and is not restated in this header.
- `UNKNOWN`: whether `x264_predict_8x8_init_arm` is ever passed a non-null `predict_filter` on ARM; the reference implementation ignores that parameter.
