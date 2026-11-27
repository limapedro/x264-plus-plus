# `common/aarch64/predict-a.S` — NEON/AArch64 intra prediction

> Dirty-side behavioural specification (reverse-engineered from the reference encoder; describes behaviour, not code).

## 1. Header

- Source path: `common/aarch64/predict-a.S`
- Role: AArch64 implementations of all H.264 intra prediction modes for 4×4 luma, 8×8 luma, 16×16 luma, 8×8 chroma, and 8×16 chroma blocks.
- This is a **dirty-side behavioural specification** artifact for the clean-room reimplementation.

## 2. Purpose

Intra prediction generates a block from already-decoded neighbouring samples. It is cheap per block but invoked extremely often during intra mode search, so SIMD implementations matter. This file covers the simple directional/DC modes (plain AArch64 scalar stores for 4×4 H/V) and the more complex plane/filtered-diagonal modes (NEON for larger blocks).

## 3. Public interface

Exported functions (declared in `predict.h`):
- 4×4 luma: `predict_4x4_{h,v}_aarch64` (plain ARMv8), `predict_4x4_{dc,dc_top,ddl,ddr}_neon`
- 8×8 luma (filtered modes, take `edge[36]`): `predict_8x8_{v,h,dc,ddl,ddr,vl,vr,hd,hu}_neon`
- 8×8 chroma: `predict_8x8c_{v}_aarch64`, `predict_8x8c_{dc,dc_top,dc_left,h,p}_neon`
- 8×16 chroma: `predict_8x16c_{v,h,dc,dc_top,dc_left,p}_neon`
- 16×16 luma: `predict_16x16_{v,h,dc,dc_top,dc_left,p}_neon`

