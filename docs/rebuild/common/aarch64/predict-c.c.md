# `common/aarch64/predict-c.c` — AArch64 intra prediction dispatcher

> Dirty-side behavioural specification (reverse-engineered from the reference encoder; describes behaviour, not code).

## 1. Header

- Source path: `common/aarch64/predict-c.c`
- Role: installs AArch64/AArch64-NEON intra prediction kernels into the generic prediction function tables according to CPU features and bit depth.
- This is a **dirty-side behavioural specification** artifact for the clean-room reimplementation.

## 2. Purpose

Intra prediction has separate function tables per block shape (4×4, 8×8, 8×8c, 8×16c, 16×16). This file decides which entries are replaced by accelerated implementations. It distinguishes plain ARMv8 kernels (needing no NEON) from NEON kernels, and it entirely skips accelerated prediction in high-bit-depth builds.

## 3. Public interface

Five exported initializers:
- `void x264_predict_4x4_init_aarch64( uint32_t cpu, x264_predict_t pf[12] )`
- `void x264_predict_8x8c_init_aarch64( uint32_t cpu, x264_predict_t pf[7] )`
- `void x264_predict_8x16c_init_aarch64( uint32_t cpu, x264_predict_t pf[7] )`
- `void x264_predict_8x8_init_aarch64( uint32_t cpu, x264_predict8x8_t pf[12], x264_predict_8x8_filter_t *predict_filter )`
- `void x264_predict_16x16_init_aarch64( uint32_t cpu, x264_predict_t pf[7] )`

All are wrapped by `x264_template`. `pf` arrays are indexed by the `I_PRED_*` enumeration values.

## 4. State and data structures

- `x264_predict_t`: function pointer taking `uint8_t *src`.
- `x264_predict8x8_t`: function pointer taking `uint8_t *src, uint8_t edge[36]`.
- `x264_predict_8x8_filter_t`: the boundary-filter function pointer (unused by this initializer — no AArch64 override is installed).

## 5. Algorithm / behaviour

### `x264_predict_4x4_init_aarch64`

In 8-bit builds only:
- `X264_CPU_ARMV8`: install `predict_4x4_h_aarch64` at `I_PRED_4x4_H` and `predict_4x4_v_aarch64` at `I_PRED_4x4_V`. These use only scalar ARMv8 instructions (replicated 32-bit stores/loads), so they work even without NEON.
- `X264_CPU_NEON`: install `predict_4x4_dc_neon`, `_dc_top_neon`, `_ddl_neon`, `_ddr_neon` at their respective enum slots.

### `x264_predict_8x8c_init_aarch64`

- `X264_CPU_ARMV8`: install `predict_8x8c_v_aarch64`.
- If NEON is absent, return early.
- Otherwise install DC, DC_TOP, DC_LEFT, H, P NEON kernels.

### `x264_predict_8x16c_init_aarch64`

- Returns immediately unless NEON is present.
- Installs all six NEON variants (V, H, DC, P, DC_LEFT, DC_TOP). Unlike the 4×4 case, there is no ARMv8-only V kernel here (it would be too slow to matter or not available).

### `x264_predict_8x8_init_aarch64`

- Returns immediately unless NEON is present.
- Installs nine NEON kernels: DDL, DDR, VL, VR, DC, H, HD, HU, V. The `predict_filter` pointer is left untouched (generic C filter used).

### `x264_predict_16x16_init_aarch64`

- Returns immediately unless NEON is present.
- Installs six NEON kernels: DC, DC_TOP, DC_LEFT, H, V, P.

All assignments are unconditional overwrites within their feature tier; there is no SVE tier in prediction.

## 6. Dependencies

- Includes `common/common.h`, `predict.h`, `pixel.h`.
- Assigns functions implemented in `predict-a.S`.
- Called by the generic intra prediction initializer in `common/predict.c` (outside this directory).

## 7. Constants and tables

- `I_PRED_*` enumeration constants from common headers.
- `X264_CPU_ARMV8`, `X264_CPU_NEON` feature flags.
- `HIGH_BIT_DEPTH` build switch gates all accelerated assignments.

## 8. Performance notes

Initialization runs once. The key design choice is the ARMv8-only tier for 4×4 H/V and 8×8c V: those kernels use simple scalar operations and are available on CPUs without NEON. For a clean-room port, preserve the tiering so feature-mask behaviour is deterministic. SVE note: no SVE prediction kernels exist in the reference; adding them must preserve exact rounding and boundary handling. Public references: x264 prediction dispatcher; ARM CPU feature flags.

## 9. References

- x264 `common/predict.c` (generic prediction tables).
- ITU-T H.264 / ISO/IEC 14496-10, Clause 8.3 (intra prediction).
- `docs/rebuild/common/aarch64/predict-a.S.md`, `predict.h.md`.

## 10. Verification checklist

- For each feature mask (none, ARMV8, ARMV8+NEON) and each block-size table, verify the assigned function pointers match the documented tiers.
- With `HIGH_BIT_DEPTH` enabled, verify no AArch64 assignments occur.
- Verify `predict_filter` is left unchanged.
- All assigned functions match the generic C predictors bit-for-bit (test via the prediction kernels themselves).

## 11. Open questions

- UNKNOWN: whether any of the 4×4 `_neon` aliases in `predict.h` are used to unify suffix naming in a different dispatcher (the header defines aliases to the `_aarch64` names).
- UNKNOWN: exact numeric values of the `I_PRED_*` enum entries (defined in common headers outside this directory).
