# `common/frame.h` - frame and plane abstractions

## 1. Header

Source path: `E:\_x264_dirty_stash\x264-source\common\frame.h` (mirrored at `common/frame.h`). Role in one sentence: defines `x264_frame_t` (the padded multi-plane picture plus all per-frame analysis/threading state), the synchronised frame list, the deblocking function table, and the frame/list/pool operations. Marker: **dirty-side behavioural specification**.

Reading note: this header is the *declaration* side of the frame contract; the allocation, padding replication and list behaviour are implemented in `frame.c.md`. Fields are grouped below by purpose, with the invariant that governs each group. A clean-room implementation should treat the constant definitions in §3, the lifetime rules in §4 and the per-field contracts in §5.1 as the normative core of this document; the remaining sections provide context and verification.

---

## 2. Purpose

Motion estimation and compensation need to read pixels outside the visible picture, and threaded encoding needs frames to carry completion state and reference counts. This header defines the object that satisfies both: a picture with guaranteed padding, four filtered variants of the luma plane, half-resolution copies for lookahead, motion/reference data, rate-control bookkeeping and thread synchronisation primitives. It also defines the producer/consumer list used to hand frames between the encoder and lookahead threads.

The header deliberately exposes a single opaque-ish struct rather than many small objects, because frames are allocated in bulk from an arena and recycled. Every field therefore has one of three lifetimes:

1. **Structural** — set once at allocation (plane pointers, strides, buffer addresses); never changes while the frame exists.
2. **Per-use** — reset by `pop_unused` when a frame is recycled (flags, reference count, weights, slice-count seed).
3. **Accumulated** — written progressively by whichever pipeline stage owns the frame at that moment (SATD costs, MVs, completion count, QP offsets).

An independent implementation must respect this split: resetting a structural field breaks the arena invariant, while failing to reset a per-use field leaks stale state into the next frame.

The header is also where the **plane numbering contract** is fixed. `plane[0]` is always luma; `plane[1]` and `plane[2]` are chroma only in I444, while NV12/NV16 store interleaved chroma in `plane[1]` and I400 has no chroma planes at all. Consumers such as the CAVLC writer and the deblocking code rely on this numbering together with `i_plane` and `i_csp`; they do not re-derive it from the picture format. Two derived quantities, `CHROMA_V_SHIFT` (0 for 4:4:4 and 4:0:0, 1 for 4:2:0) and `CHROMA_H_SHIFT` (0 except 4:2:0), come from `common/common.h` and must agree with the frame's internal csp — a mismatch produces wrong chroma plane dimensions even though the pointers are valid.

---

## 3. Public interface

Constants: `PADH = 32`, `PADV = 32`, `PADH_ALIGN = max(PADH, NATIVE_ALIGN/SIZEOF_PIXEL)`, `PADH2 = PADH_ALIGN + PADH`. Padding is expressed in pixels; horizontal padding includes an aligned portion so plane start addresses are SIMD-friendly. `LOWRES_COST_MASK = (1<<14)−1`, `LOWRES_COST_SHIFT = 14`.

`typedef struct x264_frame ... x264_frame_t`. Principal fields (grouped):

