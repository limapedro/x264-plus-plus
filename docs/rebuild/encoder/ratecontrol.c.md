# `encoder/ratecontrol.c`

**Role:** Implements x264's rate-control engine: CRF/ABR/CBR/2-pass QP derivation, adaptive quantization, MB-tree application, VBV/HRD buffer modelling, zone handling, and frame/slice-thread synchronisation of all of the above.

> **Dirty-side behavioural specification.** Describes observable behaviour and structure only; no source text or near-verbatim pseudocode is reproduced.

---

## 1. Header

- Source path: `encoder/ratecontrol.c`
- Role: decide the quantizer for each frame and macroblock so that a target bitrate/quality is met while respecting VBV and level limits, and maintain the statistics files used by multi-pass encoding.
- Marker: dirty-side behavioural specification.

---

## 2. Purpose

An x264 stream can be encoded at constant QP (CQP), constant rate factor (CRF), average bitrate (ABR, including CBR/VBV variants) or in two passes. This module converts those abstract targets into a per-frame Lagrangian `qscale` (a linearised quantizer), derives a QP from it, applies per-macroblock AQ/MB-tree offsets, tracks the hypothetical decoder buffer (VBV/CPB) to prevent overflow/underflow, emits first-pass statistics and consumes them in the second pass, and keeps all decisions deterministic in the presence of frame and slice threading.

---

## 3. Public interface

See `ratecontrol.h.md` for the full list. Principal functions implemented here:

- `x264_ratecontrol_new`, `x264_ratecontrol_delete`, `x264_ratecontrol_init_reconfigurable`, `x264_encoder_reconfig_apply`, `x264_ratecontrol_zone_init`.
- `x264_adaptive_quant_frame`, `x264_macroblock_tree_read`, `x264_reference_build_list_optimal`, `x264_ratecontrol_set_weights`, `x264_ratecontrol_slice_type`.
- `x264_ratecontrol_start`, `x264_ratecontrol_mb`, `x264_ratecontrol_qp`, `x264_ratecontrol_mb_qp`, `x264_ratecontrol_end`, `x264_ratecontrol_summary`.
- `x264_hrd_fullness`, `x264_thread_sync_ratecontrol`, `x264_threads_distribute_ratecontrol`, `x264_threads_merge_ratecontrol`.
- `x264_rc_analyse_slice` is *declared* here but implemented in `slicetype.c`.

Static helpers: `qp2qscale`, `qscale2qp`, `qscale2bits`, AC-energy helpers, MB-tree rescale, `predict_size`/`update_predictor`, `update_vbv`/`update_vbv_plan`, `get_qscale`, `get_diff_limited_q`, `rate_estimate_qscale`, `clip_qscale`, `vbv_pass1`, `vbv_pass2`, `init_pass2`, zone parsing, first/second-pass statistics I/O.

---

## 4. State and data structures

- `ratecontrol_entry_t` — one first/second-pass frame record: pict_type, frame_type, kept_as_ref flag, qscale, bit counts (texture, MV, misc), expected-bits accumulators, new qscale/QP, intra/P/skip MB counts, blurred complexity, direct-mode marker, reference weights and reference counts/usage, duration and CPB duration, output order.
- `predictor_t` — an adaptive linear size predictor: `coeff`, `count`, `offset`, `decay`, `coeff_min`. Estimate is `(coeff*var + offset) / (q*count)`; `var` is typically SATD.
- `x264_ratecontrol_t` — holds constants (`b_abr`, `b_2pass`, `b_vbv`, `b_vbv_min_rate`, `fps`, `bitrate`, `rate_tolerance`, `qcompress`, `nmb`, `qp_constant[3]`); current-frame values (`rce`, `qpm`, `qpa_rc`, `qpa_aq`, `qp_novbv`); VBV state (`buffer_size`, `buffer_fill`, `buffer_fill_final[_min]`, `buffer_rate`, `vbv_max_rate`, `pred`, `single_frame_vbv`, `rate_factor_max_increment`); ABR state (`last_satd`, `last_rceq`, `cplxr_sum`, `expected_bits_sum`, `filler_bits_sum`, `wanted_bits_window`, `cbr_decay`, `short_term_cplxsum/count`, `rate_factor_constant`, `ip_offset`, `pb_offset`); two-pass state (stat-file handles, `entry[]`, `entry_out[]`, `last_qscale[_for[3]]`, `last_non_b_pict_type`, `accum_p_qp/norm`, `lmin[3]`, `lmax[3]`, `lstep`, MB-tree buffers); MBRC state (`frame_size_estimated`, `bits_so_far`, `frame_size_maximum`, `frame_size_planned`, `slice_size_planned`, `row_pred`, `pred_b_from_p`, `bframes`, `bframe_bits`); zones; and HRD timing fields. An array of these (one per thread) is allocated by `ratecontrol_new`; thread 0's copy owns the global state.

