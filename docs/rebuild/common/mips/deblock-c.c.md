# `common/mips/deblock-c.c` — MSA deblocking filter

**Dirty-side behavioural specification.** Source path: `E:\_x264_dirty_stash\x264-source\common\mips\deblock-c.c`. Role: MSA-128 implementation of the H.264 loop filter (deblocking), including intra and inter edge filters for luma and interleaved chroma, plus the vectorized boundary-strength computation. Compiled only for 8-bit builds.

## 1. Purpose

The deblocking filter smooths block artifacts on reconstructed macroblock edges. It is invoked for every edge of every macroblock and, because it follows reconstruction and precedes reference storage, it is on the critical path of the encode loop. The MSA version processes a full 16-pixel edge (or 8-pixel chroma edge) per vector, uses per-quarter predicates stored in vector mask lanes, and skips work entirely when all boundary strengths are zero.

## 2. Public interface

Nine exports declared in `mips/deblock.h.md`:

- `x264_deblock_v_luma_msa(pix, stride, alpha, beta, tc0)` — inter vertical luma edge; `pix` points at the first pixel of the right block's boundary column.
- `x264_deblock_h_luma_msa(...)` — inter horizontal luma edge.
- `x264_deblock_v_chroma_msa(...)`, `x264_deblock_h_chroma_msa(...)` — interleaved (Cb/Cr alternating bytes) chroma variants.
- `x264_deblock_v_luma_intra_msa(pix, stride, alpha, beta)`, `x264_deblock_h_luma_intra_msa(...)`, `x264_deblock_v_chroma_intra_msa(...)`, `x264_deblock_h_chroma_intra_msa(...)` — stronger intra variants without `tc0`.
- `x264_deblock_strength_msa(nnz, ref, mv, bs, mvy_limit, bframe)` — computes boundary strength.

Contract details: `pix` is modified in place; `alpha`/`beta` are the standard thresholds; `tc0[0..3]` are four per-quarter clipping thresholds — a **negative** value disables filtering for that quarter (the wrapper converts negative to boundary strength 0). `bs` is the `uint8_t[2][8][4]` output strength array.

## 3. State and data structures

No globals. The kernels use:

- Eight 16-byte vectors holding rows `p3,p2,p1,p0 | q0,q1,q2,q3` around the edge. For horizontal (`v_*` entry points) these are loaded with stride from the frame; for vertical (`h_*`) the columns are gathered with a `TRANSPOSE16x8_UB_UB`/`TRANSPOSE8x8_UB_UB` transpose.
- Per-quarter `bs`/`tc` broadcast into four word lanes of a vector (`insve_w`), so each 16-byte vector holds four independent 4-pixel segments.
- Masks: `is_less_than`, `is_less_than_beta`, `negate_is_less_than_beta`, computed per byte lane; `bmnz_v` merges new values only where the mask bits are set.

## 4. Algorithm / behaviour

### Shared filter macros

Three macros encode the H.264 filtering equations in vector form:

**`AVC_LPF_P0P1P2_OR_Q0Q1Q2`** — the strong intra filter. Given the original boundary samples `p0/q0`, the far samples `p3/q3`, and intermediate `p1/p2`, it computes:
- `threshold = p0 + q3 + p1` (per lane),
- filtered `p0 = ((2·threshold + p2 + q1) + 4) >> 3` using a rounding arithmetic shift by 3,
- filtered `p1 = (p2 + threshold + 2) >> 2` (rounding shift by 2),
- filtered `p2 = (3·p2 + 2·p3 + threshold + 4) >> 3`.

These are exactly the H.264 strong intra edge equations with rounding. The macro is used symmetrically for the p-side and q-side by swapping arguments.

**`AVC_LPF_P0_OR_Q0`** — the weak filter for the boundary sample only: `(p0 + q1 + 2·p1 + 2) >> 2`, i.e. round-to-nearest average of `p0` and `2·p1+q1`.

**`AVC_LPF_P1_OR_Q1`** — the inter weak filter for the second sample: computes a clipped intermediate `clip3 = clip(ave(p0,q0) − 2·p1 averaged with p2, −tc, +tc)`, then `p1' = p1 + clip3`. The `aver_u_h`/`ave_s_h` instructions provide the rounding averages; `CLIP_SH` clamps to `[-tc, tc]`.

**`AVC_LPF_P0Q0`** — the boundary-pair modification: `delta = ((4·(q0−p0) + (p1−q1) + 4) >> 3)` clipped to `[−threshold, threshold]`, then `p0' = clip(p0 + delta, 0, 255)`, `q0' = clip(q0 − delta, 0, 255)`.

