# common/x86/sad-a.asm — behavioural specification

**Role:** 8-bit x86 sum-of-absolute-difference kernels: single-block SAD, multi-candidate SAD x3/x4, vertical SAD, intra-prediction SAD comparisons, and cacheline-split-safe variants across MMX2, SSE2/SSE3/SSSE3, AVX, AVX2 and AVX-512.
**Marker:** dirty-side behavioural specification.

---

## 1. Header

- Source path: `common/x86/sad-a.asm`
- Role: the hot core of motion estimation; every candidate motion vector costs one of these functions.
- Built only for 8-bit (`BIT_DEPTH == 8` / `HIGH_BIT_DEPTH == 0`); the 10-bit counterpart is `sad16-a.asm`.
- The file defines no SATD kernel — SATD lives in `dct-a.asm` (not assigned). "SAD/SATD" in the project overview refers to the family of distortion metrics; this file covers SAD only (UNKNOWN mapping of SATD call sites beyond the `pixel.c` dispatch tables).

---

## 2. Purpose

Motion estimation evaluates thousands of candidates per frame. SAD is cheap, highly regular, and dominates runtime at fast presets. This file provides:

1. Single SAD over all block sizes (4x4 through 16x16) at every ISA level.
2. SAD x3/x4 variants that compare one fenc block against three/four reference blocks simultaneously, reusing the fenc load across candidates.
3. Vertical SAD (`vsad`) for interlaced field detection.
4. Intra-prediction SAD comparisons (H, V, DC predictions) for 4x4/8x8/8x8c/16x16 blocks.
5. Cacheline-split-safe variants that avoid the severe penalty of unaligned loads crossing cache lines on pre-Nehalem CPUs.

---

## 3. Public interface

Signature conventions (all symbols are private-prefixed and carry CPU suffixes):

- `int pixel_sad_WxH[_{variant}]( uint8_t *fenc, intptr_t i_fenc_stride, uint8_t *fdec, intptr_t i_fdec_stride )`
  - Returns the sum of `|fenc[y][x] − fdec[y][x]|` over the W×H block as a 32-bit int. Argument order: `r0`=fenc, `r1`=fenc stride, `r2`=fdec, `r3`=fdec stride. Fenc stride is normally 16 (`FENC_STRIDE`).
- `void pixel_sad_x3_WxH[_{variant}]( uint8_t *fenc, uint8_t *pix0, uint8_t *pix1, uint8_t *pix2, intptr_t i_stride, int scores[3] )`
  - Writes three 32-bit SAD scores to `scores`. `r0..r3` = fenc/pix0/pix1/pix2, `r4` = common reference stride, `r5` = scores pointer (in a register on Unix64, on the stack elsewhere).
- `void pixel_sad_x4_WxH[_{variant}]( uint8_t *fenc, uint8_t *pix0, uint8_t *pix1, uint8_t *pix2, uint8_t *pix3, intptr_t i_stride, int scores[4] )`
  - Same with four candidates; `r4`=pix3, `r5`=stride, `r6`=scores.
- `int pixel_vsad[_{variant}]( uint8_t *src, intptr_t stride, int height )`
  - Sums absolute differences between adjacent rows (vertical neighbouring-pixel differences) over a 16-pixel-wide strip for `height` rows; used by interlaced detection.
- `void intra_sad_x3_4x4_mmx2( uint8_t *fenc, uint8_t *fdec, int res[3] )`,
  `void intra_sad_x3_8x8_mmx2( uint8_t *fenc, uint8_t edge[36], int res[3] )`,
  `void intra_sad_x3_8x8c[_{variant}]( uint8_t *fenc, uint8_t *fdec, int res[3] )`,
  `void intra_sad_x3_16x16[_{variant}]( uint8_t *fenc, uint8_t *fdec, int res[3] )`
  - Compute SAD of fenc against H, V and DC intra predictions; write the three costs to `res[3]` in a fixed order (V, H, DC for 4x4/16x16; V, H, DC for 8x8 — check the specific function).

Instantiated variants summarised in section 5; suffix names follow `x86inc`'s `SUFFIX` rule (e.g. `_mmx2`, `_sse2`, `_sse2_aligned`, `_sse3`, `_ssse3`, `_xop`, `_avx2`, `_avx512`, `_cache32_mmx2`, `_cache64_sse2`, `_cache64_ssse3`).

---

## 4. State and data structures

