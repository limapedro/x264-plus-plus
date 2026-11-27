# `common/ppc/mc.c` — PowerPC motion-compensation implementation

## 1. Header

- **Source path:** `common/ppc/mc.c`.
- **Role in one sentence:** AltiVec motion-compensation primitives and the initializer that installs them, including luma interpolation, chroma interpolation, half-pel filters, weighted prediction, plane copying, integral-image preparation, and MB-tree support.
- **Marker:** Dirty-side behavioural specification; all descriptions are behavioural and written independently of the reference source.


## 2. Purpose

This file contains the PowerPC equivalents of the routines in `common/arm/mc-c.c` and `common/arm/mc-a.S`, implemented with AltiVec intrinsics. It is compiled in two distinct modes: an 8-bit path with a full set of motion-compensation overrides, and a high-bit-depth path that provides only a v210 plane deinterleave routine.

## 3. Public interface

Exported symbols from `ppc/mc.h`:

- `void x264_mc_init_altivec( x264_mc_functions_t *pf )`

Additional exported primitives declared locally or in headers:

- `x264_plane_copy_swap_core_altivec`, `x264_plane_copy_interleave_core_altivec`.
- `x264_plane_copy_deinterleave_rgb_altivec` when VSX is available.
- `x264_plane_copy_deinterleave_v210_altivec` in high-bit-depth builds.
- `x264_hpel_filter_altivec`.

Internal functions include `pixel_avg2_w4/w8/w16/w20_altivec`, `mc_copy_w4/w8/w16(_aligned)_altivec`, `mc_luma_altivec`, `get_ref_altivec`, three chroma interpolation helpers, weighted-prediction width variants, `frame_init_lowres_core_altivec`, and `load_deinterleave_chroma_fenc_altivec`.

## 4. State and data structures

`x264_weight_t` fields are read directly: `i_denom`, `i_scale`, `i_offset`. The weight function table is indexed by `width >> 2`. Source plane arrays have four entries as on other architectures. Plane-copy helpers operate on caller-owned buffers with explicit strides. The v210 routine reads packed 32-bit words and writes 16-bit luma/chroma planes.

## 5. Algorithm / behaviour

**Block averaging.** Four widths are supported. Width 4 either uses scalar rounding or `vec_avg` with a length-limited store on POWER9. Width 8 uses `VEC_STORE8`, width 16 uses a full vector store, and width 20 combines the two. All compute `(a+b+1)>>1` via `vec_avg`.

**Block copy.** Widths 4 and 8 use `memcpy` or VSX length-limited stores; width 16 loads and stores one vector per row. The aligned variant uses `vec_ld`/`vec_st`.

**Luma interpolation.** `mc_luma_altivec` and `get_ref_altivec` follow the same quarter-pel phase selection as the ARM implementation: `qpel_idx = ((mvy&3)<<2)+(mvx&3)`, integer offset from the high bits of the motion vector, base pointer from `x264_hpel_ref0`, second pointer from `x264_hpel_ref1` when `qpel_idx & 5` is nonzero, with additional row/column adjustments for phase 3. Averaging is followed by optional weighted prediction. `get_ref_altivec` can return a direct reference pointer when no interpolation or weighting is needed.

**Chroma interpolation.** The bilinear formula uses weights `(8-dx)*(8-dy)`, `dx*(8-dy)`, `(8-dx)*dy`, `dx*dy` with a final `(sum+32)>>6`. Three implementations cover widths 2, 4, and 8. The width-4 and width-8 variants load two rows, align the odd sample positions with `vec_sld`, multiply-accumulate with `vec_mladd`, and use byte permutations to separate U and V results. The width-2 variant is scalar.

