# `common/x86/dct-64.asm` — x86-64 transform and zigzag

**Dirty-side behavioural specification.** Source path: `common/x86/dct-64.asm`.
Role: the x86-64-specific implementations of the 8-point DCT/IDCT butterfly and
the 8-bit `sub8x8_dct`/`add8x8_idct` kernels, built from the same 4-point macros
but with 64-bit register-rich scheduling and no spilling.

---

## 1. Header

Assembly source included by the build only for x86-64 targets. It imports the
shared `x86inc.asm`/`x86util.asm` macro library and `cextern`s several constants
(`pd_32`, `pw_pixel_max`, `pw_2`, `pw_m2`, `pw_32`, `hsub_mul`) from the C layer.
It defines no `.rodata` of its own besides what the macros emit.

## 2. Purpose

On x86-64 the 8x8 transform can be implemented with a single 8-point butterfly
per dimension keeping all eight rows in SIMD registers. This file provides that
implementation for both high-bit-depth (`DCT8_1D d` / `IDCT8_1D d`) and 8-bit
(`DCT8_1D w` / `IDCT8_1D w`) data, plus the 4-point 8x8 block composition used
by the 8-bit `sub8x8_dct`/`add8x8_idct` entry points. It complements `dct-a.asm`,
which holds the actual 4-point kernels and the 8-bit AVX-512 path.

## 3. Public interface

Exported symbols (all declared in `dct.h.md`):

### 3.1 High-bit-depth true 8x8 transform
* `sub8x8_dct8` — `(dctcoef dct[64], pixel *pix1, pixel *pix2)`; SSE2, SSE4, AVX.
  Input residual samples are 16-bit; output is 64 32-bit coefficients because
  `WIDEN_SXWD` sign-extends words to dwords before the column butterfly.
* `add8x8_idct8` — `(pixel *dst, dctcoef dct[64])`; SSE2, AVX.
  Input 64 dwords; output clamped 16-bit samples written to eight rows.

### 3.2 8-bit 8x8 block transforms (4x4-of-4x4 form)
* `sub8x8_dct` — `(int16_t dct[4][16], uint8_t *pix1, uint8_t *pix2)`; SSE2,
  SSSE3, AVX, XOP. Output is four 16-word groups (the four 4x4 quadrants).
* `sub8x8_dct8` — `(int16_t dct[64], uint8_t *pix1, uint8_t *pix2)`; SSE2,
  SSSE3, AVX, XOP. Uses `DCT8_1D w` and `TRANSPOSE8x8W`.
* `add8x8_idct` — `(uint8_t *dst, int16_t dct[4][16])`; SSE2, AVX.
  Inverse of `sub8x8_dct`.
* `add8x8_idct8` — `(uint8_t *dst, int16_t dct[64])`; SSE2, AVX.
  Inverse of `sub8x8_dct8`.
* `sub16x16_dct8` — `(dctcoef dct[4][64], pixel*, pixel*)`; AVX2 only on x86-64.
  Two 16x8 halves each transformed with the AVX2 8-point butterfly.

### 3.3 Internal helpers
* `DCT8_1D`, `IDCT8_1D` — parameterised macros taking an element size (`w`
  for 16-bit, `d` for 32-bit), eight data registers, and two temporaries.
* `%macro DCT_SUB8`, `%macro ADD8x8_IDCT8`, `%macro ADD8x8` — instantiable bodies
  reused across ISA levels.
* `.skip_prologue` labels are exported so that `dct-a.asm`'s `SUB_NxN_DCT` /
  `ADD_NxN_IDCT` can call them to build 16x16 variants without re-prologue.

## 4. State and data structures

* All eight rows of an 8x8 block are held simultaneously in `m0..m7`; temporaries
  are `m8..m13` (high-bit-depth) or `m8..m10` (8-bit).
* Output coefficient array is the caller's 64-element (or 4x16) `dctcoef`/`int16_t`
  buffer; row stores use offsets `0x00..0x70` (words) or `0x00..0xE0` (dwords).
* Strides: `FENC_STRIDE` (source) and `FDEC_STRIDE` (destination), passed in
  registers and fixed up with `FIX_STRIDES`.
