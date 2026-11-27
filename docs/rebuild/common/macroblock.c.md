# macroblock.c — dirty-side behavioural specification

Source path: `common/macroblock.c`
Role: implements the per-macroblock support layer: buffer allocation, neighbour cache loading/saving with slice and MBAFF edge cases, motion compensation dispatch, deblocking-strength preparation, and biprediction weight derivation.

> Dirty-side behavioural specification. Describes observable behaviour and structure only; no source text is reproduced.

## 1. Header / identity

This file is the bridge between the encoder's analysis/decision code and the actual pixel/syntax state. It owns the life cycle of per-macrobook data: allocate arrays for the frame, alias them at slice start, load a 4×4-block-granular "cache" of neighbour values before coding a macroblock, write reconstructed pixels and neighbour data back after coding, and derive the inputs needed for in-loop deblocking. It also implements motion compensation for all partition shapes and the direct-mode/spatial MV prediction helpers declared in `macroblock.h` (the MV prediction algorithms themselves live in `mvpred.c`).

## 2. Purpose

Neighbour-dependent H.264 coding requires that, at the moment a macroblock is encoded, the left/top/top-left/top-right values are available in a uniformly indexed scratch structure — including *sentinel* values for neighbours outside the picture, across a slice boundary, or in a different field. This file builds that structure, handling the intricate MBAFF (adaptive frame/field) rules where "left neighbour" may be in a different macroblock pair and reference indices / vertical MV components must be converted between frame and field coordinates. It also writes the reconstructed result back so the next macroblock sees it, and prepares deblocking strengths from the coded block patterns, reference indices and motion vectors.

## 3. Public interface

Contracts are declared in `macroblock.h.md`. Summary:

| Function | Contract |
|---|---|
| `x264_macroblock_cache_allocate/free` | Allocate/free per-frame arrays sized from `i_mb_count`. |
| `x264_macroblock_thread_allocate/free` | Allocate per-thread border backups, deblock strength rows and scratch buffers. |
| `x264_macroblock_slice_init` | Alias the cache arrays to the reconstructed frame's arrays; build slice/colocated maps. |
| `x264_macroblock_thread_init` | Set per-thread analysis parameters and local fenc/fdec plane pointers. |
| `x264_macroblock_cache_load_progressive / _interlaced` | Load neighbour cache and picture pointers for one macroblock. |
| `x264_macroblock_cache_save` | Write reconstructed pixels, neighbour metadata and CABAC side data for one macroblock. |
| `x264_macroblock_deblock_strength` | Fill the 8×4 vertical and 4×8 horizontal boundary-strength arrays. |
| `x264_macroblock_bipred_init` | Precompute distance-scale factors and B-slice bipred weights for all reference pairs. |
| `x264_prefetch_fenc`, `x264_copy_column8` | Small prefetch/copy utilities. |
| `x264_mb_mc`, `x264_mb_mc_8x8` | Perform motion compensation for the current macroblock / one 8×8 partition. |

## 4. State and data structures

### Grid strides

* `i_mb_stride = i_mb_width`
* `i_b8_stride = 2 * i_mb_width` (8×8 block grid)
* `i_b4_stride = 4 * i_mb_width` (4×4 block grid)

Indices are `i_mb_xy = mb_y*mb_stride + mb_x`, `i_b8_xy = 2*(mb_y*b8_stride + mb_x)`, `i_b4_xy = 4*(mb_y*b4_stride + mb_x)`.

### Allocated arrays (per frame)

| Array | Element size | Purpose |
|---|---|---|
| `qp` | `int8_t` | Quantiser per macroblock. |
| `cbp` | `int16_t` | Coded block pattern: low 4 bits luma, high nibble chroma, `0x100` luma DC, `0x200`/`0x400` chroma DC, `0x1000` PCM. |
| `mb_transform_size` | `int8_t` | 8×8 transform flag. |
| `slice_table` | `int32_t` | `first_mb` of the slice owning each macroblock; `-1` initially. |
| `intra4x4_pred_mode` | `int8_t[8]` per MB | Actual prediction modes for I4x4; DC (2) otherwise; 8th slot is padding. |
| `non_zero_count` | `uint8_t[48]` per MB | Non-zero coefficient counts for 16 luma + 32 chroma positions; 16 for PCM. |
| CABAC-only: `skipbp`, `chroma_pred_mode`, `mvd[0]`, `mvd[1]` | 1 byte / MB (mvd: 8×2 bytes) | Skip bit patterns, chroma prediction, and absolute MVD values clipped to `[0,33]`. |
| `mvr[list][ref]` | `int16_t[2]` per MB, with one leading guard entry | 16×16 MV for every reference, used as ME predictors. |

