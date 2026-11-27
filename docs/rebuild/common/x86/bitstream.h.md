# bitstream.h — x86 bitstream function interface

Source path: `common/x86/bitstream.h` (include guard `X264_X86_BITSTREAM_H`).
Role: declares x86 NAL-escaping and CABAC residual-coding entry points and the dispatch
table that selects them.
**Dirty-side behavioural specification** — own words, no source reproduction.

---

## 1. Header

This header is the x86 companion to `common/bitstream.h`. It declares the SIMD implementations
of emulation-prevention insertion and of the internal CABAC coefficient coding routines,
and it is included by `common/bitstream.c` when MMX is available. All symbols use
`x264_template` mangling.

## 2. Purpose

Bitstream writing has two hot, data-dependent parts: inserting `0x03` bytes to avoid H.264
start-code emulation, and encoding quantized transform coefficients with CABAC. This header
exposes fast x86 versions of both through a single function-pointer table so the encoder can
stay ISA-agnostic.

## 3. Public interface

### 3.1 NAL escape

`uint8_t *nal_escape_{mmx2,sse2,avx2}(uint8_t *dst, uint8_t *src, uint8_t *end)`

Writes an escaped copy of `[src,end)` into `dst` and returns the destination pointer after
the last byte. Insertion rule: before any source byte `<= 3` that follows two zero source
bytes, emit `0x03`. The first two bytes are copied verbatim. See `bitstream-a.asm.md` for the
full dataflow.

### 3.2 CABAC residual coding (RD / size-estimation variants)

`void cabac_block_residual_rd_internal_<isa>(dctcoef *l, int b_interlaced,
intptr_t ctx_block_cat, x264_cabac_t *cb)`
`void cabac_block_residual_8x8_rd_internal_<isa>(dctcoef *l, int b_interlaced,
intptr_t ctx_block_cat, x264_cabac_t *cb)`

ISA suffixes for both: `sse2`, `lzcnt`, `ssse3`, `ssse3_lzcnt`, `avx512`.
These routines do **not** write bits; they update `cb->f8_bits_encoded` and the context state
array using CABAC entropy estimates, while leaving the arithmetic coder state
(`low`/`range`/`queue`/`p`) untouched. The 4x4 form handles 16 coefficients; the 8x8 form
handles 64 and applies the 8x8-specific context offset tables.

### 3.3 CABAC residual coding (full-write variants)

`void cabac_block_residual_internal_{sse2,lzcnt,avx2,avx512}(dctcoef *l, int b_interlaced,
intptr_t ctx_block_cat, x264_cabac_t *cb)`

These call the actual CABAC encode primitives and therefore write bytes to `cb->p`, advance
the queue/state, and update `state[]`. They handle both 4x4 and 8x8 coefficient counts; the
block size is inferred from `ctx_block_cat` (8x8 categories use the 64-entry tables).

### 3.4 Dispatch table

`x264_bitstream_function_t` (in `common/bitstream.h`) holds four pointers:
`nal_escape`, `cabac_block_residual_internal`, `cabac_block_residual_rd_internal`,
`cabac_block_residual_8x8_rd_internal`. `x264_bitstream_init(cpu, pf)` fills them.

## 4. State and data structures

* `dctcoef *l` — quantized coefficients in zig-zag storage order; not modified by any of
  these routines.
* `ctx_block_cat` — an integer identifying the block category (luma DC, luma AC, luma 8x8,
  chroma DC, chroma AC, and plane variants). It indexes context-offset tables for
  significant flags, last flags, level-minus-one, and `count_cat_m1`.
* `b_interlaced` — MBAFF flag; shifted left by 4 and added to `ctx_block_cat` before indexing
  the 8x8 offset tables.
* `x264_cabac_t *cb` — the coder object; for RD variants only `state[]` and
  `f8_bits_encoded` are touched; for full-write variants all fields may change.
* `x264_run_level_t` (declared in `common/bitstream.h`, not this header) — `last`, `mask`,
  and 18-entry aligned `level[]`; used by the coefficient level/run scanners that feed these
  routines, not by the routines themselves.

## 5. Algorithm / behaviour

### 5.1 Escape semantics (summary)

Two-zero history plus current byte `<= 3` triggers insertion of `0x03`; the inserted byte
resets the history. The returned pointer accounts for all inserted bytes. The SIMD loops
detect candidate adjacent zeros with `pcmpeqb`/`pmovmskb` and use `tzcnt` to find the first
candidate, falling back to a short scalar loop for actual insertion.

### 5.2 Residual RD semantics (summary)

* Compute absolute coefficient values into a private stack copy.
* Find the last nonzero coefficient via the category-appropriate `coeff_last` routine.
* Emit entropy costs for significant/last flags at `last` (unless the last index is already
  known from `count_cat_m1`), then for each lower index emit significant=0/1 and, if
  nonzero, last=0 and the level cost.
* Level cost uses the `coeff_abs_level1_ctx`/`coeff_abs_levelgt1_ctx` context progression, a
  unary prefix table capped at 14, and an Exp-Golomb suffix cost for levels >= 15.
* Update `state[]` exactly as the real encoder would, so that subsequent blocks see the same
  probability contexts; accumulate `f8_bits_encoded` in 1/256-bit units.

### 5.3 Full-write residual semantics (summary)

Same syntax-element order, but each decision calls the real CABAC primitive (asm/BMI2 where
available), so the bitstream is emitted. The 8x8 branch is chosen by `ctx_block_cat`, and the
large-level escape is implemented by a call to `cabac_encode_ue_bypass`. See
`cabac-a.asm.md` §5.6 for full dataflow.

### 5.4 ISA dispatch details

