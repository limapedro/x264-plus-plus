# bitstream-a.asm — x86 NAL escape emulation-prevention kernel

Source path: `common/x86/bitstream-a.asm` (declared in `common/x86/bitstream.h`).
Role: SIMD implementation of H.264 emulation-prevention byte insertion for NAL payloads.
**Dirty-side behavioural specification** — own words, no source reproduction.

---

## 1. Header

This small module implements one function family, `x264_nal_escape`, at three ISA levels
(MMX2, SSE2, AVX2 on x86-64). It is byte-oriented and bit-depth independent: NAL payloads
are always 8-bit bytes even in 10-bit encoder builds.

## 2. Purpose

H.264 forbids the byte sequences `00 00 00`, `00 00 01`, `00 00 02`, and `00 00 03` inside a
NAL unit, because they would be confused with start codes or emulation-prevention codes.
The encoder must insert an extra `0x03` byte whenever a source byte `<= 3` follows two zero
bytes. This is a sequential, data-dependent operation that would be slow byte-by-byte; SIMD
detects candidate positions 16 or 32 bytes at a time and only enters a scalar insertion loop
when a candidate is found.

## 3. Public interface

`uint8_t *x264_nal_escape_<isa>(uint8_t *dst, uint8_t *src, uint8_t *end)`:

* `dst` — destination buffer where the escaped payload is written;
* `src` — start of the unescaped payload;
* `end` — one past the last source byte;
* returns the destination pointer advanced past the last written byte.

Variants: `mmx2`, `sse2`, `avx2` (AVX2 only when `ARCH_X86_64`). All have identical semantics;
only the vector width and the width of the bitmask register differ.

## 4. State and data structures

* `r0 = dst`, `r1 = src`, `r2 = end` at entry. During the main loop `r1` is transformed into a
  *negative offset* relative to `end`, so that `r1 >= 0` means "past the end" and the loop
  can terminate with a single sign test. `r0` is correspondingly transformed into the
  projected end of the destination assuming no further escapes.
* `r3`/`r4` hold the rolling two-byte history (as a 32-bit value) and the vector mask; for
  AVX2 the mask registers are full 64-bit GPRs, for 16-byte ISAs only the low 32 bits matter.
* Vector registers `m0` hold an all-zero constant; `m1`/`m2` hold overlapping source windows
  used for candidate detection.
* The algorithm reads slightly beyond `end` (up to 3 vector widths in the SIMD probe). The
  caller guarantees the payload buffer is padded with valid bytes, so this is safe; the
  padding bytes are masked out in the scalar loop by the `r1 >= 0` check.
* No stack scratch; all state lives in registers.

## 5. Algorithm / behaviour

### 5.1 Escape rule

For each source byte `S[i]`, the output is:

* insert `0x03` before `S[i]` if `S[i] <= 3` **and** the two previously written source bytes
  were both zero;
* then write `S[i]`.

The first two source bytes are written unconditionally — there cannot be two preceding zeros.
The check uses the *source* history (not the escaped output), and after inserting `0x03` the
rolling history is updated so that the `0x03` itself is treated as a nonzero byte: this is why
the reference ORs `0x0300` into the history after an insertion. Without that step, a run
like `00 00 00 00` would insert an escape before every byte instead of only alternating.

### 5.2 Scalar prologue and alignment

The kernel starts in the scalar escape loop (rather than the vector loop) for two reasons:

* there may be an escape at the very start of the payload, which the vector probe would miss;
* the destination must be 16-byte aligned before entering the aligned SIMD path, so a few
  scalar bytes are emitted until `dst + offset` is aligned.

Each scalar iteration advances the source offset, forms `history = (history << 8) | S[i]`,
and tests `history & 0xfffffc`. If the low 16 bits are zero and the current byte is `<= 3`,
an escape byte is inserted, the source history is patched, and the destination pointer is
advanced by one extra byte. When the destination is aligned and no escape is pending, the
kernel loads two vectors from the source and jumps into the SIMD loop (aligned or unaligned
variant depending on source alignment for the 16-byte case).

### 5.3 SIMD candidate detection

Each SIMD iteration examines `2 * mmsize` source bytes:

* two overlapping vectors are compared against zero, producing per-byte "is zero" masks;
* the masks are shifted and combined into a single bitmask via `pmovmskb`; the bit positions
  correspond to source positions relative to the current window;
* a candidate exists where the mask has two adjacent set bits followed by a byte whose value
  is `<= 3`. Since the value test is done by the scalar loop, the SIMD code conservatively
  treats any bit pattern with two adjacent zeros as a candidate — this is the "false positive"
  case mentioned in the code.

When no candidate bits are set (`(mask & (mask*2+1)) == 0`, i.e. no adjacent pair), the whole
window is copied to the destination with two stores and the loop advances by `2*mmsize`.
This is the fast path: two vector stores per 32 (or 64) bytes with only a few ALU operations.

### 5.4 Escape handling from a vector candidate

When a candidate pair is found:

* a "false positive" test examines the byte just before the window; if it is nonzero, the
  adjacent-zero condition cannot span the boundary, so the loop resumes without scalar work;
* otherwise the bitmask is scanned with `tzcnt` to find the first candidate position, the
  source offset is advanced to that position, the destination history is cleared (the last
  two bytes are known zeros), and control returns to the scalar escape loop;
