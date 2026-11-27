# `common/dct.c` — transform kernels and coefficient scans

**Role:** implements every integer forward/inverse transform used by the encoder plus the zigzag
scanning and CAVLC interleaving helpers; it is the arithmetic heart of the residual pipeline.
**Status:** dirty-side behavioural specification (derived from the reference source and public
H.264 material; no source text is reproduced).

---

## 1. Header

- Source path: `common/dct.c` (paired header `common/dct.h`).
- One-sentence role: converts the pixel-domain prediction residual into integer transform
  coefficients and back, and reorders those coefficients into the bitstream scan order.
- Marker: dirty-side behavioural specification.

---

## 2. Purpose

H.264 defines a family of exact integer approximations of the 4x4 and 8x8 DCT (ITU-T H.264
clause 8.5). The encoder needs (a) a forward transform that is *invertible with a known
rounding error* so the decoder reconstructs a predictable image, (b) an inverse transform that
adds the reconstructed residual back into the decoded picture buffer, (c) special "DC-only"
transforms for blocks whose only surviving coefficient is the DC term, and (d) a reordering of
the 2-D coefficient array into the zigzag sequence that the entropy coder expects. All of
these are fixed, bit-exact operations; they are gathered in one file so that the scalar
reference implementation and the SIMD replacements can be swapped through one dispatch
structure.

The file also owns the subtle distinction between **frame** (progressive) and **field**
(interlaced) scan orders, and between **16-bit** and **high bit depth** builds, both of which
change which function pointer is selected rather than the mathematics itself.

---

## 3. Public interface

Only two externally visible entry points exist (declared in `dct.h`):

- `x264_dct_init(uint32_t cpu, x264_dct_function_t *dctf)` — fills every pointer of the
  transform dispatch table with the portable C implementation, then selectively replaces
  entries with architecture-specific variants according to `cpu` feature bits.
- `x264_zigzag_init(uint32_t cpu, x264_zigzag_function_t *pf_progressive,
  x264_zigzag_function_t *pf_interlaced)` — fills **two** scan tables, one per scan type,
  because a sequence can switch between progressive and interlaced coding.

All other functions are file-static and reached exclusively through those tables. The
signatures, argument contracts and stride assumptions are enumerated in `dct.h.md`.
Important contract points:

- Forward transforms take a source block from the *encoding* picture (`FENC_STRIDE`) and a
  reference/prediction block from the *decoding* picture (`FDEC_STRIDE`) and write coefficients.
- Inverse transforms write into a decoded picture block with stride `FDEC_STRIDE`; they
  **add** to the existing samples (this is the H.264 reconstruction step) and saturate the
  result into the legal sample range.
- The `sub_*` family additionally *copies* the source samples into the reconstruction block
  as a side effect; callers rely on this for PCM/skip handling.
- Return values: forward scalar transforms return `void`; the `zigzag_sub_*` helpers return a
  boolean "any nonzero coefficient" flag.

---

## 4. State and data structures

- Coefficient arrays are flat row-major `dctcoef` (signed 16-bit in normal builds, signed
  32-bit in high-bit-depth builds — see `dct.h.md` for the `udctcoef` companion type).
  A 4x4 block occupies 16 entries, an 8x8 block 64; larger luma blocks are expressed as
  arrays of sub-blocks (`dct[4][16]`, `dct[16][16]`, `dct[4][64]`).
- The DC-only path uses compact arrays: four `dctcoef` for a 4:2:0 chroma 8x8 block, eight
  for a 4:2:2 chroma 8x16 block (two columns of four), sixteen for a luma 16x16 DC block.
- Two scratch buffers of 16 or 64 coefficients hold the intermediate result between the
  horizontal and vertical 1-D passes. They live on the stack; no persistent state exists.
- Ownership: the dispatch tables are owned by the encoder context and filled exactly once
  at startup. Coefficient buffers are owned by the caller (macroblock analysis / reconstruction).
