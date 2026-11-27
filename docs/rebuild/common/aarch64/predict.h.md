# `common/aarch64/predict.h` — AArch64 intra prediction declarations

> Dirty-side behavioural specification (reverse-engineered from the reference encoder; describes behaviour, not code).

## 1. Header

- Source path: `common/aarch64/predict.h`
- Role: declares all AArch64/AArch64-NEON intra prediction kernels and the five feature-tiered initializer functions; also defines aliases that unify naming for merged SAD/SATD prediction paths.
- This is a **dirty-side behavioural specification** artifact for the clean-room reimplementation.

## 2. Purpose

The generic intra prediction dispatcher needs a stable set of prototypes and initializer symbols. This header provides them, plus three aliases that let code expecting `_neon` names use the plain-ARMv8 kernels for 4×4 H/V and 8×8c V.

## 3. Public interface

Functions (all take `uint8_t *src` pointing to the block origin in the reconstruction plane with stride 32):
- 4×4: `x264_predict_4x4_{h,v}_aarch64`, `x264_predict_4x4_{dc,dc_top,ddl,ddr}_neon`
- 8×8 luma (take an extra `uint8_t edge[36]`): `x264_predict_8x8_{v,h,dc,ddl,ddr,vl,vr,hd,hu}_neon`
- 8×8 chroma: `x264_predict_8x8c_{v}_aarch64`, `x264_predict_8x8c_{dc,dc_top,dc_left,h,p}_neon`
- 8×16 chroma: `x264_predict_8x16c_{v,h,dc,dc_top,dc_left,p}_neon`
- 16×16 luma: `x264_predict_16x16_{v,h,dc,dc_top,dc_left,p}_neon`

Aliases:
- `x264_predict_4x4_h_neon` = `x264_predict_4x4_h_aarch64`
- `x264_predict_4x4_v_neon` = `x264_predict_4x4_v_aarch64`
- `x264_predict_8x8c_v_neon` = `x264_predict_8x8c_v_aarch64`

Initializers:
- `x264_predict_{4x4,8x8,8x8c,8x16c,16x16}_init_aarch64` with the signatures documented in `predict-c.c.md`.

All names are wrapped with `x264_template`.

## 4. State and data structures

- `x264_predict_t`: pointer to `void (*)(uint8_t *src)`.
- `x264_predict8x8_t`: pointer to `void (*)(uint8_t *src, uint8_t edge[36])`.
- `x264_predict_8x8_filter_t`: boundary filter function pointer (declared for the initializer signature but not overridden).

## 5. Algorithm / behaviour

Declaration only. The aliases exist because some common code merges 4×4 intra SAD/SATD paths and expects a uniform `_neon` suffix; mapping them to the plain ARMv8 functions avoids duplicating those tiny kernels.

## 6. Dependencies

- Included by `common/pixel.c` (which also includes `pixel.h`) and `predict-c.c`.
- Uses common typedefs and `x264_template`.

## 7. Constants and tables

None. Block dimensions and strides are implicit in the implementations.

## 8. Performance notes

Declaration-only. SVE/SVE2 tip: no SVE prediction kernels are declared or dispatched in the reference; a clean-room port adding them must keep the exact rounding/saturation semantics and update the dispatcher tiers. Public references: x264 prediction architecture.

## 9. References

- ITU-T H.264 / ISO/IEC 14496-10, Clause 8.3.
- `docs/rebuild/common/aarch64/predict-a.S.md`, `predict-c.c.md`.

## 10. Verification checklist

- Every declared kernel symbol exists in `predict-a.S`.
- The three aliases resolve to the same addresses as their `_aarch64` targets.
- Initializer prototypes match their definitions in `predict-c.c`.

## 11. Open questions

- UNKNOWN: which common code path uses the three aliases (likely the merged 4×4 intra SAD/SATD implementation in `common/pixel.c`).
