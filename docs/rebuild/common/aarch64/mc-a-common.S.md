# `common/aarch64/mc-a-common.S` — shared MC weight macros and 8-bit averaging helper

> Dirty-side behavioural specification (reverse-engineered from the reference encoder; describes behaviour, not code).

## 1. Header

- Source path: `common/aarch64/mc-a-common.S`
- Role: common NEON weight-sign macros and one shared 4-wide averaging helper used by the NEON and SVE motion-compensation kernels.
- This is a **dirty-side behavioural specification** artifact for the clean-room reimplementation.

## 2. Purpose

Weighted prediction in H.264 can use negative weights, weights above the nominal unity value, or plain offsets. Rather than branch on the weight sign inside every hot loop, the reference defines three tiny macros that negate the appropriate operands at setup time, and shares a small 4-wide averaging loop between the NEON and SVE implementations. Keeping these definitions in one file guarantees the NEON and SVE paths compute the same arithmetic.

## 3. Public interface

Three weight-sign macros (8-bit build only):
- `load_weights_add_add`: identity; used when `0 < weight < 64`.
- `load_weights_add_sub`: negates the second weight; used when `weight > 64`.
- `load_weights_sub_add`: negates the first weight; used when `weight < 0`.

One exported/internal function:
- `pixel_avg_w4_neon`: averages two 4-pixel rows with rounding, two rows per iteration.

For 10-bit builds the file defines no macros (the 10-bit kernels handle signs differently in `mc-a.S`).

## 4. State and data structures

- Weight macros operate on general-purpose registers `w6`/`w7`: `w6` is the first weight (for `src1`), `w7` the second (`src2`).
- `pixel_avg_w4_neon` uses `x0` = destination, `x1` = destination stride, `x2`/`x3` = first source and stride, `x4`/`x5` = second source and stride, `w9` = height (decremented by two per iteration).

## 5. Algorithm / behaviour

**Weight macros.** The three cases implement the signed-weight decomposition used by x264's weighted prediction:
- Normal case: result = `w1*s1 + w2*s2`, both positive.
- Weight > 64: the second weight is effectively `64 − w2`, i.e. negative contribution; the macro negates `w7` so the multiply-accumulate becomes a multiply-subtract.
- Weight < 0: the first weight is negative; the macro negates `w6`.

The macros do no memory access; they just normalize signs so the hot loop can always use a multiply-add pattern.

**`pixel_avg_w4_neon`.** Loop while `height > 0`: load one 4-byte row from each source (`ld1 {v.s}[0]`), compute the unsigned rounding halving add (`urhadd`, i.e. `(a+b+1)>>1`), store one 4-byte row to the destination; repeat for a second row. Strides are applied automatically by the post-incrementing load/store forms. The loop counter decrements by two each iteration, so callers always pass an even height (4, 8, 16, …; the 4×2 case uses a different path in the SVE kernel).

Rounding: `urhadd` rounds half-up (adds 1 before shifting). No saturation issue because the average of two bytes is always a byte.

## 6. Dependencies

- Included by `mc-a.S` and `mc-a-sve.S`.
- `mc-a-sve.S` calls `pixel_avg_w4_neon` directly for the `weight == 32` (ordinary average) fast path.
- No table dependencies.

## 7. Constants and tables

- `64`: the nominal unity weight in x264's fixed-point weighted-prediction representation; weights are compared against this value.
- No tables.

## 8. Performance notes

Tiny macros; the shared helper is used only for 4-pixel-wide blocks. SVE tip: the SVE kernel replaces the 4-wide NEON loop with predicated halfword loads and `mul`/`mla`/`mls` chains for the weighted cases but falls back to this helper for the plain average — a reasonable choice because SVE's `urhadd` equivalent still operates at fixed width. On x86-64, `_mm_avg_epu8` is the direct analogue of `urhadd`. Public references: H.264 weighted prediction; ARM NEON `urhadd`/`neg` documentation.

## 9. References

- ITU-T H.264 / ISO/IEC 14496-10, Clause 8.4 (weighted sample prediction).
- ARM NEON instruction reference (`neg`, `urhadd`, `ld1`, `st1`).
- `docs/rebuild/common/aarch64/mc-a.S.md`, `mc-a-sve.S.md`.

## 10. Verification checklist

- For each of the three weight-sign macros, verify the resulting products equal the reference weighted-prediction formula for positive, zero, and negative weights.
- `pixel_avg_w4_neon` matches a scalar `(a+b+1)>>1` for random byte rows and all supported heights.
- Verify strides are respected (use distinct sentinel rows between active rows).

## 11. Open questions

- UNKNOWN: whether any caller uses `load_weights_*` macros outside the three `AVG_WEIGHT` variants in `mc-a.S`/`mc-a-sve.S`.
