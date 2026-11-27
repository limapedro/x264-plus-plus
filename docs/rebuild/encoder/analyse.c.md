# `encoder/analyse.c` — macroblock mode-decision pipeline

**Role (one sentence):** This file decides, for every macroblock, the optimal prediction mode, partition, reference frames, motion vectors, intra directions, transform size and QP by comparing SATD/SSD estimates and, at high settings, true RD costs.

> **Dirty-side behavioural specification.** Written from the reference source and public literature for the clean-room reimplementation. No source code or near-verbatim pseudocode is reproduced; only behaviour, structure, constants and decision conditions are described.

---

## 1. Header

- Source path: `encoder/analyse.c` (about 3895 lines, including an `#include "slicetype.c"` at the end).
- Role: the "analysis" half of the encoder: cost-table initialisation, weighted-reference preparation, per-MB QP/lambda/MV-limit setup, all intra and inter candidate searches, RD comparisons, transform and QP-RD decisions, and final population of the MB caches.
- This is the largest and most configuration-sensitive file in the encoder.

---

## 2. Purpose

An H.264 macroblock can be coded in roughly 20 structural modes (I16x16/I8x8/I4x4/PCM, P16x16/P16x8/P8x16/P8x8 with 4 sub-partition shapes, B direct/L0/L1/Bi/8x8 with sub-partitions, plus skip variants) and each mode has multiple reference frames and intra directions. Exhaustively coding every combination is far too expensive, so this file implements a cascade:

1. Cheap SATD/SAD screening of all plausible modes with empirically tuned shortcuts.
2. Full-pel and sub-pel motion search for each surviving inter partition.
3. At higher settings, re-evaluate the survivors with true `D + lambda*R` by encoding them.
4. Optionally refine intra directions, sub-pel vectors and QP with local searches.
5. Write the winning choice into the MB caches for the encoder.

The same file also hosts the frame-type/slicetype code (via the trailing include), so it owns most of the encoder's notion of "cost".

---

## 3. Public interface

Declared in `analyse.h` (see `analyse.h.md`):

| Function | Contract |
|---|---|
| `int x264_analyse_init_costs(x264_t*)` | Allocates per-QP MV bit-cost tables and reference-index cost tables; allocates full-pel variants for ESA/TESA. Called once after parameter validation. |
| `void x264_analyse_free_costs(x264_t*)` | Frees all per-QP tables, adjusting pointers back to their allocation bases. |
| `void x264_analyse_weight_frame(h, end)` | Computes weighted-reference planes incrementally up to the given frame row, used when weighted prediction is enabled and frame threading limits how much of the reference is complete. |
| `void x264_macroblock_analyse(h)` | The main entry point: chooses the MB mode and leaves `h->mb` ready for `x264_macroblock_encode`. |
| `void x264_slicetype_decide(h)`, `void x264_slicetype_analyse(h, int intra_minigop)` | Frame-type decision (implemented in the included `slicetype.c`). |
| Lookahead functions | `x264_lookahead_init/is_empty/put_frame/get_frames/delete`. |

Internal data types: `x264_mb_analysis_list_t` (per-list search results) and `x264_mb_analysis_t` (all analysis state for one MB), described in §4.

---

## 4. State and data structures

### 4.1 `x264_mb_analysis_list_t`

Contains one `x264_me_t` per candidate partition:

- `me16x16`, `bi16x16` (separate because Bi MVs can differ from both L0 and L1 results);
- `me8x8[4]`, `me4x4[4][4]`, `me8x4[4][2]`, `me4x8[4][2]`, `me16x8[2]`, `me8x16[2]`;
- accumulated costs `i_rd16x16`, `i_cost8x8`, `i_cost4x4[4]`, `i_cost8x4[4]`, `i_cost4x8[4]`, `i_cost16x8`, `i_cost8x16`;
- `mvc[32][6][2]` (aligned): MV predictors indexed by reference and slot. Slot 0 is the 16x16 MV, slots 1–4 are the four 8x8 MVs of that partition, slot 5 is padding for SIMD alignment.

### 4.2 `x264_mb_analysis_t`

Groups the analysis context:

- **Cost setup:** `i_lambda`, `i_lambda2`, `i_qp`, `p_cost_mv`, `p_cost_ref[2]`, `i_mbrd` (0 SATD-only, 1 RD decision, 2 RD refinement, 3 QPRD).
- **Flags:** `b_fast_intra`, `b_force_intra` (periodic intra refresh), `b_avoid_topright`, `b_try_skip`, `b_direct_available`, `b_early_terminate`.
- **Intra results:** SATD costs and per-direction costs for 16x16/8x8/4x4/chroma, chosen predictor indices, PCM cost, `i_cbp_i8x8_luma`.
- **Inter results:** both lists' structures, costs for Bi/Direct/16x8/8x16/8x8 combinations, per-8x8 SATD values `i_satd8x8[3][4]` (L0/L1/Bi), estimated costs `i_cost_est16x8[2]`, `i_cost_est8x16[2]`, selected sub-partition/MB-type arrays for 16x8 and 8x16.
- **Lifetime:** a stack-local instance in `x264_macroblock_analyse`; borrowed `x264_me_t` pointers are copied wholesale into this structure and later replayed through `analyse_update_cache`.

### 4.3 Cost tables (module-level constants)

- `i_mb_b_cost_table[19]` indexed by internal MB type `{I_4x4,I_8x8,I_16x16,I_PCM,P_L0,P_8x8,P_SKIP,B_DIRECT,B_L0_L0,B_L0_L1,B_L0_BI,B_L1_L0,B_L1_L1,B_L1_BI,B_BI_L0,B_BI_L1,B_BI_BI,B_8x8,B_SKIP}` = `{9,9,9,9,0,0,0,1,3,7,7,7,3,7,7,7,5,9,0}` (units of lambda).
- `i_mb_b16x8_cost_table[17]` for B 16x8/8x16 MB types: `{0,0,0,0,0,0,0,0,5,7,7,7,5,7,9,9,9}`; the MB type is derived from two per-half partition choices with `B_L0_L0 + (part0>>2)*3 + (part1>>2)`.
- `i_sub_mb_b_cost_table[13]` for B sub-partitions: `{7,5,5,3,7,5,7,3,7,7,7,5,1}` indexed by D_L0_4x4 … D_DIRECT_8x8.
- `i_sub_mb_p_cost_table[4]` for P sub-partitions: `{5,3,3,1}`.
- Mode-availability tables: for 16x16 and chroma, five rows keyed by neighbor availability (none → all) listing DC128, DC-left/H or DC-top/V, DC, then Plane/P; for 8x8 and 4x4, two variants (normal and "avoid top-right" for periodic intra refresh) × five neighbor rows × up to nine directions. The tables encode which H.264 prediction modes are legal given unavailable neighbours, without runtime checks.

---

## 5. Algorithm / behaviour

### 5.1 Cost table initialisation and weighted frames

`init_costs` for each QP (from `qp_min` to `qp_max`, plus the lookahead QP):

- Allocates `cost_mv[qp]` covering `±2*4*mv_range` quarter-pel positions plus margin, and re-bases the pointer to the middle so negative indices work.
- Entry for displacement `i` is `min(round(lambda * log2(i+1)*2 + 1.718), 65535)`; entry zero is `round(lambda * 0.718)`. Here `lambda = x264_lambda_tab[qp] ≈ 2^(qp/6-2)`. Factor 4 accounts for qpel units, 2 for sign, 2 for opposite-direction movement.
- Reference-index cost: for `i` in 1..2 (used when 2 or 3 active references), `lambda * bs_size_te(i, j)` where `j = active_refs-1` clipped to 0..2; ref 0 costs zero.
- For ESA/TESA, builds four full-pel tables `cost_mv_fpel[qp][phase]` from the qpel table at `i*4 + phase`.
- Sets `cost_i4x4_mode[qp][i] = 3*lambda*(i != 8)` for 17 entries: the predicted intra mode gets a three-bit discount, matching the actual cabac/cavlc cost saving.

`x264_analyse_weight_frame` walks the reference list, finds the first reference with an active `weightfn`, and incrementally generates weighted copies (`h->fenc->weighted[k]`) for all references sharing the characteristic, for `height` rows up to the current encoding position. The counter `i_lines_weighted` makes this safe with frame threading.

### 5.2 Per-MB initialisation

`x264_macroblock_analyse` starts with `x264_ratecontrol_mb_qp(h)`. With AQ enabled and subme < 10, a QP within one step of the previous MB's QP is replaced by that previous QP, saving qp_delta bits. The QP is stored in `fdec->effective_qp` when MB info is enabled.

