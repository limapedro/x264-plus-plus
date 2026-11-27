# `encoder/analyse.h` — analysis and lookahead interface

**Role (one sentence):** This header declares the analysis entry points, including macroblock mode decision, cost-table lifecycle, weighted-reference preparation, slice-type decision and the lookahead queue API.

> **Dirty-side behavioural specification.** Written from the reference source and public literature for the clean-room reimplementation. No source code or near-verbatim pseudocode is reproduced; only behaviour, structure, constants and contracts are described.

---

## 1. Header

- Source path: `encoder/analyse.h` (about 55 lines).
- Role: the public surface of `analyse.c` and the lookahead/slicetype code that is included into the same translation unit.
- Every function declaration is preceded by a `x264_template()` renaming macro.

---

## 2. Purpose

Analysis and lookahead are invoked from the frame encoder and from rate control. The header gives them a stable, small API: set up cost tables once, feed frames into the lookahead, request frame-type decisions, prepare weighted references, then analyse one MB at a time. Keeping these declarations together lets the encoder avoid depending on the internal `x264_mb_analysis_t` type, which is private to `analyse.c`.

---

## 3. Public interface

| Function | Contract |
|---|---|
| `int x264_analyse_init_costs(x264_t *h)` | Allocates and fills the per-QP MV-bit, reference-index and (for ESA/TESA) full-pel MV cost tables. Returns 0 on success, −1 on allocation failure. Must be called before analysis. |
| `void x264_analyse_free_costs(x264_t *h)` | Frees every allocated per-QP table; safe to call with null pointers. |
| `void x264_analyse_weight_frame(x264_t *h, int end)` | Incrementally builds weighted reference planes up to the given row count; `end` is bounded by the rows already available in threaded encoding. |
| `void x264_macroblock_analyse(x264_t *h)` | Decide and cache the current MB's prediction mode. Precondition: `h->mb.i_mb_xy`, QP, neighbour caches and reference planes are valid. Postcondition: `h->mb` contains a complete decision ready for `x264_macroblock_encode`. |
| `void x264_slicetype_decide(x264_t *h)` | Decide frame types (I/P/B) for queued frames, including scenecut detection. |
| `void x264_slicetype_analyse(x264_t *h, int intra_minigop)` | Lookahead analysis with a forced-intra minigop hint. |
| `int x264_lookahead_init(x264_t *h, int i_slicetype_length)` | Initialise the lookahead queue and threads; returns 0/−1. |
| `int x264_lookahead_is_empty(x264_t *h)` | Query whether no frames are pending. |
| `void x264_lookahead_put_frame(x264_t *h, x264_frame_t *frame)` | Enqueue a frame for analysis. |
| `void x264_lookahead_get_frames(x264_t *h)` | Retrieve analysed frames into the encoder's input queue. |
| `void x264_lookahead_delete(x264_t *h)` | Tear down the lookahead. |

No public types or constants are declared here; `x264_me_t` lives in `me.h` and the analysis state struct is private to `analyse.c`.

---

## 4. State and data structures

The header itself is declaration-only. Behaviourally it implies:

- **Cost tables** are owned by `x264_t` (`cost_mv[QP+1]`, `cost_mv_fpel[QP+1][4]`, `cost_table->ref[QP+1][3][33]`, `cost_table->i4x4_mode[QP+1][17]`), allocated once and read-only during encoding. The pointer stored in `cost_mv[qp]` is re-based into the middle of the allocation so negative indices are valid; freeing must reverse that offset.
- **Lookahead queue** owns frames until `get_frames` hands them to the encoder; frame lifetime crosses threads.
- **Slice-type state** is shared with `slicetype.c`, which is included at the end of `analyse.c` and therefore has access to the private analysis structures.

---

## 5. Algorithm / behaviour

The header imposes the following ordering contracts:

1. `init_costs` before `macroblock_analyse`; QP changes do not require reallocation because tables exist for all QPs in `[qp_min, qp_max]` plus the lookahead QP.
2. `analyse_weight_frame` must be called before a P slice uses weighted references; with frame threading it is called once per row with the number of completed rows.
3. Lookahead functions form a producer/consumer pair: `put_frame` then `get_frames`; `is_empty` is a non-blocking query.
4. `slicetype_decide` may be called with delayed frames; the set of frames is owned by the lookahead until decision.
5. `macroblock_analyse` is called once per MB in raster order (or slice order with MBAFF); it relies on left/top neighbour state and on `h->mb.mvr` entries from earlier MBs.

---

## 6. Dependencies

- Included by `encoder/analyse.c`, `encoder/encoder.c`, `encoder/ratecontrol.c` and `encoder/api.c`.
- Relies on `x264_t`, `x264_frame_t` from `common/common.h`; `x264_template` from `common/base.h`.
- The functions are implemented in `analyse.c` and the included `slicetype.c`; lookahead helpers may also touch `lookahead.c`-style state owned by `x264_t`.

---

## 7. Constants and tables

None defined here. The tables populated by `x264_analyse_init_costs` are described in `analyse.c.md` §5.1 and §7.

---

## 8. Performance notes

1. Keeping cost tables per QP rather than recomputing them per MB is a major speed win [1]; the lookup is a single indexed load in the motion-search inner loop.
2. Incremental weighted-frame generation avoids re-weighting the entire reference for each MB/row [2].
3. The lookahead is asynchronous and frame-threaded; preserving `is_empty` semantics avoids stalls [3].
4. Concurrency: cost tables are immutable after init and safe to share; weighting and lookahead queues require the encoder's existing synchronisation.

---

## 9. References

[1] x264 developer documentation on lambda/MV cost tables and rate-control integration.

[2] H.264 weighted prediction syntax and x264's incremental weighting implementation notes.

[3] x264 lookahead/slicetype threading documentation (included `slicetype.c` and frame-thread manager).

---

## 10. Verification checklist

1. `init_costs` followed by `free_costs` leaves no leaks and no dangling pointers (pointer re-basing offset matches allocation size).
2. Every QP in `[qp_min, qp_max]` and the lookahead QP has a populated table.
3. `macroblock_analyse` leaves all fields required by `x264_macroblock_encode` valid for every MB type.
4. Weighted-frame generation is idempotent for the same `end` value.
5. Lookahead `put/get/is_empty/delete` behave correctly with zero, one and many frames, including threaded operation.
6. Slice-type decision produces identical frame types for a fixed sequence and settings.

---

## 11. Open questions

- **UNKNOWN:** exact ownership transition for frames between `lookahead_put_frame`, `slicetype_decide` and `lookahead_get_frames`; only the logical queue contract is visible here.
- **UNKNOWN:** whether `x264_slicetype_analyse` is part of the public API in all builds or only used internally by rate control.
- **UNKNOWN:** error-handling semantics of `x264_lookahead_init` on allocation failure beyond the 0/−1 return.
