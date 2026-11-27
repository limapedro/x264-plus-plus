# `common/aarch64/mc-a.S` — NEON motion compensation, copies, chroma interpolation, filters

> Dirty-side behavioural specification (reverse-engineered from the reference encoder; describes behaviour, not code).

## 1. Header

- Source path: `common/aarch64/mc-a.S`
- Role: large NEON implementation of motion-compensation support kernels: prefetch, averaging, weighted prediction, block copies, chroma interpolation, half-pixel filtering, low-resolution frame init, plane copies, and SATD integral-image initialization.
- This is a **dirty-side behavioural specification** artifact for the clean-room reimplementation.

## 2. Purpose

Motion compensation and its supporting utilities dominate encoder memory bandwidth. This file collects the AArch64 implementations of the function-pointer table populated by `x264_mc_init_aarch64`. It contains separate 8-bit and 10-bit sections; the 8-bit section is described in detail below, with the 10-bit differences noted.

## 3. Public interface

Exported symbols (see `mc-c.c.md` for the full table):

**Prefetch / utility:** `prefetch_ref_aarch64`, `prefetch_fenc_420_aarch64`, `prefetch_fenc_422_aarch64`, `memcpy_aligned_neon`, `memzero_aligned_neon`, `mbtree_fix8_pack_neon`, `mbtree_fix8_unpack_neon`.

**MB-tree:** `mbtree_propagate_cost_neon`, `mbtree_propagate_list_internal_neon` (called by the macro-generated list wrapper in `mc-c.c`).

**Averaging:** `pixel_avg_{16x16,16x8,8x16,8x8,8x4,4x16,4x8,4x4,4x2}_neon`, `pixel_avg2_w{4,8,16,20}_neon`, and internal `pixel_avg_w{4,8,16}_neon` and `pixel_avg_weight_w{4,8,16}_{add_add,add_sub,sub_add}_neon`.

**Weighted prediction:** `mc_weight_w{4,8,16,20}_neon`, `mc_weight_w{4,8,16,20}_nodenom_neon`, `mc_weight_w{4,8,16,20}_{offsetadd,offsetsub}_neon`.

**Copies:** `mc_copy_w{4,8,16}_neon`.

**Chroma interpolation:** `mc_chroma_neon`, internal `mc_chroma_w2_neon`, `mc_chroma_w4_neon`, `mc_chroma_w8_neon`.

**Filters/utilities:** `hpel_filter_neon`, `frame_init_lowres_core_neon`, `load_deinterleave_chroma_{fenc,fdec}_neon`, `plane_copy_core_neon`, `plane_copy_swap_core_neon`, `plane_copy_deinterleave_neon`, `plane_copy_deinterleave_rgb_neon`, `plane_copy_interleave_core_neon`, `store_interleave_chroma_neon`, `integral_init{4,8}{h,v}_neon`.

## 4. State and data structures

- Weight structure `x264_weight_t`: at offset 0 the offset (for simple offset kernels), at offset 32 the denominator, at offset 36 the scale, at offset 40 the offset used by full-weight kernels. The kernels read these directly with fixed offsets.
- `pw_0to15` table: 16 halfwords 0..15, used by `mbtree_propagate_list_internal_neon` to generate per-MB offsets.
- No persistent kernel state; all loops are register-resident.
- 10-bit section uses 32-bit lanes and 16-byte loads holding four pixels.

## 5. Algorithm / behaviour

### Prefetch

`prefetch_ref_aarch64( pix, stride, parity )` issues eight `pldl1strm` prefetches covering two groups of four cache lines, assuming a 64-byte cache line. When `parity == 1`, a row-stride offset is omitted (cselected to zero), shifting the pattern for the second parity.

`prefetch_fenc_{420,422}_aarch64( pix_y, stride_y, pix_uv, stride_uv, mb_x )` prefetches four luma lines (offset by `mb_x & 3` times four rows) and two chroma lines (or four for 4:2:2).

