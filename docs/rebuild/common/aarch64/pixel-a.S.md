# `common/aarch64/pixel-a.S` — NEON pixel metrics

> Dirty-side behavioural specification (reverse-engineered from the reference encoder; describes behaviour, not code).

## 1. Header

- Source path: `common/aarch64/pixel-a.S`
- Role: complete NEON implementation of x264's pixel-distortion metrics: SAD (single and x3/x4), VSAD, ASD, SSD, SATD, SA8D, variance, variance-difference, Hadamard-AC, and SSIM helpers.
- This is a **dirty-side behavioural specification** artifact for the clean-room reimplementation.

## 2. Purpose

Rate-distortion optimization evaluates candidate block modes millions of times per frame, so each metric kernel is critical. This file provides the 8-bit NEON versions with separate 10-bit variants below a preprocessor split. Several helpers are shared between metrics (8×8 Hadamard core, transpose networks, AC masks), and SVE versions override a subset of the exported functions.

## 3. Public interface

Exported functions (declared in `pixel.h`):
- SAD: `x264_pixel_sad_{4x4,4x8,4x16,8x4,8x8,8x16,16x8,16x16}_neon`
- Multi-candidate SAD: `x264_pixel_sad_x3_*_neon`, `x264_pixel_sad_x4_*_neon` for the same eight block sizes
- `x264_pixel_vsad_neon( pixel *pix, intptr_t stride, int height )`
- `x264_pixel_asd8_neon( pixel *pix1, intptr_t s1, pixel *pix2, intptr_t s2, int height )`
- SSD: `x264_pixel_ssd_{4x4,4x8,4x16,8x4,8x8,8x16,16x8,16x16}_neon`
- SATD: `x264_pixel_satd_{4x4,4x8,4x16,8x4,8x8,8x16,16x8,16x16}_neon`
- `x264_pixel_ssd_nv12_core_neon( pixel*, intptr_t, pixel*, intptr_t, int, int, uint64_t*, uint64_t* )`
- `x264_pixel_sa8d_8x8_neon`, `x264_pixel_sa8d_16x16_neon`, `x264_pixel_sa8d_satd_16x16_neon`
- Variance: `x264_pixel_var_{8x8,8x16,16x16}_neon`, `x264_pixel_var2_{8x8,8x16}_neon`
- `x264_pixel_hadamard_ac_{8x8,8x16,16x8,16x16}_neon`
- `x264_pixel_ssim_4x4x2_core_neon`, `x264_pixel_ssim_end4_neon`

Internal helpers: `satd_4x8_8x4_end_neon`, `satd_8x8_neon`, `satd_8x4v_8x8h_neon`, `satd_16x4_neon`, `sa8d_satd_8x8_neon`, `hadamard_ac_8x8_neon`, `var_end`, and the `mask` constant.

## 4. State and data structures

- `SAD_START_*`/`SAD_*` macros load two rows and accumulate absolute differences in 16-bit lanes (`uabdl` first, `uabal` thereafter), with separate accumulators for 16-wide blocks; final reduction by `uaddlv s0`.
- `SAD_X_4/8/16` macros keep up to four SAD accumulators live simultaneously, sharing one source row (loaded with `FENC_STRIDE`) against up to four candidate rows (loaded with a common stride).
- SSD uses 32-bit accumulators (`smull`/`smlal`); SATD/SA8D use 16-bit lanes with transposes from `asm.S` and `HADAMARD4_V` from `pixel-a-common.S`.
- `mask` constant: 16 bytes of `0xff` followed by 16 bytes of `0x00`, used to mask out trailing lanes in SSIM when width < 4.
- `var_end` returns a packed 64-bit value: low 32 bits = sum of samples, high 32 bits = sum of squares.

## 5. Algorithm / behaviour

### SAD

