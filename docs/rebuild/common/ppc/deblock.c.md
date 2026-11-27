# `common/ppc/deblock.c` — PowerPC luma deblocking implementation

**Dirty-side behavioural specification.** Source path: `common/ppc/deblock.c`. Role: AltiVec implementations of vertical and horizontal luma deblocking, including 16x6 transposition and 16x4 write-back helpers.

## 1. Purpose

This file accelerates the two luma inter-frame edge filters on PowerPC. It uses vector masks to replace the scalar filter's data-dependent branches and includes explicit transpose and store helpers for the horizontal case.

## 2. Public interface

- `void x264_deblock_v_luma_altivec( uint8_t *pix, intptr_t stride, int alpha, int beta, int8_t *tc0 )`
- `void x264_deblock_h_luma_altivec( uint8_t *pix, intptr_t stride, int alpha, int beta, int8_t *tc0 )`

Declared in `ppc/deblock.h`; installed by `x264_deblock_init` when AltiVec is available.

## 3. State and data structures

No persistent state. Vertical filtering loads six contiguous rows around the edge into six 16-byte vectors. Horizontal filtering loads a 16x6 tile, transposes it into six 16-byte vectors with a custom `read_and_transpose16x6` macro, and writes four modified columns back with `write16x4`, which stages results in an aligned 64-byte temporary buffer before strided stores.

## 4. Algorithm / behaviour

**Mask construction.** `h264_deblock_mask` forms three conditions: `abs(p0-q0) < alpha`, `abs(p1-p0) < beta`, and `abs(q1-q0) < beta`. Absolute differences are computed with `vec_absd` (or the fallback `max-min`) and compared with splat vectors of the thresholds. The `tc0` array is loaded as four signed bytes, duplicated across the vector, and AND-ed with the mask so lanes with negative `tc0` are skipped.

**Outer samples.** Two further masks test `abs(p2-p0) < beta` and `abs(q2-q0) < beta`. For lanes where the test passes, `p1` is replaced by `(p2 + ((p0+q0+1)>>1)) >> 1` limited to `p1 +/- tc0`, and the effective clipping limit is increased by one. `h264_deblock_q1` implements this with `vec_avg` and a parity-correction bit derived from the XOR of the two averaged values, giving the exact rounding of the H.264 formula. The symmetric computation updates `q1`.

**Boundary samples.** `h264_deblock_p0_q0` computes the delta using vector averages of `p1` and the complement of `q1`, and of `q0` and the complement of `p0`. A constant vector representing 160 folds the fixed offsets into the result. The delta is clipped to `+/-tc`, then added to `p0` and subtracted from `q0` with saturating byte operations.

**Vertical path.** The routine loads rows at offsets `-3*stride` through `+2*stride`, runs the filter macro, and stores the four modified rows at `-2*stride`, `-1*stride`, `0`, and `+stride`.

**Horizontal path.** The routine loads sixteen bytes from each of six rows starting at `pix-4`, transposes them into six row vectors representing columns, filters, transposes the four modified vectors back into sixteen-column form, and writes four bytes per row at `pix-2`. The early test `(tc0[0]&tc0[1]&tc0[2]&tc0[3]) >= 0` skips the whole operation when all four blocks are disabled.

## 5. Dependencies

- Includes `common/common.h`, `ppccommon.h`, and local `deblock.h`.
- Uses `LOAD_ZERO`, `vec_absd`, and byte helpers from `ppccommon.h`.
- The threshold values come from the generic deblocking caller.

## 6. Constants and tables

- Delta rounding constant 4 and shift 3, folded into the vector formulation via the constant 160.
- Threshold comparisons are strict less-than.
- Clipping limit `tc0`, incremented for passing outer-sample tests.

## 7. Performance notes

The mask-based formulation processes sixteen edge positions in parallel. The horizontal variant pays a transpose cost but avoids sixteen scalar filter calls. The write-back helper uses 32-bit strided stores from an aligned buffer because AltiVec stores are naturally vector-sized; this is the main inefficiency noted by the reference source. A clean implementation could use VSX unaligned stores where available.

## 8. References

- ITU-T H.264, adaptive deblocking filter.
- `common/deblock.c` for canonical formulas.
- AltiVec Programming Interface Manual.

## 9. Verification checklist

- Output matches the generic luma filter on randomized pixels and parameters.
- Negative `tc0` lanes are skipped.
- Saturating arithmetic prevents wraparound.
- Vertical and horizontal variants agree on transposed input.
- All sixteen byte lanes are independent.

## 10. Open questions

- `UNKNOWN`: whether the `vec_ld` of `tc0` through a temporary byte buffer has defined behaviour for all alignments on all compilers.
- `UNKNOWN`: exact overflow behaviour of the constant-160 delta formulation for extreme values.
