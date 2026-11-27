# filters/video/video.h — video filter plugin contract

*Dirty-side behavioural specification. Reference source: `filters/video/video.h`. Behavioural descriptions only; no source or near-verbatim pseudocode is reproduced.*

## 1. Header

- **Source path:** `filters/video/video.h`
- **Role:** Defines the `cli_vid_filter_t` vtable that every video filter implements, plus the registration/initialisation helpers used by the frontend.
- **Marker:** dirty-side behavioural specification.

## 2. Purpose

Video filters (source adapter, cache, crop, depth conversion, VFR PTS repair, resize, select-every) are chained together to transform frames between the demuxer and the encoder. This header defines the uniform contract: each filter exposes a name, optional help, an `init` that may wrap the previous filter, `get_frame`/`release_frame` callbacks, and a `free` callback. Understanding this contract is essential to reproducing filter-chain graph semantics in a clean-room implementation.

## 3. Public interface

### `struct cli_vid_filter_t`
Fields (all function pointers except `name`/`next`):

- `const char *name` — filter identifier used by `--vf` and by `x264_init_vid_filter`.
- `void (*help)(int longhelp)` — prints usage; implemented only for user-facing filters, may be NULL.
- `int (*init)(hnd_t *handle, cli_vid_filter_t *filter, video_info_t *info, x264_param_t *param, char *opt_string)` — initialise the filter, possibly mutating `*handle` (wrap the previous handle), `*filter` (become the active filter), and `info` (report output properties). Returns 0 on success.
- `int (*get_frame)(hnd_t handle, cli_pic_t *output, int frame)` — produce frame `frame` into `output`; returned image data must be treated as const.
- `int (*release_frame)(hnd_t handle, cli_pic_t *pic, int frame)` — signal that a frame is no longer needed.
- `void (*free)(hnd_t handle)` — tear down the filter instance.
- `cli_vid_filter_t *next` — registration-list link, unused by filters themselves.

### Helpers
- `void x264_register_vid_filters(void)` — build the static registry.
- `void x264_vid_filter_help(int longhelp)` — print help from all registered filters.
- `int x264_init_vid_filter(const char *name, hnd_t *handle, cli_vid_filter_t *filter, video_info_t *info, x264_param_t *param, char *opt_string)` — find a registered filter by (case-insensitive) name and invoke its `init`.

The header includes `input/input.h` (so `cli_pic_t`, `video_info_t`, `hnd_t` are in scope) and `filters/filters.h` (option parsing).

## 4. State and data structures

A filter instance is created by `init`, which overwrites `*handle` with its own private handle (retaining the previous handle and filter vtable inside it) and overwrites `*filter` with its own vtable. This is the chaining mechanism: each filter becomes the new head of the chain and keeps a copy of the previous `(handle, filter)` pair to delegate to. The `source` filter is the chain head by construction (it wraps the demuxer). Instances are freed by walking the chain via each filter's `free`, which must call the previous filter's `free` first.

## 5. Algorithm / behaviour

The contract defines no algorithm. Important semantics:

- `get_frame` requests must generally be increasing (the `source` filter forbids backward requests); filters that need rewind insert a `cache` filter internally (e.g. `select_every`).
- Frames returned by `get_frame` are borrowed; `release_frame` must be called with the same frame index to allow underlying resources (demuxer frames, AviSynth objects, mmaps) to be freed.
- `init` may modify `video_info_t` to reflect transformations (crop changes width/height, resize changes csp/width/height/SAR/range, select_every changes frame count/fps/timebase).
- `init` may decide to do nothing (leave `*handle`/`*filter` unchanged) when no transformation is needed — e.g. crop with all-zero margins, depth when format already matches.
- Filters are looked up by name through the global registry built by `video.c`.

## 6. Dependencies

- `input/input.h`, `filters/filters.h`, `x264cli.h`.
- Implemented/used by all files in `filters/video/`.
- The frontend builds the chain by calling `x264_init_vid_filter` for each `--vf` element after the `source` filter is installed.

## 7. Constants and tables

- No constants in this header.
- Filter names are defined by each filter's static instance (`"source"`, `"cache_8"`/`"cache_10"`, `"crop"`, `"depth_8"`/`"depth_10"`, `"fix_vfr_pts"`, `"resize"`, `"select_every"`).

## 8. Performance notes

- The vtable indirection is one call per frame per filter; negligible relative to pixel work.
- **Zero-copy:** filters should pass plane pointers through by default and only copy when the transformation requires it (cache, depth, resize).
- **Reservation sizing:** each filter should allocate its buffers once in `init`.
- **Branch behaviour:** unchanged/no-op filters are removed from the chain at init, avoiding per-frame overhead.
- Reference: x264 CLI `--vf` documentation.

## 9. References

- `filters/video/video.c`, `filters/video/source.c`, `filters/video/cache.c` (companion specifications).
- x264 CLI filter documentation: <https://x264.readthedocs.io/>.

## 10. Verification checklist

1. Confirm a freshly registered chain starts with the `source` filter.
2. Confirm `x264_init_vid_filter` finds names case-insensitively and reports unknown names.
3. Confirm `help` is only invoked for filters that implement it.
4. Confirm an `init` that no-ops leaves `*handle`/`*filter` untouched.
5. Confirm frame indices passed to `get_frame`/`release_frame` correspond to the same logical output frame.

## 11. Open questions

- UNKNOWN: exact frontend order in which built-in implicit filters (normcsp/resize/fix_vfr_pts/depth/cache) are layered relative to user `--vf` filters; not encoded in this header.
