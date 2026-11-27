# `common/tables.c` - fixed lookup table definitions

## 1. Header

Source path: `E:\_x264_dirty_stash\x264-source\common\tables.c` (mirrored at `common/tables.c`). Role in one sentence: defines every constant table used by the encoder - level limits, approximate log/exp, lambda curves, scaling matrices, decimation and trellis weights, CABAC state machinery and RDO context offsets, and static CAVLC codes. Marker: **dirty-side behavioural specification**.

## 2. Purpose

Non-linear maths and standard-defined code tables are expensive to compute at run time. This file materialises them once as read-only data. Their values come from the H.264 standard, from analytical approximations tuned for x264's RD formulas, or from empirically chosen fixed point representations. A clean implementation must reproduce the *behaviour* of these tables: identical values where they encode standard syntax, and equivalent decisions where they encode heuristics.

## 3. Public interface

All symbols are declared in `tables.h.md`. No functions are defined here. The only non-const object is `x264_zero[1024]`, declared with 64-byte alignment.

## 4. State and data structures

Tables are grouped by purpose with banner comments: levels, math, analyse, MC, CQM, quant, DCT, CABAC, RDO, VLC, plus the trailing zero buffer. All are static-storage, read-only (except the zero buffer) and shared across encoder instances and threads.

## 5. Algorithm / behaviour - groups and derivations

**Level limits.** Each `x264_level_t` record holds, in order: level idc, maximum macroblocks per second, maximum frame size in macroblocks, maximum DPB size in macroblocks, maximum bitrate, maximum CPB size, maximum reference frames, minimum compression ratio, whether 8x8 transforms are disallowed, max negative/positive MV range, B-frame factor, and a flag for whether `MaxDpbMbs` is unbounded (used for level 1b). Entries cover levels 1, 1b (idc 9), 1.1 through 6.2. Values are the standard's `Table A-1` limits; they are used by encoder open to clamp frame size, references, DPB and MV range.

**Math.** `x264_exp2_lut[64]` is an 8-bit fractional part of `2^(i/64)` scaled by 256, with mild rounding adjustments; `x264_exp2fix8` reconstructs a QP-to-qscale conversion as `(exp2_lut[index & 63] + 256) << (index >> 6) >> 8`. `x264_log2_lut[128]` holds `log2(1 + i/128)` for i = 0..127 in float; `x264_log2_lz_lut[32]` holds `31 - lz` so the log approximator avoids an int-to-float conversion. Together they implement log2 via leading-zero count plus a 7-bit mantissa lookup.

**Lambda curves.** `x264_lambda_tab[QP]` follows `2^(QP/6 - 2)` rounded to integer and clamped to at least 1. `x264_lambda2_tab[QP]` follows `0.9 * lambda^2 * 256`, capped at `134217727` to avoid 32-bit overflow. `x264_trellis_lambda2_tab[inter/intra][QP]` uses `0.85^2 * 2^(QP/3 + 10 - LAMBDA_BITS)` for inter and `0.65^2` for intra, also capped. `x264_chroma_lambda2_offset_tab[offset]` follows powers of two scaled by 16 (`2^(offset/3)*16` approximately) and saturates at 65535; index 0 is 16. These drive RD decisions in mode selection and trellis quantisation.

**Half-pel reference selection.** Two 16-byte tables map a 4-bit half-pel interpolation position to the two integer-pel reference indices used by the filter. The entries are small integers 0..3, arranged so the horizontal and vertical cases share the same lookup form.

**CQM lists.** The JVT defaults are the standard flat-ish 4x4 intra/inter lists and 8x8 intra/inter lists, chosen so that the flat matrix is all 16 and JVT uses small integers derived from JPEG-like perceptual weighting. `x264_cqm_jvt[8]` groups them by list role. The AVCIntra presets are resolution/profile-specific 16- or 64-entry lists (50/100/300 class, 4ic/4iy/8iy variants) used for AVC-Intra compliance; their exact integers are standard-defined and are not reproduced here.

**Decimation tables.** `x264_decimate_table4[i]` and `x264_decimate_table8[i]` map a coefficient index in zigzag order to the number of high-frequency positions that may be zeroed for DCT decimation: 3,2,2,1,1,1,0... for 4x4 and a slower staircase for 8x8. They are heuristic cutoffs, not standard-defined.

**DCT weights.** `x264_dct8_weight_tab` and `x264_dct4_weight_tab` contain 1/256 fixed-point inverses of the scaling factors introduced by the forward DCT, arranged in the 8x8 zigzag pattern built from six distinct values for 8x8 and three for 4x4. The `weight2` variants are the inverse-squared factors used by trellis, also in 1/256 fixed point but stored as 32-bit words so SIMD trellis can consume them directly.

