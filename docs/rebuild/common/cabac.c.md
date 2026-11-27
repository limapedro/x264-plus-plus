# cabac.c — dirty-side behavioural specification

Source path: `common/cabac.c`
Role: implements the H.264 context-adaptive binary arithmetic coder (CABAC) encoder engine: context initialisation, binary arithmetic coding with renormalisation, bypass coding, Exp-Golomb bypass coding, termination, and final flushing.

> Dirty-side behavioural specification. Describes observable behaviour and structure only; no source text is reproduced.

## 1. Header / identity

This file contains the state machine of the CABAC arithmetic encoder. It does not decide *which* syntax elements to code — that is done by the slice writer — but it provides the primitives those decisions call: encode a context-modelled binary decision, encode a bypass binary decision, encode an Exp-Golomb value in bypass mode, encode a terminating bin, and terminate the slice bitstream. It also precomputes the initial context states from the standard's per-syntax-element tables and copies them into each encoder instance at slice start.

## 2. Purpose

CABAC replaces fixed-length VLC codes with adaptive binary arithmetic coding. For each binary decision ("bin"), a context model supplies a probability estimate for the more-probable symbol (MPS). The engine subdivides the current arithmetic interval accordingly, tracks an output code value, and emits bytes whenever the interval becomes too small (renormalisation). Contexts adapt after every decision, so the same syntax element becomes cheaper as its statistics stabilise. Bins known to be equiprobable skip context modelling and use a fixed 50/50 split (bypass). This file implements all of that machinery, plus the special rule that a run of pending `0xff` bytes must be resolved by carry propagation when a non-`ff` byte finally appears.

## 3. Public interface

### `x264_cabac_init( x264_t *h )` → `void`

Precomputes all context-state bytes for every slice type, QP, and CABAC model into a static cache. Must be called once before any slice is encoded. Uses 4 slice-type slots (I, and three PB models), `QP_MAX_SPEC+1` QP slots, and up to 1024 contexts each.

### `x264_cabac_context_init( x264_t *h, x264_cabac_t *cb, int i_slice_type, int i_qp, int i_model )` → `void`

Loads the precomputed context array selected by slice type, QP and model into `cb->state`. For I slices, `i_model` is ignored and table slot 0 is used. For P/B slices, slot `i_model + 1` is used (models 0, 1, 2). Copies `CHROMA444 ? 1024 : 460` bytes.

### `x264_cabac_encode_init_core( x264_cabac_t *cb )` → `void`

Resets only the arithmetic-coder core: interval, code value, output queue and outstanding-byte counter. Does not touch pointers or contexts. Used mid-slice (e.g. after a PCM macroblock) when the byte buffer continues but contexts must survive — UNKNOWN from this file alone whether callers rely on context preservation; the name suggests core-only reset.

### `x264_cabac_encode_init( x264_cabac_t *cb, uint8_t *p_data, uint8_t *p_end )` → `void`

Calls `encode_init_core`, then sets `p_start`, `p`, `p_end` to the given buffer. Starts a new slice.

### `x264_cabac_encode_decision_c( cb, ctx, b )` → `void`

Coded (regular) binary decision. `ctx` is the context index (0–459 for non-4:4:4, up to 1023 for 4:4:4); `b` is the bin value (0 or 1). Updates the interval, adapts the context, renormalises, and emits bytes as needed. An `_asm` variant exists for x86/AArch64; the public macro `x264_cabac_encode_decision` resolves to the assembly version where available. `_noup` is an alias of the same function (no separate "no update" behaviour in the encoder path).

### `x264_cabac_encode_bypass_c( cb, b )` → `void`

Equiprobable binary decision. **The caller passes the bin already negated** relative to the arithmetic meaning: the code adds `b & i_range` to `i_low`, so `b == 1` selects the upper half of the current interval. No context state is touched.

### `x264_cabac_encode_ue_bypass( cb, exp_bits, val )` → `void`

Encodes a value with an Exp-Golomb-like structure entirely in bypass mode: prefix zeros, terminating one, then suffix bits. `exp_bits` controls how many suffix bits are implied by the caller.

### `x264_cabac_encode_terminal_c( cb )` → `void`

Encodes a terminating bin using the fixed small probability `2/range` — the standard's `end_of_slice_segment` / unary termination bin.