`i_refs` for list 0 is `min(X264_REF_MAX, i_frame_reference) << PARAM_INTERLACED`; for list 1 it is `min(X264_REF_MAX, 1 + !!b_pyramid) << PARAM_INTERLACED`. With SMART weighted prediction the count grows by `1 + (BIT_DEPTH == 8)` to accommodate duplicated references. Each `mvr` pointer is advanced by one entry so index `-1` addresses the zero-initialised guard slot.

### Per-thread buffers

* `intra_border_backup[slot][plane]` — `(mb_width*16 + 32)` pixels each, pointer advanced by 16 so negative indices can hold the left column. Number of slots is 2, or 5 for MBAFF.
* `deblock_strength[field]` — one row for progressive, or per-field; shared across threads in sliced-threads mode.
* `scratch_buffer` — reused for halfpel interpolation, SSIM, TESA (if `me_method >= ESA`), or MB-tree, sized as the maximum of those needs. `scratch_buffer2` holds lookahead-thread or MB-tree propagation scratch.

### Local picture buffers

`fenc_buf` is a 48×16 array (stride 16); `fdec_buf` is a 54×32 array (stride 32). Plane pointers are placed so that one-row and one-column borders exist for intra prediction: luma fdec starts two rows in, chroma offsets are 20 rows (4:2:0/4:2:2) or 38 rows (4:4:4), with U/V separated by 8 or 16 columns. `x264_copy_column8` copies an 8-pixel column from the right edge of the reconstructed block into the left border slot for the next macroblock.

### Neighbour cache

See `macroblock.h.md` §4 for the scan8 layout. Sentinel encodings used by this file:

| Data | Unavailable sentinel | Meaning |
|---|---|---|
| `ref[list][...]` | `-2` | neighbour outside picture/slice/field |
| `mv[list][...]` | `0` | paired with `ref = -2` |
| `intra4x4_pred_mode[...]` | `-1` | unavailable, forces DC prediction |
| `non_zero_count[...]` | `0x80` | unavailable, used by CAVLC count prediction |
| `i_cbp_top/left` | `-1` | no neighbour |
| `skip[...]` | `0` | not a skip block |
| `mvd[...]` | `0` | unavailable |

## 5. Algorithm / behaviour

### 5.1 Motion compensation helpers

Three internal helpers handle the three reference-list cases. All operate in units of 4×4 blocks: `x`/`y` are 4×4-block coordinates within the macroblock, `width`/`height` are 4×4-block counts.

**L0-only / L1-only helpers.** Steps:

1. Compute the scan8 slot of the block's top-left 4×4: `scan8[0] + x + 8*y`.
2. Read the reference index from `cache.ref[list][slot]`.
3. Read the MV from `cache.mv[list][slot]`, clip each component to `[mv_min[k], mv_max[k]]`, then add the block's pixel offset `16*x` and `16*y` (expressed as `4*4*x`, `4*4*y`). Clipping before offsetting ensures each partition's MV stays inside the legal expanded picture for the *whole* macroblock.
4. Call the luma motion compensator with destination at `p_fdec[0][4*y*FDEC_STRIDE + 4*x]`, source at `p_fref[list][ref][plane*4]` (the four halfpel planes), source stride `i_stride[0]`, dimensions `4*width × 4*height`, and a weighting function: for L0, the slice's weight table entry for that reference; for L1, no weighting.
5. **4:4:4:** repeat the same operation for planes 1 and 2.
6. **4:2:0/4:2:2:** apply a vertical parity correction: when `CHROMA_V_SHIFT != 0`, the current macroblock is interlaced, and the reference index is nonzero, adjust `mvy` by `(mb_y & 1)*4 - 2`. This compensates for chroma sampling when interpolating from a field of opposite parity. Compute the chroma destination offset as `(4*FDEC_STRIDE >> v_shift)*y + 2*x`, chroma height as `4*height >> v_shift`, and call the chroma MC with MV components `(mvx, 2*mvy >> v_shift)` and size `(2*width, chroma height)`. Then apply chroma weighting functions for planes 1 and 2 if present.

**Bi-prediction helper.** Same clip-and-offset for both lists. For each plane:

1. Use `get_ref` to interpolate L0 and L1 into two aligned 16×16 temporary buffers.
2. Call `avg[mode]` to blend them into the destination, where `mode = size2pixel[height][width]` selects a size-specialised averaging routine and `weight = bipred_weight[ref0][ref1]` supplies the blend factor.
3. For chroma in 4:2:0/4:2:2, interpolate into two temporary chroma buffers (each 8 pixels wide within a 16-byte stride), then blend using `luma2chroma_pixel[mode]` to choose the chroma-sized averaging kernel.

### 5.2 Motion-compensation dispatch

`x264_mb_mc_8x8(h, i8)` derives `x = 2*(i8&1)`, `y = 2*(i8>>1)`.

* In **P slices**, switch on `i_sub_partition[i8]`: `D_L0_8x8` calls the L0 helper once with size (2,2); `D_L0_8x4` calls it twice with size (2,1) at consecutive y; `D_L0_4x8` twice with size (1,2); `D_L0_4x4` four times with size (1,1).
* In **B slices**, inspect the reference at the 8×8 slot. If both L0 and L1 references are `>= 0`, use bi-prediction; else use whichever single list is available. The whole 8×8 is always predicted with one MV pair — the smaller sub-partitions are not independently MV-coded here.

`x264_mb_mc(h)` handles the whole macroblock:

* If `i_partition == D_8x8`, call `x264_mb_mc_8x8` four times.
* Otherwise read references at scan8 slot 0 (`ref a`) and slot 12 (`ref b`, bottom-right). For `D_16x16`, one prediction covering (4,4) using `ref a`. For `D_16x8`, two (4,2) predictions using `ref a` for the top and `ref b` for the bottom. For `D_8x16`, two (2,4) predictions using `ref a` for the left and `ref b` for the right. Each prediction chooses L0-only, L1-only or bi-prediction by testing `ref0 >= 0` and `ref1 >= 0` at that partition's top-left slot.

Decision table for a partition's prediction mode:

| `ref L0` | `ref L1` | Mode |
|---|---|---|
| `>= 0` | `>= 0` | bi-prediction with both MV/ref and bipred weight |
| `>= 0` | `< 0` | L0 only, with weighted prediction if applicable |
| `< 0` | `>= 0` | L1 only, no weighting |
| `< 0` | `< 0` | UNKNOWN / not reachable for a coded inter partition |

### 5.3 Allocation and initialisation

`x264_macroblock_cache_allocate` computes strides, allocates the arrays in §4 with a single preallocator block, zeroes the guard entries of every `mvr` row, and fills `slice_table` with `-1`. Failure returns `-1`; success `0`.

`x264_macroblock_thread_allocate` allocates the border-backup and deblock-strength buffers, then computes the maximum of several scratch requirements: halfpel buffer `(width+48+32)*sizeof(int16_t)`, SSIM buffer `b_ssim*8*(width/4+3)*sizeof(int)`, TESA buffer `(2*me_range+24)*sizeof(int16_t) + (me_range+4)*(me_range+1)*4*sizeof(mvsad_t)` when `me_method >= ESA`, and MB-tree buffer `align(width*sizeof(int16_t), NATIVE_ALIGN)`. A second scratch buffer covers lookahead threads and MB-tree propagation.

`x264_macroblock_slice_init` aliases `h->mb.mv/ref/type/partition/field` to the reconstructed frame's arrays, copies ref POCs, and builds two maps:

* **B slices:** `map_col_to_list0(-1) = -1`, `map_col_to_list0(-2) = -2`; for each reference of the colocated list-0 frame, find a POC match in the current list 0 and store the matching index, else `-2`. Used by temporal direct-mode prediction.
* **P slices with SMART weights and deblocking enabled:** `deblock_ref_table` maps `-1`/`-2` to themselves and each real reference to `frame_num & 63` (frame) or `((frame_num&63)<<1) | field_parity` (MBAFF), so duplicate frames compare equal for deblocking.

It also initialises the cache ref bytes to `-2` and computes `inv_ref_poc = (256 + delta/2) / delta` per field, where `delta = cur_poc - ref_poc` for list-0 reference 0. Finally it seeds two `i_neighbour4` arrays: blocks 6, 9, 12, 14 get all four neighbours; blocks 3, 7, 11, 13, 15 get left+top+top-left.

`x264_macroblock_thread_init` adjusts analysis settings: for B slices, subpel refinement levels 6 and 8 are reduced by one; chroma ME is enabled only for P with refinement >= 5 or B with refinement >= 9; DCT decimation is forced on for B slices and otherwise enabled for non-I slices when configured. It resets `i_mb_prev_xy` and sets up the fenc/fdec plane pointers described in §4.

