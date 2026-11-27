# input/raw.c — raw planar video demuxer

*Dirty-side behavioural specification. Reference source: `input/raw.c`. Behavioural descriptions only; no source or near-verbatim pseudocode is reproduced.*

## 1. Header

- **Source path:** `input/raw.c`
- **Role:** Implements the `raw_input` demuxer that reads uncompressed, headerless planar YUV frames from a file or stdin.
- **Marker:** dirty-side behavioural specification.

## 2. Purpose

A raw video file has no metadata: it is a concatenation of fixed-size frames. This module is the most important input path for a clean-room encoder, because it lets a test harness feed arbitrary planar frames with full control over resolution, coloursample format, and bit depth. It derives those parameters from the command line (or from the filename), computes the frame geometry using the shared helpers, counts frames from file size, and optionally memory-maps frames for zero-copy delivery.

## 3. Public interface

Exports `const cli_input_t raw_input` with the standard six callbacks (see `input/input.h` spec). Contract details:

- `open_file` requires a resolution; it parses it from `opt->resolution` if present, otherwise scans the filename for the first digit run and attempts `WxH`. Missing/zero width or height is an error labelled `raw`.
- Colourspace: if `opt->colorspace` is set, the table is searched from the highest entry downward for a case-insensitive name match; failure is an error. Default is `i420`.
- Bit depth comes from `opt->bit_depth`; valid range is 8..16 inclusive; values above 8 set the `X264_CSP_HIGH_DEPTH` flag on `info->csp`.
- `"-"` selects stdin; otherwise the file is opened read-only ("rb").
- Reported properties: `thread_safe = 1`, `vfr = 0`, `num_frames` from file size for regular files (0 for pipes/stdin). Memory mapping is attempted only for regular files whose bit depth is a multiple of 8.
- `picture_alloc` uses no-allocation initialisation when mapping is active, ordinary allocation otherwise.
- `read_frame` returns successive frames; `release_frame` unmaps; `picture_clean` zeroes when mapped, frees otherwise; `close_file` tears down the mapping and handle.

## 4. State and data structures

`raw_hnd_t` holds: `fh` (the `FILE*`), `next_frame` (index of the next frame the stream pointer would deliver, initially 0), `plane_size[4]` (per-plane sample counts, i.e. byte size divided by depth factor), `frame_size` (total bytes per frame, including all planes but no headers), `bit_depth`, `mmap` (the mapping state), and `use_mmap` (boolean). `plane_size[]` is deliberately stored in *pixels/samples*, not bytes — the byte count is recovered by multiplying by the depth factor at read time. Invariants: `frame_size = depth_factor * sum(plane_size[i])`; `next_frame` advances monotonically; a mapped handle never reads through `fh` after a frame is delivered.

## 5. Algorithm / behaviour

**Open.** A zeroed handle is allocated. Resolution is obtained as described; if it comes from the filename, the scan stops at the first character that is a digit and for which an `sscanf`-style `WxH` parse succeeds — this means a filename like `clip_1920x1080.y4m` yields 1920×1080. The colourspace lookup walks the CLI descriptor table backwards so that, if names ever collided, the highest-indexed (most specific) entry wins. Bit-depth validation rejects anything outside 8..16; depths 9..15 set the high-depth flag while retaining the underlying planar format. Opening stdin is supported; stdin is not a regular file, so frame counting and mapping are skipped.

**Geometry and frame count.** For each plane of the chosen colourspace the shared size helper gives a byte count; the handle stores the byte count divided by the depth factor (samples), and accumulates the byte count into `frame_size`. For a regular file the code seeks to end, records the length, seeks back, and sets `num_frames = floor(length / frame_size)`. A zero frame count is an "empty input file" error. Memory mapping is attempted when `bit_depth & 7 == 0` (i.e. 8 or 16 bits) — this is because mapping assumes 1 or 2 bytes per sample and the up-conversion path below is meant for non-aligned depths.

**Reading a frame.** Three modes:

1. *Mapped mode:* `x264_cli_mmap` is called with `offset = frame_index * frame_size` and `size = frame_size`. The returned pointer becomes `plane[0]`; each subsequent plane pointer is formed by advancing the previous one by `depth_factor * plane_size[previous]`. Thus all planes of a frame are contiguous slices of one mapping and no allocation or copy occurs.
2. *Buffered mode, forward seek:* if the requested frame is ahead of `next_frame` and the file is regular, the stream is repositioned with `fseek` to `frame_index * frame_size`. This permits random forward access (needed by filters) without reading intervening frames.
3. *Buffered mode, sequential/non-regular:* if the request is ahead and the file is not seekable, the routine discards frames one at a time by reading them into the supplied picture (with up-conversion disabled) and incrementing `next_frame` until the target is reached.

