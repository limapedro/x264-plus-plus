# quant.h — x86 quantization and level-run interface

Source path: `common/x86/quant.h` (include guard `X264_X86_QUANT_H`).
Role: declares every x86-accelerated quantization, dequantization, scan, and trellis entry
point, plus the function-pointer table through which the encoder reaches them.
**Dirty-side behavioural specification** — own words, no source reproduction.

> Note: this header lives in `common/x86/` in the reference tree. The companion trellis
> implementation is in `common/x86/trellis-64.asm`; the `TRELLIS_PARAMS` macro and pointer
> fields are declared in `common/quant.h` and are documented here because they are the
> contract for the x86 kernels.

---

## 1. Header

This header is the public surface of the dirty-side quantization module. Every symbol is
wrapped in `x264_template(name)` so that a build can compile multiple ABI variants under one
binary. It contains no implementation; semantics are carried by `quant-a.asm.md` and by
the trellis discussion in §5.9 below.

## 2. Purpose

The encoder needs a single polymorphic call site for quantization that resolves at startup
to the best available SIMD implementation. This header fixes that interface: a table of
function pointers (`x264_quant_function_t`) filled by `x264_quant_init()` and a flat list of
individually name-mangled declarations used by that initializer and by the test harness.
It also exposes the scan primitives that turn a quantized coefficient array into the
compact level/run representation consumed by CABAC.

## 3. Public interface

### 3.1 Dispatch table

`x264_quant_function_t` contains pointers for:

* quantization: `quant_8x8`, `quant_4x4`, `quant_4x4x4`, `quant_4x4_dc`, `quant_2x2_dc`;
* dequantization: `dequant_8x8`, `dequant_4x4`, `dequant_4x4_dc`;
* fused DC inverse-transform/dequant: `idct_dequant_2x4_dc`, `idct_dequant_2x4_dconly`;
* chroma DC zero optimization: `optimize_chroma_2x2_dc`, `optimize_chroma_2x4_dc`;
* noise reduction: `denoise_dct`;
* block scoring: `decimate_score15`, `decimate_score16`, `decimate_score64`;
* indexed scan tables `coeff_last[14]` and `coeff_level_run[13]`, plus named shortcuts
  `coeff_last4`, `coeff_last8`, `coeff_level_run4`, `coeff_level_run8`;
* trellis: `trellis_cabac_4x4`, `trellis_cabac_8x8`, their `_psy` variants,
  `trellis_cabac_dc`, `trellis_cabac_chroma_422_dc`.

`x264_quant_init( h, cpu, pf )` fills this table according to the detected CPU feature mask.

### 3.2 Function contracts (grouped)

**Quantization (all return an integer nonzero flag).**
`quant_2x2_dc(dct[4], mf, bias)`, `quant_4x4_dc(dct[16], mf, bias)`,
`quant_4x4(dct[16], mf[16], bias[16])`, `quant_8x8(dct[64], mf[64], bias[64])`,
`quant_4x4x4(dct[4][16], mf[16], bias[16])`.
The coefficient array(s) are modified in place. `mf` and `bias` are unsigned
(`udctcoef`) per-coefficient values except in the DC variants, where they are scalar
`int`. `quant_4x4x4` returns a 4-bit mask, one bit per block.

**Dequantization (void, in place).**
`dequant_4x4(dct[16], dequant_mf[6][16], i_qp)`,
`dequant_8x8(dct[64], dequant_mf[6][64], i_qp)`,
`dequant_4x4dc(dct[16], dequant_mf[6][16], i_qp)`.
`i_qp` in 0..51 selects both a multiplier row and a binary shift.

**Fused DC transforms.**
`idct_dequant_2x4_dc(dct[8], dct4x4[8][16], dequant_mf[6][16], i_qp)` writes each result
into the DC slot of one of eight 4x4 blocks;
`idct_dequant_2x4_dconly(dct[8], dequant_mf[6][16], i_qp)` writes back eight dequantized
values in place.

**Chroma DC optimization (8-bit only).**
`optimize_chroma_2x2_dc(dct[4], dequant_mf)` and `optimize_chroma_2x4_dc(dct[8], dequant_mf)`
take a dequantization factor and return whether the block can be treated as zero, possibly
reducing the coefficients in place.

**Denoising.**
`denoise_dct(dct*, sum*, offset*, size)` — in-place coefficients; `sum` is a 32-bit
accumulator array, `offset` an unsigned per-coefficient threshold array with the same
element width as `dctcoef`.

**Scoring / scanning.**
`decimate_score15/16/64(dct*)` return a score capped at 9.
`coeff_lastN(dct*)` returns the zig-zag index of the last nonzero coefficient (or the
all-zero sentinel).
`coeff_level_runN(dct*, x264_run_level_t*)` fills the level list and returns the last
index.

