# `common/mc.h` — motion compensation and plane-copy dispatch interface

**Role:** declares the large `x264_mc_functions_t` vtable, the `x264_weight_t` structure,
the SIMD plane-copy wrapper macros, and the mbtree propagation helper macro.
**Status:** dirty-side behavioural specification.

---

## 1. Header

- Source path: `common/mc.h`.
- Role in one sentence: define the complete calling contract for motion compensation,
  weighted prediction, memory shuffling and lookahead propagation primitives.
- Marker: dirty-side behavioural specification.

---

## 2. Purpose

Motion compensation and plane-format handling have many CPU-specific implementations and
subtle contracts (overreads, alignment, stride signs, field handling). This header centralizes
those contracts in one vtable so that `encoder/macroblock.c`, `encoder/me.c`, `encoder/slicetype.c`
and the input layer can call the best available implementation without knowing its identity.
It also contains reusable macro wrappers that add the "aligned bulk + scalar tail" logic
needed when a SIMD copy cannot handle an arbitrary width.

---

## 3. Public interface

Key macros:

- `MC_CLIP_ADD(s,x)`: saturating add to 32767 (`min(s+x, 0x7fff)`); `MC_CLIP_ADD2` applies it
  to two consecutive int16 values.
- `PROPAGATE_LIST(cpu)`: generates a static wrapper around an mbtree SIMD kernel that
  scatters results into `ref_costs`, with special handling for zero MVs and picture edges.
- `PLANE_COPY(align,cpu)`, `PLANE_COPY_SWAP(align,cpu)`, `PLANE_COPY_YUYV(align,cpu)`,
  `PLANE_INTERLEAVE(cpu)`: wrappers that choose the aligned SIMD core, apply it to all but the
  last row when the width is not a multiple of the vector width, and finish the last row with
  the portable routine so source data is never overread.

Types:

- `weight_fn_t` — `void (*)(pixel*, intptr_t, pixel*, intptr_t, const struct x264_weight_t*, int)`.
- `x264_weight_t` — 16-byte aligned struct with `cachea[8]`, `cacheb[8]` (packed scale/offset
  lookup values), `i_denom`, `i_scale`, `i_offset`, and a `weightfn` pointer.
  `x264_weight_none` is the zero-initialized sentinel.
- `SET_WEIGHT(w,b,s,d,o)` macro — sets scale/denominator/offset and either calls
  `h->mc.weight_cache` or clears `weightfn` if `b` is false.
- `x264_mc_functions_t` — the vtable (see section 4).
- `void x264_mc_init(uint32_t cpu, x264_mc_functions_t *pf, int cpu_independent)`.

---

## 4. State and data structures

`x264_mc_functions_t` groups its pointers as follows:

- **Interpolation:** `mc_luma(dst, i_dst, src[4], i_src, mvx, mvy, w, h, weight)`,
  `get_ref(...)` (same signature but returns a `pixel*` and may adjust `*i_dst`),
  `mc_chroma(dstu, dstv, i_dst, src, i_src, mvx, mvy, w, h)`.
  Valid luma sizes: width 4/8/16 with height 4/8/16 subject to the combinations listed in the
  file comment (4x4/4x8; 8x4/8x8/8x16; 16x8/16x16).
- **Averaging:** `avg[12]` indexed by the `PIXEL_*` enum, signature
  `(dst, dst_stride, src1, src1_stride, src2, src2_stride, i_weight)`. `weight==32` means equal
  weighting; otherwise the implicit weighted formula is used.
- **Copy:** `copy[7]` (only 16x16, 8x8 and 4x4 are defined in the C setup) and
  `copy_16x16_unaligned`.
- **Chroma cache conversion:** `store_interleave_chroma`, `load_deinterleave_chroma_fenc`,
  `load_deinterleave_chroma_fdec`.
- **Plane operations:** `plane_copy`, `plane_copy_swap`, `plane_copy_interleave`,
  `plane_copy_deinterleave` (may write up to 15 pixels past each destination plane),
  `plane_copy_deinterleave_yuyv`, `plane_copy_deinterleave_rgb` (with pixel pitch `pw`),
  `plane_copy_deinterleave_v210`.
- **Filtering:** `hpel_filter(dsth, dstv, dstc, src, i_stride, i_width, i_height, int16_t *buf)`.
- **Prefetch/memory:** `prefetch_fenc*` (400/420/422 variants), `prefetch_ref`,
  `memcpy_aligned`, `memzero_aligned`.
- **Integral images:** `integral_init4h/8h/4v/8v`.
- **Low-res / lookahead:** `frame_init_lowres_core`, `weight`, `offsetadd`, `offsetsub`,
  `weight_cache`, `mbtree_propagate_cost`, `mbtree_propagate_list`, `mbtree_fix8_pack/unpack`.

