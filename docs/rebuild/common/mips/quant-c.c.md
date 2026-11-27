# `common/mips/quant-c.c` — MSA quantization, dequantization and last-coefficient scan

**Dirty-side behavioural specification.** Source path: `E:\_x264_dirty_stash\x264-source\common\mips\quant-c.c`. Role: MSA-128 implementations of scalar quantization, dequantization and last-nonzero-index computation for 4x4 and 8x8 coefficient blocks. Compiled only for 8-bit (`HIGH_BIT_DEPTH == 0`) builds.

## 1. Header

The title line above gives the source path and the file's role in one sentence; the adjacent bold marker identifies this document as a **dirty-side behavioural specification** written from the reference source and public literature. No reference source or near-verbatim pseudocode is reproduced.

## 2. Purpose

Quantization is applied to every transformed residual block; dequantization is applied to every block during reconstruction (including skipped blocks). These routines are therefore executed tens of times per macroblock. The MSA versions vectorize the per-coefficient multiply/round/select across 16 or 64 lanes and compute the "any nonzero" predicate with a single horizontal add rather than a scalar loop.

## 3. Public interface

See `mips/quant.h.md` for the nine exported prototypes. Summary of exported behaviour:

- `x264_dequant_4x4_msa`, `x264_dequant_8x8_msa`, `x264_dequant_4x4_dc_msa` — in-place scaling of coefficient arrays; no return value.
- `x264_quant_4x4_msa`, `x264_quant_8x8_msa`, `x264_quant_4x4_dc_msa` — in-place quantization; return a truthy value iff any level is nonzero.
- `x264_quant_4x4x4_msa` — returns a four-bit mask of per-block nonzero flags.
- `x264_coeff_last64_msa`, `x264_coeff_last16_msa` — return the index of the last potentially nonzero coefficient in the block scan order.

All functions are re-entrant; `mf`/`bias`/`dequant_mf` are read-only caller tables.

## 4. State and data structures

No globals. All working data lives in MSA vector registers.

- Dequantization factor tables: `pi_dequant_mf[6][16]` or `[6][64]` of signed 32-bit factors; index `i_mf = qp % 6`, shift `q_bits = qp/6 − 4` (4x4) or `qp/6 − 6` (8x8 and 4x4-DC).
- Quantization tables: `pu_mf[...]` of unsigned 16-bit fixed-point multipliers and `pu_bias[...]` of unsigned 16-bit additive biases, indexed by coefficient position.
- Coefficients are 16-bit signed; intermediate products are computed in 32-bit lanes so no overflow occurs for valid encoder inputs.

## 5. Algorithm / behaviour

### Dequantization (4x4)

The 16 coefficients are loaded as two 8-lane halfword vectors. The four 4-lane word vectors of the selected dequantization-factor row are loaded. Then:

- If `q_bits >= 0`: the factor words are packed down to halfwords, each coefficient is multiplied by its factor in 16-bit lanes, and the result is shifted left by `q_bits` (a vector variable shift). Results are stored back as 16-bit.
- If `q_bits < 0`: coefficients are sign-extended to 32-bit word lanes, multiplied by the word factors, a rounding constant `1 << (−q_bits − 1)` is added to each, and an arithmetic right shift by `−q_bits` is performed. The results are packed back to halfwords and stored.

The two paths are mathematically the same scaling `coef · mf · 2^q_bits`; the split exists because a left shift in 16-bit lanes is cheaper than word-lane expansion, while negative shifts need rounding before truncation toward negative infinity (arithmetic shift). The addition of the rounding constant before right shift implements round-to-nearest for both signs.

### Dequantization (8x8)

Identical structure over 64 coefficients: eight halfword vectors, sixteen factor word vectors. The `q_bits` split is at `qp/6 − 6`. For `q_bits >= 0` a helper packs pairs of factor words into halfword vectors and multiplies in 16-bit lanes, then applies a left shift; for `q_bits < 0` all 64 coefficients are expanded to 32-bit lanes, multiplied, rounded and right-shifted, then packed back. No clipping is applied — the caller's inverse transform limits the range.

### Dequantization 4x4 DC

Uses only `pi_dequant_mf[i_mf][0]`. For non-negative `q_bits` the factor is pre-shifted left by `q_bits` in a scalar register and broadcast; otherwise the word-lane multiply-add-round-shift path is used. Only the DC coefficient and the other coefficients at the same 16-byte-aligned positions are touched (the helper is invoked on an 8-element group, i.e. two 8-lane vectors — the "DC" nomenclature refers to the block type, not limiting to index 0 in this SIMD variant).

### Quantization (generic 4x4)

For each coefficient:

1. Build a sign mask (`clei_s_h`, "less-or-equal-zero") before modifying the value.
2. Sign-extend the coefficient to a 32-bit lane.
3. Compute `abs(dct) + bias` using an "add absolute value" instruction (`add_a_w`) — this adds the magnitude of the first operand to the second, i.e. effectively `|dct| + bias`.
4. Multiply by the multiplier `mf` in 32-bit lanes.
5. Arithmetic shift right by **16**.
6. Pack back to halfwords and, where the sign mask is set, replace the result with its negation via a bit-select (`bmnz_v`).
7. Accumulate the magnitudes of all 16 results; return `!!nonzero`.

The bias/multiplier are Q16 fixed-point so the `>>16` yields an integer level. Quantization levels for typical x264 QPs are small (single/double digits), so the 16-bit store is safe. Note that the sign mask includes zero; negating zero is harmless.

### Quantization 8x8

The same computation is applied twice to the two 32-element halves of the block. Each half uses four halfword vectors of coefficients, bias and multiplier, unpacked into eight word vectors each, processed with the same add-absolute/multiply/shift/select pipeline, then stored. The nonzero flags from both halves are accumulated.

