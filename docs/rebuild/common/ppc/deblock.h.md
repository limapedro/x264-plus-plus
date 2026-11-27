# `common/ppc/deblock.h` — PowerPC deblocking declarations

## 1. Header

- **Source path:** `common/ppc/deblock.h`.
- **Role in one sentence:** declares the two AltiVec deblocking routines available on PowerPC.
- **Marker:** Dirty-side behavioural specification; all descriptions are behavioural and written independently of the reference source.


## 2. Purpose

Unlike ARM, the PowerPC back end accelerates only the two luma inter-frame edge filters. This header exposes those two entry points to the generic deblocking dispatcher.

## 3. Public interface

- `void x264_deblock_v_luma_altivec( uint8_t *pix, intptr_t stride, int alpha, int beta, int8_t *tc0 )`
- `void x264_deblock_h_luma_altivec( uint8_t *pix, intptr_t stride, int alpha, int beta, int tc0 )` (declared with `int8_t *tc0` in the source header)

`pix` points at the `q0` position of the edge, `stride` is the picture stride in bytes, `alpha` and `beta` are the already-derived thresholds, and `tc0` holds four clipping thresholds. A negative value in `tc0` marks a block that must be skipped.

## 4. State and data structures

Both functions are stateless. They load six samples per side of the edge into 16-byte vectors, where each byte lane represents one of sixteen edge positions. The horizontal variant must transpose data before filtering because the edge runs vertically in memory.

## 5. Algorithm / behaviour

The routines implement the standard H.264 luma loop filter:

- The edge is filtered only if `|p0-q0| < alpha`, `|p1-p0| < beta`, and `|q1-q0| < beta`.
- If `|p2-p0| < beta`, sample `p1` is adjusted toward `(p2 + ((p0+q0+1)>>1)) >> 1` with clipping to `+/-tc0`, and the delta limit is incremented. The symmetric rule applies to `q1`.
- Boundary samples are adjusted by `((q0-p0)*4 + (p1-q1) + 4) >> 3`, clipped to `+/-tc`.
- All results are saturated to the 0..255 range.

The AltiVec implementation computes all sixteen lane results in parallel using mask vectors and saturating byte arithmetic.

## 6. Dependencies

- Implemented in `common/ppc/deblock.c`.
- Declared here and installed by `x264_deblock_init` when `X264_CPU_ALTIVEC` is set.
- Uses `LOAD_ZERO` and vector helpers from `ppccommon.h`.

## 7. Constants and tables

- Delta rounding constant 4, shift 3.
- Threshold comparisons use `< alpha` and `< beta`, never `<=`.
- Clipping uses `tc0` and the incremented values `tc0+1` or `tc0+2`.

## 8. Performance notes

The vectorized filter removes the per-edge-position branches of the scalar version. The horizontal variant pays a transposition cost but still wins over scalar access. Other filter variants (chroma, intra, MBAFF, strength) remain generic on PowerPC. Rebuilders should focus on exact mask semantics and saturation.

## 9. References

- ITU-T H.264, adaptive deblocking filter.
- `common/deblock.c` for canonical edge formulas.
- AltiVec Programming Interface Manual.

## 10. Verification checklist

- Output matches the generic luma filter for randomized pixels and parameters.
- Negative `tc0` lanes are skipped without affecting neighbors.
- Saturating arithmetic prevents wraparound.
- Vertical and horizontal variants agree on transposed inputs.

## 11. Open questions

- `UNKNOWN`: whether the exported `v_luma`/`h_luma` routines are installed only in 8-bit builds.
- `UNKNOWN`: exact behaviour for lanes where `alpha` or `beta` is zero.
