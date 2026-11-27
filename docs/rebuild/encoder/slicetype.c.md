# `encoder/slicetype.c`

**Role:** Performs low-resolution lookahead analysis to choose slice types (I/P/B/B-ref), detect scene cuts, propagate macroblock-tree weights, plan VBV frame costs, and supply SATD/cost data to rate control.

> **Dirty-side behavioural specification.** Describes observable behaviour and structure only; no source text or near-verbatim pseudocode is reproduced.

---

## 1. Header

- Source path: `encoder/slicetype.c`
- Role: decide the frame-type sequence over a lookahead window using 8x8-per-MB lowres costs; run AQ/weighted-prediction analysis, MB-tree propagation, VBV planning, trellis/fast B-adapt and scenecut detection; expose `x264_rc_analyse_slice` to rate control.
- Marker: dirty-side behavioural specification.

---

## 2. Purpose

Good I/P/B decisions require knowing how much each frame would cost to code as P or B relative to intra, and how future references will be used. This module computes those costs once at low resolution (a quarter-scale luma plane, one 8x8 block per macroblock), reuses them across many candidate frame triples, and runs the decision algorithms on top: scenecut detection, B-adapt (none/fast/trellis), keyframe-interval enforcement, MB-tree weight propagation, and VBV lookahead planning. It also converts the decisions into coded order and hands the resulting SATD estimates to rate control.

---

## 3. Public interface

- `void x264_slicetype_analyse( x264_t *h, int intra_minigop )` — analyse the lookahead window, assigning `i_type` to frames; `intra_minigop != 0` marks an I-frame analysis pass (called from `lookahead.c` for MB-tree/VBV).
- `void x264_slicetype_decide( x264_t *h )` — finalise frame types for the first minigop, compute durations, insert B-refs, precompute costs for RC, analyse Weighted-P, reorder to coded order.
- `int x264_rc_analyse_slice( x264_t *h )` — return the precomputed lowres cost for the current frame (recomputed under MB-tree/AQ) and publish row SATDs into `h->fdec`.
- `void x264_weights_analyse( x264_t *h, x264_frame_t *fenc, x264_frame_t *ref, int b_lookahead )` — derive explicit Weighted-P parameters (scale/offset/denominator) by searching a small range around a variance-based guess.

Static helpers: `lowres_context_init`, weight-cost initialisation/comparison functions, `slicetype_mb_cost`, `slicetype_slice_cost`, `slicetype_frame_cost[_recalculate]`, `macroblock_tree[_propagate/_finish]`, `vbv_frame_cost`, `calculate_durations`, `vbv_lookahead`, `slicetype_path[_cost]`, `scenecut[_internal]`.

---

## 4. State and data structures

- `x264_mb_analysis_t a` — lowres ME state: QP = `X264_LOOKAHEAD_QP` (a fixed lowres QP), lambda from `x264_lambda_tab`, MV-cost tables, ME method/subpel settings.
- `x264_frame_t` fields used: `lowres` plane, `lowres_mvs[2][refs][mb][2]` (MV per reference, sentinel `0x7FFF` = not searched), `lowres_mv_costs`, `lowres_costs[p0][p1][mb]` (packed `min(cost, LOWRES_COST_MASK) | (list_used << LOWRES_COST_SHIFT)`), `i_intra_cost`, `i_cost_est[p0][p1]`, `i_cost_est_aq`, `i_row_satds[p0][p1]`, `i_intra_mbs`, `i_propagate_cost`, `f_qp_offset`/`f_qp_offset_aq`, `i_inv_qscale_factor`, `f_weighted_cost_delta`, `i_type`/`i_forced_type`, duration fields, `b_scenecut`, `i_bframes`.
- Output buffers for multithreaded lookahead are padded by `PAD_SIZE = 32` ints between threads to avoid false sharing; each thread's block holds four bookkeeping ints (`COST_EST`, `COST_EST_AQ`, `INTRA_MBS`, `NUM_ROWS`) then one row-SATD per MB row. `NUM_MBS` excludes edge MBs for interior frame scoring.

Invariants: lowres data must exist (`frames.b_have_lowres`); results are cached and must be invalidated before reuse; `x264_slicetype_decide` must run before `x264_rc_analyse_slice`.

---

## 5. Algorithm / behaviour

### 5.1 Lowres cost computation

