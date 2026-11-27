# `common/frame.c` - frame allocation, borders, filtering and synchronisation

## 1. Header

Source path: `E:\_x264_dirty_stash\x264-source\common\frame.c` (mirrored at `common/frame.c`). Role in one sentence: allocates padded multi-plane frames in a single arena, converts external pictures into internal planes, replicates border samples for motion compensation, maintains frame pools and synchronised frame lists, and implements deblock/weight helpers. Marker: **dirty-side behavioural specification**.

Scope note: this document describes the *implementation* of the frame object; the type and field declarations are in `frame.h.md`. Bit-depth templating (`x264_frame_new_8`, etc.) is applied by the `x264_template` mechanism from `base.h`.

Reading order for an implementer: §4 (geometry and arena), §5 (creation/copy/border behaviour), §5.1–5.5 (variant decision tables), then §7 (constants). The synchronisation contracts are shared with `frame.h.md` §5 and should be read alongside it.

---

## 2. Purpose

A frame must present a clean, padded raster to SIMD motion estimation regardless of input colourspace, bit depth or interlacing, and must be reusable across frames without repeated allocation. This file implements that layout, the border replication required by H.264's unrestricted motion vectors, and the concurrency state (reference counts, completion conditions, bounded frame queues) that makes frame-threaded encoding possible.

Three invariants drive every routine here:

1. **Every plane pointer points into one arena**, with `PADH` readable columns on each side and `PADV` rows above and below, so motion compensation never needs bounds checks.
2. **Geometry is derived from macroblock-padded dimensions**, not the caller's width/height. A 1920x1080 source is stored as 1920x1088 internally.
3. **Frames are recycled, not reallocated.** A frame returned to the unused pool keeps its large arrays; only the per-use fields are reset.

A fourth, cross-cutting concern is *sharing*: reference frames are read by many frame threads and by the lookahead simultaneously, so ownership is tracked by `i_reference_count` and by the `b_duplicate` flag rather than by pointer identity. Sections 5 and 5.5 spell out those rules. The functions in this file are all bit-depth templated; a clean implementation should keep the geometry independent of `BIT_DEPTH` except where the pixel size enters `SIZEOF_PIXEL` (alignment) and the copy width.

---

## 3. Public interface

All functions are bit-depth templated; see `frame.h.md` for the full list. Key contracts:

- `void x264_frame_delete(x264_frame_t *)`: frees the arena, user-owned callbacks (parameter cleanup, `mb_info_free`, SEI payloads), destroys mutex/condition, and releases OpenCL buffers; shallow duplicates free only the struct itself.
- `int x264_frame_copy_picture(x264_t *, x264_frame_t *dst, x264_picture_t *src)`: validates colourspace and bit depth, transfers metadata and copies/deinterleaves pixels into internal planes; −1 on mismatch.
- `x264_frame_expand_border*` family: replicate edge samples into padding, either for a single macroblock row, for filtered planes, for lowres planes, for one chroma plane, or to fill partial macroblock edges caused by non-multiple-of-16 dimensions.
- `x264_frame_cond_broadcast/wait`, `x264_threadslice_cond_broadcast/wait`, `x264_frame_new_slice`: thread coordination described in `frame.h.md`.
- `x264_frame_push/pop/unshift/shift`, `push_unused/pop_unused`, `push_blank_unused/pop_blank_unused`, `x264_frame_delete_list`, synchronised list operations.
- `x264_weight_scale_plane`, `x264_frame_deblock_row`, `x264_macroblock_deblock`, `x264_frame_filter`, `x264_frame_init_lowres`, `x264_deblock_init`.

Return-value contracts worth stating explicitly: `copy_picture` returns 0 on success, −1 on any colourspace/depth/parameter rejection; `x264_frame_new_slice` returns −1 when the configured slice-thread limit would be exceeded, otherwise the new slice count; condition waits return the observed `i_lines_completed` (a non-negative count) so callers can learn available work.