### Quantization 4x4 DC

A single scalar `mf` and `bias` are broadcast to all lanes; otherwise identical to the 4x4 case. Used for DC-only blocks and chroma DC.

### quant_4x4x4

Calls the 4x4 quantizer four times on successive 16-coefficient blocks and accumulates `(!!nonzero) << block_index` with bitwise OR into a four-bit mask.

### Last coefficient (64 entries)

The 64 coefficients are loaded as eight halfword vectors. Each is compared equal to zero; the resulting boolean halfwords are packed to bytes and ANDed with a repeating bit mask `{1,2,4,8,16,32,64,128}` so that each coefficient contributes one bit of a per-group byte. Pairwise horizontal byte adds reduce these to eight 16-bit sums, then further adds to a single 64-bit word whose set bits indicate which coefficients are nonzero. A "natural leading zeros" (`nloc_d`) instruction returns the number of leading zero bits of the inverted mask; the result is `63 − leading_zeros`. Because the mask is built from sign-bit-style indicators, the function really returns the index of the highest **nonzero group/position** in quantizer scan order.

### Last coefficient (16 entries)

Same technique over two vectors, reduced to a 16-bit word and `nloc_h`; result is `15 − leading_zeros`.

## 6. Dependencies

- **Calls:** `mips/macros.h` (`LD_SH2/4/8`, `LD_SW2`, `ST_SH2/4/8`, `UNPCK_SH_SW`, `PCKEV_H2/4_SH`, `ILVR_H2/4_SW`, `ILVL_H2/4_SW`, `SRA_4V`, `SLLI_4V`, `HADD_SW_S32`, `HADD_UB*`, `SUB4`, `CLIP*`, `XORI`, etc.) and MSA intrinsics (`add_a_w`, `bmnz_v`, `clei_s_h`, `ceqi_h`, `nloc_h/d`, `srari_w`, `fill_h/w`, `copy_u_w`, `insve_*`).
- **Called by:** `x264_quant_init()` in `common/quant.c`, which installs these pointers when `X264_CPU_MSA` is set.
- **Shared globals:** none; dequantization factor tables are in `common/tables.c` and passed in.

## 7. Constants and tables

- `q_bits` offsets: **4** (4x4 dequant), **6** (8x8 and 4x4-DC dequant).
- Rounding constant `1 << (−q_bits − 1)` added before right shift.
- Quantization right shift **16** (the Q16 fixed-point scale of `mf`/`bias`).
- Sign/zero masks and the bit mask byte pattern `{1,2,4,8,16,32,64,128}` for last-coefficient bit packing.
- No tables are defined in this file.

**Why:** the `qp/6` factor follows H.264's quantizer step doubling every 6 QP steps; the Q16 scale lets the same multiplier table serve all QPs with integer arithmetic.

## 8. Performance notes

- **Hotness:** extremely high — one quantization call per coded block, one dequantization call per reconstructed block.
- **SIMD status:** fully MSA-128; 4 lanes of 32-bit or 8 lanes of 16-bit per instruction.
- **Branch behaviour:** the only branch is the `q_bits >= 0` split, which is uniform across a frame (depends only on QP) and therefore perfectly predicted.
- **Optimization tips:** (a) hoist the factor-table row pointer and shift count out of block loops at the caller; (b) for `quant_4x4x4`, the four calls currently reload mf/bias each time — a fused version could keep them in registers; (c) on MSARev6 the `add_a_w`/`bmnz_v` instructions are single-cycle, making the quant kernel ~10 instructions per 4 coefficients; (d) fuse quantization with the DCT butterfly in a specialized macroblock loop if rewriting the pipeline.
## 9. References

ITU-T H.264 §8.5.1/§8.5.2 (scaling and quantization); x264 `common/quant.c` for the C reference and table derivation; MSA instruction set reference for `add_a_w`, `dpadd`, `nloc`.

## 10. Verification checklist

1. For random coefficient blocks and a range of QPs (0..51), compare all dequantization outputs bit-for-bit with the scalar `x264_dequant_*_c`.
2. Quantization: for random blocks and QPs, verify levels and the nonzero flag match scalar `x264_quant_4x4_c`; verify sign preservation for negative inputs.
3. `quant_4x4x4`: verify the returned nibble mask equals the OR of four individual quantizer return values.
4. Check the rounding boundary: construct `dct` values that make `|dct|+bias` exactly cross a multiple of `2^16` by one and verify round-to-nearest behavior.
5. Last-coefficient: for a single nonzero coefficient at scan index i in a 64- or 16-entry block, verify the returned index for every i; verify the all-zero case against the generic sentinel.
6. Run a full short encode with MSA quant enabled and compare the bitstream against a C-only build: it must be bit-identical.
7. ASan-check with coefficient arrays at 16-byte-aligned addresses and with deliberately unaligned pointers to see whether the implementation relies on alignment.

## 11. Open questions

- UNKNOWN: exact all-zero sentinel returned by `x264_coeff_last*_msa`; the expression `63 − nloc(...)` needs numeric validation against the scalar implementation.
- UNKNOWN: whether `x264_dequant_4x4_dc_msa` is intended to scale only the first element or the whole 16-byte group — the implementation operates on two vectors, so verify with non-DC coefficients present.
- UNKNOWN: behaviour for `qp` values outside 0..51 (negative `qp/6`, factor index wrapping) is not defined by the API.
- UNKNOWN: saturation of the halfword store in `quant_8x8` for extreme multipliers; valid encoder tables keep results small, but robustness beyond that is unverified.
