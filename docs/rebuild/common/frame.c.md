# `common/frame.c` - frame allocation, borders, filtering and synchronisation

## 1. Header

Source path: `E:\_x264_dirty_stash\x264-source\common\frame.c` (mirrored at `common/frame.c`). Role in one sentence: allocates padded multi-plane frames in a single arena, converts external pictures into internal planes, replicates border samples for motion compensation, maintains frame pools and synchronised frame lists, and implements deblock/weight helpers. Marker: **dirty-side behavioural specification**.

## 2. Purpose

A frame must present a clean, padded raster to SIMD motion estimation regardless of input colourspace, bit depth or interlacing, and must be reusable across frames without repeated allocation. This file implements that layout, the border replication required by H.264's unrestricted motion vectors, and the concurrency state (reference counts, completion conditions, bounded frame queues) that makes frame-threaded encoding possible.

## 3. Public interface

All functions are bit-depth templated; see `frame.h.md` for the full list. Key contracts:

- `void x264_frame_delete(x264_frame_t *)`: frees the arena, user-owned callbacks (parameter cleanup, `mb_info_free`, SEI payloads), destroys mutex/condition, and releases OpenCL buffers; shallow duplicates free only the struct itself.
- `int x264_frame_copy_picture(x264_t *, x264_frame_t *dst, x264_picture_t *src)`: validates colourspace and bit depth, transfers metadata and copies/deinterleaves pixels into internal planes; -1 on mismatch.
- `x264_frame_expand_border*` family: replicate edge samples into padding, either for a single macroblock row, for filtered planes, for lowres planes, for one chroma plane, or to fill partial macroblock edges caused by non-multiple-of-16 dimensions.
- `x264_frame_cond_broadcast/wait`, `x264_threadslice_cond_broadcast/wait`, `x264_frame_new_slice`: thread coordination described in `frame.h.md`.
- `x264_frame_push/pop/unshift/shift`, `push_unused/pop_unused`, `push_blank_unused/pop_blank_unused`, `x264_frame_delete_list`, synchronised list operations.
- `x264_weight_scale_plane`, `x264_frame_deblock_row`, `x264_macroblock_deblock`, `x264_frame_filter`, `x264_frame_init_lowres`, `x264_deblock_init`.

## 4. State and data structures

The frame object is allocated with `CHECKED_MALLOCZERO` and then filled by a PREALLOC arena (see `base.h.md`). `align_stride(x, align, disalign)` rounds `x` up to `align` and, if the result is an exact multiple of `disalign`, adds one more `align`; `align_plane_size` similarly adds `max(128, NATIVE_ALIGN)/SIZEOF_PIXEL` when the size is an exact multiple of `disalign`. This "disalign" trick avoids pathological cache/TLB conflicts where several planes would map to the same set.

`frame_internal_csp` maps external colourspaces to one of four internal ones: I400 stays I400; I420-class spaces map to NV12; I422-class spaces map to NV16; I444-class through RGB map to I444; anything else maps to NONE. Internally the encoder therefore handles at most two planes (NV12/NV16), three (I444/I400) or one.

Plane geometry: visible width/height are the padded macroblock dimensions (`mb_width*16`, `mb_height*16`); `i_stride = align_stride(width + PADH2, align, disalign)`, where `align` is `NATIVE_ALIGN/SIZEOF_PIXEL` on generic builds but 64/32/16 bytes divided by pixel size on x86 according to CACHELINE_64/AVX512, CACHELINE_32/AVX, or neither. Vertical padding is `PADV` doubled for interlaced fields. Four luma planes are allocated contiguously when subpel refinement is on and `b_fdec` is set.

## 5. Algorithm / behaviour

