# `common/loongarch/dct-a.S` — LoongArch LSX/LASX transform and zig-zag

**Dirty-side behavioural specification.** Source path: `E:\_x264_dirty_stash\x264-source\common\loongarch\dct-a.S`. Role: assembly implementations of the 4x4/8x8 forward and inverse H.264 integer transforms, DC Hadamard transforms, DC-only inverse add, and 4x4 zig-zag scan, in both 128-bit LSX and 256-bit LASX variants.

## 1. Header

The title line above gives the source path and the file's role in one sentence; the adjacent bold marker identifies this document as a **dirty-side behavioural specification** written from the reference source and public literature. No reference source or near-verbatim pseudocode is reproduced.

## 2. Purpose

Transforms are executed for every residual and reconstructed block; the LoongArch port uses hand-tuned assembly to keep the butterfly networks and transposes in registers, exploiting LASX's 256-bit vectors for 8x8 and 16x16 blocks. The file mirrors the algorithm set of `mips/dct-c.c` but with a different function split: some routines exist only in LSX form (4x4 DCT/IDCT) because a 4x4 block fits in 128 bits.

## 3. Public interface

Exports (symbols prefixed by `ASM_REF`, i.e. `x264_8_...` in 8-bit builds), declared in `loongarch/dct.h.md`:

- Forward: `sub4x4_dct_lsx`, `sub8x8_dct_lsx/lasx`, `sub16x16_dct_lsx/lasx`, `sub8x8_dct8_lsx/lasx`, `sub16x16_dct8_lsx/lasx`.
- Inverse: `add4x4_idct_lsx`, `add8x8_idct_lsx/lasx`, `add16x16_idct_lsx/lasx`, `add8x8_idct8_lsx/lasx`, `add8x8_idct_dc_lsx/lasx`, `add16x16_idct_dc_lsx/lasx`.
- DC Hadamard: `dct4x4dc_lsx/lasx`, `idct4x4dc_lsx/lasx`.
- Scan: `zigzag_scan_4x4_frame_lsx/lasx`.

ABI: standard C calling convention with `$a0` = destination, `$a1` = first source (encode plane or coefficients), `$a2` = second source (prediction plane) where applicable. Row strides are the constants `FENC_STRIDE=16` and `FDEC_STRIDE=32` from `loongson_util.S`. Coefficients are 16-bit (`dctcoef`); pixels are 8-bit in the supported build.

## 4. State and data structures

No globals beyond read-only constant tables. Working data lives in `vr0..vr31` (LSX) or `xr0..xr31` (LASX). The 8x8 and 16x16 forward kernels use macro-defined cores that process one 8x8 quadrant at a time, with pointer adjustment between quadrants.

## 5. Algorithm / behaviour

### Forward 4x4 (`sub4x4_dct_lsx`)

Loads four 32-bit words per row from source and reference at `FENC_STRIDE`/`FDEC_STRIDE`, zero-extends bytes to halfwords (`vsllwil.hu.bu`), subtracts, then applies `DCT4_1D_LSX` to the four row vectors, transposes the 4x4 halfword matrix with `LSX_TRANSPOSE4x4_H`, applies `DCT4_1D_LSX` again to the columns, and stores two 16-byte vectors of coefficients. `DCT4_1D_LSX` is the x264 lossless 4-point integer DCT: pairwise sums/differences plus the doubled outer-term recurrence (no shifts, so output magnitude stays within 16 bits).

### Forward 8x8/16x16

`SUB8x8_DCT_CORE_LASX` loads eight 8-byte rows from both planes, widens bytes with `xvilvl.b` against a zero vector, subtracts, applies `DCT4_1D_LASX` to rows, a 2x4x4 transpose, `DCT4_1D_LASX` to columns, then permutes and stores eight 16-byte coefficient vectors. The LSX core (`SUB8x8_DCT_CORE_LSX`) does the same with 128-bit vectors and an expanded sequence of `vilvl.h/w/d`, `vilvh.h/w/d` interleaves. `sub16x16_*` calls the core four times with pointer increments that step 8 columns, then `8*stride − 8` rows, then 8 columns — covering the four 8x8 quadrants in raster order. Output is `p_dst[16][16]` with each 4x4 block's coefficients contiguous.

### Forward 8x8 DCT8 (`sub8x8_dct8_*`)

Uses the dedicated 8-point 1-D transform (butterfly-4 on outer pairs plus quarter-shift odd terms) in both dimensions, producing 64 coefficients for one 8x8 residual block. `sub16x16_dct8_*` applies the 8x8 kernel to the four quadrants. Exact rounding: none in forward path; coefficients are unscaled.

### Inverse 4x4 (`add4x4_idct_lsx`)

Loads four 16-byte coefficient vectors (16 halfwords). Computes `tmp = c2 >> 1` (arithmetic), interleaves pairs to halfword vectors, widens with `vhaddw.w.h`/`vhsubw.w.h`, forms the four butterfly results in word lanes, packs back to halfwords with `vpickev.h`, transposes, repeats the vertical pass, then applies a rounded arithmetic right shift by **6** (`vsrai.h` preceded by adding 32) and adds to the destination bytes with clipping to `[0,255]`. The source coefficients are zeroed after use (consistent with the generic 4x4 inverse contract).

