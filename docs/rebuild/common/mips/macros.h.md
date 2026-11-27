# `common/mips/macros.h` — MSA intrinsic wrapper macros

**Dirty-side behavioural specification.** Source path: `E:\_x264_dirty_stash\x264-source\common\mips\macros.h`. Role: a shared portability/shorthand layer over the MSA C intrinsics (`<msa.h>`) used by every MIPS SIMD source file in this directory.

## 1. Header

The title line above gives the source path and the file's role in one sentence; the adjacent bold marker identifies this document as a **dirty-side behavioural specification** written from the reference source and public literature. No reference source or near-verbatim pseudocode is reproduced.

## 2. Purpose

MSA intrinsics are verbose and have awkward type casts. This header defines a large family of preprocessor macros that give short, uniform names to common operations: strided loads/stores, interleaves, packs, butterflies, transposes, saturating arithmetic, shifts with rounding, dot products, and sign handling. It also abstracts the ISA-revision difference between MSARev5 (unaligned `ulh`/`ulw`/`uld` helpers) and MSARev6 (native `lh`/`lw`/`ld`/`sh`/`sw`/`sd`). All MIPS algorithm files include it so their code stays readable and so a single change (e.g. an alignment fix) propagates everywhere.

## 3. Public interface

This is a macro-only header; it exports no functions or variables. Include guard `X264_MIPS_MACROS_H`. It includes `<stdint.h>` and `<msa.h>`. Macros are grouped by function:

**Scalar load/store (GPR)**
- `LD_B/UB/SB`, `LD_H/SH`, `LD_W/SW` — dereference a pointer as a vector type; the typed variants (`LD_UB`, `LD_SH`, `LD_SW`, `ST_UB`, `ST_SH`, ...) select `v16u8`/`v16i8`/`v8i16`/`v4i32`.
- `LH`, `LW`, `LD`, `SH`, `SW`, `SD` — inline-assembly unaligned (Rev5: `ulh`, `ulw`, `uld`, `ush`, `usw`; Rev6: `lh`, `lw`, `ld`, `sh`, `sw`, `sd`). On 32-bit builds `LD`/`SD` assemble two 32-bit halves.

**Strided vector load/store families**
- `LD_B2/3/4/5/8`, `LD_H2/4/8`, `LD_SW2` with typed aliases (`LD_UB4`, `LD_SB8`, `LD_SH2`, `LD_SH8`, ...). Each loads N vectors at offsets `k*stride` from the base pointer.
- `ST_B2/4/8`, `ST_H2/4/8` with typed aliases (`ST_UB4`, `ST_SB4`, `ST_SH2`, `ST_SH8`, ...).
- Specialized: `LD4x4_SH` (loads a 4x4 halfword block from a 1-D array using two vector loads plus lane duplication), `ST2x4_UB`, `ST4x4_UB`, `ST4x8_UB`, `ST8x1_UB`, `ST8x4_UB` (extract scalar words/doublewords from vectors and store with stride).

**Arithmetic / logic**
- `ADD2/4`, `SUB4`, `MUL2/4`, `ADDS_SH2/4`, `SRA_4V`, `SLLI_4V`, `SRL_H4`, `SRARI_H2/4`, `SRARI_W2/4`, `SRAR_H2/4`.
- `AVER_UB2/4` — rounding average `(a+b+1)>>1` per unsigned byte.
- `MAXI_SH2/4`, `CLIP_SH`, `CLIP_SH_0_255` (and 2/4-vector forms), `SAT_UH2/4`, `SAT_SH2/4`, `SAT_SW2`.
- `XORI_B2/3/4/5_128` — flip byte sign representation by XOR with 128.

