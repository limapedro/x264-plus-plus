# filters/video/video.c — video filter registry

*Dirty-side behavioural specification. Reference source: `filters/video/video.c`. Behavioural descriptions only; no source or near-verbatim pseudocode is reproduced.*

## 1. Header

- **Source path:** `filters/video/video.c`
- **Role:** Builds the global registry of video filters and provides name-based initialisation and help dispatch.
- **Marker:** dirty-side behavioural specification.

## 2. Purpose

The CLI must be able to instantiate a filter by the name given in `--vf`, and print help for all available filters. This file holds the singly-linked registry, seeds it with the `source` filter, appends all compiled-in filters (with bit-depth-specific variants), and implements lookup/help traversal.

## 3. Public interface

Implements the three helpers declared in `filters/video/video.h`:

- `void x264_register_vid_filters(void)`
- `void x264_vid_filter_help(int longhelp)`
- `int x264_init_vid_filter(const char *name, hnd_t *handle, cli_vid_filter_t *filter, video_info_t *info, x264_param_t *param, char *opt_string)`

Each registered filter is an externally declared `cli_vid_filter_t` instance; registration appends it to the list.

## 4. State and data structures

A single static pointer `first_filter` heads the registry. Registration appends to the tail by walking `next` links; the newly registered filter's `next` is cleared. The list is built once at startup; no locking is provided (registration is expected before any encoding begins).

## 5. Algorithm / behaviour

**Registration.** `first_filter` is set directly to the `source_filter` instance. Then, conditionally on build flags: for each enabled bit depth (`HAVE_BITDEPTH8`, `HAVE_BITDEPTH10`), the `cache_<depth>` and `depth_<depth>` filters are appended. Unconditionally, `crop`, `fix_vfr_pts`, `resize`, and `select_every` are appended. An empty GPL-conditional block exists but registers nothing in this build. The append helper walks to the last node and links the new one.

**Initialisation by name.** The lookup walks from `first_filter`, comparing the requested name case-insensitively against each filter's `name`. If no match, an error tagged `"x264"` is logged and failure returned. On match, the filter's `init` is invoked with all the caller's arguments; its return value is propagated.

**Help.** Walks the registry; for each filter whose `help` pointer is non-NULL, calls it with the `longhelp` flag.

## 6. Dependencies

- `filters/video/video.h`, `x264cli.h` (`FAIL_IF_ERR`, `strcasecmp`).
- References each filter's extern instance: `source_filter`, `cache_8/10_filter`, `depth_8/10_filter`, `crop_filter`, `fix_vfr_pts_filter`, `resize_filter`, `select_every_filter`.

## 7. Constants and tables

- Registered order: source → (cache_8, depth_8) → (cache_10, depth_10) → crop → fix_vfr_pts → resize → select_every (bit-depth pairs present only when compiled in).
- No other constants.

## 8. Performance notes

- Registration and lookup are O(number of filters) and happen once; no hot-path cost.
- **Reservation sizing:** not applicable (static instances).
- No threading: registry is read-only after `x264_register_vid_filters`.

## 9. References

- `filters/video/video.h`, `filters/video/source.c`, `filters/video/cache.c`, `filters/video/depth.c` (companion specifications).

## 10. Verification checklist

1. Confirm `first_filter` is `source` after registration and that all expected filters are reachable in the documented order.
2. Confirm case-insensitive lookup (`"CROP"` finds `"crop"`).
3. Confirm unknown names log an error and return nonzero.
4. Confirm help is printed only for filters with a `help` callback.
5. Confirm filter `init` return values propagate to the caller.

## 11. Open questions

- UNKNOWN: whether `x264_register_vid_filters` may be called more than once — doing so would duplicate entries; no guard is present.
