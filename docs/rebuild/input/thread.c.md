# input/thread.c — threaded input wrapper (read-ahead demuxer)

*Dirty-side behavioural specification. Reference source: `input/thread.c`. Behavioural descriptions only; no source or near-verbatim pseudocode is reproduced.*

## 1. Header

- **Source path:** `input/thread.c`
- **Role:** Implements a one-worker read-ahead wrapper around any thread-safe demuxer, so decoding/parsing of frame N+1 overlaps encoding of frame N.
- **Marker:** dirty-side behavioural specification.

## 2. Purpose

For slow input formats (AviSynth scripts, network streams, high-bitrate sources), the encoder can sit idle waiting for the next frame. This module spawns a single worker thread that prefetches the next frame while the main thread processes the current one. It is compiled twice — once per supported bit depth (8 and 10) — yielding `thread_8_input` and `thread_10_input`. It is a pure wrapper: it does not parse any media; it delegates to the currently selected `cli_input` and only orchestrates timing and ownership.

## 3. Public interface

Exports `const cli_input_t thread_8_input` and `thread_10_input` (generated from a bit-depth-parameterised template via the `x264_glue3` name-pasting macro). The vtable is the standard six functions, with these special contracts:

- `open_file` is called *after* the underlying demuxer has already been opened: it receives the underlying handle in `*p_handle` and the underlying `video_info_t` in `info`. It copies the current `cli_input` vtable, allocates a single staging picture using that vtable, records `info->num_frames`, initializes a one-thread pool, and replaces `*p_handle` with the wrapper handle. It requires `picture_alloc` on the wrapped module to succeed.
- `read_frame` is strictly sequential and enforces the prefetch contract; see below.
- `release_frame`, `picture_alloc`, `picture_clean` forward to the wrapped module.
- `close_file` waits implicitly via pool deletion, cleans and closes the wrapped module, and frees wrapper state.

## 4. State and data structures

`thread_hnd_t` contains: `input` (a copy of the wrapped `cli_input_t`), `p_handle` (the wrapped handle), `pic` (the staging `cli_pic_t` that the worker fills), `pool` (a one-thread `x264_threadpool_t`), `next_frame` (index currently being/should be prefetched, or -1 when no prefetch pending), `frame_total` (total frame count from `info->num_frames`), and `next_args` (pointer to a reusable job argument block).

`thread_input_arg_t` contains: `h` (owning wrapper), `pic` (destination picture, always `&h->pic`), `i_frame` (frame index to read), and `status` (return code of the wrapped `read_frame`). A single argument block is reused for every job.

Invariants: at most one prefetch job is in flight; the worker writes only into `h->pic` and `next_args->status`; the main thread must wait for the job before reusing `h->pic` or inspecting `status`.

## 5. Algorithm / behaviour

**Open.** The wrapper allocates its handle and immediately asks the wrapped module to allocate a staging picture with the source's csp/width/height; failure is fatal. It snapshots the global `cli_input` into `input`, stores the incoming handle and total frame count, allocates/initialises the job argument block (`status = 0`), creates a thread pool with one worker, and returns the new handle. Note `next_frame` starts at -1: no job pending.

**Worker.** The worker job calls the wrapped module's `read_frame` with the requested frame index into the staging picture and stores the return value in `status`. It does not touch any synchronisation primitives itself; the pool's wait/join provides the ordering.

**Read handshake.** For a call requesting frame `i`:

1. If a prefetch is pending (`next_frame >= 0`), the main thread waits on the pool for the outstanding job and ORs its `status` into the local return code. This guarantees the staging picture is complete and no longer being written.
2. If `next_frame == i` (the common sequential case), the staging picture is swapped wholesale into the caller's `cli_pic_t` using the structure-swap macro — O(1), no pixel copy.
3. Otherwise (a non-sequential request), the pending staging picture is released through the wrapped `release_frame`, and the wrapped module is asked to read frame `i` directly into the caller's picture. This supports backward/forward jumps at the cost of discarding the prefetch.
4. A new job is queued for frame `i+1` if the total frame count is unknown (`frame_total == 0`) or `i+1 < frame_total`. The job's `pic` is reset to the staging picture, its `i_frame` to `i+1`, and `next_frame` is updated to `i+1`. If `i+1` is the last frame or beyond, `next_frame` is set to -1 and no job is queued.
5. The accumulated status is returned.

The frame-total guard prevents prefetching past end-of-file, which would otherwise produce spurious errors from a wrapped demuxer.

**Release/cleanup.** `release_frame` forwards to the wrapped module (if present). `picture_alloc`/`picture_clean` also forward. `close_file` deletes the thread pool (which drains any pending job), cleans the staging picture, closes the wrapped file, and frees the argument block and wrapper.

## 6. Dependencies

- `input/input.h` (the `cli_input_t` vtable and `cli_pic_t` structure-swap macro), `common/common.h` for `x264_threadpool_init/run/wait/delete` and `XCHG`.
- The wrapped demuxer must have reported `thread_safe = 1` (this is the frontend's responsibility to check; lavf and ffms explicitly report 0).
- Wrapped by `timecode_input` if both threading and timecodes are requested (ordering is a frontend concern; see open questions).

## 7. Constants and tables

- One worker thread per instance (pool size 1).
- Two template instantiations selected at build time by `BIT_DEPTH` (8 or 10), producing the names `thread_8_input` / `thread_10_input`.
- No constants specific to the timing algorithm; the prefetch horizon is exactly one frame.

## 8. Performance notes

- Hotness: this wrapper is on the per-frame path; the structure swap makes the handoff O(1).
- **Threading:** one producer thread is enough to hide I/O latency for a single encode thread; more workers would complicate ordering and provide little benefit. Keep the pool size at 1.
- **Zero-copy:** the swap transfers plane pointers, not pixel data. Avoid calling `x264_cli_pic_copy` here — that would defeat the purpose.
- **Reservation sizing:** a single reusable job argument block avoids per-frame allocation; the staging picture is allocated once.
- **Branch behaviour:** the sequential fast path (`next_frame == i`) is the expected case; the non-sequential path is cold.
- Reference: x264 documentation on `--input-threads`/frame threading and the threadpool implementation in `common/threadpool.c`.

## 9. References

- `input/input.h`, `input/thread.c` companion specs; `common/threadpool` interface.
- Generic producer/consumer single-buffer prefetching pattern.

## 10. Verification checklist

1. With a slow or instrumented wrapped demuxer, confirm frame N+1 read starts before frame N encoding finishes (overlap).
2. Confirm sequential reads deliver correct pixel data and PTS/DTS/opaque fields after the structure swap.
3. Force a non-sequential request and confirm the stale prefetched frame is released and the requested frame is read correctly.
4. Confirm no prefetch is issued for the final frame (no spurious EOF error).
5. Confirm `status` failures from the worker propagate to the caller's return value.
6. Destroy the wrapper while a job may be pending and confirm clean shutdown (no use-after-free).
7. Verify both `thread_8_input` and `thread_10_input` exist and delegate to the active `cli_input`.

## 11. Open questions

- UNKNOWN: the exact order in which the frontend layers `thread_*` and `timecode_input` when both `--threads`-style input threading and a timecode file are requested; the source files do not encode the composition order.
- UNKNOWN: whether `release_frame` of the caller's picture is required after a structure swap in all cases — the source relies on the caller releasing each delivered picture and does not double-release.