### 5.4 Neighbour discovery (`macroblock_cache_load_neighbours`)

1. Set `mb_x`, `mb_y`, `i_mb_xy`, `i_b8_xy`, `i_b4_xy`; clear all availability flags, neighbour coordinates/types, and set the default left-index table to the "both same" variant.
2. Compute `top_y = mb_y - (1 << mb_interlaced)` and `top = top_y*mb_stride + mb_x`. By default `left = i_mb_xy - 1`, with matching 8×8/4×4 indices.
3. **MBAFF adjustments.** When MBAFF is active, the left/top/top-left/top-right MBs must be chosen according to the field flags of the current and neighbouring macroblocks:
   * For an odd (bottom) field MB whose left neighbour has a different frame/field status, the left neighbour is taken from the other row of the MB pair; if the current MB is field-coded, two distinct left MBs are used for the top and bottom halves and the "current interlaced" index table is selected; otherwise the "current progressive" table is selected and the top-left partition shifts by one.
   * For an even (top) field MB with a frame-coded top pair, the top MB is taken from the lower row of the pair; top-left/top-right rows are similarly adjusted from the neighbour field flags.
   * The top-right neighbour is suppressed entirely when a bottom field MB is frame-coded in certain configurations.
4. **Left availability:** `mb_x > 0` grants `i_neighbour_frame |= MB_LEFT` and records both left MBs. `i_neighbour |= MB_LEFT` only if `slice_table[left] == sh.i_first_mb` (same slice). `i_neighbour_intra |= MB_LEFT` additionally requires either constrained-intra mode off or the left MB being intra. (The source notes MBAFF + constrained intra is not supported.)
5. **Top / top-left / top-right availability:** refused if the neighbour row belongs to a previous thread slice (`i_threadslice_start >> mb_interlaced != mb_y >> mb_interlaced`), which prevents reading data not yet encoded. Otherwise each neighbour requires `top_y >= 0` and the same slice; availability flags and intra flags are set analogously. The top neighbour's `cbp`, `intra4x4_pred_mode`, `non_zero_count`, `mb_transform_size` and (CABAC) `skipbp` addresses are prefetched because they are cold in cache, unlike the just-written left neighbour.

Decision table for a neighbour's three availability levels:

| Condition | `i_neighbour_frame` | `i_neighbour` | `i_neighbour_intra` |
|---|---|---|---|
| Inside picture and same row/threadslice | set | — | — |
| Also same slice | set | set | — |
| Also intra (or constrained-intra off) | set | set | set |

### 5.5 Cache loading (`macroblock_cache_load`)

After neighbour discovery, the loader fills the scan8-indexed cache:

**Top neighbour (if `MB_TOP` available):** copy `cbp`, four `intra4x4_pred_mode` bytes, and three groups of four `non_zero_count` bytes (luma, U, V). Otherwise set `i_cbp_top = -1`, fill intra modes with the byte pattern `ff`, and fill NNZ with the sentinel byte `0x80`. The unavailable values are written as 32-bit stores for speed.

**Left neighbour (if `MB_LEFT` available):** for MBAFF, combine CBP from the two left MBs using the index table's `mv` entries; otherwise copy `cbp` directly. Copy four intra-mode bytes and four luma NNZ bytes using the index table's `intra`/`nnz` indices; chroma NNZ uses either dedicated `nnz_chroma` indices (4:2:0) or luma-offset indices (4:2:2/4:4:4). Otherwise set `i_cbp_left = -1`, all four intra modes to `-1`, and all NNZ entries to `0x80`.

**Transform-size neighbours:** `i_neighbour_transform_size` is the count (0, 1 or 2) of available left/top neighbours with `mb_transform_size` set. It is only populated when the PPS 8×8-transform mode is enabled.

**Picture pointers:** macroblock_load_pic_pointers is called for luma and the appropriate chroma planes. It computes pixel offsets with field-aware strides, copies fenc into the local buffer, copies saved bottom rows from `intra_border_backup` into the top border of fdec, restores the left border column from the reconstructed frame for MBAFF or re-encoding, and installs references to the four halfpel/filtered planes for every active reference in both lists, plus weighted reference pointers when applicable. For interlaced references the field plane arrays are used and the pointer offset differs between the current and opposite field.

