# `encoder/encoder.c`

**Role:** Top-level encoder: parameter validation/derivation, SPS/PPS/slice-header construction and NAL emission, frame-encode control flow, macroblock/slice write loop with re-encode paths, threading orchestration, and high-level statistics.

> **Dirty-side behavioural specification.** Describes observable behaviour and structure only; no source text or near-verbatim pseudocode is reproduced.

---

## 1. Header

- Source path: `encoder/encoder.c`
- Role: implement the depth-specific `x264_8_*`/`x264_10_*` internal API (the functions `api.c` dispatches to): open/close/reconfig, headers, encode, delayed-frame queries, plus the per-frame slice writer and all supporting state management.
- Marker: dirty-side behavioural specification.

---

## 2. Purpose

This is the conductor of the encoder. It validates and reconciles the user's parameters into a self-consistent configuration, derives the SPS/PPS and per-slice headers, drives the input frame through lookahead, rate control, reference-list management, entropy coding and reconstruction, emits the complete NAL stream (including headers, SEI, filler and emulation-prevention escaping), coordinates frame and slice threads, and finally prints a full summary on close. Deterministic output is maintained by synchronising per-thread contexts and rate-control state in a fixed order.

---

## 3. Public interface

Depth-specific implementations of every entry point declared in `api.c.md`:

- `x264_t *x264_encoder_open( x264_param_t *param, void *api )` — create and fully initialise an encoder; returns NULL on any failure.
- `int x264_encoder_headers( x264_t *, x264_nal_t **, int * )` — emit SPS, PPS and version SEI.
- `int x264_encoder_encode( x264_t *, x264_nal_t **, int *, x264_picture_t *, x264_picture_t * )` — submit a picture (`pic_in == NULL` flushes) and/or obtain one output frame.
- `int x264_encoder_reconfig( x264_t *, x264_param_t * )`, `void x264_encoder_parameters( x264_t *, x264_param_t * )`, `int x264_encoder_reconfig_apply( x264_t *, x264_param_t * )`.
- `void x264_encoder_close( x264_t * )`, `int x264_encoder_delayed_frames( x264_t * )`, `int x264_encoder_maximum_delayed_frames( x264_t * )`, `void x264_encoder_intra_refresh( x264_t * )`, `int x264_encoder_invalidate_reference( x264_t *, int64_t pts )`.

Internal helpers: `validate_parameters`, `slice_header_init/write`, `nal_start/end`, `encoder_encapsulate_nals`, `slice_write`, `slices_write`, `threaded_slices_write`, `encoder_frame_end`, reference-list/filtering helpers.

---

## 4. State and data structures

- `x264_t` — the full encoder context (opaque to callers): parameters, SPS/PPS pointers, macroblock state, frame queues (`frames.current`, `frames.reference`, `frames.unused`), lookahead pointer, rate-control array, output bitstream (`out.bs`, `out.nal[]`, `nal_buffer`), CABAC/CAVLC state, DSP function tables (`pixf`, `mc`, `quantf`, `dctf`, `zigzagf`, `loopf`, `bsf`), statistics, and thread pool/phase fields.
- `x264_bs_bak_t` — bitstream/CABAC/statistics checkpoint for four re-encode scenarios: slice-max-size, CAVLC overflow, slice-min-mbs, row-VBV. For CABAC, the previous output byte is also saved because carry propagation modifies it.
- `x264_slice_header_t` (`h->sh`) — first/last MB, slice type, PPS id, frame_num, POC fields, ref-list reordering commands, weights, MMCO commands, QP delta, deblocking controls, direct-mode flag.
- Thread contexts: `h->thread[]` (frame threads or slice threads), `h->lookahead_thread[]`, `h->reconfig_h` (scratch for reconfig validation).

Invariants: thread 0 owns global state; a frame is only freed after all threads release it (reference counting); output NAL payloads point into `nal_buffer` and are rebased when buffers grow.

---

## 5. Algorithm / behaviour

### 5.1 Parameter validation and derivation (`validate_parameters`)

This large routine normalises *every* parameter and rejects invalid configurations. Highlights:

- Requires `pf_log` set (i.e. `x264_param_default` was called); on open, verifies CPU supports the compiled SIMD level (SSE1/MMXEXT) or fails.
- Resolution must be 1…16384 in each dimension; colour-space family must be compiled in and valid; dimensions divisible by the chroma subsampling and interlacing modulus (`w_mod = 2` for 4:2:0/4:2:2, `h_mod` doubled for subsampled height and interlacing); monochrome forces zero chroma QP offset/chroma ME/undefined matrix. Crop rectangle must be within bounds and divisible by those moduli; invalid SAR is cleared.
- Thread auto-configuration: `i_threads = CPUs * (sliced ? 2 : 3) / 2`, capped at roughly two MB rows per thread (four for sliced threads), clipped to 1…`X264_THREAD_MAX`; one thread disables sliced threads and sets lookahead threads to 1; `i_thread_frames = sliced ? 1 : i_threads`; frame threading disables `nalu_process`. OpenCL is disabled when unavailable, at high bit depth, for frames smaller than 32×32, or with conflicting device selection.
- Keyint max clipped to 1…infinite; keyint 1 forces all-intra (no intra refresh/weightp/refs). Frame packing validated (7 requires cropped size divisible by 3); mastering-display coordinates (16-bit) and luminance (32-bit) ranges checked, min==max==50000 rejected; content-light levels checked. A "broken ffmpeg defaults" heuristic sums nine suspicious default values and fails at score ≥ 5.
- AVC-Intra class validation: requires 10-bit; class 50/100/200/300/480 mapped to a type; colour-space/resolution/fps/interlacing checked against a 5×2×7 lookup table of frame sizes and custom quantisation matrices; forces all-intra, ABR with VBV, filler, CQM custom, repeat headers, AUD, no deblocking, no VFR, 8x8 transform, and either 8 (Sony XAVC) or 10 (Panasonic) slices with specific thread limits and QP floor.
- CRF/QP/factor clipping: RF ∈ `[-QP_BD_OFFSET, 51]`, QP ∈ `[-1, QP_MAX]`, subme 0…11, ip/pb factors 0.01…10; CRF sets `qp_constant = rf + QP_BD_OFFSET` and zero bitrate. QP 0 (CQP/CRF) switches to lossless mode: flat CQM, no psy/AQ/trellis/noise reduction, no B-frames, IP/pb factors 1, and disables 8x8dct for CAVLC at low subme. CQP derives min/max QP from the three per-type QPs (`qp_p`, `qp_p - 6*log2(ip_factor)`, `qp_p + 6*log2(pb_factor)`), disables AQ/MB-tree and zeroes bitrate; QP step ≥ 2; bitrate/VBV clipped to 2 Mbit/s. VBV/CQP conflicts are resolved with warnings (VBV ignored for CQP, CBR assumed when maxrate missing, maxrate below bitrate becomes CBR).
- Slices: max/min MBs and size nonnegative; `slice_min_mbs ≤ slice_max_mbs/2`; slice count ≤ MB rows (or thread count for sliced threads) and reset when max-mbs/size present. Blu-ray compatibility caps B-pyramid to strict, B-frames to 3, references/DPB to 6, weighted prediction to simple, disables intra refresh, forces AUD and VBR HRD, and sets `keyint_min = 1`.
- B-frames: `i_bframe ≤ min(BFRAME_MAX, keyint_max-1)`, bias -90…100, pyramid disabled for ≤1 B, b-adapt none/fast/trellis, no B disables adaptive/direct/bipred/open-GOP; intra refresh conflicts (pyramid normal, refs > 1, open GOP) downgrade/disable features. FPS defaults 25/1; `keyint_min` auto = `min(keyint_max/10, fps)` clipped; lookahead capped by `max(keyint_max, bufsize*fps)`; timebase defaults to reciprocal FPS when not VFR/pulldown.
- qcompress ∈ [0,1]; MB-tree disabled for all-intra or qcompress 1, and for lookaheadless finite-GOP (warning); 2-pass forces lookahead 0. Sync lookahead defaults to B-frames+1, disabled for 2-pass/single-thread. Deblocking offsets ∈ [-6,6], deadzones 0…32, cabac init idc 0…2, CQM preset validated, ME method/valid range (me_range 4…1024, capped 16 for hex and below), trellis 0…2, AQ mode 0…3 and strength 0…3 (strength 0 disables AQ), MB-tree forces AQ on at zero strength. Psy parameters fixed up and clipped 0…10; chroma QP offset initially -32…32, then +6 for 4:4:4 with psy, minus 1/2 for psy-RD/trellis, finally -12…12; subme ≥ 10 requires trellis 2 and AQ. Level auto-detection iterates the level table using `x264_sps_init` and `x264_validate_levels` with a doubled maxrate guess; explicit level must exist; MV range defaults from the level table (halved for interlaced) else clips to 32…8192.
- Lookahead-thread auto count uses a `lookahead_thread_div[2][5][4]` heuristic indexed by trellis/adaptive-B, quantised subme and B-frames, capped by frame height/128 rows. Frame-thread MV range reserves half the available vertical space divided among threads, rounded up to whole MB rows via `X264_THREAD_HEIGHT`. NAL HRD requires VBV; CBR HRD requires constant bitrate and enables filler. Finally all boolean parameters are BOOLIFIED to 0/1.

