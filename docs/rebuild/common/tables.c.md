# `common/tables.c` - fixed lookup table definitions

## 1. Header

Source path: `E:\_x264_dirty_stash\x264-source\common\tables.c` (mirrored at `common/tables.c`). Role in one sentence: defines every constant table used by the encoder — level limits, approximate log/exp, lambda curves, scaling matrices, decimation and trellis weights, CABAC state machinery and RDO context offsets, and static CAVLC codes. Marker: **dirty-side behavioural specification**.

Scope note: this document describes *what the tables mean and how their values are derived*. The literal integer contents (standard-defined code words, scaling lists, context pairs, level limits) are facts that a clean implementation must obtain from the H.264 specification or an independently written generator; they are intentionally not reproduced here. The declarations are catalogued in `tables.h.md`.

---

## 2. Purpose

Non-linear maths and standard-defined code tables are expensive to compute at run time. This file materialises them once as read-only data. Their values come from the H.264 standard, from analytical approximations tuned for x264's RD formulas, or from empirically chosen fixed-point representations. A clean implementation must reproduce the *behaviour* of these tables: identical values where they encode standard syntax, and equivalent decisions where they encode heuristics.

For the I-slice path specifically, three table families are load-bearing:

1. **CAVLC static codes** (`coeff0_token`, `coeff_token`, `total_zeros`, `run_before_init`) — these are the entropy codes written by `encoder/cavlc.c`; any difference changes the bitstream.
2. **Generated CAVLC tables** (`level_token`, `run_before`) — built once from the standard tables plus the derivation rules in §5.7–5.8; their contents are deterministic functions of the standard tables.
3. **Quant/RD tables** (lambda, lambda2, chroma offset, DCT weights) — these decide which quantisation and prediction mode wins; equivalent *decisions* matter more than exact intermediate values, but using the published formulas gives identical results.

---

## 3. Public interface

All symbols are declared in `tables.h.md`. No functions are defined here. The only non-const object is `x264_zero[1024]`, declared with 64-byte alignment.

Some tables are consumed through inline wrappers in `base.h` (`x264_log2`, `x264_exp2fix8`) rather than accessed directly; others are read directly by `set.c`, `vlc.c`, `quant.c`, `cabac.c`, rate control and analysis. The `x264_levels[]` array is the only `X264_API`-exported table.

---

## 4. State and data structures

Tables are grouped by purpose with banner comments: levels, math, analyse, MC, CQM, quant, DCT, CABAC, RDO, VLC, plus the trailing zero buffer. All are static-storage, read-only (except the zero buffer) and shared across encoder instances and threads.

| Family | Consumer | Key property |
| --- | --- | --- |
| exp2/log2 | `base.h` inline helpers, rate control | approximate transcendental functions with 8-bit or float precision, no libm call in hot paths |
| lambda / lambda2 | mode decision, RD cost, AQ | monotone in QP, saturated to prevent 32-bit overflow at high QP |
| trellis lambda2 | trellis quantisation | separate inter and intra scaling factors, 0.85 and 0.65 squared |
| chroma offset | chroma QP/lambda handling | powers of two scaled by 16, saturating at 65535 |
| hpel ref | motion compensation | maps a 4-bit interpolation phase to integer-pel neighbours |
| CQM | `set.c` | standard-defined scaling lists plus x264's flat default |
| decimate | DCT decimation | position-dependent count of discardable high-frequency coefficients |
| DCT weights | trellis RD | fixed-point inverses of forward-transform scaling, in zigzag order |
| CABAC state | CABAC encoder | exact H.264 state machine and initialisation pairs |
| CABAC entropy | CABAC RD estimation | `-ln2(p)` in fixed point, so bit costs are additive integers |
| RDO offsets | CABAC RD coefficient coding | context-index bases and increments for significant/last/level flags |
| CAVLC codes | CAVLC writer/analyser | prefix code bits and lengths for tokens, zeros and runs |

**Fixed-point conventions.** `FIX8(x)` truncates `x*256 + 0.5`, so all weight and entropy entries are 8-bit fractional. Lambda2 and trellis lambda2 are plain integers used directly in squared-error comparisons; their absolute scale is not meaningful, only their ratios. The chroma lambda offset table is a 16-bit multiplier applied by shifting, so its powers-of-two structure is essential.

