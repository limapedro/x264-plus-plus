# `common/aarch64/dct-a-common.S` — shared DCT butterfly macro

> Dirty-side behavioural specification (reverse-engineered from the reference encoder; describes behaviour, not code).

## 1. Header

- Source path: `common/aarch64/dct-a-common.S`
- Role: holds the NEON 1-D transform butterfly macro shared by the pure-NEON, SVE, and SVE2 transform kernels.
- This is a **dirty-side behavioural specification** artifact for the clean-room reimplementation.

## 2. Purpose

The 4×4 integer DCT and IDCT used by H.264 share a small butterfly structure. Duplicating it in each assembly file risks divergence between the NEON and SVE implementations (which would produce different bitstreams). This file defines it once so that all variants compute identical coefficients.

## 3. Public interface

One assembler macro, conventionally named `DCT_1D`, with eight vector-register arguments (`v0` through `v7` in the reference's naming). Contract: on entry, `v16..v19` (the last four arguments) hold four rows (or columns) of 16-bit differences; on exit, `v0..v3` (the first four arguments) hold the four transformed outputs. The transform is the separable 4-point integer DCT used by H.264, and the same macro (with a different register assignment) also implements its inverse up to scaling.

## 4. State and data structures

No memory state. All operands are 16-bit lane vectors held in NEON registers. The macro works in place on its four outputs and requires four additional source registers. Its behaviour is defined over signed 16-bit lanes; the caller guarantees the inputs are small enough that intermediate sums fit without overflow (x264's 8-bit DCT of byte differences satisfies this).

## 5. Algorithm / behaviour

Let the four input vectors be named `a, b, c, d` (the macro's 5th–8th arguments) and the outputs `o0..o3`.

The macro performs the standard 4-point fast Hadamard-like butterfly used by the H.264 4×4 transform:

1. Compute two intermediate sum/difference pairs: `s1 = a + d`, `d1 = a - d`; and `s2 = b + c`, `d2 = b - c`.
2. The even outputs are `o0 = s1 + s2` and `o1 = s1 - s2` (after the reference's register renaming, the even pair is produced first).
3. The odd outputs use the "1.5×" trick: double `d2` and `d1` (`2*d`), then form `o2 = (2*d) + d1 - s2` and `o3 = d2 - (2*d1)` — i.e. `3*d - s`, implemented with adds and shifts only.

More concretely in terms of the reference's register naming: the sum/difference pairs are `(v1,v6)` and `(v3,v7)`; `v0 = v3+v1`; `v4 = 2*v7`; `v5 = 2*v6`; `v2 = v3-v1`; `v1 = v4+v6`; `v3 = v7-v5`. The outputs are taken from `v0, v1, v2, v3` (or the corresponding renamed registers when the macro is called with a different layout).

Ordering: all operations are lane-wise; no reductions or cross-lane moves occur. Because the macro is used both for rows and columns, callers transpose between the two invocations.

Rounding: none — this is an exact integer transform with no shifts. Scaling and rounding happen in the inverse transform kernels (`srshr #6`, etc.).

## 6. Dependencies

- Included by `dct-a.S`, `dct-a-sve.S`, and `dct-a-sve2.S`.
- Relies on `SUMSUB_AB` from `asm.S`.
- All three includers must use the same register-width suffix (`.4h` for 4×4, `.8h` for the 8-wide helper).

## 7. Constants and tables

No constants or tables. The implicit coefficients are those of the H.264 4×4 integer transform (basis vectors with entries 1, 2, 1/−1 patterns); the 1.5 multiplier is realized by `x + (x >> 1)` style operations in other macros, while here it arises from the doubling/add structure.

## 8. Performance notes

Tiny macro, ~8 SIMD instructions, executed once per row and once per column of each 4×4 block. On x86-64 this maps directly to `paddw`/`psubw` pairs. SVE/SVE2 tip: on wide vectors the same macro can process multiple blocks at once if the SVE vector length is larger than 8 halfwords, but the reference uses fixed-width NEON views even inside SVE functions to keep block semantics simple; a clean-room port should preserve the fixed-width results first and consider multi-block SVE only after correctness is established. Public references: H.264 transform specification; ARM NEON `add`/`sub` documentation.

## 9. References

- ITU-T H.264 / ISO/IEC 14496-10, Clause 8.5 (forward and inverse 4×4 integer transform).
- ARM NEON instruction reference (`add`, `sub`, `sshr`).
- `docs/rebuild/common/aarch64/dct-a.S.md`, `dct-a-sve.S.md`, `dct-a-sve2.S.md`.

## 10. Verification checklist

- Feed the macro known integer vectors and compare outputs with a scalar H.264 4×4 transform implementation.
- Verify it is its own inverse up to the expected scale factor when applied twice with the inverse register ordering.
- Verify no lane-to-lane contamination by using distinct values in every lane.
- Ensure all includers produce identical results for the same input (NEON vs SVE vs SVE2 paths).

## 11. Open questions

- UNKNOWN: whether any caller uses the macro at 8-wide (`.8h`) for two 4×4 blocks simultaneously in the 8x4 helper; the shared file itself imposes no width restriction.
