# bitstream.h — dirty-side behavioural specification

Source path: `common/bitstream.h`
Role: defines the CAVLC/syntax-element bit writer (`bs_t`), its Exp-Golomb helpers and static size tables, plus the dispatch structure filled in by `bitstream.c`.

> Dirty-side behavioural specification. Describes observable behaviour and structure only; no source text is reproduced.

## 1. Header / identity

This header is the interface for all *non-CABAC* bitstream emission: slice headers, picture parameter sets, sequence parameter sets, and CAVLC residual coding. CABAC data is written by a different engine (`cabac.h`) into a separate buffer. The header also declares the level/run tables used by CAVLC rate estimation and the function-pointer table shared between the bitstream and CABAC modules.

## 2. Purpose

H.264 syntax elements are variable-length bit strings, not byte-aligned fields. This header provides:

* a stateful accumulator that gathers bits into machine words and spills whole words to memory only when necessary;
* explicit alignment/flushing operations for RBSP trailing bits and for switching between 32-bit-aligned and unaligned writing;
* Exp-Golomb encoders for unsigned, signed, and truncated values, plus matching *size-only* functions used by rate–distortion decision making without emitting bits;
* static lookup tables that make Golomb length computation a single array access for common values;
* a dispatch structure for CPU-specialised byte stuffing and CABAC residual scattering.

## 3. Public interface

### Types

* `vlc_large_t` — three fields: a 16-bit code (`i_bits`), a byte count (`i_size`), and an index of the next-level table to consult (`i_next`). Used for multi-level VLC decoding/encoding of long level codes.
* `bs_t` — writer state; fields:
  * `p_start` — beginning of the backing buffer (always 32-bit aligned by construction);
  * `p` — current word cursor;
  * `p_end` — end of the buffer (informational; the inline writers do not bounds-check);
  * `cur_bits` — a machine-word accumulator holding bits not yet committed to memory, left-justified so that the next bit to write goes into the low end after shifting;
  * `i_left` — number of free bit positions remaining in `cur_bits` before a spill is required;
  * `i_bits_encoded` — RD-only counter of bits written.
* `x264_run_level_t` — holds a `last` index, a `mask`, and an aligned 18-entry coefficient array; used by CAVLC run/level analysis.
* `x264_bitstream_function_t` — four pointers: `nal_escape`, `cabac_block_residual_internal`, `cabac_block_residual_rd_internal`, `cabac_block_residual_8x8_rd_internal`. Contracts are in `bitstream.c.md`.

### Constants and extern tables

* `LEVEL_TABLE_SIZE = 128`.
* `x264_level_token[7][128]` — array of `vlc_large_t`, seven level-code table levels.
* `x264_run_before[1<<16]` — `uint32_t` table mapping a 16-bit run/level pattern to a packed `(code, size)` value; the comment in the header states that the longest zero-run code sequence is 25 bits, leaving 5 bits for the size field in a 32-bit word.

### Functions (all `static inline`)

| Function | Contract |
|---|---|
| `bs_init(s, data, len)` | Initialise a writer over `[data, data+len)`. Aligns `p_start`/`p` down to a 4-byte boundary by an offset computed from the pointer's low two bits; sets `i_left` to `(WORD_SIZE - offset)*8`; preloads and right-shifts the containing word so that writing can continue immediately after the unaligned start without overwriting preceding bytes. |
| `bs_pos(s)` | Returns the current bit position relative to `p_start`: `8*(p - p_start) + WORD_SIZE*8 - i_left`. The second term counts bits still buffered. |
| `bs_flush(s)` | Commits all remaining buffered bits as one machine word, advances `p` by `WORD_SIZE - (i_left>>3)` bytes, resets `i_left` to a full word. Result is no longer 32-bit aligned. |
| `bs_realign(s)` | Inverse of flush: if `p` is not word-aligned, round it down, recompute `i_left`, and reload the word so subsequent writes append after the already-written bits. If already aligned, does nothing. |
| `bs_write(s, count, bits)` | Append the low `count` bits of `bits`, most significant of those first, to the stream. Spills a 32-bit word when the accumulator fills. Two code paths exist for 64-bit and 32-bit `WORD_SIZE`; both produce identical bytes. |
| `bs_write32(s, bits)` | Emits exactly 32 bits as two 16-bit writes. Exists to remove a branch from the common `bs_write` path; documented as only used in slice headers because Golomb codes never have even total length. |
| `bs_write1(s, bit)` | Single-bit fast path: shift accumulator by one, OR in the bit, decrement `i_left`, and spill exactly when `i_left` reaches `WORD_SIZE*8-32` (i.e. when 32 bits have accumulated). |
| `bs_align_0(s)` | Pad with zero bits up to the next byte boundary (`i_left & 7` bits), then flush. |
| `bs_align_1(s)` | Pad with one bits up to the next byte boundary, then flush. |
| `bs_align_10(s)` | RBSP trailing bits: if there are remaining bits in the current byte, write a single `1` followed by zeros; then flush. This is the standard `rbsp_trailing_bits()` pattern. |
| `bs_write_ue(s, val)` | Unsigned Exp-Golomb for values below 255: writes `val+1` with a length taken from `x264_ue_size_tab[val+1]`. |
| `bs_write_ue_big(s, val)` | Unsigned Exp-Golomb for arbitrary values: computes the total code length by a two-step magnitude reduction plus table lookup, then writes `size>>1` zero bits followed by `val+1` in `(size>>1)+1` bits. |
| `bs_write_se(s, val)` | Signed Exp-Golomb via the mapping `code = 2*val-1` for positive `val`, `code = -2*val` for non-positive `val`; length from the same table. |
| `bs_write_te(s, x, val)` | Truncated (if `x == 1`) or unsigned Exp-Golomb. For `x == 1` writes the complement of `val` as a single bit (`1 ^ val`). |
| `bs_rbsp_trailing(s)` | Write one `1` bit, then zero-pad to the byte boundary (no word flush). |
| `bs_size_ue / bs_size_ue_big / bs_size_se / bs_size_te` | Size-only counterparts; return the bit length without touching the writer. |