---

## 4. State and data structures

The frame object is allocated with `CHECKED_MALLOCZERO` and then filled by a PREALLOC arena (see `base.h.md`). Two alignment helpers govern geometry:

```
align_stride(x, align, disalign):
    y = round_up(x, align)
    if y is an exact multiple of disalign: y += align
    return y

align_plane_size(x, disalign):
    guard = max(128, NATIVE_ALIGN) / SIZEOF_PIXEL
    y = x
    if y is an exact multiple of disalign: y += guard
    return y
```

The "disalign" trick avoids pathological cache/TLB conflicts where several planes would map to the same cache set: an exactly power-of-two-aligned stride repeated across planes makes all four luma planes alias to the same sets. Adding one `align` step when the value is exactly disaligned breaks that aliasing at a cost of a few bytes. `disalign = 512/SIZEOF_PIXEL` on PowerPC and `1024/SIZEOF_PIXEL` elsewhere; lowres disalignment is doubled.

`frame_internal_csp` maps external colourspaces to one of four internal ones: I400 stays I400; I420-class spaces map to NV12; I422-class spaces map to NV16; I444-class through RGB map to I444; anything else maps to NONE. Internally the encoder therefore handles at most two planes (NV12/NV16), three (I444/I400) or one.

| External family | Internal csp | Planes | Chroma layout |
|-----------------|--------------|--------|---------------|
| I420 / NV12 / NV21 / YUYV / UYVY | NV12 | 2 | 4:2:0, interleaved UV |
| I422 / NV16 / NV61 | NV16 | 2 | 4:2:2, interleaved UV |
| I444 / YV24 / RGB / BGR / BGRA | I444 | 3 | 4:4:4, planar |
| I400 | I400 | 1 | monochrome |

Plane geometry: visible width/height are the padded macroblock dimensions (`mb_width*16`, `mb_height*16`); `i_stride = align_stride(width + PADH2, align, disalign)`, where `align` is `NATIVE_ALIGN/SIZEOF_PIXEL` on generic builds but 64/32/16 bytes divided by pixel size on x86 according to CACHELINE_64/AVX512, CACHELINE_32/AVX, or neither. Vertical padding is `PADV` doubled for interlaced fields. Four luma planes are allocated contiguously when subpel refinement is on and `b_fdec` is set.

**Worked geometry example** (8-bit, 4:2:0, 1920x1080, generic `align=8` pixels, `disalign=1024` pixels):
- Visible padded size: `i_width=1920`, `i_lines=1088` (= 68 MB rows × 16).
- `i_stride = align_stride(1920 + 64, 8, 1024) = round_up(1984,8) = 1984`; 1984 is not a multiple of 1024, so no extra step → stride 1984 pixels.
- Luma plane size = `align_plane_size(1984 × (1088 + 64), 1024)` = `align_plane_size(1984×1152, 1024)`; 1984×1152 = 2,285,568, not a multiple of 1024 → unchanged.
- Chroma plane lines = 1088/2 = 544, plus `2*PADV` = 64 → 608 rows at stride 1984; total 1,206,272 samples.
- Plane pointer offset = `stride*PADV + PADH_ALIGN` = 1984·32 + 32 = 63,520 samples into the buffer, so `plane[0][-1,-PADV..]` is readable.

---

## 5. Algorithm / behaviour

**Frame creation.** Initial values: POC −1, type AUTO, QP-plus-one AUTO, PTS/frame/frame-number −1, `i_lines_completed = −1`, picture structure AUTO, field count −1, duration/CPB/DPB fields zero, lookahead clocks −1, `orig = self`. For NV12/NV16, a chroma buffer of `stride*(lines + 2*chroma_padv)` is allocated (and doubled for interlaced fields). For each luma plane, `luma_plane_size = align_plane_size(stride*(lines+2*padv), disalign)`, multiplied by four for reconstructed frames with subpel refinement. Reconstructed frames also allocate macroblock type/partition/MV/ref arrays, per-row bit/QP arrays, an integral buffer for ESA and, if applicable, field/effective-QP maps. Encoded frames allocate lowres buffers (four planes), lowres MV/cost arrays for all B-frame combinations, propagate costs and AQ offsets; an extra `NATIVE_ALIGN` bytes are reserved because mbtree assembly may overread. Mutex and condition variable are created last; failure at any stage frees the frame.