**Padding as an API.** Several arrays have trailing unused entries (CABAC context init contexts unused for I slices are `{0,0}`, RDO offset tables have zero padding at 14/15, `significant_coeff_flag_offset_8x8` has 64 entries for 63 coefficients). This padding is deliberate: SIMD code can load a full vector without a bounds test. Removing it would not change C behaviour but would break hand-written assembly.

---

## 5. Algorithm / behaviour — groups and derivations

**Level limits.** Each `x264_level_t` record holds, in order: level idc, maximum macroblocks per second, maximum frame size in macroblocks, maximum DPB size in macroblocks, maximum bitrate, maximum CPB size, maximum reference frames, minimum compression ratio, whether 8x8 transforms are disallowed, max negative/positive MV range, B-frame factor, and a flag for whether `MaxDpbMbs` is unbounded (used for level 1b). Entries cover levels 1, 1b (idc 9), 1.1 through 6.2. Values are the standard's Table A-1 limits; they are used by encoder open to clamp frame size, references, DPB and MV range. The 1b row shares the 1.0 limits but has a doubled CPB size and the unbounded flag. The terminating all-zero row must remain last.

**Math.** `x264_exp2_lut[64]` is an 8-bit fractional part of `2^(i/64)` scaled by 256, with mild rounding adjustments; `x264_exp2fix8` reconstructs a QP-to-qscale conversion as `(exp2_lut[index & 63] + 256) << (index >> 6) >> 8`. `x264_log2_lut[128]` holds `log2(1 + i/128)` for i = 0..127 in float; `x264_log2_lz_lut[32]` holds `31 − lz` so the log approximator avoids an int-to-float conversion. Together they implement log2 via leading-zero count plus a 7-bit mantissa lookup.

Worked example: for `index = 65`, `index & 63 = 1` and `index >> 6 = 1`. `exp2_lut[1]` is approximately 256·(2^(1/64)−1) ≈ 5.4, rounded to 5. Result = `(5 + 256) << 1 >> 8 = 522 >> 8 = 2` (integer). For `index = 64`, `exp2_lut[0] = 0` → `(0+256)<<1>>8 = 2`. For `index = 0` → 1. The function is a monotone step function with 64 distinct residues per octave.

**Lambda curves.** `x264_lambda_tab[QP]` follows `2^(QP/6 − 2)` rounded to integer and clamped to at least 1. `x264_lambda2_tab[QP]` follows `0.9 * lambda^2 * 256`, capped at `134217727` to avoid 32-bit overflow. `x264_trellis_lambda2_tab[inter/intra][QP]` uses `0.85^2 * 2^(QP/3 + 10 − LAMBDA_BITS)` for inter and `0.65^2` for intra, also capped. `x264_chroma_lambda2_offset_tab[offset]` follows powers of two scaled by 16 (`2^(offset/3)*16` approximately) and saturates at 65535; index 0 is 16. These drive RD decisions in mode selection and trellis quantisation.

Worked examples: `lambda_tab[0] = round(2^(−2)) = round(0.25) = 0 → clamped to 1`; `lambda_tab[12] = round(2^0) = 1`; `lambda_tab[18] = round(2^1) = 2`; `lambda_tab[51] = round(2^(51/6−2)) = round(2^6.5) = round(90.5) = 91`; `lambda_tab[57] = round(2^7.5) = 181`. `lambda2` at QP 51 = `0.9 · 91² · 256 ≈ 1,908,000`, below the cap; at QP 81 the raw value (`2^(81/6−2) = 2^11.5 ≈ 2896`, squared and scaled) would exceed the cap and is clamped to 134,217,727.

**Half-pel reference selection.** Two 16-byte tables map a 4-bit half-pel interpolation position to the two integer-pel reference indices used by the filter. The entries are small integers 0..3, arranged so the horizontal and vertical cases share the same lookup form.

**CQM lists.** The JVT defaults are the standard flat-ish 4x4 intra/inter lists and 8x8 intra/inter lists, chosen so that the flat matrix is all 16 and JVT uses small integers derived from JPEG-like perceptual weighting. `x264_cqm_jvt[8]` groups them by list role. The AVCIntra presets are resolution/profile-specific 16- or 64-entry lists (50/100/300 class, 4ic/4iy/8iy variants) used for AVC-Intra compliance; their exact integers are standard-defined and are not reproduced here.