- All functions are stateless leaves; only loop counters and pointer registers are live.
- Accumulators are word or dword vector registers. `psadbw` produces four/four/four/eight 16-bit lane sums (depending on register width) that are summed at the end with `MOVHL`+`paddw` or `HADD*`.
- `FENC_STRIDE = 16` and `FDEC_STRIDE = 32` constants from `x86util.asm`; high-depth byte-stride conversions do not apply here.
- `r4`/`r5`/`r6` are repurposed as loop counters and stride multiples; `DECLARE_REG_TMP` is used for x3/x4 to alias temporaries (e.g. `t0 = r6` on 64-bit, `r5` on 32-bit).
- Constants referenced: `pb_3`, `pb_shuf8x8c`, `pw_8`, `sw_64`, and local rodata `pb_shuf8x8c2`, `hpred_shuf`.

---

## 5. Algorithm / behaviour

### 5.1 Single-block SAD, MMX2
Three row-group macros feed a common `SAD` template:
- `SAD_INC_2x16P`: loads four 8-byte chunks from two consecutive fenc rows and four from two fdec rows, applies `psadbw` pairwise, and accumulates into `mm0`.
- `SAD_INC_2x8P`: two 8-byte rows per iteration.
- `SAD_INC_2x4P`: builds two 8-byte registers from two 4-pixel rows each (via `punpckldq` of dwords) before `psadbw`.
The template zero-initialises `mm0`, repeats the row macro `height/2` times, moves the single 16-bit result to EAX and returns. Instantiated for all sizes listed in §3 with `_mmx2`.

### 5.2 Single-block SAD, SSE2/SSE3
`SAD_W16` handles width 16 (and 8 via narrower loads) with 16-byte `psadbw`. On x86-64 it precomputes `3*stride` in volatile registers to use short encodings; on x86-32 it processes two rows at a time to avoid callee-saved register pressure. Variants: `_sse2`, `_sse3`, `_sse2_aligned`, `_ssse3`, `_ssse3_aligned`. A dedicated `pixel_sad_8x16_sse2` uses `SAD_INC_4x8P_SSE` (four 8-byte rows packed into two XMM registers via `movhps`) because only 8x16 benefits from that method even on Nehalem. The `aligned` variants replace `movu` with `mova` and are chosen when the reference pointer is known 16-byte aligned.

### 5.3 Single-block SAD, AVX-512
- `SAD_W48_AVX512` handles widths 4 and 8. It builds a mask `k1` with alternating bits (via `kxnorb`+`kaddb`) so a dword/qword broadcast fills only the lanes corresponding to the 4 or 8 real pixels, then performs `psadbw` with masked broadcasts of the next row, accumulating in dword lanes. For width 8 a final `punpckhqdq`+`paddd` folds the two halves. Heights 4/8/16 are supported; 32-bit/x86-32 uses the 2-row loop.
- `SAD_W16_AVX512` handles 16x8 (YMM) and 16x16 (ZMM). It assembles four consecutive fdec rows into one vector using `vinserti128`/`vinserti32x4` and compares against four 32/64-byte contiguous fenc rows (`[r0+0*64]` etc.). A fast path is taken when the fenc stride equals `FENC_STRIDE` (rows contiguous); otherwise it falls back to `pixel_sad_16xH_sse2.skip_prologue`. End reduction folds 512→256→128→64→32 bits with `paddd` and `punpckhqdq`.

### 5.4 SAD x3/x4, MMX2
A macro family compares fenc against three/four candidates using `psadbw`:
- `SAD_X3_START_1x8P` / `SAD_X4_START_1x8P` load one 8-byte fenc row and three/four 8-byte candidate rows, subtract with three/four `psadbw`s, and keep three/four accumulators in `mm0..mm2`/`mm0..mm3`.
- `SAD_X3_1x8P` / `SAD_X4_1x8P` add more rows into those accumulators.
- 2x4P variants build two rows by `punpckldq` (4-pixel blocks).
- `SAD_X3_2x16P`, `_2x8P`, `_2x4P` and x4 equivalents process two rows per iteration and advance pointers by `2*FENC_STRIDE`/`2*stride`.
- End: on Unix64 scores are stored directly from `r5`; elsewhere the stack slot is loaded into `r0` first. Four 32-bit stores (x4) or three (x3).
Instantiated for widths 4/8/16 and heights 4/8/16.

