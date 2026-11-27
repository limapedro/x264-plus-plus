# output/flv_bytestream.c — FLV byte-buffer implementation

*Dirty-side behavioural specification. Reference source: `output/flv_bytestream.c`. Behavioural descriptions only; no source or near-verbatim pseudocode is reproduced.*

## 1. Header

- **Source path:** `output/flv_bytestream.c`
- **Role:** Implements the growing byte buffer and primitive field writers declared in `flv_bytestream.h`.
- **Marker:** dirty-side behavioural specification.

## 2. Purpose

FLV tags have back-patched length fields and AMF metadata, so tags must be assembled in memory before being written. This file provides that assembly buffer: append bytes, emit big-endian fields, patch a length once the size is known, then flush the completed tag to the file. It isolates all reallocation and endian logic from `flv.c`.

## 3. Public interface

Implements exactly the functions declared in `flv_bytestream.h` (see that spec for signatures): `flv_create_writer`, `flv_append_data`, `flv_write_byte`, `flv_flush_data`, `flv_rewrite_amf_be24`, `flv_dbl2int`, `flv_put_byte`, `flv_put_be16/be24/be32/be64`, `flv_put_tag`, `flv_put_amf_string`, `flv_put_amf_double`.

Contracts: `flv_create_writer` returns an allocated writer or NULL; `flv_append_data` returns 0 on success, -1 on realloc failure; `flv_flush_data` returns 0 when there is nothing buffered or when the write fully succeeds, -1 otherwise.

## 4. State and data structures

The only state is the `flv_buffer` object: `data` (dynamic array), `d_cur` (used length), `d_max` (capacity), `fp` (destination stream), `d_total` (bytes flushed so far). Invariants: `d_cur ≤ d_max`; `d_max` is a power of two ≥ 16 while any data is allocated; `d_total` counts only flushed bytes. The writer is allocated with `calloc`, so a fresh buffer is all zero; stdout is represented by the `stdout` pointer and must not be closed by this module.

## 5. Algorithm / behaviour

**Creation.** A writer is zero-allocated; the destination is `stdout` for the filename `"-"` or a newly opened `"wb"` file. Failure to open frees the writer and returns NULL.

**Append.** The required new length is `d_cur + size`. If it exceeds capacity, the code computes the smallest power of two (starting from 16) that is at least the required length, reallocates to that size, and updates capacity; a failed realloc returns -1 without changing the logical contents. The new bytes are copied to `data + d_cur`, and `d_cur` is advanced.

**Single-byte writes.** `flv_put_byte` is a thin wrapper that appends one byte. Big-endian writers are compositions of byte writes: 32-bit emits bytes from most to least significant via four shifts; 64-bit twice calls the 32-bit writer on the high and low halves; 16-bit and 24-bit similarly decompose into two/three bytes. This makes all multi-byte fields endian-correct regardless of host byte order.

**Strings and doubles.** `flv_put_tag` iterates characters until the NUL terminator, appending each. `flv_put_amf_string` computes the string length, emits it as a 16-bit big-endian count, then appends the characters. `flv_put_amf_double` emits the AMF number marker byte followed by the 64-bit big-endian IEEE-754 bit pattern; the bit pattern is produced by a union between `double` and `uint64_t` (a defined type-pun in the relevant toolchains).

**Patching.** `flv_rewrite_amf_be24` writes three bytes at `data + start` from the low 24 bits of `length`, most significant first. No bounds check is performed; callers must supply a valid saved offset.

**Flush.** If nothing is buffered, it returns success immediately. Otherwise it writes `d_cur` bytes with a single `fwrite` of count 1; on full success it adds `d_cur` to `d_total` and resets `d_cur` to zero. A short/failed write returns -1 without resetting, so a retry can be attempted.

## 6. Dependencies

- `output/output.h`, `output/flv_bytestream.h`, `x264cli.h` (`x264_fopen`, `stdout`).
- Standard `stdio`/`stdlib`/`string` facilities.
- Called exclusively by `output/flv.c`.

## 7. Constants and tables

- Initial buffer capacity: 16 bytes; growth factor: 2.
- AMF number marker `0x00` and the FLV/AMF field constants enumerated in `flv_bytestream.h`.
- No other constants.

## 8. Performance notes

- All emission goes through the buffer, so constant factors are low; the geometric growth gives amortised O(1) appends.
- **Reservation sizing:** the doubling policy wastes at most one reallocation layer; if a clean-room writer knows the tag size in advance it can reserve once and skip the doubling entirely.
- **I/O buffering:** one `fwrite` per flushed tag; `d_total` makes absolute metadata offsets cheap to compute.
- **Branch behaviour:** every append checks capacity once; predictable.
- Reference: Adobe FLV specification (tag layout, previous-tag-size field).

## 9. References

- Adobe FLV File Format Specification v1.0; AMF0 specification.
- `output/flv_bytestream.h`, `output/flv.c` (companion specifications).

## 10. Verification checklist

1. Append data in small increments past 16/32/64-byte boundaries; verify the full byte sequence is intact.
2. Verify big-endian emission for representative 16/24/32/64-bit values.
3. Verify AMF number encoding matches the IEEE-754 bits of the double.
4. Patch a 24-bit field and verify only those three bytes change.
5. Verify flush on an empty buffer is a no-op returning 0, and that `d_total` accumulates correctly across flushes.
6. Verify a failed append (forced realloc failure) leaves the buffer usable.

## 11. Open questions

- UNKNOWN: exact behavior when `fwrite` writes partially — the implementation treats any result other than 1 (with count=1) as failure; no partial-retry logic is present.
