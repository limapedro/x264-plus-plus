# `encoder/slicetype-cl.h`

**Role:** Declares the OpenCL-accelerated low-resolution lookahead helpers used by slice-type decision code.

> **Dirty-side behavioural specification.** Describes observable behaviour and structure only; no source text or near-verbatim pseudocode is reproduced.

---

## 1. Header

- Source path: `encoder/slicetype-cl.h`
- Role: public declarations for GPU-side intra cost estimation, hierarchical motion search, mode selection/cost finalisation and queue flushing.
- Marker: dirty-side behavioural specification.

---

## 2. Purpose

When built with OpenCL support, x264 can offload the expensive low-resolution (8x8-per-MB) lookahead analysis — intra cost, motion estimation and inter mode cost accumulation — to a GPU. This header exposes the operations the CPU-side slice-type decision code calls to prepare, execute and retrieve those costs. All functions are no-ops or trivially return when OpenCL is unavailable; the implementations live in `slicetype-cl.c` behind `HAVE_OPENCL`.

---

## 3. Public interface

- `int x264_opencl_lowres_init( x264_t *h, x264_frame_t *fenc, int lambda )` — lazily allocate GPU buffers and enqueue downscaling, intra-mode cost and row-sum kernels for one frame; returns nonzero on OpenCL error. Guarded by `fenc->b_intra_calculated`.
- `int x264_opencl_motionsearch( x264_t *h, x264_frame_t **frames, int b, int ref, int b_islist1, int lambda, const x264_weight_t *w )` — run hierarchical half-pel motion search of frame `b` against `ref` (list 0 unless `b_islist1`); `w` optionally supplies weighted-prediction parameters; writes lowres MVs/costs back asynchronously.
- `int x264_opencl_finalize_cost( x264_t *h, int lambda, x264_frame_t **frames, int p0, int p1, int b, int dist_scale_factor )` — choose the cheapest prediction mode per MB (including bidirectional for B frames), sum costs per row with AQ scaling, and schedule the results for copy-back.
- `int x264_opencl_precalculate_frame_cost( x264_t *h, x264_frame_t **frames, int lambda, int p0, int p1, int b )` — ensure the cost matrix entry `[b-p0][p1-b]` is computed, running the needed searches; returns 1 when work was queued, 0 when already available or trivial.
- `void x264_opencl_flush( x264_t *h )` — block until queued kernels complete and apply all deferred host copies.
- `void x264_opencl_slicetype_prep( x264_t *h, x264_frame_t **frames, int num_frames, int lambda )` — prepare all frames (intra costs) and, for trellis B-adapt, exhaustively precompute motion searches.
- `void x264_opencl_slicetype_end( x264_t *h )` — restore any temporary thread-priority changes made in `prep`.

All symbols are bit-depth-template-generated via `x264_template`.

---

## 4. State and data structures

The header defines no structures. It assumes `x264_t.opencl` holds the device/context/command queue, function table (`x264_opencl_function_t`), shared buffers (image pyramid, MV buffers, cost buffers, frame stats, row SATDs), page-locked staging memory and a deferred-copy list; and that `x264_frame_t.opencl` holds per-frame GPU buffers (half-pel luma, scaled image pyramid, inverse-QP map, intra cost, per-reference MV and MV-cost arrays). `x264_frame_t` also carries low-res costs, cost estimates, row SATDs and sentinel MV values used by the CPU decision code.

---

## 5. Algorithm / behaviour

The interface is used in a fixed sequence: `slicetype_prep` at the start of a lookahead batch, then `precalculate_frame_cost` (or the lower-level `lowres_init`/`motionsearch`/`finalize_cost`) for each candidate (p0, p1, b) triple, `flush` when results are needed on the host, and `slicetype_end` at the batch end. Kernels are enqueued asynchronously; results are staged in page-locked host memory and copied into frame fields only at `flush`, so CPU and GPU work overlap. On any OpenCL error the encoder disables OpenCL and marks a fatal error so subsequent calls fail fast. See `slicetype-cl.c.md` for the full behaviour.

---

## 6. Dependencies

Included by `encoder/slicetype-cl.c` (definitions) and `encoder/slicetype.c` (call sites). Depends on `common/common.h` for the opaque types and `common/base.h` for `x264_template`.

---

## 7. Constants and tables

No constants here; the implementation uses `NUM_IMAGE_SCALES`, `PAGE_LOCKED_BUF_SIZE`, `MAX_FINISH_COPIES`, `LOWRES_COST_MASK`, `LOWRES_COST_SHIFT` and the MV sentinel `0x7FFF` from `slicetype-cl.c`/`slicetype.c`.

---

## 8. Performance notes

This interface exists purely for performance: GPU lookahead can be substantially faster than CPU lowres analysis at slow presets, but adds queue latency and is only beneficial for sufficiently large frames/batches. Determinism requires that GPU kernels produce the same costs as the CPU path; a clean implementation that omits OpenCL must fall back to the CPU algorithms and will produce equivalent decisions but slower. Thread-priority tweaks on Windows are a driver-latency workaround, not algorithmic.

---

## 9. References

- OpenCL 1.x API specification (`clCreateBuffer`, `clCreateImage2D`, `clEnqueueNDRangeKernel`, command queue info).
- x264 public documentation on `--opencl`.
- General hierarchical motion estimation and SATD-based lookahead literature.

---

## 10. Verification checklist

- With OpenCL disabled, confirm all entry points are inert or return immediately and produce no different output.
- With OpenCL enabled, compare slice-type decisions and final bitstream against the CPU path for the same input (should match closely; exact equality depends on kernel/CPU cost parity).
- Confirm a fatal OpenCL error disables further use without crashing.

---

## 11. Open questions

- Whether additional OpenCL helpers exist in newer x264 revisions is UNKNOWN; this reflects the reference snapshot.
