# `common/pixel.c` — pixel comparison metrics for motion estimation and mode decision

**Role:** implements SAD, SSD, SATD, SA8D, variance, Hadamard, SSIM and intra-mode metrics
used by motion search and rate–distortion analysis, plus their parallel (`x3`/`x4`) forms.
**Status:** dirty-side behavioural specification.

---

## 1. Header

- Source path: `common/pixel.c` (header `common/pixel.h`).
- One-sentence role: provide a large family of fixed-size distortion measures and batched
  comparison functions, dispatched through `x264_pixel_function_t`, for deciding which
  prediction mode and motion vector is cheapest.
- Marker: dirty-side behavioural specification.

---

## 2. Purpose

Rate–distortion analysis constantly asks "how different are these two blocks?". Different
stages need different metrics: cheap SAD for full-pel motion search, SATD (Hadamard) for
sub-pel refinement and intra mode choice because it correlates better with coding cost, SSD
for psy-RD and rate control, variance for AQ, SSIM for quality metrics, and batched
comparisons to evaluate several candidates in one call. This file implements all of them for
the block sizes defined in `pixel.h`, and layers SIMD implementations on top.

---

## 3. Public interface

Exported (from `pixel.h`):

- `void x264_pixel_init(uint32_t cpu, x264_pixel_function_t *pixf)` — zero the table, install
  every scalar function, then upgrade by CPU capability.
- `uint64_t x264_pixel_ssd_wxh(pf, pix1, stride1, pix2, stride2, w, h)` — arbitrary-size SSD
  assembled from fixed-size kernels plus scalar tails.
- `void x264_pixel_ssd_nv12(pf, pix1, stride1, pix2, stride2, w, h, *ssd_u, *ssd_v)` —
  interleaved U/V SSD.
- `float x264_pixel_ssim_wxh(pf, pix1, stride1, pix2, stride2, w, h, void *buf, int *cnt)`.
- `int x264_field_vsad(x264_t *h, mb_x, mb_y)` — frame/field decision using vertical SAD.

All primitive kernels are reached through `x264_pixel_function_t` arrays indexed by the
`PIXEL_*` enum. Callback signatures are detailed in `pixel.h.md`.

---

## 4. State and data structures

- Metric arrays `sad[8]`, `ssd[8]`, `satd[8]`, `ssim[7]`, `sa8d[4]`, plus `mbcmp`, `fpelcmp`,
  `sad_aligned`, `fpelcmp_x3/x4`, `sad_x3/x4`, `satd_x3/x4`, `ads`, `var`, `var2`,
  `hadamard_ac`, intra comparison helpers. Each is a fixed or indexed function pointer.
- `x264_pixel_size[12]` maps `PIXEL_*` indices to `(w,h)`; `x264_size2pixel[5][5]` and
  `x264_luma2chroma_pixel[4][7]` provide reverse/chroma mappings.
- Accumulator types: `sum_t` is `uint16_t` (8-bit) or `uint32_t` (high bit depth); `sum2_t` is
  twice as wide (32/64-bit). The `abs2` helper computes `abs(lo) + (abs(hi) << BITS_PER_SUM)`
  on a packed pair using sign-mask arithmetic.
- Alignment contract from `pixel.h`: SSD assumes **both** arguments aligned; other compare
  functions assume the **first** argument aligned. SIMD variants have stricter requirements.
- `x264_run_level_t`, `x264_pixel_function_t` are caller-owned; the init function fills the
  latter.

---

## 5. Algorithm / behaviour

### 5.1 SAD (`x264_pixel_sad_*`)

`Σ |pix1[x] − pix2[x]|` over the block, with independent strides. All 8 primary sizes
(16x16 … 4x4) plus the chroma sizes (4x16, 4x2, 2x8, 2x4, 2x2) exist. Values are non-negative
and bounded by `w*h*PIXEL_MAX`.