### 5.2 Open (`x264_encoder_open`)

Allocate and zero `x264_t`, copy the parameter structure, deep-copy string fields (CQM file, dump path, stats paths, zones, OpenCL binary), and free the caller's parameter if it carries a `param_free`. Store the API pointer. Detect CPU, validate parameters, parse custom CQM file, reduce FPS/timebase fractions. Initialise `i_frame = -1`, `frame_num = 0`, `idr_pic_id` (0 normally; 5/3/4 cycling for AVC-Intra). Reject a timebase denominator whose doubled value exceeds 32-bit. Set aspect ratio, SPS init + scaling lists, PPS init, validate levels, build the chroma QP table, initialise CQM. Compute MB dimensions and chroma shifts; adaptive MBAFF only when subme > 0.

Frame delay setup: trellis B-adapt gives `max(bframes,3)*4`, otherwise `bframes`; MB-tree/VBV raise it to `lookahead`; then add thread-frame count - 1, sync lookahead, and VFR flag. `i_slicetype_length` is captured before thread additions. `frames.i_bframe_delay` is 2 with pyramid, 1 with B-frames. Determine `b_have_lowres` (ABR/CRF, adaptive B, scenecut, MB-tree or weightp; plus stats-read with VBV). Allocate frame pools and cost tables; initial prev-duration from FPS/timebase; initialise RDO. Initialise all CPU DSP function tables (predict, pixel, DCT, zigzag, MC, quant, deblock, bitstream) and CABAC or CAVLC; select SATD-vs-SAD comparisons based on subme/lossless; per-chroma-format deblock/predict functions. Run a CLZ self-test to detect miscompiled binaries. Size the output bitstream as `clip(width*height*4*0.95^qp, 1e6, INT_MAX/3)` and the NAL buffer as `1.5*bitstream + 68`. Create thread pools, allocate thread contexts with their own bitstreams, NAL arrays and macroblock caches, init OpenCL if requested, init lookahead, allocate per-thread MB data, init rate control, optionally create the reconstructed-YUV dump file. Log profile/level/subampling/bit depth and return the context.

### 5.3 Slice headers and NAL emission

`slice_header_init` fills defaults: first/last MB, PPS id, frame_num, MBAFF/field flags, IDR pic id, POC placeholders, direct-mode selection (auto mode compares accumulated spatial vs temporal direct scores when list0 ref0 POC matches; otherwise spatial), active reference count override, ref-list reorder flags with `idc = diff > 0` and `arg = (|diff|-1) mod 2^log2_max_frame_num`, CABAC init idc, `qp_delta = QP - pic_init_qp`, and deblocking filter idc (disabled when effective QP ≤ 15 or filter off; idc 2 for sliced threads; alpha/beta offsets doubled).

`slice_header_write` serialises first MB (with an MBAFF index transform), `slice_type + 5` (ue), PPS id, frame_num masked to `log2_max_frame_num`, field flags, IDR id, POC LSB and optional bottom delta, redundant pic cnt, direct flag, ref-count override, ref-list reorder commands terminated by idc 3, weighted-pred table (denominators, per-ref luma/chroma flags, scale/offset se; explicit weighted bipred is a TODO), dec-ref-pic marking (IDR: two zero flags; non-IDR: MMCO type-1 commands and terminator), CABAC init idc for non-I, slice QP delta, deblocking idc and offsets.

