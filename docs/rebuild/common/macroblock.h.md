# macroblock.h — dirty-side behavioural specification

Source path: `common/macroblock.h`
Role: defines the internal macroblock type/partition vocabularies, neighbour-availability masks, scan-order and block-index tables, chroma-QP mapping, and the small inline helpers used to access neighbouring macroblock data.

> Dirty-side behavioural specification. Describes observable behaviour and structure only; no source text is reproduced.

## 1. Header / identity

This header is the shared vocabulary of the encoder's macroblock layer. Every module that reads or writes per-macroblock state — analysis, prediction, CABAC/CAVLC writing, motion compensation, deblocking — agrees on the enumerations, table layouts and neighbour conventions defined here. It also declares the allocation/load/save functions implemented in `macroblock.c`.

## 2. Purpose

H.264 macroblocks have many possible types (intra 4×4/8×8/16×16/PCM; P inter with several partition shapes; B inter with nine list-combination variants) and sub-partitions. The header provides:

* a compact internal type enum distinct from the bitstream's `mb_type` field;
* bit masks describing which of the four spatial neighbours are available;
* per-prediction-mode neighbour requirement masks;
* scan-order tables mapping 4×4/8×8 block indices to positions in the internal "scan8" cache layout;
* conversion helpers for packing pixel components into machine words;
* inline functions that derive an intra 4×4 prediction mode, a non-zero coefficient count, and 8×8-transform permission from neighbour cache entries.

## 3. Public interface

### Enumerations and macros

* `macroblock_position_e`: `MB_LEFT = 0x01`, `MB_TOP = 0x02`, `MB_TOPRIGHT = 0x04`, `MB_TOPLEFT = 0x08`, `MB_PRIVATE = 0x10`, `ALL_NEIGHBORS = 0x0f`. These are bit flags, not ordinal values.
* `mb_class_e`: internal macroblock types in order: `I_4x4=0`, `I_8x8=1`, `I_16x16=2`, `I_PCM=3`, `P_L0=4`, `P_8x8=5`, `P_SKIP=6`, `B_DIRECT=7`, `B_L0_L0=8`, `B_L0_L1=9`, `B_L0_BI=10`, `B_L1_L0=11`, `B_L1_L1=12`, `B_L1_BI=13`, `B_BI_L0=14`, `B_BI_L1=15`, `B_BI_BI=16`, `B_8x8=17`, `B_SKIP=18`, `X264_MBTYPE_MAX=19`.
* `IS_INTRA(type)` — true for `I_4x4`, `I_8x8`, `I_16x16`, `I_PCM`.
* `IS_SKIP(type)` — true for `P_SKIP`, `B_SKIP`.
* `IS_DIRECT(type)` — true for `B_DIRECT`.
* `mb_partition_e`: sub-partition types `D_L0_4x4=0`, `D_L0_8x4=1`, `D_L0_4x8=2`, `D_L0_8x8=3`, then the matching L1 (4–7) and BI (8–11) variants, `D_DIRECT_8x8=12`, and the full-block partitions `D_8x8=13`, `D_16x8=14`, `D_8x16=15`, `D_16x16=16`.
* `IS_SUB4x4 / IS_SUB4x8 / IS_SUB8x4 / IS_SUB8x8` — predicate macros keyed to those partition values.
* `cabac_ctx_block_cat_e`: coefficient context categories `DCT_LUMA_DC=0`, `DCT_LUMA_AC=1`, `DCT_LUMA_4x4=2`, `DCT_CHROMA_DC=3`, `DCT_CHROMA_AC=4`, `DCT_LUMA_8x8=5`, plus chroma U/V DC/AC/4x4/8x8 variants 6–13.

### Tables

