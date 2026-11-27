# `common/loongarch/deblock.h` — LoongArch deblocking declarations

**Dirty-side behavioural specification.** Source path: `E:\_x264_dirty_stash\x264-source\common\loongarch\deblock.h`. Role: declares the LSX/LASX loop-filter functions implemented in `loongarch/deblock-a.S`.

## 1. Header

The title line above gives the source path and the file's role in one sentence; the adjacent bold marker identifies this document as a **dirty-side behavioural specification** written from the reference source and public literature. No reference source or near-verbatim pseudocode is reproduced.

## 2. Purpose

Provides the declaration surface for LoongArch SIMD deblocking. Relative to the MIPS port it covers fewer variants: luma inter and intra filters only (no chroma entries), with a mix of LSX and LASX implementations, plus both LSX and LASX boundary-strength kernels. All declarations are wrapped in `#if !HIGH_BIT_DEPTH`.

## 3. Public interface

All names are mangled by `x264_template(...)`:

**Inter luma (LASX only)**
- `void x264_deblock_v_luma_lasx(uint8_t *pix, intptr_t stride, int alpha, int beta, int8_t *tc0)`
- `void x264_deblock_h_luma_lasx(uint8_t *pix, intptr_t stride, int alpha, int beta, int8_t *tc0)`

**Intra luma (both ISAs)**
- `void x264_deblock_v_luma_intra_lsx/lasx(uint8_t *pix, intptr_t stride, int alpha, int beta)`
- `void x264_deblock_h_luma_intra_lsx/lasx(uint8_t *pix, intptr_t stride, int alpha, int beta)`

**Strength (both ISAs)**
- `void x264_deblock_strength_lsx/lasx(uint8_t nnz[X264_SCAN8_SIZE], int8_t ref[2][X264_SCAN8_LUMA_SIZE], int16_t mv[2][X264_SCAN8_LUMA_SIZE][2], uint8_t bs[2][8][4], int mvy_limit, int bframe)`

Contract: identical semantics to the MIPS deblock entry points — filters operate in place along a 16-pixel luma edge, `alpha`/`beta` are H.264 thresholds, `tc0[0..3]` are per-quarter clipping thresholds with negative meaning "disable", and the strength kernel fills `bs[2][8][4]` following the H.264 boundary-strength rules. No chroma deblocking is declared here; the dispatcher must fall back to C for chroma edges.

## 4. State and data structures

No state. Include guard `X264_LOONGARCH_DEBLOCK_H`. The strength prototype depends on `X264_SCAN8_SIZE` and `X264_SCAN8_LUMA_SIZE` from generic frame/macroblock headers. The LASX strength kernel can hold 32 bytes in one register, so it processes two 16-pixel quarters at once; the LSX variant handles 16 bytes.

## 5. Algorithm / behaviour

Declarations only. The LoongArch implementations apply the same filter equations as the MIPS/C versions:

- strong intra: filtered `p0 = (2·(p0+q3+p1) + p2 + q1 + 4) >> 3`, `p1 = (p2 + threshold + 2) >> 2`, `p2 = (3·p2 + 2·p3 + threshold + 4) >> 3` (and symmetric for q-side);
- weak: `p0 = (p0 + q1 + 2·p1 + 2) >> 2`;
- inter: `delta = ((4·(q0−p0) + (p1−q1) + 4) >> 3)` clipped to `[-tc,tc]`, then `p0 += delta`, `q0 -= delta`, with the `tc += 1` rule when inner samples are filtered.

All predicates use `alpha`, `beta`, `alpha>>2+2` and `bs>0` exactly as in H.264. The LASX variants use 256-bit wide transposes (`LASX_TRANSPOSE`) and per-quarter masks spread across the wider registers; see `deblock-a.S.md`.

## 6. Dependencies

- **Included by:** `common/loongarch/deblock-a.S` and `common/deblock.c`.
- **Requires:** `x264_template`, `intptr_t`, `X264_SCAN8_*` constants.
- **Shared config:** `HIGH_BIT_DEPTH == 0` for declarations; the assembly includes `loongson_asm.S`/`loongson_util.S`.

## 7. Constants and tables

None in the header. The implementation uses shuffle constants `shuf_loc_locn` and `shuf_locn` for transposing 16x6 byte blocks; documented in `deblock-a.S.md`.

## 8. Performance notes

No runtime cost. Rebuild implication: because the dispatcher installs only these slots, a clean implementation should make sure C fallbacks remain for chroma deblocking and for the inter/intra variant not installed by the selected ISA.

## 9. References

ITU-T H.264 standard; x264 project documentation; architecture SIMD programmer's guides.

## 10. Verification checklist

1. Include header alone in 8-bit and 10-bit builds; confirm declarations vanish in the latter.
2. Verify symbol names after mangling against `deblock-a.S` object symbols (`nm`).
3. Check that the strength prototype's array dimensions match the generic typedef used by `common/deblock.c`.
4. Confirm no chroma declarations are missing that the dispatcher actually calls (fallback must exist elsewhere).

## 11. Open questions

- UNKNOWN: why no chroma deblock declarations exist in this port — verify whether `common/deblock.c` retains C chroma filters when `X264_CPU_LSX/LASX` is set.
- UNKNOWN: exact orientation mapping of `v_*` versus `h_*` in the LoongArch assembly relative to the MIPS naming; confirm with the dispatcher.
