# `common/dct.h` — transform and zigzag dispatch interface

**Role:** declares the two function-pointer tables through which every transform and
coefficient-scan operation is invoked.
**Status:** dirty-side behavioural specification.

---

## 1. Header

- Source path: `common/dct.h`.
- Role in one sentence: define the `x264_dct_function_t` and `x264_zigzag_function_t`
  dispatch structures and the two initialization functions that populate them.
- Marker: dirty-side behavioural specification.

---

## 2. Purpose

The encoder never calls a transform function by its C name. All transform work goes through
`h->dctf` and `h->zigzagf`, which are filled once at startup by `x264_dct_init` and
`x264_zigzag_init`. This indirection is what allows a single build to select MMX, SSE, AVX,
NEON, Altivec, MSA or LoongArch implementations at runtime. The header's job is to fix the
exact calling convention — argument order, strides, buffer shapes and side effects — so
that each SIMD replacement is a drop-in for the scalar reference.

---

## 3. Public interface

Exported types:

- `x264_dct_function_t` — a struct of function pointers grouped by operation:
  - `sub4x4_dct(dctcoef dct[16], pixel *pix1, pixel *pix2)` — forward 4x4 residual transform.
  - `add4x4_idct(pixel *dst, dctcoef dct[16])` — inverse 4x4, add-and-clip into `dst`.
  - `sub8x8_dct(dct[4][16], ...)`, `sub8x8_dct_dc(dct[4], ...)` — 8x8 split into four 4x4
    transforms, and its DC-only companion.
  - `add8x8_idct`, `add8x8_idct_dc` — matching inverse operations.
  - `sub8x16_dct_dc(dct[8], ...)` — DC-only transform for 4:2:2 chroma (8x16 samples).
  - `sub16x16_dct(dct[16][16], ...)`, `add16x16_idct`, `add16x16_idct_dc` — 16x16 luma.
  - `sub8x8_dct8(dct[64], ...)`, `add8x8_idct8` — High-profile 8x8 transform.
  - `sub16x16_dct8(dct[4][64], ...)`, `add16x16_idct8` — four 8x8 blocks.
  - `dct4x4dc(d[16])`, `idct4x4dc(d[16])` — Hadamard over the 16 DC coefficients.
  - `dct2x4dc(dct[8], dct4x4[8][16])` — 2x4 Hadamard for 4:2:2 chroma DC, also zeroes the
    eight source DC entries.
- `x264_zigzag_function_t` — a struct of scan pointers:
  - `scan_8x8(level[64], dct[64])`, `scan_4x4(level[16], dct[16])` — pure reordering.
  - `sub_8x8(level[64], const pixel *src, pixel *dst)` and `sub_4x4(...)` — compute
    differences in scan order, copy source to destination, and return a nonzero flag.
  - `sub_4x4ac(level[16], const pixel *src, pixel *dst, dctcoef *dc)` — same but writes the
    DC difference to `*dc` and places zero in `level[0]`.
  - `interleave_8x8_cavlc(dst, src, nnz)` — transpose four 8x8 coefficient sub-blocks into
    CAVLC order and fill their nonzero flags.
- Functions:
  - `void x264_dct_init(uint32_t cpu, x264_dct_function_t *dctf)`
  - `void x264_zigzag_init(uint32_t cpu, x264_zigzag_function_t *pf_progressive,
    x264_zigzag_function_t *pf_interlaced)`

Both init functions **must** install every pointer; no entry is allowed to remain null. The
zigzag initializer fills the two tables independently because progressive and interlaced
macroblocks can coexist in one stream.

---

## 4. State and data structures

- `dctcoef` is the signed coefficient type: 16-bit in the normal build, 32-bit when
  `HIGH_BIT_DEPTH` is defined. `udctcoef` is the unsigned counterpart used by quantization.
  `pixel` is `uint8_t` normally and `uint16_t` for high bit depth.
