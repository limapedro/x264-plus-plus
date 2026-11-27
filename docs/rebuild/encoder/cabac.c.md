# `encoder/cabac.c`

**Role:** Serializes one macroblock's syntax elements into a CABAC (context-adaptive binary arithmetic coding) bitstream, and provides a fast bit-count-only variant used during rate–distortion optimisation.

> **Dirty-side behavioural specification.** Describes observable behaviour and structure only; no source text or near-verbatim pseudocode is reproduced.

---

## 1. Header

- Source path: `encoder/cabac.c`
- Role: encode every macroblock-layer CABAC syntax element — macroblock type, sub-partitions, reference indices, motion-vector differences, coded-block patterns, QP delta, and transform-coefficient levels — into an `x264_cabac_t` arithmetic coder; plus an RD-only mode that estimates bit cost without producing a valid stream.
- Marker: dirty-side behavioural specification.

---

## 2. Purpose

H.264 Main/High profiles allow CABAC, which achieves better compression than CAVLC by coding binary decisions against contexts adapted to neighbouring data. This module is the *writer*: given the decided macroblock state in `h->mb`, it emits decisions with the standard-specified context indices in the correct order. It is used in two distinct modes selected at compile time by `RDO_SKIP_BS`: (a) the full writer that produces the actual slice bytes, and (b) a reduced "RD" writer that estimates bit counts (and updates the arithmetic state approximately) so the encoder can compare candidate partitions cheaply; the RD path intentionally omits CBP, chroma DC and reference-index syntax because those terms are constant across the candidates being compared.

---

## 3. Public interface

- `void x264_macroblock_write_cabac( x264_t *h, x264_cabac_t *cb )` — write the current macroblock as a full CABAC bitstream. Dispatches by chroma format: 4:4:4 uses three planes with no separate chroma coding; 4:2:0/4:2:2 use one luma plane plus a chroma pass; monochrome uses one plane and no chroma.
- `void x264_cabac_mb_skip( x264_t *h, int b_skip )` — encode the `mb_skip_flag` decision into the frame-level coder `h->cabac` (only in full-writer mode).
- `void x264_cabac_block_residual_c( x264_t *h, x264_cabac_t *cb, int ctx_block_cat, dctcoef *l )` — portable C implementation of residual coding for a coefficient block.
- `void x264_cabac_block_residual_rd_c( x264_t *h, x264_cabac_t *cb, int ctx_block_cat, dctcoef *l )`, `void x264_cabac_block_residual_8x8_rd_c( ... )` — portable RD-mode residual coders (4x4/8x8).
- SIMD counterparts are dispatched through `h->bsf` function pointers on x86-64 with MMX; the C versions are the reference behaviour.

All other helpers are `static` and internal to this file.

---

## 4. State and data structures

- `x264_cabac_t` — arithmetic coder state: current code word/range (`low`, `range`), output pointer `p`/`p_end`, per-context probability states `state[N]` (each a 6-bit finite-state-machine index), and `f8_bits_encoded` (approximate bit counter incremented by RD helpers). `x264_cabac_encode_decision` updates the state via the standard transition table; `_noup` variants are used where the caller knows no further update is needed; `encode_bypass` writes equiprobable bits with no state; `encode_terminal` terminates the arithmetic code; `encode_ue_bypass` codes an unsigned Exp-Golomb value using bypass bits; `x264_cabac_encode_flush` terminates and byte-aligns.
- `x264_t.mb` — current macroblock state read here: `i_type` (I_4x4, I_8x8, I_16x16, I_PCM, P_L0, P_8x8, B_*, etc.), `i_partition`, `i_sub_partition[4]`, `i_cbp_luma`, `i_cbp_chroma`, QP/last QP/last dQP, neighbour skip counts, intra prediction modes, transform-size flags, `i_neighbour` availability mask, and `cache` arrays of left/top CBP, non-zero counts, references, MVDs and prediction-mode candidates.
- `x264_t.dct` — quantized coefficient blocks (`luma4x4[48]`, `luma8x8[12]`, `luma16x16_dc[3]`, `chroma_dc[2]`).
- `x264_t.sh.i_type` — slice type (I/P/B) selecting which macroblock-type tree is used.
- `x264_t.quantf.coeff_last[]` — function pointers returning the index of the last non-zero coefficient in a block category.

Invariants: all neighbour caches must be populated before writing (the encoder guarantees this); contexts must be indexed in the standard-defined ranges; in RD mode the arithmetic state may diverge from the real writer by design.

