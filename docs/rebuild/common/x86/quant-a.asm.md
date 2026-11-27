# quant-a.asm — x86 quantization / dequantization / coefficient-scan kernels

Source path: `common/x86/quant-a.asm` (declared in `common/x86/quant.h`).
Role: SIMD implementations of coefficient quantization, inverse quantization, DCT-domain
noise reduction, "decimate" zero-block scoring, last-nonzero scanning, and level/run
extraction.
**Dirty-side behavioural specification** — derived from the reference source and public
literature; describes behaviour and structure only, never reproduces source text.

---

## 1. Header

This file is the x86 assembly companion to `quant.c`. It supplies the processor-dispatched
hot kernels used by rate-distortion quantization and reconstruction. Trellis quantization
kernels are *declared* in `quant.h` but are implemented in `trellis-64.asm`; their complete
semantics are specified in `quant.h.md` and cross-referenced here in §5.9.

## 2. Purpose

Quantization is applied to every 4x4 / 8x8 transform block on the encode path. Because the
operation is elementwise but applied to many blocks per frame, and because the accompanying
scans (last coefficient, run/level) are latency-sensitive for CABAC, these routines are
among the hottest in the encoder. This module exists to:

* fold the sign-magnitude rounding and multiply/divide of quantization into packed
  word/doubleword SIMD arithmetic;
* produce a nonzero flag (or four flags for `quant_4x4x4`) without a scalar reduction loop;
* invert quantization for reconstruction with correct rounding for both left and right
  shifts of the QP-dependent scale;
* accelerate the "is this block cheap enough to skip?" decimation score and the
  prefix/scan operations that feed CABAC coefficient coding.

## 3. Public interface

All symbols use the project's `x264_template` mangling (a compile-time prefix). Function
families, grouped by task (exact C signatures live in `quant.h.md`):

| Family | Variants (ISA suffix) | Inputs / outputs |
|---|---|---|
| `quant_2x2_dc` | mmx2, sse2, ssse3, sse4 | `dct[]` in place; scalar `mf`, `bias`; returns nonzero flag |
| `quant_4x4_dc` | mmx2 (32-bit builds), sse2, ssse3, sse4, avx2 | 16 coefficients, scalar `mf`/`bias` |
| `quant_4x4` | mmx2 (32-bit only), sse2, ssse3, sse4, avx2 | 16 coefficients, per-coefficient `mf[]`/`bias[]` |
| `quant_8x8` | mmx2 (32-bit only), sse2, ssse3, sse4, avx2 | 64 coefficients, per-coefficient tables |
| `quant_4x4x4` | sse2, ssse3, sse4, avx2 | four 16-coefficient blocks in one call; returns 4-bit nonzero mask |
| `dequant_4x4`, `dequant_8x8` | mmx (32-bit), sse2, avx, xop, avx2, avx512 | in-place coefficients, `dequant_mf[6][N]`, `i_qp` |
| `dequant_4x4dc` | mmx2 (32-bit), sse2, avx, xop, avx2 | DC-only scaling of all 16 coefficients |
| `dequant_4x4_flat16`, `dequant_8x8_flat16` | mmx (32-bit), sse2, avx2, avx512 | 8-bit only; non-flat QP shortcut |
| `idct_dequant_2x4_dc`, `idct_dequant_2x4_dconly` | sse2, avx | folded 2x4 DC IDCT + dequant |
| `optimize_chroma_2x2_dc` | sse2, ssse3, sse4, avx | 8-bit only; returns zero/nonzero decision |
| `denoise_dct` | mmx (32-bit), sse2, ssse3, avx, avx2 | in-place coefficients plus running sum/offset arrays |
| `decimate_score15`, `decimate_score16`, `decimate_score64` | sse2, ssse3, avx2/avx512 (varies) | score capped at 9 |
| `coeff_last4/8/15/16/64` | mmx2, sse2, lzcnt, avx2, avx512 | index of last nonzero coefficient |
| `coeff_level_run4/8/15/16` | mmx2, sse2, lzcnt, ssse3(+lzcnt), avx2 | fills `x264_run_level_t` |

