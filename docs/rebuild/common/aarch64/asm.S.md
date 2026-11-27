# `common/aarch64/asm.S` — shared AArch64 assembly macros

> Dirty-side behavioural specification (reverse-engineered from the reference encoder; describes behaviour, not code).

## 1. Header

- Source path: `common/aarch64/asm.S`
- Role: common assembler-level infrastructure for every AArch64 assembly file in the tree: symbol naming, platform directives, PIC-safe constant addressing, strides, and SIMD butterfly/transpose helpers.
- This is a **dirty-side behavioural specification** artifact for the clean-room reimplementation.

## 2. Purpose

Assembly files in this directory are preprocessed by the C preprocessor and then assembled by GNU as / Apple as on several ABIs (ELF Linux, Mach-O macOS, Windows). This file hides those differences behind a small set of macros and defines the recurring vector operations (sum/difference butterflies, unzips, transposes) used by the DCT, pixel, and deblocking kernels. Centralizing them avoids duplicating ABI conditionals in every kernel.

## 3. Public interface

Preprocessor/assembler macros (all are assembler-time only, no runtime ABI of their own):

- Symbol naming: two token-pasting helpers; a conditional symbol base; an external-symbol prefix; `X(name)`, `X264(name)`, `EXT(name)`.
- Platform toggles: `ELF`, `MACH`, `FUNC` — each is either a real directive prefix or a comment marker depending on the target.
- `function name, export=0, align=2`: begins a function body, optionally global/exported, with the requested alignment; the matching `endfunc` closes it and emits size metadata.
- `const name, align=2`: switches to a read-only data section (with a Mach-O variant), aligns, labels the table; `endconst` closes it.
- `movrel rd, val, offset=0`: materializes the address of a symbol plus an optional offset into a 64-bit register, with four platform-specific strategies.
- `FDEC_STRIDE` = 32, `FENC_STRIDE` = 16 (compile-time constants).
- `SUMSUB_AB sum, sub, a, b`: creates `sum = a + b` and `sub = a - b` with two instructions.
- `unzip t1, t2, s1, s2`: even/odd element de-interleave (`uzp1`/`uzp2`).
- `transpose t1, t2, s1, s2`: pairwise interleave (`trn1`/`trn2`).
- `transpose4x4.h`, `transpose4x8.h`: 4×4 and 4×8 halfword matrix transposes using four temporary vectors.
- `transpose8x8.h`: full 8×8 halfword transpose (8 data registers + 2 temporaries).
- `transpose_8x16.b`, `transpose_4x16.b`, `transpose_4x8.b`: byte-matrix transposes for 8×16, 4×16, and 4×8 shapes.

## 4. State and data structures

No runtime state. The macros operate on assembler registers, which the calling kernels allocate as needed. The transposes require disjoint temporary registers; callers must supply them (the macro does not allocate). `FDEC_STRIDE`/`FENC_STRIDE` describe the padded reconstructed and source plane strides used throughout x264: prediction writes into the reconstruction plane with stride 32, while source rows are fetched with stride 16.

## 5. Algorithm / behaviour

**Symbol naming.** When `PREFIX` is defined, the internal base becomes `_x264_` and exported symbols acquire a leading underscore; otherwise the base is `x264_` with no leading underscore. `BIT_DEPTH` (when defined) inserts the bit depth into the external symbol name so that 8-bit and 10-bit builds can coexist in one library. `X(s)` produces the bit-depth-aware external name, `X264(s)` the internal name, and `EXT(s)` the platform-decorated external name.

**Directives.** On ELF, `function` emits `.global`, `.type %function`, optional `.func`, and `.size`; on Mach-O some of these are omitted or replaced by comment tokens. `FUNC` gates `.func`/`.endfunc` based on `HAVE_AS_FUNC`. `const` selects `.rodata` on ELF and `.const_data` on Mach-O.

