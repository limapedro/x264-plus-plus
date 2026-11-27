# `encoder/cavlc.c`

**Role:** Serializes one macroblock's syntax elements into a CAVLC (context-adaptive variable-length coding) bitstream, and provides a bit-count-only RD variant used during mode decision.

> **Dirty-side behavioural specification.** Describes observable behaviour and structure only; no source text or near-verbatim pseudocode is reproduced.

---

## 1. Header

- Source path: `encoder/cavlc.c`
- Role: encode macroblock layer syntax — macroblock type, sub-partition types, reference indices, motion-vector differences, coded-block pattern, QP delta and quantized residuals — using Exp-Golomb and fixed VLC tables onto the frame's `bs_t`.
- Marker: dirty-side behavioural specification.

---

## 2. Purpose

CAVLC is the mandatory entropy-coding mode of H.264 Baseline profile and an optional (simpler) mode in other profiles. Unlike CABAC it writes each syntax element with a variable-length code drawn from tables whose selection depends on a small local context (`nC`, coded-block pattern, prediction mode). This module produces that final bitstream in the normal build and a fast approximation used during rate–distortion search (`RDO_SKIP_BS`), where only bit counts matter and the actual bytes need not be decodable.

---

## 3. Public interface

- `void x264_macroblock_write_cavlc( x264_t *h )` — write the current macroblock into `h->out.bs`. Handles MBAFF field flag, PCM fallback, slice-specific header, CBP, transform-size flag, QP delta and residuals; updates `h->stat.frame.i_mv_bits` and `i_tex_bits`.
- In RD mode, `int partition_size_cavlc(...)`, `subpartition_size_cavlc(...)`, `cavlc_intra4x4_pred_size(...)`, `partition_i8x8_size_cavlc(...)`, `partition_i4x4_size_cavlc(...)`, `chroma_size_cavlc(...)` return the number of bits a candidate would cost by writing into a counting-only bit writer.
- All other helpers are `static`; `x264_cavlc_block_residual` is a macro wrapping the residual writer.

---

## 4. State and data structures

- `bs_t *s = &h->out.bs` — the frame bit writer; in RD mode its `i_bits_encoded` counter is used instead of real bytes.
- `x264_t.mb` — decided macroblock state: `i_type`, `i_partition`, `i_sub_partition[4]`, `i_cbp_luma`/`i_cbp_chroma`, QP/last QP, `b_transform_8x8`, `pic.i_fref[]`, neighbour MVD/non-zero-count caches.
- `x264_t.dct` — coefficient blocks as in the CABAC writer.
- `x264_run_level_t` — produced by `h->quantf.coeff_level_run[]`: arrays of non-zero levels and run information plus `mask` and `last`.
- Static VLC tables: `x264_coeff_token[nC][total-1][trailing]`, `x264_level_token[suffix_length][value]` (each `vlc_t` carrying bit count and bits, with an `i_next` suffix-length selector), `x264_total_zeros`, `x264_total_zeros_2x2_dc`, `x264_total_zeros_2x4_dc`, `x264_run_before`, `x264_coeff0_token[nC]`.

Invariants: `nC` must be 0..4 (5 for chroma DC); coefficient tables are indexed by category; I_PCM writes are byte-aligned and then the bit writer is reinitialised.

---

## 5. Algorithm / behaviour

### 5.1 Residual coding

The coefficient-level/run function extracts non-zero coefficients in reverse zigzag order. The number of trailing coefficients with absolute value 1 (up to three) is computed branchlessly from the first three levels via a small `ctz_index` lookup; their signs are packed into a short bit field. The `coeff_token` VLC selected by `nC`, `total-1` and `trailing` is written first, followed by the trailing signs. The suffix length starts at 1 when `total > 10 && trailing < 3`, else 0. Remaining levels are written using `level_token` VLCs whose `i_next` updates the suffix length adaptively; levels outside the table use an escape path: a 15-prefix plus a suffix carrying the excess magnitude, with the prefix extended in High Profile when the code would otherwise exceed a 12-bit suffix; in lower profiles an overflow sets `h->mb.b_overflow` (real writer) or adds a 2000-bit penalty (RD writer), causing a later re-encode at coarser quantization. The suffix length after each level grows according to a `next_suffix` threshold sequence {0,3,6,12,24,48,unreachable} when the absolute level exceeds the current threshold.

