# `common/x86/dct-a.asm` — x86 transform and zigzag (shared assembly)

**Dirty-side behavioural specification.** Source path: `common/x86/dct-a.asm`.
Role: implements, for both 8-bit and high-bit-depth builds, the SIMD forward DCT,
inverse DCT, DC-only Hadamard cascades, and coefficient scan/interleave kernels.

---

## 1. Header

Assembly source using the `x86inc.asm`/`x86util.asm` macro framework. It defines
a `.rodata` section with permutation/sign masks and a `.text` section with the
kernels. Symbols are emitted through `cglobal`/`cglobal_label` and renamed by the
build's template mechanism. This file contains the implementations shared by both
32-bit and 64-bit x86; architecture-specific variants live in `dct-32.asm` and
`dct-64.asm`.

## 2. Purpose

The encoder evaluates many candidate block partitions and quantisation settings;
each evaluation needs a forward transform of a residual block, and reconstruction
needs the inverse. Doing these operations in SIMD is essential. This file also
implements the *fused* subtract-and-scan kernels used in the fast analysis path,
where the encoder gets the residual and its zig-zag coefficients in one pass,
plus the AVX-512 variants that keep whole 8x8 blocks in a single 512-bit register.

## 3. Public interface

The exported symbols are exactly those declared in `dct.h.md`. They fall into
these families:

### 3.1 DC Hadamard: `dct4x4dc`, `idct4x4dc`
* Input/output: a single `d[16]` (4x4) of `dctcoef`; high-bit-depth uses `int32_t`,
  8-bit uses `int16_t`. In-place.
* High-bit-depth: load four rows, apply a 1D four-point Walsh–Hadamard along
  columns (`WALSH4_1D`), transpose, add a rounding constant of +1 to the top-left
  element, apply the 1D butterfly again, then shift all four outputs right by 1.
  This computes the scaled 4x4 Hadamard used for the luma DC cascade.
* 8-bit `dct4x4dc`: MMX2. It flips coefficients to unsigned by XOR with 0x8000
  so that `pavgw` (which averages unsigned) can implement the 17-bit
  sum/difference; then converts back. The normalisation is a right shift by 1
  with rounding contributed by the averaging trick.
* `idct4x4dc`: the inverse cascade; two 1D butterflies separated by a transpose,
  no rounding shift (it is the exact inverse of the forward transform's linear
  part). MMX for 8-bit, SSE2/AVX for high-bit-depth.

### 3.2 `dct2x4dc`
* `(dctcoef dct[8], dctcoef dct4x4[8][16])`. Collects the eight DC values from
  eight 4x4 coefficient blocks and computes the 2x4 (in practice a 2x2-per-block)
  Hadamard transform of them, producing the chroma DC coefficients. Input rows
  are gathered with a helper macro; the butterfly is a 2-point sum/difference
  followed by a butterfly across the pair of groups; the result is stored as two
  packed vectors of four values.
* High-bit-depth: SSE2 and AVX forms using dword elements; 8-bit: MMX2 using words.

### 3.3 4x4 forward DCT: `sub4x4_dct`
* High-bit-depth (`INIT_MMX mmx`, but the body uses word/dword unpacking): loads
  four even rows of the residual (rows 0,2,4,6 for the 16-bit case), applies a
  1D four-point DCT, transposes, then applies a second stage combining the
  "s03/s12" and "d03/d12" pairs; output is written as four groups of 16-bit
  values after unpacking to 32-bit and subtracting shifted partners. Sign
  handling: the second stage uses a "sum of ±" butterfly that realises the
  `a - 2b`, `2a - b` coefficients of the H.264 DCT matrix.
* 8-bit (`SUB_DCT4` macro): MMX and SSSE3. Loads four rows via `LOAD_DIFF8x4`
  (which for SSSE3 uses `pmaddubsw` with `hsub_mul` to compute `a-b` pairs),
  applies `DCT4_1D`, transposes 4x4, applies `DCT4_1D` again, stores four 8-byte
  rows. Output: `int16_t dct[16]` natural order.
* AVX-512 8-bit form: builds four 4x4 blocks at once in one ZMM/XMM register via
  broadcast loads, executes the 4x4 butterfly with opmasked `vpaddw`/`vpsubw`
  to implement the half-shifted coefficients, then stores the two 32-byte
  halves. This is the fastest 4x4 path on AVX-512 hardware.