**Decimation tables.** `x264_decimate_table4[i]` and `x264_decimate_table8[i]` map a coefficient index in zigzag order to the number of high-frequency positions that may be zeroed for DCT decimation: 3,2,2,1,1,1,0... for 4x4 and a slower staircase for 8x8. They are heuristic cutoffs, not standard-defined.

**DCT weights.** `x264_dct8_weight_tab` and `x264_dct4_weight_tab` contain 1/256 fixed-point inverses of the scaling factors introduced by the forward DCT, arranged in the 8x8 zigzag pattern built from six distinct values for 8x8 and three for 4x4. The `weight2` variants are the inverse-squared factors used by trellis, also in 1/256 fixed point but stored as 32-bit words so SIMD trellis can consume them directly.

**CABAC state machinery.** `x264_cabac_context_init_I[1024][2]` and `x264_cabac_context_init_PB[3][1024][2]` are the standard's context initialisation pairs `(m, n)` indexed by context index and `cabac_init_idc`; entries for contexts unused in I slices are zero-filled. The conversion from a pair to an initial state is performed by the CABAC module, not here. `x264_cabac_range_lps[64][4]` gives the new range after an LPS for each of 64 states and four `rangeLPS` variants; `x264_cabac_transition[128][2]` maps state and symbol to the next state; `x264_cabac_renorm_shift[64]` gives the number of left shifts needed per state after renormalisation (6,5,4,4,3... down to 0 in power-of-two groups). `x264_cabac_entropy[128]` stores `−ln2(probability)` in 1/256 fixed point; the two columns per state correspond to the MPS and LPS probabilities and are used by RD-based CABAC cost estimation.

The state index ranges 0..63; the transition table doubles it to 0..127 with the second column for the LPS outcome. Renormalisation shifts are grouped by powers of two: state 0 requires six shifts, states 1–2 five, states 3–6 four, states 7–14 three, and so on. `range_lps` values grow monotonically with the state index and are clamped to 128 in the highest states. The entropy table stores two values per state (MPS and LPS probability costs) so RD decision making can add a cost without calling a logarithm.

**RDO context offsets.** `x264_significant_coeff_flag_offset_8x8[2][64]` and `x264_last_coeff_flag_offset_8x8[63]` map coefficient scan positions to CABAC context-index offsets for 8x8 blocks; the first is padded to 64 entries and has two variants (luma/chroma or scan type), the second covers positions 0..62. `x264_coeff_flag_offset_chroma_422_dc[7]` implements `min(i/2, 2)`. The four 16-entry tables `significant_coeff_flag_offset`, `last_coeff_flag_offset` (two variants each) encode the base context offsets for 4x4 blocks as sums of standard constants plus per-position increments; entries 14 and 15 are zero padding. `x264_coeff_abs_level_m1_offset[16]` similarly holds base offsets for level coding, with padding beyond index 13. `x264_count_cat_m1[14]` holds the category masks used to build the coeff-abs-level-minus-1 context from neighbouring count states.

**CAVLC static codes.** `x264_coeff0_token[6]` gives the code for a block with zero total coefficients for each `nC` context (0..5). `x264_coeff_token[6][16][4]` gives `coeff_token` codes indexed by `nC`, total coefficients minus one, and trailing ones (0..3); many entries are shorter forms with trailing ones 2 or 3 collapsed. `x264_total_zeros[15][16]`, `x264_total_zeros_2x2_dc[3][4]` and `x264_total_zeros_2x4_dc[7][8]` cover the total-zeros codes for normal blocks, 2x2 DC and 2x4 DC blocks respectively. `x264_run_before_init[7][16]` holds zero-run codes indexed by total-zeros context and run length; it is the seed for `vlc.c`'s generated table. All are `vlc_t` pairs of code bits and length. **UNKNOWN:** the exact code-bit values are H.264 Table 9-1/9-5/9-6 facts and are intentionally not reproduced here.

