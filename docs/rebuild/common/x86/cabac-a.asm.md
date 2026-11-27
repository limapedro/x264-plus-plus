# cabac-a.asm — x86 CABAC acceleration kernels

Source path: `common/x86/cabac-a.asm`.
Role: SIMD/GPR assembly for binary arithmetic encoding (decision, bypass, terminal,
byte emission) and for encoding transform-coefficient residual syntax elements.
**Dirty-side behavioural specification** — own words, no source reproduction.

---

## 1. Header

This file provides two distinct kinds of acceleration. First, hand-tuned x86 versions of the
three primitive CABAC encode operations and their shared byte-output path, including a BMI2
variant that collapses variable shifts and bit-field extraction into single instructions.
Second, "internal" residual-coding routines that encode the coefficient significance map,
last-position flag, and coefficient levels, operating directly on the CABAC state array and
accumulating a bit-cost estimate without touching the bitstream.

## 2. Purpose

CABAC is on the critical path for every slice: each binary decision updates a probability
state and may emit a byte. The C implementation is correct but has many branches and
variable shifts; the assembly versions remove them and keep the coder state in registers
across calls where possible. The residual variants fuse several decisions that the C code
performs through function calls, reducing call overhead for the most common syntax element
(transform coefficients).

## 3. Public interface

Exported symbols (`x264_template` mangled), grouped:

* `cabac_encode_decision_asm`, `cabac_encode_bypass_asm`, `cabac_encode_terminal_asm`;
* `cabac_encode_decision_bmi2`, `cabac_encode_bypass_bmi2` (terminal has no BMI2 variant —
  the generic one is reused);
* `cabac_block_residual_rd_internal_{sse2,lzcnt,ssse3,ssse3_lzcnt,avx512}` — 4x4 RD path;
* `cabac_block_residual_8x8_rd_internal_{sse2,lzcnt,ssse3,ssse3_lzcnt,avx512}` — 8x8 RD path;
* `cabac_block_residual_internal_{sse2,lzcnt,avx2,avx512}` — non-RD path.

The first group implements the contracts of `x264_cabac_encode_decision/bypass/terminal` in
`common/cabac.h`. The second group implements the internal residual contract declared in
`common/bitstream.h` (see `bitstream.h.md`).

## 4. State and data structures

* `x264_cabac_t` field offsets are mirrored by an assembly structure definition:
  `low`, `range`, `queue`, `bytes_outstanding`, `start`, `p`, `end`, 64-byte-aligned
  `bits_encoded`, then `state[1024]`. The queue is stored with an offset of -8 relative to
  the C description, which is why the emission code adds 10 and subtracts 18 around the
  bit extraction.
* Context state is a byte per context. Bit 0 stores the more-probable symbol (MPS); bits
  1..6 store the probability state index used to look up the LPS range and the transition.
* Rodata tables: `coeff_abs_level1_ctx`, `coeff_abs_levelgt1_ctx`,
  `coeff_abs_level_transition` (local byte arrays), plus external references to
  `cabac_range_lps`, `cabac_transition`, `cabac_renorm_shift`, `cabac_entropy`,
  `cabac_size_unary`, `cabac_transition_unary`, the significant/last context-offset tables,
  `coeff_abs_level_m1_offset`, `count_cat_m1`, and `cabac_encode_ue_bypass`.
* A table of 14 code pointers (`coeff_last_*` variants, in the order
  16/15/16/4/15/64/16/15/16/64/16/15/16/64 style) lets the RD path call the appropriate
  last-coefficient scanner through one indirect `call` indexed by context-block category.
  The table is 64-byte aligned and padded with two zero dwords.
* Scratch: the RD routines reserve `maxcoeffs * sizeof(dctcoef)` bytes on the stack
  (16 or 64 coefficients) to hold absolute values; the non-RD routine reserves `4*64` bytes
  for a local coefficient list, last index, and level-context offset.

## 5. Algorithm / behaviour

### 5.1 Binary decision (regular coding)

For context `ctx` and binary symbol `b`:

1. Load current range and state.
2. Look up the LPS range from `cabac_range_lps[state>>1][(range>>6)-4]`. The table is indexed
   by the probability state (stored in bits 1..6) and a 4-value range bucket.
3. Subtract the LPS range from the total range. If `b` differs from the MPS bit (`state & 1`),
   add the reduced range to `low` and set `range` to the LPS range; otherwise keep the
   reduced range.
