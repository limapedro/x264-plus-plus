# `common/set.c` - scaling-list and quantisation-table setup

## 1. Header

Source path: `E:\_x264_dirty_stash\x264-source\common\set.c` (mirrored at `common/set.c`). Role in one sentence: converts the chosen custom or default scaling lists into all dequantisation, quantisation, trellis-reciprocal, bias and emergency-denoise tables, and parses JM-style matrix files. Marker: **dirty-side behavioural specification**.

## 2. Purpose

H.264 quantisation uses per-coefficient scaling factors plus QP-dependent scale tables. Computing them on the fly per macroblock would be far too slow, so `set.c` expands them once into flat arrays indexed by QP and coefficient. It also enforces that the resulting tables are usable at the configured QP range and builds the special "emergency" offsets used when QP exceeds the specification limit.

## 3. Public interface

`int x264_cqm_init(x264_t *h)`, `void x264_cqm_delete(x264_t *h)`, `int x264_cqm_parse_file(x264_t *h, const char *filename)` - contracts are in `set.h.md`. Additional internal macros/constructs: `SHIFT(x,s)` performs an arithmetic left shift for non-positive `s` and a rounded right shift otherwise; `DIV(n,d)` is integer division with round-half-up for positive operands; `MAX_MF` caps quant factors according to bit depth.

## 4. State and data structures

Input state: `h->sps->scaling_list[0..7]`, `h->param.analyse.i_luma_deadzone[0..1]`, `h->param.analyse.b_transform_8x8`, `h->sps->i_chroma_format_idc`, `h->mb.b_lossless`, `h->chroma_qp_table`, and the rate-control QP bounds. Output state: the matrix pointer arrays in `x264_t` (dequant4/8_mf, unquant4/8_mf, quant4/8_mf, quant4/8_bias, quant4/8_bias0, nr_offset_emergency) plus mutated QP bounds in `h->param.rc`. All outputs are freed by `x264_cqm_delete`.

There are four 4x4 sets always, and zero/two/four 8x8 sets depending on configuration. The bias tables are shared only when both the list and the deadzone match, whereas the other matrices are shared on list equality alone. `nr_offset_emergency` is a single allocation indexed by `QP_MAX - QP_MAX_SPEC` QP slots and up to four categories.

## 5. Algorithm / behaviour

**Step 1 - deadzones and set count.** Inter/intra luma deadzones are `32 - i_luma_deadzone[1]` and `32 - i_luma_deadzone[0]`; chroma inter/intra are 21 and 11. Number of 8x8 sets: four for 4:4:4, otherwise two if 8x8 transforms are enabled, otherwise zero.

**Step 2 - allocation with aliasing.** For each set, compare the raw scaling-list bytes with all earlier sets; on a match, alias the earlier `quant*_mf`, `dequant*_mf` and `unquant*_mf` pointers. For bias tables, additionally require the same deadzone. Otherwise allocate `(QP_MAX_SPEC+1)*size` elements for quant/unquant/bias tables and `6*size` elements for dequant, where size is 16 or 64. Failure at any point calls `x264_cqm_delete` and returns -1.

**Step 3 - flat reference scales.** For QP residue q = 0..5, 4x4 dequant and quant scales are indexed by `j = (i&1) + ((i>>2)&1)`, giving three values per row drawn from small integer tables (e.g. dequant row 0 is 10/13/16, quant row 0 is 13107/8066/5243). For 8x8, `j` is taken from a 16-byte permutation of `((i>>1)&12) | (i&3)`; the six-value rows of the 8x8 scale tables are similarly fixed integers.

**Step 4 - apply the list.** Each dequant entry is the reference dequant multiplied by the corresponding list coefficient. Each quant entry is `(reference_quant*16 + coefficient/2) / coefficient` (round-half-up).

**Step 5 - per-QP expansion.** For QP q from 0 to `QP_MAX_SPEC`:
- `unquant[q][i] = (1ULL << (q/6 + 15 + 8)) / quant4_mf[q%6][i]` (8x8 uses `q/6 + 16 + 8`). The extra 8 bits leave room for fractional precision in trellis RD.
- `quant[q][i] = SHIFT(quant4_mf[q%6][i], q/6 - 1)` (8x8 uses `q/6`), cast to 16 bits. A zero result records `min_qp_err`.
- `bias[q][i] = min(DIV(deadzone<<10, factor), (1<<15)/factor)`; `bias0[q][i] = (1<<15)/factor`. The cap prevents a negative effective deadzone.
- If the factor exceeds `MAX_MF`, record the largest offending QP separately for luma and chroma lists.

