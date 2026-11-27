# `common/aarch64/dct-a.S` — NEON transforms, inverse transforms, and zig-zag

> Dirty-side behavioural specification (reverse-engineered from the reference encoder; describes behaviour, not code).

## 1. Header

- Source path: `common/aarch64/dct-a.S`
- Role: complete NEON implementation of x264's forward/inverse 4×4 and 8×8 integer transforms (including DC-only variants) and of the zig-zag scan/fused-subtract/CAVLC-interleave helpers.
- This is a **dirty-side behavioural specification** artifact for the clean-room reimplementation.

## 2. Purpose

Residual coding requires, for every block: compute `source − prediction`, apply a separable integer transform, scan coefficients in zig-zag order, and on reconstruction apply the inverse transform, add the predictor, round, and saturate. This file provides the AArch64 NEON kernels for all of those steps for 8-bit pixels. A separate SVE/SVE2 pair overrides only two of them; all others come from here.

## 3. Public interface

Exported symbols correspond to `dct.h` (see that specification):
- `dct4x4dc_neon`, `idct4x4dc_neon`
- `sub4x4_dct_neon`, `sub8x8_dct_neon`, `sub16x16_dct_neon`
- `sub8x8_dct8_neon`, `sub16x16_dct8_neon`
- `add4x4_idct_neon`, `add8x8_idct_neon`, `add16x16_idct_neon`
- `add8x8_idct8_neon`, `add16x16_idct8_neon`
- `add8x8_idct_dc_neon`, `add16x16_idct_dc_neon`
- `sub8x8_dct_dc_neon`, `sub8x16_dct_dc_neon`
- `zigzag_interleave_8x8_cavlc_neon`
- `zigzag_scan_4x4_frame_neon`, `zigzag_scan_4x4_field_neon`
- `zigzag_scan_8x8_frame_neon`, `zigzag_scan_8x8_field_neon`
- `zigzag_sub_4x4_{frame,field}_neon`, `zigzag_sub_4x4ac_{frame,field}_neon`
- `zigzag_sub_8x8_{frame,field}_neon`

Internal helpers (not exported): `sub8x4_dct_neon`, `add8x4_idct_neon`, `satd`-like end helpers, and the constant tables.

Calling conventions are fixed: `FENC_STRIDE = 16` for source rows, `FDEC_STRIDE = 32` for reconstruction rows. Coefficient arrays are signed 16-bit row-major.

## 4. State and data structures

- All computation is register-resident (16 NEON `v` registers available, heavily reused).
- The file defines eight read-only lookup tables: `scan4x4_frame`, `scan4x4_field`, `sub4x4_frame`, `sub4x4_field`, `scan8x8_frame`, `scan8x8_field`, `sub8x8_frame`, `sub8x8_field`.
- The `scan*` tables are byte-index tables (one byte per output coefficient) used with `tbl` to permute 16-bit coefficients. Because `tbl` indexes bytes, the frame tables use doubled indices (`2*z`, `2*z+1`) to address halfword pairs.
- The `sub*` tables are similarly byte-index tables that reorder loaded pixel rows so that differences can be computed in a convenient vector layout.

## 5. Algorithm / behaviour

### Helper macros

- `SUMSUB_SHR`/`SUMSUB_SHR2`: compute `sum = a + (b >> shift)` and `sub = (a >> shift) − b`, or the mirrored form. Used by the 8-point transforms. Arithmetic (`sshr`) shifts preserve sign.
- `SUMSUB_15`: adds/subtracts 1.5 times a value by computing `x + (x >> 1)`, i.e. `1.5*x` with truncation toward zero for negative values. Used in the inverse 8×8 transform.

### `dct4x4dc_neon` and `idct4x4dc_neon`

These operate in place on a 16-halfword DC block. Both apply two passes of a 4-point butterfly (sum/difference pairs, transpose, sum/difference pairs, transpose), producing a Hadamard-like transform.