`lowres_context_init` fixes the lowres QP and lambda, loads ME cost tables, and caps the ME method at HEX with subpel refinement 4 for `subpel_refine > 1` (otherwise DIA with refinement 2); chroma ME is off.

For each candidate triple `(p0, p1, b)`, `slicetype_frame_cost` first checks whether `i_cost_est[b-p0][p1-b]` is already available (and row SATDs when VBV is active). Otherwise it determines, per list, whether motion search is needed (sentinel MVs), computes `dist_scale_factor = (((b-p0)<<8) + ((p1-p0)>>1)) / (p1-p0)` (128 when p0 == p1), and runs the per-MB analysis — optionally on GPU via OpenCL, or split across `i_lookahead_threads` row slices via a threadpool, or single-threaded. `slicetype_slice_cost` walks rows **backwards** so MVs can serve as predictors for the main encode; edge MBs are skipped when MB-tree/VBV is off (they hurt predictive quality) but included when needed for spatial distribution or tiny frames.

`slicetype_mb_cost` works on one 8x8 block: for B frames it tries bidirectional prediction using scaled MV predictors from the previous B→P search (`dmv = (mvr*dsf + 128)>>8`), with a bipred weight of `64 - (dsf>>2)` when weighted bipred is enabled else 32; it also tries the zero-MV and refined-MV bidirectional candidates. For each active list it uses reverse-order MV prediction (right, bottom, bottom-left, bottom-right neighbours, median of up to three), with a fast skip when the predictor is zero and residual < 64, and adds `5*lambda` for nonzero MVs. Intra cost comes from three 8x8 SATD modes, extended to the full set for slow presets; costs are shifted by bit depth, given a small fixed penalty (4) to avoid zero-residual lookahead blocks, and AQ-weighted with `i_inv_qscale_factor`. The final per-MB cost is stored with the chosen list indicator; B frames forbid intra MBs. Frame-level scores accumulate only interior MBs; with B-adapt, inter scores are scaled by `100/(120 + bframe_bias)`.

The cached cost is reused by many decisions, which is why MB-tree changes require `slicetype_frame_cost_recalculate`: it re-derives per-row and total costs from cached `lowres_costs` and the updated `f_qp_offset` using `exp2(offset)`.

### 5.2 Weighted prediction analysis

`x264_weights_analyse` estimates a scale from the ratio of frame variances (`sqrt(fenc_var/ref_var)`, with a zero bias of 1) and offsets from mean pixel values (normalised by bit depth). Luma uses denominator up to 7 via `weight_get_h264` (reduce denominator while scale > 127); chroma searches a common denominator. It early-terminates when the mean difference is under 0.5 and scale is within `epsilon = 1/128` of 1. For each plane it scores candidate scale/offset pairs around the guess (search distances from a table indexed by `subpel_refine`, zero in lookahead), using per-MB `min(cmp, intra_cost)` for luma (plus slice-header bit cost) and DC-difference ASD for chroma; it stops scanning offsets once cost starts rising. Denominators are reduced while scales are even, a weight is rejected if it improves cost by less than 0.2% (`minscore/origscore > 0.998`), and chroma denominators are unified (avoiding the invalid implicit scale 128). Weights are cached, and in lookahead the reference's lowres plane is pre-scaled for reuse.

The slice-header cost is `lambda * numslices * (10 + denom_cost + 2*(size_se(scale) + size_se(offset)))`, with chroma lambda multiplied by 4 and denominator cost halved.

### 5.3 Macroblock tree

`macroblock_tree` walks non-B frames backwards from the end of the lookahead window. For each minigop it computes the P (or I) frame cost, clears that frame's propagate cost, and for every intervening B frame calls `macroblock_tree_propagate`, which computes per-MB propagated costs from intra/inter cost ratios and AQ factors (`mbtree_propagate_cost`) and pushes them to the two reference frames along the MVs with bipred weights (`mbtree_propagate_list`). A `fps_factor` derived from `CLIP_DURATION(frame)/(CLIP_DURATION(average)*256) * MBTREE_PRECISION` scales the influence by frame duration. B-pyramid minigops are split at the middle frame. With lookahead disabled, the last non-B frame's propagate costs are extrapolated onto the first frame. `macroblock_tree_finish` sets `f_qp_offset = f_qp_offset_aq - strength * log2((intra_cost + propagated_cost)/intra_cost) + weightdelta`, where strength is `5 * (1 - qcompress)` and the frame-duration ratio is folded into `fps_factor`; `MBTREE_PRECISION = 0.5` trades range for precision. Optional weighted-prediction cost deltas reduce the effect for fades.

