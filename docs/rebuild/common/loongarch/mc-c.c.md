# `common/loongarch/mc-c.c` — LoongArch motion-compensation dispatcher

**Dirty-side behavioural specification.** Source path: `E:\_x264_dirty_stash\x264-source\common\loongarch\mc-c.c`. Role: C-side glue that builds weight-function tables, defines `get_ref`/`mc_luma` wrappers, generates plane-copy helpers via macros, and installs all LSX/LASX implementations into the generic MC function table.

## 1. Header

The title line above gives the source path and the file's role in one sentence; the adjacent bold marker identifies this document as a **dirty-side behavioural specification** written from the reference source and public literature. No reference source or near-verbatim pseudocode is reproduced.

## 2. Purpose

The heavy lifting is in `mc-a.S`; this file wires it up. It defines the `x264_weight_t.weightfn` selection logic (denominator ≥ 1 vs zero), the width-indexed average/copy/weight tables, and the `get_ref`/`mc_luma` dispatch that resolves quarter-pel references. It also emits the plane-copy/YUYV helpers with the `PLANE_INTERLEAVE` and `PLANE_COPY_YUYV` macros, and exposes `x264_mc_init_loongarch` as the single entry point for architecture initialization.

## 3. Public interface

- `void x264_mc_init_loongarch(int cpu, x264_mc_functions_t *pf)` — declared in `loongarch/mc.h`.
- `void x264_mc_chroma_lsx(...)` and `x264_mc_chroma_lasx(...)` — forward declarations of the assembly chroma interpolators (the definitions are in `mc-a.S`), present here so the C dispatcher can reference them.

All other definitions are `static`.

## 4. State and data structures

**Static tables (all file-local):**

- `mc_wtab_lsx[6]` / `mc_wtab_lasx[6]` — normal weighted-prediction functions indexed by width code (4,4,8,16,16,20).
- `mc_noden_wtab_lsx/_lasx[6]` — no-denominator variants.
- `pixel_avg_wtab_lsx/lasx[6]` — width-indexed averaging functions; slot 0 is `NULL` (unused).
- `mc_copy_wtab_lsx/lasx[5]` — width-indexed copy functions; slots 0 and 3 are `NULL` (width 12 and unused).
- `mc_wtab_*` are assembled by the `MC_WEIGHT` macros from the individual `x264_mc_weight_w{4,8,16,20}{,_noden}_{lsx,lasx}` symbols.

**Weight cache:** `weight_cache_lsx/lasx(x264_t *h, x264_weight_t *w)` sets `w->weightfn = mc_wtab_*` when `w->i_denom >= 1`, otherwise `mc_noden_wtab_*`. The `h` parameter is unused.

**Generated helpers:** `PLANE_INTERLEAVE(lsx)` and `PLANE_COPY_YUYV(32, lsx)` / `PLANE_COPY_YUYV(64, lasx)` expand to `plane_copy_interleave_lsx`, `plane_copy_deinterleave_yuyv_lsx`, `plane_copy_deinterleave_yuyv_lasx` (the 32/64 argument is the vector width in bytes).

## 5. Algorithm / behaviour

### `get_ref_*` and `mc_luma_*`

Both variants are structurally identical:

1. Compute `r_vy = mvy & 3`, `r_vx = mvx & 3`, `qpel_idx = (r_vy<<2) + r_vx`, `offset = (mvy>>2)*src_stride + (mvx>>2)`.
2. Choose `src1 = src[x264_hpel_ref0[qpel_idx]] + offset + (r_vy==3 ? src_stride : 0)`.
3. If `qpel_idx & 5` (any half-pel component), choose `src2 = src[x264_hpel_ref1[qpel_idx]] + offset + (r_vx==3 ? 1 : 0)`, call `pixel_avg_wtab[width](dst, dst_stride, src1, src_stride, src2, height)`, then if `pWeight->weightfn` is set, apply it in place: `weightfn[width](dst, dst_stride, dst, dst_stride, pWeight, height)`.
4. Else if a weight function is set, apply it from `src1` to `dst`.
5. Else `get_ref` sets `*dst_stride = src_stride` and returns `src1` (zero-copy); `mc_luma` uses `mc_copy_wtab[width]` to copy into `dst`.