- Identity/timing: `base`, `i_poc`, `i_delta_poc[2]`, `i_type`, `i_forced_type`, `i_qpplus1`, `i_pts`, `i_dts`, `i_reordered_pts`, `i_duration` (SPS time-scale units), `f_duration` (seconds), `i_cpb_duration`, `i_cpb_delay`, `i_dpb_output_delay`, `param`, `i_frame`, `i_coded`, `i_field_cnt`, `i_frame_num`, `b_kept_as_ref`, `i_pic_struct`, `b_keyframe`, `b_fdec`, `b_last_minigop_bframe`, `i_bframes`, QP averages (`f_qp_avg_rc`, `f_qp_avg_aq`, `f_crf_avg`), `i_poc_l0ref0`.
- Planes: `i_csp`, `i_plane`, `i_stride[3]`, `i_width[3]`, `i_lines[3]`, lowres stride/width/lines, `plane[3]`, `plane_fld[3]`, `filtered[3][4]` (original, horizontal, vertical, HV), `filtered_fld[3][4]`, `lowres[4]`, `integral`, `buffer[4]`, `buffer_fld[4]`, `buffer_lowres`.
- Weighting: `weight[X264_REF_MAX][3]`, `weighted[X264_REF_MAX]`, `b_duplicate`, `orig`.
- Motion data: `mb_type`, `mb_partition`, `mv[2]`, `mv16x16`, `lowres_mvs[2][BFRAME_MAX+1]`, `field`, `effective_qp`, `lowres_costs[BFRAME_MAX+2][BFRAME_MAX+2]` (packed as `(lists_used<<14)+cost`, mask `0x3fff`), `lowres_mv_costs`, `ref[2]`, `i_ref[2]`, `ref_poc[2][X264_REF_MAX]`, `inv_ref_poc[2]`.
- Lookahead/rate control: `i_cost_est`, `i_cost_est_aq`, `i_satd`, `i_intra_mbs`, per-row SATD/bit/QP arrays, `f_qp_offset`, `f_qp_offset_aq`, `b_intra_calculated`, `i_intra_cost`, `i_propagate_cost`, `i_inv_qscale_factor`, `b_scenecut`, `f_weighted_cost_delta`, `i_pixel_sum[3]`, `i_pixel_ssd[3]`, `hrd_timing`, planned frame-type/SATD/CPB arrays indexed by `X264_LOOKAHEAD_MAX`, lookahead clocks.
- Threading: `i_lines_completed`, `i_lines_weighted`, `i_reference_count`, `mutex`, `cv`, `i_slice_count`.
- Extras: periodic intra refresh position/columns, `b_corrupt`, `extra_sei`, `opaque`, `mb_info` + `mb_info_free`, optional `opencl`, `b_duplicate`.

`typedef struct { x264_frame_t **list; int i_max_size; int i_size; mutex; cond cv_fill; cond cv_empty; } x264_sync_frame_list_t`: a bounded FIFO-like producer/consumer queue (pop takes from the tail, push appends at the front slot).

`x264_deblock_function_t`: pointer table with luma/chroma deblock functions in both directions, intra variants, 4:2:0/4:2:2 horizontal chroma variants, MBAFF variants, and a `deblock_strength` function.

Functions (all templated): deletion, picture copy, several border-expand variants (`x264_frame_expand_border`, `_filtered`, `_lowres`, `_chroma`, `_mod16`, `x264_expand_border_mbpair`), row/macroblock deblocking, half-pel filtering, lowres init, `x264_deblock_init`, condition-variable helpers (`x264_frame_cond_broadcast/wait`, `x264_threadslice_cond_broadcast/wait`), `x264_frame_new_slice`, plain list operations (`push`, `pop`, `unshift`, `shift`), pooled-frame operations (`push_unused`, `pop_unused`, `push_blank_unused`, `pop_blank_unused`), `x264_weight_scale_plane`, `x264_frame_delete_list`, and synchronised list operations (`x264_sync_frame_list_init/delete/push/pop`). The four plain list operations and the two sync push/pop are `X264_API`.

Selected function contracts in detail:

| Function | Precondition | Postcondition / return |
|----------|--------------|------------------------|
| `x264_frame_new(h)` | valid `h->param` geometry and CPU flags | fully allocated frame with sentinel fields; NULL on allocation failure |
| `x264_frame_delete(f)` | `f` non-NULL | arena/struct/sync objects freed unless `b_duplicate`; borrowed callbacks invoked if set |
| `x264_frame_copy_picture(h,dst,src)` | `dst` internally allocated with the mapped csp | 0 on success, −1 on csp/depth/parameter mismatch; pixels and metadata transferred |
| `x264_frame_expand_border(f,mb_y)` | pixel data for row band `mb_y` written | padding around that band replicates valid edges |
| `x264_frame_cond_wait(f,lines)` | `lines >= 0` means a real dependency | returns observed `i_lines_completed` (>= lines) |
| `x264_frame_new_slice(h)` | slice threads configured | returns new slice count, or −1 at the configured maximum |
| `pop_unused(list,h)` | `h` has geometry for allocation | frame with `i_reference_count == 1` and reset per-use fields |
| `push_unused(list,f)` | caller is the last user | decrements count; at zero returns frame to pool |
| `push_blank_unused(list,orig)` | `orig` has valid planes | shallow duplicate with `b_duplicate == 1`, `orig` set |
| `x264_sync_frame_list_pop(list)` | list not deleted | blocks while empty; returns the tail frame, `i_size` decremented |

