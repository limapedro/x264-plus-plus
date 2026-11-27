# `common/mips/mc.h` — MSA motion-compensation dispatcher declaration

**Dirty-side behavioural specification.** Source path: `E:\_x264_dirty_stash\x264-source\common\mips\mc.h`. Role: declares the single architecture-initialization entry point for MSA motion compensation and copy/average/filter routines.

## 1. Purpose

Unlike the other MIPS architecture headers, this one exposes exactly one function: `x264_mc_init_mips`. The generic `mc.c` calls this during encoder initialization to populate the large `x264_mc_functions_t` function-pointer table with MSA implementations. Keeping the surface to one symbol avoids leaping implementation helpers into the global namespace.

## 2. Public interface

- `void x264_mc_init_mips(uint32_t cpu, x264_mc_functions_t *pf)`

Name-mangled as `#define x264_mc_init_mips x264_template(mc_init_mips)`. Contract:

- `cpu` is a bitmask of CPU capability flags from `common/cpu.h`; the implementation acts only when `X264_CPU_MSA` is set.
- `pf` points to a caller-owned function-pointer struct; the function overwrites the MSA-supported slots in place and leaves the rest untouched.
- The function is safe to call repeatedly (idempotent per struct).
- There is no return value and no allocation; nothing is retained after return.

## 3. State and data structures

No state of its own. The referenced `x264_mc_functions_t` is the generic motion-compensation vtable declared in `common/mc.h`. Fields relevant to MIPS (installed by `mc-c.c`) include:

- `mc_luma`, `mc_chroma`, `get_ref`
- `avg[PIXEL_*]` (nine block sizes)
- `weight`, `offsetadd`, `offsetsub` (arrays of six weight functions indexed by width code)
- `copy[PIXEL_*]`, `copy_16x16_unaligned`
- `store_interleave_chroma`, `load_deinterleave_chroma_fenc/fdec`
- `plane_copy_interleave`, `plane_copy_deinterleave`, `plane_copy_deinterleave_rgb`
- `hpel_filter`, `memcpy_aligned`, `memzero_aligned`, `frame_init_lowres_core`

The header itself needs only the `x264_mc_functions_t` type in scope (it is included from `common/mc.c`, which already has it).

## 4. Algorithm / behaviour

No computation in the header. The mangling macro is the only preprocessor logic. The MSA implementations themselves cover:

- quarter-pel luma interpolation using three 6-tap filters (horizontal, vertical, and the 6x6 center/DC filter) plus 2-tap averaging for half-pel positions, with the standard `(∑ taps + 32) >> 6` scaling and signed saturation to 7-bit precision where required;
- 1/8-pel chroma bilinear interpolation with `(… + 32) >> 6` and unsigned saturation;
- plane copy, interleave/deinterleave (including RGB/RGBA), frame low-resolution half-filtering, and aligned memory helpers.

All details, including exact pointer arithmetic for the nine block sizes and the weighted-prediction rounding rules, are in `mc-c.c.md`.

## 5. Dependencies

- **Included by:** `common/mips/mc-c.c` and `common/mc.c`.
- **Requires:** `x264_mc_functions_t` (from `common/mc.h`), `x264_template` and `uint32_t` (build config / standard headers).
- No `<msa.h>` or `macros.h` dependency here.

## 6. Constants and tables

None in this header. The block-size enum values (`PIXEL_16x16`, etc.) and CPU flag `X264_CPU_MSA` are referenced only by the implementation.

## 7. Performance notes

No runtime cost. Because the init function is called once, its own branches are irrelevant to steady-state speed. What matters for rebuild is that it installs **every** slot the MSA code implements; missing a slot silently falls back to C and loses most of the SIMD benefit.

## 8. Verification checklist

1. Compile the header alone; confirm no implicit type errors.
2. Call `x264_mc_init_mips(X264_CPU_MSA, &pf)` on a zero-initialized struct and check that the expected non-null slots are filled and others remain null.
3. Call with `cpu = 0` and confirm no slots are modified.
4. Verify symbol name after `x264_template` expansion matches the definition in `mc-c.c`.

## 9. Open questions

- UNKNOWN: whether 10-bit builds expect a no-op stub to exist (the declaration is unconditional, the implementation body is `#if !HIGH_BIT_DEPTH`-guarded); confirm a definition is emitted in high-bit-depth builds to avoid link errors.
