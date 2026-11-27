# `common/deblock.c` — in-loop deblocking filter

**Role:** computes boundary strength and applies the H.264 adaptive deblocking filter to
reconstructed samples, with separate luma/chroma, intra/inter and MBAFF variants.
**Status:** dirty-side behavioural specification.

---

## 1. Header

- Source path: `common/deblock.c` (function-pointer types declared in `common/frame.h`).
- One-sentence role: remove blocking artifacts on 4x4 block boundaries of the reconstructed
  frame using per-edge strength derived from coding modes, motion and residual data.
- Marker: dirty-side behavioural specification.

---

## 2. Purpose

The H.264 deblocking filter operates in-loop: it modifies the reference pictures, so encoder
and decoder must agree sample-for-sample. The filter strength depends on whether either side
is intra, on nonzero residual coefficients, on reference-frame and motion-vector differences,
and on QP-derived thresholds `alpha` and `beta`. This file implements both the strength
computation and the four-tap/six-tap filters, plus the macroblock-row traversal and all
4:2:0/4:2:2/4:4:4 and interlaced (MBAFF) special cases. A lightweight variant
(`x264_macroblock_deblock`) supports deblock-aware rate distortion.

---

## 3. Public interface

Exported functions:

- `void x264_deblock_init(uint32_t cpu, x264_deblock_function_t *pf, int b_mbaff)` — fill
  the loop-filter vtable (type in `frame.h`) with scalar functions and CPU-specific overrides.
- `void x264_frame_deblock_row(x264_t *h, int mb_y)` — deblock one macroblock row of the
  decoded frame, including left/top neighbour edges and all plane variants.
- `void x264_macroblock_deblock(x264_t *h)` — deblock only the internal edges of the current
  macroblock (used by deblock-aware RD); does not use neighbouring MB state.

Vtable entries (see `frame.h`): `deblock_luma[2]` (horizontal/vertical inter luma),
`deblock_chroma[2]`, `deblock_h_chroma_420`, `deblock_h_chroma_422`,
`deblock_luma_intra[2]`, `deblock_chroma_intra[2]`, `deblock_h_chroma_420_intra`,
`deblock_h_chroma_422_intra`, the MBAFF variants `deblock_luma_mbaff`,
`deblock_chroma_mbaff`, `deblock_chroma_420_mbaff`, `deblock_chroma_422_mbaff`,
`deblock_luma_intra_mbaff`, `deblock_chroma_intra_mbaff`,
`deblock_chroma_420_intra_mbaff`, `deblock_chroma_422_intra_mbaff`, and `deblock_strength`.
Inter filters take `(pix, stride, alpha, beta, int8_t tc[4])`; intra filters take
`(pix, stride, alpha, beta)`; `deblock_strength` takes the nnz/ref/mv caches and writes
`bs[2][8][4]`.

---

## 4. State and data structures

- Boundary strength `bs[dir][edge][i]`: `dir=0` vertical edges (left/right of 4x4 blocks),
  `dir=1` horizontal edges; `edge=0..3` the four 4x4 boundaries within a macroblock; `i=0..3`
  the four samples along each edge. Values 0..3; 3 is used for intra edges in the RD variant.
- `tc[4]` is the clipping limit per sample along an edge, derived from QP and strength.
- `alpha`/`beta` tables are indexed by `qp + offset + 24` (offset accounts for negative QP
  and the slice alpha/beta offsets) and scaled by `1 << (BIT_DEPTH-8)` for high bit depth.
  `alpha` controls the p0/q0 difference test, `beta` the p1/p0 and q1/q0 tests, and
  `tc0_table` gives the base clipping limit as a function of strength (index 0 unused/-1).
- Inputs come from the macroblock caches `nnz`, `ref[2]`, `mv[2]`, with `X264_SCAN8_*` layout
  (a 5x8 luma neighborhood plus chroma regions); `deblock_strength` walks four positions per
  edge using strides 8 (vertical) or 1 (horizontal) inside that cache.
- Effective-QP tracking: when a block was force-skipped (`X264_MBINFO_CONSTANT`) and an edge
  is filtered, `h->fdec->effective_qp[]` may be forced to a maximal value to avoid blurring
  accumulation.
- Lifetime: `bs` arrays are per-row scratch in `h->deblock_strength[mb_y&1]` (or per-MB in the
  RD variant); the vtable is permanent.