- Invariants: forward 4x4 DC coefficient equals the sum of the 16 prediction differences;
  inverse 4x4 adds a value whose DC component is `round(dc/64)` to each sample; all sample
  writes are clipped to `[0, PIXEL_MAX]`.

---

## 5. Algorithm / behaviour

### 5.1 Forward 4x4 transform (`sub4x4_dct`)

First the 4x4 residual is formed by subtracting, sample by sample, the prediction block from
the source block. Then a separable integer transform is applied in two identical 1-D passes
(columns first, then rows, with a transpose implicit in the indexing).

For a length-4 vector `(a,b,c,d)` the 1-D kernel computes four sums using the symmetric
pairs `(a,d)` and `(b,c)`:

- `p = a+d`, `q = b+c`, `r = a-d`, `s = b-c`;
- output lanes: `y0 = p+q`, `y1 = 2r+s`, `y2 = p-q`, `y3 = r-2s`.

Equivalently the transform matrix (rows = output basis) is

```
[ 1  1  1  1 ]
[ 2  1 -1 -2 ]
[ 1 -1 -1  1 ]
[ 1 -2  2 -1 ]
```

applied to the column vector. There is **no rounding and no shift** in the forward 4x4
transform; the DC lane therefore grows by a factor of 4 after the two passes (each pass
sums four samples). The dynamic range is bounded because input differences are at most
`±PIXEL_MAX`.

Larger luma blocks are just tiled 4x4 blocks: `sub8x8_dct` performs four 4x4 transforms
(quadrants in raster order) and `sub16x16_dct` four 8x8 groups.

### 5.2 Inverse 4x4 transform (`add4x4_idct`)

The inverse is the transpose-scaled transform with a fixed shift of 6 and rounding by adding
32 before the right shift. For a length-4 vector `(a,b,c,d)` the 1-D inverse first forms:

- `u = a+c`, `v = a-c`, `w = b+(d>>1)`, `z = (b>>1)-d`;
- lanes before scaling: `t0 = u+w`, `t1 = v+z`, `t2 = v-z`, `t3 = u-w`;
- final output: `round_shift6(t0), round_shift6(t1), round_shift6(t2), round_shift6(t3)`,
  where `round_shift6(x) = (x+32)>>6`.

The halving of the odd inputs (`d>>1` and `b>>1`) compensates the factor-of-2 in the forward
odd rows so that, after dividing by 64, the round trip is correct. After the two passes,
each of the 16 output residuals is added to the existing reconstruction sample and the sum
is saturated with `x264_clip_pixel`. The 8x8 and 16x16 inverse luma transforms are again
quadrant decompositions of the 4x4 inverse.

The DC-only inverse (`add4x4_idct_dc`, used for chroma and for P/B blocks with only DC
residual) is much cheaper: compute `dc' = (dc+32)>>6` and add that single value to every
sample of each 4x4 block, clipping. `add8x8_idct_dc` and `add16x16_idct_dc` apply this to
four or sixteen 4x4 sub-blocks using the per-block DC values from the compact DC array.

### 5.3 8x8 transform (High profile)

The 8-point forward kernel is a butterfly over mirrored pairs. For input vector `x0..x7`:

- Define pair sums `s0=x0+x7`, `s1=x1+x6`, `s2=x2+x5`, `s3=x3+x4`, and pair differences
  `d0=x0-x7`, `d1=x1-x6`, `d2=x2-x5`, `d3=x3-x4`.
- Even lanes: `a0=s0+s3`, `a1=s1+s2`, `a2=s0-s3`, `a3=s1-s2`, then
  `y0=a0+a1`, `y2=a2+(a3>>1)`, `y4=a0-a1`, `y6=(a2>>1)-a3`.
- Odd lanes: form `1.5×` versions of the differences (`d + (d>>1)`), combine them into
  `a4..a7`, then
  `y1=a4+(a7>>2)`, `y3=a5+(a6>>2)`, `y5=a6-(a5>>2)`, `y7=(a4>>2)-a7`.