---

## 5. Algorithm / behaviour

### 5.1 Macroblock type trees

**Intra (`cabac_mb_type_intra`).** `I_4x4` and `I_8x8` emit a single "not 16x16" decision at the first supplied context (ctx0). `I_PCM` emits "not 16x16" then the arithmetic coder is flushed and raw samples follow. `I_16x16` emits "16x16" (ctx0 = 1) followed by a terminal bit, then a luma-CBP-present bit at ctx1, chroma-CBP bits at ctx2/ctx3 (a two-level tree for DC and AC chroma), and the 16x16 intra prediction mode as two bits at ctx4 (MSB) and ctx5 (LSB) using a fixed mapping (`x264_mb_pred_mode16x16_fix`). The actual context offsets are supplied by the caller depending on slice type and neighbouring macroblock types.

**Header dispatch (`cabac_mb_header_i`).** In an I slice the context for the intra tree is `3 + ctx`, where `ctx` counts available left/top neighbours whose type is not I_4x4; additional contexts are 3+3, 3+4, 3+5, 3+6, 3+7. In a P slice a prefix bit at context 14 = 1 selects the intra branch, then the suffix uses contexts 17+0..17+3. In a B slice a five-decision prefix at contexts 27+3, 27+4 and 27+5 (pattern 1,1,1,0,1) selects intra, then suffix contexts 32+0..32+3. After the type, unless I_PCM: if 8x8 transform is enabled in the PPS, the transform-size flag is emitted; then 4x4 (or 8x8) intra prediction modes are coded for each 4x4 (or 8x8) block; finally the chroma prediction mode is emitted when chroma is present.

**P slice (`cabac_mb_header_p`).** `P_L0` begins with context 14 = 0. For a 16x16 partition contexts 15=0 and 16=0 are written, then a reference index (only if more than one forward reference) and one MVD (4x4 block). For 16x8: 15=1, 17=1, refs and two MVDs of 4x2 blocks. For 8x16: 15=1, 17=0, refs and two 2x4 MVDs. `P_8x8` writes 14=0,15=0,16=1, then four sub-partition decisions and per-sub-partition references/MVDs. Intra macroblocks in P slices fall through to the intra header path with slice type P.

**B slice (`cabac_mb_header_b`).** A context derived from left/top types (incremented when a neighbour is neither B_SKIP nor B_DIRECT) selects direct mode at contexts 27+ctx. `B_8x8` writes the pattern 1,1,1,1,1 at contexts 27+3/4/5, then four sub-partitions, per-list reference indices only for sub-partitions using that list, and per-list MVDs. The nine three-mode B types (L0_L0 … BI_BI) are encoded from a small packed table indexed by `(mb_type - B_L0_L0)*3 + (partition - D_16x8)`; the packed bits drive a short decision tree at contexts 27+3 and 27+5. A per-type list table says which reference indices and MVDs are present for each active list/partition. Intra blocks in B slices fall through to the intra path.

### 5.2 Sub-partition and reference trees

- P sub-partitions: context 21 distinguishes 8x8 from split; context 22 distinguishes 8x4 from 4x8/4x4; context 23 chooses 4x8 vs 4x4.
- B sub-partitions: context 36 separates direct from split; context 37 separates bi-predictive 8x8 from uni-directional; for BI_8x8 contexts 38=0 and two 39=0 decisions are written; for uni-directional context 39 chooses L1 vs L0.
- Transform size: context `399 + neighbour_transform_size`, one decision.
- Reference indices (`cabac_ref_internal`): context base 54 plus an increment counting left/top references > 0 (ignoring neighbours that are skipped in B slices). References are coded as a run of decisions at that context, with the context advancing by `ctx = (ctx>>2)+4` after each one, terminated by a zero.
- Skip flag (`x264_cabac_mb_skip`): context `neighbour_skip + 11`, plus 13 for B slices.

### 5.3 Motion-vector differences

For each component (x then y): context base is 40 for x and 47 for y. A zero MVD emits a single zero; otherwise a one is emitted and the magnitude is coded as a unary prefix through an 8-entry context table `{3,4,5,6,6,6,6,6}` mapped to the base, with magnitudes ≥ 9 adding an Exp-Golomb bypass suffix of order 3 over `abs-9`. The sign is a single bypass bit. The function returns `min(abs,66)` so the MVD cache can store 8-bit magnitudes. Context selection for each component uses the sum of the left and top cached MVD magnitudes (`x264_cabac_mvd_sum`). The full writer predicts the MV with `x264_mb_predict_mv`, subtracts the predictor, writes both components, and stores the packed 8-bit pair back into the cache. The 8x8 helper selects the correct block sizes per sub-partition (8x8, two 8x4, two 4x8, or four 4x4).