The `X264_API` markers on the plain list and sync push/pop functions mean they are part of the public ABI surface used by API-level frame handoff; the pooled helpers are internal.

---

## 4. State and data structures

### 4.1 Arena ownership

A frame's memory is one allocation: `base` owns the struct-sized header block allocated first and the arena allocated by PREALLOC; all plane pointers are offsets into that arena. `orig` normally points to self; a duplicate frame is a shallow copy whose `b_duplicate` flag suppresses freeing shared pointers. `i_reference_count` counts users, not pointers: it starts at 1 for a freshly popped frame and is decremented by `push_unused`; only at zero does the frame return to the free pool.

The arena contains, in order: (1) the luma buffers `buffer[0..3]` (four only for reconstructed frames with subpel refinement; otherwise one), (2) the chroma buffer `buffer[1]`, (3) reconstructed-frame analysis arrays (`mb_type`, `mb_partition`, `mv`, `ref`, `field`, `effective_qp`, bit/QP rows, integral image), (4) encoded-frame lowres buffers `buffer_lowres` and lowres cost arrays. The exact ordering is an implementation detail, but the *properties* — single allocation, alignment phase shared by the four luma planes, and a guard region for mbtree overreads — are observable and must be preserved.

### 4.2 Plane geometry invariant

Plane geometry invariant: `plane[p] = buffer[p] + stride[p]*padv + PADH_ALIGN`, so column −1 through −PADH are readable. When subpel refinement is active for a reconstructed frame, four luma planes are allocated consecutively with identical alignment phase, and `filtered[p][0..3]` point into them. The integral image for ESA uses a separate 16-bit buffer when ME method is ESA or better. Lowres planes are four quarter-size luma copies with their own padding.

The four `filtered` variants are, in index order: original (0), horizontal half-pel (1), vertical half-pel (2), diagonal HV (3). They are used by quarter-pel motion compensation. For interlaced/MBAFF frames, `plane_fld[p]` and `filtered_fld[p][i]` are separate field planes; they are expanded independently by the border code.

### 4.3 Ownership of external resources

Ownership of `param` is external: `x264_frame_delete` calls `x264_param_cleanup` and `param_free` only when `frame->param` and its `param_free` are set. `extra_sei` payloads and `mb_info` are freed through caller-supplied callbacks. `opaque` is never inspected or freed by the frame module.

| Resource | Owned by frame? | Freed by | Guarded by |
|----------|-----------------|----------|------------|
| `base` / arena | yes | `frame_delete` | `!b_duplicate` |
| `mutex` / `cv` | yes | `frame_delete` | `!b_duplicate` |
| `param`, `param_free` | no (borrowed) | caller callback via frame | `param && param_free` |
| `extra_sei` payloads | no (borrowed) | SEI free callback | `!b_duplicate` |
| `mb_info` | no (borrowed) | `mb_info_free` callback | callback non-null |
| `opencl` buffers | yes | OpenCL delete hook | build flag |

### 4.4 Reference-count lifecycle

A freshly popped frame has `i_reference_count = 1`; each `push_unused` decrements it. When it reaches zero the frame goes back to the unused list. `push_blank_unused` instead produces a shallow duplicate with `b_duplicate = 1` and `orig` pointing at the source frame; its own reference count is independent, but its plane pointers alias the original's. This lets lookahead hold many "blank" references to one picture without copying pixels. **Rule:** code must never free `base`, `param`, SEI or planes through a duplicate — the flag exists precisely to prevent that double free.

---

## 5. Algorithm / behaviour

`x264_frame_cond_broadcast(frame, lines)` takes the frame mutex, assigns `i_lines_completed`, broadcasts the condition variable and unlocks. `x264_frame_cond_wait(frame, lines)` locks, waits while the recorded line count is below the requested value *and* the requested value is non-negative, then unlocks and returns the observed count. A negative target therefore never waits; this is how disabled dependencies are expressed. `x264_threadslice_cond_broadcast(h, pass)` stores a pass index and broadcasts only when the pass is positive; `x264_threadslice_cond_wait(h, pass)` waits until `i_threadslice_pass >= pass`. Both use the encoder's mutex/condition variable. `x264_frame_new_slice` increments `i_slice_count` atomically when slice threads are enabled (plain increment otherwise) and returns −1 once the configured maximum is reached.

