# `common/x86/predict-a.asm` — x86 intra prediction SIMD

**Dirty-side behavioural specification.** Source path:
`common/x86/predict-a.asm`. Role: SIMD implementations of all H.264 intra
prediction modes, for luma 16x16/8x8/4x4 and chroma 8x16c/8x8c, plus the 8x8
edge-filter helper and the `p_core` plane-linear predictors used by the C layer.

---

## 1. Header

Assembly source using `x86inc.asm`/`x86util.asm`. Declares a `.rodata` section
with shuffle/sign masks and a `.text` section with the kernels. Built for both
8-bit and high-bit-depth x86-32/x86-64; the `HIGH_BIT_DEPTH` conditional selects
word vs byte data paths. Exported symbols are declared in `predict.h.md`.

## 2. Purpose

Intra prediction writes a predicted block from already-decoded edge samples.
It runs for every intra macroblock and every intra 4x4/8x8 sub-block in analysis,
so SIMD is essential. The file provides direct SIMD forms for the simple modes
(vertical, horizontal, DC) and filtered forms for the directional modes
(DDL/DDR/VL/VR/HD/HU, plus plane-P via `*_p_core`).

## 3. Public interface

### 3.1 4x4 luma modes — `(pixel *src)`
`predict_4x4_{dc,ddl,ddr,vl,vr,hd,hu}_<isa>`; `predict_4x4_h_avx2`
(high-bit-depth). ISAs: mmx2, ssse3, sse2, avx, with `vr_cache64_ssse3` for
64-byte-cache CPUs. The 8-bit `hu` is only `predict_4x4_hu_mmx2`.
`src` points at the top-left of the 4x4 block; the above row is at
`src[-FDEC_STRIDE]` and the left column at `src[-1 + y*FDEC_STRIDE]`.

### 3.2 8x8 luma modes — `(pixel *src, pixel edge[36])`
`predict_8x8_{v,h,dc,dc_top,dc_left,ddl,ddr,vl,vr,hd,hu}_<isa>` plus
`predict_8x8_filter_<isa>(src, edge, i_neighbor, i_filters)`.
ISAs: mmx2, sse, sse2, ssse3 (incl. `cache64` variants), avx.
`edge[]` is a 36-element precomputed buffer filled by `predict_8x8_filter`,
containing left/top samples in a SIMD-friendly order (offsets: 0..7 left,
8..15 unknown, 16..23 top, 24..31 top2, 32 corner).

