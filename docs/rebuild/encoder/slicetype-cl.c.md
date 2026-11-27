# `encoder/slicetype-cl.c`

**Role:** OpenCL implementation of the low-resolution lookahead: frame downscaling, intra cost estimation, hierarchical motion search, mode selection and cost accumulation on a GPU.

> **Dirty-side behavioural specification.** Describes observable behaviour and structure only; no source text or near-verbatim pseudocode is reproduced.

---

## 1. Header

- Source path: `encoder/slicetype-cl.c`
- Role: enqueue and manage GPU kernels that compute the same lowres costs the CPU lookahead uses for slice-type decisions; stage results back into `x264_frame_t`.
- Marker: dirty-side behavioural specification. Entire implementation is conditional on `HAVE_OPENCL`.

---

## 2. Purpose

Slice-type decision needs per-MB intra and inter cost estimates for many candidate (past, future, current) frame triples. This is compute-heavy but highly parallel. OpenCL offloads it to the GPU: images are downscaled to an 8x8-per-MB pyramid, intra costs and row SATDs are computed once per frame, hierarchical motion search yields MVs/costs per reference, and a mode-select kernel chooses the cheapest prediction and fills the cost matrices. Results are copied back asynchronously in batches to hide latency.

---

## 3. Public interface

Implemented functions (see `slicetype-cl.h.md`): `x264_opencl_flush`, `x264_opencl_lowres_init`, `x264_opencl_motionsearch`, `x264_opencl_finalize_cost`, `x264_opencl_precalculate_frame_cost`, `x264_opencl_slicetype_prep`, `x264_opencl_slicetype_end`. Static helpers: `opencl_alloc_locked`, `optimal_launch_dims`. `x264_weights_analyse` is declared locally for weighted-prediction analysis.

---

## 4. State and data structures

- Shared GPU buffers (allocated once): a read-write single-channel 32-bit half-pel luma image; a pyramid of RGBA 8-bit scaled images (one per `NUM_IMAGE_SCALES`, halving each level); cost and MV buffers (`lowres_mv_costs`, two `lowres_costs`, two double-length `mv_buffers`, `mvp_buffer`); two write-only 4-int `frame_stats`; two write-only per-MB-row `row_satds`; and two read-only full-resolution luma buffers (double-buffered via `last_buf`).
- Per-frame GPU buffers: half-pel luma image, scaled image pyramid, read-only inverse-QP factor buffer, write-only intra cost buffer, and per-list MV/MV-cost arrays dimensioned by `i_bframe + 1` references.
- Host staging: a page-locked buffer with occupancy tracking, and a deferred-copy list `{src, dest, bytes}`. `flush` calls `clFinish` then applies all copies. `MAX_FINISH_COPIES` and `PAGE_LOCKED_BUF_SIZE` trigger early flushes.
- Sentinel `0x7FFF` in `lowres_mvs` marks "not yet computed"; costs `>= 0` mark "already computed".

Invariants: a frame's intra costs must be initialised before motion search/mode selection; results are only valid on the host after `flush`; the double-buffer index `last_buf` toggles after each enqueued cost batch.

---

## 5. Algorithm / behaviour

**Error handling (`OCLCHECK`).** Before each call, if a fatal error flag is set, return -1 immediately. On any non-success status, clear `b_opencl`, set the fatal flag, log the failing method and status, and return -1.

**Staging (`opencl_alloc_locked`).** If staging occupancy plus the requested bytes would reach the page-locked buffer size, flush first; then carve `bytes` from the staging pointer and advance occupancy.

**Lowres init (`x264_opencl_lowres_init`).** No-op if the frame already has intra costs computed. Allocate shared buffers on first use and per-frame buffers on first use of that frame; any allocation failure disables OpenCL. Upload the luma plane to the GPU (through staging) and either upload the AQ inverse-QP factors or fill them with the no-op value 256 via a memset kernel. Enqueue the downscale+half-pel kernel over `8*mb_width × 8*mb_height` work items, then progressively downscale through the image pyramid using two alternating kernels (an AMD Southern Islands driver workaround — alternating instances prevents a dependency bug), stopping when a dimension falls below 16. Enqueue the intra kernel over a 32×8-work-item grid at scale 0; it checks all 10 intra modes for slow presets (`i_subpel_refine > 7`) and 8 modes otherwise, weighted by lambda. Enqueue a row-sum kernel (256×1 work items) to accumulate row SATDs and frame statistics, applying AQ weights. Schedule asynchronous read-backs of intra costs, row SATDs and two frame statistics (plain cost and AQ-weighted cost) into the frame's `lowres_costs[0][0]`, `i_row_satds[0][0]`, `i_cost_est[0][0]` and `i_cost_est_aq[0][0]`. Toggle `last_buf`.

**Launch geometry (`optimal_launch_dims`).** Query the kernel's maximum work-group size, preferred multiple and device compute-unit count (defaults 256/64/6). Set local dims to `(preferred_multiple, 8)`, adjust so the second dimension evenly divides the global second dimension and the product fits the work-group limit, round the global x dimension to a multiple of the local x, then shrink local dims until there are at least about twice as many work groups as compute units; a special case for 6-CU devices reduces a 64×4 group to 32×4 to avoid texture-cache abuse.

