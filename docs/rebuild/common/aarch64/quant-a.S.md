# `common/aarch64/quant-a.S` — NEON/AArch64 quantization, dequantization, and coefficient analysis

> Dirty-side behavioural specification (reverse-engineered from the reference encoder; describes behaviour, not code).

## 1. Header

- Source path: `common/aarch64/quant-a.S`
- Role: AArch64 implementations of forward quantization (all block shapes), dequantization, coefficient decimation scoring, last-coefficient index search, run-level encoding, and DCT denoising.
- This is a **dirty-side behavioural specification** artifact for the clean-room reimplementation.

## 2. Purpose

After the forward transform, coefficients are quantized and scanned; the encoder needs fast kernels for the quantize, dequantize, last-nonzero, and level/run extraction steps. These are hot for every residual block. The file has separate 8-bit and 10-bit sections; both are described, with emphasis on 8-bit (the current implementation target).

## 3. Public interface

Exported functions (declared in `quant.h`):
- Quantization: `quant_2x2_dc_neon`, `quant_4x4_dc_neon`, `quant_4x4_neon`, `quant_4x4x4_neon`, `quant_8x8_neon`
- Also `quant_2x2_dc_aarch64` (declared; UNKNOWN whether implemented in this file — no exported symbol with that exact name is visible in the 8-bit section; the `_neon` version is present)
- Dequantization: `dequant_4x4_neon`, `dequant_8x8_neon`, `dequant_4x4_dc_neon`
- Decimation: `decimate_score15_neon`, `decimate_score16_neon`, `decimate_score64_neon`
- Last coefficient: `coeff_last4_aarch64`, `coeff_last8_aarch64` (scalar ARMv8), `coeff_last{15,16,64}_neon`
- Run-level: `coeff_level_run4_aarch64` (scalar), `coeff_level_run{4,8,15,16}_neon`
- Denoise: `denoise_dct_neon`

## 4. State and data structures

- `mask64`: 16 bytes of bit masks (`0x80,0x40,...,0x01` twice) used by `decimate_score64_neon` to select lanes from a 16-byte vector.
- External tables from common code: `x264_decimate_table4`, `x264_decimate_table8` — lookup tables mapping 4-bit/8-bit zero/nonzero patterns to decimation scores.
- `x264_run_level_t`: output structure with `last`, `level[]`, `mask[]` fields; pointer arithmetic in `coeff_level_run_*` assumes `mask` begins at offset 23 bytes from the start of the struct (8-bit case).
- Quantization parameters `mf` (multiply factor), `bias`, `dequant_mf[6][...]`, `i_qp`.

## 5. Algorithm / behaviour

### Quantization (8-bit)

All quantizers operate on signed 16-bit DCT coefficients and produce signed 16-bit quantized levels in place, returning 1 if any level is non-zero:

1. Take the absolute value of each coefficient (`abs`).
2. Add the bias (`bias` for DC-only, per-coefficient `bias[]` for AC) to the magnitude.
3. Multiply by the quantization factor (`mf` scalar or `mf[16/64]` vector) with unsigned widening multiply (`umull`/`umull2` into 32-bit lanes).
4. Shift right by 16 (`shrn`) to get the magnitude of the quantized level.
5. Reapply the sign: compute a sign mask as `coef >> 15` (all ones for negative), XOR the magnitude with the mask and subtract the mask — yielding `−magnitude` for negative inputs and `+magnitude` otherwise.
6. Combine the per-block "any nonzero" flags into a return value: build a mask by ORing the result vectors, narrow to bytes, move to a 64-bit register, and test for nonzero (`QUANT_END`). `quant_4x4x4_neon` ORs flags from four blocks and builds a 4-bit return value by shifting in each block's flag.
7. Store the 16/64 quantized halfwords back to the coefficient array (for `quant_4x4_dc_neon` the 16 halfwords are narrowed to bytes before the nonzero test, then stored via `QUANT_END`).

`quant_2x2_dc_neon` is the scalar-dup variant: loads four halfwords, duplicates `mf` and `bias`, does the same abs/add/multiply/shift/sign-restore sequence on four lanes, returns the nonzero flag.

### Quantization (10-bit)

The 10-bit section uses 32-bit coefficient lanes: biases and factors are 32-bit, multiplication is direct `mul`, and the sign restoration uses the sign bit of 32-bit lanes. The same steps apply; `QUANT_TWO mask` accumulates nonzero flags in a vector mask.

### Dequantization

`DEQUANT_START` computes:
- `i_qbits = (i_qp * 43) >> 8` (the reference uses the constant `0x2b = 43` as a fast divide-by-6 approximation)
- `i_mf = i_qp − 6*i_qbits`
- a pointer into `dequant_mf[i_mf]`, scaled by `2^(bits+2)` for 4×4 or `2^(bits)` for 8×8; for DC-only the single scalar factor is loaded from `dequant_mf[i_mf][0][0]`.
- `shift = i_qbits − offset` where offset is 4 for 4×4 and 6 for 8×8 (the extra scaling of the dequantization matrices).

