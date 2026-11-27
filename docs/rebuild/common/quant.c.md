# `common/quant.c` — quantization, dequantization and coefficient analysis

**Role:** implements scalar quantization of transform coefficients, their inverse
(dequantization), chroma DC optimization, psycho-visual denoising, block decimation scoring,
and the `coeff_last`/`coeff_level_run` helpers used by the entropy coder.
**Status:** dirty-side behavioural specification.

---

## 1. Header

- Source path: `common/quant.c` (header `common/quant.h`).
- One-sentence role: convert integer DCT coefficients into quantized levels and back with
  x264's deadzone/trellis-friendly rounding, and compute the coefficient statistics needed
  for rate control and CAVLC/CABAC coding.
- Marker: dirty-side behavioural specification.

---

## 2. Purpose

Quantization is where rate–distortion is decided. x264 uses a multiply-shift quantizer whose
rounding offset encodes the "deadzone" (how large a coefficient must be before it survives),
a separate dequantizer that reconstructs coefficients with a deterministic shift-and-round,
and a set of cheap analysis primitives (`coeff_last`, `coeff_level_run`, `decimate_score`) that
let the entropy coder and analysis stage find the last nonzero coefficient and decide whether
a block can be discarded entirely. Trellis (RD-optimal) quantization is dispatched through
the same table but lives in assembly / in the C rate-distortion code rather than this file;
this document describes its observable behaviour and interface.

---

## 3. Public interface

All functions are reached through `x264_quant_function_t` (see `quant.h.md`); only
`x264_quant_init(x264_t *h, uint32_t cpu, x264_quant_function_t *pf)` is exported.
Functional families:

- Forward quantizers:
  - `quant_8x8(dct[64], mf[64], bias[64])` → returns a nonzero flag;
  - `quant_4x4(dct[16], mf[16], bias[16])` → nonzero flag;
  - `quant_4x4x4(dct[4][16], mf[16], bias[16])` → a 4-bit mask, one bit per 4x4 block;
  - `quant_4x4_dc(dct[16], mf, bias)` and `quant_2x2_dc(dct[4], mf, bias)` → nonzero flag,
    for DC-only luma/chroma blocks.
- Dequantizers: `dequant_4x4`, `dequant_8x8`, `dequant_4x4_dc`, each taking
  `dequant_mf[6][64|16]` and `i_qp`, in-place on the coefficient array.
- Combined inverse: `idct_dequant_2x4_dc` and `idct_dequant_2x4_dconly` for 4:2:2 chroma DC.
- `optimize_chroma_2x2_dc` / `optimize_chroma_2x4_dc`: choose whether a chroma DC block can
  be represented more cheaply, returning a nonzero flag.
- `denoise_dct(dct, sum, offset, size)`: subtract a per-coefficient threshold and return the
  absolute coefficient to a running sum (psy-RD / noise reduction).
- Decimation: `decimate_score15/16/64` return a cost estimating how safe it is to zero a
  block.
- Coefficient statistics: `coeff_last4/8`, `coeff_last[13]`, `coeff_level_run4/8`,
  `coeff_level_run[13]`; the indexed versions are selected by CABAC block category.
- Trellis: `trellis_cabac_4x4`, `trellis_cabac_8x8`, the `_psy` variants,
  `trellis_cabac_dc`, `trellis_cabac_chroma_422_dc`.

---

## 4. State and data structures

- `dctcoef` (signed) and `udctcoef` (unsigned) coefficient types; the quantizer treats the
  magnitude as unsigned and re-applies the sign afterwards, avoiding signed-overflow issues.
- `x264_run_level_t` (in `bitstream.h`) holds `last`, `mask` and an aligned `level[18]`
  array; `coeff_level_run*` fills it with the positions/levels of nonzero coefficients.
- Scaling-matrix derived tables, prepared once per encoder in `common/set.c`, are passed in:
  - `quant_mf` is a 16-bit multiplier derived from the H.264 default quant step `qstep` and
    the custom scaling list, shifted by `qp/6 - 1` for 4x4 blocks (`qp/6` for 8x8).
  - `bias` is a rounding offset: `min(deadzone<<10 / mf, (1<<15)/mf)`. When `deadzone=0`
    the bias is exactly the half-unit `(1<<15)/mf`, giving round-to-nearest with a minimal
    deadzone; a smaller bias creates a wider deadzone around zero.
  - `dequant_mf[6][N]` are the reconstruction multipliers for `qp%6`, applied with a shift
    derived from `qp/6`.
  - `unquant_mf` (trellis) is a reciprocal multiplier with 8 fractional bits.
