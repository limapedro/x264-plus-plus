# filters/video/internal.c — plane and picture copy implementation

*Dirty-side behavioural specification. Reference source: `filters/video/internal.c`. Behavioural descriptions only; no source or near-verbatim pseudocode is reproduced.*

## 1. Header

- **Source path:** `filters/video/internal.c`
- **Role:** Implements stride-aware plane copying and whole-picture duplication used by caching and VFR-repair filters.
- **Marker:** dirty-side behavioural specification.

## 2. Purpose

When a filter must retain a frame beyond the lifetime granted by its predecessor (cache, fix_vfr_pts), it needs to snapshot the pixel data into its own buffer. This file provides the primitive for that: copy each row of each plane using the source and destination strides, after validating that the pictures describe the same format and geometry.

## 3. Public interface

Implements the two functions declared in `filters/video/internal.h`:

- `void x264_cli_plane_copy(dst, dst_stride, src, src_stride, width, height)`
- `int x264_cli_pic_copy(out, in)`

Error tag: `"x264"`.

## 4. State and data structures

No state. `x264_cli_pic_copy` reads the source colourspace's base value (masked), uses the global CLI colourspace descriptor table for plane geometry, and writes into the destination picture's already-allocated planes. `out` and `in` are independent structures; `out`'s `opaque` becomes a shallow copy of `in`'s.

## 5. Algorithm / behaviour

**Plane copy.** A simple loop over `height` rows: copy `width` bytes from the current source pointer to the current destination pointer, then advance both pointers by their own stride. No bounds checking; callers guarantee `width` fits within both strides.

**Picture copy.** Steps:

1. Mask the input csp to its base and reject an invalid input colourspace.
2. Require the input and output csp values to be exactly equal, and their width and height equal; otherwise reject with "incompatible frame properties". (Strides are deliberately allowed to differ.)
3. Copy `duration`, `pts`, and `opaque` from input to output verbatim.
4. For each output plane (index up to `out->img.planes`): compute the row byte count as `in->img.width * width_factor[plane] * depth_factor` and the row count as `in->img.height * height_factor[plane]`; call plane copy with the two pictures' strides.

Because the csp must match exactly, high-depth flags and `X264_CSP_OTHER` markers are preserved in the comparison; geometry factors always come from the masked base format.

## 6. Dependencies

- `filters/video/internal.h`, `input/input.h` helpers (`x264_cli_csp_is_invalid`, `x264_cli_csps`, `x264_cli_csp_depth_factor`), standard `string.h` (`memcpy`).
- Used by `cache.c` (snapshot a frame into a cache slot) and `fix_vfr_pts.c` (snapshot before reading the next frame).

## 7. Constants and tables

- No constants; all geometry derives from `x264_cli_csps[]`.

## 8. Performance notes

- Per-frame cost is one `memcpy` per plane row; for 1080p this is several MiB, so this helper can be measurable.
- **Reservation sizing:** destination buffers are allocated once in the filters' `init`.
- **Zero-copy:** not possible here — a snapshot is the purpose.
- **Optimization tips:** use wide SIMD `memcpy` where available; ensure destination strides are aligned (the cache filter uses ordinary `x264_cli_pic_alloc`, which gives stride = width × factor × depth; a clean-room implementation may prefer aligned allocation to enable wider loads). Avoid per-row function-call overhead by inlining the row loop where the compiler supports it.
- Reference: standard cache-friendly row copying.

## 9. References

- `filters/video/internal.h`, `filters/video/cache.c`, `filters/video/fix_vfr_pts.c` (companion specifications).

## 10. Verification checklist

1. Plane-copy between buffers with different strides and verify no row bleeds into the next.
2. Picture-copy a 4:2:0 high-depth frame and verify Y/U/V planes and depths are correct.
3. Verify mismatched csp/width/height returns failure without modifying the output.
4. Verify metadata fields are copied.
5. Verify no heap corruption with ASAN when destination strides exceed minimum width.

## 11. Open questions

- UNKNOWN: whether `out->img.planes` can exceed the valid plane count of its csp (it cannot in normal use, since pictures are initialised by the shared helpers), in which case the loop would read a zero-factor descriptor.