Plane pointers are then established: for NV12/NV16, `plane[1] = buffer[1] + stride*chroma_padv + PADH_ALIGN`; for luma planes, either `filtered[p][i] = buffer[p] + i*luma_plane_size + stride*padv + PADH_ALIGN` for four planes or `filtered[p][0] = plane[p] = buffer[p] + stride*padv + PADH_ALIGN`. Reconstructed frames advance `mv16x16` by one entry after zeroing its first element (so index −1 is readable), and point `integral` into buffer[3]. Encoded frames point `lowres[i]` into four equal-sized slices of `buffer_lowres`, zero lowres MVs, initialise `i_intra_cost` to 0xffff, and zero the inverse-QScale factor to silence a vector false positive.

**Picture copy.** Validates that the destination's internal colourspace matches the source's mapped one, rejects high-depth/8-bit mismatches and rejects v210 except in 10-bit builds. Verifies `src->i_type` lies in the public range; out-of-range forces AUTO. Transfers QP, PTS/reordered PTS, parameter pointer, picture structure, extra SEI, opaque pointer and optional MB info. Pixel paths: YUYV/UYVY are deinterleaved by a motion-compensation helper; v210 is deinterleaved by a 32-bit specialised helper; BGR/BGRA/RGB are deinterleaved with component reordering and vertical flip support; planar paths copy luma and then handle NV12/NV21 (interleaved or swapped), I420/I422/YV12/YV16 (interleaved with optional U/V swap) or I444/YV24 (separate planes). `get_plane_ptr` applies vertical flip by moving to the last row and negating stride, and rejects a width larger than the absolute stride.

**Border expansion.** `pixel_memset` replicates a single pixel value across a run, aligning the destination to word size first and using progressively wider stores (byte, 16-bit, 32-bit, 64-bit) to fill the span. `plane_expand_border` fills: left and right bands for every visible row by replicating the edge pixel (with a chroma-specific shift), then the top and bottom bands by copying whole rows including padding. `x264_frame_expand_border(h, frame, mb_y)` operates on the current 16-row band: it pads top only at the first row, bottom only at the last, extends the height by four rows at a slice end to cover deblocking overread, and for MBAFF expands the two field planes separately. Filtered-border expansion uses `padh = PADH−4`, `padv = PADV−8` and a width extended by eight, because filtering already produced eight extra columns of which up to three may be wrong. Lowres and single-plane chroma expansion call the same helper with full padding. `x264_frame_expand_border_mod16` fills the unused right/bottom area of a non-multiple-of-16 picture by replicating the last valid column/row, with field-aware row selection for interlaced bottom padding. `x264_expand_border_mbpair` replicates the last valid row into the padding for a macroblock pair.

**Filtering and deblocking.** `x264_frame_filter` computes the four half-pel variants of a reference frame's luma plane for a row band; `x264_frame_init_lowres` downscales to four lowres planes; deblocking functions are selected by `x264_deblock_init(cpu, pf, mbaff)` and invoked per row or per macroblock. Weight scaling iterates 16-row strips as described in `frame.h.md`.

**Synchronisation and lists.** See `frame.h.md` section 5 for the exact contracts. Notable detail: `x264_sync_frame_list_pop` broadcasts `cv_empty` while still holding the list mutex, whereas `push` broadcasts `cv_fill` after unlocking; both satisfy the same predicate-protected wait pattern.

### Additional behavioural detail

