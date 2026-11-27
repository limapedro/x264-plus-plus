# deblock.h — x86 deblocking interface

Source path: `common/x86/deblock.h` (include guard `X264_X86_DEBLOCK_H`).
Role: declares all x86 deblocking function-pointer targets and their ISA variants.
**Dirty-side behavioural specification** — semantics in own words only.

---

## 1. Header

This header is the declaration surface for `common/x86/deblock-a.asm`. It uses the standard
`x264_template` name mangling and groups declarations by filter kind and SIMD level. No code
is defined here except the 8-bit inline wrapper for `deblock_v_luma_mmx2`.

## 2. Purpose

The deblocking filter has many shape variants (luma/chroma, 4:2:0/4:2:2, MBAFF, intra/normal,
vertical/horizontal) and must select the fastest one available for the current CPU. This
header fixes a uniform calling convention across all of them so that `deblock.c` can install
function pointers into `x264_deblock_function_t` without knowing which ISA was chosen.

## 3. Public interface

### 3.1 Inter (normal) filters

All take `(pixel *pix, intptr_t stride, int alpha, int beta, int8_t *tc0)` and return void:

* `deblock_v_luma`, `deblock_h_luma`: sse2, avx; plus mmx2 (header wrapper on 8-bit).
* `deblock_v_chroma`, `deblock_h_chroma`: mmx2 (32-bit), sse2, avx.
* `deblock_h_chroma_mbaff`: mmx2 (32-bit), sse2, avx.
* `deblock_h_chroma_422`: mmx2, sse2, avx.
* 32-bit-only helper `deblock_v8_luma_mmx2(uint8_t*, stride, alpha, beta, tc0)` filters an
  8-pixel segment; the inline wrapper calls it twice with `tc0` and `tc0+2`.

### 3.2 Intra (strong) filters

All take `(pixel *pix, intptr_t stride, int alpha, int beta)` and return void:

* `deblock_v_luma_intra`, `deblock_h_luma_intra`: sse2, avx; mmx2 wrapper on 8-bit.
* `deblock_v_chroma_intra`, `deblock_h_chroma_intra`: mmx2 (32-bit), sse2, avx.
* `deblock_h_chroma_intra_mbaff`, `deblock_h_chroma_422_intra_mmx2/sse2/avx`.
* 32-bit-only `deblock_v8_luma_intra_mmx2`.

### 3.3 Strength kernel

`deblock_strength(uint8_t nnz[X264_SCAN8_SIZE], int8_t ref[2][X264_SCAN8_LUMA_SIZE],
int16_t mv[2][X264_SCAN8_LUMA_SIZE][2], uint8_t bs[2][8][4], int mvy_limit, int bframe)`
available as sse2, ssse3, avx, avx2, avx512. Return void; writes all 64 `bs` bytes.

### 3.4 Parameter contracts

* `pix` points to the first sample of the second block (`q0`). Samples `p3..p0` are before it
  and `q1..q3` after it, stepping by 1 for vertical edges and by `stride` for horizontal.
* `stride` is in addressable units: for 8-bit it is a byte stride; the 10-bit kernels double
  it internally because `pixel` is 16-bit.
* `alpha`, `beta` are *already* QP-derived and *already* scaled by `1 << (BIT_DEPTH-8)` by the
  caller. The kernel does not apply QP offsets.
* `tc0[i]` is the clipping bound for the i-th four-pixel group of a luma edge. Negative means
  "skip"; zero means "no clipping of p1/q1 but the central two samples may still change".
  Chroma kernels receive a single array with the caller's chroma offset already added.
* No function reads or writes outside the six samples adjacent to the edge per segment.

## 4. State and data structures

* `pixel` — `uint8_t` in 8-bit builds, `uint16_t` in 10-bit builds.
* `x264_deblock_inter_t` / `x264_deblock_intra_t` — function-pointer types in
  `common/frame.h`; each ISA variant matches one of these exactly.
* MB cache arrays are padded 8-wide scan grids: `X264_SCAN8_LUMA_SIZE = 40`,
  `X264_SCAN8_SIZE = 120`, `X264_SCAN8_0 = 4 + 1*8`. The strength kernel addresses
  neighbours at negative offsets from this origin; the padding is guaranteed by the caller.
* `bs[2][8][4]` — `[direction][edge][sample]`; direction 0 is vertical (left/right edge),
  direction 1 is horizontal (top/bottom edge). Values 0/1/2 map to the three filter cases.

## 5. Algorithm / behaviour

### 5.1 Filter selection logic (caller side, for context)

For each edge the caller computes `index_a = QP + a`, `index_b = QP + b` and looks up
`alpha`, `beta`, `tc0` from QP tables. If `alpha` or `beta` is zero, or all four `bs` entries
are zero, the edge is skipped. Intra macroblock edges are always passed to the intra filter
regardless of `bs`. This explains why the kernels assume nonzero `alpha`/`beta`.

### 5.2 Inter kernel semantics (summary)

The inter kernels implement the H.264 default filter: three threshold tests
(`|p0-q0| < alpha`, `|p1-p0| < beta`, `|q1-q0| < beta`) gate the operation; optional p1/q1
modification occurs under `|p2-p0| < beta` / `|q2-q0| < beta`; and a central delta
`clip(((q0-p0)*4 + (p1-q1) + 4) >> 3, -tc, tc)` is applied symmetrically with output
clipping to the legal sample range. Full dataflow, including the byte/word trickery, is in
`deblock-a.asm.md` §5.2–5.3.

### 5.3 Intra kernel semantics (summary)

