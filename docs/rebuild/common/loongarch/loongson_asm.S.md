# `common/loongarch/loongson_asm.S` — LoongArch LSX/LASX assembly macro library

**Dirty-side behavioural specification.** Source path: `E:\_x264_dirty_stash\x264-source\common\loongarch\loongson_asm.S`. Role: third-party (Loongson, ISC-licensed) assembly helper library providing register aliases, function/constant definition macros, LSX/LASX dot-product, clip, sign-flip, strided-load and transpose macros used by all x264 LoongArch SIMD sources.

## 1. Purpose

Writing LoongArch SIMD by hand with raw instruction mnemonics is verbose and error-prone, especially for the two vector ISAs (LSX: 128-bit `$vr*`; LASX: 256-bit `$xr*`). This library, version 0.4.0, defines a consistent set of aliases and high-level macros so the algorithm files can express operations like "dot-product two byte vectors into halfwords" or "transpose an 8x8 halfword block" without spelling out every shuffle. It is licensed separately (ISC) from x264's GPL code and is vendored here as-is.

## 2. Public interface

Macro-only file; no runtime functions. Include guard: none (it is an `.S` file included with `#include`, relying on the assembler's one-time inclusion via the including files). Defines:

**Version / configuration**
- `LML_VERSION_MAJOR/MINOR/MICRO = 0/4/0`, `ASM_PREF` (empty by default), `DEFAULT_ALIGN = 5`.

**Function/constant definition macros**
- `function name, align=DEFAULT_ALIGN` / `endfunc` — define an aligned global `@function` symbol (prefixed by `ASM_PREF`) with ELF size tracking and `jirl $r0,$r1,0` return.
- `const name, align=DEFAULT_ALIGN` / `endconst` — define a local read-only data object (`.section .rodata`, aligned) with size tracking.

**Register aliases**
- GPR: `a0..a7`, `t0..t8`, `s0..s8`, `zero`, `sp`, `ra`.
- Floating-point: `fa0..fa7`, `ft0..ft15`, `fs0..fs7`, `f0..f31`.
- Vector LSX: `vr0..vr31`; vector LASX: `xr0..xr31`.

**Dot-product macros** (`vdp2.*`, `xvdp2.*`, `vdp2add.*`, `xvdp2add.*`) — compressed forms of the LSX/LASX `dp2`/`dp2add` instructions for byte→halfword and halfword→word pairwise products. Variants with a trailing `.b` produce a boolean result (all bits set) for comparison-style tests.

**Clip/saturate macros** (`vclip.h/w`, `xvclip.h/w`, `vclip255.h/w`, `xvclip255.h/w`) — clamp a vector between two bounds (`vj`, `vk` operands) or to `[0,255]` with a single instruction sequence.

**Element-store macros** (`vstelmx.b/h/w/d`, `xvstelmx.d`) — store one vector element to memory via a GPR index, used when a lane must be written to a strided scalar location.

**Move macros** (`vmov`, `xmov`) — convenient vector-to-vector moves (the underlying ISA uses separate `v*` / `xv*` opcodes).

**Strided load macros**
- `FLDS_LOADX_4`, `FLDD_LOADX_4`, `LSX_LOADX_4`, `LASX_LOADX_4` — load four vectors from a base pointer with one, two or three stride registers (`stride`, `stride2`, `stride3`), producing `out0..out3`. The `FLD*` variants use 32-bit/64-bit floating-point loads widened into vectors; the `LSX`/`LASX` variants use full-width vector loads.

**Transpose macros**
- `LSX_TRANSPOSE4x4_H`, `LSX_TRANSPOSE8x8_H` — halfword (16-bit) transposes of 4x4 and 8x8 blocks using `vilvl.h`/`vilvh.h`, `vilvl.w`/`vilvh.w`, `vilvl.d`/`vilvh.d`.
- `LASX_TRANSPOSE16X8_B` — byte transpose of a 16x8 block into 8 vectors.
- `LASX_TRANSPOSE4x4_H`, `LASX_TRANSPOSE4x8_H`, `LASX_TRANSPOSE8x8_H` — halfword transposes of 4x4, 4x8 and 8x8 blocks with 256-bit vectors.
- `LASX_TRANSPOSE2x4x4_H` — transpose of a 2x4x4 halfword arrangement (used by the 8x8 DCT).
- `LASX_TRANSPOSE4x4_W`, `LASX_TRANSPOSE8x8_W` — 32-bit-word transposes.
- `LASX_TRANSPOSE4x4_D` — 64-bit doubleword transpose.
- Each macro has scratch-vector parameters (`tmp0`, `tmp1`, ...) so callers control register allocation.

## 3. State and data structures

No runtime state. Macros expand inline; the transpose macros declare no local labels and use only their explicit output/scratch operands. The register aliases are simple preprocessor renames (`#define a0 $a0`, etc.) so mixing them with raw `$r*` names is safe.

## 4. Algorithm / behaviour

No algorithm of its own. Semantics of the key macro families:

- **Dot-product (`vdp2.h.bu`):** interprets each byte operand as unsigned (`bu` suffix), multiplies adjacent byte pairs and adds each pair into a halfword lane. `dp2add` variants accumulate into an existing vector. The `.b` boolean variants produce an all-ones mask when the result condition holds — useful for SAD/SSE predicate accumulation.
- **Clip:** `vclip.h vd, vj, vk, va` computes elementwise `max(va, min(vj, vd))` ... more precisely it clamps `vd` to the inclusive range `[vj, vk]` using `va` as a scratch; `clip255` hardcodes `[0,255]`.
- **Strided loads:** emit a base load followed by `vldx`/`xvldx` with index registers, advancing the base by the stride operand(s). The four-vector forms load rows `base + k*stride`.
- **Transposes:** all are butterfly networks of interleave-left/interleave-right (`vilvl`/`vilvh`, `xvilvl`/`xvilvh`) at successively wider element sizes (byte → halfword → word → doubleword). For example, an 8x8 halfword transpose first interleaves row pairs into 16-bit lane pairs, then word pairs, then doublewords, producing eight vectors that each hold one column of the original block. Output ordering is column-major in vector-index order.

The function/const macros use `.align` (log base 2) with default 5 (32-byte alignment), `.globl`, `.type @function`, `.size`, and `.purgem` for correct ELF metadata.

## 5. Dependencies

- **Included by:** all nine LoongArch `.S` files, before `loongson_util.S`.
- **Requires:** GNU assembler macro syntax and LoongArch LSX/LASX instruction support in the toolchain.
- **No** C or x264-header dependency.

## 6. Constants and tables

- Version triple `0.4.0`.
- `DEFAULT_ALIGN = 5` (32-byte alignment).
- `ASM_PREF` empty (overridden indirectly by `loongson_util.S`'s `ASM_REF` for x264 symbols).
- Clip bound 255 in the `clip255` macros.

## 7. Performance notes

No runtime cost; the macros map nearly one-to-one to instructions. Optimization tips for users of the library:

- The transpose macros need multiple scratch registers; choose them to avoid spilling (the algorithm files pass explicit temporaries).
- Prefer the LASX (`xv*`) variants when the CPU supports LASX — they process twice the lanes per instruction.
- `vstelmx` is slower than a vector store; use it only for strided scalar writes.
- The `.b` boolean dot-product variants are useful for computing SAD masks without separate compare instructions.

## 8. Verification checklist

1. Instantiate each macro family in a small assembly test file, assemble it, and disassemble to verify instruction counts and operand types.
2. For each transpose macro, run a runtime test that transposes a known matrix twice and recovers the original.
3. For clip macros, test values below, inside and above the bounds.
4. For strided-load macros, verify each output vector contains the expected row for distinct base/stride values.
5. Verify `function`/`endfunc` produce correct global symbol, size, alignment and return instruction.

## 9. Open questions

- UNKNOWN: whether the vendored library exactly matches LoongML v0.4.0 or carries x264-specific patches; compare with the upstream release if needed.
- UNKNOWN: some aliases (`fa*`, `f*`) may be unused by x264 — a minimal rebuild can omit them but must keep `vr*`/`xr*` and the GPR aliases.