### 5.2 SSD (`x264_pixel_ssd_*`)

`Σ (pix1[x] − pix2[x])²`. `x264_pixel_ssd_wxh` decomposes an arbitrary rectangle into
16x16/8x16/8x8 kernels (using 16x16 only when all pointers and strides are 16-byte aligned),
then handles the right/bottom scalar remainders. `x264_pixel_ssd_nv12` accumulates U at even
indices and V at odd indices separately, using the aligned core on `w & ~7` columns and a
scalar core on the remainder.

### 5.3 SATD (sum of absolute Hadamard-transformed differences)

The 4x4 SATD applies a 2-D unnormalised Hadamard transform to the difference block and sums
absolute values, returning `sum >> 1` (because the 2-D Hadamard has gain 4 and the standard
metric includes a 1/2 factor). The implementation packs two 16-bit partial sums into a
`sum2_t` word so the four-lane Hadamard butterfly operates on two columns at once; `abs2`
extracts both absolute values with a sign-mask trick.

The 8x4 SATD packs columns 0..3 and 4..7 into the low/high halves of packed words, applies the
4-point Hadamard across each row and then across each column, and returns
`(((sum_t)sum) + (sum>>BITS_PER_SUM)) >> 1`. Larger blocks are composed of 4x4 or 8x4
sub-blocks via the `PIXEL_SATD_C` macro: 8x8 = four 8x4, 16x8 = two 8x4 horizontally,
8x16 = two, 16x16 = four, etc. SATD is the default `mbcmp`/`fpelcmp` metric for sub-pel
refinement and intra decisions.

### 5.4 SA8D (8x8 Hadamard of 8x8 blocks)

`sa8d_8x8` computes an 8x8 Hadamard on the difference block: first a 1-D 8-point Hadamard on
each row (packed two-wide), then on columns, then combines the four 4x4 quadrants with
additional Hadamard stages and absolute sums. `x264_pixel_sa8d_8x8` returns `(sum+2)>>2`;
`x264_pixel_sa8d_16x16` sums four 8x8 blocks and applies the same `(sum+2)>>2`. SA8D is used
for 8x8dct intra/mode decisions.

### 5.5 Variance and related metrics

- `pixel_var_WxH`: returns `sum + (sqr << 32)` where `sum = Σ pixel`, `sqr = Σ pixel²` over the
  block — a packed pair consumed by AQ/rate control.
- `pixel_var2_8x8/8x16`: computes, for 8 columns of two chroma components per row, the
  difference sums and squared differences relative to the prediction, returning
  `Σsqr_u − (sum_u² >> shift) + Σsqr_v − (sum_v² >> shift)` (shift 6 for 8x8, 7 for 8x16), and
  writes the two squared sums to `ssd[2]`.

### 5.6 Hadamard AC (`pixel_hadamard_ac`)

For an 8x8 block it computes a full Hadamard transform with packed arithmetic, accumulating
the sum of absolute 4x4 coefficients (`sum4`) and 8x8 coefficients (`sum8`), subtracting the
DC, and returning `((sum8<<32) + sum4)` after a final normalization `((sum>>34)<<32) +
((uint32_t)sum>>1)`. Larger blocks sum multiple 8x8 contributions. Used for intra AC cost
estimation and quality analysis.

### 5.7 Batched comparison functions

- `sad_x3`/`sad_x4`: call the corresponding SAD three/four times against the same encoded
  block and different candidates, writing results to `scores[]`.
- `satd_x3`/`satd_x4`: same for SATD; the C versions are no faster than single SATD but exist
  so SATD can replace SAD as a drop-in. Architecture versions (mmx2, sse2, ssse3, sse4, avx,
  xop, avx512, neon) share data loading across candidates.
- `intra_*_x3_*`: build a candidate intra prediction (V/H/DC or DC/H/V for chroma) using the
  corresponding `predict` function and immediately measure SAD/SATD/SA8D against the source.
  The 8x8 luma variant takes a prepared `edge[36]` and evaluates V/H/DC.