### MB-tree kernels

`mbtree_propagate_cost_neon` processes eight entries per iteration: converts integer fields (propagate_intra, propagate_num, propagate_denom) to float, computes a reciprocal estimate with one Newton refinement (`frecpe`/`frecps`), and accumulates `propagate_amount * (intra + (1 − num/denom))` into a 16-bit output using `fcvtns` and `sqxtn`.

`mbtree_propagate_list_internal_neon` distributes eight propagation amounts across four spatial positions using bilinear weights derived from `(x & 31, y & 31)`: weights `(32−y)(32−x)`, `(32−y)x`, `y(32−x)`, `yx`, multiplied by the amount and rounded right by 10; results stored as four 32-byte groups. It also handles the bipred case (`lists_used == 3`) by replacing the amount with `(amount * bipred_weight + 32) >> 6`, and writes per-column offsets (`(mv >> 5) + index`) for later accumulation.

### Memory utilities

`memcpy_aligned_neon` copies a byte count assumed to be a multiple of 16: one optional 16-byte head, one optional 32-byte block, then 64-byte chunks using pair loads/stores. `memzero_aligned_neon` zeroes 128-byte chunks with four pair stores. `mbtree_fix8_pack_neon` converts floats to signed 16-bit with 8 fractional bits, byte-swaps 16-bit lanes (`rev16`) for a portable fixed-point file format, with a scalar tail; unpack reverses the process (`rev16`, `scvtf` with scale 8).

### Averaging kernels

`pixel_avg_WxH_neon` dispatches on `weight` exactly like the SVE variant: weight 32 → plain `pixel_avg_w{8,16}` or `pixel_avg_w4`; otherwise one of three weighted loops. Plain averaging uses `urhadd` (rounding halving add) on 8/16-byte rows, processing 2–4 rows per iteration. Weighted kernels broadcast the two weights into byte vectors, compute unsigned 16-bit products with `umull`/`umlal` (or `umlsl`), and narrow with `sqrshrun #6` (rounding shift right by 6 with saturation). The three sign cases are:
- `add_add`: `w1*s1 + w2*s2`
- `add_sub`: `w1*s1 − w2*s2` (weight > 64)
- `sub_add`: `w2*s2 − w1*s1` (weight < 0)

`pixel_avg2_w{4,8,16,20}_neon` is the simpler fixed 50/50 variant used by `mc_luma` when no explicit weight is set; w20 handles the chroma width-20 case by processing 16 bytes plus one 4-byte tail with a separately adjusted destination stride.

### Weighted prediction kernels

`mc_weight_wN_neon` (full form): reads denominator, scale, and offset from the weight structure; computes per-sample `(src * scale) >> denom + offset` using `umull` + `srshl` (arithmetic rounding shift by negative denominator) and `sqxtun` saturation. Widths 4, 8, 16 use 4/8/16-byte loads; w20 handles the interleaved chroma layout by zipping two 8-byte halves into a 16-byte vector plus a 4-byte tail.

`mc_weight_wN_nodenom_neon`: denominator is zero, so it uses `umlal` into an accumulator initialized with the offset, then saturates — computing `src*scale + offset` without a shift.

`mc_weight_wN_offsetadd_neon` / `offsetsub_neon`: scale is unity and only an offset applies; uses `uqadd`/`uqsub` (saturating byte add/subtract) with a broadcast offset. The corresponding offset sign is cached by `weight_cache_neon` in `mc-c.c`.

### Copies

`mc_copy_w{4,8,16}_neon` copies four rows per iteration using single-lane (w4) or 8/16-byte loads and stores, with explicit strides.

### Chroma interpolation

`mc_chroma_neon( dst_u, dst_v, dst_stride, src, src_stride, dx, dy, width, height )` reads height from the stack (9th argument), computes the integer part of `dx`/`dy` (`sbfx` = arithmetic shift right by 3 with sign extension) to offset the source pointer, masks the fractions to 0..7, and dispatches by width (2 → `mc_chroma_w2`, 4 → `mc_chroma_w4`, else `mc_chroma_w8`).

