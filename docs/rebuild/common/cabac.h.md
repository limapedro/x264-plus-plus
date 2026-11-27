# cabac.h — dirty-side behavioural specification

Source path: `common/cabac.h`
Role: declares the CABAC encoder state object, the public primitive functions implemented in `cabac.c`, the C-vs-assembly dispatch macros, and side-effect-free bit-cost estimators used by rate–distortion decisions.

> Dirty-side behavioural specification. Describes observable behaviour and structure only; no source text is reproduced.

## 1. Header / identity

This header is the contract between the CABAC engine (`cabac.c`) and the slice-writing code. It exposes one state structure, a set of exported functions (renamed through `x264_template`), a compile-time dispatch that selects assembly implementations where available, and four inline helpers that price bins without emitting bits.

## 2. Purpose

The header exists to (a) define the exact in-memory layout of the arithmetic coder state so that assembly and C agree, (b) hide the choice of C versus SIMD implementation behind stable macro names, and (c) give RD analysis a way to estimate CABAC bit cost without mutating the real bitstream. The last point is essential: x264 evaluates many coding choices per macroblock, and duplicating the full arithmetic coder for each trial would be prohibitively expensive.

## 3. Public interface

### Type `x264_cabac_t`

Fields and contracts:

| Field | Type | Contract |
|---|---|---|
| `i_low` | `int` | Arithmetic code value / lower interval bound. Modified by every encode primitive. |
| `i_range` | `int` | Current interval length; invariant `[256,510]` after renormalising operations. |
| `i_queue` | `int` | Pending bits **stored with an offset of -8**; see `cabac.c.md` §5.3 for the emission rule. |
| `i_bytes_outstanding` | `int` | Number of withheld `0xff` bytes awaiting carry resolution. |
| `p_start`, `p`, `p_end` | `uint8_t *` | Output buffer start, current cursor, and end. `p` points one past the last committed byte. |
| `f8_bits_encoded` | `int`, 64-byte aligned | Accumulator for RD-only bit cost, 8 fractional bits. Not used by the bitstream path. |
| `state` | `uint8_t[1024]` | Context states. Only indices `0..459` are used without 4:4:4; the array is sized for the 4:4:4 case. |
| `padding` | `uint8_t[12]` | Explicit padding to make the structure a multiple of 16 bytes for SIMD loads. |

### Exported functions

| Name | Contract |
|---|---|
| `x264_cabac_context_init(h, cb, slice_type, qp, model)` | Copy the precomputed context array for the given slice type/model/QP into `cb->state`. I slices ignore `model`; P/B use models 0–2. |
| `x264_cabac_encode_init_core(cb)` | Reset arithmetic core (`i_low`, `i_range`, `i_queue`, `i_bytes_outstanding`) without touching pointers or contexts. |
| `x264_cabac_encode_init(cb, data, end)` | Core reset plus install output pointers; starts a slice. |
| `x264_cabac_encode_decision_c(cb, ctx, b)` | Regular context-coded bin; adapts `state[ctx]` and renormalises. |
| `x264_cabac_encode_decision_asm(cb, ctx, b)` | Assembly equivalent, installed by the dispatcher below. |
| `x264_cabac_encode_bypass_c` / `_asm` | Equiprobable bin; caller passes the bin already negated. No context update. |
| `x264_cabac_encode_terminal_c` / `_asm` | Terminating bin (fixed small LPS interval of 2). |
| `x264_cabac_encode_ue_bypass(cb, exp_bits, val)` | Exp-Golomb-like value coded entirely with bypass bins. |
| `x264_cabac_encode_flush(h, cb)` | Finalise the arithmetic coder; must be the last operation of the slice. |

### Dispatch macros

* On `HAVE_MMX` or `HAVE_AARCH64`: `x264_cabac_encode_decision`, `..._bypass`, `..._terminal` resolve to the `_asm` variants; otherwise to the `_c` variants.
* `x264_cabac_encode_decision_noup` is defined as an alias of `x264_cabac_encode_decision` — in the encoder path there is no separate no-update variant.

### Inline helpers

* `x264_cabac_pos(cb)` → `int`: returns produced bits so far, including bytes still withheld:
  `(cb->p - cb->p_start + cb->i_bytes_outstanding)*8 + cb->i_queue`.
  The `-8` storage offset of `i_queue` is absorbed by this expression, so the result is exact at any point.
* `x264_cabac_size_decision(cb, ctx, b)`: read state `s`, update `state[ctx]` via the transition table, add `entropy[s ^ b]` to `cb->f8_bits_encoded`. **Never touches the bitstream.**
* `x264_cabac_size_decision2(state_ptr, b)`: same, but operates on a bare byte pointer (useful for a copied context array); returns the incremental cost.
* `x264_cabac_size_decision_noup(cb, ctx, b)`: price a bin without adapting the state; updates only the accumulator.
* `x264_cabac_size_decision_noup2(state_ptr, b)`: bare-pointer, no-update variant; returns the cost.