Contracts: quantization routines overwrite their coefficient arrays in place and return a
truth value (0 = all coefficients became zero); dequant routines overwrite in place and
return void; scan routines never modify coefficients. `coeff_last*` return `-1` when every
coefficient is zero for the non-AVX512 paths (see §11 for the AVX512 edge case).

## 4. State and data structures

* `dctcoef` is the transform coefficient type: 16-bit signed in 8-bit builds, 32-bit signed
  in high-bit-depth (10-bit) builds. `udctcoef` is the unsigned counterpart used for
  multiplier/bias arrays, so that a 16-bit `mf` can hold the full scale without a sign bit.
* `dequant_mf[6][N]` holds one table per QP residue class (QP mod 6). The SIMD code selects
  the row once per call and addresses coefficients relative to that row.
* `x264_run_level_t` (defined in `common/quant.h`) contains a `last` index, a `mask` of
  nonzero positions, 16-byte alignment padding, and an array of levels. The SIMD paths
  use a local struct-of-arrays shadow with byte-sized `mask` and word/dword `level` entries.
* The AVX2/AVX512 code uses YMM/ZMM registers; 32-bit x86 has fewer GPRs, so some kernels
  spill arguments to the stack (e.g. the 64-coefficient last-index scan keeps two halves
  in separate GPRs and recombines via a 32-bit shift/or).
* Scratch: most routines are register-only. High-bit-depth dequant uses a 64-byte-aligned
  rodata block of shuffle constants. `coeff_level_run` LUT paths need a 256-entry byte
  shuffle table plus a population-count lookup table.
* Trellis (in the sibling file) allocates a large stack frame for a level tree and two
  node arrays stored as structure-of-arrays to permit four-way SIMD updates.

## 5. Algorithm / behaviour

### 5.1 Scalar rounding rule implemented by every quantizer

For each coefficient `d` with multiplier `mf` and bias `f`:

* magnitude is taken (`abs(d)`);
* the unsigned sum `abs(d) + f` is multiplied by `mf`;
* the product is shifted right by 16 bits (or 32-bit multiply-shift for 10-bit builds);
* the original sign is re-applied.

For 8-bit builds the packed word datapath computes
`q = (abs(d) + f) * mf >>> 16` with an unsigned high-half multiply, then restores sign with
a sign-flip instruction. For 10-bit builds coefficients are 32-bit: the high path uses a
packed 32x32->32 low multiply plus a 16-bit right shift, while the SSE2 fallback emulates
that with two `pmuludq` operations on the even/odd lanes and recombines them. Bias and
multiplier are scalar for `*_dc` variants (broadcast once into a vector) and per-position
for the AC variants.

Return value: a logical OR reduction of all resulting coefficients. For `quant_4x4x4` the
four blocks are OR-reduced separately and packed into a 4-bit mask; the AVX2 variant
combines 256-bit lane halves with `vpermq`/`packssdw` and a `movmskps` extraction, then
masks to the low four bits.

ISA progression: MMX2 only supplies the 2x2 DC form on 64-bit hosts (SSE2 is faster there);
SSSE3 improves sign handling and byte shuffles; SSE4 adds `ptest` for the zero test and
`pmulld` for 10-bit; AVX2 widens to 8 words / 4 dwords per vector. On 32-bit x86 the MMX
variants of 4x4/8x8 and dequant are retained because SSE2 is not universally available.

### 5.2 Dequantization

QP decomposition is identical for all dequant kernels: `q = floor(QP/6)` gives the binary
scale (`i_qbits`) and `QP mod 6` selects the multiplier row. The code uses the multiply
trick `floor(QP * 43 / 256)` to compute `floor(QP/6)` without division, then
`QP - q - 5*q` for the remainder. The multiplier row pointer is advanced once before the
coefficient loop.

Two paths exist:

* **Left shift** (`i_qbits >= 0`, minus a per-block constant: 4 for 4x4, 6 for 8x8 and DC):
  multiply coefficient by multiplier, then shift left. For 8-bit builds the multiplier
  table is packed from dwords to words and the multiply is `pmullw` followed by `psllw`;
  for 10-bit builds it is `pmaddwd` on dword lanes followed by `vpsllvd`/`pslld`.