This is the H.264 8x8 integer transform with its characteristic `1/2` and `1/4` scaling on
odd/even output lanes; the right shifts are arithmetic (sign-preserving). Two passes with
an implicit transpose give the 2-D result. `sub16x16_dct8` tiles four 8x8 transforms.

The inverse 8x8 kernel is the corresponding butterfly with divisions folded into right
shifts: it first adds a rounding constant of 32 to the DC coefficient, performs a 1-D pass
in place over the coefficient array, then a second pass whose output is right-shifted by 6,
clipped, and added to the reconstructed samples. The two passes are indexed so the second
one reads down columns and writes across rows.

### 5.4 DC-only transforms for chroma (4:2:0 and 4:2:2)

`sub8x8_dct_dc` first obtains the four 4x4 block DC values by summing each 4x4 residual
(16 signed pixel differences per quadrant), then applies a 2x2 Hadamard:

- `u0=d0+d1`, `u1=d2+d3`, `u2=d0-d1`, `u3=d2-d3`;
- output `c0=u0+u1`, `c1=u0-u1`, `c2=u2+u3`, `c3=u2-u3`.

For 4:2:2 chroma (`sub8x16_dct_dc`) eight 4x4 DC values (two columns by four rows) are
combined by a 2x4 Hadamard: sum/difference the pairs in each row, then sum/difference the
resulting pairs, with the final stage producing the eight output coefficients in a specific
interleaved order (the even/odd split is done one level earlier than in the 2x2 case).

`dct4x4dc` / `idct4x4dc` form an inverse pair of 4-point Hadamard transforms over a
16-coefficient DC array (the four luma 16x16 or chroma blocks): the forward version applies
a `+1` rounding before a one-bit right shift in its second pass (so two applications give
the standard half-scale), while the inverse has no rounding and no shift.

`dct2x4dc` consumes the eight per-4x4 DC coefficients of a 4:2:2 chroma block, applies the
2x4 Hadamard, writes the eight results into the dedicated DC coefficient array, and **zeros
all eight source DC coefficients** so they are not coded twice.

### 5.5 Zigzag scans

A scan is a permutation from scan position `i` to coefficient array index. Coordinate
convention: for a 4x4 block the coefficient at row `r`, column `c` is stored at `dct[c*4+r]`;
for 8x8 it is `dct[c*8+r]`. The scans are listed by scan position as `(row,column)` pairs of
the underlying 2-D coefficient array.

- **4x4 frame (progressive):** the classic alternating anti-diagonal traversal:
  (0,0), (0,1), (1,0), (2,0), (1,1), (0,2), (0,3), (1,2), (2,1), (3,0), (3,1), (2,2),
  (1,3), (2,3), (3,2), (3,3).
- **4x4 field (interlaced):** DC and the first AC coefficient are emitted in raster order,
  then the remaining coefficients are read in field-scan order:
  (0,0), (1,0), then (0,1),(2,0),(3,0),(1,1),(2,1),(3,1),(0,2),(1,2),(2,2),(3,2),
  (0,3),(1,3),(2,3),(3,3). The first two positions are copied directly rather than
  permuted; the rest follow the field permutation.
- **8x8 frame:** the standard 8x8 zigzag, beginning (0,0),(0,1),(1,0),(2,0),(1,1),(0,2),
  (0,3),(1,2),(2,1),(3,0),(4,0),(3,1),(2,2),(1,3),(0,4),(0,5), … and ending with the
  bottom-right corner (7,7). The full 64-entry table is a fixed public H.264 constant
  (`Annex Table 8-12`-style zigzag).
- **8x8 field:** a vertical-field-first ordering, beginning (0,0),(1,0),(2,0),(0,1),
  (1,1),(3,0),(4,0),(2,1),(0,2), … and likewise ending at (7,7).

`scan_4x4` / `scan_8x8` write the reordered coefficients into a separate `level` array; the
original coefficient array is left untouched.

