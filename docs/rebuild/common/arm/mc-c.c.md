# `common/arm/mc-c.c` — ARM motion-compensation orchestration

## 1. Header

- **Source path:** `common/arm/mc-c.c`.
- **Role in one sentence:** C-level glue that selects among NEON primitives to implement full luma motion compensation, reference get, weighted prediction caching, half-pel filtering, and dispatch installation.
- **Marker:** Dirty-side behavioural specification; all descriptions are behavioural and written independently of the reference source.


## 2. Purpose

The assembly kernels in `mc-a.S` operate on fixed widths (4, 8, 16, 20). This file supplies the higher-level logic that interprets a quarter-pel motion vector, chooses the right pair of half-pel reference planes, invokes the averaging or copying kernel, applies weighted prediction, and installs the complete set of ARM motion-compensation pointers. It also implements memory helpers and low-resolution frame preparation by calling the relevant assembly primitives.

## 3. Public interface

- `void x264_mc_init_arm( uint32_t cpu, x264_mc_functions_t *pf )` — the sole exported initializer, declared in `arm/mc.h`.

Everything else in the file is file-local:
- `mc_luma_neon(...)` and `get_ref_neon(...)` implement luma interpolation.
- `hpel_filter_neon(...)` orchestrates vertical, central, and horizontal half-pel filters over a frame row.
- `weight_cache_neon(...)` chooses the fast weighted-prediction path.
- Static tables `pixel_avg_wtab_neon` and `mc_copy_wtab_neon` dispatch by width.
- Five `MC_WEIGHT` macro expansions create the four weighted-prediction function tables.

## 4. State and data structures

`src[4]` is the caller-provided array of half-pel planes: index 0 is the integer-pel plane, 1 the horizontally interpolated plane, 2 the vertically interpolated plane, and 3 the centre-interpolated plane. `x264_weight_t` carries `i_scale`, `i_denom`, `i_offset`, `cachea`, and a `weightfn` table pointer. The layout matters to the assembly: fields are aligned to 16 bytes, with `i_denom` at byte offset 32, `i_scale` at 36, and `i_offset` at 40.

## 5. Algorithm / behaviour

**Quarter-pel selection.** The low two bits of each motion-vector component select the interpolation phase, combined as `qpel_idx = ((mvy & 3) << 2) + (mvx & 3)`. The integer part is `offset = (mvy >> 2) * stride + (mvx >> 2)`. Two lookup tables, `x264_hpel_ref0` and `x264_hpel_ref1`, map the 16 phases to source-plane indices:

- `ref0 = {0,1,1,1, 0,1,1,1, 2,3,3,3, 0,1,1,1}`
- `ref1 = {0,0,1,0, 2,2,3,2, 2,2,3,2, 2,2,3,2}`

The primary pointer is `src[ref0[qpel_idx]] + offset`, with an extra row added when the vertical phase is 3. When `qpel_idx & 5` is nonzero, a second pointer is formed from `ref1` with an extra column when the horizontal phase is 3, and the two planes are averaged with rounded arithmetic. Otherwise the block is copied directly.

**Luma motion compensation.** `mc_luma_neon` writes the result to the destination. It dispatches the averaging or copying operation through `pixel_avg_wtab_neon[width>>2]` or `mc_copy_wtab_neon[width>>2]`. Widths 12 and 16 share the 16-wide averaging routine; the copy table has no entry for 12 and relies on 16-wide copy only for aligned data.

**Reference get.** `get_ref_neon` performs the same computation but may return a pointer directly into the reference frame when no interpolation or weighting is needed; in that case it updates `*i_dst_stride` to the reference stride. This zero-copy path is a major performance optimization for integer-pel vectors.

**Weighted prediction.** Four specialized variants are generated: plain scaled weighting, no-denominator scaling, offset addition, and offset subtraction. `weight_cache_neon` selects them as follows: if `i_scale == 1 << i_denom`, the operation is a pure offset and the sign of `i_offset` selects `offsetadd` or `offsetsub`, with the magnitude stored in `cachea[0]`; otherwise zero denominator selects `nodenom`; otherwise the full `weight` path is chosen.

**Half-pel filtering.** `hpel_filter_neon` realigns all pointers by `(uintptr_t)src & 15`, widens the nominal width by that alignment offset, and processes each row with three kernels: vertical filtering into `dstv`, central filtering into `dstc`, and horizontal filtering into `dsth`. The vertical and horizontal half-pel filters implement the standard 5-tap `(a - 5b + 20c + 16) >> 5` clipped to the byte range. The central filter implements `(a - 5b + 20c + 32) >> 6` clipped similarly.

**Initialization.** `x264_mc_init_arm` first checks `X264_CPU_ARMV6`, installs ARMv6 prefetch helpers (8-bit only), then checks `X264_CPU_NEON` and installs all NEON overrides, and finally installs the aligned memory helpers (skipping `memcpy_aligned` on macOS-style builds).

## 6. Dependencies

- Includes `common/common.h` and local `mc.h`.
- Declares and calls roughly fifty assembly symbols from `arm/mc-a.S`, including prefetch, memory helpers, pixel averages, weighted prediction, copies, chroma interpolation, half-pel filters, low-res preparation, plane-copy helpers, integral initialization, and MB-tree helpers.
- Uses the generic `PLANE_COPY`, `PLANE_COPY_SWAP`, `PLANE_INTERLEAVE`, and `PROPAGATE_LIST` macros from `common/mc.h` to build wrapper functions around the assembly cores.

## 7. Constants and tables

- `FENC_STRIDE = 16`, `FDEC_STRIDE = 32`.
- `x264_hpel_ref0`/`x264_hpel_ref1` phase tables (listed above).
- `pixel_avg_wtab_neon[6] = {NULL, w4, w8, w16, w16, w20}` indexed by `width >> 2`.
- `mc_copy_wtab_neon[5] = {NULL, w4, w8, NULL, w16}`.
- Half-pel filter constants 5 and 20, rounding constants 16 and 32, shift counts 5 and 6.

## 8. Performance notes

The zero-copy reference path in `get_ref_neon` is the most valuable optimization here: it avoids copying an entire block for integer-pel candidates. The phase lookup tables remove branches from interpolation. Weighted-prediction caching collapses common scale/offset combinations into saturating byte add/sub loops with no multiply. All paths process rows with vector loads/stores; alignment is handled by explicit realignment in the half-pel filter. When rebuilding, preserve these high-level decisions even if the SIMD implementation differs.

## 9. References

- `common/mc.c` for the canonical luma/chroma interpolation semantics.
- `common/mc.h` for function-pointer table layout and plane-copy macro contracts.
- ITU-T H.264, clause on fractional-pel interpolation and weighted prediction.

## 10. Verification checklist

- For each of the 16 quarter-pel phases, `mc_luma_neon` matches the generic `mc_luma_c`.
- `get_ref_neon` returns a direct reference pointer only when no interpolation/weighting is needed, and writes correct strides otherwise.
- All five width variants (4, 8, 12/16, 20) produce identical results to generic averaging.
- Weighted prediction matches the generic formula for scale/denom/offset combinations, including negative offsets.
- Half-pel filter output matches generic filters including edge pixels.
- Plane-copy and deinterleave helpers match their generic counterparts for all widths.

## 11. Open questions

- `UNKNOWN`: exact semantics of the 4:2:2 source prefetch, explicitly marked as an approximation in the reference source.
- `UNKNOWN`: whether width 12 ever reaches `mc_luma_neon`; the copy table has no 12-wide entry, suggesting it may not.
