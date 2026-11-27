# `common/opencl/bidir.cl`

**Role:** Selects the least-cost prediction mode (intra / list0 / list1 / bidirectional) for every lowres macroblock using SATD, testing the three H.264 B-slice bidirectional candidates, and reduces the resulting costs into frame-level statistics for lookahead decisions.

> **Dirty-side behavioural specification.** Describes observable behaviour and structure only; no source text or near-verbatim pseudocode is reproduced.

---

## 1. Header

- Source path: `common/opencl/bidir.cl`
- Role: final stage of the GPU lookahead — mode selection plus per-row/per-frame cost accumulation — consuming all earlier analysis buffers.
- Marker: this document is a dirty-side behavioural specification derived from the reference implementation and public H.264 literature.

---

## 2. Purpose

After intra costs and motion vectors for both reference directions are available, the lookahead must decide, per lowres macroblock, which prediction mode is cheapest. For P frames the choice is among intra, list0 and list1. For B frames it additionally tests three bidirectional combinations (temporal, direct, and combined L0+L1). This file implements that decision and writes a compact 16-bit word combining the saturated cost with a two-bit list identifier. It then reduces the costs across each row and across the frame, applying the B-frame bias and AQ weighting used by the rate controller.

---

## 3. Public interface

Device helper:

- **`bidir_satd_8x8_ii_coop4( fenc_lowres, fencpos, fref0_planes, qpos0, fref1_planes, qpos1, weight, tmpp, idx )`** — four threads cooperate on one 8×8 bidirectional SATD. Thread `idx` handles two rows; each sample is the current luma value minus a weighted average of two interpolated reference samples (one from each list). The routine packs two column differences per `sum2_t`, performs two Hadamard stages through local memory, accumulates `abs2`, and returns the normalized SATD.

Kernels:

- **`mode_selection( read_only image2d_t fenc_lowres, read_only image2d_t fref0_planes, read_only image2d_t fref1_planes, const global short2 *fenc_lowres_mvs0, const global short2 *fenc_lowres_mvs1, const global short2 *fref1_lowres_mvs0, const global int16_t *fenc_lowres_mv_costs0, const global int16_t *fenc_lowres_mv_costs1, const global uint16_t *fenc_intra_cost, global uint16_t *lowres_costs, global int *frame_stats, local int16_t *cost_local, local sum2_t *satd_local, int mb_width, int bipred_weight, int dist_scale_factor, int b, int p0, int p1, int lambda )`** — global `[mb_width, mb_height]` for P, `[mb_width*4, mb_height]` for B (four threads per macroblock for the bidirectional measurements).
- **`sum_inter_cost( const global uint16_t *fenc_lowres_costs, const global uint16_t *inv_qscale_factor, global int *fenc_row_satds, global int *frame_stats, int mb_width, int bframe_bias, int b, int p0, int p1 )`** — global `[256, mb_height]`, local `[256,1]`.

---

## 4. State and data structures

- **Cost/MV buffers:** `fenc_lowres_mv_costs0/1` (one saturated SAD+SATD cost per macroblock per frame distance), `fenc_intra_cost` (best intra SATD + 5λ), `fenc_lowres_mvs0/1` (per-direction MVs), `fref1_lowres_mvs0` (MVs of the future reference toward the past reference, used for temporal B prediction).
- **Large multi-frame layout:** MV/cost buffers hold `(bframe+1)` frames worth of vectors. The kernel indexes them as `(b - p0 - 1)*mb_count + mb_xy` for list0 and `(p1 - b - 1)*mb_count + mb_xy` for list1. `fref1_lowres_mvs0` is indexed directly at `mb_xy` because it already belongs to the relevant frame.
- **`lowres_costs`:** one 16-bit word per macroblock. Low 14 bits hold `min(bcost, 16383)`; bits 14–15 hold `list_used` (0=intra, 1=list0, 2=list1, 3=bidir) shifted by `LOWRES_COST_SHIFT`.
- **`frame_stats`:** four-element `int`; indices 0/1/2 are `COST_EST`, `COST_EST_AQ`, `INTRA_MBS`. `mode_selection` zeroes all four entries via work items with `mb_x < 4 && mb_y == 0`.
- **Local scratch:** `cost_local` (four `int16_t` per macroblock in the group) and `satd_local` (sixteen `sum2_t` per macroblock) support the cooperating bidirectional threads.

---

## 5. Algorithm / behaviour

### 5.1 Bidirectional sample construction