Invariants: thread 0 is authoritative for VBV/HRD, ABR accumulators shared across frames; per-thread predictors are initialised from a common template; `qpm` is the unclamped frame QP used for row control.

---

## 5. Algorithm / behaviour

### 5.1 Conversions and bit models

- `qp2qscale(qp) = 0.85 * 2^((qp - (12 + QP_BD_OFFSET))/6)`; inverse `qscale2qp(q) = 12 + QP_BD_OFFSET + 6*log2(q/0.85)`. The constant 12+offset anchors QP 0 (or offset-adjusted 8-bit QP) to qscale 0.85.
- `qscale2bits(rce, q)`: clamp q to ≥ 0.1; texture bits scale as `(tex_bits + 0.1) * (rce.qscale/q)^1.1`; MV bits scale as `mv_bits * (max(rce.qscale,1)/max(q,1))^0.5`; misc bits are added unchanged. The exponents capture that texture bits fall slightly slower than inverse qscale (due to skip blocks) and MV bits level off at low QP.
- `predict_size(p, q, var)` and `update_predictor`: see §5.9.

### 5.2 Adaptive quantization

For each macroblock the "AC energy" is the sum over planes of the pixel variance `ssd - sum^2 >> shift` (shift 8 for luma, `7 - CHROMA_V_SHIFT` for chroma), optionally for both field and frame layouts when MBAFF is on, taking the smaller variance. With AQ off, offsets are zeroed (or copied from an external `quant_offsets` array when MB-tree still needs them), but variance data is still computed if weighted prediction is enabled.

With auto-variance AQ, a first pass computes `qp_adj = (energy * bit_depth_correction + 1)^0.125` per MB, averages it and its square, and sets `strength = aq_strength * avg_adj` and a bias term using `avg_adj - 0.5*(avg_adj_pow2 - 14)/avg_adj`. The biased variant then uses `strength*(qp_adj - avg_adj) + aq_strength*(1 - 14/qp_adj^2)`; the unbiased variant uses just `strength*(qp_adj - avg_adj)`. Simple AQ uses `aq_strength * 1.0397 * (log2(max(energy,1)) - (14.427 + 2*(BIT_DEPTH-8)))`. External offsets are added, results stored in `f_qp_offset`/`f_qp_offset_aq`, and `i_inv_qscale_factor` is a fixed-point `exp2(offset)` for lowres use. Finally the mean is removed from the per-plane SSD sums used by the complexity model.

### 5.3 MB-tree

In first-pass/stats-write mode, per-reference-frame quantizer maps are packed with `mbtree_fix8_pack` (8-bit fixed point), written beside the main stats file, and in second pass read back and unpacked. When the first-pass resolution differs from the current one, a separable triangular-tap resampler (vertical then horizontal) rescales the QP map; filter coefficients are computed as `max(1 - d, 0)` normalised to sum 1, with tap count from `1 + (2*src + dst - 1)/dst` when downscaling and 3 when upscaling. `macroblock_tree_read` reads records until the type matches the actual frame type, unpacks/rescales into `f_qp_offset`, and fills `i_inv_qscale_factor`; non-reference frames fall back to plain AQ. `x264_reference_build_list_optimal` reorders references (except ref 0) by descending stored reference counts, using lower POC as a tiebreak.

