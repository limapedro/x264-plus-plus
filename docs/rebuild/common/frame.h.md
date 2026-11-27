# `common/frame.h` - frame and plane abstractions

## 1. Header

Source path: `E:\_x264_dirty_stash\x264-source\common\frame.h` (mirrored at `common/frame.h`). Role in one sentence: defines `x264_frame_t` (the padded multi-plane picture plus all per-frame analysis/threading state), the synchronised frame list, the deblocking function table, and the frame/list/pool operations. Marker: **dirty-side behavioural specification**.

## 2. Purpose

Motion estimation and compensation need to read pixels outside the visible picture, and threaded encoding needs frames to carry completion state and reference counts. This header defines the object that satisfies both: a picture with guaranteed padding, four filtered variants of the luma plane, half-resolution copies for lookahead, motion/reference data, rate-control bookkeeping and thread synchronisation primitives. It also defines the producer/consumer list used to hand frames between the encoder and lookahead threads.

## 3. Public interface

Constants: `PADH = 32`, `PADV = 32`, `PADH_ALIGN = max(PADH, NATIVE_ALIGN/SIZEOF_PIXEL)`, `PADH2 = PADH_ALIGN + PADH`. Padding is expressed in pixels; horizontal padding includes an aligned portion so plane start addresses are SIMD-friendly.

`typedef struct x264_frame ... x264_frame_t`. Principal fields:

- Identity/timing: `base`, `i_poc`, `i_delta_poc[2]`, `i_type`, `i_forced_type`, `i_qpplus1`, `i_pts`, `i_dts`, `i_reordered_pts`, `i_duration` (SPS time-scale units), `f_duration` (seconds), `i_cpb_duration`, `i_cpb_delay`, `i_dpb_output_delay`, `param`, `i_frame`, `i_coded`, `i_field_cnt`, `i_frame_num`, `b_kept_as_ref`, `i_pic_struct`, `b_keyframe`, `b_fdec`, `b_last_minigop_bframe`, `i_bframes`, QP averages (`f_qp_avg_rc`, `f_qp_avg_aq`, `f_crf_avg`), `i_poc_l0ref0`.
- Planes: `i_csp`, `i_plane`, `i_stride[3]`, `i_width[3]`, `i_lines[3]`, lowres stride/width/lines, `plane[3]`, `plane_fld[3]`, `filtered[3][4]` (original, horizontal, vertical, HV), `filtered_fld[3][4]`, `lowres[4]`, `integral`, `buffer[4]`, `buffer_fld[4]`, `buffer_lowres`.
- Weighting: `weight[X264_REF_MAX][3]`, `weighted[X264_REF_MAX]`, `b_duplicate`, `orig`.
- Motion data: `mb_type`, `mb_partition`, `mv[2]`, `mv16x16`, `lowres_mvs[2][BFRAME_MAX+1]`, `field`, `effective_qp`, `lowres_costs[BFRAME_MAX+2][BFRAME_MAX+2]` (packed as `(lists_used<<14)+cost`, mask 0x3fff), `lowres_mv_costs`, `ref[2]`, `i_ref[2]`, `ref_poc[2][X264_REF_MAX]`, `inv_ref_poc[2]`.
- Lookahead/rate control: `i_cost_est`, `i_cost_est_aq`, `i_satd`, `i_intra_mbs`, per-row SATD/bit/QP arrays, `f_qp_offset`, `f_qp_offset_aq`, `b_intra_calculated`, `i_intra_cost`, `i_propagate_cost`, `i_inv_qscale_factor`, `b_scenecut`, `f_weighted_cost_delta`, `i_pixel_sum[3]`, `i_pixel_ssd[3]`, `hrd_timing`, planned frame-type/SATD/CPB arrays indexed by `X264_LOOKAHEAD_MAX`, lookahead clocks.
- Threading: `i_lines_completed`, `i_lines_weighted`, `i_reference_count`, `mutex`, `cv`, `i_slice_count`.
- Extras: periodic intra refresh position/columns, `b_corrupt`, `extra_sei`, `opaque`, `mb_info` + `mb_info_free`, optional `opencl`, `b_duplicate`.

