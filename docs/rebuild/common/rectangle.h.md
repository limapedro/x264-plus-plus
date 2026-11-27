# `common/rectangle.h` - neighbour-cache rectangle filling

## 1. Header

Source path: `E:\_x264_dirty_stash\x264-source\common\rectangle.h` (mirrored at `common/rectangle.h`). Role in one sentence: provides always-inline primitives that fill rectangular regions of the Scan8 neighbour caches with a repeated value using the widest convenient stores. Marker: **dirty-side behavioural specification**.

## 2. Purpose

Macroblock analysis repeatedly writes the same MV, MVD, reference index or skip flag into small rectangles of the neighbour cache (e.g. all 16 blocks of an inter 16x16 macroblock). Doing that with byte stores in a loop is unnecessarily slow; doing it with a generic loop when the dimensions are compile-time constants wastes branches. This header implements a specialised constant-dimension writer plus a runtime-dimension dispatch table.

## 3. Public interface

- `static ALWAYS_INLINE void x264_macroblock_cache_rect(void *dst, int w, int h, int s, uint32_t v)`: writes `h` rows of `w` bytes at `dst`, repeating the `s`-byte value `v`. Precondition documented in the source: it should be called only with **constant** `w`, `h` and `s`, so the compiler can specialise the store sequence. Supported element sizes are 1, 2 and 4 bytes (values promoted to 16/32/64-bit patterns); supported widths are 2, 4, 8 and 16 bytes; supported heights are 1, 2, 3, 4, with 16 handled by a loop. Any other width triggers an assertion failure.
- `x264_macroblock_cache_mv(h, x, y, width, height, i_list, mv)`: fill the MV cache at Scan8 position `(x,y)` with a packed 32-bit MV; the macro `_ptr` variant accepts a two-element array and packs it.
- `x264_macroblock_cache_mvd(h, x, y, width, height, i_list, mvd)`: fill the MVD cache with a 16-bit value.
- `x264_macroblock_cache_ref(h, x, y, width, height, i_list, ref)`: fill the reference-index cache with an 8-bit value (negative indices are cast to unsigned before packing).
- `x264_macroblock_cache_skip(h, x, y, width, height, b_skip)`: fill the skip cache with an 8-bit flag.
- `x264_macroblock_cache_intra8x8_pred(h, x, y, i_mode)`: write a 2x2 block of intra prediction modes in one call.
- Extern function-pointer tables `x264_cache_mv_func_table[10]`, `x264_cache_mvd_func_table[10]`, `x264_cache_ref_func_table[10]` (templated names), each `void (*)(void *, uint32_t)`; used when width or height is not a compile-time constant.

All caches are indexed as `&h->mb.cache.<name>[...][X264_SCAN8_0 + x + 8*y]`, so the initial offset is the top-left luma block and the row stride is eight Scan8 cells.

## 4. State and data structures

The destination caches belong to `x264_t.mb.cache` (see `common.h.md`): `mv[2][40][2]`, `mvd[2][40][2]`, `ref[2][40]`, `skip[40]`, `intra4x4_pred_mode[40]`. Their declared alignments (4/8/16 bytes) are chosen so a 2/4/8-byte store at a Scan8 position never splits an alignment boundary; `non_zero_count` is deliberately misaligned for assembly-specific reasons but is not written by this header.

The function tables contain seven non-null entries each; the remaining three slots are null and must never be called. The index formula maps `(width,height)` pairs to slots 0,1,2,3,5,7,9.

## 5. Algorithm / behaviour

The value is replicated by constructing progressively wider patterns: a 16-bit pattern for 1-byte elements, a 32-bit pattern for 2-byte elements (or 1-byte elements duplicated), and a 64-bit pattern for 4-byte elements or two 32-bit halves. The width is multiplied by the element size to obtain a byte width, then the writer specialises on byte width:
- Width 2: one 16-bit store per row, with early returns for heights 1 and 2, then four rows.
- Width 4: one 32-bit store per row with the same height specialisation.
- Width 8: on 64-bit targets one 64-bit store per row; on 32-bit targets two 32-bit stores per row.
- Width 16: on targets with SSE vector extensions, four 128-bit stores (two rows immediately, early return for height 2, then two more rows); otherwise a loop of 64-bit pair stores or four 32-bit stores per row. Height 1 is asserted not to occur for width 16.
- Any other width: assertion failure.

