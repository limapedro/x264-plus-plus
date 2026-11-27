# `common/aarch64/deblock-a-common.S` — shared deblocking filter-start macro

> Dirty-side behavioural specification (reverse-engineered from the reference encoder; describes behaviour, not code).

## 1. Header

- Source path: `common/aarch64/deblock-a-common.S`
- Role: holds the NEON macro that decides whether a deblocking filter invocation has any work, shared by pure-NEON and SVE filter kernels.
- This is a **dirty-side behavioural specification** artifact for the clean-room reimplementation.

## 2. Purpose

Every deblocking call passes an `alpha`, `beta`, and a `tc0` array. If `alpha` and `beta` are zero, or all four `tc0` entries are zero/non-negative, the filter is a no-op. This early-out avoids loading and transposing pixels for the many block edges that need no filtering. Centralizing the test guarantees NEON and SVE paths agree on the decision.

## 3. Public interface

One assembler macro, conventionally named `h264_loop_filter_start`. Contract:
- Inputs: `w2` = alpha, `w3` = beta, `x4` = pointer to the four `int8_t` `tc0` values (a 32-bit word).
- Output: either returns immediately from the enclosing function (no filtering needed), or falls through with `w6` = the loaded 32-bit `tc0` word and `v24.s[0]` = the same word, ready for broadcast/predicate construction by the caller.
- Additionally sets condition flags so the caller can distinguish "all tc0 zero" from "some tc0 nonzero" if desired.

## 4. State and data structures

No persistent state. The macro touches `w2`, `w3`, `w4`, `w6`, `w8`, and vector register `v24`. It assumes `tc0` is addressable as a single aligned 32-bit load at `x4`.

## 5. Algorithm / behaviour

The macro performs a three-part test:

1. Compare `alpha` (`w2`) against zero and `beta` (`w3`) against zero using a conditional compare chain: if alpha is nonzero the beta comparison is made under the "not equal" condition; the combined result decides whether the whole filter is skipped. In effect: skip when `alpha == 0 && beta == 0`.
2. Load the four `tc0` bytes as one 32-bit word into `w6` and copy it into lane 0 of `v24`.
3. Compute `w8 = w6 & (w6 << 16)` and then test `w8 & (w8 << 8)`. If that value is non-negative (sign bit clear), the macro returns; otherwise it falls through.

The bit trick detects "any `tc0` byte is negative" (negative values are the x264 convention for "filter this edge"): ANDing a word with itself shifted by 16 puts the sign bit of each byte into overlapping positions, and the second AND/shift collapses the four sign bits so that a `b.ge` (non-negative result) means no byte has its sign bit set. Combined with the alpha/beta test, the macro returns for all no-op cases.

Edge cases: if `alpha == 0` but `beta != 0`, filtering may still be required by the caller's semantics; the conditional compare handles that by only requiring one of the two to be nonzero. If `tc0` contains a mix of zero and negative entries, the macro falls through and the per-lane predicates in the filter macro decide which lanes are modified.

## 6. Dependencies

- Included by `deblock-a.S` and `deblock-a-sve.S`.
- No external function calls.
- Follows the AArch64 calling convention of the filter functions (`x0` pixels, `x1` stride, `w2` alpha, `w3` beta, `x4` tc0).

## 7. Constants and tables

- `16` and `8`: shift amounts in the tc0 sign-detection trick.
- `0`: comparison threshold for alpha/beta.
- No tables.

## 8. Performance notes

Executed once per deblocking edge (up to thousands per frame), so the early-out is valuable: the common case for low-QP or smooth areas is no filtering at all. The bit trick replaces four byte comparisons and a reduction with three scalar instructions. SVE/SVE2 tip: the same check can be expressed with SVE `cmplt` predicates and `any`, but the scalar trick is cheaper for a single 32-bit word; keep the scalar form even in SVE kernels (the reference does). Public references: H.264 deblocking filter decision rules; ARM conditional-compare (`ccmp`) documentation.

## 9. References

- ITU-T H.264 / ISO/IEC 14496-10, Clause 8.7 (deblocking filter), boundary strength and alpha/beta thresholds.
- ARM AArch64 `cmp`, `ccmp`, `ands`, `bic` instruction documentation.
- `docs/rebuild/common/aarch64/deblock-a.S.md`, `deblock-a-sve.S.md`.

## 10. Verification checklist

- For all combinations of alpha/beta zero vs nonzero and all 16 sign patterns of four tc0 bytes, the macro returns exactly when a scalar reference decides "no filtering needed".
- Verify that `v24.s[0]` contains the unmodified tc0 word on fall-through.
- Verify the enclosing filter produces identical results whether or not the early-out is taken (edge case: alpha/beta nonzero but every tc0 byte zero).

## 11. Open questions

- UNKNOWN: exact treatment of `alpha != 0, beta == 0` versus `alpha == 0, beta != 0` in the caller; the macro's conditional compare may make one case fall through even though the filter does nothing, but that is harmless.
- UNKNOWN: whether any caller passes a tc0 word with positive nonzero values (x264 uses −1..−3 or 0).
