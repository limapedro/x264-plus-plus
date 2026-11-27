# `common/aarch64/deblock-a-sve.S` — SVE chroma deblocking filter

> Dirty-side behavioural specification (reverse-engineered from the reference encoder; describes behaviour, not code).

## 1. Header

- Source path: `common/aarch64/deblock-a-sve.S`
- Role: SVE implementation of the vertical chroma deblocking filter, selected when SVE is available.
- This is a **dirty-side behavioural specification** artifact for the clean-room reimplementation.

## 2. Purpose

Chroma deblocking is a simple two-tap/one-tap filter applied across a block boundary. The SVE version replaces NEON's compare-and-mask sequence with per-byte SVE predicates and writes only the filtered lanes with predicated stores, avoiding blind writes of unchanged pixels.

## 3. Public interface

One exported function (declared in `deblock.h`):

`void x264_deblock_v_chroma_sve( uint8_t *pix, intptr_t stride, int alpha, int beta, int8_t *tc0 );`

- `x0` = pixel pointer positioned at the first row of the boundary; `x1` = row stride.
- `w2` = alpha threshold, `w3` = beta threshold, `x4` = four `tc0` values (negative means filter).
- Return: nothing; the two rows adjacent to the boundary are modified in place.

An internal macro `h264_loop_filter_chroma_sve` implements the filter body; it is also usable by other SVE kernels in the same file in principle (only this function is present).

## 4. State and data structures

- Predicates: `p0.b` with `vl16` (all 16 byte lanes), `p1.b`, `p2.b`, `p3.b` for the three filter conditions.
- Vector registers: `v22` holds replicated alpha then beta; `v24` holds the tc0 word widened from bytes to halfwords and shifted to align each 8-bit tc0 value with its lane; `v25` is its negation; `v16`/`v0`/`v18`/`v2` hold `p1`, `p0`, `q0`, `q1` rows; `v4`/`v5` hold the computed delta.
- Predicated byte loads/stores (`ld1`/`st1b`) under the 16-lane predicate.

## 5. Algorithm / behaviour

1. Invoke the shared filter-start macro: return immediately if alpha and beta are both zero or no tc0 entry requests filtering.
2. Position `pix` four rows before the boundary (`pix − 2*stride`) and load four rows with ordinary NEON 16-byte loads: `p1 = pix`, `p0 = pix+stride`, `q0 = pix+2*stride`, `q1 = pix+3*stride`. (The reference comment notes SVE loads did not improve performance here, so NEON loads are retained.)
3. Form the three filter-condition predicates under `p0.b`:
   - `abs(p0 − q0) < alpha`
   - `abs(p1 − p0) < beta`
   - `abs(q1 − q0) < beta`
   Combined into `p1.b` = AND of the three.
4. Compute the filter delta exactly as the H.264 chroma filter specifies: `delta = ((q0 − p0) << 2 + (p1 − q1)) >> 3`, using unsigned widen/accumulate instructions, then rounded right shift by 3 (`rshrn`).
5. Clamp `delta` to the range `[−tc0, +tc0]` using `smin`/`smax` against the widened tc0 lane values and their negations.
6. Apply `p0 += delta`, `q0 −= delta` with widening add/subtract and unsigned saturating narrow (`sqxtun`), so results stay in [0,255].
7. Rewind `pix` to `p0`'s row and write back only lanes selected by `p1.b` using SVE predicated byte stores (`st1b`): first the `p0` row, then the `q0` row at `pix+stride`.

Rounding: the delta uses a rounding right shift by 3 (half-up), followed by symmetric clipping to ±tc0. Saturation: byte results are clamped, not wrapped. Predication: lanes whose tc0 is zero or whose gradients exceed thresholds remain untouched in memory (their registers may be modified but are not stored).

### Exact filter equation

For each eligible byte lane:

```
d0 = p0 − q0 ;  d1 = p1 − p0 ;  d2 = q1 − q0
eligible = (|d0| < alpha) && (|d1| < beta) && (|d2| < beta)
delta_raw = (4*(q0 − p0) + (p1 − q1) + 4) >> 3   // rounded
delta = clamp( delta_raw, −tc, +tc )
p0' = Clip1( p0 + delta )
q0' = Clip1( q0 − delta )
```