The row stride used by these stores is `s * 8` bytes, i.e. the original element size scaled to a Scan8 row. Because the compiler sees constant `w`, `h`, `s` at the call site, it can eliminate nearly all branches and keep the store patterns in registers.

For non-constant dimensions, the three public MV/MVD/ref helpers first compute a table index of the form `width + (height << 1) - 3` and call the corresponding specialised function. This index maps the legal (width,height) pairs to the seven populated slots; a caller passing an illegal pair would index a null slot. The skip and intra-mode helpers have no runtime-dispatch variants because their dimensions are always constant.

## 6. Dependencies

Included at the end of `common.h` (after `x264_t` is defined). Uses `M16/M32/M64/M128` from `base.h`, Scan8 constants and `x264_t` from `common.h`, `WORD_SIZE`, `HAVE_VECTOREXT`, `ALWAYS_INLINE`, `MAY_ALIAS` from `osdep.h`, and the function tables defined in `rectangle.c`. Called from macroblock analysis and prediction decision code throughout `encoder/`.

## 7. Constants and tables

Constant table: the three dispatch tables of ten pointers each, with populated indices 0 (1x1), 1 (2x1), 2 (1x2), 3 (2x2), 5 (4x2), 7 (2x4), 9 (4x4); indices 4, 6 and 8 are null. Element widths: MV 4 bytes, MVD 2 bytes, ref/skip/intra-mode 1 byte. `X264_SCAN8_0 = 12`. Replication patterns are byte-lane broadcasts (`0x0101`, `0x01010101`, etc.) derived from the element width.

## 8. Performance notes

- **Cache layout:** writing an entire cache row with one 16/32/64/128-bit store minimises store instructions and avoids read-modify-write; keep destination caches aligned as declared.
- **Write combining:** successive rows are `s*8` bytes apart; for 8-wide Scan8 rows this is a moderate stride, so wide stores stay within one cache line for typical element sizes.
- **False sharing:** neighbour caches are per-encoder/per-thread state inside `x264_t.mb`, which is not shared between frame threads; no padding needed.
- **Dispatch hierarchy:** constant dimensions are inlined and fully unrolled; only genuinely variable partition shapes pay an indirect call. Preserve this two-level strategy when porting.
- **Allocation strategy:** no allocations.
- Public references: Intel Optimization Reference Manual on store width and write combining; Drepper (2007) on cache-line writes; compiler documentation on `always_inline` and `may_alias`.

## 9. References

- x264 `common.h` Scan8 layout (`base.h.md` documents the map).
- Intel Intrinsics Guide and SDM on 128-bit stores (`MOVAPS`/`MOVDQA`).
- GCC/Clang attribute documentation for `always_inline` and type aliasing.
- Drepper, *What Every Programmer Should Know About Memory* (2007).

## 10. Verification checklist

- For each legal combination of element size (1,2,4), width (2,4,8,16) and height (1,2,3,4, and 16 where applicable), fill a guarded buffer and verify every byte of the rectangle equals the expected replicated pattern and that no byte outside the rectangle changed (guard bands).
- Verify the runtime-dispatch path (variable width/height) produces identical results to the inline path for all seven legal (width,height) pairs.
- Verify negative reference indices are stored as their unsigned byte representation.
- Verify `x264_macroblock_cache_intra8x8_pred` writes exactly a 2x2 block at the requested Scan8 offset.
- Verify on 32-bit and 64-bit builds that width-8 writes cover the full 8 bytes.
- Check that all ten table slots are initialised at library load and that the three intended slots are null.

## 11. Open questions

- UNKNOWN: the exact mapping from macroblock partition shape to `(width,height)` arguments is caller-side and not documented here; only the table index arithmetic is visible.
- UNKNOWN: whether compilers other than GCC honour `ALWAYS_INLINE` sufficiently to keep branches eliminated; the contract relies on constant arguments rather than a guaranteed optimisation.
- UNKNOWN: behaviour for width 16 and height 1 is an assertion (undefined in release builds if assertions are disabled); callers must avoid it.
