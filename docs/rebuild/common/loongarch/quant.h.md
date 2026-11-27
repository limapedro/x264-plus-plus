# `common/loongarch/quant.h` — LoongArch quantization declarations

**Dirty-side behavioural specification.** Source path: `E:\_x264_dirty_stash\x264-source\common\loongarch\quant.h`. Role: declares the LSX/LASX quantization, dequantization, coefficient-scan, decimate-score and level/run helpers implemented in `loongarch/quant-a.S`.

## 1. Header

The title line above gives the source path and the file's role in one sentence; the adjacent bold marker identifies this document as a **dirty-side behavioural specification** written from the reference source and public literature. No reference source or near-verbatim pseudocode is reproduced.

## 2. Purpose

Exposes a broader quantizer API than the MIPS port: in addition to the usual quant/dequant/last-coefficient functions, it declares decimation scores and level/run pair extraction (used by the CAVLC/Zigzag coefficient encoders), plus 2x2-DC and 15/8-coefficient variants that appear for chroma and 4x4 blocks in scan order. Every name is mangled with `x264_template`.

## 3. Public interface

**Last coefficient index**
- `x264_coeff_last64/16/15/8/4_lsx` — `int32_t (int16_t *coef)`.
- `x264_coeff_last64/16/15_lasx` — LASX variants.

**Quantization**
- `x264_quant_4x4_lsx(int16_t *dct, uint16_t *mf, uint16_t *bias)` → nonzero flag.
- `x264_quant_4x4x4_lsx/lasx(int16_t dct[4][16], uint16_t mf[16], uint16_t bias[16])` → four-bit mask.
- `x264_quant_8x8_lsx(int16_t *dct, uint16_t *mf, uint16_t *bias)` → nonzero flag.
- `x264_quant_4x4_dc_lsx(dctcoef dct[16], int32_t mf, int32_t bias)` → nonzero flag.
- `x264_quant_2x2_dc_lsx(dctcoef dct[4], int32_t mf, int32_t bias)` → nonzero flag.

**Dequantization**
- `x264_dequant_4x4/8x8/4x4_dc_lsx(dctcoef dct, int dequant_mf[6][...], int i_qp)` and matching `_lasx` variants.

**Decimate score**
- `x264_decimate_score15/16/64_lsx(dctcoef *dct)` → integer score used by the "skip zero coefficients at the end" heuristic.

**Level/run encoding**
- `x264_coeff_level_run16/15/8_lsx(dctcoef *, x264_run_level_t *)` and `x264_coeff_level_run16/15_lasx` — fill a run/level structure for CAVLC.

## 4. State and data structures

No state. Include guard `X264_LOONGARCH_QUANT_H`. Types: `dctcoef` (int16_t in 8-bit builds), `udctcoef` (uint16_t), `x264_run_level_t` from `common/quant.h`, and the `dequant_mf` factor tables owned by the caller.

## 5. Algorithm / behaviour

Declarations only. Behaviour mirrors the MIPS quantizers and the generic x264 C code:

- Quantization: `level = sign(dct) · ((|dct| + bias[i]) · mf[i] >> 16)`, with sign restored after the multiply; the nonzero flag is the OR/horizontal sum of all levels.
- Dequantization: `dct[i] = (dct[i] · mf[qp%6][i]) · 2^(qp/6 − k)` where `k = 4` for 4x4 and `6` for 8x8/4x4-DC, with rounding when the shift is negative.
- Last-coefficient helpers return the index of the highest nonzero coefficient in the scan order for block lengths 64, 16, 15, 8, and 4.
- Decimate score computes a per-coefficient "cost" that decides whether trailing coefficients can be zeroed without disturbing the CAVLC run structure.
- Level/run helpers emit `(run, level, last)` triples into `x264_run_level_t` for entropy coding.

The assembly uses `vadda.h`/`xvadda.h` (add absolute value), `vmuh.hu` (multiply unsigned high half), `vsigncov.h` (sign coverage/restore) — a shorter pipeline than the MIPS implementation.

## 6. Dependencies

- **Included by:** `common/loongarch/quant-a.S` and `common/quant.c`.
- **Requires:** `x264_template`, `dctcoef`, `udctcoef`, `x264_run_level_t`, `int32_t`, `uint16_t` from generic headers.
- The assembly includes `loongson_asm.S`/`loongson_util.S`.

## 7. Constants and tables

None in the header. Implementation constants: Q16 shift 16, dequant offsets 4/6, rounding `1<<(−shift−1)`, and the `last64_shuf` table `{0,4,1,5,2,6,3,7}` documented in `quant-a.S.md`.

## 8. Performance notes

No runtime cost. Rebuild implication: the additional 15/8/4-coefficient variants and decimate/level-run helpers are specific to CAVLC paths; a clean implementation must provide them or disable the corresponding dispatcher slots.

## 9. References

ITU-T H.264 standard; x264 project documentation; architecture SIMD programmer's guides.

## 10. Verification checklist

1. Include header alone under 8-bit and high-bit-depth configurations.
2. Verify each declared symbol exists in `quant-a.S` after mangling.
3. Check that `x264_run_level_t` layout matches the generic definition — the assembly writes fields by offset.
4. Verify the nonzero-flag semantics for `quant_4x4x4` and the individual quantizers.
5. Confirm each `coeff_last*` sentinel matches the generic scalar implementation for an all-zero input.

## 11. Open questions

- UNKNOWN: exact block-length semantics of the 15- and 8-coefficient variants (which coefficient is omitted relative to the 16-entry block) — validate with a known scan pattern.
- UNKNOWN: whether `decimate_score*` and `coeff_level_run*` are used by CABAC contexts in this version or only CAVLC; confirm from `common/quant.c`.