### 3.4 4x4 inverse DCT: `add4x4_idct`
* High-bit-depth: loads 4 dwords, applies `IDCT4_1D` dword version, transposes,
  adds +32 (stored as `pd_32`), applies `IDCT4_1D` again, then `STORE_DIFFx2`
  shifts each word right by 6, packs to words, adds the destination samples,
  clamps to `[0, pixel_max]`, and writes back two rows at a time.
* 8-bit MMX: the same structure with words, `paddw` +32, and `STORE_DIFF` to
  add/clip/store each row.
* SSE4/AVX 8-bit (`ADD4x4` macro): a fused formulation that computes the 1D
  inverse using shifts and `pshuflw`/`pshufhw` plus `SBUTTERFLY2`, adds the
  rounding constant into the first row only (`pw_32_0`), then `packuswb`
  produces all four clipped output rows in one register. AVX (VEX) form avoids
  the MMX/SSE domain penalty.
* AVX2 8-bit `add8x8_idct` is implemented directly in this file with 256-bit
  loads (`vinserti128`), a 4x4-of-4x4 structure via `TRANSPOSE2x4x4W`, and
  `STOREx2_AVX2` that shifts, adds, packs, and stores four rows per iteration.

### 3.5 Composite 8x8/16x16 DCT and IDCT
* `SUB_NxN_DCT` macro: calls a 4x4 (or 8x8) sub-kernel four times with pointer
  arithmetic `%3` (output offset), `%4-%5-%6*STRIDE` (source stepping) to tile
  the larger block. Used to build `sub8x8_dct`, `sub16x16_dct`,
  `sub16x16_dct8`.
* `ADD_NxN_IDCT` macro: analogous for inverses, calling a 4x4/8x8 IDCT four times.
* Instantiations are conditional on architecture and bit depth: on 32-bit only
  MMX/SSE paths for some sizes; on 64-bit the 4x4-of-4x4 and 8x8-of-8x8 variants.
* The AVX-512 `sub8x8_dct`/`sub16x16_dct` path uses a special helper
  `dct4x4x4_internal` that transforms four 4x4 blocks in one 512-bit register,
  and `SUB4x16_DCT_AVX512` that transforms four blocks of a 16x16.
* AVX-512 `sub8x8_dct_dc`/`sub8x16_dct_dc`: load whole rows with `vpermt2d`,
  compute per-row sums with `psadbw`, then perform the 2x2/4x2 Hadamard with
  word shuffles (`vprold`, `punpck`, `psubw`/`paddw`). For the 8x16 case an
  additional 4-point butterfly combines the eight sub-block sums.

### 3.6 DC inverse: `add8x8_idct_dc`, `add16x16_idct_dc`
* High-bit-depth: load four dwords, add +32, shift right 6, replicate each DC
  value eight times via `pshuflw`/`pshufhw`/`pshufd`, then `ADD_DC` adds the
  signed DC to each of four rows with word saturating add and clamp against
  zero and `pw_pixel_max`.
* 8-bit MMX2: same with words; `packuswb` produces unsigned byte constants;
  `paddusb`/`psubusb` implements add-clamp-subtract-clamp (adding a signed
  value using two unsigned saturation ops).
* SSSE3/AVX/AVX2: use `pmulhrsw` with 512 to implement `(dc+32)>>6` with proper
  rounding in one instruction; `pshufb` with `pb_unpackbd1`/`pb_unpackbd2`
  broadcasts the four bytes to rows. AVX2 handles two 8x8 halves at once via
  `vinserti128`.

### 3.7 Zigzag scans
* `zigzag_scan_8x8_frame`: 8-bit SSE2/SSSE3 and high-bit-depth SSE2/AVX use a
  sequence of `punpck` and `palignr` operations to pick the 64 coefficients in
  the exact frame zig-zag order (documented in a comment as an 8x8 index matrix).
  AVX-512 uses bit-packed 32/16-bit permutation indices stored in `.rodata`
  (`scan_frame_avx512`), applied with `vpermd`/`vpermi2d` or `vpermi2w` in one
  or two instructions.