4. Update `state[ctx]` from the transition table (`cabac_transition[state][b]`).
5. Renormalize: shift `range` and `low` left until `range >= 256`, counting shifts. The SSE2
   path obtains the shift count from `cabac_renorm_shift[range>>3]`; the BMI2 path uses
   `lzcnt(range) - 23` and `shlx` to shift both values in one instruction each.
6. Add the shift count to `queue`; if the result is nonnegative, call the byte-emission
   routine, otherwise store `low` and `queue` back and return.

The two C "if" branches (MPS/LPS, renorm needed) become `cmovne` and a conditional jump; the
state table load and transition update use the same index computation. The BMI2 version
additionally uses `shrx`/`bzhi` in the emission path (§5.4).

### 5.2 Bypass coding

Bypass coding is a raw arithmetic shift with no probability update: `low = 2*low + (b & range)`,
`queue += 1`, then emit a byte if `queue >= 0`. The assembly implements this with a `lea`
for the doubled-plus-bit value and a conditional jump to the shared emission routine. On
64-bit the emission tail is a tail call; on 32-bit it re-enters with a fresh prologue because
of the different register allocation.

### 5.3 Terminal coding

Terminal decrements `range` by 2 and renormalizes. The reference notes a shortcut: the
renormalization shift can only be 0 or 1 and is zero over 99% of the time, so the common case
is a single `RET`; the rare case shifts `low`/`range` once, increments `queue`, and may emit.
No BMI2-specific terminal exists.

### 5.4 Byte emission (`cabac_putbyte`)

When `queue >= 0`, one byte is ready:

* form the 9-bit output from bits 10+ of `low` after compensating for the -8 queue offset;
  BMI2 extracts it with `shrx` and masks `low` with `bzhi`, while the generic path builds a
  bit mask with `shl`/`not`/`and`;
* if the low 8 bits of the output are `0xff`, the byte is not written yet; instead
  `bytes_outstanding` is incremented and the carry is deferred;
* otherwise, add the carry bit (bit 8 of the output) to the last byte already written at
  `p[-1]`, then write `carry-1` once for each outstanding `0xff` byte, then write the low
  byte and update `p`; reset `bytes_outstanding` to 0.

This is the standard H.264 carry-propagation rule: a run of `0xff` bytes cannot absorb a
carry, so the carry is applied to the first preceding non-`0xff` byte and the run becomes a
run of `0x00` bytes. Finally the routine jumps into the decision routine's state-store tail
so that `low` and `queue` are written back without duplicating code.

### 5.5 RD residual kernels

These encode coefficient levels using only the *size estimator* tables (`cabac_entropy`) and
do not write bits. They update `cb->bits_encoded` (kept as `f8_bits_encoded`, a 1/256-bit
fixed-point accumulator).

Common preparation:

1. Combine `b_interlaced` (shifted left 4) with `ctx_block_cat` to index four context-offset
   tables: significant-flag offset, last-flag offset, and coefficient-level-minus-one offset.
2. Take absolute values of all coefficients into a stack copy, leaving the caller's array
   untouched. 8-bit builds operate on words, 10-bit builds on dwords; the width difference
   changes how many vectors are copied.
3. Run the appropriate `coeff_last` scanner on the original coefficients to obtain the last
   nonzero index, via a dispatch table indexed by block category. The implementation
   deliberately runs this on the originals for out-of-order execution benefit.
4. Pre-add the context state base pointers so that subsequent lookups are single indexed
   loads.
5. If `last != count_cat_m1[category]` (i.e. the last coefficient is not already known to be
   zero), emit significant=1 and last=1 at that index; otherwise skip that step.
6. Encode the absolute level at `last` using the level-1/level>1 context logic (§5.7).
7. Walk indices `last-1` down to 0: for each coefficient emit significant=0 or 1; if nonzero,
   emit last=0 then the level. All decisions use the entropy lookup and update the local
   state copy directly in registers/memory.

The 8x8 variant adds per-position significant/last context offsets loaded from
`significant_coeff_flag_offset_8x8` / `last_coeff_flag_offset_8x8` for each index (a 4 KiB
table indexed by interlace flag and position), and uses 64 coefficients on the stack.

