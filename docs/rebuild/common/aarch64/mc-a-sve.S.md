# `common/aarch64/mc-a-sve.S` — SVE weighted 4-wide averaging kernels

> Dirty-side behavioural specification (reverse-engineered from the reference encoder; describes behaviour, not code).

## 1. Header

- Source path: `common/aarch64/mc-a-sve.S`
- Role: SVE implementations of the 4-pixel-wide weighted/ordinary average kernels used by motion compensation.
- This is a **dirty-side behavioural specification** artifact for the clean-room reimplementation.

## 2. Purpose

Weighted prediction for 4×2, 4×4, 4×8, and 4×16 blocks is handled by a dispatch wrapper that selects one of three arithmetic forms depending on the weight's sign and magnitude. The SVE version implements the weighted forms with SVE predicated loads and `mul`/`mla`/`mls`, while delegating the plain 50/50 average to the NEON helper for best performance.

## 3. Public interface

Four exported functions (declared in `mc-c.c`):

`void x264_pixel_avg_4x{2,4,8,16}_sve( pixel *dst, intptr_t dst_stride, pixel *src1, intptr_t src1_stride, pixel *src2, intptr_t src2_stride, int weight );`

- `x0` = destination, `x1` = destination stride
- `x2`/`x3` = first source and its stride
- `x4`/`x5` = second source and its stride
- `w6` = fixed-point weight
- Return: nothing; destination rows overwritten with the weighted average.

## 4. State and data structures

- Predicate `p0.b` with `vl8` (8 byte lanes, interpreted as 8 halfword lanes for widening byte loads).
- Vector registers `v30`/`v31` hold the two weights broadcast as 8 halfwords; `z0`–`z3`/`v4`/`v5` hold loaded rows and accumulators.
- The weight-sign macros from `mc-a-common.S` normalize `w6`/`w7`.

## 5. Algorithm / behaviour

### Dispatch wrapper (`AVGH_SVE` macro)

1. Set the nominal weight constant to 64 and the row counter to the block height.
2. If `weight == 32`, branch to the NEON plain-average helper (`pixel_avg_w4_neon`) — this is the common 50/50 case.
3. Compute `64 − weight` into `w7`.
4. If that value is negative (`weight > 64`), branch to the `add_sub` weighted kernel.
5. Otherwise if `weight >= 0`, branch to the `add_add` kernel.
6. Otherwise (`weight < 0`), branch to the `sub_add` kernel.

### Weighted kernels (`AVG_WEIGHT_SVE` macro, instantiated three times)

1. Apply the corresponding weight-sign macro to normalize operands.
2. Set `p0.b` to `vl8` and broadcast `w6` into `v30.8h`, `w7` into `v31.8h`.
3. For each pair of source rows:
   - Predicate-load four bytes from each source row into halfword lanes (`ld1b {z.h}, p0/z`), advancing both source pointers by their strides.
   - Compute the 16-bit weighted sum with `mul` + `mla` (normal), `mul` + `mls` (weight > 64), or `mls` from the other operand (weight < 0).
   - Load the next pair of rows similarly.
   - Narrow the 16-bit result back to bytes with `sqrshrun #6` (signed saturating rounding shift right by 6), which implements the fixed-point divide by 64 with rounding and clamping to [0,255].
   - Store four bytes to the destination row (`st1 {v.s}[0]`), advancing by the destination stride. Repeat for the second row.

Rounding: `sqrshrun #6` rounds half-up (adds 32 before shifting) and saturates to unsigned bytes. This is the standard H.264 weighted-prediction rounding. No extra offset is applied here — the caller's weight already includes any offset handling (offset-only cases are handled by the NEON `mc_weight` offsetadd/offsetsub kernels).

Ordering: rows processed in pairs; source pointers advance even for the last pair. Heights 2, 4, 8, 16 are all even, so the pair loop terminates exactly.

## 6. Dependencies

- Includes `asm.S` and `mc-a-common.S`.
- Requires `.arch armv8-a+sve`.
- Calls `pixel_avg_w4_neon` from `mc-a-common.S`.
- Dispatched from `x264_mc_init_aarch64` in `mc-c.c` when `HAVE_SVE` and `X264_CPU_SVE` are set in 8-bit builds (the four 4-wide `avg` entries are overridden).

## 7. Constants and tables

- `64`: unity weight / fixed-point scale.
- `32`: plain-average weight, delegated to NEON.
- `6`: narrowing shift after weighted accumulation.
- No tables.

## 8. Performance notes

These kernels are hot for B-frames and weighted-P frames but only for 4-wide blocks (chroma DC / 4×4 luma). SVE predicated loads reduce the number of instructions versus NEON single-lane loads, but the effective width is only four bytes per row. Optimization tip: if the encoder processes multiple 4-wide rows from different blocks, SVE can batch them under a wider predicate; otherwise the NEON fallback for weight=32 is appropriate. On x86-64, use `_mm_madd_epi16`/`_mm_mulhi_epu16` patterns and `_mm_packus_epi16` for the narrowing. Public references: H.264 weighted sample prediction; ARM SVE `mul`/`mla`/`mls`/`sqrshrun` documentation.

## 9. References

- ITU-T H.264 / ISO/IEC 14496-10, Clause 8.4 (weighted prediction).
- ARM SVE Architecture Reference Manual (`ptrue`, `ld1b`, `mul`, `mla`, `mls`, `sqrshrun`).
- `docs/rebuild/common/aarch64/mc-a-common.S.md`, `mc-c.c.md`, `mc.h.md`.

## 10. Verification checklist

- For each block size and each weight category (negative, 0, 1..63, 32, 64, >64), compare output against a scalar weighted-prediction reference.
- Verify the `weight == 32` path is exactly equivalent to the plain `(a+b+1)>>1` average.
- Verify saturation: large weights driving results above 255 clamp, not wrap.
- Verify predicate width: only four bytes per row are modified (guard-page test).
- Verify row stride handling with sentinel rows between active rows.

## 11. Open questions

- UNKNOWN: exact behaviour for `weight == 0` or `weight == 64` (the dispatch treats 64 as `add_sub` with `w7 = 0`, which should reduce to `src1`; verify).
- UNKNOWN: whether negative weights actually occur in x264 streams (typically encoded via the offset path instead).
