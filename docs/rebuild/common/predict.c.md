# `common/predict.c` — intra prediction sample generation

**Role:** produces the predicted sample block for every H.264 intra prediction mode and
block size, writing predictions directly into the decoded picture buffer.
**Status:** dirty-side behavioural specification.

---

## 1. Header

- Source path: `common/predict.c` (header `common/predict.h`).
- One-sentence role: implement the 16x16 luma, 4x4 luma, 8x8 luma and chroma (8x8 for
  4:2:0, 8x16 for 4:2:2) intra predictors and the 8x8 edge prefilter, and dispatch them
  through per-block-size function tables.
- Marker: dirty-side behavioural specification.

---

## 2. Purpose

Intra coding predicts a block from already-decoded neighbouring samples. The H.264 standard
defines a small set of fixed predictors (vertical, horizontal, DC with several availability
variants, and a plane/"Paeth" predictor, plus five directional 4x4/8x8 modes). These must be
bit-exact between encoder and decoder, and the encoder additionally evaluates several modes
per block to choose the cheapest. This file supplies the complete set of predictors and the
dispatch machinery; a companion edge filter prepares the 8x8 luma neighbourhood for its
directional modes.

---

## 3. Public interface

Exported initialization functions (declared in `predict.h`):

- `x264_predict_16x16_init(cpu, pf[7])` — fill the seven 16x16 luma predictors.
- `x264_predict_8x8c_init(cpu, pf[7])` — fill the seven 4:2:0 chroma predictors.
- `x264_predict_8x16c_init(cpu, pf[7])` — fill the seven 4:2:2 chroma predictors.
- `x264_predict_4x4_init(cpu, pf[12])` — fill the twelve 4x4 luma predictors.
- `x264_predict_8x8_init(cpu, pf[12], *predict_filter)` — fill twelve 8x8 luma predictors
  **and** a pointer to the 8x8 edge filtering function.
- Individual `x264_predict_*_c` entry points (the portable implementations) are exported
  for use by the metric helpers in `pixel.c` and by checkasm.

Function signatures: a predictor takes a single `pixel *src` pointing at the top-left sample
of the block inside the decoded picture (FDEC_STRIDE layout), except the 8x8 luma variants,
which take `(pixel *src, pixel edge[36])` and read their neighbours from the prepared edge
array. The edge filter takes `(pixel *src, pixel edge[36], int i_neighbor, int i_filters)`.

All predictors **write** the block; they do not read it. Neighbours are addressed via
negative offsets from `src` using `FDEC_STRIDE` for vertical steps.

---

## 4. State and data structures

- The 8x8 luma edge array is a 36-byte scratch buffer with a fixed layout:
  - indices 7..14 hold the left samples `l7..l0` (bottom-to-top order),
  - index 15 holds the top-left sample,
  - indices 16..31 hold the top samples `t0..t15`,
  - index 32 holds a top-right extension (a duplicate of `t15` or of the last available top
    sample when the top-right neighbour is unavailable).
  Indices 0..6 and 33..35 are unused padding / alignment.
- Neighbour availability is passed as a bit mask with flags `MB_LEFT`, `MB_TOP`,
  `MB_TOPRIGHT`, `MB_TOPLEFT` (values 1, 2, 4, 8). A second mask (`i_filters`) says which of
  the left / top / top-right filters to apply; this lets the same function handle blocks at
  picture or slice edges.
- Mode enums (`predict.h`) number the predictors 0..11; values 9..11 are the "DC-left",
  "DC-top" and "DC-128" fallbacks used when the corresponding neighbours are unavailable.
  A fix-up table maps an unavailable requested mode to a legal one (see `predict.h.md`).
- `x264_pred_i4x4_neighbors[12]` (in `macroblock.h`) lists, per 4x4 mode, which neighbours
  must be available: e.g. V needs top, H and HU need left, DDL/VL need top+top-right,
  DDR/VR/HD need left+top-left+top, DC needs left+top, DC-128 needs none.

---

## 5. Algorithm / behaviour

Notation: `PIXEL_MAX = (1<<BIT_DEPTH)-1`, `c = 1 << (BIT_DEPTH-1)` (128 at 8-bit, the
mid-grey constant). `F1(a,b) = (a+b+1)>>1` is a rounded two-sample average;
`F2(a,b,c) = (a+2b+c+2)>>2` is a rounded three-sample weighted average with the centre
sample weighted twice. All sample outputs are in `[0, PIXEL_MAX]`; the directional modes
naturally stay in range because they only average Available samples, and the plane modes
explicitly clip.

### 5.1 16x16 luma predictors (block in FDEC_STRIDE layout)

