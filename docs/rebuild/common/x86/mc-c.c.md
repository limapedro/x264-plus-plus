# common/x86/mc-c.c — behavioural specification

**Role:** C-side glue for x86 motion compensation: declares the assembly kernel symbols, composes quarter-pel interpolation from half-pel planes and averaging/copy kernels, prepares weighted-prediction caches, and dispatches every `x264_mc_functions_t` slot according to detected CPU flags.
**Marker:** dirty-side behavioural specification.

---

## 1. Header

- Source path: `common/x86/mc-c.c`
- Role: the bridge between the assembler kernels in `mc-a.asm`/`mc-a2.asm` and the architecture-independent `x264_mc_functions_t` used by `common/frame.c`, `common/me.c` and `common/analyse.c`.
- Built once per bit depth; uses the `x264_template` macro to add the bit-depth-specific symbol prefix.

---

## 2. Purpose

The assembly files export dozens of ISA-specific symbols, but the rest of the encoder sees a single function-pointer table. This file:

1. Declares all those symbols with their exact signatures (using `DECL_SUF`, `MC_WEIGHT`, `MC_LUMA`, etc. macros).
2. Implements in C the operations that are awkward in assembly: luma quarter-pel interpolation (`mc_luma`/`get_ref`) by selecting among the four half-pel planes and averaging when needed, and weighted-prediction parameter precomputation (`weight_cache`).
3. Builds the dispatch hierarchy in `x264_mc_init_mmx(cpu, pf)`, overwriting table entries with progressively faster variants as higher CPU features are detected.

---

## 3. Public interface

- `void x264_mc_init_mmx( uint32_t cpu, x264_mc_functions_t *pf )` — the only externally visible function (declared in `common/x86/mc.h`). Fills `pf` in place; should be called after the portable `x264_mc_init_c` has installed C fallbacks.
- All other symbols are declarations of assembly functions (see the corresponding docs for their contracts). Static C helpers:
  - `mc_luma_<isa>( dst, i_dst, src[4], i_src, mvx, mvy, i_width, i_height, weight )` — writes the interpolated block.
  - `get_ref_<isa>( dst, *i_dst, src[4], i_src, mvx, mvy, i_width, i_height, weight )` — like `mc_luma` but may return a pointer directly into a half-pel plane when no interpolation or weighting is needed (setting `*i_dst = i_src`), allowing zero-copy.
  - `weight_cache_mmx2` / `weight_cache_ssse3( h, w )` — compute the packed `cachea`/`cacheb` vectors and choose `weightfn`.
  - Macro-generated `HPEL(align, cpu, cpuv, cpuc, cpuh)` drivers call the three row filters per row with alignment correction.
  - `PLANE_COPY`, `PLANE_COPY_SWAP`, `PLANE_COPY_YUYV`, `PLANE_INTERLEAVE`, `PROPAGATE_LIST` macro-generated wrappers handle alignment/tail cases around the `_core` assembly functions.
  - Inline-asm overrides `MC_CLIP_ADD`/`MC_CLIP_ADD2` for saturating adds of one/two int16 values when `HAVE_X86_INLINE_ASM`.

---

## 4. State and data structures

- `x264_mc_functions_t` (from `common/mc.h`) fields populated here: `mc_luma`, `get_ref`, `mc_chroma`, `avg[12]`, `copy[7]`, `copy_16x16_unaligned`, `store_interleave_chroma`, `load_deinterleave_chroma_{fenc,fdec}`, `plane_copy*`, `hpel_filter`, `prefetch*`, `memcpy_aligned`, `memzero_aligned`, `integral_init*`, `frame_init_lowres_core`, `weight`, `offsetadd`, `offsetsub`, `weight_cache`, `mbtree_*`.
- `x264_weight_t` as described in `mc-a.asm.md`. The `weightfn` pointer table (`pf->weight`, `pf->offsetadd`, `pf->offsetsub`) is indexed by `i_width>>2` (widths 4/8/12/16/20 → indices 1..5).
- Quarter-pel selection tables (from `common/tables.c`):
  - `x264_hpel_ref0[16] = {0,1,1,1,0,1,1,1,2,3,3,3,0,1,1,1}`
  - `x264_hpel_ref1[16] = {0,0,1,0,2,2,3,2,2,2,3,2,2,2,3,2}`
  indexed by `qpel_idx = ((mvy&3)<<2) + (mvx&3)`. Source planes: 0 = original, 1 = H, 2 = V, 3 = C (diagonal).
- Weight lookup tables: `mc_weight_wtab_<isa>[6]`, `mc_offsetadd_wtab_<isa>[6]`, `mc_offsetsub_wtab_<isa>[6]`, and `pixel_avg_wtab_<isa>[6]` (index 0 is NULL/unused; indices 1..5 correspond to widths 4/8/12/16/20).
- `PIXEL_*` enum values index the `avg[]`/`copy[]`/etc. arrays by block size.

---

## 5. Algorithm / behaviour

