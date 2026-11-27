# output/mp4.c — MP4 muxer (GPAC backend)

*Dirty-side behavioural specification. Reference source: `output/mp4.c`. Behavioural descriptions only; no source or near-verbatim pseudocode is reproduced.*

## 1. Header

- **Source path:** `output/mp4.c`
- **Role:** Implements `mp4_output` using the GPAC library: creates an ISO BMFF visual track, writes samples with DTS/CTS offsets, builds an edit list, and recomputes bitrate metadata at close.
- **Marker:** dirty-side behavioural specification.

## 2. Purpose

MP4 (ISO BMFF) is the dominant distribution container. This muxer configures a GPAC file with an AVC track, translates x264 timing into media timescale ticks (optionally compressing B-frame delay), and finalises duration/bitrate boxes. It is one of two alternative MP4 backends in the tree; this one requires a seekable regular file. A clean-room encoder can skip it but should understand its timing conventions because they are shared with `mp4_lsmash.c`.

## 3. Public interface

Exports `const cli_output_t mp4_output` with the standard five callbacks.

- `open_file`: rejects non-regular files (including `"-"`), opens GPAC for writing, creates a sample object, sets brand `AVC1`, and stores the DTS-compress option.
- `set_param`: computes delay frames, timescale multiplier, media timescale, creates the visual track and AVC config, enables it, sets visual info/SAR, and allocates the sample data buffer (size `width*height*3/2`).
- `write_headers`: configures the AVC config from SPS/PPS and appends SEI bytes to the first sample's buffer.
- `write_frame`: appends the access unit to the sample buffer, computes DTS/CTS with optional delay compression, sets RAP flag, adds the sample via GPAC, then resets its data length.
- `close_file`: adjusts last sample duration, writes an edit box for positive CTS offset, recomputes bitrate, sets profile/storage mode, closes the file and frees state.

## 4. State and data structures

`mp4_hnd_t` fields: `p_file` (GPAC ISO file), `p_config` (AVC config), `p_sample` (reusable sample), `i_track`, `i_descidx`, `i_time_res` (media timescale), `i_time_inc` (ticks per source timebase unit, after DTS multiplier), `i_delay_time`, `i_init_delta`, `i_numframe`, `i_delay_frames`, `b_dts_compress`, `i_dts_compress_multiplier`, `i_data_size` (sample buffer capacity). Invariants: media timescale ≤ UINT32_MAX; `p_sample->dataLength` is reset to zero after each `gf_isom_add_sample`; the sample buffer is grown on demand by `check_buffer`.

## 5. Algorithm / behaviour

**Open.** The filename is opened as a regular file; anything else (pipes, `"-"`) is rejected because GPAC needs seeking. The handle is allocated, GPAC opened for writing, a sample object created, and brand info set to AVC1 (version 0).

**Parameter configuration.** Delay frames are 0 without B-frames, 1 with B-frames, 2 with pyramid B-frames. The DTS-compression multiplier is `b_dts_compress * delay_frames + 1`. Media timescale = `timebase_den * multiplier`, and ticks-per-unit = `timebase_num * multiplier`; the timescale is rejected if it exceeds UINT32_MAX. A visual track is created at that timescale, an AVC config object created and attached, the track enabled, and visual dimensions set. A non-square SAR sets the pixel aspect ratio and track layout (display width/height scaled by 16.16 fixed point). The sample data buffer is allocated to `width*height*3/2` bytes; failures set capacity to zero.

**Headers.** SPS/PPS payloads are stripped of their 4-byte start codes; SEI is kept whole. The AVC config fields are filled: version 1, profile/compatibility/level from SPS bytes 1..3, and parameter-set slots for SPS and PPS copied into GPAC config lists; the config is committed with `gf_isom_avc_config_update`. SEI bytes are appended into the first sample's data buffer via `check_buffer` and `memcpy`. Return value is the summed header sizes.

