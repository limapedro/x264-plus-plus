# `common/x86/dct.h` — x86 transform and zigzag declarations

**Dirty-side behavioural specification.** Source path: `common/x86/dct.h` in the
reference encoder. Role: declares the full set of SIMD-accelerated forward/inverse
DCT, Hadamard/DC-only transform, and zigzag-scan entry points that the portable
transform layer dispatches to on x86.

---

## 1. Header

This is a declarations-only header. It contains no executable code and no data
definitions; every symbol is a C function prototype wrapped in a macro that lets
the build system rename it (the `x264_template(...)` wrapper). It is included by
the x86 CPU-dispatch initialisation file (`x86/x86inc.c` style) and by the x86
assembly files to cross-reference symbols from the C translation units.

## 2. Purpose

The portable encoder computes 4x4 and 8x8 integer forward transforms of residual
blocks, their inverses for reconstruction, DC-only transforms for very coarse
quantisation, and reorders the resulting coefficient arrays into the zig-zag
sequences required by the bitstream syntax. These operations dominate the
analysis loop at medium and high quality. On x86 each has several hand-written
implementations keyed by ISA level; this header is the contract between those
implementations and the dispatcher. Its existence lets the clean implementation
team know the exact set of behavioural variants that must be reproduced.

## 3. Public interface

All names are gated by `#define x264_<name>_<suffix> x264_template(<name>_<suffix>)`,
so the actual emitted symbol depends on build configuration (bit depth, architecture,
assembly optimisation on/off). The suffix denotes the ISA family: `mmx`, `mmx2`,
`sse`, `sse2`, `ssse3`, `sse4`, `avx`, `xop`, `avx2`, `avx512`. The generic type
`dctcoef` is `int16_t` in 8-bit builds and `int32_t` in high-bit-depth builds;
`pixel` is `uint8_t` / `uint16_t` correspondingly. Every forward transform takes
two source pointers (`pix1` = current/fencoded, `pix2` = predicted/fdecoded with
its own stride) and writes coefficients; every inverse transform takes a
destination pointer and a coefficient array and *adds* the reconstructed residual
into the destination (it does not overwrite).

### 3.1 Forward transforms, 4x4 blocks

* `sub4x4_dct_mmx(dctcoef dct[16], pixel *pix1, pixel *pix2)` — MMX (8-bit only).
* `sub4x4_dct_ssse3(int16_t dct[16], uint8_t *pix1, uint8_t *pix2)`.
* `sub4x4_dct_avx512(int16_t dct[16], uint8_t *pix1, uint8_t *pix2)`.
* The high-bit-depth variant `sub4x4_dct_mmx` operates on `dctcoef[16]`/`pixel*` and
  is dispatched under the 32-bit path only (see the assembly document).

Contract: compute `F = DCT4x4(pix1[y] - pix2[y])` for all 16 positions; the output
is 16 signed coefficients in natural (row-major) order. No scaling is applied
beyond the integer transform's own normalization. Input pointers must point at the
top-left of an aligned 4x4 block with strides `FENC_STRIDE` and `FDEC_STRIDE`.

### 3.2 Forward transforms, 8x8 blocks (four 4x4 sub-blocks)

* `sub8x8_dct_mmx`, `sub8x8_dct_sse2`, `sub8x8_dct_ssse3`, `sub8x8_dct_avx`,
  `sub8x8_dct_xop`, `sub8x8_dct_avx2`, `sub8x8_dct_avx512`.
* Signatures vary by ISA: the MMX form uses `int16_t dct[4][16]` with `uint8_t*`
  pointers, whereas the SSE2/AVX forms use `int16_t dct[4][16]` (8-bit) or
  `dctcoef dct[16][16]` (high-bit-depth path is exposed only for the composite
  16x16 call). The AVX2/AVX512 8-bit forms write `int16_t dct[4][16]`.

Contract: the 8x8 block is treated as four independent 4x4 blocks (quadrants);
output is four 16-element coefficient groups in raster order. This matches the
H.264 8x8 transform's "4x4 of 4x4" structure used when `8x8dct` is enabled but the
transform is still four separate 4x4s (the 8x8-specific transform is the `dct8`
family below).

### 3.3 Forward transforms, 16x16 blocks

* `sub16x16_dct_mmx`, `sub16x16_dct_sse2`, `sub16x16_dct_ssse3`,
  `sub16x16_dct_avx`, `sub16x16_dct_xop`, `sub16x16_dct_avx2`, `sub16x16_dct_avx512`.
* Output is `dctcoef dct[16][16]` (or `int16_t dct[16][16]` for 8-bit AVX forms),
  i.e. sixteen 4x4 coefficient groups.

Contract: sixteen 4x4 transforms of the 4x4 tiles of the 16x16 block.

### 3.4 Forward 8x8 transform (`dct8` family)

