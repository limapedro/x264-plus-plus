# `common/mips/predict-c.c` — MSA intra prediction

**Dirty-side behavioural specification.** Source path: `E:\_x264_dirty_stash\x264-source\common\mips\predict-c.c`. Role: MSA-128 implementations of H.264 intra prediction for 4x4, 8x8 and 16x16 blocks. Compiled only for 8-bit builds.

## 1. Purpose

Intra prediction generates a candidate block from already-reconstructed neighbouring samples. The MSA implementations replace row-by-row byte loops with vector fills, shuffles and horizontal adds, accelerating the many candidate modes evaluated at slow presets and the fast intra-metric helpers used by `pixel-c.c`.

## 2. Public interface

The exported set is declared in `mips/predict.h.md` and consists of 17 functions:

- 16x16: `dc`, `dc_left`, `dc_top`, `dc_128`, `hor`, `vert`, `plane`.
- 8x8: `dc_4blk`, `hor`, `vert`, `plane` (neighbour-based) and `ddl`, `dc`, `h`, `v` (edge-array-based, taking `uint8_t pu_xyz[36]`).
- 4x4: `dc`, `hor`, `vert`.

Each writes one block at `p_src` with reconstruction pitch `FDEC_STRIDE` (32 bytes). The neighbour-based functions read above at `p_src − FDEC_STRIDE` and left at `p_src − 1`; the edge-based 8x8 variants read only from `pu_xyz`.

## 3. State and data structures

No globals. All temporaries are MSA vectors or 32/64-bit GPRs used to broadcast byte patterns.

- Destination block layout: rows are `FDEC_STRIDE` bytes apart; a 4x4 block occupies four 4-byte words, an 8x8 block eight 8-byte doublewords, a 16x16 block sixteen 16-byte vectors.
- The edge array `pu_xyz[36]` has the standard x264 layout; this code uses:
  - offset `+16`: the eight top-row samples plus the top-left corner at index 16, used by vertical and DC predictions;
  - offset `+14`: the eight left-column samples (indices 14, 15 and then stride-stepped), used by horizontal prediction;
  - offset `+7`: the four upper-left samples for 8x8 DC;
  - for DDL, a 16-byte vector loaded from `pu_xyz + 16` (top row and corner) with the last element replaced by `pu_xyz[15]`.

## 4. Algorithm / behaviour

### Vertical prediction

Read the first `W` bytes above the block (4, 8 or 16 bytes depending on size) and store that same vector to every output row. The 4x4 case reads one 32-bit word and stores it four times with stride; 8x8 reads one 64-bit doubleword and stores eight times; 16x16 reads one 128-bit vector and stores sixteen times (as two groups of eight).

### Horizontal prediction

For each output row `y`, read the single byte to the left at `p_src[y*FDEC_STRIDE − 1]`, broadcast it across a word (4x4), doubleword (8x8) or vector (16x16) by multiplying with `0x01010101` / `0x0101010101010101` or by a fill-byte intrinsic, and store one row. The 16x16 version processes four rows per iteration.

### DC prediction

The four public DC variants share a core helper parameterised by `is_above` and `is_left`:

- **Both neighbours available:** sum the top `N` bytes (horizontal pairwise adds in vector lanes) and the left `N` bytes (scalar loop over `p_src_left[row*FDEC_STRIDE]`), add them, then compute `(sum + N) >> (log2(N)+1)`, i.e. for 4x4 `(sum+4)>>3`, for 16x16 `(sum+16)>>5`. Broadcast the result and fill all rows.
- **Left only:** `(sum_left + N/2) >> log2(N)`, i.e. `(sum+2)>>2` (4x4) or `(sum+8)>>4` (16x16).
- **Above only:** `(sum_top + N/2) >> log2(N)`, computed with a rounded shift-right on the word-lane sum (4x4 uses `srari_w(...,2)`, 16x16 uses `srari_d(...,4)`).
- **Neither:** fill with 128.