Because the low bit of a packed state byte is the MPS, `s ^ b` selects the even entropy entry (MPS, cheap) when `b == MPS` and the odd entry (LPS, expensive) otherwise.

## 4. State and data structures

Layout / alignment facts:

* `f8_bits_encoded` is annotated `ALIGNED_64`, ensuring a 64-byte boundary within the structure; the comment notes this is needed only for `x264_cabac_size_decision` usage.
* `state` is annotated `ALIGNED_16`; `mv`-related caches in the wider encoder are aligned similarly. `padding[12]` brings the total to a 16-byte multiple.
* The first five fields (`i_low`, `i_range`, `i_queue`, `i_bytes_outstanding`, and the three pointers) are the "core" reset by `encode_init_core`; everything after `f8_bits_encoded` is context or RD state.

Invariants (see `cabac.c.md` for derivations):

* `i_range ∈ [256,510]` after any regular/bypass/terminal decision.
* `i_queue >= 0` is the only condition under which bytes are emitted; otherwise bits stay in `i_low`.
* `i_bytes_outstanding` counts consecutive `0xff` bytes not yet written; they are immediately before `p` in the conceptual stream.
* Context indices are in `[0,459]` for 4:2:0/4:2:2 and `[0,1023]` for 4:4:4. Out-of-range indices are undefined behaviour.
* Bins passed to the size helpers are 0 or 1; the entropy index `s ^ b` stays in `[0,127]` because `s < 128`.

Lifetime: one `x264_cabac_t` per slice encoder, reused across slices; contexts are reloaded at each slice start. RD size helpers may use a temporary copy of `state[]` for speculative coding decisions.

## 5. Algorithm / behaviour

The header itself contains no multi-step algorithms; its inline functions implement two small behaviours:

1. **Position accounting.** `x264_cabac_pos` counts fully written bytes (`p - p_start`), adds withheld `0xff` bytes (each still represents 8 emitted bits conceptually), and adds the pending bit queue. This is the only public way to query CABAC progress and is used for rate control and slice-size decisions.
2. **Cost estimation.** The four size helpers implement the identity
   `cost(bin) = -log2(P(bin | state))` in 8-bit fixed point. With packed state `s`, the MPS is `s & 1` and the state index is `s >> 1`; XOR-ing with the bin yields an even index when the bin is the MPS and an odd index when it is the LPS. The `entropy` table stores the fixed-point costs for both cases. The `_noup` variants deliberately skip the transition-table write so speculative trials are cheap and side-effect-free with respect to the model; the `2` variants bypass the `cb` object entirely so multiple trials can share a copied state array without aliasing the live encoder.

The compile-time dispatch means callers write one function name and get either the portable C implementation or an assembly routine specialised for the target ISA; the two must produce identical bytes. `x264_cabac_encode_decision_noup` being an alias indicates that, in this encoder, the "no update" concept only exists in the RD cost helpers, not in the actual bitstream encoder.

## 6. Dependencies

* Included by `common.h` and by all CABAC slice-writing code.
* Functions are defined in `cabac.c`; assembly variants live in architecture-specific files (`common/x86/cabac-a.asm`, etc.).
* Tables `x264_cabac_transition` and `x264_cabac_entropy` are declared in `tables.h` and defined in `tables.c`.
* `x264_template` provides symbol renaming for multi-library builds; `ALIGNED_*` and `ALWAYS_INLINE` come from the base headers.
* Used together with `bitstream.h` (CAVLC path) and with `mvpred.c` (MVD cache population).

## 7. Constants and tables

| Item | Value / derivation | Role |
|---|---|---|
| `state` array size | 1024 bytes | Maximum context count (4:4:4); 460 used otherwise. |
| `padding` size | 12 bytes | Rounds structure to 16-byte alignment after the 1024-byte state. |
| `f8_bits_encoded` alignment | 64 bytes | Enables aligned SIMD loads in RD-only helpers. |
| `i_queue` offset | -8 | Assembly-friendly representation of pending bits. |
| Entropy index | `state ^ bin` | Even = MPS cost, odd = LPS cost. |
| `x264_cabac_pos` | `(bytes + outstanding)*8 + queue` | Exact bit position including pending bytes. |
| Dispatch condition | `HAVE_MMX` or `HAVE_AARCH64` | Chooses assembly vs C primitives. |

The referenced `range_lps`, `transition`, `renorm_shift`, `entropy` and context-init tables are specified in `cabac.c.md` §7.

## 8. Performance notes

Hotness: every decision primitive is called millions of times per second at high resolution; the size helpers dominate analysis time in RD-enabled presets.