**Data movement / layout**
- `ILVEV_*2`, `ILVOD_*`, `ILVL_B2/4`, `ILVR_B2/4`, `ILVRL_B2/H2/W2`, `ILVL_H2/4`, `ILVR_H2/4`, `ILVL_W2`, `ILVR_W2`, `ILVR_D2/4`, `ILVL_D2`.
- `PCKEV_B2/3/4`, `PCKEV_H2/4`, `PCKEV_D2/4`, `PCKOD_B2/4`, `PCKOD_D2`.
- `VSHF_B2`, `VSHF_H2`, `SLDI_B2`, `SLDI_B2_0`, `INSERT_W2/4`, `INSERT_D2`.
- `UNPCK_R_SH_SW`, `UNPCK_SH_SW`, `UNPCK_UB_SH`.

**Reductions / transforms**
- `HADD_SB2/4`, `HADD_UB2/4`, `HSUB_UB2/4`, `HADD_SW_S32`, `HADD_UH_U32`, `DOTP_UB2/4`, `DPADD_SB2/4`, `DPADD_SH2/4`, `DPADD_SH3_SH`, `SAD_UB2_UH`.
- `BUTTERFLY_4`, `BUTTERFLY_8` — the core radix-2 butterfly used by DCT/SATD.
- `TRANSPOSE8x8_UB`, `TRANSPOSE16x8_UB_UB`, `TRANSPOSE4x4_SH_SH`, `TRANSPOSE4X8_SH_SH`, `TRANSPOSE8X4_SH_SH`, `TRANSPOSE8x8_H` (aliased to `TRANSPOSE8x8_SH_SH`), `TRANSPOSE4x4_SW_SW`.

**Block-oriented composites**
- `ADDBLK_ST4x4_UB` — add a 4x4 halfword residual to four rows of destination bytes, clip, store.
- `PCKEV_XORI128_UB`, `PCKEV_ST4x4_UB`, `PCKEV_ST_SB`.
- `AVC_CALC_DPADD_H_6PIX_2COEFF_SH`, `AVC_HORZ_FILTER_SH` — the luma 6-tap filter kernels used by `mc-c.c`.

## 4. State and data structures

None. All macros are functional: they either produce an expression (statement-expression style with `__msa_*` intrinsics) or expand to a brace-enclosed block that writes to output operands. Vector types are the MSA built-ins `v16i8`, `v16u8`, `v8i16`, `v8u16`, `v4i32`, `v4u32`, `v2i64`, `v2u64`. The `RTYPE` template parameter of a macro selects the result element type; typed aliases fix it (e.g. `..._UB` → `v16u8`, `..._SH` → `v8i16`, `..._SW` → `v4i32`).

**Important convention:** stride arguments to load/store macros are expressed in units of the *base pointer's element type*, not bytes. For `uint8_t*` this is bytes; for `int16_t*` a stride of 4 means four 16-bit elements. Mixed usage is possible (e.g. `LD_SW2(pi_dequant_mf[i_mf], 4, ...)` passes 4 words = 16 bytes).

## 5. Algorithm / behaviour

The header defines no algorithms of its own; it delegates to MSA intrinsics. Key semantics that a clean-room rebuild must reproduce:

- **Butterfly-4:** given four vectors `a,b,c,d`, produces `a+d`, `b+c`, `b−c`, `a−d` in that output order. Butterfly-8 similarly produces sums `a0+a7, a1+a6, a2+a5, a3+a4` followed by differences `a3−a4, a2−a5, a1−a6, a0−a7`. This ordering is used by DCT and SATD and must be preserved exactly.
- **Rounding shifts:** `SRARI_*` add the last discarded bit (round-to-nearest) before shifting; `SRA_4V` is plain arithmetic shift with variable count.
- **Saturation:** `SAT_UH2/4(sat_val=n)` clamps unsigned halfword lanes to `2^(n+1)−1`; `SAT_SH*` clamps signed lanes symmetrically; `CLIP_SH_0_255` clamps to `[0,255]`.
- **Sign conversion:** XOR with 128 transforms signed byte values in `[-128,127]` to unsigned `[0,255]` by flipping the sign bit, used before unsigned saturating arithmetic and after filtering.
- **Transposes:** the macros implement fixed-size 4x4/8x8 transposes for byte, halfword and word lanes via interleaves; `TRANSPOSE16x8_UB_UB` converts sixteen 16-byte rows into eight vectors holding columns of a 16x8 byte block.
- **Horizontal sums:** `HADD_UH_U32` reduces eight unsigned halfwords to one 32-bit sum via word/doubleword horizontal adds; `HADD_SW_S32` reduces four signed words similarly. Widening dot products (`DOTP_*`/`DPADD_*`) accumulate adjacent pair products and are the basis of SAD/SSE/SSIM kernels.
- **ISA selection:** for `__mips_isa_rev >= 6` scalar loads/stores use `lh/lw/ld/sh/sw/sd`; for older revisions the `ul*`/`us*` unaligned forms are used. On 32-bit MIPS, 64-bit load/store is composed from two 32-bit halves.

## 6. Dependencies

- **Included by:** all MIPS `.c` files: `dct-c.c`, `deblock-c.c`, `mc-c.c`, `pixel-c.c`, `predict-c.c`, `quant-c.c`.
- **Requires:** `<msa.h>` (the compiler-provided intrinsic header), `<stdint.h>`, and compiler support for GCC statement expressions (`({ ... })`) and `__msa_*` builtins.
- **Not used directly by** the public headers; it is an implementation detail.

## 7. Constants and tables

- The XOR constant **128** for signed↔unsigned byte conversion.
- Saturation widths passed as immediates (e.g. `7` → clamp to 255, `9` → clamp to 1023).
- Shift immediates: 1, 2, 3, 5, 6, 10, 16 depending on caller.
- Zig-zag/filter constants are not here; they belong to the algorithm files.
- No data tables are defined in this header.

## 8. Performance notes

- **Hotness:** N/A (header), but code quality here determines all MIPS kernels' performance.
- **Optimization tips:** (a) macro expansion should produce single MSA instructions for the simple wrappers — inspect compiler output to confirm no temporaries spill; (b) statement-expression macros can inhibit CSE when used twice with the same inputs; hoist into a local vector if needed; (c) on MSARev6 the `LD_UB*/ST_UB*` macros via typed dereference are usually compiled to aligned `ld`/`st` when pointers are known aligned — keep pointers 16-byte aligned; (d) `BUTTERFLY_8` is only eight add/sub instructions — do not replace with a loop.
## 9. References

MSA Intrinsics specification (Imagination Technologies / MIPS "MSA Programmer's Guide"), the `msa.h` header shipped with GCC/binutils, and the Loongson/MIPS x264 port documentation.

## 10. Verification checklist

1. Compile a test TU that instantiates every macro family with each typed alias under both `__mips_isa_rev` branches (via cross-compiler or `-D`).
2. For butterfly macros, verify output ordering with distinct input values.
3. For transpose macros, verify `transpose(transpose(x)) == x` for byte/halfword/word variants.
4. For rounding shifts, test values exactly at the half-way boundary (e.g. `x = (7 << 5) + 32` with shift 5 yields 8, not 7).
5. For `CLIP_SH_0_255`, test −1, 0, 255, 256.
6. For load/store macros, test with 8-, 16- and 32-bit-strided pointers and confirm no overlap or gap.
7. Check that no macro relies on undefined C behaviour (strict-aliasing violations from typed pointer casts) in the target compiler.

## 11. Open questions

- UNKNOWN: whether any macro is unused by the current algorithm files and could be dropped in a minimal rebuild (inventory shows most are used).
- UNKNOWN: exact behaviour of `LD4x4_SH` when passed a non-8-element-aligned `int16_t*`; documented as taking a 1-D 4x4 block but may require 16-byte alignment.
- UNKNOWN: whether the statement-expression style macros are portable to clang's MSA backend in the target toolchain; GCC is assumed.