ISA variants differ only in how they take absolute values and which `coeff_last` implementation
they call: SSE2 uses `pabsw`/`ABSW`, SSSE3 uses `pabsb`/`pabsd`, LZCNT changes the scanner,
AVX512 uses ZMM (8-bit) or ZMM (10-bit, with dword lanes) and the AVX512 scanner.

### 5.6 Non-RD residual kernel

This variant calls the real CABAC encode primitives, so it writes bits. It:

1. Computes context offsets and calls `coeff_last` through the dispatch table.
2. Runs a significance-map loop over indices 0..`count_cat_m1`, gathering nonzero
   coefficients into a local array and emitting significant/last decisions through
   `cabac_encode_decision` (BMI2 variant when available).
3. Walks the gathered levels in reverse, computing `abs_level` with a sign-extension trick,
   selecting the level-1 or level>1 context, emitting the unary prefix decisions with
   repeated calls, and handling `abs_level >= 15` via a call to `cabac_encode_ue_bypass`
   (the only C function called from this routine).
4. Emits the sign bit through the bypass primitive.

The ISA suffix selects which `coeff_last` implementation is called (sse2/lzcnt/avx2/avx512).
The 8x8 mode is selected at runtime by `countcat == 63` rather than a separate function.
Windows ABI requires restoring the CABAC pointer from a saved register after each call;
Unix64 lets it stay in place.

### 5.7 Coefficient-abs-level context logic (`COEFF_ABS_LEVEL_GT1`)

A macro implements the H.264 level coding decision tree:

* For `abs_level <= 1`, encode decision 0 at context `ctx_level + coeff_abs_level1_ctx[k]`,
  then advance the "node context" index through `coeff_abs_level_transition[k]`.
* For `abs_level > 1`, encode decision 1 at that context, then encode the unary prefix at
  context `ctx_level + coeff_abs_levelgt1_ctx[k]` using `cabac_transition_unary` and
  `cabac_size_unary` tables with `k = min(abs_level-1, 14)`.
* For `abs_level >= 15`, add the Exp-Golomb suffix cost: the code computes
  `ilog2(abs_level - 14) + 1` via `bsr`/`lzcnt` and adds `2*(that) + 1` bits.
* A "node context init" form starts from context index 1 (or 5 for the >1 table) without
  consulting the transition table, used for the first (last) coefficient.

This logic is duplicated in the trellis cost estimator (see `quant.h.md` §5.9), so bit-cost
estimates and the actual encoder must agree exactly for RD decisions to be consistent.

### 5.8 Context ingestion and renormalization helpers

`LOAD_GLOBAL` handles position-independent addressing of rodata: on 32-bit x86 it computes a
byte load directly; on 64-bit it uses a base register pointing at the code section with a
section-relative displacement, optionally adding a scaled index. `COEFF_LAST` loads a 32-bit
relative pointer from the dispatch table and calls it. `LOAD_DCTCOEF` selects word vs dword
load based on `HIGH_BIT_DEPTH`. `ABS_DCTCOEFS` computes absolute values into the stack copy
with the widest available vector, handling the case where the vector is half or quarter the
coefficient count.

### 5.9 8-bit vs 10-bit differences

* Coefficient loads: `movsx word` (8-bit) vs full dword (10-bit).
* Absolute-value macros use `ABSW`/`pabsw` vs `ABSD`/`pabsd`.
* The AVX512 RD instantiation uses YMM for 8-bit and ZMM for 10-bit; the non-RD AVX512
  variant exists for both.
* Context tables and entropy arithmetic are identical; only coefficient width changes.

## 6. Dependencies

* `x86inc.asm`/`x86util.asm` — `cglobal`, `INIT_*`, `DECLARE_REG_TMP`, global symbol mangling.
* `common/cabac.c` and `common/cabac.h` — C reference, state layout, `cabac_encode_ue_bypass`.
* `common/tables.c` — entropy, range-LPS, transition, renorm-shift, context-offset tables.
* `common/x86/quant-a.asm` / `trellis-64.asm` — `coeff_last_*` dispatch targets.
* `encoder/cabac.c` — installs these function pointers via `x264_cabac_init`.

## 7. Constants and tables

* `coeff_abs_level1_ctx = {1,2,3,4,0,0,0,0}`,
  `coeff_abs_levelgt1_ctx = {5,5,5,5,6,7,8,9}`,
  `coeff_abs_level_transition = {1,2,3,3,4,5,6,7, 4,4,4,4,5,6,7,7}` — context evolution as
  more nonzero levels are encountered.