`mb_analyse_init` derives:

- `subme = i_subpel_refine - (B slice)`. `i_mbrd = (subme>=6) + (subme>=8) + (subpel_refine>=10)` → values 0..3. `b_deblock_rdo` is on for subme >= 9 unless deblocking is disabled. `b_early_terminate` is true for subme < 11.
- Lambdas: `i_lambda = x264_lambda_tab[qp]`, `i_lambda2 = x264_lambda2_tab[qp]` where `lambda2 ≈ 0.9*lambda^2*256` capped at 134217727. Trellis lambdas come from `trellis_lambda2_tab[intra][qp]` (inter factor 0.85², intra 0.65², with `LAMBDA_BITS=4`). Psy-RD lambda equals `i_lambda`. Chroma RD lambda offset index is `min(qp - effective_chroma_qp + 12, MAX_CHROMA_LAMBDA_OFFSET)`; the offset is 256 with psy disabled.
- Out-of-spec QPs (>51) switch to a precomputed "emergency" noise-reduction table and clamp the working QP to 51 for lambda purposes.
- PCM cost = `((X264_PCM_COST * lambda2 + 128) >> 8)` where `X264_PCM_COST = 384*bitdepth + 16` bytes; it is only considered when not AVC-Intra, psy-RD is off, `i_mbrd` is on and the cost fits in the sentinel, otherwise set to `COST_MAX`.
- `i_skip_intra`: 0 in lossless mode; 2 when `i_mbrd` is nonzero; otherwise 1 when trellis and noise reduction are both off, else 0.

**MV limits** (non-intra slices): horizontal limits are `4*(-16*mb_x - 24)` and `4*(16*(width-mb_x-1)+24)` in qpel units, then clipped to `±4*i_mv_range`; vertical limits use the same formula with `mb_y`/height and are additionally clipped to `4*i_mv_range_thread` when frame threading is active. The full-pel search limits are the qpel limits shifted right by 2 plus/minus a border of 6 full-pel pixels (1 for diamond, 2 for octagon, 2 for half-pel), because search patterns can overshoot. In MBAFF, three sets of vertical row limits (top field, bottom field, interlaced) are computed when the first MB of a row is reached and selected per MB.

**Fast-intra heuristic:** after the first four MBs of a slice, intra analysis is marked "fast" unless a spatial neighbour (left/top/top-left/top-right) is intra, the reference MB at the same position is intra in P slices, or the count of intra MBs so far exceeds one third of the MB index within the slice. Periodic intra refresh forces full intra analysis and disables top-right prediction on the refresh column's right edge.

All candidate costs are initialised to `COST_MAX`/`INT_MAX`.

### 5.3 Intra SATD analysis

`mb_analyse_intra` receives an inter SATD threshold (or `COST_MAX` in I slices). All costs include `lambda * bits` for the mode index, using the Exp-Golomb size of the mode value; B slices add `lambda * i_mb_b_cost_table[I_*]`.

**16x16:** available modes are selected from the neighbour table; DC/H/V are evaluated in one grouped SIMD call (`intra_mbcmp_x3_16x16`), with the mode bit cost added. Plane prediction is only tried if the best of those costs is within `i16x16_thresh`, which in fast-intra mode is `(i16x16_thresh_lut[subme] * inter_cost) >> 1` with LUT `{2,2,2,3,3,4,4,4,4,4,4}`, otherwise infinite. If the MB is lossless or the plane mode is unavailable, all available modes are evaluated one by one. If the resulting best exceeds the threshold, the rest of intra analysis is skipped entirely.

**8x8:** enabled by `X264_ANALYSE_I8x8`. Base cost `4*lambda` plus B-slice MB type cost. For each of four 8x8 blocks: filter edges for all neighbours, then either use the nine-mode SIMD comparator (no shortcuts when available), or evaluate DC/H/V in a grouped call, decide whether vertical or horizontal directions are favoured from their SATDs, discount the predicted mode by `3*lambda`, and then evaluate only the direction subset in `intra_analysis_shortcut[avoid_topright][has_nine_mode][favor_vertical]` when `mbrd < 1 + fast_intra`; otherwise evaluate directions 3..8. Each examined mode's stored cost is `SATD + 4*lambda` (or `SATD - 3*lambda + 4*lambda` when it equals the predicted mode). After each block the encoder immediately encodes it with the chosen mode so subsequent blocks see correct reconstructed neighbours; partial runs extrapolate the cost with `{1024,512,341}/256` and set the total to `COST_MAX`. Early exit uses `min(cost, i16x16_cost) > (inter*LUT[subpel_refine])>>2` with LUT `{4,4,4,5,5,5,6,6,6,6,6}`. Successful full runs save the reconstruction, NNZ, CBP and (in RD mode) DCT buffers for reuse.

