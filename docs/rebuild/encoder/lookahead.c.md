# `encoder/lookahead.c`

**Role:** Implements the buffered frame lookahead pipeline that decides slice types / scene cuts, in either synchronous (on-the-fly) or dedicated-thread mode, and feeds decided frames to the encoder.

> **Dirty-side behavioural specification.** Describes observable behaviour and structure only; no source text or near-verbatim pseudocode is reproduced.

---

## 1. Header

- Source path: `encoder/lookahead.c`
- Role: manage the three-frame-queue lookahead (`ifbuf` → `next` → `ofbuf`), the lookahead worker thread, and the hand-off of B-frame groups to the frame encoder.
- Marker: dirty-side behavioural specification.

---

## 2. Purpose

Slice-type decisions (I/P/B) and MB-tree/VBV lookahead need frames ahead of the one currently being encoded. With no thread, decisions are made lazily just before encode; with `i_sync_lookahead` threads, a dedicated worker runs continuously between `x264_encoder_open` and `x264_encoder_close`, keeping a queue of upcoming frames analysed so the frame threads never stall for analysis. This module owns that concurrency, the reference counting of the "last non-B" frame needed as a predictor, and the grouping of frames into B-pyramid display-order groups.

---

## 3. Public interface

- `int x264_lookahead_init( x264_t *h, int i_slicetype_length )` — allocate the shared `x264_lookahead_t`, attach it to every frame thread, initialise the three synchronised frame lists, and (if threaded) create the worker thread cloned from `h->thread[h->param.i_threads]`. Returns -1 on allocation/thread-creation failure.
- `void x264_lookahead_delete( x264_t *h )` — signal thread exit, join, free the worker's caches, delete the three lists, release `last_nonb`, free the lookahead object.
- `void x264_lookahead_put_frame( x264_t *h, x264_frame_t *frame )` — enqueue an input frame into `ifbuf` (threaded) or `next` (synchronous).
- `int x264_lookahead_is_empty( x264_t *h )` — true when both `next` and `ofbuf` are empty.
- `void x264_lookahead_get_frames( x264_t *h )` — make decided frames available: wait for/drain `ofbuf` in threaded mode; in synchronous mode perform the decision on demand and drain.

Static helpers: `lookahead_shift`, `lookahead_update_last_nonb`, `lookahead_slicetype_decide`, `lookahead_thread`, `lookahead_encoder_shift`.

---

## 4. State and data structures

- `x264_lookahead_t` (shared by all threads): `ifbuf`, `next`, `ofbuf` — each a `x264_sync_frame_list_t` with a mutex, `cv_fill`/`cv_empty` condition variables, a fixed-size array and `i_size`; `last_nonb` (the most recent non-B frame, reference-counted, used for MB-tree propagation and as a reference anchor); `i_last_keyframe`; `b_analyse_keyframe`; `i_slicetype_length`; `b_exit_thread`; `b_thread_active`; `thread_handle`.
- `x264_frame_t.i_bframes` — number of B-frames that follow a non-B frame in display order; determines how many frames are shifted as one group.
- Ownership: frames flow from the input reader into `ifbuf`/`next`, then into `ofbuf`, then into `h->frames.current`; `last_nonb` is separately referenced and returned to the unused pool on delete.

Invariants: shifting moves frames without copying; enqueue/dequeue must respect each list's `i_max_size` (asserted); a group is always a non-B frame plus its `i_bframes` B-frames.

---

## 5. Algorithm / behaviour

**Shifting (`lookahead_shift`).** Move `count` frames from the head of `src` to the tail of `dst`, asserting neither underflows nor overflows. If any frame moved, broadcast `dst->cv_fill` (waking a consumer) and `src->cv_empty` (waking a producer).

**Last non-B tracking (`lookahead_update_last_nonb`).** Release the previous `last_nonb` to the unused pool, store the new frame, and increment its reference count.

**Threaded decision (`lookahead_slicetype_decide`).** Run `x264_slicetype_decide(h)` on the `next` queue (this assigns types and `i_bframes` to the head frame). Update `last_nonb` to the head frame. The group size is `head->i_bframes + 1`. Lock `ofbuf`, wait while it is full (`cv_empty`), lock `next`, shift that many frames from `next` to `ofbuf`, unlock `next`. If keyframe analysis is enabled and the new `last_nonb` is an I-frame, call `x264_slicetype_analyse(h, shift_frames)` — MB-tree and VBV lookahead must propagate through I-frames too. Unlock `ofbuf`.

**Worker thread (`lookahead_thread`).** Loop: lock `ifbuf`; if `b_exit_thread`, break. Lock `next`, move as many frames as fit (`min(next.max_size - next.size, ifbuf.size)`) from `ifbuf` to `next`, unlock `next`. If `next.size <= i_slicetype_length + b_vfr_input`, wait on `ifbuf.cv_fill` until a frame arrives or exit is signalled; otherwise call `lookahead_slicetype_decide`. On exit, move all remaining `ifbuf` frames to `next`, repeatedly decide until `next` is empty, then mark `b_thread_active = 0` and broadcast `ofbuf.cv_fill`.

