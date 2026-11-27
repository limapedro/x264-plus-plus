# `common/arm/deblock.h` — ARM deblocking declarations

## 1. Header

- **Source path:** `common/arm/deblock.h`.
- **Role in one sentence:** declares NEON-accelerated deblocking-filter entry points for luma and chroma edges plus boundary-strength computation.
- **Marker:** Dirty-side behavioural specification; all descriptions are behavioural and written independently of the reference source.


## 2. Purpose

The deblocking filter is applied to every decoded macroblock edge and is both hot and highly branchy in the scalar reference. This header exposes the ARM-optimized routines so the generic dispatcher can replace the scalar edge filters and the strength estimator when NEON is available.

## 3. Public interface

The declared functions fall into three groups.

Inter-frame edge filters (all take `uint8_t *pix, intptr_t stride, int alpha, int beta, int8_t *tc0`):
- `x264_deblock_v_luma_neon`, `x264_deblock_h_luma_neon` — 16-sample luma edges, vertical and horizontal.
- `x264_deblock_v_chroma_neon`, `x264_deblock_h_chroma_neon` — 4:2:0 chroma edges.
- `x264_deblock_h_chroma_422_neon` — 4:2:2 chroma horizontal edges.
- `x264_deblock_h_chroma_mbaff_neon` — MBAFF chroma horizontal edges.

Intra-frame edge filters (take the same parameters but no `tc0`, since intra edges use a fixed filtering formula):
- `x264_deblock_v_luma_intra_neon`, `x264_deblock_h_luma_intra_neon`.
- `x264_deblock_v_chroma_intra_neon`, `x264_deblock_h_chroma_intra_neon`, `x264_deblock_h_chroma_422_intra_neon`, `x264_deblock_h_chroma_intra_mbaff_neon`.

Strength computation:
- `void x264_deblock_strength_neon( uint8_t nnz[X264_SCAN8_SIZE], int8_t ref[2][X264_SCAN8_LUMA_SIZE], int16_t mv[2][X264_SCAN8_LUMA_SIZE][2], uint8_t bs[2][8][4], int mvy_limit, int bframe )`

Contracts: `pix` points at the edge's `q0` sample; `stride` is the picture stride in bytes; `alpha`/`beta` are the already-looked-up threshold values; `tc0` holds four per-4x4-block clipping thresholds, with a negative value meaning "skip this block". The strength routine writes `bs[dir][edge][i]` for both vertical (`dir=1`) and horizontal (`dir=0`) edges of a macroblock.

## 4. State and data structures

`nnz` is a scan-order map of nonzero flags for a macroblock; `ref` holds reference indices for both lists; `mv` holds motion vectors in quarter-pel units; `bs` is the 2x8x4 output byte array. The `X264_SCAN8_*` constants describe the scan-buffer geometry. No function allocates memory; all output buffers are caller-owned.

## 5. Algorithm / behaviour

The algorithms follow the H.264 deblocking specification as implemented in `common/deblock.c`:

- An edge is filtered only when the three gradient tests pass: `|p0-q0| < alpha`, `|p1-p0| < beta`, and `|q1-q0| < beta`.
- For luma inter edges, when `|p2-p0| < beta` an extra sample `p1` is modified with a clipped average, and `tc` is incremented; the symmetric test applies to `q1`. The boundary samples `p0` and `q0` are then changed by a delta derived from `(q0-p0)*4 + (p1-q1) + 4`, right-shifted by 3 and clipped to `+/-tc`, with the final samples clipped to the valid pixel range.
- For chroma edges only `p0` and `q0` are modified, using the same delta formula and the supplied `tc` value; edges with `tc <= 0` are skipped.
- Intra edges use stronger fixed filters: when `|p0-q0| < alpha/4 + 2` three samples per side may be replaced by weighted averages; otherwise only `p0` and `q0` get a four-tap average.
- Boundary strength is 2 if either adjacent 4x4 block has any nonzero coefficient; otherwise 1 if reference indices or motion-vector components differ by at least the thresholds (4 horizontally, `mvy_limit` vertically, for both lists when `bframe` is set); otherwise 0.

The NEON versions compute all four or eight edge positions in parallel using byte vectors, transposing where needed for horizontal edges.

## 6. Dependencies

- Installed by `x264_deblock_init` in `common/deblock.c` when NEON is detected.
- Implemented in `common/arm/deblock-a.S`.
- Needs `x264_clip_pixel` semantics (saturate to 0..255) and the shared alpha/beta/tc0 tables in `common/deblock.c` for parameter derivation (the caller supplies the derived values).

## 7. Constants and tables

- The clipping limit for the delta is `tc`, incremented to `tc+1` or `tc+2` when the outer-sample tests pass.
- Strong intra filtering uses the threshold `alpha >> 2 + 2`; final averages divide by 4 or 8 with a rounding constant of 2 or 4 respectively.
- Strength thresholds: 4 for horizontal MV components, `mvy_limit` for vertical components.
- Tables `i_alpha_table`, `i_beta_table`, `i_tc0_table` live in the generic file and are indexed by `qp + offset + 24`.

## 8. Performance notes

Deblocking runs on every macroblock in every decoded frame and is therefore extremely hot. The scalar implementation has unpredictable branches per 4-pixel group; the NEON version removes them by computing masks and selecting results with bitwise operations. Cache usage is a few rows around the edge, so locality is good. For a clean rebuild, implement the mask-based formulation in scalar form first, validate against the reference, then map it to SIMD. Public references on the H.264 loop filter and FFmpeg's deblocking implementations are useful.

## 9. References

- ITU-T H.264, clause on the adaptive deblocking filter.
- JVT-B118 (decimation reference used elsewhere in x264; not needed for filtering itself).
- `common/deblock.c` as the canonical behavioural reference.
- ARM NEON Programmer's Guide for byte-mask selection.

## 10. Verification checklist

- All-zero boundary strength leaves pixels unchanged.
- `alpha == 0` or `beta == 0` leaves pixels unchanged.
- A single failing gradient test disables filtering for that edge position only.
- Output is saturated to 0..255 and never wraps.
- Vertical and horizontal variants produce identical results on transposed input.
- Strength computation matches the scalar reference for randomized `nnz`/`ref`/`mv` arrays, including B-frame mode.

## 11. Open questions

- `UNKNOWN`: exact treatment of chroma edges with `tc0 == 0` in the NEON variants; the generic code skips them, and the assembly appears to rely on a sign test.
- `UNKNOWN`: whether all declared functions are installed in high-bit-depth builds; the header is unconditional but the implementation may be 8-bit-only.
