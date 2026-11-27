# example.c — rebuild specification

## 1. Header

- Source path: `example.c` (root of the reference tree).
- Role: a minimal, self-contained demonstration program that encodes a raw I420 YUV file to an H.264 Annex-B bytestream using only the public libx264 API.
- Marker: **dirty-side behavioural specification** — behaviour and structure only, not source text.

## 2. Purpose

This file is both documentation and a smoke test for the public API. It shows the required call order for a simple encoder application: obtain parameters, override the few mandatory fields, apply a profile, allocate a picture, open the encoder, feed frames, drain delayed frames, and clean up. It deliberately avoids the CLI's demuxer/muxer/filter infrastructure, timecode handling and rate-control options, so it can be compiled and understood in isolation. It also exercises the two-phase lifecycle (encode plus flush) that a naive implementation often gets wrong.

## 3. Public interface

- `int main(int argc, char **argv)` — program entry point.
  - Invocation contract: exactly one argument is required, of the form `<width>x<height>`. Standard input supplies raw planar I420 frames; standard output receives the encoded Annex-B stream. A typical invocation encodes a `352x288` source.
  - Return value: `0` on success, `-1` on any failure (bad arguments, allocation failure, encoder-open failure, read/write error, encode error).
- Local failure macro (behavioural contract): on a true condition it writes a caller-supplied diagnostic to `stderr` and jumps to a shared cleanup label. The label is redefined twice with distinct names so that cleanup starts at the correct stage: picture-only cleanup, picture-plus-encoder cleanup, or plain return. This staged cleanup prevents closing an encoder handle that was never opened.
- No symbols are exported for other programs; this is an application, not a library.

## 4. State and data structures

- `x264_param_t param` — encoder configuration. Defaulted through the preset API, then overridden directly by field assignment.
- `x264_picture_t pic` — input picture, allocated by the library. Holds plane pointers/strides for the three I420 planes and the presentation timestamp.
- `x264_picture_t pic_out` — output picture filled by the encoder; the example does not read any of its fields, but it must be supplied.
- `x264_t *h` — opaque encoder handle; `NULL` until successfully opened.
- `x264_nal_t *nal`, `int i_nal` — output NAL array and count returned by each encode call.
- `int i_frame` — monotonically increasing frame counter; also used as the PTS.
- `int i_frame_size` — byte count returned by `x264_encoder_encode`; negative signals error, zero means "no output this call" (the normal delayed-frame case).
- `int luma_size`, `chroma_size` — byte counts for one Y plane and one U or V plane, derived as `width*height` and one quarter of that. These assume 8-bit I420 with no padding.

The picture's plane layout follows the library's I420 allocation: plane 0 is full-resolution luma, planes 1 and 2 are quarter-resolution chroma. Strides come from `x264_picture_alloc`, not from the caller.

## 5. Algorithm / behaviour

Step-by-step behaviour:

1. **Platform setup (Windows).** Before any other standard I/O use, stdin, stdout and stderr are switched to binary mode. Without this, CRLF translation on Windows would corrupt the elementary stream. On non-Windows platforms this step is absent because the streams are already binary.
2. **Argument validation.** If `argc <= 1`, print a usage line naming the expected resolution and input/output redirection, then fail. Otherwise parse `argv[1]` with a two-integer `<width>x<height>` pattern; if fewer than two values are converted, print "resolution not specified or incorrect" and fail. Extra trailing characters are not explicitly validated beyond the two conversions.
3. **Parameter setup.** Call the preset helper with preset `"medium"` and no tuning. This fills defaults and applies the medium preset (which is the baseline set of defaults). On failure, jump to the plain return path.
4. **Direct overrides.** Set bit depth `8`, colorspace `X264_CSP_I420`, `i_width`/`i_height` from the parsed arguments, disable VFR input (`b_vfr_input = 0`), and enable both `b_repeat_headers` and `b_annexb`. The last two make every keyframe preceded by in-band SPS/PPS and make each NAL carry a four-byte Annex-B start code, so the output is a standalone decodable bytestream.
5. **Profile restriction.** Apply the `"high"` profile. This only lowers settings that exceed High-profile limits; it does not raise a Baseline-compatible configuration to High. On failure, jump to cleanup.
6. **Picture allocation.** Allocate the input picture for the chosen colorspace and dimensions. Failure jumps to the picture-stage cleanup.
7. **Encoder open.** Open the encoder with the configured parameters. A null return is treated as failure and jumps to the encoder-stage cleanup, which closes the (null) handle harmlessly and then cleans the picture.
8. **Frame loop.** Repeat indefinitely:
   - Read exactly `luma_size` bytes into plane 0; if short, break out of the loop (end of input, not an error).
   - Read exactly `chroma_size` bytes into plane 1; short read breaks.
   - Read exactly `chroma_size` bytes into plane 2; short read breaks. A partially consumed final frame therefore produces no encoded output for that frame.
   - Assign `pic.i_pts = i_frame` so timestamps are a simple frame index.
   - Call `x264_encoder_encode` with the input picture. A negative return jumps to the encoder-stage failure path; a positive return writes `i_frame_size` bytes from `nal[0].p_payload` to stdout, treating a short `fwrite` as failure; zero means no output and is simply ignored.
   - Increment `i_frame` at each loop iteration via the loop counter expression.
