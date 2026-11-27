# `common/ppc/mc.h` — PowerPC motion-compensation declaration

**Dirty-side behavioural specification.** Source path: `common/ppc/mc.h`. Role: declares the single entry point that installs PowerPC motion-compensation overrides.

## 1. Purpose

The PowerPC back end exposes one initializer so the generic code can install AltiVec motion-compensation routines after CPU detection. The header keeps the architecture-specific symbol out of the generic dispatcher.

## 2. Public interface

- `void x264_mc_init_altivec( x264_mc_functions_t *pf )`

Unlike the ARM counterpart, this function takes only the function-pointer table; it does not take a CPU feature mask. The caller is responsible for checking `X264_CPU_ALTIVEC` before invoking it.

Contract: mutates the caller-owned table in place, replacing selected entries with Altivec implementations. Returns nothing.

## 3. State and data structures

`x264_mc_functions_t` is the generic table of motion-compensation function pointers. The PowerPC initializer overrides luma interpolation, reference retrieval, chroma interpolation, block copy, half-pel filtering, low-resolution preparation, weighted prediction, and several plane-copy helpers. Entries without an Altivec implementation are left unchanged.

## 4. Algorithm / behaviour

The initializer checks the build's bit depth at compile time:

- In 8-bit builds it installs `mc_luma`, `get_ref`, `mc_chroma`, unaligned and aligned 16x16 block copies, half-pel filtering, low-resolution frame preparation, the weighted-prediction table, plane-copy swap/interleave/deinterleave helpers, chroma store/load helpers, and RGB deinterleave when VSX is available.
- In high-bit-depth builds it installs only the v210 plane deinterleave routine.

The routines themselves implement the same algorithms as their ARM counterparts: quarter-pel phase selection via the `x264_hpel_ref0`/`ref1` tables, bilinear chroma interpolation with denominator 64, 5-tap half-pel filtering, weighted prediction with scale/denominator/offset, and byte-plane shuffling for interleaved chroma or RGB input.

## 5. Dependencies

- Implemented in `common/ppc/mc.c`.
- Called from the generic motion-compensation initialization path in `common/mc.c`.
- Uses `x264_mc_functions_t` from `common/mc.h`.

## 6. Constants and tables

- Pixel-size enum values index copy and weight tables.
- `x264_hpel_ref0 = {0,1,1,1,0,1,1,1,2,3,3,3,0,1,1,1}` and `x264_hpel_ref1 = {0,0,1,0,2,2,3,2,2,2,3,2,2,2,3,2}`.
- Half-pel constants 5 and 20, shifts 5 and 6; chroma denominator 64; weighted prediction denominator 64.

## 7. Performance notes

Dispatch runs once per encoder instance. The installed routines are memory-throughput oriented. VSX builds use unaligned vector loads directly; older AltiVec builds rely on the emulated unaligned helpers in `ppccommon.h`, which are slower.

## 8. References

- `common/mc.h` for the function-pointer table.
- `common/ppc/mc.c` for concrete overrides.
- ITU-T H.264 fractional interpolation and weighted prediction.

## 9. Verification checklist

- With 8-bit build settings, all documented entries point to Altivec symbols.
- With high-bit-depth settings, only the v210 routine is installed.
- Installed routines match generic results on representative blocks.
- Repeated calls produce identical tables.

## 10. Open questions

- `UNKNOWN`: exact build-time condition under which this initializer is called versus the generic one.
- `UNKNOWN`: whether the VSX-only RGB deinterleave pointer is left null on non-VSX builds.
