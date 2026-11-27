# `encoder/me.c` — motion estimation engine

**Role (one sentence):** This file finds the best integer and fractional motion vector for a block by evaluating candidate displacements with a distortion-plus-bit-cost metric using several selectable search patterns and optional rate-distortion refinement.

> **Dirty-side behavioural specification.** Written from the reference source and public literature for the clean-room reimplementation. No source code or near-verbatim pseudocode is reproduced; only behaviour, structure, constants and decision conditions are described.

---

## 1. Header

- Source path: `encoder/me.c` (reference tree `E:\_x264_dirty_stash\x264-source\encoder\me.c`, about 1355 lines).
- Role: full-pel motion search (five algorithms), sub-pel refinement (half-pel and quarter-pel), bi-predictive vector refinement, and RD-based vector refinement, all operating on an `x264_me_t` supplied by the caller.
- This translation unit owns the static functions `refine_subpel` and `me_refine_bidir`, the exported `x264_*` entry points, and the mutable global `x264_iter_kludge` used to defeat loop unrolling.

---

## 2. Purpose

Motion estimation is the single largest CPU cost of an inter frame. The file isolates two distinct jobs:

1. **Full-pel search.** Given a block, one or more reference planes, a predicted MV (MVP) and a list of extra MV predictors, scan a region of integer positions and keep the one minimising `SATD/SAD + lambda * mv_bits` (plus a reference-frame bit cost that is added by the caller).
2. **Sub-pel refinement.** Starting from the winning full-pel vector, search the surrounding half-pel and quarter-pel positions using interpolated reference samples, optionally switching the distortion measure from SAD to SATD, optionally adding chroma distortion, and optionally finishing with a true RD (entropy-coded residual) comparison.

A third job, bidirectional refinement, jointly optimises the two MVs of a Bi prediction with a four-dimensional local search. All of it is deliberately structured so that the caller can reuse results (e.g. "refine only the winner") and so that a single search routine serves every block size from 4x4 to 16x16.

---

## 3. Public interface

All symbols use the `x264_template()` indirection so that a build can rename them; the clean implementation must expose the same logical entry points.

| Entry point | Contract |
|---|---|
| `int x264_analyse_init_costs(x264_t*)` (declared in `analyse.h`, used here) | Allocates the MV-bit lookup table indexed by QP; must run before any search. |
| `void x264_me_search_ref(x264_t *h, x264_me_t *m, int16_t (*mvc)[2], int i_mvc, int *p_fullpel_thresh)` | Runs the configured full-pel method (`h->mb.i_me_method`) and then, for `subme >= 2`, the configured sub-pel passes. `mvc` is an array of up to 16 extra candidate MVs in quarter-pel units; `i_mvc` is its length. `p_fullpel_thresh` is an optional in/out pruning threshold shared across reference frames. Writes `m->mv`, `m->cost`, `m->cost_mv`; for `subme >= 3` the cost may refer to a sub-pel predictor instead of the full-pel winner. |
| `void x264_me_search(h, m, mvc, i_mvc)` | Convenience form of the above with the pruning pointer null. |
| `void x264_me_refine_qpel(x264_t *h, x264_me_t *m)` | Refines the *winner* with the iteration counts reserved for post-selection refinement (`subpel_iterations[subme][0]` half-pel, `[1]` quarter-pel). For blocks `<= 8x8`, the caller's reference-frame bit cost is first subtracted from `m->cost`, because the comparison should be internal to this block. |
| `void x264_me_refine_qpel_refdupe(x264_t *h, x264_me_t *m, int *p_halfpel_thresh)` | Special shortcut for reference frames whose content is a known duplicate: no half-pel search and at most two quarter-pel iterations. Optionally uses the shared multi-ref threshold. |
| `void x264_me_refine_qpel_rd(x264_t *h, x264_me_t *m, int i_lambda2, int i4, int i_list)` | RD-driven sub-pel search on one partition. Uses SATD only as a cheap pre-filter; the final decision is `x264_rd_cost_part()`. Writes the chosen MV and the packed MVD into the MB caches. |
| `void x264_me_refine_bidir_satd(x264_t*, x264_me_t *m0, x264_me_t *m1, int i_weight)` | Four-dimensional SATD refinement of a Bi prediction; no RD. |
| `void x264_me_refine_bidir_rd(x264_t*, x264_me_t *m0, x264_me_t *m1, int i_weight, int i8, int i_lambda2)` | Same search but with RD final selection. Sets `h->mb.b_skip_mc` around the call so the entropy/MC work is not duplicated. |
| `uint64_t x264_rd_cost_part(...)` | Declared here, implemented in `rdo.c`. |
| `int x264_iter_kludge` | Public mutable integer, deliberately not compile-time constant, so a compiler cannot unroll the 9-position MC preparation loop. Readers must not rely on its value. |

