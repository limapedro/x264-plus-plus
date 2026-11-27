# filters/video/internal.h — internal video-filter utilities

*Dirty-side behavioural specification. Reference source: `filters/video/internal.h`. Behavioural descriptions only; no source or near-verbatim pseudocode is reproduced.*

## 1. Header

- **Source path:** `filters/video/internal.h`
- **Role:** Declares two small helpers used internally by filters to copy plane and picture data.
- **Marker:** dirty-side behavioural specification.

## 2. Purpose

Filters such as `cache` and `fix_vfr_pts` need to duplicate a frame into their own buffer while preserving stride and metadata. This header declares the plane-copy and picture-copy helpers that implement that operation consistently with the CLI colourspace geometry.

## 3. Public interface

- `void x264_cli_plane_copy(uint8_t *dst, int i_dst, uint8_t *src, int i_src, int w, int h)` — copy `w` bytes per row for `h` rows, advancing source and destination by their respective strides.
- `int x264_cli_pic_copy(cli_pic_t *out, cli_pic_t *in)` — copy a whole picture's metadata and all plane data; returns 0 on success, nonzero if the pictures are incompatible or the colourspace invalid.

The header includes `filters/video/video.h`, so `cli_pic_t`, `cli_image_t`, and the colourspace helpers are in scope.

## 4. State and data structures

No state; both helpers are pure with respect to global data. `x264_cli_pic_copy` assumes `out`'s planes are already allocated with sufficient size and matching strides are not required (it copies row-by-row using each picture's own stride).

## 5. Algorithm / behaviour

Contracts (details implemented in `internal.c`):

- Plane copy performs exactly `h` iterations, each copying `w` bytes from `src` to `dst`, then adding the respective stride. It assumes `w ≤ min(stride_src, stride_dst)`.
- Picture copy validates the input colourspace, requires source and destination to have identical csp, width, and height, copies `pts`, `duration`, and `opaque` from input to output, then for each output plane derives the byte width as `width * width_factor * depth_factor` and the row count as `height * height_factor`, and calls plane copy.

## 6. Dependencies

- `filters/video/video.h` → `input/input.h` (colourspace table, picture types).
- Implemented by `filters/video/internal.c`; used by `cache.c` and `fix_vfr_pts.c`.

## 7. Constants and tables

- No constants; geometry comes from `x264_cli_csps[]`.

## 8. Performance notes

- Copy helpers are on the per-frame path for cache/VFR filters; they are simple `memcpy` loops.
- **Reservation sizing:** destination planes must be allocated once (cache/fix_vfr_pts do this in `init`).
- **Zero-copy:** these helpers are the explicit exception — copying is required because the caller needs a stable snapshot.
- No SIMD in the reference implementation; a clean-room implementation may use wider vector copies while respecting strides.
- Reference: standard memory-copy optimization (stride-aware row copies).

## 9. References

- `filters/video/internal.c`, `filters/video/cache.c`, `filters/video/fix_vfr_pts.c` (companion specifications).

## 10. Verification checklist

1. Plane-copy with different source/destination strides and verify each row's bytes are correct and no overread occurs.
2. Picture-copy between compatible pictures and verify metadata (pts/duration/opaque) and all planes match.
3. Verify incompatible csp/width/height is rejected.
4. Verify invalid input csp is rejected.
5. Verify high-depth (2-byte) planes are copied completely.

## 11. Open questions

- UNKNOWN: whether `opaque` should be shallow-copied in all cases — the reference does so; filters using it must avoid double release (cache stores copies but does not release them itself).