- `intra_*_x9_*` (4x4 and 8x8): evaluate all nine intra modes in one call, with CABAC bit
  costs, and return the best while writing the chosen prediction into `fdec`. No portable C
  implementation exists — the source explicitly directs verifiers to `checkasm` or to the
  alternative algorithm in `mb_analyse_intra` used when SIMD is unavailable. This is an
  important `UNKNOWN` boundary for a clean implementation.

### 5.8 SSIM

`ssim_4x4x2_core` accumulates, for two adjacent 4x4 blocks, four sums per block: Σa, Σb,
Σa²+Σb², Σab. `ssim_end1` computes the local SSIM term with constants
`c1 = 0.01²·PIXEL_MAX²·64` and `c2 = 0.03²·PIXEL_MAX²·64·63`, using floating point for bit
depths above 9 and integer otherwise. `ssim_end4` combines five columns of such sums.
`x264_pixel_ssim_wxh` walks the image in 4x4 steps with two rolling sum rows, accumulates SSIM
over adjacent block pairs, and reports the sample count in `*cnt`.

### 5.9 Vertical SAD, ASD and successive elimination

- `pixel_vsad`: `Σ_{i=1..height-1} Σ_{j=0..15} |src[j] − src[j+stride]|` — vertical gradient
  energy used to choose frame vs field coding (`x264_field_vsad` compares frame SAD with the
  sum of two field SADs plus neighbour penalties).
- `pixel_asd8`: `|Σ (pix1[x] − pix2[x])|` over 8 columns × height (signed sum then absolute).
- `x264_pixel_ads1/2/4`: successive-elimination filters over precomputed `uint16_t` partial
  sums; accumulate absolute differences between encoded DC sums and candidate sums plus a
  motion-vector cost, and emit candidate MV indices whose score is below `thresh`. The 16x16
  version uses four sub-sums (at offsets 0, 8, delta, delta+8), 16x8 uses two, 8x8 uses one;
  the remaining sizes are aliased to these.

### 5.10 Initialization / dispatch

`x264_pixel_init` starts with `memset` to zero, then installs all portable kernels (SAD/SSD/
SATD/var/hadamard/ads/SSIM/intra x3), then upgrades by CPU: MMX/MMX2 → SSE2 (with
`SSE2_IS_SLOW`/`SSE2_IS_FAST`/`CACHELINE_32`/`CACHELINE_64`/`STACK_MOD4` qualifiers) → SSE3 →
SSSE3 → SSE4 → AVX → XOP → AVX2 → AVX512 on x86; NEON/SVE on ARM; Altivec, MSA, LSX/LASX
elsewhere. Some `intra_satd_x9`/`intra_sad_x9` entries remain null when no SIMD implementation
is available; the caller then uses a different algorithm.

---

## 6. Dependencies

- **Calls:** predictor functions from `predict.c/h` (intra metric helpers), SIMD metric kernels,
  `x264_clip3` for MV costs.
- **Called by:** `encoder/me.c` (motion search), `encoder/analyse.c`/`rdo.c` (mode decisions),
  `encoder/macroblock.c`, quality/AQ logic (`x264_pixel_var*`), `common/frame.c`.
- **Shared tables:** `x264_pixel_size`, `x264_size2pixel`, `x264_luma2chroma_pixel` in `pixel.h`.

---

## 7. Constants and tables

- `BITS_PER_SUM = 8*sizeof(sum_t)` = 16 (8-bit build) or 32 (high bit depth).
- SATD/SA8D final scaling `>>1` / `(sum+2)>>2`; Hadamard AC `>>34`/`>>1` normalization.
- SSIM constants `c1`, `c2` as derived in section 5.8.
- ADS uses `uint16_t` partial sums with wraparound, and the cost `cost_mvx[]`.
- Block-size tables from `pixel.h` (12 sizes).

