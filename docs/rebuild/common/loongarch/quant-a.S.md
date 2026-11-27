# `common/loongarch/quant-a.S` — LoongArch LSX/LASX quantization assembly

**Dirty-side behavioural specification.** Source path: `E:\_x264_dirty_stash\x264-source\common\loongarch\quant-a.S`. Role: assembly implementations of quantization, dequantization, last-coefficient index, coefficient decimation scoring, and CAVLC level/run pair extraction, in LSX and LASX variants.

## 1. Header

The title line above gives the source path and the file's role in one sentence; the adjacent bold marker identifies this document as a **dirty-side behavioural specification** written from the reference source and public literature. No reference source or near-verbatim pseudocode is reproduced.

## 2. Purpose

Quantization and coefficient scanning run on every coded block. The LoongArch port uses the ISA's `adda`/`muh`/`signcov` instruction trio (add absolute value, multiply-unsigned-high, sign coverage) to implement x264's quantization in three instructions per coefficient group, plus vectorized search for the last nonzero coefficient and CAVLC run/level generation.

## 3. Public interface

Exports (prefixed by `ASM_REF`), declared in `loongarch/quant.h.md`:

- Quantize: `quant_4x4_lsx`, `quant_4x4x4_lsx/lasx`, `quant_8x8_lsx`, `quant_4x4_dc_lsx`, `quant_2x2_dc_lsx`.
- Dequantize: `dequant_4x4_lsx/lasx`, `dequant_8x8_lsx/lasx`, `dequant_4x4_dc_lsx/lasx`.
- Last coefficient: `coeff_last64/16/15/8/4_lsx`, `coeff_last64/16/15_lasx`.
- Decimate score: `decimate_score15/16/64_lsx`.
- Level/run: `coeff_level_run16/15/8_lsx`, `coeff_level_run16/15_lasx`.

Signatures match the generic quant typedefs: quantizers take `(dctcoef*, udctcoef *mf, udctcoef *bias)` and return a nonzero flag or mask; dequantizers take `(dctcoef*, int dequant_mf[6][...], int qp)`; last/score take `(dctcoef*)`; level/run take `(dctcoef*, x264_run_level_t*)`.

## 4. State and data structures

No globals. One constant table:

- `last64_shuf` (`.int 0,4,1,5,2,6,3,7`) — word-index permutation used to gather coefficient lanes for the 64-element last-coefficient scan.

Working vectors are `vr*`/`xr*`. The `x264_run_level_t` output structure is caller-owned; the assembly writes its fields at the offsets defined by that header.

## 5. Algorithm / behaviour

### Quantization core

The macro `QUANT_ONE_LASX` (and its LSX counterpart) captures the essential operation:

1. Load 16 coefficients into a vector (`xvld`/`vld`).
2. `adda.h` — add the absolute value of each coefficient to the corresponding bias lane, producing `|dct| + bias` in halfword lanes.
3. `muh.hu` — multiply by the unsigned multiplier `mf`, keeping the high half of the Q16 product (equivalent to `>>16`).
4. `signcov.h` — restore the sign of the original coefficient onto the magnitude result.
5. Store the quantized levels back.

For `quant_4x4x4`, four blocks are processed in succession; the per-block "any nonzero" flags are accumulated by narrowing (`vssrlni.h.w`), comparing equal to zero, masking less-than-zero, extracting to GPRs, combining with `alsl`/`and`, and finally returning `mask ^ 0xf` — a four-bit mask whose set bits indicate blocks with nonzero coefficients.

`quant_2x2_dc` operates on four coefficients packed into a single vector; `quant_4x4_dc` uses one scalar `mf`/`bias` broadcast to all lanes.

### Dequantization

Reads the QP modulo 6 factor row and computes `q_bits = qp/6 − 4` (4x4) or `qp/6 − 6` (8x8/DC). For non-negative `q_bits`, multiplies halfword coefficients by the halfword factors and shifts left by `q_bits`. For negative `q_bits`, widens to word lanes, multiplies, adds `1 << (−q_bits−1)`, and arithmetic-shifts right by `−q_bits`, then packs back to halfwords. LASX variants process 32 coefficients per vector sequence; LSX processes 16.