**Frame sample.** The access unit is appended to the sample buffer. On the first frame, `i_delay_time = -DTS`. With DTS compression: on frame 1 the initial delta is captured as `(DTS + delay_time) * time_inc`; the first `delay_frames+1` frames get synthetic DTS `framenum * init_delta / multiplier`, later frames use `DTS * time_inc`; CTS uses `PTS * time_inc`. Without compression, both DTS and CTS have `delay_time` added before scaling. The sample's RAP flag is the keyframe flag, DTS is set, `CTS_Offset = cts - dts`, and the sample is added to the track. Data length is reset and frame count incremented.

**Close.** If the computed mdhd duration (`(2*largest_pts - second_largest_pts) * time_inc`) differs from GPAC's media duration, the last sample duration is patched: either the difference from the final DTS, or `(largest_pts - second_largest_pts) * time_inc` as a fallback. If the first sample's CTS offset is positive, an edit box is appended mapping presentation time to media time (tkhd duration derived from mdhd duration and the movie timescale). Bitrate metadata is recomputed by reading all samples back: maximum sample size becomes `bufferSizeDB`, average bitrate is total bytes divided by media duration in seconds times 8, and maximum bitrate is the largest per-window byte sum times 8. Finally profile indication `GF_ISOM_PL_VISUAL 0x15`, flat storage mode, and `gf_isom_close`.

## 6. Dependencies

- GPAC headers (`gpac/isomedia.h`), `output/output.h`, `x264cli.h` (`FAIL_IF_ERR`, `x264_fopen`, `x264_is_regular_file`, logging tagged `mp4`).
- No other project modules.

## 7. Constants and tables

- Brand: `GF_ISOM_BRAND_AVC1`, minor version 0.
- Media handler type: visual.
- Sample buffer initial size: `width * height * 3/2` bytes.
- Profile indication set at close: `GF_ISOM_PL_VISUAL = 0x15`.
- Storage mode: flat (`GF_ISOM_STORE_FLAT`).
- Error/log tag: `"mp4"`.

## 8. Performance notes

- Per frame: one append into a reserved sample buffer and one GPAC add-sample call.
- **Reservation sizing:** `check_buffer` grows geometrically on demand; pre-sized to `1.5 * width * height`, which covers typical I-frames — a clean-room writer can size to a measured worst case to avoid reallocs.
- **I/O buffering:** GPAC handles box writes; flat storage mode avoids interleaved chunks.
- **Zero-copy:** not possible — MP4 samples are stored in box payloads, so bytes are copied once.
- **Branch behaviour:** bitrate recomputation reads all samples at close (O(frame count) but one-off).
- Reference: ISO/IEC 14496-12/-15 for mdhd/tkhd/elst/stsd boxes and edit lists.

## 9. References

- ISO/IEC 14496-12 (ISO BMFF), ISO/IEC 14496-15 (AVC file format) — box structure, edit lists, `avcC`.
- GPAC documentation: <https://gpac.wp.imt.fr/>.
- `output/mp4_lsmash.c`, `output/output.h` (companion specifications).

## 10. Verification checklist

1. Confirm non-regular output (pipe/stdout) is rejected.
2. Verify `ftyp` brand is AVC1 and a visual track with `avc1` sample entry exists.
3. Verify `avcC` contains the encoder's SPS/PPS with 4-byte NAL-length field.
4. Verify sample DTS values are monotonic and CTS offsets are non-negative.
5. With B-frames, verify the initial negative delay is handled (delay compensation or DTS compression per option).
6. Verify an `elst` edit box appears when the first CTS offset is positive.
7. Verify Duration and bitrate fields at close are finite and plausible.
8. Verify the file opens and decodes in an independent MP4 demuxer.

## 11. Open questions

- UNKNOWN: exact GPAC version-dependent branch (`GPAC_VERSION_MAJOR > 8`) chooses between `gf_isom_append_edit` and `gf_isom_append_edit_segment`; behavior is equivalent for normal inputs.
- UNKNOWN: whether `gf_isom_set_visual_info` is called with a valid descriptor index when SAR is absent — the code passes `i_descidx` from AVC config creation regardless.
