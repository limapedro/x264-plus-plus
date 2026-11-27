# `extras/cl_platform.h` — dirty-side behavioural specification

## 1. Header

- Source path: `extras/cl_platform.h`.
- Role: the platform-abstraction header for the vendored OpenCL API: scalar types, limits, calling conventions, availability/deprecation macros and OpenCL vector types with natural alignment.
- Marker: this file is a **dirty-side behavioural specification**; its layered conditional structure is described in our own words.

## 2. Purpose

OpenCL's C API must present identical type widths and alignments across operating systems and compilers, and must degrade gracefully on platforms where a given API version is unavailable. This header implements that layer so `cl.h` can contain only the portable API declarations. It also defines the vector types that mirror OpenCL C's `char2`, `float4` and similar types on the host.

## 3. Public interface

### Calling conventions and attributes

- `CL_API_ENTRY`, `CL_API_CALL`, `CL_CALLBACK`: empty on non-Windows, with `__stdcall` for calls and callbacks on Windows.
- Availability macros for API versions 1.0–1.2: on macOS they map to `AVAILABLE_MAC_OS_X_VERSION_*` annotations with weak-import support; elsewhere they are empty.
- Deprecation macros for versions 1.0 and 1.1: controlled by `CL_USE_DEPRECATED_OPENCL_1_0_APIS` and `CL_USE_DEPRECATED_OPENCL_1_1_APIS`; otherwise GCC uses `__attribute__((deprecated))` and MSVC uses `__declspec(deprecated)`.
- `CL_EXTENSION_WEAK_LINK` is `__attribute__((weak_import))` on Apple platforms.

### Scalar types

Two branches:

- MSVC on Windows: `cl_char` through `cl_ulong` use `__int8`/`__int16`/`__int32`/`__int64` extensions; `cl_half` is unsigned 16-bit; float and double are native.
- Otherwise: `<stdint.h>` types are used, with explicit alignment attributes on GCC-style compilers for 2-, 4- and 8-byte types.

Limits are defined for all scalar types, including float/double digits, exponent ranges, radix, maximum/minimum values and epsilon. The MSVC branch uses decimal literals for float/double limits; the standards branch uses hexadecimal floating literals where supported.

### GL mirror types

`cl_GLuint`, `cl_GLint`, `cl_GLenum` mirror OpenGL types so the header need not decide which GL library to include.

### Vector types

Host-side vector typedefs are selected by feature macros:

- `__VEC__` (AltiVec): `vector` types for 16-byte char/uchar/short/ushort/int/uint/float vectors.
- `__SSE__`: 4-float vectors, using GCC vector extensions or `__m128`.
- `__SSE2__`: 16-byte integer/double vectors, using GCC vector extensions or `__m128i`/`__m128d`.
- `__MMX__`: 8-byte vectors, using GCC extensions or `__m64`.
- `__AVX__`: 8-float and 4-double vectors, using GCC extensions or `__m256`/`__m256d`.

For each family, union types such as `cl_char2`, `cl_char3`, `cl_char4`, `cl_char8`, `cl_char16` (and the unsigned, short, int, long, float and double families) expose:

- An aligned array member `s[n]`.
- Named fields `.x`, `.y`, `.z`, `.w` and `.s0`…`.sf`, plus `.lo`/`.hi` when compiler extensions allow.
- A native vector member such as `v2` or `v4` when the corresponding feature macro is defined.

Three-component types are typedefs of the four-component type. Alignment is supplied by `CL_ALIGNED`, which is `__attribute__((aligned(x)))` on GCC, neutered on MSVC because it cannot handle aligned function arguments, and replaced with a warning on unknown compilers.

## 4. State and data structures

No runtime state; all definitions are compile-time. The include guard is `__CL_PLATFORM_H`. On macOS the header includes `AvailabilityMacros.h`; on other platforms it does not. `extern "C"` wrapping is applied in C++.

## 5. Algorithm / behaviour

