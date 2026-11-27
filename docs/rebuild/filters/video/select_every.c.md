# filters/video/select_every.c — frame-selection filter

*Dirty-side behavioural specification. Reference source: `filters/video/select_every.c`. Behavioural descriptions only; no source or near-verbatim pseudocode is reproduced.*

## 1. Header

- **Source path:** `filters/video/select_every.c`
- **Role:** Implements `select_every_filter`, which keeps only selected offsets of a repeating frame pattern (AviSynth `SelectEvery` semantics), inserting a cache filter to allow the required rewinds.
- **Marker:** dirty-side behavioural specification.

## 2. Purpose

Decimation and pattern-based frame selection require reading frames out of strict order (e.g. selecting offsets 1 and 3 of every 4). Since the source filter cannot seek backward, this filter computes the maximum rewind needed by its pattern and installs a cache filter of exactly that size below itself. It then maps output frame indices to source indices and adjusts frame-count/fps/timebase metadata.

## 3. Public interface

Exports `cli_vid_filter_t select_every_filter = { "select_every", help, init, get_frame, release_frame, free_filter, NULL }`.

- `help` prints `select_every:step,offset1[,...]` and a long description referencing AviSynth's `SelectEvery`.
- `init`: parses the comma-separated `step,offsets...` string, validates offsets, computes the required cache size, installs the cache filter by name, updates `video_info_t`, and installs itself.
- `get_frame`/`release_frame`: translate output frame indices to pattern-derived source indices and delegate.
- `free_filter`: frees previous filter (the cache) and pattern storage.

## 4. State and data structures

`selvry_hnd_t` fields: `prev_hnd`, `prev_filter`, `pattern` (array of selected offsets), `pattern_len`, `step_size`, `vfr`, `pts` (running PTS for VFR reconstruction). Constants: `MAX_PATTERN_SIZE = 100`. A local `offsets[100]` array collects offsets during parsing; the final pattern is heap-allocated with the exact length.

## 5. Algorithm / behaviour

**Parsing.** The option string is split on commas with `strtok_r`. The first token is the step size, which must be positive. Remaining tokens are offsets, each required to be ≥ 0 and < step size; at most 100 offsets are accepted. An absent step or absent offsets is an error.

**Cache sizing.** The maximum rewind is computed by scanning the offset list from the end: a running minimum `min` is maintained, and for each adjacent pair the required rewind is `offsets[i-1] - min + 1`; the maximum of these is taken, capped conceptually at the step size (the scan stops early if it equals the step). The required cache size is passed to `x264_init_vid_filter("cache_<bitdepth>", ..., (void*)max_rewind)`, which installs the cache beneath this filter.

**Metadata update.** If the number of selected offsets differs from the step size, the output frame count is `input_frames * pattern_len / step_size` (integer arithmetic), `fps_den *= step_size` and `fps_num *= pattern_len` then reduced. For VFR input, `timebase_den *= pattern_len` and `timebase_num *= step_size` then reduced — this keeps tick values consistent with the new frame rate. If selection keeps all frames, metadata is unchanged. `pts` starts at zero and `vfr` copies `info->vfr`.

**Frame mapping.** For output frame `f`, the source index is `pattern[f % pattern_len] + (f / pattern_len) * step_size`. This yields, for example, with step 4 and offsets {1,3}: f=0→1, f=1→3, f=2→5, f=3→7, etc. `get_frame` delegates with that index; `release_frame` recomputes the same index and delegates the release.

**VFR PTS reconstruction.** When VFR is active, `get_frame` overwrites `output->pts` with the running `pts` and then advances it by `output->duration`. This is needed because the source frames' original PTS would have gaps; the output stream must have continuous timestamps. (Duration preservation is handled by the fix_vfr_pts filter where needed.)

## 6. Dependencies

- `filters/video/video.h`, `filters/video/video.c` (`x264_init_vid_filter`), `filters/filters.h` (`x264_otoi`), `input/input.h` (frame-rate reduction helpers), error macros tagged `"select_every"`.
- Inserts a `cache_<depth>` filter internally, so it depends on `cache.c`.

## 7. Constants and tables

- `MAX_PATTERN_SIZE = 100`.
- Cache filter name built as `"cache_%d"` with the encoder's bit depth.
- Filter name: `"select_every"`.
- No magic numbers beyond the pattern validation rules.

## 8. Performance notes

- Per frame: one integer division/modulo, one delegation, and (VFR) two adds/stores — negligible.
- **Reservation sizing:** the cache is sized exactly to `max_rewind`, not the step size, minimising memory; pattern storage is one small allocation.
- **Zero-copy:** frames are passed through unchanged; no pixel copies.
- **Branch behaviour:** VFR PTS reconstruction is a per-filter constant branch.
- Reference: AviSynth `SelectEvery` documentation (pattern semantics).

## 9. References

- AviSynth `SelectEvery` documentation: <http://avisynth.nl/index.php/Select#SelectEvery>.
- `filters/video/cache.c`, `filters/video/video.c`, `filters/filters.c` (companion specifications).

## 10. Verification checklist

1. With step 2 and offset {0}, confirm every other frame is selected and frame count/fps are halved.
2. With step 4 and offsets {1,3}, confirm output indices map to source indices 1,3,5,7,... and that the inserted cache is large enough.
3. Confirm invalid step (≤0), out-of-range offset, or >100 offsets are rejected.
4. Confirm VFR output PTS are continuous and durations are preserved where available.
5. Confirm no metadata change when all frames are selected.
6. Verify no double release of cached frames.
7. Confirm `timebase` scaling for VFR uses the pattern ratio and is reduced.

## 11. Open questions

- UNKNOWN: exact behaviour when the source frame count is not divisible by the step size — integer division truncates, so trailing partial patterns are dropped; not explicitly documented.
