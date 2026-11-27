# `common/aarch64/dct-a-sve.S` — SVE 4×4 forward DCT and CAVLC interleave

> Dirty-side behavioural specification (reverse-engineered from the reference encoder; describes behaviour, not code).

## 1. Header

- Source path: `common/aarch64/dct-a-sve.S`
- Role: SVE implementations of the 4×4 forward DCT and the 8×8 CAVLC zig-zag interleave helper.
- This is a **dirty-side behavioural specification** artifact for the clean-room reimplementation.

## 2. Purpose

Provides two SVE-accelerated kernels selected when the CPU reports SVE support: a 4×4 forward transform that uses SVE predicated byte loads, and a zig-zag interleave that uses SVE's predicate-driven byte stores to emit per-8×8-block non-zero flags. Both must be numerically identical to their NEON counterparts.

## 3. Public interface

Two exported functions (declared in `dct.h`):

- `void x264_sub4x4_dct_sve( int16_t dct[16], uint8_t *pix1, uint8_t *pix2 );`
  - `x0` = output coefficient array (16 halfwords), `x1` = source plane pointer, `x2` = reconstruction plane pointer.
  - Computes the 4×4 forward transform of `pix1 - pix2`, using source stride 16 and reconstruction stride 32.
- `void x264_zigzag_interleave_8x8_cavlc_sve( dctcoef *dst, dctcoef *src, uint8_t *nnz );`
  - `x0` = destination coefficient array, `x1` = source array (four interleaved 8×8 coefficient blocks, 256 coefficients), `x2` = destination for four non-zero flags.
  - Performs the same de-interleave/interleave as the NEON version and writes four flag bytes (one per 8×8 block) to `nnz`.

## 4. State and data structures

- `sub4x4_dct_sve`: SVE predicate `p0.h` with `vl4` (four halfword lanes); eight SVE vector registers used as 128-bit NEON views (`z0`–`z7`) for the row data; four difference registers.
- `zigzag_interleave_8x8_cavlc_sve`: predicate `p2.s` with `vl2` for the two-byte flag stores; vector constant `z31.s = 1` used as the "non-zero" mask; eight 8-halfword registers holding the source coefficients.

No persistent state.

## 5. Algorithm / behaviour

### `sub4x4_dct_sve`

1. Set predicate to four halfword lanes. Load four rows of four source bytes from `pix1` (stride 16) and four rows of four reconstruction bytes from `pix2` (stride 32), widening bytes to halfwords on load.
2. For each row, compute the signed difference `src - rec` (16-bit).
3. Apply the shared 1-D DCT butterfly to the four row-difference vectors.
4. Transpose the resulting 4×4 halfword matrix.
5. Apply the same 1-D butterfly to the transposed columns.
6. Store the sixteen resulting halfwords to `dct` with a single four-register store.

The arithmetic has no rounding or saturation; the outputs are the exact H.264 4×4 transform coefficients of the signed byte differences. Row/column ordering follows the standard raster order of the input block.

Comparison with NEON: the only difference is the load mechanism — SVE predicated widening byte loads (`ld1b` under `p0.h`) versus NEON scalar-lane loads. The butterfly and transpose are the shared NEON macros, so results are bit-identical.

### `zigzag_interleave_8x8_cavlc_sve`

1. Load the source as two groups of four interleaved 8-halfword vectors (i.e. four 8×8 blocks interleaved coefficient-by-coefficient).
2. Compute pairwise unsigned maxima of coefficient magnitudes between corresponding vectors of the two groups, progressively reducing to a single 8-halfword vector whose lanes hold the max magnitude for each coefficient position across the four blocks.
3. Simultaneously store the de-interleaved coefficients to the destination: for each of the eight positions, store the coefficients belonging to group A and group B as separate 16-byte stores (eight stores total, alternating groups). This converts the source's interleaved layout into the destination's block-major layout.
4. From the reduced magnitude vector, compare each lane against 1 (`cmhs` against a vector of ones) to produce a per-lane "non-zero" mask, and mask it with ones.
5. Store the first two flag bytes to `nnz[0]` and `nnz[1]` using SVE predicated byte stores with `vl2`.
6. Move the high 64-bit half of the flag vector into the low half and store two more flag bytes at `nnz + 8` (the reference advances `nnz` by 8 between the two stores). Result: four bytes, one per 8×8 block, indicating whether that block has any non-zero coefficient.

Note the flag layout: the NEON version writes bytes at offsets 0, 7, 8, 12 (via scalar stores with a stride of 7); the SVE version writes at 0, 1 and then 8 (after adding 8), i.e. offsets 0, 1, 8, 9. UNKNOWN whether the SVE caller expects a different flag packing, or whether one of these readings is a misobservation; this is a verification priority because a wrong flag layout changes CAVLC decisions.

## 6. Dependencies

- Includes `asm.S` and `dct-a-common.S`.
- Requires `.arch armv8-a+sve`.
- Uses shared `SUMSUB_AB`, `DCT_1D`, and `transpose4x4.h` macros.
- Dispatched from `common/dct.c` when `HAVE_SVE` and `X264_CPU_SVE` are set; the interleave function is additionally dispatched for progressive CAVLC in the zig-zag function table.

## 7. Constants and tables

- `1`: threshold for the non-zero flag comparison.
- Predicate widths `vl4` (halfwords) and `vl2` (words) are compile-time immediates.
- No lookup tables (unlike the NEON zig-zag scan kernels, which use permutation tables).

## 8. Performance notes

The forward DCT is medium-hot (one per 4×4 residual block); the interleave is hot only for CAVLC streams. SVE predicated loads avoid the NEON kernel's series of single-lane loads, reducing instruction count on high-width SVE implementations, but only four halfwords are used so the extra width is wasted — the key benefit is the widening-load form, not parallelism. Optimization tip: on a clean-room port with SVE2, consider using `sli`/`uzp` to widen more rows at once, or use `ld1b` with `vl8` to process two 4×4 blocks in parallel when the encoder has them available. Public references: ARM SVE intrinsics (`svld1_u8`, `svptrue_b16`, `svst1b_u32`) and H.264 4×4 transform definition.

## 9. References

- ITU-T H.264 / ISO/IEC 14496-10, 4×4 forward transform and CAVLC block flag semantics.
- ARM SVE Architecture Reference Manual (`ptrue`, `ld1b`, `st1b`, `umax`, `cmhs`).
- `docs/rebuild/common/aarch64/dct-a-common.S.md`, `dct-a.S.md`, `dct.h.md`.

## 10. Verification checklist

- `sub4x4_dct_sve` output matches `x264_sub4x4_dct_neon` exactly for random byte blocks with the documented strides.
- `zigzag_interleave_8x8_cavlc_sve` output coefficients match the NEON version for random 256-coefficient inputs.
- The four `nnz` flags match the NEON version's flags; resolve the UNKNOWN offset layout by checking against the C reference CAVLC caller.
- Predicated stores touch exactly the intended bytes (guard-page test around `nnz`).
- Works on SVE hardware with vector lengths greater than 128 bits (predicate `vl4` must restrict activity to four halfwords).

## 11. Open questions

- UNKNOWN: exact byte offsets of the four `nnz` flags written by the SVE interleave (needs cross-check with the common C caller).
- UNKNOWN: whether the stride arguments are ever anything other than 16/32 for `sub4x4_dct_sve` (the kernel hard-codes them).