| Table | Shape | Semantics |
|---|---|---|
| `x264_pred_i4x4_neighbors[12]` | 12 bytes | For each intra 4×4 prediction mode, the set of neighbouring blocks required. |
| `x264_mb_type_fix[19]` | 19 bytes | Maps internal type to a "canonical" internal type for saving; notably maps `I_8x8` to `I_4x4`. |
| `x264_mb_type_list_table[19][2][2]` | per-type 2×2 booleans | For each macroblock type, which reference lists are used by each of the two 8×8-row groups. |
| `x264_mb_partition_listX_table[2][17]` | 2×17 bytes | For each sub/partition type, whether list 0 and/or list 1 is used. |
| `x264_mb_partition_count_table[17]` | 17 bytes | Number of motion vectors for each partition type (4, 2, 1). |
| `x264_mb_partition_pixel_table[17]` | 17 bytes | Block-size class (`PIXEL_4x4`, `PIXEL_8x4`, …, `PIXEL_16x16`). |
| `x264_zigzag_scan4[2][16]` | frame/field × 16 | Transposed zigzag order for 4×4 blocks. |
| `x264_zigzag_scan8[2][64]` | frame/field × 64 | Transposed zigzag order for 8×8 blocks. |
| `block_idx_x/y[16]`, `block_idx_xy[4][4]`, `block_idx_xy_1d`, `block_idx_yx_1d` | 16 entries each | Conversions between raster 4×4 block index and x/y coordinates / transposed order. |
| `block_idx_xy_fenc[16]`, `block_idx_xy_fdec[16]` | 16 entries each | Precomputed byte offsets of each 4×4 block within the local fenc/fdec buffers (stride 16 and 32 respectively). |
| `i_chroma_qp_table[]` | `QP_MAX+1+12*2` bytes | Maps luma QP with bit-depth offset to chroma QP. |
| `ctx_cat_plane[6][3]` | 6×3 bytes | Maps plane/category index to CABAC coefficient context category for luma/Cb/Cr. |
| `x264_transform_allowed[19]` | 19 bytes | Whether 8×8 transform is permitted for each macroblock type. |

### Functions declared (implemented in `macroblock.c`)

`x264_macroblock_cache_allocate/free`, `x264_macroblock_thread_allocate/free`, `x264_macroblock_slice_init`, `x264_macroblock_thread_init`, `x264_macroblock_cache_load_progressive`, `x264_macroblock_cache_load_interlaced`, `x264_macroblock_deblock_strength`, `x264_macroblock_cache_save`, `x264_macroblock_bipred_init`, `x264_prefetch_fenc`, `x264_copy_column8`, `x264_mb_predict_mv_16x16`, `x264_mb_predict_mv_pskip`, `x264_mb_predict_mv`, `x264_mb_predict_mv_direct16x16`, `x264_mb_predict_mv_ref16x16`, `x264_mb_mc`, `x264_mb_mc_8x8`. Their contracts are described in `macroblock.c.md`.

### Inline helpers

* `pack16to32(a,b)`, `pack8to16(a,b)`, `pack8to32(a,b,c,d)`, `pack16to32_mask(a,b)`, `pack32to64(a,b)` — combine two 16-bit, two 8-bit, four 8-bit, two masked 16-bit, or two 32-bit values into one machine word in memory order. On big-endian targets the argument order is reversed to preserve byte order. `pack_pixel_1to2` / `pack_pixel_2to4` alias the 8/16-bit or 16/32-bit variants depending on `HIGH_BIT_DEPTH`.
* `x264_mb_predict_intra4x4_mode(h, idx)` — derive the most likely intra 4×4 prediction mode for block `idx` from the left and above cache entries.
* `x264_mb_predict_non_zero_code(h, idx)` — derive the expected non-zero coefficient count for CAVLC from left and above neighbours.
* `x264_mb_transform_8x8_allowed(h)` — decide whether `transform_size_8x8_flag` may be used for the current macroblock.

## 4. State and data structures