- **V (mode 0):** copy the 16 samples of the row above the block down into every row of the
  block (four 4-sample groups replicated vertically).
- **H (mode 1):** for each row, take the single sample immediately left of the block and
  replicate it across all 16 columns of that row.
- **DC (mode 2):** sum the 16 left neighbours and the 16 top neighbours (32 samples),
  compute `(sum + 16) >> 5`, and fill the whole block with that value.
- **DC-left / DC-top / DC-128 (modes 4/5/6):** same shape but average only the left 16
  (`(sum+8)>>4`), only the top 16 (`(sum+8)>>4`), or use `c` directly.
- **P (plane, mode 3):** compute horizontal and vertical gradient accumulators:
  - `H = Σ_{i=0..7} (i+1) × (top[8+i] - top[6-i])`, where `top[k]` is the sample `k` above
    the block (i.e. `src[k - FDEC_STRIDE]`);
  - `V = Σ_{i=0..7} (i+1) × (left[8+i] - left[6-i])`, with `left[k]` the sample to the left
    of row `k` (`src[-1 + k*FDEC_STRIDE]`).
  - `a = 16 × (left[15] + top[15])`;
  - `b = (5H + 32) >> 6`, `cgrad = (5V + 32) >> 6`;
  - the prediction at `(x,y)` is `clip((a - 7b - 7cgrad + 16 + x*b + y*cgrad) >> 5)`.
  The horizontal gradient uses the top row's right half minus its left half; the vertical
  gradient uses the left column's bottom half minus its top half, each weighted linearly by
  distance from the block centre.

### 5.2 4:2:0 chroma 8x8 predictors

Neighbours: four top samples `top[0..3]` (left half), `top[4..7]` (right half), four left
samples `left[0..3]` (upper half) and `left[4..7]` (lower half).

- **V:** replicate the row of eight top samples into every row.
- **H:** replicate each left-side sample across its row.
- **DC:** compute four quadrant DC values:
  - `s0 = Σ top[0..3]`, `s1 = Σ top[4..7]`, `s2 = Σ left[0..3]`, `s3 = Σ left[4..7]`;
  - `dc00 = (s0+s2+4)>>3`, `dc01 = (s1+2)>>2`, `dc10 = (s3+2)>>2`, `dc11 = (s1+s3+4)>>3`;
  - fill the top-left 4x4 quadrant with `dc00`, top-right with `dc01`, bottom-left with
    `dc10`, bottom-right with `dc11`. The mixed quadrants use both a top and a left sum.
- **DC-left / DC-top / DC-128:** use `(s2+s3+2)>>2`, `(s0+s1+2)>>2`, or `c` uniformly.
- **P (plane):** gradient over 4 top and 4 left samples:
  - `H = Σ_{i=0..3} (i+1) × (top[4+i] - top[2-i])`, `V = Σ_{i=0..3} (i+1) × (left[4+i] - left[2-i])`;
  - `a = 16 × (left[7] + top[7])`;
  - `b = (17H + 16) >> 5`, `cgrad = (17V + 16) >> 5`;
  - `start = a - 3b - 3cgrad + 16`, prediction `(x,y) = clip((start + x*b + y*cgrad) >> 5)`.

### 5.3 4:2:2 chroma 8x16 predictors

The chroma block is 8 wide and 16 tall. The DC variant computes six neighbour sums:
two groups of four top samples and four groups of four left samples (rows 0..3, 4..7,
8..11, 12..15). Eight quadrant DC values are produced: the first row of quadrants uses
top-left + left-group-0, top-right uses only top-right sum; subsequent rows combine the
appropriate left group with the top-right sum. DC-left averages four left groups in
four horizontal bands; DC-top averages the two top groups across the whole height; DC-128
uses `c`. Plane mode uses `H` over 4 top samples with factor `(17H+16)>>5`, `V` over 8
left samples with factor `(5V+32)>>6`, `a = 16 × (left[15] + top[15])`, and
`start = a - 3b - 7cgrad + 16`.

### 5.4 4x4 luma predictors

Neighbours: `t0..t3` above, `t4..t7` above-right (same row), `l0..l3` to the left, and `lt`
at the top-left corner.

- **V:** copy the four top samples into all four rows.
- **H:** replicate each left sample across its row.
- **DC:** `(t0+t1+t2+t3 + l0+l1+l2+l3 + 4) >> 3`; variants use only left, only top, or `c`.
- **DDL (diagonal down-left, mode 3):** uses only the top row and top-right samples. Each
  output sample is `F2` of three consecutive samples taken along the anti-diagonal that
  passes through it: starting at `(0,0)` with `F2(t0,t1,t2)`, continuing with
  `F2(t1,t2,t3)` at `(0,1)` and `(1,0)`, and so on; the last sample `(3,3)` uses
  `F2(t6,t7,t7)` — the missing eighth top-right sample is replaced by `t7` (edge clamp).
  Sample selection is a pure function of `x+y`: the filter centre index is `x+y+1`.