**Geometry invariants, expanded.** For NV12 and NV16 the chroma plane has the same stride as luma but only half the luma height in the 4:2:0 case; for NV16 it has full height. The chroma vertical padding is therefore `PADV` for 4:2:0 and `PADV*2` for interlaced 4:2:2. For 4:4:4 all three planes have identical width/height/stride and each gets its own allocation slot. For 4:0:0 only one plane exists. All plane sizes are computed from the macroblock-padded dimensions, not from `param.i_width/i_height`, so a 1920x1080 source is internally 1920x1088.

**Padding budget and MV reach.** H.264 allows motion vectors outside the picture when `b_motion_vectors_over_pic_boundaries` is set. With quarter-pel interpolation, an MV can address up to `16 + 8` samples beyond a MB edge (16 for the block extent plus the 8-tap half-pel filter). `PADH = 32` and `PADV = 32` provide a safety margin above that bound; reducing them in a test build produces artifacts only at extreme MV offsets, which is a good black-box proof that the padding is really used.

**Initialisation semantics on reuse.** When a frame is taken from the unused pool, only a small set of fields is reset; the large per-frame arrays are left as they were. This is safe because every consumer must write a field before reading it. `pop_unused` does, however, zero the weighting tables and the weighted-cost deltas, because those may be read before being fully rewritten. It also seeds `i_slice_count` to the number of slice threads (or 1), so that the first `x264_frame_new_slice` call sees a value that already accounts for the master thread.

**Copy-picture edge conditions.** A vertical flip is expressed by a negative stride; `get_plane_ptr` validates `width <= abs(stride)` after the flip adjustment, so a source whose declared stride is smaller than its width is rejected regardless of flip state. Interleaved formats (YUYV/UYVY, v210, BGR family) bypass the planar path entirely, which is why their chroma placement is decided by the deinterleave helper rather than by `CHROMA_H_SHIFT`. `X264_CSP_V210` is only accepted in a 10-bit build. `i_forced_type` is clamped to the public type range; an out-of-range forced type becomes AUTO and a warning is logged once per occurrence.

**Border expansion details.** The left and right bands are filled with a repeated pixel value using `pixel_memset`, which first aligns its destination pointer to the machine word size and then uses 64/32/16-bit stores; this is why padding spans are not merely `memset`-compatible. The top and bottom bands copy whole rows including the padding, so the corner regions are filled by the row copies after the vertical bands. For MBAFF, the frame and two field planes are expanded separately, each with its own row stride doubling. At a slice end the expanded height is extended by four rows to accommodate deblocking reading across the slice boundary, but only when the band is not also the slice start.

**Wait/predicate nuances.** `x264_frame_cond_wait` returns the observed completed-line count even when it did not need to wait; callers use this to learn how much work is already available. Its loop condition includes `i_lines_completed >= 0`, so a negative target returns immediately with the current value. `x264_threadslice_cond_broadcast` suppresses the broadcast for pass 0 because no waiter can exist before the first pass starts, while `x264_threadslice_cond_wait` uses a `>=` predicate rather than equality, allowing a waiter to skip passes that have already completed.

**Synchronised list ordering.** The lists are stack-like from the consumer side (pop takes the last element) and queue-like from the producer side (push appends at the current size). This LIFO consumption is intentional for lookahead frame reuse: the most recently produced frame is the most likely to be reused soon, improving cache locality. The broadcast on `cv_fill` in `push` happens after unlocking, while the broadcast on `cv_empty` in `pop` happens before unlocking; both are legal because the shared predicate is protected by the mutex.

