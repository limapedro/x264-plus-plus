# `common/arm/bitstream.h` — ARM bitstream interface declarations

## 1. Header

- **Source path:** `common/arm/bitstream.h`.
- **Role in one sentence:** declares the ARM/NEON acceleration hook for H.264 NAL-unit emulation-prevention byte insertion.
- **Marker:** Dirty-side behavioural specification; all descriptions are behavioural and written independently of the reference source.


## 2. Purpose

This header is the compile-time bridge between the generic bitstream writer and the ARM-specific assembly routine. It gives the rest of the encoder a single, mangled symbol name to call for the hot "escape a NAL payload" operation, without exposing any assembly details. It exists because the 8-bit ARM port supplies a NEON implementation of an operation that the generic C code otherwise performs byte by byte.

## 3. Public interface

One function is declared:

- `uint8_t *x264_nal_escape_neon( uint8_t *dst, uint8_t *src, uint8_t *end )`

Contract:
- `src` points to the first unescaped payload byte, `end` to one past the last.
- `dst` points to the destination buffer, which must be at least as large as the source plus the worst-case number of inserted bytes.
- The return value is the destination pointer advanced past the last byte written.
- The routine preserves all source bytes, inserting `0x03` where required by the H.264 start-code-emulation rule. It does not write NAL headers or start codes.

The declaration is prefixed with `x264_template(...)`, which expands to the bit-depth-qualified public symbol in high-bit-depth builds and to the plain `x264_nal_escape_neon` symbol in 8-bit builds. This is a naming convention only; the algorithm is identical for both depths.

## 4. State and data structures

The function is stateless. All state is the three pointers passed by the caller. No global tables are read directly by the header; the assembly counterpart uses only constant vectors synthesized with immediate instructions. Ownership of the `dst` buffer stays with the caller; the function never frees or reallocates memory.

The caller typically installs this function pointer into `h->bsf.nal_escape` during CPU-feature dispatch, after verifying that the CPU has NEON. The generic fallback is `nal_escape_c`.

## 5. Algorithm / behaviour

The interface describes only the observable transformation:

1. The first two source bytes are copied verbatim (provided they exist). This is a direct consequence of the rule needing two preceding zero bytes before a candidate byte is escaped.
2. For each further source byte, the byte is copied to the destination. If it is at most `0x03` and the two bytes immediately preceding it in the *destination* stream are both zero, a `0x03` byte is inserted first.
3. Processing stops when `src == end`; the function returns the current destination pointer.

Because the decision uses destination bytes, inserted `0x03` bytes themselves affect subsequent tests: after an insertion the last two destination bytes are `0x00,0x03`, so the next byte is never escaped until two more zeros are emitted.

## 6. Dependencies

- Included by `common/bitstream.c` when ARM support is compiled.
- The implementation lives in `common/arm/bitstream-a.S`.
- Depends on the generic `x264_template` macro from the build configuration.
- The generic reference implementation in `common/bitstream.c` defines the semantics that must be matched byte for byte.

## 7. Constants and tables

The only constant is the emulation-prevention byte `0x03` and the threshold value `3`. Both originate in the H.264 specification's start-code emulation prevention rule. No lookup tables are needed.

## 8. Performance notes

This routine runs once per NAL unit and scans the entire encoded payload, so its cost scales with output size and matters at high bitrates. The byte-at-a-time C version has a dependency chain through the last-two-destination-bytes state. The NEON version processes 16 bytes per iteration using vector compares and a narrowed mask to find candidate positions, then handles those positions with scalar code while keeping the two-byte history in vector registers. For a clean-room rebuild, prioritize a correct scalar implementation first; vectorize only when the scalar path is verified, since a single extra or missing `0x03` corrupts the bitstream. Cache behaviour is effectively linear streaming with little reuse.

## 9. References

- ITU-T H.264 (ISO/IEC 14496-10), section on emulation prevention in NAL unit payloads.
- x264 public source tree, `common/bitstream.c` (generic `nal_escape_c`), used as the behavioural reference.
- ARM Architecture Reference Manual, NEON intrinsics/instruction set, for the vectorized variant.

## 10. Verification checklist

- Empty input (`src == end`) returns `dst` unchanged and writes nothing.
- One- and two-byte inputs are copied verbatim.
- Input `00 00 00` produces `00 00 03 00`.
- Input `00 00 01`, `00 00 02`, `00 00 03` each produce an inserted `0x03`.
- Input `00 00 04` produces no insertion.
- Long runs of zeros produce a `0x03` after every pair of preceding zeros.
- Returned pointer equals the number of bytes written past `dst`.
- Output of the NEON path is byte-identical to the generic scalar path for randomized payloads.

## 11. Open questions

- `UNKNOWN`: whether the NEON implementation is selected at runtime through `x264_cpu_detect` flags in every supported build configuration, or only in specific ARM variants.
- `UNKNOWN`: exact behaviour when `dst` overlaps `src`; the contract does not specify overlap and the generic implementation is not designed for it.