**CABAC state machinery.** `x264_cabac_context_init_I[1024][2]` and `x264_cabac_context_init_PB[3][1024][2]` are the standard's context initialisation pairs (m, n) indexed by context index and `cabac_init_idc`; entries for contexts unused in I slices are zero-filled. The conversion from a pair to an initial state is performed by the CABAC module, not here. `x264_cabac_range_lps[64][4]` gives the new range after an LPS for each of 64 states and four `rangeLPS` variants; `x264_cabac_transition[128][2]` maps state and symbol to the next state; `x264_cabac_renorm_shift[64]` gives the number of left shifts needed per state after renormalisation (6,5,4,4,3... down to 0 in power-of-two groups). `x264_cabac_entropy[128]` stores `-ln2(probability)` in 1/256 fixed point; the two columns per state correspond to the MPS and LPS probabilities and are used by RD-based CABAC cost estimation.

**RDO context offsets.** `x264_significant_coeff_flag_offset_8x8[2][64]` and `x264_last_coeff_flag_offset_8x8[63]` map coefficient scan positions to CABAC context-index offsets for 8x8 blocks; the first is padded to 64 entries and has two variants (luma/chroma or scan type), the second covers positions 0..62. `x264_coeff_flag_offset_chroma_422_dc[7]` implements `min(i/2,2)`. The four 16-entry tables `significant_coeff_flag_offset`, `last_coeff_flag_offset` (two variants each) encode the base context offsets for 4x4 blocks as sums of standard constants plus per-position increments; entries 14 and 15 are zero padding. `x264_coeff_abs_level_m1_offset[16]` similarly holds base offsets for level coding, with padding beyond index 13. `x264_count_cat_m1[14]` holds the category masks used to build the coeff-abs-level-minus-1 context from neighbouring count states.

**CAVLC static codes.** `x264_coeff0_token[6]` gives the code for a block with zero total coefficients for each `nC` context (0..5). `x264_coeff_token[6][16][4]` gives `coeff_token` codes indexed by `nC`, total coefficients minus one, and trailing ones (0..3); many entries are shorter forms with trailing ones 2 or 3 collapsed. `x264_total_zeros[15][16]`, `x264_total_zeros_2x2_dc[3][4]` and `x264_total_zeros_2x4_dc[7][8]` cover the total-zeros codes for normal blocks, 2x2 DC and 2x4 DC blocks respectively. `x264_run_before_init[7][16]` holds zero-run codes indexed by total-zeros context and run length; it is the seed for `vlc.c`'s generated table. All are `vlc_t` pairs of code bits and length. **UNKNOWN:** the exact code-bit values are H.264 Table 9-1/9-5/9-6 facts and are intentionally not reproduced here.

**Zero buffer.** `x264_zero[1024]` is a 64-byte-aligned writable block of zeros, used as a source for SIMD zero fills and as a safe overread region in some kernels.

### Additional behavioural detail

**Table families and where they are consumed.**

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

**CABAC state semantics.** The state index ranges 0..63; the transition table doubles it to 0..127 with the second column for the LPS outcome. Renormalisation shifts are grouped by powers of two: state 0 requires six shifts, states 1-2 five, states 3-6 four, states 7-14 three, and so on. `range_lps` values grow monotonically with the state index and are clamped to 128 in the highest states. The entropy table stores two values per state (MPS and LPS probability costs) so RD decision making can add a cost without calling a logarithm.

**Padding as an API.** Several arrays have trailing unused entries (CABAC context init contexts unused for I slices are `{0,0}`, RDO offset tables have zero padding at 14/15, `significant_coeff_flag_offset_8x8` has 64 entries for 63 coefficients). This padding is deliberate: SIMD code can load a full vector without a bounds test. Removing it would not change C behaviour but would break hand-written assembly.

**Level record layout.** Each level record packs limits that encoder open uses to cap: macroblocks per second, frame size, decoded-picture buffer size, bitrate, CPB size, reference count, compression ratio, MV range, an 8x8dct prohibition, a B-frame dependent scaling factor, and a special flag for unbounded DPB (used by level 1b). The 1b row shares the 1.0 limits but has a doubled CPB size and the unbounded flag. The terminating zero row must remain last.

**CAVLC table indexing.** `coeff_token` is indexed by `nC` (number of nonzero neighbours, clipped to 0..5), then total coefficients minus one, then trailing ones 0..3. Entries for more trailing ones than exist are still populated so the index arithmetic needs no clipping. `total_zeros` variants are indexed by total coefficients minus one and total zeros; the 2x2 and 2x4 DC variants have smaller dimensions because those blocks have fewer coefficients. `run_before_init` is indexed by a clipped total-zeros context (1..7) and run length (0..15), and is consumed by the generator in `vlc.c`.

## 6. Dependencies

Includes only `base.h` (which supplies `x264.h`, `osdep.h`, math helpers `FIX8` and alignment macros). Consumed by `base.h` inline functions, `set.c`, `vlc.c`, `quant.c`, `cabac.c`, `predict.c`, `mc.c`, rate control and encoder analysis.

## 7. Constants and tables