### Intra luma edge (horizontal orientation)

1. Broadcast alpha/beta to 16-byte vectors.
2. Load the four pairs of rows around the boundary (`p1,p0,q0,q1`) with stride.
3. Compute `|p0−q0| < alpha`, `|p1−p0| < beta`, `|q1−q0| < beta`. Combine with AND into `is_less_than`; if all lanes are zero (`test_bz_v`) the function returns immediately — the common case for flat edges.
4. Load `p2,p3,q2,q3`. Compute the strong-filter predicate: `|p2−p0| < beta` AND `|p0−q0| < (alpha>>2)+2`. Split the 16-byte vectors into low/high 8-byte halves (`sldi_b` with zero) and, for each half where any lane qualifies, run `AVC_LPF_P0P1P2_OR_Q0Q1Q2` on widened halfwords. Pack back to bytes and merge with `bmnz_v`; store `p1,p2` and `q1,q2` rows.
5. The complement mask qualifies the weak filter: run `AVC_LPF_P0_OR_Q0` on p-side and q-side, merge, store `p0` and `q0`.
6. Repeat steps 4–5 for the q-side using `q3,q2,p0,q1,p2,p1` as arguments.

### Intra luma edge (vertical orientation)

Same algorithm, but the 16 rows of the 16x8 block around the vertical edge are loaded with `LD_UB8` twice and transposed with `TRANSPOSE16x8_UB_UB` into eight vectors that hold `p3,p2,p1,p0,q0,q1,q2,q3` as columns. After filtering, the results are re-transposed via interleaves and stored column-wise with `ST4x4_UB`/`ST2x4_UB`.

### Intra chroma (interleaved Cb/Cr)

Only the weak `AVC_LPF_P0_OR_Q0` is applied (H.264 chroma uses the 4-tap filter). Horizontal variant loads `p1,p0,q0,q1` with stride and computes the combined mask; vertical variant loads eight rows, transposes with `TRANSPOSE8x8_UB_UB` into U/V-separated lanes, filters using `p0/q0` pairs, then reinterleaves the filtered U and V bytes into the original interleaved chroma plane with `ILVR_D2_SB`/`ILVRL_B2_SB` and `ST4x8_UB`.

### Inter luma edge