### `x264_cabac_encode_flush( h, cb )` → `void`

Finalises the arithmetic interval, forces at least the required number of bytes into the output queue, resolves the queue, injects a frame-dependent tie-break bit, and drains all pending `0xff` bytes. Must be the last CABAC operation of a slice.

### `x264_cabac_pos( cb )` → `int` (inline in `cabac.h`)

Returns the number of CABAC bits produced so far, including bytes still pending: `(p - p_start + i_bytes_outstanding)*8 + i_queue`. Because `i_queue` is stored with an offset of -8, this expression is exact.

### RD-only size helpers (in `cabac.h`)

`x264_cabac_size_decision`, `..._decision2`, `..._decision_noup`, `..._decision_noup2` — update only the context state and a fixed-point bit-cost accumulator (`f8_bits_encoded`), never the bitstream. `decision2`/`noup2` operate directly on a `uint8_t *state` rather than through `cb`, enabling RD trials on a copied context array.

## 4. State and data structures

### `x264_cabac_t` (declared in `cabac.h`)

| Field | Meaning |
|---|---|
| `i_low` | Lower bound / code value of the current arithmetic interval; a signed integer wide enough for 32-bit shifts plus carry headroom. |
| `i_range` | Current interval length; starts at `0x01FE` (510) and stays in `[256, 510]` after renormalisation. |
| `i_queue` | Number of buffered bits above the byte boundary, stored **with an offset of -8** (comment in the header). Negative values mean bits are not yet ready to emit. |
| `i_bytes_outstanding` | Count of `0xff` bytes emitted but not yet resolved; they may still turn into `0x00` after carry propagation. |
| `p_start`, `p`, `p_end` | Output buffer bounds; `p` points at the next byte to be written, i.e. one past the most recently committed byte. |
| `f8_bits_encoded` | 64-bit-aligned fixed-point accumulator for RD bit-cost estimation (8 fractional bits). |
| `state[1024]` | Context state bytes. Only the first 460 are meaningful without 4:4:4, but the array is full size for alignment. |
| `padding[12]` | Alignment padding to 16-byte boundaries for SIMD loads/stores. |

Invariants:

* After every regular decision or termination, `i_range ∈ [256, 510]`.
* `i_queue` is kept in `[-8, ...]` by the -8 storage convention; `cabac_putbyte` only emits when `i_queue >= 0`.
* `p[-1]` is always a valid, already-written byte when carry propagation occurs; the comment notes a slice header always precedes CABAC data, so an early carry cannot underflow the buffer meaningfully.
* `i_bytes_outstanding` counts only unresolved `0xff` bytes; they are contiguous immediately before `p`.

### Static caches

* `cabac_contexts[4][QP_MAX_SPEC+1][1024]` — precomputed packed context bytes. Index 0 is I; indices 1–3 are the three P/B models.
* The source tables (declared in `tables.h`, defined in `tables.c`) are `x264_cabac_context_init_I[1024][2]`, `x264_cabac_context_init_PB[3][1024][2]`, `x264_cabac_range_lps[64][4]`, `x264_cabac_transition[128][2]`, `x264_cabac_renorm_shift[64]`, `x264_cabac_entropy[128]`, and the file-local `bypass_lut[16]`.

Ownership: the static cache is global and read-only after init; each `x264_cabac_t` belongs to a slice encoder and is reused across slices.

## 5. Algorithm / behaviour

### 5.1 Context initialisation (`x264_cabac_init`)

For each of the four table slots and each QP from 0 to `QP_MAX_SPEC`, and for each context index `j < ctx_count` (1024 for 4:4:4, otherwise 460):

1. Read the pair `(m, n)` from the appropriate source table — I table for slot 0, PB model `(slot-1)` for slots 1–3.
2. Compute an intermediate probability state:
   `state = clip3( ((m * qp) >> 4) + n, 1, 126 )`
   This is the standard H.264 formula: `m` scales with QP (arithmetic right shift by 4 is floor division by 16, including for negative products), `n` is the offset, and the result is clamped to `[1,126]` so the two extreme states 0 and 127 are never initial contexts.