**4x4:** base cost `40*lambda` (24 from JVT SATD0 plus 16 base prediction bits), plus B-slice cost. Threshold is the minimum of inter, 16x16 and 8x8 costs when early termination is on; in RD mode it is widened by `(10 - fast_intra)/8`. Per block, DC/H/V are grouped, the predicted mode discounted by 3 lambda, and direction shortcuts are used similarly; evaluation stops early when a discounted mode drives the SATD to zero. Each selected mode is encoded immediately to feed the next block's neighbours. Full completion stores the SATD and reuse buffers; otherwise the 4x4 cost is set to `COST_MAX`.

**Chroma:** at most four or five modes (DC128/DC-left/DC-top/H/V/P depending on neighbours). When all neighbours exist and not lossless, three modes are measured in one SIMD call, plane-like prediction for the fourth; otherwise predictions are generated one by one. Cost is the sum of U and V SATDs plus `lambda * bs_size_ue(fixed_mode_index)`. In 4:4:4, chroma is approximated using the luma 16x16 prediction (or zero cost when chroma ME is off) rather than a full analysis.

### 5.4 Intra RD evaluation and refinement

`intra_rd` re-evaluates each surviving intra type (16x16, 4x4, 8x8) through `rd_cost_mb`, replacing the SATD cost with true RD. Types above the threshold are set to `COST_MAX`.

`intra_rd_refine` (mbrd >= 2):

- For 16x16: retry all allowed directions whose earlier SATD cost is at most `9/8` of the chosen direction's SATD.
- For chroma: retry modes whose SATD is at most `5/4` of the best; if a zero-residual mode has been found, skip DCT work for the remaining modes. Chroma lambda uses the chroma QP.
- For 4x4: for each of 16 blocks, try every legal direction with `rd_cost_i4x4`, saving the best reconstruction pixels and NNZ and restoring them afterwards so subsequent blocks see the winner.
- For 8x8: for each of four blocks, try directions whose SATD is at most `11/8` of the chosen one, using `rd_cost_i8x8` with precomputed edges, again saving/restoring border pixels and NNZ.

### 5.5 P-slice analysis

**Fast skip.** Before any search, P_SKIP may be decided by:

- MB-info constant-block shortcut: if the reference block is marked constant, the frame distance is one, no weighted prediction, and the reference's effective QP <= current QP, then either P_SKIP (zero P-skip MV) or a forced zero-MV 16x16 prediction is used.
- Off-frame MBs (bottom rows beyond the coded height) are skipped.
- With `b_fast_pskip` and subme >= 3, `b_try_skip` is enabled: the decision is deferred until after the 16x16 search. At lower subme, if any neighbour is P_SKIP, `x264_macroblock_probe_pskip` is called immediately and can short-circuit everything.

**16x16 search.** For each reference frame (including duplicate references):

- Set luma/chroma pointers, weighted plane, and MVP from `x264_mb_predict_mv_16x16`.
- If this reference is a known blind duplicate, reuse the previous MV and call the duplicate-refinement shortcut; otherwise call `x264_me_search_ref` with the full predictor list from `x264_mb_predict_mv_ref16x16` and an optional shared half-pel threshold (enabled when early termination is on and more than one reference exists).
- Add the reference bit cost and keep the cheapest.
- **Early P_SKIP:** on reference 0, if `b_try_skip`, the SATD part of the cost is below `300*lambda`, the MV differs from the P-skip MV by at most one quarter-pel unit in Manhattan distance, and `x264_macroblock_probe_pskip` succeeds, the MB becomes P_SKIP and the function returns.
- Save the per-reference 16x16 MV for future prediction.

**8x8 partitions.** With `X264_ANALYSE_PSUB16x16`:

- Mixed-reference mode: evaluate references from 0 up to an `i_maxref` that is trimmed to neighbour references when 16x16 chose ref 0 and both top and left neighbours are inter; duplicate references are still visited. Each 8x8 block searches each reference using up to five predictors (the 16x16 MV plus earlier 8x8 MVs). Add ref cost and a partition bit cost (`i_sub_mb_p_cost_table[D_L0_8x8] = 1`), except with CABAC when sub-8x8 analysis is disabled, where the partition cost is effectively zero. For non-CABAC cases where all four blocks use reference 0, four ref costs are refunded.
- Non-mixed mode: use only the 16x16 winner's reference, the 16x16 MV as the sole predictor, and add ref/partition costs with a CABAC correction (`i_cost8x8 -= ref_cost` because four ref flags are not really written).

**Sub-8x8.** For each 8x8 block, when its 4x4 cost beats its 8x8 cost plus two MVD costs:

- 4x4: search each of four 4x4 blocks with the 8x8 MV as predictor; cost = sum of four ME costs + ref cost + `5*lambda`.
- 8x4 and 4x8: two blocks each, using the first 4x4 MV as the predictor; costs add ref cost + `3*lambda`.
- Choose the cheapest sub-partition, cache its MVs.

**16x8 / 8x16.** Only attempted if the 8x8 cost beats the 16x16 cost plus two MVD costs from the relevant 8x8 blocks. Candidate references are the (min,max) of the two underlying 8x8 references (one or two trials), and three predictors (16x16 MV and two 8x8 MVs) are used. Early termination after the first half uses `cost0 + estimated_cost1 > best_so_far * (4 + mbrd) / 4`. Estimates are built from the 8x8 SATD values plus the average MV/ref bit cost of the other two blocks.

**Qpel refinement (non-RD).** When `mbrd == 0` and subme > 0, refine only the winning partition's MVs with `x264_me_refine_qpel` and recompute the cost. With `mbrd` set, refinement happens later via the RD path.

**Intra comparison.** Chroma-intra analysis is run first when chroma ME is enabled (subtracting chroma SATD from the inter threshold so luma intra is compared fairly); otherwise luma intra is analysed directly. The best of P_L0/P_8x8/I_16x16/I_8x8/I_4x4/I_PCM is selected by strict `<` comparisons with fixed priority.

**RD stage (mbrd > 0).** `mb_analyse_p_rd` re-encodes survivors with `rd_cost_mb`, using a threshold `inter_satd * 5/4 + 1`:

- 16x16 is re-evaluated if its SATD cost is at most `3/2` of the best (or when early termination is off);
- 16x8, 8x16, 8x8 are re-evaluated only below the threshold;
- for 8x8 with sub8x8 enabled, each 8x8 cell re-evaluates its four sub-partition shapes with `x264_rd_cost_part` under a threshold of `5/4` of the cheapest shape, then caches the winner.
- After RD, transform-size RD and intra RD (`threshold = inter_RD * 5/4 + 1`) are run.

At `mbrd >= 2`, each partition's MVs are refined with `x264_me_refine_qpel_rd`; at `mbrd == 3`, `mb_analyse_qp_rd` runs (§5.8).

### 5.6 B-slice analysis

**Direct mode.** `x264_mb_predict_mv_direct16x16` supplies Direct MVs (spatial/temporal/auto). With `direct=auto`, both spatial and temporal predictions are tried and the one producing more skips is selected by a frame-level score. Direct MC is performed into `p_fdec`.

**B_SKIP detection:**

- Off-frame MBs skip immediately.
- With mbrd, the SSD of the Direct prediction is compared with `6*lambda2/256` (6 bits being the minimum CAVLC cost of a non-skipped MB); on success `b_skip_mc` is set.
- Without mbrd and without auto-direct, `x264_macroblock_probe_bskip` is used; at subme < 3 it immediately decides skip.

**16x16 B search.** The loop is deliberately ordered: search list 1 ref 0, list 0 ref 0, try skip, finish list 0, then finish list 1. Fast skip requires both lists' best MVs to be within one quarter-pel unit of the Direct MVs. Costs include ref bits. The Bi candidate averages the two best predictions with the per-ref-pair weight and adds both ref and MV costs; an all-zero MV pair is always tested explicitly (it helps in fades), with its own ref/MV costs and optional chroma. MB-type costs from `i_mb_b_cost_table` are added at the end (Direct 1, L0/L1/Bi 3 or 5 as tabulated).