* The high-bit-depth `add8x8_idct8` uses the coefficient array itself as scratch:
  it adds 128 to `r1`, stores intermediate rows back into `dct[4..7]`, then
  reloads them — this avoids needing 16 simultaneous registers on SSE2.

## 5. Algorithm / behaviour

### 5.1 8-point 1D DCT (`DCT8_1D`)
The macro implements the 8-point integer DCT in a butterfly network:
1. Form even/odd pairs: `s07=x0+x7`, `d07=x0-x7`, `s16=x1+x6`, `d16=x1-x6`,
   `s25=x2+x5`, `d25=x2-x5`, `s34=x3+x4`, `d34=x3-x4`.
2. Form the four-point sums `a0=s34+s07`, `a1=s25+s16`, `a2=s34-s07`,
   `a3=s25-s16` (stored in the high half of the register file).
3. Compute the odd coefficients using shift-and-add factors approximating the
   DCT's irrational constants:
   * `a4 = d07 + (d07>>1) + d16 + d25`
   * `a5 = (d25 + (d25>>1)) - d07 - d34`
   * `a6 = (d16 + (d16>>1)) - d07 + d34`
   * `a7 = (d34 + (d34>>1)) + d16 - d25`
   (exact per macro: signs and operand order as written in the source).
4. Second stage: `b0=a0+a1`, `b4=a0-a1`; `b1=a2+(a3>>1)`,
   `b7=(a2>>2)-a3`; `b2=a1+(a1>>1)-a3`, `b6=(a1>>1)-a3`;
   `b3=a5+(a6>>2)`, `b5=a5-(a5>>2)-a6`, and variations.
5. Final `SWAP` reorders the eight outputs into natural coefficient order.

All shifts are arithmetic (`psra*`) — rounding is achieved by the surrounding
`+32`/`>>6` in the inverse path; the forward transform's `>>1`,`>>2` are the
H.264-defined integer approximations.

### 5.2 8-point 1D inverse (`IDCT8_1D`)
The inverse network starts from the even/odd coefficient pairs and rebuilds the
eight samples with the same shift-and-add factors, arranged as two cascaded
butterflies (`a*`, `b*`, `c*`) ending in eight sums/differences. The final
`SWAP` sequence maps the intermediate values to output rows.

### 5.3 High-bit-depth `sub8x8_dct8`
1. `LOAD_DIFF8x4` twice loads eight rows of `pix1-pix2` as words into `m0..m7`.
2. `DCT8_1D w` transforms along columns.
3. `TRANSPOSE4x4W` transposes the low four rows; `WIDEN_SXWD` sign-extends each
   word to dword (four registers). `DCT8_1D d` transforms along rows in dwords.
4. Store eight dword rows at offsets `0x00,0x20,...,0xE0`.
5. Repeat steps 3–4 for the high four rows (transpose with temporary `m0`).
Output: 64 `int32_t` coefficients in natural order.

### 5.4 High-bit-depth `add8x8_idct8`
1. Load eight dword coefficient rows; `IDCT8_1D d` along columns; transpose the
   two 4x4 dword halves; add +32 (`pd_32`) to the DC row of each half.
2. Use the coefficient buffer as scratch to save mid-stage rows; load the
   remaining eight coefficient rows and apply `IDCT8_1D d` + transpose again.
3. `IDCT8_1D d` twice more (once per half) to complete the 2D inverse.
4. `STORE_DIFF` for each of eight rows: shift right 6, add the destination word,
   clamp to `[0,pw_pixel_max]`, store.

### 5.5 8-bit `sub8x8_dct`
1. `add r2, 4*FDEC_STRIDE` then `.skip_prologue` (a shared entry).
2. `LOAD_DIFF8x4` loads four rows at a time via `LOAD_DIFF`, using `hsub_mul`
   with `pmaddubsw` on SSSE3.
3. `DCT4_1D` word butterfly, `TRANSPOSE2x4x4W` (transposes two 4x4 blocks
   together), `DCT4_1D` again, `STORE_DCT` writes four 16-word groups to `dct`.

