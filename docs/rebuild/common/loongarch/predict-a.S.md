# `common/loongarch/predict-a.S` — LoongArch LSX/LASX intra prediction assembly

**Dirty-side behavioural specification.** Source path: `E:\_x264_dirty_stash\x264-source\common\loongarch\predict-a.S`. Role: assembly implementations of H.264 intra prediction for 4x4, 8x8 (luma and chroma) and 16x16 blocks, with LASX variants for selected directional modes.

## 1. Header

The title line above gives the source path and the file's role in one sentence; the adjacent bold marker identifies this document as a **dirty-side behavioural specification** written from the reference source and public literature. No reference source or near-verbatim pseudocode is reproduced.

## 2. Purpose

Intra prediction is evaluated for many candidate modes at slow presets. This file implements each mode with wide vector stores and shuffles; LSX handles 4/8/16-byte rows, LASX doubles throughput for the five 8x8 directional modes and 16x16 plane prediction. The C file `predict-c.c` installs the appropriate variants.

## 3. Public interface

Exports (prefixed by `ASM_REF`), declared in `loongarch/predict.h.md`:

- 4x4 (LSX): `predict_4x4_v`, `_h`, `_dc`, `_ddl`, `_dc_top`, `_dc_left`, `_dc_128`.
- 8x8c (LSX): `predict_8x8c_p`, `_v`, `_h`, `_dc`, `_dc_128`, `_dc_top`, `_dc_left`.
- 16x16: `predict_16x16_dc`, `_dc_left`, `_dc_top`, `_dc_128`, `_h`, `_v`, `_p` (LSX), plus `predict_16x16_p_lasx`.
- 8x8 (LSX): `predict_8x8_v`, `_dc`, `_dc_left`, `_dc_top`, `_dc_128`, `_h`, `_ddl`, `_ddr`, `_vr`, `_vl`.
- 8x8 (LASX): `predict_8x8_h`, `_ddl`, `_ddr`, `_vr`, `_vl`.

Signatures: `void (pixel *p_src)` for plane-neighbour modes, `void (pixel *src, pixel edge[36])` for 8x8 edge-based modes.

## 4. State and data structures

No globals. Working vectors hold replicated byte patterns (`vldrepl.b`/`xvldrepl.b`) for horizontal/DC fill, and edge bytes loaded into vector lanes for directional modes. Destination row pitch is `FDEC_STRIDE` (32 or 64 bytes depending on bit depth); the edge array has the standard 36-byte x264 layout, with this code reading the top row at offset 16, the left column around offset 14, and corner samples around offset 7.

## 5. Algorithm / behaviour

### Vertical / horizontal

Vertical loads the `W` bytes above the block and stores the same vector to all rows. Horizontal reads each left neighbour byte, broadcasts it across a vector, and stores one row. The 16x16 versions process 16 bytes per row (LSX) or 32 bytes per two columns (LASX where available).

### DC modes

Compute the average of the available neighbours with H.264 rounding:
- both sides: `(Σtop + Σleft + N/2) >> (log2 N + 1)` (4x4: `+4 >>3`, 16x16: `+16 >>5`);
- one side: `(Σ + N/2) >> log2 N`;
- neither: constant 128.

Sums are formed with horizontal byte→halfword→word add instructions; the result is broadcast and stored to all rows. 8x8c DC computes four separate DC values for the four 4x4 sub-blocks from the top row and left column (using offsets 7 and 14/15 as in the MIPS port) and stores four quadrants.

### Plane prediction

Implements H.264 §8.3.1.2.3 / §8.3.2.2:

- 16x16: load 16 top-row/corner bytes and left-column bytes, reorder with byte shuffles, compute the two slope sums with weights 1..8 for H and V, scale by **5** with `(x·5 + 32) >> 6`, form `A = 16·(corner samples + 1)` and `base = A − 7·(H'+V')`, then generate each output row from `base + k·V'` (k = 0..3 plus +4·V'), shift right by **5**, clip to `[0,255]`, pack and store.
- 8x8: same but weights 1..4, scale by **17** with `+16 >>5`, `base = A − 3·(H'+V')`, and emit two 4-byte groups per row.
- LASX 16x16 plane (`predict_16x16_p_lasx`) uses 256-bit registers to compute both halves of the top row at once.

The constants and lane order mirror the MSA implementation; the sign of the corner-based `A` term and the `+1` inside it are exactly as in H.264.

### Directional 8x8 modes (edge array)

- **DDL (diagonal down-left):** output byte = `(2·edge[i] + edge[i+1] + edge[i+2] + 2) >> 2`, with boundary replication by reusing the last available edge byte; implemented with byte shifts and pairwise adds.
- **DDR (diagonal down-right):** the mirrored access pattern over the top/right edge bytes.
- **VR (vertical-right) and VL (vertical-left):** interpolate between vertical and diagonal references, again with `(a + b + 1) >> 1` or four-tap `(2a + b + c + 2) >> 2` rounding as specified by H.264.

LASX variants of H/DDL/DDR/VR/VL process 8 output bytes per vector (two 4-byte groups) versus LSX's single group, halving instruction count.

### 128-fill

Store the byte 128 to every row of the block (used when neither neighbour is available).

## 6. Dependencies

- **Calls:** `loongson_asm.S` shuffle/clip macros, `loongson_util.S` function macros and strides; raw LSX/LASX instructions (`vshuf.b`, `xvshuf.b`, `vhaddw`, `xvhaddw`, `vsrai.h`, `xvsrai.h`, `vldrepl.b`, `xvldrepl.b`, `vpackev`, `xvpackev`).
- **Called by:** `predict-c.c` init functions, `pixel-c.c` intra metric helpers.
- **Shared config:** `HIGH_BIT_DEPTH == 0`, `BIT_DEPTH`, `FDEC_STRIDE`.

## 7. Constants and tables

- DC rounding: `+4>>3`, `+2>>2`, `+16>>5`, `+8>>4`.
- Plane: weights 1..4 (8x8) / 1..8 (16x16), scale 17 / 5, rounding `+16>>5` / `+32>>6`, `A = 16·(corners+1)`, slope multipliers 3 / 7, final `>>5`.
- Directional: `+2>>2` or `+1>>1` rounding; byte shift amounts 1/2/3/4.
- Fill value 128.

## 8. Performance notes

- **Hotness:** moderate/high; more modes evaluated at slow presets.
- **SIMD status:** fully LSX/LASX; LASX used only where it halves loop counts.
- **Optimization tips:** (a) keep replicated fill vectors in registers across mode evaluations; (b) the plane slope sums are the most arithmetic-intensive part — use the pairwise `hsub`/`hadd` pattern; (c) ensure destination is 32-byte aligned for LASX stores; (d) reuse LSX predictions even in LASX-based metrics (as `pixel-c.c` already does) to avoid duplicate implementations.
## 9. References

ITU-T H.264 §8.3.1/§8.3.2; x264 `common/predict.c`; LoongArch ISA manuals.

## 10. Verification checklist

1. Compare every predictor output with the scalar reference for deterministic neighbour patterns.
2. DC availability matrix: all four above/left combinations for 4x4 and 16x16; check half-way rounding.
3. Plane 8x8/16x16: test with ramps and extreme values to verify the `>>5` clip.
4. Directional 8x8 modes: test all five LASX variants against their LSX counterparts and the scalar reference.
5. Verify edge-array reads stay within `edge[0..35]` (ASan guard).
6. Confirm no writes cross into the next macroblock row (`FDEC_STRIDE` pitch).
7. Verify the init functions install LSX first, LASX overrides second.

## 11. Open questions

- UNKNOWN: exact edge-array index mapping for DDR/VR/VL — validate against the generic `x264_predict_8x8_filter` output.
- UNKNOWN: whether the 8x8c `P` (plane) predictor's rounding matches the 8x8 luma plane predictor exactly (it should, per H.264 chroma rules, but verify).
- UNKNOWN: high-bit-depth behavior of this file — it is largely gated behind `HIGH_BIT_DEPTH == 0`, so `FDEC_STRIDE=32` may not describe wide-pixel rows.