For a quarter-pel position `q` in a reference frame, two half-pel samples are selected:

- Position **A** uses `q >> 2` as the texel coordinate and a plane index derived from bit 1 of x plus bit 2 of y.
- Position **B** uses `q + (q&1)*2` (i.e. adds two qpel units to each component whose low bit is set) before the same coordinate/plane derivation.

The two bytes are extracted from the packed HPEL word by shifting by `8*hpel_index` and masking `0xFF`. List0 and list1 are each reduced to one interpolated sample this way. The bidirectional prediction sample is then:

`pred = (ref0*weight + ref1*(64-weight) + 32) >> 6`

i.e. a 6-bit weighted average with a rounding bias of 32. `bipred_weight` is computed on the host as `64 - (dist_scale_factor>>2)` when weighted bipred is enabled, else 32 (equal weighting). `dist_scale_factor` itself is derived from the distances `b-p0` and `p1-p0`, encoding the temporal position of the B frame between its references.

The residual for each pixel is `current_lowres_sample - pred`, accumulated in SAD/SATD form.

### 5.2 Cooperating SATD

Four work items (`idx = 0..3`, obtained for B launches from `global_id(0) & 3`) share one 8×8 block:

- Each handles two consecutive rows, computing eight residuals by reading the current image and the two interpolated reference samples at columns 0..7.
- Two residuals (columns `a` and `a+4`) are packed into one `sum2_t` as a 16-bit low half and 16-bit high half. Four such words correspond to the four logical columns of a row-pair.
- A first Hadamard transform is applied along each row and stored into `tmp[idx][0..3]` in local memory; a second transform reads `tmp[0..3][idx]` (transposed) to transform along columns. Both use the `HADAMARD4` macro from `x264-cl.h`.
- Each thread sums `abs2` of its four resulting coefficients and returns `(low + high) >> 1` normalization.
- Because the local store pattern is transposed between the two stages, the precise 32-bit `sum2_t` layout and the `volatile` qualifier on the local pointer matter; the reference uses them to enforce the required ordering.

### 5.3 `mode_selection` decision logic

For each macroblock:

1. **Thread mapping.** `mb_x = global_id(0) >> 2`, `mb_i = global_id(0) & 3`. For B launches, work items beyond `mb_width` return; extra X padding threads are tolerated. `mb_y = global_id(1)`, `mb_height = global_size(1)`, `mb_count = mb_width*mb_height`, `mb_xy = mb_x + mb_y*mb_width`.
2. **Statistics initialization.** Work items with `mb_x < 4 && mb_y == 0` zero `frame_stats[0..3]` for the next kernel.
3. **Unidirectional candidates.** `bcost` starts at `COST_MAX` and `list_used` at 0. If this is not a B frame, the intra cost is considered (`list_used = 0`). If `b != p0`, the list0 MV cost is considered (`list_used = 1`). If `b != p1`, the list1 cost is considered (`list_used = 2`). Each comparison uses `COPY2_IF_LT`, so a strictly lower cost replaces both the best cost and the list identifier.
4. **Bidirectional candidates (only when `b < p1`, i.e. a true B slice).** The launch has four threads per macroblock; `coord = (mb_x,mb_y)<<3`, and local pointers are advanced by `mb_in_group` using the same formula as the HME/subpel kernels. Each candidate calls the cooperative SATD helper, sums the four thread results, and updates `bcost`/`list_used = 3` with cost `penalty*lambda + satd`:
   - **Temporal prediction:** derive a motion vector from the future reference's MV toward the past: `dmv0 = (mvr * dist_scale_factor + 128) >> 8` (scaling with rounding), and `dmv1 = dmv0 - mvr`, where `mvr = fref1_lowres_mvs0[mb_xy]`. Penalty 0.
   - **B-direct prediction:** if either component of `dmv0`/`dmv1` is nonzero, also test the zero/zero vector pair. Penalty 0.
   - **Combined L0+L1:** use `fenc_lowres_mvs0` and `fenc_lowres_mvs1` for this frame. Penalty 5.
5. **Writeback.** `lowres_costs[mb_xy] = min(bcost, LOWRES_COST_MASK) + (list_used << LOWRES_COST_SHIFT)`.

### 5.4 `sum_inter_cost`

One work row `y` per group of 256 threads. Threads stride across macroblocks and accumulate four quantities:

- `cost = lowres_costs[mb_xy] & LOWRES_COST_MASK`, `list = lowres_costs[mb_xy] >> LOWRES_COST_SHIFT`.
- `b_frame_score_mb` is true for strictly interior macroblocks, or for all macroblocks when `mb_width <= 2 || mb_height <= 2`.
- `intra_mbs` increments when `list == 0` and the block is scoreable.
- `cost_aq = (cost * inv_qscale_factor[mb_xy] + 128) >> 8` is added to `row_satds` for all macroblocks.
- `cost` and `cost_aq` are added to `cost_est` / `cost_est_aq` only for scoreable blocks.

Each accumulator is reduced with `parallel_sum` across the 256 lanes using one local `int[256]`. If `b != p1`, `cost_est` is converted to float and scaled by `100 / (120 + bframe_bias)` — floating point is used deliberately to avoid 32-bit integer overflow. Thread `x==0` writes `fenc_row_satds[y]` and atomically adds `cost_est`, `cost_est_aq` and `intra_mbs` into `frame_stats[0..2]`.

### 5.5 Host dispatch contract

`x264_opencl_finalize_cost()` in `encoder/slicetype-cl.c`:

- Computes `bipred_weight` as described above.
- For P frames: global `[mb_width, mb_height]`, no explicit local dimensions, local allocation sizes 4 bytes each (unused by the unidirectional path).
- For B frames: global `[mb_width*4, mb_height]`, local dimensions from `optimal_launch_dims()`, `cost_local_size = 4*mb_per_group*sizeof(int16_t)`, `satd_local_size = 16*mb_per_group*sizeof(uint32_t)`.
- Arguments in order: `fenc->scaled_image2Ds[0]`, `fref0->luma_hpel`, `fref1->luma_hpel`, `fenc->lowres_mvs0`, `fenc->lowres_mvs1`, `fref1->lowres_mvs0`, `fenc->lowres_mv_costs0`, `fenc->lowres_mv_costs1`, `fenc->intra_cost`, `lowres_costs[last_buf]`, `frame_stats[last_buf]`, the two local allocations, then `mb_width`, `bipred_weight`, `dist_scale_factor`, `b`, `p0`, `p1`, `lambda`.
- `sum_inter_cost` is then launched with global `[256, mb_height]`, local `[256,1]`, arguments `(lowres_costs[last_buf], inv_qscale_factor, row_satds[last_buf], frame_stats[last_buf], mb_width, i_bframe_bias, b, p0, p1)`.
- The host reads back `lowres_costs` into `fenc->lowres_costs[0][0]`, `row_satds` into `fenc->i_row_satds[0][0]`, and the three frame statistics into `fenc->i_cost_est[0][0]`, `i_cost_est_aq[0][0]` and the intra-MB statistic.

`x264_slicetype_frame_cost()` in `encoder/slicetype.c` selects this path when `h->param.b_opencl` is true, calling `x264_opencl_finalize_cost()` after the two motion searches; otherwise it runs the CPU mode decision in `slicetype.c`.

### 5.6 GPU versus CPU output; host selection

All outputs are lookahead estimates. The GPU path is gated solely by `b_opencl` and successful OpenCL initialization; on OpenCL errors the host sets `b_opencl = 0` and flags a fatal error, after which results may be stale (see Open questions). The bidirectional SATD uses the simplified two-plane `rhadd` interpolation and the packed-lane SATD, so its costs can differ from a full CPU bidirectional evaluation. The reference treats these as equivalent for frame-type decisions. UNKNOWN: whether the project expects the chosen `list_used` to always match the CPU's choice.

---

## 6. Dependencies

- **Uses from `x264-cl.h`:** `sum2_t`, `LOWRES_COST_MASK`, `LOWRES_COST_SHIFT`, `COST_MAX`, `COPY2_IF_LT`, `HADAMARD4`, `abs2`, `BITS_PER_SUM`, `parallel_sum`, the sampler, `rhadd`.
- **Inputs:** all outputs of `intra.cl`, `motionsearch.cl`, `subpel.cl` and `downscale.cl` (F/H/V/C planes).
- **Outputs:** `lowres_costs`, `fenc_row_satds`, `frame_stats` — consumed by the host for rate control and frame-type scoring.
- **Host:** kernels created in `common/opencl.c`; dispatched in `encoder/slicetype-cl.c` and invoked from `encoder/slicetype.c`.

---

## 7. Constants and tables

