# `common/aarch64/bitstream.h` — AArch64 bitstream declarations

> Dirty-side behavioural specification (reverse-engineered from the reference encoder; describes behaviour, not code).

## 1. Header

- Source path: `common/aarch64/bitstream.h`
- Role: declares the single AArch64-accelerated bitstream function, NAL escaping.
- This is a **dirty-side behavioural specification** artifact for the clean-room reimplementation.

## 2. Purpose

Provides the C-visible prototype for the NEON-accelerated emulation-prevention-byte inserter implemented in `bitstream-a.S`. Keeping the declaration in a tiny header lets the generic bitstream code call the accelerated routine through the normal build-time symbol templating mechanism.

## 3. Public interface

One exported function (the macro in the header applies the build's symbol-templating scheme to map the generic name to the architecture-specific one):

```
uint8_t *x264_nal_escape_neon( uint8_t *dst, uint8_t *src, uint8_t *end );
```

Contract:
- `src` points to the first input byte; `end` points one past the last input byte.
- Bytes in `[src, end)` are copied to `dst`, with a `0x03` byte inserted before any byte that would complete a three-byte sequence `00 00 00`, `00 00 01`, `00 00 02`, or `00 00 03`.
- Return value is the pointer one past the last byte written to `dst`.
- The function reads at most `end - src` source bytes (it maintains a two-byte rolling history internally, so earlier bytes influence the decision but are not re-read).
- `src`/`dst` may overlap only in the same way the generic C version permits; the reference kernel assumes distinct buffers for the bulk-copy fast path. This is UNKNOWN for overlapping edge cases.

## 4. State and data structures

No persistent state. The kernel keeps a two-byte "previous bytes" history in a vector register and a byte counter/negative index in a general-purpose register. The caller's buffers are plain byte arrays; no structure or alignment requirement is documented beyond what the 16-byte bulk path implies (the fast path uses aligned-capable 16-byte loads/stores, but ARM's `ld1`/`st1` do not require alignment).

## 5. Algorithm / behaviour

Semantics are those of H.264 emulation prevention: an encoder must ensure that no NAL-unit payload contains a start-code emulation sequence. For each source byte `x[i]`, the encoder looks at the previous two emitted/considered bytes. If the last two bytes were both zero and `x[i] <= 3`, a `0x03` byte is inserted before copying `x[i]`. After insertion, the rolling history becomes `00 03` (or, more precisely, the two most recent written bytes).

The AArch64 implementation operates in two modes:
- **Bulk mode**: while at least sixteen bytes remain, a 16-byte vector is loaded and inspected. A vectorized test determines whether any byte in the block could complete a `00 00 [0..3]` pattern given the incoming history. If none can, the whole 16-byte block is copied verbatim and the history is replaced by the block's final two bytes. This is the common case.
- **Scalar mode**: when fewer than sixteen bytes remain, or when the vector test flags a potential pattern, bytes are processed one at a time with a three-byte window composed from the two history bytes and the current byte. When the window value is at most three, the escape byte is emitted first.

The loop terminates when the processed count reaches `end - src`. The return value advances `dst` by exactly the number of bytes written (source bytes plus inserted escapes).

Edge cases: zero-length input returns `dst` unchanged; a run of many zero bytes emits `00 00 03 00 00 03 ...`; a trailing sequence ending in `00 00 01` at the very end still gets an inserted `0x03` because the decision depends only on the current and preceding bytes, not on what follows.

## 6. Dependencies

- Implemented in `bitstream-a.S`.
- Called from the common bitstream/NAL code when `X264_CPU_NEON` is available (dispatch detail lives outside this directory; UNKNOWN whether the generic code calls it directly via a function pointer or through a macro).
- Depends on the build's symbol templating macro (`x264_template`) to resolve the exported name.

## 7. Constants and tables

No constants or tables are declared in this header. The implementation uses the immediate constant `3` for the escape byte and `0xff`/`4` as vector masks for the bulk test; those are implementation details documented in `bitstream-a.S.md`.

## 8. Performance notes

NAL escaping is moderately hot at high bitrates (every NAL payload passes through it) but far less hot than the transforms and motion compensation. The vector fast path amortizes the pattern test over sixteen bytes; for mostly non-zero data (typical compressed payload) almost every block takes the fast path. SVE/SVE2 optimization tip: compress-store (`st1b` under predicate) could in principle emit only the non-escaped bytes, but the need to insert bytes makes SIMD result compaction awkward; the reference's NEON scalar fallback is likely close to optimal. Public references: H.264 start-code emulation prevention rules; ARM NEON `cmhi`, `cmeq`, `ext`, `movi` documentation.

## 9. References

- ITU-T H.264 / ISO/IEC 14496-10, Annex B start-code emulation prevention.
- ARM NEON intrinsics reference (`vld1q_u8`, `vceqq_u8`, `vextq_u8`, `vst1q_u8`).
- x264 bitstream abstraction (`x264_template` symbol naming).

## 10. Verification checklist

- For random byte arrays, output of this function matches a straightforward C reference that inserts `0x03` after `00 00` whenever the next byte is ≤ 3.
- Empty input returns the original `dst` pointer.
- All four trigger values 0, 1, 2, 3 produce an inserted escape; value 4 does not.
- Long zero runs produce the exact standard byte stream.
- Returned pointer equals `dst + (number of source bytes) + (number of inserted escapes)`.

## 11. Open questions

- UNKNOWN: whether the exported symbol is selected via a function pointer or compile-time macro in the caller.
- UNKNOWN: behaviour for overlapping `src`/`dst` ranges; test before relying on it.