**Frame creation.** Initial values: POC -1, type AUTO, QP-plus-one AUTO, PTS/frame/frame-number -1, `i_lines_completed = -1`, picture structure AUTO, field count -1, duration/CPB/DPB fields zero, lookahead clocks -1, `orig = self`. For NV12/NV16, a chroma buffer of `stride*(lines + 2*chroma_padv)` is allocated (and doubled for interlaced fields). For each luma plane, `luma_plane_size = align_plane_size(stride*(lines+2*padv), disalign)`, multiplied by four for reconstructed frames with subpel refinement. Reconstructed frames also allocate macroblock type/partition/MV/ref arrays, per-row bit/QP arrays, an integral buffer for ESA and, if applicable, field/effective-QP maps. Encoded frames allocate lowres buffers (four planes), lowres MV/cost arrays for all B-frame combinations, propagate costs and AQ offsets; an extra `NATIVE_ALIGN` bytes are reserved because mbtree assembly may overread. Mutex and condition variable are created last; failure at any stage frees the frame.

Plane pointers are then established: for NV12/NV16, `plane[1] = buffer[1] + stride*chroma_padv + PADH_ALIGN`; for luma planes, either `filtered[p][i] = buffer[p] + i*luma_plane_size + stride*padv + PADH_ALIGN` for four planes or `filtered[p][0] = plane[p] = buffer[p] + stride*padv + PADH_ALIGN`. Reconstructed frames advance `mv16x16` by one entry after zeroing its first element (so index -1 is readable), and point `integral` into buffer[3]. Encoded frames point `lowres[i]` into four equal-sized slices of `buffer_lowres`, zero lowres MVs, initialise `i_intra_cost` to 0xffff, and zero the inverse-QScale factor to silence a vector false positive.

**Picture copy.** Validates that the destination's internal colourspace matches the source's mapped one, rejects high-depth/8-bit mismatches and rejects v210 except in 10-bit builds. Verifies `src->i_type` lies in the public range; out-of-range forces AUTO. Transfers QP, PTS/reordered PTS, parameter pointer, picture structure, extra SEI, opaque pointer and optional MB info. Pixel paths: YUYV/UYVY are deinterleaved by a motion-compensation helper; v210 is deinterleaved by a 32-bit specialised helper; BGR/BGRA/RGB are deinterleaved with component reordering and vertical flip support; planar paths copy luma and then handle NV12/NV21 (interleaved or swapped), I420/I422/YV12/YV16 (interleaved with optional U/V swap) or I444/YV24 (separate planes). `get_plane_ptr` applies vertical flip by moving to the last row and negating stride, and rejects a width larger than the absolute stride.

**Border expansion.** `pixel_memset` replicates a single pixel value across a run, aligning the destination to word size first and using progressively wider stores (byte, 16-bit, 32-bit, 64-bit) to fill the span. `plane_expand_border` fills: left and right bands for every visible row by replicating the edge pixel (with a chroma-specific shift), then the top and bottom bands by copying whole rows including padding. `x264_frame_expand_border(h, frame, mb_y)` operates on the current 16-row band: it pads top only at the first row, bottom only at the last, extends the height by four rows at a slice end to cover deblocking overread, and for MBAFF expands the two field planes separately. Filtered-border expansion uses `padh = PADH-4`, `padv = PADV-8` and a width extended by eight, because filtering already produced eight extra columns of which up to three may be wrong. Lowres and single-plane chroma expansion call the same helper with full padding. `x264_frame_expand_border_mod16` fills the unused right/bottom area of a non-multiple-of-16 picture by replicating the last valid column/row, with field-aware row selection for interlaced bottom padding. `x264_expand_border_mbpair` replicates the last valid row into the padding for a macroblock pair.

**Filtering and deblocking.** `x264_frame_filter` computes the four half-pel variants of a reference frame's luma plane for a row band; `x264_frame_init_lowres` downscales to four lowres planes; deblocking functions are selected by `x264_deblock_init(cpu, pf, mbaff)` and invoked per row or per macroblock. Weight scaling iterates 16-row strips as described in `frame.h.md`.

**Synchronisation and lists.** See `frame.h.md` section 5 for the exact contracts. Notable detail: `x264_sync_frame_list_pop` broadcasts `cv_empty` while still holding the list mutex, whereas `push` broadcasts `cv_fill` after unlocking; both satisfy the same predicate-protected wait pattern.

