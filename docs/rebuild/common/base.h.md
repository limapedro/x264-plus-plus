# `common/base.h` - bit-depth-independent shared primitives

## 1. Header

Source path: `E:\_x264_dirty_stash\x264-source\common\base.h` (mirrored at `common/base.h`). Role in one sentence: the bit-depth-independent foundation header of the encoder internals. Marker: **dirty-side behavioural specification**.

This header is the lowest-level include of the encoder internals: it defines the numeric/type vocabulary, the small inline math helpers, the public enum constants describing H.264 profiles, chroma formats, slice types and SEI payload types, and declarations of general-purpose allocation, logging and parameter-string utilities.

## 2. Purpose

Many encoder modules must agree on a common set of abbreviations and bit-width conventions before bit-depth-specific types exist. `base.h` supplies that shared language: min/max/clip helpers, fixed-width memory-access aliases, aligned-allocation macros, a canonical macroblock-neighbourhood index map (Scan8), and bounded constants used to size per-frame and per-thread arrays. It is deliberately includable very early (before platform details are fully configured), so pure numeric macros are declared ahead of the system includes. A second role is the ownership convention: every pointer obtained through `x264_malloc` must be released with `x264_free`, and strings duplicated through `x264_param_strdup` are owned by the parameter object until `x264_param_cleanup`.

## 3. Public interface

Pure macros (no call, arguments may be evaluated more than once, so side-effecting arguments are forbidden):

- `X264_MIN/MAX`, `X264_MIN3/MAX3`, `X264_MIN4/MAX4`: two-, three- and four-operand extrema.
- `XCHG(type,a,b)`: typed swap; `FIX8(f)`: real to 8-bit fixed point with round-to-nearest; `ARRAY_ELEMS(a)`: static array element count; `ALIGN(x,a)`: round up to the next multiple of a power-of-two `a`.
- `IS_DISPOSABLE(type)`: true only for a B picture; used to decide whether a frame may be dropped.

Type-punning accessors: `M16/M32/M64/M128(src)` read an aligned native-endian integer of the named width from `src`; `CP16/CP32/CP64/CP128(dst,src)` copy that many bits. They exist because `memcpy` does not communicate alignment to the compiler and because wide loads map directly to SIMD moves. `MAY_ALIAS` on the union types permits this reinterpretation without strict-aliasing violations. `x264_uint128_t` is a pair of 64-bit words; `M128_ZERO` is its zero value. `MEM_FIX(x,t,s)` / `MEM_DYN(x,t)` communicate object size to inline-assembly memory constraints (array casts on modern GCC, structure casts on older compilers).

Enums and constant sets:

| Group | Values |
| --- | --- |
| `enum profile_e` | Baseline 66, Main 77, High 100, High10 110, High422 122, High 4:4:4 Predictive 244 |
| `enum chroma_format_e` | 400 = 0, 420 = 1, 422 = 2, 444 = 3 |
| `enum slice_type_e` | P = 0, B = 1, I = 2; `slice_type_to_char` maps to 'P','B','I' |
| `enum sei_payload_type_e` | buffering period 0, pic timing 1, pan/scan rect 2, filler 3, registered user data 4, unregistered user data 5, recovery point 6, dec-ref-pic marking 7, frame packing 45, mastering display 137, content light level 144, alternative transfer 147 |

Capacity constants: `X264_BFRAME_MAX` 16, `X264_REF_MAX` 16, `X264_THREAD_MAX` 128, `X264_LOOKAHEAD_THREAD_MAX` 16, `X264_LOOKAHEAD_MAX` 250, `X264_THREAD_HEIGHT` 24 (16 rows of the macroblock in flight + 3 deblocking + 3 motion-compensation filter + 2 safety rows). `X264_WEIGHTP_FAKE` is -1: it selects lookahead-only fade detection used when mb-tree and psy are enabled but real weighted prediction is forbidden; the negative value keeps the usual `i_weighted_pred > 0` test false.

