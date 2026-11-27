# output/mp4_lsmash.c — MP4 muxer (L-SMASH backend)

*Dirty-side behavioural specification. Reference source: `output/mp4_lsmash.c`. Behavioural descriptions only; no source or near-verbatim pseudocode is reproduced.*

## 1. Header

- **Source path:** `output/mp4_lsmash.c`
- **Role:** Implements an alternative `mp4_output` using L-SMASH, with support for fragmented MP4 when writing to non-seekable destinations (including stdout).
- **Marker:** dirty-side behavioural specification.

## 2. Purpose

Where the GPAC backend requires a seekable file, this backend can stream fragmented MP4 to stdout or pipes. It also carries richer colour metadata (primaries/transfer/matrix/full-range) and optional brands for recovery groups, though recovery is explicitly disabled here. Its timing math mirrors `mp4.c`, making the two backends behaviourally close apart from fragmentation and metadata.

## 3. Public interface

Exports `const cli_output_t mp4_output` (the backend chosen at build time; only one of `mp4.c`/`mp4_lsmash.c` is compiled).

- `open_file`: determines whether the destination is regular; sets fragmented mode for non-regular/stdout; creates root, opens file, creates a video summary with codec type `AVC1`.
- `set_param`: computes timescale/delay as in `mp4.c`, selects brands (MP42/MP41/ISOM, plus AVC1/ISO6 when recovery is enabled), sets file/movie/track/media parameters, colour metadata, and stores movie/video timescales.
- `write_headers`: builds structured H.264 codec-specific data from SPS/PPS, adds bitrate info, adds a sample entry, and buffers the SEI bytes for the first frame.
- `write_frame`: creates a sample sized `i_size + sei_size`, prepends SEI, computes DTS/CTS with delay compression, sets random-access flags, flushes fragmented samples on keyframes, appends the sample, and records previous DTS.
- `close_file`: flushes pooled samples with the final delta, creates an explicit timeline map (edit list), finishes the movie, and destroys state.

## 4. State and data structures

`mp4_hnd_t` fields: `p_root`, `summary`, `b_stdout`, `i_movie_timescale`, `i_video_timescale`, `i_track`, `i_sample_entry`, `i_time_inc`, `i_start_offset`, `i_first_cts`, `i_prev_dts`, `i_sei_size`/`p_sei_buffer`, `i_numframe`, `i_init_delta`, `i_delay_frames`, `b_dts_compress`, `i_dts_compress_multiplier`, `b_use_recovery` (always 0), `b_fragments`, `file_param`. Invariants: `i_first_cts` is 0 with DTS compression, otherwise the negated initial DTS scaled to ticks; SEI buffer exists until the first frame; fragmented mode implies no final edit-box creation at close (it is created at first frame instead).

## 5. Algorithm / behaviour

**Open.** Regularity is determined by filename: `"-"` is non-regular by definition; other paths are probed with `x264_is_regular_file_path` and then by opening/closing a handle. `b_fragments = !regular`, `b_stdout = (filename == "-")`. The handle is allocated, `b_use_recovery` is set to 0 (the source comments that recovery is not really supported), root and file are created/opened, and fragmented mode is OR-ed into L-SMASH's file mode when streaming. A video summary is created and its sample type set to AVC1.

**Parameter configuration.** Delay/timescale math is identical to `mp4.c`: delay frames 0/1/2, multiplier `compress*delay+1`, media timescale `timebase_den*multiplier`, `i_time_inc = timebase_num*multiplier`, timescale limited to UINT32_MAX. Brands start with MP42, MP41, ISOM; if recovery were enabled, AVC1 would be added and ISO6 for open GOP. Movie parameters are set and movie timescale read back. A video track is created; width/height and 16.16 display dimensions are set (SAR stretches one dimension), along with PAR and colour primaries/transfer/matrix/full-range. Track parameters enable the track in movie/preview, and media parameters set the timescale (plus roll/RAP grouping only if recovery were enabled). Video timescale is read back.

**Headers.** SPS/PPS are stripped of 4-byte prefixes. A structured H.264 codec-specific object is created with `lengthSizeMinusOne = 3`, SPS and PPS appended, and attached to the summary; a bitrate-info object is added if L-SMASH provides one. The sample entry is added to the track. SEI is copied into a private buffer for deferred prepending.

