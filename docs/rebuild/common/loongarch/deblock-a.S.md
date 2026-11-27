# `common/loongarch/deblock-a.S` — LoongArch LSX/LASX deblocking filter

**Dirty-side behavioural specification.** Source path: `E:\_x264_dirty_stash\x264-source\common\loongarch\deblock-a.S`. Role: assembly implementations of the luma loop filter (intra and inter, vertical and horizontal edges) and the boundary-strength computation, in LSX and LASX variants. Compiled only for 8-bit builds.

## 1. Header

The title line above gives the source path and the file's role in one sentence; the adjacent bold marker identifies this document as a **dirty-side behavioural specification** written from the reference source and public literature. No reference source or near-verbatim pseudocode is reproduced.

## 2. Purpose

Deblocking runs on every reconstructed macroblock edge and directly affects both quality and bitstream (filtered frames become references). This file provides wide-vector versions of the filter equations, using LASX's 256-bit registers to handle two 16-pixel quarters at once and dedicated byte-transpose macros for vertical edges.

## 3. Public interface

Exports (symbols prefixed with `ASM_REF`), declared in `loongarch/deblock.h.md`:

- `deblock_h_luma_lasx`, `deblock_v_luma_lasx` — inter luma filters (LASX only), signature `(uint8_t *pix, intptr_t stride, int alpha, int beta, int8_t *tc0)`.
- `deblock_v_luma_intra_lsx/lasx`, `deblock_h_luma_intra_lsx/lasx` — intra luma filters, `(pix, stride, alpha, beta)`.
- `deblock_strength_lsx/lasx` — boundary strength, `(nnz, ref, mv, bs, mvy_limit, bframe)`.

Contract: same H.264 semantics as the MIPS deblock kernels — in-place filtering of 16-pixel edges, `alpha`/`beta` thresholds, `tc0[0..3]` per-quarter with negative disabling that quarter, and `bs[2][8][4]` strength output. No chroma filters are implemented here.

## 4. State and data structures

No globals. Working vectors hold the eight samples `p3,p2,p1,p0,q0,q1,q2,q3` around the edge. For horizontal edges (`h_*`) these are contiguous 16-byte rows loaded with stride; for vertical edges (`v_*`) they are assembled by transposing a 16x8 byte block using the local `LASX_TRANSPOSE` macro (a 16-input → 6-output byte transpose built from `xvilvl.b` and `xvpermi.d`).

Per-quarter `tc`/`bs` values are broadcast into word lanes via `xvldrepl.w` (LASX) or `vldrepl.w` (LSX). Predicate masks are byte vectors; `bmnz_v`/`xvbitsel` style instructions merge filtered values only where the mask is set.

## 5. Algorithm / behaviour

### Intra luma filter

1. Broadcast `alpha` and `beta`.
2. Load p1,p0,q0,q1 rows; compute `|p0−q0| < alpha`, `|p1−p0| < beta`, `|q1−q0| < beta`; combine into a global predicate. If all lanes fail, return.
3. Load p2,p3,q2,q3; compute the strong-filter condition `|p2−p0| < beta` and `|p0−q0| < (alpha>>2)+2`.
4. For qualifying lanes, apply the strong equations with rounding shifts: `p0' = (2·(p0+q3+p1) + p2 + q1 + 4) >> 3`, `p1' = (p2 + (p0+q3+p1) + 2) >> 2`, `p2' = (3·p2 + 2·p3 + (p0+q3+p1) + 4) >> 3`, and the symmetric q-side formulas.
5. For the remaining qualifying lanes, apply the weak filter `p0' = (p0 + q1 + 2·p1 + 2) >> 2`.
6. Mask-merge results into the original rows and store them back.
7. The vertical variants transpose before filtering and re-transpose/reinterleave before storing; the shuffle constants `shuf_loc_locn` and `shuf_locn` map each quarter's samples to the correct vector lanes across the transpose.

### Inter luma filter

1. Build `bs` and `tc` vectors from the four caller values; if all `bs` are zero, return.
2. Compute the combined predicate including `bs > 0` and the `alpha`/`beta` tests.
3. Correction of inner samples: `clip3 = clip(ave(p0,q0) − 2·p1 averaged with p2, −tc, +tc)`, then `p1' = p1 + clip3`. Where p1 was modified, `tc += 1`.
4. Boundary pair: `delta = ((4·(q0−p0) + (p1−q1) + 4) >> 3)` clipped to `[−tc, tc]`, then `p0' = clip(p0+delta)`, `q0' = clip(q0−delta)`.
5. Mask-merge and store all four rows.