- `LOWRES_COST_MASK` = 16383, `LOWRES_COST_SHIFT` = 14: cost/list packing.
- `COST_MAX` = `1<<28`: initial best-cost sentinel.
- `bipred_weight`: 32 for equal weighting, `64 - (dist_scale_factor>>2)` when weighted bipred is on; the complementary weight is `64 - bipred_weight`.
- `dist_scale_factor` = `((b-p0)<<8 + (p1-p0)/2) / (p1-p0)` when `p1 != p0`, else 128.
- Bidirectional penalties: 0 for temporal and direct, 5 for combined L0+L1 (multiplied by lambda).
- Rounding biases: `+128` before `>>8` for temporal MV scaling and AQ; `+32` before `>>6` for the weighted average.
- B-frame bias formula: `cost_est * 100 / (120 + bframe_bias)` in floating point when `b != p1`.

---

## 8. Performance notes

- **Parallelism:** B launches use four threads per macroblock so the three bidirectional modes and the four-thread SATD are spread across more of the GPU; the X dimension includes padding threads that return early.
- **Coalescing:** image reads through the packed HPEL planes return four bytes per fetch; neighbouring macroblocks read consecutive texels.
- **Local memory:** `satd_local` (16 `sum2_t` per macroblock) dominates; the `volatile` transposed exchange is required for correctness on the reference's target devices.
- **Branch behaviour:** `b_bidir` is uniform per launch; within a B launch all four threads execute the same candidate sequence, so there is no divergence across the cooperating group.
- **Reductions:** four sequential `parallel_sum` calls in `sum_inter_cost`, each with a barrier only above 32 lanes; 256 threads per row group is the host's fixed choice.
- **Platform limitations:** launches for B frames use `optimal_launch_dims()` to respect device work-group limits; the mode-select kernel's local allocations scale with `mb_per_group`.

---

## 9. References

- ITU-T Rec. H.264 / ISO/IEC 14496-10, clauses on B-slice prediction modes (direct/temporal, L0, L1, bidir) and weighted prediction.
- ITU-T H.264, rate-distortion mode decision with `lambda` penalties.
- Khronos Group, *OpenCL C Specification*: `read_imageui`, local memory, barriers, `volatility`, `min`, `abs_diff`, `rhadd`.
- Hierarchical SATD/ Hadamard transform literature for fast mode decision.

---

## 10. Verification checklist

- For a P frame with zero MVs and a static current/reference image, the cheapest mode should be list0 (or intra if that is strictly cheaper by the given `+5*lambda` bias) and `list_used` should reflect it.
- For a B frame with both references identical to the current frame, verify that a bidir candidate wins when its cost plus penalty is lowest and that `list_used == 3`.
- Verify the temporal MV scaling `(mvr*dist_scale_factor + 128)>>8` and the relation `dmv1 = dmv0 - mvr` for known `mvr` and `dist_scale_factor`.
- Verify that B-direct is only attempted when at least one component of the temporal vector pair is nonzero.
- Verify the combined L0+L1 penalty is exactly `5*lambda`.
- Verify the weighted average formula with `bipred_weight = 32` (equal) and with a non-symmetric weight: check the `+32` rounding at the `>>6` step.
- Verify the cooperative SATD result against a scalar reference SATD for random frames and MVs.
- Verify the packed output word: low 14 bits equal the saturated cost and upper bits equal `list_used`.
- Verify `frame_stats[INTRA_MBS]` counts only scoreable macroblocks with `list == 0`, and that `cost_est` is scaled by `100/(120+bframe_bias)` only when `b != p1`.
- Verify AQ weighting and row SATDs against the CPU lookahead for a representative frame.

---

## 11. Open questions

- **UNKNOWN:** exact equality of GPU and CPU mode decisions; the reference does not assert bit-exact equivalence for this stage.
- **UNKNOWN:** the role of the fourth `frame_stats` entry (initialized to zero by `mode_selection`, never accumulated by `sum_inter_cost`).
- **UNKNOWN:** behaviour on OpenCL error: `OCLCHECK` sets `b_opencl = 0` and `b_fatal_error = 1` and returns `-1`, but `slicetype.c` ignores these return values and still reads `i_score` from the frame buffer; whether the fatal flag stops the encode before stale values are used is not verified here.
- **UNKNOWN:** whether the `volatile` local exchange in the cooperative SATD is required on all devices or is a legacy workaround.
- **UNKNOWN:** treatment of padded X threads for B launches when `mb_width*4` is not a multiple of the local X size; the early `mb_x >= mb_width` return appears sufficient but was not exhaustively verified.