### 5.6 `sub_*` scan helpers

The `sub_4x4` and `sub_8x8` helpers combine three operations in one pass: compute each
coefficient as `source_sample - destination_sample` in scan order, OR all coefficients into
a nonzero accumulator, and copy the source samples into the destination block. `sub_4x4ac`
is the chroma-DC variant: it stores the single DC difference into a caller-provided pointer
and writes zero into the DC position of the `level` array, so only AC coefficients are
retained. The return value is `!!accumulator` — zero iff every coefficient vanished.

### 5.7 CAVLC interleaving

`interleave_8x8_cavlc` takes four 8x8 coefficient sub-blocks (`src[i]`, i=0..3) and produces a
layout suitable for CAVLC: for each sub-block, the 16 coefficients at stride-4 positions
`src[i + 4*j]` are written contiguously as `dst[i*16 + j]`. While doing so it computes a
nonzero flag for that sub-block and stores it at `nnz[(i&1) + (i>>1)*8]`, i.e. in the first
two columns of a 8x2 nnz cache with the two block rows separated by 8 entries.

### 5.8 Dispatch / initialization contract

`x264_dct_init` first assigns every C function, then, depending on compiled-in support and
`cpu` bits, swaps in SIMD implementations in increasing capability order so the strongest
available wins. On x86 the ladder is roughly MMX → MMX2 (plus cache/ Atom exceptions) →
SSE2 (with a "SSE2 slow" qualifier) → SSSE3 → SSE4 → AVX → XOP → AVX2 → AVX512, with
different subsets upgraded for high-bit-depth versus 8-bit builds; e.g. the high-bit-depth
path enables SSE2 transforms but reserves the SSSE3/SSE4 upgrades mainly for the 8x8
transform. ARM (NEON, SVE, SVE2), PowerPC Altivec, MIPS MSA and LoongArch LSX/LASX follow
the same pattern. `x264_zigzag_init` similarly installs progressive and field scan
functions, and a separate interleave pointer shared by both tables.

---

## 6. Dependencies

- **Calls:** `x264_clip_pixel` / saturation helpers from `common.h`, `memcpy` for the field
  scan's untouched prefix, and (in SIMD variants) architecture headers pulled in at the top.
- **Called by:** macroblock encode/decode and reconstruction (`encoder/macroblock.c`), rate
  distortion analysis (`encoder/rdo.c`), deblock-aware RD (`common/deblock.c`), and the
  zigzag function users in the entropy coders.
- **Shared globals:** none; all state is passed in. The `cpu` feature mask comes from
  `common/cpu.c`; scaling-list/QM values that feed the quant stage are prepared elsewhere.

---

## 7. Constants and tables

- Forward 4x4 rows: `(1,1,1,1)`, `(2,1,-1,-2)`, `(1,-1,-1,1)`, `(1,-2,2,-1)`.
- Inverse 4x4 scaling: right shift 6 with bias 32; odd input lanes are halved before the pass.
- 8x8 forward constant factors: pair sums/differences, `1.5 = 1 + (>>1)` on odd differences,
  quarter shifts `(>>2)` and half shifts `(>>1)` on selected lanes; inverse adds 32 to DC and
  shifts by 6 at the end.
- DC-only 2x2 and 2x4 Hadamard matrices are unnormalised; the inverse 4x4 DC Hadamard uses
  a one-bit shift in the forward direction only.
- Zigzag tables: 16-entry frame/field 4x4 and 64-entry frame/field 8x8 permutations.
- `FDEC_STRIDE`/`FENC_STRIDE` are compile-time strides used by all block addressing.

---

## 8. Performance notes

These kernels run once or more for **every residual block of every macroblock**, so they are
among the hottest functions at all presets, especially `--preset ultrafast/veryfast` where
the encoder is transform/quantization bound. The C code is deliberately written as short
butterflies with accumulate-in-register temporaries; compilers vectorise the row passes only
partially because of the transposes.

Concrete optimization guidance:

- The two separable passes should be fused in SIMD: load a 4x4 or 8x8 tile into vector
  registers, do the horizontal butterfly, transpose in-register (or via shuffle), then do
  the vertical butterfly. This removes the scratch arrays entirely.
- The saturating add benefits from SIMD `packus`/`saturate` instructions; doing the clip with
  scalar branches is a measurable loss.
- DC-only paths should be kept separate (as they are) — they are an order of magnitude
  cheaper and are extremely common in P/B slices and chroma.
- Data layout: keep `FENC_STRIDE` and `FDEC_STRIDE` multiples of 16 bytes and align plane
  starts; the SIMD variants assume at least 16-byte (often 32-byte for AVX2) alignment for
  full speed, while the scalar path has no alignment constraint.
- Branch behaviour: the field zigzag has a couple of special first coefficients; keep those
  outside the main loop to avoid per-coefficient branches.
- Reference literature: the H.264 integer transform derivation is covered in the ITU-T
  H.264 standard clause 8.5 and in the widely cited paper "A Fast Transform for the H.264
  Video Coding Standard" and in Bjøntegaard/IEEE treatments of the 4x4/8x8 integer DCT;
  x264's own SIMD transform tricks are documented in the project's developer notes and
  checkasm test suite.

---

## 9. References

- ITU-T Recommendation H.264 (ISO/IEC 14496-10), clauses 8.5 (transform) and 9.2/9.3
  (coefficient scanning).
- "Overview of the H.264/AVC Video Coding Standard", Wiegand et al., IEEE TCSVT 2003.
- "The H.264/MPEG-4 AVC standard and its applications", reservoir/overview literature.
- x264 developer documentation and the `checkasm` transform tests (public source tree).

---

## 10. Verification checklist

An independent implementation can be validated observably as follows:

1. **Golden coefficient vectors:** feed a known 4x4 residual (e.g. a single impulse in the
   top-left, or a uniform block) and compare the full coefficient array against the expected
   transform values; a constant block must produce all energy in the DC lane, with
   `DC = 16 × constant` (4x4) or `64 × constant` (8x8).
2. **Round-trip identity:** forward transform followed by inverse transform and clipping,
   starting from an arbitrary reconstruction block, must reproduce the source block within
   the known one-LSB tolerance; compare bit-exactly against the reference for a fixed
   pseudo-random seed.
3. **Symmetry/orthogonality probes:** the transform of a horizontally mirrored input must be
   the mirrored output with odd-row signs flipped; this detects reversed basis vectors.
4. **Zigzag coverage:** for every scan function, verify that feeding coefficients `1..N` at
   distinct positions yields a permutation containing each integer exactly once, and that
   frame/field scans differ only in the prescribed positions.
5. **Side effects:** `sub_*` helpers must leave the destination memory equal to the source
   block; verify with guard bytes around the block to detect overreads/overwrites.
6. **Dispatch equivalence:** for each `cpu` feature level that is available, the SIMD entry
   must produce byte-identical outputs and identical nonzero flags to the C reference over
   thousands of random blocks (this is exactly the checkasm methodology).
7. **High-bit-depth:** repeat with 10-bit/12-bit samples and verify that clipping uses
   `PIXEL_MAX = (1<<BIT_DEPTH)-1` and that coefficient widths are wide enough to hold the
   larger intermediate sums.

---

## 11. Open questions

- `UNKNOWN`: the exact register-level implementation of each SIMD variant (lane ordering,
  use of `pmaddwd`/`phaddw`/`vpermd`, stack alignment tricks) is architecture-specific and
  intentionally not specified here; only the observable output contract matters.
- `UNKNOWN`: whether a future build enables the 8x8 transform's DC-only shortcut differently
  for 4:4:4 (the file delegates plane count to the caller).
- `UNKNOWN`: the precise rounding behaviour of the field 8x8 zigzag's last few coefficients is
  defined by the published scan table, which must be consulted rather than inferred from the
  forward transform.