- No persistent state inside this file; every function is pure apart from in-place coefficient
  updates and the caller's `sum`/`offset` arrays.

---

## 5. Algorithm / behaviour

### 5.1 Scalar quantization (`QUANT_ONE` rule)

For each coefficient `c`, magnitude `m = |c|`, multiplier `f = mf[i]`, offset `b = bias[i]`:

```
q = ((b + m) * f) >> 16       (32-bit unsigned intermediate arithmetic)
c' = (c >= 0) ? +q : -q
nz |= c'
```

The function returns `!!nz` (or a 4-bit OR for `quant_4x4x4`, with bit `j` set iff the
j-th 4x4 block had any nonzero coefficient).

Interpretation: the multiplier already contains the QP step (shifting right by 16), and the
bias places the decision threshold. With `b = (1<<15)/mf`, the expression is the standard
"add half a unit then truncate": a coefficient survives when its magnitude exceeds
`qstep/2` minus a tiny amount — this is x264's minimal-deadzone behaviour. Reducing `b`
towards zero widens the deadzone: small coefficients quantize to zero, which is used by some
tuning modes and required for CAVLC-friendly sparse blocks. The multiplication is done in
unsigned arithmetic so the sign can be reattached without overflow at large QP.

### 5.2 Dequantization

Let `qp6 = qp % 6`, `qbits4 = qp/6 - 4`, `qbits8 = qp/6 - 6`.

- For 4x4: `dct[i] = dct[i] * dequant_mf[qp6][i]`, shifted **left** by `qbits4` if
  `qbits4 >= 0`; otherwise compute `f = 1 << (-qbits4 - 1)` and use
  `(dct[i]*mf + f) >> (-qbits4)` (round-to-nearest). The 8x8 case is identical with
  `qbits8` and 64 coefficients.
- For DC-only (`dequant_4x4_dc`) the multiplier uses only `dequant_mf[qp6][0]`, pre-shifted
  by `qp/6` when that shift is non-negative; otherwise the same rounding shift applies.
- `idct_dequant_2x4_dc`: before dequantization it runs a 4-point Hadamard over the eight 4:2:2
  chroma DC coefficients (sum/difference adjacent pairs, then combine), multiplies by
  `dequant_mf[qp6][0] << (qp/6)`, rounds with `+32` and shifts right by 6, and writes the
  eight results into the eight 4x4 blocks' DC positions. The `_dconly` variant writes the
  same eight values back into the compact DC array.

### 5.3 Chroma DC optimization

`optimize_chroma_2x2_dc` / `_2x4_dc` reconstruct the DC-only chroma samples for a candidate
quantized DC set (2x2 or 2x4 Hadamard then dequantization with an offset of 32), and check
whether changing the coefficients changes the reconstructed samples at all. The internal
routine first tests whether the dequantized result rounds to zero in the 6-bit domain; if so
it returns "no nonzero coefficients" immediately. Otherwise it walks coefficients from the
highest-frequency down, repeatedly decreasing each coefficient by its sign, and stops as
soon as the reconstruction differs from the zero reconstruction. This chooses the sparsest
DC representation that preserves the reconstructed chroma values.

### 5.4 Denoising

`denoise_dct` accumulates `sum[i] += |dct[i]|`, then subtracts `offset[i]` from the magnitude
and stores `sign × max(0, magnitude - offset)`. This is a soft-threshold operation driven by
psy-RD / `--psy` settings.

### 5.5 Decimation scoring (JVT-B118)

`decimate_score_internal` scans a coefficient array from its last index downwards:

- Skip trailing zeros to find the last nonzero coefficient.
- Any coefficient whose value is not `-1`, `0` or `+1` immediately returns the maximum penalty
  (9), because such a coefficient cannot be safely dropped.
- Otherwise count the run of zeros before each surviving ±1 and add a table lookup value:
  for 4x4 (15 coefficients) `x264_decimate_table4[run] = {3,2,2,1,1,1,0,...}`;
  for 8x8 (64 coefficients) `x264_decimate_table8[run]` starts `3,3,3,3,2,2,...` and reaches
  zero after run 24. A low total score means the block is safe to zero.
