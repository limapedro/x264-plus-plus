# `common/aarch64/cabac-a.S` — AArch64 CABAC arithmetic encoder kernels

> Dirty-side behavioural specification (reverse-engineered from the reference encoder; describes behaviour, not code).

## 1. Header

- Source path: `common/aarch64/cabac-a.S`
- Role: hand-optimized AArch64 implementations of the three low-level CABAC arithmetic-encoding operations: decision, bypass, and terminate.
- This is a **dirty-side behavioural specification** artifact for the clean-room reimplementation.

## 2. Purpose

CABAC arithmetic encoding dominates the entropy-coding cost of high-quality H.264 encoding. Each binary decision updates an interval (`i_low`, `i_range`), possibly renormalizes, and occasionally emits a byte with carry propagation. This file implements those operations directly in assembly, sharing the byte-emission routine among all three entry points and relying on frozen structure offsets for state access.

## 3. Public interface

Three exported functions, all taking the Cabac state pointer in `x0`:

- `x264_cabac_encode_decision_asm( x264_cabac_t *cb, int i_state_ctx, int b )`
  - `w1` = context index, `w2` = binary decision (0 or 1).
  - Updates the context state, selects LPS/MPS interval, renormalizes, emits bytes as needed.
  - Return: nothing; state updated in place.
- `x264_cabac_encode_bypass_asm( x264_cabac_t *cb, int b )`
  - `w1` = binary value; no context, no state update; single-bit renormalization.
- `x264_cabac_encode_terminal_asm( x264_cabac_t *cb )`
  - Encodes a terminating bin (the final bin of a terminating series), updating the interval and flushing.

An internal shared label performs byte output with carry propagation; it is reached via branches from all three functions, not exported.

## 4. State and data structures

Fields accessed via `asm-offsets.h` (see that specification):
- `i_low` (`0x00`), `i_range` (`0x04`): arithmetic interval.
- `i_queue` (`0x08`): count of bits queued for output; may become negative when more than 8 bits must be emitted during renormalization.
- `i_bytes_outstanding` (`0x0c`): number of pending `0xff` bytes awaiting carry resolution.
- `p` (`0x18`): current output pointer; the byte immediately before it is the target of carry propagation.
- `state` (`0x34`): 1024-byte context state array.

Register allocation convention: `w11` holds `i_low`, `w12` holds `i_range` throughout. General-purpose registers `w3`–`w10`, `w13`–`w14` are temporaries.

External tables (defined in the common C code, referenced by symbol):
- `x264_cabac_range_lps`: range-LPS lookup indexed by context state and interval range.
- `x264_cabac_transition`: state transition lookup indexed by the decision and current state.

## 5. Algorithm / behaviour

### Decision encoding

1. Compute the address of context `i_state_ctx` as `cb + CABAC_STATE + ctx`, load the current state byte.
2. Load `i_range`; obtain `rangeLPS` from `cabac_range_lps` using two indices: the state's upper seven bits (state >> 1, taken from bits 1..7) and `range >> 6`. The table base is deliberately addressed with a `-4` bias so the index can be applied with a scaled-add without a separate subtract.
3. Compute `b ^ (state & 1)` and test its low bit: this decides whether the LPS branch is taken.
4. For LPS: new range = `rangeLPS`, new low = `low + (range - rangeLPS)`. For MPS: new range = `range - rangeLPS`, low unchanged.
5. Update the context state from `cabac_transition[ (b << 1) | state ]` and store it back.
6. Renormalize (shared code below).

The state table is therefore indexed by "decision bit in bit 1, current state in bits 0..7", allowing a single byte load of `(b<<1)|state`.

### Renormalization

1. Compute the leading-zero count of `range` and subtract 23 (the number of leading zeros of a normalized 12-bit range). The result is the shift amount, normally 0 or 1 but possibly larger for unusual ranges.
2. Shift `low` and `range` left by that amount.
3. Add the shift amount to `i_queue`.
4. If the addition result is negative (no carry out of bit 31), store low, range, queue and return.
5. Otherwise enter the byte-emission routine.

Using `clz` this way is exact for the range values CABAC produces; the shift is never negative for valid encoder state.

### Byte emission with carry propagation

1. Read `i_bytes_outstanding`.
2. Compute the output byte as `low >> (queue + 10)` (i.e. bits 10..17 after the pending-bit accounting). Reduce `queue` by 8 for the next iteration.
3. Build a mask of all bits above the output byte and clear them from `low`.
4. If the output byte is `0xff`, increment the outstanding count and skip the immediate write.
5. Otherwise:
   - Compute the carry bit as the sign of the output byte after subtracting 255 (equivalently bit 8 of the byte).
   - Add the carry to the byte immediately preceding `p` (the last emitted byte), then write a `0xff`-adjusted value (`byte - 1`, i.e. the "carry occurred" marker) in its place.
   - If there are outstanding bytes, emit that many `0xff`-minus-carry bytes (loop decrementing the counter).
   - Emit the current byte and advance `p`.