**CAVLC table indexing.** `coeff_token` is indexed by `nC` (number of nonzero neighbours, clipped to 0..5), then total coefficients minus one, then trailing ones 0..3. Entries for more trailing ones than exist are still populated so the index arithmetic needs no clipping. `total_zeros` variants are indexed by total coefficients minus one and total zeros; the 2x2 and 2x4 DC variants have smaller dimensions because those blocks have fewer coefficients. `run_before_init` is indexed by a clipped total-zeros context (1..7) and run length (0..15), and is consumed by the generator in `vlc.c`.

**Zero buffer.** `x264_zero[1024]` is a 64-byte-aligned writable block of zeros, used as a source for SIMD zero fills and as a safe overread region in some kernels.

### 5.1 Level-record layout

Each level record packs limits that encoder open uses to cap: macroblocks per second, frame size, decoded-picture buffer size, bitrate, CPB size, reference count, compression ratio, MV range, an 8x8dct prohibition, a B-frame dependent scaling factor, and a special flag for unbounded DPB (used by level 1b). The 1b row shares the 1.0 limits but has a doubled CPB size and the unbounded flag. The terminating zero row must remain last. The B-frame factor is used to scale `MaxMBPS` checks when B frames are present.

### 5.2 Lambda table decision use

Mode decision compares `RD = distortion + lambda * bits` (or `lambda2 * distortion` in integer form). Because `lambda` grows by a factor of `2^(1/6)` per QP step, `lambda_tab` is a geometric staircase; an implementation can regenerate it from the formula rather than copy it. The saturation of `lambda2` at 134,217,727 keeps the product within 32 bits for high QP; using an unsaturated value would overflow and invert comparisons.

### 5.3 CABAC state derivation (out of scope for I1)

The initial state for a context is computed in `cabac.c` from `(m,n)` as `state = clip( (m*(init_idc? ... ) + n) ... )` following H.264 clause 9.3.1; only the `(m,n)` pairs are stored here. The transition table maps `(state, bin)` to the next state, with bin=0 (MPS) generally lowering the state index and bin=1 (LPS) raising it. `renorm_shift[state]` gives `6 − floor(log2(state+1))` for states 0..63, i.e. the leading-zero count of `state+1` minus 1. I-slice implementation needs these only if CABAC is enabled; CAVLC-only implementations can defer them.

### 5.4 DCT weight derivation

The forward 4x4 and 8x8 integer transforms introduce scaling factors per frequency. To keep RD calculations in sample units, the trellis quantiser multiplies squared coefficients by the inverse scaling in 1/256 fixed point. Six distinct values suffice for the 8x8 case (grouped by row/column parity) and three for 4x4; the `weight2` variants store the squared inverse in 32 bits. A clean implementation can recompute these from the transform matrix, but must ensure the fixed-point rounding matches to keep trellis decisions identical.

### 5.5 CQM preset selection

`x264_cqm_jvt[8]` points to the four 4x4 lists (intra luma, intra chroma, inter luma, inter chroma) and four 8x8 lists. `set.c` selects them by `i_cqm_preset` and copies the chosen pointers into `x264_sps_t.scaling_list[8]`. The flat list is all 16 (4x4) and all 16 (8x8), which yields the standard flat quantisation; JVT lists use small integers derived from perceptual weighting. AVC-Intra presets replace all eight pointers with resolution/profile-specific lists.

### 5.6 Decimate table use

During DCT decimation, a coefficient at zigzag position `i` is zeroed if the current decimation level exceeds `decimate_table4[i]` (or `table8[i]`). The tables are non-increasing and end at zero, so higher zigzag positions (high frequencies) are zeroed first. This is a speed/quality heuristic, not a normative part of H.264.

### 5.7 Generated `level_token` derivation

For each suffix length S in 0..6 and each level L in `[−64, 63]` (index `L + 64` into the 128-entry row), the generator computes:

```
mask      = L >> 15                      (0 for >=0, -1 for <0)
abs       = (L ^ mask) - mask
levelCode = abs ? 2*abs - mask - 2 : 0
```

and then chooses one of four cases:

| Condition | Code length | Code bits |
|-----------|-------------|-----------|
| `(levelCode >> S) < 14` | `(levelCode >> S) + 1 + S` | `(1<<S) + (levelCode & ((1<<S)−1))` |
| `S == 0 && levelCode < 30` | 19 | `(1<<4) + (levelCode − 14)` |
| `S > 0 && (levelCode >> S) == 14` | `15 + S` | `(1<<S) + (levelCode & ((1<<S)−1))` |
| otherwise | 28 | `(1<<12) + (levelCode − (15<<S) − (S==0?15:0))` |

The `i_next` field is computed after the code: start at S; if it is 0 increment to 1; then if `abs > (3 << (i_next−1))` and `i_next < 6`, increment once more. This is the standard suffix-length update rule, so the writer never recomputes thresholds. Worked example: S=0, L=+2 → `levelCode=2`, first case, length 3, bits 1 → code `001`, `i_next=1`. L=−2 → `levelCode=3`, length 4, bits `(1<<0)+(3&0)=1` → `0001`? (prefix three zeros then 1), sign embedded in the odd levelCode. L=+64 exceeds the table range and takes the escape path in the writer, not here.

### 5.7.1 CAVLC table selection decision table

The CAVLC writer selects among several total-zeros tables based on block category and coefficient count. This is a frequent source of ordering/index bugs:

| Block category | Condition | Table used | Index form |
|----------------|-----------|------------|------------|
| Luma DC | any total | none (no `total_zeros` written) | — |
| Luma AC / chroma AC | `total < count_cat[cat]` | `total_zeros[15][16]` | `[total−1][total_zeros]` |
| Luma AC / chroma AC | `total >= count_cat[cat]` | none (inferred by decoder) | — |
| Chroma DC 4:2:0 | `total < 4` | `total_zeros_2x2_dc[3][4]` | `[total−1][total_zeros]` |
| Chroma DC 4:2:2 | `total < 8` | `total_zeros_2x4_dc[7][8]` | `[total−1][total_zeros]` |
| Chroma DC | `total` at full count | none | — |

`count_cat[14] = {16,15,16,0,15,64,16,15,16,64,16,15,16,64}` indexes by block category; a zero (DCT_LUMA_DC) means never write normal total-zeros, and 64 means always write it. Chroma DC uses the separate `8 >> CHROMA_V_SHIFT` threshold, not `count_cat`.

### 5.7.2 `coeff_token` table family

`x264_coeff_token[6][16][4]` stores, for each context category nC = 0..5, the code for (`TotalCoeff−1`, `TotalTrailing`). Categories 0–4 follow H.264 Table 9-5; category 5 is the dedicated chroma-DC category and uses the same table shape. Rows for small `TotalCoeff` have fewer populated trailing entries, but the array is fully filled so the writer can index `[total−1][trailing]` without clipping. Example from category 0: `TotalCoeff=4, trailing=2` → row index 3, code 8 bits (`00000101`). Every code is prefix-free within its nC category; codes from different categories may differ for the same symbol, which is the whole point of context adaptation.

### 5.8 Generated `run_before[mask]` derivation

The table has `1<<16 = 65536` entries, one per 16-bit occupancy mask. Entry 0 and 1 are zero (no runs). For each mask `i >= 2`:

1. Materialise a 16-sample block whose coefficient at position j is `i & (1<<j)` (all coefficients ±1 by occupancy).
2. Run `coeff_level_run[DCT_LUMA_4x4]` to obtain `total` and `last`; compute `zeros = last + 1 − total`.
3. Build a rotated mask `rot = i << (clz(i) + 1)` so the leading run is visible at the top bits.
4. For each of the `total−1` coefficients while `zeros > 0`: index `x264_run_before_init[min(zeros,7)−1][clz(rot)]`, accumulate its bits by shifting left and OR-ing, subtract the run from `zeros`, and advance `rot` past the consumed coefficient.
5. Store `(accumulated_bits << 5) + accumulated_length`.

The 5-bit length field can represent up to 31 bits; the longest possible run-before sequence sums to 25 bits, so both code and length fit in one `uint32_t`. The writer extracts them as `code & 0x1f` (length) and `code >> 5` (bits). This derivation is fully reproducible from `run_before_init` and the standard's run-before table; no source text is needed.

---

## 6. Dependencies

