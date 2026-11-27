# `common/mc.c` — motion compensation and plane utilities

**Role:** implements luma/chroma interpolation, weighted prediction, averaging, block
copies, plane-format conversions, hpel filter generation, integral images and mbtree
propagation helpers.
**Status:** dirty-side behavioural specification.

---

## 1. Header

- Source path: `common/mc.c` (header `common/mc.h`).
- One-sentence role: generate predicted samples from reference frames at integer/half/quarter
  offsets and provide the memory-shuffling and lookahead helpers that surround motion
  estimation.
- Marker: dirty-side behavioural specification.

---

## 2. Purpose

Motion compensation reconstructs inter-predicted blocks from previously decoded reference
pictures. H.264 luma uses a separable 6-tap half-pixel filter with quarter-pel values formed
by averaging half-pel samples; chroma uses direct bilinear interpolation at eighth-pel
resolution. This file holds those interpolators plus weighted prediction (explicit and
implicit), bidirectional averaging, and the "plane" utilities that convert between packed
(YUYV/UYUV/v210/RGB) and planar formats. It also builds the half-pel filtered reference planes
and the integral images used by successive-elimination motion search, and implements the
lookahead mbtree cost propagation arithmetic.

---

## 3. Public interface

Only `x264_mc_init(uint32_t cpu, x264_mc_functions_t *pf, int cpu_independent)` is exported;
all work goes through the vtable `x264_mc_functions_t` (declared in `mc.h`). Function
families:

- `mc_luma`, `get_ref` — luma motion compensation with weighting; `get_ref` may return a
  pointer into the reference instead of copying.
- `mc_chroma` — chroma bilinear interpolation for U and V.
- `avg[12]` — bidirectional averaging at all block sizes, with weight 32 or general weights.
- `copy[7]`, `copy_16x16_unaligned` — straight block copies.
- `weight`, `offsetadd`, `offsetsub`, `weight_cache` — weighted prediction callbacks and
  the `x264_weight_t` setup.
- `hpel_filter` — build half-pel filtered planes from a source plane.
- `plane_copy`, `plane_copy_swap`, `plane_copy_interleave`, `plane_copy_deinterleave*`,
  `store_interleave_chroma`, `load_deinterleave_chroma_fenc/fdec` — format conversion.
- `prefetch_fenc*`, `prefetch_ref`, `memcpy_aligned`, `memzero_aligned`.
- `integral_init4h/8h/4v/8v` — integral-image construction for successive elimination.
- `frame_init_lowres_core` — quarter-resolution downsampling for lookahead.
- `mbtree_propagate_cost`, `mbtree_propagate_list`, `mbtree_fix8_pack/unpack`.

---

## 4. State and data structures

- Reference planes: `src[4]` is an array of pointers to the original (index 0), horizontal
  (1), vertical (2) and centre (3) half-pel filtered planes. Quarter offsets index into
  these via two lookup tables:
  - `x264_hpel_ref0[16] = {0,1,1,1,0,1,1,1,2,3,3,3,0,1,1,1}`,
  - `x264_hpel_ref1[16] = {0,0,1,0,2,2,3,2,2,2,3,2,2,2,3,2}`,
  indexed by `qpel_idx = ((mvy&3)<<2) + (mvx&3)`.
- `x264_weight_t` stores scale, denominator, offset, two 8-entry cache arrays, and a function
  pointer. `x264_weight_none` is the zero-valued sentinel meaning "no weighting".
- `hpel_filter` needs an `int16_t *buf` scratch row of width+5 entries to hold intermediate
  vertical-filter results before the horizontal pass.
- Integral images are `uint16_t` planes; `frame->integral` stores 8x8 sums in the upper half
  and optional 4x4 sums in the lower half.
- Lifetime: all buffers are caller-owned; the vtable is initialized once.

---

## 5. Algorithm / behaviour

### 5.1 Half-pel filter (`hpel_filter`)

For each row, the 6-tap filter over samples `x[-2..x+3]` along a direction with stride `d` is

```
T = s[-2] + s[3] - 5*(s[-1] + s[2]) + 20*(s[0] + s[1])
```

(from the macro `TAPFILTER`). The filter is applied:

- **Vertically** (`d = stride`) for x from −2 to width+2: `dstv[x] = clip((T+16)>>5)`. The
  unclipped value `T` is stored into `buf[x+2]` (shifted by `pad` for high bit depth) for the
  subsequent horizontal pass.
- **Horizontally** on the buffered values (`d = 1`): `dstc[x] = clip((T2 - 32*pad + 512)>>10)`.
  The extra `512` and the 10-bit shift combine the two 5-bit scalings (32×32) with rounding.
  `pad = -10*PIXEL_MAX` for bit depths above 9, otherwise zero, to keep the 16-bit buffer from
  overflowing.
- **Horizontally** on the source row (`d = 1`): `dsth[x] = clip((T+16)>>5)`.

Thus the H plane is the horizontally filtered image, V the vertically filtered, and C the
centre (both directions) image. `x264_frame_filter` calls this per plane with 8-pixel
borders (`start = mb_y*16-8`), and, for interlaced coding, a second time on the field planes
with doubled stride so interpolation stays within a field.

### 5.2 Luma motion compensation (`mc_luma`, `get_ref`)

Given a motion vector `(mvx,mvy)` in quarter-pel units:

- `qpel_idx = ((mvy&3)<<2) + (mvx&3)`, `offset = (mvy>>2)*stride + (mvx>>2)`.
- `src1 = hpel_ref0[qpel_idx]` plane plus `offset`, with an extra stride when the vertical
  fraction is 3 (so the 3/4 position is read from one line below the 1/4 position's base).
- If `qpel_idx & 5` is nonzero — i.e. at least one fractional component is odd — a second
  pointer `src2 = hpel_ref1[qpel_idx]` plus `offset + ((mvx&3)==3)` is formed, and the output
  is the rounded average `(src1 + src2 + 1)>>1` per sample (`pixel_avg`).
- If no interpolation is needed, the block is copied directly (or, in `get_ref`, a pointer to
  `src1` is returned with the output stride set to the source stride).
- Weighted prediction, when active, is applied to the result with `mc_weight`.

The averaging is exact integer averaging with rounding up by one half; the 3/4 positions work
by averaging a full-pel sample with a half-pel sample from the appropriate neighbour plane.

### 5.3 Chroma motion compensation (`mc_chroma`)

Chroma motion vectors are in eighth-pel units. Let `dx = mvx&7`, `dy = mvy&7` and

```
cA = (8-dx)(8-dy),  cB = dx(8-dy),  cC = (8-dx)dy,  cD = dx dy.
```

The source pointer is advanced by `(mvy>>3)*stride + (mvx>>3)*2` (two chroma samples per
luma-pel step), and `srcp = src + stride`. For each output position `x`, U and V are formed by
bilinear interpolation of the four surrounding chroma samples:

```
u[x] = (cA*s[2x] + cB*s[2x+2] + cC*sp[2x] + cD*sp[2x+2] + 32) >> 6
v[x] = same with odd sample offsets (2x+1 etc.)
```

The `+32` implements rounding for the `>>6`. The implementation may write up to two bytes
past the end of the destination row, so callers must process blocks left-to-right.

### 5.4 Averaging and weighted prediction

- `pixel_avg_wxh`: `dst = (src1 + src2 + 1) >> 1` (equal-weight bidirectional prediction).
- `pixel_avg_weight_wxh` (implicit weighted bipred, `log2_denom=5`, zero offset, weights
  summing to 64): `dst = clip((src1*w + src2*(64-w) + 32) >> 6)`.
- General `mc_weight`: `offset = i_offset << (BIT_DEPTH-8)`; if `denom >= 1`,
  `dst = clip(((src*scale + (1<<(denom-1))) >> denom) + offset)`; otherwise the shift is
  skipped: `dst = clip(src*scale + offset)`.
- `weight_cache` selects `h->mc.weight` as the active weight function; `SET_WEIGHT` in `mc.h`
  either calls it (weights present) or clears the pointer.

### 5.5 Block copies and plane conversions

`mc_copy` uses `memcpy` per row. `copy[7]` covers the main block sizes, `copy_16x16_unaligned`
a 16x16 copy without alignment assumptions.

Plane helpers:
- `x264_plane_copy_c`: row-wise `memcpy` of `w` pixels with arbitrary strides.
- `plane_copy_swap_c`: swaps adjacent byte pairs over `2*w` values (16-bit sample swap).
- `plane_copy_interleave_c`: interleaves U and V into `dst[2x]=U[x], dst[2x+1]=V[x]`.
- `plane_copy_deinterleave_c`: the inverse, writing even samples to plane A and odd to B.
- `plane_copy_deinterleave_rgb_c`: extracts three planes with pixel pitch `pw` from packed RGB.
- `plane_copy_deinterleave_v210_c`: unpacks 10-bit v210 words into 16-bit Y/U/V; each 32-bit
  word carries three 10-bit components (U,Y,V / Y,U,Y), with a big-endian byte fix when needed.
- `store_interleave_chroma` / `load_deinterleave_chroma_*`: convert between the 8-wide FDEC
  chroma cache and interleaved frame memory.

The SIMD wrappers in `mc.h` (`PLANE_COPY`, `PLANE_COPY_SWAP`, `PLANE_COPY_YUYV`,
`PLANE_INTERLEAVE`) handle widths that are not a multiple of the vector width by processing
the aligned bulk with SIMD and the last row with the C routine, careful not to overread.

### 5.6 Integral images (successive elimination)

- `integral_init4h`: running horizontal sum of 4-sample windows, adding the value from the
  row `stride` entries earlier (16-bit wrapping).
- `integral_init8h`: same with 8-sample windows.
- `integral_init4v`: builds 4x4 sums as differences of 8-row sums, and refreshes the 8x8 sums
  as `sum8[y+8] + sum8[y+8][x+4] − sum8[y][x] − sum8[y][x+4]`.
- `integral_init8v`: `sum8 = sum8[y+8] − sum8[y]`.
`x264_frame_filter` calls these per row to maintain the integral planes; they wrap modulo
2^16 by design so the SIMD versions can use narrow accumulators.

### 5.7 Low-resolution frame and mbtree

- `x264_frame_init_lowres` duplicates the last column/row of the luma plane, calls
  `frame_init_lowres_core`, expands borders, and initializes cost/MV caches to −1 / 0x7FFF.
- `frame_init_lowres_core` computes four half-resolution planes from a 2x2 neighbourhood
  using nested rounded pair averages:
  `F(a,b,c,d) = (( (a+b+1)>>1 + (c+d+1)>>1 ) + 1) >> 1`. This is intentionally slower than
  plain bilinear to match the SIMD implementation bit-exactly.
- `mbtree_propagate_cost`: for each entry compute `inter = min(intra_cost, inter_cost &
  mask)`, `amount = propagate_in + intra_cost*inv_qscale*fps`, and
  `dst = min((amount*(intra−inter)/intra + 0.5), 32767)` (signed 16-bit).
- `mbtree_propagate_list`: distributes a propagation amount over the four macroblocks touched
  by a motion vector using bilinear weights `(32-y)(32-x)` etc., scaled with `(…+512)>>10`,
  and clipped adds into `ref_costs`; handles picture-edge cases with unsigned comparisons that
  also catch negative coordinates. In B frames the amount is first scaled by the bipred weight
  with `(…+32)>>6`.
- `mbtree_fix8_pack/unpack`: convert floats to/from Q8.8 fixed point with byte-order fixing.

### 5.8 Initialization / dispatch

`x264_mc_init` installs the C versions, then architecture init functions override them.
When `cpu_independent` is set, the two mbtree functions are forced back to the portable C
versions (used for multithreaded determinism). `x264_mc_init_mmx` selects among MMX2/SSE2/
SSSE3/AVX/AVX2 variants for copies, avg, weight, hpel, integral and plane conversions; ARM,
Altivec, MSA and LSX do likewise.

---

## 6. Dependencies

- **Calls:** `x264_clip_pixel`, `memcpy`/`memset`, SIMD MC kernels, `x264_frame_expand_border_lowres`.
- **Called by:** macroblock motion compensation (`encoder/macroblock.c`), motion search
  (`encoder/me.c`), lookahead/slice type decision (`encoder/slicetype.c`), frame filter setup,
  input format conversion.
- **Shared config:** `cpu_independent` forces deterministic mbtree; `PARAM_INTERLACED` and
  chroma format select field vs frame filtering.

---

## 7. Constants and tables

- `x264_hpel_ref0`/`x264_hpel_ref1` 16-entry tables (given in section 4).
- 6-tap coefficients `{1, −5, 20, 20, −5, 1}` with scale 1/32 (and the second pass 1/32 → total
  1/1024 for the centre plane).
- Rounding constants: `+16` for one 5-bit shift, `+512` for the 10-bit centre shift, `+32` for
  chroma's 6-bit shift, `+1` for equal-weight averaging and implicit-weight `1<<5`.
- `MC_CLIP_ADD`: `s = min(s+x, 32767)`.
- mbtree: `LOWRES_COST_SHIFT = 14`, bilinear scale `(…+512)>>10`, bipred `(…+32)>>6`.

---

## 8. Performance notes

MC is invoked for every inter macroblock and every motion-search candidate; weighted
prediction and averaging are hot in B slices and with `--weightp`.

Optimization guidance:

- Hpel filtering should be fused: compute the vertical 6-tap for a whole row into a 16-bit
  scratch buffer, then run the horizontal 6-tap on both the buffer and the original row,
  producing H/V/C planes in one pass. SIMD versions use `pmaddwd`/`pshufb` style taps.
- Quarter-pel luma MC is just two aligned loads and one rounded average — avoid branches by
  computing `qpel_idx & 5` as a mask and blending.
- Chroma MC is four multiplies and one shift per sample; use packed multiply-add where
  available and process U/V together.
- Averaging and weighted prediction should use saturating arithmetic and be fused into the MC
  copy to avoid an extra pass over the block.
- Plane copies benefit from non-temporal stores for large widths (the reference chooses a
  threshold of 256 pixels); for small widths plain `memcpy` is faster.
- Data layout: align reference planes and filtered planes to at least 16 bytes; keep the
  `src[4]` pointer array in cache.
- Literature: H.264 clause 8.4.2/8.4.3 defines luma 6-tap and chroma bilinear interpolation;
  efficient SIMD motion compensation is covered in the x264 developer notes and in numerous
  SIMD video-coding papers.

---

## 9. References

- ITU-T H.264 clauses 8.4.2 (luma MC), 8.4.3 (chroma MC), 7.4.3 (weighted prediction).
- Wiegand et al., "Overview of the H.264/AVC Video Coding Standard", IEEE TCSVT 2003.
- x264 public source: `common/mc.c`, `common/mc.h`, `common/tables.c`, SIMD MC kernels.

---

## 10. Verification checklist

1. **6-tap filter:** feed an impulse and a linear ramp into `hpel_filter` and verify the H/V/C
   outputs against the `{1,−5,20,20,−5,1}/32` filter and the combined `/1024` centre filter.
2. **Quarter positions:** for all 16 `(mvx&3,mvy&3)` combinations, compare `mc_luma` output
   against a reference that explicitly indexes the four half-pel planes per the two lookup
   tables; pay special attention to fractions 3 (extra row/column offset).
3. **Chroma:** for all 64 eighth-pel offsets, verify U and V interpolation weights and the
   `+32>>6` rounding; check the documented two-byte overrun by using guard pixels.
4. **Weighted prediction:** verify equal-weight, implicit-weight (weights summing to 64) and
   general explicit weights including the no-denominator path and negative offsets.
5. **Plane conversions:** round-trip planar ↔ interleaved ↔ deinterleaved data; for v210,
   test known 10-bit component triples.
6. **Integral images:** compare the four initializers against a brute-force 8x8/4x4 box-sum
   implementation, accounting for 16-bit wraparound.
7. **mbtree:** verify clip-to-32767, bipred scaling, bilinear edge handling and the
   `cpu_independent` override.
8. **Dispatch equivalence:** all SIMD vtable entries must match the C versions bit-exactly
   over random frames (checkasm-style).

---

## 11. Open questions

- `UNKNOWN`: exact SIMD lane ordering and the precise overread/overwrite behaviour of SIMD
  plane-copy wrappers are architecture-specific; only observable results matter.
- `UNKNOWN`: the `cpu_independent` flag may be set by threading code for determinism; its
  decision policy is outside this file.
- `UNKNOWN`: the 4:4:4 handling of chroma MC is delegated to callers (which invoke luma MC on
  all three planes).