The forward version then applies an offset-and-round trick: it adds 1 to certain intermediate lanes (`v31.4h = 1`), then uses `srhadd` (rounding halving add) and `shsub` (halving subtract with the +1-adjusted operand) to produce the final scaled outputs. The inverse version applies the final butterfly without the offset and returns the original block up to scale. No saturation occurs; inputs/outputs are halfwords.

### `sub4x4_dct_neon`

Loads four 4-byte rows from source and reconstruction using single-lane loads with strides 16 and 32, forms unsigned byte differences widened to 16-bit (`usubl`), applies `DCT_1D`, transposes 4×4 halfwords, applies `DCT_1D` again, and stores 16 halfwords. No rounding or shift.

### `sub8x4_dct_neon` (internal) and `sub8x8_dct_neon` / `sub16x16_dct_neon`

`sub8x4_dct_neon` processes an 8×4 residual: loads four rows of eight bytes per plane, widens differences, applies the 4-point butterfly at 8-wide, transposes 4×8 halfwords, then applies an 8-point butterfly built from sums/differences and doublings (`2*x`), and finally zips the results into four 8-halfword output vectors (one 4×4 coefficient block each).

`sub8x8_dct_neon` calls `sub8x4_dct_neon` twice (top and bottom halves) with the link register saved in a callee-saved register. `sub16x16_dct_neon` calls it eight times, walking the macroblock in 8×4 tiles with pointer adjustments (`−8*stride+8` and `−8`) to move between tile columns and rows. Output ordering: 16 coefficient blocks for 16×16, four for 8×8.

### `sub8x8_dct8_neon` / `sub16x16_dct8_neon`

High-profile 8×8 forward transform. `DCT8_1D` computes the eight-point DCT using three sum/difference pairs, four intermediate combinations (`a0..a7`), the 1.5× trick via `sshr #1` plus `add`, and final `SUMSUB_SHR` steps with shifts 2 and 1. `sub8x8_dct8_neon` loads eight rows of eight bytes, forms differences, applies `DCT8_1D`, does a full 8×8 halfword transpose (`transpose8x8.h`), applies `DCT8_1D` again, and stores 64 halfwords. `sub16x16_dct8_neon` calls it four times with pointer rewinding.

### `add4x4_idct_neon`, `add8x4_idct_neon`, `add8x8_idct_neon`, `add16x16_idct_neon`

`IDCT_1D` is the inverse butterfly: from four halfword inputs it produces `a+b`, `a−b`, `(c>>1)−d`, `(d>>1)+c`. `add4x4_idct_neon` loads 16 coefficients, applies `IDCT_1D`, transposes 4×4, applies `IDCT_1D` again, then performs `srshr #6` (arithmetic shift right by 6 with rounding) on each result, widens four predictor rows from bytes to halfwords (`uaddw`), adds, and saturates to unsigned bytes (`sqxtun`). Stores rows in order 0, 1, 3, 2 — the swap is required by the transpose register ordering.

`add8x4_idct_neon` handles 8×4 blocks: it first transposes two 8-halfword vectors to get columns, applies `IDCT_1D` at 8-wide, transposes 4×8, applies `IDCT_1D` again, rounds with `srshr #6`, widens four predictor rows, saturates, and stores four rows of eight bytes. `add8x8_idct_neon` calls it twice; `add16x16_idct_neon` calls it eight times with pointer rewinding.

### `add8x8_idct8_neon` / `add16x16_idct8_neon`

Inverse 8×8 transform. `IDCT8_1D` is the inverse of `DCT8_1D`: it builds the eight contributions using `SUMSUB_SHR` and `SUMSUB_15`, with shifts 1 and 2 and the 1.5× trick, then combines them with a final butterfly. The kernel loads 64 coefficients, applies `IDCT8_1D`, transposes 8×8, applies it again, rounds with `srshr #6`, widens eight predictor rows, saturates, and stores. The 16×16 version calls it four times with pointer rewinding.

### DC-only paths

