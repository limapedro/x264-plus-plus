# `common/mips/quant.h` — MSA quantization declarations

**Dirty-side behavioural specification.** Source path: `E:\_x264_dirty_stash\x264-source\common\mips\quant.h`. Role: declares the MSA quantization, dequantization, and last-coefficient-index helpers installed by the generic quant dispatcher.

## 1. Purpose

Quantization and coefficient scanning are executed for every coded block; combined with DCT they dominate the "front end" of encoding. This header exposes the MSA implementations of those primitives to `common/quant.c`, using the standard `x264_template` name mangling and an 8-bit-only guard.

## 2. Public interface

All names are mangled through `x264_template(...)`.

**Dequantization**
- `void x264_dequant_4x4_msa(int16_t *dct, int32_t dequant_mf[6][16], int qp)` — multiply each of 16 coefficients by the per-position dequantization factor selected by `qp % 6`, then scale by `2^(qp/6 − 4)`.
- `void x264_dequant_8x8_msa(int16_t *dct, int32_t dequant_mf[6][64], int qp)` — same for 64 coefficients with scale `2^(qp/6 − 6)`.
- `void x264_dequant_4x4_dc_msa(int16_t *dct, int32_t dequant_mf[6][16], int qp)` — DC-only variant: uses only factor `[0]`, scale `2^(qp/6 − 6)`.

**Quantization**
- `int32_t x264_quant_4x4_msa(int16_t *dct, uint16_t *mf, uint16_t *bias)` — quantize one 4x4 block in place; return nonzero iff any quantized coefficient is nonzero.
- `int32_t x264_quant_4x4x4_msa(int16_t dct[4][16], uint16_t mf[16], uint16_t bias[16])` — quantize four 4x4 blocks; return a 4-bit mask whose bit `j` is set iff block `j` has any nonzero coefficient.
- `int32_t x264_quant_8x8_msa(int16_t *dct, uint16_t *mf, uint16_t *bias)` — quantize 64 coefficients; return nonzero iff any is nonzero.
- `int32_t x264_quant_4x4_dc_msa(int16_t *dct, int32_t mf, int32_t bias)` — DC-only quantization with scalar multiplier/bias.

**Last coefficient**
- `int32_t x264_coeff_last64_msa(int16_t *coef)` — index of the last nonzero coefficient in a 64-entry block in scan order (i.e. `63 − count_leading_zero_groups`).
- `int32_t x264_coeff_last16_msa(int16_t *coef)` — the 16-entry analogue.

Contracts: quant routines overwrite the input coefficient array with signed quantized levels and preserve sign; `mf`/`bias` arrays are caller-owned lookup tables. The `bias` values are already Q16 fractional offsets such that `(|dct| + bias) * mf >> 16` yields the level (the implementation adds absolute value then restores sign).

## 3. State and data structures

No state. Include guard `X264_MIPS_QUANT_H`. Dequantization factor tables are owned by the caller (generic `common/quant.c`); the MSA code reads them by `qp % 6` and does not cache them. Quantization multiplier/bias arrays are likewise caller-owned and are indexed by coefficient position (0..15 or 0..63).

## 4. Algorithm / behaviour

Declarations only. Key semantic points implemented in `quant-c.c.md`:

- Dequantization is `dct[i] = (dct[i] * mf[qp%6][i]) << (qp/6 − k)` when the shift is non-negative; otherwise `(dct[i]*mf + (1 << (−shift−1))) >> (−shift)`, i.e. round-to-nearest with signed arithmetic shift.
- Quantization is the x264 trellis-free dead-zone approximation `level = sign(dct) · ((abs(dct) + bias[i]) * mf[i] >> 16)` with sign restored by a select. The non-zero flag is the horizontal sum of all quantized magnitudes.
- `quant_4x4x4` accumulates per-block flags with an OR-shift so the return value is a nibble mask.
- Last-coefficient helpers compute a bitmask of zero/nonzero groups, then count leading zeros to find the highest nonzero index.

## 5. Dependencies

- **Included by:** `common/mips/quant-c.c` and `common/quant.c`.
- **Requires:** `x264_template`, `int16_t`, `int32_t`, `uint16_t` from standard/config headers.
- No dependency on `mips/macros.h` at the declaration level.

## 6. Constants and tables

None in the header. The factor-table dimensions `[6][16]` and `[6][64]` reflect the six QP-modulo quantizer matrices defined by H.264; the `>>16` quantizer scale and the `−4`/`−6` dequantization offsets are x264's fixed-point conventions.

## 7. Performance notes

No runtime cost. For rebuild, preserve the exact `int32_t` vs `uint16_t` types: the MSA code relies on `bias` being unsigned 16-bit so the "add absolute value" step cannot overflow the 32-bit lane for valid coefficient ranges.

## 8. Verification checklist

1. Include header alone in 8-bit build; check all nine declarations compile.
2. Verify symbol names after `x264_template` expansion against `quant-c.c`'s object.
3. Check that `quant_4x4x4`'s 4-bit return mask convention matches the generic dispatcher's expectation.
4. Confirm the last-coefficient functions return −1 (or the documented sentinel) for an all-zero block — cross-check with the generic implementation.

## 9. Open questions

- UNKNOWN: exact sentinel for an all-zero input to `x264_coeff_last*_msa` as used by this version (the MSA code returns `63 − leading_zeros`, which yields a specific value when all groups are zero); verify numerically against the scalar `x264_coeff_last64_c`.
- UNKNOWN: whether `dequant_4x4_dc_msa`'s shift (`qp/6 − 6`) differs intentionally from `dequant_4x4_msa` (`qp/6 − 4`); confirm against generic `quant.c` to avoid off-by-two scaling.