### 5.4 Coded-block patterns

Luma CBP: four decisions at base context 76, each context offset formed from the already-coded bits of the current CBP and the left/top cached CBP bits (specific bit rotations of the 4-bit luma CBP mask). Chroma CBP: a context 77 + 0..3 from whether left/top have any chroma CBP, then for non-zero chroma a second decision at context 77 + 4 + (left chroma DC ? 1 : 0) + (top chroma DC ? 2 : 0) selecting DC-only vs DC+AC.

QP delta: `dqp = qp - last_qp`, with a special case suppressing the delta (and restoring QP to the previous value) when the macroblock is an empty I_16x16 and the new QP would be higher, to avoid deblocking artefacts. Context is nonzero when the previous macroblock had a nonzero delta and was either I_16x16 or had a coded luma block. Nonzero deltas are mapped to a non-negative unary count via doubling and sign-folding, with modular wrap at `QP_MAX_SPEC` (values above the spec maximum are folded by `2*QP_MAX_SPEC+1 - val`); a run of one-decisions at context `60 + ctx` is written with context evolving as `ctx = 2 + (ctx >> 1)`, terminated by a zero.

### 5.5 Residuals

Coefficient categories select three context-base offsets: significant-coefficient flags (`x264_significant_coeff_flag_offset`, with a separate 8x8 table), last-coefficient flags (similar), and coefficient-level-minus-one (`x264_coeff_abs_level_m1_offset`). A `base_ctx[14]` table supplies CBP-based context increments `{85,89,93,97,101,1012,460,464,468,1016,472,476,480,1020}` for coded-block-flag decisions; for chroma DC the increments come from neighbouring chroma CBP bits, for other coefficients from neighbouring non-zero counts (with intra macroblocks masking the count to a binary value).

Normal residual coding: walk the block in zigzag order up to the last non-zero coefficient, emitting a significance flag at the category's context (plus a scanned offset for 8x8 and 4:2:2 DC blocks) and a last-coefficient flag (one at the last, zero before). Collect the coefficients in scanning order. Then walk them backwards: for each, choose a level-1 context from `coeff_abs_level1_ctx = {1,2,3,4,0,0,0,0}` or a greater-than-1 context from `{5,5,5,5,6,7,8,9}` (a variant `{5,5,5,5,6,7,8,8}` is used for 4:2:2 chroma DC), keyed by a node state that transitions through `coeff_abs_level_transition`. Magnitudes above 1 emit unary one-decisions up to 15 then an order-0 Exp-Golomb bypass for the excess; a sign bypass bit follows each coefficient.

RD-mode residual coding merges significance and level coding: it emits the last coefficient's significance/last flags directly (skipping the ones before it), updates the arithmetic state and bit counter through precomputed `x264_cabac_size_unary` / `x264_cabac_transition_unary` tables, and emits the remaining coefficients in reverse. Because 8x8 and 4:2:2-DC context tables repeat contexts, the sigmap is not exactly invertible in this mode — an accepted approximation of roughly 0.001 dB with about a 30% speed gain for the RD search.

### 5.6 Macroblock assembly

`macroblock_write_cabac_internal` performs, in order: optional field-decoding flag (MBAFF only, at context 70+ctx); macroblock header for the slice type; raw PCM sample bytes when I_PCM (using a byte-aligned bit writer, then reinitialising the arithmetic coder); luma CBP and chroma CBP for non-I_16x16; optional transform-size flag when 8x8 transform is allowed and luma CBP is nonzero; QP delta when any residual is present; and residual coding — I_16x16 writes luma DC then AC blocks; 8x8-transform blocks write 8x8 residuals; otherwise 4x4 residuals; chroma writes DC blocks for both chroma components and, when chroma CBP is 2, AC blocks in a stepped traversal depending on chroma vertical shift. Statistics counters accumulate MV bits and texture bits around the header and residual sections.

The RD-only tail of the file provides per-partition helpers (`partition_size_cabac`, `subpartition_size_cabac`, `partition_i8x8_size_cabac`, `partition_i4x4_size_cabac`, `chroma_size_cabac`) used by the analysis code to price individual candidate partition sizes; they write only the syntax elements that vary between candidates.