* after the scalar loop inserts the required escape and the destination becomes aligned again,
  the kernel reloads vectors and re-enters the SIMD loop.

The `tzcnt` scan is what turns the bitmask into the first escape position without a loop over
bits; it is the key "prefix/scan" pattern in this kernel.

### 5.5 Termination and return

The loop terminates when the negative source offset becomes nonnegative (i.e. `src` has passed
`end`). The return value is the projected destination end pointer `r0`, which was kept in sync
with the source offset plus the number of inserted escapes, so no final fixup is needed.

### 5.6 ISA differences

* **MMX2** (`mmsize = 8`): 8-byte mask in a 32-bit GPR; the unaligned loop is used.
* **SSE2** (`mmsize = 16`): 16-byte mask; both aligned and unaligned source loops exist,
  selected by testing the source pointer's alignment.
* **AVX2** (`mmsize = 32`): 32-byte mask in a full 64-bit GPR (`k3`/`k4` become `r3`/`r4`
  aliases); only the unaligned source loop is emitted, because the wide load is cheap.
* The false-positive check and scalar loop are shared by all variants via the `NAL_ESCAPE`
  macro; only the mask width and store instruction (`mova` vs `movu`) change.

## 6. Dependencies

* `x86inc.asm` / `x86util.asm` — `cglobal`, `INIT_*`, `mmsize`, `ALIGN`.
* `common/bitstream.c` — C fallback `nal_escape_c` and dispatch in `x264_bitstream_init`;
  `x264_nal_encode()` calls the chosen function pointer after writing the NAL header.
* `common/bitstream.h` — declaration and `x264_bsf_t` function-pointer table.

## 7. Constants and tables

* `0xfffffc` — test mask: low 16 bits must be zero (two preceding zeros) and bits 2..23
  must be zero (current byte `<= 3`).
* `0x0300` — patches the rolling history after an inserted escape so the inserted `0x03`
  counts as a nonzero byte.
* `mmsize` (8/16/32) — vector width; loop advances `2*mmsize` bytes per iteration.
* No lookup tables; the kernel relies entirely on comparison masks and bit scans.

## 8. Performance notes

NAL escaping runs once per NAL unit (slice header + payload). It is not usually top-hot, but
it is a serial dependency at the end of slice encoding and can dominate for slices with many
zero bytes (e.g. all-skip frames), where escapes are frequent.

**Concrete SIMD speed opportunities (referenced):**
1. *Wider vectors.* The AVX2 variant doubles throughput over SSE2 by using a 32-byte mask;
   an AVX512 version could use opmask compares and `vptestmb` to detect adjacent zeros in one
   instruction, with `kmov`+`tzcnt` for the first candidate.
2. *Prefix/scan patterns.* The current implementation uses `tzcnt` on a `pmovmskb` result to
   locate the first candidate — this is already the optimal "find first set" pattern. A clean
   implementation should preserve it; avoid a byte-at-a-time rescan of the window.
3. *Branch removal.* The aligned/unaligned split and the false-positive check are the only
   branches. Predicating the scalar loop entry with a mask and using `cmov` for the pointer
   update can remove one branch; the reference's `setnz`/`xor`/`jnz` sequence for the false
   positive is already branchless apart from the final jump.
4. *Load/store overlap.* Reading two overlapping windows (`[src]` and `[src+mmsize]`) lets one
   comparison pair cover the boundary between windows; preserve this overlap when widening.
5. *False-positive filtering.* The extra check of the byte before the window avoids entering
   the scalar loop for patterns that only look like adjacent zeros; keep this cheap check,
   since false positives otherwise cost a full loop transition.

## 9. References

* ITU-T H.264 recommendation, clause on NAL unit syntax and emulation prevention (7.4.1).
* x264 public source: `common/x86/bitstream-a.asm`, `common/bitstream.c`,
  `common/x86/bitstream.h`.
* Intel Intrinsics Guide: `pmovmskb`, `pcmpeqb`, `tzcnt`, `mova`/`movu`.

## 10. Verification checklist

1. Compare output buffer and returned pointer against the C `nal_escape_c` for randomized
   payloads, including: empty payload, single byte, all zeros, all `0x03`, alternating
   `00 00 01`, long runs of zeros, and values at the `<=3` boundary.
2. Test all three destination alignments (0, 8, 16 mod 16) and source offsets spanning the
   aligned/unaligned SIMD loops.
3. Verify the padding-read assumption: place canaries after `end` and confirm they are read
   but never written.
4. Specifically test the case where an inserted `0x03` is immediately followed by more zeros —
   confirm only one escape is inserted per pair (history patch).
5. Run `checkasm`-style ISA equivalence checks across mmx2/sse2/avx2.

## 11. Open questions

* **UNKNOWN**: the exact amount of valid padding the caller guarantees after `end`; the code
  reads up to `3*mmsize` bytes ahead in the SIMD probe.
* **UNKNOWN**: whether the AVX2 variant is dispatched on non-64-bit builds (it is guarded by
  `ARCH_X86_64` in the source).
* The project's `bitstream.h` also declares CABAC residual functions; their semantics are
  documented in `bitstream.h.md` and `cabac-a.asm.md`, not here.