**Motion search (`x264_opencl_motionsearch`).** If weighted prediction is requested, run kernels that apply the weight/offset/scale/denominator to the reference's scaled images and half-pel luma, and use those weighted planes. Otherwise use the reference's own planes. Search hierarchically from the coarsest scale down to scale 0; per-scale iteration counts are `{1,1,2,3}`; the motion range is `i_me_range >> scale`; the MV predictor buffer and alternating MV buffers implement progressive refinement, with top-left vs bottom-right reference selection alternated at coarse scales (disabled at finer scales). Then run a subpel-refine kernel at scale 0, writing MVs and costs into the requested list (list 1 when `b_islist1`) at the reference index offset. An AMD Southern Islands scheduling workaround performs a small dummy buffer copy. Schedule asynchronous read-back of the MV block into `frames[b]->lowres_mvs[list][ref_offset]`.

**Cost finalisation (`x264_opencl_finalize_cost`).** Compute the bidirectional prediction weight as `64 - (dist_scale_factor >> 2)` when weighted bipred is enabled, else 32. The mode-select kernel, for each MB, picks the minimum-cost prediction among intra, reference 0, reference 1 (and bidirectional combinations for B frames, hence four work items per MB), stores `min(cost, LOWRES_COST_MASK) | (list_used << LOWRES_COST_SHIFT)` into a cost buffer, and accumulates frame statistics. A row-sum kernel then sums costs with AQ inverse-QP weights and the B-frame bias, producing row SATDs and final frame costs. Schedule read-backs into `lowres_costs[b-p0][p1-b]`, `i_row_satds[b-p0][p1-b]`, `i_cost_est[b-p0][p1-b]`, `i_cost_est_aq[b-p0][p1-b]`, and for P frames `i_intra_mbs[b-p0]`.

**Batch preparation (`slicetype_prep` / `slicetype_end`).** On Windows, temporarily raise the current thread's priority and, when AMD's `CL_QUEUE_THREAD_HANDLE_AMD` is exposed, the OpenCL driver thread's priority, restoring both in `slicetype_end`. Precompute intra costs (hence lowres init) for every frame in the batch, then flush. For trellis B-adapt, exhaustively precompute forward and backward motion searches for every (b, ref) pair whose MV sentinel is still unset, analysing weights when weighted prediction is enabled, then flush.

**On-demand cost (`precalculate_frame_cost`).** If the requested cost entry is already computed, or the triple is trivial (`b == p0 == p1`), return 0. Otherwise mark the entry as 0 to prevent duplicate work, decide which search directions are needed from the MV sentinels, compute `dist_scale_factor = (((b-p0)<<8) + ((p1-p0)>>1)) / (p1-p0)` (a weighted average of the two distances scaled by 256; 128 when p1 == p0), run lowres init for b and the needed references, the required motion searches, then `finalize_cost`; return 1.

---

## 6. Dependencies

Calls OpenCL API functions through `h->opencl.ocl` (`clCreateBuffer`, `clCreateImage2D`, `clSetKernelArg`, `clEnqueueNDRangeKernel`, `clEnqueueReadBuffer`, `clEnqueueWriteBuffer`, `clEnqueueCopyBuffer`, `clFinish`, work-group/device queries), `x264_weights_analyse` for WeightedP, and `x264_log`. Called from `slicetype.c` around `x264_slicetype_frame_cost`/trellis B-adapt.

---

## 7. Constants and tables

- `CL_QUEUE_THREAD_HANDLE_AMD = 0x403E` — AMD-specific command-queue handle query.
- Launch defaults: max work group 256, preferred multiple 64, compute units 6; local dims start `(64,8)`; stop downscaling at dimensions < 16.
- Per-scale ME iterations `{1,1,2,3}`; MV sentinel `0x7FFF`; AQ no-op factor 256.
- `dist_scale_factor` default 128; bipred weight `64 - (dsf>>2)` or 32.

---

## 8. Performance notes

This entire module is a performance optimisation: moving lowres lookahead to a GPU can greatly accelerate slow presets on capable hardware.

Honest trade-offs:
- **Transfer/latency**: staging through page-locked memory and deferred copies hides latency, but very small frames or short lookahead windows may not amortise kernel-launch overhead; the CPU path can be faster.
- **Driver workarounds**: alternating downscale kernels and a dummy copy exist for specific AMD driver bugs; they cost little but must be preserved for correctness on those devices.
- **Determinism**: kernel arithmetic must match the CPU lowres path closely enough that slice-type decisions are identical; a clean implementation without OpenCL uses the CPU path and produces equivalent output at the cost of speed.
- **Priority boosting** is Windows-only latency mitigation; removing it affects responsiveness, not output.
- No SIMD concerns here; GPU kernels are the acceleration.

---

## 9. References

- OpenCL 1.x specification (memory objects, images, kernels, command queues, work-group sizing).
- Hierarchical motion estimation and SATD-based lookahead (x264 lowres analysis).
- x264 public documentation on `--opencl` and lookahead.

---

## 10. Verification checklist

- Build with `HAVE_OPENCL=0`: confirm no OpenCL symbols are referenced and the CPU lookahead path is used.
- Build with OpenCL enabled and compare slice-type decisions / final bitstream against the CPU path for representative clips.
- Verify deferred copies are applied before results are read (flush at the right points), especially the frame stats and row SATDs.
- Exercise trellis B-adapt's exhaustive precompute path and confirm all (b, ref) MVs are filled (sentinel cleared).
- Confirm graceful disable on simulated OpenCL errors (no crash, warnings logged).

---

## 11. Open questions

- Kernel bodies are in OpenCL C source not included in this C file; their exact cost formulas are UNKNOWN here and must be matched to the CPU `slicetype.c` behaviour.
- Constants `NUM_IMAGE_SCALES`, `PAGE_LOCKED_BUF_SIZE`, `MAX_FINISH_COPIES`, `LOWRES_COST_MASK/SHIFT` are defined in headers not shown; their exact values are UNKNOWN but affect buffer sizing and cost packing.
