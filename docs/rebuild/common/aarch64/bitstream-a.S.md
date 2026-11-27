# `common/aarch64/bitstream-a.S` — NEON NAL escaping

> Dirty-side behavioural specification (reverse-engineered from the reference encoder; describes behaviour, not code).

## 1. Header

- Source path: `common/aarch64/bitstream-a.S`
- Role: AArch64/NEON implementation of H.264 emulation-prevention-byte insertion (`x264_nal_escape_neon`).
- This is a **dirty-side behavioural specification** artifact for the clean-room reimplementation.

## 2. Purpose

NAL units must not contain byte sequences that look like start codes. This kernel scans a source buffer and writes an escaped copy into a destination buffer, inserting `0x03` after two zero bytes whenever the next byte is in the range 0–3. It accelerates the common case (no escapes) with 16-byte vector loads/stores and falls back to a byte-wise window check for the rest.

## 3. Public interface

Exported symbol (via the shared naming macros): `x264_nal_escape_neon`.

```
uint8_t *x264_nal_escape_neon( uint8_t *dst, uint8_t *src, uint8_t *end );
```

- `x0` = `dst` (write pointer, advances)
- `x1` = `src` (read pointer, advances)
- `x2` = `end` (one past last valid source byte)
- Return `x0` = final write pointer.

Contract: see `bitstream.h.md`; the kernel writes exactly one output byte per input byte plus one extra `0x03` for each detected pattern.

## 4. State and data structures

Register-resident state:
- A 16-byte vector holding the two most recently processed source bytes in lanes 14 and 15; all other lanes are set to `0xff` so vector comparisons cannot false-positive on them. This is the rolling history.
- A vector constant of `4` (strict upper bound test: byte < 4) and a scalar constant `3` (the escape byte).
- A signed byte counter `x6`, initially `src - end` (negative). The main loop advances it toward zero; when it is still negative at the top of the loop there are bytes remaining.
- A 64-bit mask extracted from a shrunk vector comparison result, used as a quick "any escape candidate in this block?" test.

No memory state, no stack use beyond the link register.

## 5. Algorithm / behaviour

**Initialization.** The history lanes are set to `0xff` (a value that cannot participate in a zero pattern), the comparison mask is set to 4, and the escape byte constant to 3. The remaining-count register is computed as `src - end`. If it is zero, the function returns immediately.

**Bulk test.** While at least 16 bytes remain (`count >= -16` in the signed convention used), the kernel:
1. Loads the next 16 source bytes and advances the source pointer.
2. Builds two shifted versions of the block that bring the two history bytes into alignment with the block's first bytes: one shift supplies the second-previous byte, the other the immediately previous byte.
3. Compares each block byte strictly against 4 (byte < 4), and compares each shifted-history byte against zero. The three predicates are ANDed.
4. Narrows the 16-byte predicate to a 64-bit mask and tests it. If the mask is zero, no byte in the block can complete a `00 00 [0..3]` sequence, so the block is stored verbatim to the destination, the history is replaced with the block's final two bytes, and the loop continues.

If the mask is nonzero, or if fewer than 16 bytes remain, the kernel switches to scalar processing for the current position: it resets the counter to `-16` (so the scalar loop consumes exactly the current 16-byte block plus subsequent bytes) and rewinds the destination/source positions appropriately by re-reading from the current source pointer.

**Scalar loop.** For each source byte:
1. Compose a 24-bit window from the two history bytes and the current byte.
2. Compare the window against 3 using an unsigned "greater than" test. If the window is ≤ 3, emit the escape byte `0x03` and update the history to include it.
3. Emit the current byte and shift the history by one byte.
4. Increment the counter; loop while it remains negative.

**Loop bookkeeping.** After the scalar pass consumes a block, the kernel recomputes the remaining count from the source and end pointers, updates the history lanes from the low bits of the composed window, and branches back to the bulk test. This lets the function alternate between vector and scalar modes as escape candidates appear.

**Termination.** When the recomputed count is zero (or positive in the signed convention), the function returns with `dst` advanced past all emitted bytes.

Rounding/saturation: none (byte copy and insertion only). Ordering: output bytes are written strictly in input order; an inserted escape always precedes the byte that triggered it.

## 6. Dependencies

- Includes `asm.S` for symbol naming and function macros.
- Called from common bitstream code (dispatch outside this directory; see `bitstream.h.md`).
- No calls to other functions in this directory.

## 7. Constants and tables

- `0xff`: filler for unused history lanes.
- `4`: strict upper bound for the "byte < 4" vector test.
- `3`: the escape byte itself and the comparison bound for the 24-bit scalar window.
- No lookup tables.

## 8. Performance notes

This is one of the lighter SIMD kernels. The bulk path processes 16 bytes per iteration with two loads, three compares, two ANDs, one narrowing, one branch, and one store — very cheap. Escapes are rare in already-compressed data, so the scalar path is exceptional. A clean-room AVX2 port can use the same approach with 32-byte blocks; a useful tip is to compute the candidate mask with `_mm256_cmpeq_epi8` against zero plus a `_mm256_cmpgt_epi8` against 3, then use a movemask to branch. SVE note: predicated compress-store could remove the need for a scalar fallback, but insertion changes the output length and thus needs a two-pass or prefix-sum approach; UNKNOWN whether that pays off. Public references: H.264 Annex B; ARM NEON comparison/extract/narrow documentation.

## 9. References

- ITU-T H.264 / ISO/IEC 14496-10, Annex B (start-code emulation prevention).
- ARM Architecture Reference Manual, NEON `cmhi`, `cmeq`, `and`, `shrn`, `ext`, `ld1`, `st1`.
- `docs/rebuild/common/aarch64/bitstream.h.md`.

## 10. Verification checklist

- Compare against a scalar C reference on random buffers, long zero runs, buffers beginning/ending with `00 00 01`, and every trigger value 0–3.
- Verify the returned pointer equals `dst + source_length + inserted_count`.
- Verify that the source is not modified and that no byte beyond `end` is read (use guard pages or canaries in a test harness).
- Verify the fast path is actually taken on all-nonzero data (performance counter or breakpoint).
- Check zero-length input returns `dst` unchanged.

## 11. Open questions

- UNKNOWN: exact behaviour for overlapping source and destination buffers.
- UNKNOWN: whether the caller guarantees any alignment for `src`/`dst` (the kernel uses non-alignment-requiring NEON loads/stores, so it works regardless).