Helper macros defined in this file (and in `me.h`) encode packed-MV updates and relative cost additions; they are implementation shorthand, not part of the ABI.

---

## 4. State and data structures

- **`x264_me_t`** (defined in `me.h`): input fields `i_pixel` (block class), `p_cost_mv` (lambda times bit count for each MV component, indexed relative to the MVP), `i_ref_cost`, `i_ref`, `weight`, `p_fref[12]` (luma planes at full/half-pel offsets plus chroma), `p_fref_w` (weighted full-pel plane for the fast SAD path), `p_fenc[3]`, `integral` (for ESA/TESA), `i_stride[3]`, `mvp[2]`. Output fields `cost_mv`, `cost` (distortion plus MV bits; reference cost is added by the caller), `mv[2]` in quarter-pel units. The first member is aligned to 64 bytes and the struct size is a multiple of that alignment so copies can be SIMD-aligned.
- **Local search state:** current best cost/vector (`bcost`, `bmx`, `bmy`), origin vector for grouped four-candidate evaluations, predictor vector, packed 32-bit MV values with a special mask trick used to test range membership in one comparison, and scratch arrays for four candidate costs and up to 16 candidate MVs.
- **`x264_me_t` ownership:** the caller owns the structure (typically a field of `x264_mb_analysis_t`); the search routines mutate only output fields plus `i_ref`/`mvp` as documented. The `p_fref[]` pointers remain owned by the lookahead/frame manager.
- **Scratch buffers:** `h->scratch_buffer` receives the list of x-candidates produced by successive elimination; stack arrays are aligned (16/32/64 bytes) because SIMD comparison functions read them in wide chunks.
- **Invariants:** costs are non-negative and saturate at `COST_MAX` (2^28) or `COST_MAX64` (2^60); MVs are clipped to `h->mb.mv_limit_fpel` (full-pel) or `mv_min_spel/mv_max_spel` (sub-pel) before use; the stored MV is always in quarter-pel units.

---

## 5. Algorithm / behaviour

### 5.1 Cost model

A candidate at full-pel coordinates `(mx,my)` costs:

`cost = fpelcmp(p_fenc, p_fref_w[my*stride+mx]) + cost_mvx[mx] + cost_mvy[my]`

where `cost_mvx` and `cost_mvy` are views into the shared table `p_cost_mv` shifted so index zero corresponds to the MVP; each entry is `round(lambda * exp_golomb_bits(delta))` precomputed in `analyse.c`, and clipped to 16 bits. The bit cost is therefore *relative to the predictor* and includes sign. The reference-frame bit cost is deliberately not included in the search's `m->cost`; the caller adds `REF_COST(list, ref)` after the search (except where a particular fast path needs it removed again).

Candidates are evaluated one at a time or in groups of three or four using SIMD-accelerated "compare against one source, three/four destinations" primitives; the bit cost is added per candidate afterwards. Ties are broken by a small integer tag encoded in the low bits of the running best cost so that the branchy `if (candidate < best)` test becomes a single plain comparison.

**Predictor handling (full-pel phase).**