The 8x8 edge-array DC helper computes four separate DC values for the four 4x4 sub-blocks: top sums from `edge[16..23]` split into two 4-byte groups; left sums from columns at offsets 14/15 and 7 for the upper/lower halves; then
- upper-left `(sum_t0 + sum_l_upper + 4) >> 3`,
- lower-left `(sum_t1 + sum_l_lower + 4) >> 3`,
- upper-right `(sum_t1 + 2) >> 2`,
- lower-right `(sum_l_lower + 2) >> 2`.

These four bytes are broadcast to words and stored to the four 4x4 quadrants of the 8x8 block.

### Plane prediction (8x8)

This follows H.264 §8.3.1.2.3 with the exact x264 simplifications:

1. Load 16 bytes from `p_src − (stride+1)` (the top row plus the top-left corner) and reorder them with the byte shuffle mask `{3,5,2,6,1,7,0,8,3,5,2,6,1,7,0,8}` into pairs suitable for horizontal subtraction.
2. Compute pairwise horizontal differences; multiply by the 8-weight vector `{1,2,3,4,1,2,3,4}`; horizontally add pairs to words, then add words to a doubleword. This yields `H`.
3. Compute `V` from the left column via a scalar expression: `(p[4s−1]−p[2s−1]) + 2(p[5s−1]−p[s−1]) + 3(p[6s−1]−p[−1]) + 4(p[7s−1]−p[−s−1])`.
4. Scale both by 17 and round: `H' = (H·17 + 16) >> 5`, `V' = (V·17 + 16) >> 5`.
5. Form `A = 16·(p[7s−1] + p[−s+7] + 1)` and `base = A − 3·(H'+V')`.
6. For each pair of output rows, generate four word-lane values `base + k·H'` (k=0..3 for columns 0..3) and their `+4·H'` counterparts, add `V'` accumulated by row into `base`, shift each right by **5** with arithmetic shift, pack to halfwords, clip to `[0,255]`, pack to bytes, and store 8 bytes per row.

### Plane prediction (16x16)

Same algorithm with extended masks and weights:

- Shuffle mask `{7,8,6,9,5,10,4,11,3,12,2,13,1,14,0,15}` applied to two 8-byte loads from `p_src − (stride+1)` and `+9`.
- Horizontal-difference weights `{1,2,3,4,5,6,7,8}`.
- `H'` and `V'` are scaled by **5** and rounded with `(x·5 + 32) >> 6`.
- `A = 16·(p[15s−1] + p[−s+15] + 1)`, `base = A − 7·(H'+V')`.
- Loop over 16 rows; each iteration emits four word-lane candidates `base + k·V'` (k=0..3, accumulated by column multiplier vector `{0,1,2,3}` plus `4·V'` for the higher columns), shifts right by **5**, clips and stores one 16-byte row.

(Note the roles of the slope variables are transposed relative to the 8x8 case because the vector lanes represent columns here.)

### DC 4-block 8x8

Computes the four chroma-style DC values described above from the top row and left column of an 8x8 block, then stores four 4x4 quadrants with two word stores per row.

### DDL 8x8

Loads 16 neighbour bytes from `edge+16`, creates byte-shifted copies (`sldi_b` by 1, 2, 3 bytes), with the last element of the two-byte-shifted copy replaced by `edge[15]` for boundary replication. Widens to halfwords, forms the four-point averages `(2·a + b + c + 2) >> 2` via horizontal byte adds and a rounded right shift by **2**, packs to bytes, and stores four rows; then shifts the byte vector by 4 and stores four more rows.

### 128-DC

Fills the whole 16x16 block with the byte 128 using two groups of eight 16-byte stores.

## 5. Dependencies