**Frame sample.** On the first frame, `i_start_offset = -DTS`; `i_first_cts` is 0 in compressed mode or `start_offset * time_inc` otherwise. In fragmented mode an explicit timeline map with unknown 32-bit duration and `i_first_cts` is created immediately. Each frame creates an L-SMASH sample of size `i_size + sei_size`; if the SEI buffer exists it is copied in and freed. DTS/CTS computation matches `mp4.c` with `start_offset` in place of `delay_time`: compressed mode synthesizes early DTS and uses raw `PTS*time_inc` for CTS; normal mode adds the offset to both. The sample's random-access flag is set for keyframes. In fragmented mode, a keyframe (after the first) triggers flushing pooled samples with the DTS gap and creating a movie fragment. The sample is appended; `i_prev_dts` and frame count updated.

**Close.** Any remaining pooled samples are flushed with `(largest_pts - second_largest_pts) * time_inc` (or 1 tick when zero). Actual duration is `(largest_pts + last_delta) * time_inc / video_timescale * movie_timescale`. In non-fragmented mode an explicit timeline map is created with that duration and `i_first_cts`; in fragmented stdout mode an existing map is modified instead. `lsmash_finish_movie` finalises boxes, then root/summary/file/SEI buffer are destroyed. Errors during close are logged but do not change the success return.

## 6. Dependencies

- L-SMASH headers (`lsmash.h`), `output/output.h`, `x264cli.h` (`FAIL_IF_ERR`, `x264_is_regular_file_path`, logging macros tagged `mp4`).
- No other project modules.

## 7. Constants and tables

- `H264_NALU_LENGTH_SIZE = 4`.
- Brand list: `ISOM_BRAND_TYPE_MP42`, `MP41`, `ISOM`; optional `AVC1` and `ISO6` when recovery is enabled (disabled here).
- Track handler: video; sample type `ISOM_CODEC_TYPE_AVC1_VIDEO`.
- Edit mode: `ISOM_EDIT_MODE_NORMAL`; fragmented duration sentinel `ISOM_EDIT_DURATION_UNKNOWN32`.
- Random-access flags: `ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC` / `NONE`.
- Error/log tag: `"mp4"`.

## 8. Performance notes

- Per frame: one L-SMASH sample allocation and append; fragments add a flush/fragment creation only on keyframes.
- **Reservation sizing:** L-SMASH pooling batches samples, reducing box writes; keep it for streaming.
- **I/O buffering:** fragmented mode writes `moof`/`mdat` chunks incrementally; non-fragmented mode buffers the whole movie.
- **Zero-copy:** not possible; SEI and access-unit bytes are copied into samples.
- **Branch behaviour:** fragmentation branch is fixed per file; keyframe branch is cold relative to P/B frames.
- Reference: ISO/IEC 14496-12/15 and fragmented MP4 conventions (`moof`/`traf`/`tfhd`/`trun`).

## 9. References

- ISO/IEC 14496-12 (ISO BMFF), including fragmented files; ISO/IEC 14496-15 for `avcC`.
- L-SMASH project documentation: <https://github.com/l-smash/l-smash>.
- `output/mp4.c`, `output/output.h` (companion specifications).

## 10. Verification checklist

1. Confirm a regular output path produces a non-fragmented MP4 (no `moof` boxes) and stdout produces fragmented output with `moof`/`mdat` pairs.
2. Verify brand list contains MP42/MP41/ISOM.
3. Verify colour metadata (primaries/transfer/matrix/full-range) matches encoder parameters.
4. Verify SEI appears exactly once, prepended to the first sample.
5. Verify DTS are monotonic with positive CTS offsets; compare with the GPAC backend for the same input.
6. Verify a keyframe in fragmented mode closes the previous fragment.
7. Verify final Duration/timeline map is created or updated appropriately.
8. Confirm stdout output is not rejected (unlike `mp4.c`).

## 11. Open questions

- UNKNOWN: whether `b_use_recovery` can ever be enabled by a frontend option in this build — the source hardcodes it to 0, so the brand/recovery branches are effectively dead.
- UNKNOWN: exact L-SMASH semantics of `lsmash_modify_explicit_timeline_map` for stdout fragments; behavior may depend on library version.