Total-zero and run-before VLCs follow; for chroma DC a smaller 2x2 (4:2:0) or 2x4 (4:2:2) total-zeros table is used when fewer than the full set of DC coefficients is coded. A block with an all-zero non-zero-count writes the single `coeff0_token[nC]` code. `nC` for a block is derived from the predicted number of non-zero coefficients of neighbours, quantized to 0..4 through `ct_index[17] = {0,0,1,1,2,2,2,2,3,3,3,3,3,3,3,3,3}`; chroma DC uses 4 (4:2:0) or 3 (4:2:2) directly.

For 8x8 transform blocks, CAVLC requires 4x4 sub-blocks: an `interleave_8x8_cavlc` helper shuffles the 8x8 coefficients into four 4x4 arrays (updating non-zero counts) before writing 4x4 residuals.

### 5.2 Headers by slice type

**I slice** (`offset = 0`): `I_16x16` writes `ue(1 + pred_mode + cbp_chroma*4 + (cbp_luma==0?0:12))`, combining mode and CBP; `I_4x4`/`I_8x8` write `ue(0)`, then a transform-size bit when enabled, then for each 4x4 (or each 8x8 when 8x8 transform selected) a prediction-mode syntax: a single 1 if the predicted mode equals the chosen mode, otherwise a 4-bit value `mode - (mode > pred)`. Chroma prediction mode is written as `ue(fix(mode))` when chroma is present. For intra macroblocks in P and B slices the same tree is used with offsets 5 and 23 respectively (accounting for the macroblock-type code space).

**P slice**: `P_L0` 16x16 writes a single 1 bit then optional reference index (truncated binary with range `fref[0]-1`) and one MVD; 16x8 writes `ue(1)`, two refs and two MVDs; 8x16 writes `ue(2)`, two refs and two MVDs. `P_8x8` writes `ue(4)` when all four sub-block references are zero (no per-sub-block refs), else `ue(3)`; four sub-partition codes from a small table; per-sub-block references when needed; and per-sub-block MVDs with block dimensions chosen by sub-partition shape.

**B slice**: `B_8x8` writes `ue(22)`, four sub-partition codes from a 13-entry table, references per active list and sub-partition, then MVDs per active list. The nine inter-B types (L0_L0 … BI_BI) use a 3x9 table indexed by partition shape (16x8, 8x16, 16x16) and type; a per-type list table gates which reference indices and MVDs are written. `B_DIRECT` writes a single 1 bit. Intra macroblocks in B slices use offset 23.

### 5.3 QP delta, MVD, CBP and macroblock assembly

`cavlc_qp_delta` computes `qp - last_qp` and suppresses it for an empty I_16x16 (no luma/chroma CBP and no DC coefficients) when the new QP would be higher, restoring the previous QP in the real writer. Nonzero deltas are wrapped into `[-((QP_MAX_SPEC+1)/2), QP_MAX_SPEC/2]` by adding or subtracting `QP_MAX_SPEC+1`, then written as signed Exp-Golomb (`se`). MVDs are the signed Exp-Golomb differences between the cached MV and the predictor from `x264_mb_predict_mv`, one per component; the 8x8 helper emits them per sub-partition.

The macroblock writer assembles: optional MBAFF field-decoding flag (1 bit, written on the top macroblock of a pair or when the above macroblock is skipped); I_PCM writes `ue(offset + 25)` with offsets `{5,23,0}` for P/B/I slices, byte-aligns, writes raw samples at `BIT_DEPTH` for each plane and chroma component, then reinitialises the bit writer; macroblock header for the slice type; CBP as `ue(cbp_to_golomb[chroma][intra][(cbp_chroma<<4)|cbp_luma])` for non-I_16x16; optional transform-size flag when allowed and luma CBP nonzero; QP delta; residuals. I_16x16 writes luma DC then (if luma CBP) 16 AC blocks per plane; other coded macroblocks write luma residuals per CBP bit and sub-block; chroma writes both DC blocks and, for `cbp_chroma == 2`, AC blocks in a traversal stepped by `8 << CHROMA_V_SHIFT`.

### 5.4 RD-only mode

When `RDO_SKIP_BS` is set, the bit writer counts bits instead of emitting bytes; CBP, chroma DC and reference indices are omitted because they do not vary among the candidates being priced; sub-partitions are written only for P_8x8 to support sub-8x8 decisions; I_PCM is not supported in this mode. Per-partition helpers reset the counter, write the varying elements, and return the count. Chroma-format handling: 4:4:4 uses three planes with no separate chroma pass; 4:2:0/4:2:2 use one luma plane plus chroma; monochrome uses one plane only.

---

## 6. Dependencies

