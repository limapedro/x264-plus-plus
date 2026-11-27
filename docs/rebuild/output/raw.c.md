# output/raw.c — raw Annex-B H.264 muxer

*Dirty-side behavioural specification. Reference source: `output/raw.c`. Behavioural descriptions only; no source or near-verbatim pseudocode is reproduced.*

## 1. Header

- **Source path:** `output/raw.c`
- **Role:** Implements `raw_output`, the simplest muxer: it writes the encoder's H.264 bytes directly to a file or stdout with no container framing.
- **Marker:** dirty-side behavioural specification.

## 2. Purpose

This is the output path every clean-room encoder needs first. It produces an Annex-B H.264 elementary stream: SPS, PPS, and SEI are written as-is from the encoder, and every coded access unit is appended verbatim. Because there is no index, duration, or timestamp metadata, `set_param` is a no-op and `close_file` only needs to close the descriptor. Its transparency makes it ideal for conformance testing with bitstream analyzers.

## 3. Public interface

Exports `const cli_output_t raw_output = { open_file, set_param, write_headers, write_frame, close_file }`.

- `open_file`: `"-"` selects stdout; otherwise a file is opened for read/write-binary creation (`"w+b"`). Failure returns -1.
- `set_param`: always returns 0 (does nothing).
- `write_headers`: sums the payload lengths of the first three NALs (SPS, PPS, SEI) and writes them as one `fwrite` from the first NAL's buffer. Returns the total size on success, -1 on failure. This relies on the encoder placing those three NALs contiguously in one payload buffer.
- `write_frame`: writes `i_size` bytes from `p_nalu` in one `fwrite`; returns the size or -1.
- `close_file`: returns 0 for null/stdout handles; otherwise closes the file and returns `fclose`'s result. The PTS arguments are ignored.

## 4. State and data structures

There is no custom handle type: `hnd_t` is used directly as a `FILE*`. Stdout is represented by the standard `stdout` pointer and is recognised by pointer comparison in `close_file`. No buffering state, no sample tables, no timestamps — the module is entirely stateless apart from the stream position.

## 5. Algorithm / behaviour

**Open.** If the filename is exactly `"-"`, the handle is set to `stdout` without opening anything. Otherwise `x264_fopen(filename, "w+b")` creates/truncates a file; a null result fails. `"w+b"` (rather than `"wb"`) is used so a consumer could in principle read back, though this module never does.

**Header writing.** The three header NAL descriptors are expected at indices 0/1/2. Their `i_payload` fields are summed into `size`. A single `fwrite` is issued with `p_nal[0].p_payload` as the source, `size` as the byte count, and count 1. Because libx264 places SPS/PPS/SEI back-to-back in the first NAL's payload buffer, this writes all three start-code-prefixed NALs in one syscall. A short write is reported as failure.

**Frame writing.** Each call writes the supplied buffer verbatim in one `fwrite`, returning the byte count. No start-code conversion occurs because the encoder already emits Annex-B byte streams for this path (the caller arranges the appropriate output format in `x264_param_t`).

**Close.** Null and stdout handles are ignored (stdout must not be closed). Regular files are closed. The ignored PTS arguments are a deliberate contrast with container muxers, which need them to compute final durations.

## 6. Dependencies

- `output/output.h` (vtable and options), `x264cli.h` for `x264_fopen`, `x264_nal_t`, `x264_picture_t`.
- No external libraries.

## 7. Constants and tables

- Standard stream sentinel: filename `"-"` → `stdout`.
- Open mode: `"w+b"`.
- Header NAL indices fixed at 0=SPS, 1=PPS, 2=SEI (a convention of the encoder frontend).
- Error/log tag: none specific (the module uses no logging macros in this path).

## 8. Performance notes

- This is the fastest muxer: one `fwrite` per header group and one per frame, no copying, no rewriting.
- **I/O buffering:** a single `fwrite` per frame lets stdio buffer efficiently. Do not split into per-NAL writes.
- **Zero-copy:** caller buffers are written directly; no allocation occurs ever.
- **Reservation sizing:** not applicable — no internal buffers.
- **Seekability:** not required; works with pipes/stdout for streaming.
- Reference: ITU-T H.264 Annex B (start-code-prefixed NAL unit stream format).

## 9. References

- ITU-T H.264 / ISO IEC 14496-10, Annex B — NAL unit stream format.
- `output/output.h`, `input/raw.c` (companion specifications).

## 10. Verification checklist

1. Encode a short clip to raw output and confirm the byte stream starts with `00 00 00 01 67` (SPS start code + NAL type 7).
2. Confirm SPS, PPS, and SEI appear consecutively and no container box/magic surrounds them.
3. Confirm each frame's bytes equal the encoder's access unit exactly (no length prefixes added).
4. Confirm writing to `"-"` produces the identical stream on stdout and that stdout remains usable afterward.
5. Confirm a short/failed write returns -1.
6. Confirm `set_param` returns 0 and `close_file` ignores the PTS arguments.

## 11. Open questions

- UNKNOWN: whether the frontend sets `b_annexb`/equivalent parameter before calling this muxer; the muxer itself assumes the NAL buffers are already Annex-B formatted.