* `zigzag_scan_8x8_field`: 8-bit MMX2/SSE4/AVX/XOP/AVX-512 and high-bit-depth
  SSE4/AVX variants; output order is the field zig-zag (different from frame).
* `zigzag_scan_4x4_frame`: MMX for 8-bit, SSE2/AVX for high-bit-depth, SSSE3/XOP
  for 8-bit using `pshufb`/`vpperm` masks `pb_scan4framea/b`, AVX-512 using
  `scan_frame_avx512`.
* `zigzag_scan_4x4_field`: SSE/SSE2/AVX-512; permutes the inner four entries.

### 3.8 Fused subtract-and-scan: `zigzag_sub_4x4_*`
* SSSE3/AVX only, 8-bit only. Loads four source rows (`src`) into `m0..m3`, four
  destination rows (`dst`) into `m4..m7`, writes the source into the destination
  (so the encoder can treat `src` as the new reference), packs them into two
  dqwords, applies `pshufb` with `pb_sub4frame` or `pb_sub4field` to put bytes in
  scan order, then `pmaddubsw` with `hsub_mul` (a `[1,-1,1,-1,…]` pattern) to
  compute the signed residual differences and pack to words.
* `ac` variants copy the DC coefficient (first word) to `*dc`, mask it out of
  the AC array, and compute the non-zero-AC flag differently (only the AC words
  are tested). Return value: 1 if any tested coefficient is non-zero, else 0,
  computed by `pcmpeqb`/`pmovmskb` against zero and subtracting 0xffff then
  shifting.
* CVAVLC interleave: `zigzag_interleave_8x8_cavlc` reads four 4x4 coefficient
  groups, transposes them with `TRANSPOSE4x4W/D`/`SBUTTERFLY`, writes interleaved
  output, and builds a non-zero bitmap by packing coefficient words to bytes,
  comparing with zero, adding 1 (`pb_1`) and storing two 16-bit words to `nnz`.
  AVX2/AVX-512 variants use `vpermd`/`vptestmd` to compute the bitmap directly.

## 4. State and data structures

* Coefficient scratch arrays: `dctcoef dct[...]` in the caller; assembly uses only
  registers for the 4x4/8x8 transforms except the AVX-512 path which uses opmasks
  `k1`,`k2`,`k3` derived from constants `0xf0aa`, `0xf0f0f0f0`.
* The `.rodata` masks: `pw_ppmmmmpp` (sign pattern `[1,1,-1,-1,-1,-1,1,1]`),
  `pb_sub4frame`/`pb_sub4field` (byte scan orders), `pb_subacmask`,
  `pb_scan4frame*`, `pb_scan8frame*`/`pb_scan8field*`, and the bit-packed
  `scan_frame_avx512`/`scan_field_avx512`/`cavlc_shuf_avx512`.
* `cextern` declarations import constants from the C translation unit
  (`pw_32`, `pw_8000`, `hsub_mul`, `pd_1`, `pd_32`, `pw_ppppmmmm`, …).
* Op mask setup for AVX-512 is per-function and self-contained.

## 5. Algorithm / behaviour

### 5.1 1D butterflies
* `DCT4_1D` (from `x86util.asm`) implements the four-point 1D transform:
  sum/difference pairs `(s0,d0)=(x0+x3,x0-x3)`, `(s1,d1)=(x1+x2,x1-x2)`,
  then outputs `(s0+s1)>>1` (scaled DC), `(d0 + (d1>>1))`, `(s0-s1)>>1`,
  `((d0>>1)-d1)` — equivalently the H.264 4x4 DCT basis. Shifts are arithmetic
  (round toward negative infinity) in the SSE/MMX implementations.
* `IDCT4_1D` is the inverse: it reconstructs the four input samples from the
  transformed coefficients using shifts by 1 and 2 and sum/difference pairs.
* `WALSH4_1D` is the unscaled Hadamard: three nested sum/difference butterflies.
* `SUMSUB_17BIT` implements a 17-bit-safe sum/difference using XOR with 0x8000
  plus `pavgw` (unsigned averaging adds one bit of headroom).

