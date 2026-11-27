# deblock-a.asm — x86 in-loop deblocking filter kernels

Source path: `common/x86/deblock-a.asm` (declared in `common/x86/deblock.h`).
Role: SIMD implementations of the H.264 in-loop deblocking filter for luma/chroma,
inter/intra, vertical/horizontal edges, plus the boundary-strength computation.
**Dirty-side behavioural specification** — descriptions only, no source reproduction.

---

## 1. Header

This module implements the pixel-facing half of x264's deblocking filter. The companion
`deblock.c` chooses QP-derived thresholds and supplies edge strengths; this file consumes
those thresholds and writes filtered pixels back in place. It also contains the SIMD
boundary-strength kernel that turns per-block motion/nonzero-cache data into the `bs` array.

## 2. Purpose

Deblocking removes block artifacts before a reconstructed frame is used as a reference.
It runs once per decoded frame (and in x264 also on the encoder's reconstruction loop),
touching every luma and chroma sample near a 4x4 block boundary. Because it is a serial,
sample-neighbouring filter over the whole frame, SIMD parallelism within a row/edge and
avoidance of per-sample branches are essential. The module additionally computes boundary
strength from motion vectors, reference indices, and nonzero flags — a comparison-heavy
task that gains from wide vector compares.

## 3. Public interface

Implemented functions (see `deblock.h.md` for signatures):

* Inter (normal) filters: `deblock_v_luma`, `deblock_h_luma`, `deblock_v_chroma`,
  `deblock_h_chroma`, `deblock_h_chroma_mbaff`, `deblock_h_chroma_422`;
  32-bit-only `deblock_v8_luma` helper.
* Intra filters: `deblock_v_luma_intra`, `deblock_h_luma_intra`,
  `deblock_v_chroma_intra`, `deblock_h_chroma_intra`,
  `deblock_h_chroma_intra_mbaff`, `deblock_h_chroma_422_intra`.
* Strength: `deblock_strength`.
* Every function is emitted for multiple ISA levels; the header's list of suffixes
  (`sse2`, `ssse3`, `avx`, `avx2`, `avx512`, and `mmx2` on 32-bit x86) defines the dispatch
  ladder.

Contracts: `pix` points at the edge sample (the first pixel of the right/bottom block);
`stride` is the picture stride in *bytes for 8-bit* and in *element units for 10-bit*
(the assembly doubles it before use in high-bit-depth builds). `alpha`, `beta` are scalar
thresholds scaled by `1 << (BIT_DEPTH-8)`. `tc0` is a pointer to four signed clipping values;
a negative value means "do not filter this segment". Intra variants have no `tc0` and always
apply their strong/weak filters when the threshold tests pass. `deblock_strength` writes a
two-direction, four-edge, four-sample `bs` array and returns void.

## 4. State and data structures

* **Pixel layout.** Around an edge, samples are named `p3 p2 p1 p0 | q0 q1 q2 q3`, with `p0`
  at `pix[-xstride]` and `q0` at `pix[0]`. `xstride` is 1 for vertical edges (neighbouring
  pixels are adjacent in memory) and the picture stride for horizontal edges.
* **Threshold/tc vectors.** `alpha` and `beta` are splatted to every lane (word lanes for
  10-bit, byte lanes for 8-bit). `tc0` is expanded from four signed bytes into a vector with
  one clipping value per lane, replicated across the pixels of a 4-pixel segment.
* **Masks.** All threshold decisions produce an all-ones/all-zeros SIMD mask; arithmetic is
  then performed on all lanes and blended/ANDed with the mask, eliminating per-sample
  branches.
* **Scratch.** Horizontal filters first transpose pixel rows into a temporary stack area
  (`pix_tmp`), call the vertical filter body, then transpose back. Sizes range from 0x30 to
  0x80 bytes plus Windows shadow space; host builds also reserve 16 bytes for ABI alignment.
  Vertical filters need no scratch except `tc`/mask spill slots on 32-bit x86 and the
  high-bit-depth path's five vector-sized stack slots (`tcm`, `ms1`, `ms2`, `am`, `bm`).
* **Scan8 cache.** Strength reads `nnz[48]`, `ref[2][40]`, `mv[2][40][2]` starting at
  `X264_SCAN8_0 = 4 + 1*8`, i.e. the top-left corner of the current macroblock in a padded
  8-wide cache grid. Each list/edge step advances the `ref` pointer by 40 and the `mv`
  pointer by `4*8*5` bytes.

## 5. Algorithm / behaviour

### 5.1 Threshold decision (shared)

A normal (inter) edge is filtered only when all three comparisons hold:

`|p0 - q0| < alpha`, `|p1 - p0| < beta`, `|q1 - q0| < beta`.

In assembly the comparisons are implemented by saturating subtractions in both directions
ORed together, then a saturating subtract of `alpha-1`/`beta-1`; the resulting zero/nonzero
bytes are combined and inverted into a mask. Note the deliberate `threshold - 1` form:
strict `< alpha` is equivalent to `|diff| > alpha-1` failing.

For 10-bit, signed word arithmetic (`psubusw`, `psraw`, `pcmpgtw`) is used so that
10-bit sample values up to 1023 are handled without wraparound. For 8-bit, unsigned byte
saturating arithmetic is used and the final clipping uses `paddusb`/`psubusb` against
`tc`.

### 5.2 Inter luma filter (normal)

1. **Strong inner-sample filtering.** For each of the two outer pairs, the test
   `|p2 - p0| < beta` (respectively `|q2 - q0| < beta`) decides whether the adjacent
   sample `p1`/`q1` is modified. When modified, its new value is the average of the far
   sample and `avg(p0,q0)`, clipped to `±tc0` around the original value; the clip bound is
   then incremented by one for the central filter.
2. **Central filter.** A delta is formed from `(q0-p0)*4 + (p1-q1) + 4`, shifted right by
   three, and clipped to `[-tc, tc]`. The reference applies this delta as
   `p0' = clip_pixel(p0 + delta)`, `q0' = clip_pixel(q0 - delta)`. The SIMD implementation
   computes the same value with average-based identities and applies the clip with
   saturating adds/subtracts; rounding parity is corrected using `(p0^q0)&1`.
3. The mask from step 1 gates the writes: lanes that fail any threshold retain their
   original pixels. A `tc0 <= 0` lane is excluded entirely.

The vertical variant reads six rows directly (`p2,p1,p0` before `pix`, `q0,q1,q2` at/after
it) and processes 16 pixels per iteration on XMM (two iterations per 4x4 segment for MMX).
The horizontal variant transposes 8 rows of pixels into temporary storage, runs the same
vertical body with `xstride = 1`, then transposes the four *changed* middle rows back.

### 5.3 Inter chroma filter (normal)

Chroma uses only four samples (`p1 p0 | q0 q1`) and no `p2/q2` correction. The same
threshold test gates a two-tap central filter; chroma `tc` values are the luma `tc0`
increased by a caller-supplied chroma offset. The vertical path handles 4 or 8 column
pairs per iteration depending on vector width; the horizontal path transposes rows of four
bytes (8-bit) or four words (10-bit) in and out. MBAFF horizontal chroma filters only one
row of 8 pixels and a 4:2:2 horizontal chroma filter processes 16 rows.

### 5.4 Strong intra luma filter

The intra filter has no `tc0`; it uses a stronger multi-tap filter when
`|p0-q0| < alpha/4 + 2` (8-bit) or `alpha>>2 + 2` (10-bit), and a weaker filter otherwise.
The strong path computes:

* `p0' = (p2 + 2p1 + 2p0 + 2q0 + q1 + 4) >> 3`,
* `p1' = (p2 + p1 + p0 + q0 + 2) >> 2`,
* `p2' = (2p3 + 3p2 + p1 + p0 + q0 + 4) >> 3` when `|p2-p0| < beta`,

with symmetric formulas on the q side (using `q3` for `q2'`). If the outer test fails, only
`p0'` (and symmetrically `q0'`) is updated with the four-tap formula
`(2p1 + p0 + q1 + 2) >> 2`. The weak path applies that four-tap formula to both sides.
All decisions are computed as masks, then used to blend between original and filtered
values via XOR/AND sequences (8-bit) or masked adds (10-bit).

Scratch for the 8-bit strong filter includes `p3`/`q3` registers held across the two halves;
the horizontal variant transposes a full 8x8 block, calls the vertical body, and transposes
back. The 10-bit strong filter uses a different structure-of-arrays formulation but the same
arithmetic and masks (`alpha/4+2` computed as `alpha>>2` plus a constant).

### 5.5 Intra chroma filter

Only `p1 p0 | q0 q1` are used. The weak two-tap formula
`p0' = (2p1 + p0 + q1 + 2) >> 2` and its q-side mirror are applied when the normal threshold
test passes; otherwise pixels are unchanged. 8-bit uses `pavgb` with a parity correction;
10-bit uses word adds and a right shift with masks. MBAFF and 4:2:2 variants change only the
number of rows traversed.

### 5.6 Boundary-strength kernel

The strength kernel computes, for each of the two edge directions and four 4x4 edges, four
boundary strength samples:

1. Start at 0.
2. If either side of the edge has any nonzero coefficient (`nnz` cache), set strength 2.
3. Otherwise, if the two sides have different reference indices, or the horizontal MV
   difference is at least 4, or the vertical MV difference is at least `mvy_limit`, set
   strength 1. In B-frames the test also applies to list 1 (second reference list).
4. Clamp all strengths to the range 0..2 and store them into `bs[dir][edge][sample]`.

The SIMD implementation loads neighboring cache rows with `palignr` or `psllq`+`shufps`,
XORs reference indices to detect inequality, subtracts and absolutizes motion-vector
components, and compares against a splat of `(mvy_limit << 8) | 3` (encoding the
horizontal threshold 4 and the vertical threshold `mvy_limit` in one word). Nonzero flags
are loaded last, doubled to give strength 2, and combined with `pmaxub` so that coefficient
information overrides motion information.

**Output ordering.** `bs0` receives the vertical-direction strengths after a byte shuffle
(`pshufb` with `transpose_shuf`, or `movhlps`+`punpcklbw` chains on SSE2), `bs1` receives
the horizontal-direction strengths. The 32-byte output is written as two aligned 16-byte
stores. AVX2 handles both directions simultaneously in YMM lanes and extracts with
`vextracti128`; AVX512 broadcasts 64-bit MV halves into ZMM lanes, uses `pmaddubsw` to
difference adjacent reference bytes, `vpternlogd` to combine comparisons, and opmask
`vptestmw`/masked `vpaddb`/`vpmaxub` to build the strengths without any branch.

**Loop structure.** The outer loop walks list 0 and list 1 (`bframe ? 2 : 1` iterations, the
count supplied as an argument), then a single pass over the nonzero cache finishes the
score. Plane traversal is implicit: the scan8 cache covers luma; chroma strengths are
derived by the caller from the same `bs` data.

### 5.7 8-bit vs 10-bit differences

* 10-bit files use word lanes throughout; `alpha`/`beta` are already shifted up by two bits
  by the caller. The 8-bit implementation compensates for saturation with byte-rounding
  tricks and MBAAFF/422 variants that exist only in that path.
* Horizontal 10-bit filters transpose 4/8 words and use `TRANSPOSE4x4W`/`TRANSPOSE8x8W`
  macros; 8-bit horizontal filters transpose bytes via `TRANSPOSE4x8W_LOAD` and
  `TRANSPOSE8x2W_STORE` macros.
* 10-bit has no MMX path on 32-bit x86 (MMX cannot handle the sample range); 8-bit retains
  MMX2 variants for several kernels.
* Both bit depths have SSE2/AVX variants for the main luma/chroma kernels; AVX2/AVX512 are
  used only for the strength kernel.

### 5.8 ISA dispatch and alignment

`INIT_MMX mmx2`, `INIT_XMM sse2/ssse3/sse4/avx`, `INIT_YMM avx2`, `INIT_ZMM avx512` select
vector width and instruction set at assembly time. 64-bit hosts use full-register XMM paths
for most luma filters; 32-bit hosts use MMX for the byte filters to conserve registers.
`LOAD_AB` splats thresholds with `punpck`/`pshuf` on SSE2 and `pshufb` on SSSE3. Temporary
buffers on the stack are aligned to 16 bytes on 64-bit; 32-bit stack alignment is arranged
so that after pushing call arguments the vertical helper sees aligned storage.

## 6. Dependencies

* `x86inc.asm` / `x86util.asm` — `cglobal`, `INIT_*`, `TRANSPOSE*`, `SBUTTERFLY`,
  `CLIPW`, `SPLATW`, `FIX_STRIDES`, `DECLARE_REG_TMP`.
* `deblock.c` — computes `alpha`, `beta`, `tc0` from QP and calls the function pointers;
  also holds the scalar fallbacks and the cache-loading logic.
* `common.h`/`base.h` — `pixel`, `BIT_DEPTH`, `X264_SCAN8_*` constants.
* `x264_frame_t` / `x264_t` — reconstructed image buffers and MB cache arrays.

## 7. Constants and tables

* `pw_4`, `pw_2`, `pw_00ff`, `pw_pixel_max` — rounding constants and clipping bounds for
  word arithmetic; `pb_1`, `pb_3`, `pb_a1` — rounding/parity/clip constants for byte
  arithmetic; `pb_unpackbd1` — AVX blend shuffle for expanding `tc0`.
* `transpose_shuf` — byte permutation that converts the strength kernel's lane order into
  `bs[dir][edge][i]` order.
* `load_bytes_ymm_shuf`, `load_bytes_zmm_shuf` — shuffle constants for gathering the
  strided scan8 neighbor bytes in AVX2/AVX512 strength kernels.
* Threshold tables `i_alpha_table`, `i_beta_table`, `i_tc0_table` (in `deblock.c`) indexed by
  `QP + 24` with per-plane offsets `a`/`b`.

## 8. Performance notes

Deblocking is a fixed per-frame cost and often one of the top three consumers at high
resolutions, especially with low QP where most edges pass the threshold.

**Cache/branch behaviour.** Each inner filter tests three thresholds; SIMD converts them
into masks so the whole vector is processed regardless of outcome. The common "skip entire
segment" case (`tc0 < 0`) is handled before entering the pixel loop by the caller. Horizontal
filters pay for a transpose, but this amortizes over four changed rows.

**Concrete SIMD speed opportunities (referenced):**
1. *Wider vectors.* The strength kernel already scales from XMM to YMM to ZMM, using
   `vpermq`/`vpermd`/`vbroadcasti64x2` to keep strided cache data aligned. A clean
   implementation should similarly structure the pixel kernel so a single instruction
   filters 16 (SSE2/AVX2) or 32 (AVX512) samples; the 10-bit vertical filter's two
   iterations per macroblock row are the natural place to widen.
2. *Prefix/scan patterns.* The strided neighbor gather in the strength kernel is effectively
   a small fixed permutation: replace multiple `palignr`/`psllq` steps with one `pshufb`
   (SSSE3) or `vpermd`/`vpshufb` (AVX2/512), as the reference does at higher ISAs.
3. *Branch removal.* Use `ptest`/`ktest` and masked arithmetic so that no `if` remains in
   the per-sample loop; the AVX512 strength kernel's `vpternlogd` collapsing of three
   comparisons into one logical op is a concrete model.
4. *Transpose reduction.* The horizontal path transposes 8x8 blocks and filters vertically;
   `TRANSPOSE2x4x4W`/`TRANSPOSE6x8_MEM` minimize the shuffle count. Further gains come from
   fusing store with transpose (`TRANSPOSE8x4B_STORE`, `STORE_8x4B`) instead of writing to
   a temporary and reloading.
5. *Load widening.* High-bit-depth kernels load two vectors ahead of time and process both
   with one mask computation (`LOAD_MASK` produces `mask0` for p-side and a second mask for
   q-side), halving the threshold arithmetic per output vector.

## 9. References

* ITU-T H.264 recommendation, clause on the adaptive in-loop deblocking filter (default
  filter, strong intra filter, boundary strength derivation).
* x264 public source: `common/x86/deblock-a.asm`, `common/deblock.c`,
  `common/x86/deblock.h`, `tools/checkasm.c`.
* Intel Intrinsics Guide: `palignr`, `pshufb`, `pavgb`, `pabsb`, `vptestmw`, `vpternlogd`,
  `kmov`, `vpmaxub`.

## 10. Verification checklist

1. For each ISA variant and both bit depths, compare filtered pixel buffers against the C
   implementation on randomized images, with `tc0` entries negative, zero, and positive and
   thresholds chosen to exercise all three decisions.
2. Test edge geometry explicitly: vertical edge at the left/right picture edge (out-of-range
   neighbors are still read from the padded picture buffer — verify padding), horizontal
   edge at top/bottom, MBAFF and 4:2:2 chroma strides.
3. Verify `deblock_strength` exhaustively over small random caches: for every `bs` entry,
   independently recompute the C decision (nnz, both MV lists, both reference lists,
   `bframe=0/1`) and compare.
4. Confirm output ordering of `bs0`/`bs1`: each 16-byte store must match the C layout
   `[dir][edge][i]`; a wrong shuffle here passes all pixel tests but fails CABAC decisions.
5. Confirm no writes outside the six filtered rows/columns by placing canaries around the
   test buffer.
6. Use the project's `checkasm` harness to verify register/stack preservation across calls
   (it clobbers all GPRs with random patterns before invoking each assembly function).

## 11. Open questions

* **UNKNOWN**: exact preset-dependent share of total encode time spent in deblocking; no
  profiling data available in this workspace.
* **UNKNOWN**: whether any AVX512 pixel-filter kernel exists beyond the strength kernel;
  the header lists only `deblock_strength_avx512`.
* The 8-bit `deblock_v_luma_mmx2` wrapper in the header is documented as having significant
  CPU cost; whether it is still reachable in optimized builds is **UNKNOWN**.