---

## 8. Performance notes

These are the single hottest functions in the encoder: SAD/SATD run for every motion-search
candidate (thousands per macroblock at slow presets), so even small gains matter.

Optimization guidance:

- SAD should use saturating absolute-difference instructions (`psadbw`) over rows, accumulating
  into a wide register; the batched x3/x4 forms should load the encoded block once and reuse it
  across candidates.
- SATD/SA8D should keep data in packed 16-bit lanes and perform the Hadamard with
  `paddw`/`psubw`/`punpck` shuffles, using `abs2`-style sign-mask absolute value.
- Alignment: SAD variants split into "aligned first argument" and unaligned; choose the
  aligned one for the encoded block (which is always padded/aligned) and keep candidate
  pointers unaligned-safe. SSD additionally needs the second argument aligned.
- Cache: 16x16 blocks should be read row-by-row into registers; for x3/x4, prefetch candidates.
- Avoid per-sample branches in SSIM; the rolling sum rows should be swapped by pointer, not
  copied.
- The intra x9 helpers are worth implementing in SIMD because they fold prediction, metric
  and CABAC cost into one pass, but a correct portable fallback must exist.
- Literature: SATD's correlation with coding cost is discussed in video-coding literature on
  rate-distortion optimized motion estimation; SIMD SAD/SATD techniques are documented in the
  x264 developer notes and checkasm suite; SSIM is from Wang et al., "Image quality assessment:
  from error visibility to structural similarity", IEEE TIP 2004.

---

## 9. References

- Wang, Bovik, Sheikh, Simoncelli, "Image quality assessment: from error visibility to
  structural similarity", IEEE Transactions on Image Processing, 2004.
- ITU-T H.264 clause 8.3 (intra prediction used by intra metrics).
- x264 public source: `common/pixel.c`, `common/pixel.h`, `common/x86/pixel-a.asm`, checkasm.

---

## 10. Verification checklist

1. **SAD/SSD:** verify against a brute-force loop for random blocks and all sizes; check the
   `ssd_wxh` remainder logic with non-multiple-of-16 widths/heights and both aligned and
   unaligned pointers.
2. **SATD:** compute the 4x4 Hadamard manually for a known difference block and confirm the
   `>>1` scaling; verify each composed size equals the sum of its sub-blocks.
3. **SA8D:** verify the 8x8 transform and `(sum+2)>>2` rounding, and that 16x16 sums four 8x8s.
4. **Packed arithmetic:** at high bit depth confirm `sum_t`/`sum2_t` widths and `abs2` on
   negative packed values.
5. **Batched forms:** `sad_x3/x4` and `satd_x3/x4` must return exactly the same scores as N
   separate calls with identical inputs.
6. **Intra helpers:** verify V/H/DC candidates against the corresponding `predict` functions
   followed by SAD/SATD/SA8D; for x9, compare to `checkasm` vectors or the documented fallback.
7. **SSIM:** check constants and rolling-sum alignment; compare against a direct SSIM
   implementation on a small image within floating-point tolerance.
8. **ADS/vsad/asd:** verify thresholds and the sign convention (`ads < thresh`, strict).
9. **Dispatch equivalence:** every SIMD entry must match the scalar reference bit-exactly
   over thousands of random blocks (checkasm methodology).

---

## 11. Open questions

- `UNKNOWN`: the exact metric used for `mbcmp` (SAD vs SATD) is a caller/configuration choice;
  the file supplies both.
- `UNKNOWN`: no portable C implementation of `intra_satd_x9_*`/`intra_sad_x9_*` exists in this
  file; its algorithm must be obtained from `checkasm` or the alternative analyser path, so a
  clean-room implementation should document this gap explicitly.
- `UNKNOWN`: architecture-specific overreads in SIMD metrics are not specified; callers must
  provide padded buffers.