- **DDR (diagonal down-right, mode 4):** uses left, top-left and top samples; the diagonal
  direction is the main diagonal. `(3,3)` uses `F2(t3,t2,t1)`; the centre sample of the
  three-tap is indexed by `3-(y-x)` style progression, with `lt` substituting for the
  missing neighbour when the diagonal crosses the top-left corner; `(0,3)` is
  `F2(l1,l2,l3)`.
- **VR (vertical-right, mode 5):** a mixture: samples above and to the right of the
  prediction direction use left-column taps, samples near the vertical axis use top-row
  taps. For example `(0,3)` uses `F2(l2,l1,l0)`, `(0,1)` and `(1,3)` use `F2(l0,lt,t0)`,
  while `(1,0)` and `(2,2)` use the rounded two-tap `F1(t0,t1)`. The transition happens
  along the line `x = 2y+1` (conceptually), with `lt` bridging top and left.
- **HD (horizontal-down, mode 6):** the transpose-like counterpart of VR: lower samples
  use left-column taps, upper-right samples use top-row taps. `(0,3)` is `F1(l2,l3)`,
  `(3,0)` is `F2(t2,t1,t0)`, and the centre region uses `F2(t0,lt,l0)` etc.
- **VL (vertical-left, mode 7):** uses top and top-right samples only; the 45-degree
  projection maps sample `(x,y)` to three-tap centred at `x+2y`, using `F1` on even
  positions and `F2` on odd ones, with the last sample `(7,7)` style clamp replaced by
  `F2(t10,t11,t12)` at `(3,3)`.
- **HU (horizontal-up, mode 8):** uses only the left column; the diagonal rises to the
  right, positions beyond the available neighbours repeat the last left sample `l3`:
  the bottom-right 2x2 area is filled with `l3` and intermediate positions use
  `F1(l2,l3)` / `F2(l1,l2,l3)`.

The exact per-pixel assignment is the H.264 specified intra_4x4 prediction; it can be
recovered by the generic rule "take the three samples along the prediction ray, clamp the
ray's extension to the last available neighbour, apply `F2` (or `F1` when the standard
specifies a half-sample position)".

### 5.5 8x8 luma predictors and edge filtering

The 8x8 directional modes mirror the 4x4 ones with a longer 16-sample top span and an
8-sample left span. They operate on the prefiltered edge array rather than raw neighbours:

- The left filter computes, for each left sample index, `F2` of the sample above, the
  current sample twice, and the sample below; at the top-left corner the missing top-left
  neighbour is substituted with the left sample itself, and at the bottom edge the last
  sample is repeated, giving `edge[6] = edge[7] = F2(l6,l7,l7)`-style clamping.
- The top filter is analogous, using three horizontal neighbours, substituting the
  top-left sample when unavailable and repeating the last top sample at the right edge.
- The top-right extension is filled by replicating the last top sample `t7` eight times,
  unless the top-right neighbour is available, in which case the real top samples `t8..t15`
  are filtered.
- DC uses the mean of the eight filtered left and eight filtered top samples
  (`(sum+8)>>4`); variants use only one side or `c`.
- DDL/DDR/VR/HD/VL/HU apply the same diagonal projection as 4x4 but over the 8x8 extent,
  using `F1`/`F2` on the filtered edge values; the HD/HU implementations pack pairs of
  two-sample predictions into 16-bit/32-bit words to ease SIMD, but the observable output
  is the standard filtered prediction.

### 5.6 Dispatch and fallback rules

Each init function stores the portable C predictor into each slot and then lets the
architecture init routine override entries (MMX/MMX2/SSE2/SSSE3/SSE4/AVX for x86; NEON for
32/64-bit ARM; Altivec, MSA, LSX/LASX elsewhere). The 8x8 init also installs the optional
SIMD edge filter. The mode-to-neighbour fix tables (in `predict.h`) make an unavailable mode
legal by mapping it to DC, and the analysis code checks neighbour availability before
calling a predictor.

---

## 6. Dependencies

- **Calls:** `x264_clip_pixel` for plane modes; packing helpers `pack_pixel_1to2` /
  `pack_pixel_2to4` in the 8x8 HD/HU variants; SIMD predictor implementations from
  architecture headers.
