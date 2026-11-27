# `common/arm/mc-a.S` — ARM/NEON motion-compensation primitives

## 1. Header

- **Source path:** `common/arm/mc-a.S`.
- **Role in one sentence:** assembly primitives for prefetching, aligned memory operations, block averaging, weighted prediction, block copy, chroma interpolation, half-pel filters, plane conversion, integral-image preparation, and MB-tree propagation.
- **Marker:** Dirty-side behavioural specification; all descriptions are behavioural and written independently of the reference source.


## 2. Purpose

This file contains the low-level assembly routines orchestrated by `common/arm/mc-c.c`. It covers a wide range of memory-moving and interpolation operations, all of which benefit from NEON's wide loads/stores and integer multiply-accumulate instructions.

## 3. Public interface

Exported symbols (declared in `mc-c.c` or headers):

- Prefetch: `x264_prefetch_ref_arm( pix, stride, parity )`, `x264_prefetch_fenc_arm( pix_y, stride_y, pix_uv, stride_uv, mb_x )`.
- Memory: `x264_memcpy_aligned_neon( dst, src, n )`, `x264_memzero_aligned_neon( dst, n )`.
- Averaging: `x264_pixel_avg_<w>x<h>_neon` for nine sizes; `x264_pixel_avg2_w4/w8/w16/w20_neon`.
- Weighted prediction: four width variants (4/8/16/20) times four modes (`full`, `nodenom`, `offsetadd`, `offsetsub`).
- Copy: `x264_mc_copy_w4_neon`, `w8`, `w16`, and `w16_aligned`.
- Interpolation: `x264_mc_chroma_neon`, `x264_hpel_filter_v_neon`, `x264_hpel_filter_c_neon`, `x264_hpel_filter_h_neon`, `x264_frame_init_lowres_core_neon`.
- Plane helpers: core plane copy, swap, interleave, deinterleave, RGB deinterleave, store/load chroma interleave.
- Analysis helpers: `x264_integral_init4h_neon`, `init8h`, `init4v`, `init8v`, `x264_mbtree_propagate_cost_neon`, `x264_mbtree_fix8_pack_neon`, `x264_mbtree_fix8_unpack_neon`.

Internal helpers include `mbtree_propagate_list_internal_neon` and the dispatch tables for aligned memcpy variants.

## 4. State and data structures

Constants include `pw_0to15` (16-bit indices 0..15), `memcpy_table` (jump table of four aligned-copy variants), and various shuffle masks embedded as immediate vectors. Weighted prediction reads `x264_weight_t` fields at fixed offsets: denominator at 32, scale at 36, offset at 40.

## 5. Algorithm / behaviour

**Prefetch.** The reference prefetch routine assumes a 64-byte cache line and issues eight `pld` instructions covering rows around the predicted block; the parity argument selects an offset. The source prefetch routine similarly prefetches luma and chroma rows based on the macroblock column.

**Aligned memory.** `memcpy_aligned_neon` computes a 2-bit alignment signature from the two pointers and jumps through `memcpy_table` to one of four specialized loops. Each loop copies 16, 32, or 64 bytes per iteration using aligned NEON loads/stores. `memzero_aligned_neon` stores 128 zero bytes per iteration.

**Averaging.** Plain averaging uses `vrhadd.u8` for rounded `(a+b+1)>>1`. The `pixel_avg` entry points inspect the weight argument: weight 32 selects plain averaging; other values select weighted add/add, add/subtract, or subtract/add paths. Weighted paths compute `(w*s1 + (64-w)*s2 + 32) >> 6` with widening multiplies and saturating narrowing.

**Weighted prediction.** The full path multiplies source bytes by `scale`, applies a rounding right shift by `denom`, and adds `offset`, saturating to byte range. The no-denominator path multiplies by `scale` and adds `offset` in 16-bit lanes. The offset-only paths use saturating byte add or subtract with the cached magnitude in `cachea[0]`. All variants process two or four rows per loop iteration.

**Copy.** Widths 4 and 8 copy rows with scalar or 64-bit vector loads; width 16 copies 128 bits per row. The aligned variant uses aligned load/store hints.

**Chroma interpolation.** `mc_chroma_neon` computes bilinear interpolation with coefficients `(8-dx)*(8-dy)`, `dx*(8-dy)`, `(8-dx)*dy`, `dx*dy`, adding 32 and shifting right by 6. It deinterleaves U/V pairs with `vld2.8`, processes widths 2, 4, and 8 with separate code paths, and writes U and V to distinct destination pointers.

**Half-pel filters.** The vertical filter computes `(a - 5b + 20c + 16) >> 5` with saturation using widening unsigned arithmetic and multiply-accumulate with constants 5 and 20. The horizontal filter applies the same formula across a row with `vext` shifts. The central filter computes `(a - 5b + 20c + 32) >> 6` from a temporary buffer of vertically filtered rows.

**Low-resolution preparation.** The core routine averages pairs of source rows and columns, producing four output planes: full average, horizontal-only, vertical-only, and centre. It uses pairwise average instructions and byte-permutation masks to interleave results correctly.

**Plane helpers.** Swap, interleave and deinterleave routines use byte-permutation masks to reorder packed U/V or RGB data. Widths are processed 16 bytes at a time.

**Integral and MB-tree.** The integral helpers compute horizontal and vertical prefix sums of 16-bit values using shifted vector adds and subtracts. MB-tree propagation converts integer cost values to float, multiplies by propagation factors, and converts back with saturation. The fix8 pack/unpack helpers convert between float values and 8-bit fixed-point integers with byte swapping.

## 6. Dependencies

- Includes `common/arm/asm.S`.
- Called from `common/arm/mc-c.c` and through function-pointer tables in `common/mc.c`.
- Uses `x264_weight_t` layout from `common/mc.h` and pixel-size enum from `common/pixel.h`.

## 7. Constants and tables

- `pw_0to15` index vector; `memcpy_table` jump table.
- Weighted averaging denominator 64 with rounding 32.
- Half-pel constants 5 and 20, shifts 5 and 6, rounding 16 and 32.
- Chroma interpolation denominator 64 with rounding 32.
- MB-tree fix8 scale: 256 (`vcvt` with 8 fractional bits).

## 8. Performance notes

This file is dominated by memory throughput. The key optimizations are aligned wide loads/stores, jump-table dispatch for alignment cases, and processing multiple rows per loop to hide latency. Weighted prediction caching in `mc-c.c` converts common cases into simple saturating byte operations. For a clean rebuild, implement the high-level operations exactly and use SIMD only where measurements justify it; correctness of rounding and saturation is more important than instruction count.

## 9. References

- `common/mc.c` and `common/mc.h` for canonical semantics and function-pointer contracts.
- ITU-T H.264, fractional-pel interpolation and weighted prediction clauses.
- ARM NEON Programmer's Guide for `vld2`, `vst2`, `vext`, `vmull`, and saturating narrowing.

## 10. Verification checklist

- All averaging and weighted-prediction variants match generic results for every supported width.
- Chroma interpolation matches the generic bilinear formula for all 64 fractional offsets.
- Half-pel filters match generic output including edge samples.
- Plane-copy helpers match generic results for plain, swapped, interleaved, and RGB cases.
- Integral helpers produce identical prefix sums to the scalar implementation.
- MB-tree fix8 pack/unpack round-trips values within the representable range.
- Callee-saved registers are preserved across all routines.

## 11. Open questions

- `UNKNOWN`: exact cache-line assumption (64 bytes) on non-Cortex-A8 cores.
- `UNKNOWN`: whether the RGB deinterleave routine is installed on all NEON builds or only those with specific feature flags.
