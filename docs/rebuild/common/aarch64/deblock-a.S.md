# `common/aarch64/deblock-a.S` — NEON deblocking filters and boundary strength

> Dirty-side behavioural specification (reverse-engineered from the reference encoder; describes behaviour, not code).

## 1. Header

- Source path: `common/aarch64/deblock-a.S`
- Role: complete NEON implementation of the H.264 deblocking filter variants (luma/chroma, inter/intra, vertical/horizontal, MBAFF and 4:2:2 special cases) and of boundary-strength computation.
- This is a **dirty-side behavioural specification** artifact for the clean-room reimplementation.

## 2. Purpose

Deblocking is applied to every reconstructed macroblock edge and is one of the most visible quality/performance trade-offs in H.264. This file implements the filter in SIMD: vertical filters operate on 16-pixel rows directly; horizontal filters transpose columns into rows, filter, transpose back, and scatter. A separate kernel computes boundary strengths from non-zero flags, reference indices, and motion vectors.

## 3. Public interface

All functions are declared in `deblock.h`. Exported entries:
- `deblock_v_luma_neon`, `deblock_h_luma_neon`
- `deblock_v_luma_intra_neon`, `deblock_h_luma_intra_neon`
- `deblock_v_chroma_neon`, `deblock_h_chroma_neon`, `deblock_h_chroma_422_neon`, `deblock_h_chroma_mbaff_neon`
- `deblock_v_chroma_intra_neon`, `deblock_h_chroma_intra_neon`, `deblock_h_chroma_intra_mbaff_neon`, `deblock_h_chroma_422_intra_neon`
- `deblock_strength_neon`

Internal macros: `h264_loop_filter_luma`, `h264_loop_filter_luma_intra`, `h264_loop_filter_chroma`, `h264_loop_filter_chroma8`, `h264_loop_filter_chroma_intra`, plus the shared start macro from `deblock-a-common.S`. Internal label `deblock_h_chroma` is reused by the 4:2:2 variant.

## 4. State and data structures

- Pixel rows are held in 16-byte NEON registers; horizontal filters use `transpose_8x16.b` / `transpose_4x16.b` / `transpose4x8.h` / `transpose4x4.h` from `asm.S`.
- `v22`/`v30`/`v31` hold replicated alpha/beta thresholds; `v24` holds the tc0 word widened and lane-shifted; `v23` holds its negation.
- For boundary strength, `v4`/`v5` accumulate reference-index differences and motion-vector differences; `v6` holds the threshold `mvy_limit` folded into a saturating-subtract constant; `v7` is the `transpose_table` permutation.

## 5. Algorithm / behaviour

### Common structure

Every inter filter begins with the shared start macro (early-out on alpha=beta=0 or no negative tc0). Every intra filter begins with its own start macro that early-outs only when alpha and beta are both zero, then broadcasts alpha and beta into vector registers.

### `h264_loop_filter_luma` (normal / strong luma filtering)

Inputs: `p2,p1,p0,q0,q1,q2` rows in `v20,v18,v16,v0,v2,v4`; alpha/beta/tc0 in `v22/v24`.

1. Form three masks: `abs(p0−q0) < alpha`, `abs(p1−p0) < beta`, `abs(q1−q0) < beta`. Combine into a master mask.
2. Form two additional masks: `abs(p2−p0) < beta` and `abs(q2−q0) < beta`. AND each with the master mask, giving the conditions for strong filtering on the p and q sides.
3. Compute the clipping bound `tc` from tc0, replicated to each lane and sign-extended; form `−tc`.
4. Compute the normal (weak) filter delta: `delta = ((q0−p0) << 2 + (p1−q1)) >> 3` with rounding, using unsigned widen/accumulate and `rshrn #3`. Clamp to `[−tc, +tc]`.
5. Compute the strong-filter outputs:
   - `p0' = (2*p1 + p0 + q1 + 2*q0 + (q0−p0)??)` — more precisely the standard H.264 strong filter averages: `p0' = (p2 + 2*p1 + p0 + q0 + q1 + 2*q2...)`; the implementation builds these with `uaddl`/`uaddw`/`uqrshrn`-style chains and shifts by 2 or 3.
   - `p1'`, `p2'`, `q0'`, `q1'`, `q2'` are similarly computed as combinations of up to five samples with rounding shifts of 2 or 3.