- `add8x8_idct_dc_neon`: takes four DC coefficients (one per 4×4 block of an 8×8), rounds each with `srshr #6`, duplicates each into an eight-lane vector, forms the negative, widens eight predictor rows, applies saturating byte add (`uqadd`) for the top four rows and saturating byte subtract (`uqsub`) with the negated value for the bottom four rows, then stores. Each 4×4 quadrant gets its own DC value.
- `add16x16_idct_dc_neon`: same idea at 16-wide: rounds 16 DC values, then uses the `ADD16x4_IDCT_DC` macro four times, each handling four 16-byte rows with a distinct DC value, using `uqadd`/`uqsub` and storing through a second pointer.
- `sub8x8_dct_dc_neon`: sums the eight pixel differences of each 4×4 block (`sub4x4x2_dct_dc`), producing two vectors of four partial DC sums. It then performs three transpose+butterfly stages and pairwise adds (`addp`) to reduce the 8×8 block to four DC coefficients, storing four halfwords.
- `sub8x16_dct_dc_neon`: extends the above to two 8×8 blocks (eight DC values) by summing four groups, transposing at 4-word / 2-dword granularity, and pairwise adding.

### Zig-zag interleave and scans

- `zigzag_interleave_8x8_cavlc_neon`: loads the source as two groups of four interleaved 8-halfword vectors, computes the per-coefficient maximum magnitude across the four blocks by pairwise `umax`/`umaxp`, stores the de-interleaved coefficients as eight 16-byte stores, reduces the magnitude vector with `cmhs`/`and`, and writes four flag bytes at sparse offsets (0, 7, 8, 12 relative to the `nnz` pointer, using a stride-7 store for the second flag). Each flag is 1 if the corresponding 8×8 block has any non-zero coefficient.
- `zigzag_scan_4x4_frame_neon` / `_field_neon`: load 16 halfwords and permute them with `tbl` using the corresponding byte-index table, then store 16 halfwords. The frame table implements the standard zig-zag order; the field table implements field-order scanning.
- `zigzag_sub_4x4_<field|frame>_neon` (macro-generated, with optional `ac`): load four source rows (stride 16) and four reconstruction rows (stride 32), reorder both by the `sub4x4_*` table, compute unsigned byte differences widened to halfwords, write the source rows back to the reconstruction plane, compute the maximum absolute difference with `umax`/`umaxv`, and return 1 if any difference is non-zero else 0. The `ac` variant additionally stores the first (DC) difference through the `dc` pointer and zeroes that coefficient in the output.
- `zigzag_scan_8x8_frame_neon` / `_field_neon`: same principle at 64 halfwords, using 4-register `tbl` lookups into `scan8x8_*` tables, with a handful of manual lane insertions for indices that fall outside the four-register table window.
- `zigzag_sub_8x8_<field|frame>_neon`: loads 8×8 source and reconstruction rows, reorders both with `sub8x8_*` tables, forms halfword differences, writes the source rows back to reconstruction, computes the maximum magnitude with nested `umax` reductions, stores 64 halfwords, and returns the non-zero flag.

Tables: `scan4x4_frame`/`scan4x4_field` map output index to byte-offset in the coefficient array; `sub4x4_frame`/`sub4x4_field` and their 8×8 counterparts map row positions for the fused subtract. The 8×8 scan tables are built from macro-generated `T(x,y)` expressions that encode the zig-zag traversal of an 8×8 matrix; the field variant traverses alternating fields.

Rounding/saturation summary: forward transforms have no shift or saturation; inverse transforms use `srshr #6` (rounding) followed by widening add and unsigned saturating narrow; DC-only paths use `uqadd`/`uqsub` for byte saturation; DC forward transforms use rounding halving adds with an explicit +1 adjustment.

## 6. Dependencies

- Includes `asm.S` (symbol macros, strides, transposes) and `dct-a-common.S` (`DCT_1D`).
- Uses `SUMSUB_AB` from `asm.S`.
- Dispatched from `common/dct.c` via `dct.h`.
- Internal calls use the link register; helpers save/restore it in `x5`/`x7` where nested calls are needed.

