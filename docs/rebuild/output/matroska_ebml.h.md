# output/matroska_ebml.h — EBML/Matroska writer interface

*Dirty-side behavioural specification. Reference source: `output/matroska_ebml.h`. Behavioural descriptions only; no source or near-verbatim pseudocode is reproduced.*

## 1. Header

- **Source path:** `output/matroska_ebml.h`
- **Role:** Declares the opaque Matroska writer and the functions used by `matroska.c` to build EBML elements, clusters, and simple blocks.
- **Marker:** dirty-side behavioural specification.

## 2. Purpose

Matroska is an EBML-based container. This header exposes a small stateful writer API: create a writer, emit the header (EBML + Segment info + track with `avcC`), then feed frames one at a time with timestamps/flags, and finally close with duration patching. Keeping this as a separate layer lets `matroska.c` focus on translating x264 parameters into Matroska metadata while this API handles EBML encoding and cluster/block layout.

## 3. Public interface

### Display-size constants
`DS_PIXELS = 0`, `DS_CM = 1`, `DS_INCHES = 2`, `DS_ASPECT_RATIO = 3` — values for Matroska's `DisplayUnit` element.

### Opaque type
`typedef struct mk_writer mk_writer;` — implementation hidden in `matroska_ebml.c`.

### Functions
- `mk_writer *mk_create_writer(const char *filename)` — allocate writer and open output; `"-"` means stdout.
- `int mk_write_header(w, writing_app, codec_id, codec_private, codec_private_size, default_frame_duration, timescale, width, height, d_width, d_height, display_size_units, stereo_mode)` — emit all header elements; `stereo_mode < 0` disables stereo elements and sets DocTypeVersion 2, otherwise version 3.
- `int mk_start_frame(w)` — begin a new frame (flushes any previous frame).
- `int mk_add_frame_data(w, data, size)` — append bytes to the current frame (may be called multiple times, e.g. SEI then access unit).
- `int mk_set_frame_flags(w, timestamp, keyframe, skippable)` — set the frame's timestamp (in timescale ticks) and flags.
- `int mk_close(w, last_delta)` — flush frame/cluster, patch duration for seekable files, and destroy writer.

All `int` functions return 0 on success, negative on error.

## 4. State and data structures

State is entirely inside the opaque `mk_writer` (fields documented in the `.c` spec). Callers own the `codec_private` buffer passed to `mk_write_header` (the Matroska muxer builds and frees its own `avcC` block).

## 5. Algorithm / behaviour

The header defines no algorithm. Its contract implies a specific call sequence: `create_writer` → `write_header` → repeat {`start_frame`, `add_frame_data`*, `set_frame_flags`} → `close`. `start_frame` implicitly flushes the previous frame, so the first call after the header is harmless. Frames can be assembled from multiple `add_frame_data` calls (used to prepend SEI). `set_frame_flags` must be called before the frame is implicitly flushed by the next `start_frame` or by `close`.

## 6. Dependencies

- Included only by `output/matroska.c`.
- Uses base types from `x264cli.h` via `output/output.h` in the implementation; the header itself is plain C.

## 7. Constants and tables

- Display-unit values listed in section 3 (Matroska `DisplayUnit` enumeration).
- No other constants; EBML element IDs are implementation details in `matroska_ebml.c`.

## 8. Performance notes

- The API is designed around a growing in-memory frame/cluster buffer so EBML size fields can be back-patched.
- **Reservation sizing:** frame and cluster buffers grow geometrically; a clean-room implementation may reserve based on expected frame size.
- **I/O buffering:** data is flushed at cluster boundaries and at close; each cluster is written in one `fwrite`.
- **Zero-copy:** frame bytes are copied into the frame buffer because block size must be known before writing; unavoidable for this container.
- Reference: Matroska specification, SimpleBlock and Cluster layout.

## 9. References

- Matroska specification: <https://www.matroska.org/technical/specs/index.html> (EBML header, Segment, Tracks, Cluster, SimpleBlock, TimecodeScale, Duration).
- `output/matroska_ebml.c`, `output/matroska.c` (companion specifications).

## 10. Verification checklist

1. Confirm `mk_create_writer("-")` returns a writer that writes to stdout and that `mk_close` does not close stdout.
2. Confirm `write_header` fails if called twice.
3. Confirm frames can be built from multiple `add_frame_data` calls and that all data appears contiguously in the block.
4. Confirm `set_frame_flags` fails if called before `start_frame`.
5. Confirm a keyframe flag maps to the SimpleBlock flags byte's high bit and skippable to bit 0.
6. Confirm `mk_close` flushes the last frame and cluster and patches Duration only for regular files.

## 11. Open questions

- UNKNOWN: whether `mk_add_frame_data` after `set_frame_flags` is legal — likely yes (flags only affect the block header), but not explicitly documented.