### 5.4 VBV lookahead and durations

`vbv_lookahead` fills each upcoming frame's `i_planned_satd`, `i_planned_type` and planned CPB durations in **coded order**, using `vbv_frame_cost` (plain, AQ-weighted, or MB-tree-recalculated cost). `calculate_durations` sets `i_cpb_delay` from an accumulating delay counter, `i_dpb_output_delay = field_count - coded_fields + 2*num_reorder_frames`, corrects negatives by moving the excess into CPB delay, resets CPB delay on keyframes (except intra refresh), and advances both counters by the frame's duration. Rate control later consumes these planned arrays in `vbv_pass1`.

### 5.5 Slice-type decision algorithms

`scenecut_internal` compares P cost against intra cost: a scenecut occurs when `pcost >= (1 - bias)*icost`. The threshold scales with `--scenecut` (percent/100), minimum threshold is a quarter of maximum, and `bias` interpolates between min and max based on GOP position relative to `keyint_min`/`keyint_max` (with a quarter-minimum bias in very short GOPs and intra refresh). `scenecut` additionally suppresses "flash" cuts: short scenes are detected by checking larger frame windows, clearing `b_scenecut` on intermediate frames; frame-packing type 5 right views are excluded.

B-adapt has three modes:
- **None**: at most `i_bframe` consecutive B-frames, forcing P otherwise.
- **Fast**: greedily compares the cost of the current frame as P versus B (`slicetype_path_cost` over a small path), choosing the cheaper.
- **Trellis**: a Viterbi search over all paths of up to `i_bframe` B-frames; paths are character strings, best partial paths are cached by `length % (X264_BFRAME_MAX+1)`, and `slicetype_path_cost` stops early once cost exceeds the best-so-far threshold. B-pyramid splits candidate paths at a middle frame.

`x264_slicetype_analyse` orchestrates: caps deterministic search length, sets up the `last_nonb` anchor, runs an immediate scenecut check on frame 1, replaces forced keyframes with I/IDR, closes GOPs before IDRs, runs B-adapt, MB-tree, enforces `keyint_max`/`keyint_min` (allowing open-GOP I-frames and a Blu-ray ordering adjustment), runs VBV lookahead, then restores `i_forced_type` for frames not yet decided. Search length is limited to `i_slicetype_length + 1 - intra_minigop` for determinism.

`x264_slicetype_decide` computes per-frame durations (VFR from PTS differences, otherwise a `delta_tfi_divisor` table indexed by pic_struct), applies first-pass frame types in 2-pass mode or calls `x264_slicetype_analyse`, then scans the first minigop: enforces B-pyramid compatibility (B-refs require B-pyramid ≥ normal and enough references), resolves keyframes, limits GOP size, caps B-frame runs, inserts a middle B-ref when B-pyramid is enabled and no B-ref exists, precomputes costs for rate control (including B and intra costs for VBV row SATDs), analyses Weighted-P for the next P frame, reorders frames into coded order (B-refs first, then B, then P), assigns `i_coded` indices and computes final durations.

`x264_rc_analyse_slice` maps the current frame type to a `(p0, p1, b)` triple (I: 0/0/0; P: 0/bframes+1/bframes+1; B: POC-midpoint derived), reads the cached cost (recomputing under MB-tree/AQ), publishes row SATDs into `fdec`, and, for intra refresh with VBV, adds the intra-minus-inter cost of refreshed columns (scaled by `ip_factor = 256*ip_factor` and AQ weights).

---

## 6. Dependencies

Calls `x264_me_search` and lowres helpers from `me.c`, motion-compensation/pixel functions through `h->mc`/`h->pixf`/`h->zigzagf`, OpenCL helpers from `slicetype-cl.c`, `x264_ratecontrol_slice_type` from `ratecontrol.c`, and threadpool functions `x264_threadpool_run/wait`. Called by `lookahead.c` (`x264_slicetype_decide`/`analyse`) and `ratecontrol.c` (`x264_rc_analyse_slice`).

---

## 7. Constants and tables