**MV/ref/mvd cache:** for each active list:
* top-left neighbour: bottom-right 4×4 of the top-left MB, or in MBAFF with a shifted top-left partition, the middle 4×4. Unavailable → `ref = -2`, `mv = 0`.
* top neighbour: four 8×8 ref slots (two distinct refs) and four 4×4 MVs copied with wide loads. Unavailable → all refs `-2`, all MVs zero.
* top-right neighbour: one ref/MV pair; unavailable → `ref = -2`.
* left neighbour: four ref/MV pairs, using the index table for MBAFF. Unavailable → four `-2`/zero pairs.
* MBAFF top-right fallback: when the regular top-right slot is absent but entries exist in the left macroblock, three special "topright" MV/ref entries are loaded from the left MB's upper/lower halves.
* CABAC MVD cache: top row copied when available; left column copied only when the corresponding left ref is usable (`>= 0` in MBAFF); otherwise zero.
* **Field/frame MV conversion:** in MBAFF, a neighbour from a frame MB when the current MB is field-coded (or vice versa) must be converted: reference indices are doubled or halved, the vertical MV component is halved or doubled, and the MVD vertical component is shifted by one, but only when that neighbour's reference is `>= 0`. This is applied to top-left, top, top-right, left and the special MBAFF top-right entries.

**B-slice skip cache:** `skipbp` bits from the left and top neighbours are extracted into the scan8 slots using the index table (MBAFF) or fixed bit positions (progressive: left bits 1 and 3, top bits 2 and 3).

**P-slice skip MV:** `x264_mb_predict_mv_pskip` is called to fill `cache.pskip_mv`.

**Intra neighbour masks:** `i_neighbour4`/`i_neighbour8` are set for the first few blocks using combinations of `i_neighbour_intra`, e.g. block 0 gets left+top+top-left plus top-right when top is available; blocks 1 and 4 get left plus (if top available) top+top-left+top-right; blocks 2, 8, 10 get top+top-right plus (if left available) left+top-left; block 5 gets left plus top-right plus (if top available) top+top-left. These arrays are indexed by 4×4 block number and consumed by intra prediction.

### 5.6 Deblocking-strength preparation

`x264_macroblock_deblock_strength` fills `bs[vertical][8][4]` and `bs[horizontal][4][8]`:

1. **Intra shortcut:** if the macroblock is intra, all internal vertical and horizontal edges are set to strength 3 and the function returns.
2. **8×8-DCT shortcut:** if the 8×8 transform is active, 4:4:4 is off, and every luma CBP bit in the mask `0xf >> CHROMA_V_SHIFT` is set, all relevant edges are set to strength 2 and the function returns. (For 4:2:0 this mask is `0x7` because the third bit is the luma-DC flag.)
3. If the deblocking filter ID is not 2, adopt `i_neighbour_frame` as the effective neighbour mask (ignoring slice boundaries) and remember which bits changed; for changed neighbours, reload the NNZ/ref/MV values from the frame arrays (with MBAFF-specific left indices).
4. **SMART-weight P slices:** reference indices in the cache are passed through `deblock_ref_table` so that duplicate references compare equal; the four current-block refs are mapped similarly and replicated into the cache rows.
5. **CAVLC + 8×8-DCT:** when 8×8 DCT is in use without CABAC, neighbour NNZ values are collapsed to 0/1 "any coefficient in this 8×8" flags for both top and left neighbours, and the current block's four 8×8 groups are similarly collapsed and replicated.
6. The actual strength for each edge is computed by `h->loopf.deblock_strength`, which receives the NNZ cache, ref cache, MV cache, the output arrays, a height factor of `4 >> MB_INTERLACED`, and the B-slice flag. The standard rule is: strength 2 if either side has a non-zero coefficient (or the refs/MVs differ sufficiently), strength 1 if refs/MVs differ, strength 0 otherwise; intra is handled by step 1.
7. **MBAFF correction:** `macroblock_deblock_strength_mbaff` adjusts the vertical left boundary using an offset table indexed by field parity and row, and possibly forces horizontal edges to at least strength 1 when frame and field macroblocks meet.

### 5.7 Cache save (`x264_macroblock_cache_save`)