**Initialisation.** `i_last_keyframe = -keyint_max` (no keyframe seen yet). Keyframe analysis is enabled when MB-tree or (VBV buffer size with lookahead) is configured and the stream is not in stats-read (two-pass) mode. List capacities: `ifbuf = i_sync_lookahead + 3`, `next` and `ofbuf` = `h->frames.i_delay + 3`. With zero sync lookahead the function returns after list setup (no worker). Otherwise a cloned encoder context is created at `h->thread[i_threads]`, given its own macroblock caches, and the worker is spawned with it.

**Frame hand-off (`lookahead_encoder_shift`).** If `ofbuf` is empty, nothing to do. The first queued frame's `i_bframes` determines the group size; that many frames are moved (in order) to `h->frames.current`, then `ofbuf.cv_empty` is broadcast.

**`x264_lookahead_get_frames`.** Threaded: wait while `ofbuf` is empty and the worker is active (`cv_fill`), then shift the group. Synchronous: if a frame is already in `frames.current` or `next` is empty, return; otherwise decide, update `last_nonb`, shift `bframes+1` frames from `next` to `ofbuf`, run keyframe analysis if applicable, and hand off the group.

This design guarantees the frame encoder always sees a head frame whose type is known and whose following B-frames have been buffered together, preserving display-order output via the `i_bframes` grouping.

---

## 6. Dependencies

Calls `x264_slicetype_decide` and `x264_slicetype_analyse` from `slicetype.c`; uses `x264_frame_shift`/`push`/`push_unused` and `x264_sync_frame_list_init/delete/push` from `common/frame.h`; uses `x264_macroblock_cache_allocate/free`, `x264_macroblock_thread_allocate/free` from `macroblock.c`; uses pthread wrappers `x264_pthread_mutex_lock/unlock`, `x264_pthread_cond_wait/broadcast`, `x264_pthread_create/join` from `common/thread.h`. Called from `encoder.c` (init/delete/put/get/is-empty) and from the rate-control/MB-tree paths via `last_nonb`.

---

## 7. Constants and tables

- No numeric constants beyond the list-size formulas: `ifbuf` capacity `i_sync_lookahead + 3`, `next`/`ofbuf` capacity `frames.i_delay + 3`.
- Recommended `rc_lookahead` (documented in the file) is the number of B-frames plus the number of threads.
- Condition variables: `cv_fill` signalled by producers, `cv_empty` by consumers.

---

## 8. Performance notes

Lookahead is a major latency/throughput component at slow presets; parallelising it is the primary speed opportunity.

Concrete opportunities with honesty:
- **Dedicated lookahead thread**: already implemented; enabling `i_sync_lookahead` overlaps the expensive slice-type/MB-tree analysis with frame encoding, improving throughput on multi-core machines. It does *not* change the bitstream: decisions are made on the same frames in the same order, so output is deterministic given fixed threading parameters.
- **Frame threading**: multiple frame threads each use the shared lookahead object; increasing threads raises latency (`frames.i_delay`) but improves throughput. Determinism of rate control depends on synchronisation hooks (`x264_thread_sync_ratecontrol` in `ratecontrol.c`), not on lookahead itself.
- **Queue sizing**: capacities include a small slack (+3) to avoid stalls; tuning `i_sync_lookahead` trades memory/latency against throughput.
- **Keyframe analysis gating**: skipping `x264_slicetype_analyse` for non-I head frames and in stats-read mode avoids unnecessary MB-tree propagation with no output impact.

No SIMD in this module. Contention on the mutexes is modest because each queue has a single producer/consumer in the common case.

---

## 9. References

- x264 public documentation on `--lookahead`, `--sync-lookahead`, `--bframes`, `--mbtree`, and frame threading.
- General producer/consumer pipeline design with condition variables (pthreads documentation).
- H.264 B-pyramid and display-order/reordering concepts (ITU-T H.264 Annex F).

---

## 10. Verification checklist

- With `i_sync_lookahead = 0`, verify encoder output (NAL bytes) is identical to a build with a lookahead thread for the same parameters and input — the decision order must be unchanged.
- Verify that `x264_lookahead_is_empty` returns true only after all frames have been consumed, including during flush at end of stream.
- Feed a sequence with known B-frame runs and check that groups shifted to `frames.current` always consist of one non-B frame followed by exactly `i_bframes` B-frames.
- Check that end-of-stream flush drains all remaining `ifbuf`/`next` frames before marking the thread inactive.
- Verify `last_nonb` reference counting does not leak or double-free under repeated reconfiguration/close.
- Confirm keyframe analysis runs on I-frames when MB-tree or VBV lookahead is enabled and is skipped in stats-read mode.

---

## 11. Open questions

- The exact definition of `frames.i_delay` (and hence queue capacity) for arbitrary combinations of frame threads and B-frames is UNKNOWN from this file alone; it is computed in `encoder.c`.
- Behaviour when `i_sync_lookahead` exceeds the number of B-frames + threads is bounded only by the queue sizes; any quality effect is UNKNOWN.