For each block shape, the kernel loads pairs of rows from the two planes with their strides, computes unsigned absolute byte differences widened to halfwords, and accumulates into one or two 8-lane halfword vectors. Rows are processed in groups of two via `.rept height/2−1`, with a separate start macro for the first pair (using `uabdl` instead of `uabal`) and a final `uaddlv` horizontal sum. Widths 4, 8, 16 use single-lane loads (4 bytes), 8-byte loads, and 16-byte loads respectively. No saturation needed (each lane accumulates at most 255×8 per row group; the final 32-bit sum is exact).

`pixel_sad_x3_*` and `x4_*` compute three/four SADs against a shared source row: the source is loaded once per row pair with stride 16 (`FENC_STRIDE`), candidates with a shared stride; results stored as 32-bit words to the output `int *`. For x3, the fourth accumulator is omitted and only three words are stored.

### VSAD and ASD

`pixel_vsad_neon` computes the sum of absolute differences between consecutive rows of a single plane over `height` rows, using a two-row sliding window and the same 16-bit accumulation pattern. `pixel_asd8_neon` computes signed differences between two planes (unlike SAD's absolute byte differences) over a configurable height, useful for certain distortion metrics.

### SSD

For width 4: load one 4-byte row from each plane, compute signed difference widened to halfwords, multiply-accumulate the low four lanes into a 32-bit vector (`smull`/`smlal`). Width 8: same with `smlal`/`smlal2` for both halves. Width 16: two 32-bit accumulators, one per 8-lane half. After all rows, `addv s0` sums the four 32-bit lanes and the result is returned as `int`.

### SATD

The generic SATD is the sum of absolute values of the 2-D Hadamard transform of the pixel differences:
- 4×4: load four 4-byte rows, form differences, apply a 4-point butterfly along rows, zip/transpose, apply another butterfly, transpose again at word granularity, take `abs` and pairwise `umax`, then `uaddlv`.
- 4×8/8×4: load eight rows (transposed arrangement), form differences, apply row-wise butterflies, transpose with `trn`, apply column-wise butterflies, transpose at word granularity, `abs`/`umax`, `uaddlv`.
- 8×8/8×16/16×8/16×16: use the shared `satd_8x8_neon` / `satd_16x4_neon` cores and sum multiple 8×8 results into 32-bit accumulators before the final reduction.
All transforms are exact integer Hadamards with no scaling; no rounding.

### SA8D and SA8D+SATD

`pixel_sa8d_8x8_neon` calls `sa8d_satd_8x8_neon` (which returns two 8-lane halfword vectors holding per-lane SA8D and SATD contributions), adds them, horizontally sums, then returns `(sum+1)>>1`. `pixel_sa8d_16x16_neon` calls the 8×8 core four times, accumulating 16-bit lanes into 32-bit vectors with `uaddlp`/`uadalp`, then sums and applies the same rounding. `pixel_sa8d_satd_16x16_neon` returns a packed 64-bit value: low 32 bits = SA8D (rounded by `urshr #1`), high 32 bits = SATD (unrounded sum). The SVE file provides an 8×8 SVE override for SA8D only.

### Variance and var2

`pixel_var_8x{h}_neon` / `16x16`: accumulate the sum of samples in 16-bit lanes and the sum of squares in 32-bit lanes (via `umull`/`uaddlp`/`uadalp`), then `var_end` returns `sum | (sum_sq << 32)`. `pixel_var2_8x{h}_neon` computes the variance of the *difference* between two planes: forms halfword differences, accumulates their sum and sum of squares, then computes `sum_sq − (sum^2 >> (6 + (h>>4)))` for each lane and returns their sum in `w0`, while storing the two per-lane sums of squares into the `int*` output.

### Hadamard-AC

`pixel_hadamard_ac_WxH_neon` calls `hadamard_ac_8x8_neon` one to four times depending on block shape, with pointer adjustments for right/bottom halves. The 8×8 core computes a 2-D Hadamard transform of the *samples* (not differences), takes absolute values, ANDs with the AC mask to drop DC for one accumulator, and accumulates two different sums in `v28`/`v29`. The final reduction returns `sum0>>1 | (sum1>>2)<<32`, matching the generic "AC energy" estimate used for I/P mode decisions. No rounding in these shifts.

### SSIM

`pixel_ssim_4x4x2_core_neon` processes two 4×4 blocks (8 rows) at once, accumulating for each of four quarter-quadrants: sum of x, sum of y, sum of x², and sum of xy (stored via `st4` into `sums[2][4]`). Uses `umull`/`uaddlp`/`uaddw` chains. `pixel_ssim_end4_neon` combines five rows of such sums into variance/covariance terms: shifts sums by 6/7 to get scaled variances and covariances, adds constants `ssim_c1 = 416` and `ssim_c2 = 0x0399bb` (which encode `.01²·255²·64` and `.03²·255²·64·63`), converts to float, and computes the SSIM product/sum with `fdiv` and `faddp`. It masks out lanes beyond `width` using the `mask` table and returns the final float in `s0`.

### 10-bit section

The `#else` branch repeats SAD, SSD, SATD, variance, SA8D, and Hadamard-AC with 16-bit pixel lanes: SAD uses `uabdl` on 16-bit lanes widening to 32-bit, SSD uses 32-bit multiply-accumulate directly, SATD/SA8D use wider halfword/word vectors, and masks/rounding are adapted. The block shapes and return conventions are the same. SSIM helpers are not instantiated for 10-bit in this file.

Rounding/saturation summary: SAD/SSD/SATD have no rounding or saturation beyond the final sums; SA8D rounds with `(sum+1)>>1`; hadamard-ac shifts without rounding; SSIM uses floating-point division and fixed constants; variance-difference uses a right shift by `6+(h>>4)`.

### Metric definitions

| Metric | Definition | Accumulator width |
| --- | --- | --- |
| SAD | `sum |pix1[i] − pix2[i]|` | 16-bit lanes, 32-bit final |
| SSD | `sum (pix1[i] − pix2[i])²` | 32-bit lanes |
| SATD | `sum abs( H( pix1 − pix2 ) )` where H is the 2-D Hadamard transform | 16-bit lanes, 32-bit final |
| SA8D | `(sum abs(H(pix1 − pix2)) + 1) >> 1` over an 8×8 block | 16-bit lanes, 32-bit final |
| Variance | `sum pix | (sum pix²) << 32` | 16/32-bit lanes |
| Var2 | `sum((d−mean)²)` with `mean = sum(d) >> (6 + h/16)` | 16/32-bit lanes |
| Hadamard-AC | two sums of absolute Hadamard coefficients, one DC-inclusive and one AC-only, returned as `sum0>>1 | (sum1>>2)<<32` | 32-bit lanes |
| SSIM | standard local SSIM with fixed-point constants C1/C2 | float at reduction |

SAD's `uabdl`/`uabal` chain widens byte differences to halfwords and accumulates up to eight samples per lane before the final `uaddlv`; this cannot overflow because `8 × 255 = 2040 < 65536`. SSD's `smull`/`smlal` chain accumulates squares directly in 32-bit lanes. SATD uses the same butterfly as the 4×4 transform but without scaling, and the pairwise `umax` instructions implement "keep the larger of two absolute values" before the final horizontal sum.

### SAD x3/x4 interface

The multi-candidate variants take one source pointer and three or four candidate pointers, plus a single shared stride. For each row pair the source is loaded once (with `FENC_STRIDE = 16`) and reused against every candidate. Accumulators live in `v16..v19` (plus `v20..v23` for 16-wide blocks). Results are written as consecutive 32-bit words to the output `int *`. The x3 variant writes only three words; x4 writes four.

### Rounding and packing details

- `urhadd` implements `(a+b+1)>>1` for byte averaging and VSAD-style reductions.
- SA8D's `add #1; lsr #1` is the only rounding in the metric family; SATD itself is unrounded.
- `var_end` packs the sample sum into the low 32 bits and the square sum into the high 32 bits with `orr x0, x0, x1, lsl #32`.
- `pixel_sa8d_satd_16x16_neon` packs the rounded SA8D into the low half and the unrounded SATD into the high half, using `urshr #1` for the former.
- Hadamard-AC's AC masks clear the DC lane before accumulation; the two returned halves are plain logical shifts by 1 and 2.

### SSIM reduction

With per-quadrant sums `sx, sy, sxx, sxy`, the kernel forms:
```
mu_x = sx / N,  mu_y = sy / N
var_x = (sxx − sx²/N),  var_y = (syy − sy²/N)
cov   = (sxy − sx*sy/N)
SSIM = ((2*mu_x*mu_y + C1)(2*cov + C2)) / ((mu_x²+mu_y²+C1)(var_x+var_y+C2))
```
The implementation uses fixed-point shifts (6 and 7) to scale the variance/covariance terms into the same domain as the constants `C1 = 416` and `C2 = 0x000399bb`, converts to float, and computes one `fdiv` per 4-lane group. The `mask` table zeroes lanes beyond `width`.

## 6. Dependencies

- Includes `asm.S` (transpose macros, strides) and `pixel-a-common.S` (`HADAMARD4_V`, `mask_ac_4_8`).
- Dispatched from `common/pixel.c` via `pixel.h`; SVE variants override a subset.
- Uses internal `bl` calls with link register saved in `x4` for nested helpers.

## 7. Constants and tables

- `mask`: 32 bytes (16 × `0xff`, 16 × `0x00`) for SSIM lane masking.
- `mask_ac_4_8`: AC coefficient masks (from `pixel-a-common.S`).
- `ssim_c1 = 416`, `ssim_c2 = 0x000399bb`: fixed-point SSIM constants.
- `1`: SA8D rounding increment.
- No other tables.

## 8. Performance notes

Among the hottest kernels in the encoder. The reference keeps two to four accumulators live to hide latency, unrolls row loops with `.rept`, and uses `uaddlv`/`addv` for final reductions. The x3/x4 SAD variants share one source load across candidates — critical for motion-estimation throughput.

Optimization tips for a clean-room AVX2 port: use `_mm256_sad_epu8` for SAD, `_mm256_madd_epi16` for SSD, `_mm256_hadd_epi32` for reductions; implement SATD with 128-bit butterflies and `_mm_abs_epi16` (SSSE3); use `_mm256_dpbusd_epi32` (AVX2 VNNI) where available for weighted sums. SVE tip: the SVE overrides in the reference cover only SSD/var/SA8D/hadamard-ac; SAD remains NEON because x3/x4 sharing is hard to express with SVE predicates. Public references: x264 RDO metric definitions; ARM NEON reduction/SIMD docs.

## 9. References

- x264 rate-distortion metric definitions (`common/pixel.c`).
- ITU-T H.264 / ISO/IEC 14496-10 (transform basis for SATD).
- SSIM paper (Wang, Bovik, Sheikh, Simoncelli, "Image quality assessment: from error visibility to structural similarity") and x264's fixed-point adaptation.
- ARM NEON instruction reference (`uabdl`, `uabal`, `smlal`, `uaddlv`, `trn`, `abs`, `umax`).
- `docs/rebuild/common/aarch64/pixel-a-common.S.md`, `pixel-a-sve.S.md`, `pixel.h.md`.

## 10. Verification checklist

- Every metric matches the generic C implementation exactly for random blocks of all supported shapes.
- SAD x3/x4 output words correspond to the correct candidate planes and strides.
- SATD values match a scalar Hadamard-and-abs computation (no scaling).
- SA8D rounding `(sum+1)>>1` verified at boundary values; packed SA8D/SATD return halves correct.
- Variance and hadamard-ac packed returns match the C reference's low/high word convention.
- `pixel_var2_*` stores the expected sums-of-squares to the output pointer.
- SSIM end function matches the standard SSIM formula to float precision, including lane masking at width < 4.

## 11. Open questions

- UNKNOWN: exact constants used in the 10-bit SSIM path (no SSIM helpers are instantiated for 10-bit in this file).
- UNKNOWN: whether `pixel_asd8_neon` is still used by any caller (no dispatch is visible in the listed sources).
- UNKNOWN: overflow behaviour of 32-bit SSD accumulators for the largest blocks (16×16 at 8-bit fits comfortably; 10-bit needs verification).