- `delta_tfi_divisor[10] = {0,2,1,1,2,2,3,3,4,6}` — duration per pic_struct for CFR input.
- MV sentinel `0x7FFF`; `LOWRES_COST_MASK/SHIFT` for packed costs; `lowres_penalty = 4`; intra/NZ-MV penalties `5*lambda`.
- `PAD_SIZE = 32`, `NUM_INTS = 4` with indices `COST_EST=0`, `COST_EST_AQ=1`, `INTRA_MBS=2`, `NUM_ROWS=3`.
- `MBTREE_PRECISION = 0.5`; MB-tree strength `5*(1-qcompress)`; `fps_factor` factor 256.
- `dist_scale_factor` = weighted average of distances scaled by 256 (default 128); bipred weight `64 - (dsf>>2)` or 32.
- `epsilon = 1/128` weight-search threshold; 0.998 weight-acceptance threshold; chroma denominator starts at 7.
- B-adapt fast/trellis path costs scaled by `100/(120 + bframe_bias)`.
- Scenecut threshold min = max/4; bias interpolation over keyint_min/max; intra-refresh/frame-packing special cases.

---

## 8. Performance notes

Lowres lookahead is one of the most expensive stages at slow presets; it is deliberately parallelised and cached.

Opportunities, with honesty:
- **Lookahead threads**: `slicetype_frame_cost` splits MB rows across `i_lookahead_threads` using padded output buffers; this scales well and does not change results (costs are summed deterministically). More threads increase latency/memory but improve throughput.
- **Cost caching**: results for each (p0,p1,b) triple are reused across B-adapt, scenecut, MB-tree and VBV; preserving the cache is essential for speed. Changing MB-tree QPs forces `slicetype_frame_cost_recalculate`.
- **OpenCL**: offloads lowres analysis to GPU (`slicetype-cl.c`), a large speedup when available; decisions must match the CPU path for determinism.
- **Backward MB traversal** improves MV prediction quality at no cost; keep it.
- **Edge skipping** when MB-tree/VBV is off reduces work slightly; including edges is required for accurate spatial propagation.
- **Trellis early termination** via `threshold` avoids computing clearly-worse paths — a pure speed win.
- **AQ/VBV/MB-tree gating**: when these features are off, whole analysis passes are skipped; enabling them changes decisions and bitstream (a quality/RD trade, not a free optimisation).

No SIMD here directly, but the pixel/MC/ME primitives it calls are SIMD-optimised; a clean implementation should provide equivalent primitives.

---

## 9. References

- x264 public documentation on `--bframes`, `--b-adapt`, `--scenecut`, `--keyint`, `--mbtree`, `--aq-mode`, `--weightp`, `--vbv-*`, `--lookahead`.
- "Adaptive frame/field and B-frame decision" literature; Viterbi/trellis sequence estimation.
- H.264 weighted prediction (Annex B.6.1) and B-pyramid concepts.

---

## 10. Verification checklist

- Compare frame-type decisions and final bitstream against a reference encoder for a fixed input across bframe/b-adapt/scenecut/keyint settings.
- Verify scenecut behaviour on synthetic flashes and hard cuts; check frame-packing type 5 exclusion.
- Verify B-pyramid B-ref insertion and that B-refs are demoted when pyramid/ref count is insufficient.
- Check that 2-pass mode reproduces first-pass frame types exactly.
- Check determinism: `--deterministic` search cap yields identical decisions regardless of available lookahead frames.
- Validate MB-tree QP offsets against a reference for a fixed clip; check resolution-mismatch stats path.
- Verify VBV planned SATD/type/duration arrays are filled in coded order and match rate control's consumption.
- Check weighted-prediction parameters (scale/offset/denom) for known fades and that rejected weights fall back to identity.
- Confirm `x264_rc_analyse_slice` row SATDs match the cached lowres costs and that intra-refresh column adjustments are correct.

---

## 11. Open questions

- The exact numeric value of `X264_LOOKAHEAD_QP` and its lambda table are UNKNOWN here but affect cost scale (constant across implementations).
- Precise kernel-level cost formulas for the OpenCL path are UNKNOWN; GPU/CPU parity is assumed by design.
- The `weight_check_distance` table's dependence on `subpel_refine` beyond the shown values is UNKNOWN for refinements outside the table's range.