1. Build a 16-byte `bs` vector by inserting each of the four caller-supplied strengths into one word lane; if all are zero, return. Build a `tc` vector similarly and compute `is_bs_greater_than0 = bs > 0`.
2. Load/transpose the edge samples as in the intra case.
3. Combine `|p0−q0| < alpha`, `|p1−p0| < beta`, `|q1−q0| < beta`, and `bs>0` into `is_less_than`.
4. Compute `−tc` and its sign vector; widen `tc`, `p1`, `p0`, `q0` to halfwords.
5. If `|p2−p0| < beta` (combined with `is_less_than`), run `AVC_LPF_P1_OR_Q1` on p1 and q1; merge results with `bmnz_v`. Where p1/q1 were filtered, increment `tc` by 1 (H.264's `tc += 1` rule when the second sample was modified).
6. Compute the final `−threshold` from the (possibly incremented) `tc` and run `AVC_LPF_P0Q0` on p0/q0; merge and store all four sample rows.

### Inter chroma edge

Only the p0/q0 boundary pair is modified, using `AVC_LPF_P0Q0` with `tc` (and the `bs < 4` condition in the vertical variant). The vertical variant transposes the interleaved chroma rows, filters U and V lanes, then reinterleaves and stores via `ST4x8_UB`.

### Strength computation

`avc_deblock_strength_msa` is the vectorized version of the standard strength rules. For each of the four 4x4-block positions along a macroblock edge it computes:

- strength **2** if either adjacent block has any nonzero coefficient (`nnz[loc] | nnz[locn] != 0`);
- else strength **1** if the reference indices differ in list 0 (`ref0[loc] != ref0[locn]`) or the motion-vector difference in either component meets/exceeds a threshold (`|mvx diff| >= 4` or `|mvy diff| >= i_mvy_limit`), or in B-frames the same test on list 1;
- else strength **0**.

It processes vertical edges in one SIMD pass (loading `nnz`, `ref`, `mv` for five consecutive 4x4 blocks and using `splati_w` to align neighbours) and horizontal edges in a second pass using `TRANSPOSE8X4_SH_SH` on the MV arrays. Each result word is extracted and stored as four bytes into `bs[1][edge]` or `bs[0][edge]`. The public wrapper falls back to a scalar nested loop when `bframe != 0`, since the B-frame list-1 condition is not vectorized.

## 5. Dependencies

- **Calls:** `mips/macros.h` (`LD_UB4/5/8`, `ST_UB`, `ST4x4_UB`, `ST2x4_UB`, `ST4x8_UB`, `ILVR_B*`, `ILVL_B*`, `TRANSPOSE16x8_UB_UB`, `TRANSPOSE8x8_UB_UB`, `TRANSPOSE8X4_SH_SH`, `PCKEV_B*`, `CLIP_SH`, `CLIP_SH2_0_255`, `SW4`, `SD4`), MSA intrinsics (`asub_u_b`, `test_bz_v`, `srari_h`, `aver_u_h`, `ave_s_h`, `bmnz_v`, `xori_b`, `sldi_b`, `maxi_s_h`, `mini_s_h`, `cle_u_h`, `asub_s_h`, `insve_w`, `copy_u_w`).
- **Called by:** `x264_deblock_init()` in `common/deblock.c` when `X264_CPU_MSA` is set.
- **Shared config:** `HIGH_BIT_DEPTH` gate; `X264_SCAN8_*` constants for the strength arrays.

## 6. Constants and tables

- Threshold predicates: `alpha`, `beta`, `alpha>>2 + 2` (the "tmp_flag" strong-filter condition).
- Rounding shifts: `>>2` and `>>3` with rounding (`srari_h`) for the strong boundary filters; `>>2` for the weak p0/q0 filter; delta computation `(4·(q0−p0)+(p1−q1)+4)>>3`.
- Clip: `[−tc, tc]` for `p1`/`q1` corrections; `[0,255]` for final samples.
- Strength thresholds: **4** for horizontal MV difference, `i_mvy_limit` for vertical MV difference.
- Broadcast constants 0, 1, 2, 4 used in strength masks.

**Why:** all constants follow H.264 §8.4.2/§8.4.3 exactly; the `+2/+4` terms implement round-to-nearest; `tc` is the clipping threshold derived from QP and index.

## 7. Performance notes

- **Hotness:** very high; each frame filters four edges per macroblock.
- **SIMD status:** fully MSA-128, including conditional execution via masks rather than branches. The early-out `test_bz_v(is_less_than)` is the only data-dependent branch and is common (most edges do not need filtering).
- **Cache behaviour:** sequential row access for horizontal edges; for vertical edges a 16x8 row-block transpose trades strided GPR loads for vector registers.
- **Optimization tips:** (a) check the four `tc` values first — if all are negative, the wrapper already returns; (b) the strongest speedup comes from the `test_bz_v` early-out, so keep predicate computation minimal before it; (c) on MSARev6, `bmnz_v` and `insve_w` are single-cycle, so the per-quarter masks are cheap; (d) the strength computation's B-frame fallback is scalar — a vectorized version of the list-1 condition would help B-frame-heavy content.
- **References:** ITU-T H.264 §8.4 (deblocking filter); x264 `common/deblock.c`; MSA programmer's guide for `bmnz_v`, `asub_u_b`, `test_bz_v`.

## 8. Verification checklist

1. For each of the eight filter entry points, feed deterministic edge patterns and compare output with the scalar reference bit-for-bit.
2. Coverage of all three regimes: no filtering (`|p0−q0| >= alpha`), weak filter, strong intra filter — construct inputs that trigger each.
3. Check the `tc0 < 0` disable rule: set one of the four tc values negative and verify that quarter is unchanged.
4. Verify rounding at half-way cases for the `>>2`/`>>3` filters.
5. Chroma interleaving: verify filtered U/V bytes land in the correct even/odd positions after the vertical transpose/reinterleave path.
6. Strength: construct blocks with known nnz/ref/mv patterns and verify `bs` equals the scalar computation for P and B frames.
7. ASan: check that vertical kernels do not read above `pix−4*stride` or beyond 16 rows.

## 9. Open questions

- UNKNOWN: exact orientation convention of `v_*` versus `h_*` entry points relative to macroblock edges; the names in the implementation map `v_*` to the horizontal-edge filter (row-based) and `h_*` to the vertical-edge filter — confirm against the generic dispatcher.
- UNKNOWN: whether the `bs[2][8][4]` direction index in the strength function matches the generic `deblock.c` usage; verify with a known macroblock layout.
- UNKNOWN: behaviour when neighbors are outside the picture (frame edges) is handled by callers passing saturated `tc`/flags; not tested here.
- UNKNOWN: the B-frame strength fallback is scalar — confirm this is intentional and not a partially vectorized path.