- With `subme >= 3`, the MVP itself is clipped in quarter-pel units and compared at *sub-pel* precision using weighted/interpolated reference data (`get_ref`). Extra predictors are clipped to the tighter full-pel MV limits, and zero and duplicates of the MVP are discarded. All survivors are compared at sub-pel precision, and the tag-in-low-bits trick selects the cheapest. The winning sub-pel predictor is rounded to full-pel with `(v+2)>>2`; if that rounded point is not the same as the sub-pel winner it is evaluated once with the fast SAD path, otherwise its already-known cost is reused. The zero vector is always checked unless it equals the MVP, in which case its sub-pel cost is used directly.
- With `subme < 3`, the MVP is rounded to full-pel first and its distortion is compared **without** any MV bit cost. Source comments explain this deliberately favours the MVP: rounding to full-pel adds MV bits in nearly all cases, but the subsequent sub-pel search tends to return to the MVP, so charging the bits here would unfairly penalise it. Other predictors are rounded, clipped, de-duplicated and charged normally. Zero is evaluated when the MVP is non-zero.

### 5.2 Full-pel search methods

`h->mb.i_me_method` selects one of five methods. Common helpers: `CHECK_MVRANGE` checks both axes against the per-MB full-pel limits with one packed 32-bit comparison; a four-neighbour diamond, three-neighbour group and cross scan use grouped SIMD comparisons.

**Diamond (`X264_ME_DIA`, id 0).** Iterative radius-1 search. Each iteration evaluates the four orthogonal neighbours of the current point, selects the best with a direction tag, and moves one step in that direction. The loop stops when no neighbour improves, when the iteration count reaches `i_me_range`, or when the new point leaves the allowed range. Cost tag values 1, 3, 4 and 12 encode the four directions with different priorities, so ties resolve deterministically.

**Hexagon (`X264_ME_HEX`, id 1).** Two phases. First a radius-2 hexagon: six points at `(-2,0), (-1,+2), (+1,+2), (+2,0), (+1,-2), (-1,-2)` around the current point, evaluated in two groups of three. The winning direction index drives a "half hexagon" continuation that only tests the three *new* points adjacent to the chosen direction, rotating the direction through a six-entry modulo table; at most `(i_me_range>>1)-1` continuation steps are made, bounded by the MV range. Second, a 3x3 square refinement around the final point tests the four orthogonal and four diagonal neighbours in two groups of four and moves by the winning offset from a nine-entry square table (entry zero is the centre).

**Uneven-cross multi-hexagon (`X264_ME_UMH`, id 2).** The most elaborate method:

1. Diamond-refine the rounded MVP; if it is non-zero, also diamond-refine the zero vector.
2. For blocks larger than 4x4, remember that cost and diamond-refine the current best once more if it moved away from the MVP. If no improvement occurred, the subsequent cross starts at radius 3 instead of 1.
3. Early termination uses `bcost < threshold >> shift`, where `shift` is selected by block size from the table `{0,1,1,2,3,3,4}` for pixel classes 16x16 down to 4x4. First an eight-point ring around the current point is tested; if the cost is unchanged *and* the cost is below `500` scaled by block size, search ends. Otherwise a cross of radius `(i_me_range>>1)|1` is tested plus eight further diagonal points; if still unchanged, search ends. The cross starts at 3 when the earlier refinement did not help, and its later start is set to `range+2`.
4. Adaptive search range: when predictors exist, a disagreement measure is computed. For one predictor it is the Manhattan distance between MVP and that predictor, with a fixed value of 25 for 16x16 (where the predictor list largely duplicates the MVP). For multiple predictors it is the sum of pairwise Manhattan differences, plus the MVP-vs-first difference unless the block is 16x16, divided by `i_mvc-1` (plus one for the extra term). A "sad context" is chosen from thresholds 1000, 2000, 4000 (after block-size scaling) and an "mvd context" from thresholds 10, 20, 40 times the denominator. A 4x4 multiplier table (rows = mvd context, columns = sad context, values 3..6) scales `i_me_range` with `range * mul >> 2`.
5. Cross at the (possibly enlarged) range, a four-corner check at offset 2, then a multi-hexagon grid: 16 positions at offsets `{0,±4}`, `{±2,±3}`, `{±4,±1}`, `{±4,±2}`, `{±4,0}` scaled by ring index `i`, for `i = 1 .. i_me_range>>2`. When the ring fits inside the MV limits, four groups of four SIMD comparisons are used; otherwise each candidate is checked individually with a range test. The winning position becomes the new origin.
6. If the final vector is inside the legal range, finish with the same radius-2 hexagon plus square refinement used by method 1.

