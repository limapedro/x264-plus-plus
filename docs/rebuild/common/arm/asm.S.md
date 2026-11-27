# `common/arm/asm.S` — ARM assembly utility macros

**Dirty-side behavioural specification.** Source path: `common/arm/asm.S`. Role: supplies the symbol-naming, section-layout, relocation, and SIMD-helper macros used by every ARM assembly file in the back end.

## 1. Purpose

ARM assembly in x264 must build for ELF and Mach-O, for 32-bit ARMv6/v7 and for high bit-depth variants, with or without position-independent code and with several assembler dialects. This file centralizes those differences behind a small set of macros so the individual kernel files contain only algorithm code.

## 2. Public interface

The file exports no runtime symbols. It defines assembly-time macros:

- Symbol naming: `GLUE`, `JOIN`, `BASE`, `SYM_PREFIX`, `EXTERN_ASM`, `X(s)`, `X264(s)`, `EXT(s)`.
- Format selection: `ELF`, `MACH`, `NONMACH`, `FUNC`, `HAVE_SECTION_DATA_REL_RO`.
- EABI annotations: `require8`, `preserve8`.
- Code/section helpers: `function name[, export=1]`, with a matching auto-defined `endfunc`; `const name[, align=2][, relocate=0]`, with `endconst`.
- Relocation helpers: `movrel`, `movrelx`, `movconst`.
- SIMD helpers: `HORIZ_ADD`, `SUMSUB_AB`, `SUMSUB_ABCD`, `ABS2`, `HADAMARD`, `TRANSPOSE8x8`, `TRANSPOSE4x4`, `TRANSPOSE4x4_16`.
- Constants: `FENC_STRIDE = 16`, `FDEC_STRIDE = 32`.

## 3. State and data structures

All state is in the assembler's macro namespace. The naming layer computes an external symbol prefix: with `PREFIX` defined, exported names gain a leading underscore and the internal base is `_x264_`; otherwise the base is `x264_`. For high-bit-depth builds the bit depth is inserted into the external name. Internal `X264(...)` references always use the unqualified `x264_` base.

## 4. Algorithm / behaviour

**Function declaration.** `function name` switches to `.text`, aligns to 4 bytes, declares the symbol global and hidden (for exported functions), emits ELF function type and `.func` annotations when supported, and defines a nested `endfunc` macro that emits the matching size annotation and cleans itself up. Nonexported functions are local but still get type/size metadata.

**Constant declaration.** `const` selects `.data.rel.ro` for relocatable read-only data when the platform supports it, otherwise `.rodata` (or Mach-O's `.const_data`), aligns to the requested power of two, and defines a paired `endconst`.

**Relocation.** `movrel` loads a symbol address into a register. Under PIC it emits a PC-relative literal load and a `pc`-relative add; on ARMv6T2 and later it can use `movw`/`movt`; otherwise it uses the assembler's literal-pool `ldr =symbol`. `movrelx` adds GOT-based handling for ELF PIC and Mach-O indirect symbol pointers. `movconst` loads a 32-bit constant with `movw`/`movt` when available and a literal load otherwise.

**EABI attributes.** `require8` and `preserve8` emit EABI attributes 24 and 25, used to tell the linker the stack alignment requirements of NEON code.

**SIMD helpers.** `HORIZ_ADD` reduces a vector of 16-bit lanes to a scalar by pairwise widening adds. `SUMSUB_AB` computes element-wise sum and difference of two 16-bit vectors. `HADAMARD` performs one butterfly stage: with distance 0 it transposes 32-bit lanes, with distance 1 it transposes 16-bit lanes; the `sumsub` variant computes sum/difference, and the `amax` variant computes maxima of absolute values. The transpose macros implement 8x8 and 4x4 byte/halfword/word transposes using sequences of `vtrn` instructions.

## 5. Dependencies

Included at the top of every ARM `.S` file. Depends on `config.h` for feature macros such as `HAVE_AS_FUNC`, `HAVE_ARMV6T2`, `PIC`, `SYS_LINUX`, `SYS_OPENBSD`, `SYS_MACOSX`, `BIT_DEPTH`, and `PREFIX`.

## 6. Constants and tables

- `FENC_STRIDE = 16` and `FDEC_STRIDE = 32` are the fixed strides for source and reconstructed pixel buffers.
- EABI attribute numbers 24 (required stack alignment) and 25 (preserved stack alignment).
- `HAVE_SECTION_DATA_REL_RO` is 1 on Linux and OpenBSD, 0 elsewhere.

## 7. Performance notes

This file generates no executed code beyond the macros it expands. The transpose macros matter for performance: an 8x8 transpose is implemented with three stages of `vtrn` at 32-, 16-, and 8-bit granularity, which is substantially cheaper than scalar transposition. The `HADAMARD` macro's `amax` variant is the key trick used by the SATD code to compute sums of absolute values with reductions. Rebuilders should preserve these identities even if they use different instruction sequences.

## 8. References

- ARM Architecture Reference Manual and ARM EABI documentation.
- GNU assembler manual for `.func`, `.size`, `.eabi_attribute`, and Mach-O section directives.
- ARM NEON Programmer's Guide for `vtrn`, `vpaddl`, and widening arithmetic.

## 9. Verification checklist

- Every kernel file assembles for ELF and Mach-O targets with and without PIC.
- Exported symbols have exactly the names expected by the C headers in 8-bit and high-bit-depth builds.
- `function`/`endfunc` pairs emit balanced `.func`/`.endfunc` and size metadata.
- `movconst` handles constants that fit in 16 bits and constants that need 32 bits.
- SIMD helper macros produce the documented arithmetic identities for representative 16-bit inputs.

## 10. Open questions

- `UNKNOWN`: exact behaviour on Thumb-only targets, where the reference source contains a `FIXME: thumb` comment for PIC literal loading.
- `UNKNOWN`: whether all listed feature macros are defined in every supported build system.