---

## 5. Algorithm / behaviour

### 5.1 Boundary strength (`deblock_strength_c`)

For each direction and each of the four edges, walk the four samples. Let `loc` be the cache
index of the current 4x4 block and `locn` the neighbour across the edge:

1. If either side has any nonzero coefficient (`nnz[loc]` or `nnz[locn]`), `bs = 2`.
2. Else, if list-0 reference indices differ, or the list-0 horizontal MV difference is at
   least 4 (quarter-pel units), or the vertical MV difference is at least `mvy_limit`, or
   (in a B slice) the analogous list-1 condition holds, `bs = 1`.
3. Otherwise `bs = 0`.

`mvy_limit` is 4 for progressive MBAFF rows and 2 for MBAFF fields (passed as
`4 >> MB_INTERLACED`).

### 5.2 Threshold tables

`alpha_table(q)`: zero for very low QP, then a monotonically rising sequence (4,4,5,6,7,
8,9,10,12,13,15,17,20,22,25,28,32,36,40,45,50,56,63,71,80,90,101,113,127,144,162,182,
203,226,255,255), with 12 extra saturating entries of 255 at the top.
`beta_table(q)`: a gentler rise (2,2,2,3,3,3,3,4,4,4,6,6,7,7,8,8,9,9,10,10,11,11,12,12,
13,13,14,14,15,15,16,16,17,17,18,18) plus saturating 18 entries.
`tc0_table(q)[s]` gives the clipping limit for strength `s=0..3`: `-1` for invalid, and a
rising sequence from 1 up to 25 for strength 3 at high QP (e.g. near the top: `{-1,13,17,25}`).
Low QP entries are zero, which disables filtering.

`deblock_edge` computes `index_a = qp+a`, `index_b = qp+b`, where `a`/`b` are the slice alpha
and beta offsets minus the bit-depth QP offset; `alpha = alpha_table(index_a) << (BIT_DEPTH-8)`,
`beta = beta_table(index_b) << (BIT_DEPTH-8)`. If all four `bS` entries are zero, or alpha or
beta is zero, it returns immediately. Otherwise `tc[i] = tc0_table(index_a)[bS[i]] × (1<<(BIT_DEPTH-8)) + b_chroma`, where `b_chroma` is 1 for chroma in 4:2:0/4:2:2 and 0 for luma or 4:4:4.

### 5.3 Inter luma edge filter (`deblock_edge_luma_c`)

Let samples across the boundary be `p2,p1,p0 | q0,q1,q2` (the boundary is between p0 and q0,
`xstride` selects horizontal vs vertical). Tests:

- Gate: `|p0-q0| < alpha` AND `|p1-p0| < beta` AND `|q1-q0| < beta`.
- If `|p2-p0| < beta`: when `tc0 != 0`, set `p1' = p1 + clip3( ((p2 + ((p0+q0+1)>>1))>>1) - p1, -tc0, tc0 )`; then
  increment the clipping limit by 1.
- If `|q2-q0| < beta`: analogously update `q1'` with the clipped correction toward
  `((q2 + ((p0+q0+1)>>1))>>1)`, then increment the limit.
- Core filter: `delta = clip3( (4*(q0-p0) + (p1-q1) + 4) >> 3, -tc, tc )`, then
  `p0' = clip_pixel(p0 + delta)`, `q0' = clip_pixel(q0 - delta)`.

The 4× factor makes the corner samples (p1,q1) contribute only a quarter weight; the `+4`
is rounding for the `>>3`. `deblock_luma_c` applies this to four rows, skipping a row entirely
when `tc0[i] < 0`. Horizontal and vertical variants differ only in the stride arguments.

### 5.4 Inter chroma edge filter (`deblock_edge_chroma_c`)

Only p1,p0 | q0,q1 are used. Same gate as luma. Then
`delta = clip3( (4*(q0-p0) + (p1-q1) + 4) >> 3, -tc, tc )` and the two inner samples are
updated with clipping. No p2/q2 filtering occurs; `tc <= 0` rows are skipped.

### 5.5 Intra luma edge filter (`deblock_edge_luma_intra_c`)

Uses p3,p2,p1,p0 | q0,q1,q2,q3. Gate as before, then a **stronger** condition
`|p0-q0| < (alpha>>2)+2`:

- If true and `|p2-p0| < beta`: three samples are replaced by rounded weighted averages:
  `p0' = (p2 + 2p1 + 2p0 + 2q0 + q1 + 4) >> 3`,
  `p1' = (p2 + p1 + p0 + q0 + 2) >> 2`,
  `p2' = (2p3 + 3p2 + p1 + p0 + q0 + 4) >> 3`.
  The q side is symmetric with `q0' = (p1 + 2p0 + 2q0 + 2q1 + q2 + 4)>>3`, etc.
- If the strong condition holds but the p2/q2 test fails, only `p0' = (2p1+p0+q1+2)>>2`
  (and symmetric q0') is applied.
- If the strong condition fails, the two inner samples are set to
  `p0' = (2p1+p0+q1+2)>>2`, `q0' = (2q1+q0+p1+2)>>2`.
Intra edges are thus filtered more aggressively and with more taps than inter edges.

### 5.6 Intra chroma edge filter (`deblock_edge_chroma_intra_c`)

Same gate; then `p0' = (2p1+p0+q1+2)>>2`, `q0' = (2q1+q0+p1+2)>>2`.

### 5.7 Row traversal (`x264_frame_deblock_row`)

For each macroblock in the row (advancing by two and toggling parity in MBAFF):

- Determine `a`, `b` from slice offsets and `qp_thresh = 15 - min(a,b) - max(0, chroma_qp_offset)`.
  `first_edge_only` is true for a 16x16 inter macroblock with zero luma CBP or when
  `qp <= qp_thresh`; in that case only the outermost edges are filtered.
- Compute pointers into the luma and chroma planes with the appropriate chroma height
  (`16 >> CHROMA_V_SHIFT`) and MBAFF stride doubling; use `uvdiff` to reach the V plane in
  4:4:4.
- **Left neighbour:** if present, use the average of the current and left QP
  (`(qp+qp_left+1)>>1`) and chroma QP, choose intra or inter filter if either side is intra,
  and handle the MBAFF mixed-field case with two separate half-field edges. Otherwise use the
  plain left QP. Effective-QP reset happens if nonzero strength and `mb_info` is present.
- **Internal vertical edges 1,2,3:** filtered with the current QP unless `first_edge_only`.
  Edge 2 (the centre 8x8 boundary) is skipped when `transform_8x8` is active.
- **Top neighbour:** analogous, with averaged top QP; if MBAFF field transitions are involved
  the two field top edges are handled separately, and intra edges force `bs` to 3.
- **Internal horizontal edges 1,2,3:** same rules as vertical.
- Chroma filtering follows the format: 4:2:0 filters only even internal edges (because two
  luma rows share one chroma row) with `b_chroma=1`; 4:2:2 filters all relevant edges with a
  height of 16; 4:4:4 treats U and V as two luma planes with `b_chroma=0`; MBAFF uses the
  corresponding `*_mbaff` pointers.

### 5.8 Deblock-aware RD (`x264_macroblock_deblock`)

Uses the current macroblock's cached `bs`. If the MB is intra, all internal edges are forced
to strength 3 by writing four-byte 0x03 patterns into the `bs` rows. Otherwise it calls
`deblock_strength` on the local caches. It then filters edges 1,2,3 in each direction
(skipping 1 and 3 for 8x8 transforms), using the current QP/chroma QP and no neighbour
averaging. Intra edges are not handled here (documented as a TODO in the source).

### 5.9 Initialization / dispatch

Scalar functions are installed into every slot; then x86 MMX2 → SSE2 (with a
`STACK_MOD4` caveat disabling some vertical variants) → SSSE3 → AVX → AVX2 → AVX512
strength implementations are layered in; ARM NEON/SVE, Altivec, MSA and LSX/LASX follow.
Two equivalences are aliased at the end: `deblock_chroma_422_mbaff = deblock_h_chroma_420`
and `deblock_chroma_422_intra_mbaff = deblock_h_chroma_420_intra`.

---

## 6. Dependencies

- **Calls:** `x264_clip_pixel`/`x264_clip3`, `x264_prefetch_fenc`, SIMD deblock kernels.
- **Called by:** frame reconstruction (`encoder/macroblock.c` calls `x264_frame_deblock_row`
  per MB row), rate-distortion analysis (`x264_macroblock_deblock`).
- **Shared state:** `h->sh` slice header offsets, `h->mb` caches, `h->fdec` planes and
  `effective_qp`/`mb_info`, `h->chroma_qp_table`, `h->loopf` vtable.

---

## 7. Constants and tables

- 52+36-entry `alpha`, `beta` and `tc0` tables indexed at `qp+offset+24` (so negative
  indices up to −24 are valid). Values are the standard H.264 Table 8-16/8-17-derived
  sequences given in section 5.2.
- `b_chroma` offset of 1 for 4:2:0/4:2:2 chroma.
- `0x03030303` / `0x0303030303030303` fill patterns for intra strength 3.
- `qp_thresh = 15 - min(a,b) - max(0,chroma_qp_offset)` early-out rule.

---

## 8. Performance notes

Deblocking touches every reconstructed sample of every frame, so it is a major cost at all
presets; the strength calculation is also run for every inter macroblock.

Optimization guidance:

- The four-sample edge filter is SIMD-friendly: load p2..q2 into a vector, compute the
  average `(p0+q0+1)>>1` once, use saturating arithmetic for p0/q0, and pack the results
  back. The reference SIMD kernels process 4 or 8 edges in parallel.
- The `alpha/beta` gates are per-edge branches; computing them as masks and using conditional
  moves/blends avoids mispredictions.
- `deblock_strength` should be vectorized by comparing MV/ref arrays in parallel and OR-ing
  the resulting masks; the AVX2/AVX512 implementations do this.
- Data layout: luma stride should be a multiple of 16 bytes, chroma pointers 8-byte aligned;
  MBAFF doubles strides, so keep both field planes contiguous.
- QP averaging and the intra decision are per-edge but can be hoisted to per-macroblock.
- Cache behaviour: the filter streams over the frame; prefetching the next macroblock row
  (`x264_prefetch_fenc`) hides memory latency.
- Literature: the algorithm is H.264 clause 8.4 / Fig. 8-5; efficient SIMD formulations are
  discussed in the x264 developer notes and in "Implementation and optimization of the
  H.264 deblocking filter on SIMD architectures" style papers.

---

## 9. References

- ITU-T H.264 clause 8.4 (deblocking filter), Tables 8-16/8-17.
- JVT documents on deblocking (e.g. JVT-B118-related discussions).
- x264 public source: `common/deblock.c`, `common/frame.h`, SIMD deblock kernels.

---

## 10. Verification checklist

1. **Strength rules:** construct cache entries that trigger each case (nonzero residual → 2,
   ref/MV change → 1, identical → 0) including the B-frame list-1 condition and `mvy_limit`.
2. **Gate tests:** for a single edge, sweep `p0-q0`, `p1-p0`, `q1-q0` around `alpha`/`beta`
   and verify filtering is skipped at or above the thresholds (strict `<`).
3. **Inter filter numerics:** verify `delta` and p1/q1 corrections against hand-computed
   values, including the `tc` increment when the p2/q2 tests pass and the `tc0==0` exception.
4. **Intra filter:** verify both branches of the strong condition and the weak fallback with
   known sample values.
5. **QP scaling:** at 10-bit, verify alpha/beta/tc are multiplied by 256 and clipping uses
   `PIXEL_MAX`.
6. **Edge order:** check that left, internal vertical, top, internal horizontal are applied
   in that order and that 8x8 transform skips the centre edges.
7. **Format variants:** validate 4:2:0 (even chroma edges only), 4:2:2 (all chroma edges),
   4:4:4 (two luma-like chroma planes), and MBAFF field handling.
8. **Dispatch equivalence:** compare each SIMD vtable entry against the scalar reference
   over random frames (checkasm-style).

---

## 11. Open questions

- `UNKNOWN`: the exact effective-QP reset semantics depend on `X264_MBINFO_CONSTANT` being
  set by the lookahead; only the WHEN is described here.
- `UNKNOWN`: the full contents of `x264_deblock_function_t` are in `frame.h`; any future
  additional MBAFF/chroma slots are outside this file's scope.
- `UNKNOWN`: the C reference for `deblock_strength` is exact for the described cache layout;
  SIMD variants may read a few bytes past an edge but must not change observable results.