**Half-pel filtering.** `x264_hpel_filter_altivec` processes rows with three stages. Horizontal filtering forms `(a-5b+20c+16)>>5` from six horizontally shifted 16-bit samples. Vertical filtering applies the same formula to six rows. Central filtering computes `(a-5b+20c+32)>>6` from vertically filtered values held in registers. Constants are 5 and 20 with shifts 5 and 2; rounding constants 16 and 32.

**Low-resolution preparation.** `frame_init_lowres_core_altivec` averages adjacent source rows and columns to produce four output planes: full average, horizontal-only, vertical-only, and centre. It processes sixteen output columns at a time using merge and permutation masks, with a tail path for widths not divisible by 16.

**Weighted prediction.** Five width variants (2, 4, 8, 16, 20) implement both a denominator path `(scale*src + round) >> denom + offset` and a no-denominator path `scale*src + offset`, saturating with `vec_packsu`. The width-20 variant uses a mixed vector because four of its twenty bytes are handled separately.

**Plane helpers.** Swap, interleave, and deinterleave routines reorder bytes with fixed permutation masks. The 4:2:2 and RGB variants handle interleaved chroma or RGB input. The RGB routine has separate paths for 3- and 4-byte pixel widths and is VSX-only.

**Integral and MB-tree preparation.** The reference file declares integral helpers in ARM but the PowerPC implementation does not expose equivalents; the PowerPC initializer installs no integral pointers. High-bit-depth v210 deinterleave extracts 10-bit samples from packed 32-bit words using three selector masks and three shift patterns per 24-pixel group.

**Initializer.** `x264_mc_init_altivec` installs all 8-bit pointers except chroma load-fdec and some prefetch hooks, and installs only the v210 routine in high-bit-depth builds.

## 6. Dependencies

- Includes `common/common.h`, `ppccommon.h`, local `mc.h`, and uses macros from `common/mc.h`.
- Called from the generic motion-compensation initialization path.
- Uses `x264_weight_t` layout from `common/mc.h` and pixel-size enums from `common/pixel.h`.

## 7. Constants and tables

- `x264_hpel_ref0 = {0,1,1,1,0,1,1,1,2,3,3,3,0,1,1,1}`; `x264_hpel_ref1 = {0,0,1,0,2,2,3,2,2,2,3,2,2,2,3,2}`.
- Half-pel constants 5, 20; shifts 5 and 6; rounding 16 and 32.
- Chroma denominator 64 with rounding 32.
- Numerous byte-permutation masks for U/V separation, RGB extraction, and v210 unpacking.
- `HAVE_VSX` controls availability of unaligned load/store and RGB deinterleave.

## 8. Performance notes

The implementation avoids emulated unaligned loads where possible by using `vec_vsx_ld` on VSX-capable hardware. Weighted prediction saturates in vector registers with `vec_packsu`. The chroma kernels process two output rows per loop iteration to amortize loads. The v210 path processes 24 pixels per iteration with three 128-bit loads. Clean implementations should preserve the zero-copy `get_ref` path and the exact rounding constants; these matter more than micro-optimizing the shuffle sequences.

## 9. References

- `common/mc.c` for canonical interpolation semantics.
- `common/mc.h` for function-pointer and weight-table definitions.
- ITU-T H.264 fractional interpolation and weighted prediction clauses.
- AltiVec/VSX documentation for `vec_mladd`, `vec_vsx_ld`, and `vec_xst_len`.

## 10. Verification checklist

- All quarter-pel phases match the generic luma interpolation.
- Chroma interpolation matches generic results for all 64 fractional offsets and widths 2/4/8.
- Half-pel filters match generic output including edges.
- Weighted prediction matches the scalar formula for all supported widths and scale/offset combinations.
- Plane-copy helpers match generic results for all input formats.
- v210 deinterleave matches expected 10-bit sample extraction.
- Repeated initializer calls produce identical tables.

## 11. Open questions

- `UNKNOWN`: exact build conditions under which the RGB deinterleave pointer is installed.
- `UNKNOWN`: whether any of the declared internal helpers are used outside this file.
