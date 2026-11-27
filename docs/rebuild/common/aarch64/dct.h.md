# `common/aarch64/dct.h` — AArch64 transform/zigzag declarations

> Dirty-side behavioural specification (reverse-engineered from the reference encoder; describes behaviour, not code).

## 1. Header

- Source path: `common/aarch64/dct.h`
- Role: declares every AArch64-accelerated transform, inverse transform, DC-only transform, zig-zag scan, fused zig-zag/subtract, and CAVLC interleave function, including the SVE/SVE2 variants.
- This is a **dirty-side behavioural specification** artifact for the clean-room reimplementation.

## 2. Purpose

The generic transform code in `common/dct.c` selects architecture-specific implementations through function-pointer tables. This header is the contract between those tables and the assembly implementations: it fixes signatures, array shapes, and naming for NEON, SVE, and SVE2 entry points. It also documents, through the suffix conventions, which CPU feature level each implementation requires.

## 3. Public interface

Functions grouped by role (all names are wrapped by the build's symbol-templating macro):

**4×4 DC transforms (in place):**
- `void x264_dct4x4dc_neon( int16_t d[16] )` — transform the 4×4 DC block in place.
- `void x264_idct4x4dc_neon( int16_t d[16] )` — inverse of the above, in place.

**Forward 4×4 / 8×8 / 16×16 DCT (source − reconstruction):**
- `void x264_sub4x4_dct_neon( int16_t dct[16], uint8_t *pix1, uint8_t *pix2 )`
- `void x264_sub8x8_dct_neon( int16_t dct[4][16], uint8_t *pix1, uint8_t *pix2 )`
- `void x264_sub16x16_dct_neon( int16_t dct[16][16], uint8_t *pix1, uint8_t *pix2 )`
- `void x264_sub4x4_dct_sve( int16_t dct[16], uint8_t *pix1, uint8_t *pix2 )` (SVE)

**Inverse DCT with add:**
- `void x264_add4x4_idct_neon( uint8_t *p_dst, int16_t dct[16] )`
- `void x264_add8x8_idct_neon( uint8_t *p_dst, int16_t dct[4][16] )`
- `void x264_add16x16_idct_neon( uint8_t *p_dst, int16_t dct[16][16] )`
- `void x264_add4x4_idct_sve2( uint8_t *p_dst, int16_t dct[16] )` (SVE2)

**DC-only inverse / DC-only forward:**
- `void x264_add8x8_idct_dc_neon( uint8_t *p_dst, int16_t dct[4] )`
- `void x264_add16x16_idct_dc_neon( uint8_t *p_dst, int16_t dct[16] )`
- `void x264_sub8x8_dct_dc_neon( int16_t dct[4], uint8_t *pix1, uint8_t *pix2 )`
- `void x264_sub8x16_dct_dc_neon( int16_t dct[8], uint8_t *pix1, uint8_t *pix2 )`

**8×8 transform (High profile):**
- `void x264_sub8x8_dct8_neon( int16_t dct[64], uint8_t *pix1, uint8_t *pix2 )`
- `void x264_sub16x16_dct8_neon( int16_t dct[4][64], uint8_t *pix1, uint8_t *pix2 )`
- `void x264_add8x8_idct8_neon( uint8_t *p_dst, int16_t dct[64] )`
- `void x264_add16x16_idct8_neon( uint8_t *p_dst, int16_t dct[4][64] )`

**Zig-zag scans (in place, 16-bit input/output):**
- `void x264_zigzag_scan_4x4_frame_neon( int16_t level[16], int16_t dct[16] )`
- `void x264_zigzag_scan_4x4_field_neon( ... )`
- `void x264_zigzag_scan_8x8_frame_neon( int16_t level[64], int16_t dct[64] )`
- `void x264_zigzag_scan_8x8_field_neon( ... )`

**Fused zig-zag + subtract + non-zero test (return int):**
- `int x264_zigzag_sub_4x4_field_neon( dctcoef level[16], const pixel *p_src, pixel *p_dst )`
- `int x264_zigzag_sub_4x4ac_field_neon( ..., dctcoef *dc )` (AC-only; DC passed separately)
- `int x264_zigzag_sub_4x4_frame_neon( ... )`
- `int x264_zigzag_sub_4x4ac_frame_neon( ..., dctcoef *dc )`
- `int x264_zigzag_sub_8x8_field_neon( dctcoef level[16], const pixel *p_src, pixel *p_dst )`
- `int x264_zigzag_sub_8x8_frame_neon( ... )`

**CAVLC interleave:**
- `void x264_zigzag_interleave_8x8_cavlc_neon( dctcoef *dst, dctcoef *src, uint8_t *nnz )`
- `void x264_zigzag_interleave_8x8_cavlc_sve( ... )` (SVE)

All functions obey generic x264 conventions: `pix1` is the source plane with stride 16, `pix2`/`p_dst` the reconstruction plane with stride 32, coefficient arrays are row-major, and the return value of a `zigzag_sub` function is nonzero when any resulting coefficient is non-zero.

## 4. State and data structures

- Coefficient types: `int16_t`/`dctcoef` (16-bit signed), `udctcoef` (unsigned, used in quant).
- `pixel` is `uint8_t` in 8-bit builds and `uint16_t` in high-bit-depth builds; all functions in this header are for 8-bit pixel data (10-bit transforms live in the generic C code / other headers).
- The `dct[4][16]` and `dct[16][16]` shapes represent 2×2 and 4×4 arrays of 4×4 blocks; `dct[4][64]` represents four 8×8 blocks of a 16×16 macroblock.

## 5. Algorithm / behaviour

The header performs no computation. Its semantic content is the mapping from generic transform names to accelerated implementations:

- `*_neon` symbols require `X264_CPU_NEON` and are available in 8-bit builds (some also in high-bit-depth builds via separate C implementations; UNKNOWN for this header).
- `*_sve` symbols require `X264_CPU_SVE` and override only `sub4x4_dct` and the progressive CAVLC interleave.
- `*_sve2` symbols require `X264_CPU_SVE2` and override only `add4x4_idct`.
- The generic dispatcher applies overrides in increasing feature order, so SVE2 wins over SVE/NEON where both provide a function.

## 6. Dependencies

- Included by `common/dct.c`.
- Implementations live in `dct-a.S`, `dct-a-sve.S`, `dct-a-sve2.S`.
- Uses build-wide typedefs (`dctcoef`, `pixel`) and `x264_template` for symbol naming.

## 7. Constants and tables

No constants or tables. Block shapes (16, 64, 256 coefficients) and strides (16/32) are implicit in the signatures and the implementations.

## 8. Performance notes

Not executed; it is a declaration-only header. SVE/SVE2 guidance for a clean-room port: keep the dispatcher semantics identical — a kernel that is numerically different breaks bitstream compatibility. The SVE/SVE2 overrides in the reference are deliberately few (only one forward and one inverse transform), so a clean-room implementation should not assume all transforms have SVE versions. Public references: x264 function-table design; ARM SVE feature detection documentation.

## 9. References

- ITU-T H.264 / ISO/IEC 14496-10, transform and scan specifications.
- x264 common transform dispatcher (`common/dct.c`).
- `docs/rebuild/common/aarch64/dct-a.S.md`, `dct-a-sve.S.md`, `dct-a-sve2.S.md`.

## 10. Verification checklist

- Every declared function has a matching exported symbol in one of the three assembly files.
- Signatures match the generic function-pointer types in `common/dct.c` (a compile-time check in C).
- The CPU-feature override order (NEON → SVE → SVE2) produces the expected function for each feature combination.
- No declaration uses a type that conflicts with the build's bit-depth configuration.

## 11. Open questions

- UNKNOWN: whether high-bit-depth builds use any of these NEON declarations (the assembly implementations shown are byte-pixel oriented).
- UNKNOWN: exact declared type of `dctcoef` in configurations where `HIGH_BIT_DEPTH` is enabled.