The interpolation coefficients are `cA=(8−dx)(8−dy)`, `cB=dx(8−dy)`, `cC=(8−dx)dy`, `cD=dx*dy`, stored duplicated in vector registers. Three paths:
- Both fractions nonzero: four samples per output (two rows × two horizontal positions) are multiplied and accumulated in 16-bit lanes, then narrowed with `rshrn #6` (rounding divide by 64).
- `dx == 0`: vertical-only interpolation of two rows; results de-interleaved into U and V planes with `uzp1`/`uzp2`.
- `dy == 0`: horizontal-only interpolation; neighbour samples obtained by `ext` shifting by one byte within interleaved U/V data.

Widths 2 and 4 use 8-byte vectors with `idx2` set to 1 or 2 to place the second store in the correct half; width 8 uses 16-byte vectors. All outputs are de-interleaved into separate U and V destination planes.

### Half-pixel filter

`hpel_filter_neon( dsth, dstv, dstc, src, stride, width, height, buf )` implements the 6-tap H.264 half-pixel filters:
- Horizontal: `(src[−2] − 5*src[−1] + 20*src[0] + 20*src[1] − 5*src[2] + src[3] + 16) >> 5`, computed with `uaddl`/`umlsl`/`umlal` (coefficients 5 and 20) and `sqrshrun #5`.
- Vertical: same formula along columns using six rows loaded with stride.
- Centre (dstc): applying the vertical filter to the horizontal-filtered rows, built via `ext` shifts of already-computed horizontal results and the same `(a − 5b + 20c)/16` idiom with a final `sqrshrun #6`.
It aligns `src` and destination pointers to 16 bytes using the low bits of the source pointer, and loops over 16-pixel chunks and all rows.

### Low-resolution frame init

`frame_init_lowres_core_neon` takes three consecutive source rows and produces four half-resolution outputs: plain downsample (average of two vertically adjacent samples and two horizontally adjacent samples), and h/v/c variants used for motion estimation. Uses `ld2` to de-interleave U/V-style source data? (for luma it loads 16 bytes and the next 16), `urhadd` for all averaging, and writes four destination rows per iteration with a width loop unrolled twice.

### Plane copies and chroma load/store

- `plane_copy_core_neon`: row-by-row 16-byte load/store loop.
- `plane_copy_swap_core_neon`: same with byte reversal within each 16-byte block (`rev64`/`rev32`) for 4:2:0 ↔ 4:2:2 conversions.
- `plane_copy_deinterleave_neon`: `ld2` into U and V planes.
- `plane_copy_deinterleave_rgb_neon`: `ld3`/`ld4` based de-interleave for RGB input with configurable plane width and total width.
- `plane_copy_interleave_core_neon`: `st2` to interleave U/V into interleaved chroma.
- `load_deinterleave_chroma_{fenc,fdec}_neon`: load interleaved chroma with strides 16/32 and split into U/V planes.
- `store_interleave_chroma_neon`: reverse operation.

### SATD integral initialization

`integral_init4h_neon`: for each output position, sums four consecutive source pixels plus the previous row's integral value (loaded from `dst − 2*stride`), producing 16-bit horizontal SATD integrals; `integral4h` macro uses `ext` shifts by 1,2,3 and `uaddl`.
`integral_init8h_neon`: same with eight taps, using `ext` shifts 1..7.
`integral_init4v_neon`: vertical 4-tap differences between rows separated by 8 and 16 rows, producing two outputs per iteration: raw vertical sums and second-stage differences.
`integral_init8v_neon`: vertical 8-tap difference between row 0 and row 16, with an 8-byte head for alignment.

### 10-bit section