`nal_start` records NAL ref-idc/type, sets a long start code flag and points payload at the current bit position; `nal_end` computes payload length, writes 64 bytes of 0xFF past the end (so the SIMD escape routine can over-read safely), invokes `nalu_process` callback when present, increments the NAL count and grows the NAL array if needed. `encoder_encapsulate_nals` sizes the destination for worst-case emulation prevention (`payload*1.5 + 4*nal_count + 68 + padding`), reallocates and rebases existing NAL pointers, then calls `x264_nal_encode` for each NAL (long start codes for the first NAL, SPS, PPS, AVC-Intra or Annex-B output). `x264_encoder_headers` writes SPS (highest priority), PPS, and version SEI in one bitstream, encapsulates them and returns the byte count.

### 5.4 Reconfiguration

`encoder_try_reconfig` copies only safe-to-change fields (reference count, B bias, scenecut threshold if enabled, deblocking, frame packing, HDR metadata, analyse inter/intra/direct/me_range/noise/subme/trellis/chroma-me/dct-decimate/fast-pskip/mixed-refs/psy, crop, me method when not ESA, transform 8x8 if enabled, pyramid if >1 ref, slice controls, tff, VBV/CRF/bitrate with `rc_reconfig` flags), then revalidates. VBV cannot be turned on if it was off; CRF/bitrate changes mark `rc_reconfig`. `x264_encoder_reconfig_apply` applies `mbcmp_init`, refreshes reconfigurable SPS fields and calls `x264_ratecontrol_init_reconfigurable` when needed. `x264_encoder_reconfig` applies against a scratch context `reconfig_h` so a failed validation leaves state untouched; success sets `h->reconfig = 1` for deferred application at the next frame. `x264_encoder_parameters` copies the active thread's parameters out, clearing `opaque`.

### 5.5 Reference management and filtering

`reference_check_reorder` flags list reordering when any reference is corrupt or when list order violates the default (P: decreasing frame_num; B list0: increasing POC; B list1: decreasing POC). `reference_build_list` splits reference frames into list 0 (POC < current) and list 1 (POC > current), skips corrupt frames, optionally sorts list 0 for MMCO removals and emits those commands, sorts both lists by `reference_distance` from the current frame (frame-packing type 5 uses pair-aware distance), caps list lengths to `max_ref0/max_ref1/frame_reference`, limits B references to the minigop for Blu-ray, and for P frames with weighted prediction inserts duplicate references (blind duplicate, offset-reduced duplicate) using `weighted_reference_duplicate` (Smart weightp only; high bit depth only for unweighted dupes). `weighted_pred_init` sets per-ref weighted planes, suppresses identity weights, computes common denominators, pre-scales reference planes (single-thread case), and fills missing chroma weights. `reference_update` applies prior MMCO removals, pushes the current decoded frame into the reference list and evicts the oldest beyond `num_ref_frames`; `reference_reset` clears all references and resets POC on IDR; `reference_hierarchy_reset` removes stale B-refs/open-GOP frames via MMCO and reserves DPB room for delayed B display (`i_mmco_remove_from_end`).

`fdec_filter_row` debocks rows (deferred until needed), copies deblocked pixels between planes for interlaced, expands borders and generates half-pel filtered frames when the frame is a reference, with a three-pass protocol for sliced threads (encode pass, post-encode pass, final cross-slice pass), swaps MBAFF border backups, signals frame/slice condition variables with completion row offsets, and accumulates PSNR/SSIM quality metrics with a pixel offset to avoid SSIM/DCT block alignment.

### 5.6 Slice writing and re-encode control