**Ownership and lifetime rules.** A freshly allocated frame owns: the struct block (`base`), the PREALLOC arena, the mutex/condition, and any caller-supplied resources recorded in `extra_sei`, `param`/`param_free` and `mb_info_free`. A shallow duplicate (`b_duplicate = 1`) owns *none* of these except its own struct; `frame_delete` on a duplicate frees only the struct and leaves `base`, `param`, SEI and the original's planes untouched. `orig` normally points to the frame itself; for a duplicate it points to the original whose planes are shared. `i_reference_count` counts *users*, not pointers: it starts at 1 when a frame is popped for use and is decremented once per `push_unused`; the frame returns to the free pool only when the count reaches zero. `push_blank_unused` produces shallow duplicates specifically so that multiple lookahead consumers can reference the same pixel data without copying.

**Failure handling.** `frame_new` has a single failure label that frees the struct but not the PREALLOC arena pointer (there is nothing to free yet on early failures, and `frame->base` is zero-initialised). Mutex/condition creation failures occur after the arena allocation and therefore leak it in the reference implementation; a clean implementation should call `x264_frame_delete` on the partially built object after assigning `base`.

### 5.1 Border-expansion variant decision table

The expansion family shares one lower-level helper (`plane_expand_border`-like) but differs in which region and which padding budget it applies. A clean implementation must select the correct variant per call site:

| Variant | Region filled | Effective padh / padv | Called when |
|---------|---------------|-----------------------|-------------|
| `x264_frame_expand_border(plane, stride, width, lines, mb_y)` | one 16-row band of a plane | full `PADH` / `PADV` | after each row of macroblocks is reconstructed; top only when `mb_y==0`, bottom only on the last band |
| `x264_frame_expand_border_filtered` | the four half-pel planes | `PADH−4` / `PADV−8`, width +8 | on reference frames, because filtering already produced eight extra columns of which up to three are unreliable |
| `x264_frame_expand_border_lowres` | each lowres plane | full lowres padding | once after `x264_frame_init_lowres` |
| `x264_frame_expand_border_chroma` | one chroma plane | full padding, chroma-shifted edge value | when only chroma was copied (e.g. deinterleave path) |
| `x264_frame_expand_border_mod16` | unused right/bottom columns of a partial MB | replicates last valid column/row | for pictures whose width/height is not a multiple of 16 |
| `x264_expand_border_mbpair` | row padding of an MBAFF pair | replicates last valid row | at a macroblock-pair boundary |

The slice-end extension (four extra rows) is *in addition* to the per-band bottom padding and exists solely so the deblocking filter can read across the slice boundary without a special case. It is suppressed when the current band is also the slice start, because then there is no previously-filtered row to protect.

### 5.2 Picture-copy colourspace decision table

`x264_frame_copy_picture` first maps the source's `i_csp` through `frame_internal_csp`, then rejects a destination mismatch. The pixel path is chosen as follows:

| Source CSP class | Path | Notes |
|------------------|------|-------|
| YUYV / UYVY | MC deinterleave helper | chroma placement decided by helper, not by `CHROMA_H_SHIFT` |
| v210 | 32-bit specialised deinterleave | accepted only in 10-bit builds |
| BGR / BGRA / RGB | component-reorder deinterleave | supports vertical flip via negated pointer stride |
| NV12 / NV21 | planar copy, luma then interleaved chroma | NV21 swaps U/V order |
| I420 / I422 / YV12 / YV16 | planar copy with optional U/V swap | luma copied first |
| I444 / YV24 | three independent planar copies | no chroma subsampling |
| I400 | single luma copy | no chroma planes |

All paths funnel through `get_plane_ptr`, which applies VFLIP by moving the pointer to the last valid row and negating the stride. It validates `width <= abs(stride)` after this adjustment, so a malformed source with `stride < width` is rejected even without a flip. The forced picture type is clamped to the public enum and an out-of-range value becomes AUTO (type −1 semantics), with a one-time warning per occurrence.

### 5.3 Half-pel filtering and lowres downscale