### 5.6 8-bit `sub8x8_dct8`
1. Same row loads; `DCT8_1D w` word butterfly along columns.
2. `TRANSPOSE8x8W` full 8x8 transpose.
3. `DCT8_1D w` again along rows; eight `movdqa` stores write 64 words.
The SSE2 instantiation redefines `movdqa`→`movaps` and `punpcklqdq`→`movlhps`
to save code size (equivalent for the data paths used).

### 5.7 8-bit inverses
`ADD8x8_IDCT8` and `ADD8x8` apply `IDCT8_1D w` / `IDCT4_1D w`, transpose,
add `pw_32` to DC, apply the 1D inverse again, then `DIFFx2` + `STORE_IDCT`
perform `shift>>6`, add-to-destination, `packuswb`, and store two rows at a time.

### 5.8 AVX2 `sub16x16_dct8`
1. `add r0,128; add r2,4*FDEC_STRIDE`.
2. Two calls to `.sub16x8_dct8`, each loading eight 16-byte rows via
   `LOAD_DIFF16x2_AVX2` into 256-bit registers, applying `DCT8_1D w`,
   `TRANSPOSE8x8W`, `DCT8_1D w`, then extracting 128-bit halves to write eight
   coefficient rows at `[r0-0x80+...]` and `[r0+...]`.

### 5.9 Explicit per-stage equations

To make the butterfly behaviour reproducible without reading assembly, the
following equations describe the 8-point forward DCT stage algebraically. Let
`x0..x7` be one column (or row) of the residual block and let `>>k` denote an
arithmetic shift right by `k` (rounding toward negative infinity).

Even/odd decomposition:
```
s07 = x0 + x7 ;  d07 = x0 - x7
s16 = x1 + x6 ;  d16 = x1 - x6
s25 = x2 + x5 ;  d25 = x2 - x5
s34 = x3 + x4 ;  d34 = x3 - x4
```
Four-point stage:
```
a0 = s34 + s07 ;  a2 = s34 - s07
a1 = s25 + s16 ;  a3 = s25 - s16
```
Odd coefficients (integer approximations of `cos` products):
```
a4 = d07 + (d07>>1) + d16 + d25
a5 = d25 + (d25>>1) - d07 - d34
a6 = d16 + (d16>>1) - d07 + d34
a7 = d34 + (d34>>1) + d16 - d25
```
Final stage:
```
b0 = a0 + a1 ;  b4 = a0 - a1
b1 = a2 + (a3>>1) ;  b7 = (a2>>2) - a3
b2 = a3 + (a3>>1) - a1?  (per source: a1+(a1>>1)-a3) ;  b6 = (a1>>1) - a3
b3 = a5 + (a6>>2) ;  b5 = a5 - (a5>>2) - a6
```
The eight results are then permuted by the macro's final `SWAP` chain into
coefficient order `[b0,b1,b2,b3,b4,b5,b6,b7]` with the odd coefficients
interleaved as required by the H.264 basis ordering. A clean implementation
must reproduce the exact shift operands and operand order above; swapping the
sign of any term changes the transform's orthogonality and breaks exact
equivalence.

**Decision conditions** in the inverse path:
* The `+32` rounding bias is added only to the DC coefficient *before* the
  second 1D stage; adding it to all coefficients changes the result.
* The final right shift is by 6 for both 8-bit and high-bit-depth. For 8-bit
  the pack to bytes (`packuswb`) performs the clamp; for high-bit-depth an
  explicit `CLIPW` against zero and `pw_pixel_max` is required.
* `FIX_STRIDES` converts `intptr_t` strides to register-sized values; on x86-32
  this matters for frames larger than 2^31 bytes.

### 5.10 Shared-entry composition

The `.skip_prologue` labels exported by this file are the composition points
used by `dct-a.asm`:
* `sub8x8_dct_<isa>.skip_prologue` is loaded with `cextern` and called four
  times by `SUB_NxN_DCT sub16x16_dct_<isa>, sub8x8_dct_<isa>, ...` to build the
  16x16 forward transform.