Strong intra filter uses `alpha/4+2` to choose between a 5-tap and a 4-tap filter for each
side, with `p2'` / `q2'` only when the corresponding `beta` test passes. Weak intra filter
uses the 4-tap formula unconditionally when the main threshold test passes. See
`deblock-a.asm.md` §5.4–5.5.

### 5.4 Strength kernel semantics (summary)

`bs` is 2 if either adjacent 4x4 block has nonzero coefficients; else 1 if reference
indices differ or an MV component exceeds 4 horizontal or `mvy_limit` vertical; else 0.
B-frame mode also checks list 1. See `deblock-a.asm.md` §5.6 for the SIMD ordering.

### 5.5 Edge ordering and plane traversal

* Vertical luma: four 4-pixel groups per macroblock edge, 16 rows total; the 16-pixel SSE2
  kernel processes one row per vector and loops twice.
* Horizontal luma: same four groups but each spans four rows; the kernel transposes 8 rows
  at a time.
* Chroma 4:2:0: four groups, each 2 pixels wide by 2 rows; MBAFF uses 1 row and 4:2:2 uses
  4 rows. Vertical chroma loops over 16 rows with 2-pixel groups; horizontal chroma loops
  over 8 rows.
* Intra luma: all 16 samples along the edge; intra chroma: 16 samples for vertical, 8 for
  horizontal, 16 for 4:2:2.
* Plane order in the caller is Y, then U, then V; the kernels themselves operate on one
  plane per call.

### 5.6 8-bit vs 10-bit differences

The header's `#if ARCH_X86` block provides 8-bit-only MMX2 declarations and a conditional
wrapper for the 16-pixel vertical luma kernel. `#if HIGH_BIT_DEPTH` switches the two
`deblock_v_luma_mmx2` declarations to real functions (10-bit path) instead of the inline
split into two 8-pixel calls. The comment in the header explicitly flags that wrapper as
having significant CPU cost — the reason real 16-wide kernels exist for SSE2/AVX.

### 5.7 ISA dispatch

`x264_deblock_init(cpu, pf)` in `deblock.c` assigns the highest available variant. The header
lists variants up to AVX512 for `deblock_strength` but only up to AVX for pixel filters,
reflecting that the strength kernel benefits most from wide compares and opmask arithmetic.

## 6. Dependencies

* `common/frame.h` — function-pointer types and dispatch table.
* `common/deblock.c` — threshold tables, cache loading, dispatch initialization, C fallbacks.
* `common/base.h` — scan8 constants, `BIT_DEPTH`, `pixel` type.
* `common/x86/deblock-a.asm` — implementations.

## 7. Constants and tables

* `X264_SCAN8_LUMA_SIZE = 40`, `X264_SCAN8_SIZE = 120`, `X264_SCAN8_0 = 12`.
* QP threshold tables in `deblock.c`: `i_alpha_table[64]`, `i_beta_table[64]`,
  `i_tc0_table[64][4]`, indexed with QP+24 / QP+24+offset.
* Boundary-strength thresholds: MV horizontal 4, vertical `mvy_limit` (typically 1 or 2
  depending on field coding).

## 8. Performance notes

The pixel kernels are branchless SIMD but transpose-heavy on horizontal edges; the strength
kernel is compare-heavy and benefits most from AVX2/AVX512.

**Concrete SIMD speed opportunities (referenced):**
1. *Wider vectors*: use the existing YMM/ZMM strength kernels as the template for widening
   pixel kernels; combine two 8-pixel vertical segments into one 16-pixel vector.
2. *Prefix/scan*: the neighbor-byte gathering in the strength kernel is a fixed permutation;
   implement it as one `pshufb`/`vpermd` rather than several shifts (already done in AVX2/512).
3. *Branch removal*: replace conditional writes with mask blends (`pand`/`pandn`/`por` or
   masked AVX512 stores); avoid scalar `if (tc0[i] < 0)` inside the kernel by having the
   caller skip the segment.
4. *Transpose avoidance*: for high-bit-depth horizontal edges, keep data transposed across
   calls if the caller can arrange it; otherwise use fused transpose-store macros as the
   assembly does.

## 9. References

* ITU-T H.264 clause 8.7 / 8.8 — deblocking filter and boundary strength.
* x264 public source: `common/x86/deblock.h`, `common/x86/deblock-a.asm`, `common/deblock.c`.
* Intel Intrinsics Guide for the SIMD instructions named above.

## 10. Verification checklist

1. For every declared function pointer, confirm the ISA suffix chosen by
   `x264_deblock_init` matches the CPU capabilities and is non-null.
2. Compare each function against its C counterpart on randomized buffers for both bit
   depths, all `bs` patterns, and QP 0..51.
3. Verify `deblock_strength` output byte-for-byte against the C loop on random caches,
   including `bframe=1` and `mvy_limit` edge values.
4. Check that negative `tc0` entries leave pixels untouched, and that zero `alpha`/`beta`
   cause the caller to skip the call.
5. Run `checkasm` with stack-clobber patterns to catch register-allocation mistakes.

## 11. Open questions

* **UNKNOWN**: whether `deblock_h_luma_mmx2` is dispatched anywhere in 64-bit builds (it is
  declared only under `#if ARCH_X86`).
* **UNKNOWN**: exact performance cost of the MMX2 inline wrapper, described only
  qualitatively in the header.
* Some declarations (e.g. `deblock_v_chroma_intra_mbaff_mmx2`) exist in 8-bit builds only;
  whether they are reachable on 64-bit hosts is **UNKNOWN**.