**Trellis.**
`TRELLIS_PARAMS` expands to the common argument list: unquantization multipliers, zig-zag
order table, `lambda2`, last nonzero index, original coefficients, quantized coefficients,
output DCT array, two CABAC context-state arrays, and two words of packed level-context
state. Each trellis entry point appends one or three extra arguments (see §5.9).

## 4. State and data structures

* `dctcoef` / `udctcoef`: signed/unsigned transform coefficient type. 16-bit in 8-bit
  builds, 32-bit in high-bit-depth builds. All signatures are templated over this width.
* `x264_run_level_t` (declared in `common/bitstream.h`):
  * `int32_t last` — zig-zag index of the last nonzero coefficient;
  * `int32_t mask` — bit i set iff coefficient i is nonzero (bit 0 = DC);
  * `dctcoef level[18]` — 16-byte-aligned compacted levels. Entry i holds the nonzero
    coefficient corresponding to the i-th set bit of `mask`; entries beyond the count are
    undefined and must not be consumed.
* `dequant_mf[6][N]`: six rows indexed by `QP mod 6`; row entries are per-position
  multipliers.
* CABAC context state arrays passed to trellis are `uint8_t state` values with the H.264
  state encoding: bit 0 is the大概率/MPS symbol and bits 1..6 encode the probability state.
* Dispatch functions are pure: no hidden per-thread state except that CABAC state arrays
  supplied by the caller are modified by the trellis and residual-coding kernels.

## 5. Algorithm / behaviour

### 5.1 Quantization equation

The mathematical rule (identical in C and SIMD) is a sign-magnitude operation:

`q = sign(d) * ( (|d| + f) * mf >> 16 )`

`f` is a bias that centers the quantization dead zone; `mf` is a per-coefficient
16-bit multiplier derived from QP and the quantization matrix. The SIMD implementation
uses an unsigned high multiply for 8-bit coefficients and a low 32-bit multiply with a
16-bit right shift (or its `pmuludq` emulation) for 10-bit coefficients. In every case the
result replaces the input, and the OR of all results forms the return flag.

### 5.2 Dequantization equation

Let `q = floor(QP/6)`, `r = QP mod 6`, and `s = q - k`, where `k` is 4 for 4x4, 6 for 8x8
and DC-only. Then:

* if `s >= 0`: `d' = d * dequant_mf[r][i] * 2^s`;
* if `s < 0`: `d' = (d * dequant_mf[r][i] + 2^(-s-1)) >> (-s)`.

The rounding constant is exactly half the divisor, with ties rounding toward +infinity
before the arithmetic right shift. Flat-16 variants use a pre-shifted multiplier table and
a plain word multiply when the scale fits in 16 bits.

### 5.3 Scan and decimation semantics

The decimation score turns the coefficient array into a run-length cost: a coefficient of
magnitude greater than one forces the maximum score 9; otherwise each run of zeros between
nonzero coefficients contributes a table lookup, and the score stops accumulating once it
reaches 9. The exact table values are implementation facts from `tables.c` and must be
reproduced bit-identically for equivalence.

`coeff_last` and `coeff_level_run` operate on the *quantized* array in zig-zag storage order;
the DC slot is excluded for the "15" variants. For level/run, `mask` and `last` are derived
from the same zero bitmap, and `level[]` is the compaction of nonzero coefficients into
zig-zag order. The consumer (CABAC residual coder) uses `last` to decide when to emit the
`last_coeff_flag` and uses `mask`/`level` to walk coefficients backwards.

### 5.4 ISA dispatch

Each declaration has an ISA suffix naming the minimum feature set: `mmx2`, `sse2`, `ssse3`,
`sse4`, `avx`, `avx2`, `xop`, `lzcnt`, `avx512`. `x264_quant_init` walks the detected CPU
mask from weakest to strongest, overwriting pointers as stronger variants become available.
A variant may exist only for one bit depth (e.g. `optimize_chroma_2x2_dc` is 8-bit only;
`coeff_last4_avx512` takes `int32_t*` in 8-bit builds where the DC coefficient is stored as
a dword in the shuffle path).

### 5.5 Alignment and scratch

No x86 kernel in this header requires aligned input pointers: word and dword loads use
`mova` where the caller guarantees alignment (DCT buffers are 16-byte aligned in the frame
structure) and `movu`/`movq` otherwise. AVX512 dequant paths use 64-byte-aligned constants.
Stack scratch is internal to each routine; the caller supplies only the arrays named in the
signatures.

### 5.6 8-bit vs 10-bit differences

* Coefficient width changes from 16 to 32 bits, doubling vector iteration counts and
  changing multiply primitives from `pmulhuw` to `pmulld`/`pmaddwd`.