Two paths:
- **Left shift** (`shift >= 0`): load the 32-bit dequant factors, narrow to halfwords with `sqxtn`, multiply the coefficient halfwords by the factor, shift left by `shift` (`sshl`), and store. For 8×8, loop four times (four groups of 16 coefficients).
- **Right shift** (`shift < 0`): widen the coefficient×factor product to 32-bit lanes (`smull`/`smull2`), apply an arithmetic right shift with rounding (`srshl` with a negative shift amount), narrow back with `sqxtn`, and store.

`dequant_4x4_dc_neon` loads the scalar factor, duplicates it, and applies the same left/right shift paths to 16 coefficients.

### Decimate score

`decimate_score_1x` (sizes 15 and 16):
1. Load coefficients (16 halfwords), saturate to signed bytes (`sqxtn`).
2. Compute a bitmask of zero coefficients (`cmeq` against 0) and a bitmask of coefficients with magnitude > 1 (`abs` then `cmhi` against 1).
3. Narrow the masks to 8-bit lanes, move to 64-bit registers. If any coefficient has magnitude > 1, return 9 immediately (decimation impossible).
4. Invert the zero mask; if all coefficients are zero, return 0.
5. For size 15, drop the highest bit (only 15 coefficients are considered).
6. Reverse the bit order (`rbit`) and scan groups of four bits: `clz` finds the next set bit, the corresponding entry of `x264_decimate_table4` is added to the score, and the bit is consumed. Repeat until no bits remain.

`decimate_score64_neon` does the same for 64 coefficients: builds a 64-bit zero mask using `mask64` to select byte lanes, checks for any magnitude > 1 (return 9), then scans leading 1-bits with `clz` and `x264_decimate_table8`.

### Last coefficient

`coeff_last4_aarch64`: loads two 64-bit words (4 halfwords), uses `clz` on the 64-bit value to find the index of the last nonzero (`w0 = 3 − clz/4`), relying on halfword lanes.

`coeff_last8_aarch64`: checks the upper four halfwords first with `clz`; if all zero (`clz == 64`), checks the lower four and subtracts 4 from the base index.

`coeff_last15_neon`/`16`/`64`: saturate coefficients to bytes, build a nonzero bitmask with `cmtst`, narrow to 8-bit lanes, and use `clz` (and for 64, a vector `clz` plus reduction) to find the last set bit. Size 15 adjusts the input pointer by −2 (skipping the DC coefficient) and subtracts one from the base index.

### Run-level encoding

`coeff_level_run4_aarch64` (scalar): loads four coefficients as one 64-bit word, calls the shared run-level start macro (`mask` pointer = `x1 + 23`, `w7 = 0`, `w8 = 0`, `w9 = 1`, `w4 = 3`), then the run-level loop:
1. `clz` on the remaining coefficient bitmask gives the number of leading zeros; `w4 − (clz >> 2)` gives the index of the last nonzero coefficient; store it as the first output word (the `last` index).
2. For each set bit in the mask (from high to low), store the coefficient level at `x0[index*2]` to the output level array and mirror it to the mask array, increment the count, build the run mask (`w8 |= 1 << index`), and advance to the next set bit using `clz` again.
3. Store the final mask word and return the number of levels.

`coeff_level_run{8,15,16}_neon` first build the nonzero bitmask with NEON (`cmtst`, `fmov` to a 64-bit register), then use the same run-level loop with a shift parameter of `4 − (size+1)/8` (so 8→3, 15/16→2), adjusting the base index for size 15 (DC skipped).

### Denoise DCT

`denoise_dct_neon` processes 16 coefficients per iteration: loads coefficients, dequantization thresholds (`udctcoef`), and a sum accumulator array (`uint32_t*`). It computes the absolute coefficient, saturating-subtracts the threshold (`uqsub`), restores the sign with `bsl`, adds the absolute coefficient to the running sum (`uaddw`), and stores the denoised coefficients and updated sums.

### Quantization equation

For coefficient `c`, multiplication factor `mf`, and bias `b`:

```
level = sign(c) * ( (|c| + b) * mf >> 16 )
```

The assembly computes `|c| + b` in halfword lanes, multiplies by `mf` with `umull`/`umull2` into 32-bit lanes, takes the high 16 bits with `shrn #16`, and re-applies the sign by XOR/subtract with `c >> 15`. The nonzero flag is the OR-reduction of all resulting level lanes; `quant_4x4x4_neon` produces a four-bit mask by testing each block's accumulator separately and shifting the bits together.

Because the multiplication is unsigned, the bias must be applied to the magnitude and the sign restored afterwards — this is why `abs` precedes the add. Quantizer tables are precomputed so `mf` and `bias` are already in the fixed-point domain expected by the kernel.

### Dequantization parameter derivation

Given `qp`:

```
qbits = (qp * 43) >> 8          // fast approximation of qp/6
mf_idx = qp − 6*qbits           // qp mod 6
shift  = qbits − (4 for 4x4, 6 for 8x8)
```