## 4. State and data structures

Invariants of `bs_t`:

* `p_start` is the 4-byte-aligned base; the actual user data starts at `p_start + offset`, where `offset = (intptr_t)data & 3` captured at init.
* `cur_bits` is left-justified: on a little-endian machine a full accumulator holds the next 32 output bytes in descending significance; `endian_fix`/`endian_fix32` normalise before storing. On big-endian machines the store expression differs but the emitted byte order is identical.
* `i_left ∈ [0, WORD_SIZE*8]`; after a complete word spill it is reset to `WORD_SIZE*8` (64-bit) or 32 (32-bit path).
* `bs_pos` is monotonic and exact even while bits are buffered.
* Writers do **not** check `p_end`; callers must size buffers conservatively.

Ownership: the backing buffer belongs to the caller (typically a NAL payload buffer). `bs_t` is a stack-local cursor with no dynamic allocation. After `bs_flush` the writer is in an unaligned state; `bs_realign` must be used before resuming aligned-style word writes.

`x264_bitstream_function_t` is owned by `x264_t::bsf` and populated once by `x264_bitstream_init`.

## 5. Algorithm / behaviour

### 5.1 Word accumulator model

Bits are appended from most significant to least significant within the emitted byte stream. On 64-bit builds, `cur_bits` is a 64-bit register. A write of `n` bits performs `cur_bits = (cur_bits << n) | bits` and `i_left -= n`. When `i_left <= 32`, the top 32 bits are committed with a 32-bit store and `i_left` is increased by 32 while `p` advances by four bytes. The store uses `cur_bits >> (32 - i_left)` on big-endian targets and an endian-corrected `cur_bits << i_left` on little-endian targets; both yield the same byte sequence. This design keeps at most 32 bits pending, so the working set is one or two registers.

On 32-bit builds the branch is inverted: if the incoming width is strictly smaller than the remaining space, the simple shift/OR path is taken; otherwise the code first fills the accumulator to exactly 32 bits, emits one word, and then starts a new accumulator with the surplus bits of the input. The surplus is `i_count - old_i_left`, and the new `i_left` is `32 - surplus`.

`bs_write1` is a specialisation for one-bit writes with a spill exactly when 32 bits have accumulated, avoiding the general `i_left <= 32` comparison and any shift-by-variable for the width.

### 5.2 Alignment and flushing

* `bs_flush` writes the still-buffered bits as a single machine word, left-shifted so that they occupy the high `WORD_SIZE*8 - i_left` bits. `p` advances by `WORD_SIZE - (i_left>>3)`, i.e. by the number of whole bytes that were buffered. `i_left` is set back to a full word. The byte stream is left at an arbitrary (not necessarily 32-bit) boundary.
* `bs_realign` recovers word alignment by moving `p` down to the previous 4-byte boundary, recomputing available bits as `(WORD_SIZE - offset)*8`, and reloading the word from memory shifted so that previously written bits are preserved. This allows a flush followed by more writes to resume without corrupting the tail.
* `bs_align_0` pads with `i_left & 7` zero bits and flushes; `bs_align_1` pads with `i_left & 7` one bits; `bs_align_10` writes a single `1` at position `i_left-1` within the current byte (provided there are outstanding bits) and then flushes. `bs_rbsp_trailing` writes one `1` and then zero-pads to the byte boundary **without** flushing, which is correct when the caller will continue writing in the same word-aligned regime.

