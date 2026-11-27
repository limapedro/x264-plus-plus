# `encoder/ratecontrol.h`

**Role:** Declares the rate-control interface and frame-duration normalisation constants used by the encoder.

> **Dirty-side behavioural specification.** Describes observable behaviour and structure only; no source text or near-verbatim pseudocode is reproduced.

---

## 1. Header

- Source path: `encoder/ratecontrol.h`
- Role: public declarations for rate-control lifecycle, per-frame/per-macroblock QP decisions, VBV/HRD bookkeeping, MB-tree, zone handling and frame-thread synchronisation.
- Marker: dirty-side behavioural specification.

---

## 2. Purpose

This header is the contract between the frame-encoding loop and the rate-control implementation. It exposes the complete set of hooks the encoder calls around slice decisions, macroblock encoding and frame completion, plus the constants that define the "reference" frame duration used to make bitrate control frame-rate independent.

---

## 3. Public interface

Lifecycle / configuration:

- `int x264_ratecontrol_new( x264_t *h )` — allocate and initialise the rate-control state for the chosen RC method; nonzero on failure.
- `void x264_ratecontrol_delete( x264_t *h )` — free it.
- `void x264_ratecontrol_init_reconfigurable( x264_t *h, int b_init )` — (re)initialise fields that may change via reconfiguration, including HRD/VBV parameters; `b_init` distinguishes first setup from reconfig.
- `int x264_encoder_reconfig_apply( x264_t *h, x264_param_t *param )` — apply a runtime parameter change relevant to rate control.
- `void x264_ratecontrol_zone_init( x264_t *h )` — parse/prepare per-frame qp/bitrate zones.

Analysis hooks:

- `void x264_adaptive_quant_frame( x264_t *h, x264_frame_t *frame, float *quant_offsets )` — compute AQ quant offsets for a frame (variance/edge based).
- `int x264_macroblock_tree_read( x264_t *h, x264_frame_t *frame, float *quant_offsets )` — propagate macroblock-tree weights into quant offsets.
- `int x264_reference_build_list_optimal( x264_t *h )` — choose reference list order using MB-tree info.
- `void x264_ratecontrol_set_weights( x264_t *h, x264_frame_t *frm )` — set B-frame reference weights (for weighted prediction / distortion scaling).
- `int x264_ratecontrol_slice_type( x264_t *h, int i_frame )` — return a rate-control-forced slice type for a frame (or -1 for no override).

Per-frame / per-MB QP:

- `void x264_ratecontrol_start( x264_t *h, int i_force_qp, int overhead )` — begin a frame, given a forced QP if any and a known header bit overhead.
- `int x264_ratecontrol_mb( x264_t *h, int bits )` — after encoding a macroblock, return the new QP (may adapt within a row when aq/ratecontrol variants require it).
- `int x264_ratecontrol_qp( x264_t *h )` — current frame QP.
- `int x264_ratecontrol_mb_qp( x264_t *h )` — per-macroblock QP (with AQ offsets).
- `int x264_ratecontrol_end( x264_t *h, int bits, int *filler )` — complete a frame; returns a status and may set the number of filler bytes needed to satisfy VBV.
- `int x264_rc_analyse_slice( x264_t *h )` — compute a slice's bit cost estimate for lookahead (used by MB-tree/VBV).

Threading / VBV:

- `void x264_thread_sync_ratecontrol( x264_t *cur, x264_t *prev, x264_t *next )` — propagate rate-control state between frame threads to keep decisions deterministic.
- `void x264_threads_distribute_ratecontrol( x264_t *h )`, `void x264_threads_merge_ratecontrol( x264_t *h )` — split/merge per-thread rate-control accounts for slice threading.
- `void x264_hrd_fullness( x264_t *h )` — update the HRD CPB fullness model after a frame.
- `void x264_ratecontrol_summary( x264_t *h )` — print end-of-stream rate-control statistics.

All symbols are template-generated (`x264_template`, bit-depth-specialised).

---

## 4. State and data structures

The header defines no structures; all state is opaque `x264_ratecontrol_t` inside `x264_t`. It assumes `x264_t` carries `param`, `fenc`, `sps`, `mb`, `stat`, and a per-thread replicated rate-control context for frame/slice threading.

---

## 5. Algorithm / behaviour

The interface enforces this call order per frame: `start` → (per-MB `mb`/`mb_qp` during encoding) → `end`; with `slice_type`, `set_weights`, `adaptive_quant_frame` and `macroblock_tree_read` invoked earlier in lookahead/analysis. HRD fullness is updated at frame completion; filler bytes returned by `end` are emitted by `set.c`. Zones are initialised once at stream start and may be refreshed on reconfig. Frame-thread synchronisation functions are called at frame boundaries so the RDO order (and hence bitstream) is independent of how many threads run.

---

## 6. Dependencies

Included by `encoder/ratecontrol.c` (definitions) and by `encoder/encoder.c`, `encoder/lookahead.c`, `encoder/slicetype.c` (call sites). Depends on `common/common.h` for opaque types and `common/base.h` for the `x264_template` macro.

---

## 7. Constants and tables

- `BASE_FRAME_DURATION = 0.04 / ((i_frame_packing == 5) + 1)` seconds — the frame-duration centre point chosen arbitrarily; rate control lowers relative quality at higher frame rates and raises it at lower ones. Frame-packing type 5 (frame alternation) halves it to compensate for the doubled effective frame rate.
- `MAX_FRAME_DURATION = 1.00 / ((i_frame_packing == 5) + 1)`, `MIN_FRAME_DURATION = 0.01 / ((i_frame_packing == 5) + 1)`.
- `CLIP_DURATION(f)` clips `f` to `[MIN_FRAME_DURATION, MAX_FRAME_DURATION]`.
- These are described as "arbitrary limitations as a sanity check" in the reference; exact values matter for bit-exact behaviour.

---

## 8. Performance notes

Rate control is called once per frame plus once per macroblock for QP decisions, so the per-MB hook (`x264_ratecontrol_mb`/`mb_qp`) is mildly hot; the frame-level model and zone handling are negligible. With frame threading, synchronisation calls can create barriers: keeping the rate-control state replicated per thread and merging at boundaries (as the interface supports) avoids serialising all threads. A clean implementation that omits MB-tree or complex AQ can simplify these hooks substantially at some quality cost — a legitimate speed/quality trade, but it changes rate-control decisions and therefore the bitstream. No SIMD applies.

---

## 9. References

- x264 public documentation on `--crf`, `--bitrate`, `--vbv-maxrate`, `--vbv-bufsize`, `--qp`, `--aq-mode`, `--mbtree`, `--zones`, and frame packing (SMPTE).
- ITU-T H.264 Annex F.1 (HRD / CPB model) and Annex E (VUI HRD parameters).

---

## 10. Verification checklist

- Verify the three duration macros evaluate to the documented values for `i_frame_packing == 5` and other values.
- Confirm every declared function has a bit-depth-specialised definition and is callable from the encoder control flow.
- Black-box: calling `x264_ratecontrol_slice_type` with no zone override must return "no override"; with a zone forcing a type it must return that type.

---

## 11. Open questions

- Whether any rate-control entry points are added in newer revisions is UNKNOWN; this reflects the reference snapshot.