- Coefficient arrays are flat, row-major, with the sub-block convention documented in
  `dct.c.md`: a 4x4 is 16 entries, an 8x8 is 64, and larger blocks are arrays of these.
- `x264_zigzag_function_t` deliberately carries no mutable state; it is a pure vtable.
- Lifetime: the tables live inside `x264_t` and are initialized before any macroblock is
  processed. Multiple encoder contexts may hold independent, identically filled tables.

---

## 5. Algorithm / behaviour

The header itself contains no algorithms, but it encodes three behavioural contracts that
implementations must honour:

1. **Stride contract.** In `sub*` functions the first pixel argument (`pix1` / `p_src`) uses
   `FENC_STRIDE`, the second (`pix2` / `p_dst`) uses `FDEC_STRIDE`, and inverse functions write
   to a `FDEC_STRIDE` destination. These strides include padding and are not necessarily
   equal to the block width.
2. **Add-and-clip contract.** Every `add*` function adds the reconstructed residual to the
   existing sample and saturates to `[0, PIXEL_MAX]`. It never overwrites blindly.
3. **Scan-order contract.** `sub_4x4`/`sub_8x8` write coefficients in the *scan* order
   (frame or field depending on which table was used), not in raster order, while the pure
   `scan_*` functions transform a raster coefficient array into scan order.
4. **Dispatch contract.** The `*_init` functions assign scalar defaults first and then
   upgrade entries as CPU flags allow, so the last matching flag wins. Architecture-specific
   `UNKNOWN` entry points are not part of this header.

---

## 6. Dependencies

- Included by `common/common.h` consumers, `encoder/macroblock.c`, `encoder/rdo.c`,
  `encoder/cabac.c`, `encoder/cavlc.c` and the SIMD headers under `common/x86`, `common/arm`,
  `common/aarch64`, etc.
- Depends on type definitions (`pixel`, `dctcoef`, `udctcoef`) and the stride macros from
  `common.h`; on `x264_template` naming macro for symbol prefixing.
- The init functions are implemented in `dct.c`; no other file writes these tables.

---

## 7. Constants and tables

- The enum-like block shapes are implicit in the array sizes: 16, 64, 4, 8 coefficients.
- No numeric constants are defined in the header; the transform constants and zigzag tables
  live in `dct.c` and `tables.c`.

---

## 8. Performance notes

- Because every pointer call is indirect, the tables should stay hot in L1; grouping them
  by operation (as done) gives good locality for the analysis loop.
- Alignment of `dct` arrays to 16 or 32 bytes is expected by SIMD variants; callers normally
  declare them with `ALIGNED_ARRAY`.
- The header is included in nearly every encoder translation unit, so it must stay free of
  heavyweight definitions.
- See `dct.c.md` section 8 for the SIMD upgrade ladder and transform literature.

---

## 9. References

- ITU-T H.264, clause 8.5 (integer transforms) and the coefficient-scanning tables.
- x264 `dct.c` implementation and its checkasm coverage.
- Public H.264 transform literature cited in `dct.c.md`.

---

## 10. Verification checklist

1. Compile a trivial translation unit that includes only this header; confirm all declared
   types and the two init prototypes resolve (no missing typedefs).
2. After `x264_dct_init(0, &dctf)`, assert every function pointer is non-null; repeat after
   `x264_zigzag_init(0, &prog, &field)` for both tables.
3. Call each pointer once with a minimal valid block and confirm no crash and the documented
   side effects (clip, copy, nonzero flag).
4. On a SIMD-capable machine, compare outputs of the table built with `cpu=0` against the
   table built with the real CPU mask over random inputs (checkasm-style).
5. For high bit depth, compile with `HIGH_BIT_DEPTH` and verify that coefficient arrays are
   wide enough (no truncation of large DC values).

---

## 11. Open questions

- `UNKNOWN`: whether any downstream caller expects additional entries in these structs in a
  future revision; the documented set is exactly what the reference builds.
- `UNKNOWN`: the precise CPU-bit-to-implementation mapping on non-x86 architectures is left
  to the architecture init functions.
