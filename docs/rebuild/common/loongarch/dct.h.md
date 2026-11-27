# `common/loongarch/dct.h` — LoongArch transform declarations

**Dirty-side behavioural specification.** Source path: `E:\_x264_dirty_stash\x264-source\common\loongarch\dct.h`. Role: public interface for the LoongArch LSX (128-bit) and LASX (256-bit) transform implementations in `loongarch/dct-a.S`.

## 1. Header

The title line above gives the source path and the file's role in one sentence; the adjacent bold marker identifies this document as a **dirty-side behavioural specification** written from the reference source and public literature. No reference source or near-verbatim pseudocode is reproduced.

## 2. Purpose

Exposes the transform primitives to x264's architecture dispatcher. Unlike the MIPS port, this header declares **two variants per operation** — one suffixed `_lsx` (128-bit vectors) and one `_lasx` (256-bit vectors) — because LoongArch machines may support either or both instruction sets. Every declaration is name-mangled with `x264_template`. There is no `HIGH_BIT_DEPTH` guard around the declarations themselves (the implementations behave per build config).

## 3. Public interface

Each symbol follows the pattern `x264_<op>_lsx` or `x264_<op>_lasx`:

**Forward 4x4 / 8x8 / 16x16 residual DCT**
- `sub4x4_dct_lsx` — only LSX variant exists for 4x4: `void x264_sub4x4_dct_lsx(int16_t p_dst[16], uint8_t *p_src, uint8_t *p_ref)`
- `sub8x8_dct_lsx`, `sub8x8_dct_lasx` — `int16_t p_dst[4][16]`
- `sub16x16_dct_lsx`, `sub16x16_dct_lasx` — `int16_t p_dst[16][16]`
- `sub8x8_dct8_lsx`, `sub8x8_dct8_lasx` — 8x8 transform: `int16_t pi_dct[64]`
- `sub16x16_dct8_lsx`, `sub16x16_dct8_lasx` — four 8x8 transforms: `int16_t pi_dct[4][64]`

**Inverse transforms with add**
- `add4x4_idct_lsx` — `void (uint8_t*, int16_t[16])`, only LSX.
- `add8x8_idct_lsx`, `add8x8_idct_lasx` — `int16_t[4][16]`.
- `add16x16_idct_lsx`, `add16x16_idct_lasx` — `int16_t[16][16]`.
- `add8x8_idct8_lasx`, `add8x8_idct8_lsx` — `int16_t[64]`.
- `add8x8_idct_dc_lsx/lasx` — `int16_t dct[4]`.
- `add16x16_idct_dc_lsx/lasx` — `int16_t dct[16]`.

**DC Hadamard**
- `idct4x4dc_lsx/lasx`, `dct4x4dc_lsx/lasx` — `int16_t d[16]`, in-place.

**Zig-zag**
- `zigzag_scan_4x4_frame_lsx/lasx` — `void (int16_t level[16], int16_t dct[16])`.

Contracts mirror the generic DCT vtable exactly: forward transforms produce unscaled integer-transform coefficients, inverse transforms add to the destination (pitch `FDEC_STRIDE` = 32 for 8-bit, or 64 for high-bit-depth) and clip to the pixel range, DC-only variants add a uniform offset, and the zig-zag routine reorders 16 coefficients into the H.264 frame scan order.

## 4. State and data structures

No state. Include guard `X264_LOONGARCH_DCT_H`. Coefficient arrays are caller-owned and laid out as in the generic x264 DCT:

- 4x4 block: 16 `int16_t` (or `dctcoef`, which is `int16_t` in 8-bit builds) row-major.
- 8x8 with 4x4 transform: four 4x4 blocks in raster quadrant order.
- 8x8 with 8x8 transform: 64 consecutive coefficients.
- LSX vectors are 128 bits (8 halfwords); LASX vectors are 256 bits (16 halfwords), so the LASX variants can hold a full 4x4 coefficient block in one register.

## 5. Algorithm / behaviour

Declarations only. The implementations are behavioural equivalents of the MIPS/C versions, with the following ISA-specific facts:

- LSX kernels use `vr*` registers; LASX kernels use `xr*` registers; both are defined via aliases in `loongson_asm.S`.
- The 4x4 forward transform uses a 1-D butterfly macro (`DCT4_1D_LSX/LASX`) applied to rows, a 4x4 halfword transpose, then the 1-D transform applied to columns.
- The 8x8 forward transform has a dedicated core macro (`SUB8x8_DCT_CORE_LASX/LSX`) that loads eight 8-byte rows from source and reference, widens and subtracts them, applies the 4-point butterfly in both dimensions and stores eight 16-byte coefficient vectors.
- The inverse 4x4 uses `vhaddw.w.h`/`vhsubw.w.h` widening pair operations, two transposes and a final rounded `>>6` before clipping.
- The 8x8 inverse uses the 8-point extension with quarter shifts and rounding.
- Zig-zag uses the constant table `zigzag_scan4 = {0,4,1,2,5,8,12,9,6,3,7,10,13,14,11,15}` with `vshf.h`/`xvshuf.h`.

## 6. Dependencies

- **Included by:** `common/loongarch/dct-a.S` (which implements these symbols) and `common/dct.c` (dispatcher).
- **Requires:** `x264_template`, `int16_t`, `uint8_t` from generic headers; `x264_dct_functions_t` typedef at dispatcher sites.
- **No** dependency on `loongson_asm.S` at declaration level — that is assembly-only.

## 7. Constants and tables

None declared in the header. The constants used by the implementations (zig-zag table, `hsub_mul` byte pattern, rounding offsets) are documented in `dct-a.S.md`.

## 8. Performance notes

No runtime cost. For rebuild, note that declaring both LSX and LASX variants lets the dispatcher select the wider one when `X264_CPU_LASX` is set; the two are functionally interchangeable, so a clean implementation may initially provide only one variant and alias the other.

## 9. References

ITU-T H.264 standard; x264 project documentation; architecture SIMD programmer's guides.

## 10. Verification checklist

1. Compile header alone under 8-bit and high-bit-depth configs.
2. Verify expanded symbol names against the object file produced from `dct-a.S`.
3. Confirm each generic DCT vtable slot claimed by the LoongArch path has both an LSX and LASX declaration where applicable.
4. Check no high-bit-depth-specific type mismatch (`dctcoef` vs `int16_t`).

## 11. Open questions

- UNKNOWN: whether the dispatcher prefers LASX over LSX when both flags are set, and whether any routine lacks a LASX variant intentionally (observed: 4x4 DCT and 4x4 IDCT are LSX-only).
- UNKNOWN: exact `FDEC_STRIDE`/`FENC_STRIDE` values used by the assembly in high-bit-depth builds (the header names `pixel`/`pixel` typedefs in some files; verify the bit-depth macros in `loongson_util.S`).