- `decimate_score15` skips index 0 (the DC is handled separately), so it operates on the
  array starting at offset 1; `decimate_score16` and `decimate_score64` use the whole array.

### 5.6 `coeff_last` and `coeff_level_run`

`coeff_lastN` returns the highest index `i` with `dct[i] != 0`, or `-1` for an all-zero array.
`coeff_level_runN` calls `coeff_lastN`, records it in `runlevel->last`, then walks downward
collecting nonzero coefficient values into `runlevel->level` in order, building a bitmask of
their positions in `runlevel->mask`, and returns the number of nonzero coefficients.
Variants exist for 4, 8, 15 (4x4 AC), 16 (4x4 full) and 64 coefficients. The indexed
`coeff_last`/`coeff_level_run` arrays map CABAC block categories to the appropriate variant:
luma/chroma DC and 4x4 categories share the 16-coefficient routine, AC categories the
15-coefficient routine, and 8x8 categories the 64-coefficient routine.

### 5.7 Trellis (RD-optimal) quantization — observable behaviour

The trellis functions implement Viterbi-style optimal quantization: instead of choosing each
level independently with the deadzone rule, they search a small set of candidate levels per
coefficient (typically the nearest quantized value and the one below it) and pick the
sequence minimizing

```
J = Σ SSD(candidate reconstruction, original coefficient) × weight
  + λ² × (estimated CABAC bit cost of the coefficient sequence)
```

The bit cost is accumulated from CABAC state probabilities: `sig` and `last` flags,
the `coeff_abs_level_minus1` prefix/suffix (unary prefix with Exp-Golomb suffix for levels
≥15), and context-state transitions propagated between trellis nodes. `lambda2` is the
Lagrange multiplier squared and scaled by the CABAC size precision. `level_state0` and
`level_state1` pack the eight/nine relevant CABAC context states into 64-bit/16-bit words
for fast copying. The `_psy` variants add a term favouring higher reconstructed AC energy to
preserve texture (psycho-visual trellis), using the original (unquantized) DCT block passed
in as `fenc_dct`. The `dc` and `chroma_422_dc` variants apply the same search to the small
DC coefficient arrays. A C reference implementation of the same algorithm exists in the
rate-distortion module (`encoder/rdo.c`); its assembly counterpart is wired into this table
only for x86-64.

The exact DP state topology and per-context transition tables are fixed by the H.264 CABAC
specification and x264's entropy tables; the observable contract is that trellis returns a
nonzero flag and writes the chosen levels into `dct`, matching the C reference bit-exactly.

### 5.8 Initialization / dispatch

`x264_quant_init` installs every scalar function, then upgrades entries by CPU capability:
MMX/MMX2 (coefficient scanning, denoise), SSE2 (quant/dequant/decimate), LZCNT (leading-zero
scanning), SSSE3 (quantizers, decimate, trellis, level-run), SSE4, AVX, XOP (dequant), AVX2,
AVX512, NEON (ARM), Altivec, MSA, LSX/LASX. Special cases:
- Flat quantization matrices allow "flat16" fast dequantizers.
- Trellis is only wired on `ARCH_X86_64` with MMX support, via the `INIT_TRELLIS` macro.
- The indexed `coeff_last`/`coeff_level_run` entries are aliased to the three core variants
  at the end of initialization.

---

## 6. Dependencies

- **Calls:** `x264_clip_pixel`/clip helpers, `x264_cabac_*` entropy tables and state
  transitions (trellis), `memcpy`, SIMD kernels from architecture headers.
- **Called by:** `encoder/macroblock.c`, `encoder/rdo.c` (which selects deadzone, trellis,
  psy-trellis or quantizer-QNS), `encoder/cabac.c`/`cavlc.c` for coefficient statistics.
- **Shared config:** `h->param.i_cqm_preset` selects flat fast paths; deadzone/psy settings
  are prepared in `common/set.c` and passed through `quant_bias` / `psy_trellis`.

---

## 7. Constants and tables

- Shift constants: forward `>>16`; dequant 4x4 `qp/6-4`, 8x8 `qp/6-6`, DC `qp/6-6`; rounding
  half-unit `1 << (-qbits-1)`; chroma reconstruction offset 32 (or 2080 = 32 + 32<<6 for the
  2x4 case).
- Rounding offsets: `(1<<15)/mf` (round-to-nearest bias) and `deadzone<<10/mf` (deadzone bias).
- Decimation tables: `x264_decimate_table4[16]`, `x264_decimate_table8[64]` (values listed
  above).