Includes only `base.h` (which supplies `x264.h`, `osdep.h`, math helpers `FIX8` and alignment macros). Consumed by `base.h` inline functions, `set.c`, `vlc.c`, `quant.c`, `cabac.c`, `predict.c`, `mc.c`, rate control and encoder analysis. The generated tables (`level_token`, `run_before`) are initialised at encoder open by `x264_cavlc_init` in `vlc.c`, which calls `coeff_level_run` from the quant module — a one-time dependency from tables to the quant function pointers.

---

## 7. Constants and tables

Key const values: `QP_MAX_MAX = 81`; lambda2 cap `134217727`; chroma offset table max `65535`; CABAC states 0..63 with transition indices 0..127; entropy scale 256; `FIX8` scale 256; level list terminated by `{0}`. Counting: 21 level records plus terminator, 64 exp entries, 128+32 log entries, 82 entries per lambda table, 8 CQM pointers, 11 AVCIntra lists, 1024·2 I contexts and 3·1024·2 P/B contexts, 12·12 kernels' worth of VLC entries. `LEVEL_TABLE_SIZE = 128` (defined in `bitstream.h`, not here) bounds the generated level table; `x264_run_before` has 65536 entries.

### 7.1 Table-size cross-check

| Table | Dimensions | Element type | Total size (approx) |
|-------|-----------|--------------|---------------------|
| `x264_coeff_token` | 6·16·4 | `vlc_t` (2 B) | 768 B |
| `x264_level_token` | 7·128 | `vlc_large_t` (4 B) | 3584 B |
| `x264_run_before` | 65536 | `uint32_t` | 256 KiB |
| `x264_cabac_context_init_I` | 1024·2 | `int8_t` | 2 KiB |
| `x264_cabac_context_init_PB` | 3·1024·2 | `int8_t` | 6 KiB |
| `x264_cabac_entropy` | 128 | `uint16_t` | 256 B |

The 256 KiB `run_before` table is by far the largest; it is generated once and shared, and the CAVLC writer prefetches the specific entry it needs. A clean implementation may replace it with an on-the-fly run loop (slower but identical output) if memory is constrained.

---

## 8. Performance notes

- **Cache layout:** the hot tables (log/exp, lambda, CABAC entropy/transition, hpel ref) are tens to hundreds of bytes and stay in L1; the CABAC context-init arrays are 2 KiB and 6 KiB respectively, touched only at slice start. The 256 KiB `run_before` table is L2-resident; prefetching its entry hides latency on the CAVLC critical path.
- **False sharing / allocation:** all read-only; no runtime cost beyond cache misses. `x264_zero` is writable and must not be shared for conflicting uses.
- **Dispatch hierarchy:** the same tables are consumed by C and SIMD paths, guaranteeing identical bitstreams across architectures. Keep fixed-point widths exactly (8, 16, 32 bits) because SIMD loads assume them.
- **Prefetch:** table lookups are random by nature; do not attempt software prefetch inside CABAC inner loops — the tables are small enough that hardware handles them.
- Public references: ITU-T H.264 tables for CAVLC/CABAC/levels; Drepper (2007) on cache footprint; x264 `doc/ratecontrol.txt` for lambda usage.
- **Generation cost:** `x264_cavlc_init` iterates 7·128 levels and all 65536 masks once at open; this is a few milliseconds, amortised over the whole encode. An implementation that generates lazily must still finish before the first CAVLC macroblock is written.
- **Thread safety:** once generated, all tables are read-only and share cleanly across encoder threads; only the one-time generation must be serialised or performed before threads start.

---

## 9. References

- ITU-T Rec. H.264 / ISO/IEC 14496-10: Annex A Table A-1 (levels), clause 9.2.2 (CAVLC tables), clause 9.3.4 (CABAC context initialisation, rangeLPS, state transition), clause 8.5 (quantisation/scaling), Annex E (CQM).
- AVC-Intra (SMPTE RP 2021 / ISO IFF) scaling-list definitions for the AVCIntra presets.
- Drepper, *What Every Programmer Should Know About Memory* (2007).
- x264 `doc/ratecontrol.txt`, `doc/standards.txt`.
- Intel *Optimization Reference Manual* — fixed-point SIMD loads and table-lookup alignment.

---

## 10. Verification checklist