The extra subtraction compensates for the extra scaling baked into the dequantization matrices. For `shift >= 0`, the kernel multiplies halfword coefficients by halfword factors and shifts left by `shift`. For `shift < 0`, it forms 32-bit products with `smull`/`smull2`, applies `srshl` with a negative shift (arithmetic right shift with rounding), and narrows with `sqxtn`. Both paths saturate only when narrowing back to halfwords.

### Decimation score algorithm

The decimation score estimates whether a block's coefficients are too sparse to be worth coding. The implementation follows this decision tree:

1. Build a 16-bit (or 64-bit) pattern of zero lanes and a pattern of lanes with magnitude > 1.
2. If any magnitude > 1 → return 9 immediately (block cannot be decimated).
3. If all lanes are zero → return 0.
4. Otherwise scan the zero/nonzero pattern from the most significant end: each group of four (or eight) bits indexes a score table; `clz` locates the next set bit, the table entry is added, and the bit is cleared by shifting.
5. For size 15, the highest bit is dropped before scanning because the DC coefficient is excluded.

### Last-index and run-level scans

`clz` is used uniformly as a "find next set bit from the top" primitive. For 8-bit coefficients, each halfword is saturated to one byte and the nonzero predicate is compressed into a 64-bit mask with `shrn`. The last index is `N − 1 − (clz(mask) >> 2)` (with a shift appropriate to the lane width). The run-level loop then:
1. stores the last index,
2. for each set bit in descending order, copies the coefficient from `dct[index]` to both `level[]` and `mask[]`,
3. accumulates the emitted-bit mask `w8 |= 1 << index`,
4. advances to the next set bit by masking off leading zeros and recomputing `clz`,
5. returns the count of emitted levels.

### Denoising equation

For each coefficient `c` with threshold `t` and running sum `S`:
```
a = |c|
c' = sign(c) * max(a − t, 0)
S  = S + a
```
The kernel accumulates `S` in 32-bit lanes and writes both `c'` and the updated `S` arrays.

## 6. Dependencies

- Includes `asm.S` only (plus common headers via the C side).
- References external `x264_decimate_table4/8`.
- Dispatched from `common/quant.c` via `quant.h`.

## 7. Constants and tables

- `0x2b = 43`: fast multiply for `i_qp/6`.
- `6`, `4`, `2`: dequantization offsets and shifts.
- `16`: quantization shift and vector width.
- `23`: byte offset of `run_level->mask` used by the run-level macros.
- `mask64`: lane-selection table.
- `x264_decimate_table4`, `x264_decimate_table8`: score lookup tables defined in common code.

## 8. Performance notes

Hot for every residual block. Quantization is fully SIMD with 16/64 lanes in flight; decimate/last/run-level are a mix of SIMD mask building and scalar `clz` scans (efficient on AArch64). The reference keeps the whole quantizer loop in registers and uses `rept` for 8×8.

Optimization tips for a clean-room AVX2 port: use `_mm_abs_epi16`, `_mm_mulhi_epu16`, `_mm_srai_epi16` for sign/quantization; use `_mm_movemask_epi8` plus `lzcnt`/`bsr` for last-nonzero and run-level scans; preserve the decimate table lookups. SVE tip: predicated compress-store can build the level/mask arrays directly from nonzero lanes, but the scalar `clz` scan is already near-optimal. Public references: H.264 quantization/dequantization spec; x264 decimation scoring; ARM `clz`, `sqxtn`, `umull` docs.

## 9. References

- ITU-T H.264 / ISO/IEC 14496-10, Clauses 8.5–8.6 (transform coefficient quantization and dequantization).
- x264 `common/quant.c` (quantizer parameter tables, decimate tables, run-level structure).
- ARM NEON/AArch64 instruction reference (`clz`, `umull`, `sqxtn`, `rbit`, `cmtst`, `uqsub`).
- `docs/rebuild/common/aarch64/quant.h.md`.

## 10. Verification checklist

- Every quantizer matches the C reference for random coefficients and random mf/bias arrays, including sign handling and nonzero return flags.
- Dequantization matches for all i_qp values 0..51 and both shift directions.
- Decimate score matches the C table-driven reference for all-zero, isolated-nonzero, and magnitude>1 cases (return 9).
- `coeff_last*` returns the exact C reference index for all-zero and sparse patterns.
- `coeff_level_run*` returns matching `last`, levels, and mask words.
- `denoise_dct_neon` matches the C reference coefficients and running sums.

## 11. Open questions

- UNKNOWN: whether `quant_2x2_dc_aarch64` exists as a distinct implementation (only `quant_2x2_dc_neon` is visible in the 8-bit assembly).
- UNKNOWN: exact layout of `x264_run_level_t` justifying the `+23` mask offset (comes from common headers).
- UNKNOWN: whether a 10-bit `quant_2x2_dc` path is dispatched (the 10-bit section is not fully enumerated here).
