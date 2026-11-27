# output/output.h — CLI muxer (output module) plugin contract

*Dirty-side behavioural specification. Reference source: `output/output.h`. Behavioural descriptions only; no source or near-verbatim pseudocode is reproduced.*

## 1. Header

- **Source path:** `output/output.h`
- **Role:** Defines the interface between the x264 CLI and its container writers (muxers), plus the small options structure they share.
- **Marker:** dirty-side behavioural specification.

## 2. Purpose

The encoder produces H.264 NAL units and picture metadata; it does not know how to write MP4/MKV/FLV files. This header defines a five-callback vtable that every muxer implements, allowing the frontend to open a destination, push encoder parameters, write the three header NALs (SPS/PPS/SEI), write each coded frame, and finalise the file. It is the contract that makes the raw Annex-B writer (essential for a clean-room encoder) interchangeable with the container writers.

## 3. Public interface

### `cli_output_opt_t`
A single flag `use_dts_compress`. When set, muxers that care (FLV, MP4) apply a DTS-compression trick to avoid negative/offset timestamps caused by B-frame reordering delay; see the individual muxer specs.

### `cli_output_t`
Five function pointers:

- `open_file(filename, *handle, *opt)` — create/prepare the destination and return an opaque handle. `"-"` conventionally means stdout, though not every muxer supports it.
- `set_param(handle, *x264_param_t)` — receive encoder parameters before headers are written; muxers use this to configure timescales, track metadata, and delay compensation.
- `write_headers(handle, x264_nal_t *p_nal)` — write the SPS, PPS, and SEI NALs (indices 0, 1, 2). Returns the number of payload bytes written, or negative on error.
- `write_frame(handle, uint8_t *p_nal, int size, x264_picture_t *picture)` — write one coded access unit; `picture` supplies DTS, PTS, keyframe flag, and picture type. Returns bytes written or negative.
- `close_file(handle, largest_pts, second_largest_pts)` — finalise the file; the two largest PTS values let muxers compute the final frame duration/duration fields. Return convention: 0 on success.

Global exported instances: `raw_output`, `mkv_output`, `mp4_output`, `flv_output`.

## 4. State and data structures

The header is stateless. Each muxer defines its own handle type behind `hnd_t` (a `void*`-like abstraction). Lifetime: `open_file` → `set_param` → `write_headers` → zero or more `write_frame` → `close_file`. The frontend must call them in that order; muxers assume `set_param` has been called before `write_headers` (they read encoder parameters there).

## 5. Algorithm / behaviour

The contract itself imposes no algorithm; the semantics of each callback are as described in section 3. Notable conventions derived from the implementations:

- Header NAL payloads (except SEI) arrive with a 4-byte start-code prefix; container muxers skip it when building length-prefixed `avcC` records, whereas the raw muxer writes it verbatim.
- `write_frame` receives an already length-prefixed bitstream for container muxers (each NAL preceded by its 4-byte length), i.e. AVCC format, not Annex-B start codes.
- `close_file`'s PTS arguments are used to derive the last sample duration (`largest_pts - second_largest_pts`) in container boxes/metadata.
- DTS compression is opt-in via `cli_output_opt_t.use_dts_compress` and only affects FLV/MP4 timing math.

## 6. Dependencies

- Included by every file under `output/`.
- Depends on `x264cli.h` for `hnd_t`, `x264_param_t`, `x264_nal_t`, `x264_picture_t`, file helpers.
- Selected by the CLI frontend based on the output filename/format option.

## 7. Constants and tables

- No numeric constants defined in this header.
- NAL index convention: `p_nal[0]` = SPS, `p_nal[1]` = PPS, `p_nal[2]` = SEI.
- Output format names (frontend-level, not in the header): `raw`/`.264`, `.mkv`, `.mp4`, `.flv`.

## 8. Performance notes

- The vtable calls are once per frame, negligible overhead.
- **I/O buffering:** muxers should buffer writes and flush at tag/cluster boundaries; the raw muxer deliberately writes directly (one `fwrite` per NAL group).
- **Reservation sizing:** container writers preallocate sample buffers based on frame size estimates to avoid `realloc` per frame (see mp4.c/matroska specs).
- **Zero-copy:** raw output writes caller buffers directly; container muxers must copy into sample records because they rewrite headers later.
- Reference: ISO BMFF/FLV/Matroska container specifications for the fields populated through these callbacks.

## 9. References

- `output/raw.c`, `output/flv.c`, `output/matroska.c`, `output/mp4.c` (companion specifications).
- ISO/IEC 14496-12 (ISO BMFF), ISO/IEC 14496-15 (AVC file format), Matroska specification, Adobe FLV specification.

## 10. Verification checklist

1. Confirm each exported muxer instance supplies all five callbacks (the raw muxer supplies a no-op `set_param`).
2. Confirm the frontend can select any muxer through the same call sequence without format-specific branching.
3. Confirm `write_headers`/`write_frame` return values are treated as byte counts and negative values as errors.
4. Confirm `close_file` receives the two largest PTS and produces a sane final duration in each container.
5. Confirm `"-"` selects stdout where supported and is rejected where not (MP4 requires a seekable regular file).

## 11. Open questions

- UNKNOWN: exact return-value convention of `close_file` (raw/mp4 return 0; the field is described as void-like in some frontends) — treat nonzero as error conservatively.
- UNKNOWN: whether `set_param` may be called more than once; no muxer in this set is documented to support that.