### 5.5 SAD x3/x4, SSE2/SSE3/SSSE3/AVX
- 16-pixel rows use `SAD_X3_START_1x16P_SSE2`, which loads one aligned fenc row into a register and computes three `psadbw`s against memory operands. With AVX, 3-operand forms keep the fenc value in a register and avoid explicit loads of the candidates.
- 8-pixel rows use `SAD_X3_START_2x8P_SSE2`, which builds two 8-byte rows in one XMM register via `movq`+`movhps` so each `psadbw` covers 16 pixels.
- The 4-row macro (`SAD_X3_4x16P_SSE2` etc.) processes four rows at a time, using `3*stride` and a parity trick (`(%1&1)*4`) to advance the fenc pointer by 8 rows only every other group; candidate pointers advance by 4 rows each group.
- SSSE3 adds `SAD_X4_2x8P_SSSE3`, which broadcasts duplicated 8-byte fenc rows with `movddup` so one `psadbw` serves two candidate planes.
- End macros reduce the four 64-bit lane sums to 32-bit scores: SSE2 uses `movhlps`+`paddw`; SSSE3 uses `packssdw`+`phaddd` and writes all four scores with one 16-byte store.
- AVX (`INIT_XMM avx`) uses VEX 3-operand forms of the SSE2 macros with fewer XMM registers declared.

### 5.6 SAD x3/x4, AVX2
- `SAD_X3_2x16P_AVX2` builds 32-byte fenc rows from two 16-byte halves (`movu`+`vinserti128`) and candidate rows similarly; `psadbw` then covers two rows at once.
- `SAD_X4_2x16P_AVX2` broadcasts each 16-byte fenc row across a YMM register with `vbroadcasti128`, forms candidate rows with `vinserti128`, and uses four `psadbw`s per two rows.
- End: `packssdw` collapses lanes, `phaddd` and `vextracti128` fold to a 128-bit vector of up to four 32-bit scores, written with one `mova`.

### 5.7 SAD x3/x4, AVX-512
Three width-specific macro families:
- `SAD_X_W4_AVX512`: uses mask registers `k1..k3` built from a small integer in GPR (`kmovb`, `kaddb`, `kshiftlb`) to gather four rows' worth of pixels out of a ZMM via masked `vpbroadcastd`, then `psadbw` and `vpmovqd` to form scores. Supports x3/x4 at heights 4/8.
- `SAD_X_W8_AVX512`: `movddup`+`vpbroadcastq` with masks to build candidate rows; `psadbw` accumulates in dword lanes, `packusdw`+`vextracti128` finalise. Heights 4/8/16.
- `SAD_X_W16_AVX512`: assembles rows into ZMM with `vinserti128`/`vinserti32x4`, uses `vpermq` to rotate fenc lanes, and `vshufi32x4`/`vpcompressd` with mask `0x1111` to compact the four scores. Heights 8/16.
Register aliases `s1=xmm16/xmm17` on Win64 (xmm6/7 are callee-saved) and `s1=xmm6/xmm7` elsewhere; temporaries `t0`=stride, `t1`=scratch, `t2`=scores.

### 5.8 Vertical SAD (`pixel_vsad`)
Loads two rows, `psadbw`/`psubw`+`ABSW` the difference, then processes each subsequent pair of rows while keeping the previous bottom row as the next top row (a sliding window of two rows). Height is decremented by 2 each loop; the 32-bit MMX variant uses `paddd` because its per-row sum can exceed 16 bits, while the 8-bit SSE2/SSSE3/XOP variants use `paddw` with a final horizontal fold. The maximum documented sum is `31*16*255 = 126480`, which fits in 32 bits but not 16 — hence the careful fold. AVX2 version uses 32-byte rows and `pabsw`/`paddw`.

### 5.9 Intra SAD comparisons
- `intra_sad_x3_4x4_mmx2`: builds the V prediction from the row above the block, the H prediction from the column to the left (loaded as four bytes from each of four rows and packed), and the DC prediction as the average of the top row and left column (computed with `psadbw` vs zero, `psrlw 2`, `pavgw`). Computes SAD of each prediction against the four fenc rows and writes V/H/DC costs to `res[0..2]` (V at +0, H at +4, DC at +8).
- `intra_sad_x3_8x8_mmx2`: uses an `edge[36]` array holding top and left neighbour pixels; computes DC = `(sum(top)+sum(left)+8)>>4`, V = top row repeated, H = left column repeated; iterates eight fenc rows, accumulating SAD against all three predictions with `psadbw`. Uses the `ACCUM` macro to avoid initialising accumulators.
- `intra_sad_x3_8x8c`: similar but the prediction source is the decoded neighbours around the block in `fdec`; SSSE3 path uses `pshufb` with `pb_3` to replicate edge bytes; AVX2 path builds V/H predictions with `vpbroadcastq`, `pinsrb`, `pshufb` (`hpred_shuf`) and `vpermd`, then computes the three costs in parallel.
- `intra_sad_x3_16x16`: computes DC via `psadbw` of the top row plus a scalar sum of the left column (16 byte loads, `>>5` rounding with +16), V from the top row, H from the left column replicated with `SPLATB_LOAD`. Loops 16 fenc rows with `psadbw` against each prediction; AVX2 version uses `vpbroadcastb`/`vbroadcasti128` to hold DC and V in one register pair and `psadbw` in 3-operand form.

