# filters/filters.h — common filter option-parsing interface

*Dirty-side behavioural specification. Reference source: `filters/filters.h`. Behavioural descriptions only; no source or near-verbatim pseudocode is reproduced.*

## 1. Header

- **Source path:** `filters/filters.h`
- **Role:** Declares the small library of text-option parsing helpers shared by the video filters and the CLI frontend.
- **Marker:** dirty-side behavioural specification.

## 2. Purpose

Video filters are configured with compact strings such as `crop=left=0:top=0:right=0:bottom=0` or `select_every=2,0`. This header declares helpers that turn those strings into a normalised name/value array and convert values to booleans, integers, floats, and strings with defaults. Centralising parsing keeps every filter's `init` consistent about accepting named or positional options and about invalid input.

## 3. Public interface

Declarations:

- `char **x264_split_options(const char *opt_str, const char * const *options)` — parse an option string into a flat array of alternating names and values, terminated by a NULL pointer pair. `options` is a NULL-terminated array of positional names.
- `char *x264_get_option(const char *name, char **split_options)` — return the value most recently associated with `name`, or NULL if absent/empty.
- `int x264_otob(const char *str, int def)` — string to boolean.
- `double x264_otof(const char *str, double def)` — string to double.
- `int x264_otoi(const char *str, int def)` — string to integer (base auto-detected).
- `char *x264_otos(char *str, char *def)` — string or default pointer.

The header also includes `filters/video/video.h`, so including it brings the whole video-filter interface into scope.

## 4. State and data structures

No global state. `x264_split_options` returns a single heap allocation containing both the pointer array and the duplicated strings; callers must free the returned array with a single `free`. The array layout is `[name0, value0, name1, value1, ..., NULL, ...]`; the implementation reserves two pointer slots per option plus a terminator.

## 5. Algorithm / behaviour

The header itself defines no algorithm; contracts are:

- `x264_split_options` accepts a mix of positional values and `name=value` pairs separated by commas (and internally treats `=` as a separator). Positional values are assigned to `options[i]` in order. Once a named option appears, a later positional value is an error. Unknown names are an error. Too many positional values are an error. A NULL `opt_str` returns NULL.
- `x264_get_option` scans the flat array in steps of two and remembers the last matching name; an empty value string is treated as absent.
- Conversion helpers fall back to the supplied default when the input is NULL or not fully parseable (no trailing garbage; integer parsing uses base auto-detection so `0x` prefixes work). Boolean accepts case-insensitive `true`/`yes` and exact `1` as true; everything else (including NULL) yields the default.

## 6. Dependencies

- `x264cli.h` for base types and error macros; `filters/video/video.h` (included).
- Implemented by `filters/filters.c`; used by `crop.c`, `depth.c`, `resize.c`, and potentially the frontend.

## 7. Constants and tables

- No numeric constants in this header.
- Positional option names are supplied per filter by the caller (e.g. `crop`'s `left/top/right/bottom`).

## 8. Performance notes

- Called only during filter initialisation; no hot-path impact.
- **Reservation sizing:** the parser computes the exact allocation size in one pass before allocating, so there is no reallocation; a clean-room implementation should do the same.
- No SIMD or threading relevance.

## 9. References

- `filters/filters.c`, `filters/video/video.h` (companion specifications).

## 10. Verification checklist

1. Confirm mixed positional/named input is normalised into alternating name/value pairs.
2. Confirm a positional option after a named one is rejected.
3. Confirm unknown names and excess positional values are rejected.
4. Confirm `x264_get_option` returns the last duplicate value and treats an empty value as absent.
5. Confirm conversion helpers respect defaults on malformed input and accept `0x` integers.
6. Confirm a NULL option string returns NULL rather than crashing.

## 11. Open questions

- UNKNOWN: whether the separator set includes characters beyond `=` and `,` in practice — the implementation uses only those two.
