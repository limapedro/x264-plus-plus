# filters/video/source.c — source filter (demuxer→filter adapter)

*Dirty-side behavioural specification. Reference source: `filters/video/source.c`. Behavioural descriptions only; no source or near-verbatim pseudocode is reproduced.*

## 1. Header

- **Source path:** `filters/video/source.c`
- **Role:** Implements `source_filter`, the mandatory head of every video-filter chain, which converts the demuxer API into the filter API.
- **Marker:** dirty-side behavioural specification.

## 2. Purpose

Demuxers deliver frames through `cli_input.read_frame` with an opaque handle; filters expect `cli_vid_filter_t.get_frame` with the filter-chain handle abstraction. The source filter bridges these two worlds: it wraps the demuxer handle, allocates the staging picture, and enforces the forward-only frame-order rule that not all demuxers can honour backward seeks. It is the first filter in the registry and is never removed.

## 3. Public interface

Exports `cli_vid_filter_t source_filter = { "source", NULL, init, get_frame, release_frame, free_filter, NULL }`.

- `init`: allocates a source handle, allocates a picture via the active demuxer's `picture_alloc` using the source `video_info_t`, stores the demuxer handle, replaces `*handle` with the new handle, and sets `*filter` back to `source_filter`.
- `get_frame`: rejects backward/duplicate requests; otherwise reads frame N from the demuxer into the staging picture and returns a copy of its descriptor.
- `release_frame`: forwards to the demuxer's `release_frame` (if present).
- `free_filter`: cleans the staging picture, closes the demuxer, and frees the handle.

## 4. State and data structures

`source_hnd_t` fields: `pic` (the single staging `cli_pic_t`), `hin` (the wrapped demuxer handle), `cur_frame` (index of the last successfully delivered frame, initially -1). Invariant: `cur_frame` only increases; a request with `frame <= cur_frame` is rejected, because the demuxer's single staging picture cannot preserve older frames.

## 5. Algorithm / behaviour

**Init.** The handle is zero-allocated and `cur_frame` set to -1. The demuxer's `picture_alloc` is called with the source csp/width/height; failure is fatal. The incoming handle is saved as `hin`, `*handle` becomes the wrapper, and `*filter` is set to `source_filter` (idempotent since it is already the chain head).

**Frame delivery.** If the requested frame is not greater than `cur_frame`, the call fails immediately — this is the explicit prohibition on back-seeking documented at the top of the file. Otherwise the demuxer's `read_frame` is called with the staging picture and requested index; failure propagates. On success `cur_frame` is updated and the staging picture is copied wholesale into the caller's output descriptor (a structure assignment, so plane pointers are shared, not copied).

**Release/cleanup.** `release_frame` delegates to the demuxer's `release_frame` if the callback exists. `free_filter` cleans the staging picture via the demuxer's `picture_clean`, closes the demuxer via its `close_file`, and frees the wrapper.

## 6. Dependencies

- `filters/video/video.h` (filter vtable), `input/input.h` (global `cli_input` with its `picture_alloc`/`read_frame`/`release_frame`/`picture_clean`/`close_file`).
- No other project modules.

## 7. Constants and tables

- Filter name: `"source"`.
- Initial `cur_frame`: -1.
- No options (`help` and `opt_string` unused).

## 8. Performance notes

- Per frame: one demuxer call and one structure assignment — no pixel copy.
- **Zero-copy:** the output picture aliases the demuxer's staging picture; downstream filters must release frames promptly so it can be reused.
- **Reservation sizing:** the staging picture is allocated once in `init`.
- **Branch behaviour:** the backward-request check is a single compare on the hot path.
- Reference: x264 CLI filter chain design notes (source filter as chain head).

## 9. References

- `filters/video/video.h`, `filters/video/video.c`, `input/input.h` (companion specifications).

## 10. Verification checklist

1. Confirm the source filter is first in the registry and that `init` keeps it as the active filter.
2. Deliver frames 0,1,2 and confirm correct descriptors and pixel data.
3. Request frame 1 after frame 2 and confirm rejection.
4. Confirm `release_frame` reaches the demuxer's callback.
5. Confirm no pixel data is copied (pointer identity between staging and output).
6. Confirm `free_filter` closes the demuxer exactly once.

## 11. Open questions

- UNKNOWN: how a filter that needs rewind (e.g. select_every) avoids the backward-request rejection — it inserts a `cache` filter above source, which serves stored frames without asking source to rewind (see `cache.c` spec).
