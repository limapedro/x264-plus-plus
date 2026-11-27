# `extras/inttypes.h` — dirty-side behavioural specification

## 1. Header

- Source path: `extras/inttypes.h`.
- Role: a Microsoft-Visual-C++-compatible implementation of the C99 `<inttypes.h>` format-specifier macros and greatest-width integer conversion helpers.
- Marker: this file is a **dirty-side behavioural specification**; the interface is described in our own words.

## 2. Purpose

C99 defines `printf`/`scanf` format macros such as `PRIu64` so that format strings are portable across platforms where 64-bit integers have different underlying types. MSVC's runtime uses its own length modifiers (`I64`, `I32`) and lacks the standard macros. This header supplies them, together with `imaxabs`, `imaxdiv` and the string-conversion aliases, so shared source can use standard spellings everywhere.

## 3. Public interface

**Preconditions**: the header deliberately emits a preprocessor error if `_MSC_VER` is not defined — it is only for Microsoft Visual C++ compilers. It uses an include guard `_MSC_INTTYPES_H_` and a `#pragma once` for newer compiler versions. It includes the companion `stdint.h` compatibility header.

**Type**: `imaxdiv_t`, a structure with `quot` and `rem` members of type `intmax_t`.

**Format macros** (defined only in C, or in C++ when `__STDC_FORMAT_MACROS` is defined):

- Signed printf macros for 8/16/32/64-bit and least/fast/max/ptr variants: `PRId8`, `PRIi8`, `PRId16`, `PRId32`, `PRId64`, `PRIdMAX`, `PRIdPTR`, etc.
- Unsigned printf macros for octal, decimal, lowercase/uppercase hex across the same families: `PRIo8`, `PRIu8`, `PRIx8`, `PRIX8`, …, `PRIuPTR`, etc.
- Scanf macros: `SCNd16`, `SCNu16`, `SCNx16`, `SCNd32`, `SCNu32`, `SCNd64`, `SCNu64`, and least/fast/max/ptr variants.

**Functions/macros**:

- `imaxabs`, defined as the compiler's 64-bit absolute-value routine.
- `imaxdiv`, implemented in the header (inline or static depending on `STATIC_IMAXDIV`) and returning quotient and remainder.
- `strtoimax` / `strtoumax`, mapped to the compiler's 64-bit string-conversion routines.
- `wcstoimax` / `wcstoumax`, mapped to wide-character variants.

## 4. State and data structures

`imaxdiv_t` is the only data structure: two `intmax_t` fields. The implementation of `imaxdiv` is function-local state only.

Macro families map widths to runtime length modifiers:

| Width | printf modifier | scanf modifier |
| --- | --- | --- |
| 8 | none (`d`, `u`, `x`) | not defined for 8-bit in this header |
| 16 | `h` | `h` |
| 32 | `I32` for printf, `l` for scanf | `l` |
| 64 | `I64` | `I64` |
| max | `I64` | `I64` |
| ptr | `I` / `Io`, `Iu`, `Ix` on printf; `I64` or `l` on scanf depending on `_WIN64` | same |

## 5. Algorithm / behaviour

### 5.1 Format macros

Each macro expands to a string literal containing the appropriate MSVC length modifier and conversion character. For example, the 64-bit decimal printf macro yields the `I64d` form, while the 32-bit scanf decimal macro yields the `ld` form. Pointer-width scanf macros are selected at compile time by checking `_WIN64`.

### 5.2 `imaxdiv`

1. Compute `quot = numer / denom` and `rem = numer % denom` using native 64-bit division.
2. If the numerator is negative and the remainder is positive, adjust: increment the quotient and subtract the denominator from the remainder. This enforces the C rule that the remainder has the same sign as the numerator, because MSVC's native division may truncate differently.
3. Return the structure.

Edge behaviour: division by zero is undefined, as in the C standard. No check is performed.

### 5.3 Linkage control

`imaxdiv` is declared `static` when `STATIC_IMAXDIV` is defined, otherwise `_inline`. This lets a build avoid multiple-definition problems by choosing per-translation-unit linkage.

### 5.4 String conversions

The four conversion functions are pure macro aliases to the MSVC-specific 64-bit entry points. No wrapping or validation is performed.

## 6. Dependencies

- **Includes**: `stdint.h` from the same directory (the compatibility header described in `extras/stdint.h.md`).
- **Included by**: common headers and source files that print 64-bit values, such as the rate-control statistics and benchmark output code.
- **Compiler requirement**: `_MSC_VER` must be defined; otherwise the header is unusable by design.

## 7. Constants and tables

| Macro family | Count | Notes |
| --- | --- | --- |
| `PRId*`, `PRIi*` | signed decimal/hex-like spelling | 8/16/32/64, least, fast, max, ptr. |
| `PRIo*`, `PRIu*`, `PRIx*`, `PRIX*` | unsigned octal/decimal/hex | same families. |
| `SCNd*`, `SCNi*`, `SCNo*`, `SCNu*`, `SCNx*`, `SCNX*` | scanf forms | 16/32/64, least, fast, max, ptr; 8-bit scanf macros absent. |
| `STATIC_IMAXDIV` | build flag | Selects static versus inline linkage for `imaxdiv`. |
| `_WIN64` | platform flag | Selects pointer-width scanf modifiers. |

## 8. Performance notes

All macros expand to string literals at compile time and have no runtime cost. `imaxdiv` performs one division and one remainder; the adjustment branch is rare (only for negative numerators with positive remainder) and cheap.

### 8.1 Relation to faster-kernel development

Benchmark output from the assembly conformance tool prints 64-bit cycle totals using these macros. When adding a new kernel benchmark:

- Use `PRId64`/`PRIu64` rather than a hard-coded format so Windows builds print the full value.
- Keep timing arithmetic in fixed-width types so the printed numbers agree across 32- and 64-bit builds.
- Verify the harness output on the target platform; a truncated cycle count can make a candidate kernel appear faster than it is.

References: C99 §7.8 for the intended macro semantics; `tools/checkasm.c.md` for the benchmark output path.

## 9. References

- The source file itself, `extras/inttypes.h`, attributed to Alexander Chemeris with a BSD-style licence.
- ISO/IEC 9899:1999 (C99), section 7.8, and the WG14 N1124 draft cited in the file.
- Microsoft CRT documentation for `I32`/`I64` format modifiers and `_strtoi64`, `_strtoui64`, `_abs64`.
- `extras/stdint.h.md` for the underlying types.

## 10. Verification checklist

- Including the header with `_MSC_VER` undefined produces the documented preprocessor error.
- `PRIu64` prints the full 64-bit value on MSVC.
- `SCNu64` reads back the same value.
- `imaxdiv` returns a quotient and remainder satisfying `numer = quot * denom + rem` and `rem` having the sign of `numer` for negative numerators.
- Defining `STATIC_IMAXDIV` avoids multiple-definition link errors across translation units.
- `strtoimax` and `strtoumax` accept the same inputs as their native aliases.
- In C++, macros are unavailable unless `__STDC_FORMAT_MACROS` is defined.

## 11. Open questions

- UNKNOWN: why 8-bit scanf macros are omitted while 8-bit printf macros are provided.
- UNKNOWN: whether `imaxdiv` handles `INTMAX_MIN / -1`; native division would overflow, and no guard is present.
- UNKNOWN: whether modern MSVC versions provide native `<inttypes.h>`, making this header obsolete; selection is build-system controlled.