3. Pack into one byte:
   `packed = ( min(state, 127 - state) << 1 ) | (state >> 6)`
   The low bit is the MPS: 0 when `state < 64`, 1 when `state >= 64`. The upper 7 bits are the state index, folded around 63 by `min(state, 127-state)` so that a state and its mirror share the same probability index but have opposite MPS bits.
4. Store `packed` in `cabac_contexts[slot][qp][j]`.

`x264_cabac_context_init` then selects the slot (`0` for I slices, `i_model+1` otherwise) and copies `ctx_count` bytes into `cb->state`. No other computation happens per slice — the QP-dependent arithmetic is done once globally.

### 5.2 Core reset

`encode_init_core` sets:

* `i_low = 0`
* `i_range = 0x01FE` (decimal 510) — the standard CABAC initial interval.
* `i_queue = -9` — documented as "the first bit will be shifted away and not written"; with the -8 storage convention this means the effective queue starts at -1, so one leading bit is discarded before the first byte can appear.
* `i_bytes_outstanding = 0`

`encode_init` additionally installs the output pointers, allowing bytes to be appended at `p` and compared against `p_end` by callers.

### 5.3 Byte emission and carry propagation (`cabac_putbyte`)

This is the heart of the output path and is called from renormalisation, bypass, Exp-Golomb bypass and flush.

Condition to emit: `i_queue >= 0`. If false, nothing is written (the bits are still inside `i_low` and not yet aligned to a byte boundary).

Emission steps:

1. Extract the next byte candidate from the high part of the accumulator:
   `out = i_low >> (i_queue + 10)`.
   The `+10` reflects that 8 bits occupy the byte and 2 more are the carry guard bits maintained in `i_low`. Only the low 8 bits of `out` are data; bits above bit 7 are carry.
2. Mask those bits out of the accumulator:
   `i_low &= (0x400 << i_queue) - 1`.
   This keeps the lower `i_queue + 10` bits, i.e. everything except the just-extracted byte.
3. Decrease the queue by 8: `i_queue -= 8`.
4. If `(out & 0xff) == 0xff`, the byte cannot be written yet: increment `i_bytes_outstanding`. Reason: writing `ff` immediately would be ambiguous with a possible future carry; the standard requires delaying a run of `ff` bytes until the next non-`ff` byte determines whether they stay `ff` (no carry) or become `00` (carry).
5. Otherwise resolve the run and write bytes:
   * `carry = out >> 8` (the guard bit above the byte).
   * Add `carry` to the byte already at `p[-1]`. The source comments explain that this cannot carry beyond one byte: any `0xff` bytes were withheld in `i_bytes_outstanding`, so `p[-1]` is known not to be `0xff`; adding one can at most produce `0x100` at that position. It may also write before the beginning of the CABAC data region, which is safe because a slice header precedes it.
   * For each outstanding byte, emit `(carry - 1)` as `uint8_t`. When `carry == 1` this is `0x00` (the withheld `ff` + carry wraps to `00` and propagates onward); when `carry == 0` it is `0xff` (the run stays unchanged).
   * Emit `out & 0xff` as the new byte.
   * Reset `i_bytes_outstanding` to 0.

Observable effect: carry propagation is *deferred* until a non-`ff` byte appears, at which point at most one prior byte is incremented and all pending bytes are resolved in one burst. This is the standard CABAC "0xff delay" rule.

### 5.4 Renormalisation (`cabac_encode_renorm`)

1. Look up `shift = x264_cabac_renorm_shift[i_range >> 3]`. The index `i_range >> 3` is in `[32, 63]` because `i_range ∈ [256, 510]`; the table covers 64 entries.
2. Shift `i_range`, `i_low` left by `shift`; add `shift` to `i_queue`.
3. Call `cabac_putbyte` (possibly multiple times internally via the queue, though one call emits at most one byte in the current design; repeated renormalisations drive repeated emissions).

The shift table has the shape: state range 0 → 6, 1 → 5, 2–3 → 4, 4–7 → 3, 8–15 → 2, 16–31 → 1, 32–63 → 0. Equivalently, for state index `s > 0`, `shift = 5 - floor(log2(s))`, with `s = 0` mapped to 6. This is the number of left shifts needed to bring the LPS range back into the legal `[256,512)` interval.

### 5.5 Regular (context-coded) decision (`x264_cabac_encode_decision_c`)