The header assumes the existence of `h->mb.cache` arrays indexed by `x264_scan8[...]` positions (defined in `base.h`). The scan8 layout is a 15-row × 8-column scratch grid: luma blocks occupy rows 1–4 and 6–9 and columns 4–7; chroma U rows 6–9 columns 4–7 offset by 16; chroma V rows 11–14 offset by 32. `X264_SCAN8_0 = 4 + 1*8` is the position of the top-left luma 4×4 block. Positions `-1`, `-8`, `-8+width`, `-9` etc. therefore designate the left, top, top-right and top-left neighbours of a block. The first row's column 0 (`0+0*8`), column 0 row 5 (`0+5*8`) and column 0 row 10 (`0+10*8`) are reserved for luma/chroma DC blocks. This is why neighbour access is a simple index subtraction rather than a bounds check: the cache is padded so that unavailable neighbours land in sentinel slots filled with defined "unavailable" values by `macroblock.c`.

Ownership: all tables are static read-only; the cache arrays they index are owned by the encoder thread. The enums are pure compile-time facts.

## 5. Algorithm / behaviour

### 5.1 Intra 4×4 mode prediction

Given block index `idx`, read:

* `ma = cache.intra4x4_pred_mode[scan8[idx] - 1]` (left neighbour);
* `mb = cache.intra4x4_pred_mode[scan8[idx] - 8]` (above neighbour).

Map each through the mode-fixing table (defined in `predict.h`): indices 0–8 map to themselves; the DC variants 9, 10 and 11 all map to plain DC (2); index `-1` (unavailable) maps to `-1`. Then take `m = min(fix(ma), fix(mb))`. If `m < 0` (i.e. at least one neighbour is unavailable and therefore mapped to `-1`), return `I_PRED_4x4_DC` (2). Otherwise return `m`. This implements the H.264 rule "if either neighbour is unavailable, use DC; otherwise use the smaller of the two modes". Importantly, a DC variant stored in the cache is already normalised to DC before comparison.

### 5.2 Non-zero count prediction (CAVLC)

Read the same two neighbours from `cache.non_zero_count`: `za` (left) and `zb` (above). Unavailable neighbours are pre-filled with the sentinel `0x80` by the cache loader. Compute `sum = za + zb`. If `sum < 0x80`, return `(sum + 1) >> 1` (rounded-up average); otherwise return `sum & 0x7f`. Rationale: when both neighbours are available (`<= 0x7f`), their sum is at most 254 and the average is the standard `nA`/`nB` prediction; when either sentinel is present, the sum is at least `0x80`, and masking with `0x7f` recovers the other neighbour's value exactly (because `0x80 + v` masked to 7 bits gives `v`; two sentinels give `0`). The `sum < 0x80` test is therefore a branchless availability check.

### 5.3 8×8 transform permission

1. If the PPS flag `b_transform_8x8_mode` is off, return 0 immediately.
2. If the macroblock type is not `P_8x8`, return `x264_transform_allowed[type]`. The table value is 0 for all intra types and `SKIP`, 1 for `P_L0`, `B_DIRECT` and all B inter types, and 2 for `P_8x8` (handled by the next step).
3. For `P_8x8`, inspect the four sub-partition indices. Permission is granted only if all four equal `D_L0_8x8`; the implementation tests this with one 32-bit load compared against the repeated byte pattern. This is the documented case where an 8×8 transform is allowed for an 8×8-partitioned macroblock.

The helper deliberately does **not** check CBP: a macroblock with no residual coefficients can still report the flag as allowed; callers combine it with CBP later.

### 5.4 Chroma QP table construction

The table is built with a macro `QP(q) = q + QP_BD_OFFSET` and starts with twelve zero entries. For bit depths above 8, six additional zero entries precede the negative-QP entries; the remainder lists luma QP values 0…51, with the standard chroma-QP plateau pattern (two QPs sharing a chroma QP at low values, four sharing one at the top). For 8-bit builds the table has `QP_MAX+1+12` entries; each additional bit depth adds 12. The mapping implements the H.264 chroma QP derivation `chroma_qp = clip3(0,51, table[luma_qp + QP_BD_OFFSET])`.