Scan8 constants: `X264_SCAN8_LUMA_SIZE` = 40, `X264_SCAN8_SIZE` = 120, `X264_SCAN8_0` = 4 + 1*8 = 12. `LUMA_DC` 48, `CHROMA_DC` 49. The `x264_scan8[51]` array maps each of the 16 luma 4x4 blocks, 16 Cb and 16 Cr blocks plus three DC entries into a padded 8-wide, 15-row address space. Rows 0, 5, 10 hold DC slots (DY, DU, DV); rows 1-4 hold luma, 6-9 Cb, 11-14 Cr, with a one-cell border and a two-cell horizontal guard. Arrays indexed by this map can use a fixed `+8*y` stride and need no per-block bounds logic.

Inline helpers:

- `x264_clip3(v,lo,hi)` and `x264_clip3f` clamp integers/doubles; undefined if `lo > hi`.
- `x264_exp2fix8(x)` converts a QP-like value to an 8-bit fixed-point scale using `x264_exp2_lut`; indices below 0 return 0, above 1023 return 0xffff.
- `x264_log2(x)` approximates log2 of a nonzero `uint32_t` via leading-zero count and a two-level lookup; `x == 0` is UNKNOWN (the fallback `x264_clz` returns 32, out of table range).
- `x264_median(a,b,c)` returns the middle value using branch-free bit tricks.
- `x264_median_mv(dst,a,b,c)` applies the scalar median independently to both motion-vector components.
- `x264_predictor_difference(mvc,n)` sums Manhattan distances between consecutive candidate motion vectors.
- `x264_cabac_mvd_sum(left,top)` packs two coarse MVD-summary counters (thresholds at 2 and 32) into a 16-bit value, low byte x, high byte y.

Declared utilities (defined in `base.c`): `x264_reduce_fraction`, `x264_reduce_fraction64`, `x264_log_default`, `x264_log_internal`, `x264_malloc`, `x264_free`, `x264_slurp_file`, `x264_param_strdup`, `x264_param2string`. `X264_API` marks exported shared-library symbols; `x264_param_strdup` is internal-only.

Allocation macros: `CHECKED_MALLOC(var,size)` assigns and jumps to `fail` on null; `CHECKED_MALLOCZERO` additionally zeroes; `CHECKED_PARAM_STRDUP` duplicates into a parameter object. `PREALLOC_INIT` declares an offset counter and up to `PREALLOC_BUF_SIZE` = 1024 destination pointers; `PREALLOC(var,size)` records the current aligned offset as a temporary integer address; `PREALLOC_END(ptr)` performs one `x264_malloc` for the sum and rebases every recorded pointer. This single-allocation arena cuts malloc traffic and improves huge-page/TLB behaviour.

## 4. State and data structures

The header itself is stateless. Its union types define representation invariants: 16/32/64/128-bit aggregates are native-endian, aligned, and may be accessed through narrower member arrays. The PREALLOC scheme's implicit state is a pair (running offset, pointer-to-pointer list); the offset advances by `ALIGN(size, NATIVE_ALIGN)`, so each member begins on a 64-byte boundary on x86/LOONGARCH and a 16-byte boundary elsewhere. Exactly 1024 members may be registered; exceeding that count corrupts adjacent stack data - a caller-side invariant, not checked.

## 5. Algorithm / behaviour

`ALIGN(x,a)` rounds up via `x + (a-1)` masked with `~(a-1)`; `FIX8` uses +0.5 before truncation after multiplication by 256. `x264_exp2fix8` reconstructs a wide-range scale from a 64-entry fractional table plus a shift equal to `index >> 6`, then divides by 256: effectively a piecewise-linear `2^x` with 6-bit fractional precision. `x264_log2` takes the leading-zero count `lz`, normalises the mantissa by `x << lz`, indexes a 128-entry table with the top 7 bits, and adds the lz-derived offset from a 32-entry table. `x264_median` sorts three integers by exchanging only the minimum and is branch-free. `x264_cabac_mvd_sum` counts exceeded thresholds per axis, which is the coarse context selector for CABAC MVD coding.

The Scan8 layout is the de-facto coordinate system for neighbour caches: entry `4+1*8` is the top-left luma 4x4 block; the left neighbour is at offset -1, top at -8, top-right at -7, top-left at -9. DC slots sit in column 0 of rows 0/5/10 so they cannot collide with real blocks. Every row is 8 cells wide, so `x264_macroblock_cache_rect` (see `rectangle.h.md`) can write a whole row with one wide store.