### 5.10 Cacheline-split variants
On CPUs with high cacheline-split penalties (flagged `X264_CPU_CACHELINE_32/64`), loads that straddle a cache boundary cost 4–10× more. The file provides dispatchers that test the reference pointer's low bits and, when a split is detected, jump to specialised loops that load two aligned vectors and realign them in registers:
- `SAD16_CACHELINE_LOOP_SSE2`/`_SSSE3`: 16-wide loops with 15 specialisations (one per alignment offset 1..15). SSE2 uses `pslldq`/`psrldq`/`por`; SSSE3 uses `palignr`. The loops are padded to exactly 80/64 bytes so a computed jump (`lea r5,[address table]; add r5, offset; call r5`) reaches the right one.
- `SAD16_CACHELINE_FUNC_MMX2` / `SAD8_CACHELINE_FUNC_MMX2`: 32-bit-only MMX paths using variable `psrlq`/`psllq` with `sw_64` and the alignment offset in MMX registers.
- `SADX3_CACHELINE_FUNC` / `SADX4_CACHELINE_FUNC`: test all candidate pointers with `CHECK_SPLIT`; if any splits, call the single-block cache64 SAD once per candidate and store each result; otherwise tail-jump to the normal x3/x4 variant. Handles both stack and register ABIs with explicit shadow-space management on Win64.
- The documented Core2 cost model: aligned 49 cycles, cacheline split 234, page split 3600, but only ~57 with the realignment path — the reason these variants exist.

### 5.11 Dispatch hierarchy (which ISA each kernel uses)
- 8-bit SAD: MMX2 baseline → SSE2/SSE3/SSSE3 (16 and 8 wide) → AVX2 (16 wide single + x3/x4) → AVX-512 (all widths, x3/x4). Cacheline variants override on `CACHELINE_32/64` CPUs; `aligned` variants chosen when pointer alignment guarantees it.
- x3/x4: MMX2 baseline → SSE2/SSE3/SSSE3 → AVX → AVX2 → AVX-512. XOP adds x3/x4 for AMD Bulldozer.
- Intra SAD: MMX2 baseline, SSE2/SSSE3/AVX/AVX2 for specific block sizes (see §5.9).
- `pixel.c` (`x264_pixel_init`) installs these into `pixf->sad[]`, `pixf->sad_x3[]`, `pixf->sad_x4[]`, `pixf->vsad`, and `pixf->intra_sad_x3_*` in ascending ISA order, with later levels overwriting earlier ones; cacheline flags select the `_cache*` names.

---

## 6. Dependencies

- Includes `x86inc.asm` and `x86util.asm`; uses `ABSW/ABSW2`, `HADDW/HADDUW`, `MOVHL`, `SPLATB_LOAD`, `ACCUM`, `SWAP`, `psadbw`, `DECLARE_REG_TMP`.
- `cextern` of `pb_3`, `pb_shuf8x8c`, `pw_8`, `sw_64` from `const-a.asm`; local rodata `pb_shuf8x8c2`, `hpred_shuf`.
- Called from `common/pixel.c` dispatch and from motion estimation (`common/me.c`) through `x264_t::pixf`.
- `sad16-a.asm` is the high-depth counterpart; `dct-a.asm` provides SATD (not in this file).

---

## 7. Constants and tables

- `pb_shuf8x8c2`: `times 2 db 0,0,0,0,8,8,8,8,-1×8` — expands 8-bit DC predictions into lanes for the AVX2 8x8c path.
- `hpred_shuf`: `db 0,0,2,2,8,8,10,10,1,1,3,3,9,9,11,11` — gathers horizontal-prediction bytes.
- `pw_8`: all-ones word vector scaled by 8 (division by 16 after horizontal add).
- `sw_64`: 64-bit scalar used as the MMX shift base for cacheline realignment.
- `pb_3`: byte 3 repeated, used to broadcast edge pixels with `pshufb`.
- Core2 cycle figures in §5.10 are documented observations, not constants used by code.

---

## 8. Performance notes