Calls `bs_write*`/`bs_align_0`/`bs_init` from `common/bitstream.h`, prediction helpers from `macroblock.c`, coefficient-level/run and interleave functions in `h->quantf`/`h->zigzagf`, and the VLC tables from `common/tables.h`. Called from `macroblock.c` and `rdo.c`; `x264_cavlc_block_residual` is also used by the RD helpers. Overflow handling interacts with the macroblock re-encode path in `analyse.c`/`encoder.c` via `h->mb.b_overflow`.

---

## 7. Constants and tables

- `cbp_to_golomb[2][2][48]` — two chroma-format families (monochrome and 4:2:0-like) times intra/inter, mapping the combined CBP index to an Exp-Golomb code number.
- `mb_type_b_to_golomb[3][9]`: 16x8 = `{4,8,12,10,6,14,16,18,20}`, 8x16 = `{5,9,13,11,7,15,17,19,21}`, 16x16 = `{1,-1,-1,-1,2,-1,-1,-1,3}`; `-1` means the mode is invalid for that partition.
- `subpartition_p_to_golomb = {3,1,2,0}` (8x8, 8x4, 4x8, 4x4).
- `subpartition_b_to_golomb[13] = {10,4,5,1,11,6,7,2,12,8,9,3,0}`.
- `next_suffix[7] = {0,3,6,12,24,48,0xffff}`; escape level prefix base 15; table threshold `1<<12`; RD overflow penalty 2000 bits.
- `ctz_index[8] = {3,0,1,0,2,0,1,0}`; `ct_index[17]` as above; `count_cat[14] = {16,15,16,0,15,64,16,15,16,64,16,15,16,64}`; `i_offsets[3] = {5,23,0}` for PCM.
- `LEVEL_TABLE_SIZE` bounds the direct level-token lookup.

---

## 8. Performance notes

CAVLC is used mainly for Baseline profile and for fast presets; it is a hot path but less than CABAC in high-quality encodes.

Opportunities, with impact:
- **Table-driven coding**: coeff-token, level, total-zero and run-before tables make residual coding mostly lookups; keeping them compact and cache-resident is the main speed lever. No output change.
- **Branchless trailing-zero detection**: the `ctz_index` trick avoids per-level branches; a clean implementation may use a simple loop — identical output, modest slowdown.
- **SIMD coefficient scanning**: `coeff_level_run` and `interleave_8x8_cavlc` are hot and may be accelerated with SIMD; must remain bit-exact.
- **RD approximate mode**: omitting CBP/chroma-DC/reference terms and using a counting bit writer is an intentional speed/accuracy trade; it affects only decisions, not emitted bytes.
- **Overflow avoidance**: the escape path and the overflow flag trigger a macroblock re-encode at coarser QP; a clean implementation must preserve that fallback to avoid invalid streams at low profiles.

No threading within a frame; bit-writer state is per-frame. Determinism requires the exact VLC table contents and level/run ordering.

---

## 9. References

- ITU-T H.264, Annex B (CAVLC): Tables 9-5 (`coeff_token`), 9-6 (`level`), 9-7 (`total_zeros`), 9-8 (`run_before`), 9-9 (chroma DC total zeros), 9-10 (CBP/mb_type mappings).
- "Context-Adaptive Variable Length Coding for H.264/AVC" (Luthra, Chen, Zakharov), overview of CAVLC tables.
- x264 public source documentation and `common/tables.h` VLC definitions.

---

## 10. Verification checklist

- Compare emitted CAVLC bytes bit-for-bit with a known-good encoder for fixed macroblock decisions across all slice types and profiles.
- Decode streams with an independent H.264 decoder and verify reconstructed output.
- Exercise residual edge cases: all-zero blocks, exactly three trailing ones, suffix-length growth, escape coding, level magnitudes near `LEVEL_TABLE_SIZE`, and High-Profile extended prefixes.
- Verify the `b_overflow` re-encode fallback triggers at low profiles for large levels and does not trigger at High Profile.
- Check `nC` derivation for luma DC/AC and chroma DC (4:2:0 vs 4:2:2), including neighbour-unavailable cases.
- Verify QP-delta suppression for empty I_16x16 and modular wrap at QP_MAX_SPEC.
- Confirm 8x8-transform CAVLC interleaving produces the same bitstream as the reference.

---

## 11. Open questions

- The exact derivation of `x264_mb_predict_non_zero_code` is defined in `macroblock.c`; only its quantisation via `ct_index` is described here.
- Precise contents of the VLC tables are standard-defined and intentionally not reproduced; a clean implementation must source them from the H.264 specification.
- The behaviour of RD mode for I_PCM is UNKNOWN/unsupported in the reference; it is not exercised.