`width = i_width >> 2` indexes the tables (4→1, 8→2, 16→4, 20→5). `get_ref` returns `p_dst` in the averaging/weighting cases and `p_src1` in the zero-copy case.

### Initialization

`x264_mc_init_loongarch` first installs LSX slots when `cpu & X264_CPU_LSX` (mc_luma, mc_chroma, get_ref, all nine avg entries, weight tables, weight_cache, copy slots, chroma interleave/deinterleave, plane copies, hpel_filter, memcpy/memzero, frame_init_lowres, three prefetch slots), then installs LASX slots when `cpu & X264_CPU_LASX` (mc_luma, mc_chroma, get_ref, avg entries 16x8 through 4x2 — **not** 16x16, weight tables, weight_cache, plane deinterleave + YUYV, copy slots, hpel_filter, memzero, frame_init_lowres). LASX therefore overrides most but not all LSX entries.

## 6. Dependencies

- **Calls:** the assembly symbols declared in `loongarch/mc.h`; `x264_hpel_ref0/1` tables from `common/mc.c`; generic macros `PLANE_INTERLEAVE`, `PLANE_COPY_YUYV` from `common/mc.h`/`macroblock.h`.
- **Called by:** `x264_mc_init()` in `common/mc.c`.
- **Shared config:** `HIGH_BIT_DEPTH` gates the whole implementation; `X264_CPU_LSX`/`X264_CPU_LASX` flags.

## 7. Constants and tables

- Width-index tables `[6]` and `[5]` with the layout described above.
- The `MC_WEIGHT`/`MC_WEIGHT_LSX` macros generate four 6-entry tables from eight base function names.
- Denominator threshold **1** selects normal vs no-denominator weighting.
- `PLANE_COPY_YUYV(32, ...)` / `(64, ...)` reflect LSX/LASX vector widths in bytes.

## 8. Performance notes

- **Hotness:** init runs once; the installed tables are on every inter macroblock's critical path.
- **Optimization tips:** (a) the `get_ref` zero-copy path avoids a block copy — preserve it; (b) `weight_cache` should be invoked once per weight configuration, not per macroblock (the generic code does this); (c) on LASX CPUs the wider avg/filter/copy kernels are installed — keep the override list accurate; (d) width-code lookup via `i_width>>2` is a single shift; do not replace it with a branch chain.
## 9. References

x264 `common/mc.c`, `mc.h`; ITU-T H.264 §8.4.

## 10. Verification checklist

1. Call `x264_mc_init_loongarch` with each CPU flag combination and inspect all `pf` slots for expected values.
2. Verify `weight_cache` picks the normal table for `i_denom >= 1` and no-denominator otherwise.
3. For all 16 qpel combinations and widths 4/8/16/20, compare `get_ref`/`mc_luma` behaviour with a scalar MC reference.
4. Verify that the zero-copy path leaves the destination buffer untouched and returns a pointer with the source stride.
5. Round-trip plane interleave/deinterleave and YUYV deinterleave with random data.
6. Confirm generated `plane_copy_deinterleave_yuyv_*` symbols exist after macro expansion.

## 11. Open questions

- UNKNOWN: exact contents of the `PLANE_INTERLEAVE` and `PLANE_COPY_YUYV` macros (defined in generic headers) — a clean rebuild must obtain them from `common/mc.h`/`common/frame.h`, not guess them.
- UNKNOWN: why `avg[PIXEL_16x16]` is not overridden by LASX — likely because no LASX 16x16 avg symbol exists; verify from `mc-a.S` symbol list.
- UNKNOWN: behaviour of `get_ref` for `i_width` values other than 4/8/12/16/20 (table index out of range) is undefined by this API.
