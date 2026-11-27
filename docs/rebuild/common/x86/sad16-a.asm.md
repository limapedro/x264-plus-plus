# common/x86/sad16-a.asm — behavioural specification

**Role:** high-bit-depth (9–14 bit) x86 SAD kernels: single-block SAD, vertical SAD, multi-candidate SAD x3/x4, and intra-prediction SAD comparisons using 16-bit pixel lanes.
**Marker:** dirty-side behavioural specification.

---

## 1. Header

- Source path: `common/x86/sad16-a.asm`
- Role: the 10-bit counterpart of `sad-a.asm`; built when `HIGH_BIT_DEPTH == 1`, with `SIZEOF_PIXEL = 2` and `pixel = word`.
- No SATD kernels here (SATD/SA8D for high depth live in `dct-a.asm` / `dct-64.asm`, not assigned). The SAD/SATD "distortion family" note in the rebuild overview applies to the family, not to this file's contents.

---

## 2. Purpose

With 16-bit pixels, the single-instruction `psadbw` byte trick is unavailable: absolute differences must be formed explicitly with `psubw` + absolute value, accumulated in 16-bit lanes, and reduced with word→dword horizontal adds. This file provides those kernels, plus the multi-candidate and intra variants that share the fenc loads, for MMX2, SSE2, SSSE3, XOP and AVX2 dispatch levels.

---

## 3. Public interface

- `int pixel_sad_WxH[_{variant}]( uint16_t *fenc, intptr_t i_fenc_stride, uint16_t *fdec, intptr_t i_fdec_stride )`
  - Sum of absolute 16-bit differences over the block, returned as `int`. Strides are in *pixels* at the call boundary; `FIX_STRIDES`/`lea [r0+2*r1]` convert to bytes where needed.
  - Widths 4/8/16, heights 4/8/16; supported combinations per variant below.
- `int pixel_vsad[_{variant}]( uint16_t *src, intptr_t stride, int height )` — vertical SAD over 16-pixel rows.
- `void pixel_sad_x3_WxH[_{variant}]( uint16_t *fenc, uint16_t *pix0, uint16_t *pix1, uint16_t *pix2, intptr_t i_stride, int scores[3] )`
- `void pixel_sad_x4_WxH[_{variant}]( uint16_t *fenc, uint16_t *pix0, uint16_t *pix1, uint16_t *pix2, uint16_t *pix3, intptr_t i_stride, int scores[4] )`
  - Note: for x4 the stride sits in `r5` and the scores pointer is the 7th argument (`r6m`/stack), unlike the 8-bit file where x4 uses `r5` for stride and `r6` for scores.
- `void intra_sad_x3_4x4[_{variant}]( uint16_t *fenc, uint16_t *fdec, int res[3] )`
- `void intra_sad_x3_8x8[_{variant}]( uint16_t *fenc, uint16_t edge[36], int res[3] )`
  - `edge` is expressed in pixels; code indexes it with `SIZEOF_PIXEL`.

Variants: `_mmx2`, `_sse2`, `_sse2_aligned`, `_ssse3`, `_ssse3_aligned`, `_xop`, `_avx2`, plus `sad_x3/x4` equivalents.

---

## 4. State and data structures

- Accumulators are word vectors; a 16x16 block of 10-bit pixels has a maximum SAD of `256*1023 = 261888`, which overflows 16 bits, so the reference uses *unsigned* word saturation accumulation (`HADDUW`) for the largest block and ordinary `HADDW` otherwise.
- `FENC_STRIDEB = 2*16`, `FDEC_STRIDEB = 2*32`, `PIXEL_MAX = (1<<BIT_DEPTH)-1`.
- `XMM_REGS` is a macro parameter controlling how many vector registers a kernel declares (e.g. 7/9/11 for SSE2 x3/x4, 8 for AVX2).
- `cextern pw_1`, `pw_4`, `pw_8` from `const-a.asm`.
- Register temporaries: for x3/x4, `STRIDE` is aliased to `r%1` where `%1 = x+1` (x3 → r4, x4 → r5).

---

## 5. Algorithm / behaviour

### 5.1 Single SAD, MMX2
Three increment macros:
- `SAD_INC_1x16P_MMX`: loads 32 bytes (16 words) from one fenc row and one fdec row into four MMX registers, subtracts words pairwise, takes absolute values with `ABSW2`, adds into the word accumulator `m0`, and advances each pointer by `2*stride` (`lea r0,[r0+2*r1]`).
- `SAD_INC_2x8P_MMX`: two rows of 8 pixels, four MMX registers.
- `SAD_INC_2x4P_MMX`: two rows of 4 pixels each (8 bytes per register).
The `SAD_MMX` template zeroes `m0`, calls the increment macro `height/x` times (or twice for height 4), then reduces with `HADDUW` if `W*H == 256` else `HADDW`, and returns EAX. Instances: 16x16, 16x8, 8x16, 8x8, 8x4, 4x8, 4x4; plus SSSE3 re-instantiations of 4x8/4x4 that benefit from `pabsw`.

