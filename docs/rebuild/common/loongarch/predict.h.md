# `common/loongarch/predict.h` — LoongArch intra-prediction declarations

**Dirty-side behavioural specification.** Source path: `E:\_x264_dirty_stash\x264-source\common\loongarch\predict.h`. Role: declares all LSX/LASX intra prediction functions and the four architecture init routines implemented in `predict-a.S` and `predict-c.c`.

## 1. Header

The title line above gives the source path and the file's role in one sentence; the adjacent bold marker identifies this document as a **dirty-side behavioural specification** written from the reference source and public literature. No reference source or near-verbatim pseudocode is reproduced.

## 2. Purpose

This header is the LoongArch counterpart of `mips/predict.h`, but it also declares the init functions that populate the generic prediction function-pointer tables — a design difference from MIPS. It provides both 128-bit and 256-bit variants for several modes (notably 8x8 H/DDL/DDR/VR/VL and 16x16 plane), so the dispatcher can choose the wider implementation when LASX is available.

## 3. Public interface

All names are mangled through `x264_template(...)`.

**4x4 (LSX only)**
- `x264_predict_4x4_v_lsx`, `_h_lsx`, `_dc_lsx`, `_ddl_lsx`, `_dc_top_lsx`, `_dc_left_lsx`, `_dc_128_lsx` — each `void (pixel *p_src)`.
- Init: `void x264_predict_4x4_init_loongarch(int cpu, x264_predict_t pf[12])`.

**8x8 chroma (LSX only)**
- `x264_predict_8x8c_p_lsx`, `_v_lsx`, `_h_lsx`, `_dc_lsx`, `_dc_128_lsx`, `_dc_top_lsx`, `_dc_left_lsx` — `void (uint8_t*)` / `void (pixel*)`.
- Init: `void x264_predict_8x8c_init_loongarch(int cpu, x264_predict_t pf[7])`.

**16x16 luma**
- LSX: `x264_predict_16x16_dc_lsx`, `_dc_left_lsx`, `_dc_top_lsx`, `_dc_128_lsx`, `_h_lsx`, `_v_lsx`, `_p_lsx`.
- LASX: `x264_predict_16x16_p_lasx` (plane mode only).
- Init: `void x264_predict_16x16_init_loongarch(int cpu, x264_predict_t pf[7])`.

**8x8 luma (edge-array variants)**
- LSX: `x264_predict_8x8_v_lsx`, `_dc_lsx`, `_dc_left_lsx`, `_dc_top_lsx`, `_dc_128_lsx`, `_h_lsx`, `_ddl_lsx`, `_ddr_lsx`, `_vr_lsx`, `_vl_lsx`.
- LASX: `_h_lasx`, `_ddl_lasx`, `_ddr_lasx`, `_vr_lasx`, `_vl_lasx`.
- All take `void (pixel *src, pixel edge[36])`.
- Init: `void x264_predict_8x8_init_loongarch(int cpu, x264_predict8x8_t pf[12], x264_predict_8x8_filter_t *filter)`.

Contracts: neighbours are read from the reconstruction plane at `src - FDEC_STRIDE` (above) and `src - 1` (left) for the non-edge variants; edge-array variants read only from `edge[36]`. Output uses `FDEC_STRIDE` row pitch. Nothing returns a value.

## 4. State and data structures

No state. Include guard `X264_LOONGARCH_PREDICT_H`. The `x264_predict_t`, `x264_predict8x8_t`, `x264_predict_8x8_filter_t` types come from `common/predict.h`. The `pf` arrays passed to the init functions are caller-owned tables of size 7 (16x16/chroma) or 12 (8x8), indexed by the `I_PRED_*` enum values.

## 5. Algorithm / behaviour

Declarations only. Semantics follow H.264 §8.3.1/§8.3.2 identically to the MIPS implementations (see `mips/predict-c.c.md`): vertical replication, horizontal replication, DC averaging with available-side rounding, constant-128 fill, plane prediction with the ±1 corner extrapolation and `>>5` output shift, and the diagonal-down-left/right plus vertical-right/left 8x8 directional modes computed from the 36-byte edge array with byte shifts and pairwise averages.

The init functions in `predict-c.c` install:
- all seven 16x16 LSX predictors plus `p_lsx`, then override `p` with `p_lasx` when LASX is set;
- all seven 8x8c LSX predictors when LSX is set (no LASX overrides);
- ten 8x8 LSX predictors when LSX is set, then override H/DDL/DDR/VR/VL with their LASX versions when LASX is set;
- seven 4x4 LSX predictors when LSX is set.

## 6. Dependencies

- **Included by:** `common/loongarch/predict-a.S`, `predict-c.c`, `pixel-c.c` (intra metric helpers), and `common/predict.c`.
- **Requires:** `pixel`, `uint8_t`, `x264_predict_t` et al., `x264_template`, `FDEC_STRIDE`.
- The assembly implementations include `loongson_asm.S`/`loongson_util.S`.

## 7. Constants and tables

None in the header. Plane prediction constants (17/5 scaling, `+16>>5`/`+32>>6`, `A = 16·(corners+1)`, slope multipliers) and DDL rounding constants are in `predict-a.S.md`.

## 8. Performance notes

No runtime cost. The init functions are called once; the LASX override pattern is the performance-relevant detail — a rebuild should keep the wider variants installed for the five 8x8 directional modes and 16x16 plane, since those are the most frequently evaluated at slow presets.

## 9. References

ITU-T H.264 standard; x264 project documentation; architecture SIMD programmer's guides.

## 10. Verification checklist

1. Include header alone under both bit-depth configurations.
2. Verify each declared symbol exists in `predict-a.S` or `predict-c.c`.
3. Call each init function with each CPU flag combination and verify the expected table entries are installed.
4. Compare all predictor outputs against scalar references for deterministic neighbour patterns.
5. Verify no writes outside the destination block (stride `FDEC_STRIDE`).

## 11. Open questions

- UNKNOWN: whether any 8x8 mode other than H/DDL/DDR/VR/VL benefits from LASX but was omitted intentionally; confirm from the init function.
- UNKNOWN: exact `edge[36]` layout relative to the generic predictor filter output (same uncertainty as the MIPS port).