`x264_bitstream_init` (in `common/bitstream.c`) starts from the C fallbacks and upgrades as
features are detected:

* MMX2 → `nal_escape_mmx2`; SSE2 → SSE2 escape and the sse2 residual variants;
* SSE3/SSSE3 → the `ssse3` residual RD variants; LZCNT → `lzcnt` variants (and the combined
  `ssse3_lzcnt` when both are present);
* AVX2 → AVX2 escape and AVX2 full-write residual;
* AVX512 → AVX512 residual variants for both RD and full-write.

AVX2 escape requires 64-bit x86 (guarded by `ARCH_X86_64` in the assembly).

### 5.5 8-bit vs 10-bit differences

`dctcoef` is 16-bit in 8-bit builds and 32-bit in high-bit-depth builds. The assembly
selects word vs dword loads and absolute-value operations with `HIGH_BIT_DEPTH` conditionals.
For AVX512, the 8-bit RD kernel uses YMM vectors and the 10-bit one uses ZMM. Stack scratch
sizes scale with coefficient width for the absolute-value copies. NAL escaping is byte
oriented and identical in both builds.

### 5.6 Alignment and scratch

The residual kernels use stack copies of the absolute coefficients (16 or 64 entries) and a
small local structure for level/run data; no caller-supplied scratch is required. `l` itself
needs no special alignment (the assembly uses word/dword loads that tolerate it), although
the encoder's DCT buffers are 16-byte aligned in practice. `dst` alignment affects which NAL
escape loop is taken but is not required.

## 6. Dependencies

* `common/bitstream.c` — C fallbacks, `x264_bitstream_init`, `x264_nal_encode` caller.
* `common/bitstream.h` — `bs_t`, `x264_run_level_t`, `x264_bitstream_function_t`, level token
  tables.
* `common/cabac.h` — `x264_cabac_t`, encode primitives, size helpers.
* `common/tables.c` — CABAC entropy/transition tables, context offsets, `count_cat_m1`.
* `common/x86/bitstream-a.asm`, `common/x86/cabac-a.asm` — implementations.
* `common/x86/quant-a.asm` — `coeff_last` dispatch targets used internally.

## 7. Constants and tables

* Escape thresholds: two preceding zero bytes and current byte `<= 3`; inserted byte `0x03`.
* `count_cat_m1` — per-category index of the last coefficient that may be nonzero; used to
  short-circuit the significance map at the top end.
* Context offset tables: `significant_coeff_flag_offset`, `significant_coeff_flag_offset_8x8`,
  `last_coeff_flag_offset`, `last_coeff_flag_offset_8x8`, `coeff_abs_level_m1_offset`.
* Level prefix cap 14; Exp-Golomb escape threshold 15; rate fixed-point 1/256 bits.
* `coeff_abs_level1_ctx`, `coeff_abs_levelgt1_ctx`, `coeff_abs_level_transition` — context
  progression tables in `cabac-a.asm`.

## 8. Performance notes

Residual coding is the dominant CABAC workload at high bitrates; NAL escaping is a small but
serial post-step.

**Concrete SIMD speed opportunities (referenced):**
1. *Wider vectors.* The header already exposes AVX2 escape and AVX512 residual variants; a
   clean implementation should template the absolute-value copy and entropy lookup over
   vector width, as the assembly does with its `ABS_DCTCOEFS` macro.
2. *Prefix/scan patterns.* Use `tzcnt` on a `pmovmskb`/opmask result for the first escape
   candidate, and leading-zero counts for Exp-Golomb suffix costs; both are already used in
   the reference kernels.
3. *Branch removal.* RD variants avoid the bitstream entirely and can be fully table-driven;
   use opmask arithmetic on AVX512 to eliminate the `coeff_last` dispatch table call where
   possible.
4. *Cache locality.* Keep the four function pointers in `x264_bitstream_function_t` together
   so one cache line covers the dispatch table; this is already the case.

## 9. References

* ITU-T H.264 recommendation: NAL syntax (7.4.1), CABAC residual coding (9.3.3).
* x264 public source: `common/x86/bitstream.h`, `common/x86/bitstream-a.asm`,
  `common/bitstream.c`, `common/bitstream.h`, `common/x86/cabac-a.asm`.
* Intel Intrinsics Guide: `pcmpeqb`, `pmovmskb`, `tzcnt`, `lzcnt`, `vptestmb`.

## 10. Verification checklist

1. Assert `x264_bitstream_init` selects the expected pointers for a given CPU feature mask.
2. Compare escaped output and return pointer against `nal_escape_c` for the edge cases listed
   in `bitstream-a.asm.md` §10.
3. For each residual variant, encode the same coefficient block with C and SIMD starting from
   identical state, then compare emitted bytes, final state array, and `f8_bits_encoded`.
4. Verify RD variants never modify `low`, `range`, `queue`, `p`, or `bytes_outstanding`.
5. Verify full-write variants handle `ctx_block_cat` values for both 4x4 and 8x8 blocks and
   levels crossing the 15 threshold.
6. Run `checkasm`-style tests for ISA equivalence and stack integrity.

## 11. Open questions

* **UNKNOWN**: whether `cabac_block_residual_internal_avx512` is selected only for 10-bit
  builds or also for 8-bit; both variants are declared in the header but dispatch conditions
  in `bitstream.c` were only partially inspected.
* **UNKNOWN**: the exact mapping of `ctx_block_cat` integer values to the abstract block
  categories; callers in `encoder/cabac.c` supply them via macros outside this file.
* **UNKNOWN**: whether the `ssse3_lzcnt` residual variants are selected on CPUs with both
  features in all build configurations.