## 6. Dependencies

Included by nearly every internal translation unit directly or via `common.h`. Includes `osdep.h` (platform macros, `NATIVE_ALIGN`, thread primitives), `stdarg.h`, `stddef.h`, `stdlib.h`, `string.h`, `assert.h`, `limits.h`, then `cpu.h` and `tables.h`. Consumed by `base.c`, `cpu.c`, `osdep.c`, `win32thread.c`, `tables.c`, and indirectly by the encoder core. It publishes no configuration state; `h->param` owns runtime settings.

## 7. Constants and tables

Constants are listed in section 3. `x264_scan8` is the only data table in this header; its 51 entries encode 48 block positions plus 3 DC positions. SEI payload numbers are assigned by H.264 Annex D and must match exactly; profile IDs are assigned by Annex A. The scan map is internal and not itself visible in the bitstream, but all neighbour-cache users must agree on it.

## 8. Performance notes

- **Cache layout:** Scan8's 8-wide rows and `+8*y` addressing let neighbour caches be written with 16/32/64-bit stores; the one-cell border removes per-block bounds tests. Keep the Scan8-indexed arrays at their declared alignments so SIMD stores do not split cache lines.
- **False sharing:** per-frame and per-thread mutable words (`i_lines_completed`, `i_slice_count`, `i_threadslice_pass`) live in separate objects with their own mutexes; when adding per-thread counters, pad them to a cache line (64 bytes on modern x86, see `X264_CPU_CACHELINE_64`) rather than placing them adjacent in one struct.
- **Allocation strategy:** PREALLOC collapses dozens of small allocations into one, reducing allocator lock contention and the number of TLB entries needed by a frame. The 1024-slot limit is a compile-time budget, not a runtime check.
- **Dispatch hierarchy:** these helpers are always-inline and CPU-independent; SIMD specialisation belongs in function tables, not here.
- Public references: Drepper, *What Every Programmer Should Know About Memory* (2007), sections 2-3 on cache-line aliasing and TLB reach; Intel *Optimization Reference Manual*, section 3.6 on alignment; Hennessy & Patterson, *Computer Architecture*, memory-hierarchy chapter.

## 9. References

- ITU-T Rec. H.264 / ISO/IEC 14496-10, Annex A (profiles), Annex D (SEI payload types), clause 6.2.
- Public `x264.h` API header shipped with x264 (parameter constants, log levels, `X264_API` convention).
- x264 project documentation `doc/threads.txt` for the thread-count constants' usage context.
- Drepper, *What Every Programmer Should Know About Memory* (2007).
- Intel Intrinsics Guide and Intel SDM Vol. 1 for aligned wide loads/stores.

## 10. Verification checklist

- Compile a tiny client including only `base.h`; all enums, macros and inline helpers must resolve without linking encoder internals beyond the declared `X264_API` functions.
- For random 32-bit values, `x264_clip3(x,0,255)` must equal a reference clamp; `x264_median` must equal sorting-based median for all permutations including duplicates.
- `x264_exp2fix8` must be monotone non-decreasing and produce 0 and 0xffff at the two documented saturation ends.
- `x264_log2(1<<k)` must be within a small tolerance of k for k = 0..30 (black-box numeric check).
- In a PREALLOC harness, all recorded pointers must be distinct, aligned to `NATIVE_ALIGN`, non-overlapping and inside the single returned block; `x264_free` must accept that block.
- Confirm `x264_scan8` has 51 entries, the three DC entries at indices 48-50, and no duplicate luma entries.

## 11. Open questions

- UNKNOWN: the exact rounding-error envelope of `x264_log2` is not specified; equivalent implementations may differ in rate-control decisions.
- UNKNOWN: whether downstream code relies on the numeric identity of individual `x264_scan8` entries rather than only on neighbour offsets; check macroblock users before changing the map.
- UNKNOWN: `x264_log2(0)` behaviour on compilers without a builtin leading-zero count.
