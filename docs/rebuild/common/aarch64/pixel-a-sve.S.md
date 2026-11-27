# `common/aarch64/pixel-a-sve.S` — SVE pixel metrics (SSD, variance, SA8D, Hadamard-AC)

> Dirty-side behavioural specification (reverse-engineered from the reference encoder; describes behaviour, not code).

## 1. Header

- Source path: `common/aarch64/pixel-a-sve.S`
- Role: SVE implementations of SSD, variance, SA8D (8×8), and Hadamard-AC metrics for 8-bit pixels, plus SSD for 10-bit pixels.
- This is a **dirty-side behavioural specification** artifact for the clean-room reimplementation.

## 2. Purpose

Rate-distortion optimization calls pixel metrics millions of times per frame. The SVE kernels replace NEON single-lane/wide loads with predicated widening loads and use SVE's horizontal reductions (`addv`, `uaddlv`) to cut instruction counts. They must produce exactly the same integer values as the NEON kernels, since any difference changes encoder decisions and bitstreams.

## 3. Public interface

Exported functions (declared in `pixel.h`):
- `int x264_pixel_ssd_{4x4,4x8,4x16,8x4,8x8}_sve( pixel *pix1, intptr_t i_pix1, pixel *pix2, intptr_t i_pix2 )`
- `uint64_t x264_pixel_var_8x8_sve( pixel *pix, intptr_t stride )`
- `uint64_t x264_pixel_var_8x16_sve( pixel *pix, intptr_t stride )`
- `int x264_pixel_sa8d_8x8_sve( pixel *pix1, intptr_t i_pix1, pixel *pix2, intptr_t i_pix2 )`
- `uint64_t x264_pixel_hadamard_ac_{8x8,8x16,16x8,16x16}_sve( pixel *pix, intptr_t stride )`

Internal helpers: `var_end`, `satd`-style `sa8d_satd_sve_8x8`, `hadamard_ac_8x8_sve`.

## 4. State and data structures

- Predicates: `p0.h` with `vl4` or `vl8` depending on block width; `p2` not needed except in interleave (not here).
- Accumulators: 4-lane 32-bit vectors for SSD (`v0`), 4-lane 32-bit for variance squares (`v1`/`v2`), 8-lane 32-bit for hadamard-ac sums (`v28`/`v29`).
- `v30`/`v31` hold the two AC masks from `pixel-a-common.S`; `v28` accumulates SATD and `v29` accumulates SA8D in hadamard-ac.

## 5. Algorithm / behaviour

### SSD (8-bit)

For width 4: predicate `vl4` halfword lanes; load four bytes from each row widened to halfwords, compute the signed difference, multiply-accumulate into a 4-lane 32-bit vector (`smull`/`smlal`); unroll `height−2` middle rows, then a final row. Finish with `addv s0, v0.4s` — horizontal sum of 32-bit lanes — and return as `w0`.

For width 8: predicate `vl8`; same structure but `smlal`/`smlal2` accumulate both halves of the 8-halfword difference vector. Heights 4, 8, 16 supported for width 4; widths 4 and 8 for height 4/8.

10-bit section: predicate `p0.s` with `vl4`, 16-bit halfword loads widened to 32-bit, `mul`/`mla` accumulation directly in 32-bit lanes, `addv`+`fmov` to return. Only widths 4 and heights 4, 8, 16 are instantiated.

No saturation; sums fit in 32 bits for the supported block sizes.

### Variance (`pixel_var_8x{h}_sve`)

1. Predicate `vl8`. Load two rows as widened bytes.
2. Initialize two 32-bit accumulators with the squares of the first two rows (`mul`, `uaddlp` to pairwise-add 16-bit squares into 32-bit lanes).
3. Initialize a 16-bit sum accumulator with the first two rows' sample sum.
4. Loop over remaining rows in groups of four: add each row to the sum accumulator, compute its squares, and pairwise-add into alternating 32-bit accumulators (`uadalp`).
5. Add the final two rows similarly.
6. `var_end`: add the two square accumulators, horizontally sum the 16-bit sample sum (`uaddlv s0`) and the 32-bit square sum (`uaddlv d1`), pack the two 32-bit halves of the sample sum into `w0` and the square sum into `x1`, and return the 64-bit combination `sample_sum | (square_sum << 32)`.