`slice_write` is the core macroblock loop. It realigns the bitstream, starts a NAL, writes the slice header, aligns to a byte, initialises CABAC contexts (QP clipped to 0…51 plus init idc) or CAVLC state, and walks MBs in raster (or MBAFF pair) order. Per row it checks/grows the bitstream buffer and saves a full VBV row checkpoint. Per MB it saves light checkpoints for CAVLC overflow / slice-max-size / slice-min-mbs, optionally picks interlaced MB mode via `x264_field_vsad`, loads neighbour caches, runs `x264_macroblock_analyse` (mode decision, described elsewhere) and `x264_macroblock_encode`, then writes the macroblock: CABAC emits a terminal, skip flag and full macroblock syntax; CAVLC accumulates skip runs and writes `ue(skip_run)` plus the MB syntax, re-encoding at a higher QP if a level-code overflow is flagged. Escape-byte counts (00 00 ≤3) reduce the slice-max-size budget. If the slice exceeds max size, the writer rolls back to a checkpoint, ends the slice early (respecting slice-min-mbs, with a warning if impossible) and re-encodes. After cache save, `x264_ratecontrol_mb` may return -1 (VBV/MinCR row failure), which triggers a full row rollback and re-encode from the start of the row. Statistics accumulate MB types, partitions, reference usage, CBP, 8x8dct, prediction modes and field flags; deblock strength is computed. At slice end, CAVLC writes the final skip run and RBSP trailing bits, CABAC flushes; NAL bookkeeping completes and misc bits are derived from total bits minus MV/texture bits; hpel/quality passes and condition broadcasts run at the last thread slice.

`slices_write` loops over slices for a thread, computing slice boundaries from `slice_max_mbs`, `slice_count` (with a rounding bias) or the thread's MB range, invoking `slice_write` per slice. `threaded_slices_write` divides rows among threads (round-biased), syncs contexts, distributes rate control via `x264_threads_distribute_ratecontrol`, runs `slices_write` on the thread pool, waits for the deblock pass, merges rate control and NAL/stat results deterministically. `thread_sync_context` increments/releases frame references and copies all fields from `i_frame` up to the per-thread MB state, including parameters and statistics, so frame threads see a consistent predecessor state.

### 5.7 Frame-encode control flow (`x264_encoder_encode`)

With frame threading, advance `i_thread_phase` cyclically, sync the new context from the previous frame and the oldest frame's rate control via `x264_thread_sync_ratecontrol`, and select `thread_current`/`thread_oldest`. Clear output NALs.

For `pic_in != NULL`: refuse if the lookahead thread has exited; obtain an unused frame, copy the picture, expand borders mod 16 if needed, stamp `i_frame`, track first/largest PTS (warn on non-monotonic), resolve pic_struct (auto → progressive or top/bottom-first based on interlacing), apply MB-tree read or AQ offsets (freeing caller quant_offsets), initialise lowres data if required, and enqueue via `x264_lookahead_put_frame`. If the delay has not been filled, return with `pic_out->i_type = AUTO` and zero NALs. For `pic_in == NULL` (flush), signal the lookahead thread to exit.

Then: increment `i_frame`, obtain decided frames from lookahead, and if none remain call `encoder_frame_end` to drain. Otherwise shift the next frame into `fenc`, apply deferred/per-frame reconfiguration, init zones, update references (possibly forcing an IDR if no valid references remain), set keyframe/IDR state, POC = `2*(frame - last_idr)`, and choose NAL type/ref-idc/slice type from the frame type (IDR resets references; I/P call hierarchy reset; B-ref uses high or low priority depending on pyramid; disposable B has lowest priority). A frame is kept as reference when ref-idc != disposable and keyint > 1. Copy PTS/DTS (with B-frame reorder delay via a ring of previous reordered PTS). Build reference lists, init the bitstream (per-thread for slice threads), emit AUD, SPS/PPS on keyframes (with AVC-Intra padding to magic lengths), HRD buffering-period SEI (at `encoder_frame_end` for frame threads), extra user SEIs, version SEI on the first frame, recovery-point SEI for non-IDR keyframes, HDR/lighting/transfer SEIs, frame-packing SEI, pic-timing SEI, and Blu-ray dec-ref-pic-marking SEI. Update intra-refresh PIR columns. Call `x264_ratecontrol_start` with the accumulated header overhead; set global QP and `pic_out->i_qpplus1`. Optionally reorder references optimally from 2-pass stats. `slice_init` builds the slice header and per-slice MB state; initialise bipred and weighted pred; increment frame_num for reference frames. Dispatch slices (frame threadpool, sliced-thread path, or synchronous). Finally call `encoder_frame_end`.

### 5.8 Frame end, output and statistics