**Exhaustive / test exhaustive (`X264_ME_ESA` id 3, `X264_ME_TESA` id 4).** The search rectangle is the intersection of `best ± i_me_range` and the MV limits; its width is rounded up to a multiple of four. Instead of scanning every point, the encoder exploits that the sum of absolute differences is at least the absolute difference of the sums: an integral image supplies the DC of each candidate block, and the `ads` primitive returns the x-offsets whose DC bit cost passes a threshold (for TESA `best*17/16`, for ESA `best` itself) as a compact list in scratch memory. For each row, a vertical MV bit cost can discard the whole row before any pixel work.

- ESA: candidates from `ads` are evaluated directly with SAD/SATD plus MV bits.
- TESA: candidates from `ads` are first screened with SAD against a threshold of `best * sad_thresh / 8`, where `sad_thresh` is 10, 11 or 12 for `i_me_range <= 16`, `<= 24`, or larger. Survivors are collected with their SAD; if there are more than `2*(i_me_range>>1)`, the threshold is repeatedly tightened toward `best` until the list is small enough, then the worst entries are removed until at most `i_me_range>>1` remain. Only these finalists are evaluated with the full SATD/SAD comparison. This is why TESA is much faster than ESA at large ranges with only a small quality loss.

### 5.3 Sub-pel refinement (called from the full-pel search)

When `subme >= 2`, the full-pel winner is refined with iteration counts taken from the 12-row `subpel_iterations` table whose four columns are `{refine_hpel, refine_qpel, me_hpel, me_qpel}`:

| subme | hpel-refine | qpel-refine | hpel-in-search | qpel-in-search |
|---|---|---|---|---|
| 0 | 0 | 0 | 0 | 0 |
| 1 | 1 | 1 | 0 | 0 |
| 2 | 0 | 1 | 1 | 0 |
| 3 | 0 | 2 | 1 | 0 |
| 4 | 0 | 2 | 1 | 1 |
| 5 | 0 | 2 | 1 | 2 |
| 6–7 | 0 | 0 | 2 | 2 |
| 8–11 | 0 | 0 | 4 | 10 |

(Columns 2 and 3 are the counts used inside `x264_me_search_ref`; columns 0 and 1 are used by `x264_me_refine_qpel` on the winner.)

`refine_subpel` behaviour:

1. **Half-pel phase.** When half-pel iterations are requested, the sub-pel part of the MVP is tried first (clipped two quarter-pel units inside the limits) for `subme < 3`, using the SAD measure. Then a radius-1 diamond in *half-pel* steps is repeated up to the iteration count: four neighbours at `(0,±2)` and `(±2,0)` are fetched through `get_ref` (which performs interpolation and weighting) and compared with the full-precision cost table. A four-value tag ({2,6,16,48}) encodes which neighbour won; no improvement (tag zero) exits early. The search direction prioritisation is asymmetric, giving deterministic tie-breaking.
2. **Distortion measure switch.** Unless this is the "refine Qpel" call *and* the fast SATD and SAD primitives are identical *and* chroma ME is off, the current best is re-measured with SATD (plus optional chroma SATD) before quarter-pel work. In 4:4:4 the chroma planes are measured with the same block primitive; in subsampled formats the vertical chroma MV is adjusted with an interlaced field offset when the reference is not ref 0. Chroma is only added while the luma cost of the trial is already better than the current best — a cheap early-out.
3. **Multi-reference pruning.** If a shared half-pel threshold pointer was supplied, a candidate whose cost exceeds `7/8` of that threshold is abandoned immediately (its current vector/cost is written back first). A candidate better than the threshold lowers it. This is the main mechanism that makes many reference frames affordable.
4. **Quarter-pel phase.** Except for `subme == 1`, a radius-1 diamond in one-quarter-pel steps runs for the requested number of iterations, bounded by the sub-pel MV limits. Each candidate's SATD is compared with the best; the previous search direction's opposite neighbour is skipped when the same direction was used in the last step (the hexagon-like "don't repeat the centre" rule). Direction tags are updated in the low bits for the same reason as above. For `subme == 1`, a single SAD-based quarter-pel diamond is evaluated in one grouped comparison instead.
5. Final `m->cost`, `m->mv` and `m->cost_mv` are written; `cost_mv` is looked up at the final quarter-pel coordinates.