The frame itself is then read: in mapped mode the plane pointers are already set; in buffered mode each plane is `fread` as `depth_factor` bytes × `plane_size[i]` samples, with a short read treated as failure. If the source bit depth is not byte-aligned (`bit_depth & 7` nonzero), every sample in every plane is left-shifted by `16 - bit_depth` so that 9..15-bit input is presented to the encoder as 16-bit samples. This is the same left-shift used by the depth filter's up-conversion. Finally `next_frame` is set to `frame_index + 1`.

**Release/cleanup.** `release_frame` only has work in mapped mode, where it unmaps the first plane pointer using the total frame size. `picture_clean` zeroes the picture when mapped (no owned plane memory) and otherwise delegates to the shared picture cleaner. `close_file` is idempotent for null handles, closes the mapping (if any), closes the file (never closing stdin is explicitly avoided by the caller convention in practice — see open questions), and frees the handle.

## 6. Dependencies

- `input/input.h` and its helpers: `x264_cli_get_csp`, `x264_cli_pic_plane_size`, `x264_cli_csp_depth_factor`, `x264_cli_pic_alloc`/`init_noalloc`, `x264_cli_mmap*`.
- Error/logging macros (`FAIL_IF_ERR`, tagged `raw`), `x264_fopen`, `x264_is_regular_file`.
- Called by the CLI frontend; wrapped by the `thread_*` and `timecode_input` demuxers and reached through the `source` filter.

## 7. Constants and tables

- Default colourspace is `X264_CSP_I420` (planar 4:2:0).
- Accepted bit depth range: 8..16.
- No magic bytes or file header: frame size is entirely determined by resolution, colourspace, and depth.
- Error tag is `"raw"` for all failures emitted by this module.

## 8. Performance notes

- This is a top-hotness path: one read/mmap and one pointer-arithmetic chain per frame.
- **Zero-copy:** mapped mode is the fast path — no `fread`, no per-plane copy. Prefer regular files for benchmarks.
- **I/O buffering:** in buffered mode a single `fread` per plane is used; a clean implementation should keep that granularity and rely on stdio buffering rather than many small reads.
- **Seek vs. discard:** forward random access on a regular file uses one `fseek`; on a pipe it must discard frames, which is inherently slow and only used when filters request non-sequential frames.
- **Reservation sizing:** `next_frame` bookkeeping avoids redundant seeks; plane buffers are allocated once at `picture_alloc`.
- **Up-conversion:** the shift loop for 9..15-bit input is O(samples) and unavoidable for those sources; for 8/16-bit sources it is skipped by the `bit_depth & 7` test.
- References: `mmap(2)`/`MapViewOfFile` docs; x264 CLI input documentation (<https://x264.readthedocs.io/>).

## 9. References

- `input/input.h`, `input/input.c` (companion specifications).
- ITU-T H.264 / ISO 14496-10 — planar YUV sample formats.
- POSIX/Win32 file mapping APIs (see `input.c` spec).

## 10. Verification checklist

1. Create a raw I420 file of known `W*H*3/2` bytes; open with explicit resolution and confirm `num_frames = filesize/frame_size`.
2. Feed a known gradient frame and confirm each plane pointer is contiguous and sample values match the file bytes byte-for-byte (8-bit case).
3. For 10-bit input, confirm each stored sample equals the file's little-endian 16-bit value shifted left by 6.
4. Confirm filenames containing `WxH` are parsed correctly and that a missing resolution is rejected.
5. Confirm an unsupported colorspace name and out-of-range bit depths are rejected with the `raw` tag.
6. With stdin input, confirm frames are read sequentially and that a skipped frame request is handled by discarding.
7. In mapped mode, confirm `release_frame` actually unmaps and that mapping again works (no address-space leak).
8. Feed a truncated final frame and confirm the short read returns failure.

## 11. Open questions

- UNKNOWN: whether `close_file` intentionally closes stdin when `"-"` was used; the source calls `fclose(h->fh)` unconditionally, so a single-shot CLI invocation is unaffected, but this matters for embedders.
- UNKNOWN: behaviour when `frame_size` is zero (resolution parse succeeded but a format has zero-size planes) — division by zero during frame counting; not documented as guarded.