* **Right shift** (`i_qbits < 0`): the magnitude is multiplied by the multiplier, a
  rounding constant `f = 1 << (-i_qbits - 1)` is added, and the result is shifted arithmetically
  right by `-i_qbits`. The 8-bit path widens words to dwords with `punpck`/`pmovzxwd`, uses
  `pmadcswd`, shifts, and repacks with saturation. The AVX512 path instead broadcasts
  `1 << (-i_qbits-1)` and the negative shift count, adds the bias with `paddd`, and applies
  `vpsravd` — one uniform operation per lane, no branch inside the block.

The **flat16** variants (8-bit builds only) exploit the fact that at QP >= 12 for 8x8 (and
for all qualifying QP for 4x4) the dequantization scale fits in 16 bits: they fetch the
pre-scaled `dequant4_scale`/`dequant8_scale` constants, shift them left by `q`, and do a
plain `pmullw`, skipping the widening/repacking path entirely. For 8x8 at QP < 12 they tail
call into the general AVX512 kernel.

`dequant_4x4dc` broadcasts only the DC multiplier `dequant_mf[row][0]`, shifts it, and
applies it to all 16 entries; the right-shift path folds the rounding constant into a
`pmaddwd` operand pair so that the same multiply produces the rounded result.

`idct_dequant_2x4_dc`/`dconly` merge the 1-D 4-point DC inverse transform with dequantization
for chroma DC blocks. The eight input coefficients are combined by butterfly sums/differences,
then multiplied by a sign pattern derived from the QP row (`(-dmf)<<16 | dmf`) so that one
packed multiply applies both signs. For QP/6 >= 6 the multiply is left-shifted; otherwise a
rounding constant is subtracted before an arithmetic right shift. The results are either
written back as eight dequantized coefficients (`dconly`) or scattered into the DC slot of
eight 4x4 blocks (`dc`). 10-bit builds keep dwords through the butterfly with 16-bit
intermediate precision; 8-bit builds pack to words early.

### 5.3 Chroma DC zero optimization

`optimize_chroma_2x2_dc` (8-bit only) decides whether the four 2x2 chroma DC coefficients
round to zero after dequantization and inverse transform, allowing the whole block to be
skipped. It forms the four even/odd sums with sign masks, multiplies by the dequant factor,
adds the rounding constant, and early-exits if all four results round to zero. Otherwise it
iterates over coefficients, decrementing them one step at a time while the rounded result
stays zero, and writes back the reduced values. SSE4 uses `ptest` for the zero test; older
ISAs compare against an all-ones mask via `pmovmskb`. SSSE3 uses byte-shuffle masks where
MMX/SSE2 need sign-extension tricks.

### 5.4 DCT denoising

`denoise_dct` subtracts a per-coefficient adaptive offset and accumulates absolute values
into a running sum array:

* **10-bit**: absolute values are computed as dwords; `sum[i]` is increased by `abs(dct[i])`;
  the offset is subtracted from the magnitude; negative results are clamped to zero before
  the sign is restored; the cleaned coefficient is stored back.
* **8-bit**: magnitudes are word values; `paddusw`/`psubusw` implement saturating subtraction
  against the offset; the signed result is written back. The absolute values are widened to
  dwords and added into the 32-bit `sum` array. AVX2 uses `vpermq q3120` to restore lane order
  after packing.

The loop walks backwards from `size`, processing two (`mmsize/2` for 8-bit AVX2) vectors per
iteration.

### 5.5 Decimation scoring