### 5.5 Zigzag and block-index tables

The scan tables are explicitly "transposed with respect to the tables in the standard": entry `k` gives the raster index of the `k`-th coefficient in zigzag order after transposition. Two variants exist for frame and field scanning; field order differs substantially (e.g. 4×4 field scan starts `0,1,4,2,3,5,...` versus frame `0,4,1,2,5,8,...`). `block_idx_xy_1d` and `block_idx_yx_1d` are the two 1-D transpositions of the 4×4 block grid; `block_idx_xy_fenc/fdec` precompute the stride-multiplied byte offsets used by motion compensation and reconstruction.

### 5.6 Neighbour masks

`x264_pred_i4x4_neighbors` uses the flag bits to state, per intra prediction mode, whether the left, top, top-right and top-left samples are required:

| Mode (index) | Required neighbours |
|---|---|
| V (0) | top |
| H (1) | left |
| DC (2) | left + top |
| DDL (3) | top + top-right |
| DDR (4) | left + top-left + top |
| VR (5) | left + top-left + top |
| HD (6) | left + top-left + top |
| VL (7) | top + top-right |
| HU (8) | left |
| DC_LEFT (9) | left |
| DC_TOP (10) | top |
| DC_128 (11) | none |

`ALL_NEIGHBORS = 0x0f` is the union of the four spatial neighbours. `MB_PRIVATE = 0x10` is an extra bit used internally and excluded from that union.

## 6. Dependencies

* Included by `common.h` and nearly every encoder module.
* Depends on `predict.h` for the mode-fixing tables, `base.h` for `x264_scan8` and `X264_SCAN8_0`, `set.h` for `PIXEL_*` constants, and `common.h` for PPS/SPS structures.
* The declared allocation/load/save functions are implemented in `macroblock.c`; motion prediction functions in `mvpred.c`.
* `ctx_cat_plane` is used by CABAC coefficient coding to select context categories.

## 7. Constants and tables

| Item | Value / derivation | Why |
|---|---|---|
| Neighbour flags | 0x01 left, 0x02 top, 0x04 top-right, 0x08 top-left | Bitwise composition allows one mask per prediction mode. |
| `ctx_count` in CABAC | 460 / 1024 | Matches the context categories enumerated here. |
| `X264_SCAN8_0` | `4 + 1*8` | Top-left luma block slot; neighbours at -1/-8/-9. |
| Sentinel unavailable intra mode | `-1` | Forces DC prediction via the `< 0` test. |
| Sentinel unavailable NNZ | `0x80` | Enables branchless averaging/masking. |
| `transform_allowed` values | 0 intra/skip, 1 P_L0 and B types, 2 P_8x8 | Distinguishes unconditional, conditional and forbidden. |
| Zigzag variants | 2 (frame/field) | Field scans follow interlaced coefficient ordering. |

## 8. Performance notes

Hotness: the inline helpers are called for every intra block and every CAVLC block; the pack helpers are used in tight pixel-copy loops.

Concrete optimisation opportunities:

1. **Branch elimination in `x264_mb_predict_non_zero_code`.** The availability check is already expressed as an arithmetic comparison instead of a branch; this pattern should be preserved in any reimplementation, since the CAVLC path calls it once per block and mispredictions are costly. Public reference: Intel Optimization Reference Manual, "Branch Prediction Optimization" (https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html).
2. **SIMD byte-stream scattering / context-state layout.** The `ctx_cat_plane` mapping and the 460-byte CABAC context array are designed so that a SIMD coefficient-coding routine can gather several context bytes with one aligned load, compute category offsets with `pshufb`, and scatter the adapted states back with one store. Public reference: Intel Intrinsics Guide, `_mm_shuffle_epi8` (https://www.intel.com/content/www/us/en/docs/intrinsics-guide/).
3. **Batched neighbour reads.** Because the scan8 layout places the four left neighbours and four top neighbours contiguously, a single 32-byte load can fetch all of them; the helpers can then extract fields with shifts rather than eight separate loads. This is the same trick used by the cache loader in `macroblock.c`.
4. **Precomputed offsets.** `block_idx_xy_fenc/fdec` eliminate address arithmetic from motion compensation; keeping the entire 16-entry table in registers (or as immediate constants) avoids L1 lookups.
5. **Table compression for cache.** The two 64-entry zigzag tables plus partition tables occupy only a few cache lines; they should be kept in read-only data adjacent to each other to avoid cache-line straddling.
6. **Endianness-safe packing.** The pack helpers compile to single `shl`/`or` sequences on little-endian and to byte swaps on big-endian; using them consistently avoids per-byte stores. Public reference: ARM NEON Intrinsics Reference for `vsetq_lane_u8` / `vcombine_u8` (https://developer.arm.com/architectures/instruction-sets/intrinsics/).

## 9. References

* ITU-T Recommendation H.264 (ISO/IEC 14496-10), §7.3.2 — macroblock syntax and partition types; §8.3.1 — intra prediction; §9.2.2 — CAVLC `nA`/`nB` prediction; Annex G — CABAC context categories. https://www.itu.int/rec/T-REC-H.264
* Intel Intrinsics Guide. https://www.intel.com/content/www/us/en/docs/intrinsics-guide/
* Intel® 64 and IA-32 Architectures Optimization Reference Manual. https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html
* ARM NEON Intrinsics Reference. https://developer.arm.com/architectures/instruction-sets/intrinsics/

## 10. Verification checklist

1. **Enumeration coverage:** assert `X264_MBTYPE_MAX == 19` and that `x264_mb_type_fix` maps `I_8x8` to `I_4x4` and all other types to themselves.
2. **Intra prediction table:** for all 12 modes, verify the neighbour mask against the H.264 intra prediction sample requirements (table in §5.6).
3. **Intra mode helper:** construct cache entries `(left, top)` = `(unavailable, V)` and assert the helper returns DC; `(H, V)` returns V (smaller mode); `(DC_LEFT, V)` returns DC after normalisation.
4. **NNZ helper:** verify `(0x80, 5) -> 5`, `(5, 0x80) -> 5`, `(0x80, 0x80) -> 0`, `(3, 5) -> 4` (round-up average).
5. **Partition tables:** for each of the 17 partition types, assert `partition_count_table` matches the number of ones in the corresponding `listX_table` entries, and that `pixel_table` reflects the geometric size.
6. **Transform permission:** with `b_transform_8x8_mode` off, helper returns 0 for every type; with it on, verify P_8x8 with four `D_L0_8x8` returns nonzero and any other sub-partition returns 0.
7. **Zigzag invertibility:** for both frame and field tables, assert each is a permutation of `0..15` / `0..63`, and that applying it then inverse-scanning recovers raster order.
8. **Pack helpers:** assert `pack8to16(0x12, 0x34)` produces memory bytes `12 34` on little-endian and `34 12` on big-endian; repeat for other widths.
9. **Chroma QP monotonicity:** verify the table is non-decreasing and matches the standard chroma QP mapping at QP 0, 29, 30, 39, 51.

## 11. Open questions

* `QP_BD_OFFSET` and the exact `QP_MAX` value depend on build configuration; the precise number of leading zero entries in `i_chroma_qp_table` is therefore build-dependent — UNKNOWN from this header alone.
* The meaning of `x264_transform_allowed[P_8x8] == 2` beyond "handled specially" is inferred from the helper; any other consumer is UNKNOWN.
* `MB_PRIVATE` usage sites are not visible in this header — UNKNOWN.
* The exact semantics of `PIXEL_*` values used by `x264_mb_partition_pixel_table` are defined in `set.h`, outside the assigned files — UNKNOWN.
