# `common/arm/mc.h` — ARM motion-compensation dispatch declaration

## 1. Header

- **Source path:** `common/arm/mc.h`.
- **Role in one sentence:** declares the one entry point used to install ARM-specific motion-compensation function pointers.
- **Marker:** Dirty-side behavioural specification; all descriptions are behavioural and written independently of the reference source.


## 2. Purpose

The ARM back end exposes exactly one configuration function rather than a long list of individual routines. This header declares that function so that the generic CPU-detection code can call it after determining which ARM features are available. The indirection keeps assembly symbols out of the generic code base.

## 3. Public interface

- `void x264_mc_init_arm( uint32_t cpu, x264_mc_functions_t *pf )`

Contract:
- `cpu` is a bit mask of ARM/NEON capability flags (notably `X264_CPU_ARMV6` and `X264_CPU_NEON`) obtained from CPU detection.
- `pf` points to a caller-owned table of motion-compensation function pointers.
- The function mutates `pf` in place, replacing selected generic pointers with optimized ARM implementations.
- It returns nothing.

The declaration uses `x264_template` to mangle the symbol name appropriately for the build's bit depth.

## 4. State and data structures

`x264_mc_functions_t` is the generic table of function pointers covering luma interpolation, chroma interpolation, block averaging, weighted prediction, plane copies, deinterleaving, integral-image preparation, and MB-tree propagation. The ARM initializer selects which entries are overridden. The ownership of `pf` remains with the caller; the initializer does not allocate or free it.

The ARM initializer is defined in `common/arm/mc-c.c`. Its behaviour is gated on compile-time `HIGH_BIT_DEPTH`: most ARM NEON motion-compensation routines are provided only for 8-bit samples, while the aligned memory helpers are installed unconditionally for NEON CPUs.

## 5. Algorithm / behaviour

Dispatch proceeds in two stages:

1. If `X264_CPU_ARMV6` is not set, the function returns immediately and leaves the table untouched.
2. With ARMv6 present, the source and reference prefetch hooks are installed (for 8-bit builds only). The 4:2:2 source prefetch is deliberately mapped to the same function as 4:2:0 and marked as a known approximation in the reference source.
3. If `X264_CPU_NEON` is not set, the function returns at this point.
4. With NEON present and 8-bit samples, the initializer overwrites the function-pointer entries for unaligned and aligned block copies, plane copies (plain, swapped, interleaved, deinterleaved, and RGB deinterleave), chroma store/load helpers, pixel averaging at all supported widths, weighted-prediction tables and cache selection, luma/chroma interpolation, half-pel filtering, low-resolution frame preparation, integral-image initialization, and MB-tree propagation.
5. Finally, aligned memory helpers are installed for NEON CPUs regardless of bit depth. On macOS the aligned `memcpy` override is deliberately skipped because the platform compiler cannot align stack variables as required.

The function never clears the table; entries for which no ARM routine exists retain whatever value the generic initializer placed there.

## 6. Dependencies

- Called from the generic motion-compensation initialization path in `common/mc.c`.
- Reads CPU capability bits produced by `x264_cpu_detect`.
- Writes into the `x264_mc_functions_t` structure declared in `common/mc.h`.
- The actual routines are in `common/arm/mc-a.S` (assembly) and `common/arm/mc-c.c` (C glue and block-level orchestration).

## 7. Constants and tables

- Capability flags: `X264_CPU_ARMV6`, `X264_CPU_NEON`.
- Pixel-size enum values `PIXEL_4x4`, `PIXEL_8x8`, `PIXEL_16x16` index the copy and average arrays.
- The six-entry `weight` table is indexed by `width >> 2`, so widths 4, 8, 12/16 and 20 select entries. The 12-wide and 16-wide entries share a routine in the NEON implementation.

## 8. Performance notes

Dispatch itself runs once per encoder instance and is not performance-critical. The quality of the selected routines is critical: luma interpolation and pixel averaging dominate motion search. The ARM initializer chooses table entries deliberately; for example, the 12-wide average reuses the 16-wide routine because a separate 12-wide version was not faster. Verification should compare the installed function pointers, not just output, when checking dispatch correctness.

## 9. References

- `common/mc.h` for the function-pointer table definition.
- `common/arm/mc-c.c` for the concrete overrides.
- ARM architecture documentation for `X264_CPU_ARMV6` and `X264_CPU_NEON` semantics.

## 10. Verification checklist

- With `cpu == 0`, `pf` is unchanged.
- With only `X264_CPU_ARMV6`, only the prefetch pointers are changed (8-bit builds).
- With `X264_CPU_NEON`, all documented entries point to ARM-specific symbols.
- With `HIGH_BIT_DEPTH` builds, pixel-path entries remain generic while `memzero_aligned` is still overridden.
- On macOS-style builds, `memcpy_aligned` is not overridden.
- Repeated calls produce identical tables (no accumulated state).

## 11. Open questions

- `UNKNOWN`: exact list of CPU flags required on aarch64, where ARMv6 is implied; the source gates on `X264_CPU_ARMV6` in the 32-bit path and on NEON in another build branch.
- `UNKNOWN`: whether any downstream code reads `pf->prefetch_fenc_422` in practice, given the documented approximation.