### 3.3 Chroma plane modes — `(pixel *src)` or `(uint8_t*/uint16_t* src,
int i00, int b, int c)`
* `predict_8x8c_{v,h,dc,dc_top}_<isa>`, `predict_8x16c_{v,h,dc,dc_top}_<isa>`
  — mmx/mmx2/sse/sse2/ssse3/avx/avx2 (exact set per bit depth).
* `predict_8x8c_p_core_<isa>`, `predict_8x16c_p_core_<isa>`,
  `predict_16x16_p_core_<isa>` — `(src, i00, b, c)`, called by the C layer's
  plane prediction setup in `predict-c.c`. ISAs mmx2 (32-bit only), sse2, avx,
  avx2.

### 3.4 16x16 luma modes — `(pixel *src)`
`predict_16x16_{v,h,dc,dc_top,dc_left}_<isa>`; `predict_16x16_v` mmx2/sse/avx,
`_h` mmx2/sse2/ssse3/avx2, `_dc*` sse2/avx2.

## 4. State and data structures

* `.rodata` masks: `pw_43210123` (`[-3,-2,-1,0,1,2,3,4]` twice), `pw_m3`,
  `pw_m7`, `pb_00s_ff`, `pb_0s_ff`, `shuf_fixtr`, `shuf_nop`, `shuf_hu`,
  `shuf_vr`, `pw_reverse`.
* `cextern` masks: `pb_0`, `pb_1`, `pb_3`, `pw_1`, `pw_2`, `pw_4`, `pw_8`,
  `pw_16`, `pw_00ff`, `pw_pixel_max`, `pw_0to15`.
* `STORE8`/`STORE16` macros write a replicated vector to all rows of an 8x8
  or 16x16 block, incrementing `r0` by `FDEC_STRIDEB` (byte-addressable stride
  for high bit depth).
* `PRED_H_LOAD`/`PRED_H_STORE`/`PRED_H_4ROWS` implement horizontal prediction:
  load the left pixel of a row, replicate it (`SPLATB_LOAD`/`SPLATW`/
  `vpbroadcastpix`), and store it across the row's width.

## 5. Algorithm / behaviour

### 5.1 Lowpass filter (`PRED8x8_LOWPASS`)
Computes `out = (left + 2*center + right + 2) >> 2` (the H.264 3-tap
`(1,2,1)` filter with rounding).
* High-bit-depth: `paddw left,right` then `psrlw 1` (unsigned average of the
  two neighbours) and `pavgw center, that` — `pavgw` adds the +1 rounding.
* 8-bit: `pavgb left,right` then correct the discarded low bit using
  `(left^right)&1` subtracted via `psubusb`, then `pavgb center, result`.
  This reproduces the exact rounding of the word formula in byte arithmetic.

### 5.2 4x4 DDL (`predict_4x4_ddl`)
1. Load the row above (`src[-FDEC_STRIDEB]`) into `m1`; form a left-shifted
   version `m2` and a right-shifted version `m1` (with sign/rounding correction
   for 8-bit via XOR/PSRL/pxor).
2. `PRED8x8_LOWPASS` produces the four filtered top samples.
3. Shift `m0` right by one pixel four times, storing 4 bytes/words per row —
   this implements `pred[y][x] = filtered_top[x+y]`.

### 5.3 4x4 VR (`predict_4x4_vr`, SSSE3 8-bit special)
Uses `palignr` at shift 7 to assemble a sliding window that contains
`[t3,t2,t1,t0,lt,l0,l1,l2]` from the top row and left column, then
`pavgb`/`PRED8x8_LOWPASS` and `pshufw`/`palignr` to produce the four rows.
The high-bit-depth/general `PREDICT_4x4` macro assembles the same window with
`pinsrw` and `punpck` and uses `PSRLPIX` shifts to walk down the rows.

### 5.4 4x4 DDR / HD / VL / HU
Each follows the same pattern: assemble the relevant edge samples into one
register (via `movd`/`punpck` on MMX or `pinsrw`/`palignr` on SSE), apply
`PRED8x8_LOWPASS` for the filtered positions, then generate the four output rows
by repeated `PSRLPIX`/`PSLLPIX` shifts and `PALIGNR`. The HD mode additionally
computes `pavg` of two neighbours for the diagonal half; HU uses a reversed
edge (`pshufw q3333` etc.).

### 5.5 4x4 DC
* High-bit-depth: sum the four left samples (dwords packed to words via `psrlq`)
  and the four top samples (via `HADDW`), add +4, shift right 3, `SPLATW`,
  store four rows.
* 8-bit: sum above with `psadbw`, sum left with scalar `movzx`/`add`s, add 4,
  shift 3, multiply by `0x01010101` to replicate a byte four times, store four
  dwords.

### 5.6 8x8 edge filter (`predict_8x8_filter`)
1. If `i_neighbor & 1` (left neighbours available), gather eight left-column
   samples and apply the lowpass filter to produce `edge[8..15]`, plus a
   scalar `(3*l7 + l6 + 2)>>2` for `edge[6..7]`.
2. If `i_neighbor & 2` (top available), gather eight top-row samples (and the
   top-right corner when `i_neighbor & 8`), apply the lowpass filter to produce
   `edge[16..23]`, optionally `edge[24..31]`, and the corner `edge[32]`.
3. The SSSE3 8-bit path uses `pshufb` with `shuf_fixtr`/`shuf_nop` to handle the
   top-right-not-available case by replicating the top-left sample.
4. `i_filters` bitmask selects which filtered variants to compute, allowing the
   caller to skip unnecessary work.

### 5.7 8x8 V / H / DC modes
* V: load `edge[16]` (eight top samples) and `STORE8` it to all eight rows.
* H: load `edge[7]` (eight left samples), unpack to words/dwords, use
  `SPLAT*` to replicate each left sample into a row, store eight rows.
* DC: sum `edge[14]` (left) and `edge[32]` (top) with `psadbw`/`HADDW`, add 8,
  shift 4, `packuswb`/`SPLATW`, `STORE8`.
* DC_TOP / DC_LEFT: sum only top or only left, add 4, shift 3.

### 5.8 8x8 DDL / DDR / VL / VR / HD / HU
* DDL: load `edge[16]` and `edge[24]` (top, top-filtered); apply lowpass to
  produce a 16-sample filtered sequence; store eight rows by repeated
  `PALIGNR` at 7 pixels (or `psrldq` for bytes).
* DDR: symmetric using `edge[8]`/`edge[16]` (left filter) and reversed walking.
* VL/HD/HU: combine the top-filtered and left-filtered sequences with
  `punpck`/`pshufb` to build the two interleaved prediction rows at a time,
  exploiting the H.264 alternate-pixel filtering structure.
* AVX/SSSE3 variants use `pshufb` masks `shuf_hu`, `shuf_vr`, `pw_reverse`
  to perform these combinations in one instruction instead of several shifts.

### 5.9 Plane-P cores (`predict_*_p_core`)
Given precomputed `i00` (top-left corner extrapolation), `b` (horizontal
gradient), `c` (vertical gradient), generate `pred[x,y] =
clip((i00 + b*x + c*y + 16) >> 5, 0, pixel_max)`.
* 8-bit: precompute the eight/four x-values `i00 + b*x - 3*c` into a register
  (`pmullw` with `pw_0to15`), then add `c` per row; `psraw 5`, `packuswb`, and
  store one/two rows per iteration. AVX2 handles 16 columns via
  `vbroadcasti128` and `pmullw` with `pw_0to15`.
* High-bit-depth: same but with `CLIPW` against `[pb_0]`/`[pw_pixel_max]`
  instead of `packuswb` saturation, and `psraw 5`.
* MMX 32-bit forms (guarded by `ARCH_X86_64==0 && HIGH_BIT_DEPTH==0`) use
  four word registers for the 16 columns, one per 4-pixel group.

### 5.10 Chroma DC modes
`predict_8x8c_dc`/`8x16c_dc` compute four/eight DC values from the top row
(`psadbw` into words) and four/eight left samples (`LOAD_LEFT` scalar adds),
combine them with `(s0+s1+s2+s3+... )` and `pavgw`, `packuswb`, and replicate
each DC value to its 4x4 quadrant with `pshufw`/`punpck`.
`DC_TOP` variants sum only the top row (two halves for 8x16c), shift, average
with zero, and `STORE8`/`STORE16`.

### 5.11 16x16 V/H/DC
* V: load 16/32 bytes of the row above with as many vectors as needed
  (`16*SIZEOF_PIXEL/mmsize` iterations), `STORE16`.
* H: `PRED_H_4ROWS 16, 1` called four times (or looped) to broadcast each left
  pixel across its row.
* DC/DC_TOP/DC_LEFT: `predict_16x16_dc_left_internal` sums 16 left samples into
  two dwords; the DC variant adds `+16`, top sums with `psadbw` and `+8`,
  then shifts by 5/4 respectively, `SPLATW`/`packuswb`/`STORE16`.

### 5.12 Explicit prediction equations and decision conditions

For reproducibility, the per-mode sample equations are stated here in terms of
the edge samples. Let `T[x]` (x=0..2N-1) be the row above, `L[y]` (y=0..2N-1)
the column to the left, and `lt = T[-1] = L[-1]` the top-left corner. The
filtered half-sample values are:
```
G[x] = (T[x] + T[x+1] + 1) >> 1
F[x] = (T[x] + 2*T[x+1] + T[x+2] + 2) >> 2
```
and analogously `H[y]`, `J[y]` from the left column. `PRED8x8_LOWPASS` implements
exactly the `F`/`J` expression; the `pavgb` correction `(a^b)&1` exists because
`pavgb` discards the low bit of the sum of two operands.

Mode equations (for an `N×N` block, output `p[y][x]`):
* **Vertical:** `p[y][x] = T[x]`.
* **Horizontal:** `p[y][x] = L[y]`.
* **DC (available samples):** `p[y][x] = (sum(T[0..N-1]) + sum(L[0..N-1]) + N) >> (log2(N)+1)`.
  For 4x4 the bias is +4 and shift 3; for 8x8 +8 and shift 4; for 16x16 +16
  and shift 5. DC_TOP/DC_LEFT omit the missing half and use a correspondingly
  smaller bias/shift.
* **DDL (diagonal down-left):** `p[y][x] = F[x+y+1]` using the top half-sample
  array with `T` indices beyond `2N` replicating `T[2N-1]`.
* **DDR:** `p[y][x] = J[x+y+1]` from the left half-sample array.
* **VL (vertical-left):** for even `x`, `p[y][x] = (T[2x+y] + T[2x+y+1] + 1)>>1`;
  for odd `x`, `p[y][x] = (T[2x+y+1] + 2*T[2x+y+2] + T[2x+y+3] + 2)>>2`.
* **VR:** symmetric to VL using the left half-sample array with the roles of
  `x` and `y` exchanged and the corner replicated.
* **HD (horizontal-down):** even/odd rows alternate between `G`-filtered and
  `F`-filtered left-column predictions with the top corner replication.
* **HU (horizontal-up):** the same with the left column reversed.
* **Plane P:** `p[y][x] = clip((i00 + b*x + c*y + 16) >> 5, 0, MAX)`.

**Decision conditions** in the assembly:
* `predict_8x8_filter` uses `i_neighbor & 1` (left samples available),
  `& 2` (top available), `& 4` (top-right available via `shuf_nop`), and
  `& 8` (needs corner fixup). `i_filters` further selects which filtered
  outputs are written. The SSSE3 path handles the `&8` case by substituting
  `shuf_fixtr` for `shuf_nop` so the top-left corner is replicated into
  unavailable top-right positions.
* The 4x4 DC kernel's bias is `+4` (not `+8`) because only eight samples are
  summed; the 8x8c/8x16c DC kernels use four separate quadrant averages with
  `+2` bias each (`(s+2)>>2`) and then `pavgw` with zero.
* High-bit-depth paths must clamp with `CLIPW` against `[pb_0]`/`[pw_pixel_max]`
  after the arithmetic shift; 8-bit paths rely on `packuswb` saturation.
* The `PRED_H_STORE` macro uses an 8-byte store when `width*SIZEOF_PIXEL==8`
  and otherwise issues `mmsize`-sized stores; using a wider store than the
  block width would overwrite adjacent plane data.

### 5.13 SIMD construction of directional predictions

The directional modes are generated by building a single sliding register that
contains the needed edge sequence and then shifting it once per output row.
For DDL the register holds `[F0,F1,...,F15]`; iteration `y` stores elements
`[y..y+N-1]` by `PSRLPIX m0,1` between stores. For DDR the register is built
from the left filter and walked in the opposite direction. VL/HD/HU interleave
two such sequences (`G` and `F`) with `punpck` into even/odd output positions,
then use `PALIGNR` at 2/4/6 pixel offsets to produce successive rows without
reloading edges. AVX/SSSE3 variants replace multi-instruction shift chains with
one `pshufb` using `shuf_hu`, `shuf_vr`, or `pw_reverse`.

## 6. Dependencies

* Includes `x86inc.asm`, `x86util.asm`; uses `SPLATB_LOAD`, `SPLATW`,
  `vpbroadcastpix`, `PALIGNR`, `PSLLPIX`/`PSRLPIX`, `HADDW`, `CLIPW`,
  `STORE8`/`STORE16`, `INIT_*`, `cglobal`.
* Imports C masks via `cextern`.
* Called from `predict-c.c`'s dispatch functions (`predict_16x16_init_mmx`,
  etc.) which fill the `x264_predict_t pf[]` arrays.
* `predict_*_p_core` is called from `predict-c.c`'s static `predict_*_p_*`
  wrappers.

## 7. Constants and tables

* `pw_43210123`: weights `[-3,-2,-1,0,1,2,3,4]` for plane-P x-values.
* `pw_m3`, `pw_m7`: constants for 8x8c and 8x16c plane-P gradient scaling.
* `shuf_fixtr`/`shuf_nop`: top-right corner fixup for 8x8 filter SSSE3 path.
* `shuf_hu`, `shuf_vr`, `pw_reverse`: byte-reversal masks for HU/VR modes.
* `pb_00s_ff`, `pb_0s_ff`: zero/saturation masks for the lowpass filter.
* `pw_0to15`: `[0..15]` multiplier for the plane-P x-coordinate.
* `pw_pixel_max`: clamp ceiling for high-bit-depth plane-P.

## 8. Performance notes

* **Wider vectors:** AVX2 plane-P and H modes broadcast and compute 16/32
  columns at once via `vpbroadcastpix`/`vpbroadcastw`; high-bit-depth 16x16 V
  uses YMM (`INIT_YMM avx`). Prefer the widest path for large blocks.
* **Transpose strategy:** The horizontal modes avoid transposes by broadcasting
  each left pixel into a register and storing whole rows; directional modes
  use `PALIGNR`/`pshufb` to slide a pre-built filter window instead of
  re-loading edges per row.
* **Avoiding spills:** The 8x8 filter and directional modes assemble the needed
  edge samples into two registers once, then derive all rows by shifts —
  no per-row memory reads of edges.
* **Preprocessing constants:** The C layer precomputes `i00`, `b`, `c` for
  plane-P; the assembly only applies them. Edge filtering is similarly
  precomputed once per macroblock into `edge[36]` and reused by all 8x8 modes.
* **Cache variants:** `*_cache64_*` suffixes exist for CPUs with 64-byte cache
  lines; dispatch distinguishes them.
* **Branch behaviour:** `predict_8x8_filter` has three `test`/`je` branches on
  `i_neighbor`/`i_filters`; these are predictable because neighbour topology is
  fixed per macroblock position.
* **Store macros:** `STORE8`/`STORE16` use multiple `mova` stores to all rows;
  on modern CPUs the store buffer handles this well. `PRED_H_STORE` uses `movq`
  for 8-byte rows to avoid overstore.

## 9. References

* ITU-T H.264 intra prediction specification (all 9 luma 4x4/8x8 modes, 4
  chroma modes, 16x16 modes).
* Public x264 source history for the lowpass filter rounding correction.
* Intel Intrinsics Guide for `pshufb`, `palignr`, `vpbroadcast*`.

## 10. Verification checklist

* For every mode and ISA variant, compare predicted samples bit-exactly against
  the portable C intra prediction for random edge samples.
* Test edge cases: top/left unavailable (edge replication), top-right
  unavailable (`i_neighbor` bit 3 clear), and corner-only cases.
* `predict_8x8_filter`: verify `edge[36]` contents against the C filter for all
  16 combinations of `i_neighbor` bits and several `i_filters` masks.
* `p_core`: verify the clip to `[0,pixel_max]` with gradients that overflow
  and with negative `i00`.
* Verify 8-bit vs high-bit-depth paths produce the same clipped results after
  accounting for the different pixel range.
* AVX2/AVX paths: confirm no overstore beyond the 8x8/16x16 block rows.

## 11. Open questions

* `UNKNOWN`: exact layout of `edge[36]` beyond the offsets inferred from the
  assembly (`0,7,8,14,16,24,32`); consult `common/predict.c`.
* `UNKNOWN`: whether `predict_4x4_h_avx2` is reachable in 8-bit builds (it is
  declared under high-bit-depth in `predict.h`).
* `UNKNOWN`: the reason for the separate `predict_4x4_vr_cache64_ssse3` alias;
  exact cache-line dispatch conditions are in the CPU detection code.
