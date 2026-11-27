# filters/video/fix_vfr_pts.c — VFR duration/PTS reconstruction filter

*Dirty-side behavioural specification. Reference source: `filters/video/fix_vfr_pts.c`. Behavioural descriptions only; no source or near-verbatim pseudocode is reproduced.*

## 1. Header

- **Source path:** `filters/video/fix_vfr_pts.c`
- **Role:** Implements `fix_vfr_pts_filter`, which computes or repairs per-frame durations and rebuilds continuous PTS for VFR sources, so filters that request frames out of order still see correct timing.
- **Marker:** dirty-side behavioural specification.

## 2. Purpose

Some demuxers supply PTS but no duration; others (notably select_every) reorder frames so their original PTS would be non-monotonic. This filter stores a snapshot of the current frame, reads the next frame to derive the duration as `next_pts - current_pts` (with a floor of 1 to avoid non-monotonic warnings), and then assigns cumulative PTS starting from zero. It only inserts itself when the source is VFR.

## 3. Public interface

Exports `cli_vid_filter_t fix_vfr_pts_filter = { "fix_vfr_pts", NULL, init, get_frame, release_frame, free_filter, NULL }`.

- `init`: no-ops when `info->vfr` is false; otherwise allocates handle, snapshots previous filter/handle, sets `holder_frame = -1`, and installs itself.
- `get_frame`: manages a one-frame holder and one snapshot buffer; see below.
- `release_frame`: skips release for the buffered frame (already released internally); otherwise delegates.
- `free_filter`: frees previous filter, cleans the snapshot buffer if allocated, frees handle.

## 4. State and data structures

`fix_vfr_pts_hnd_t` fields: `prev_hnd`, `prev_filter`, `buffer` (allocated snapshot picture), `holder` (picture slot holding either the current or the next frame), `buffer_allocated`, `holder_frame` (index currently in `holder`, -1 when none), `holder_ret` (status of the read that filled `holder`), `pts` (running cumulative PTS), `last_duration` (fallback duration from the last successfully computed interval). Two picture slots are used: one for the frame being output and one for looking ahead.

## 5. Algorithm / behaviour

**Init.** VFR only; a non-VFR source passes through untouched. The handle is zero-allocated with `holder_frame = -1`.

**Frame delivery.** The logic has two main paths:

1. *Holder already matches the request:* if `frame == holder_frame`, reuse the holder; if its stored read status (`holder_ret`) is nonzero, return that error.
2. *Holder does not match:* if a holder exists at an index between 0 and the requested frame, release it through the previous filter; clear `holder_frame`; then read the requested frame into `holder` via the previous filter. Failure propagates.

Then duration handling:

- If `holder.duration` is already nonzero (the demuxer supplied it), the holder is used directly.
- If not, the filter must look ahead: it lazily allocates `buffer`, sets `holder_frame = frame+1`, copies the holder frame into `buffer` via `x264_cli_pic_copy`, releases the holder frame through the previous filter, and reads frame+1 into the holder (saving the status in `holder_ret`). If that read succeeded, `last_duration = max(holder.pts - buffer.pts, 1)` — the floor of 1 suppresses non-monotonic PTS warnings for zero/negative intervals. The snapshot's duration is set to `last_duration` and returned as the output picture.
- In either case, `output->pts` is overwritten with the running `pts`, and `pts` is advanced by `output->duration`, producing a gapless presentation timeline.

**Release.** If the released frame is the buffered one (index == `holder_frame - 1`), it was already released during the look-ahead copy, so the call is a no-op; otherwise it is delegated.

**Cleanup.** Free previous filter; clean the snapshot buffer if it was allocated; free handle.

## 6. Dependencies

- `filters/video/video.h`, `filters/video/internal.h` (`x264_cli_pic_copy`), `input/input.h` (picture allocation), error macros tagged `"fix_vfr_pts"`.
- Placed in the chain when the source is VFR; works together with `select_every` and `timecode_input`.

## 7. Constants and tables

- Filter name: `"fix_vfr_pts"`.
- Duration floor: 1 tick.
- No options; no other constants.

## 8. Performance notes

- Per frame: one holder check; when duration is missing, one picture copy plus one extra frame read — unavoidable for look-ahead.
- **Reservation sizing:** the snapshot buffer is allocated once on first need; no per-frame allocation.
- **Zero-copy:** the pass-through case (duration already supplied) returns the holder descriptor directly; only the missing-duration path copies.
- **Branch behaviour:** the common case for timecode-based sources has no duration (the timecode filter sets duration), so look-ahead runs every frame; the copy is the main cost.
- Reference: VFR timestamp reconstruction in video encoding.

## 9. References

- `input/timecode.c`, `input/input.h`, `filters/video/internal.c`, `filters/video/select_every.c` (companion specifications).

## 10. Verification checklist

1. Feed a VFR source with PTS but zero duration; confirm output durations equal PTS differences and PTS become gapless from zero.
2. Force a zero or negative PTS difference and confirm duration becomes 1.
3. Feed a source that already supplies duration and confirm the picture passes through unchanged apart from the PTS rewrite.
4. Confirm releasing the buffered frame is a no-op and does not double-release.
5. Confirm non-VFR input does not install the filter.
6. Confirm out-of-order frame requests are handled via the holder logic.
7. Verify clean teardown with ASAN after partial look-ahead.

## 11. Open questions

- UNKNOWN: exact behaviour if the look-ahead read fails (`holder_ret != 0`) — the filter still returns the buffered frame with `last_duration` as its duration; whether that is intended for truncated files is not documented.
