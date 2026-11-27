# input/ffms.c — FFMS2 (FFmpegSource) demuxer

*Dirty-side behavioural specification. Reference source: `input/ffms.c`. Behavioural descriptions only; no source or near-verbatim pseudocode is reproduced.*

## 1. Header

- **Source path:** `input/ffms.c`
- **Role:** Implements the `ffms_input` demuxer, which uses the FFMS2 library to index a file once and then provide random-access decoded frames and timestamps.
- **Marker:** dirty-side behavioural specification.

## 2. Purpose

FFMS2 provides a simple index-once/seek-anywhere API on top of FFmpeg, which suits frame-accurate random access and VFR. This module creates or reuses an index file, creates a video source, reports source properties, and delivers frames with PTS in a reduced timebase that avoids 64-bit overflow. Like `lavf.c`, it is a convenience path rather than a requirement for a clean-room encoder.

## 3. Public interface

Exports `const cli_input_t ffms_input` with the standard callbacks. `release_frame` is **NULL** (FFMS2 owns a single shared frame buffer); `picture_alloc` creates a descriptor-only picture with four planes.

- `open_file`: initialises FFMS2, creates or reads an index (optionally with progress callback and on-disk cache), selects the first video track, creates a video source with the requested seek mode, reads frame 0 for properties, and derives a safe timebase for VFR.
- `read_frame`: fetches frame N, copies plane pointers/linesizes, and supplies PTS from FFMS2's frame info.
- `picture_clean` zeroes the descriptor; `close_file` destroys the video source and handle.

## 4. State and data structures

`ffms_hnd_t` fields: `video_source` (FFMS2 source object), `track` (frame-info track used for PTS, only when VFR), `reduce_pts` (number of right shifts applied to timestamps), `vfr_input`, `num_frames`, and `time` (last progress-callback timestamp). Ownership: the index is destroyed immediately after creating the video source; the source and track live as long as the handle. The library's single shared frame buffer is why `thread_safe` is reported as 0.

## 5. Algorithm / behaviour

**Index creation/reuse.** `FFMS_Init(0, 1)` initialises the library. Seek mode is chosen from `opt->seek`: normal seeking when requested, otherwise linear-no-rewrite (faster indexing, no seeking during indexing). If `opt->index_file` names an existing index whose modification time is newer than the input's, it is read and validated with `FFMS_IndexBelongsToFile`; a mismatched index is discarded. Otherwise a new indexer is created, an optional progress callback is installed (throttled to `UPDATE_INTERVAL`, printing a carriage-return progress line and updating the console title), and indexing runs with abort-on-error. After indexing, the module prints a clearing sequence and optionally writes the index to disk (a write failure is only a warning).

**Source creation.** The first video track is located; a video source is created with a cache window of 1 and the chosen seek mode. The index is then destroyed. Failure to find a track or create the source is fatal.

**Property extraction.** Video properties give frame count, SAR, fps, and VFR flag. Frame 0 is fetched to obtain encoded width/height and pixel format. The `handle_jpeg` helper maps deprecated JPEG-range pixel formats to limited-range equivalents and sets `fullrange`; the format is marked `X264_CSP_OTHER` for later normalisation. Interlacing and top-field-first are copied, and full-range is OR-ed with FFMS2's colour-range field.

**Timebase reduction (VFR only).** FFMS2 timestamps are in milliseconds and its timebase uses 64-bit fields, which can exceed the 32-bit H.264 limit. The module starts from `timebase_num = Num` and `timebase_den = Den * 1000` (seconds), then repeatedly right-shifts both by one and increments `reduce_pts` until numerator ≤ UINT32_MAX and denominator ≤ INT32_MAX. The resulting pair is reported in `info`.

**Frame delivery.** Out-of-range indices fail. `FFMS_GetFrame` returns the frame; linesizes and plane pointers are copied; width/height/csp are set as at open. If VFR is active, frame info supplies PTS, which is right-shifted by `reduce_pts`; an invalid PTS produces an error advising `--force-cfr`/`--fps`. Duration is left at zero (the timecode/fix_vfr_pts path can compute it elsewhere).

## 6. Dependencies

- FFMS2 headers (`ffms.h`), FFmpeg pixel-format headers (`libavcodec/avcodec.h`, `libswscale/swscale.h` for format constants), `input/input.h` helpers, `x264_mdate`/`x264_cli_set_console_title` for progress reporting.
- `handle_jpeg` is duplicated from `lavf.c` but behaves identically.
- The resize filter normalises the `X264_CSP_OTHER` pixel format.

## 7. Constants and tables

- `PROGRESS_LENGTH = 36` characters for the indexing progress line.
- Cache window passed to FFMS2: 1 frame.
- Seek modes: normal vs. linear-no-rewrite.
- Deprecated JPEG formats mapped as in `lavf.c` (YUVJ420P/422P/444P → limited-range with `fullrange = 1`).
- Timebase reduction targets: numerator ≤ UINT32_MAX, denominator ≤ INT32_MAX.
- Error/log tag: `"ffms"`.

## 8. Performance notes

- Indexing is one-off and can dominate startup; reuse an on-disk index when available.
- Per-frame cost: one `FFMS_GetFrame` plus pointer copies.
- **Zero-copy:** planes alias FFMS2's buffer; keep descriptor-only pictures.
- **Threading:** not thread-safe (single frame buffer); never apply input threading.
- **Reservation sizing:** one index, one video source, one frame-info track; all allocated once.
- Reference: FFMS2 API documentation (`FFMS_CreateVideoSource`, `FFMS_GetFrame`, `FFMS_GetTimeBase`).

## 9. References

- FFMS2 documentation: <https://github.com/FFMS/ffms2>.
- FFmpeg pixel format definitions (`AV_PIX_FMT_*`).
- `input/lavf.c`, `input/input.h`, `filters/video/resize.c` (companion specifications).

## 10. Verification checklist

1. Index a small file with and without an on-disk index; confirm reuse when the index is newer than the source.
2. Confirm progress output is throttled and produces a clean line at completion.
3. Confirm the first video track is selected and frame count/SAR/fps are reported.
4. Confirm deprecated JPEG-range pixel formats set `fullrange = 1`.
5. Verify VFR PTS equal the FFMS2 PTS shifted right by `reduce_pts`, and that the reported timebase fits the 32-bit limits.
6. Confirm an invalid PTS is rejected with the documented advice.
7. Confirm `thread_safe = 0` and no `release_frame` callback.
8. Verify clean close destroys the video source and frees the handle.

## 11. Open questions

- UNKNOWN: the exact value of `UPDATE_INTERVAL` (defined in a shared header, not in this file).
- UNKNOWN: whether duration-zero frames are later repaired by `fix_vfr_pts` or left as zero for the muxer; FFMS2 output has no explicit duration here.
