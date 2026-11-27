# `common/x86/pixel.h` — x86 pixel metric declarations

**Dirty-side behavioural specification.** Source path: `common/x86/pixel.h`.
Role: declares the complete set of x86 SIMD pixel comparison, distortion, and
intra-prediction metric entry points, grouped by ISA suffix and block size.

---

## 1. Header

Declarations-only header. It consists almost entirely of `#define
x264_<name>_<suffix> x264_template(<name>_<suffix>)` lines followed by
prototypes, plus four macro families (`DECL_PIXELS`, `DECL_X1`, `DECL_X4`,
`DECL_ADS`) that expand prototypes for all eight block sizes. No data or code is
defined. It is included by the x86 dispatch initialisation code.

## 2. Purpose

The portable encoder's `pixel.c`/`mc.c` function tables are filled at runtime
with the best available x86 implementation. This header is the authoritative
list of which (metric × block size × ISA) combinations exist, and therefore the
authoritative checklist for a clean-room reimplementation's dispatch surface.

## 3. Public interface

### 3.1 Naming convention
Every symbol is `x264_pixel_<metric>_<WxH>_<isa>` (or
`x264_intra_<metric>_x<3|9>_<WxH>_<isa>`), and every symbol is wrapped in
`x264_template(...)` so the build can prefix/version it. `isa` is one of:
`mmx`, `mmx2`, `sse`, `sse2`, `sse2slow`, `sse2_aligned`, `sse3`, `ssse3`,
`ssse3_aligned`, `ssse3_atom`, `sse4`, `avx`, `xop`, `avx2`, `avx512`,
`cache32_mmx2`, `cache64_mmx2`, `cache64_sse2`, `cache64_ssse3`.

