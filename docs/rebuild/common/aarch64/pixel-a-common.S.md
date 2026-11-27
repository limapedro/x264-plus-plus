# `common/aarch64/pixel-a-common.S` — shared pixel-metric macros and AC mask

> Dirty-side behavioural specification (reverse-engineered from the reference encoder; describes behaviour, not code).

## 1. Header

- Source path: `common/aarch64/pixel-a-common.S`
- Role: shared NEON macro/constant definitions for the pixel-metric kernels, including the Hadamard vertical butterfly and the AC coefficient masks used by hadamard-ac, shared between NEON and SVE implementations.
- This is a **dirty-side behavioural specification** artifact for the clean-room reimplementation.

## 2. Purpose

SATD and Hadamard-AC metrics need the same 4-point sum/difference butterfly and the same masks that select which transformed coefficients contribute to the "AC" energy estimate. Centralizing them guarantees NEON and SVE paths produce identical metric values, which is essential because rate-distortion decisions depend on them.

## 3. Public interface

- Constant `mask_ac_4_8`: 16 halfwords forming two 8-lane masks. The first mask selects halfword lanes 1..3 of each 4-halfword group (zero at lane 0); the second selects lanes 1..7 of each 8-halfword group (zero at lane 0). Used to exclude the DC coefficient from AC energy sums.
- Macro `SUMSUB_ABCD s1, d1, s2, d2, a, b, c, d`: applies `SUMSUB_AB` to the pairs `(a,b)` and `(c,d)`, producing sums in `s1/s2` and differences in `d1/d2`.
- Macro `HADAMARD4_V r1, r2, r3, r4, t1, t2, t3, t4`: performs a two-stage 4-point Hadamard transform on four input vectors, using four temporaries, leaving results in `r1..r4`.

## 4. State and data structures

No runtime state. Macros operate on 8-lane or 4-lane halfword vectors; the masks are read-only 32-byte constants.

## 5. Algorithm / behaviour

`SUMSUB_ABCD` is just two independent sum/difference pairs, allowing eight registers to be transformed in one macro invocation.

`HADAMARD4_V` implements the 4×4 Hadamard butterfly:
1. First stage: form `(a+b, a−b)` and `(c+d, c−d)` in the temporary registers.
2. Second stage: form `(t1+t3, t1−t3)` and `(t2+t4, t2−t4)` in the result registers, i.e. it applies the butterfly along the transposed pairing so that a subsequent transpose completes a 2-D Hadamard transform.

`mask_ac_4_8` values: the first 8 halfwords are `0, −1, −1, −1, 0, −1, −1, −1` (all-ones in halfword lanes where the mask is active), the second 8 are `0, −1, −1, −1, −1, −1, −1, −1`. When ANDed with a vector, masked lanes pass through and the DC lane becomes zero. The 4-halfword variant is used for 4×4-block AC sums, the 8-halfword variant for 8×8 blocks.

## 6. Dependencies

- Included by `pixel-a.S` and `pixel-a-sve.S` (and indirectly relies on `SUMSUB_AB` from `asm.S`).
- No external calls.

## 7. Constants and tables

- `mask_ac_4_8` (32 bytes) described above.
- Implicit constants: 0 and all-ones (−1 as a halfword).
- No other tables.

## 8. Performance notes

Tiny macros on the SATD/hadamard-ac hot path. SVE/SVE2 tip: the masks work unchanged with SVE's 128-bit NEON-compatible views; with wider SVE vectors the masks can be broadcast with `ld1` under predicate instead of loaded as a fixed table. On x86-64, the equivalent is a static `__m128i` mask with `_mm_and_si128`. Public references: H.264 SATD/Hadamard metric definitions; ARM NEON `add`/`sub` documentation.

## 9. References

- ITU-T H.264 / ISO/IEC 14496-10 (transform basis used for SATD metrics).
- ARM NEON reference (`add`, `sub`, `and`).
- `docs/rebuild/common/aarch64/pixel-a.S.md`, `pixel-a-sve.S.md`.

## 10. Verification checklist

- `HADAMARD4_V` on known inputs matches a scalar 4-point Hadamard transform.
- The AC masks zero exactly the intended lanes (compare against a reference that sums only AC coefficients).
- NEON and SVE hadamard-ac produce identical results on the same input.

## 11. Open questions

- UNKNOWN: whether `mask_ac_4_8` is also used by any non-pixel kernel (no other consumer is visible in this directory).