`decimate_score15/16/64` estimate whether a block's coefficients are small enough that the
block can be skipped without a full RD decision. They first build a byte mask of nonzero
coefficients and a byte mask of coefficients whose magnitude exceeds one; any magnitude > 1
immediately returns the maximum score 9. The nonzero mask is then consumed as a bit sequence:
a lookup table (`decimate_mask_table4` for 16-coefficient blocks, `decimate_table8` for
64-coefficient blocks) maps groups of bits to partial scores, and a loop over set bits
using `tzcnt` (with `lzcnt`/`bsr` for the table indexing) accumulates until the score reaches
9 or the mask is exhausted. For the 15-coefficient variant the first bit is discarded because
the DC coefficient is excluded. AVX512 replaces mask extraction with `vptestmw`/`vptestmd`
and `vpcmpu*` opmask compares; AVX2 uses `ptest` to short-circuit the >1 case and `vpermq`
to align lanes. The returned value is clamped at 9.

### 5.6 Last-nonzero scans

`coeff_lastN` finds the highest zig-zag index with a nonzero coefficient. All paths reduce
the coefficient array to a bitmap of zero/nonzero lanes, invert it, and find the highest set
bit with `bsr` or — when LZCNT is available — with `lzcnt` plus an XOR of the bit width.
High-bit-depth inputs are first packed from dwords to words and then to bytes; 8-bit inputs
are packed directly to bytes; `pmovmskb` extracts the bitmap into a GPR. The 15-coefficient
variant reads from `dct - 1` and shifts the bitmap right by one to skip the DC slot; for
64 coefficients the bitmap is assembled from four 16-lane groups into a 64-bit mask (32-bit
hosts handle the two halves separately and add 32 to the index for the upper half).

AVX512 replaces the pack/shuffle chain with `vptestm*`/`vpcmp*` into opmask registers,
`kunpckwd`/`kunpckdq` to assemble halves, `kmov` to a GPR, and `lzcnt` to derive the index.
The 8-bit AVX512 `coeff_last8` uses a 128-bit vector; 10-bit uses 128/256/512-bit vectors
for 4/8/15/16/64 respectively.

### 5.7 Level/run extraction

`coeff_level_runN` produces, for a block of N coefficients, the index of the last nonzero
coefficient, a bitmask of nonzero positions, and the compacted list of nonzero levels.
The straightforward paths derive the mask as above, invert it, place it in the high bits of
a register, and repeatedly use a leading-zero count to skip runs of zeros: each iteration
advances by the run length, stores the next level, and decrements the remaining index.

The SSSE3 "LUT" variants keep the coefficients in vector registers and use `pshufb` with a
256-byte shuffle table indexed by each 8-bit slice of the nonzero mask. This performs a
SIMD gather of the nonzero levels in one operation per half-block, using a population-count
table to compute the destination offset. Stores intentionally overlap to avoid a variable
128-bit shift; the reference notes this causes store-forwarding pressure but is still much
faster than the serial version. AVX2 builds the two shuffle masks into the two 128-bit lanes
of a YMM register with `vinserti128` and extracts the result with `vextracti128`.

### 5.8 Register/ABI details

The Windows 64-bit ABI's register set forces different temporary allocation than Unix64;
the file uses `DECLARE_REG_TMP` to remap symbolic temporaries. On 32-bit x86, stack argument
access macros (`r0mp`, `r1mp`, ...) replace register arguments. Functions using the AVX
3-operand form avoid destructive moves; MMX/SSE2 paths rely on `SWAP` macros to move values
between registers. None of these routines calls out except the `coeff_level_run` LUT helper
(which may call nothing) and the dequant tail-call into the AVX512 kernel.

### 5.9 Trellis variants (implemented in `trellis-64.asm`)

`trellis_cabac_4x4`, `_8x8`, `_dc`, `_chroma_422_dc` and their `_psy` forms are part of this
kernel family but live in the sibling file. In outline: they walk the zig-zag coefficients,
maintain eight dynamic-programming nodes (four for the low context range and four for the
high range), evaluate two candidate absolute levels (the quantized value and that value
minus one), add a distortion term weighted by a coefficient-position table and a rate term
`bits * lambda2 >> 4`, and pick the minimum-score path. Psycho-visual variants subtract a
reward derived from the unquantized source DCT, DC variants special-case rounding to
multiples of 16, and the 4:2:2 chroma DC variant uses eight coefficients and a dedicated
context-offset table. Full parameter and state semantics are in `quant.h.md` §5.9.

## 6. Dependencies