- Trellis constants: CABAC size precision, λ² scaling, `levelgt1_ctx` 8/9 for chroma DC vs
  luma, and the unary/Exp-Golomb level prefix cut at 14.

---

## 8. Performance notes

Quantization runs for every residual coefficient of every macroblock; the analysis stage may
run it several times (deadzone, trellis, psy-trellis), making it one of the top hot spots at
medium/slow presets.

Optimization guidance:

- The per-coefficient quantizer is a multiply–add–shift plus sign fixup: ideal for SIMD with
  packed 16×16→32 multiplies, a single bias vector, and a sign-mask XOR to reattach signs.
  Keep `mf`/`bias` contiguous so they can be loaded into a vector register.
- `quant_4x4x4` should process four blocks in parallel (four vector registers) — this is why
  it exists as a separate entry point.
- Dequantization's branch on `qbits >= 0` should be hoisted out of the coefficient loop; use
  a shift instruction with a variable count where available.
- `coeff_last`/`level_run` benefit enormously from `lzcnt`/`bsf`/`clz` instructions on a
  bitmask of nonzero flags; the SIMD versions build that mask with a movemask-style operation
  and scan it in a few instructions.
- Decimation scoring is branchy but small; the table lookups should stay in L1.
- Trellis is the most expensive path; keep the CABAC state words in registers and avoid
  recomputing prefix costs — the assembly versions do exactly this.
- Data layout: coefficients should be 16-byte aligned; `x264_run_level_t.level` is explicitly
  aligned to 16 bytes for the SIMD level-run routines.
- Literature: the deadzone quantizer is analysed in JVT-B118; RD-optimal trellis quantization
  for H.264 is described in ORTEGA/RAMCHANDRAN work and in the x264 developer documentation;
  the CABAC cost model follows ITU-T H.264 clause 9.3.

---

## 9. References

- JVT-B118, "Quantization optimization for H.264" (decimation scoring).
- ITU-T H.264 clauses 8.5/8.6 (quantization/dequantization) and 9.3 (CABAC).
- x264 public source: `common/quant.c`, `common/set.c`, `encoder/rdo.c`, SIMD quant kernels.
- General literature on trellis-coded quantization for video coding (e.g. "Rate-distortion
  optimized quantization" by Ortega and Ramchandran).

---

## 10. Verification checklist

1. **Quantizer formula:** for known `mf`, `bias`, and coefficient sequences, verify the exact
   level output, including negative coefficients and zero results; compare `quant_4x4x4`'s
   mask to four independent `quant_4x4` calls.
2. **Deadzone:** sweep coefficient magnitudes across the threshold and verify the survival
   boundary changes with `bias` as expected.
3. **Dequant round trip:** quantize then dequantize a random coefficient block and compare
   against an independently coded reference implementation bit-exactly for several QPs,
   including `qp%6` extremes and the shift sign boundary at `qp/6 == 4` (4x4) / `6` (8x8).
4. **DC paths:** validate `quant_2x2_dc`, `quant_4x4_dc`, `dequant_4x4_dc` and the 2x4
   combined inverse against the general path restricted to DC.
5. **Decimation:** feed arrays with values outside {−1,0,1} (must return 9), and with known
   zero-run patterns (must return the sum of the published table entries).
6. **Coefficient statistics:** `coeff_last` must return −1 for all-zero and the correct last
   index otherwise; `coeff_level_run` must produce identical `last`, `mask`, `level[]` and
   count across scalar and SIMD variants.
7. **Trellis equivalence:** compare the chosen levels against the C reference in `rdo.c`
   for fixed λ², CABAC states and candidate coefficients; verify psy variants change the
   choice only through the reconstructed-energy bias.
8. **Dispatch equivalence:** for every CPU feature level, all table entries must produce
   byte-identical outputs to the scalar reference (checkasm methodology).

---

## 11. Open questions

- `UNKNOWN`: the exact assembly-level trellis state machine is not specified here; only its
  observable output and interface are. Implementers may use the C reference in `rdo.c`.
- `UNKNOWN`: which deadzone values are enabled for a given `--tune` is a parameter choice
  outside this file; only the bias formula is fixed here.
- `UNKNOWN`: the exact list of CPU bits that enable each SIMD variant on non-x86 platforms
  is delegated to the architecture-specific translation units.