## 7. Constants and tables

- `FENC_STRIDE = 16`, `FDEC_STRIDE = 32`.
- Shift immediates: `1`, `2`, `6` (inverse scaling), `3`/`2` (DC averaging), shifts in `SUMSUB_SHR` macros.
- Eight byte-index tables described in section 4. The tables encode only permutations and are not numerical coefficients.
- Mask/constant vectors: `1` (DC bias), `8` (coefficient count in last-stage reduction), `0x01` (non-zero flag threshold).

## 8. Performance notes

Among the hottest kernels in the encoder: every residual block passes through a forward transform, and every coded block through an inverse transform. Register pressure is high (8×8 transforms use nearly all 32 NEON registers), so the reference keeps pointer adjustments and link-register saves in general-purpose registers and unrolls row loops with `.rept`. Branch behaviour is deterministic (no data-dependent branches except the scan flag reduction).

Optimization tips for a clean-room AVX2 port: implement the 4×4 transform with a single 128-bit butterfly per row/column pair; use `_mm_cvtm88_ss`? (not applicable) — prefer `_mm_srai_epi16` + add for rounding and `_mm_packus_epi16` for saturation. For 8×8, AVX2's 256-bit width can hold a full row pair; use `_mm256_unpacklo/hi_epi16` transposes. The scan tables map naturally to `_mm_shuffle_epi8` (SSSE3) with doubled indices. SVE/SVE2 tip: the reference's SVE overrides only two functions; a broader port could use predicated widening loads for all block sizes, but must preserve exact rounding and output ordering. Public references: H.264 integer transform papers (Malvar et al.), ARM NEON transpose patterns, x264 developer documentation.

## 9. References

- ITU-T H.264 / ISO/IEC 14496-10, Clause 8.5 (integer transforms), Clause 9.2 (zig-zag scanning), Annex C (CAVLC).
- H.264 transform design literature (Malvar, Hallapuro, Karczewicz, Kerofsky, "Low-complexity transform and quantization in H.264/AVC").
- ARM NEON instruction reference (`usubl`, `srshr`, `sqxtun`, `uqadd`, `tbl`, `trn`, `zip`, `uzp`).
- `docs/rebuild/common/aarch64/dct-a-common.S.md`, `dct.h.md`, `dct-a-sve.S.md`, `dct-a-sve2.S.md`.

## 10. Verification checklist

- For random byte blocks, `sub4x4_dct_neon`, `sub8x8_dct_neon`, and `sub16x16_dct_neon` match a scalar H.264 transform exactly.
- Forward/inverse round-trip: transform then inverse-transform a zero predictor yields the original residual scaled by the expected quantization-independent factor (test with tolerance consistent with H.264).
- Inverse outputs saturate correctly for extreme coefficients (force negative and >255 sums).
- Row-store order (0,1,3,2) verified with distinct predictor row values; final reconstructed block must match reference bitstreams.
- Zig-zag scans match the standard scan order (compare against the H.264 tables, not against another implementation's tables).
- `zigzag_sub_*` return flag equals `any(level != 0)`; `ac` variant writes the DC value to the pointer and zeroes the first output coefficient.
- `zigzag_interleave_8x8_cavlc_neon` flag bytes match the C reference's `nnz` values at all four offsets.
- 16×16 multi-block pointer rewinding verified by checking all 16 sub-blocks against scalar computation.

## 11. Open questions

- UNKNOWN: whether the `nnz` flag offsets (0, 7, 8, 12) in the NEON interleave are consumed with exactly that spacing by all callers; needs cross-check with the common C code.
- UNKNOWN: overflow behaviour of `SUMSUB_15` for coefficient magnitudes beyond the range emitted by x264's quantizer.
- UNKNOWN: whether the `sub8x4_dct_neon` helper is ever called directly from outside this file (it is not exported, so likely not).