* `cabac_range_lps[64][4]`, `cabac_transition[128][2]`, `cabac_renorm_shift[64]`,
  `cabac_entropy[128]` (fixed-point `-log2(p)` in 1/256 units), `cabac_size_unary`,
  `cabac_transition_unary`.
* Level escape threshold 15; unary prefix capped at 14; Exp-Golomb suffix cost `2*k+1`.
* Queue offset -8; emission extraction offsets +10/-18; initial range 0x01FE; initial queue -9.

## 8. Performance notes

CABAC primitives are called millions of times per frame; the residual kernels dominate at
high bitrates.

**Concrete SIMD speed opportunities (referenced):**
1. *Wider vectors.* `ABS_DCTCOEFS` already instantiates at 1x, 2x, and 4x vector width; a
   clean implementation should template absolute-value collection over vector width so
   AVX512 can consume a whole 8x8 block in one pass. The ZMM RD variant is the model.
2. *Prefix/scan patterns.* The level-prefix decision loop is essentially a unary count;
   on AVX512 the unary transition table could be replaced by a masked compare plus a
   `popcnt`/`lzcnt` prefix, eliminating the repeated table lookups. The C `cabac_encode_ue_bypass`
   already uses a leading-zero count — mirror that in SIMD where possible.
3. *Branch removal.* BMI2 `shlx`/`shrx`/`bzhi`/`lzcnt` remove the variable-shift mask
   construction in emission and renormalization (`cabac_encode_*_bmi2`). The decision kernel
   uses `cmovne` instead of an MPS/LPS branch; preserve that shape.
4. *Interprocedural register allocation.* The reference keeps `cb` and state pointers live
   across the significance loop instead of reloading arguments; a clean implementation with
   inlining or a dedicated internal calling convention should do the same.
5. *Table locality.* Pack the 14-entry `coeff_last` dispatch table into a 64-byte-aligned
   block (as done) so one cache line covers it; keep entropy tables in rodata near each other.

## 9. References

* ITU-T H.264 / ISO IEC 14496-10, CABAC binary arithmetic coding and residual syntax.
* x264 public source: `common/cabac.c`, `common/cabac.h`, `common/tables.c`,
  `common/x86/cabac-a.asm`, `encoder/cabac.c`, `common/x86/trellis-64.asm`.
* Marpe, Schwarz, Wiegand, "Context-Based Adaptive Binary Arithmetic Coding in the
  H.264/AVC Video Compression Standard" (IEEE TCSVT, 2003).
* Intel Intrinsics Guide: `lzcnt`, `shlx`, `shrx`, `bzhi`, `cmov`, `pabs*`.

## 10. Verification checklist

1. Compare each SIMD primitive against `x264_cabac_encode_*_c` with identical initial state
   over thousands of random decision sequences; compare final `low`, `range`, `queue`,
   `bytes_outstanding`, `p`, emitted byte stream, and `state[]`.
2. Specifically exercise carry propagation: force sequences producing runs of `0xff`,
   including a carry that propagates through more than one outstanding byte.
3. Verify reset/init state: `range = 0x01FE`, `queue = -9`, no bytes emitted for the first
   several decisions.
4. For each residual variant, compare the encoded bitstream and final `state[]` against the C
   residual coder for random coefficient blocks (all-zero, single nonzero, level 1, 2, 14,
   15, large) at every context-block category, `b_interlaced` 0/1, both bit depths.
5. For RD variants, compare `f8_bits_encoded` deltas against the sum of C size-estimator
   calls for the same decisions; verify state arrays are updated identically.
6. Run `checkasm` for register and stack integrity across ABI variants.

## 11. Open questions

* **UNKNOWN**: exact ordering/content of the 14-entry `coeff_last` dispatch table beyond the
  macro text; the list is assembled from `COEFF_LAST_TABLE` invocations and should be
  cross-checked against `common/x86/quant-a.asm` if byte-exact dispatch matters.
* **UNKNOWN**: whether the `_bmi2` `cabac_encode_bypass` is reachable in current builds
  (no header declaration exists for it; it is an internal symbol used by the residual kernel).
* **UNKNOWN**: precise numerical effect of the `+10/-18` queue offsets under all carry
  conditions — derived from the -8 storage offset but not independently verified here.
* Performance figures are qualitative; no profiling data available.
