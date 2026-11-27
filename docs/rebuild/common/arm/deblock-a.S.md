# `common/arm/deblock-a.S` — ARM/NEON deblocking filters

## 1. Header

- **Source path:** `common/arm/deblock-a.S`.
- **Role in one sentence:** NEON implementations of the H.264 loop filter for luma and chroma edges and of boundary-strength derivation.
- **Marker:** Dirty-side behavioural specification; all descriptions are behavioural and written independently of the reference source.


## 2. Purpose

The deblocking filter is applied to every macroblock edge and is highly branch-heavy in scalar form. This file replaces those branches with vector masks so all edge positions are processed in parallel. It covers vertical and horizontal luma/chroma edges, intra variants, MBAFF and 4:2:2 variants, and the strength estimator.

## 3. Public interface

Exported symbols (declared in `arm/deblock.h`):

- Inter luma: `x264_deblock_v_luma_neon`, `x264_deblock_h_luma_neon`
- Intra luma: `x264_deblock_v_luma_intra_neon`, `x264_deblock_h_luma_intra_neon`
- Inter chroma: `x264_deblock_v_chroma_neon`, `x264_deblock_h_chroma_neon`, `x264_deblock_h_chroma_422_neon`, `x264_deblock_h_chroma_mbaff_neon`
- Intra chroma: `x264_deblock_v_chroma_intra_neon`, `x264_deblock_h_chroma_intra_neon`, `x264_deblock_h_chroma_422_intra_neon`, `x264_deblock_h_chroma_intra_mbaff_neon`
- `x264_deblock_strength_neon`

All inter-filter functions use the signature `(uint8_t *pix, intptr_t stride, int alpha, int beta, int8_t *tc0)`; the intra filters omit `tc0`. The strength function uses the five-argument form declared in the header.

ABI: `pix` in `r0`, `stride` in `r1`, `alpha` in `r2`, `beta` in `r3`, and `tc0` on the stack. `h264_loop_filter_start` reads the `tc0` pointer from the stack, loads four thresholds, duplicates them into `d24`, and returns early when all four are negative.

## 4. State and data structures

No persistent state. The routine loads three pixels on each side of the edge (`p2,p1,p0,q0,q1,q2`) into NEON registers `q8..q2` for luma, or two per side for chroma. The `tc0` array is converted to a duplicated vector of signed bytes. The strength routine reads scan-order neighbor maps and writes a `[2][8][4]` byte array.

## 5. Algorithm / behaviour

**Common luma inter path.** The macro builds a mask from three conditions: `|p0-q0| < alpha`, `|p1-p0| < beta`, and `|q1-q0| < beta`. It also tests `|p2-p0| < beta` and `|q2-q0| < beta`. The outer-sample tests increase the effective clipping threshold by one and allow modification of `p1`/`q1`. The inner samples are updated by a delta computed as `((q0-p0)*4 + (p1-q1) + 4) >> 3`, clipped to `+/-tc`. All additions and subtractions use saturating byte instructions and `vqmovun.s16` narrowing, so output stays in 0..255. The macro operates on 16 edge positions in parallel (one byte lane per position), processing two 4-pixel groups at a time.

**Vertical vs horizontal.** `deblock_v_luma_neon` loads six contiguous rows and filters them directly. `deblock_h_luma_neon` loads a 16x8 tile, transposes it with the shared 8x8 transpose macro, runs the same filter macro, transposes back, and stores four modified columns.

**Luma intra path.** The macro evaluates the same three basic tests plus the strong-filter condition `|p0-q0| < (alpha>>2)+2`. In the strong case, three samples per side are replaced with 3-tap, 4-tap, or 5-tap averages with rounding constants 2 or 4; otherwise only `p0` and `q0` get 4-tap averages. Bitwise selects (`vbit`) choose between the two results per lane without branching.

**Chroma paths.** Only `p0` and `q0` are modified. The macro uses the same delta formula and threshold mask but does not touch outer samples. The 4:2:2 variant processes twice as many rows per edge; the MBAFF variant uses a reduced-width filter macro. Intra chroma uses the short 4-tap average.

**Strength.** The routine loads reference-index and motion-vector arrays for both list directions, shifts and compares them against thresholds, ORs comparisons into per-direction masks, then combines with the nonzero-coefficient map. Nonzero coefficients force strength 2; motion/reference differences force strength 1; otherwise strength 0. Results are written in the two `[8][4]` planes.

## 6. Dependencies

- Includes `common/arm/asm.S` for transpose and symbol macros.
- Declared in `arm/deblock.h` and installed by `x264_deblock_init` in `common/deblock.c`.
- Shares the alpha/beta/tc0 values computed by the generic `deblock_edge` helper.

## 7. Constants and tables

- Delta rounding constant 4, right shift 3.
- Strong intra threshold `(alpha >> 2) + 2`; averaging divisors 4 and 8 with rounding constants 2 and 4.
- Strength thresholds: 4 for horizontal motion-vector difference, `mvy_limit` for vertical.
- `X264_SCAN8_*` constants define the scan buffer geometry used by the strength routine.

## 8. Performance notes

The mask-based formulation removes all data-dependent branches from the inner loop, which is the principal performance gain. Transposition for horizontal edges costs several `vtrn` instructions but avoids scalar access patterns. Memory access is confined to a few rows around the edge, giving good locality. For a clean implementation, first build a scalar version that computes masks and selects results, then vectorize. Validate against the generic filter on boundary cases where gradients are exactly at threshold.

## 9. References

- ITU-T H.264, adaptive deblocking filter specification.
- `common/deblock.c` for canonical edge formulas and strength rules.
- ARM NEON Programmer's Guide for `vabd`, `vclt`, `vbsl`, and saturating arithmetic.

## 10. Verification checklist

- Zero strength or zero alpha/beta leaves pixels unchanged.
- Output matches the generic C filter for randomized pixels and parameters.
- Saturating arithmetic prevents wraparound at 0 and 255.
- Vertical and horizontal variants agree on transposed inputs.
- Strength output matches the scalar implementation for both frame and B-frame modes.
- All edge positions within one 16-wide vector are independent: a failing test at one lane must not change neighbors.

## 11. Open questions

- `UNKNOWN`: exact handling of chroma edges where `tc0` is zero; the generic code skips them and the assembly uses a sign-based early exit.
- `UNKNOWN`: whether all declared variants are installed in high-bit-depth builds.