The `weight`, `offsetadd` and `offsetsub` pointers are arrays of six width-specific functions
(widths 2, 4, 8, 12, 16, 20), selected by block width at the call site.

---

## 5. Algorithm / behaviour

The header fixes several non-obvious behavioural contracts:

1. **mc_chroma write overrun:** it may write up to two bytes to the right of the destination,
   so callers must process blocks left-to-right. The scalar implementation in `mc.c` does not
   do this, but SIMD ones may.
2. **get_ref aliasing:** when no interpolation/weighting is needed, `get_ref` returns a direct
   pointer into the reference plane and overwrites `*i_dst_stride` with the source stride.
   When interpolation or weighting is needed it writes into `dst` and returns `dst`, leaving
   the caller's stride unchanged.
3. **Plane-copy wrappers:** the `align` parameter determines the vector width; widths not a
   multiple of that width are handled by rounding the bulk width up, processing all but the
   last row, and finishing with the C routine. `PLANE_COPY` also avoids non-temporal hints for
   widths below 256 pixels.
4. **Stride sign rule:** `PLANE_INTERLEAVE` only takes the fast path when `(i_srcu ^ i_srcv) >= 0`,
   i.e. the two source strides have the same sign, because the tail logic assumes matching
   memory direction.
5. **mbtree propagation:** `PROPAGATE_LIST` scatters each computed weight to up to four
   neighbouring macroblocks, with a fast path for zero MV, and uses unsigned comparisons to
   reject negative/out-of-range coordinates without extra branches.
6. **Dispatch:** `x264_mc_init` installs all C functions, then architecture-specific init
   functions override them; `cpu_independent` restores the portable mbtree cost/list functions
   for deterministic multi-threading.

---

## 6. Dependencies

- Types from `common.h`/`base.h` (`pixel`, `intptr_t`, `ALIGNED_16`, `SIZEOF_PIXEL`).
- Implemented in `mc.c`; consumed by `encoder/macroblock.c`, `encoder/me.c`,
  `encoder/slicetype.c`, `input/input.c`, `common/frame.c`, `common/threadpool.c`.
- The `PIXEL_*` enum and `x264_pixel_size` come from `pixel.h`; `x264_weight_t` is referenced
  by `common/frame.h` and the entropy/weight-prediction code.

---

## 7. Constants and tables

- `MC_CLIP_ADD` limit `0x7fff` (32767).
- Weight widths 2, 4, 8, 12, 16, 20 mapping to `mc_weight_wtab[6]`.
- Alignment thresholds in the wrapper macros: `align` for plane copy, `align>>1` for swap/YUYV,
  a fixed 16 for interleave; small-width cutoff 256 pixels for non-temporal hints.
- `x264_hpel_ref0`/`x264_hpel_ref1` tables are declared in `tables.h` (values in `mc.c.md`).

---

## 8. Performance notes

- The vtable is large; grouping related pointers (interpolation, copy, plane ops, lookahead)
  keeps the hot first cache lines focused on the frequently used entries.
- The wrapper macros are compiled per `align` value, so the tail handling is inlined and the
  SIMD core has no runtime width branch.
- Callers should choose the largest `copy`/`avg` entry that matches the block to avoid the
  width loop.
- Alignment of `x264_weight_t` to 16 bytes is deliberate (the cache arrays are vector-loaded).
- See `mc.c.md` section 8 for interpolation SIMD guidance and literature.

---

## 9. References

- ITU-T H.264 clauses 8.4.2/8.4.3 (motion compensation) and 7.4.3 (weighted prediction).
- x264 public source: `common/mc.h`, `common/mc.c`, `common/tables.c`.

---

## 10. Verification checklist

1. Compile a translation unit including only this header; all types and macros must resolve.
2. After `x264_mc_init(0, &pf, 0)`, assert every required pointer is non-null, including all
   12 `avg` entries and the 6 weight functions.
3. Verify `SET_WEIGHT` clears `weightfn` when `b==0` and installs a pointer otherwise.
4. Exercise `MC_CLIP_ADD` at the 32767 boundary and with negative inputs.
5. For each `PLANE_*` wrapper, test widths below, at, and above the alignment threshold with
   guard bytes to confirm no overread of the last row.
6. Compare portable and native-CPU vtables using the checks in `mc.c.md`.
7. Confirm `cpu_independent=1` restores exactly the C mbtree functions.

---

## 11. Open questions

- `UNKNOWN`: the exact overrun written by SIMD `mc_chroma` varies by architecture; callers
  must honor the left-to-right rule rather than rely on a specific byte count.
- `UNKNOWN`: which SIMD variant each CPU bit selects is defined in the architecture-specific
  init files, not in this header.
- `UNKNOWN`: future block sizes would extend the `avg[12]`/`copy[7]` arrays; the documented
  sizes match the current `PIXEL_*` enum.
