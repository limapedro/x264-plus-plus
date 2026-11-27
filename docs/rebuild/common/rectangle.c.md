# `common/rectangle.c` - runtime dispatch table definitions

## 1. Header

Source path: `E:\_x264_dirty_stash\x264-source\common\rectangle.c` (mirrored at `common/rectangle.c`). Role in one sentence: generates the seven specialised rectangle-filling functions for MV, MVD and reference caches and installs them into the three dispatch tables used when dimensions are not compile-time constants. Marker: **dirty-side behavioural specification**.

## 2. Purpose

`rectangle.h` handles constant-size fills inline. When a partition dimension is computed at run time (for example from a macroblock partition type chosen by rate distortion), the inline specialisation cannot be selected at compile time. This file supplies a small indirect-call table so those cases still reach fully specialised store sequences rather than a generic loop.

## 3. Public interface

Three exported arrays (names templated by bit depth):

- `void (*x264_cache_mv_func_table[10])(void *, uint32_t)`
- `void (*x264_cache_mvd_func_table[10])(void *, uint32_t)`
- `void (*x264_cache_ref_func_table[10])(void *, uint32_t)`

Each entry takes a destination pointer produced by `&h->mb.cache.<kind>[list][X264_SCAN8_0 + x + 8*y]` and a value already packed into an unsigned 32-bit word. Each function writes the corresponding rectangle using the element width of its family (4, 2 or 1 bytes).

No other functions are exported; the specialised functions themselves are file-static.

## 4. State and data structures

Each table has ten slots but only seven are populated. The generator emits one function per (family, width, height) pair from the list: 4x4, 2x4, 4x2, 2x2, 2x1, 1x2 and 1x1 (width and height measured in Scan8 cells, not bytes). Slot assignment uses the same index formula as the caller, `width + (height<<1) - 3`, giving indices 9, 7, 5, 3, 1, 2 and 0 respectively; slots 4, 6 and 8 remain null. Each generated function simply forwards to `x264_macroblock_cache_rect(target, width*size, height, size, val)`, where `size` is the family's element width. Because the arguments are compile-time constants inside the generated functions, the inline header's specialisation is fully realised.

## 5. Algorithm / behaviour

The file uses two macro layers: one generates a single named wrapper for a given family, element size, width and height; the second generates all seven wrappers and the table for one family, then is instantiated three times (MV with 4-byte elements, MVD with 2-byte elements, ref with 1-byte elements). The resulting table order is fixed and must match the index formula in `rectangle.h`: a mismatch would silently write the wrong rectangle shape. On failure to match, the null slots are the observable symptom - calling one is undefined behaviour.

The wrapper's `val` parameter is always `uint32_t` even for 8- and 16-bit families; the inline cache writer truncates to the needed width when constructing its replication patterns. Callers must therefore avoid setting high bits that are not meaningful for the family, though the writer naturally ignores them.

### Additional behavioural detail

**Index formula derivation.** The caller computes `width + (height<<1) - 3`. Substituting the seven legal pairs gives: (1,1) -> 0, (2,1) -> 1, (1,2) -> 2, (2,2) -> 3, (4,1) -> 3 (collides with 2x2, so 4x1 is not used), (4,2) -> 5, (2,4) -> 7, (4,4) -> 9. The generator's table order is chosen to match exactly these values; indices 4, 6 and 8 correspond to shapes the encoder never produces, so they are left null. A clean implementation should either reproduce the same sparse table or replace it with a dense mapping, but must never call a null slot.

**Generated function shape.** Each wrapper is a single call to the inline rectangle writer with compile-time constant arguments. This is intentional: it lets the compiler fully unroll the store sequence inside the wrapper while presenting a uniform two-argument function pointer to the caller. The value passed in is already packed by the caller (`M32` of an MV, a 16-bit MVD, or a cast reference index), so the wrapper performs no conversion beyond what the inline writer does.

**Element widths and replication.** For the MV family, four bytes are written per cell and the 32-bit value is replicated into the required store pattern; for MVD, two bytes; for reference indices, one byte. The inline writer builds its patterns by multiplying the value with byte-lane broadcast constants (`0x0101`, `0x01010101`, or a 64-bit duplicate), which is why negative reference indices appear as their unsigned byte value. The store width never exceeds the destination cache array's declared alignment.

**Why the file is separate.** Putting the table definitions in a separate translation unit avoids duplicating seven functions per family in every module that includes `rectangle.h`. It also keeps the always-inline constant path in the header available without emitting out-of-line copies for shapes that never need runtime dispatch. The trade-off is that the header and this file must agree on the index formula and element sizes; they are coupled by convention rather than by a shared macro.

**Testing implications.** Because the table entries are function pointers with no metadata, a test cannot ask a slot what shape it writes; it must call it with a guarded buffer and infer the shape from the bytes changed. Guard bands around the buffer are therefore essential. Comparing against the inline path for each legal shape gives an independent oracle.

## 6. Dependencies

Includes `common.h`, hence `rectangle.h`, `base.h` and the Scan8 layout. Provides the symbols declared as `extern` in `rectangle.h`. Used only by the cache helper macros in `rectangle.h` when `x264_nonconstant_p(width)` or `x264_nonconstant_p(height)` is true.

## 7. Constants and tables

Table sizes: 10 pointers per family, three families. Populated indices and their shapes are listed in section 4. Element sizes: MV 4, MVD 2, ref 1. The three null slot indices are 4, 6 and 8, which correspond to illegal shapes under the index formula (e.g. 3x1 or 2x3).

## 8. Performance notes

- **Dispatch hierarchy:** indirect call only for non-constant shapes; the constant path in `rectangle.h` avoids the call entirely. This is a deliberate speed/robustness trade-off: 21 tiny functions add code size but keep store sequences optimal.
- **Cache layout:** each wrapper writes rows separated by the Scan8 stride; wide stores keep writes inside a cache line for 2/4-byte elements.
- **False sharing / allocation:** none; tables are read-only and functions have no state.
- Public references: compiler documentation on macro-generated specialisation; Intel Optimization Reference Manual on store width.

## 9. References

- `rectangle.h.md` in this directory for the inline writer contract and Scan8 indexing.
- `common.h.md` for `x264_t.mb.cache` layout and alignments.
- Intel Optimization Reference Manual, section on store instructions and write combining.

## 10. Verification checklist

- Iterate all ten slots of each table: indices 0,1,2,3,5,7,9 must be non-null; 4,6,8 must be null.
- For each non-null slot, allocate a guarded destination buffer, call the function with a known pattern value, and verify the rectangle dimensions implied by the index formula are exactly filled and guards untouched.
- Compare table results with the inline `x264_macroblock_cache_rect` path for all seven shapes and all three element widths; bytes must match exactly.
- Verify high bits of the 32-bit value are ignored for the 1- and 2-byte families.
- Confirm templated symbol names differ between 8-bit and 10-bit builds (black-box via `nm` on a static library).

## 11. Open questions

- UNKNOWN: whether any caller relies on the null slots being callable (no such caller is visible in this file); treat as a hard invariant.
- UNKNOWN: the mapping from partition types to the seven legal shapes is defined in macroblock analysis, not here.