Plain list operations are simple pointer-array manipulations: `push` appends after the first null, `pop` removes the last non-null element, `unshift` shifts all elements right and inserts at index 0, `shift` removes index 0 and compacts left. `pop_unused` reuses a pooled frame or allocates a new one, then resets the per-use flags (`b_last_minigop_bframe`, `i_reference_count = 1`, `b_intra_calculated`, `b_scenecut`, `b_keyframe`, `b_corrupt`, slice-count seed, weight and weighted-cost arrays). `pop_blank_unused` returns either a pooled shallow duplicate or a bare struct with `b_duplicate = 1`.

Synchronised list operations bound the queue at `i_max_size`: `push` waits on `cv_empty` while full, stores at `list[i_size++]`, unlocks, then broadcasts `cv_fill`; `pop` waits on `cv_fill` while empty, takes `list[--i_size]`, clears that slot, broadcasts `cv_empty`, and unlocks. Broadcasts are issued after unlocking the mutex in `push` but before unlocking in `pop`; both are correct because the predicate is protected by the same mutex.

`x264_weight_scale_plane` divides the area into 16-row horizontal strips (found optimal for cache loads) and calls a width-specific weight function for 16-wide chunks, with an 8-wide tail.

### 5.1 Field-level contracts and invariants

| Field group | Invariant / contract |
|-------------|----------------------|
| `i_poc`, `i_delta_poc[2]` | POC −1 means "not yet assigned"; `delta_poc` is set after slice-type decision. Two entries correspond to the two reference lists. |
| `i_qpplus1` | QP + 1, or AUTO sentinel −1; +1 avoids a reserved zero. |
| `i_type`, `i_forced_type` | `i_type` is the decided frame type; `i_forced_type` comes from the caller and is clamped to the public range. |
| `i_duration`, `f_duration`, `i_cpb_*`, `i_dpb_output_delay` | Duration in SPS time-scale units and in seconds; CPB/DPB delays feed HRD SEI. All zero until set. |
| `i_plane`, `i_csp` | Number of planes (1/2/3) and internal chroma format (I400/NV12/NV16/I444). Consistent with `frame_internal_csp`. |
| `i_stride[p]`, `i_width[p]`, `i_lines[p]` | Padded geometry; `i_width` is MB-padded, `i_stride` includes `PADH2` and alignment. |
| `filtered[p][0..3]` | Four half-pel luma variants; only index 0 exists when subpel refinement is off. |
| `lowres_costs[a][b]` | Packed `(lists_used << 14) + cost`; mask `0x3fff` recovers the cost. `a,b` index lookahead depths up to `X264_BFRAME_MAX+1`. |
| `i_intra_cost` | 0xffff sentinel = not yet measured. |
| `i_lines_completed` | −1 initially; row bands report progress through `cond_broadcast`. Negative targets disable waits. |
| `i_reference_count` | 1 on pop; zero triggers pool return. |
| `i_slice_count` | Seeded to slice-thread count (or 1) on reuse; `new_slice` increments and caps. |

### 5.2 Condition-variable decision table

| Caller action | Effect on state | Wait behaviour |
|---------------|-----------------|----------------|
| `cond_broadcast(h, lines)` | sets `i_lines_completed = lines` | wakes all waiters; returns immediately |
| `cond_wait(h, lines >= 0)` | none | blocks until `i_lines_completed >= lines`; returns that value |
| `cond_wait(h, lines < 0)` | none | never blocks; returns current `i_lines_completed` |
| `threadslice_cond_broadcast(h, pass)` | sets pass index | broadcasts only for `pass > 0` |
| `threadslice_cond_wait(h, pass)` | none | blocks until stored pass `>= pass` (skips finished passes) |

The `>=` rather than `==` comparison is essential: it allows a waiter that resumes late to proceed without waiting for a pass that has already completed. The suppression of the pass-0 broadcast is safe because no waiter can exist before the first pass begins.

### 5.3 Worked packing example

`lowres_costs[2][1]` stores `lists_used = 2` and `cost = 300`: packed value `= (2<<14) + 300 = 8068`. A consumer recovers `lists_used = value >> 14` (2) and `cost = value & 0x3fff` (300). If a cost ever exceeded `0x3fff` (16383) it would corrupt the `lists_used` field, so the cost must be saturated before packing — this is a contract on the producer, not enforced by the struct.