### 5.4 Reconfiguration, CRF constant and VBV setup

`rate_factor_constant` for CRF is `base_cplx^(1-qcompress) / qp2qscale(rf_constant + mbtree_offset + QP_BD_OFFSET)`, where `base_cplx = nmb * (bframes ? 120 : 80)` and `mbtree_offset = mb_tree ? (1-qcompress)*13.5 : 0`. This arbitrary rescaling makes CRF numerically similar to QP and compensates for MB-tree.

VBV setup: if CBR (`b_vbv_min_rate`) the max bitrate is forced to the ABR bitrate; the buffer is raised to at least one frame's worth of bits (`bitrate/fps`) with a warning. Bitrates are in units of 1000 bits (1024 for AVC-Intra). HRD parameters in the SPS are normalised: count = 1, CBR flag from `i_nal_hrd`, bit-rate scale = `clip(ctz(rate) - 6, 0, 15)` with value `rate >> (scale+6)`, CPB-size scale = `clip(ctz(size) - 4, 0, 15)` with value `size >> (scale+4)`; unscaled values are retained. Delay lengths are derived from a 0.5 s maximum: initial-removal length = `2 + clip(32 - clz(max_delay), 4, 22)` where `max_delay ≈ 90000*cpb/bitrate`; CPB/DPB output delay lengths = `clip(32 - clz(max_*_delay), 4, 31)`. `buffer_rate = vbv_max_bitrate / fps`, `single_frame_vbv` when `buffer_rate*1.1 > buffer_size`; `cbr_decay = 1 - buffer_rate/buffer_size * 0.5 * max(0, 1.5 - buffer_rate*fps/bitrate)` for ABR. Initial buffer fill is clamped to `[buffer_rate/buffer_size, 1]` (a value >1 is interpreted as kilobits and divided by buffer size) and multiplied by `buffer_size * time_scale`. A CRF-max greater than CRF sets `rate_factor_max_increment`. Changing VBV parameters while NAL HRD is active is refused with a warning.

### 5.5 Initialisation (`ratecontrol_new`)

`b_abr` is true for any non-CQP mode without stats-read; `b_2pass` only for ABR with stats-read. FPS defaults to 25 if unspecified. With MB-tree enabled, `pb_factor` is forced to 1 and qcompress to 1. Rate tolerance below 0.01 is raised with a warning. `b_variable_qp` is set when VBV or AQ is active. ABR initial state: `accum_p_norm = 0.01`, `accum_p_qp = ABR_INIT_QP * 0.01` where ABR_INIT_QP is CRF constant (or 24) plus QP_BD_OFFSET; `cplxr_sum = 0.01 * 7e5^qcompress * sqrt(nmb)`, `wanted_bits_window = bitrate/fps`. Offsets: `ip_offset = 6*log2(ip_factor)`, `pb_offset = 6*log2(pb_factor)`; CQP QPs are `qp_constant`, `clip(qp-ip_offset+0.5)` for I, `clip(qp+pb_offset+0.5)` for B; `lstep = 2^(qp_step/6)`; initial qscale corresponds to QP 26+offset. Predictors are seeded: frame predictors coefficient `{1.0,1.0,1.5}` with `coeff_min` half that, `count=1`, `decay=0.5`; row predictors coefficient 0.25; B-from-P predictor coefficient 0.5. Five predictors are allocated per (slice-)thread plus one.

Two-pass input parsing validates that the option line matches (resolution, timebase, bit depth, weightp, bframes, pyramid, intra refresh, open GOP, Blu-ray, MB-tree, interlacing, keyint), rescales first-pass bit counts by `(res_factor)^0.7` and MB counts by `res_factor`, reads frame records, maps pict-type characters to frame types, and sets `pic_init_qp` to the rounded average AQ QP unless stitchable. A binary search (`init_pass2`) distributes bits. Two-pass output files are written to `.temp` siblings and renamed on success. Per-thread rate-control copies are made.

