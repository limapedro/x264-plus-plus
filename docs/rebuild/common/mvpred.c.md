# mvpred.c — dirty-side behavioural specification

Source path: `common/mvpred.c`
Role: implements every motion-vector prediction rule used by the encoder: normal inter prediction, P-skip prediction, B direct-mode prediction (temporal and spatial), and the ME predictor list builder.

> Dirty-side behavioural specification. Describes observable behaviour and structure only; no source text is reproduced.

## 1. Header / identity

This file contains the decision logic that selects a predictor MV for a block from already-coded neighbours or from a colocated frame. It does not perform motion estimation or compensation; it reads the neighbour cache prepared by `macroblock.c` and writes either a predictor into a caller-supplied array or reference indices/MVs into the current macroblock's cache. Direct-mode prediction is special: it *populates* the current MB's MV/ref cache (and partition type) rather than returning a single vector, because B_SKIP/B_DIRECT inherit a full set of partition MVs.

## 2. Purpose

H.264 codes motion vectors differentially: the bitstream carries `mvd = mv - mvp`, so decoder and encoder must agree exactly on `mvp`. The rules depend on partition shape, reference-index equality, neighbour availability, field parity, and — for B direct mode — on the colocated macroblock in list 1. This file encodes those rules, including the MBAFF correction that converts neighbour MVs between frame and field coordinates, and the optional "col zero" shortcut that zeroes a predictor when the colocated list-1 MV is tiny.

One function, `x264_mb_predict_mv_ref16x16`, is explicitly documented as an encoder-side performance optimisation, not part of the bitstream specification: it builds a list of candidate MVs to seed motion search.

## 3. Public interface

| Function | Contract |
|---|---|
| `x264_mb_predict_mv(h, list, idx, width, mvp[2])` | For any non-skip/non-direct block `idx` with known `width`, write the median-rule MV predictor into `mvp`. Requires valid ref/partition/sub-partition for the current block and valid MV/ref for neighbours. |
| `x264_mb_predict_mv_16x16(h, list, ref, mvp[2])` | Predictor for a 16×16 block (also used internally by P-skip). `ref` is the current block's reference index. |
| `x264_mb_predict_mv_pskip(h, mv[2])` | Special P-skip rule: zero if a neighbour is unavailable or has a zero MV/ref pair; otherwise delegates to the 16×16 rule with `ref = 0`. |
| `x264_mb_predict_mv_direct16x16(h, b_changed)` | For B_SKIP/B_DIRECT, fill `cache.mv`/`cache.ref` (and `i_partition`) from either temporal or spatial direct prediction. Returns 1 on success, 0 on failure. If `b_changed` is non-NULL, set it to whether the result differs from the cached direct values. |
| `x264_mb_predict_mv_ref16x16(h, list, ref, mvc, i_mvc)` | Append ME seed vectors to `mvc` and write their count to `i_mvc`. Not part of the spec. |

Internal helpers: `mb_predict_mv_direct16x16_temporal(h)`, `mb_predict_mv_direct16x16_spatial(h, b_interlaced)`, and the two thin wrappers selecting interlaced/progressive spatial mode.

## 4. State and data structures

Inputs are the neighbour cache arrays declared in `common.h` and indexed through `x264_scan8`:

* `cache.ref[list][slot]` — reference index; `-2` means unavailable, `-1` means intra/unused.
* `cache.mv[list][slot][2]` — motion vector; zero when unavailable.
* `topright_mv[list][0..2]`, `topright_ref[list][0..2]` — MBAFF-specific fallback neighbours from the left macroblock.
* `direct_mv[2][4][2]`, `direct_ref[2][4]`, `direct_partition` — cached result of the last direct prediction, used to detect changes.
* `h->mb.i_partition`, `h->mb.i_sub_partition[4]`, `h->mb.field[]`, `h->mb.i_mb_*` coordinates, `h->fref[1][0]` colocated frame arrays (`mb_type`, `mb_partition`, `ref`, `mv`, `field`, POC/delta POC), `h->mb.dist_scale_factor[ref][0]`, `map_col_to_list0[]`, `h->mb.mvr[list][ref][xy]`, `h->mb.mv_max_spel`.

Candidate naming convention for a block at scan8 slot `p`:

| Name | Slot | Meaning |
|---|---|---|
| A | `p - 1` | left neighbour |
| B | `p - 8` | above neighbour |
| C | `p - 8 + width` | above-right neighbour (width in 4×4 blocks) |
| D (fallback) | `p - 8 - 1` | above-left neighbour, used when C is unavailable |

For a 16×16 block at `X264_SCAN8_0` (slot `4+8`), C is at slot `+4`, i.e. the top-right 8×8's left 4×4.

Invariant: unavailable neighbours carry `ref == -2`, so a single comparison decides availability; MV values paired with `-2` are always zero.

## 5. Algorithm / behaviour

### 5.1 General MV prediction (`x264_mb_predict_mv`)

1. Compute `p = scan8[idx]`. Read refs and MVs for A (`p-1`), B (`p-8`) and C (`p-8+i_width`).
2. **C replacement rule.** If `(idx & 3) >= 2 + (i_width & 1)` **or** `refC == -2`, replace C with the top-left neighbour D (`p-8-1`). The first condition means "this partition starts to the right of the top neighbour's coverage given its width", i.e. the nominal above-right 4×4 belongs to a partition not yet coded in scan order; the second means the above-right neighbour is unavailable. In MBAFF, when the left neighbour exists and the current MB's field status differs from the left MB's, special `topright_*` entries are substituted for blocks `idx = 2, 8, 10` (the first block of each lower row / column in scan order).
3. **Partition shortcuts**, applied before the general rule:
   * `D_16x8`: for the first half (`idx == 0`), if `refB == current ref`, copy MV B and return. For the second half, if `refA == current ref`, copy MV A and return. (A 16×8 partition has its top neighbour directly above and its left neighbour directly beside.)
   * `D_8x16`: for the left half, if `refA == current ref`, copy A; for the right half, if `refC == current ref`, copy C.
   These shortcuts implement the standard's special case for partitions split horizontally or vertically, where using the adjacent partition's MV is preferred over a three-way median.
4. Count matching references: `n = (refA == ref) + (refB == ref) + (refC == ref)`.
5. Decision table:

| `n` | Condition | Result |
|---|---|---|
| `> 1` | two or three neighbours share the current ref | component-wise median of A, B, C |
| `1` | exactly one shares | copy that neighbour's MV (A first, then B, then C) |
| `0` | `refB == -2` and `refC == -2` and `refA != -2` | copy A (only the left neighbour exists) |
| `0` | otherwise | component-wise median of A, B, C |

The last-but-one rule is the standard's "only one available neighbour" exception; note it requires *both* top neighbours unavailable and the left available. Median is computed independently for the x and y components.

### 5.2 16×16 prediction (`x264_mb_predict_mv_16x16`)

Same candidate set and same decision table as §5.1, with `idx` fixed at the top-left block, width 4, and no partition shortcuts. C is taken at slot `X264_SCAN8_0 - 8 + 4`; if unavailable it falls back to D at `X264_SCAN8_0 - 8 - 1`. There is no MBAFF `topright_*` substitution in this function.

### 5.3 P-skip prediction (`x264_mb_predict_mv_pskip`)

Read A and B (left and above of the top-left block). If **any** of the following holds, write `(0,0)`:

* `refA == -2` or `refB == -2` (unavailable neighbour);
* `(uint32_t)refA | low32(mvA) == 0` (left neighbour has ref 0 and zero MV);
* `(uint32_t)refB | low32(mvB) == 0` (above neighbour has ref 0 and zero MV).

Otherwise call `x264_mb_predict_mv_16x16(h, 0, 0, mv)`. Rationale: the H.264 P-skip rule predicts zero when there is no usable neighbour or when either neighbour is a zero/zero reference-0 block; only when both neighbours carry a nonzero ref or MV does the full 16×16 median rule apply with assumed `ref = 0`.

Decision table:

| A (ref, MV) | B (ref, MV) | Result |
|---|---|---|
| unavailable | any | `(0,0)` |
| `(0,0,0)` | any | `(0,0)` |
| any | `(0,0,0)` | `(0,0)` |
| usable | usable | 16×16 predictor for ref 0 |

### 5.4 Temporal direct mode