Concrete optimisation opportunities:

1. **Context-state layout.** Keeping `state[1024]` byte-wide and 16-byte aligned lets SIMD code load a group of contexts with one `movdqa`/`ldr q`, compute several `range_lps` lookups, and store results back in one write. Placing `f8_bits_encoded` on its own 64-byte boundary prevents RD accumulator updates from evicting the context cache line. Public reference: Intel Optimization Reference Manual, "Cache Optimization" and "Data Alignment" (https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html).
2. **Branch elimination.** The regular decision's LPS test is a frequent misprediction source. A branchless formulation computes a mask from `b != MPS` and selects the interval updates with masked arithmetic; the assembly implementations already do this. Public references: Agner Fog, "Optimizing subroutines in assembly language" (https://www.agner.org/optimize/); Intel Optimization Reference Manual, "Branch Prediction Optimization".
3. **SIMD byte-stream scattering.** For a block of coefficients, the `cabac_block_residual_*` pointers do context-index generation and bit scattering in parallel using `pshufb`-style gathers/scatters and `lzcnt` for prefix lengths. Public reference: Intel Intrinsics Guide, `_mm_shuffle_epi8`, `_mm256_shuffle_epi8`, `_lzcnt_u32` (https://www.intel.com/content/www/us/en/docs/intrinsics-guide/).
4. **Batching RD trials.** `size_decision2` works on a bare `uint8_t *state`, so an analysis pass can copy only the 460 live context bytes into a small scratch buffer and price an entire candidate macroblock there, then discard it. This avoids touching the live `cb` and keeps speculative state in L1.
5. **Bypass bulk encoding.** When many bypass bins occur consecutively (e.g. MVD suffixes), the queue can be advanced in bulk with one shift and one `cabac_putbyte` per byte instead of one per bin, reducing function-call and branch overhead.
6. **Avoiding false sharing.** With sliced threads, each thread owns a `cb`; the 16-byte structure padding should be preserved when adding fields so two adjacent objects never share a cache line. Public reference: Intel Optimization Reference Manual, "False Sharing" section.

## 9. References

* ITU-T Recommendation H.264 (ISO/IEC 14496-10), Annex G — CABAC context models, transition table and entropy coding. https://www.itu.int/rec/T-REC-H.264
* Marpe, Schwarz, Wiegand, "Context-Based Adaptive Binary Arithmetic Coding in the H.264/AVC Video Compression Standard", IEEE TCSVT 13(7), 2003.
* Intel Intrinsics Guide. https://www.intel.com/content/www/us/en/docs/intrinsics-guide/
* Intel® 64 and IA-32 Architectures Optimization Reference Manual. https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html
* Agner Fog, optimization manuals. https://www.agner.org/optimize/

## 10. Verification checklist

1. **Structure invariants:** after `x264_cabac_encode_init`, assert `i_range == 510`, `i_low == 0`, `i_queue == -9`, `i_bytes_outstanding == 0`, `p == p_start`, and that `state[]` was loaded from the expected table slot.
2. **Dispatch equality:** for identical inputs, verify `_c` and `_asm` primitives produce identical output bytes and identical final `state[]`.
3. **Position accounting:** emit a known sequence of regular and bypass bins; after each emission, compare `x264_cabac_pos()` against an independent bit counter derived from the same decisions.
4. **Size estimator:** for 1000 random `(state, bin)` pairs, assert `size_decision2(&s, b)` equals `entropy[s ^ b]` and that `s` afterwards equals `transition[s][b]`.
5. **No-update variants:** assert `size_decision_noup` leaves `state[ctx]` unchanged while adding the same cost as the updating version.
6. **Context bounds:** with 4:2:0 builds, assert valid operation for indices up to 459; document that indices 460–1023 are unused (not necessarily safe to access).
7. **Flush position:** after `x264_cabac_encode_flush`, assert the position matches the actual byte count of the slice payload produced.
8. **Alignment:** verify at compile/run time that `offsetof(x264_cabac_t, f8_bits_encoded) % 64 == 0` and that `sizeof(x264_cabac_t) % 16 == 0`.

## 11. Open questions

* The exact value of `QP_MAX_SPEC` and the scaling of `FIX8()` are defined elsewhere; they affect the entropy table resolution — UNKNOWN from this header.
* Whether any caller uses `x264_cabac_encode_decision_noup` expecting a genuinely different behaviour from the alias is UNKNOWN; here it is defined as the same symbol.
* Assembly implementations are assumed to preserve the documented structure layout; their ABI details are outside the assigned files — UNKNOWN.
* The rationale for a 64-byte (not 16- or 32-byte) alignment on `f8_bits_encoded` is stated as "for `x264_cabac_size_decision()`" but the exact SIMD instruction requirement is UNKNOWN.
