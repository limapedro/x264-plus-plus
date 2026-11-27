# `encoder/cavlc.c`

**Role:** Serializes one macroblock's syntax elements into a CAVLC (context-adaptive variable-length coding) bitstream, and provides a bit-count-only RD variant used during mode decision.

> **Dirty-side behavioural specification.** Describes observable behaviour and structure only; no source text or near-verbatim pseudocode is reproduced.

---

## 1. Header

- Source path: `encoder/cavlc.c`
- Role: encode macroblock layer syntax — macroblock type, sub-partition types, reference indices, motion-vector differences, coded-block pattern, QP delta and quantized residuals — using Exp-Golomb and fixed VLC tables onto the frame's `bs_t`.
- Marker: dirty-side behavioural specification.

Scope note: this document covers the *writer*. The tables of code words it indexes are declared in `common/tables.h` and defined in `common/tables.c` (see those documents); the generated level/run tables live in `common/vlc.c`. The bit-put primitives are in `common/bitstream.h`. All facts below are behavioural; the standard's normative tables are referenced, not reproduced.

---

## 2. Purpose

CAVLC is the mandatory entropy-coding mode of H.264 Baseline profile and an optional (simpler) mode in other profiles. Unlike CABAC it writes each syntax element with a variable-length code drawn from tables whose selection depends on a small local context (`nC`, coded-block pattern, prediction mode). This module produces that final bitstream in the normal build and a fast approximation used during rate–distortion search (`RDO_SKIP_BS`), where only bit counts matter and the actual bytes need not be decodable.

Two properties make this module the highest-risk item for an independent I-slice implementation:

1. **Ordering is normative.** A decoder reads elements in a fixed sequence; any permutation (e.g. writing `total_zeros` before the levels, or CBP before the macroblock type) yields a corrupt picture. Section 5.6 gives the complete order.
2. **The codes are context-selected, not global.** The same integer can have a different code in a different `nC` category. Section 5.2–5.5 derive the context indices and section 7 documents where the code values come from.

The RD variant deliberately omits fields that do not vary among the candidates being priced (CBP, chroma DC, reference indices). It is *not* a decodable stream; it is a pricing shortcut. A clean implementation must keep the two paths separate.

---

## 3. Public interface

- `void x264_macroblock_write_cavlc( x264_t *h )` — write the current macroblock into `h->out.bs`. Handles MBAFF field flag, PCM fallback, slice-specific header, CBP, transform-size flag, QP delta and residuals; updates `h->stat.frame.i_mv_bits` and `i_tex_bits` (only in the real writer). Precondition: all `h->mb.*` decision fields, neighbour caches and coefficient buffers for the current macroblock are final. Postcondition: `bs_t.p` has advanced past this macroblock's bits; statistics reflect the split between header/MV bits (`i_mv_bits`) and texture bits (`i_tex_bits`).
- In RD mode (`RDO_SKIP_BS != 0`), `int partition_size_cavlc(...)`, `subpartition_size_cavlc(...)`, `cavlc_intra4x4_pred_size(...)`, `partition_i8x8_size_cavlc(...)`, `partition_i4x4_size_cavlc(...)`, `chroma_size_cavlc(...)` return the number of bits a candidate would cost by writing into a counting-only bit writer. Each resets `s->i_bits_encoded` to zero on entry and returns the accumulated count; no bytes are emitted.
- All other helpers are `static`; `x264_cavlc_block_residual` is a macro wrapping the residual writer. It computes `nC`, then either emits the single zero-block token or calls the internal residual encoder, and writes back the discovered non-zero count into the neighbour cache so subsequent blocks see the correct context.

Exp-Golomb primitives used here (from `common/bitstream.h`): `bs_write_ue` (unsigned), `bs_write_se` (signed, mapped by `codeNum = 2|k| − (k>0?1:0)`), `bs_write_te` (truncated binary over `0..range-1`), `bs_write1` (single flag), `bs_write` (fixed-width). `bs_write_vlc` writes a `vlc_t` as `bs_write(s, v.i_size, v.i_bits)`.

---

## 4. State and data structures

