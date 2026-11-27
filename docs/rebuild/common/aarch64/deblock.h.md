# `common/aarch64/deblock.h` — AArch64 deblocking declarations

> Dirty-side behavioural specification (reverse-engineered from the reference encoder; describes behaviour, not code).

## 1. Header

- Source path: `common/aarch64/deblock.h`
- Role: declares the AArch64 NEON deblocking filters and the boundary-strength computation, plus the one SVE filter variant.
- This is a **dirty-side behavioural specification** artifact for the clean-room reimplementation.

## 2. Purpose

The generic deblocking dispatcher (`common/deblock.c`) selects accelerated filters through function pointers. This header enumerates every AArch64 entry point and fixes the calling signature shared by all filter variants, so that luma/chroma, inter/intra, vertical/horizontal, and MBAFF/4:2:2 special cases can be swapped in uniformly.

## 3. Public interface

All filter functions share the signature:

`void fn( uint8_t *pix, intptr_t stride, int alpha, int beta, ... )`

- `pix`: pointer to the block edge (vertical filters point at the column boundary; horizontal filters point at the row boundary minus four columns).
- `stride`: row stride of the plane (typically 32).
- `alpha`, `beta`: H.264 deblocking thresholds derived from QP.
- The trailing parameter is `int8_t *tc0` for inter filters, or absent for intra filters (intra uses a fixed strength of 3).

Declared functions:
- Luma inter: `x264_deblock_v_luma_neon`, `x264_deblock_h_luma_neon`
- Luma intra: `x264_deblock_v_luma_intra_neon`, `x264_deblock_h_luma_intra_neon`
- Chroma inter: `x264_deblock_v_chroma_neon`, `x264_deblock_h_chroma_neon`, `x264_deblock_h_chroma_422_neon`, `x264_deblock_h_chroma_mbaff_neon`
- Chroma intra: `x264_deblock_v_chroma_intra_neon`, `x264_deblock_h_chroma_intra_neon`, `x264_deblock_h_chroma_intra_mbaff_neon`, `x264_deblock_h_chroma_422_intra_neon`
- Boundary strength:
  ```
  void x264_deblock_strength_neon( uint8_t nnz[X264_SCAN8_SIZE],
                                   int8_t ref[2][X264_SCAN8_LUMA_SIZE],
                                   int16_t mv[2][X264_SCAN8_LUMA_SIZE][2],
                                   uint8_t bs[2][8][4],
                                   int mvy_limit, int bframe );
  ```
  Computes the 4×4 boundary strengths for one macroblock edge set.
- SVE: `void x264_deblock_v_chroma_sve( uint8_t *pix, intptr_t stride, int alpha, int beta, int8_t *tc0 );`

`bs` layout: `bs[dir][edge_index][4]`, where `dir=0` is vertical (left/right edges) and `dir=1` horizontal (top/bottom edges); four 4×4 edges per macroblock side.

## 4. State and data structures

- `nnz`: scan-indexed array of non-zero flags per 4×4 block.
- `ref[2][...]`: reference indices for list 0/1 per 4×4 block.
- `mv[2][...][2]`: motion vectors (x,y) for list 0/1 per 4×4 block.
- `tc0`: four signed bytes per edge; negative values select filtering and their magnitude is the clipping limit.
- `mvy_limit`, `bframe`: B-frame motion-vector difference threshold and flag.

## 5. Algorithm / behaviour

The header itself performs no computation. Semantically it asserts that all filter variants obey the same contract:
- Vertical filters read four rows centered on the boundary and write back the two rows adjacent to it.
- Horizontal filters operate on columns: they transpose 16-byte rows into columns, filter, then transpose back and scatter columns into rows.
- Intra variants ignore `tc0` and use a fixed filter strength; they early-out when both alpha and beta are zero.
- The SVE variant is a drop-in replacement for the vertical chroma inter filter only.

## 6. Dependencies

- Included by `common/deblock.c`.
- Implementations in `deblock-a.S` and `deblock-a-sve.S`.
- Uses `X264_SCAN8_SIZE`, `X264_SCAN8_LUMA_SIZE` from the common headers, and `x264_template` for symbol naming.

## 7. Constants and tables

No constants defined in the header. `X264_SCAN8_*` are compile-time layout constants from the common code. The filter strength constants (3 for intra, tc0-derived for inter) are behavioural facts implemented in the assembly.

## 8. Performance notes

Declaration-only. Dispatch note: the SVE override is applied after NEON selection, so a CPU with both features uses SVE for the chroma-V filter and NEON for everything else. SVE/SVE2 optimization tip: predicated stores are the main advantage; a wider SVE port could batch multiple block edges. Public references: H.264 deblocking filter standard; x264 deblock dispatcher design.

## 9. References

- ITU-T H.264 / ISO/IEC 14496-10, Clause 8.7 (deblocking filter) and Clause 7.4.3 (boundary strength derivation).
- x264 `common/deblock.c`.
- `docs/rebuild/common/aarch64/deblock-a.S.md`, `deblock-a-sve.S.md`.

## 10. Verification checklist

- Every declared symbol exists in the assembly files with the exact signature.
- Function-pointer assignment in `common/deblock.c` type-checks for every declaration.
- For each feature set (NEON only, NEON+SVE), the selected filter set is the expected one.

## 11. Open questions

- UNKNOWN: whether `x264_deblock_strength_neon` is also dispatched in high-bit-depth builds.
- UNKNOWN: the exact numeric values of `X264_SCAN8_SIZE` and `X264_SCAN8_LUMA_SIZE` in this build configuration (they come from the common headers).