**8x8 B.** With mixed references, each list searches all refs up to a neighbour-trimmed maximum and considers L0, L1, Bi and Direct-8x8 per 8x8 cell; otherwise only the 16x16 winner's reference is used with one predictor. The per-partition costs add `i_sub_mb_b_cost_table` entries and optional chroma Bi SATD. The cheapest of the four types per cell is cached.

**16x8 / 8x16 B.** For each half, candidate refs come from the two underlying 8x8 results; L0, L1 and Bi are compared, Bi getting a `1*lambda` bias. A whole-MB type is assembled from the two halves via the 17-entry B16x8 cost table. Estimates for ordering the two partition orientations are computed from 8x8 SATD values plus average MV/ref costs; the cheaper orientation is tried first.

**Qpel refinement (non-RD).** Only the MVs actually used by the chosen partition are refined (Bi refines both lists).

**RD stage.** `mb_analyse_b_rd` uses threshold `inter_satd * (17 + psy_rd) / 16 + 1`, and re-evaluates Direct, L0, L1, Bi, 8x8, 16x8 and 8x16 through `rd_cost_mb`. An early Direct check at `direct_cost <= best*33/32` can trigger RD before the sub-partitions are searched, followed by a B_SKIP re-check against all RD costs. At `mbrd >= 2`, `x264_me_refine_qpel_rd` / `x264_me_refine_bidir_rd` refine the final vectors; at subme >= 5 a SATD-only bidirectional refinement (`refine_bidir`) runs for all Bi partitions.

### 5.7 Transform-size decision

Without RD, `mb_analyse_transform` performs full MC and compares SA8D (8x8 transform residual cost) with SATD (4x4 transform residual cost) over the luma plane (all three in 4:4:4 when chroma ME is on), setting `b_transform_8x8` to whichever is smaller and setting `b_skip_mc` so the final encode reuses the MC result.

With RD, `mb_analyse_transform_rd` flips the transform flag, temporarily forces P_8x8 sub-partitions to 8x8 (so 8x8dct is allowed), re-evaluates `rd_cost_mb`, and keeps the flip only if the RD cost improves; when the cost improves it also rescales the SATD value by `new_rd / old_rd` so later thresholds remain comparable.

### 5.8 QP-RD

At mbrd == 3, `mb_analyse_qp_rd` searches the QP dimension:

- Baseline is the current MB's RD cost and CBP. If CBP is zero, only lower QPs are tried; otherwise higher first, then lower.
- The failure tolerance is 0 without psy-RD and 1 with psy-RD, incremented when moving toward the previous MB's QP (to allow the non-monotonicity psy introduces).
- When the original CBP is zero, it jumps ahead to `qp - threshold - 1` (bounded by `qp_min`) and tests that QP first; if still empty, the whole downward direction is skipped.
- Each candidate QP updates the chroma QP via `chroma_qp_table` and calls `rd_cost_mb`; the best strict improvement wins. Ties count as failures. Stop when failures exceed tolerance or when a higher QP produces zero CBP.
- The previous MB's QP is always tried explicitly. After choosing a QP, the 8x8-transform decision is checked again.

### 5.9 Final cache update

`analyse_update_cache` writes the chosen decision into `h->mb`:

- Intra: all 16 (or 4) intra prediction modes, the 16x16 mode, or PCM (no writes), plus chroma prediction selection.
- P: reference indices and MVs for 16x16/16x8/8x16/8x8 and the sub-partition MVs; P_SKIP uses ref 0 and the P-skip MV.
- B: direct MVs for skip/direct; B_8x8 caches all four cells; other B types write ref/MV for used lists and explicitly mark unused lists with ref −1, MV 0 and MVD 0.
- In debug builds, a multi-thread safety check verifies that MVs do not reach beyond the rows completed in the reference frame; if they do, the MB is forcibly replaced by I_16x16.

Afterwards `x264_macroblock_analyse` may collapse a partition back to 16x16 when qpel-RD made all four 16x8/8x16 sub-MVs identical with identical references, runs the non-RD transform decision when mbrd == 0, runs QPRD when mbrd == 3, and finally sets `b_trellis` to the configured trellis value, enables noise reduction for out-of-spec QPs or inter MBs with configured NR, and initialises psy-trellis source DCTs when needed.

---

## 6. Dependencies