### 5.7 Per-element decision walkthrough (bit-exact detail)

The following enumerates the decisions in emission order for each macroblock category, so a clean implementation can reproduce context selection exactly:

**Intra macroblock type, I slice.** The context offset is formed from neighbour types: add 1 when the left neighbour exists and is not I_4x4, add 1 when the top neighbour exists and is not I_4x4. The intra tree then emits, for I_16x16, a "16x16" one at `3+ctx`, a terminal, luma-CBP presence at context 6, chroma-CBP presence at 7 (with a second decision at 8 selecting DC+AC), and the prediction mode's high and low bits at 9 and 10. For I_4x4/I_8x8 a single zero at `3+ctx` selects the sub-block path.

**Intra macroblock in a P slice.** A prefix one at context 14 selects intra, then the tree suffix reuses contexts 17,18,19,20,20 (note that the chroma-CBP pair collapses to a single context 19). In a B slice the prefix is the five-decision sequence at 30,31,32,32,32 with values 1,1,1,0,1, and the suffix uses contexts 32,33,34,34,35.

**Intra prediction modes.** For each 4x4 block (or each of the four blocks of an 8x8 when 8x8 transform is active) the predictor is derived from left/top already-coded blocks; a one at context 68 means "predictor equals actual". Otherwise a zero at 68 is followed by three decisions at context 69 encoding the mode with a decrement when it exceeds the predictor. Chroma prediction uses context `64 + (#neighbours with nonzero chroma mode)`, then a second level at 67.

**P macroblock types.** P_L0 begins with a zero at 14; the partition is encoded as context 15 zero for 16x16, or one followed by context 17 selecting 16x8 (one) vs 8x16 (zero). P_8x8 writes 14=0, 15=0, 16=1. Sub-partitions use the 21/22/23 tree. Reference indices are emitted only when more than one forward reference exists.

**B macroblock types.** The base context is `27 + (#neighbours that are neither B_SKIP nor B_DIRECT)`. B_DIRECT emits a zero there and returns. B_8x8 emits the 1,1,1,1,1 prefix at 30/31/32. The nine inter-B types are expanded from a packed 27-byte table into at most five decisions at 30 and 32; the exact pattern depends on type and partition. Reference indices and MVDs are emitted per active list as indicated by the per-type list table.

**MVD component coding.** Each component writes a leading decision at context 40 (x) or 47 (y) plus a component-specific offset from the neighbour MVD magnitudes. A nonzero component emits unary ones through contexts mapped by `{3,4,5,6,6,6,6,6}`; magnitudes ≥ 9 add an order-3 Exp-Golomb bypass of `abs-9`; sign is a bypass bit. The returned magnitude is capped at 66 for the 8-bit cache.

**Residual coefficient levels.** The significance-map traversal is per block category; the level coder maintains an 8-state node FSM with two transition tables (after a level-1, after a level>1). Levels equal to 1 use contexts `{1,2,3,4,0,0,0,0}` plus the category offset; levels greater than 1 use `{5,5,5,5,6,7,8,9}` plus the offset. The unary prefix for level>1 stops at 15; beyond that an order-0 Exp-Golomb bypass carries the excess. Every coefficient is followed by a sign bypass.

---

## 6. Dependencies

Calls CABAC primitives from `common/cabac.h` (`x264_cabac_encode_decision[_noup]`, `_bypass`, `_terminal`, `_ue_bypass`, `_flush`, `x264_cabac_pos`, `x264_cabac_encode_init_core`), bit-writer primitives for PCM, prediction helpers from `macroblock.c` (`x264_mb_predict_mv`, `x264_mb_predict_intra4x4_mode`, mode-fix tables), coefficient-last functions in `h->quantf`, and static tables from `common/tables.h` (`x264_scan8`, scan offsets, `x264_mb_type_list_table`, `x264_mb_partition_listX_table`, `x264_pixel_size`). Called from `macroblock.c`/`rdo.c` (`x264_macroblock_write_cabac`) and from the analysis code via the RD helpers; `x264_cabac_mb_skip` is called from the skip decision path.

---

## 7. Constants and tables