* Includes `x86inc.asm` and `x86util.asm` for `cglobal`, `INIT_*` SIMD dispatch, register
  allocation, and macros such as `ABSW`, `ABSD`, `PSIGNW`, `PSIGND`, `SPLATW`, `SPLATD`,
  `SUMSUB_BA`, `TRANSPOSE4x4W`.
* External C symbols: `pb_1`, `pw_1`, `pw_2`, `pw_256`, `pd_1`, `pb_01`, `pd_1024`,
  `deinterleave_shufd`, `popcnt_table`, `decimate_table4`, `decimate_table8`; and the
  dequantization scale tables `dequant4_scale`, `dequant8_scale`.
* Called from `x264_quant_init()` (in `quant.c`) which installs function pointers into
  `x264_quant_function_t`; called indirectly from `x264_quant_init` consumers in
  `encoder/quant.c` / `encoder/cabac.c`.
* `coeff_last_*` functions are also consumed by `cabac-a.asm` through a dispatch table of
  relative offsets (see `cabac-a.asm.md`).

## 7. Constants and tables

* **dequant4_scale / dequant8_scale** — six rows of pre-scaled 16-bit multipliers for the
  flat-16 shortcut, derived from the six QP residue classes; stored as word pairs so both
  4x4 and 8x8 blocks can use them with word multiplies.
* **decimate_mask_table4 / decimate_table8** — precomputed partial scores for runs of
  nonzero coefficients; the 4-bit table is indexed by a 5-bit pattern, the 8-bit table by
  8-bit patterns. Values encode the expected "cost" of a run so that the cumulative score
  approximates a bit-cost threshold; the score saturates at 9.
* **chroma DC sign masks** (`chroma_dc_dct_mask*`, `chroma_dc_dmf_mask*`) — encode the
  alternating signs of the 2x2 chroma DC inverse transform.
* **dct_coef_shuffle** — 256-byte table mapping an 8-bit nonzero mask to a `pshufb`
  control that gathers the corresponding coefficient bytes.
* **dequant_shuf_avx512 / decimate_shuf_avx512** — lane-reorder constants that convert
  between packed-word storage and dword lanes for 512-bit paths.
* `imul ..., 0x2b; shr ..., 8` is a division-by-6 idiom (43/256 approximates 1/6), deliberately
  chosen to avoid a slow integer divide.

## 8. Performance notes

**Hotness.** Quantization runs for every candidate block in RD mode decision, so the
4x4/8x8 and 4x4x4 variants dominate at `preset=medium` and slower where multiple QP
candidates are evaluated. `coeff_last`/`coeff_level_run` run once per coded block and are
on the critical path of CABAC residual coding. Decimation runs on skipped-block candidates
and is hot on static content.

**Cache/branch behaviour.** The 256-byte `dct_coef_shuffle` table and the decimate tables
are small enough for L1; the AVX512 paths deliberately avoid them where opmask comparisons
can produce the mask directly. The decimate loops are branch-heavy but bounded at nine
iterations by the score cap. The quantization kernels are branch-free except for the QP
left/right-shift decision, which is resolved once per call.

**Concrete SIMD speed opportunities (referenced):**

1. *Wider vectors.* The 8-bit AVX2 quantizer processes 16 coefficients per `QUANT_TWO`
   iteration instead of eight; a future AVX512 path could broadcast `mf`/`bias` to 32 words
   and process a whole 8x8 block in two operations. The reference already uses ZMM for the
   10-bit AVX512 dequantizer (`vpsllvd`, `vpsravd`), showing the intended direction.
2. *Prefix/scan patterns.* `coeff_last64` assembles a 64-bit zero mask and uses one `bsr`/
   `lzcnt`; the `coeff_level_run` LUT path replaces serial skipping with a `pshufb` gather
   plus a population-count-based prefix offset. A further speedup would be a true SIMD
   prefix sum over lane masks (e.g. chained `paddd` with exclusive-scan shuffles) to compute
   all compaction destinations at once, avoiding the overlapping unaligned stores the
   reference explicitly calls out as a store-forwarding bottleneck.