The `#else` branch repeats the averaging, copy, weight, chroma, integral, and plane-copy kernels with 32-bit lanes (four 10-bit pixels per 16-byte vector), different weight-loading macros (`load_weights_add_add` etc. negate `w6`/`w7` differently because of signed 10-bit samples), and a `pixel_avg_w4_neon` instead of w8/w16 variants. It omits the MB-tree, prefetch, and lowres functions (those are shared above the `#if`).

### Quarter-pixel reference selection

`mc_luma_neon` and `get_ref_neon` use a single index to choose the two integer-plane references that must be averaged. With `mx = mvx & 3`, `my = mvy & 3`, `qpel_idx = (my << 2) + mx`:

| `(mx,my)` | Interpolation needed? | Planes |
| --- | --- | --- |
| (0,0) | no | integer plane only |
| (2,0), (0,2), (2,2) | yes (half-pel) | two half-pel planes |
| odd values | yes (quarter-pel) | one integer/half plane and one complementary half/centre plane |

The test `qpel_idx & 5` is a compact way of identifying the cases requiring an average (bit 0 = mx odd, bit 2 = my odd; combined with the pure-half cases via the table). The extra row added when `my == 3` compensates for the reference tables pointing at row `floor(mvy/4)` rather than `ceil`.

### Weighted-prediction equation

For the full-weight kernels, each output byte is:

```
out = Clip1( (src * scale + (denom >> 1)) >> denom + offset )
```

The assembly realizes this as `umull` (or `umlal` for the nodenom case) followed by `srshl` with a negative shift amount (rounding arithmetic shift right) and `sqxtun` saturation. For the nodenom case (`denom == 0`) the accumulator is initialized with the offset and no shift is applied. For the offset-only case the formula reduces to `Clip1(src ± offset)` via `uqadd`/`uqsub`.

### Averaging equation

Plain half-pel averaging is `out = (s1 + s2 + 1) >> 1` (`urhadd`). Weighted averaging in the 6-bit fixed-point domain is:

```
out = Clip1( (w1*s1 + w2*s2 + 32) >> 6 )
```

with `w1 + w2 = 64`. The three sign cases map to:
- `0 < w1 < 64`: `w2 = 64 − w1 > 0` → multiply-add.
- `w1 > 64`: `w2 = 64 − w1 < 0` → multiply-subtract.
- `w1 < 0`: `w1` negative, `w2 = 64 − w1 > 64` → multiply-subtract with operands swapped.

### Chroma interpolation coefficients

With fractional offsets `dx,dy ∈ [0,7]`:

```
cA = (8−dx)(8−dy)   cB = dx(8−dy)
cC = (8−dx)dy       cD = dx·dy
out = (cA*p(0,0) + cB*p(1,0) + cC*p(0,1) + cD*p(1,1) + 32) >> 6
```

The kernel special-cases `dx == 0` or `dy == 0` to reduce four products to two, and de-interleaves the U/V samples with `uzp`/`ext` before storing.

### Half-pixel filter equations

The 6-tap filters (integer samples `s[-2]..s[3]`) are:

```
horizontal/temporary = (s[-2] − 5*s[-1] + 20*s[0] + 20*s[1] − 5*s[2] + s[3] + 16) >> 5
centre               = (row[-2] − 5*row[-1] + 20*row[0] + 20*row[1] − 5*row[2] + row[3] + 32) >> 6
```

where the centre filter applies the vertical form to already-horizontally-filtered rows. Coefficients 5 and 20 are broadcast vectors; the dividing shifts (5 and 6) use `sqrshrun` for simultaneous rounding, shifting, and byte saturation.

### Low-resolution downsampling

For each 2×2 source group, the four outputs are vertical averages of horizontally averaged pairs:

```
a = (s0[2i] + s1[2i] + 1) >> 1
b = (s0[2i+1] + s1[2i+1] + 1) >> 1
c = (s1[2i] + s2[2i] + 1) >> 1
d = (s1[2i+1] + s2[2i+1] + 1) >> 1
dst0 = (a+b+1)>>1 ; dstv = (c+d+1)>>1 ; dsth = (a+c+1)>>1 ; dstc = (b+d+1)>>1
```

