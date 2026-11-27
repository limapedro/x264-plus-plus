# `common/loongarch/predict-c.c` — LoongArch intra-prediction initialization

**Dirty-side behavioural specification.** Source path: `E:\_x264_dirty_stash\x264-source\common\loongarch\predict-c.c`. Role: installs LSX/LASX intra prediction function pointers into the four generic prediction tables based on CPU capabilities.

## 1. Purpose

All actual prediction kernels are implemented in assembly (`predict-a.S`). This short C file contains only the architecture-dispatch logic: it decides, per CPU feature, which function pointers to write into the caller's tables. It is the LoongArch analogue of the `x264_predict_*_init_mips` functions but includes explicit LASX overrides.

## 2. Public interface

Four functions declared in `loongarch/predict.h`:

- `void x264_predict_16x16_init_loongarch(int cpu, x264_predict_t pf[7])`
- `void x264_predict_8x8c_init_loongarch(int cpu, x264_predict_t pf[7])`
- `void x264_predict_8x8_init_loongarch(int cpu, x264_predict8x8_t pf[12], x264_pred8x8_filter_t *predict_filter)`
- `void x264_predict_4x4_init_loongarch(int cpu, x264_predict_t pf[12])`

Each is name-mangled by `x264_template`. Contracts: `pf` is a caller-owned table indexed by `I_PRED_*` enum values; the functions overwrite only the slots for which an LSX/LASX implementation exists; `predict_filter` is unused here (the generic filter function is left in place).

## 3. State and data structures

No state. `cpu` is a bitmask of `X264_CPU_LSX` and `X264_CPU_LASX`. Table sizes are fixed by the H.264 intra mode counts: 7 for 16x16 luma (V, H, DC, DC_LEFT, DC_TOP, DC_128, P), 7 for 8x8 chroma (the same modes with chroma naming), 12 for 8x8 luma (V, DC, DC_LEFT, DC_TOP, DC_128, H, DDL, DDR, VR, VL, plus two reserved), 12 for 4x4 luma.

## 4. Algorithm / behaviour

Conditional installation in source order:

- **16x16:** when LSX, all seven slots are set to `x264_predict_16x16_{v,h,dc,dc_left,dc_top,dc_128,p}_lsx`. When LASX, slot `P` is overwritten with `x264_predict_16x16_p_lasx`.
- **8x8 chroma:** when LSX, all seven slots are set to `x264_predict_8x8c_{p,v,h,dc,dc_128,dc_top,dc_left}_lsx`. No LASX overrides.
- **8x8 luma:** when LSX, ten slots are set — V, DC, DC_LEFT, DC_TOP, DC_128, H, DDL, DDR, VR, VL — all `_lsx`. When LASX, five are overwritten: H, DDL, DDR, VR, VL with their `_lasx` variants.
- **4x4:** when LSX, seven slots are set — V, H, DC, DDL, DC_LEFT, DC_TOP, DC_128. No LASX overrides.

All bodies are guarded by `#if !HIGH_BIT_DEPTH`; in high-bit-depth builds the functions are no-ops (the CPU flags are ignored). The order matters: LSX is installed first so that LASX can selectively override.

## 5. Dependencies

- **Calls:** none at runtime beyond writing pointers; references all the `x264_predict_*_lsx/_lasx` symbols declared in `loongarch/predict.h`.
- **Called by:** `x264_predict_init()` in `common/predict.c` during encoder setup.
- **Requires:** `x264_predict_t`, `x264_predict8x8_t`, `x264_predict_8x8_filter_t`, `I_PRED_*` enums, `x264_template`, `X264_CPU_*` flags from generic headers.

## 6. Constants and tables

No data constants. The mode-to-slot mapping is the H.264 intra mode enumeration (`I_PRED_16x16_V` etc.); the exact numeric enum values are defined in `common/macroblock.h` and must be respected by a rebuild.

## 7. Performance notes

No steady-state cost. For rebuild, the LASX override pattern is the only performance-relevant detail: the five 8x8 directional modes and 16x16 plane are the ones with wider implementations. Preserve the LSX→LASX ordering so that a CPU with both flags gets the wider kernels.

## 8. Verification checklist

1. Call each init function with `cpu=0`, `cpu=X264_CPU_LSX`, `cpu=X264_CPU_LASX`, and both; verify the expected slots are non-null and others are unchanged.
2. Verify that LASX overrides exactly the five 8x8 modes plus 16x16 P, and nothing else.
3. Confirm high-bit-depth builds compile and leave the tables untouched.
4. Compare installed function names against the symbols actually emitted by `predict-a.S`.

## 9. Open questions

- UNKNOWN: whether the `predict_filter` parameter is ever used by other architecture init functions and whether leaving it untouched here is correct — verify against x86 init code.
- UNKNOWN: exact numeric values of `I_PRED_*` enums in the target version (they affect which table index is written); obtain from `common/macroblock.h` rather than assuming standard ordering.