### 5.4 Plane-pointer derivation (worked)

For an 8-bit 4:2:0 frame of width 1920, height 1088 (68 MB rows), with `PADV = 32` and `PADH_ALIGN = 32`, and a stride of 1984 pixels (see `frame.c.md` §4):

```
plane[0] = buffer[0] + 1984*32 + 32 = buffer[0] + 63,520
plane[1] = buffer[1] + 1984*32 + 32     (NV12 chroma, stride 1984, lines 544+64)
filtered[0][i] = buffer[0] + i*luma_plane_size + 1984*32 + 32   (i = 0..3)
```

Reading `plane[0][x][y]` with `x = −1` and `y = −1` lands 1 pixel left and 1 row above the visible origin, inside the padding. With `filtered[0][1]` (horizontal half-pel), the same negative coordinates address the horizontally-filtered plane at the same phase, because all four planes were given identical alignment. A clean implementation must preserve the `+ PADH_ALIGN` term; omitting it shifts every plane left by 32 pixels and silently corrupts motion compensation at the right edge.

For 4:4:4, `plane[1]` and `plane[2]` are independent I444 buffers with the same stride/height as luma, so all three share the same pointer formula. For 4:0:0 only `plane[0]` exists and `i_plane == 1`. The chroma vertical padding is `PADV` for 4:2:0, `PADV*2` for interlaced 4:2:2, and `PADV*2` for 4:4:4 interlaced.

### 5.5 Deblocking function table contract

`x264_deblock_function_t` is populated once by `x264_deblock_init(cpu, pf, mbaff)` and holds pointers for: luma horizontal and vertical deblocking (normal and intra variants), chroma horizontal and vertical (with separate 4:2:0 and 4:2:2 horizontal variants), MBAFF variants, and a `deblock_strength` calculator. The table is selected by CPU features and by whether MBAFF is enabled; a clean implementation should select once at encoder open and call through the table in the hot loop, never re-dispatching per macroblock. The intra variants skip filtering across intra block edges where the standard forbids it; using the wrong variant changes reconstructed samples and hence the bitstream of subsequent inter frames.

### 5.6 Synchronised-list init/delete contracts

`x264_sync_frame_list_init(list, max_size)` allocates the pointer array of `max_size` entries, zeroes `i_size`, and initialises `mutex`, `cv_fill` and `cv_empty`. `x264_sync_frame_list_delete(list)` wakes any blocked producers/consumers (by broadcasting both conditions), joins them by contract, frees the array and destroys the synchronisation objects. Deleting a list with live waiters is a caller error. The plain list operations (`push`/`pop`/`unshift`/`shift`) are not synchronised and must not be mixed with the sync operations on the same list.

The distinction matters for ownership: the synchronised lists (`ifbuf`, `next`, `ofbuf` in `x264_lookahead_t`) hand frames between threads, so they transfer *reference ownership*; the plain lists are used within a single thread for free-frame management. A frame passed through a sync list is still counted by `i_reference_count` until its consumer calls `push_unused`.

---

## 6. Dependencies

Includes `base.h`/`common.h` types, `x264.h` picture/SEI types, `threadpool.h` for synchronised lists, and optional `opencl.h`. Implemented by `frame.c`; used by lookahead, rate control, encoder core and frame threads. `x264_deblock_init` depends on architecture-specific deblocking implementations declared in `common/deblock.h`.

Consumers of this header that matter for an I-slice implementation: the CAVLC/CABAC writers read `plane[]` and the `non_zero_count` cache indirectly through `mb`; the motion-compensation code reads `filtered[][]`; mode decision reads `lowres[]` and the cost arrays; the entropy writers' QP logic reads `f_qp_offset`. All of these depend on the padding and geometry invariants above holding before the frame is handed to them.

---

## 7. Constants and tables

Padding constants above; `LOWRES_COST_MASK = (1<<14)−1`, `LOWRES_COST_SHIFT = 14`; `X264_SCAN8_SIZE` for deblock-strength arguments; `X264_REF_MAX`, `X264_BFRAME_MAX`, `X264_LOOKAHEAD_MAX` bound the analysis arrays. The deblocking table contains two directions times luma/chroma variants plus intra and MBAFF families. `PADH_ALIGN` depends on `SIZEOF_PIXEL` and `NATIVE_ALIGN`, so it is 8-bit and 10-bit specific in its pixel count even though its byte size is fixed.