### 5.2 Single SAD, XMM
`SAD_INC_2ROW` handles one or two 16-byte halves per row depending on `2*width > mmsize`; it loads fdec rows, subtracts the fenc rows from memory (`psubw m1, [r0]`), applies `ABSW2`, and accumulates. `SAD` template zeroes `m0`, loops `height/2` times (twice for height 4), reduces with `HADDW`, and returns `movd eax, xm0`. Instances: SSE2 (16x16,16x8,8x16,8x8,8x4), SSE2 aligned (16x16,16x8,8x16,8x8), SSSE3 (same plus 8x4), SSSE3 aligned (four big ones), AVX2 (16x16,16x8 with 32-byte rows).

### 5.3 Vertical SAD
- XMM variants (`SSE2`, `SSSE3`, `XOP`): load two 16-byte (8-pixel) fenc rows, subtract and `ABSW2`, accumulate; subsequent iterations subtract the newly loaded row from the previous bottom row (sliding window). For `BIT_DEPTH == 9` the maximum sum (`62*511 = 31682`) fits a signed word, so `HADDW` is used; otherwise (`62*1023 = 63426`) `HADDUW` is used because the signed word would overflow — but note `HADDUW` implies the accumulator's bit pattern is treated as unsigned during the widening, which is why the 9-bit case can use signed.
- AVX2 variant: 32-byte rows (16 pixels), `pabsw` instead of the generic `ABSW`, sliding the bottom row in `m1`, same depth-dependent reduction; returns `movd eax, xm0`.

### 5.4 SAD x3/x4
`SAD_X3_ONE_START`/`SAD_X4_ONE_START` load one fenc row (`mova`, aligned) and three/four candidate rows (`movu`), subtract the fenc from each, and take absolute values (`ABSW2`/`ABSW`). `SAD_X3_ONE`/`SAD_X4_ONE` process further rows with offsets expressed in bytes (`x*mmsize`) and row-stride multiples. `SAD_X3_INC_P`/`SAD_X4_INC_P` advance pointers by four rows. `SAD_X_2xNP` emits `N` pairs of row macros. The `SAD_X` template handles the first two rows specially (`SAD_X%1_ONE_START` plus one `ONE` at row 2), then loops.

End macros reduce each accumulator with `HADDW` (or `HADDUW` for 8-bit-register 16x16) and store scores: Unix64 writes directly to `r5`; elsewhere loads `r5mp` into `r0` first; x4 similarly uses `r6mp`.

The x4 `SAD_X4_ONE` has three register-budget branches:
- `num_mmregs > 8` (x86-64 XMM): use registers m4..m10 freely.
- `num_mmregs == 8 && ssse3`: reuse `m4`, use `pabsw` directly, reload the fenc row when needed.
- `num_mmregs == 8 && !ssse3`: process two candidates, then reload and process the other two with `ABSW2`.

Instances: MMX2 and SSSE3 x3/x4 for widths 4/8/16 and heights 4/8/16; SSE2/SSSE3/XOP x3 (7–8 XMM regs) and x4 (9–11 regs) for widths 8/16 and heights 4/8/16; AVX2 x3/x4 for 16x8/16x16.

### 5.5 Intra SAD comparisons
- `intra_sad_x3_4x4`: builds the V prediction from the row above the block (`movddup`/`movq`+`punpcklqdq`), the H prediction from the left column (loaded as four dwords and replicated), and the DC prediction as `(A+B+C+D+4)>>3` using `pshuflw`/`paddw` and `[pw_4]`. Computes SAD of fenc rows against all three and stores V/H/DC costs. With SSSE3 it combines results with `phaddw`+`pmaddwd [pw_1]` into one 16-byte store; otherwise it stores three separate `movd`s. Instantiated for SSE2, SSSE3 and AVX (`INIT_XMM avx`, VEX form of SSSE3).
- `intra_sad_x3_8x8`: uses `edge[36]` (top row at offset 7*SIZEOF_PIXEL, V prediction at 16*SIZEOF_PIXEL). DC is `(sum(top)+sum(left)+8)>>4` computed with `HADDW` and a word splat. Eight iterations of `INTRA_SAD_HVDC_ITER` accumulate SAD for DC/V/H using the `ACCUM` macro to skip the first accumulation. SSSE3 folds with `phaddw`; the AVX2 variant assembles two 8-pixel fenc rows into one YMM with `vinserti128`, uses `vbroadcasti128` for the V prediction and `vpermq` for H, and folds with `phaddw`+`vextracti128`+`pmaddwd`.

### 5.6 Dispatch hierarchy
`pixel.c` selects MMX2 first, then SSE2 (aligned variants, `sad_x3/x4` for two sizes when `SSE2_IS_FAST`), then SSSE3 (all widths, x3/x4), then AVX (x3/x4 only and the 4x4 intra), then AVX2 (16-wide single and x3/x4, 8x8 intra). No AVX-512 SAD for high depth is defined in this file — the 8-bit AVX-512 kernels in `sad-a.asm` are byte-specific.

