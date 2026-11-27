# input/timecode.c — timecode/VFR overlay demuxer

*Dirty-side behavioural specification. Reference source: `input/timecode.c`. Behavioural descriptions only; no source or near-verbatim pseudocode is reproduced.*

## 1. Header

- **Source path:** `input/timecode.c`
- **Role:** Wraps another demuxer and supplies per-frame presentation timestamps and durations parsed from a timecode (tcfile) file, converting a CFR source into a VFR source.
- **Marker:** dirty-side behavioural specification.

## 2. Purpose

To encode variable frame rate video, the encoder needs a PTS and a duration for every input frame. Raw and Y4M sources provide neither. This module reads an external timecode file in one of two text formats, derives (or accepts) a timebase, converts each timestamp into integer ticks in that timebase, and attaches them to frames read from the wrapped demuxer. It also handles the common case where the timecode file is shorter than the video by extrapolating at an assumed constant frame rate.

## 3. Public interface

Exports `const cli_input_t timecode_input` with the standard six callbacks, all forwarding image work to the wrapped demuxer:

- `open_file` receives the *timecode file* as `psz_filename` and the already-opened wrapped demuxer via `*p_handle`/`info`. It parses `opt->timebase` if present, reads the tcfile, fills `info->timebase_num/den` and sets `info->vfr = 1`.
- `read_frame` delegates to the wrapped demuxer, then computes `pic->pts` and `pic->duration`.
- `release_frame`, `picture_alloc`, `picture_clean` forward.
- `close_file` frees parsed timestamps, closes the wrapped demuxer, frees the handle.

The tcfile must be a regular file (pipes are rejected). Two formats are accepted: version 1 (frame-range/fps sections with an `assume` line) and version 2 (one timestamp in milliseconds per line).

## 4. State and data structures

`timecode_hnd_t` fields: `input` (snapshot of wrapped `cli_input_t`), `p_handle` (wrapped handle), `auto_timebase_num` / `auto_timebase_den` (flags: numerator or denominator must be derived), `timebase_num` / `timebase_den` (uint64 working values), `stored_pts_num` (number of parsed timestamps), `pts` (array of int64 ticks), `assume_fps` (double, fallback frame rate), `last_timecode` (double seconds of the final parsed timestamp). Invariants: `pts[]` is strictly increasing; `timebase_num/den` fit in uint32 on success; `vfr = 1` is reported to the caller.

Constants: `DOUBLE_EPSILON = 5e-6` (tolerance when approximating a floating FPS with a rational), `MKV_TIMEBASE_DEN = 1000000000` (1 GHz Matroska-style denominator used when auto-deriving a timebase numerator).

## 5. Algorithm / behaviour

### Timebase option handling
If `opt->timebase` is `num/den`, both are parsed (uint64) and range-checked against H.264's 32-bit limit. A single number is treated as a numerator with denominator to be derived. If no option is given, both numerator and denominator are derived automatically: the numerator starts at `info->fps_den` (so ticks initially align with frame durations) and the denominator starts at 0.

### Format detection
The first line must match `# timecode format v1` or `# timestamp format v1`, or the v2 equivalent, with version 1 or 2 only.

### Version 1 parsing
The file consists of `start,end,fps` sections and an `assume <fps>` line. The parser:

1. Skips comment/blank lines, then finds the `assume`/`Assume` line and stores its floating frame rate; a non-positive value is rejected.
2. Records the stream position just after that line, then scans section lines validating monotonic, non-overlapping `start`/`end` ranges with positive fps. A special comment `# TDecimate Mode 3: Last Frame = N` supplies an explicit stored-frame count. If absent, the count is derived as `end + 2` from the last section.
3. Allocates an array of doubles of that length and rewinds to the recorded position.
4. Converts the assumed fps and each section fps to rational approximations using `correct_fps` (below), which also accumulates the least-common-multiple denominator when auto-deriving. The first timestamp is zero; gaps before a section are filled with steps of `1/assume_fps`; each section's range is filled with steps of `1/section_fps`; any remaining frames use the assumed rate.
5. If only the numerator is automatic (denominator given), the parser tries a Matroska-style timebase: for each fps it computes a denominator from `1e9 / fps_significand`, takes the GCD across sections, then rebuilds all timestamps using that timebase. This produces timebases friendly to Matroska's 1 ms/1 ns conventions.
6. Stores the corrected `assume_fps` and the last timestamp.