3. *Branch removal.* Replace the `bsr`-based table indexing in decimation with `tzcnt`+
   `lzcnt` (already done partly) and, on AVX512, accumulate with `vptestmb` opmasks and
   `ktest` so that no GPR mask loop is needed at all. Replace the variable shifts in
   quantizer sign handling with `psignd`/`psignw` (SSSE3+) and `shlx`/`shrx` (BMI2), as the
   reference does in its SSE4 and BMI2 paths.
4. *Lane-order fixes.* `vpermq q3120` after `packssdw` is used repeatedly in the 8-bit
   AVX2/AVX512 dequantizers; fusing the pack and permutation into a single `vpermt2w`
   (as the AVX512 8x8 path already does) removes a shuffle uop.
5. *Early-out.* `ptest` (SSE4) turns the nonzero reduction into one instruction; on earlier
   ISAs the equivalent needs `packsswb` + `movq` + GPR test. Keeping the accumulator in a
   vector and testing once at the end (rather than per block in `quant_4x4x4`) is the
   pattern the AVX2 `quant_4x4x4` uses by AND-ing the two lane-halves of the mask.

## 9. References

* ITU-T H.264 / ISO IEC 14496-10, sections on transform coefficient quantization and
  inverse quantization (dead-zone with QP-dependent offset).
* x264 project documentation and public source distribution (`common/x86/quant-a.asm`,
  `common/quant.c`, `common/tables.c`) — behavioural reference only.
* Intel Intrinsics Guide: `pmulhuw`, `pmuludq`, `pmadcswd`, `pshufb`, `ptest`, `tzcnt`,
  `lzcnt`, `vpermt2w`, `vpsravd`, `kmov`, `ktest`.
* Hacker's Delight (Warren), chapter on bit-scan and population-count techniques.

## 10. Verification checklist

For each function, compare SIMD output against the scalar C reference over randomized
inputs (including all-zero, all-one, sign extremes, QP 0..51, and both bit depths):

1. `quant_*` — coefficient array equality plus exact nonzero return value; for `quant_4x4x4`
   verify each of the four flag bits.
2. `dequant_*` — equality for QP values that exercise left shift, right shift and the
   boundary (`QP/6 - offset == 0`); check the flat16 variants only in their documented QP
   range and confirm they fall through otherwise.
3. `idct_dequant_2x4*` — compare scattered DC values in all eight destination blocks.
4. `optimize_chroma_2x2_dc` — compare both the reduced coefficients and the return flag.
5. `denoise_dct` — verify in-place coefficients and the accumulated `sum[]` after multiple
   calls with different offsets.
6. `decimate_score*` — exhaustive over all 16-bit/64-bit masks with coefficients in
   `{0, 1, -1, 2, -2}`; score must equal C and never exceed 9.
7. `coeff_last*` — all-zero input, single coefficient at index 0 and N-1, and random
   positions; verify the `-1` convention for the non-AVX512 paths and document the AVX512
   all-zero result explicitly (see §11).
8. `coeff_level_run*` — verify `last`, `mask`, level order, and that padding entries are not
   read by the consumer.
9. ISA equivalence: run the project's `checkasm` harness, which calls each SIMD variant
   with fresh stack-clobber patterns and compares against C.

## 11. Open questions

* **UNKNOWN**: the exact all-zero return value of the AVX512 `coeff_last*` variants. The
  non-AVX512 implementations return the "no set bit" value from `bsr`/`lzcnt` (typically
  -1 or a large unsigned value); the AVX512 path uses `lzcnt` + XOR and may instead return
  the vector width. Consumers appear to rely on `last == N-1` comparisons rather than the
  negative case, but this needs confirmation.
* **UNKNOWN**: whether the MMX 4x4/8x8 quantizers and MMX dequantizers are still reachable
  in current builds; they are guarded by 32-bit x86 and may be dead code.
* **UNKNOWN**: precise preset-dependent hotness numbers; no profiling data is available.
* The trellis implementation is outside the assigned file; its stack layout and node
  transition details are described from `trellis-64.asm` for completeness but should be
  confirmed against that file's own specification.