`x264_frame_filter` produces, for one 16-row band of a reference frame's luma plane, the four interpolation variants used by quarter-pel motion compensation: the original samples, a horizontally half-pel-filtered plane, a vertically half-pel-filtered plane, and the diagonal (HV) plane. Four variants are stored contiguously with identical alignment phase so a SIMD routine can compare them using one pointer offset. The horizontal/vertical filters are separable 6-tap filters with fixed integer taps defined by the standard (clause 8.4); the HV plane is the cascade of the two. Because the filter reads neighbours outside the block, the filtered planes need the reduced padding budget described in §5.1.

`x264_frame_init_lowres` downscales the luma plane by two in each dimension into four quarter-resolution planes used by lookahead and mbtree motion estimation. It writes four planes because sub-8x8 partitions need four phase offsets; their MVs are stored in `lowres_mvs` and their costs in `lowres_costs`. The lowres planes carry their own padding and are expanded with `x264_frame_expand_border_lowres`. `i_intra_cost` is initialised to 0xffff as a sentinel meaning "not yet measured".

### 5.4 Weight-scale strip algorithm

`x264_weight_scale_plane` applies weighted prediction (Annex E) to a rectangular region. The reference partitions the area into horizontal strips of 16 rows — chosen empirically as optimal for cache-line reuse of the source and destination rows — and within each strip calls a width-specialised kernel that processes 16-pixel chunks with an 8-pixel tail. Each output sample is computed as:

```
out = clip( ( (src * weight + offset) >> log2denom ) + round )
```

where `weight`, `offset` and `log2denom` come from the `x264_weight_t` entry selected by reference list and index. The same function is used for both monochrome and chroma planes with different weight entries. A clean implementation must cover widths that are exact multiples of 16, exact multiples of 8, and neither, or it will leave a tail unwritten.

### 5.5 Synchronised-list state machine

A `x264_sync_frame_list_t` is a bounded queue with two condition variables. Its invariant is `0 <= i_size <= i_max_size`, protected by `mutex`. The two blocking operations are:

- **push**: lock; wait on `cv_empty` while `i_size == i_max_size`; store the frame at `list[i_size++]`, clear `cv_empty`'s predicate; unlock; broadcast `cv_fill`.
- **pop**: lock; wait on `cv_fill` while `i_size == 0`; take `list[--i_size]`, clear that slot; broadcast `cv_empty`; unlock.

The asymmetric broadcast placement (push after unlock, pop before unlock) is safe because every wait rechecks the predicate under the mutex; there is no lost-wakeup window. The zero-capacity case blocks both sides permanently until the list is deleted — a documented edge case, not an error. LIFO pop order is deliberate: the most recently produced frame is the hottest in cache for lookahead reuse.

---

## 6. Dependencies

Includes `common.h`. Calls `frame_new`'s PREALLOC macros from `base.h`, motion-compensation copy/deinterleave functions from `h->mc`, CPU feature flags from `h->param.cpu`, OpenCL frame deletion when enabled, and pthread primitives from `osdep.h`. Called by encoder open/close, lookahead, rate control and frame threads.

---

## 7. Constants and tables

`PADH/PADV = 32`, `PADH2 = PADH_ALIGN + PADH`, `PADH_ALIGN = max(PADH, NATIVE_ALIGN/SIZEOF_PIXEL)`. Align choices are 64/32/16 bytes on x86 (AVX512/AVX/none) and 64 or 16 bytes for `NATIVE_ALIGN`. `disalign = 512/SIZEOF_PIXEL` on PowerPC and `1024/SIZEOF_PIXEL` elsewhere; lowres disalign is doubled. `align_stride` adds one extra `align` when the rounded value is exactly disaligned; `align_plane_size` adds `max(128, NATIVE_ALIGN)/SIZEOF_PIXEL` under the same condition. The four half-pel filter planes share one allocation to keep their alignment phase identical. No large constant tables are defined here; the half-pel integer-reference lookup tables live in `tables.c`.

