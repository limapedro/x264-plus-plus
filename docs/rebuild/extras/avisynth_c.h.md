# `extras/avisynth_c.h` — dirty-side behavioural specification

## 1. Header

- Source path: `extras/avisynth_c.h`.
- Role: a self-contained C-language interface to the AviSynth / AviSynth+ script environment, allowing a plugin or host to create clips, request frames, read plane pointers, register functions and access frame properties without C++ headers.
- Marker: this file is a **dirty-side behavioural specification**; the extensive API surface is summarised in our own words.

## 2. Purpose

AviSynth's native interface is C++, which cannot be used directly from a C program or from compilers without compatible C++ ABI support. This header declares a C ABI with opaque handles and function tables, plus inline convenience helpers. It supports classic AviSynth and the extended AviSynth+ interface (up to interface version 9 in this file), with optional fallback simulation when newer entry points are unavailable.

In the encoder tree it supports the AviSynth input module, allowing the CLI to read frames from an AviSynth script rather than a raw YUV file.

## 3. Public interface

### Platform configuration

- Calling convention macros select `__stdcall` on Windows by default; `AVSC_USE_STDCALL` can be undefined for cdecl.
- Architecture macros identify x86-32, x86-64, ARM32/64, PowerPC, RISC-V and SPARC.
- Compiler macros distinguish MSVC, clang-cl and GCC; OS macros distinguish Windows, Linux, BSD, macOS and Haiku.
- `AVS_FRAME_ALIGN` is 64, with an explicit warning that plugins should request their exact required alignment rather than relying on this value.
- Endianness is reported as a string, with MSVC assumed little-endian.

### Core opaque types

- `AVS_Clip`, `AVS_ScriptEnvironment`, `AVS_Library`: opaque handles.
- `AVS_VideoInfo`: layout-compatible with AviSynth's `VideoInfo`; fields include width/height, frame-rate numerator/denominator, frame count, pixel type, audio sample rate/type/count/channels and image type properties. Width zero means no video; audio rate zero means no audio.
- `AVS_VideoFrameBuffer`: reference-counted memory block with data pointer, size, a volatile sequence number, a volatile reference count and a device pointer. Buffers are recycled rather than freed when the count reaches zero.
- `AVS_VideoFrame`: a window into a buffer, with reference count, buffer pointer, luma/chroma offsets and pitches, row sizes and heights, an alpha-plane offset/pitch/row size, and a frame-properties pointer.
- `AVS_Map`: opaque placeholder for frame-property maps.
- `AVS_Value`: a tagged union ("fat pointer") whose type byte encodes array, clip, bool, int, float, string, void, long, function or error. It has an array size and a union payload. On x86-64 the payload also carries 64-bit long and double members.
- `AVS_FilterInfo`: describes a filter instance, with child clip, video info, environment, callbacks for get_frame/get_parity/get_audio/set_cache_hints/free_filter, an error string and user data.
- `AVS_Library`: a table of resolved function pointers for dynamic loading.

### Representative API groups

- **Video info queries**: presence of video/audio, colour-space family checks (RGB, YUV, YUY2, YV12/16/24, Y8, 444/422/420, planar RGB/RGBA, YUVA), subsampling queries, bits per pixel/component, row size, bitmap size, pixel/audio conversions, frame-rate setter with GCD reduction, same-colorspace test, property setters and field-parity helpers.
- **Frame access**: pitch/row size/height per plane, read and write pointers, writability query, frame copy/release, subframes (including planar and alpha variants), frame creation with properties, writable-frame acquisition, reference counting.
- **Value handling**: copy/release, clip extraction/injection, type predicates, accessors, constructors and array helpers. `avs_void` is a static undefined value; initialized values should be reset to it after release.
- **Clip handling**: copy/release, error query, video info query, version, frame/parity/audio access, cache hints.
- **Filter creation**: `avs_new_c_filter` with a `store_child` flag; when false all callbacks must be supplied, when true child lifetime is managed.
- **Environment**: CPU feature flags, variable get/set, script invocation, function registration, sprintf/vsprintf, global variables, memory limits, working directory, at-exit callbacks, version checks, environment properties (device count, frame/plane alignment, thread suppression, recursive frame access), typed variable getters and pooled allocation.
- **Frame properties (interface V8+)**: map copy, read-only/read-write map access, key enumeration, element counts and types, typed getters/setters for int/float/data/clip/frame and arrays, key deletion and map clearing.
- **Dynamic loading**: `avs_load_library()` allocates an `AVS_Library`, loads `avisynth`, resolves every entry point by name and returns the table or null on any failure; `avs_free_library()` unloads and frees it.

## 4. State and data structures

- Reference counts in frame and buffer structures are volatile, indicating cross-thread use.
- Sequence numbers let stale frame views detect that a buffer changed.
- `AVS_Value` must be treated like a pointer: copy before storing, release after use; storing the union fields directly is explicitly discouraged.
- `AVS_FilterInfo.error` is cleared before callbacks and set by the filter when an error occurs.
- The library table is heap-allocated; any missing function pointer aborts loading unless fallback simulation is enabled.

## 5. Algorithm / behaviour

### 5.1 Colour-space encoding

Colour-space constants are bit fields: planar/interleaved, YUV/BGR/YUVA family, sample-bit width (8/10/12/14/16/32), chroma subsampling widths and heights, and plane order (V-first or U-first). Generic families are combined with sample-bit constants to produce named formats such as YUV420P10, RGBP16 or YUVA422. Helpers test properties by masking and comparing against a filtered template that ignores plane-order bits where appropriate.