### 5.3 Exp-Golomb coding

Unsigned code `ue(v)` encodes `v+1` using the standard prefix: let `L = floor(log2(v+1))`; emit `L` zero bits, then the `L+1` low bits of `v+1`. The size table implements this without logarithms: index `v+1` maps directly to `2*floor(log2(v+1))+1`. Its structure is:

| `v+1` range | code length |
|---|---|
| 1–2 | 1, 3 |
| 3–6 | 5 |
| 7–14 | 7 |
| 15–30 | 9 |
| 31–62 | 11 |
| 63–126 | 13 |
| 127–254 | 15 |

(The table is 256 entries; entries beyond 254 are not reached by `bs_write_ue`, which is restricted to values `< 255`.)

`bs_write_ue_big` handles larger values by conditionally shifting the incremented value right by 16 and then by 8, adding 32 and 16 to the accumulated length respectively, then indexing the table with the remaining byte-sized value. The final write emits `L` zeros and then `v+1` in `L+1` bits.

Signed values map to unsigned code numbers as follows: for `v > 0`, `code = 2v - 1`; for `v <= 0`, `code = -2v`. Equivalently the implementation computes `1 - 2v` and, if that is negative, uses `2v` instead — a branch-reduced formulation of the same mapping.

Truncated values: when the alphabet size `x` is 1, only two symbols exist and the code is the complement of the value in one bit; otherwise ordinary `ue(v)` is used.

Size-only helpers mirror these computations exactly, with a single table lookup for values below 256 and a shifted lookup above. `bs_size_ue_big` treats `val < 255` as the direct table case and otherwise indexes the table with `(val+1)>>8` plus 16.

### 5.4 Level/run tables

`x264_level_token` is a seven-level VLC structure: each `vlc_large_t` gives a code word, its bit length, and an index selecting the next table when the decoded magnitude exceeds the current level's coverage. `x264_run_before[65536]` packs a zero-run code and its length into one 32-bit integer; the header documents that 25 bits suffice for the longest run code, leaving 5 bits for length. These tables are consumed by CAVLC analysis/RD estimation, not by this header's inline writers.

## 6. Dependencies

* Included by `common.h` / encoder and set-up code; used by the slice header writer and CAVLC coefficient writer.
* `bitstream.c` fills the `x264_bitstream_function_t` declared here; `cabac.c` also uses the CABAC residual pointers from that table.
* Depends on platform macros `WORD_SIZE`, `WORDS_BIGENDIAN`, `endian_fix`, `endian_fix32`, `M32`, `ALWAYS_INLINE`, and on `X264_MAX` etc. from the common base headers.
* The Golomb size tables in `tables.c` are *not* the same as the small `x264_ue_size_tab` embedded here.

## 7. Constants and tables

| Item | Value / derivation | Purpose |
|---|---|---|
| `WORD_SIZE` | 8 on 64-bit, 4 on 32-bit | Accumulator width; selects the `bs_write` implementation. |
| `i_left` after init | `(WORD_SIZE - offset)*8` | Space remaining before the first word slot boundary, accounting for a pointer offset of 0–3 bytes. |
| `bs_pos` formula | `8*(p-p_start) + WORD_SIZE*8 - i_left` | Exact emitted-bit count including buffered bits. |
| Word spill triggers | `i_left <= 32` (64-bit `bs_write`), `i_left == WORD_SIZE*8-32` (`bs_write1`) | Thresholds chosen so a 32-bit store is always safe. |
| `x264_ue_size_tab` | 256 bytes, values 1,1,3,3,5×4,7×8,9×16,11×32,13×64,15×... | O(1) Golomb length. |
| `LEVEL_TABLE_SIZE` | 128 | CAVLC level table depth; chosen as a cache/bitrate trade-off (header notes QP18 on Nehalem). |
| `x264_run_before` size | `1<<16` entries | Packs 25-bit run code + 5-bit length in 32 bits. |
| Alignment masks | `i_left & 7` | Bits remaining in the current byte. |

## 8. Performance notes

Hotness: these inline functions execute for every syntax element of every CAVLC slice header and for every coefficient; in CAVLC presets they are among the top call sites, in CABAC presets only the header paths remain active.

Concrete optimisation opportunities:

1. **Branch elimination in bit emission.** `bs_write32` and `bs_write1` exist specifically to remove the general-width branch from hot paths. Further wins come from specialising writes for widths 1, 8, 16 and 32 so the compiler emits fixed shifts and a single store. Public discussion: Intel Optimization Reference Manual, "Branch Prediction Optimization" (https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html).
2. **Batched word spilling.** Accumulate several syntax elements and emit one 32-bit store per word rather than checking after every element; the current `i_left <= 32` design already batches up to 32 bits, but callers can improve further by writing grouped fields (e.g. all ref indices) through a single wide write.
3. **SIMD byte-stream scattering.** CAVLC coefficient output can be produced by SIMD: compute code lengths in a vector, form a prefix-sum of lengths to get per-code byte offsets, then scatter code bytes with `pshufb`/`vpshufb`. This is the same principle used by x264's SIMD CABAC residual scattering routines selected in `bitstream.c`. Public reference: Intel Intrinsics Guide, `_mm_shuffle_epi8`, `_mm256_shuffle_epi8` (https://www.intel.com/content/www/us/en/docs/intrinsics-guide/).
4. **Context-state layout for RD size queries.** `bs_size_*` functions must be side-effect free; keeping only a length accumulator (no `cur_bits`) makes them trivially vectorisable and avoids false dependencies on the writer state. Grouping all size queries for a macroblock into one pass improves ILP.
5. **Table locality.** `x264_ue_size_tab` is 256 bytes and stays in L1. `x264_run_before` is 256 KiB and `x264_level_token` about 3.5 KiB; for high-bitrate CAVLC, consider compressing the run table or reordering by access pattern to reduce L2 pressure. Public cache guidance: Intel Optimization Reference Manual, "Cache Optimization".
6. **Endian handling.** `endian_fix` operations compile to a single `bswap` on x86; keeping them at word granularity (not per byte) is essential. Public reference: Intel Intrinsics Guide, `_bswap32` / `_loadu_si128` plus `_mm_shuffle_epi8`.

## 9. References

* ITU-T Recommendation H.264 (ISO/IEC 14496-10), §9.1 — Exp-Golomb coding `ue(v)`, `se(v)`, `te(v)`; §7.4.1 — RBSP trailing bits. https://www.itu.int/rec/T-REC-H.264
* Intel Intrinsics Guide. https://www.intel.com/content/www/us/en/docs/intrinsics-guide/
* Intel® 64 and IA-32 Architectures Optimization Reference Manual. https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html
* ARM NEON Intrinsics Reference. https://developer.arm.com/architectures/instruction-sets/intrinsics/
* AMD64 Architecture Programmer's Manual, Vol. 1 — endianness and word-store semantics. https://www.amd.com/en/support/tech-docs/processor-manuals

## 10. Verification checklist

1. **Bit position accuracy:** starting from an arbitrary 4-byte-aligned or unaligned pointer, write a known sequence of fields; assert `bs_pos()` after each write equals the sum of field widths, including while bits are still buffered.
2. **Golomb vectors:** verify `ue(0)` → single `1`; `ue(1)` → `010`; `ue(2)` → `011`; `ue(3)` → `00100`; `se(0)` → `1`; `se(1)` → `010`; `se(-1)` → `011`; `te(0)` with `x=1` → `1`; with `x=1`, `te(1)` → `0`.
3. **Big-value Golomb:** for `v = 255, 256, 65535, 2^20`, verify `bs_write_ue_big` output length matches `bs_size_ue_big(v)` and that decoding recovers `v`.
4. **Alignment variants:** for the same pending bits, compare the final byte emitted by `bs_align_0`, `bs_align_1`, and `bs_align_10`. `bs_align_10` must produce a `1` immediately after the last data bit followed by zeros; `bs_align_1` must fill the remainder with ones.
5. **Flush/realign round-trip:** after `bs_flush`, call `bs_realign`, write a further known field, flush, and assert the resulting byte stream equals one produced by writing both fields without flushing.
6. **Endianness:** run the same vector on little-endian and big-endian builds (or emulate the store expressions) and assert identical byte sequences.
7. **Size/write consistency:** for 10 000 pseudo-random values in `[0, 2^24)`, assert `bs_size_se(v) == (length of bs_write_se output)` and similarly for `ue`/`te`.
8. **Buffer boundary:** assert that `bs_pos()` never exceeds `8*len` for a sequence of writes within a declared buffer, and that no store touches memory before `p_start`.
9. **Dispatch table:** verify `x264_bitstream_init` zeroes unused pointers and installs a non-NULL `nal_escape` on every CPU mask.

## 11. Open questions

* The exact contents and derivation of `x264_level_token` and `x264_run_before` are defined in `tables.c`, outside the assigned files — UNKNOWN beyond the size/packing facts stated here.
* `i_bits_encoded` is declared "RD only"; whether it is maintained by any of these inline helpers or solely by the CALAC/CABAC size functions is UNKNOWN from this header.
* The precise numeric thresholds for choosing `bs_write_ue` versus `bs_write_ue_big` at call sites are caller decisions — UNKNOWN here.
* Whether `bs_realign` is ever called on a pointer with offset 0 in practice (it is a no-op then) is caller-dependent — UNKNOWN.