Derived bounds worth stating for an implementer: `X264_BFRAME_MAX + 2` rows/columns in `lowres_costs` (one extra for the current frame and one for the end-of-lookahead sentinel), `X264_REF_MAX` entries per list in `ref_poc`, and two lists throughout. The `f_qp_offset`/`f_qp_offset_aq` pair is a per-frame float applied by adaptive quantisation and mbtree; they are accumulated across passes and must be reset by `pop_unused`. The `hrd_timing` sub-struct carries `cpb_removal_delay` and `dpb_output_delay` computed from the SPS HRD parameters described in `set.h.md` §5.

The constants are compile-time fixed and are identical in 8-bit and 10-bit builds except for `PADH_ALIGN`, which scales with `NATIVE_ALIGN/SIZEOF_PIXEL`. An independent implementation should derive `PADH_ALIGN` from the pixel size rather than hard-code 32, because a 10-bit build doubles `SIZEOF_PIXEL` and halves the aligned-pixel count for the same byte alignment.

---

## 8. Performance notes

- **Cache layout:** all planes and per-frame arrays share one arena; the `PADH_ALIGN` prefix and `align_plane_size` tail deliberately break exact cache-line/divisor alignment to avoid conflicts when several planes are read at once (see `frame.c.md`). Four luma filter planes are allocated in phase so cache-line-split code can compare them with the same offset.
- **False sharing:** `i_lines_completed` and `i_slice_count` are per-frame and touched by different threads; keep them in distinct cache lines from unrelated hot counters, and rely on the mutex for the conditional wait.
- **Allocation strategy:** frame pools (`unused`, `blank_unused`) recycle whole frames; `pop_unused` resets state instead of reallocating. This avoids allocator churn and preserves huge-page-backed buffers.
- **Dispatch hierarchy:** deblocking and weight scaling go through function tables selected by CPU and colour format; 16-row strips match cache-line reuse of source/destination rows.
- Public references: x264 `doc/threads.txt` (frame-level parallelism, deblock dependency), Drepper (2007) on cache conflicts and false sharing, Intel Optimization Reference Manual on prefetching reference rows.
- **Struct layout hint:** because the struct is copied between frame threads up to the synchronisation boundary in `x264_t`, keeping the hot per-frame scalar fields contiguous improves the copy's cache behaviour. A clean implementation may reorder fields within a group but must not split a group across the ownership boundary described in §4.
- **Packing density:** the analysis arrays (`mb_type`, `mv`, `ref`, costs) are sized to the MB-padded dimensions and are read row-wise; storing them plane-major (all rows for one array together) rather than interleaved by macroblock improves locality for the row-band pipeline. The reference's arena layout already does this.
- **Condition-variable overhead:** each `cond_wait` reacquires the mutex after waking; with many frame threads this is the main contention point. The `>=` predicate minimises the number of wakeups needed, so preserve it rather than signalling one thread at a time.
- Public references (additional): Intel *Optimization Reference Manual* on cache-line partitioning of producer/consumer ring buffers; POSIX `pthread_cond_wait` documentation on the predicate/mutex pattern used by the synchronised lists.

---

## 9. References

- ITU-T H.264 clause 8.3 (deblocking filter), 8.4 (motion compensation), Annex B (SEI), clause 7.4.2.1.4 (HRD timing).
- x264 `doc/threads.txt`, `doc/ratecontrol.txt`.
- Public `x264.h` (`x264_picture_t`, `x264_sei_t`, `x264_hrd_t`).
- Drepper, *What Every Programmer Should Know About Memory* (2007).
- Intel *Optimization Reference Manual* — SIMD alignment, cache-line partitioning and store-forwarding rules relevant to multi-plane buffers.
- POSIX / Win32 condition-variable documentation — the predicate-plus-mutex wait pattern used by `cond_wait` and the synchronised lists.

---

## 10. Verification checklist