### 5.6 Zones

A zone string `start,end,q=Q` forces QP; `start,end,b=factor` scales bitrate; `start,end` alone is neutral; extra `key=value` pairs are parsed with `x264_param_parse` and applied via `x264_encoder_reconfig_apply` at the zone's first frame. A default zone covering all frames with factor 1 is inserted at index 0; `get_zone` searches from last to first.

### 5.7 Per-frame QP derivation (`ratecontrol_start` and `rate_estimate_qscale`)

At frame start, in stats-read mode the matching entry is selected and direct-mode flags recovered. With VBV, per-row bit/QP/qscale arrays are cleared, the row predictor set for the slice type, `buffer_rate` recomputed from the frame's CPB duration, and the VBV plan updated (subtracting in-progress frames' planned bits). The maximum frame size is determined by MinCR from the level table: normally `384 * BIT_DEPTH * cpb_duration * time_scale_factor * mbps / mincr`; for the very first frame the spec's special case uses `max(pic_size_in_mbs, fr*max_mbps)` with `fr = 1/300` for levels ≥ 60 else `1/172`; Blu-ray forces mincr = 4; profiles above High use an effectively unlimited value.

QP selection:
- **CQP**: per-type constant QP (B-refs use the average of B and P constants); zones add `zone_qp - p_constant` or subtract `6*log2(bitrate_factor)`.
- **CRF/ABR (1-pass)**: compute complexity from lowres SATD (`x264_rc_analyse_slice`), maintain a decaying short-term complexity measure normalised by `CLIP_DURATION(duration)/BASE_FRAME_DURATION`, build a temporary entry and call `get_qscale`. For CRF the rate factor is `rate_factor_constant`; for ABR it is `wanted_bits_window/cplxr_sum` further multiplied by an overflow factor `clip(1 + (predicted_bits - wanted_bits)/abr_buffer, 0.5, 2)` where `abr_buffer = 2*rate_tolerance*bitrate*max(1, sqrt(time_done))`. I-frames following P frames take the accumulated P QP divided by `ip_factor`. Asymmetric qstep clipping (`last_qscale_for/lstep` … `*lstep`) limits QP swings. B-frame QP is derived separately (below). VBV constraints are applied by `vbv_pass1`.
- **2-pass**: `q` starts from the stored `new_qscale`, adjusted by `clip((abr_buffer - diff)/abr_buffer, 0.5, 2)` where `diff = predicted_bits - expected_bits`, with an end-of-video buffer scaling and a long-term correction `pow(total_bits/expected_bits_sum, w)`. VBV safety raises q in 5% steps until the expected buffer state matches the recorded `expected_vbv`.
- **B frames (all modes)**: QP is the duration-weighted average of the two nearest reference frames' average QPs (`(q0*dt1 + q1*dt0)/(dt0+dt1)`), with adjustments when a neighbour is I or B-ref, plus `ip_offset` or `pb_offset/2`/`pb_offset` depending on reference types and whether the B is kept as a reference. `qp_novbv` is stored before VBV scaling.
- A forced QP (`i_force_qp != AUTO`) overrides to `force_qp - 1`; final QP is clipped to `[qp_min, qp_max]`, and accumulators/frame QP averages are initialised.

`frame_size_planned` is either `qscale2bits(rce, q)` (2-pass) or `predict_size(pred, q, last_satd)` (1-pass), capped by `min(frame_size_maximum, max(buffer_fill, 0.001))`; single-frame VBV uses the entire `buffer_rate`.

### 5.8 Row-level VBV control (`ratecontrol_mb`)