`x264_me_refine_qpel` calls this with `b_refine_qpel = 1`, which suppresses the re-measurement and keeps SAD from the hpel phase (the "refine the winner" case). `x264_me_refine_qpel_refdupe` forces zero half-pel iterations and caps quarter-pel iterations at two.

### 5.4 Bidirectional refinement

`me_refine_bidir` jointly adjusts `(mv0x,mv0y,mv1x,mv1y)`:

- It refuses to run unless both current MVs are at least 8 quarter-pel units away from every MV limit in both directions — this guarantees the 3x3 neighbourhood caches and 4D probes stay in bounds.
- Around each MV it caches nine interpolated reference blocks (the 3x3 square) for both lists, with chroma cached only for the RD variant.
- The offset set contains 33 entries: the centre, all single-axis ±1 offsets (8), and all offsets that change exactly two of the four components by ±1 (24). A byte-array "visited" bitmap of shape 8x8x8 (three coordinates explicit, the fourth a bit) prevents re-evaluating a 4D point across passes.
- For each pass, the two cached prediction blocks are averaged with the per-ref-pair biprediction weight, and the configured SATD comparator measures the residual against the source plus the four MV bit costs. In RD mode a candidate first has to beat `best_cost + best_cost/16` on SATD; then the true RD cost is obtained with `x264_rd_cost_part` on the corresponding 8x8 index.
- The best offset moves both MVs; a pass with no movement ends the loop (up to 8 passes). In RD mode the final MVs and clipped MVDs (`min(|mv-mvp|,33)`) are written into the MB cache so the subsequent encode sees them; `b_skip_mc` is set by the caller to avoid recomputing motion compensation.

### 5.5 RD quarter-pel refinement

`x264_me_refine_qpel_rd` refines a single partition using entropy-based costs:

- For partitions other than the first 16x16, the MVP is recomputed for that partition; the MV-bit table is re-based on it.
- The starting MV is measured with SATD; for 16x16 the incoming RD cost is reused instead of recomputing. The predicted MV is also measured (with a flag telling the search to skip revisiting it).
- A cheap SATD screen `satd <= best_satd + best_satd/16` gates the expensive `x264_rd_cost_part` call; chroma is included (or, for 4:4:4, all three planes) before the RD call.
- Search order: initial and predicted vectors, a radius-2 hex pattern (the same six offsets as `hex2`), a half-hexagon continuation of at most nine iterations with vertical range checks, and a final 3x3 square sweep (eight neighbours). Every accepted candidate updates the MV cache and a clipped MVD cache (`min(|delta|,66)`).
- `b_skip_mc` is set during the function and cleared on exit, including the early-return path at the MV boundary.

### 5.6 Interpolation boundary conditions

The sub-pel path obtains samples through `h->mc.get_ref` / `mc_luma` / `mc_chroma`: luma uses the H.264 6-tap half-pel filter with quarter-pel values formed by averaging adjacent half-pel samples; chroma uses a 2-tap filter at half the luma resolution. Weighted prediction is applied either in `get_ref` or by a separate weight function after chroma MC. These primitives live in `common/mc.c` and its SIMD variants; this file only selects coordinates and strides.

---

## 6. Dependencies