### 5.1 Declaration macros
`DECL_SUF` declares all five ISA suffixes (`_mmx2`, `_sse2`, `_ssse3`, `_avx2`, `_avx512`) for a function. `MC_WEIGHT`/`MC_WEIGHT_OFFSET` declare the weight/offset functions per width and ISA. `MC_CHROMA`, `LOWRES`, `PIXEL_AVG_WALL`, `PIXEL_AVG_WTAB`, `MC_COPY_WTAB`, `MC_WEIGHT_WTAB` similarly generate declarations and table initialisations. Some declarations refer to functions that do not exist (explicitly noted in the source) — harmless because only referenced table entries are used.

### 5.2 Quarter-pel luma interpolation (`mc_luma` / `get_ref`)
For MV components `mvx`, `mvy`:
1. `qpel_idx = ((mvy&3)<<2) | (mvx&3)`.
2. `offset = (mvy>>2)*i_src_stride + (mvx>>2)` (full-pel displacement).
3. `src1 = src[x264_hpel_ref0[qpel_idx]] + offset + ((mvy&3)==3) * i_src_stride` — the primary half-pel plane; the `+stride` correction handles the case where the 3/4 position must use the next row.
4. If `qpel_idx & 5` is nonzero (quarter-pel component present), also compute `src2 = src[x264_hpel_ref1[qpel_idx]] + offset + ((mvx&3)==3)` and average `src1` and `src2` with `pixel_avg_wtab[width>>2]( dst, i_dst, src1, i_src, src2, height )` (weight=0 → truncating average). Then apply `weight->weightfn` in place if weighted prediction is active.
5. If no qpel interpolation is needed, apply weighting directly or, in `get_ref`, return `src1` and set `*i_dst = i_src` (zero-copy fast path).

`mc_luma` always writes to `dst`; `get_ref` can avoid the write. Both are instantiated for mmx2, sse2, avx2, and (8-bit) cache32_mmx2, cache64_mmx2, cache64_sse2, cache64_ssse3, cache64_ssse3_atom. The `cache*` variants use the corresponding cacheline-safe `pixel_avg_wtab`.

### 5.3 Weighted prediction caches
- **8-bit MMX2:** if `scale == 1<<denom`, choose `offsetadd`/`offsetsub` based on offset sign and set all `cachea[i] = |offset|`. Otherwise `weightfn = pf->weight`, `den1 = (offset<<denom) | ((denom?1<<(denom−1)):0)`; `cachea[i] = scale`, `cacheb[i] = den1`.
- **8-bit SSSE3:** when scale is a pure power of two, same offset path; otherwise `den1 = scale << (8−denom)`, `cachea[i] = den1`, `cacheb[i] = offset`, enabling the `pmulhrsw` fast path.
- **High depth:** analogous but offsets and rounding are scaled by `1<<(BIT_DEPTH−8)`; cachea holds `1<<denom`, cacheb holds either `2*scale` or `1 + offset*2^(BIT_DEPTH−7+1)`.

### 5.4 Table aliases
For high depth, widths 12/20 are replaced by 10/18 because only 9/17 pixels carry data (`#undef`/`#define` tricks). For 8-bit, width-12 SSE2/SSSE3 variants alias the width-16 versions because the aligned 16-wide SSE2 path is faster than MMX width-12 once cacheline issues are resolved.

### 5.5 Dispatch (`x264_mc_init_mmx`)
The function is a cascade of feature checks; later checks overwrite earlier assignments:
1. **MMX** (high depth only first): copy/integral mmx; **MMX2**: prefetch, interleave, avg (all sizes), mc_luma/get_ref/mc_chroma/hpel_filter, weight/offset mmx2 tables, frame_init_lowres.
2. **SSE**: memcpy/memzero/plane_copy (sse).
3. **High-depth branch:** if SSE2 → frame_init_lowres, deinterleave, plane ops, integral, weight sse2, offset sse2; if `SSE2_IS_SLOW` return; else avg sse2, aligned copy, mc_chroma sse2 (unless `STACK_MOD4`); SSSE3 → more lowres/swap/v210/mbtree/weight ssse3; AVX → lowres/deinterleave/plane ops/mc_chroma avx; XOP → lowres xop; AVX2 → mc_luma avx2, deinterleave avx2, v210 avx2; AVX512 → v210 avx512.
4. **8-bit branch:** CACHELINE_32/64 selects cache-safe luma/get_ref/lowres (32-bit only); SSE2 installs integral/v210/mem ops/weight/avg aligned etc., with `SSE2_IS_SLOW` limiting which; SSE2_IS_FAST selects plane interleave, mc_luma/get_ref sse2 (cache64 variants when flagged); SSSE3 installs avg ssse3 for all sizes, plane copy swap, RGB deinterleave, mbtree, weight ssse3, chroma ssse3 (or cache64), SLOW_ATOM/SLOW_SHUFFLE variants; SSE4 adds integral4h/8h; AVX adds lowres/chroma/hpel; XOP lowres; AVX2 adds hpel avx2, chroma avx2, weight avx2, avg 16x16/16x8 avx2, integral/lowres/deinterleave; AVX512 adds avg variants for 16/8-wide and chroma deinterleave.
5. **Common tail:** AVX → memcpy/memzero/plane copy/mbtree avx; FMA4 → mbtree cost fma4; AVX2 → plane copy swap/deinterleave, get_ref avx2, mbtree avx2, fix8 avx2; AVX512 → memcpy/memzero/plane copy/mbtree/fix8 avx512, propagate_list avx512 (x86-64 only).