**`movrel`.** Four strategies are selected at preprocessing time:
- Apple: `adrp`+`add` with `@PAGE`/`@PAGEOFF`; a negative offset is applied by a following subtract rather than folding it into the page expression.
- PIC on Windows: `adrp` with `:lo12:` relocation, again handling negative offsets specially.
- PIC elsewhere: `adrp` of symbol+offset plus `:lo12:` of the same expression.
- Non-PIC: a literal `ldr` of symbol+offset.

The negative-offset handling matters because some callers index *before* a table base (notably the CABAC range-LPS table).

**Butterflies.** `SUMSUB_AB` is the classic in-place sum/difference pair. `unzip` and `transpose` wrap NEON's `uzp1/uzp2` and `trn1/trn2` with a consistent (dest1, dest2, src1, src2) argument order.

**Transposes.** The halfword 4×4 and 4×8 variants first transpose 32-bit lanes then 16-bit lanes, using the generic two-register transpose primitive three times per stage. The 8×8 halfword variant uses a three-stage network: byte-adjacent pairs, then 32-bit pairs, then 64-bit pairs, with careful register renaming so it can operate in place on eight registers. The byte variants follow the same interleave hierarchy at byte/halfword/word granularity; `transpose_8x16.b` and `transpose_4x16.b` additionally need two scratch registers.

No control flow or memory access occurs in these macros; all effects are register-level.

## 6. Dependencies

- Included by every `.S` file in this directory (directly or transitively through the `*-common.S` files).
- Depends on build-generated `config.h` (for `HAVE_AS_FUNC`, `HAVE_*`, `SYS_*`) and the build's `-D` flags (`PREFIX`, `BIT_DEPTH`, `PIC`, `__ELF__`, `__MACH__`, `__APPLE__`, `_WIN32`).

## 7. Constants and tables

`FDEC_STRIDE` = 32 and `FENC_STRIDE` = 16 are the only numeric constants. No data tables are defined; table definitions live in the kernel files that include this one.

## 8. Performance notes

These macros are on the hot path of nearly every kernel. The transpose networks are implemented with single-cycle NEON permutes; a clean-room port should map them to the target's native shuffle/permute intrinsics (on x86-64: `UNPCKL*`/`UNPCKH*`, `_mm_unpacklo_epi16` etc., or AVX2 `vpermd`-based transposes) rather than looping over elements. SVE/SVE2 tip: fixed-width transposes still map to NEON `trn`/`uzp` on SVE-capable cores; SVE's variable-length permutes are useful only when the block width is not known at compile time. Refer to ARM's NEON/SVE intrinsic documentation and standard x264 assembly style guides for the register-allocation patterns.

## 9. References

- ARM Architecture Reference Manual (AArch64), NEON `add`/`sub`/`trn`/`uzp` instruction descriptions.
- ARM SVE/SVE2 Architecture Reference Manual (`ptrue`, `ld1b`, `st1b`, predication).
- GNU assembler and Apple assembler directive documentation (`.func`, `.size`, `.type`, `.align`, `.section`).
- x264 developer documentation on bit-depth-specific symbol naming.

## 10. Verification checklist

- A minimal assembly file including this header assembles successfully for ELF, Mach-O, and Windows-style predefines.
- Exported symbol names have the expected leading underscore/no underscore and bit-depth suffix in each configuration.
- `movrel` works with both positive and negative offsets, verified by reading the referenced table at run time.
- Each transpose macro, when fed a known 4×4/8×8 matrix, produces the mathematically transposed matrix; verify in a unit test against a scalar transpose.
- `SUMSUB_AB` produces correct results for signed 16-bit inputs including overflow-adjacent values (it wraps, as the hardware does).

## 11. Open questions

- UNKNOWN: which exact toolchain combinations require `HAVE_AS_FUNC` to be disabled.
- UNKNOWN: whether any consumer relies on `EXT()` in this directory (no use is visible in the listed kernels; it may be used by other architecture directories).
