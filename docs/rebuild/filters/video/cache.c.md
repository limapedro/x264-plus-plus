# filters/video/cache.c — frame cache filter

*Dirty-side behavioural specification. Reference source: `filters/video/cache.c`. Behavioural descriptions only; no source or near-verbatim pseudocode is reproduced.*

## 1. Header

- **Source path:** `filters/video/cache.c`
- **Role:** Implements `cache_8`/`cache_10`, a sliding window of decoded frames that allows bounded backward/random frame requests within the window.
- **Marker:** dirty-side behavioural specification.

## 2. Purpose

The source filter forbids backward seeks, yet some filters (notably `select_every`) and B-frame encoding need to re-request earlier frames. The cache filter maintains a ring of N allocated pictures, filled by copying from the previous filter, so any frame still in the window can be served instantly and without asking the demuxer to seek backward. It is compiled twice (8- and 10-bit variants) because picture allocation depends on bit depth.

## 3. Public interface

Exports `cli_vid_filter_t cache_8_filter` / `cache_10_filter` with names `"cache_8"` / `"cache_10"`, and the standard vtable. `init` receives the cache size encoded in `opt_string` as an integer pointer (an internal calling convention used by `select_every`, which calls `x264_init_vid_filter("cache_<depth>", ..., (void*)max_rewind)`).

- `init`: a size ≤ 0 is a no-op (returns 0 without changing `*handle`/`*filter`); otherwise allocates the ring, snapshots the previous handle/filter, and becomes active.
- `get_frame`: serves a frame from the window, shifting old frames out and reading new ones from the previous filter as needed.
- `release_frame`: no-op (the previous filter's frame was released during cache fill).
- `free_filter`: frees the previous filter, then all cache pictures and the cache structure.

## 4. State and data structures

`cache_hnd_t` fields: `prev_hnd`, `prev_filter`, `max_size` (window capacity), `first_frame` (index of the oldest cached frame), `cache` (array of `max_size` allocated `cli_pic_t` pointers plus a NULL terminator), `cur_size` (number of valid frames currently in the window), `eof` (index one past the last readable frame, or 0 when not at EOF). Helper macro `LAST_FRAME = first_frame + cur_size - 1`.

The ring is managed by moving the front `cli_pic_t*` to the back (via the frame-list push/shift helpers) after overwriting it with a newly read frame — so the same allocated picture objects are recycled; only their contents change.

## 5. Algorithm / behaviour

**Init.** The requested size is interpreted as an `intptr_t`. If ≤ 0, no filter is inserted. Otherwise a handle is allocated, `max_size` stored, and an array of `max_size+1` picture pointers allocated; each of the first `max_size` entries is a heap `cli_pic_t` with planes allocated by `x264_cli_pic_alloc` for the current csp/size, and the final entry is NULL (so the frame-list helpers can operate). `first_frame`/`cur_size`/`eof` start at 0. The previous handle/filter are snapshotted and the wrapper installed.

**Filling (`fill_cache`).** Given a requested `frame`:

1. Compute `shift = frame - LAST_FRAME`. If non-positive or EOF already reached, nothing is done.
2. The first new frame to read is the larger of "just past the current window" and "frame - max_size + 1" (so a large jump re-bases the window to end at the requested frame).
3. `first_frame` becomes the minimum of "old first + shift" and that re-based start; `cur_size` is reduced by `shift` (clamped at zero).
4. While `cur_size < max_size`, take the front picture slot, ask the previous filter for the next frame into a temporary picture, copy it into that slot with `x264_cli_pic_copy`, release the temporary frame through the previous filter, move the slot to the back of the ring, advance the frame index, and increment `cur_size`. Any failure sets `eof` to the failed frame index and stops.

Note that `eof` is used as a sentinel: once set, `fill_cache` refuses further reads.

**Getting a frame.** If `frame < first_frame`, an error is logged (the requested frame has already been shifted out). `fill_cache` is called; then if `frame > LAST_FRAME`, EOF is reported. Otherwise the index into the ring is computed as `frame - (eof ? eof - max_size : first_frame)` — this subtle expression keeps indexing correct after an EOF truncated the fill — and the cached picture is returned by structure assignment.

**Release/cleanup.** `release_frame` is intentionally a no-op because the previous filter's frame was already released during `fill_cache`. `free_filter` calls the previous filter's `free`, cleans and frees every cached picture, the pointer array, and the handle.

## 6. Dependencies

- `filters/video/video.h`, `filters/video/internal.h` (`x264_cli_pic_copy`), `common/common.h` (`x264_frame_shift`/`x264_frame_push` list helpers), `input/input.h` (picture allocation/cleanup), error macros tagged `cache_8`/`cache_10`.
- Inserted internally by `select_every.c` and may be inserted by the frontend for B-frame lookahead.

## 7. Constants and tables

- Filter names: `"cache_8"`, `"cache_10"` selected by `BIT_DEPTH`.
- Cache terminator: a NULL pointer after the last picture slot.
- Initial buffer growth: none — all `max_size` pictures are allocated up front in `init`.
- No numeric limits on `max_size` beyond allocation success.

## 8. Performance notes

- Per frame in steady state: one ring-slot selection plus one structure assignment — no copy. Copies occur only when the window advances (typically one per output frame).
- **Reservation sizing:** all pictures are allocated once in `init`; the ring recycles picture objects, so no per-frame `malloc`.
- **Zero-copy:** impossible for cached frames (they must survive), so `x264_cli_pic_copy` is the necessary cost. Use aligned picture strides to speed those copies.
- **Branch behaviour:** the common path (frame already in window) skips `fill_cache` entirely — very cheap.
- **Threading:** none; the cache is accessed by the encoding thread(s) through filter calls.
- Reference: sliding-window/ring-buffer caching patterns.

## 9. References

- `filters/video/internal.c`, `filters/video/select_every.c`, `filters/video/source.c` (companion specifications).

## 10. Verification checklist

1. Insert a cache of size 4 over a test source; request frames 0..9 sequentially and verify data and no spurious rewinds.
2. After reaching frame 9, request frame 6 and verify it is served from the ring without touching the source.
3. Request a frame older than the window and confirm an error.
4. Jump far ahead (e.g. 9 → 40) with a small cache and confirm the window re-bases correctly.
5. Read to EOF and confirm subsequent requests fail without extra source reads.
6. Verify `release_frame` is a no-op and no double-release occurs.
7. Verify both 8-bit and 10-bit variants allocate pictures with correct depth.

## 11. Open questions

- UNKNOWN: exact semantics of `eof` being stored as a frame index and the derived index expression when a partial fill at EOF leaves fewer than `max_size` valid frames — verify with a short source and a large cache.