Return convention matches the NEON `pixel_var_*`: low 32 bits = sum of samples, high 32 bits = sum of squares.

### SA8D 8×8 (`pixel_sa8d_8x8_sve` and `sa8d_satd_sve_8x8`)

The wrapper calls the shared 8×8 Hadamard core (which returns two 8-halfword vectors `v0`/`v1` holding the per-lane absolute-transform sums), adds them, horizontally sums with `uaddlv`, adds 1 and shifts right by 1 (`(sum+1)>>1`) to produce the SA8D value. This is twice the average of absolute Hadamard coefficients, matching x264's SA8D definition.

The core itself:
1. Loads eight rows of eight bytes (widening to halfwords) and forms the eight signed differences `pix1 - pix2`.
2. Applies a 1-D Hadamard pass along rows (`SUMSUB_AB` pairs), then a second 1-D pass (`HADAMARD4_V`).
3. Optionally (when the `satd_` variant is requested) transposes, applies another Hadamard pass, takes absolute values, and pairwise-maxes pairs of lanes — producing both the SATD and SA8D contributions in separate vectors.
4. Applies the final 2-D Hadamard stages with transposes, then `abs` and `umax` pairwise reductions, leaving two 8-lane vectors that are summed by the wrapper.

### Hadamard-AC (`HADAMARD_AC_SVE`)

1. Load masks into `v30`/`v31`, clear accumulators `v28`/`v29`.
2. Call `hadamard_ac_8x8_sve` once for 8×8, twice for 8×16, plus a third and fourth time for 16-wide / 16×16 cases, adjusting the input pointer between calls (`−8*stride+8` for the right half, `−16*stride` for the bottom).
3. `hadamard_ac_8x8_sve` computes an 8×8 Hadamard transform of the *samples* (not differences), takes absolute values, ANDs the AC mask into the SATD accumulator (`v28`) to exclude DC, computes a second set of pairwise differences for the sa8d-like accumulator (`v29`) with the other mask, and accumulates with `uadalp`.
4. Final reduction: `addv` both accumulators, extract to `w0`/`w1`, shift `w0` right by 1 and `w1` right by 2, and combine as `w0 | (w1 << 32)`.

Return convention: low 32 bits = SATD/DC-inclusive sum >> 1, high 32 bits = AC-only sum >> 2.

Rounding: SA8D uses `(sum+1)>>1`; hadamard-ac uses plain logical shifts by 1 and 2 (no rounding). No saturation in accumulators (64-bit return values).

### Metric equations and lane widths

| Kernel | Width predicate | Accumulator | Final reduction |
| --- | --- | --- | --- |
| SSD 4-wide | `p0.h, vl4` | 4×32-bit (`v0`) | `addv s0` |
| SSD 8-wide | `p0.h, vl8` | 4×32-bit with `smlal`/`smlal2` | `addv s0` |
| SSD 10-bit | `p0.s, vl4` | 4×32-bit with `mul`/`mla` | `addv` + `fmov` |
| Variance 8×h | `p0.h, vl8` | 4×32-bit squares ×2, 8×16-bit sum | `uaddlv` twice |
| SA8D 8×8 | `p0.h, vl8` | 8×16-bit | `uaddlv` + `(sum+1)>>1` |
| Hadamard-AC | `p0.h, vl8` | 8×32-bit ×2 | `addv` + shifts 1/2 |

SSD computes `sum (pix1 − pix2)²` with signed byte-to-halfword widening (`ld1b` under `p0.h`) and `smull`/`smlal` into 32-bit lanes; the 10-bit variant uses 16-bit loads widened to 32-bit and direct `mul`/`mla`. There is no saturation in either case.