* `add8x8_idct_<isa>.skip_prologue` plays the same role for the inverse.
* `add8x8_idct8_<isa>.skip_prologue` builds `add16x16_idct8_<isa>`.
* `sub8x8_dct8_<isa>.skip_prologue` builds `sub16x16_dct8_<isa>`.
The calling macro advances `r0` (output), `r1` (source row), and `r2` (predicted
row) by fixed offsets between calls. The callee must not modify these registers
beyond the documented pointer advancement, and must leave no ISA state (e.g.
upper YMM bits) dirty, since the caller uses `TAIL_CALL` to chain the final call.

## 6. Dependencies

* Includes `x86inc.asm`, `x86util.asm`.
* Uses macros from `dct-a.asm` by `cextern` of `.skip_prologue` labels for the
  composite 16x16 kernels built there.
* Called from the x86 dispatch layer for the listed ISA levels.
* Consumes C constants imported via `cextern`.

## 7. Constants and tables

* `pd_32` — four dwords of 32, the inverse rounding bias for high-bit-depth.
* `pw_32` — eight words of 32, the 8-bit inverse rounding bias.
* `pw_pixel_max`, `pw_2`, `pw_m2` — clamp and butterfly helpers from the C layer.
* `hsub_mul` — `[1,-1,1,-1,…]` byte multiplier used by `pmaddubsw` to compute
  `a-b` pairs in the 8-bit row loads.
* No locally defined tables.

## 8. Performance notes

* **Wider vectors:** The AVX2 `sub16x16_dct8` processes 16 bytes per vector
  (two 8x8 blocks' rows at once) with `vinserti128`, roughly halving the number
  of butterfly instructions versus two separate SSE2 calls.
* **Transpose strategy:** `TRANSPOSE2x4x4W` and `TRANSPOSE8x8W` are chosen to
  minimise shuffles for the given vector width; the high-bit-depth 8x8 transform
  transposes two 4x4 dword blocks separately rather than a single 8x8, because
  4x4 transposes need fewer instructions on SSE2.
* **Avoiding spills:** Unlike `dct-32.asm`, the x86-64 versions keep all eight
  data rows plus temporaries in registers, so no `SPILL`/`UNSPILL` macros are
  needed. This is the primary performance advantage of the 64-bit code.
* **Scratch reuse:** `add8x8_idct8` writes intermediate rows back into the
  caller's coefficient buffer rather than allocating stack, saving store/load
  traffic and preserving registers.
* **Code-size tricks:** aliasing `movdqa`→`movaps` in the SSE2 block removes
  instruction prefixes with no behavioural change for aligned data.
* **References:** see `dct-a.asm.md` section 8 for general transform speedups.
* **UNKNOWN:** exact cycle counts on modern CPUs; all variants are considered
  mature and dispatch picks the highest available ISA.

## 9. References

* ITU-T H.264 integer 8x8 transform definition.
* Public x264 source history (transform butterfly derivations).
* Intel/AMD SIMD programming manuals for `pmaddubsw`, `psrad`, `vinserti128`.

## 10. Verification checklist

* Compare `sub8x8_dct8` (both 8-bit and high-bit-depth) bit-exactly with the
  portable C implementation for random residuals.
* Verify `add8x8_idct8` reproduces reference reconstruction including the
  `+32 >> 6` rounding and final clamping.
* For 8-bit `sub8x8_dct`/`add8x8_idct`, confirm the four 4x4 groups match four
  independent `sub4x4_dct`/`add4x4_idct` calls.
* Check the high-bit-depth scratch-reuse path does not corrupt caller data beyond
  the documented in-place intermediate use (compare against a copy).
* AVX2 `sub16x16_dct8`: verify both 16x8 halves write to the correct coefficient
  offsets; off-by-one errors in the `-0x80` addressing are a risk.

## 11. Open questions

* `UNKNOWN`: the `DCT8_1D`/`IDCT8_1D` shift constants are hard-coded and their
  exact equivalence to the H.264 spec's `1/2`, `1/4` fractions for all input
  ranges should be confirmed by exhaustive small-range testing.
* `UNKNOWN`: whether the `sse4` variant differs behaviourally from `sse2` (it
  appears to be the same algorithm instantiated under a higher ISA label).