The temporal method derives list-0/list-1 MVs from the colocated macroblock in reference list 1, scaled by POC distance.

Preparation:

1. Read the colocated MB's type and partition at the current MB coordinates. An interlaced/field mismatch triggers adjustments:
   * If the current MB is field-coded but the colocated MB is frame-coded, use both fields of the colocated MB pair; if both are intra or 16×16 (and not 8×8), set the effective partition to `D_16x8`, otherwise `D_8x8`. Set `preshift = 0`, `yshift = 0`, and adjust the MB row index.
   * Otherwise (frame current, field colocated), choose the colocated parity by comparing POC distances, set `preshift = 1`, `yshift = 2`, and use the single parity's type/partition.
   * `offset = 1` in the normal case, `0` after a mismatch adjustment.
2. Compute 4×4 and 8×8 base indices for the (possibly adjusted) colocated MB.
3. Clear all current refs for list 0.

Partition iteration depends on the colocated partition through the enum order `D_8x8 < D_16x8 < D_8x16 < D_16x16`:

* `max_i8 = (D_16x16 - partition) + 1` — 4, 3, 2, 1 respectively.
* `step = 1 + (partition == D_16x8)` — `D_16x8` visits indices 0 and 2 only.
* `width = 4 >> ((D_16x16 - partition) & 1)`, `height = 4 >> ((D_16x16 - partition) >> 1)` — gives 2×2, 4×2, 2×4, 4×4 in 4×4 units.

For each visited 8×8 index:

1. Compute x8/y8 and the colocated 4×4-row offset `ypart` (3*y8 normally, or field-specific offsets for MBAFF).
2. If the colocated partition is intra: write ref 0 and zero MV for both lists into the current cache and continue.
3. Read the colocated list-0 reference index; map it through `map_col_to_list0` (with `preshift` and field-parity offset): `ref = map_col_to_list0(colref >> preshift) * 2^postshift + (offset & colref & MB_INTERLACED)`.
4. If `ref < 0` (colocated reference not present in current list 0), return **failure (0)** for the whole macroblock — direct mode cannot be used.
5. Otherwise compute:
   * `dsf = dist_scale_factor[ref][0]`;
   * `mv_col` = colocated MV;
   * `mv_y = (mv_col[1] * 2^yshift) / 2` — vertical component scaled for field parity;
   * `l0x = (dsf * mv_col[0] + 128) >> 8`;
   * `l0y = (dsf * mv_y + 128) >> 8` (rounding right shift by 8).
6. If multithreaded and either `l0y > mv_max_spel[1]` or `l0y - mv_y > mv_max_spel[1]`, return failure — prevents race conditions where a neighbouring thread's frame is not yet available at the scaled MV.
7. Write into the current cache: list-0 `ref = i_ref`, `mv = (l0x, l0y)`; list-1 `mv = (l0x - mv_col[0], l0y - mv_y)`. List-1 ref stays 0 (set in step 3).

If all partitions succeed, return 1.

### 5.5 Spatial direct mode

Applies the normal 16×16 predictor independently to list 0 and list 1, then consults the colocated frame for the "col_zero" shortcut.

For each list:

1. Read A, B, C (with the same C→D fallback as §5.2).
2. Set `i_ref = min_unsigned(refA, refB, refC)`. Because unavailable neighbours are `-2` (interpreted as a large unsigned value), a negative result means at least one neighbour is genuinely negative. If `i_ref < 0`, set `i_ref = -1` and MV `(0,0)`.
3. Otherwise count matches `n = (refA == ref) + (refB == ref) + (refC == ref)`; if `n > 1` use the component-wise median of A, B, C; else copy the single matching neighbour (A, then B, then C). This is a simplified form of §5.2 with the "only A available" case folded into the `n == 1` branch.
4. Write `ref` and MV for the whole 16×16 into the cache for that list; remember `ref[list]`.

Then:

1. Derive the effective colocated partition with the same interlaced adjustments as temporal mode.
2. If **both** `ref[0] < 0` and `ref[1] < 0`, set all refs to 0 for both lists and return success — no usable spatial information, so zero everything.
3. In multithreaded mode, if either predicted vertical MV exceeds `mv_max_spel[1]`, return failure.
4. Early success: if both MVs are zero, or (progressive and the colocated MB is intra), or both refs are nonzero, return success without the col-zero loop.
5. **Col-zero loop** over the colocated partitions (using the same `max_i8`/`step`/`width`/`height` derivation):
   * Compute colocated 8×8 and 4×4 offsets.
   * Skip intra colocated partitions when interlaced.
   * Determine which list to test: `idx = 0` if colocated list-0 ref is 0; `idx = 1` if colocated list-0 ref `< 0` and colocated list-1 ref is 0; otherwise skip.
   * If **both** components of that colocated MV satisfy `abs(...) <= 1`, zero out the current predictor for each list whose own `ref` is 0 (over the corresponding partition area).

Rationale: when the colocated list-1 MV is essentially zero, the decoder can infer zero MV for the corresponding list; writing an explicit zero MVD saves bits and remains decodable.

The two wrappers `..._spatial_interlaced` and `..._spatial_progressive` call the same routine with the field flag set or clear.

### 5.6 Direct-mode dispatcher and change detection

`x264_mb_predict_mv_direct16x16(h, b_changed)`:

1. If `i_direct_mv_pred == X264_DIRECT_PRED_NONE`, return 0 immediately (direct mode disabled).
2. If `sh.b_direct_spatial_mv_pred` is set, run spatial prediction (interlaced variant under MBAFF, progressive otherwise); otherwise run temporal prediction.
3. If prediction failed, return 0.
4. If `b_changed` is non-NULL, compute a bitwise difference between the newly written MVs/refs and the cached `direct_mv`/`direct_ref` values. Check the top-left block first; if unchanged and the partition is not 16×16, also check the bottom-right block; if still unchanged and the partition is 8×8, check the other two 8×8 blocks. Set `*b_changed` accordingly. If nothing changed, return without updating the cache (the cache already holds the same values).
5. Otherwise copy all four 8×8 MVs and refs for both lists into the direct cache and store `direct_partition = i_partition`.
6. Return 1.

This change detection avoids recomputing costs when a direct prediction is reused across RD trials.

### 5.7 ME predictor list (`x264_mb_predict_mv_ref16x16`)

Explicitly an optimisation, not a normative rule. It appends candidate vectors in this order:

1. **Direct candidate:** in B slices, if the bottom-right 8×8's list reference equals `i_ref`, append that MV.
2. **Lowres candidate:** when `i_ref == 0` and low-resolution motion data exists and the frame index gap is within `i_bframe`, append the lowres MV shifted left by one with the low bit cleared (`(lowres*2) & 0xfffeffff` per 32-bit word).
3. **Spatial candidates:** the four neighbours (left, top, top-left, top-right) from `mvr[list][ref]`. Under MBAFF each is converted with `shift = 1 + MB_INTERLACED - neighbour_field`, indexing `mvr[list][ref<<1>>shift]` and scaling the vertical component as `mv_y * 2 >> shift`.
4. **Temporal candidates:** when the colocated list-0 frame has references, append up to three scaled MVs from the colocated MB and its right and bottom neighbours. Scale factor is `(cur_poc - ref_poc) * inv_ref_poc[MB_INTERLACED & field]`, and each resulting component is `(mv * scale + 128) >> 8` clipped to `INT16_MIN..INT16_MAX`.

The output count `*i_mvc` can therefore be up to 1 + 1 + 4 + 3 = 9 entries. Motion search uses this list to start from good candidates.

## 6. Dependencies

* Reads the neighbour cache built by `macroblock.c`; writes `cache.mv/ref` for direct mode.
* Uses `x264_median_mv` (component-wise median, from `base.h`), `pack16to32`, `map_col_to_list0`, `dist_scale_factor`, `inv_ref_poc`, and the colocated frame arrays in `h->fref[1][0]`.
* Uses `x264_macroblock_cache_ref/mv/mv_ptr` inline helpers (declared in `rectangle.h`/`common.h`) to fill whole partitions in direct mode.
* Called by the slice writer for every inter block and at the start of B_SKIP/B_DIRECT coding; `mv_ref16x16` is called by motion estimation only.

## 7. Constants and tables