All functions take a single `uint8_t *src` pointer (positioned at the block's top-left in the reconstruction plane with `FDEC_STRIDE = 32`), except 8×8 luma which additionally takes `uint8_t edge[36]` holding the pre-filtered boundary samples.

## 4. State and data structures

- `p8weight`: eight 16-bit weights `1,2,3,4,1,2,3,4` used by the 8×8 chroma plane predictor.
- `p16weight`: eight 16-bit weights `1..8` used by the 8×16 chroma and 16×16 luma plane predictors.
- `ldcol.8` / `ldcol.16` macros load a column of 8/16 bytes from addresses separated by `FDEC_STRIDE`, with optional high-half control, assembling a vector of column samples.
- `loadsum4` macro loads four left-edge bytes and sums them.

No persistent state.

## 5. Algorithm / behaviour

### Simple 4×4 modes

- `predict_4x4_h_aarch64`: reads the four left neighbours (at column −1 of rows 0..3), multiplies each by `0x01010101`, and stores one 32-bit word per row — broadcasting the neighbour across all four columns.
- `predict_4x4_v_aarch64`: loads one 32-bit word from the row above the block and stores it to each of the four rows — copying the top row downward.
- `predict_4x4_dc_neon`: sums the four top samples and four left samples (8 samples total), shifts right by 3 with rounding (`rshrn #3`), and fills all 16 pixels with the result.
- `predict_4x4_dc_top_neon`: same with only the four top samples, shift right by 2.
- `predict_4x4_ddr_neon` (diagonal down-right): for each position, averages three boundary samples with rounding shift 2; produces four rows by successive `ext` shifts of the computed vector.
- `predict_4x4_ddl_neon` (diagonal down-left): iteratively averages the top row with its shifted versions using `uhadd`/`urhadd`, writing one row at a time.

### 8×8 luma modes (filtered boundary from `edge[36]`)

- `predict_8x8_v_neon`, `h_neon`, `dc_neon`: copy a boundary row/column or fill with the rounded average of top+left samples (shift 4).
- `predict_8x8_ddl_neon`, `ddr_neon`: filtered diagonal predictions using pairwise `uhadd`/`urhadd` and `ext` rotations to generate the eight rows.
- `predict_8x8_vl_neon`, `vr_neon`: vertical-left / vertical-right filtered modes, combining two boundary samples with weights (1:2:1 style) using `uhadd`/`urhadd` chains, then generating rows by shifts.
- `predict_8x8_hd_neon`, `hu_neon`: horizontal-down / horizontal-up filtered modes, using `zip1`/`zip2`/`ext` to interleave two computed half-rows into the eight output rows.

All 8×8 luma functions write eight rows of eight bytes with stride 32.

### Chroma DC modes

- `predict_8x8c_dc_top_neon`: averages the eight top samples (shift 2), splits the 8×8 block into four 4×4 quadrants, and gives the left quadrants one average and the right quadrants another (from the 8-sample top row split into two groups).
- `predict_8x8c_dc_left_neon`: same using the eight left samples.
- `predict_8x8c_dc_neon`: combines top and left sums, producing four quadrant averages with shifts 2/3, then writes four 8-byte rows per quadrant group.
- `predict_8x16c_dc_neon`: extends to 16 rows with six left-sample groups; computes four DC values (shift 3) and writes 16 rows.
- `predict_8x16c_dc_top_neon`: top-only variant; produces two 8-byte halves per row from two averages.
- `predict_8x16c_dc_left_neon`: left-only variant; computes four groups of eight left samples (shift 2) and writes four rows per group.

### Chroma H/V modes

- `predict_8x8c_h_neon`, `predict_8x16c_h_neon`: broadcast each of the eight left neighbours across a row (or pair of rows for 8×16).
- `predict_8x8c_v_aarch64`, `predict_8x16c_v_neon`: copy one 8-byte top row to all 8/16 rows using 64-bit loads/stores.

### Plane modes (H.264 I_Pred_8x8c_P / 16x16_P)

These implement the standard plane equation:
`pred[x,y] = Clip1((a + b*(x−7) + c*(y−7) + 16) >> 5)` for 16×16 (with appropriate offsets for 8×8/8×16), where:
- `a` is the average of the top-right and bottom-left corner samples (with weighting),
- `b` is a weighted sum of horizontal differences (top row),
- `c` is a weighted sum of vertical differences (left column).

The NEON implementations use the weight tables:
1. Load top row and left column (reversed as needed), compute pairwise differences, multiply by weights (`p8weight` 1..4 or `p16weight` 1..8), pairwise-add and fold (`saddlp`, `addp`) to get `b` and `c` with shifts 5/6.
2. Compute `a` from the corner values with another weighted sum (`a = (sum(top-left..top-right) + sum(left...) + 16) >> 4` style).
3. Derive the initial pixel value `pix = a − 3*(b+c)` (or `a − 7*(b+c)` for 16×16) and the per-row increment `c` and per-column increment `b`.
4. Generate each row in 16-bit lanes by adding the column-increment vector (0,1,2,3,… or 0..7) times `b` to the row base, narrowing with `sqshrun`/`sqrshrun #5` (signed saturating shift right with rounding) to produce bytes, and incrementing the row base by `c` for the next row.

This avoids per-pixel multiply operations: the row vector is computed once and only an add per row is needed.

### 16×16 modes

- `predict_16x16_v/h_neon`: copy one top row / broadcast left column across 16 rows.
- `predict_16x16_dc_neon`, `_dc_top_neon`, `_dc_left_neon`: average 32 samples (shift 5) or 16 samples (shift 4), fill 16 rows of 16 bytes.
- `predict_16x16_p_neon`: the plane predictor described above with `p16weight`, producing two 8-pixel halves per row (`sqshrun`/`sqshrun2`) over 16 rows.

### Plane prediction derivation

For an `N×N` block with top row `T[x]` (`x = 0..2N−1`) and left column `L[y]` (`y = 0..2N−1`), H.264 defines:

```
a = (T[N−1] + L[N−1]) >> 1
H = sum_{x=0..N−2} (x+1) * (T[N+x] − T[N−2−x])
V = sum_{y=0..N−2} (y+1) * (L[N+y] − L[N−2−y])
b = (H + 2^(k)) >> (k+1)
c = (V + 2^(k)) >> (k+1)
pred[x,y] = Clip1( (a + b*(x − (N−1)) + c*(y − (N−1)) + round) >> shift )
```

For 16×16, `N = 8`, the weights are 1..8, `shift = 5`, and the corner term contributes `16`. For 8×8 chroma, weights are 1..4 (two segments), `shift = 5`. For 8×16 chroma, the horizontal weights are 1..8 and the vertical grouping uses four 8-sample groups.

The NEON kernels compute `H` and `V` by widening the boundary differences, multiplying by the weight vectors (`p8weight`/`p16weight`), and reducing with `saddlp`/`addp`. `b` and `c` are produced with `rshrn #5` / `#6`. The corner value `a` comes from a separate weighted sum, and the initial pixel is `a − 3*(b+c)` (8×8) or `a − 7*(b+c)` (16×16). Each output row is then a vector of `initial + i*b` for `i = 0..N−1`, narrowed with `sqshrun #5`; the row base advances by `c` after each row.

### DC-mode shift table

| Mode | Samples summed | Shift | Rounding |
| --- | --- | --- | --- |
| 4×4 DC | 8 (top + left) | 3 | `rshrn` |
| 4×4 DC_TOP | 4 (top only) | 2 | `rshrn` |
| 8×8 luma DC | 16 (top + left) | 4 | `rshrn` |
| 8×8 chroma DC | 16 per quadrant group | 2/3 | `rshrn` |
| 16×16 DC | 32 | 5 | `rshrn` |
| 8×16 chroma DC | 8 per group | 3 | `rshrn` |

`rshrn` adds the rounding bit before shifting, matching H.264's `(sum + (1<<(shift−1))) >> shift`.

### Directional filtered modes

The 8×8 luma filtered modes (DDL, DDR, VL, VR, HD, HU) operate on the 36-byte `edge` array prepared by the common boundary filter. Each mode forms for every output sample a two- or three-tap weighted average of two/three boundary samples with weights 1:2:1 or 1:1 (implemented by `uhadd`/`urhadd`). The eight rows are then generated by rotating a precomputed vector with `ext` by 1..8 bytes rather than recomputing the filter per row.

- DDL/DDR: diagonal taps from the top and left edges.
- VL/VR: vertical prediction blended with one left/top neighbour.
- HD/HU: horizontal prediction blended with one top/bottom neighbour, with `zip1`/`zip2` interleaving the two precomputed half-rows into the eight output rows.

### Mode coverage

- 4×4 luma: 6 modes (V, H, DC, DC_TOP, DDL, DDR) — the remaining intra_4x4 modes are computed by the generic C code.
- 8×8 luma: 9 filtered modes.
- 8×8 / 8×16 chroma: 6 modes each (V, H, DC, DC_TOP, DC_LEFT, P).
- 16×16 luma: 6 modes (V, H, DC, DC_TOP, DC_LEFT, P).

## 6. Dependencies

- Includes `asm.S` for strides and symbol macros; uses `movrel` for the weight tables.
- Dispatched from `predict-c.c` via `predict.h`.
- 8×8 luma functions depend on the pre-filtered `edge[36]` array produced by the common code (the filter itself is not in this file).

## 7. Constants and tables

- `p8weight`: `short 1,2,3,4,1,2,3,4` — plane weights for 8×8 chroma (two groups of four).
- `p16weight`: `short 1,2,3,4,5,6,7,8` — plane weights for 8×16 chroma / 16×16 luma.
- `FDEC_STRIDE = 32`.
- Shift amounts: `2`, `3`, `4`, `5`, `6` for DC averaging and plane coefficient scaling.
- `16`: plane rounding bias.

## 8. Performance notes

Medium-hot: every intra candidate block invokes a predictor. The plane modes are the most expensive and benefit most from SIMD: the reference computes a row of 8/16 predictions in one vector and uses a single add per subsequent row. Mode-specific early-outs are minimal; all modes have fixed iteration counts.

Optimization tips for a clean-room AVX2 port: use `_mm_set1_epi8` for DC/H modes, `_mm_madd_epi16` with the weight vectors for plane coefficients, and `_mm_packus_epi16` for final byte saturation; precompute the per-column increment vector once. SVE tip: predicated loads of boundary columns can replace the `ldcol` macros, and wider SVE vectors could compute two rows at once, but the reference's fixed 8/16-byte outputs should be reproduced first. Public references: H.264 intra prediction specification; ARM NEON `sqshrun`/`urhadd`/`ld1r` docs.

## 9. References

- ITU-T H.264 / ISO/IEC 14496-10, Clause 8.3 (intra prediction), including the plane prediction formula and filtered diagonal modes.
- ARM NEON instruction reference (`ld1r`, `urhadd`, `uhadd`, `sqshrun`, `saddlp`, `movrel` addressing).
- `docs/rebuild/common/aarch64/predict-c.c.md`, `predict.h.md`.

## 10. Verification checklist

- For every mode, compare output against the generic C predictor for random boundary samples; byte-identical required.
- Plane modes: verify the `a/b/c` derivation at the exact half-integer boundaries (rounding direction matters).
- Verify DC modes' rounding shifts (top-only shift 2/4, combined shift 3/5).
- Verify 8×16 chroma writes 16 rows and uses the correct left-sample groups.
- Verify `edge[36]` offsets for the 8×8 luma filtered modes (vl/vr/hd/hu) against the standard boundary layout.
- Verify saturation: extreme boundary values (0 and 255) must clamp to byte range in plane modes.

## 11. Open questions

- UNKNOWN: exact mapping of `edge[36]` indices for the four filtered 8×8 modes (requires cross-check with the common prediction filter that builds the array).
- UNKNOWN: whether any of the `_aarch64` suffixed functions are also used via the merged `_neon` aliases in `predict.h` (the header defines aliases for 4×4 H/V and 8×8c V).