- **Calls:** `x264_me_search_ref`, `x264_me_refine_qpel`, `_refdupe`, `_qpel_rd`, `_bidir_satd`, `_bidir_rd` (`me.c`); `rd_cost_mb`, `x264_rd_cost_part`, `ssd_mb`, `rd_cost_i4x4/i8x8/chroma` (`rdo.c`); `x264_macroblock_encode`, `x264_macroblock_encode_p8x8/p4x4`, probe functions (`macroblock.c`); ratecontrol `x264_ratecontrol_mb_qp`; prediction/MV helpers `x264_mb_predict_mv*`, `x264_macroblock_cache_*`, `x264_mb_mc`; SIMD `h->pixf.*`; frame threading `x264_frame_cond_wait`.
- **Called by:** `encoder.c` for every MB; `slicetype.c` (included at the end) uses the same analysis structures for lookahead cost estimation.
- **Global config:** every field of `param.analyse` (intra/inter flags, subme, me method/range, trellis, psy, AQ, mixed refs, weighted bipred, MB info, NR, fast pskip), slice type, QP bounds, MB info and thread count.

---

## 7. Constants and tables

- Lambda tables (defined in `common/tables.c`): `lambda ≈ 2^(qp/6-2)`; `lambda2 ≈ 0.9*lambda²*256` clamped to 134217727; trellis lambdas with 0.85 (inter) and 0.65 (intra) factors; chroma lambda offset table with 16..65535 entries.
- Mode bit-cost tables listed in §4.3.
- Intra fast thresholds: `i16x16_thresh_lut = {2,2,2,3,3,4,4,4,4,4,4}`, `i8x8_thresh = {4,4,4,5,5,5,6,6,6,6,6}`, partial-cost divisors `{1024,512,341}/256`, base costs `4*lambda` (8x8) and `40*lambda` (4x4), predicted-mode discount `3*lambda`, stored mode cost `+4*lambda`.
- P early exits: P_SKIP SATD < `300*lambda` with `|mv - pskip_mv|_1 <= 1`; 16x8/8x16 pre-checks with `2*cost_mv`; 16x8 early `best*(4+mbrd)/4`; sub8x8 `5/4`.
- RD thresholds: P `5/4 + 1`, P-16x16 `3/2`, B `(17 + psy)/16 + 1`, B-Direct early `33/32`, intra RD refinement `9/8` (16x16), `5/4` (chroma), `11/8` (8x8).
- B_SKIP RD threshold `6*lambda2/256`.
- MV range: `±(16*distance + 24)*4` qpel, clipped by `4*i_mv_range`, `4*i_mv_range_thread` vertically; full-pel border 6.
- PCM cost `384*bitdepth + 16` bits times lambda2.

---

## 8. Performance notes

This file's cost is dominated by the number of motion searches and the number of full RD encodes. The highest-value opportunities are:

1. **Adaptive search pruning [1][2].** The cascade of `COST_MAX` initialisation, neighbour-trimmed mixed-ref ranges, UMH SAD thresholds in `me.c`, the `7/8` multi-ref half-pel threshold and the `(4+mbrd)/4`, `5/4`, `17/16` RD pre-checks eliminates most candidates. A clean implementation should preserve the *order* of checks, not just their results, because early exits leave side effects (cached MVs, `i_skip_intra` buffers). Quality trade-off: more aggressive pruning loses small amounts of PSNR/SSIM on high-motion content; thresholds are empirically tuned and should be kept fixed for equivalence.
2. **Early P_SKIP / B_SKIP detection [3].** The `300*lambda` + one-quarter-pel check, MB-info constant-block shortcut, and the 6-bit SSD threshold avoid all searches for static blocks — frequently the majority of a frame. Mis-tuned thresholds cause visible blocking in low-motion areas because skip blocks inherit the predictor MV.
3. **SIMD SATD and intra comparisons [4].** Grouped `intra_mbcmp_x3/x9`, `sa8d_satd` and SATD/SSD functions make the intra cascade affordable. The direction-shortcut tables (`intra_analysis_shortcut`) matter only when the grouped primitives are unavailable; a clean implementation must decide which path it emulates because the two paths can choose different modes on ties.
4. **Lookahead reuse [5].** `cost_mv`, `cost_mv_fpel` and reference cost tables are computed once per QP; lookahead (`slicetype.c`, included here) reuses low-resolution SATD estimates to choose frame types. Recomputing these per MB is catastrophic; sharing stale values after a QP change is bitstream-changing.
5. **Concurrency [6].** Frame threading must respect `i_mv_range_thread` and the `x264_frame_cond_wait` logic: MV limits may be tightened to rows already completed in the reference. Disabling this makes output non-deterministic and can read uninitialised reference rows; making it overly conservative costs quality. `b_deterministic` forces a fixed limit.
6. **Intra block reuse.** `i_skip_intra` and the saved fdec/NNZ/DCT buffers avoid re-encoding 15 of 16 intra blocks. The `i_skip_intra == 2` DCT restore is essential in RD mode; omitting it changes CABAC contexts and therefore the bitstream.
7. **QP-RD and psy.** QP-RD is a local search over QP with a small failure tolerance; psy-RD widens the tolerance because its RD curves are noisier. Disabling psy-RD changes both the QP search path and the cost comparisons — a quality/speed trade-off that is not equivalent.