9. **Flush.** While `x264_encoder_delayed_frames` reports buffered frames, call `x264_encoder_encode` with a null input picture, write any returned bytes exactly as in the main loop, and treat negative returns or short writes as failure. This is essential because B-frames and lookahead delay output relative to input.
10. **Cleanup and success.** Close the encoder, free the picture with `x264_picture_clean`, and return `0`.
11. **Failure cleanup.** The staged labels ensure that: encoder-open failure cleans only the picture; any later failure closes the encoder and then cleans the picture; argument/parameter/profile failures before allocation simply return `-1`.

Behavioural details worth noting:

- The example never calls `x264_param_default` directly; the preset helper performs the default initialization.
- It never calls `x264_encoder_headers`; header emission is delegated to the encoder because `b_repeat_headers` is set.
- It does not set frame-total, frame-rate, SAR, level, threading, rate-control method or any analysis option — those come from defaults (CRF 23 at 25 fps in the reference build).
- Only the first NAL's payload pointer is written; the API guarantees all returned NAL payloads are contiguous in memory, so the single `fwrite` of the total byte count emits all NALs.

## 6. Dependencies

- Headers: `x264.h` (public API) plus `<stdint.h>`/`<stdio.h>`, and on Windows `<io.h>`/`<fcntl.h>` for `_setmode` and `_O_BINARY`.
- Library: links against `libx264.a` or the shared library; the `Makefile` has an `example` target that links `example.o` with `$(LIBX264)` and `$(LDFLAGS)`. It is built only when `EXE` is non-empty.
- Called API functions: `x264_param_default_preset`, `x264_param_apply_profile`, `x264_picture_alloc`, `x264_encoder_open`, `x264_encoder_encode`, `x264_encoder_delayed_frames`, `x264_encoder_close`, `x264_picture_clean`.
- Installed headers required at compile time: `x264.h` and the generated `x264_config.h` that it includes.

## 7. Constants and tables

| Constant / value | Meaning |
| --- | --- |
| `"medium"` | Preset name passed to the preset helper; the baseline speed/quality trade-off |
| `"high"` | Profile whose restrictions are applied |
| `8` | Input/output bit depth |
| `X264_CSP_I420` | Planar 4:2:0 colorspace |
| `0` | Value assigned to `b_vfr_input` (CFR mode) |
| `1` | Values assigned to `b_repeat_headers` and `b_annexb` |
| `luma_size = width*height`, `chroma_size = luma_size/4` | Plane sizes for 8-bit I420 |
| `i_pts = i_frame` | Timestamp model |

There are no lookup tables; the string formats are `"%dx%d"` for the argument and a usage line naming `352x288`.

## 8. Performance notes

The program's own work is I/O-bound: three `fread` calls and one `fwrite` per frame. Encoding cost dwarfs everything else. Still, some choices are speed-relevant:

- Using the `"medium"` preset keeps default analysis work moderate; switching that literal to a faster preset (`"ultrafast"`, `"veryfast"`) is the single largest speed lever available without changing any other code.
- `b_vfr_input = 0` avoids VFR timestamp processing in the rate controller — a small but real saving.
- `b_repeat_headers = 1` adds SPS/PPS NALs at each keyframe, a negligible cost but relevant for stream size.
- The example uses no threading overrides, so it inherits the library's automatic thread count; a caller wanting deterministic timing can set `param.i_threads` explicitly.
- The single `fwrite` per encoded frame (relying on NAL contiguity) avoids per-NAL syscall overhead, unlike a naive loop over `i_nal`.
- Reading whole planes in single `fread` calls is cache-friendly and avoids per-row overhead.

Public references: the libx264 API documentation (`x264.h`) for the preset list and NAL contiguity guarantee, and the x264 preset guide on the project wiki for the relative cost of presets.

## 9. References

- Reference tree `x264.h` — definitions of every API type and function used.
- Reference tree `Makefile` — the `example` link target.
- x264 project documentation on presets and tuning (`doc/` in the reference tree, and the public x264 wiki).
- H.264 Annex B byte-stream format, for the start-code behaviour enabled by `b_annexb`.

## 10. Verification checklist

- Build the `example` target and run with a synthetically generated `352x288` I420 file; confirm the output is a valid Annex-B stream decodable by a reference decoder.
- Verify usage/error paths: no argument, malformed resolution, short input file — each returns `-1` and prints the expected diagnostic.
- Verify that the encoded stream contains SPS/PPS before the first IDR and again at later IDRs (evidence that `b_repeat_headers` took effect).
- Verify all NALs begin with a four-byte start code (`00 00 00 01`).
- Verify that the `High` profile is signalled in the SPS for a default build.
- Verify the flush loop by using a source long enough to produce B-frames: the number of frames decoded from the output must equal the number of complete input frames read.
- Verify behaviour on Windows that no CR/LF insertion occurs in the output (binary mode).
- Verify that a truncated final plane does not emit a partial frame.

## 11. Open questions

- The example relies on default frame rate (25 fps) and default CRF (23); these are library defaults, not values chosen here, and may differ if the library is built with modified defaults.
- Whether `x264_param_default_preset` can fail for reasons other than an invalid preset name is **UNKNOWN** from this file.
- The exact meaning of `pic_out` fields is not consumed here; see `x264.h.md` for their contracts.