SAD is called for every motion-vector candidate; at fast presets it can consume a large fraction of encode time. Key properties:
- `psadbw` is the workhorse; its 16-bit lane sums require a final horizontal fold, which is a bottleneck for small blocks.
- Multi-candidate (`x3`/`x4`) kernels amortise the fenc load and address computation over several reference blocks, roughly halving cost versus three/four separate calls.
- The 4-row macro structure reduces loop overhead and Lets `psadbw` use memory operands on AVX.
- Cacheline-split handling adds a branch but avoids catastrophic penalties on older CPUs; on modern CPUs the flag is not set and the plain kernels run.
- x86-32 paths avoid 3*stride registers because saving/restoring them costs more than the encoding savings.

**AVX2/AVX-512 vectorization opportunities and bottlenecks (with references):**
- AVX2 doubles row coverage per `psadbw` (32 bytes vs 16), but the four 64-bit lane sums still need extraction (`vextracti128`, `phaddd`). AVX-512 doubles again to eight lanes; `SAD_W16_AVX512` uses `vinserti32x4`/`vinserti128` to assemble rows, then `paddd` folds — a clear win when the fenc stride is 16 (contiguous fenc fast path).
- AVX-512 x3/x4 kernels use mask registers to broadcast only the real pixels of 4/8-wide blocks, avoiding wasted lanes; this is the main AVX-512-specific technique (Intel Intrinsics Guide, masked broadcasts).
- Bottleneck: `psadbw` throughput is 1/cycle on many ports; with multiple candidates the limiting factor is often load throughput and register pressure, not ALU. AVX-512's 32 registers relieve pressure for x4 (Intel Optimization Reference Manual; Agner Fog instruction tables).
- Bottleneck: on AVX2, `vinserti128` from memory costs a uop; assembling four fdec rows costs several uops before any `psadbw`. A rebuild could use `vperm2i128` from two loaded registers if data patterns allow.
- Frequency throttling when using wide 512-bit vectors on some Intel CPUs can negate the theoretical 2× gain for short blocks; a rebuild should keep the 256-bit variants dispatchable (Intel Optimization Reference Manual; Travaglini et al. on AVX-512 frequency).

---

## 9. References

- Intel SDM Vol. 2: `psadbw`, `vpbroadcast*`, `vinserti128/32x4`, `vpermq`, `vpcompressd`, `k*` mask instructions.
- Intel Intrinsics Guide: `_mm256_sad_epu8`, `_mm512_sad_epu8`, masked broadcast forms.
- Agner Fog, *Optimizing subroutines in assembly language* (instruction latency/throughput, cacheline split penalties).
- x264 project documentation / source comments on Core2 cacheline-split cost model.
- ITU-T H.264 (AVC) standard: block sizes and motion-compensation semantics.

---

## 10. Verification checklist

Black-box equivalence method: build a test harness that, for every instantiated symbol, generates deterministic pseudo-random fenc/fdec buffers with controlled strides and alignment, calls the SIMD function and a scalar C reference, and compares results.

1. Single SAD: for each WxH and each variant, verify result equals `Σ|fenc−fdec|` over random data, including all-zero and all-255 buffers.
2. Alignment sweep: vary the fdec pointer over alignments 0..31 and confirm identical results (including cacheline-split variants invoked by forcing the `_cache*` symbols directly).
3. Extend fenc/fdec strides beyond the nominal values to verify stride handling (the functions must use `r1`/`r3`, not hardcoded 16/32).
4. SAD x3/x4: verify all three/four scores independently against scalar SADs, including the case where some candidates are identical and where strides differ.
5. `pixel_vsad`: verify against scalar row-difference sum for several heights (including height 2).
6. Intra SAD: construct fenc/fdec with known H/V/DC predictions and verify cost ordering and exact values; check the `res[]` index for V/H/DC matches the C fallback (`pixel.c`).
7. Dispatch: run the same test with CPU flags masked to force each ISA level (MMX2, SSE2, SSSE3, AVX, AVX2, AVX512) and confirm all variants pass; use `checkasm` for automated coverage.

---

## 11. Open questions

- SATD kernels are not present in this file; their exact location (`dct-a.asm`) and behaviour are outside the assigned set — UNKNOWN for this specification.
- Whether the AVX-512 `SAD_W48_AVX512` mask construction produces correct results for width 4 with the final `punpckhqdq` fold is UNKNOWN without executing it; the described sequence is from the source.
- Exact return-index convention of `intra_sad_x3_8x8` (`res[0]=V`, `res[4]=H`, `res[8]=DC`) is inferred from the store order; the upstream consumer's expectation should be cross-checked.
- Behaviour of `pixel_sad_*_aligned` when passed an unaligned pointer is UNKNOWN/undefined by contract; callers must guarantee alignment.