- In a debug harness, check that every `plane[p]` and `filtered[p][i]` lies inside `base`/the arena and has at least `PADH` readable pixels to the left and `PADV` rows above and below.
- Verify a frame pool never grows: after encoding many frames with constant geometry, `pop_unused` must return recycled frames and allocation counts must stabilise.
- Verify `i_reference_count` reaches zero before a frame returns to the unused list, and that `b_duplicate` frames never free shared pointers (run under a memory checker).
- With two threads, drive `x264_frame_cond_broadcast`/`wait` with increasing line counts and verify the waiter awakens exactly when the threshold is reached; verify a negative target never waits.
- Fill a synchronised list to capacity from one thread and verify a second producer blocks until a consumer pops; verify no lost wakeups over many iterations.
- Verify `x264_weight_scale_plane` covers the entire rectangle for widths that are exact multiples of 16, exact multiples of 8, and neither.
- Verify plain-list push/pop/unshift/shift preserve FIFO or LIFO order as documented, including empty-list delimiters.

Additional black-box checks:

- Verify the §5.1 invariants with a debug harness: after `pop_unused`, exactly the documented per-use fields differ from their previous values; all structural plane pointers and strides are unchanged.
- Verify the §5.2 decision table by driving condition waits with a negative target while no broadcast has occurred: the call must return immediately with `i_lines_completed == −1`.
- Verify the §5.3 packing example by writing `(2<<14)+300` into `lowres_costs[2][1]` and reading back `lists_used == 2`, `cost == 300`.
- Attach a shallow duplicate to two consumers and free each in turn under a memory checker; confirm no double free of `base`, `mutex`, SEI or planes and confirm `orig` still resolves to valid pixel data.
- Verify that a reconstructed frame (`b_fdec`) has four `filtered[0][i]` pointers in increasing address order with equal phase, while a source frame (`!b_fdec`) has only `filtered[0][0]`.
- Confirm that all `X264_API` functions are exported from both bit-depth builds and that their symbols are specialised by `x264_template`.
- Verify §5.4 by computing `plane[0] − buffer[0]` in a debug harness and asserting it equals `stride*PADV + PADH_ALIGN`; assert the same offset for all four `filtered[0][i]`.
- Verify the deblock table contract: after `x264_deblock_init(flags, pf, mbaff)` every pointer in `pf` is non-null for the selected feature set, and the intra variants differ from the normal variants for at least one direction.
- Verify sync-list lifecycle: initialise a list of capacity 3, push three frames, confirm a fourth push blocks; pop two, confirm the blocked push proceeds; delete only after unblocking, under a thread sanitizer.
- Verify that deleting a plain list frees the pointer array but never calls `frame_delete` on any frame still referenced — ownership stays with the caller.
- Confirm plane numbering for each internal csp: `i_plane` must be 1 for I400, 2 for NV12/NV16, 3 for I444, and `plane[p]` non-null exactly for `p < i_plane`.
- Confirm `CHROMA_V_SHIFT`/`CHROMA_H_SHIFT` agree with the internal csp for every supported colourspace (4:2:0 → 1/1, 4:2:2 → 0/1, 4:4:4 → 0/0, 4:0:0 → 0/0).
- Verify that `lowres_costs` saturation is enforced: write a cost of `0x4000` and confirm the producer clamps it so `lists_used` is not corrupted.

---

## 11. Open questions

- UNKNOWN: the exact internal SPS chroma format used for a given external colourspace beyond the mapping implemented in `frame.c` (`frame_internal_csp`); see `frame.c.md`.
- UNKNOWN: whether `i_lines_weighted` supports more than one weighted reference; the header flags this as a FIXME.
- UNKNOWN: the required alignment of `integral` for all SIMD ESA implementations; only its type is fixed here.
- UNKNOWN: the precise initial value of `f_weighted_cost_delta` after reuse; `pop_unused` zeroes it, but no reader constraints are documented in this header.
- UNKNOWN: whether consumers other than mbtree read `i_pixel_sum`/`i_pixel_ssd`; they are accumulated here but their final use is in rate control, not documented in this header.
- UNKNOWN: the exact field offset of `i_lines_completed` relative to the struct base; it is described as per-frame and mutex-protected, but no explicit cache-line alignment directive is visible here.
- UNKNOWN: whether `i_field_cnt` uses −1 or a different sentinel for progressive frames in all builds; only the initial −1 is visible in this header.

This header declares layout only. Numeric initial values, reset behaviour and call ordering are specified in `frame.c.md` sections 4–5; an independent implementation should treat the two documents as one contract.

Where this document is silent (for example the exact per-MB array contents at frame start), the answer is intentionally left UNKNOWN rather than guessed.