Sentinel values used at creation (all observable through the struct): POC −1, `i_qpplus1 = X264_QP_AUTO` (−1), PTS/DTS −1, `i_frame = −1`, `i_frame_num = −1`, `i_picture_structure` AUTO, `i_field_cnt = −1`, `i_lines_completed = −1`, `f_duration = 0`, lookahead clocks −1, `orig = self`. The negative sentinels are deliberately chosen so that "unknown" is distinguishable from any legal value; a clean implementation must preserve them because several consumers test `< 0` rather than equality with a specific enum member. `b_fdec` distinguishes reconstructed (`fdec`) frames, which carry MB analysis arrays, from source (`fenc`) frames, which carry lowres lookahead arrays; only the former get four luma filter planes.

---

## 8. Performance notes

- **Cache layout:** the disalignment trick is intentional — avoid power-of-two strides/plane sizes that alias cache sets. Preserve it even if it wastes a few bytes. Four luma planes are kept in phase so SIMD cache-line-split code can use a common offset.
- **False sharing:** frame completion state is per-frame and guarded by a mutex; do not move `i_lines_completed` into a shared array.
- **Allocation strategy:** PREALLOC plus frame pooling is the core strategy; avoid per-frame `malloc` in any reimplementation. Reserve the extra NATIVE_ALIGN for mbtree overreads.
- **Dispatch hierarchy:** deblocking, plane copy/deinterleave and weight functions are CPU-dispatched through `x264_deblock_function_t` and `h->mc`; select once, call often.
- **Prefetch:** border replication and filtering are streaming operations; the reference implementation relies on sequential access and wide stores. A clean implementation may prefetch the next row band at the start of each iteration.
- Public references: Drepper (2007) sections on cache conflicts, huge pages and false sharing; x264 `doc/threads.txt`; Intel Optimization Reference Manual on SIMD alignment and store forwarding.
- **Copy granularity:** the planar copy routines select 16-byte-aligned bulk copies where the stride permits, and fall back to scalar tails for odd widths. Because strides are already aligned by `align_stride`, an implementation can assume the bulk path for all encoder-internal frames; only external pictures with caller-provided strides need the tail path.
- **Conditional wait cost:** `cond_wait` is on the frame-thread critical path but is invoked only once per row band, so mutex overhead is negligible. The `>=` predicate in the threadslice wait lets a waiter skip already-completed passes instead of waking once per pass — preserve this to avoid thundering-herd wakeups with many slice threads.
- **Border expansion as a streaming kernel:** left/right bands are word-aligned before wide stores, so an implementation should fill the unaligned head/tail scalars first and then issue 64-bit stores. This is both faster and avoids touching bytes outside the plane allocation.

---

## 9. References

- ITU-T H.264 clause 8.3 (deblocking), 8.4 (motion compensation and sample interpolation), Annex E (weighted prediction), clause 7.4.2.1.4 (HRD).
- Public `x264.h`: picture colourspace flags including VFLIP and HIGH_DEPTH, `x264_picture_t`, `x264_sei_t`.
- x264 `doc/threads.txt` for frame-level dependencies and lookahead buffering.
- Drepper, *What Every Programmer Should Know About Memory* (2007).

---

## 10. Verification checklist

- Allocate frames for 4:2:0, 4:2:2, 4:4:4 and 4:0:0 inputs and verify plane counts 2/2/3/1 and that all padding regions are readable (write/read under AddressSanitizer).
- Feed a synthetic picture with distinct values per plane through `x264_frame_copy_picture` for each supported colourspace, including VFLIP, and verify the internal planes contain the expected samples.
- Verify bit-depth mismatch and invalid type values are rejected with the documented return code/force-to-AUTO behaviour.
- After border expansion, verify every padding pixel equals the corresponding edge pixel for left/right/top/bottom and that the four filtered planes are expanded independently.
- For a non-multiple-of-16 picture, verify `expand_border_mod16` fills the unused right/bottom area with replicated edge samples.
- Stress the frame pool with thousands of frames at fixed geometry and verify stable allocation counts and no leaks.
- Stress synchronised lists with two producers and two consumers; verify no deadlock, no loss, and capacity is never exceeded.
- Verify `x264_weight_scale_plane` against a scalar reference for several widths/heights and weight values.