- **Called by:** `analyse.c` for every inter candidate (16x16, 16x8, 8x16, 8x8, 8x4, 4x8, 4x4) and for the refinement stages; `rdo.c` indirectly via `x264_rd_cost_part`.
- **Calls:** `h->pixf.fpelcmp[*]` (SAD/SATD), `fpelcmp_x3/x4`, `mbcmp`, `hadamard`-based SATD, `sad`, `ads` (successive elimination), `h->mc.get_ref`, `mc_luma`, `mc_chroma`, `avg`, `x264_predictor_clip`, `x264_predictor_roundclip`, `x264_predictor_difference` (all in `common/`), `x264_mb_predict_mv`, MB cache writers, `x264_rd_cost_part` (`rdo.c`).
- **Shared config:** `i_me_method`, `i_me_range`, `i_subpel_refine`, `b_chroma_me`, `b_weighted_pred`, `b_deterministic`, `i_mv_range`, `i_mv_range_thread`, MB type/neighbour caches, `h->cost_mv[qp]` and `h->cost_mv_fpel`.
- `analyse.c` includes `rdo.c` which in turn includes `cavlc.c` and `cabac.c`; hence the whole analysis/ME/RD code is one translation unit in the reference. The clean implementation may split it but must preserve the shared state semantics.

---

## 7. Constants and tables

- `COST_MAX = 1<<28`, `COST_MAX64 = 1<<60` — sentinel and saturation values.
- `subpel_iterations[12][4]` — reproduced as a table in §5.3; these are empirically chosen preset points, not derived from theory.
- `mod6m1[8] = {5,0,1,2,3,4,5,0}` — six-direction rotation for the half-hexagon; duplicated entries avoid a modulo.
- `hex2[8][2]` — six radius-2 hexagon offsets, with the first two duplicated so an index in `0..7` can be used without a second modulo.
- `square1[9][2]` — centre plus eight radius-1 square offsets in a fixed order used by both square refinements and by the bidirectional 3x3 MC cache.
- `pixel_size_shift[7] = {0,1,1,2,3,3,4}` — block-size-dependent right shift for UMH SAD thresholds.
- `range_mul[4][4] = {{3,3,4,4},{3,4,4,4},{4,4,4,5},{4,4,5,6}}` — adaptive search-range multipliers.
- `hex4[16][2]` — 16 hexagonal-grid offsets at radius 4 (two columns at x=±4 with y=−2..2, plus near-diagonal points at y=±3 and x=±2).
- `sad_thresh` for TESA: 10/11/12 by search range; ADS threshold multiplier 17/16.
- Bidirectional 4D offset set: 33 vectors (centre + 8 single-axis + 24 two-axis combinations); MVD clip 33; RD qpel MVD clip 66; `SATD_THRESH(cost) = cost + cost/16`.
- Four-candidate tie tags: DIA `{1,3,4,12}`, HALFPEL `{2,6,16,48}`; larger tags mean lower priority.
- `x264_iter_kludge` — intentionally non-constant loop bound.

---

## 8. Performance notes

This is the hottest file in a P/B frame: the full-pel search dominates at fast presets, while SATD-based sub-pel and RD refinement dominate at slow presets.

1. **Adaptive search pruning [1][2].** UMH's SAD thresholds, the multi-reference half-pel threshold (`7/8` rule) and TESA's successive elimination each avoid large numbers of pixel comparisons. The quality trade-off is that low-motion predictions can be accepted too early; the thresholds are chosen empirically and are the first thing to re-tune when porting to a different content mix. A clean implementation should keep the exact thresholds for equivalence and expose them as knobs only in a non-equivalence mode.
2. **Grouped SIMD comparisons.** Everything is arranged to compare one source block against three or four candidates in a single call; this is why the search patterns use fixed offsets and tie tags. SIMD versions of SATD/SAD/ADS exist for x86 (SSE2/SSSE3/AVX2), ARM (NEON) and others; a scalar fallback changes speed, not output (bit-exactness of SIMD `satd` vs scalar is an implementation assumption that must be checked — see §11).
3. **Early skip / predictor reuse [3].** The caller (`analyse.c`) can bypass the search entirely for P_SKIP/B_SKIP, and uses already-found 16x16 MVs as predictors for smaller blocks. Preserving the predictor list order matters for bit-exactness because tie-breaking is index-based.
4. **Lookahead reuse [4].** The MV-bit table and integral images are prepared once per frame/QP; recomputing them per block destroys performance. `cost_mv_fpel` tables exist only for ESA/TESA.
5. **Concurrency [5].** Searches are independent per MB and thread-safe provided the shared cost tables are immutable; however `mv_limit` depends on how many reference rows have completed, so the frame-threading semantics in `analyse.c` must be preserved for deterministic output.
6. **RD paths.** `x264_me_refine_qpel_rd` and bidirectional RD are far more expensive than SATD search but buy roughly the last fraction of a dB; they are gated by `mbrd >= 2`. Caching the MC result (`b_skip_mc`) avoids a duplicate pass through the transform/quant path.
7. **Branch behaviour.** Search loops are short and unpredictable; the tag-in-low-bits trick replaces "update three variables on improvement" with a single integer comparison plus a table lookup. Removing it changes tie-breaking and therefore bitstream output.