### 3.2 Single-plane metrics (`DECL_X1`)
Signature: `int pixel_<name>_<WxH>_<isa>(pixel *pix1, intptr_t i_stride1,
pixel *pix2, intptr_t i_stride2)`.
* **sad**: single-reference sum of absolute differences. Sizes all eight; ISAs
  mmx2, sse2, sse3, sse2_aligned, ssse3, ssse3_aligned, avx2, avx512,
  cache32_mmx2, cache64_mmx2, cache64_sse2, cache64_ssse3. (Implemented in
  `sad-a.asm`/`sad16-a.asm`, not in this file's assembly.)
* **ssd**: sum of squared differences. Sizes all eight; ISAs mmx, mmx2,
  sse2slow, sse2, ssse3, avx, xop, avx2.
* **satd**: sum of absolute Hadamard differences. Sizes all eight; ISAs mmx2,
  sse2, ssse3, ssse3_atom, sse4, avx, xop, avx2, avx512.
* **sa8d**: only 16x16, 16x8, 8x16, 8x8; ISAs mmx2, sse2, ssse3, ssse3_atom,
  sse4, avx, xop, avx2, avx512.

### 3.3 Multi-reference SAD (`DECL_X4`)
* `void pixel_sad_x3_<WxH>_<isa>(pixel *pix1, pixel *pix2, pixel *pix3,
  pixel *pix4, intptr_t i_stride, int sad[3])`.
* `void pixel_sad_x4_<WxH>_<isa>(pixel *pix1, pixel *pix2, pixel *pix3,
  pixel *pix4, pixel *pix5, intptr_t i_stride, int sad[4])`.
* Compute SAD between `pix1` and each of the other 2/3 references sharing the
  same stride, writing results into `sad[]`. ISAs: mmx2, sse2, sse3, ssse3,
  xop, avx, avx2, avx512; plus cache variants. (Implemented in the SAD files.)

### 3.4 Single-input metrics
* `uint64_t pixel_var_<WxH>_<isa>(pixel *pix, intptr_t i_stride)` — ISAs sse2,
  avx, avx2, avx512. Returns packed `(sumsq<<32)|sum`.
* `uint64_t pixel_hadamard_ac_<WxH>_<isa>(pixel*, intptr_t)` — ISAs mmx2, sse2,
  ssse3, ssse3_atom, sse4, avx, xop, avx2. Returns packed SATD/SA8D values.
* `int pixel_var2_8x8_<isa>(pixel *fenc, pixel *fdec, int ssd[2])` and `8x16`
  forms — ISAs sse2, ssse3, avx2, avx512.
* `int pixel_vsad_<isa>(pixel *src, intptr_t stride, int height)` — mmx2, sse2,
  ssse3, xop, avx2 (AVX2 form takes `uint16_t*`). Vertical SAD.
* `int pixel_asd8_<isa>(pixel*, intptr_t, pixel*, intptr_t, int height)` — sse2,
  ssse3, xop.

### 3.5 Intra prediction metrics
* `void intra_satd_x3_4x4_mmx2(pixel*, pixel*, int*)`;
  `void intra_sad_x3_4x4_{mmx2,sse2,ssse3,avx}(uint8_t*/uint16_t*, ..., int*)`.
* `void intra_satd_x3_8x8c_{mmx2,ssse3}`, `intra_sad_x3_8x8c_*`,
  `intra_satd_x3_16x16_mmx2/ssse3`, `intra_sad_x3_16x16_*`,
  `intra_sa8d_x3_8x8_{mmx2,sse2}`, `intra_sad_x3_8x8_*`.
* `int intra_satd_x9_4x4_{ssse3,sse4,avx,xop}`,
  `int intra_sad_x9_4x4_{ssse3,sse4,avx}` — signature
  `(uint8_t *fenc, uint8_t *fdec, uint16_t *bitcosts)`.
* `int intra_sa8d_x9_8x8_{ssse3,sse4,avx}` and
  `int intra_sad_x9_8x8_{ssse3,sse4,avx,avx2}` — signature
  `(uint8_t *fenc, uint8_t *fdec, uint8_t edge[36], uint16_t *bitcosts,
  uint16_t *satds)`.
* Return `(best_cost << 16) | (mode_index+1)`; x3 variants write three costs
  into `int res[3]`.

### 3.6 NV12 / chroma helpers
* `void pixel_ssd_nv12_core_{sse2,avx,xop,avx2}(pixel *pixuv1, intptr_t
  stride1, pixel *pixuv2, intptr_t stride2, int width, int height,
  uint64_t *ssd_u, uint64_t *ssd_v)`.
* `void pixel_ssim_4x4x2_core_{mmx2,sse2,avx}(const uint8_t*/pixel* pix1,
  intptr_t stride1, const uint8_t*/pixel* pix2, intptr_t stride2,
  int sums[2][4])`.
* `float pixel_ssim_end4_{sse2,avx}(int sum0[5][4], int sum1[5][4], int width)`.

### 3.7 Combined metrics
* `uint64_t pixel_sa8d_satd_16x16_{sse2,ssse3,ssse3_atom,sse4,avx,xop,avx2}`
  `(pixel*, intptr_t, pixel*, intptr_t)` — returns `(satd<<32)|sa8d`.

### 3.8 Successive elimination ADS
Macro `DECL_ADS(size,suffix)` expands:
`int pixel_ads<size>_<suffix>(int enc_dc[size], uint16_t *sums, int delta,
uint16_t *cost_mvx, int16_t *mvs, int width, int thresh)`.
Instantiated for sizes 1, 2, 4 and suffixes mmx2, sse2, ssse3, avx, avx2.

## 4. State and data structures

The header defines no state. Relevant caller-side structures:
* Pixel planes with independent strides; `intptr_t` so 64-bit builds can address
  large frames.
* `int sad[3/4]` / `int res[3]` output arrays, naturally aligned.
* `uint16_t *satds` optional per-mode distortion output.
* CABAC/CAVLC bit-cost arrays `bitcosts[]` for intra mode selection.
* `uint8_t edge[36]`: the 8x8 intra prediction edge buffer (left+top+corner
  samples, padded for SIMD).
* `int sums[2][4]` / `int sum[5][4]`: SSIM accumulation buffers.

## 5. Algorithm / behaviour

The header itself defines no algorithms. It encodes the following behavioural
contracts through its type signatures:
* Metrics that return `int` produce 32-bit values; for SAD/SATD the maximum
  block distortion fits comfortably in 32 bits.
* Metrics that return `uint64_t` pack two 32-bit values (`var`: sum/sumsq;
  `hadamard_ac`: satd/sa8d; `sa8d_satd`: sa8d/satd).
* Multi-reference SAD writes results to caller arrays rather than returning.
* Intra x9 returns a packed value *and* writes predicted samples into `fdec`,
  making it a dual-output primitive.
* The `_aligned` suffixes require 16-byte-aligned `pix1`/`pix2` and strides
  that are multiples of 16; other suffixes accept arbitrary (but typically
  16-byte-aligned) addresses.

## 6. Dependencies

* Included by the x86 dispatch initialisation source and by the assembly files'
  callers.
* Uses project typedefs `pixel`, `intptr_t`, and the `x264_template` macro.
* The declared symbols are implemented in `pixel-a.asm`, `pixel-32.asm`,
  `sad-a.asm`, `sad16-a.asm` (the latter two not assigned to this specification
  set).

## 7. Constants and tables

No constants defined in this header. The ISA suffixes are effectively constants
that enumerate dispatch levels; the numeric tables (SSIM C1/C2, hmul masks,
shuffle tables) live in `pixel-a.asm` `.rodata`.

## 8. Performance notes

* The header exposes a large dispatch matrix: up to ~15 ISA variants per metric.
  A clean implementation should mirror this structure so the dispatcher can
  select the fastest variant at runtime without recompilation.
* **Wider vectors:** AVX-512 variants exist for SAD, SATD, SSD (implicitly),
  var, var2, and sa8d; prefer them when available.
* **Transpose strategy:** SATD/SA8D variants with `_atom` suffixes avoid certain
  shuffles slow on Atom CPUs; preserve those dispatch distinctions.
* **Avoiding spills:** the multi-reference SAD variants compute 3/4 SADs in one
  pass to amortise loads of the common reference — a clean implementation
  should keep that structure.
* **Preprocessing constants:** bit costs for intra modes are passed in
  `bitcosts[]`, already precomputed by the caller; do not recompute them.
* **Cache variants:** `cache32_*`/`cache64_*` SAD variants use non-temporal or
  prefetch-aware loads for large-frame motion search; see the SAD files.

## 9. References

* ITU-T H.264 motion estimation / intra prediction definitions.
* SSIM paper (Wang et al.).
* Intel/AMD SIMD reference manuals for the listed ISA suffixes.

## 10. Verification checklist

* Confirm every declared symbol is emitted by some assembly file in a full build
  (a missing symbol is a dispatch crash at runtime).
* Confirm each prototype's argument count/types match the assembly `cglobal`
  declaration (especially `intptr_t` on 64-bit).
* For each metric family, verify that the highest-ISA variant produces identical
  numeric results to the baseline (`mmx2`/`sse2`) variant.
* Verify packed-return metrics' bit layouts with black-box tests.
* Confirm `_aligned` variants are only dispatched with aligned inputs.

## 11. Open questions

* `UNKNOWN`: exact set of suffixes for which an implementation exists in a
  given build configuration (many are gated by `HIGH_BIT_DEPTH`, `ARCH_X86_64`,
  and assembly-optimisation flags).
* `UNKNOWN`: whether `pixel_vsad_avx2` taking `uint16_t*` is only available in
  high-bit-depth builds (signature suggests so; verify at dispatch site).
* `UNKNOWN`: precise numerical contract of `pixel_ads*` return values (packed
  bytes vs bitmask) — consult the C caller.