6. Blend weak and strong results using the masks from step 2 (`bsl`), then apply the master mask so only eligible lanes are changed.
7. Widen, add/subtract delta, saturate with `sqxtun`, producing filtered `p1,p0,q0,q1`.

Ordering: the weak delta is applied to `p0`/`q0`; strong filtering replaces up to three samples on each side. The output registers are `v17` (p1'), `v16` (p0'), `v0` (q0'), `v19` (q1').

### `deblock_v_luma_neon` / `deblock_h_luma_neon`

- Vertical: loads six rows (three on each side of the boundary) using the boundary pointer and stride, calls `h264_loop_filter_luma`, then stores four rows (p1, p0, q0, q1) back.
- Horizontal: subtracts 4 from `pix`, loads eight 8-byte segments per row for two halves of the 16-pixel span, transposes 8×16 bytes to get columns, calls the luma filter, transposes back with `transpose_4x16.b`, and writes four columns as 16 separate 4-byte stores (four lanes × four rows).

### `h264_loop_filter_luma_intra`

Implements the H.264 intra deblocking rules:
- Conditions: `abs(p0−q0) < alpha`, `abs(p1−p0) < beta`, `abs(q1−q0) < beta`, and a stricter `abs(p0−q0) < (alpha>>2)+2`.
- If the stricter condition fails, only p0/q0 are modified using `(2*p1 + p0 + q1 + 2) >> 2` and `(2*q1 + q0 + p1 + 2) >> 2` (rounded shifts by 2).
- If it passes, also compute p0'/p1'/p2' and q0'/q1'/q2' from wider neighborhoods with shifts 3/2/3, selected by `abs(p2−p0) < beta` and `abs(q2−q0) < beta`.
- The implementation uses early `cbz` on a narrowed mask word to skip the expensive second branch entirely when no lane qualifies.

### `deblock_v_luma_intra_neon` / `deblock_h_luma_intra_neon`

Vertical loads seven rows (p3..q3) and writes six (p2,p1,p0,q0,q1,q2). Horizontal loads 16 columns via two 8×16 byte transposes, filters, transposes back, and stores 16 columns.

### `h264_loop_filter_chroma` and chroma functions

Chroma filtering is the simple one-tap variant:
- Conditions `abs(p0−q0) < alpha`, `abs(p1−p0) < beta`, `abs(q1−q0) < beta` combined into a mask.
- `delta = ((q0−p0) << 2 + (p1−q1)) >> 3`, clamped to `[−tc0, tc0]`.
- `p0 += delta`, `q0 −= delta`, saturated to bytes.
- `deblock_v_chroma_neon`: loads four rows, filters, stores p0/q0 rows.
- `deblock_h_chroma_neon`: transposes four 8-byte rows via `transpose4x8.h`, filters, transposes back, and scatters with `st2`-style interleaved stores (two bytes at a time) so U/V planes are separated correctly.
- `deblock_h_chroma_422_neon`: doubles the stride and processes two chroma rows (U and V at 4:2:2 height) by calling the shared horizontal label twice with the tc0 word reloaded from `w6`.
- `deblock_h_chroma_mbaff_neon`: uses a 4×4 transpose and 8-byte delta macro (`h264_loop_filter_chroma8`), writing interleaved U/V pairs with `st2`.

### `h264_loop_filter_chroma_intra` and intra chroma functions

- Conditions: `abs(p0−q0) < alpha`, `abs(p1−p0) < beta`, `abs(q1−q0) < beta` combined.
- Outputs `p0' = (p1 + 2*p0 + q0 + 2) >> 2` and `q0' = (q1 + 2*q0 + p0 + 2) >> 2` (rounded shifts by 2), selected by the mask.
- Width parameter (8 or 16) controls whether the second half of a 16-byte vector is processed.
- `deblock_v_chroma_intra_neon` loads four rows and writes two; horizontal/mbaff/422 variants transpose 4×8 or 4×4, filter, and scatter with `st2`.

### `deblock_strength_neon`