* `sub8x8_dct8_mmx`, `sub8x8_dct8_sse2`, `sub8x8_dct8_ssse3`, `sub8x8_dct8_sse4`,
  `sub8x8_dct8_avx`, and `sub16x16_dct8_{mmx,sse2,ssse3,sse4,avx,avx2}`.
* 8-bit signatures: `int16_t dct[64]` / `dctcoef dct[64]` with `pixel*` or
  `uint8_t*` sources. `sse4` uses `int32_t dct[64]` and `uint16_t*` sources — the
  high-bit-depth 8x8 transform.

Contract: a single true 8x8 integer Hadamard-like transform of the 8x8 residual,
producing 64 coefficients in natural order. `sub16x16_dct8` performs four such
transforms on the four 8x8 quadrants.

### 3.5 DC-only forward transforms

* `sub8x8_dct_dc_mmx2(int16_t dct[4])`, `sub8x8_dct_dc_sse2(dctcoef dct[4])`,
  `sub8x8_dct_dc_avx512(int16_t dct[4])`.
* `sub8x16_dct_dc_{sse2,ssse3,avx,avx512}` (output `dctcoef dct[8]` or
  `int16_t dct[8]`).

Contract: compute only the four (or eight) DC coefficients of the 4x4 tiles of an
8x8 (or 8x16) block. Each DC coefficient is the sum of the 16 (or 4x4) residual
samples of the corresponding tile, i.e. the unscaled DC term of a 4x4 Walsh–Hadamard
transform. No other coefficients are produced.

### 3.6 Inverse transforms

* 4x4: `add4x4_idct_mmx(uint8_t*, int16_t[16])`,
  `add4x4_idct_sse2(uint16_t*, int32_t[16])`,
  `add4x4_idct_sse4(uint8_t*, int16_t[16])`,
  `add4x4_idct_avx(pixel*, dctcoef[16])`.
* 8x8 (4x4-of-4x4): `add8x8_idct_mmx`, `add8x8_idct_sse2`, `add8x8_idct_avx`,
  `add8x8_idct_avx2`, `add8x8_idct_avx512`; 16x16 analogues
  `add16x16_idct_{mmx,sse2,avx,avx2}`.
* DC-only: `add8x8_idct_dc_{mmx2,sse2,ssse3,avx}`,
  `add16x16_idct_dc_{mmx2,sse2,ssse3,avx,avx2}`.
* True 8x8 inverse: `add8x8_idct8_{mmx,sse2,avx}`, `add16x16_idct8_{mmx,sse2,avx}`.

All inverse routines take `(destination, coefficients)` and *add* the dequantised,
inverse-transformed, rounded residual to existing samples in the destination
plane. The rounding constant is +32 with a right shift of 6 (12-bit / 8-bit
transform normalization), implemented as `pmulhrsw` with 512 on SSSE3+ paths.

### 3.7 Secondary Hadamard / DC helpers

* `dct4x4dc_{mmx2,sse2,avx}(d[16])` and `idct4x4dc_{mmx,sse2,avx}(d[16])` —
  in-place 4x4 Hadamard transform / inverse of a 4x4 block of DC coefficients.
  This is the luma 16x16 DC transform cascade. High-bit-depth forms operate on
  `int32_t d[16]`; 8-bit forms on `int16_t d[16]`.
* `dct2x4dc_{mmx2,sse2,avx}(dctcoef dct[8], dctcoef dct4x4[8][16])` — extracts
  the eight 2x2 DC Hadamard coefficients from eight 4x4 DC arrays (chroma DC).

### 3.8 Zigzag and scanning helpers

* `zigzag_scan_8x8_frame_*`, `zigzag_scan_8x8_field_*`,
  `zigzag_scan_4x4_frame_*`, `zigzag_scan_4x4_field_*` — reorder a block of
  coefficients from natural raster order into the standard H.264 frame/field
  zig-zag order. Signatures are `(level, dct)` arrays of 16/64 `dctcoef` or
  `int16_t`/`int32_t` depending on ISA and bit depth.
* `zigzag_sub_4x4_{frame,field}_{ssse3,avx}` and the `_ac_` variants — fused
  "subtract + scan" primitives: take a source and destination 4x4 pixel block,
  store the source into the destination, and write the 16 residual coefficients
  directly in zig-zag order to `level`. Return a boolean (int) indicating whether
  any AC coefficient is non-zero; the `ac` variants also write the DC coefficient
  to `*dc` through a fourth argument.
* `zigzag_interleave_8x8_cavlc_*` — transpose four 4x4 coefficient arrays into
  the interleaved layout needed by CAVLC 8x8, and compute a per-group non-zero
  bitmap written as two 16-bit words to `uint8_t *nnz`.

### 3.9 Invariants and contracts