---

## 6. Dependencies

- Includes `x86inc.asm` and `x86util.asm`; uses `ABSW/ABSW2`, `HADDW/HADDUW`, `SPLATW`, `ACCUM`, `SWAP`.
- `cextern pw_1/pw_4/pw_8` from `const-a.asm`.
- Called via `pixf->sad[]`, `sad_x3[]`, `sad_x4[]`, `vsad`, `intra_sad_x3_*` for high-depth builds.
- Counterpart to `sad-a.asm` for 8-bit.

---

## 7. Constants and tables

- `pw_1`, `pw_4`, `pw_8` word broadcast vectors; `pw_4` is the +4 rounding term for 4x4 DC, `pw_8` the +8 term for 8x8 DC.
- `PIXEL_MAX` derived from `BIT_DEPTH`; used in clipping contexts elsewhere in the library, not directly in these SAD kernels.
- The `HADDUW` threshold is driven by block area and bit depth as described in §5.1/§5.3.

---

## 8. Performance notes

- 16-bit SAD is roughly twice the work of byte SAD per pixel; the `psubw`+`ABSW` sequence replaces one `psadbw`, and the reduction is more expensive.
- Processing two rows per loop iteration (`SAD_INC_2ROW`) amortises pointer updates.
- Reusing one fenc row across three/four candidates (`SAD_X3/4`) is even more important here than in 8-bit, because each extra candidate otherwise needs its own `psubw`+`ABSW` chain.
- SSSE3 `pabsw` replaces a 3-instruction emulation and is selected for narrow blocks; XOP offers a different fused form.
- The 9-bit special case in `vsad` avoids unsigned reduction where signed is safe.

**AVX2/AVX-512 vectorization opportunities and bottlenecks (with references):**
- AVX2 kernels use 32-byte rows (16 pixels) so one `psubw`/`pabsw` covers twice as many pixels as SSE2; reduction still folds 32→16 bytes via `vextracti128`/`HADDW`.
- A true AVX-512 high-depth SAD would hold 32 pixels per vector and use `vpmovsxwd`/`vpaddusw`; the reference does **not** provide one — UNKNOWN whether this is an oversight or because high-depth AVX-512 builds were not targeted. A rebuild should consider it (Intel Intrinsics Guide: `_mm512_sad_epu16` does not exist, so emulate with `vpsubw`/`vpsadbw` on unpacked words).
- Bottleneck: word-lane SAD accumulates into 16-bit lanes that overflow quickly; unsigned accumulation (`paddusw`) saturates and loses correctness if a 16x16 block is split into too few lanes. The reference uses `HADDUW` only at the final reduction, after which the scalar sum is formed with zero-extended words — a rebuild must preserve that exact widening order or the result diverges (Agner Fog, instruction tables).
- On AVX2, `ABSW2` uses `pabsw` (SSSE3+); the XOP path can fuse absolute value with sign handling. Modern Intel CPUs lack XOP, so the AVX2 path dominates.

---

## 9. References

- Intel SDM Vol. 2: `psubw`, `pabsw`, `paddusw`, `phaddw`, `pmaddwd`, `vpxor`, `vinserti128`.
- Intel Intrinsics Guide: word absolute value and horizontal-add patterns.
- ITU-T H.264 standard: 4:2:0/4:2:2 chroma and high-bit-depth sample ranges.
- Agner Fog, *Optimizing subroutines in assembly language*.

---

## 10. Verification checklist

1. For each WxH and variant, compare against a scalar 32-bit sum of `|int(fenc)−int(fdec)|` over random 9/10/12-bit data (mask to `PIXEL_MAX`).
2. Specifically test the 16x16 case with alternating min/max pixels to exercise the `HADDUW` path near saturation.
3. Verify `vsad` for `BIT_DEPTH=9` (signed `HADDW`) and `BIT_DEPTH=10` (unsigned `HADDUW`) produce identical results to scalar.
4. SAD x3/x4: verify each of the three/four scores independently and confirm the scores pointer is found correctly on all ABIs (Unix64 register vs Win64/32-bit stack).
5. Intra SAD: verify V/H/DC costs against a C implementation for random edges, including the `+4`/`+8` rounding terms and the SSSE3/AVX2 combined stores.
6. Force each ISA variant via CPU flags and rerun; use `checkasm` coverage for all high-depth variants.

---

## 11. Open questions

- The `SAD_MMX` register-count expression `5-(%2&4/4)` is unusual; UNKNOWN whether it intends to drop a register for 4-row blocks and what the exact parsed value is for each height.
- Whether `INIT_MMX ssse3` variants of 4x8/4x4 SAD are actually dispatched by `pixel.c` for high depth is UNKNOWN (the dispatch list only names `_mmx2` and `_sse2` SETs for those sizes; the SSSE3 names may be used via macro aliases).
- No high-depth AVX-512 SAD exists in this file; confirm with the clean team whether the rebuilt encoder needs one for full feature parity (UNKNOWN requirement).