6. Store back `low`, `range`, `queue`, and `bytes_outstanding`.

This implements the standard CABAC "if output == 0xff, postpone; otherwise flush pending 0xff bytes and propagate any carry into the previous byte" rule. UNKNOWN: whether the internal `0xff`-minus-carry write occurs before or after the outstanding-byte loop in the reference's exact byte order — observable equivalence is what matters; a clean-room port must match the emitted bitstream exactly.

### Bypass encoding

1. Load `range`, `low`, `queue`.
2. Compute `low = (low << 1) + (b & range)`. Using `b & range` rather than `b * (range>>8)` is the standard trick: because CABAC keeps `range` in a shifted representation, masking gives the correct contribution with one instruction.
3. Increment `queue`; if it goes non-negative, branch to the shared byte-emission routine; else store `low` and `queue` and return.
4. `range` and state are unchanged.

### Terminal encoding

1. Load `range` and subtract 2.
2. Test whether bit 8 of the reduced range is set. If it is, store the reduced range and return without renormalizing (the termination path uses a partial-renorm shortcut).
3. Otherwise: shift `low` and the reduced `range` left by one, increment `queue`, renormalize once, and either go to byte emission or store back.

This encodes the terminating bin with the special rule that the range contribution is `range - 2`; only one renormalization can be needed.

## 6. Dependencies

- Includes `asm.S` (symbol macros) and `asm-offsets.h` (field offsets).
- References the external tables `x264_cabac_range_lps` and `x264_cabac_transition` from the common C code.
- Called by the generic CABAC encoder through the architecture dispatch (outside this directory). The same three operations exist in the C reference; the assembly must be bit-for-bit equivalent.

## 7. Constants and tables

- Range-LPS table: indexed by (state >> 1, range >> 6). Implementation detail: the base pointer is formed with a `-4` bias so the state index can be scaled by 4 and added directly.
- Transition table: indexed by `(b << 1) | state`; yields the next context state.
- Constants: `23` (clz correction), `8` (bits per byte), `10` (output-bit offset), `0xff`, `2` (terminal range subtraction), `1`.

## 8. Performance notes

Extremely hot: one decision per CABAC bin, and H.264 can generate tens of millions of bins per second at high resolution. The implementation is carefully scheduled: loads are issued early, index computation is interleaved with independent work, and the byte-emission path is shared and aligned (`.align 5`) to avoid branch penalties. The register comments (`w11`/`w12` reserved for low/range) are a micro-optimization to avoid spills on the shared path.

SVE/SVE2 tips: there is little to gain from SVE here — CABAC is inherently scalar and serial. SVE predicates could compress the state-table updates for multiple independent contexts, but the interval dependency prevents pipelining across bins. On x86-64, the equivalent implementation uses `bsr`/`lzcnt` for renormalization, `cmov` for MPS/LPS selection, and branchless byte emission; preserve the exact bitstream semantics rather than copying instruction choices. Public references: ITU-T H.264 CABAC specification; x264 CABAC tables; ARM `clz`, `csel`, `stp`, `ldurb` documentation.

## 9. References

- ITU-T H.264 / ISO/IEC 14496-10, Clause 9.3.4 (CABAC arithmetic decoding/encoding process) and context state transition tables.
- ARM Architecture Reference Manual, AArch64 `clz`, `csel`, `cinc`, `stp`, `ldurb`/`sturb`.
- `docs/rebuild/common/aarch64/asm-offsets.h.md`, `asm-offsets.c.md`.

## 10. Verification checklist

- For randomized sequences of decision/bypass/terminal bins and random initial states, the encoded byte stream must match the C reference CABAC bit-for-bit.
- Renormalization shift computed by `clz(range) - 23` must match a reference loop that shifts while `range < 256` (or the equivalent threshold used by the reference encoder) for all valid ranges.
- Byte-emission carry propagation must match on adversarial sequences: many consecutive `0xff` outputs followed by a carry-generating byte.
- State transition table indices must stay within bounds for all 1024 contexts and both bin values.
- Output pointer `p` must not exceed `p_end` in a properly sized output buffer (test with canaries).

## 11. Open questions

- UNKNOWN: exact byte-emission ordering in corner cases with more than one outstanding byte and simultaneous carry (the observable bitstream is well-defined, but the internal sequence of virtual `0xff` writes is not visible to callers).
- UNKNOWN: whether `f8_bits_encoded` is updated by these kernels (no access is visible in the listed implementation; the bit count may be maintained entirely in C).