Additional black-box checks:

- Encode a sequence whose dimensions are not multiples of 16 (e.g. 641x361) and verify no pixel outside the visible area leaks into the decoded output via border replication.
- Encode with `PADH`/`PADV` deliberately reduced in a test build and confirm the failure mode is a visible artifact at motion-vector extremes, proving the padding is actually used.
- Verify that a frame returned to the unused pool has `i_lines_completed` reset by its next consumer rather than retaining a stale completion value.
- Attach a reference frame to two frame threads and verify it is not recycled until both have called `push_unused` (instrument `i_reference_count`).
- Verify that blank duplicate frames never attempt to free `base`, `param` or SEI payloads of the original frame.
- Stress border expansion with a one-row frame and verify that top and bottom padding both replicate the single valid row without overlapping writes.
- For 4:2:2 input, verify that chroma vertical padding differs from 4:2:0 and that the filtered chroma planes are not allocated when not needed.
- Verify that `x264_frame_new_slice` with slice threads returns −1 on the call that would exceed `i_slice_count_max`, and that the non-threaded increment path never wraps.
- Verify that a synchronised list with capacity zero blocks both producers and consumers until deleted (documented edge case of the zero-capacity init path).
- Verify the alignment equations directly: for a geometry whose `width + PADH2` is exactly a multiple of `disalign`, assert the chosen stride is `round_up(width+PADH2, align) + align`, confirming the disalign step fired.
- Verify the worked 1920x1080 geometry end to end: internal lines 1088, stride 1984 (8-bit generic), luma plane base offset 63,520 samples, chroma lines 608 — check each against the equations in §4.
- For each entry in the §5.1 expansion-variant table, construct a plane with a unique value at every edge and assert only the designated region changes; in particular confirm filtered expansion uses the reduced `PADH−4`/`PADV−8` budget.
- Verify the §5.2 colourspace decision table by feeding one synthetic frame of each CSP class and checking that the internal plane layout matches the mapped internal csp (NV12/NV16/I444/I400).
- Verify the four half-pel planes produced by `x264_frame_filter` against a scalar separable 6-tap filter for random rows, confirming original/h/v/HV ordering and identical alignment phase.
- Verify `x264_weight_scale_plane` for a width that is neither a multiple of 16 nor of 8, confirming the 8-pixel tail is written and no sample past the rectangle is touched.
- Confirm that `pop_unused` resets the documented per-use fields (reference count 1, flags, weight tables, slice-count seed) while leaving large arrays byte-identical to their previous contents.

---

## 11. Open questions

- UNKNOWN: whether the exact cache-conflict-avoidance constants (512/1024 pixel disalign, extra NATIVE_ALIGN) are optimal on modern CPUs; they are empirical.
- UNKNOWN: how `i_lines_weighted` behaves when more than one reference frame needs weighting; the source marks it as a FIXME supporting only one.
- UNKNOWN: the precise set of overread distances required by each SIMD mbtree/deblock implementation; the extra alignment padding is a safety margin, not a documented bound.
- UNKNOWN: the exact stride-alignment rule on non-x86 architectures where `NATIVE_ALIGN` and `CACHELINE_*` macros resolve differently; the description above covers the generic and x86 cases visible in the reference.
- UNKNOWN: whether a consumer ever writes to `x264_zero` (used as a SIMD zero-fill source); it is declared non-const but no writer is visible from these files.
- UNKNOWN: the exact verticalflip behaviour for interleaved formats with a caller stride whose absolute value is below the declared width; the validation rejects them, but the reference logs no diagnostic in all paths.
- UNKNOWN: the precise ordering of SEI payload transfer versus pixel copy in `copy_picture` when both fail partway; partial-failure atomicity is not documented.