### SATD integral equations

Horizontal 4-tap: `integral4h[i] = src[i]+src[i+1]+src[i+2]+src[i+3] + prev_row_integral[i]`. Vertical 4-tap: `integral4v[i] = sum(src[i], src[i+8], src[i+16])` differences combined with an 8-row-delayed copy. The kernels use `ext` shifts to form the shifted operands and `uaddl`/`sub` to accumulate/saturate into 16-bit lanes.

## 6. Dependencies

- Includes `asm.S` and `mc-a-common.S`.
- Called from `mc-c.c`'s `x264_mc_init_aarch64`, which assigns all function pointers and defines the weight tables.
- `mc_chroma_neon` dispatches internally to the three width helpers.
- No calls into other architecture files.

## 7. Constants and tables

- `64`: nominal weight and cache-line size.
- `5`, `20`, `16`, `32`: half-pixel filter coefficients and rounding constants.
- `8`, `6`: chroma filter scale/shift; weighted-prediction shift 6.
- `pw_0to15`: 16 halfwords used for MB-tree per-MB offsets.
- Weight-structure offsets 0, 32, 36, 40.
- No other tables.

## 8. Performance notes

Extremely hot for motion estimation/compensation and frame copying. The reference unrolls row loops by 2 or 4, uses post-incrementing loads/stores to avoid explicit address arithmetic, and keeps weight constants in registers across whole blocks. Branching is limited to weight-sign dispatch and chroma width dispatch.

Optimization tips for a clean-room AVX2 port: use `_mm256_avg_epu8`, `_mm256_maddubs_epi16`, `_mm256_mulhi_epu16`, and `_mm256_packus_epi16` for averaging/weights; use `_mm256_shuffle_epi8` for U/V de-interleave; prefetch with `_mm_prefetch`. SVE tip: predicated loads can eliminate tail handling in plane copies, and SVE's `ld2`/`st2` equivalents can de-interleave chroma at variable width. Public references: H.264 interpolation filter spec; ARM NEON/SVE memory and filter intrinsic docs.

## 9. References

- ITU-T H.264 / ISO/IEC 14496-10, Clause 8.4 (interpolation, weighted prediction), Clause 8.3 (decoding process).
- ARM NEON/SVE instruction reference (`urhadd`, `umull`, `sqrshrun`, `ld2`, `st2`, `rev64`, `prfm`).
- `docs/rebuild/common/aarch64/mc-c.c.md`, `mc-a-common.S.md`, `mc-a-sve.S.md`, `mc.h.md`.

## 10. Verification checklist

- All avg/weighted kernels match a scalar reference for random data and all weight categories.
- `mc_copy_w*` produces byte-identical copies; strides verified with sentinel rows.
- `mc_chroma_neon` matches the H.264 1/8-pixel bilinear interpolation for all dx/dy in 0..7 and widths 2/4/8; verify U/V de-interleave.
- `hpel_filter_neon` matches the 6-tap filter formula exactly, including rounding, for random rows.
- `frame_init_lowres_core_neon` outputs match scalar averaging of three input rows.
- Plane copy/interleave/deinterleave round-trips preserve all bytes.
- Integral init functions match a scalar SATD integral computation.
- MB-tree kernels match their C references within the documented rounding.

## 11. Open questions

- UNKNOWN: exact semantics of `mbtree_propagate_list_internal_neon`'s `bipred_weight` scaling for edge cases where `lists_used` is neither 1 nor 3.
- UNKNOWN: whether the 10-bit section's `pixel_avg_w4_neon` is ever used, given the dispatch in `mc-c.c` only wires 8-bit SVE averages.
- UNKNOWN: alignment guarantees required by the RGB de-interleave function on macOS vs other platforms (there is a platform conditional around a symbol name).