* Chroma-DC optimization is compiled only in 8-bit builds.
* Denoise uses saturating word subtraction in 8-bit builds and masked dword subtraction in
  10-bit builds.
* Decimate/last/level-run pack dwords down to words and bytes before mask extraction in
  10-bit builds; 8-bit builds pack words directly to bytes.
* AVX512 `coeff_last` uses word compares (`vptestmw`) for 8-bit and dword compares
  (`vptestmd`) for 10-bit, with different vector widths for the same coefficient count.

### 5.7 Preset dependence

These are low-level kernels: they are called regardless of preset whenever the corresponding
block type is coded. Their *relative* hotness shifts — RD-heavy presets call the quantizers
many times per block, while fast presets rely more on `decimate_score` and `quant_4x4x4` to
reject zero blocks early.

### 5.8 Verification-oriented invariants

* Quantization is deterministic and bit-exact against the C formula for every coefficient.
* Dequantization is exact for all QP values; the two shift paths must agree at the boundary.
* `decimate_score*` never returns > 9 and returns 9 iff any magnitude > 1.
* `coeff_last*` and `coeff_level_run*` agree on `last` for identical input.

### 5.9 Trellis kernels — full semantics

The six trellis entry points share the `TRELLIS_PARAMS` argument block:

1. `unquant_mf` — per-zig-zag-position inverse quantization factors used to reconstruct a
   candidate coefficient magnitude from an absolute level candidate:
   `unquant = (unquant_mf[pos] * abs_level + 128) >> 8`.
2. `zigzag` — byte table mapping scan index to storage index.
3. `lambda2` — the Lagrangian cost multiplier; rate costs are accumulated in 1/256-bit
   units and folded into the score as `rate_bits * lambda2 >> 4` (8 fractional rate bits,
   4 fractional lambda bits).
4. `last_nnz` — index of the last potentially nonzero coefficient.
5. `coefs` — original (unquantized) coefficients.
6. `quant_coefs` — the ordinary quantized coefficients; a zero here means the coefficient
   is known to be zero and only the `significant_flag=0` path is considered.
7. `dct` — output array receiving the trellis-chosen quantized coefficients.
8. `cabac_state_sig`, `cabac_state_last` — arrays of CABAC context states for the
   significant and last coefficient flags.
9. `level_state0`, `level_state1` — packed initial states for the coefficient-abs-level
   contexts (level==1 and level>1 groups respectively).

Extra arguments: `trellis_cabac_4x4` takes `b_ac` (start index: 0 for DC-only luma 4x4,
1 otherwise); `trellis_cabac_8x8` takes `b_interlaced` (selects the 8x8 context-offset
tables for MBAFF); the `_psy` variants add `fenc_dct` (the prediction/source DCT used for
the psycho-visual reward) and `psy_trellis` (the strength); `trellis_cabac_dc` takes
`i_coefs` (4 or 16 coefficients); `trellis_cabac_chroma_422_dc` has no extra argument and
always handles eight coefficients with the dedicated 4:2:2 context offsets.

**Dynamic program.** Eight nodes are kept, indexed by how many levels greater than one have
already been emitted (context bucket) and by which CABAC level context is active. Node 0 is
the initial state with score 0; all others start at a maximum score. For each coefficient in
reverse zig-zag order the implementation computes two distortion candidates — the
quantized level and (when different) one below it — using the reconstruction formula above.
Distortion is the squared reconstruction error multiplied by a per-position weight
(`dct4_weight2_tab`/`dct8_weight2_tab`), with a special rounding rule for DC coefficients of
DC-only luma blocks: the reconstructed magnitude is rounded to the nearest multiple of 16
with a sign-dependent tie. Rate cost is the CABAC entropy estimate for the significant flag,
last flag, and level prefix/suffix, plus one bit for the sign when the level is nonzero.
The level > 1 path uses a unary prefix cost table indexed by `min(abs_level-1, 14)` and an
Exp-Golomb suffix cost for levels >= 15; the state transition is tracked per node.

**Psy variants.** For non-DC coefficients the distortion term is reduced by
`psy_weight * |unquant_level + sign(predicted_coef) * sign_coef|`, where `psy_weight` is the
position weight (`dct4_weight_tab`/`dct8_weight_tab`) times `psy_trellis`. This biases the
search toward preserving perceptually significant coefficients.

**DC variants.** For DC blocks the unquantization factor is doubled from the single DC
multiplier, and the special DC rounding rule is applied to every candidate. The chroma 4:2:2
DC variant uses eight coefficients and the `coeff_flag_offset_chroma_422_dc` table.

