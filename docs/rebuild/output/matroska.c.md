# output/matroska.c — Matroska muxer (parameter layer)

*Dirty-side behavioural specification. Reference source: `output/matroska.c`. Behavioural descriptions only; no source or near-verbatim pseudocode is reproduced.*

## 1. Header

- **Source path:** `output/matroska.c`
- **Role:** Implements `mkv_output`, translating x264 encoder parameters and NAL units into the calls of the EBML writer in `matroska_ebml.c`.
- **Marker:** dirty-side behavioural specification.

## 2. Purpose

This module sits between the generic muxer vtable and the EBML layer. It computes display dimensions (including SAR correction and stereo packing), builds an `avcC` codec-private record from SPS/PPS, sets track metadata (codec ID, default duration), and converts picture PTS into Matroska timestamps. The EBML layer handles all byte-level encoding.

## 3. Public interface

Exports `const cli_output_t mkv_output` with the standard five callbacks.

- `open_file`: allocates handle, creates EBML writer; fails cleanly.
- `set_param`: computes frame duration (nanoseconds) for CFR, display dimensions from SAR and frame-packing mode, stores timebase, and sets stereo mode.
- `write_headers`: builds `avcC` from SPS/PPS, calls `mk_write_header` with codec id `V_MPEG4/ISO/AVC`, then starts a frame and adds the SEI bytes.
- `write_frame`: continues the current frame, adds the access unit, converts PTS to the hardcoded 50 kHz track timescale, sets keyframe/skippable flags, and returns.
- `close_file`: computes the last frame delta from the two largest PTS and calls `mk_close`.

## 4. State and data structures

`mkv_hnd_t` fields: `w` (EBML writer), `width`/`height` (coded dimensions), `d_width`/`d_height` (display dimensions), `display_size_units` (always `DS_PIXELS`), `stereo_mode`, `frame_duration` (default duration in nanoseconds, 0 for VFR/unknown), `b_writing_frame`, `i_timebase_num`/`i_timebase_den`. Invariants: `b_writing_frame` is true between `mk_start_frame` and `mk_set_frame_flags`; dimensions must be nonzero before `write_headers`.

## 5. Algorithm / behaviour

**Open.** A zeroed handle is allocated and the EBML writer created; on failure the handle is freed and the output handle set to NULL.

**Parameter translation.** If fps is positive and input is CFR, default frame duration is computed as `fps_den * 1e9 / fps_num` nanoseconds (integer division); otherwise it is zero. Coded width/height come from parameters, and display dimensions start equal to them with `DS_PIXELS`. A valid frame-packing index in `[0, 7)` selects a Matroska stereo mode from a 7-entry table and divides display width/height by the corresponding divisor (some modes halve width, others height). A non-square SAR then stretches whichever display dimension corresponds to the wider pixel aspect: width is multiplied by `sar_w/sar_h` if SAR > 1, otherwise height by `sar_h/sar_w`. Timebase numerator/denominator are stored for PTS conversion.

**Stereo tables (facts):** mode list `{5,9,7,1,3,13,0}` indexed by frame-packing value; width divisor `{1,2,1,2,1,1,1}`; height divisor `{1,1,2,1,2,1,1}`.

**Header/avcC.** SPS/PPS payload sizes are taken after their 4-byte prefixes; SEI size is the full payload. A buffer of length `5 + 1 + 2 + sps_size + 1 + 2 + pps_size` is built: configuration version 1, SPS bytes 1..3 (profile/compatibility/level), `0xff` (4-byte NAL lengths), `0xe1` (one SPS), 16-bit SPS length and data, then count 1, 16-bit PPS length and data. The EBML header is written with writing-app string `"x264" + version`, codec id `V_MPEG4/ISO/AVC`, the `avcC` as private data, default duration, track timescale **50000** (i.e. 50 kHz), coded/display dimensions, display units, and stereo mode. The temporary `avcC` buffer is freed.

SEI is then attached to a Matroska frame: if no frame is open, one is started; the SEI bytes are added. The return value is the sum of SEI/SPS/PPS sizes.

**Frame writing.** If no frame is open (e.g. header phase did not start one), one is started; the access-unit bytes are appended to it. The frame's timestamp is computed as `round(pts * 1e9 * timebase_num / timebase_den)` — converting encoder ticks to nanoseconds — then passed to `mk_set_frame_flags` along with the keyframe flag and a "skippable" flag set for B-pictures (`p_picture->i_type == X264_TYPE_B`). Note the EBML layer later divides this timestamp by the 50 kHz timescale for the cluster-relative field.

**Close.** The last frame delta is `round((largest_pts - second_largest_pts) * timebase_num / timebase_den)` nanoseconds, or zero if the denominator is zero; this is passed to `mk_close` for duration patching. The handle is freed and the EBML writer's return code propagated.

## 6. Dependencies

- `output/output.h`, `output/matroska_ebml.h/.c`, `x264cli.h` (`X264_VERSION`, `X264_TYPE_B`, parameters).
- No external libraries.

## 7. Constants and tables

- Codec ID: `"V_MPEG4/ISO/AVC"`.
- Track timescale: 50000 (50 kHz), so one tick = 20 µs.
- Writing app: `"x264" X264_VERSION`.
- Stereo mode/divisor tables listed in section 5.
- Display units: `DS_PIXELS`.
- Error tag: `"matroska"` (via `mk` layer conventions) / no dedicated tags in this file.

## 8. Performance notes

- Per frame: one `mk_add_frame_data` call, one timestamp conversion, one `mk_set_frame_flags` — negligible.
- **I/O buffering:** handled by the EBML layer's cluster buffering.
- **Reservation sizing:** the `avcC` blob is sized analytically once at header time; the EBML layer reuses frame buffers.
- **Zero-copy:** frame bytes are copied into the EBML frame context (unavoidable for block size back-patching).
- **Branch behaviour:** frame-open check runs per frame; stereo/SAR paths are cold (config time).
- Reference: Matroska specification for CodecID/DefaultDuration/StereoMode semantics.

## 9. References

- Matroska specification: <https://www.matroska.org/technical/specs/index.html>.
- ISO/IEC 14496-15 — `avcC` structure.
- `output/matroska_ebml.c`, `output/output.h` (companion specifications).

## 10. Verification checklist

1. Verify CodecID, WritingApp (contains version), and TimecodeScale 1000000 ns in SegmentInfo.
2. Verify TrackEntry DefaultDuration equals `1e9/fps` for CFR and is absent for VFR.
3. Verify display dimensions reflect SAR and frame-packing divisors.
4. Verify CodecPrivate is a valid `avcC` with the SPS/PPS from the encoder.
5. Verify frame timestamps correspond to PTS converted to nanoseconds, and that B-frames are marked skippable.
6. Verify SEI is prepended to the first frame's block.
7. Verify final Duration is patched from the two largest PTS on seekable output.

## 11. Open questions

- UNKNOWN: why timestamps are computed in nanoseconds while the track timescale is 50 kHz — the EBML layer divides by its own `timescale` (which `matroska.c` passes as 50000), so the intended units may be inconsistent; verify against a reference file if exact PTS matter.
- UNKNOWN: behavior when `frame_packing` is out of the 0..6 range — stereo mode stays -1 and no stereo elements are written.