### Last coefficient

Each coefficient vector is compared to zero; boolean halfwords are packed to bytes and combined with the bit mask `{1,2,4,8,16,32,64,128}` so each coefficient contributes one bit of a group word. Horizontal byte/halfword adds reduce to a single word/doubleword; a leading-zero count (`nloc`) yields the index of the highest nonzero coefficient. Length-specific variants (64, 16, 15, 8, 4) use different shuffle patterns to handle the exact block length.

### Decimate score

Computes a measure of whether trailing coefficients can be replaced by zeros without changing CAVLC run structure — typically a weighted sum of the magnitudes of the last few coefficients, compared against a threshold. LSX variants process 15/16 or 64 coefficients with vector dot products and horizontal reductions.

### Level/run

`coeff_level_run*` walks the coefficient block in reverse scan order, extracting `(run, level)` pairs into `x264_run_level_t` until the last nonzero coefficient is found. Assembly uses vectorized leading-zero counting and masking to compute runs in groups; LASX handles 16/15 coefficients per pass, LSX also provides an 8-coefficient variant.

## 6. Dependencies

- **Calls:** `loongson_asm.S` dot-product/clip macros, `loongson_util.S` function macros; raw instructions `xvadda.h`, `xvmuh.hu`, `xvsigncov.h`, `vssrlni.h.w`, `vseqi.w`, `vmskltz.w`, `vpickve2gr.w`, `xvpermi.q`, `nloc`.
- **Called by:** `x264_quant_init()` in `common/quant.c`.
- **Shared config:** `HIGH_BIT_DEPTH == 0`, `BIT_DEPTH`.

## 7. Constants and tables

- Q16 quantization shift 16 (implicit in `muh.hu`).
- Dequantization offsets 4 (4x4) and 6 (8x8/DC); rounding `1 << (−q_bits−1)`.
- `last64_shuf = {0,4,1,5,2,6,3,7}`.
- Bit mask byte pattern `{1,2,4,8,16,32,64,128}` for last-index detection.
- Four-bit block mask return convention for `quant_4x4x4` (`mask ^ 0xf` trick).

## 8. Performance notes

- **Hotness:** extremely high — one quantization per coded block, one dequantization per reconstructed block.
- **SIMD status:** fully LSX/LASX; the `adda`/`muh`/`signcov` trio is the shortest known path for x264 quantization on this ISA.
- **Optimization tips:** (a) hoist `mf`/`bias` loads out of the four-block `quant_4x4x4` loop (currently reloaded per macro in some variants); (b) use LASX for 4x4x4 and dequant to halve loop counts; (c) the last-coefficient and level/run routines benefit from keeping the whole block in one 256-bit register; (d) avoid scalar fallbacks for the decimate/level-run paths if CAVLC is enabled.
## 9. References

x264 `common/quant.c`; ITU-T H.264 §8.5.1; LoongArch ISA manual for `adda`, `muh`, `signcov`, `nloc`.

## 10. Verification checklist

1. Compare all quantization outputs and nonzero flags against the scalar reference for random blocks and QPs 0..51.
2. Verify `quant_4x4x4` returns the correct four-bit mask for every combination of empty/nonempty blocks.
3. Dequantization: bit-exact comparison across QPs, including the negative-`q_bits` rounding path.
4. Last coefficient: for a single nonzero at each index and for all-zero input, compare with scalar sentinel.
5. Decimate score: compare scores with the C implementation for random coefficient distributions.
6. Level/run: verify `x264_run_level_t` contents against the scalar encoder for known coefficient arrays.
7. Verify LSX and LASX variants agree wherever both exist.

## 11. Open questions

- UNKNOWN: exact all-zero sentinel for each `coeff_last*_lsx/lasx` variant; the `nloc`-based computation needs numeric validation.
- UNKNOWN: the precise decimate-score formula (weights/threshold) is not obvious from the macro expansion — derive from `common/quant.c` rather than guessing.
- UNKNOWN: whether `dequant_4x4_dc_*` scales only lane 0 or the whole 16-byte group; verify with nonzero non-DC coefficients present.
- UNKNOWN: field offsets written into `x264_run_level_t` must match the C header exactly — confirm with a structural test.