`encoder_frame_end` waits for frame-thread completion, and for frame threading with HRD inserts the buffering-period SEI at the correct position (after AUD/SPS/PPS, before other SEIs) using a `memmove` on the NAL array. It encapsulates NALs, fills `pic_out` (type, keyframe, pic_struct, PTS/DTS with a warning if PTS < DTS, opaque, reconstructed image planes/strides), releases `fenc`, calls `x264_ratecontrol_end` to get filler bytes, emits filler NALs (splitting to respect slice-max-size, or appended as zero bytes to the last slice for AVC-Intra with MP4 size-header fixup), publishes `*pp_nal`/`*pi_nal`, updates noise reduction and statistics (frame counts/sizes/QP/PSNR/SSIM per slice type, MB type/partition/CBP/prediction-mode histograms, direct-mode scores, consecutive B distribution), logs the per-frame debug line, and syncs stats back to thread 0 and the next thread. `x264_encoder_intra_refresh` sets a queued-refresh flag; `x264_encoder_invalidate_reference` marks reference frames (and current fdec) corrupt by PTS and rejects use with B-frames or intra refresh.

`x264_encoder_close` deletes lookahead/OpenCL, waits for active threads, deletes thread pools, syncs rate control, prints the full summary (per-type averages, consecutive B percentages, MB-type/partition histograms, intra prediction distributions, weighted-P and reference usage, PSNR/SSIM, bitrate), then deletes rate control, CQM, buffers, frame lists, duplicate frames, MB caches and thread contexts. `x264_encoder_delayed_frames` counts active frame threads, queued current frames and all lookahead queues under mutexes; `maximum_delayed_frames` returns `frames.i_delay`.

### 5.9 Determinism and ordering constraints

Several ordering rules are essential for reproducible output:

- The slice header is written **before** macroblock data, and its QP is set from the first macroblock's QP so CABAC context initialisation matches the real first QP.
- CABAC requires a byte alignment after the header; CAVLC does not. CABAC macroblock decisions are separated by a terminal bit except the first, and skip flags are emitted by the frame writer rather than by `cabac.c` itself.
- Emulation-prevention bytes must be counted in slice-max-size accounting; the writer scans the produced bytes for `00 00 ≤3` sequences and reduces the remaining budget by 8 bits each.
- Frame threads advance in a fixed cyclic phase; each new context copies from the immediately previous and immediately oldest contexts in that order, so rate-control and reference state are independent of thread scheduling.
- Slice threads write NALs into per-thread bitstreams that are concatenated in thread index order; statistics are summed in the same order.
- Buffering-period SEI for frame threading is generated at `encoder_frame_end` and inserted into the NAL array before all other SEIs, after AUD/SPS/PPS, so its position is deterministic regardless of which thread finished first.

---

## 6. Dependencies

Calls nearly every other module: `set.c` (SPS/PPS/SEI/filler), `ratecontrol.c`, `slicetype.c`, `lookahead.c`, `macroblock.c` (cache/analyse/encode/slice init/deblock strength), `cabac.c`/`cavlc.c` (writers), `me.c`/`analyse.c` (cost init, weight frame, field VSAD), `common/` (DSP init, frames, bitstream, threading, CPU detection, tables, CQM), and `api.c` (the `api` pointer for callbacks). It is itself the implementation target of `api.c`'s dispatch.

---

## 7. Constants and tables

- `MAX_RESOLUTION = 16384`; broken-ffmpeg default score threshold 5 (nine heuristics).
- Bitstream size `clip(w*h*4*0.95^qp, 1e6, INT_MAX/3)`; NAL buffer `1.5*size + 68`; escape detection `00 00 ≤ 3`; 64-byte over-read padding.
- AVC-Intra lookup `avcintra_lut[5][2][7]` (fps, interlaced, frame size, CQM pointers); IDR pic-id cycle 5→3→4→5; slice padding 256 bytes (Sony: +18 or +10 × 512); VANC lengths 5780/2900 and 17/9 × 512.
- Thread auto factor `CPUs * (sliced?2:3)/2`, caps two/four rows per thread; `lookahead_thread_div[2][5][4]`; MV-range thread reserve `X264_THREAD_HEIGHT`.
- Delay formula `max(bframes,3)*4` (trellis) else `bframes`, plus threads-1 + sync lookahead + VFR.
- QP factor `6*log2(ip_factor/pb_factor)`; SLICE header `slice_type+5`; ref-list `arg = (abs(diff)-1) mod 2^log2_max_frame_num`; PIR increment `max((mb_width-1)/keyint, 1)`.
- `overhead_guess = NALU_OVERHEAD - (annexb && nal_index) + 1 + cabac + 5`; filler overhead `FILLER_OVERHEAD - annexb`; `NAL_PRIORITY_*` levels.