### 5.2 Forward 4x4 (high-bit-depth)
1. Load four *even* rows of `pix1-pix2` (word differences).
2. Apply `DCT4_1D` in words.
3. Transpose 4x4 words.
4. Form `s03±s12`, `d03±d12` pairs; store the unscaled "sum" pairs to output
   rows 0/2 after unpacking to dwords with `DCT_UNPACK` (which sign-extends).
5. Form `2*d03+d12` and `d03-2*d12` by doubling one operand via `SUMSUB2_AB`
   in dwords; store to rows 1/3.
This produces four 4-element vectors of 32-bit coefficients (16 total) with the
same values as the portable 4x4 transform.

### 5.3 Forward 4x4 (8-bit)
1. MMX: `LOAD_DIFF8x4` loads four rows with byte-to-word difference via
   `LOAD_DIFF` (unsigned subtract both directions + OR) or `pmaddubsw` on SSSE3.
2. `DCT4_1D` word butterfly, `TRANSPOSE4x4W`, `DCT4_1D` again.
3. Eight `movq` stores write the 16 words. No explicit rounding shift is needed
   because `DCT4_1D`'s `>>1` is part of the integer DCT definition.

### 5.4 Inverse 4x4
1. Load coefficient rows from `dct`.
2. `IDCT4_1D` (word or dword), transpose, add +32 to the DC row only.
3. `IDCT4_1D` again; shift right 6 (word) or 6 (dword, via `psrad`).
4. Add destination pixels, clamp `[0,pixel_max]`, store.

### 5.5 DC-only
`sub8x8_dct_dc` computes, per 4x4 tile, the sum of four pairs of rows, then a
2-point butterfly over the four sums; in 8-bit the byte sums are accumulated
with `psadbw` into dwords, then packed back to words and put through `DCT2x2`
which forms `s0±s1`, `s2±s3` and `d02±d13`. The 10-bit version uses word adds
and `pmaddwd` with `[1,1,-1,-1]`.

### 5.6 Zigzag / scan
All scan kernels are pure permutations: they select source words/bytes by index
and store them in a new order. The exact orders are standard H.264 frame/field
zig-zag and are documented as index matrices beside `SCAN_8x8_FRAME` /
`SCAN_8x8_FIELD`. No arithmetic is performed except in the fused `zigzag_sub`
kernels.

### 5.7 Fused subtract + scan
`zigzag_sub_4x4` computes `level[k] = src[scan[k]] - dst[scan[k]]`, while also
copying the source block into the destination. The sign-correct byte difference
is obtained with `pmaddubsw` against an alternating `[1,-1,...]` multiplier.
A block is "all zero" iff every produced word equals zero; the function returns
a boolean accordingly.

## 6. Dependencies

* Includes `x86inc.asm`, `x86util.asm`; uses their macros `SUMSUB_BA`,
  `SUMSUB_BADC`, `SBUTTERFLY`, `TRANSPOSE4x4W/D`, `TRANSPOSE2x4x4W`,
  `LOAD_DIFF`, `LOAD_DIFF8x4`, `STORE_DIFF`, `CLIPW`, `INIT_*`, `cglobal`,
  `DECLARE_REG_TMP`, `TAIL_CALL`, `REP_RET`.
* Imports C constants via `cextern`.
* Called from the dispatch layer; `dct-32.asm`/`dct-64.asm` reference its
  `.skip_prologue` labels via `cextern` to compose larger transforms.
* `zigzag_*` outputs feed the CAVLC/CABAC coefficient coders.

## 7. Constants and tables

* `pw_ppmmmmpp`: signs `[1,1,-1,-1,-1,-1,1,1]` for the 10-bit DC butterfly.
* `pb_sub4frame`, `pb_sub4field`: byte-order tables mapping natural 4x4 positions
  to frame/field zig-zag.
* `pb_subacmask`: word mask `[0,-1,-1,-1,-1,-1,-1,-1]` to isolate AC.
* `pb_scan4framea/b`, `pb_scan4frame2a/b`: SSSE3/XOP shuffle masks for 4x4 scan.
* `pb_scan8framet1..t9`, `pb_scan8frame1..8`, `pb_scan8field*`: shuffle masks
  composing the 8x8 scans.
