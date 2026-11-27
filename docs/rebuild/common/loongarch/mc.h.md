# `common/loongarch/mc.h` — LoongArch motion-compensation declarations

**Dirty-side behavioural specification.** Source path: `E:\_x264_dirty_stash\x264-source\common\loongarch\mc.h`. Role: declares the LSX/LASX motion-compensation, weighted-prediction, block-average/copy, plane and memory helpers, plus the dispatcher `x264_mc_init_loongarch`.

## 1. Purpose

This header is the larger of the two architecture MC interfaces: unlike MIPS, it exposes every internal helper used by the generic MC dispatcher because the implementation lives in assembly (`mc-a.S`) and the C file (`mc-c.c`) only wires them up. It declares LSX and LASX variants of each primitive, with some operations available only on one ISA.

## 2. Public interface

**Dispatcher**
- `void x264_mc_init_loongarch(int cpu, x264_mc_functions_t *pf)` — installs LSX functions when `cpu & X264_CPU_LSX`, then LASX functions when `cpu & X264_CPU_LASX` (LASX path overrides overlapping slots).

**Block average (explicit WxH variants)**
- `x264_pixel_avg_16x16_lsx`, `16x8`, `8x16`, `8x8`, `8x4`, `4x16`, `4x8`, `4x4`, `4x2` — and LASX variants for all except 16x16 (LSX-only there).
- Signature: `void (pixel *dst, intptr_t dstride, pixel *src0, intptr_t s0, pixel *src1, intptr_t s1, int weight)` — the last argument is the blend weight in `[0,63]`, with 32 meaning equal average.

**Width-based average helpers (used as vtable entries)**
- `x264_pixel_avg2_w4/w8/w16/w20_lsx` and `_lasx` — `void (uint8_t*, intptr_t, uint8_t*, intptr_t, uint8_t*, int)`.

**Weighted prediction**
- `x264_mc_weight_w4/w8/w16/w20_lsx` and `_lasx`, plus `_noden_lsx/_lasx` variants for each width.
- Signature: `void (pixel *dst, intptr_t dstride, pixel *src, intptr_t sstride, const x264_weight_t *weight, int height)`.
- The `_noden` variants handle weights whose denominator is zero (no rounding shift).

**Copy / memory**
- `x264_mc_copy_w4/w8/w16_lsx` and `_lasx` — `void (uint8_t *dst, intptr_t dstride, uint8_t *src, intptr_t sstride, int height)`.
- `x264_memcpy_aligned_lsx` — `void* (void *dst, const void *src, size_t n)`.
- `x264_memzero_aligned_lsx/_lasx` — `void (void *dst, size_t n)`.

**Chroma plane transform helpers**
- `x264_store_interleave_chroma_lsx`, `x264_load_deinterleave_chroma_fenc_lsx`, `x264_load_deinterleave_chroma_fdec_lsx`.
- `x264_plane_copy_interleave_core_lsx/_lasx`, `x264_plane_copy_deinterleave_lsx/_lasx`, and `x264_plane_copy_deinterleave_yuyv_lsx/_lasx` (declared/used internally via `PLANE_COPY_YUYV` macro).
- `x264_frame_init_lowres_core_lsx/_lasx`.

**Prefetch**
- `x264_prefetch_fenc_420_lsx`, `x264_prefetch_fenc_422_lsx`, `x264_prefetch_ref_lsx`.

**Filtering**
- `x264_hpel_filter_lsx/_lasx` — `void (pixel *dsth, pixel *dstv, pixel *dstc, pixel *src, intptr_t stride, int width, int height, int16_t *buf)`.
- `x264_mc_chroma_lsx/_lasx` — `void (uint8_t *dstu, uint8_t *dstv, intptr_t dstride, uint8_t *src, intptr_t sstride, int mvx, int mvy, int width, int height)`.

## 3. State and data structures

No state. Include guard `X264_LOONGARCH_MC_H`. Types: `pixel` (uint8_t in 8-bit builds, uint16_t in high-bit-depth), `x264_weight_t`, `x264_mc_functions_t`. The `int cpu` dispatcher parameter is a bitmask of `X264_CPU_LSX`/`X264_CPU_LASX` and related flags.

## 4. Algorithm / behaviour

Declarations only. The implementations follow the generic x264 MC semantics:

- **Average:** `dst = (src0 + src1 + 1) >> 1` for weight 32; otherwise `(w·src0 + (64−w)·src1 + round) >> 6` with clipping to the pixel range. Width-based helpers operate on 4/8/16/20 bytes per row for `height` rows.
- **Weighting:** `dst = clip((src·scale + offset + rounding) >> log2_denom, 0, pix_max)`; `_noden` skips the shift.
- **Copy:** straightforward strided row copies using vector loads/stores.
- **hpel_filter:** generates horizontal-only, vertical-only and center 6-tap half-pel planes from a full-pel luma plane.
- **mc_chroma:** 1/8-pel bilinear interpolation of interleaved chroma into separate U/V planes.
- **plane_copy_deinterleave_yuyv:** splits interleaved YUYV into Y and UV planes.
- **prefetch:** issues architecture-specific prefetch hints for future macroblock rows; no output.

## 5. Dependencies

- **Included by:** `common/loongarch/mc-c.c` and `common/mc.c`.
- **Requires:** `x264_mc_functions_t`, `x264_weight_t`, `pixel`, `x264_template`, `intptr_t`, `size_t` from generic headers.
- The assembly implementations include `loongson_asm.S` and `loongson_util.S` for register aliases and macros.

## 6. Constants and tables

None in the header. Constants used by the implementations (6-tap coefficients, shift amounts, mask tables) are documented in `mc-a.S.md`.

## 7. Performance notes

No runtime cost. Important for rebuild: the dispatcher installs LSX first, then LASX; slots not overridden by LASX (e.g. 16x16 avg, prefetch) remain LSX even on LASX CPUs. This mixed installation is intentional and must be preserved.

## 8. Verification checklist

1. Include header alone under 8-bit and 10-bit builds; check type definitions resolve.
2. Verify every declared symbol exists in `mc-a.S` after `x264_template` expansion.
3. Call `x264_mc_init_loongarch` with LSX-only, LASX-only and both flags; verify the expected non-null slots and the LASX override pattern.
4. Confirm no high-bit-depth-only declarations are missing (e.g. wide-pixel copy variants).

## 9. Open questions

- UNKNOWN: exact set of slots overridden by the LASX block in `mc-c.c` — verify against `x264_mc_functions_t` layout to avoid accidentally leaving a C fallback in place.
- UNKNOWN: whether `x264_plane_copy_deinterleave_yuyv_lsx/_lasx` are declared elsewhere (the header does not list them explicitly; they are generated by the `PLANE_COPY_YUYV` macro in `mc-c.c`).
