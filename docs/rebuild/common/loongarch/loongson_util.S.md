# `common/loongarch/loongson_util.S` — LoongArch assembly utility macros

**Dirty-side behavioural specification.** Source path: `E:\_x264_dirty_stash\x264-source\common\loongarch\loongson_util.S`. Role: provides x264-specific assembly helpers on top of the generic `loongson_asm.S` macro library — token pasting, the bit-depth-aware symbol prefix, frame stride constants, and the `function_x264`/`endfunc_x264` function definition macros used by every LoongArch `.S` file.

## 1. Purpose

The generic `loongson_asm.S` library defines `function`/`endfunc` with an empty `ASM_PREF`. x264 needs its exported assembly symbols to carry the same bit-depth prefix as its C symbols (`x264_8_...`, `x264_10_...`). This file redefines the function macros to prepend `ASM_REF = x264_<BIT_DEPTH>_` and provides the two hardcoded frame strides used by all SIMD kernels. It is included by every LoongArch assembly file after `loongson_asm.S`.

## 2. Public interface

This is an assembly-macro-only file; it exports no runtime symbols of its own. It defines:

- `GLUE(a,b)` — token pasting (raw `##`).
- `JOIN(a,b)` — two-level token pasting, allowing macro expansion of arguments before concatenation.
- `ASM_REF` — `JOIN(JOIN(x264_, BIT_DEPTH), _)`; expands to `x264_8_` in an 8-bit build and `x264_10_` in a high-bit-depth build. This is the LoongArch equivalent of x264 C's `x264_template` mangling.
- `FENC_STRIDE` — **16** (bytes per row of the encode-plane scratch buffer; for high-bit-depth builds this is still 16 bytes = 8 pixels, but see open questions).
- `FDEC_STRIDE` — **32** (bytes per row of the reconstruction plane).
- `function_x264 name, align=DEFAULT_ALIGN` — starts an aligned, global, `@function`-typed symbol named `ASM_REF<name>` in `.text`.
- `endfunc_x264` — emits `jirl $r0, $r1, 0` (return), sets the ELF symbol size for the current function, and purges the macro so it can be redefined for the next function.

## 3. State and data structures

No runtime state. The macros rely on `DEFAULT_ALIGN` (defined as 5, i.e. 32-byte alignment, in `loongson_asm.S`) and on the GNU assembler's `.macro`/`.purgem` facilities. `BIT_DEPTH` is supplied by the build system's preprocessor defines.

## 4. Algorithm / behaviour

`JOIN` is needed because a single `##` would paste the literal token `BIT_DEPTH` instead of its value; the two-level indirection ensures the preprocessor expands `BIT_DEPTH` first. `ASM_REF` then mirrors the C-level name mangling so that a declaration like `void x264_template(predict_4x4_v_lsx)(pixel*)` in C compiles to `x264_8_predict_4x4_v_lsx`, which is exactly the symbol emitted by `function_x264 predict_4x4_v_lsx`.

`function_x264` expands to: switch to `.text`, align to `1 << align` (normally 32 bytes), declare the symbol global and of function type, then emit its label. `endfunc_x264` emits the standard LoongArch return instruction `jirl $r0, $r1, 0` (jump-and-link-register to `$r1`, the return address, with link register `$r0`, the zero register), closes the symbol size, and removes the macro definition so a fresh `endfunc_x264` can be declared inside the next `function_x264`. This pattern lets the assembler check that every function has a matching end.

## 5. Dependencies

- **Included by:** all nine LoongArch `.S` files, always after `#include "loongson_asm.S"`.
- **Requires:** `loongson_asm.S` for `DEFAULT_ALIGN` and register aliases; `BIT_DEPTH` from the build; GNU as macros.
- **No** C header dependency.

## 6. Constants and tables

- `FENC_STRIDE = 16`, `FDEC_STRIDE = 32` — the same values as the MIPS port. They are the byte pitches of x264's temporary encode and reconstruction planes for 8-bit video, and are used by all DCT/MC kernels as immediate offsets in load/store instructions.
- Alignment exponent 5 (32 bytes) inherited from `DEFAULT_ALIGN`.

## 7. Performance notes

No runtime cost. For rebuild, the critical correctness property is that `ASM_REF` matches the C `x264_template` expansion exactly; a mismatch produces link errors or, worse, silently calls the wrong bit-depth function.

## 8. Verification checklist

1. Preprocess an assembly file and verify `ASM_REF` expands to the expected prefixed symbol.
2. Assemble one function and inspect the ELF symbol table: it must be global, `@function`, 32-byte aligned, with a correct size.
3. Verify `endfunc_x264` emits exactly one `jirl` return and no stray macro leakage.
4. Compare `nm` output against the declarations in the LoongArch headers.

## 9. Open questions

- UNKNOWN: correctness of `FENC_STRIDE=16`/`FDEC_STRIDE=32` for high-bit-depth builds — most LoongArch SIMD code is gated behind `HIGH_BIT_DEPTH == 0`, so this may be harmless, but a rebuild should confirm.
- UNKNOWN: whether any LoongArch assembly file relies on `ASM_PREF` being empty (it is defined as empty in `loongson_asm.S`; overriding it would break `ASM_REF` matching).