## 6. Dependencies

Includes `common.h`. Calls `frame_new`'s PREALLOC macros from `base.h`, motion-compensation copy/deinterleave functions from `h->mc`, CPU feature flags from `h->param.cpu`, OpenCL frame deletion when enabled, and pthread primitives from `osdep.h`. Called by encoder open/close, lookahead, rate control and frame threads.

## 7. Constants and tables

`PADH/PADV = 32`, `PADH2 = PADH_ALIGN + PADH`, align choices 64/32/16 bytes on x86 and 64 or 16 bytes for `NATIVE_ALIGN`; `disalign = 512/SIZEOF_PIXEL` on PowerPC and 1024/SIZEOF_PIXEL elsewhere; lowres disalign is doubled. `align_stride` uses one extra `align` when the rounded value is exactly disaligned. No large constant tables are defined here.

## 8. Performance notes

- **Cache layout:** the disalignment trick is intentional - avoid power-of-two strides/plane sizes that alias cache sets. Preserve it even if it wastes a few bytes. Four luma planes are kept in phase so SIMD cache-line-split code can use a common offset.
- **False sharing:** frame completion state is per-frame and guarded by a mutex; do not move `i_lines_completed` into a shared array.
- **Allocation strategy:** PREALLOC plus frame pooling is the core strategy; avoid per-frame `malloc` in any reimplementation. Reserve the extra NATIVE_ALIGN for mbtree overreads.
- **Dispatch hierarchy:** deblocking, plane copy/deinterleave and weight functions are CPU-dispatched through `x264_deblock_function_t` and `h->mc`; select once, call often.
- **Prefetch:** border replication and filtering are streaming operations; the reference implementation relies on sequential access and wide stores. A clean implementation may prefetch the next row band at the start of each iteration.
- Public references: Drepper (2007) sections on cache conflicts, huge pages and false sharing; x264 `doc/threads.txt`; Intel Optimization Reference Manual on SIMD alignment and store forwarding.

## 9. References

- ITU-T H.264 clause 8.3 (deblocking), 8.4 (motion compensation and sample interpolation), Annex E (weighted prediction), clause 7.4.2.1.4 (HRD).
- Public `x264.h`: picture colourspace flags including VFLIP and HIGH_DEPTH, `x264_picture_t`, `x264_sei_t`.
- x264 `doc/threads.txt` for frame-level dependencies and lookahead buffering.
- Drepper, *What Every Programmer Should Know About Memory* (2007).

## 10. Verification checklist

- Allocate frames for 4:2:0, 4:2:2, 4:4:4 and 4:0:0 inputs and verify plane counts 2/2/3/1 and that all padding regions are readable (write/read under AddressSanitizer).
- Feed a synthetic picture with distinct values per plane through `x264_frame_copy_picture` for each supported colourspace, including VFLIP, and verify the internal planes contain the expected samples.
- Verify bit-depth mismatch and invalid type values are rejected with the documented return code/force-to-AUTO behaviour.
- After border expansion, verify every padding pixel equals the corresponding edge pixel for left/right/top/bottom and that the four filtered planes are expanded independently.
- For a non-multiple-of-16 picture, verify `expand_border_mod16` fills the unused right/bottom area with replicated edge samples.
- Stress the frame pool with thousands of frames at fixed geometry and verify stable allocation counts and no leaks.
- Stress synchronised lists with two producers and two consumers; verify no deadlock, no loss, and capacity is never exceeded.
- Verify `x264_weight_scale_plane` against a scalar reference for several widths/heights and weight values.

## 11. Open questions

- UNKNOWN: whether the exact cache-conflict-avoidance constants (512/1024 pixel disalign, extra NATIVE_ALIGN) are optimal on modern CPUs; they are empirical.
- UNKNOWN: how `i_lines_weighted` behaves when more than one reference frame needs weighting; the source marks it as a FIXME supporting only one.
- UNKNOWN: the precise set of overread distances required by each SIMD mbtree/deblock implementation; the extra alignment padding is a safety margin, not a documented bound.