- Verify numerically that `x264_lambda_tab[q]` is the rounded value of `2^(q/6−2)` for q = 0..81, allowing the documented minimum of 1.
- Verify `x264_lambda2_tab[q]` respects the `0.9*lambda^2*256` relation until the saturation point and never exceeds the cap.
- Verify `x264_chroma_lambda2_offset_tab[0] == 16`, monotonicity, and saturation at 65535.
- Verify all CABAC ranges in `range_lps` are within 1..128 and that `renorm_shift` is non-increasing (6,5,4,4,3,...).
- Verify that every `vlc_t` entry's `i_bits` fits within `i_size` bits and that no code is longer than 28 bits in the level-related tables.
- Verify the level list is strictly ordered by idc except the 1b entry and ends with an all-zero record.
- Verify `x264_zero` alignment is 64 bytes and all bytes are zero at program start.
- End-to-end: encode identical content with CPU features toggled via `asm=`; bitstreams must match, confirming the SIMD and C paths consume the same tables.

Additional black-box checks:

- Verify the §5 worked exp2 example: `x264_exp2fix8(0)==1`, `(64)==2`, `(65)==2`, and monotonicity over 0..255.
- Verify the §5 lambda examples: `lambda_tab[0]==1`, `[12]==1`, `[18]==2`, `[51]==91`, `[57]==181` (allowing a one-unit rounding difference if the implementation uses a different round-half rule).
- Verify the §5.7 derivation directly: generate `level_token[0][+2+64]` and assert length 3, bits 1, `i_next=1`; generate `level_token[0][−2+64]` and confirm the odd levelCode encodes the sign.
- Verify the §5.8 derivation: for mask `0b...0101` (two non-zero coefficients), confirm `run_before[mask]` decodes to the concatenation of the two run codes and that entry 0 and 1 are zero.
- Verify the §7.1 size cross-check with `sizeof` on each table in a debug build; confirm the `vlc_t` element is 2 bytes and `vlc_large_t` is 4 bytes.
- Verify the generated tables are identical across bit depths (they are bit-depth independent) by hashing them in 8-bit and 10-bit builds.
- Verify all static VLC codes are prefix-free within each table family (no code is the prefix of another at the same index space), as required by H.264.
- Verify every RDO offset table's padding entries are zero and that no offset exceeds the CABAC context count minus one after adding its base.
- Verify the §5.7.1 decision table with constructed blocks: a luma DC block writes no `total_zeros`; a chroma-DC 4:2:0 block with `total=3` uses the 2x2 table; a luma AC block with `total >= count_cat[cat]` writes none.
- Verify the §5.7.2 example: `x264_coeff_token[0][3][2]` is 8 bits with bits `0x05`, confirming the standard Table 9-5 value for TotalCoeff=4, trailing=2.
- Verify the decimation tables are non-increasing, that `x264_decimate_table4[15] == 0` and `x264_decimate_table8[63] == 0`.
- Verify the DCT weight entries are positive fixed-point values and that their squared variants equal the square divided by 256 within one unit.

---

## 11. Open questions

- UNKNOWN: exact integer contents of the CAVLC code tables, CQM JVT/AVCI lists and CABAC context pairs are not reproduced here; they are fixed standard facts to be taken from the H.264 specification or an independently derived table generator.
- UNKNOWN: whether any consumer writes to `x264_zero`; declared non-const but no writer is visible in these files.
- UNKNOWN: the precise rounding rule used to generate the exp2 lookup's small deviations from a pure `round(256*2^(i/64))`; independent implementations may differ by one unit without changing visible decisions in most cases.
- UNKNOWN: the exact constant `LAMBDA_BITS` used in the trellis lambda2 formula; it is a local tuning constant and its numeric value is not visible from the declaration.
- UNKNOWN: whether the empirical decimate-table thresholds are tuned per preset or are the same for all presets.
- UNKNOWN: the exact number of AVCIntra preset lists and their resolution/profile mapping beyond the count stated here; they are selected by profile flags in `set.c` and are not standard CAVLC facts.

This file contains no executable logic; every behaviour described here is a *property of the data*. An independent implementation is free to generate the values at run time instead of storing them, provided the generated values and the fixed-point widths match those documented here.