`correct_fps(fps, h)` decomposes the floating value into a decimal significand and a power-of-ten exponent, then searches increasing denominators `i * timebase_num` for a rational whose significand matches within `DOUBLE_EPSILON`, bounded by UINT32_MAX. If the denominator is automatic, the resulting numerator is folded into a running LCM (capped at UINT32_MAX). `try_mkv_timebase_den` computes a timebase numerator as the GCD of rounded `1e9/fps` values across all sections.

### Version 2 parsing
Each non-comment line holds one timestamp in milliseconds, converted to seconds by multiplying by `1e-3`. The first timestamp is read directly; subsequent ones must be strictly greater than their predecessor. If only one timestamp exists, the denominator is set to the source `fps_num`; otherwise, when the denominator is automatic, each inter-frame interval's reciprocal FPS is approximated as a rational (same significand/exponent technique) and the LCM of the numerators becomes the timebase denominator. When only the numerator is automatic, the Matroska-style GCD path is used. `assume_fps` is set to the reciprocal of the final interval (or the source FPS for a single timestamp), and `last_timecode` to the final timestamp.

### Timebase finalisation and tick conversion
When any auto flag is set, numerator and denominator are reduced by their GCD and an informational log line reports the generated timebase. When neither is auto, the denominator must be nonzero and ≤ UINT32_MAX. The parsed double timestamps are converted to integer ticks with `round(time_seconds * timebase_den / timebase_num)`; strict monotonicity is re-checked in tick space (a too-coarse timebase is an "invalid timebase or timecode" error).

### Frame delivery
`get_frame_pts(frame, real_frame)` returns `pts[frame]` while stored data exists. Beyond that, it extrapolates: on the first real request past the end it logs an informational message and frees the pts array (subsequent calls stay silent), then advances `last_timecode` by `1/assume_fps` when `real_frame` is set and converts to ticks. `read_frame` asks for the current frame with `real_frame = 1` (advancing state) and the next frame with `real_frame = 0` (peeking), so duration is `pts(frame+1) - pts(frame)`.

## 6. Dependencies

- `input/input.h` (vtable, `cli_pic_t` PTS/duration fields), `x264cli.h` for `x264_cli_log`, `x264_fopen`, `x264_is_regular_file`, `lcm`/`gcd` helpers.
- The wrapped demuxer (captured from global `cli_input` at open time).
- Used by the frontend whenever `--tcfile-in` is supplied; the Matroska/MP4 muxers consume the resulting timebase.

## 7. Constants and tables

- `DOUBLE_EPSILON = 5e-6`: rational-approximation tolerance.
- `MKV_TIMEBASE_DEN = 1000000000`: target denominator for auto-generated Matroska-friendly timebases.
- Accepted tcfile versions: 1 and 2 only.
- Timebase fields after finalisation must not exceed UINT32_MAX (H.264 VUI limitation).

## 8. Performance notes

- Parsing is one-off at open; the per-frame cost is two array lookups/one extrapolation — negligible.
- **Reservation sizing:** the pts array is allocated once with the exact stored count; no per-frame allocation.
- **Branch behaviour:** the "past end of file" path frees the array once and then stays on the cheap extrapolation path.
- **Threading:** no internal threading; if combined with input threading, the composition order is a frontend choice (see open questions).
- No SIMD relevance. Reference: Matroska timecode-scale conventions and the `tcfile` format documentation.

## 9. References

- Public `tcfile` / timecode file format documentation (MKVToolNix `--timecode-file` v1/v2 format).
- Matroska specification, TimecodeScale element (1,000,000 ns default).
- `input/input.h`, `input/thread.c` (companion specifications).

## 10. Verification checklist

1. Feed a v2 file with timestamps `0, 33.333, 66.667, ...` and confirm PTS are monotonic ticks in the generated timebase and durations match the intervals.
2. Feed a v1 file with an `assume` line and one `start,end,fps` section; confirm gaps use the assumed rate and the section uses its own fps.
3. Supply an explicit `timebase` option (`num/den`, and single-number numerator) and confirm no auto-generation log and exact tick conversion.
4. Confirm a v2 file with a single timestamp sets the denominator to the source fps.
5. Confirm non-monotonic timestamps are rejected with the `timecode` tag.
6. Confirm a non-regular tcfile (pipe) is rejected.
7. Extend a video beyond the tcfile and confirm the one-time "missing data" log and constant-fps extrapolation.
8. Verify info->vfr is set to 1 and timebase values are written into `video_info_t`.

## 11. Open questions

- UNKNOWN: exact interaction when both input threading and a timecode file are enabled — which wrapper is outermost is not visible in these files.
- UNKNOWN: whether the frontend pre-supplies `info->fps_num/fps_den` for V2 single-timestamp files; the module reads them from `info`, so a zero would yield a zero denominator and an error.