| Item | Value / derivation | Why |
|---|---|---|
| Unavailable ref sentinel | `-2` | Single-test availability check; treated as large unsigned in spatial direct `min3`. |
| Intra/unused ref | `-1` | Forced to `-1` when any spatial neighbour is negative. |
| Candidate slots | A=`p-1`, B=`p-8`, C=`p-8+w`, D=`p-9` | Direct mapping from the scan8 layout. |
| `D_16x8` shortcut | first half uses B, second half uses A | Horizontal split: top neighbour / left neighbour. |
| `D_8x16` shortcut | left half uses A, right half uses C | Vertical split: left neighbour / above-right neighbour. |
| `max_i8` | `(D_16x16 - partition) + 1` → 4/3/2/1 | Number of 8×8 blocks for colocated partitions. |
| `step` | 2 for `D_16x8`, else 1 | 16×8 has two distinct 8×8 regions. |
| `width,height` | `4 >> ((D_16x16-p)&1)`, `4 >> ((D_16x16-p)>>1)` | Partition dimensions in 4×4 units. |
| Temporal MV scale | `dsf*mv + 128 >> 8` | Standard `DistScaleFactor` scaling with rounding. |
| Field vertical scale | `mv_y = mv_col_y * 2^yshift / 2` | Converts field MVs to frame coordinates. |
| Spatial direct `min3` | `min` of refs interpreted as unsigned | Makes `-2` lose to any real reference. |
| Col-zero threshold | `abs(col MV component) <= 1` | Treats near-zero colocated MVs as zero. |
| Lowres MV scaling | `(mv*2) & 0xfffeffff` | Half-pel to quarter-pel conversion with low bit masked. |
| Temporal ME scale | `((mv*scale)+128)>>8`, clipped to int16 | POC-ratio scaling. |

## 8. Performance notes

Hotness: `x264_mb_predict_mv` is called for every inter partition; direct prediction is called for every B macroblock; the ME predictor list runs for every motion search.

Concrete optimisation opportunities:

1. **Branch elimination.** The predictor-selection decision table is a small integer comparison chain with data-dependent branches. Expressing `n` as a sum of three equality tests and selecting with masks removes mispredictions; the partition shortcuts should be folded into the same masked select. Public reference: Intel Optimization Reference Manual, "Branch Prediction Optimization" (https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html); Agner Fog, "Optimizing subroutines in assembly language" (https://www.agner.org/optimize/).
2. **Batched neighbour loads.** The A/B/C/D cache slots are four 16-bit pairs at fixed offsets; one aligned 16-byte load can fetch B and C together, reducing load ports pressure. Public reference: Intel Intrinsics Guide, `_mm_load_si128`, `_mm_shuffle_epi8` (https://www.intel.com/content/www/us/en/docs/intrinsics-guide/).
3. **SIMD byte-stream scattering / context-state layout.** In direct mode, writing four 8×8 ref/MV pairs can be done with two 16-byte stores instead of eight scalar writes; keeping `cache.ref` and `cache.mv` 4-/16-byte aligned (as in `common.h`) is what makes this legal. The same aligned layout lets SIMD CABAC residual code gather MVD contexts for a whole 8×8 at once. Public reference: Intel Intrinsics Guide, `_mm_store_si128`, `_mm_shuffle_epi8`; H.264 Annex G for context categories.
4. **Batching direct partition iteration.** The `max_i8`/`step` loop computes colocated offsets repeatedly; precomputing the four 8×8 offsets for the current partition type into a small table removes per-iteration divisions and shifts.
5. **Change-detection batching.** Comparing cached direct values with the current cache uses several 32-bit XORs; these can be combined into two 128-bit compares for all four blocks, with the "partition not 16×16" conditional handled by masking unused lanes.
6. **Avoiding redundant scaling.** `dsf * mv + 128 >> 8` is computed for both components; a SIMD version can use `_mm_mulhi_epi16` with pre-rounded constants to process four candidates at once. Public reference: Intel Intrinsics Guide, `_mm_mulhi_epi16`, `_mm_adds_epi16`.

## 9. References

* ITU-T Recommendation H.264 (ISO/IEC 14496-10), §8.3.4 — derivation of motion vector predictors; §8.3.1.2 — direct prediction modes (temporal and spatial); §7.4.5 — reference index and MVD syntax; Annex G — CABAC neighbour context derivation. https://www.itu.int/rec/T-REC-H.264
* Richardson, "H.264 and MPEG-4 Video Compression", Wiley, 2003 — direct mode scaling and MBAFF MV conversion.
* Intel Intrinsics Guide. https://www.intel.com/content/www/us/en/docs/intrinsics-guide/
* Intel® 64 and IA-32 Architectures Optimization Reference Manual. https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html
* Agner Fog, optimization manuals. https://www.agner.org/optimize/

## 10. Verification checklist

1. **Normal predictor, three matching refs:** set A, B, C to distinct MVs with the same ref as the current block; assert the result is the component-wise median (e.g. A=(1,2), B=(5,4), C=(3,9) → (3,4)).
2. **Single matching ref:** with only B sharing the current ref, assert the predictor equals B exactly, even if A/C have larger MVs.
3. **Only-left exception:** set `refB = refC = -2`, `refA == ref` with a nonzero MV; assert the predictor equals A. Then set `refA != ref` and assert the median path (with zero top MVs) is used instead.
4. **Above-right fallback:** mark the nominal C slot `ref = -2` and place a distinct MV in D (top-left); assert the predictor uses D.
5. **16×8 / 8×16 shortcuts:** for a 16×8 second partition with `refA == current ref` and a mismatching B, assert A is copied without median; for an 8×16 right partition with `refC == current ref`, assert C is copied.
6. **P-skip zero rules:** enumerate eight combinations of `(refA, mvA)` and `(refB, mvB)` from `{unavailable, (0,0,0), (0, nonzero), (1, anything)}` and verify the eight-row decision table in §5.3.
7. **Temporal direct scaling:** construct a colocated MB at 16×16 with `dsf = 512`, `mv_col = (4,6)`, and `map_col_to_list0` identity; assert list-0 MV is `(8,12)` and list-1 MV is `(4,6)` after the `+128>>8` rounding (exact arithmetic to be checked).
8. **Temporal failure:** set a colocated ref that maps to `-1`; assert the function returns 0 and the cache is left in a consistent state (refs cleared).
9. **Spatial direct negative refs:** make all three neighbours unavailable; assert both list refs become 0, both MVs zero, and the function returns 1.
10. **Spatial `min3` unsigned semantics:** with refs `(-2, 2, 3)`, assert the chosen ref is 2 (because `-2` is treated as a large unsigned value), not `-2`.
11. **Col-zero shortcut:** with list-0 `ref = 0` and a colocated list-1 ref of 0 with MV `(1,-1)`, assert the predicted list-0 MV is zeroed for that partition; with MV `(2,0)` assert it is not.
12. **Direct change detection:** after a successful prediction with no changes, assert `*b_changed == 0` and `direct_partition` unchanged; after modifying one MV, assert `*b_changed == 1`.
13. **ME predictor ordering:** with all sources available, assert the candidate list is direct → lowres → (left, top, top-left, top-right) → (co-located, co-located right, co-located bottom), for 9 total entries.

## 11. Open questions

* The exact conditions under which `mb_predict_mv_direct16x16_temporal` sets `preshift = 0` versus `1` and `postshift` (the code uses `1 << postshift` but `postshift` is only assigned in the interlaced mismatch branch) are complex and field-parity dependent; full enumeration across all MBAFF combinations is UNKNOWN from this file alone.
* The meaning of `offset & i_ref1_ref & MB_INTERLACED` in the temporal mapping is a bit-level trick whose full truth table across field parities was not derived — UNKNOWN beyond the expression.
* The exact caller conditions for `b_direct_spatial_mv_pred` (which selects spatial vs temporal direct mode, e.g. via frame type and B-pyramid settings) are outside this file — UNKNOWN.
* `X264_DIRECT_PRED_NONE` and other `i_direct_mv_pred` enum values are defined elsewhere; only the NONE behaviour is visible here — UNKNOWN.
* The lowres MV indexing formula `fref[1][0]->i_frame - fenc->i_frame - 1` versus its list-0 counterpart assumes a specific frame numbering; edge cases at sequence boundaries are UNKNOWN.
* The precise fixed-point rounding of `(tb*tx + 32) >> 6` in biprediction belongs to `macroblock.c`; the MV scaling here uses the stored factor and is exact.
