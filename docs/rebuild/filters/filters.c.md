# filters/filters.c — filter option parsing implementation

*Dirty-side behavioural specification. Reference source: `filters/filters.c`. Behavioural descriptions only; no source or near-verbatim pseudocode is reproduced.*

## 1. Header

- **Source path:** `filters/filters.c`
- **Role:** Implements the shared option-string parser and scalar converters declared in `filters/filters.h`.
- **Marker:** dirty-side behavioural specification.

## 2. Purpose

All video filters share one grammar for their option strings. This file implements that grammar in a single pass: it validates the mix of positional and named options, computes the exact memory needed, then fills a name/value array. The converters provide consistent fallback behaviour so each filter's `init` can simply ask for a value and a default.

## 3. Public interface

Implements `x264_split_options`, `x264_get_option`, `x264_otob`, `x264_otof`, `x264_otoi`, `x264_otos` (see `filters/filters.h` spec for signatures). Return conventions: parser returns NULL on error (logging with tag `"options"`); converters return the supplied default on unusable input.

## 4. State and data structures

The returned flat array is a single allocation: first `2*(opt_count+1)` pointers, then the duplicated strings back-to-back with NUL terminators. A local macro (conceptually "insert option") appends a string copy at the current offset, stores the pointer in the next array slot, and advances both the offset and the source pointer. Invariants after parsing: even-indexed slots hold option names, odd-indexed slots hold values, and slot `2*opt_count` is NULL.

## 5. Algorithm / behaviour

**Validation/sizing pass.** If `opt_str` is NULL, return NULL. Count the caller's positional option names until the NULL terminator. Then walk the string:

- Find the length of the current token up to `=` or `,`.
- If the token is followed by `=`, it is a named option: search the positional name list for an exact length-and-content match; if none matches, error "Invalid option". Set a "found named" flag and advance past the `=` to include the value up to the next comma.
- Otherwise it is a positional value: error if more values than available names, or if a named option has already been seen ("Ordered option given after named"). Account for the length of the corresponding name in the eventual allocation.
- Advance past the separator; repeat while characters remain.

The pass accumulates the total string bytes needed. After the pass, allocate `2*(count+1)` pointers plus the string bytes plus the original string length (a conservative bound).

**Fill pass.** Walk the string again, now inserting pairs:

- For a named token, insert the token text itself as the name, then insert the value substring up to the comma.
- For a positional token, insert the corresponding name from the caller's list, then insert the token text as the value.

The final offset must equal the computed allocation size (an assertion in the reference). The array is returned.

**Lookups and converters.** `x264_get_option` walks the array in steps of two, remembering the last index whose name matches, and returns its value only if non-empty; otherwise NULL. `x264_otob` returns true for case-insensitive `true`, exact `1`, or case-insensitive `yes`. `x264_otof`/`x264_otoi` use `strtod`/`strtol` (base 0) and fall back to the default if no characters were consumed or trailing characters remain. `x264_otos` returns the string pointer or the default.

## 6. Dependencies

- `filters/filters.h`, `x264cli.h` (`RETURN_IF_ERR`, logging), standard C string/stdlib functions.
- Used by `crop.c`, `depth.c`, `resize.c`; potentially by frontend option handling.

## 7. Constants and tables

- Separators: `=` (name/value) and `,` (option separator).
- True spellings: `true`, `1`, `yes` (case-insensitive for the words).
- Error tag: `"options"`.

## 8. Performance notes

- Init-time only; the two-pass design makes allocation exact and avoids realloc.
- **Reservation sizing:** the conservative allocation bound includes the original string length, so even unusual token boundaries cannot overflow.
- No hot-path or SIMD concern.

## 9. References

- `filters/filters.h`, `filters/video/crop.c`, `filters/video/resize.c`, `filters/video/depth.c` (companion specifications).

## 10. Verification checklist

1. Parse `left=1,right=2,top=3,bottom=4` with the crop option list and verify four pairs.
2. Parse `1,2,3,4` positionally and verify names are filled from the list.
3. Confirm mixing orders is rejected with the correct message.
4. Confirm an unknown name is rejected.
5. Verify duplicated names return the last value via `x264_get_option`.
6. Verify all converters on valid and malformed inputs (including hexadecimal integers and trailing garbage).

## 11. Open questions

- UNKNOWN: exact behavior when an option value itself contains an `=` — the first `=` delimits, so the rest becomes part of the value; not documented as supported.