* All coefficient arrays are stored in *natural* (row-major) order except the
  zigzag scans' outputs, which are in scan order.
* All transforms are integer reversible (within their declared intermediate
  precision) when no quantisation is applied.
* Inverse transforms saturate results to the legal pixel range `[0, (1<<BIT_DEPTH)-1]`.
* The `_mmx` entry points are only compiled for 8-bit builds; high-bit-depth
  forms start at SSE2 (MMX cannot hold 16-bitpixel data).
* Dispatch always selects the highest ISA variant available, except where a
  slower-vs-faster quirk is noted in the source (e.g. SSE2-is-slow CPUs).

## 4. State and data structures

The header itself holds only preprocessor declarations and prototypes. The
state relevant to its functions lives in the caller:

* Coefficient arrays (`dctcoef[...]`), stack- or heap-resident, naturally aligned
  to 16 bytes for the SIMD loads used by the assembly.
* Pixel planes with strides `FENC_STRIDE` (source/encode) and `FDEC_STRIDE`
  (destination/decode), both normally a multiple of 16.
* The `x264_template` macro namespace, which links each declared symbol to a
  possibly renamed assembly label. This is a build-time naming convention, not
  runtime state.

## 5. Algorithm / behaviour

The header defines no algorithm itself; the algorithms are implemented in
`dct-a.asm`, `dct-64.asm`, `dct-32.asm`. The relationships are:

* A forward 4x4 transform is a separable 2D Walsh–Hadamard-like integer transform:
  a 1D butterfly along rows, transpose, a 1D butterfly along columns. The 1D
  butterfly implements coefficients `a±b`, `c±d`, then `(a±b) ± (c±d)` with
  a one-bit shift on the outer pair, matching the H.264 integer DCT matrix.
* 8x8 and 16x16 forward transforms decompose into 4x4 transforms; the `dct8`
  variants implement a single 8-point butterfly per dimension using shifts by
  1 and 2 instead of multiplications.
* Inverse transforms apply the transposed butterfly, add a rounding bias of 32,
  shift right by 6, add the existing samples, and clamp.
* DC cascades apply the same butterfly hierarchy only to the DC terms.

Detailed per-kernel rounding, saturation, and precision are documented in the
three assembly-file specifications.

## 6. Dependencies

* Included by the x86 CPU-detection/dispatch source and by files that need to
  call these primitives.
* Cross-references assembly symbols emitted by `dct-a.asm`, `dct-64.asm`,
  `dct-32.asm`.
* Uses the project-wide typedefs `pixel`, `dctcoef`, and stride constants from
  `common/common.h`.
* Table constants shared with the C layer (zigzag orders, CAVLC tables) are
  consumed by the assembly implementations, not by this header.

## 7. Constants and tables

No constants are defined in this header. The numeric constants used by the
implementations (rounding +32 / 512, shifts of 6/8, the `pw_ppmm...` sign masks,
zig-zag index tables) live in the assembly files' `.rodata` sections and in
`common/tables.c`. Their values are allowed facts; see the implementation docs.

## 8. Performance notes

This header is hotness-neutral. It enables the dispatcher to choose among 6–10
ISA variants per primitive. Concrete speed opportunities are:
* wider vectors (AVX2/AVX-512) to process two/four 4x4 blocks per instruction —
  see `dct-a.asm.md` section 8;
* transpose strategies that avoid explicit 8x8 transposes by using shuffles on
  packed words — see `dct-64.asm.md`;
* preprocessing of sign/shuffle masks into read-only constants so the dispatch
  loop needs no per-call setup.
* Keeping all coefficient arrays 16-byte aligned lets the SIMD paths use aligned
  loads/stores, which the dispatcher assumes.

## 9. References

* H.264/AVC specification (ITU-T H.264), clauses on 4x4 and 8x8 integer transform
  and zig-zag scanning.
* Public x264 documentation and source history (transform design notes).
* Intel/AMD SIMD instruction set reference manuals for MMX/SSE/AVX semantics.

## 10. Verification checklist

* For each declared prototype, confirm the dispatched assembly symbol exists in
  the build with the expected signature (bit depth and ISA).
* Compare forward-transform output against the portable C implementation for
  random residual blocks: exact equality expected (transforms are integer).
* Confirm inverse transform + forward transform round-trip (without quantisation)
  reproduces the original samples within the documented ±1 rounding tolerance.
* Check that high-bit-depth builds never dispatch to an `_mmx` symbol.

## 11. Open questions

* `UNKNOWN`: the exact ABI mangling produced by `x264_template` for each build
  configuration is determined by external build macros not present in this file.
* `UNKNOWN`: a few `dctcoef` vs `int16_t`/`int32_t` signature mismatches across
  ISA variants are intentional in the reference; the dispatcher's exact cast
  behaviour at call sites must be verified against `common/x86/x86inc.c`.