---

## 9. References

[1] ITU-T Rec. H.264 / ISO-IEC 14496-10 — macroblock types, partitions, intra predictions, CBP and reference indices.

[2] Public descriptions of x264's mode-decision cascade and early terminations (x264 developer log, codex.wordpress.com "x264" articles, mailing-list archives).

[3] Fast-skip and static-block detection literature; x264 MB-info constant-block mechanism.

[4] SIMD SATD/Hadamard implementations in `common/x86/`, `common/arm/`; "intra_mbcmp_x9" grouped comparison design notes.

[5] Lookahead/slicetype design in x264 (included `slicetype.c`), low-resolution SATD cost propagation (`mbtree`).

[6] x264 frame-threading and deterministic-mode documentation (`x264_frame_cond_wait`, `i_mv_range_thread`).

[7] Psy-RD and psy-trellis: x264 developer documentation on complexity preservation (see `rdo.c.md`).

---

## 10. Verification checklist

1. For a fixed frame/QP/configuration, the chosen MB type, partition, reference indices, MVs, intra modes and transform flag match the reference bit-for-bit.
2. `x264_analyse_init_costs` produces identical `cost_mv` values for every QP and every displacement, including negative indices and the 65535 clamp.
3. Reference costs match `lambda * bs_size_te(ref-1, active_refs-1)`; ref 0 cost is zero.
4. MV limits at frame borders, slice edges and with frame threading match the formulas, including the ±6 full-pel border.
5. Intra mode availability for all 16 neighbor combinations (including periodic-intra-refresh right-edge restriction).
6. Fast-intra: verify the exact set of MB indices where `b_fast_intra` becomes 1.
7. Early P_SKIP fires only with SATD < 300*lambda, Manhattan MV distance <= 1 and a successful probe; verify boundary case exactly equal to 300*lambda.
8. B_SKIP RD threshold exactly `6*lambda2 >> 8` and Direct early threshold `33/32`.
9. RD re-evaluation sets costs to `COST_MAX` for modes above thresholds; verify the priority order on exact ties between I/P/B modes.
10. `analyse_update_cache` marks unused B lists with ref −1, MV 0 and MVD 0 for every B partition type.
11. QP-RD chooses the same QP and transform flag for a synthetic MB with a non-monotonic RD curve.
12. Debug MV-range recovery switches to I_16x16 under the same conditions.

---

## 11. Open questions

- **UNKNOWN:** exact empirical derivation of all threshold LUTs (`i16x16_thresh_lut`, `i8x8_thresh`, `300*lambda`, `33/32`, `17/16`, `5/4`); they are documented in source comments as "not heavily tuned" or experimentally chosen.
- **UNKNOWN:** whether the `cost_div_fix8` extrapolation for partially analysed 8x8 blocks is intended to approximate four-, three- or two-block totals; only the divisors are visible.
- **UNKNOWN:** the exact rule that sets `ref_blind_dupe` (duplicate reference detection) — it is consumed here but computed outside the assigned files.
- **UNKNOWN:** full semantics of `b_direct_auto_write` scoring across frames; the per-MB accumulation is visible, the reset/tie policy is not.
- **UNKNOWN:** interaction of `b_deblock_rdo` with slice boundaries and `i_disable_deblocking_filter_idc` values other than 1.
- **UNKNOWN:** the exact bit-cost model behind `i_mb_b_cost_table` values (they are hand-tuned approximations, not sizes of actual syntax elements in all cases).