- **Calls:** local helpers in `mips/macros.h` (`LW4`, `SW4`, `LD`, `SD4`, `LD_UB`, `ST_UB8`, `INSERT_D2_UB`, `SRA_4V`, `PCKEV_H2_SH`, `CLIP_SH2_0_255`, `PCKEV_ST_SB`) and MSA intrinsics (`fill_b`, `ldi_b`, `splati_b`, `vshf_b`, `hadd_u_h/w/d`, `hsub_u_h`, `srari_w/d/h`, `sldi_b`, `insert_b`, `pckev_b`, `maxi_s_h`, `min_s_h`).
- **Called by:** `x264_predict_*_init()` in `common/predict.c` (by symbol name), `mips/pixel-c.c` intra-metric helpers, and the macroblock analysis code when MSA is selected.
- **Shared config:** `HIGH_BIT_DEPTH`, `FDEC_STRIDE` (32).

## 6. Constants and tables

- Broadcast multipliers: `0x01010101` (32-bit), `0x0101010101010101` (64-bit), fill-byte intrinsics for 128-bit.
- DC rounding constants: `+4 >>3` (4x4 both), `+2 >>2` (4x4 one side), `+16 >>5` (16x16 both), `+8 >>4` (16x16 one side), `+4 >>3` (8x8 four-block DC corners), `+2 >>2` (8x8 single-side DC).
- Plane constants: 17 (8x8 slope scale), 5 (16x16 slope scale), `+16>>5` / `+32>>6` rounding, `A` offset 16·(corner samples + 1), `−3·(H'+V')` (8x8) / `−7·(H'+V')` (16x16), final `>>5`.
- DDL: `+2 >>2` rounding, 1/2/3/4-byte shifts.
- Fill value 128 for unavailable neighbours.

**Why:** these constants are the exact integer approximations mandated by H.264 for plane prediction and the natural round-to-nearest constants for averaging.

## 7. Performance notes

- **Hotness:** moderate to high depending on preset; at `--preset veryslow`, dozens of intra predictions per macroblock.
- **SIMD status:** fully MSA-128; vertical/DC fills are pure vector stores; horizontal uses GPR multiply-broadcast; plane uses shuffles and word-lane arithmetic.
- **Branch behaviour:** only the uniform `is_above`/`is_left` flags in the DC helpers are branches; these are call-site constants, so no per-sample branching.
- **Optimization tips:** (a) the scalar left-column accumulation loops in DC and plane could be replaced with a strided vector gather if the neighbour layout allows; (b) keep the fill vectors in registers across multiple mode evaluations; (c) the plane prediction's per-row word candidates are independent — unrolling both rows per iteration (as currently done for 8x8) improves throughput; (d) ensure the destination is 16-byte aligned to use aligned vector stores.
- **References:** ITU-T H.264 §8.3.1 (intra prediction); x264 `common/predict.c`; MSA programmer's guide for `vshf_b`, `hadd_u_h`, `srari`.

## 8. Verification checklist

1. For each block size and mode, generate deterministic neighbours (e.g. ramp values 0..255) and compare MSA output byte-for-byte with scalar reference.
2. DC availability matrix: test all four combinations of above/left available for 4x4 and 16x16; verify rounding at half-way cases.
3. Plane 8x8/16x16: test with a linear ramp (should produce near-constant output) and with extreme values 0/255 to check clamping.
4. Verify edge-array functions only read `edge[0..35]` (ASan with a guarded 36-byte buffer).
5. Check that no write crosses into the next macroblock row: the stride is `FDEC_STRIDE`, not the logical block width.
6. Intra metric wrappers in `pixel-c.c`: verify the three candidate predictions use the expected modes in the expected output slots.

## 9. Open questions

- UNKNOWN: exact mapping of the 8x8 edge-array offsets (14, 16, 7) to the generic `x264_predict_8x8_filter` output layout; validate against a known edge pattern.
- UNKNOWN: whether the 8x8 plane helper's mask `{3,5,2,6,1,7,0,8,...}` intentionally duplicates indices (it does), and whether the duplicate elements affect the H sum for certain inputs — verify numerically against scalar.
- UNKNOWN: saturation of the plane predictor's word-lane arithmetic for pathological neighbour values (0 and 255) beyond the clip step; no overflow is expected but not formally bounded here.