### 5.6 Inline-asm clip-add
`MC_CLIP_ADD`/`MC_CLIP_ADD2` perform `paddsw` on one/two int16 values using xmm0/xmm1 with a memory clobber, used by analyse code where a portable saturating add would be slower.

---

## 6. Dependencies

- Includes `common/common.h`, local `mc.h`, `x264.h` (CPU flags) and `tables.h` (hpel ref tables).
- Calls assembly symbols from `mc-a.asm`, `mc-a2.asm`; declarations are bit-depth-prefixed via `x264_template`.
- Called from `common/mc.c: x264_mc_init()` which first installs C defaults then calls `x264_mc_init_mmx` on x86.
- `common/pixel.c` handles SAD/SATD dispatch separately (see `sad-a.asm.md`).

---

## 7. Constants and tables

- `x264_hpel_ref0`/`ref1` tables (values in §4).
- `pixel_avg_wtab_*`, `mc_copy_wtab_*`, `mc_weight_wtab_*`, `mc_offset{add,sub}_wtab_*` arrays of six function pointers.
- Weight-table aliases described in §5.4.
- `LOWRES_COST_SHIFT` used in the AVX-512 mbtree wrapper to build the `list_mask` argument.

---

## 8. Performance notes

- `get_ref`'s zero-copy path is critical: for integer-pel MVs with no weighting, motion estimation references pixels directly rather than copying a block.
- Quarter-pel interpolation is two source reads + one average; choosing the right half-pel planes via the tables avoids conditional branches in the hot path.
- `weight_cache` precomputes the packed vectors once per reference/frame so the kernel reads them from a 16-byte-aligned struct.
- The dispatch cascade means a high-end CPU gets AVX-512 variants for nearly everything; a rebuild must preserve the order so a missing feature does not leave a faster-than-allowed function selected.

**AVX2/AVX-512 vectorization opportunities and bottlenecks (with references):**
- AVX2 lets `mc_luma`'s averaging call use 32-byte `pixel_avg_*_avx2`, halving the number of calls for 16-wide blocks; AVX-512 goes further but only for avg, not for the C-side composition itself.
- Embedded broadcasts in AVX-512 weight kernels remove the `cachea` vector load; the C-side cache layout (two 8-word arrays) is still compatible.
- Bottleneck: `get_ref` returning a pointer requires the caller to handle a variable destination stride; a rebuild must keep that ABI contract (documented in `mc.h`).
- Bottleneck: the qpel table lookup plus two pointer computations are scalar and can stall on a wide-vector pipeline; however the actual data movement is in `pixel_avg_*`, which is fully vectorised.

---

## 9. References

- ITU-T H.264 (AVC): quarter-pel luma interpolation and weighted prediction semantics.
- `common/mc.h` in the reference tree: `x264_mc_functions_t`, `x264_weight_t`, MV validity rules.
- Intel SDM / Intrinsics Guide for the kernel instructions selected here.
- x264 project documentation on `checkasm` and CPU flag semantics.

---

## 10. Verification checklist

1. For each combination of `mvx,mvy ∈ [−16,19]`, verify `mc_luma`/`get_ref` outputs match a scalar H.264 qpel interpolator built from independently generated half-pel planes; check both the write-to-dst and zero-copy paths.
2. Verify `get_ref` sets `*i_dst` correctly when returning a source pointer.
3. Verify `weight_cache_mmx2`/`ssse3` produce cache values matching the scalar formula for random scale/denom/offset, and that `weightfn` selects offsetadd/sub vs weight correctly.
4. Verify the dispatch cascade by calling `x264_mc_init_mmx` with synthetic CPU masks (one flag at a time and combinations) and asserting each function pointer equals the expected symbol (compare addresses against the declared symbols).
5. Check table indices: widths 4/8/12/16/20 map to indices 1..5 and index 0 is never called.
6. Run `checkasm` across all ISA levels for avg/weight/offset/copy/chroma/hpel/mbtree.

---

## 11. Open questions

- Some declared functions do not exist (source comment "This declares some functions that don't exist"); UNKNOWN which table entries are therefore unused in practice — a rebuild should avoid relying on them.
- `MC_CLIP_ADD` uses an inline-asm `paddsw`; UNKNOWN whether the compiler can reorder it relative to FP code without the memory clobber on all supported compilers.
- The exact AVX-512 qpel dispatch (there is no AVX-512 `mc_luma`) is UNKNOWN; the C composition uses the AVX2 variant on AVX-512 CPUs.