The assembly forms `4*(q0−p0) + (p1−q1)` with unsigned widening (`uxtl`, `usubw`, `uaddw`, `shl #2`) into 16-bit lanes, applies `rshrn #3` (rounding right shift by 3 while narrowing to bytes), and clips with `smin`/`smax` against the replicated tc0 word and its negation. The widening add/subtract before `sqxtun` implements `Clip1`.

### Predicate construction and lane accounting

- `p0.b, vl16`: all 16 byte lanes active.
- `p1.b`: lanes where `alpha > |p0−q0|`, formed with `cmhi` (strict greater-than, so equality does not filter).
- `p2.b`, `p3.b`: lanes where `beta > |p1−p0|` and `beta > |q1−q0|`.
- `p1.b = p1 & p2 & p3`: the final write mask; lanes failing any condition are not stored.
- `v24` is the tc0 word widened from bytes to halfwords and then shifted left by 8 and 16 bits (`sli`) so each 8-bit tc0 value lands in the correct 32-bit lane for the 16-lane byte comparison.

### Threshold and tc0 semantics

- `alpha` and `beta` are runtime QP-derived thresholds; larger values filter more edges.
- `tc0` is four signed bytes, one per 4×4 edge segment; only negative entries request filtering, and the magnitude is the clipping limit. The shared start macro detects "all non-negative" and returns before any loads.
- `p1` and `q1` are read but not written — chroma filtering touches only the two rows adjacent to the boundary.

### Dispatch conditions

Installed only when `HAVE_SVE` and `X264_CPU_SVE` are set, overriding the `deblock_chroma[1]` (vertical chroma) entry. All other deblock functions remain NEON. The SVE kernel must be bit-identical to `deblock_v_chroma_neon`; it is a pure optimization, not a semantic change.

## 6. Dependencies

- Includes `asm.S` and `deblock-a-common.S`.
- Requires `.arch armv8-a+sve`.
- Dispatched from `common/deblock.c` when `HAVE_SVE` and `X264_CPU_SVE` are set, overriding the chroma-V function pointer (index 1).
- Must be bit-identical to the NEON `deblock_v_chroma_neon` for all inputs.

## 7. Constants and tables

- `2`, `3`, `8`: shifts in delta computation and tc0 alignment.
- `16`: predicate width in bytes.
- Thresholds `alpha`, `beta` are runtime parameters; `tc0` is a runtime four-byte array.
- No tables.

## 8. Performance notes

Moderately hot: chroma edges are filtered for every macroblock, but there are fewer chroma than luma edges. The SVE predicates avoid unconditional writes of two rows; on real SVE hardware with 512-bit vectors the `vl16` predicate still restricts work to 128-bit worth of lanes, so the main benefit is the predicated store. Optimization tip: for a clean-room port with wider vectors, filter two chroma blocks at once using separate predicates per block; ensure the tc0 values are broadcast to the corresponding lanes. On x86-64 AVX2, use `_mm256_cmpgt_epi8` predicates, `_mm256_blendv_epi8` for conditional writes, and `_mm256_packus_epi16` for saturation. Public references: H.264 deblocking filter spec; ARM SVE predicate and `sqxtun` documentation.

## 9. References

- ITU-T H.264 / ISO/IEC 14496-10, Clause 8.7.2 (chroma deblocking filter).
- ARM SVE Architecture Reference Manual (`ptrue`, `cmhi`, `st1b`, `smin`, `smax`).
- `docs/rebuild/common/aarch64/deblock-a-common.S.md`, `deblock-a.S.md`, `deblock.h.md`.

## 10. Verification checklist

- Compare output against the NEON chroma-V filter on randomized pixels, alpha, beta, and tc0 values; byte-identical required.
- Verify no-op cases (alpha=beta=0, all tc0 zero) leave the buffer untouched.
- Verify threshold boundaries: gradients exactly equal to alpha/beta must not filter (strict less-than).
- Verify clipping: large positive/negative delta clamped to ±tc0; result saturates to [0,255].
- Verify predicated stores touch exactly the two boundary rows (guard-page test).

## 11. Open questions

- UNKNOWN: whether SVE variants of other deblock functions were intended but not implemented in this snapshot (only chroma-V is present).
- UNKNOWN: exact behaviour for tc0 values that are positive (non-standard); the sign-detection early-out may skip them.