**Step 6 - emergency denoising.** Allocate one slot per QP above `QP_MAX_SPEC`. Within a slot, categories 0..(2+CHROMA444) are processed; odd categories (8x8) are skipped when 8x8 transforms are disabled. For each coefficient, if this is the last slot, set the offset to the per-pixel maximum `(1<<(7+BIT_DEPTH))-1`. Otherwise derive a threshold: DC coefficients use roughly two thirds of the available QP range, luma AC coefficients the same, chroma AC coefficients zero. Below the threshold the offset is zero; above it, compute a position in 0..1 and an exponential bias `pow(2, position*range/10)*0.003 - 0.003` times the unquant value at `QP_MAX_SPEC`, then add 0.5 and clamp to the maximum. The formula is explicitly heuristic, tuned for random-noise input.

**Step 7 - QP-range repair (non-lossless only).** Raise `i_qp_min` until the chroma-mapped QP at that minimum exceeds the maximum chroma factor error. Lower `i_qp_max` to just below `min_qp_err` if necessary, then raise `i_qp_min` above `max_qp_err` if necessary. For CAVLC below High profile, keep raising `i_qp_max` until both the QP and its chroma mapping exceed 12, preventing long level codes. If `i_qp_min > i_qp_max`, log an error and return -1.

**File parsing.** The file is slurped; every `#`-to-end-of-line span is blanked. `cqm_parse_jmlist` searches for a variable name, skips an optional `U` or `V` suffix after it, finds the next `INT` token as a stop marker, and scans decimal numbers separated by whitespace or commas. If the first coefficient is zero it copies the provided JVT default; otherwise it requires exactly `length` coefficients in 1..255. A missing name fills 16. Parsed names: `INTRA4X4_LUMA`, `INTER4X4_LUMA`, both `INTRA4X4_CHROMA` and `INTER4X4_CHROMA` (applied to both chroma lists), `INTRA8X8_LUMA`, `INTER8X8_LUMA`, plus `INTRA8X8_CHROMA`/`INTER8X8_CHROMA` in 4:4:4 mode.

## 6. Dependencies

Includes `common.h`. Uses `tables.h` JVT arrays, `base.h` allocation/error macros, `x264_slurp_file`, `x264_log`, `x264_emms` before SIMD-read tables, `x264_free`, and the chroma-QP table. Called by encoder open after SPS setup and by parameter handling of custom matrices.

## 7. Constants and tables

Reference dequant 4x4 rows (QP mod 6) use triples such as (10,13,16), (11,14,18), (13,16,20), (14,18,23), (16,20,25), (18,23,29); quant 4x4 rows are (13107,8066,5243), (11916,7490,4660), (10082,6554,4194), (9362,5825,3647), (8192,5243,3355), (7282,4559,2893). The 8x8 tables have six values per row and are indexed through a 16-byte permutation; their exact integers are fixed H.264 constants and are not reproduced here. `MAX_MF = min(0xffff, (1<<(25-BIT_DEPTH))-1)`. Emergency thresholds use `2/3` of the extra QP range.

## 8. Performance notes

- All work is at encoder-open time; the QP-indexed outputs are read in trellis and quantisation hot loops. Keep the `[qp][coef]` layout so one QP's coefficients are contiguous and prefetch-friendly.
- **Cache layout:** sharing identical lists reduces the number of distinct cache lines touched by trellis quantisation; keep the aliasing rule.
- **Allocation strategy:** one allocation per distinct list-type pair, not per list; deletion must mirror this to avoid double frees.
- **False sharing:** tables are immutable after init and safely shared by all frame threads.
- **Dispatch hierarchy:** assembly quantisation kernels consume these arrays directly; element widths and alignment are part of the ABI between this file and the SIMD code.
- Public references: H.264 clause 8.5.1, x264 `doc/ratecontrol.txt`, Drepper (2007) on array locality.

## 9. References

- ITU-T H.264 clause 8.5.1 (dequantisation), Annex E (scaling matrices), clause 7.4.2.1.1.
- JM reference software CQM file format.
- Public `x264.h` CQM presets and rate-control fields.
- x264 `doc/ratecontrol.txt`.

## 10. Verification checklist

- Independently compute H.264 dequant values for flat matrices at QP 0, 6, 12, 26 and 51; compare with the derived tables.
- Verify the per-QP reciprocal relationships (unquant times quant approximately a power of two) stay within expected precision for all QPs.
- Check bias tables never exceed `(1<<15)/factor` and that a zero factor produces zero quant and no division by zero.
- Construct two identical custom lists and verify pointer aliasing plus a clean run under a memory checker at teardown.
- Verify QP-bound repair with a deliberately extreme matrix: the resulting min/max must respect all documented constraints or an error must be returned.
- Parse a JM file with comments, a missing list, a zero-first-coefficient list, and an invalid coefficient; verify each documented outcome.
- Verify 8x8 emergency categories are skipped when `b_transform_8x8` is off and present when on.

## 11. Open questions

- UNKNOWN: exact integer contents of the 8x8 scale tables and position permutation are intentionally omitted; obtain them from the H.264 standard or an independently derived implementation.
- UNKNOWN: whether the `x264_emms()` call before emergency allocation is still required on modern x86; it is a historical safeguard.
- UNKNOWN: how `i_qp_min`/`i_qp_max` changes interact with later rate-control reconfiguration; not specified here.