- `base_ctx[14] = {85,89,93,97,101,1012,460,464,468,1016,472,476,480,1020}` — context bases for CBP decisions.
- `coeff_abs_level1_ctx = {1,2,3,4,0,0,0,0}`, `coeff_abs_levelgt1_ctx = {5,5,5,5,6,7,8,9}`, 4:2:2-DC variant `{5,5,5,5,6,7,8,8}`.
- `coeff_abs_level_transition[0] = {1,2,3,3,4,5,6,7}` (after level 1), `[1] = {4,4,4,4,5,6,7,7}` (after level > 1).
- `ctxes[8] = {3,4,5,6,6,6,6,6}` — MVD magnitude unary context sequence.
- `i_mb_bits[9*3]` — packed B-mode prefix decision patterns for the nine inter-B macroblock types across three partition shapes.
- Context bases: intra type 3/17/32, P type 14/15/16/17, B type 27, skip 11 (+13 for B), reference 54, MVD 40/47, QP delta 60, chroma prediction 64, intra4x4 mode 68/69, field flag 70, luma CBP 76, chroma CBP 77, transform size 399.
- MVD magnitudes cap at 66; coefficient level unary cap at 15; `QP_MAX_SPEC` defines QP-delta modular wrap.

---

## 8. Performance notes

This is one of the hottest encoding paths — every macroblock of every frame passes through it, so it is SIMD-optimised and carefully branch-tuned.

Concrete opportunities, with honesty about impact:
- **SIMD residual coding**: on x86-64 with MMX the residual loop dispatches through `h->bsf` to hand-written assembly; preserving bit-exact context transitions and state updates is mandatory — any deviation changes the bitstream and therefore the quality at a given QP. A portable C fallback is provided and is behaviourally identical.
- **RD-mode approximation**: the merged sigmap/level RD writer gains roughly 30% speed in mode decision at a measured ~0.001 dB quality cost; this affects only *decisions*, not the final syntactic stream (the real writer is still used for output). A clean implementation may adopt it safely, but must keep the exact-writer path for emission.
- **Context-state caching**: grouping `_noup` decisions where no later decision uses the same context avoids redundant state transitions; already exploited here.
- **Table-driven trees**: B-mode and sub-partition trees are encoded as packed constants to replace branches; a clean implementation can use equivalent lookup tables without behavioural change.
- **Skip/stats shortcuts**: when only bit counts are needed, `f8_bits_encoded` accumulation replaces full arithmetic renormalisation.

No threading is present; each thread owns its `x264_cabac_t`. Determinism requires that contexts be updated in exactly the standard order — frame threading changes which macroblocks are neighbours, so slice boundaries must reset context state as specified.

---

## 9. References

- ITU-T H.264, Annex B (CABAC): context indexing tables 9-1 … 9-12, `mb_type`, `ref_idx`, `mvd`, `coded_block_pattern`, `mb_qp_delta`, residual block syntax.
- "Context-Based Adaptive Binary Arithmetic Coding in the H.264/AVC Video Compression Standard" (Marpe, Schwarz, Wiegand), IEEE TCSVT 2003.
- x264 public source documentation and `common/cabac.h` interface.

---

## 10. Verification checklist

- Compare CABAC bytes emitted for a fixed sequence of macroblock decisions against a known-good encoder at each QP; must be bit-identical.
- Decode emitted streams with an independent H.264 decoder and verify reconstructed frames match within the expected CABAC-only path.
- For RD mode, confirm bit counts are close to (but need not equal) the exact writer's count, and that final encoded output still uses the exact writer.
- Exercise all macroblock types: I_4x4, I_8x8, I_16x16, I_PCM, P_L0 (three partitions), P_8x8 (four sub-partitions), B_DIRECT, B_8x8, nine B inter types, and intra in P/B slices.
- Verify QP-delta edge cases: empty I_16x16 QP increase suppression, large deltas near `QP_MAX_SPEC` wrap, negative deltas.
- Verify 4:2:0, 4:2:2 (special chroma DC path), 4:4:4, and monochrome chroma handling.
- Verify SIMD and C residual writers produce identical output for random coefficient blocks.

---

## 11. Open questions

- The precise effect of the RD-mode state approximation on rate-control bit accounting (as opposed to RD mode decisions) is UNKNOWN; it is used only during analysis.
- Exact x86 assembly dispatch conditions beyond "ARCH_X86_64 && HAVE_MMX" and the `h->bsf` function-pointer names are UNKNOWN from this file alone.
- Some context-index derivations (e.g. the exact bit rotations in luma CBP context calculation) are described by their behaviour here; a clean implementer should cross-check against H.264 Table 9-11 for bit-exactness.
