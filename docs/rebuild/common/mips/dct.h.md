# `common/mips/dct.h` — MSA transform declarations

**Dirty-side behavioural specification.** Source path: `E:\_x264_dirty_stash\x264-source\common\mips\dct.h`. Role: public declaration surface for the MSA transform implementations in `mips/dct-c.c`.

## 1. Header

The title line above gives the source path and the file's role in one sentence; the adjacent bold marker identifies this document as a **dirty-side behavioural specification** written from the reference source and public literature. No reference source or near-verbatim pseudocode is reproduced.

## 2. Purpose

This header is a thin contract layer. It exposes the MSA transform functions to the architecture-dispatch layer (`common/dct.c`) without leaking implementation details, and applies x264's `x264_template` name mangling so that one source compiles into the correct bit-depth-specific namespace (e.g. `x264_8_add4x4_idct_msa`). It exists so the generic dispatcher can assign function pointers conditionally on `X264_CPU_MSA`.

## 3. Public interface

Every declaration is preceded by `#define <name> x264_template(<name>)`. All routines are compiled only when `HIGH_BIT_DEPTH == 0`. The exact set is:

**DC Hadamard helpers**
- `void x264_dct4x4dc_msa(int16_t d[16])` — in-place 4x4 Hadamard of sixteen DC coefficients.
- `void x264_idct4x4dc_msa(int16_t d[16])` — in-place inverse of the same transform.

**Inverse transforms with add**
- `void x264_add4x4_idct_msa(uint8_t *dst, int16_t dct[16])`
- `void x264_add8x8_idct_msa(uint8_t *dst, int16_t dct[4][16])`
- `void x264_add16x16_idct_msa(uint8_t *dst, int16_t dct[16][16])`
- `void x264_add8x8_idct8_msa(uint8_t *dst, int16_t dct[64])`
- `void x264_add16x16_idct8_msa(uint8_t *dst, int16_t dct[4][64])`
- `void x264_add8x8_idct_dc_msa(uint8_t *dst, int16_t dct[4])`
- `void x264_add16x16_idct_dc_msa(uint8_t *dst, int16_t dct[16])`

**Forward residual transforms**
- `void x264_sub4x4_dct_msa(int16_t dst[16], uint8_t *src, uint8_t *ref)`
- `void x264_sub8x8_dct_msa(int16_t dst[4][16], uint8_t *src, uint8_t *ref)`
- `void x264_sub16x16_dct_msa(int16_t dst[16][16], uint8_t *src, uint8_t *ref)`
- `void x264_sub8x8_dct_dc_msa(int16_t dct[4], uint8_t *p1, uint8_t *p2)`
- `void x264_sub8x16_dct_dc_msa(int16_t dct[8], uint8_t *p1, uint8_t *p2)`

**Scan**
- `void x264_zigzag_scan_4x4_frame_msa(int16_t level[16], int16_t dct[16])`

Contracts are defined in `dct-c.c.md`: forward routines produce unscaled integer-transform coefficients in raster order; inverse routines add to the destination with `FDEC_STRIDE` pitch, clip to 8-bit range, and (4x4 paths) zero their source; DC-sum helpers return Hadamard-transformed sums of per-4x4 prediction residuals.

## 4. State and data structures

None. This is a declarations-only header with an include guard (`X264_MIPS_DCT_H`). It does not define types, tables, or globals. All state is in the caller (coefficient arrays and frame buffers).

## 5. Algorithm / behaviour

The header itself performs no computation. The `x264_template(X)` macro is a preprocessor indirection that concatenates the current bit-depth token with `X` and `_msa` suffix; the resulting symbol is the one actually linked. This is a naming convention only — it does not affect calling convention or ABI. Declarations are guarded by `#ifndef X264_MIPS_DCT_H` so multiple inclusion is harmless.

The function set mirrors the generic DCT vtable slots one-for-one: `dct4x4dc`, `idct4x4dc`, `add4x4_idct`, `add8x8_idct`, `add16x16_idct`, `add8x8_idct8`, `add16x16_idct8`, `add8x8_idct_dc`, `add16x16_idct_dc`, `sub4x4_dct`, `sub8x8_dct`, `sub16x16_dct`, `sub8x8_dct_dc`, `sub8x16_dct_dc`, `zigzag_scan_4x4_frame`. There is deliberately **no** 16x16 8x8-transform inverse slot here (handled via composition in `dct-c.c`), and no field-scan variant.

## 6. Dependencies

- **Included by:** `common/mips/dct-c.c` (its own implementation) and `common/dct.c` (the dispatcher that installs function pointers).
- **Requires from the build environment:** `x264_template` (defined by `common/common.h`/`config.h`), `int16_t` and `uint8_t` from `<stdint.h>` (pulled in transitively), and the `FDEC_STRIDE`/`FENC_STRIDE` constants understood by implementations (not needed for declarations).
- No dependency on `macros.h` or `<msa.h>` — those are implementation-private.

## 7. Constants and tables

No constants or tables are declared here. The numeric constants used by the implementations (zig-zag masks, rounding offsets, clip bounds) are documented in `dct-c.c.md`.

## 8. Performance notes

Headers have no runtime cost. For rebuild, keep the declaration order stable so that a generated vtable-initializer can refer to symbols without forward-declaration issues. Because the mangling is done at preprocessing time, calling code and implementation must see the same `x264_template` definition; a mismatch silently produces unresolved symbols.

## 9. References

ITU-T H.264 standard; x264 project documentation; architecture SIMD programmer's guides.

## 10. Verification checklist

1. Compile a translation unit that includes this header alone under both bit-depth configurations and confirm no implicit-declaration warnings.
2. Confirm the preprocessor-expanded symbol names match those actually defined in `dct-c.c` (e.g. by inspecting the object symbol table with `nm`).
3. Check that every function pointer slot in the generic DCT vtable that the MSA path claims to support has a matching declaration here.
4. Ensure no declaration leaks a `static` implementation helper.

## 11. Open questions

- UNKNOWN: whether some downstream builds expect additional declarations (e.g. `x264_sub4x4_dct_msa` is present but no `x264_sub4x4_dct_dc_msa`); confirm against `common/dct.c` vtable layout in the target version.
- UNKNOWN: whether the include guard name is relied upon by any external code.