Key const values: `QP_MAX_MAX = 81`; lambda2 cap `134217727`; chroma offset table max `65535`; CABAC states 0..63 with transition indices 0..127; entropy scale 256; `FIX8` scale 256; level list terminated by `{0}`. Counting: 21 level records plus terminator, 64 exp entries, 128+32 log entries, 82 entries per lambda table, 8 CQM pointers, 11 AVCIntra lists, 1024*2 I contexts and 3*1024*2 P/B contexts, 12*12 kernels' worth of VLC entries.

## 8. Performance notes

- **Cache layout:** the hot tables (log/exp, lambda, CABAC entropy/transition, hpel ref) are tens to hundreds of bytes and stay in L1; the CABAC context-init arrays are 4 KiB and 12 KiB respectively, touched only at slice start.
- **False sharing / allocation:** all read-only; no runtime cost beyond cache misses. `x264_zero` is writable and must not be shared for conflicting uses.
- **Dispatch hierarchy:** the same tables are consumed by C and SIMD paths, guaranteeing identical bitstreams across architectures. Keep fixed-point widths exactly (8, 16, 32 bits) because SIMD loads assume them.
- **Prefetch:** table lookups are random by nature; do not attempt software prefetch inside CABAC inner loops - the tables are small enough that hardware handles them.
- Public references: ITU-T H.264 tables for CAVLC/CABAC/levels; Drepper (2007) on cache footprint; x264 `doc/ratecontrol.txt` for lambda usage.

## 9. References

- ITU-T Rec. H.264 / ISO/IEC 14496-10: Annex A Table A-1 (levels), clause 9.2.2 (CAVLC tables), clause 9.3.4 (CABAC context initialisation, rangeLPS, state transition), clause 8.5 (quantisation/scaling), Annex E (CQM).
- AVC-Intra (SMPTE RP 2021 / ISO IFF) scaling-list definitions for the AVCIntra presets.
- Drepper, *What Every Programmer Should Know About Memory* (2007).
- x264 `doc/ratecontrol.txt`, `doc/standards.txt`.

## 10. Verification checklist

- Verify numerically that `x264_lambda_tab[q]` is the rounded value of `2^(q/6-2)` for q = 0..81, allowing the documented minimum of 1.
- Verify `x264_lambda2_tab[q]` respects the `0.9*lambda^2*256` relation until the saturation point and never exceeds the cap.
- Verify `x264_chroma_lambda2_offset_tab[0] == 16`, monotonicity, and saturation at 65535.
- Verify all CABAC ranges in `range_lps` are within 1..128 and that `renorm_shift` is non-increasing (6,5,4,4,3,...).
- Verify that every `vlc_t` entry's `i_bits` fits within `i_size` bits and that no code is longer than 28 bits in the level-related tables.
- Verify the level list is strictly ordered by idc except the 1b entry and ends with an all-zero record.
- Verify `x264_zero` alignment is 64 bytes and all bytes are zero at program start.
- End-to-end: encode identical content with CPU features toggled via `asm=`; bitstreams must match, confirming the SIMD and C paths consume the same tables.

Additional black-box checks:

- Verify the exp2 lookup is non-decreasing and that `x264_exp2fix8` output is a step function with exactly 64 distinct residues per exponent octave.
- Verify `x264_log2_lz_lut[lz] == 31 - lz` for all 32 entries.
- Verify the hpel reference tables contain only values 0..3 and that no row contains two identical values in adjacent positions where interpolation would then read the same sample twice.
- Verify that all CQM JVT lists contain values in 1..255, that the 8x8 lists are 64 entries and the 4x4 lists 16, and that `x264_cqm_jvt` points to exactly those eight arrays.
- Verify the decimate tables are non-increasing, that `x264_decimate_table4[15] == 0` and `x264_decimate_table8[63] == 0`.
- Verify all DCT weight entries are positive fixed-point values and that their squared variants equal the square divided by 256 within one unit.
- Verify CABAC transition targets stay in 0..127 and that each state's MPS target is lower than its LPS target for states above 1.
- Verify that CABAC entropy values are nonnegative and that the two entries for each state sum to a fixed value within rounding.
- Verify every RDO offset table's padding entries are zero and that no offset exceeds the CABAC context count minus one after adding its base.
- Verify all static VLC codes are prefix-free within each table family (no code is the prefix of another at the same index space), as required by H.264.

## 11. Open questions

- UNKNOWN: exact integer contents of the CAVLC code tables, CQM JVT/AVCI lists and CABAC context pairs are not reproduced here; they are fixed standard facts to be taken from the H.264 specification or an independently derived table generator.
- UNKNOWN: whether any consumer writes to `x264_zero`; declared non-const but no writer is visible in these files.
- UNKNOWN: the precise rounding rule used to generate the exp2 lookup's small deviations from a pure `round(256*2^(i/64))`; independent implementations may differ by one unit without changing visible decisions in most cases.