Each macroblock adds its bits to `fdec->i_row_bits[y]`; at the last MB of a row the predictor is updated (absolute SATD, plus an intra predictor when the current QP is lower than the reference's). With VBV, the row QP is adjusted iteratively in 0.5 steps within `[prev_qp - qp_step, prev_qp + qp_step]` (further capped by `qp_max` and the CRF-max increment). Estimate `b1 = bits_so_far + predicted bits for remaining rows + other slices' bits`; tolerance `rc_tol = buffer_left_planned / threads * rate_tolerance` (halved for non-I). QP is raised while `b1` exceeds the planned frame size/tolerance or buffer-fill thresholds; lowered while it stays below 80% of plan and below a trust-weighted maximum; raised again to avoid VBV underflow/MinCR. A "trust coefficient" based on how much of the frame has been encoded prevents premature QP increases on flat top rows. B-frames are not allowed a lower QP than their references. If the required QP jump exceeds the per-frame step and the row is re-encodable, the function restores the QP averages, resets that row's bit count and returns -1 to request a row re-encode; a last-row underflow similarly retries at `qp_max`.

`ratecontrol_qp` returns `clip(qpm + 0.5, qp_min, qp_max)`; `ratecontrol_mb_qp` adds the AQ offset (using MB-tree-adjusted offsets for reference frames) and scales AQ toward zero in emergency mode above `QP_MAX_SPEC`, then clips.

### 5.9 Predictors

`predict_size = (coeff*var + offset) / (q*count)`. `update_predictor` ignores samples with `var < 10`; new coefficient is `max((bits*q - old_offset)/var, coeff_min)`, clipped to `old_coeff/1.5 .. old_coeff*1.5`; if the resulting offset would be negative it is set to 0 and the unclipped coefficient kept. State is decayed by `decay` (0.5) then the new term added and count incremented.

### 5.10 End of frame and HRD

`ratecontrol_end` aggregates MB counts (skip/I/P), computes average QPs and `f_crf_avg`, and in stats-write mode emits a line with frame in/out numbers, type, durations, QP, AQ QP, texture/MV/misc bits, MB counts, direct-mode choice, reference counts and weights. MB-tree QP maps are written for kept reference frames. ABR state is updated: `cplxr_sum += bits*q2q(qpa_rc)/(last_rceq * (pb_factor for B))` then multiplied by `cbr_decay`; `wanted_bits_window += duration*bitrate` then decayed. Filler bytes come from `update_vbv`. With NAL HRD, CPB removal and arrival times are tracked: first access unit initialises removal delay; later frames compute removal time from CPB delay, earliest arrival from initial delay/offset, CBR uses the previous final arrival time directly while VBR takes the max with earliest arrival; final arrival = initial arrival + `(bits + filler_bits)/bit_rate_unscaled` (H.264 Equation C-6); DPB output time adds `dpb_output_delay * time_factor`.

### 5.11 VBV/HRD buffer model

`update_vbv` subtracts `bits * time_scale` from `buffer_fill_final` and `buffer_fill_final_min`, logs underflow and clamps to zero (distinguishing CRF-max-induced underflow), then adds `bitrate * num_units_in_tick * cpb_duration` (or the full buffer for AVC-Intra). On overflow, if filler is enabled it emits enough 0xFF bytes (rounded up with `time_scale*8`) to bring the buffer back to size, accounting for filler NAL overhead; otherwise it clamps. `update_vbv_plan` starts from thread 0's minimum fill and provisionally subtracts in-progress frames' planned bits, adding each frame's `buffer_rate`, clamped. `hrd_fullness` derives the initial CPB removal delay and offset from current buffer state using the reduced fraction `90000 / (bit_rate * time_scale)`, and warns on under/overflow.

### 5.12 Two-pass allocation

`get_qscale` forms `q` from blurred complexity (or from frame duration with MB-tree, using `BASE_FRAME_DURATION/CLIP_DURATION(duration)^(1-qcompress)`), divides by the rate factor, and applies zone QP/bitrate overrides. `get_diff_limited_q` forces I QP from accumulated P QP (`accum_p_qp/accum_p_norm` divided by `ip_factor`, blended by `accum_p_norm`), B QP from the last non-B type times `pb_factor`, and limits changes to `lstep` multiples; it maintains the P-QP accumulator with mask `1 - (i_count/nmb)^2`. `clip_qscale` clips to `[lmin, lmax]`, with a soft logistic mapping in 2-pass. `vbv_pass1` implements lookahead VBV: up to 1000 iterations raise q by 1% until planned buffer fill over the lookahead window reaches 50–80%; without lookahead it lowers q when the buffer is below 50%, applies hard fill thresholds (`max_fill_factor` 1 or 2, `min_fill_factor` 1 or 2), and uses B-frame predicted bits to fill spare capacity; non-CBR keeps `q >= q0`. `find_underflow`/`fix_underflow`/`vbv_pass2` repeatedly adjust groups of frames' qscales between buffer under/overflow intervals (thresholds 10% and 90% of buffer), first shrinking overall size by 0.9–0.999 factors for overflow, then growing by 1.001 for underflow, storing expected VBV fill per frame. `init_pass2` blurs complexities with a Gaussian over ±`2*cplxblur` frames (`exp(-j^2/200)` with a decreasing intra-weight factor), binary-searches a rate factor from a coarse `1e4*step_mult` down to `1e-7*step_mult` halving each step, smooths qscales with an odd Gaussian filter of size `(qblur*4)|1`, and warns if the final bit total deviates by more than 1%.

### 5.13 Thread synchronisation

`x264_thread_sync_ratecontrol` copies "start-updated" fields (P-QP accumulators, last SATD/qscale, short-term complexity, B-frame counts, zone pointer, MB-tree buffer position, and reconfigurable VBV constants) from the most recently started frame context to the one about to start, and "end-updated" fields (complexity sums, expected/filler bits, ABR window, B-frame bits, HRD delays) from the most recently ended context to the one about to end. Slice threading distributes a frame's planned size among slices by row SATD via per-thread predictors, normalises the slice sizes to the frame plan, merges QP averages and updates predictors at frame end.

---

## 6. Dependencies

Depends on `common/common.h` (levels table, math helpers), `common/frame.h`, `me.h` (lowres SATD/MV helpers), `set.h` (HRD field validation helpers), `x264_rc_analyse_slice` in `slicetype.c`, and `x264_encoder_reconfig_apply`. Called extensively from `encoder.c` around each frame and macroblock, from `lookahead.c` for MB-tree/VBV analysis, and from `slicetype.c` for planned frame costs.

---

## 7. Constants and tables

- `qp2qscale` base 0.85, offset 12 + QP_BD_OFFSET, divisor 6; qscale2bits exponents 1.1 (texture) and 0.5 (MV); qscale floor 0.1.
- AQ constants: auto-variance exponent 0.125, bias `14/qp_adj^2`, simple-AQ scale 1.0397 and offset `14.427 + 2*(bitdepth-8)`, normalisation 14.
- CRF base complexity `nmb * (bframes ? 120 : 80)`; MB-tree offset `(1-qcompress)*13.5`.
- ABR init: `accum_p_norm = 0.01`, ABR_INIT_QP = 24+offset for ABR; `cplxr_sum = 0.01 * 7e5^qcompress * sqrt(nmb)`; `wanted_bits_window = bitrate/fps`.
- VBV: BR_SHIFT 6, CPB_SHIFT 4, HRD max duration 0.5 s; delay-length clamps 4..22 / 4..31; initial fill >= `buffer_rate/buffer_size`; `single_frame_vbv` threshold 1.1.
- Rate tolerance floor 0.01; predictor decay 0.5, range 1.5, coefficient floor `pred_coeff_table[i]/2` with table `{1.0, 1.0, 1.5}`.
- Row RC: step 0.5, `max_frame_error = clip(1/mb_height, 0.05, 0.25)`, trust threshold 0.05, plan threshold 0.8, 0.9 safety factor.
- Two-pass: blur `exp(-j^2/200)`, qblur filter size `(qblur*4)|1`, rate-factor search `1e4*step_mult … 1e-7*step_mult`, VBV iteration 5% steps / 1000 iterations, overflow factor 0.9–0.999, underflow 1.001, buffer thresholds 10%/90%, convergence 1%.
- MinCR first-frame factor `fr = 1/300` (levels ≥ 60) else `1/172`; Blu-ray mincr 4; bits-per-MB constant 384 * bit depth.

---

## 8. Performance notes

Rate control touches every frame and macroblock; the per-MB hook is mildly hot, and the SATD analysis (`x264_rc_analyse_slice`) is significant at slow presets.

Opportunities, with honesty:
- **Lookahead VBV (`i_lookahead`)**: using planned future frame types/SATDs to smooth QP improves CBR compliance and quality, but costs analysis time. Disabling it is a legitimate speed/quality trade that changes bitstream and rate accuracy.
- **Simplified models**: dropping MB-tree (forces `pb_factor=1`, `qcompress=1`), AQ, or 2-pass blur removes substantial computation and changes quality/rate; must be an explicit design choice, not a transparent optimisation.
- **Header/state caching**: the constants computed in `init_reconfigurable` (HRD scales, delay lengths, CRF constant) can be cached across reconfigs that do not affect them — no output change.
- **Row RC**: the current `predict_row_size_to_end` is O(N²) in row count (noted as a TODO in the reference); an incremental prefix-sum predictor would reduce work without changing results if carefully matched.
- **Frame threading**: synchronisation copies only a documented set of fields; keeping rate control replicated per thread avoids serialising frames and preserves determinism when the copy set is exact. Omitting fields changes output.
- **Satd/energy**: AC-energy and lowres SATD are SIMD-accelerated through `h->pixf`/`h->mc`; a clean implementation should use equivalent vector primitives for speed, with no output change.

---

## 9. References

- ITU-T H.264 Annex F.1 (HRD/CPB model; Equation C-6 for CPB arrival times) and Annex E (VUI HRD parameters, MinCR).
- x264 public documentation: `--crf`, `--bitrate`, `--vbv-*`, `--aq-mode`, `--mbtree`, `--qcomp`, `--qblur`, `--complexity-blur`, `--zones`, `--pass`.
- "Rate Control for DCT Based Video Coding" / general R-λ (Lagrangian) rate-control literature; H.264 rate-distortion optimisation papers.

---

## 10. Verification checklist

- Verify `qp2qscale`/`qscale2qp` round-trip and the exact constants for several bit depths.
- With CQP, ensure emitted QP per frame type matches `qp_constant` offsets and zone overrides.
- With CRF, confirm constant quality (similar QP across complexity changes) and that the final ratefactor log matches the formula.
- With ABR/CBR, measure achieved bitrate against target and check VBV never underflows/overflows on stress clips; check filler emission when `b_filler` is set.
- Compare first-pass stats files line-by-line against a reference encoder for a fixed input.
- Compare second-pass allocated QPs/frame sizes and final bitrate within tolerance; check the "curve failed to converge" warning path.
- Verify HRD SEI fields (CPB count/scales/values/delay lengths) against an independent HRD parser and confirm CPB arrival times stay within buffer.
- Exercise zones: forced QP, bitrate factor, and per-zone parameter overrides at frame boundaries.
- Verify frame- and slice-threaded output is bit-identical to single-threaded output for the same parameters (determinism).
- Check MB-tree stats round-trip at same and different resolutions (rescaling path).

---

## 11. Open questions

- `x264_encoder_reconfig_apply` is declared and called but its full behaviour is spread across `encoder.c`; only the rate-control-relevant effects are described here.
- The exact set of parameters copied between threads (and hence determinism guarantees) for newer threading modes is UNKNOWN; this reflects the reference snapshot.
- `FILLER_OVERHEAD` is defined outside this file (`common/common.h`); its exact value affects filler byte counts and is UNKNOWN here.
- First-pass stats parsing edge cases (malformed files) are validated with specific `sscanf` field-count checks; only the general structure is described.