1. Back up the bottom rows (and, for MBAFF, the bottom row of each field) into `intra_border_backup` for use as top borders by the next macroblock.
2. Store the reconstructed 16×16 luma and chroma planes into the frame using `macroblock_store_pic`, with field-aware strides.
3. Prefetch the next macroblock's fenc lines.
4. Write `type[i_mb_xy] = x264_mb_type_fix[i_type]`, `slice_table[i_mb_xy] = sh.i_first_mb`, and `partition[i_mb_xy] = D_16x16` for intra types or `i_partition` otherwise; record `i_mb_prev_xy`.
5. **Intra 4×4 modes:** for `I_4x4`, copy the eight cache modes into the per-MB array, packing the trailing entries; otherwise store all DC (2) if constrained-intra is off or the MB is intra, and all `-1` for inter MBs under constrained intra.
6. **QP handling:** for PCM, set `qp = 0`, `i_last_dqp = 0`, `i_cbp_chroma = 0` (or 2 for subsampled chroma), `i_cbp_luma = 0xf`, `cbp = 0x1700 | ...`, disable 8×8 transform, and set every NNZ entry to 1 (CABAC) or 16 (CAVLC). Otherwise, if the MB is not I16x16 and has no coded coefficients, reuse `i_last_qp`; then write `qp`, update `i_last_dqp = i_qp - i_last_qp`, and set `i_last_qp = i_qp`.
7. **NNZ:** copy the 16 luma and (depending on chroma format) 16 or 32 chroma values from the cache into the per-MB array.
8. If luma CBP is zero and the type is not `I_8x8`, clear `b_transform_8x8`; store it in `mb_transform_size`.
9. **MV/ref:** for P/B slices, if the MB is inter, copy the four 8×8 ref indices and sixteen 4×4 MV pairs from the cache into the frame arrays; if intra, write ref `-1` and MV zero for both lists.
10. **CABAC side data:** set `chroma_pred_mode` to the fixed/normalised prediction mode for intra (non-PCM) MBs and DC otherwise. For types where `(0x3FF30 >> i_mb_type) & 1` (i.e. non-intra, non-skip, non-direct), copy the eight MVD pairs from the cache; otherwise zero them. For B slices write `skipbp`: `0xf` for B_SKIP/B_DIRECT, a bitmask of which 8×8 sub-partitions are `D_DIRECT_8x8` for `B_8x8`, and 0 otherwise.

### 5.8 Biprediction weight derivation (`x264_macroblock_bipred_init`)

For each field-mode combination, reference pair `(i_ref0, i_ref1)`:

1. Compute colocated POCs with field deltas:
   `poc0 = l0.i_poc + mbfield * l0.i_delta_poc[field ^ (i_ref0 & 1)]`,
   `poc1 = l1.i_poc + mbfield * l1.i_delta_poc[field ^ (i_ref1 & 1)]`,
   `cur_poc = fdec.i_poc + mbfield * fdec.i_delta_poc[field]`.
2. `td = clip3(poc1 - poc0, -128, 127)`.
3. If `td == 0` (or a long-term reference, not modelled here): `dist_scale_factor = 256`, `weight = 32` (equal blend).
4. Otherwise: `tb = clip3(cur_poc - poc0, -128, 127)`, `tx = (16384 + (abs(td) >> 1)) / td` (rounding division), and
   `dist_scale_factor = clip3((tb * tx + 32) >> 6, -1024, 1023)`.
5. Store the scale factor. Then form `d = dist_scale_factor >> 2`. If weighted biprediction is enabled and `-64 <= d <= 128`, set `weight = 64 - d`; an assertion confirms `d` is actually in `[-63,127]` so an SSSE3 averaging kernel that cannot handle the extrema remains valid. Otherwise `weight = 32`.

This implements the H.264 `DistScaleFactor` and default `weighted_bipred` derivation.

### 5.9 Small utilities

* `x264_prefetch_fenc` computes luma and chroma offsets from `(i_mb_x, i_mb_y)` and calls the MC-layer prefetch with a NULL chroma pointer when chroma is absent.
* `x264_copy_column8` copies eight pixels vertically with a stride, using indices from `-4` to `+3` so the call site's pointer arithmetic can be shorter.

## 6. Dependencies

* Calls MC functions through `h->mc` (copy, mc_luma, mc_chroma, avg, get_ref, load/store interleave, prefetch), loop filter through `h->loopf.deblock_strength`, and prediction helpers from `mvpred.c`.
* Uses `h->fdec`, `h->fref`, `h->fenc`, `h->sh`, `h->pps`, `h->param`, `h->mb`, `h->intra_border_backup`.
* Depends on `predict.h` mode-fix tables, `set.h` pixel constants, `tables.h` (size2pixel, luma2chroma_pixel), and base alignment macros.
* Called by the encoder's analysis/slice-writing loop: cache load before analysis, MC after mode decision, deblock-strength before filtering, cache save before advancing to the next macroblock.

## 7. Constants and tables

