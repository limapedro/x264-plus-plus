# `common/arm/bitstream-a.S` — NEON NAL escaping

**Dirty-side behavioural specification.** Source path: `common/arm/bitstream-a.S`. Role: ARM/NEON implementation of H.264 emulation-prevention byte insertion.

## 1. Purpose

This file implements the `x264_nal_escape_neon` routine declared in `arm/bitstream.h`. Its job is to scan a NAL payload and insert `0x03` bytes where the byte sequence would otherwise resemble a start code, while preserving all original bytes.

## 2. Public interface

- `uint8_t *x264_nal_escape_neon( uint8_t *dst, uint8_t *src, uint8_t *end )`

ARM EABI argument mapping: `r0 = dst`, `r1 = src`, `r2 = end`; the returned pointer is in `r0`. The routine pushes `r4-r5` and `lr`, and uses NEON registers `q0`..`q3`, `q8`..`q11`, and `d22` for candidate detection.

## 3. State and data structures

The routine maintains a rolling two-byte history of recently written destination bytes. In the fast loop this history is kept in bytes 6 and 7 of `d1`; in the scalar recovery loop it is held in a 32-bit register `r5`. The history is initialized from the first two source bytes, which are always copied without testing.

## 4. Algorithm / behaviour

1. The routine computes the remaining count `src - end` (negative while work remains) in `lr`. If it is zero, it returns immediately.
2. It sets up constant vectors: `q0` filled with `0xFF`, `q8` filled with 4, and `r3 = 3` for the inserted byte.
3. In the vector loop, 16 source bytes are loaded into `q1`. Two shifted views are created with `vext` at offsets 14 and 15, representing "two bytes before" and "one byte before" each candidate position.
4. A mask is built where both predecessor bytes are zero and the current byte is at most 3. This is done by unsigned compares against zero and against 4, combined with bitwise AND, then narrowed from 16-bit lanes to an 8-bit mask with `vshrn`.
5. If any candidate exists, the code falls into a scalar loop: for each byte, it checks whether the current 32-bit rolling history value is at most 3. If so it emits a `0x03` byte and shifts the history. It then emits the source byte and updates the history.
6. If no candidate exists in the current 16-byte block, the whole vector is stored directly and used as the new history, and processing continues with the next block.
7. The loop terminates when `src` reaches `end`, and the final history is written back to `d1` for consistency with the next invocation pattern.

The critical semantic detail is that the history is taken from the destination stream, including inserted bytes, exactly as in the generic scalar implementation.

## 5. Dependencies

- Includes `common/arm/asm.S` for symbol and macro definitions.
- Declared by `common/arm/bitstream.h` and called by `common/bitstream.c`.
- Must match the behaviour of `nal_escape_c` in the same file.

## 6. Constants and tables

- Inserted byte `0x03`, comparison threshold `4`.
- Vector constants: `0xFF` fill and `4` fill.
- `vext` offsets 14 and 15 encode the two predecessor positions.

## 7. Performance notes

The fast path processes 16 bytes per iteration with two shifted vector views and a single narrowed mask, so the common case of no candidates is very cheap. The scalar recovery loop is entered only when a candidate exists; it handles a handful of bytes and then re-enters vector processing. Cache behaviour is purely streaming. For a clean implementation, a byte-at-a-time version is correct and simple; vectorization should be added only after exhaustive equivalence testing because an off-by-one in the predecessor window corrupts the output.

## 8. References

- ITU-T H.264, start-code emulation prevention rule.
- `common/bitstream.c` for the canonical algorithm.
- ARM NEON Programmer's Guide for `vext`, `vcgt`, `vceq`, and `vshrn`.

## 9. Verification checklist

- Empty, one-byte, and two-byte inputs are copied verbatim.
- The sequences `00 00 00`, `00 00 01`, `00 00 02`, `00 00 03` each cause an insertion before the third byte.
- `00 00 04` does not cause an insertion.
- Output is byte-identical to the generic scalar implementation for randomized inputs, including long zero runs and values crossing 16-byte block boundaries.
- The returned pointer equals `dst` plus the number of bytes written.
- Callee-saved registers are preserved.

## 10. Open questions

- `UNKNOWN`: whether the routine is ever called with `dst` overlapping `src`; no overlap contract is documented.
- `UNKNOWN`: behaviour for extremely large payloads beyond the signed-loop range implied by the 32-bit counter.