### 5.2 Video-info helpers

- `avs_set_fps` reduces the supplied fraction by computing the greatest common divisor with Euclid's algorithm before storing it.
- `avs_audio_samples_from_frames` scales a frame count by the audio rate and the frame-rate denominator divided by the numerator; the inverse helper scales samples to frames.
- `avs_bytes_per_channel_sample` maps sample-type codes to 1, 2, 3, 4 or 4 bytes.
- `avs_is_same_colorspace` compares pixel types verbatim, with a special allowance that both YV12-class formats compare equal.

### 5.3 Dynamic loading and fallbacks

- With `AVSC_NO_DECLSPEC`, inline helpers that call API functions are guarded out, and the library table is intended to be filled by `avs_load_library`.
- With `AVS26_FALLBACK_SIMULATION`, missing AviSynth+ entry points are substituted: format predicates that have no classic equivalent return false, 444/422/420/Y predicates fall back to the older YV24/YV16/YV12/Y8 predicates, component size falls back to 1 byte, bits per component to 8, and component count uses a small heuristic (unknown → 0, single-plane formats → 1, BGR32 → 4, otherwise 3).
- `avs_load_library` resolves every classic entry first, then AviSynth+ entries with or without fallback, then V8/V9 entries with no fallback; any unresolved required entry jumps to a failure path that frees the table and returns null.

### 5.4 Calling conventions

On POSIX, `__declspec` is defined away (except on Haiku) and `EXTERN_C` expands appropriately. On Windows, dllexport/dllimport attributes depend on whether the core is being built and on compiler family.

## 6. Dependencies

- **Includes**: `<stdint.h>`, platform headers, optionally `<malloc.h>`-style declarations for the library loader (the source documents expected `malloc`, `free`, `LoadLibraryA`, `GetProcAddress`, `FreeLibrary`).
- **Consumers**: the AviSynth input module of the encoder CLI and any C plugin.
- **Related**: `extras/stdint.h.md` provides fixed-width types on toolchains without C99 headers.

## 7. Constants and tables

| Group | Examples | Meaning |
| --- | --- | --- |
| Interface versions | classic 6, current 9, bugfix 0 | Compatibility negotiation. |
| Sample types | int8/16/24/32, float | Audio sample encodings. |
| Plane masks | Y/U/V/A/R/G/B and aligned variants | Plane identification in accessors. |
| Colour-space flags | planar, interleaved, YUV, BGR, YUVA | Family bits. |
| Subsampling codes | width 1/2/4, height 1/2/4 | Chroma geometry. |
| Sample-bit codes | 8/10/12/14/16/32 | Component width. |
| CPU flags | MMX through AVX2/FMA and vendor-specific bits | Feature reporting. |
| Environment properties | device count, frame/plane alignment, thread suppression, recursive frame access | System queries. |
| Alpha/property extensions | V8/V9 entry points | Frame properties and writability. |

## 8. Performance notes

- Inline helpers avoid a function call for simple field access; API calls go through the library table when dynamically loaded.
- Frame buffers are reference-counted and recycled, reducing allocation churn in streaming playback.
- Plane access via `avs_get_read_ptr_p` / `avs_get_write_ptr_p` returns direct pointers, so zero-copy handoff to an encoder is possible when alignment permits.
- `AVS_FRAME_ALIGN` is deliberately large; plugins should request only the alignment their kernels need to avoid unnecessary padding.

### 8.1 Relation to faster-kernel development

When an input path feeds SIMD kernels, plane pointers must satisfy the kernels' alignment assumptions. The header explicitly warns against assuming `AVS_FRAME_ALIGN`; check the actual pitch and pointer alignment at runtime and fall back to unaligned kernels if necessary. Validate the SIMD kernels with `tools/checkasm.c.md` and the end-to-end reconstruction test in `doc/regression_test.txt.md` before trusting speed measurements from AviSynth input.

References: AviSynth+ C API documentation; `doc/standards.txt.md` for alignment assumptions; `tools/checkasm.c.md` for the conformance suite.

## 9. References

- The source file itself, `extras/avisynth_c.h`, carrying AviSynth C interface version 0.20 and later AviSynth+ updates.
- AviSynth+ documentation for interface versions 6–9 and frame properties.
- ITU-T Rec. H.262 / ISO/IEC 13818-2 for MPEG-2 metadata values mentioned in the VUI guide.
- `extras/stdint.h.md` for the fixed-width types used by the header.

## 10. Verification checklist

- A C translation unit can include the header without any C++ headers.
- All opaque handle types are usable as pointers without inspecting their internals.
- `AVS_VideoInfo` field accessors correctly report video/audio presence and colour families.
- `avs_set_fps` reduces the fraction and preserves the ratio.
- `avs_void` is recognized as undefined by `avs_defined`.
- Clips and frames can be copied and released without leaks or double frees.
- `avs_load_library` returns null when any required entry point is missing.
- With fallback simulation enabled, missing AviSynth+ functions are replaced and classic-mode queries still work.
- Frame property getters/setters round-trip int, float, data, clip and frame values.

## 11. Open questions

- UNKNOWN: which interface version features the encoder's AviSynth input module actually uses; the header supports far more than one module may need.
- UNKNOWN: exact semantic differences between V8 and V9 property-writability functions; the header only declares them.
- UNKNOWN: whether dynamic loading is used on non-Windows platforms; the loader section is guarded by Windows-specific macros.