---

## 9. References

[1] ITU-T Rec. H.264 / ISO-IEC 14496-10, *Advanced video coding* — block matching, motion vector prediction, quarter-pel interpolation.

[2] Y.-C. Chen and C.-Y. Tsai, "Fast block matching algorithm based on winner-update search," and Chen et al., "UMHexagonS" contributions to JM reference software (the source notes UMH follows JM except for early termination).

[3] x264 project documentation and developer notes on fast P-skip and early termination (codex.wordpress.com, x264 mailing lists).

[4] L. Merritt, "Successive elimination motion search" discussions, x264 developer list; standard successive-elimination inequality `sum|x-y| >= |sum x - sum y|`.

[5] x264 frame-threading design notes (`x264_frame_cond_wait`, `i_mv_range_thread`); public x264 source documentation on deterministic multithreading.

All numeric thresholds and table values above are observations from the reference tree; their optimality is empirical and not established by the cited literature.

---

## 10. Verification checklist

For a fixed QP, block size, reference frame, MVP, predictor list and configuration, an independent implementation should match the reference on:

1. The winning full-pel MV and `cost` for each of the five search methods, including tie cases (equal-cost neighbours and the zero vector).
2. The effect of `i_me_range` on the number of search steps and on clipping at the MB image border.
3. Sub-pel MV and `cost_mv` for each `subme` value 0–11, especially the `subme=1` special path and the `subme=8..11` iteration counts.
4. Multi-reference pruning: whether `p_fullpel_thresh` causes an early return, and the resulting MV for a deliberately poor second reference.
5. UMH early termination at the 2000/500 thresholds for different block sizes, and the adaptive-range multiplier chosen for a given predictor disagreement.
6. TESA finalist count and ordering for a synthetic block where ADS and SATD disagree; ESA and TESA should agree on the global best for small search ranges.
7. Bidirectional refinement: the 4D offset chosen from an artificial cost field, the visited-bitmap behaviour across passes, and the boundary guard at MV limits.
8. RD qpel refinement: the SATD pre-filter decision at `satd == best + best/16`, and the MVD cache values clipped at 66.
9. Weighted prediction: costs with and without `weightfn` must match the corresponding `get_ref`/`mc_chroma` results.
10. Saturation: artificially huge distortion must clamp at `COST_MAX`/`COST_MAX64` without wraparound.

---

## 11. Open questions

- **UNKNOWN:** whether SIMD `satd` and the scalar C reference are bit-identical on every supported architecture; the reference assumes it, but no formal equivalence proof was found in the tree.
- **UNKNOWN:** exact rationale and tuning history for the individual tie-break tag values (1/3/4/12, 2/6/16/48) — only the effect (priority order) is evident.
- **UNKNOWN:** the precise derivation of `range_mul` values; source comments state they came from "casual inspection" and were not tuned by encoding tests.
- **UNKNOWN:** why `i_fpel_border` is 6 (comment mentions 1 for diamond, 2 for octagon, 2 for hpel) — the safety margin for UMH's largest cross is not fully documented.
- **UNKNOWN:** behaviour of `x264_iter_kludge` on compilers that constant-fold the loop despite the external symbol; output should be unaffected, but timing may change.
- **UNKNOWN:** exact semantics of biprediction weight derivation (`64 - dist_scale_factor>>2`) as it is computed outside these files; the value is consumed here as an opaque 0..64 weight.