Variance returns a packed 64-bit value according to the same convention as the NEON kernels: low 32 bits are `sum(pix)`, high 32 bits are `sum(pix²)`. The two square accumulators are paired so that `uadalp` can accumulate four rows at a time without overflow.

SA8D returns `(Σ|H(diff)| + 1) >> 1` for an 8×8 block. The Hadamard core produces two 8-lane vectors of per-lane absolute sums whose horizontal total is taken by `uaddlv`.

Hadamard-AC's two returned halves are `sum0 >> 1` (DC-inclusive absolute coefficient sum) and `sum1 >> 2` (AC-only sum). The AC masks clear the DC lane before accumulation; there is no rounding in these shifts.

### Predicate widths and hardware vector length

`ptrue p0.h, vl4` and `vl8` restrict activity to four or eight halfword lanes regardless of the physical SVE vector length. This makes the kernels produce identical results on 128-bit, 256-bit, and 512-bit SVE implementations. The `st1`/`ld1` forms are predicated, so inactive lanes neither read nor write memory.

### Dispatch conditions

Installed when `HAVE_SVE` and `X264_CPU_SVE` are set. In 8-bit builds the overrides cover five SSD shapes, `sa8d[8x8]`, `var[8x8]`, `var[8x16]`, and all four `hadamard_ac` entries. In 10-bit builds a smaller SSD subset is wired (see the UNKNOWN in section 11). SAD and SATD remain NEON in both cases.

## 6. Dependencies

- Includes `asm.S` and `pixel-a-common.S`.
- Requires `.arch armv8-a+sve`.
- Dispatched from `common/pixel.c` when `HAVE_SVE` and `X264_CPU_SVE`: overrides SSD entries, `var[8x8]`, `var[8x16]`, `sa8d[8x8]`, and all four `hadamard_ac` entries.

## 7. Constants and tables

- `mask_ac_4_8` (from `pixel-a-common.S`).
- `1`: SA8D rounding increment.
- Shifts `1`, `2`, `3`, `6` in Hadamard reductions and pointer arithmetic.
- Predicate widths `vl4`/`vl8`.

## 8. Performance notes

Hot metrics. SVE's predicated loads reduce instruction count for narrow (4-wide) SSD, and `addv`/`uaddlv` replace multi-instruction NEON reductions. Optimization tip: SVE's variable-length width is not exploited because predicates are fixed at `vl4`/`vl8`; a multi-block SVE version could process several candidates at once for x3/x4 SAD-style calls (not implemented here). On AVX2, use `_mm256_sad_epu8` for SAD, `_mm256_madd_epi16` for SSD squares, and `_mm256_hadd_epi32` for reductions. Public references: ARM SVE reduction intrinsics; x264 pixel metric definitions.

## 9. References

- x264 pixel metric definitions (`common/pixel.c`).
- ARM SVE Architecture Reference Manual (`ld1b`, `addv`, `uaddlv`, `uadalp`).
- `docs/rebuild/common/aarch64/pixel-a-common.S.md`, `pixel-a.S.md`, `pixel.h.md`.

## 10. Verification checklist

- SVE and NEON SSD/variance/SA8D/hadamard-ac produce identical values for random blocks of all supported sizes.
- Return conventions for variance and hadamard-ac (packed low/high 32-bit halves) verified against the generic implementations.
- SA8D rounding `(sum+1)>>1` verified at boundary values.
- Predicated loads touch exactly the block width (guard-page test).
- Works on SVE hardware with vector lengths > 128 bits (fixed-width predicates must restrict activity).

## 11. Open questions

- UNKNOWN: whether the 10-bit SSD kernels are dispatched in 10-bit SVE builds (the dispatch code in `common/pixel.c` shows them guarded by `HAVE_SVE` and `!HIGH_BIT_DEPTH` conditions; verify).
- UNKNOWN: exact mapping of the two hadamard-ac return halves to the generic `pixf->hadamard_ac` contract (test against the C reference).