`typedef struct { x264_frame_t **list; int i_max_size; int i_size; mutex; cond cv_fill; cond cv_empty; } x264_sync_frame_list_t`: a bounded FIFO-like producer/consumer queue (pop takes from the tail, push appends at the front slot).

`x264_deblock_function_t`: pointer table with luma/chroma deblock functions in both directions, intra variants, 4:2:0/4:2:2 horizontal chroma variants, MBAFF variants, and a `deblock_strength` function.

Functions (all templated): deletion, picture copy, several border-expand variants (`x264_frame_expand_border`, `_filtered`, `_lowres`, `_chroma`, `_mod16`, `x264_expand_border_mbpair`), row/macroblock deblocking, half-pel filtering, lowres init, `x264_deblock_init`, condition-variable helpers (`x264_frame_cond_broadcast/wait`, `x264_threadslice_cond_broadcast/wait`), `x264_frame_new_slice`, plain list operations (`push`, `pop`, `unshift`, `shift`), pooled-frame operations (`push_unused`, `pop_unused`, `push_blank_unused`, `pop_blank_unused`), `x264_weight_scale_plane`, `x264_frame_delete_list`, and synchronised list operations (`x264_sync_frame_list_init/delete/push/pop`). The four plain list operations and the two sync push/pop are `X264_API`.

## 4. State and data structures

A frame's memory is one allocation: `base` owns the struct-sized header block allocated first and the arena allocated by PREALLOC; all plane pointers are offsets into that arena. `orig` normally points to self; a duplicate frame is a shallow copy whose `b_duplicate` flag suppresses freeing shared pointers. `i_reference_count` counts users, not pointers: it starts at 1 for a freshly popped frame and is decremented by `push_unused`; only at zero does the frame return to the free pool.

Plane geometry invariant: `plane[p] = buffer[p] + stride[p]*padv + PADH_ALIGN`, so column -1 through -PADH are readable. When subpel refinement is active for a reconstructed frame, four luma planes are allocated consecutively with identical alignment phase, and `filtered[p][0..3]` point into them. The integral image for ESA uses a separate 16-bit buffer when ME method is ESA or better. Lowres planes are four quarter-size luma copies with their own padding.

Ownership of `param` is external: `x264_frame_delete` calls `x264_param_cleanup` and `param_free` only when `frame->param` and its `param_free` are set. `extra_sei` payloads and `mb_info` are freed through caller-supplied callbacks.

## 5. Algorithm / behaviour

`x264_frame_cond_broadcast(frame, lines)` takes the frame mutex, assigns `i_lines_completed`, broadcasts the condition variable and unlocks. `x264_frame_cond_wait(frame, lines)` locks, waits while the recorded line count is below the requested value *and* the requested value is non-negative, then unlocks and returns the observed count. A negative target therefore never waits; this is how disabled dependencies are expressed. `x264_threadslice_cond_broadcast(h, pass)` stores a pass index and broadcasts only when the pass is positive; `x264_threadslice_cond_wait(h, pass)` waits until `i_threadslice_pass >= pass`. Both use the encoder's mutex/condition variable. `x264_frame_new_slice` increments `i_slice_count` atomically when slice threads are enabled (plain increment otherwise) and returns -1 once the configured maximum is reached.

Plain list operations are simple pointer-array manipulations: `push` appends after the first null, `pop` removes the last non-null element, `unshift` shifts all elements right and inserts at index 0, `shift` removes index 0 and compacts left. `pop_unused` reuses a pooled frame or allocates a new one, then resets the per-use flags (`b_last_minigop_bframe`, `i_reference_count = 1`, `b_intra_calculated`, `b_scenecut`, `b_keyframe`, `b_corrupt`, slice-count seed, weight and weighted-cost arrays). `pop_blank_unused` returns either a pooled shallow duplicate or a bare struct with `b_duplicate = 1`.

Synchronised list operations bound the queue at `i_max_size`: `push` waits on `cv_empty` while full, stores at `list[i_size++]`, unlocks, then broadcasts `cv_fill`; `pop` waits on `cv_fill` while empty, takes `list[--i_size]`, clears that slot, broadcasts `cv_empty`, and unlocks. Broadcasts are issued after unlocking the mutex in `push` but before unlocking in `pop`; both are correct because the predicate is protected by the same mutex.