### Inverse 8x8/16x16

`add8x8_idct_*` uses four 4x4 inverse cores at the four quadrant positions. `add8x8_idct8_*` performs the full 8x8 inverse: adds **32** to the DC coefficient, applies the 2-D 8-point inverse with `>>2` odd-term scaling, rounds right by **6**, adds to destination bytes and clips. `add16x16_idct8_*` applies the 8x8 kernel four times.

### DC-only inverse add

`add8x8_idct_dc_*` and `add16x16_idct_dc_*` take uniform DC values, broadcast `(dc + 32) >> 6` across each 4x4 sub-block, add to destination bytes, clip to `[0,255]`, and store. They skip all butterfly work.

### DC Hadamard (`dct4x4dc_*`, `idct4x4dc_*`)

Treat the 16 coefficients as a 4x4 matrix; apply a 4-point Hadamard butterfly to rows, transpose (using `LASX_TRANSPOSE4x4_H` or the LSX equivalent), apply the butterfly to columns, and store. No scaling; applying the routine twice yields `4 × input`. The two variants (`dct`/`idct`) differ only in output permutation convention.

### Zig-zag

`xvshuf.h`/`vshuf.h` permute the 16 coefficients using the constant table `zigzag_scan4 = {0,4,1,2,5,8,12,9,6,3,7,10,13,14,11,15}`, producing `level[i] = dct[zigzag[i]]` for the H.264 frame scan.

### Supporting constants

- `hsub_mul`: 16 repetitions of the byte pair `{1, -1}` — used by `xvdp2.h.bu` to compute horizontal differences in Hadamard/SATD-style kernels.
- `last64_shuf`: `{0,4,1,5,2,6,3,7}` word indices used by quant/scan code (defined here and reused).
- `zigzag_scan4` as above.

## 6. Dependencies

- **Calls (macros):** `loongson_asm.S` (`LASX_TRANSPOSE2x4x4_H`, `LASX_TRANSPOSE4x4_H`, `LSX_TRANSPOSE4x4_H`, dot-product helpers, clip helpers), `loongson_util.S` (`function_x264`, `FENC_STRIDE`, `FDEC_STRIDE`, `ASM_REF`).
- **Called by:** `x264_dct_init()` in `common/dct.c` when `X264_CPU_LSX`/`X264_CPU_LASX` is set.
- **Shared config:** `HIGH_BIT_DEPTH` gate; `BIT_DEPTH` for symbol prefix.

## 7. Constants and tables

- `hsub_mul` bytes `{1,-1}` × 16.
- `last64_shuf` ints `{0,4,1,5,2,6,3,7}`.
- `zigzag_scan4` shorts `{0,4,1,2,5,8,12,9,6,3,7,10,13,14,11,15}`.
- Rounding constants: `32` before `>>6` (4x4 inverse, DC add), `+32` to 8x8 DC before inverse, `>>2` odd terms in 8x8 inverse.
- Clip bound 255 for reconstruction.

## 8. Performance notes

- **Hotness:** very high; every macroblock transforms residual and reconstruction blocks.
- **SIMD status:** fully LSX/LASX; LASX variants process two 4x4 blocks or one 8x8 block per vector sequence.
- **Optimization tips:** (a) the transpose macros are the main cost — use the specialized 4x4/2x4x4 variants rather than generic 8x8; (b) keep `FENC_STRIDE`/`FDEC_STRIDE` constants as immediates; (c) the DC-only paths are far cheaper than full transforms — ensure callers select them for DC-only blocks; (d) LASX loads/stores require 32-byte alignment for best throughput; align coefficient arrays.
## 9. References

ITU-T H.264 §8.5.1/§8.5.3; x264 `common/dct.c`; LoongArch LSX/LASX ISA manuals; LoongML v0.4.0 macro library.

## 10. Verification checklist

1. For all 4x4/8x8/16x16 forward transforms, compare assembly output bit-for-bit with the scalar reference for random residuals.
2. Inverse: compare reconstructed bytes for known coefficient blocks, including clipping cases (negative and >255 results).
3. Verify the DC coefficient "+32" pre-add in the 8x8 inverse with a targeted test.
4. DC Hadamard: apply twice and confirm `4 × input`.
5. Zig-zag: input `dct[i]=i` and check output order matches the constant table.
6. Verify LSX and LASX variants of each operation produce identical results.
7. ASan-style bounds check with coefficient arrays at end of a page to catch over-reads.
8. Full-encode bitstream equivalence against a C-only build.

## 11. Open questions

- UNKNOWN: exact output permutation of `idct4x4dc_*` relative to `dct4x4dc_*` — the two are nominally inverses but may use different butterfly output orders; validate numerically.
- UNKNOWN: whether `sub8x8_dct8_*` zeroes its source coefficients (4x4 inverse does, 8x8 inverse may not) — verify against the generic contract.
- UNKNOWN: alignment requirements of `xvld`/`vld` in this file; the macros use plain loads that may fault on unaligned addresses on strict implementations.