| Item | Value / derivation | Why |
|---|---|---|
| Grid strides | `mb_stride = W`, `b8_stride = 2W`, `b4_stride = 4W` | Uniform indexing of three block grids. |
| `intra4x4_pred_mode` stride | 8 bytes per MB | Only 7 used; 8th is write-combining padding. |
| `non_zero_count` stride | 48 bytes per MB | 16 luma + 32 chroma positions. |
| `mvr` guard entry | 1 leading zeroed entry | Allows index `-1` for unavailable predictors. |
| `i_refs` | `min(X264_REF_MAX, refs) << PARAM_INTERLACED`, plus SMART extras | Doubles for interlaced field refs. |
| Left index tables | 4 variants: "current progressive", "current interlaced", "left field"), "both same" | Select which 4×4 rows of the left MB supply each neighbour value in MBAFF. |
| Reference sentinel | `-2` unavailable, `-1` intra/unused | Distinguishes edge from intra. |
| NNZ sentinel | `0x80` | Enables branchless CAVLC count prediction. |
| Intra mode sentinel | `-1` | Forces DC prediction. |
| CBP bits | luma low 4, chroma high nibble, `0x100`/`0x200`/`0x400` DC flags, `0x1000` PCM | Compact neighbour CBP representation. |
| PCM NNZ | 1 (CABAC) / 16 (CAVLC) | Matches coefficient-count semantics for PCM. |
| `deblock_ref_table` | `frame_num & 63` (+ field bit) | Avoids collisions with `-1`/`-2`. |
| `inv_ref_poc` | `(256 + delta/2)/delta` | Reciprocal for temporal MV scaling. |
| `dist_scale_factor` | `clip3((tb*tx+32)>>6, -1024, 1023)`, `tx = (16384 + abs(td)/2)/td` | Standard H.264 formula. |
| Bipred weight | `64 - (dsf>>2)` when in `[-63,127]`, else 32 | Keeps SSSE3 kernel's supported range. |

## 8. Performance notes

Hotness: cache load/save and motion compensation run once per macroblock; deblock-strength preparation runs once per coded macroblock. These are among the top functions at all presets, especially at high resolution.

Concrete optimisation opportunities:

1. **SIMD byte-stream scattering / wide neighbour loads.** The cache layout deliberately places four top NNZ/MV/ref values contiguously so they can be fetched with one 16-byte load and unpacked with shifts; the left-index tables exist to keep these accesses to one per row. A clean implementation should preserve 16-byte alignment of `state`/cache arrays so `movdqa`/`ldr q` can be used. Public reference: Intel Intrinsics Guide, `_mm_load_si128`, `_mm_shuffle_epi8` (https://www.intel.com/content/www/us/en/docs/intrinsics-guide/); ARM NEON Intrinsics Reference (https://developer.arm.com/architectures/instruction-sets/intrinsics/).
2. **Context-state layout.** CABAC context arrays and the MVD cache are byte-wide and aligned; grouping mvd values by 8×8 block lets coefficient-coding SIMD gather all contexts for a block in one load, update them, and scatter back in one store. Public reference: Intel Optimization Reference Manual, "Cache Optimization" (https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html).
3. **Branch elimination.** The availability masks are computed as bit operations rather than nested conditionals; preserve this so neighbour checks are predictable. Similarly, the `b_transform_8x8_allowed` test uses one 32-bit compare against a repeated pattern instead of four comparisons. Public reference: Agner Fog, "Optimizing subroutines in assembly language" (https://www.agner.org/optimize/).
4. **Batching.** Copy the top neighbour's cbp/intra/NNZ/MV/ref with a few wide loads/stores instead of dozens of scalar accesses; batch prefetches for the top row (left is already hot in L1 due to write-allocate). The source itself notes this asymmetry.
5. **Prefetch strategy.** The top neighbour's arrays are prefetched explicitly because they are one macroblock-row away; a clean implementation can extend this to the next row's fenc data using `x264_prefetch_fenc`.
6. **Avoiding aliasing penalties.** The source notes that GCC pessimises direct loads/stores of heap arrays because of aliasing, so it dereferences pointers once into locals. A clean implementation should use `restrict`-like reasoning or local pointer aliases to keep wide loads.
7. **Motion compensation batching.** Group the four 4×4 calls of a `D_L0_4x4` partition into one 16×16 MC when possible, or at least share the MV clip and reference pointer setup across them.

## 9. References

* ITU-T Recommendation H.264 (ISO/IEC 14496-10), §8.3 — motion compensation and sample interpolation; §8.4 — deblocking filter strength; §7.4.2.3 — weighted biprediction and `DistScaleFactor`; Annex G — CABAC neighbour context derivation. https://www.itu.int/rec/T-REC-H.264
* Richardson, "H.264 and MPEG-4 Video Compression", Wiley, 2003 — MBAFF neighbour rules and chroma MV scaling.
* Intel Intrinsics Guide. https://www.intel.com/content/www/us/en/docs/intrinsics-guide/
* Intel® 64 and IA-32 Architectures Optimization Reference Manual. https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html
* ARM NEON Intrinsics Reference. https://developer.arm.com/architectures/instruction-sets/intrinsics/

## 10. Verification checklist

1. **Stride layout:** for a frame of width `W`, assert `b8_stride = 2W`, `b4_stride = 4W`, and that `i_b8_xy`/`i_b4_xy` for `(0,0)` and `(W-1,H-1)` index the expected grid cells.
2. **Cache sentinels at edges:** load the top-left macroblock `(0,0)` and assert no neighbour flags are set, `i_cbp_top/left == -1`, all left/top intra modes are `-1`, all NNZ sentinels are `0x80`, and all refs are `-2` with zero MVs.
3. **Slice boundary:** load a macroblock immediately after a slice start; assert `i_neighbour` excludes the left neighbour while `i_neighbour_frame` may include it, and that deblock-strength preparation can recover the frame-level flag.
4. **Constrained intra:** with constrained intra enabled and an inter left neighbour, assert `i_neighbour_intra` excludes left; with an intra left neighbour it includes it.
5. **MBAFF left selection:** construct a field/frame macroblock pair with differing parity and assert the correct one or two left MB indices are used; check that frame↔field MV conversion doubles/halves ref indices and the vertical MV/MVD components only for neighbours with `ref >= 0`.
6. **MV clipping:** set an MV outside the legal range for a partition not at the macroblock origin and assert the MC call receives the clipped value plus the partition offset `16*x, 16*y`.
7. **MC partition coverage:** for each partition type, count the number of MC helper invocations; assert 1 for 16×16/8×8, 2 for 16×8/8×16/8×4/4×8, and 4 for 8×8 sub-partitioned into 4×4.
8. **Reconstruction round-trip:** after `cache_save`, assert `fdec` pixels at the macroblock position match `p_fdec` local buffer, and that `intra_border_backup` holds the bottom row(s) for the next macroblock.
9. **PCM save:** force I_PCM and assert `qp=0`, `cbp=0x1700|...`, all 48 NNZ entries are the CABAC/CAVLC PCM sentinel, and MV/ref are zero/`-1`.
10. **Bipred vector:** for `td = 0` assert scale factor 256 and weight 32; for `td = 2, tb = 1` compute `tx = (16384+1)/2 = 8192`, `dsf = clip3((1*8192+32)>>6,...) = 128`, `weight = 64 - 32 = 32` (verify with exact integer arithmetic); for a negative `td` verify the sign flips and weight stays in `[0,64]` or defaults to 32.
11. **Deblock shortcut:** with all luma CBP bits set and 8×8 transform active, assert every internal edge strength is 2 without consulting the loopfilter; with an intra macroblock assert strength 3.
12. **Skipbp:** for B_SKIP/B_DIRECT assert `skipbp = 0xf`; for B_8x8 with sub-partitions `[DIRECT, L0, L0, DIRECT]` assert `skipbp = 0b1001`.

## 11. Open questions

* The exact numeric contents of the four `left_indices` tables are fully determined by MBAFF semantics but were not exhaustively derived here; the "both same" case uses intra indices 4,5,6,3 and ref indices 0,0,1,1, while the other variants select the upper/lower field rows — full enumeration is left to the implementer consulting the standard — UNKNOWN in full detail.
* The bit pattern `0x3FF30` used to select MVD-saving macroblock types is an opaque constant; its decoded set of types is "non-intra, non-skip, non-direct" per the adjacent comment — UNKNOWN whether that reading covers every enum value exactly.
* The internal `x264_macroblock_cache_skip` helper is referenced for B slices but defined outside the assigned files — UNKNOWN.
* `x264_macroblock_cache_ref/mv/mv_ptr` used by `mvpred.c` are inline helpers in another header; their exact bounds behaviour is UNKNOWN here.
* The precise handling of long-term references in biprediction is explicitly marked as a FIXME in the source (the POC-based derivation assumes short-term refs) — UNKNOWN.
* The exact deblocking-strength rule implemented by `h->loopf.deblock_strength` is an assembly/C function outside the assigned files; only its inputs and the standard's rule are specified here — UNKNOWN in implementation detail.
