# output/flv.c — FLV muxer

*Dirty-side behavioural specification. Reference source: `output/flv.c`. Behavioural descriptions only; no source or near-verbatim pseudocode is reproduced.*

## 1. Header

- **Source path:** `output/flv.c`
- **Role:** Implements `flv_output`, which wraps H.264 access units in FLV video tags and writes an `onMetaData` AMF script tag with back-patched duration/size/bitrate fields.
- **Marker:** dirty-side behavioural specification.

## 2. Purpose

FLV is a streaming-friendly container used by Flash-era players. This muxer writes the FLV file header, an `onMetaData` tag with placeholder values, an H.264 sequence-header tag containing an `avcC` record, and one video tag per frame with composition-time offsets. Because metadata values are unknown until encoding ends, the muxer records their absolute file positions and patches them at close. It also implements optional DTS compression to avoid negative initial timestamps caused by B-frame delay.

## 3. Public interface

Exports `const cli_output_t flv_output` with the standard five callbacks.

- `open_file`: allocates handle and buffered writer, writes the FLV file header, stores the DTS-compress option, returns handle; failures clean up and return -1.
- `set_param`: writes the `onMetaData` script tag (with placeholders for framerate/duration/filesize/bitrate when VFR or unknown), records patch positions, and caches fps, timebase, VFR flag, and B-frame delay count.
- `write_headers`: saves the SEI bytes for later, writes the AVC sequence-header tag (`avcC` from SPS/PPS), flushes, and returns the summed header payload sizes.
- `write_frame`: computes DTS/CTS in milliseconds, writes a video tag with keyframe/inter frame type and composition-time offset, prepends the deferred SEI to the first frame, flushes, increments frame count.
- `close_file`: flushes, computes total duration from the two largest PTS, and (for regular seekable files) patches metadata fields; always closes and frees everything.

## 4. State and data structures

`flv_hnd_t` fields: `c` (the `flv_buffer`), `sei`/`sei_len` (deferred SEI bytes), `i_fps_num`/`i_fps_den`/`i_framenum`, `i_framerate_pos`/`i_duration_pos`/`i_filesize_pos`/`i_bitrate_pos` (absolute file offsets of placeholder AMF doubles, computed as `c->d_cur + c->d_total + 1` — the `+1` skips the AMF type byte), `b_write_length`, `i_prev_dts`/`i_prev_cts` (duplicate-timestamp detection), `i_delay_time`, `i_init_delta`, `i_delay_frames`, `d_timebase` (seconds per tick as double), `b_vfr_input`, `b_dts_compress`, and `start` (offset used for back-patching tag lengths). Invariants: the four metadata positions are valid only for regular files; SEI exists until the first frame consumes it; frame count is exact.

## 5. Algorithm / behaviour

**File header.** Written immediately on open: ASCII `FLV`, version byte 1, flags byte 1 (video only, no audio), a 32-bit big-endian data offset of 9, and a 32-bit previous-tag-size0 of 0. The buffer is flushed so the header is on disk first.

**Metadata tag.** `set_param` emits a script-data tag (type 0x12) with a zero length/timestamp placeholder, then AMF data: a string key `"onMetaData"`, a mixed-array marker with 7 entries, and key/value pairs for width, height, framerate, videocodecid (=7), duration, filesize, and videodatarate, terminated by an empty-string key plus object-end marker `0x09`. For CFR sources the framerate is computed directly; for VFR a zero placeholder is written and its position saved. Duration/filesize/bitrate are always placeholders with saved positions. The tag's 24-bit length is back-patched to `length - 10` and a 32-bit previous-tag-size is appended; note the tag length is not flushed here (it is written to the buffer and flushed with the next header tag). The handler caches `d_timebase = timebase_num/timebase_den`, `b_vfr_input`, and `i_delay_frames` (0 without B-frames, 1 with B-frames, 2 with pyramid B-frames).

**AVC sequence header.** `write_headers` skips the 4-byte prefix of SPS/PPS, stores the SEI verbatim for later, and emits a video tag with frame type key + codec H.264 (`0x17`) and AVC-sequence-header payload type 0. The `avcC` record contains: configuration version 1, the SPS's profile/compatibility/level bytes (indices 1..3 after the start code), `0xff` (6 reserved bits + 2-bit NAL-length-minus-one = 3, meaning 4-byte lengths), `0xe1` (3 reserved bits + one SPS), the SPS length/sample, then one PPS count/length/sample. The tag length is back-patched and a previous-tag-size field appended; the buffer is flushed.