Computes `bs[2][8][4]` for one macroblock from `nnz`, `ref`, and `mv`:

1. Prepare constants: `mvy_limit` is shifted left by 8 and combined with `(1<<8)−3` so a saturating subtract yields a per-byte threshold; `v6.8h` holds it.
2. For each of eight 4×4 positions, load reference indices from both lists, interleave/de-interleave to align current and neighbour values, and OR their differences into `v4` (vertical edges) and `v5` (horizontal edges).
3. Load motion vectors from both lists, compute `sabd` (signed absolute difference) between neighbours, narrow to bytes, and saturating-subtract the threshold; OR into the accumulators. This yields 1 where a reference/MV difference requires filtering.
4. Load `nnz`, compute per-block "non-zero" flags by ORing adjacent entries and clamping to 1, double them to weight 2, and OR with the MV/ref flags: boundary strength is max(ref/mv flag, 2×nnz flag), giving values 0, 1, or 2 (intra would be 3 but is handled elsewhere).
5. Apply a `tbl` permutation (`transpose_table` with byte order 0,4,8,12 / 1,5,9,13 / ...) to transpose the 4×4 edge-strength matrix into the `bs` layout and store `bs[0]` with a −32 stride and `bs[1]` contiguous.

Rounding/saturation: only in the filter deltas (`rshrn`/`uqrshrn`) and byte clamping (`sqxtun`). Boundary strength uses saturating arithmetic (`uqsub`) and max (`umax`) to avoid wraparound.

### Boundary-strength and threshold equations

Boundary strength `BS` for an edge between two 4×4 blocks is derived before this file runs:
- intra block or non-reference edge → 3,
- differing reference indices or motion-vector difference ≥ `mvy_limit` → 2,
- non-zero residual coefficients on either side → 1,
- otherwise → 0.

`deblock_strength_neon` computes the 0/1/2 part by combining per-byte flags: `bs = max( ref_or_mv_flag, 2 × nnz_flag )`, with `ref_or_mv_flag` produced by ORing reference-index differences and saturating-subtracted MV differences, and `nnz_flag` by ORing adjacent block non-zero flags and clamping to 1.

Alpha and beta are runtime QP-derived thresholds. For each sample pair the filter decision is a conjunction of strict comparisons:

| Condition | Meaning |
| --- | --- |
| `abs(p0 − q0) < alpha` | edge is strong enough to filter |
| `abs(p1 − p0) < beta` | p-side gradient is small |
| `abs(q1 − q0) < beta` | q-side gradient is small |
| `abs(p2 − p0) < beta` | enables strong filtering on p side |
| `abs(q2 − q0) < beta` | enables strong filtering on q side |

Eligible lanes are selected by ANDing the corresponding predicates; lanes outside the mask are not written back.

### Weak (normal) filter equation

For inter luma/chroma edges with `BS < 4`:

```
delta0 = (q0 − p0) << 2
delta1 = p1 − q1
delta  = (delta0 + delta1 + 4) >> 3
delta  = clip( delta, −tc, +tc )
p0' = Clip1( p0 + delta )
q0' = Clip1( q0 − delta )
```

The assembly computes `delta0 + delta1` in widened 16-bit lanes and applies a rounding right shift by 3 (`rshrn #3`), then clips with `smin`/`smax` against the broadcast tc0 value and its negation. `Clip1` is implemented by unsigned saturating narrow (`sqxtun`) after the widening add/subtract.

### Strong luma filter equations

When the p-side (q-side) strong condition holds, up to three samples on that side are replaced by weighted averages of neighbouring samples, e.g.:

```
p0' = (p2 + 2*p1 + 2*p0 + q0 + q1 + 4) >> 3
p1' = (p2 + p1 + p0 + q0 + 2) >> 2
p2' = (2*p2 + p1 + p0 + q0 + q1 + 4) >> 3
```

with symmetric formulas on the q side. The implementation builds these with wide unsigned adds and `rshrn`/`uqrshrn` shifts by 2 or 3, then blends with the weak result using the strong-condition masks.

### Intra filter equations

For intra edges with fixed strength 3, the primary rule is:

```
p0' = (2*p1 + p0 + q1 + 2) >> 2
q0' = (2*q1 + q0 + p1 + 2) >> 2
```

If `abs(p0 − q0) < (alpha >> 2) + 2` also holds (the stricter intra condition), the wider strong formulas for p0/p1/p2 and q0/q1/q2 apply, selected by `abs(p2 − p0) < beta` and `abs(q2 − q0) < beta`. The reference shortcuts the entire strong branch with a single `cbz` test on a narrowed mask word.

For intra chroma there is no strong branch; only the two-tap formulas above are applied with a mask combining the three gradient conditions.

### Transpose and lane selection

Horizontal filters transpose 8×16 or 4×8 byte matrices into columns, run the same per-lane filter, transpose back, and store with `st2` (de-interleaving U/V) or with 4-byte scatter stores. The masks lane-align with the transposed layout, so a single predicate/byte-mask simultaneously controls all columns of an edge.

## 6. Dependencies

- Includes `asm.S` and `deblock-a-common.S`; uses all transpose macros.
- Dispatched from `common/deblock.c`; the SVE chroma-V variant overrides one entry.
- `deblock_strength_neon` consumes arrays produced by the common macroblock analysis code.

## 7. Constants and tables

- `alpha`, `beta`, `tc0`: runtime thresholds/strengths.
- `3` (intra fixed strength), `2`, `3`, `7`, `8`: shift amounts in filter arithmetic and tc0 packing.
- `mvy_limit`: runtime MV-difference threshold.
- `transpose_table`: 16-byte permutation (0,4,8,12 / 1,5,9,13 / 2,6,10,14 / 3,7,11,15) used for bs transposition.
- No coefficient tables.

## 8. Performance notes

Very hot: up to eight luma and four chroma edge calls per macroblock, each touching up to 16 pixels per side. The reference processes two 8-pixel halves in parallel for horizontal luma filters and uses unrolled `.rept` blocks. Conditional branches are limited to the early-out and the intra "strong" branch.

Optimization tips for a clean-room AVX2 port: implement the weak filter with `_mm_avg_epu8`-style arithmetic and `_mm_maddubs_epi16` for the weighted sum; use `_mm_min_epi8`/`_mm_max_epi8` for clipping; transpose with `_mm_unpacklo/hi_epi8` networks. SVE/SVE2 tip: predicated stores avoid writing unchanged rows; wider predicates can batch two block edges, but tc0 and thresholds must be broadcast per edge. Public references: H.264 deblocking filter specification; ARM NEON transpose and saturating-arithmetic docs.

## 9. References

- ITU-T H.264 / ISO/IEC 14496-10, Clause 8.7 (deblocking filter), including strong/weak luma and chroma formulas and intra variants.
- ARM NEON instruction reference (`uabd`, `urhadd`, `sqxtun`, `uqadd`, `bsl`, `st2`).
- `docs/rebuild/common/aarch64/deblock-a-common.S.md`, `deblock-a-sve.S.md`, `deblock.h.md`.

## 10. Verification checklist

- Compare every filter variant against the C reference filter on randomized pixels, alpha/beta values, and tc0 arrays; byte-identical output required.
- Verify no-op early-outs leave the buffer unchanged.
- Verify strict thresholds: gradients equal to alpha/beta do not filter.
- Verify clipping to ±tc0 and byte saturation at 0/255.
- Verify horizontal variants produce the same result as a scalar row-by-row implementation; check the `st2` interleaving for chroma U/V separation.
- Verify `deblock_strength_neon` produces bs values in {0,1,2} matching a scalar boundary-strength reference for random nnz/ref/mv inputs, including the transposed layout.
- Verify MBAFF and 4:2:2 variants on their specific block sizes.

## 11. Open questions

- UNKNOWN: exact H.264 formula constants in the strong luma filter as implemented (the assembly builds them from shifted sums; equivalence to the standard formula should be verified against the spec, not inferred from instruction sequences).
- UNKNOWN: whether `deblock_strength_neon` ever produces strength 3 (intra) or whether that is handled entirely in C.
- UNKNOWN: behaviour for `tc0` values of 0 mixed with negative values in the same word (per-lane masks should handle it, but validate explicitly).
