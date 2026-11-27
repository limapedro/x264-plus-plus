# input/y4m.c — YUV4MPEG2 (Y4M) demuxer

*Dirty-side behavioural specification. Reference source: `input/y4m.c`. Behavioural descriptions only; no source or near-verbatim pseudocode is reproduced.*

## 1. Header

- **Source path:** `input/y4m.c`
- **Role:** Implements the `y4m_input` demuxer for the YUV4MPEG2 container: a text sequence header followed by `FRAME` headers and fixed-size raw planar frames.
- **Marker:** dirty-side behavioural specification.

## 2. Purpose

Y4M adds just enough metadata to raw video to make it self-describing: resolution, frame rate, aspect ratio, interlacing, and colour format are all in a text header, while the frame payload remains uncompressed planar samples. This module parses that header, maps the advertised format onto the CLI colourspace registry, computes frame geometry including the per-frame header length, and delivers frames (optionally memory-mapped). Together with `raw.c` it is the input path an independent clean-room encoder must implement to be useful with test files.

## 3. Public interface

Exports `const cli_input_t y4m_input` with the standard six callbacks. Contract details:

- `open_file` opens the file (or stdin for `"-"`), reads and parses the sequence header, derives all `video_info_t` fields, counts frames for regular files, and decides whether to map.
- `picture_alloc`, `picture_clean`, `close_file` mirror the raw demuxer's mapped/buffered split.
- `read_frame` validates each frame header, computes plane pointers, and applies high-depth up-conversion when needed.
- `release_frame` unmaps when mapped.

The parsed sequence header must begin with the literal magic `YUV4MPEG2`; the per-frame header must begin with `FRAME`. Any violation is reported with the `y4m` error tag.

## 4. State and data structures

`y4m_hnd_t` holds: `fh`, `next_frame`, `seq_header_len` (bytes consumed by the sequence header including its newline), `frame_header_len` (bytes in a per-frame header including newline; measured once from the first frame), `frame_size` (frame header + all planes, in bytes), `plane_size[3]` (per-plane sample counts), `bit_depth`, `mmap`, and `use_mmap`. Invariants: `frame_size = frame_header_len + depth_factor * sum(plane_size[i])`; the first frame's header length is assumed to hold for all frames; a mapped frame's `plane[0]` initially points at the start of the `FRAME` header and is advanced past the header before planes are laid out.

**Header buffer limit.** `Y4M_MAX_HEADER = 256` bytes: both the sequence header and a frame header longer than this are rejected. The header is also required to be at least as long as the frame magic.

## 5. Algorithm / behaviour

### Sequence header parsing
The opener reads bytes up to and including the first newline, storing them in a 256-byte buffer. On encountering the newline it writes a space just after it and a terminator after that — the trailing space makes token matching unambiguous (so `444` is distinguishable from `444alpha`). It then verifies the magic prefix and that a newline was actually found within the limit.

Tokens are scanned one character at a time starting just after the magic; a space advances to the next token, and the first non-space character selects a property:

- `W` — width, parsed as a decimal integer; required in practice.
- `H` — height, parsed as a decimal integer.
- `C` — colour format; passed to the format/depth parser (below).
- `I` — interlacing: `t` sets interlaced with top-field-first, `b` sets interlaced with bottom-field-first, `m` sets interlaced with field order unknown; `p` and `?` leave progressive defaults.
- `F` — frame rate `num:den`; only used when both are nonzero, and reduced to lowest terms before storing.
- `A` — pixel aspect ratio `num:den`; likewise reduced and stored, but only if the caller has not already set a SAR explicitly (the frontend's override is respected — see open questions).
- `X` — vendor extensions. Two are recognised: `YSCSS=` supplies an older nonstandard colour-format token (and its depth), used as a fallback if `C` was absent; `COLORRANGE=` with value `FULL` or `LIMITED` sets `fullrange` to 1 or 0.

After tokenisation, if no `C` format was parsed the fallback from `YSCSS` (and its depth) is used; if still none, the module defaults to 8-bit `i420`. The format value must lie strictly between NONE and the CLI maximum, and the depth must be 8..16. `thread_safe = 1` and `vfr = 0` are reported; `num_frames` starts at zero.

### Colour format parsing
The format token is matched by prefix: `mono` → `i400`; `420` → `i420`; `422` → `i422`; `444` → `i444` only if it is not `444alpha` (alpha formats are rejected). Depth is extracted by trying two patterns: a `mono` prefix followed by a decimal depth, or a decimal subsampling token followed by `p`/`P` and a decimal depth (e.g. `420p10`). If neither matches, depth defaults to 8. Depths above 8 set the high-depth flag on the reported colourspace.

### Geometry and frame counting
Plane sizes are derived with the shared helpers and stored in samples; `frame_size` accumulates their byte sizes. For a regular file, the module records the stream position right after the sequence header, reads one frame header to learn `frame_header_len`, adds that length to `frame_size`, seeks to end, and computes `num_frames = (file_length - seq_header_len) / frame_size`. A zero count is an "empty input file" error. Mapping is enabled only for byte-aligned depths (bit depth multiple of 8).

### Frame reading
A frame's bytes begin at `seq_header_len + frame_index * frame_size`. In mapped mode the whole frame (header included) is mapped at that offset; in buffered seekable mode the stream is positioned there; in buffered non-seekable mode frames are skipped by reading and discarding. The frame header is then validated:

- *Mapped:* the header is the start of the mapping; `plane[0]` is advanced by `frame_header_len`. The code walks the header checking for its terminating newline and requires the newline to be exactly at `frame_header_len` — a variable-length frame header would invalidate the fixed-size mapping and is rejected outright (the reference notes only fuzzers produce such files).
- *Buffered:* the `FRAME` magic is read into a small stack buffer, then bytes are consumed up to the newline (bounded by the 256-byte limit); the magic is compared and a bad value rejected.

Plane pointers are then laid out: `plane[0]` is either the advanced mapped pointer or the allocated buffer; each later plane advances from the previous by `depth_factor * plane_size[previous]`. In buffered mode each plane is `fread` as `depth_factor` bytes × sample count, with a short read as failure. Non-byte-aligned depths are up-converted in place by left-shifting every 16-bit sample by `16 - bit_depth`. `next_frame` is set to `frame_index + 1`.

### Release and teardown
`release_frame` unmaps using `plane[0] - frame_header_len` and the full frame size so the mapping base is recovered. `picture_clean` zeroes the picture when mapped and frees otherwise. `close_file` closes the mapping (if any), closes the file, and frees the handle.

## 6. Dependencies

- `input/input.h` and `input/input.c` helpers (colourspace table, picture allocation, mmap).
- Error/logging macros tagged `y4m`; `x264_fopen`, `x264_is_regular_file`, `x264_reduce_fraction`.
- Wrapped by `thread_*` and `timecode_input`; reached through the `source` filter.

## 7. Constants and tables

- Sequence magic: `YUV4MPEG2` (9 characters).
- Frame magic: `FRAME` (5 characters).
- `Y4M_MAX_HEADER = 256` bytes for both header types.
- Defaults when `C` is missing: `i420`, depth 8.
- Recognised `C` prefixes: `mono`, `420`, `422`, `444` (the literal `444alpha` is explicitly excluded).
- Recognised extensions: `XCOLORRANGE=FULL|LIMITED`, `XYSCSS=<format>`.
- Error tag: `"y4m"`.

## 8. Performance notes

- Per-frame work is one mapping (or one file seek plus one `fread` per plane) and a small header check — comparable to the raw path.
- **Zero-copy:** mapped mode avoids all payload copies; keep the header-length assumption so a single mapping covers header+planes.
- **I/O buffering:** buffered mode uses one `fread` per plane; skip-ahead on non-seekable input reuses the same read path with up-conversion disabled.
- **Reservation sizing:** frame buffers are allocated once; the measured first-frame header length is cached, avoiding re-parsing.
- **Branch behaviour:** the header scan is cold; the plane loop is small and predictable.
- References: YUV4MPEG2 format description; `mmap(2)`/`MapViewOfFile` docs.

## 9. References

- YUV4MPEG2 format specification (public description of the `YUV4MPEG2`/`FRAME` header grammar), e.g. <https://wiki.multimedia.cx/index.php/YUV4MPEG2>.
- `input/input.h`, `input/input.c`, `input/raw.c` (companion specifications).
- ITU-T H.264 / ISO 14496-10 for planar chroma geometry.

## 10. Verification checklist

1. Generate a Y4M with `W192 H108 C420 F30:1 A1:1 Ip` and confirm width/height/FPS/SAR/progressive are parsed.
2. Confirm `C420p10` (or `C420p10` via prefix) yields 10-bit high-depth and that samples are shifted left by 6.
3. Confirm `Cm` interlacing variants set `interlaced` and the correct `tff`.
4. Confirm `XCOLORRANGE=FULL` sets `fullrange = 1`.
5. Confirm filenames/headers with `444alpha` are rejected while plain `444` is accepted.
6. Verify frame count equals `(filesize - seq_header_len) / (frame_header_len + plane_bytes)`.
7. Confirm a corrupted `FRAME` magic is rejected in both buffered and mapped modes.
8. Confirm a frame header longer than the first one is rejected in mapped mode.
9. Verify plane sample data round-trips exactly for 8-bit and 16-bit sources.

## 11. Open questions

- UNKNOWN: whether the SAR override behaviour is implemented here or entirely in the frontend; the module sets SAR whenever `A` is present, and the header comment says it avoids overriding an explicit command-line SAR, but the actual suppression may happen before this module is called.
- UNKNOWN: exact behaviour for `F0:0` / `A0:0` (documented as "unknown") — values are ignored here, leaving `fps_num/fps_den` at whatever the caller supplied.
