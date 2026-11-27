# `extras/stdint.h` — dirty-side behavioural specification

## 1. Header

- Source path: `extras/stdint.h`.
- Role: a public-domain compatibility implementation of the C99 `<stdint.h>` fixed-width integer types and limit macros for toolchains that lack a native C99 header, notably older Microsoft compilers.
- Marker: this file is a **dirty-side behavioural specification**; the interface and its consequences are described in our own words.

## 2. Purpose

The reference source uses fixed-width integer types throughout (for example 8-bit pixel buffers, 16-bit transform coefficients, 32/64-bit bit counts). When building with a compiler that predates C99 standard-library support, those types would be unavailable. This header supplies them, plus the associated limit and constant macros, so the rest of the code can be written once.

## 3. Public interface

The header defines, subject to its include guard:

**Exact-width types**: `int8_t`, `uint8_t`, `int16_t`, `uint16_t`, `int32_t`, `uint32_t`, `int64_t`, `uint64_t`, mapped to the compiler's native signed/unsigned char, short, int and 64-bit extensions.

**Minimum-width types**: `int_least8_t` … `uint_least64_t`, with identical mappings.

**Fastest minimum-width types**: `int_fast8_t` … `uint_fast64_t`. The source explicitly notes these are not guaranteed to be fastest for all purposes; exact-width types are used for 8- and 16-bit cases.

**Greatest-width types**: `intmax_t`, `uintmax_t`, both 64-bit.

**Pointer-sized limits**: `INTPTR_MIN/MAX`, `UINTPTR_MAX`, `PTRDIFF_MIN/MAX`, `SIZE_MAX`, selected by whether `_WIN64` or `__LP64__` is defined.

**Other limits**: `SIG_ATOMIC_MIN/MAX`, `WCHAR_MIN/MAX`, `WINT_MIN/MAX`.

**Limit macros** for all exact-, least- and fast-width families, with `MIN` values written so that the most-negative value is expressible without implementation-defined narrowing.

**Constant macros**: `INT8_C`, `INT16_C`, `INT32_C`, `INT64_C`, and unsigned equivalents, plus `INTMAX_C` / `UINTMAX_C`. The source includes a quoted rationale explaining that C99 TC1 relaxed the requirement from exact-type constants to promoted-type expressions, which is why a subtraction-of-zero trick is used.

Include behaviour: it defines `__need_wint_t` and `__need_wchar_t` before including `<stddef.h>`, so the size type and wide-character types come from the host while the fixed-width types come from this header. Constant and limit macros are only defined in C++ when the corresponding `__STDC_LIMIT_MACROS` / `__STDC_CONSTANT_MACROS` feature macros are defined.

## 4. State and data structures

No runtime state. All definitions are type aliases or preprocessor macros. The include guard is `_STDINT_H`.

Notably, the typedefs for `intptr_t` and `uintptr_t` are **commented out** in the source; only their limit macros are provided. This is an observable gap: code needing those types must obtain them elsewhere.

## 5. Algorithm / behaviour

1. On inclusion, guard against double definition.
2. Request `wint_t` and `wchar_t` from `<stddef.h>` and include it.
3. Define the exact-width family using the compiler's native 64-bit extension (`__int64` / `unsigned __int64`).
4. Define least-width and fast-width families, deliberately reusing exact-width types for the narrow cases as documented.
5. Leave pointer-sized typedefs commented out, but define pointer-related limits conditionally on 64-bit macros.
6. Define limit macros in two groups: the standard C group (guarded for C++ with `__STDC_LIMIT_MACROS`) and the wchar/wint group.
7. Define constant macros using the trick of subtracting a maximum from itself and adding the literal, yielding a promoted expression of the intended width.
8. End the guard.

Behavioural consequences:

- Two's-complement representation is assumed by the MIN macro expressions; this matches the project-wide assumption documented in `doc/standards.txt.md`.
- `int` is assumed to be at least 32 bits, again matching that document.
- The header is only suitable for compilers whose 64-bit extension is spelled `__int64`; this is a Microsoft-style compatibility header, not a general portable one.

## 6. Dependencies

- **Includes**: `<stddef.h>` with feature macros requesting wide-character types.
- **Included by**: `extras/inttypes.h.md` and, on affected toolchains, the common headers and source files that need fixed-width types.
- **Related**: `doc/standards.txt.md` states the two's-complement and minimum-`int`-width assumptions this header embodies.

## 7. Constants and tables

| Family | Widths | Underlying types |
| --- | --- | --- |
| Exact | 8, 16, 32, 64 | signed/unsigned char, short, int, `__int64`. |
| Least | 8, 16, 32, 64 | Same as exact. |
| Fast | 8, 16, 32, 64 | `char`/`short`/`int`/`__int64` variants; not guaranteed fastest. |
| Greatest | — | `intmax_t` = `int64_t`, `uintmax_t` = `uint64_t`. |
| Pointer limits | 32 or 64 | Chosen by `_WIN64` or `__LP64__`. |
| `SIZE_MAX` | 32 or 64 | Chosen the same way, but only if not already defined. |
| `WCHAR_MIN/MAX`, `WINT_MIN/MAX` | — | Note in source: `wint_t` is unsigned short for MS runtime compatibility. |

## 8. Performance notes

No runtime cost; all definitions are compile-time. The use of exact-width types for narrow values helps SIMD code because it makes array layouts predictable.

### 8.1 Relation to faster-kernel development

SIMD kernels frequently rely on exact element widths for aligned loads and on limit macros for saturation constants. When adding a kernel:

- Use the fixed-width types rather than bare `int` where width matters, so 32-bit and 64-bit builds agree.
- Derive saturation constants from the limit macros rather than hard-coding decimal values, so a 10-bit build and an 8-bit build stay consistent.
- Validate width assumptions with the conformance tool (`tools/checkasm.c.md`), which builds at the configured bit depth and exercises overflow cases.

References: C99 §7.18 for the intended semantics; `doc/standards.txt.md` for the project's representation assumptions.

## 9. References

- The source file itself, `extras/stdint.h`, marked public domain and attributed to Danny Smith.
- ISO/IEC 9899:1999 (C99), section 7.18, and TC1 clarification about integer constant expressions.
- `extras/inttypes.h.md`, which builds on these types.
- `doc/standards.txt.md` for the two's-complement and 32-bit-`int` assumptions.

## 10. Verification checklist

- `sizeof(int8_t) == 1`, `sizeof(int16_t) == 2`, `sizeof(int32_t) <= 4`, `sizeof(int64_t) == 8` on the target.
- Unsigned types have the corresponding widths and wrap modulo 2ⁿ.
- `INT64_MIN` expands without triggering an integer-overflow diagnostic.
- `INT32_C(1)` and `UINT64_C(1)` expand to promoted expressions of the expected width.
- On a 64-bit build, `SIZE_MAX` and pointer limits are 64-bit; on 32-bit they are 32-bit.
- In C++, limit macros require `__STDC_LIMIT_MACROS` and constant macros require `__STDC_CONSTANT_MACROS` as documented.
- Attempting to use `intptr_t` from this header fails, confirming the commented-out typedefs.

## 11. Open questions

- UNKNOWN: why pointer typedefs are commented out; the source gives no reason.
- UNKNOWN: whether modern builds still use this header or the compiler's native one; selection is in the build system.
- UNKNOWN: behaviour with compilers where `wchar_t` is not unsigned short; the source notes the MS runtime assumption without handling alternatives.