Given context `i_ctx` and bin `b`:

1. Read packed state `s = cb->state[i_ctx]`.
2. `stateIdx = s >> 1` (0–63), `MPS = s & 1` (0 or 1).
3. Compute the LPS sub-interval width:
   `lps = x264_cabac_range_lps[stateIdx][ (cb->i_range >> 6) - 4 ]`.
   The second index is `(i_range >> 6) - 4`, which for `i_range ∈ [256,510]` gives `i_range>>6 ∈ {4,5,6,7}` → index 0–3. These are the four range buckets `256–319`, `320–383`, `384–447`, `448–511` of H.264 Table 9-11.
4. Set the MPS sub-interval width: `i_range -= lps`.
5. If the bin differs from MPS (`b != MPS`), this is the LPS branch: add the **reduced** range (the MPS width) to `i_low` so the code value moves into the LPS part, then set `i_range = lps`. If the bin equals MPS, leave `i_low` unchanged and keep the reduced range.
6. Adapt the context: `state[i_ctx] = x264_cabac_transition[s][b]`.
7. Renormalise (§5.4), which also emits bytes.

The transition table implements the standard 64-state × 2-MPS-bit state machine. Its structure: an MPS bin moves the state index one step toward 63 (saturating at the most confident odd/even state), while an LPS bin moves it toward 0 and flips the MPS bit exactly when the state index reaches 0. Concretely, even states (MPS = 0) transition on bin 1 to high odd states (e.g. state 2 → 50, state 4 → 50, state 6 → 52), and odd states (MPS = 1) transition on bin 0 to high even states (e.g. state 3 → 51, state 5 → 51, state 7 → 53). The exact 128×2 table is a standard fact from H.264 Table 9-12; its values are not enumerated here.

The `range_lps` table starts at `{2,2,2,2}` for state index 0 (the least confident state gives an almost 50/50 split) and reaches `{128,167,197,240}` for state index 63 (the most confident state gives a very small LPS interval). Values increase monotonically in both dimensions.

### 5.6 Bypass decision (`x264_cabac_encode_bypass_c`)

With the caller's already-negated bin `b`:

1. `i_low <<= 1`
2. `i_low += b & i_range` — when `b` is 0 the code stays in the lower half; when `b` is 1 it moves up by exactly the current range. This is the 50/50 split implemented without any probability table.
3. `i_queue += 1`
4. `cabac_putbyte`

No context state is read or written and no renormalisation occurs (the interval is unchanged). This is much cheaper than a regular decision and is used for sign bits, and for the suffix bits of large coefficients.

### 5.7 Exp-Golomb bypass coding (`x264_cabac_encode_ue_bypass`)

This routine emits an Exp-Golomb-like code using only bypass bins. Its steps:

1. Form `v = val + 2^exp_bits`. The effective magnitude is therefore shifted by the caller-provided exponent.
2. Compute `k = 31 - clz(v)`, the bit length of `v`.
3. Build a composite bit pattern `x = (bypass_lut[k - exp_bits] << exp_bits) + v`. The lookup table supplies precomputed unary-prefix masks so that no per-leading-zero loop is needed. `bypass_lut[0] = -1` (all ones in two's complement) and subsequent entries are sparse masks: `0x2, 0x14, 0x68, 0x1d0, 0x7a0, 0x1f40, 0x7e80, 0x1fd00, ...` up to `0x7ffe8000`. Each entry has a single `1` at the position where the terminating bit of the unary prefix belongs, with trailing zeros left for the suffix.
4. Total number of bypass bins: `K = 2*k + 1 - exp_bits`.
5. Choose the first chunk width `i = ((K - 1) & 7) + 1`, i.e. `K mod 8` with 0 mapped to 8. This makes the final chunk alignment match the byte boundaries used by `cabac_putbyte`.
6. Loop while `K > 0`: subtract the chunk width from `K`, shift the accumulator left by that width, add `((x >> K) & 0xff) * i_range` (each bin contributes either 0 or the current range), add the width to `i_queue`, call `cabac_putbyte`, then set the chunk width to 8 for subsequent iterations.

The effect is a run of `k` zeros, a `1`, and then `k - exp_bits` suffix bits, all coded as equiprobable bins. Processing in chunks of at most eight bits keeps the arithmetic inside a machine word and lines up with the byte emission logic.

(The exact interpretation of `exp_bits` at each call site is caller-dependent; see Open questions.)

### 5.8 Terminal bin (`x264_cabac_encode_terminal_c`)

`i_range -= 2`, then renormalise. This corresponds to the standard termination bin whose LPS interval is exactly 2 units wide; choosing the MPS branch (the overwhelmingly likely one) means taking the `range - 2` part. No context is updated.

### 5.9 Flush (`x264_cabac_encode_flush`)

1. `i_low += i_range - 2` — move the code value to the upper end of the remaining interval, as required by the standard's finalisation rule.
2. `i_low |= 1` — force at least one low bit set so the final byte is never ambiguous.
3. `i_low <<= 9; i_queue += 9` — guarantee at least two output bytes' worth of bits are available in the queue.
4. Call `cabac_putbyte` twice. Because the queue advanced by 9 bits and the storage offset is -8, these two calls commit the final bytes (resolving any pending carry).
5. `i_low <<= -i_queue` — shift the remaining bits into place for one final emission.
6. Inject a pseudo-random tie-break bit: `i_low |= ( (0x35a4e4f5 >> (h->i_frame & 31)) & 1 ) << 10`. The bit depends only on the low five bits of the frame number, so it cycles with period 32. Its purpose is to avoid pathological identical CABAC tails across frames (e.g. in all-skip slices).
7. Set `i_queue = 0` and call `cabac_putbyte` once more.
8. Drain all `i_bytes_outstanding` by writing `0xff` bytes repeatedly. At flush time no future non-`ff` byte can arrive to resolve them, so they are committed as-is.

The minimum final slice length is therefore at least two bytes plus any withheld `ff` run; this satisfies the decoder's requirement for two `0xff`-free terminating bytes in some profiles (verify against the exact standard rule in Open questions).

### 5.10 RD-only size accounting

`x264_cabac_size_decision` reads state `s`, advances it via the transition table, and adds `x264_cabac_entropy[s ^ b]` to `f8_bits_encoded`. Because the low bit of `s` is the MPS, `s ^ b` has even parity when `b == MPS` (cheap bin) and odd parity when `b == LPS` (expensive bin). The entropy table stores `FIX8(-log2(p))`, i.e. an 8-bit fixed-point bit cost. At the extremes it ranges from about 0.0273 bits (most confident MPS) to 5.737 bits (most confident LPS); both entries for state index 63 equal 1.0. The `_noup` variants omit the state update, allowing an RD trial to price a bin without changing the model.

## 6. Dependencies

* **Called by:** the CABAC slice writer (in `encoder.c`/`macroblock.c` analysis paths) for every bin of every syntax element; `x264_cabac_init` once at encoder startup; `x264_cabac_encode_flush` at slice end.
* **Calls:** only the table lookups and `cabac_putbyte`/`cabac_encode_renorm` internally; no external functions.
* **Tables from `tables.c`:** context init tables, `range_lps`, `transition`, `renorm_shift`, `entropy`. The local `bypass_lut` is defined in this file.
* **Configuration:** `CHROMA444`, `QP_MAX_SPEC`, `SLICE_TYPE_I`, `h->param.b_cabac`, `h->i_frame`.
* **Related header:** `cabac.h` declares the public macros that choose `_asm` vs `_c` implementations on x86/AArch64.

## 7. Constants and tables

| Constant / table | Value / shape | Why |
|---|---|---|
| Initial `i_range` | `0x01FE` = 510 | Standard CABAC interval `[256,512)` start. |
| Initial `i_queue` | `-9` | One leading bit is consumed by the -8 offset convention and discarded. |
| Carry guard | `0x400 << i_queue` mask, `>> (i_queue+10)` extraction | Two guard bits above the byte absorb carry propagation. |
| `0xff` hold rule | outstanding counter | Required by H.264 to defer ambiguous `ff` bytes. |
| `range_lps` | 64×4 bytes, `{2,2,2,2}` … `{128,167,197,240}` | H.264 Table 9-11; four range buckets. |
| `transition` | 128×2 bytes | H.264 Table 9-12 state machine; MPS advances, LPS retreats and flips. |
| `renorm_shift` | 64 bytes: 6,5,4,4,3×4,2×8,1×16,0×32 | Normalises the interval; `5 - floor(log2(stateIdx))` with zero special-cased. |
| `entropy` | 128 × uint16, FIX8(−log2 p) | RD bit cost; even = MPS, odd = LPS. |
| Context init formula | `clip3(((m*qp)>>4)+n, 1, 126)` | Standard QP-dependent probability initialisation. |
| Packed state formula | `(min(state,127-state)<<1) | (state>>6)` | Fold 7-bit state into 6-bit index + MPS bit. |
| `ctx_count` | 460 (non-444) / 1024 (444) | Number of CABAC contexts in H.264. |
| `bypass_lut` | 16 entries, `-1, 0x2, 0x14, 0x68, 0x1d0, ... 0x7ffe8000` | Unary-prefix masks for Exp-Golomb bypass coding. |
| Flush tie-break | `0x35a4e4f5 >> (frame & 31) & 1` | Period-32 pseudo-random bit; avoids identical tails. |

## 8. Performance notes

Hotness: CABAC encoding is one of the two hottest paths in the encoder (with motion estimation). Every residual coefficient, skip flag, ref index and MV component passes through a decision; bypass coding handles signs and MVD suffixes.

Concrete optimisation opportunities:

1. **Context-state layout.** `state[1024]` is byte-sized and aligned so that a whole group of related contexts fits in one or two cache lines. Keeping the 460 live states contiguous and 16-byte aligned lets SIMD load/store several context bytes at once; the separate `f8_bits_encoded` field is 64-bit aligned for the RD-only path so RD trials never contend with the bitstream state. Public guidance: Intel Optimization Reference Manual, "Cache Optimization" and "Alignment" chapters (https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html).
2. **Branch elimination in regular decisions.** The LPS branch (`b != MPS`) is data-dependent and mispredicts often. It can be rewritten branchlessly by computing `lps`, the MPS width, a mask from the comparison, and selecting `i_low`/`i_range` via masked arithmetic. The source itself notes that special-casing known bin values helped old compilers but hurt modern x86-64 — a reminder to measure. Public discussion: Intel Optimization Reference Manual, "Branch Prediction Optimization"; Agner Fog's "Optimizing subroutines in assembly language", https://www.agner.org/optimize/.
3. **SIMD byte-stream scattering.** The SSE2/AVX2/AVX-512 `cabac_block_residual_*` routines (selected in `bitstream.c`) process groups of coefficients in parallel: compute context state indices, gather states, compute range-LPS values, and scatter packed decision bits into the arithmetic engine with `pshufb`/`vpshufb`, using `lzcnt`/`bsr` for leading zeros. Public references: Intel Intrinsics Guide entries for `_mm_shuffle_epi8`, `_mm256_shuffle_epi8`, `_lzcnt_u32` (https://www.intel.com/content/www/us/en/docs/intrinsics-guide/); H.264 Annex G coefficient coding rules.
4. **Batch renormalisation.** `cabac_putbyte` is called from every decision but usually emits nothing. Accumulating several bins' queue increments before calling the emitter once per byte removes repeated conditionals. The current -8 offset queue design already batches up to 8 bits; widening the batch to 16 or 32 bits with a wider accumulator is a documented SIMD-CABAC technique.
5. **Batching context adaptation.** For regular patterns (e.g. a run of zero coefficients), the resulting state transitions are predictable; applying a precomputed state delta table to multiple contexts in one pass reduces per-bin loads/stores.
6. **Precomputed contexts.** `x264_cabac_init` removes all QP arithmetic from the slice path; keeping the table resident (4 × QP_MAX_SPEC × 460 bytes, a few hundred KiB) trades cache for deterministic per-slice startup. For memory-constrained targets, compute states lazily per slice instead.
7. **Carry resolution locality.** The deferred `ff` run means a burst of stores can occur at a single position; ensuring the output buffer has at least `i_bytes_outstanding + 2` bytes of headroom avoids branchy bounds checks in the hot loop.

## 9. References

* ITU-T Recommendation H.264 (ISO/IEC 14496-10), Annex G — CABAC: Tables 9-11 (rangeLPS), 9-12 (state transitions), 9-1 (context initialisation), 7.3.2.2 (flush). https://www.itu.int/rec/T-REC-H.264
* Marpe, Schwarz, Wiegand, "Context-Based Adaptive Binary Arithmetic Coding in the H.264/AVC Video Compression Standard", IEEE Transactions on Circuits and Systems for Video Technology, 13(7), 2003 — the canonical paper for the state machine and bypass design.
* ISO/IEC 14496-10 (AVC) specification, Annex G. https://www.iso.org/standard/83498.html
* Intel Intrinsics Guide (SIMD shuffles, LZCNT). https://www.intel.com/content/www/us/en/docs/intrinsics-guide/
* Intel® 64 and IA-32 Architectures Optimization Reference Manual. https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html
* Agner Fog, "Optimizing subroutines in assembly language" (branch elimination, microarchitecture). https://www.agner.org/optimize/

## 10. Verification checklist

1. **Initial interval:** after `encode_init`, assert `i_range == 510`, `i_low == 0`, `i_queue == -9`, `i_bytes_outstanding == 0`, and `p == p_start`.
2. **Context init formula:** for several `(qp, table, index)` triples, compute `clip3(((m*qp)>>4)+n,1,126)`, fold as `min(state,127-state)` and extract MPS as `state>=64`; assert the stored byte matches. Check boundary clamping at QP 0 and QP `QP_MAX_SPEC`.
3. **Legal interval invariant:** after a long pseudo-random sequence of decisions, assert `256 <= i_range <= 510` after every operation.
4. **Bypass correctness:** construct a sequence of bypass bins implementing a known binary value; after flush, decode with an independent CABAC decoder (e.g. JM or FFmpeg) and assert the recovered value matches.
5. **Carry propagation vector:** force `i_bytes_outstanding` to a known positive value by encoding a run of bins that produce `0xff` bytes, then a bin producing a non-`ff` byte. Assert the emitted bytes are: previous byte + carry, then `carry-1` repeated `outstanding` times, then the new byte.
6. **`ff` completeness:** scan the finalised slice for a `00 00 ff` pattern that should have become `00 00 03 ff` after emulation prevention — this is a joint check with `bitstream.c` and verifies the hold rule did not leak ambiguous bytes.
7. **Flush minimum length:** assert a completely empty slice (only terminal + flush) produces at least the required number of bytes, and that the last byte is not `0xff` unless an outstanding run legitimately remains.
8. **Frame-dependent tie-break:** encode the same minimal slice at frame numbers 0 and 32; assert the outputs are identical, and compare frames 0 and 1 to confirm at least one tail bit differs.
9. **Position accounting:** after each emission, assert `x264_cabac_pos()` equals the count obtained by independently counting bypass + regular bins weighted by their emitted bits.
10. **RD size additivity:** feed the same bin sequence to `x264_cabac_size_decision` starting from an identical context state and to the real encoder; assert `f8_bits_encoded` is within one fixed-point unit of the number of bits actually produced by the arithmetic coder (allowing for the flush overhead).
11. **Cross-implementation bitstream equality:** on a machine with both C and SIMD paths, encode identical macroblock decision sequences through each and assert byte-identical slice payloads.

## 11. Open questions

* The precise role of `exp_bits` in `x264_cabac_encode_ue_bypass` (how many suffix bits are caller-supplied versus emitted by the routine) is caller-dependent; the formula `K = 2*k+1-exp_bits` and the `bypass_lut[k-exp_bits]` indexing are observable, but the semantic mapping to specific syntax elements (MVD vs delta QP vs level prefix) is UNKNOWN from this file.
* Whether `x264_cabac_encode_init_core` is called mid-slice with live contexts and, if so, which callers depend on context preservation — UNKNOWN.
* The exact flush rule (minimum byte count and the significance of the `0x35a4e4f5` constant) is standard-defined but its rationale is not explained in the file — UNKNOWN beyond the observed behaviour.
* The assembly implementations (`_asm`) are assumed bit-identical to the C versions; their internals were not part of the assigned files — UNKNOWN.
* `QP_MAX_SPEC`, `CHROMA444` and the numeric value of `FIX8()` scaling are from other headers; their exact values affect the cache size and entropy resolution — UNKNOWN from this file.