**Backtracking and writeback.** Every accepted transition appends a level-tree entry
containing the previous level index and the chosen absolute level. After the last
coefficient, the node with the lowest score is selected (the low-context group for 4x4, the
high-context group for 8x8) and the level tree is walked forward, writing
`sign(original) * level` into each storage slot. A 4x4 AC block whose backtrack yields the
zero sentinel has its entire 16-coefficient output zeroed.

**SIMD structure of the implementation.** Distortion and psy terms are computed two ways in
parallel (the two candidate levels); context handling for four nodes of the same coefficient
is done four ways in parallel even for invalid nodes; node updates use a structure-of-arrays
layout so scores and level indices can be processed with packed adds and shuffles. The
implementation notes that further specialization of the node-ctx switch was tried and
rejected as not faster due to instruction-cache pressure — a useful guidance for any clean
reimplementation.

## 6. Dependencies

* `common/common.h` — `dctcoef`, `udctcoef`, alignment macros, `x264_t`.
* `common/bitstream.h` — `x264_run_level_t`.
* `common/x86/quant-a.asm` — all non-trellis implementations.
* `common/x86/trellis-64.asm` — trellis implementations (x86-64 only, SSE2 and SSSE3).
* `common/cabac.h` — state encoding and entropy tables used by trellis cost estimation.
* `common/quant.c`, `encoder/quant.c`, `encoder/cabac.c` — callers and C fallbacks.

## 7. Constants and tables

* Six QP residue classes; scale derived from `QP*43>>8`.
* Multiply/divide normalization constant 16 for quantization, 8 for trellis unquantization.
* Score bias `1<<60` and maximum score sentinel used by the dynamic program.
* CABAC rate granularity: 8 fractional bits; trellis lambda granularity: 4 fractional bits.
* Position weight tables `dct4_weight_tab`, `dct8_weight_tab`, `dct4_weight2_tab`,
  `dct8_weight2_tab`; context-offset tables for 8x8 and 4:2:2 chroma DC.

## 8. Performance notes

All functions are hot during RD decision and residual coding. The table-driven dispatch means
a single indirect call per kernel; branches inside the kernels are limited to QP path
selection and early-out conditions.

**Concrete opportunities (referenced):**
1. *Wider vectors*: the existing AVX2 quantizer and AVX512 dequantizer show how to double or
   quadruple lane count; a clean implementation should structure loops so the same macro
   body can be instantiated at 128/256/512 bits.
2. *Prefix/scan*: `coeff_level_run` LUT uses `pshufb` + popcount offsets instead of a serial
   loop; a clean implementation can express this as a prefix sum over a byte mask, which
   maps to `tzcnt` chains on GPRs or widening scans on vectors.
3. *Branch removal*: `ptest`, `vptestm*`/`ktest`, `bsr`→`lzcnt`, and BMI2 `shlx`/`shrx`/`bzhi`
   eliminate data-dependent branches; use them where the ISA allows.
4. *Cache*: keep the 256-byte shuffle table and decimate tables contiguous; the reference
   halves the shuffle table by storing bytes and unpacking them on load.

## 9. References

* H.264 / MPEG-4 AVC standard (ITU-T H.264), quantization and CABAC residual syntax.
* x264 public source: `common/quant.h`, `common/quant.c`, `common/x86/quant-a.asm`,
  `common/x86/trellis-64.asm`, `common/tables.c`.
* Intel Intrinsics Guide for the named SIMD/BMI instructions.
* "Trellis quantization" literature (e.g. Sullivan, video coding rate-distortion optimization).

## 10. Verification checklist

1. Instantiate the dispatch table for each CPU feature level; assert every pointer is
   non-null and corresponds to the highest available ISA suffix.
2. Compare each function pointer's output against the corresponding C implementation over
   randomized coefficient arrays, both bit depths, and QP 0..51.
3. Verify that `coeff_last*` and `coeff_level_run*` agree on `last`, and that `mask` has
   exactly `count` set bits where `count` equals the number of levels written.
4. Verify trellis output equality against the C trellis for all six variants, with
   `psy_trellis = 0` and nonzero, `b_ac`/`b_interlaced` 0 and 1, and `i_coefs` 4/16.
5. Confirm no kernel reads beyond `level[18]` or writes outside the supplied arrays by
   guarding them with canary values.

## 11. Open questions

* **UNKNOWN**: the exact value of the all-zero sentinel returned by AVX512 `coeff_last*`.
* **UNKNOWN**: whether `optimize_chroma_2x4_dc` has an x86 implementation; only the 2x2
  variant is declared in `common/x86/quant.h`, so the 2x4 entry is presumably C-only.
* **UNKNOWN**: whether `coeff_last[14]` / `coeff_level_run[13]` array slots beyond the named
  sizes (4/8/15/16) are ever indexed with other values in current code.
* Trellis register-level details are inferred from `trellis-64.asm`; if that file is outside
  the assigned scope, treat micro-optimization notes as advisory only.
