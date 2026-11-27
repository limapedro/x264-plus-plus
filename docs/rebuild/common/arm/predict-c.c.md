# `common/arm/predict-c.c` — ARM intra-prediction dispatch installation

## 1. Header

- **Source path:** `common/arm/predict-c.c`.
- **Role in one sentence:** installs ARM-specific intra-prediction function pointers into the generic prediction tables based on detected CPU features.
- **Marker:** Dirty-side behavioural specification; all descriptions are behavioural and written independently of the reference source.


## 2. Purpose

This small C file is the only non-assembly part of the ARM intra-prediction back end. It translates capability bits into a set of function-pointer assignments, allowing the generic encoder to call optimized kernels without any architecture checks in the inner loops.

## 3. Public interface

Five exported initializers, all matching the declarations in `arm/predict.h`:

- `void x264_predict_4x4_init_arm( uint32_t cpu, x264_predict_t pf[12] )`
- `void x264_predict_8x8c_init_arm( uint32_t cpu, x264_predict_t pf[7] )`
- `void x264_predict_8x16c_init_arm( uint32_t cpu, x264_predict_t pf[7] )`
- `void x264_predict_8x8_init_arm( uint32_t cpu, x264_predict8x8_t pf[12], x264_predict_8x8_filter_t *predict_filter )`
- `void x264_predict_16x16_init_arm( uint32_t cpu, x264_predict_t pf[7] )`

Each takes the capability mask and the table to modify. The 8x8 initializer also accepts the edge-filter function pointer but does not use it. None of the functions returns a value.

## 4. State and data structures

`x264_predict_t` is `void (*)(uint8_t *src)`. `x264_predict8x8_t` is `void (*)(uint8_t *src, uint8_t edge[36])`. `x264_predict_8x8_filter_t` is the producer of the filtered edge array. The `pf` arrays are indexed by the prediction-mode enumerations declared in `common/predict.h`.

## 5. Algorithm / behaviour

Dispatch is strictly monotone: each initializer either returns immediately or replaces a subset of table entries, and later feature checks never undo earlier assignments.

- **4x4**: requires `X264_CPU_ARMV6`; installs horizontal, vertical, DC and diagonal-down-right ARMv6 routines. If `X264_CPU_NEON` is also present, additionally installs DC-top and diagonal-down-left NEON routines. All assignments are compiled only for 8-bit builds.
- **8x8 chroma**: requires `X264_CPU_NEON`; installs DC, DC-top, DC-left, horizontal, vertical and plane NEON routines.
- **8x16 chroma**: requires `X264_CPU_NEON`; installs only DC-top, horizontal and plane NEON routines. The reference source notes that the other functions were not faster than C on the contemporary Cortex cores, so they are deliberately left generic.
- **8x8 luma**: requires `X264_CPU_NEON`; installs diagonal-down-left, diagonal-down-right, vertical-left, vertical-right, DC, horizontal, horizontal-down, horizontal-up and vertical NEON routines. The `predict_filter` output pointer is not modified.
- **16x16**: requires `X264_CPU_NEON`; installs DC, DC-top, DC-left, horizontal, vertical and plane NEON routines.

The assignments do not include the "DC-128" variants or the 4x4 plane and 8x8 luma edge filter, which remain generic.

## 6. Dependencies

- Called from the corresponding `x264_predict_*_init` functions in `common/predict.c`.
- Includes `common/common.h`, local `predict.h`, and local `pixel.h`.
- References assembly symbols defined in `common/arm/predict-a.S`.

## 7. Constants and tables

Prediction-mode enum values used as array indices: `I_PRED_4x4_H/V/DC/DDR/DC_TOP/DDL`, `I_PRED_CHROMA_DC/DC_TOP/DC_LEFT/H/V/P`, `I_PRED_8x8_DDL/DDR/VL/VR/DC/H/HD/HU/V`, and `I_PRED_16x16_DC/DC_TOP/DC_LEFT/H/V/P`. Feature flags used are `X264_CPU_ARMV6` and `X264_CPU_NEON`.

## 8. Performance notes

Dispatch runs once per encoder instance and has negligible cost. The interesting performance decision is the deliberate omission of certain 8x16 routines: the reference authors measured them as no faster than C on Cortex A8/A9 and chose to keep the scalar versions. A rebuild should not assume "more SIMD is always better"; validate each kernel against the generic version on the target core before installing it.

## 9. References

- `common/predict.h` for the prediction-mode numbering and function-pointer typedefs.
- `common/predict.c` for the dispatch call sites and generic implementations.
- `common/arm/predict-a.S` for the actual kernels.

## 10. Verification checklist

- With no capability flags, no table entry changes.
- With `X264_CPU_ARMV6` only, exactly the four documented 4x4 entries change.
- With `X264_CPU_NEON`, all documented NEON entries change and no others.
- The `predict_filter` pointer is untouched.
- High-bit-depth builds leave all tables unchanged because assignments are compile-time guarded.
- Repeated calls with identical flags produce identical tables.

## 11. Open questions

- `UNKNOWN`: whether aarch64 builds use these initializers or a separate aarch64 dispatch path; this file gates on 32-bit ARM feature flags.
- `UNKNOWN`: whether the 8x16 chroma NEON kernels become faster on newer cores, in which case the reference's omission decision may not apply.