### Strength computation

`deblock_strength_lsx/lasx` implement the H.264 boundary-strength rules in vector form:

- strength 2 if `nnz[loc] | nnz[locn] != 0`;
- else strength 1 if `ref0[loc] != ref0[locn]` or `|mvx diff| >= 4` or `|mvy diff| >= mvy_limit`, plus the analogous list-1 test when `bframe != 0`;
- else 0.

The LSX variant processes 16 bytes (four quarters) per vector; the LASX variant processes 32 bytes (two edges). Neighbour values are aligned using `xvpermi`/`vshuf` and splatted word lanes; results are extracted to GPRs and stored into the `bs[2][8][4]` array. The B-frame list-1 condition may be handled by a scalar path or an additional vector pass — see open questions.

## 6. Dependencies

- **Calls:** `loongson_asm.S` clip/compare/dot-product macros, `loongson_util.S` function macros; raw LSX/LASX intrinsics via inline assembly (`xvilvl.b`, `xvpermi.d`, `xvldrepl.w`, `vadda.h`, `vclip`, `xvmax.h`, `xvmin.h`, `xvbitsel`).
- **Called by:** `x264_deblock_init()` in `common/deblock.c`.
- **Shared config:** `HIGH_BIT_DEPTH == 0`, `BIT_DEPTH`, `X264_CPU_LSX`/`LASX`.

## 7. Constants and tables

- `shuf_loc_locn` (32 bytes): indices `{1,9,17,25, 2,10,18,26, 3,11,19,27, 4,12,20,28, 16,24,0,8, 17,25,1,9, 18,26,2,10, 19,27,3,11}` — maps the 16x6 transposed block's sample positions to quarter lanes.
- `shuf_locn` (16 bytes): `{0,8,16,24, 1,9,17,25, 2,10,18,26, 3,11,19,27}` — neighbour-location gather.
- Filter constants: `alpha>>2 + 2`, rounding shifts `>>2`/`>>3`, clip bounds `[−tc,tc]` and `[0,255]`.
- Strength thresholds: `4` (horizontal MV), `mvy_limit` (vertical MV).

## 8. Performance notes

- **Hotness:** very high; four edges per macroblock per frame.
- **SIMD status:** fully LSX/LASX with mask-based conditional execution; only the all-zero early-out branches.
- **Optimization tips:** (a) the all-zero predicate early-out is the biggest win — keep predicate computation cheap and before any loads of p2/p3; (b) the byte transpose for vertical edges is expensive — reuse the `LASX_TRANSPOSE` macro's register allocation to avoid spills; (c) on LASX, use 256-bit loads to process two quarters at once; (d) B-frame strength's list-1 path is a likely bottleneck — a fully vectorized version would help B-heavy content.
## 9. References

ITU-T H.264 §8.4.3/§8.4.2; x264 `common/deblock.c`; LoongArch ISA manuals.

## 10. Verification checklist

1. Compare each exported filter against the scalar reference for deterministic edge patterns covering no-filter, weak, strong and inter paths.
2. Verify `tc0 < 0` disables only the corresponding quarter.
3. Check rounding at half-way boundaries for the `>>2`/`>>3` filters.
4. Verify LSX and LASX variants produce identical output for the same inputs.
5. Chroma is absent — confirm the dispatcher falls back to C for chroma edges.
6. Strength: compare `bs` with the scalar computation for P and B frames and all nnz/ref/mv patterns.
7. ASan-style bounds check around the 16x8 edge block for vertical kernels.

## 11. Open questions

- UNKNOWN: whether the B-frame strength path is vectorized or scalar in this version — inspect the tail of `deblock_strength_lasx` before assuming.
- UNKNOWN: exact orientation of `v_*` vs `h_*` in this port relative to the macroblock edge definition; verify with the dispatcher.
- UNKNOWN: whether `shuf_loc_locn`'s second 16 bytes are used for the q-side only or for both sides in the LASX kernel; validate with a known asymmetric pattern.