1. Establish calling conventions and availability/deprecation annotations based on OS and compiler.
2. Choose scalar type definitions based on `_WIN32` + `_MSC_VER`, defining both types and numeric limits.
3. Include `<stddef.h>` for `size_t` and define GL mirror types.
4. Probe vector feature macros and define internal vector representations.
5. Define `CL_ALIGNED` and named-vector-field availability macros (`CL_HAS_NAMED_VECTOR_FIELDS`, `CL_HAS_HI_LO_VECTOR_FIELDS`) for GCC extensions outside strict ANSI mode.
6. Generate the full family of host vector union types by combining arrays, named fields and native vectors.
7. Leave OpenCL C's "natural alignment" requirement to the caller: the source explicitly states that maintaining proper alignment is the user's responsibility, and that a `cl_float4` inside a struct is only 16-byte aligned if the struct itself is.

## 6. Dependencies

- **Includes**: `<AvailabilityMacros.h>` on macOS, `<stdint.h>` on non-MSVC platforms, `<stddef.h>`, and SIMD intrinsics headers (`xmmintrin.h`, `emmintrin.h`, `mmintrin.h`, `immintrin.h`, `altivec.h`) depending on enabled features.
- **Included by**: `extras/cl.h.md`, which relies on the scalar types and API-call macros.
- **Consumers**: OpenCL host code that wants to pass vector values as kernel arguments or assemble them into structures.

## 7. Constants and tables

| Group | Meaning |
| --- | --- |
| Calling convention macros | Empty off Windows; `__stdcall` on Windows. |
| Availability macros | Map API versions to OS availability attributes. |
| Deprecation macros | Controlled by two `CL_USE_DEPRECATED_OPENCL_*_APIS` flags. |
| Scalar limits | Character, short, int, long and floating-point ranges/precision. |
| Feature macros | `__VEC__`, `__SSE__`, `__SSE2__`, `__MMX__`, `__AVX__`, `__GNUC__`, `__STRICT_ANSI__`. |
| Alignment macro | `CL_ALIGNED(x)`, with compiler-dependent expansion. |
| Vector families | char, uchar, short, ushort, int, uint, long, ulong, float, double. |
| Vector widths | 2, 3, 4, 8, 16 as available per element type. |

## 8. Performance notes

- Using native SIMD vector representations where available lets host-side vector values be passed as kernel arguments without conversion.
- `CL_ALIGNED` neutering on MSVC can silently remove alignment guarantees; host code must not assume a `cl_float4` argument is 16-byte aligned there.
- The source notes that alignment inside structs is the caller's responsibility; a misaligned struct member defeats the natural-alignment requirement.
- No runtime cost; all types are compile-time.

### 8.1 Relation to faster-kernel development

When tuning OpenCL kernels, host-side vector arguments should match the kernel's expected alignment. Use the scalar and vector types from this header rather than raw C arrays so widths and alignments are explicit. Validate the OpenCL results against a CPU reference as described in `extras/cl.h.md`, and use the assembly conformance suite (`tools/checkasm.c.md`) for the CPU side.

References: OpenCL 1.2 specification §6.1 (scalar and vector types); `extras/cl.h.md`; `doc/standards.txt.md` for the project's general alignment assumptions.

## 9. References

- The source file itself, `extras/cl_platform.h`, with a revision string dated 2010 from the Khronos Group.
- OpenCL 1.2 specification, section on scalar and vector data types and alignment.
- GCC documentation for vector extensions and alignment attributes; MSVC documentation for `__declspec(deprecated)` and calling conventions.
- `extras/cl.h.md` for the API declarations layered on top.

## 10. Verification checklist

- `cl_int` is 32 bits and `cl_long`/`cl_ulong` are 64 bits on both branches.
- `cl_half` is 16 bits.
- Floating-point limits match IEEE single/double expectations.
- `CL_API_CALL` expands to `__stdcall` on Windows and nothing elsewhere.
- Deprecated functions are marked when the opt-in macros are absent and unmarked when present.
- `cl_float4` is 16-byte aligned where the compiler supports alignment attributes.
- Named vector fields (`.x`, `.s0`, `.hi`, `.lo`) are available under GCC outside strict ANSI mode.
- Three-component vector types are aliases of their four-component counterparts.
- Unknown compilers receive a warning about the missing alignment mechanism.

## 11. Open questions

- UNKNOWN: whether the vendored revision includes any later Khronos corrections after 2010; the header carries a single revision string.
- UNKNOWN: which OpenCL vector families are actually used by the encoder's OpenCL kernels.
- UNKNOWN: exact behaviour on compilers that define neither GCC nor MSVC alignment support; the source emits a warning and defines no alignment.