- `bs_t *s = &h->out.bs` — the frame bit writer; in RD mode its `i_bits_encoded` counter is used instead of real bytes. Fields: `p_start`, `p`, `p_end`, `cur_bits` (pending output word), `i_left` (bits still free in `cur_bits`), `i_bits_encoded`.
- `x264_t.mb` — decided macroblock state: `i_type`, `i_partition`, `i_sub_partition[4]`, `i_cbp_luma`/`i_cbp_chroma`, QP/last QP, `b_transform_8x8`, `pic.i_fref[]`, neighbour MVD/non-zero-count caches.
- `x264_t.dct` — coefficient blocks: `luma16x16_dc[]` (one 16-sample DC block per luma plane), `luma4x4[]` (16 per plane, plus chroma AC at indices 16..47), `luma8x8[]` (4 per plane), `chroma_dc[2]`.
- `x264_run_level_t` — produced by `h->quantf.coeff_level_run[cat](l, &runlevel)`. Fields: `last` (index of the last non-zero coefficient in scan order), `mask` (16-bit bitmask of coefficient positions, also the direct index into `x264_run_before`), `level[18]` (non-zero levels in reverse scan order, with two sentinel entries of value 2 appended). The `mask` is consumed by the run-before precomputed table; `last + 1 − total` gives the number of trailing zeros.
- Static VLC tables in `tables.c`: `x264_coeff_token[nC][total-1][trailing]`, `x264_level_token[suffix_length][value]` (each a `vlc_large_t` carrying `i_bits`, `i_size`, `i_next`), `x264_total_zeros`, `x264_total_zeros_2x2_dc`, `x264_total_zeros_2x4_dc`, `x264_run_before`, `x264_coeff0_token[nC]`.

Invariants: `nC` must be 0..5 (category 5/4 reserved for chroma DC); coefficient tables are indexed by category; I_PCM writes are byte-aligned and then the bit writer is reinitialised. The `non_zero_count` cache is written back after each block so that a following block's `nC` prediction uses the just-decided value. In RD mode the cache writes still occur (they affect later pricing), but no bytes are produced.

---

## 5. Algorithm / behaviour

### 5.1 Residual extraction: levels, runs and trailing ones

The coefficient-level/run function (a quant-module function pointer, `coeff_level_run[ctx_block_cat]`) walks the block in reverse zigzag order — i.e. from the highest-frequency scan position back toward DC — and emits only non-zero coefficients. Its result is:

- `total` = number of non-zero coefficients.
- `level[k]` = the signed value of the k-th non-zero coefficient, k = 0..total−1, in that reverse order. Two extra entries are set to the sentinel 2 so that trailing-one detection can read three positions without a bounds test.
- `last` = scan index of the final non-zero coefficient.
- `mask` = 16-bit occupancy bitmap of non-zero scan positions.
- `total_zeros = last + 1 − total` = number of zero coefficients that precede the last non-zero one in scan order.