---

## 8. Performance notes

This file orchestrates hot paths; most per-MB cost is in `macroblock.c`/`analyse.c`/entropy writers, but several here matter.

Opportunities, with honesty:
- **Frame threading**: overlapping frames is the main throughput lever; determinism requires the exact `thread_sync_context`/`x264_thread_sync_ratecontrol` copy sets — omitting fields changes the bitstream. Frame threading changes latency (`frames.i_delay`) and disables `nalu_process`.
- **Slice threading / lookahead threads**: `threaded_slices_write` and `slicetype_frame_cost`'s row split parallelise within a frame; boundary handling (hpel passes, condition variables) is subtle and must be preserved to avoid race-induced nondeterminism.
- **Re-encode paths**: CAVLC overflow, slice-max-size, slice-min-mbs and row-VBV rollbacks cost extra encoding of a row/MB; they are required for compliance and cannot be removed without changing output. Avoiding them by choosing safer QPs changes rate/quality.
- **Bitstream buffer growth**: sized generously up front from resolution/QP to avoid frequent reallocations; correct emulation-prevention padding must be kept.
- **Header caching**: SPS/PPS/SEI bytes depend on few parameters; caching the serialized blobs is a pure startup win with no output change.
- **Validation**: runs only at open/reconfig; can be made table-driven but has no steady-state cost.

SIMD is selected once at open via the DSP init functions; `mbcmp_init` and `chroma_dsp_init` pick SATD vs SAD and chroma variants — a clean implementation should preserve those selections for bit-exactness. `b_cpu_independent` disables x9 intra comparisons and certain SIMD paths to stabilise cross-CPU output.

---

## 9. References

- ITU-T H.264 Annex B (slice header syntax, NAL unit syntax and emulation prevention), Annex D (SEI), Annex A (profiles/levels).
- x264 public API (`x264.h`) contracts for encode/flush/reconfig/delayed frames.
- pthreads/threadpool design; deterministic multi-threaded video encoding literature.

---

## 10. Verification checklist

- Validate parameter rejection/derivation with boundary cases (resolution, crop, SAR, threads, slices, VBV/CQP conflicts, AVC-Intra matrix, ffmpeg defaults).
- Compare emitted SPS/PPS/slice-header bytes against a known-good encoder for all slice types, MBAFF, B-pyramid and ref-list reorder cases.
- Confirm NAL encapsulation: start-code lengths, emulation prevention on `00 00 00/01/02/03`, AUD/SPS/PPS/SEI ordering, filler NALs and no buffer overruns (valgrind).
- Flush test: submit N frames then `pic_in = NULL`; verify all delayed frames are emitted and `delayed_frames` returns 0.
- Determinism: encode with 1 vs N frame threads and with sliced threads; compare final bitstream byte-for-byte (with `b_cpu_independent` if cross-machine).
- Exercise all re-encode triggers: CAVLC level overflow at low profile, slice-max-size/min-mbs, VBV row re-encode; confirm streams remain valid.
- Verify reference invalidation and intra refresh under error conditions.
- Check `x264_encoder_parameters` reflects applied reconfigs and that failed reconfigs leave state unchanged.
- Verify final statistics and PSNR/SSIM values against a reference implementation for a short clip.

---

## 11. Open questions

- `X264_THREAD_HEIGHT`, `NALU_OVERHEAD`, `FILLER_OVERHEAD`, `SEI_OVERHEAD`, `X264_LOOKAHEAD_THREAD_MAX`, `X264_REF_MAX` exact values are defined in headers not shown; they affect slicing/encapsulation sizes.
- The full `validate_parameters` rule set is extremely long; only the substantive rules are described here — a clean implementation must mirror all clipping/derivation to be bit-identical.
- Per-frame `pic_in->param` reconfiguration semantics are applied through `x264_encoder_reconfig_apply`; any frame-type-specific restrictions beyond those listed are UNKNOWN.