`x264_weight_scale_plane` divides the area into 16-row horizontal strips (found optimal for cache loads) and calls a width-specific weight function for 16-wide chunks, with an 8-wide tail.

## 6. Dependencies

Includes `base.h`/`common.h` types, `x264.h` picture/SEI types, `threadpool.h` for synchronised lists, and optional `opencl.h`. Implemented by `frame.c`; used by lookahead, rate control, encoder core and frame threads. `x264_deblock_init` depends on architecture-specific deblocking implementations.

## 7. Constants and tables

Padding constants above; `LOWRES_COST_MASK = (1<<14)-1`, `LOWRES_COST_SHIFT = 14`; `X264_SCAN8_SIZE` for deblock-strength arguments; `X264_REF_MAX`, `X264_BFRAME_MAX`, `X264_LOOKAHEAD_MAX` bound the analysis arrays. The deblocking table contains two directions times luma/chroma variants plus intra and MBAFF families.

## 8. Performance notes

- **Cache layout:** all planes and per-frame arrays share one arena; the `PADH_ALIGN` prefix and `align_plane_size` tail deliberately break exact cache-line/divisor alignment to avoid conflicts when several planes are read at once (see `frame.c.md`). Four luma filter planes are allocated in phase so cache-line-split code can compare them with the same offset.
- **False sharing:** `i_lines_completed` and `i_slice_count` are per-frame and touched by different threads; keep them in distinct cache lines from unrelated hot counters, and rely on the mutex for the conditional wait.
- **Allocation strategy:** frame pools (`unused`, `blank_unused`) recycle whole frames; `pop_unused` resets state instead of reallocating. This avoids allocator churn and preserves huge-page-backed buffers.
- **Dispatch hierarchy:** deblocking and weight scaling go through function tables selected by CPU and colour format; 16-row strips match cache-line reuse of source/destination rows.
- Public references: x264 `doc/threads.txt` (frame-level parallelism, deblock dependency), Drepper (2007) on cache conflicts and false sharing, Intel Optimization Reference Manual on prefetching reference rows.

## 9. References

- ITU-T H.264 clause 8.3 (deblocking filter), 8.4 (motion compensation), Annex B (SEI), clause 7.4.2.1.4 (HRD timing).
- x264 `doc/threads.txt`, `doc/ratecontrol.txt`.
- Public `x264.h` (`x264_picture_t`, `x264_sei_t`, `x264_hrd_t`).
- Drepper, *What Every Programmer Should Know About Memory* (2007).

## 10. Verification checklist

- In a debug harness, check that every `plane[p]` and `filtered[p][i]` lies inside `base`/the arena and has at least `PADH` readable pixels to the left and `PADV` rows above and below.
- Verify a frame pool never grows: after encoding many frames with constant geometry, `pop_unused` must return recycled frames and allocation counts must stabilise.
- Verify `i_reference_count` reaches zero before a frame returns to the unused list, and that `b_duplicate` frames never free shared pointers (run under a memory checker).
- With two threads, drive `x264_frame_cond_broadcast`/`wait` with increasing line counts and verify the waiter awakens exactly when the threshold is reached; verify a negative target never waits.
- Fill a synchronised list to capacity from one thread and verify a second producer blocks until a consumer pops; verify no lost wakeups over many iterations.
- Verify `x264_weight_scale_plane` covers the entire rectangle for widths that are exact multiples of 16, exact multiples of 8, and neither.
- Verify plain-list push/pop/unshift/shift preserve FIFO or LIFO order as documented, including empty-list delimiters.

## 11. Open questions

- UNKNOWN: the exact internal SPS chroma format used for a given external colourspace beyond the mapping implemented in `frame.c` (`frame_internal_csp`); see `frame.c.md`.
- UNKNOWN: whether `i_lines_weighted` supports more than one weighted reference; the header flags this as a FIXME.
- UNKNOWN: the required alignment of `integral` for all SIMD ESA implementations; only its type is fixed here.