* AVX-512 masks `scan_frame_avx512`, `scan_field_avx512`, `cavlc_shuf_avx512`,
  `dct_avx512`: bit-packed indices (5 bits per index for 32 entries) to keep
  tables small.
* Rounding constants: +32 (`pw_32`/`pd_32`), `pw_8000` (0x8000) for the unsigned
  averaging trick, `pw_512` (512) for `pmulhrsw` rounding.

## 8. Performance notes

This file is extremely hot: forward/inverse transforms run for every candidate
block. Concrete opportunities observed in the reference:
* **Wider vectors:** The AVX-512 `dct4x4x4_internal` transforms four 4x4 blocks
  in one 512-bit register, amortising the butterfly across blocks. AVX2
  `sub16x16_dct`/`add8x8_idct` pack two rows per 256-bit register with
  `vinserti128`, halving instruction count. A clean implementation should prefer
  the widest available vector and keep a 4-block-per-register structure.
* **Transpose strategy:** `TRANSPOSE2x4x4W` packs two 4x4 blocks and transposes
  them together; this is cheaper than two separate 4x4 transposes. The 8-bit
  path uses `SBUTTERFLY` on qwords after the first 1D transform, avoiding a
  full 8x8 transpose.
* **Avoiding spills:** The high-bit-depth 4x4 inverse keeps six live vectors in
  registers; the AVX2 `add8x8_idct` keeps eight and uses `STOREx2_AVX2` to consume
  them directly. On x86-32 the same kernels spill (see `dct-32.asm.md`); a
  clean implementation should target x86-64 register richness first.
* **Preprocessing constants:** sign masks (`pw_ppmm...`), `hsub_mul`, and the
  bit-packed AVX-512 shuffle tables should be static read-only data, loaded once
  outside loops. Deriving them per call wastes load ports.
* **Fused operations:** `zigzag_sub_4x4` merges subtract + scan + all-zero test
  into one pass; this is a large win in trellis/CABAC analysis because it avoids
  materialising the residual array.
* **Branch behaviour:** Dispatch is compile-time/one-time at init; kernels are
  branch-free inside loops, which is essential for predictable pipeline flow.
* **`TAIL_CALL`:** the composite `_NxN_` kernels tail-call the 4x4/8x8
  `.skip_prologue` entry to avoid an extra return and preserve register state.

## 9. References

* ITU-T H.264 integer DCT and Hadamard transform definitions.
* "x264 transform design" notes in the public source history.
* Intel Intrinsics Guide — `vpermt2d`, `vpshufb`, `pmaddubsw`, `pmulhrsw`.
* AMD64/Intel optimization manuals on store-forwarding and register spilling.

## 10. Verification checklist

* Feed random 4x4/8x8/16x16 residual blocks (8-bit and 10-bit) to forward
  transforms; compare bit-exactly with the portable C transform.
* For inverses: quantise to zero, inverse-transform with a known coefficient
  pattern, confirm output equals the reference (clamping included).
* Round-trip: forward then inverse (no quantisation) must reproduce input within
  the documented ±1 rounding.
* DC-only: verify `sub8x8_dct_dc` outputs match the DC terms of four full
  `sub4x4_dct` calls, and `add8x8_idct_dc` matches reconstruction with all AC
  coefficients zero.
* Zigzag: apply each scan to an array containing a single 1 at every position in
  turn; the output position must match the standard H.264 frame/field tables.
* Fused `zigzag_sub`: verify `level`, copied `dst`, return flag, and (for `ac`)
  the extracted DC all match a separate subtract + scan.
* AVX-512 paths: compare against the SSE2 path for the same inputs; opmask
  handling at the tails of partial vectors is a common bug source.

## 11. Open questions

* `UNKNOWN`: whether the `sub4x4_dct_mmx` high-bit-depth path is reachable in a
  normal 64-bit high-bit-depth build; it appears under `INIT_MMX mmx` but may be
  compiled out by the 32-bit-only guard in the build system.
* `UNKNOWN`: exact bit-exact output of the 10-bit DC `SUMSUB_17BIT` path for
  extreme input values (possible 17-bit overflow corner cases).
* `UNKNOWN`: the `dct_avx512` bit-packed table's derivation for `idct8x8`
  variants is not fully documented in this file; verify by black-box comparison.