**Frame timing.** On the first frame, `i_delay_time` is set to minus the picture's DTS; unless DTS compression is enabled, an informational message reports the initial delay in milliseconds. Conversion to milliseconds multiplies the timestamp by `d_timebase * 1000` and rounds with `+0.5`.

- *Normal mode:* DTS and CTS both have the delay added.
- *DTS-compressed mode:* on frame 1 the initial delta is captured; the first `i_delay_frames+1` frames get a synthetic DTS of `framenum * init_delta / (delay_frames+1)`, while later frames use the real DTS; CTS uses the real PTS with no delay.

The composition offset is `cts - dts`. Consecutive equal DTS/CTS values (caused by millisecond rounding at rates above 1000 fps) produce warnings. The frame tag header contains: video tag type, back-patched length, 24-bit DTS, extended timestamp byte (DTS >> 24), stream id 0, then the frame byte (key/inter | H.264), AVC-NALU payload type 1, and a 24-bit composition offset. The deferred SEI (if present) is appended first, then the access-unit bytes. Tag length and previous-tag-size are back-patched, buffer flushed, frame count incremented.

**Close.** After a final flush, total duration is computed as `fps_den/fps_num` for a single-frame stream, otherwise `(2*largest_pts - second_largest_pts) * d_timebase`. If the destination is a regular file and duration > 0, the module seeks to each saved metadata position and overwrites an 8-byte big-endian double (using the same bit-punning as `flv_dbl2int`): framerate = `framenum/duration` (VFR only), duration, filesize from `ftell`, and bitrate = `filesize*8/(duration*1000)`. Errors are collected but cleanup always happens.

## 6. Dependencies

- `output/output.h`, `output/flv_bytestream.h/.c`, `x264cli.h` (`x264_is_regular_file`, `ftell`, `fseek`, `endian_fix64`, logging macros tagged `flv`).
- No external container library.

## 7. Constants and tables

- FLV tags: video `0x09`, script data `0x12`.
- Video tag bytes: key H.264 = `0x17`, inter H.264 = `0x27`, sequence header payload type = 0, NALU payload type = 1.
- `avcC`: configuration version 1, NAL-length-minus-one = 3, one SPS (`0xe1`), one PPS (1).
- `onMetaData` has 7 fields; AMF mixed-array `0x08`, object end `0x09`, number marker `0x00`.
- Initial buffer/tag sizes are in `flv_bytestream.h`.
- Error/log tag: `"flv"`.

## 8. Performance notes

- Per frame: one tag assembled in the reserved buffer, one `fwrite` flush, and a few in-buffer patches — efficient.
- **I/O buffering:** the `flv_buffer` keeps tag assembly in memory; flushing once per tag minimises syscalls while preserving patchability of previous-tag-size fields.
- **Reservation sizing:** buffer doubles as needed; a clean-room writer can pre-size to the largest expected access unit.
- **Zero-copy:** SPS/PPS/SEI/frame bytes are copied once into the buffer (unavoidable because lengths are back-patched); the frame payload copy is the only per-frame copy.
- **Branch behaviour:** the metadata-patching block runs once at close; the per-frame duplicate-timestamp checks are cold.
- Reference: Adobe FLV specification; x264 FLV timing notes on B-frame delay and composition offsets.

## 9. References

- Adobe Flash Video (FLV) File Format Specification v1.0 — tag structure, timestamps, previous-tag-size.
- AMF0 specification — `onMetaData` encoding.
- ISO/IEC 14496-15 — `avcC` record layout.
- `output/flv_bytestream.c`, `output/output.h` (companion specifications).

## 10. Verification checklist

1. Verify the first 13 bytes are `FLV 01 01 00 00 00 09 00 00 00 00`.
2. Verify the `onMetaData` tag lists all seven keys and that placeholders are patched to nonzero values after encoding.
3. Verify the AVC sequence-header tag contains profile/level from the SPS and one SPS + one PPS with 4-byte length fields.
4. Verify keyframe tags use byte `0x17` and non-keyframes `0x27`.
5. Verify CTS offsets are non-negative and equal PTS-DTS in milliseconds.
6. With DTS compression enabled, verify the first frames get synthetic monotonically increasing DTS.
7. Confirm duplicate-timestamp warnings appear only above 1000 fps.
8. Confirm single-frame duration equals `1/fps`.
9. Verify stdout (`"-"`) output is identical except metadata patching is skipped (non-regular file).

## 11. Open questions

- UNKNOWN: the purpose of `b_write_length` (declared but not obviously used in the shown paths) — possibly a legacy flag; treat as inert unless proven otherwise.
- UNKNOWN: exact handling of a CTS offset that exceeds 24 bits (encoding beyond ~4.6 hours at 1 ms resolution); no guard is documented.