- **Called by:** macroblock analysis (`encoder/macroblock.c`, `encoder/analyse.c`) which
  evaluates candidate modes, reconstruction/inverse paths, and the intra-metric helpers in
  `pixel.c` that call the V/H/DC predictors to build a candidate block before measuring SAD/SATD.
- **Shared globals:** the neighbour-availability enum from `macroblock.h`; fix-up tables in
  `predict.h`.

---

## 7. Constants and tables

- Mid-grey constant `1 << (BIT_DEPTH-1)` (128 at 8-bit).
- Rounding constants: `+16` for 32-sample DC, `+8` for 16/8-sample DC, `+4` for
  three-/four-sample DC and `F2`, `+2` for `F1`.
- Plane coefficients: 16x16 uses `(5H+32)>>6` and `(5V+32)>>6`; 8x8 chroma uses
  `(17H+16)>>5` both axes; 8x16 chroma mixes `17/5` factors; all use final `>>5`.
- Mode fix tables: 16x16/chroma map modes 4..6 to DC; 4x4/8x8 map the unavailable directional
  modes and DC variants to DC with a sentinel `-1` for "mode not available".

---

## 8. Performance notes

Intra prediction is executed for **every intra macroblock and for every candidate mode
evaluated** (x264 evaluates V/H/DC and often all nine 4x4/8x8 modes), so it is hot at all
presets and especially for intra-heavy content (`--intra-refresh`, all-I, low-latency).

Optimization guidance:

- The fill operations (V, H, DC, DC-128) are pure memory writes; use 4/8/16-byte splat
  stores and unroll by row. The reference macros already splat four pixels at a time.
- Directional modes are multiply-free but branchy if written naively. The fastest approach
  is to precompute the source-index and filter-type per output pixel (they depend only on
  `x+y` or `x+2y`), then apply a small `F1`/`F2` table; SIMD versions use `pshufb`/`tbl`
  style lookups to gather the three taps.
- The 8x8 edge filter should be computed once per block and shared by all directional modes;
  recomputing it per candidate mode is wasteful.
- Plane modes should keep the gradients in 32-bit accumulators; the final `>>5` plus clip
  maps cleanly to saturating packed shifts.
- Keep the 36-byte edge buffer 16-byte aligned, with its special indices in the documented
  positions so SIMD gathers can load 8 or 16 consecutive neighbours at once.
- Literature: H.264 intra prediction is described in ITU-T H.264 clause 8.3; the plane
  predictor derivation and efficient SIMD realisation are discussed in the x264 project
  documentation and in common video-coding optimisation surveys.

---

## 9. References

- ITU-T H.264 / ISO IEC 14496-10, clause 8.3 (intra prediction) and the mode tables.
- Wiegand et al., "Overview of the H.264/AVC Video Coding Standard", IEEE TCSVT, 2003.
- x264 `predict.c` and architecture-specific `predict` implementations, public source tree.

---

## 10. Verification checklist

1. **Availability:** for a block with all neighbours present, verify each mode produces the
   standard predictor for a hand-computed small example (e.g. 4x4 DC with eight known
   neighbours, plane with a linear ramp).
2. **Edge fallbacks:** construct blocks at the top/left/right of a picture and confirm the
   DC-left/DC-top/DC-128 variants and the top-left/top-right substitutions give the
   expected values.
3. **Filter layout:** for 8x8, check the edge array contents after filtering: left indices
   7..14, top-left at 15, top at 16..31, extension at 32, with the specified clamping.
4. **Monotonic ramps:** feed a linear gradient neighbourhood; directional modes should
   reproduce the gradient exactly (or with the one-LSB rounding the standard prescribes).
5. **Dispatch equivalence:** for every CPU feature level, compare all mode outputs against
   the C reference over random neighbourhoods (checkasm methodology).
6. **No corruption:** predictors must not write outside the block; verify with guard pixels
   around the destination in a padded FDEC_STRIDE buffer.
7. **Bit depth:** repeat at 10-bit and verify the mid-grey constant and clipping boundary.

---

## 11. Open questions

- `UNKNOWN`: the exact per-pixel sample-index map for the 8x8 directional modes is fixed by
  the H.264 standard tables; it is described here by the projection rule rather than
  enumerated exhaustively. An implementer should consult clause 8.3 for the literal table.
- `UNKNOWN`: SIMD implementations of the 8x8 modes may reorder writes within the block;
  observable results must still match exactly, but the order of memory stores is unspecified.
- `UNKNOWN`: whether the top-right extension always uses `t7` or a filtered `t15` depends on
  the `i_neighbor`/`i_filters` combination supplied by the caller.