Trailing ones (T1s) are the leading entries of `level[]` whose absolute value is exactly 1, capped at three. The reference computes this branchlessly from the first three levels: for each position k it forms a bit that is set when `|level[k]| == 1`, packs bits 0/1/2, and maps the 3-bit pattern through `ctz_index[8] = {3,0,1,0,2,0,1,0}` to the count (the pattern's trailing-zero count gives the run of consecutive ones). A clean implementation may use a simple three-iteration loop with identical output. The signs of the T1s are packed (bit = 1 for negative) and written as one bit each, in scan order, *immediately after* `coeff_token`.

Worked example (4x4 block, zigzag index → value): suppose non-zero positions are index 1→+3, 2→+2, 4→+1, 7→−1. Reverse scan order gives `level[] = [−1, +1, +2, +3]`, so `total = 4`, `last = 7`, `total_zeros = (7+1) − 4 = 4`. Leading abs-1 entries are −1 and +1, then +2 stops the run, so `trailing = 2` and the two sign bits are `1` (−1) and `0` (+1). These are the inputs to the VLC lookups below.

### 5.2 Context selection `nC` and the zero-block token

For a luma AC block, `nC` is derived from the predicted number of non-zero coefficients of neighbouring blocks (`x264_mb_predict_non_zero_code`, defined in `macroblock.c`; only its quantisation is specified here). The predicted sum is clipped to 0..16 and mapped through:

```
ct_index[17] = {0,0,1,1,2,2,2,2,3,3,3,3,3,3,3,3,3}
nC = ct_index[clamp(sum,0,16)]
```

so sums 0–1 → category 0, 2–3 → 1, 4–7 → 2, and 8–16 → 3. Luma DC blocks are predicted separately (the DC neighbour index is `idx−LUMA_DC` scaled by 16) but use the same quantisation. Chroma DC does **not** use neighbour prediction: `nC = 5 − CHROMA_V_SHIFT`, i.e. 5 for 4:2:0 and 4 for 4:2:2 (these are the standard's dedicated chroma-DC categories). Chroma AC uses the same `ct_index` path as luma AC.

Before any residual syntax is written, the writer checks `non_zero_count[scan8[idx]]`. If it is zero, the single `coeff0_token[nC]` code is emitted and the function returns; no `coeff_token`, T1 signs, levels, zeros or runs follow. Representative `coeff0_token` lengths by category are: nC=0 → `1` (1 bit), nC=1 → `11` (2), nC=2 → `1111` (4), nC=3 → `000011` (6), nC=4 → `01` (2), nC=5 → `1` (1). This is the single most important early-exit for flat I-slices: most all-zero chroma/luma blocks cost only one or two bits.

### 5.3 `coeff_token` VLC

`coeff_token` is looked up as `x264_coeff_token[nC][total−1][trailing]`. The three indices are exactly the syntax categories of H.264 Table 9-5: context category nC (0..5), `TotalCoeff − 1` (0..15), and `TotalTrailing` (0..3). Entries with more trailing ones than physically possible for a given total are still populated, so no clipping of `trailing` is needed. The table is prefix-free within each nC category. Derivation of its code values is from the standard (Table 9-5) — the reference stores them as literal `vlc_t` pairs, not computed; see `tables.c.md` §7.

From the worked example above (nC=0, total=4 → row index 3, trailing=2): the code word is the entry at `[0][3][2]`, which is 8 bits (`00000101` in the standard table). A clean implementation that uses a canonical Table 9-5 listing will produce this exact value.

### 5.4 Level coding and adaptive suffix length

After T1 signs, every remaining non-zero level is coded with `level_token`. The initial suffix length is:

```
suffix_length = (total > 10 && trailing < 3) ? 1 : 0
```

For each level (in reverse scan order), the level code is formed from magnitude and sign. Let `mask = level >> 31` (arithmetic shift: 0 for ≥0, −1 for <0), `abs = (level ^ mask) − mask` (absolute value), then

```
levelCode = (abs != 0) ? 2*abs − mask − 2 : 0
```

so positive level L maps to `2L−2` and negative level −L maps to `2L−1`; level ±1 never reaches this path (it was a T1). With current suffix length S:

- **Normal case**, `(levelCode >> S) < 14`: prefix length is `levelCode >> S`, total code length `= prefix + 1 + S`, and the code bits are `(1 << S) + (levelCode & ((1<<S)−1))`. Equivalently: `prefix` zeros, a one, then S suffix bits.
- **Escape case** (High Profile extension): when the prefix would be 15, `levelCode -= 15 << S` (and an extra 15 when S=0). If the remaining code is below `1<<12`, write a 16-bit escape prefix (fifteen 1-bits then a 0) followed by 12 suffix bits. If it is ≥ `1<<12`, High Profile (profile idc ≥ 100) extends the prefix: repeatedly subtract `1 << (level_prefix − 3)` and increment `level_prefix` while the remainder is large; low profiles instead flag an overflow.

The suffix length adapts after each level using `next_suffix[7] = {0,3,6,12,24,48,0xffff}` and the rules: if S==0 increment to 1; then if `abs > next_suffix[S]` increment once more (capped). This is the standard's suffix-length update rule (a level magnitude crossing 3, 6, 12, 24 or 48 widens the suffix). The generated table stores the *next* suffix length in each entry's `i_next` field, so the writer does not recompute the threshold sequence at run time.

Worked example (continuing §5.1): levels +2 and +3 remain after the two T1s, with S initially 0.
- Level +2: `levelCode = 2*2−2 = 2`; `2>>0 = 2 < 14` → prefix 2, length 3, code `001`. `i_next` for S=0 becomes 1 (since 2 is not > 3), so S ← 1.
- Level +3: `levelCode = 2*3−2 = 4`; `4>>1 = 2 < 14` → prefix 2, length 4, code `0010` (prefix `00`, one, one suffix bit `0`). `i_next` stays 1 (3 is not > 3), S ← 1.

Both codes are looked up from `x264_level_token[S][level + LEVEL_TABLE_SIZE/2]`; magnitudes outside `±LEVEL_TABLE_SIZE/2 = ±64` go through the escape path. `LEVEL_TABLE_SIZE = 128` was chosen empirically (reportedly optimal near QP18 on Nehalem); a larger table helps only at extreme bitrates and costs cache.

### 5.5 `total_zeros` and `run_before`

After the levels, `total_zeros` is written from a context selected by `total`:

- **Luma AC / normal blocks**: `total_zeros[total−1][total_zeros]` when `total < count_cat[cat]`, where `count_cat[14] = {16,15,16,0,15,64,16,15,16,64,16,15,16,64}`. The category `0` entry (DCT_LUMA_DC) means a normal `total_zeros` is never written for luma DC; the DC block uses the 2x2/2x4 form instead (below). A category value below `total` means that total is large enough that `total_zeros` is inferred by the decoder and the syntax element is omitted.
- **Chroma DC**: when `total < 8 >> CHROMA_V_SHIFT` (i.e. fewer than 4 DC coefficients in 4:2:0, fewer than 8 in 4:2:2), the smaller `total_zeros_2x2_dc[total−1][total_zeros]` (4:2:0) or `total_zeros_2x4_dc[total−1][total_zeros]` (4:2:2) table is used. If `total` reaches the full DC count, no `total_zeros` is written.

`run_before` describes, for each non-zero coefficient except the last, the number of zeros before it in scan order. Rather than run a loop per coefficient, the reference precomputes `x264_run_before[mask]` for all 65536 possible 16-bit occupancy masks at encoder open (see §7 and `tables.c.md`). The writer emits it as `bs_write(s, code & 0x1f, code >> 5)`: the low 5 bits are the combined length (at most 25 bits) and the upper bits are the concatenated code. When `total == 1` no run-before values are needed and the table entry is the empty code.

In the worked example, `total_zeros = 4`, `total = 4`, so normal `total_zeros[3][4]` is written, and the run zeros before the three earlier non-zero coefficients are packed via `x264_run_before[mask]`.

### 5.6 Complete syntax-element ordering (normative)

For an I slice, the real writer emits, in this exact order:

| # | Element | Condition / encoding |
|---|---------|----------------------|
| 1 | `mb_field_decoding_flag` | MBAFF only, on the top MB of a pair or when the above MB is skipped; 1 bit |
| 2 | `mb_type` | `ue(offset + 1 + mode16x16 + cbp_chroma*4 + (cbp_luma==0?0:12))` for I_16x16; else `ue(offset)` with offset 0 for I |
| 3 | `transform_8x8_mode_flag` | only when 8x8 transform allowed and I_4x4/I_8x8; 1 bit |
| 4 | intra prediction modes | one per 4x4 (or per 8x8 when 8x8 transform chosen): `1` if mode==predicted, otherwise 4 bits `mode − (mode>pred)` |
| 5 | chroma prediction mode | 4:2:0/4:2:2 only; `ue(fix(mode))` |
| 6 | `mb_qp_delta` | for I_16x16 always; for others only when any CBP bit is set; `se(wrapped(qp−last_qp))` |
| 7 | luma residual | I_16x16: DC block then (if cbp_luma) all 16 AC blocks per plane. I_4x4/I_8x8: per CBP bit, four 4x4 blocks (after 8x8→4x4 interleave when 8x8 transform used) |
| 8 | chroma residual | both DC blocks when cbp_chroma≠0; then AC blocks (4 per 8x8 quadrant) only when cbp_chroma==2 |
| 9 | `rbsp_trailing_bits` | added later by the NAL assembly, not here |

`mb_type` for I_4x4/I_8x8 is `ue(0)` in an I slice, `ue(5)` in a P slice (offset 5), `ue(23)` in a B slice (offset 23). The offsets account for the inter macroblock-type code space that precedes the intra codes in those slice types.

For non-I_16x16 macroblocks, CBP is a **separate** field written right after the macroblock header and before the transform-size flag and QP delta: `ue(cbp_to_golomb[chroma][intra][(cbp_chroma<<4)|cbp_luma])`. For I_16x16, CBP is folded into `mb_type` and no separate CBP is emitted — this is the single most common source of ordering bugs.

### 5.7 Headers by slice type

**I slice** (`offset = 0`): `I_16x16` writes `ue(1 + pred_mode + cbp_chroma*4 + (cbp_luma==0?0:12))`, combining mode and CBP; `I_4x4`/`I_8x8` write `ue(0)`, then a transform-size bit when enabled, then for each 4x4 (or each 8x8 when 8x8 transform selected) a prediction-mode syntax: a single 1 if the predicted mode equals the chosen mode, otherwise a 4-bit value `mode − (mode > pred)`. Chroma prediction mode is written as `ue(fix(mode))` when chroma is present. For intra macroblocks in P and B slices the same tree is used with offsets 5 and 23 respectively.

**P slice**: `P_L0` 16x16 writes a single 1 bit then optional reference index (truncated binary with range `fref[0]−1`, only when more than one reference) and one MVD; 16x8 writes `ue(1)`, two refs and two MVDs; 8x16 writes `ue(2)`, two refs and two MVDs. `P_8x8` writes `ue(4)` when all four sub-block references are zero (no per-sub-block refs), else `ue(3)`; four sub-partition codes from `{3,1,2,0}` (8x8, 8x4, 4x8, 4x4); per-sub-block references when needed; and per-sub-block MVDs with block dimensions chosen by sub-partition shape. When sub-8x8 analysis is disabled the four sub-partitions are written as the fixed 4-bit value `1111`.

**B slice**: `B_8x8` writes `ue(22)`, four sub-partition codes from the 13-entry table, references per active list and sub-partition (gated by `x264_mb_partition_listX_table`), then MVDs per active list. The nine inter-B types (L0_L0 … BI_BI) use a 3x9 table indexed by partition shape (16x8, 8x16, 16x16) and type; `−1` marks impossible combinations. A per-type list table gates which reference indices and MVDs are written. `B_DIRECT` writes a single 1 bit. Intra macroblocks in B slices use offset 23.

### 5.8 QP delta, MVD, CBP and macroblock assembly

`cavlc_qp_delta` computes `qp − last_qp` and suppresses it for an empty I_16x16 (no luma/chroma CBP and no DC coefficients) **when the new QP would be higher**, restoring the previous QP in the real writer. Nonzero deltas are wrapped into `[−((QP_MAX_SPEC+1)/2), QP_MAX_SPEC/2]` by adding or subtracting `QP_MAX_SPEC+1`, then written as signed Exp-Golomb (`se`). Worked example: last_qp=50, new qp=2, 8-bit QP_MAX_SPEC=51 → raw dqp=−48; since −48 < −26, add 52 → dqp=4 → `se(4)`. Decoder adds 4 to 50 → 54, wraps to 2. Reverse wrap: last_qp=2, new qp=50 → dqp=48 > 25 → subtract 52 → −4 → `se(−4)`.

MVDs are the signed Exp-Golomb differences between the cached MV and the predictor from `x264_mb_predict_mv`, one per component (x then y); the 8x8 helper emits them per sub-partition with the block shape deciding whether one, two or four MVD pairs are written. `x264_predictor_roundclip` in `common/common.h` supplies the full-pel predictor list for full-pel search.

The macroblock writer assembles: optional MBAFF field-decoding flag (1 bit, written on the top macroblock of a pair or when the above macroblock is skipped); I_PCM writes `ue(offset + 25)` with offsets `{5,23,0}` for P/B/I slices, byte-aligns, writes raw samples at `BIT_DEPTH` for each plane and chroma component, then reinitialises the bit writer (restoring `p_start`); macroblock header for the slice type; CBP as `ue(cbp_to_golomb[...])` for non-I_16x16; optional transform-size flag when allowed and luma CBP nonzero; QP delta; residuals. I_16x16 writes luma DC then (if luma CBP) 16 AC blocks per plane; other coded macroblocks write luma residuals per CBP bit and sub-block; chroma writes both DC blocks and, for `cbp_chroma == 2`, AC blocks in a traversal stepped by `8 << CHROMA_V_SHIFT` (4:2:0 step 16, 4:2:2 step 32).

Statistics: bits before the header (field flag + mb_type + prediction) accumulate into `i_mv_bits`; bits from CBP onward accumulate into `i_tex_bits`. This split is observable only through `x264_frame_stat_t`, not the bitstream.

### 5.9 RD-only mode

When `RDO_SKIP_BS` is set, the bit writer counts bits instead of emitting bytes; CBP, chroma DC and reference indices are omitted because they do not vary among the candidates being priced; sub-partitions are written only for P_8x8 to support sub-8x8 decisions; I_PCM is not supported in this mode. Per-partition helpers reset the counter, write the varying elements, and return the count. Chroma-format handling: 4:4:4 uses three planes with no separate chroma pass (`plane_count = 3`, `chroma = false`); 4:2:0/4:2:2 use one luma plane plus chroma (`plane_count = 1`); monochrome uses one plane only. `cavlc_intra4x4_pred_size` returns 1 or 4 bits depending on whether the mode equals its predictor — the only intra-pricing primitive needed. Overflow in RD mode adds a fixed 2000-bit penalty rather than setting `b_overflow`, steering the decision away from an unrepresentable level without triggering a re-encode.

### 5.10 8x8-transform interleaving

For 8x8 transform blocks, CAVLC requires 4x4 sub-blocks: `interleave_8x8_cavlc` (a `zigzagf` function pointer) shuffles one 8x8 coefficient array into four 4x4 arrays and updates the non-zero counts for each quadrant before writing 4x4 residuals. Without this step, CAVLC would write coefficients in an order the decoder cannot reconstruct. It is invoked only when `b_transform_8x8` is set and the quadrant's non-zero count is nonzero.

### 5.11 Per-element bit-layout walkthrough (condensed)

**Macroblock type in an I slice.** `I_16x16` is `ue(offset + 1 + mode + cbp_chroma*4 + (cbp_luma==0 ? 0 : 12))` with offset 0; this single code carries both the 16x16 prediction mode and the CBP, so no separate CBP field follows. `I_4x4`/`I_8x8` write `ue(offset)` (0 for I, 5 in P, 23 in B), then an optional 8x8-transform bit, then one entry per 4x4 (or 8x8) block: a single 1 when the mode equals the predictor, else a 4-bit value `mode − (mode > pred)`. Chroma prediction mode is `ue(fix(mode))` for 4:2:0/4:2:2 only.

**P macroblock types.** 16x16 is a single 1 bit plus one reference index (truncated binary over `fref[0]−1`) and one MVD; 16x8 is `ue(1)`, 8x16 is `ue(2)`, each with two refs/MVDs. P_8x8 is `ue(4)` when all four sub-block references are zero, else `ue(3)`; sub-partitions use `{3,1,2,0}`. MVDs are signed Exp-Golomb pairs of `MV − predictor`.

**B macroblock types.** B_8x8 is `ue(22)` followed by four sub-partition codes from the 13-entry table `{10,4,5,1,11,6,7,2,12,8,9,3,0}`, refs and MVDs per active list. The nine inter-B types use the 3x9 table below, with `−1` meaning invalid. B_DIRECT is a single 1 bit.

| Partition | Code column for types 0…8 |
|-----------|---------------------------|
| 16x8 | 4, 8, 12, 10, 6, 14, 16, 18, 20 |
| 8x16 | 5, 9, 13, 11, 7, 15, 17, 19, 21 |
| 16x16 | 1, −1, −1, −1, 2, −1, −1, −1, 3 |

**Coded block pattern.** For non-I_16x16 macroblocks, CBP is `ue(cbp_to_golomb[chroma][intra][(cbp_chroma<<4)|cbp_luma])`. The `[2][2][48]` table separates 400 vs 4:2:0-like families and intra vs inter permutations; the index is the 6-bit combination of 4 luma bits and 2 chroma bits. The transform-size flag follows when 8x8 transform is allowed and luma CBP is nonzero.

**QP delta.** `dqp = qp − last_qp` is wrapped into `[−(QP_MAX_SPEC+1)/2, QP_MAX_SPEC/2]` by adding/subtracting `QP_MAX_SPEC+1`, then written as `se`. The empty-I_16x16 suppression (no CBP or DC coefficients and QP would increase) sets dqp to zero and restores the previous QP in the real writer.

**Coefficient syntax order.** `coeff_token` (selected by nC, total−1, trailing) is written first, followed by trailing signs, then levels for the non-trailing coefficients (adaptive suffix length with escape), then `total_zeros`, then `run_before` values. Chroma DC uses a 2x2 (4:2:0) or 2x4 (4:2:2) total-zeros table. A zero non-zero-count writes only `coeff0_token[nC]`.

---

## 6. Dependencies

Calls `bs_write*`/`bs_align_0`/`bs_init` from `common/bitstream.h`, prediction helpers from `macroblock.c`, coefficient-level/run and interleave functions in `h->quantf`/`h->zigzagf`, and the VLC tables from `common/tables.h` plus the generated `x264_level_token`/`x264_run_before` initialised by `x264_cavlc_init` in `common/vlc.c`. Called from `macroblock.c` and `rdo.c`; `x264_cavlc_block_residual` is also used by the RD helpers. Overflow handling interacts with the macroblock re-encode path in `analyse.c`/`encoder.c` via `h->mb.b_overflow`. `QP_MAX_SPEC`, `PROFILE_HIGH`, `CHROMA_*` and the scan macros come from `common/common.h`.

---

## 7. Constants and tables

- `cbp_to_golomb[2][2][48]` — two chroma-format families (400 and 4:2:0-like) times intra/inter, mapping the combined CBP index `(cbp_chroma<<4)|cbp_luma` to an Exp-Golomb code number. Values are standard CBP mappings; they are *permutations*, not sorted. A clean implementation must copy the mapping from the standard (Table 9-10 and the intra CBP semantics), not assume a bit pattern.
- `mb_type_b_to_golomb[3][9]` — table above; `−1` means invalid.
- `subpartition_p_to_golomb = {3,1,2,0}` (8x8, 8x4, 4x8, 4x4); `subpartition_b_to_golomb[13] = {10,4,5,1,11,6,7,2,12,8,9,3,0}`.
- `next_suffix[7] = {0,3,6,12,24,48,0xffff}`; escape prefix base 15; escape suffix width 12 bits (`1<<12` threshold); High-Profile prefix extension subtracts `1<<(prefix−3)` per step; RD overflow penalty 2000 bits.
- `ctz_index[8] = {3,0,1,0,2,0,1,0}`; `ct_index[17] = {0,0,1,1,2,2,2,2,3,3,3,3,3,3,3,3,3}`; `count_cat[14] = {16,15,16,0,15,64,16,15,16,64,16,15,16,64}`; `i_offsets[3] = {5,23,0}` for PCM.
- `LEVEL_TABLE_SIZE = 128` — level-token lookup width; levels in `[−64,63]` index it directly, others escape.
- `coeff0_token[6]`, `coeff_token[6][16][4]`, `total_zeros[15][16]`, `total_zeros_2x2_dc[3][4]`, `total_zeros_2x4_dc[7][8]`, `run_before_init[7][16]` — literal standard codes in `tables.c`; `level_token[7][128]` and `run_before[65536]` — generated at open in `vlc.c`. **Derivation**: `level_token` is built by the same prefix/suffix equations as §5.4 for every suffix length 0..6 and every level in the table, with `i_next` computed from the magnitude thresholds; `run_before[mask]` is built by iterating all 16-bit masks, extracting the run sequence through `coeff_level_run`, concatenating the `run_before_init` codes and packing `(bits<<5)|length`. This derivation is reproducible from the standard tables; no source text is needed.

Design rationale for the two generated tables: `level_token` avoids per-level arithmetic in the hot loop (one lookup returns code, length and next suffix), while `run_before[mask]` collapses an entire variable-length loop into one lookup keyed by a 16-bit mask. Both trade a one-time O(65536) setup for a constant-time inner loop.

---

## 8. Performance notes

CAVLC is used mainly for Baseline profile and for fast presets; it is a hot path but less than CABAC in high-quality encodes.

Opportunities, with impact:
- **Table-driven coding**: coeff-token, level, total-zero and run-before tables make residual coding mostly lookups; keeping them compact and cache-resident is the main speed lever. `coeff_token` is ~768 bytes (6·16·4·2) and fits in L1; `level_token` is 7·128·4 ≈ 3.5 KiB and is the largest hot table.
- **Branchless trailing-zero detection**: the `ctz_index` trick avoids per-level branches; a clean implementation may use a simple loop — identical output, modest slowdown.
- **SIMD coefficient scanning**: `coeff_level_run` and `interleave_8x8_cavlc` are hot and may be accelerated with SIMD; must remain bit-exact. Public references: Intel *Optimization Reference Manual* on SIMD scan/compaction; Drepper (2007) on cache-resident lookup tables.
- **RD approximate mode**: omitting CBP/chroma-DC/reference terms and using a counting bit writer is an intentional speed/accuracy trade; it affects only decisions, not emitted bytes.
- **Overflow avoidance**: the escape path and the overflow flag trigger a macroblock re-encode at coarser QP; a clean implementation must preserve that fallback to avoid invalid streams at low profiles.
- **Prefetch**: the reference prefetches `x264_run_before[mask]` immediately after extracting the run-level mask, hiding the latency of the large 256 KiB table on the critical path; an independent implementation should do the same or keep a smaller on-demand encoding.

No threading within a frame; bit-writer state is per-frame. Determinism requires the exact VLC table contents and level/run ordering.

---

## 9. References

- ITU-T H.264, Annex B/Table 9-5 (`coeff_token`), 9-6 (`level`), 9-7 (`total_zeros`), 9-8 (`run_before`), 9-9 (chroma DC total zeros), 9-10 (CBP/mb_type mappings); clause 7.4.5 (macroblock layer syntax).
- "Context-Adaptive Variable Length Coding for H.264/AVC" (Luthra, Chen, Zakharov), overview of CAVLC tables.
- Drepper, *What Every Programmer Should Know About Memory* (2007) — lookup-table cache behaviour.
- Intel *Optimization Reference Manual* — SIMD compaction and store alignment.
- x264 public source documentation and `common/tables.h` VLC definitions.

---

## 10. Verification checklist

- Compare emitted CAVLC bytes bit-for-bit with a known-good encoder for fixed macroblock decisions across all slice types and profiles.
- Decode streams with an independent H.264 decoder and verify reconstructed output.
- **Worked vector A** (I_16x16, prediction mode 0, cbp_luma=0, cbp_chroma=0, qp==last_qp): expect `mb_type = ue(1 + 0 + 0*4 + 0) = ue(1)` = `010`; no CBP field; `mb_qp_delta = se(0)` = `1`; luma DC `coeff0_token[5−VS]`, both chroma DC `coeff0_token[5−VS]`; total stream for the macroblock is a few bits. This confirms the CBP-folding and zero-block early exit.
- **Worked vector B** (I_4x4, one block with `level[]=[−1,+1,+2,+3]`, nC=0): expect `coeff_token = 00000101` (8 bits), trailing signs `10`, level codes `001` and `0010`, then `total_zeros[3][4]` and `run_before[mask]`. Confirm suffix length evolves 0→1→1.
- **Worked vector C** (QP wrap): last_qp=50, qp=2 → dqp=4 → codeNum `2*4−1 = 7` → `se(4) = ue(7)` = `0001000` (7 bits); last_qp=2, qp=50 → dqp=−4 → codeNum `2*4 = 8` → `se(−4) = ue(8)` = `0001001` (7 bits). Confirm empty-I_16x16 suppression when qp would increase.
- Exercise residual edge cases: all-zero blocks, exactly three trailing ones (verify `ctz_index` path), suffix-length growth across 3/6/12/24/48, escape coding both with and without High-Profile prefix extension, level magnitudes near `LEVEL_TABLE_SIZE`.
- Verify the `b_overflow` re-encode fallback triggers at low profiles for large levels and does not trigger at High Profile; in RD mode verify the 2000-bit penalty instead.
- Check `nC` derivation for luma DC/AC and chroma DC (4:2:0 vs 4:2:2), including neighbour-unavailable cases.
- Verify QP-delta suppression for empty I_16x16 and modular wrap at QP_MAX_SPEC.
- Confirm 8x8-transform CAVLC interleaving produces the same bitstream as the reference; confirm the `non_zero_count` write-back supplies the correct context to the next block.
- Confirm `i_mv_bits`/`i_tex_bits` split matches the documented boundary (header through CBP boundary vs texture onward).

---

## 11. Open questions

- The exact derivation of `x264_mb_predict_non_zero_code` is defined in `macroblock.c`; only its quantisation via `ct_index` is described here.
- Precise contents of the VLC tables are standard-defined and intentionally not reproduced; a clean implementation must source them from the H.264 specification (Tables 9-5…9-9) or an independently derived generator.
- The behaviour of RD mode for I_PCM is UNKNOWN/unsupported in the reference; it is not exercised.
- Whether the 2000-bit RD overflow penalty and the `LEVEL_TABLE_SIZE = 128` choice are tuned for specific presets is UNKNOWN; both are documented as empirical in the reference.
- The exact bit cost of an escape-coded level with High-Profile prefix extension beyond 12 bits is UNKNOWN without consulting the profile-specific syntax; it is not exercised by ordinary content.
