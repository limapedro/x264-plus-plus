# `encoder/rdo.c` — rate-distortion costs and trellis quantization

**Role (one sentence):** This file evaluates true entropy-coded rate-distortion costs for candidate macroblock modes and performs trellis/near-optimal coefficient-level quantization decisions for CABAC and CAVLC.

> **Dirty-side behavioural specification.** Written from the reference source and public literature for the clean-room reimplementation. No source code or near-verbatim pseudocode is reproduced; only behaviour, structure, constants and decision conditions are described.

---

## 1. Header

- Source path: `encoder/rdo.c` (about 1184 lines).
- Role: (a) bit-count-only twins of the CABAC and CAVLC macroblock writers used as cost evaluators, (b) SSD plus psy-RD distortion measurement with caching, (c) RD cost functions at MB/partition/8x8/4x4/chroma granularity, (d) CABAC trellis quantization and CAVLC QNS quantization.
- `x264_rdo_init()` is declared in `macroblock.h`; `x264_rd_cost_part` in `me.h`; the quantisation entry points in `macroblock.h`.

---

## 2. Purpose

SATD is a cheap distortion proxy but does not know how the residual will be entropy coded. At higher `subme` values the encoder must decide between modes whose SATD costs are close but whose bit costs differ substantially. This file supplies that decision by *actually encoding* each candidate into a temporary bit-counting writer, measuring distortion, and forming `D + lambda * R`. It also replaces dead-zone quantization with a dynamic-programming search over coefficient levels, where the state includes CABAC context probabilities (or, for CAVLC, a greedy QNS over rounding choices), so the chosen coefficients minimise the same RD objective.

A second responsibility is psy-RD: the distortion term can include a complexity-preservation penalty so that the encoder favours reconstructions that keep the block's Hadamard "energy" close to the source's, countering the smoothing tendency of ordinary SSD.

---

## 3. Public interface

| Symbol | Contract |
|---|---|
| `void x264_rdo_init(void)` | Precomputes, for all 15 unary prefix lengths and all 128 CABAC contexts, the fractional-bit cost and resulting context state of a run of ones followed by a terminating zero; likewise for the five-ones/sign pattern used for large MVD levels. Must be called once before encoding. |
| `uint64_t x264_rd_cost_part(x264_t*, int i_lambda2, int i4, int i_pixel)` | RD cost for a trial prediction of a 16x16, 16x8/8x16, 8x8/8x4/4x8, or 4x4 partition. It re-encodes the relevant 8x8 or 4x4 blocks, computes distortion over the covered region, counts bits for the corresponding syntax elements, and returns `(SSD << 8) + bits*lambda2` in an 8-bit-extra-precision fixed-point format (except the 16x16 case, which returns a 32-bit `D + lambda*R`). |
| `int x264_quant_luma_dc_trellis(h, dct, quant_cat, qp, ctx_cat, b_intra, idx)` | Trellis quantise the 4x4 DC block of an I16x16 luma plane; returns whether any coefficient survived. |
| `int x264_quant_chroma_dc_trellis(h, dct, qp, b_intra, idx)` | Trellis quantise the 2x2 (4:2:0) or 2x4 (4:2:2) chroma DC block. |
| `int x264_quant_4x4_trellis(h, dct, quant_cat, qp, ctx_cat, b_intra, b_chroma, idx)` | Trellis quantise one 4x4 AC block. |
| `int x264_quant_8x8_trellis(h, dct, quant_cat, qp, ctx_cat, b_intra, b_chroma, idx)` | Trellis quantise one 8x8 AC block; under CAVLC it processes the four embedded 4x4 blocks sequentially and ORs their nonzero flags. |

Static helpers: `ssd_plane`, `ssd_mb`, `rd_cost_mb`, `rd_cost_subpart`, `rd_cost_i8x8`, `rd_cost_i4x4`, `rd_cost_chroma`, `cached_hadamard`, `cached_satd`, `trellis_dc_shortcut`, `trellis_coef*`, `quant_trellis_cabac`, `quant_trellis_cavlc`.

The file obtains bit counters by including `cavlc.c` and `cabac.c` twice with preprocessor substitutions that turn bit-writing calls into additions. CAVLC counts are exact; CABAC counts track fractional bits with finite precision (`f8_bits_encoded`, eight fractional bits).

---

## 4. State and data structures

- **CABAC bit-count tables:** `x264_cabac_transition_unary[15][128]` and `x264_cabac_size_unary[15][128]` (global, exported), plus private five-ones equivalents. Entry `[prefix][ctx]` gives the fractional cost of coding `prefix-1` binary ones followed by a zero (with a sign bit) and the resulting context state. Large levels beyond the unary range add an Exp-Golomb suffix sized by `bs_size_ue_big`.
- **Trellis node:** a score (64-bit), an index into a level tree, and four saved CABAC context bytes (contexts 0, 4, 8, 9 of the `abs_level_m1` set). Two arrays of eight nodes are ping-ponged between coefficient positions. A score of all-ones marks an invalid node; a bias of `1<<60` keeps valid scores positive even after negative psy contributions.
- **Trellis level tree:** packed 16-bit `next` index and 16-bit absolute level; a new node is appended whenever a level is chosen.
- **CABAC state snapshot:** `COPY_CABAC` copies the fractional bit counter plus 460 bytes of state (or 1024+12 for 4:4:4) so a candidate can be evaluated without disturbing the real encoder state.
- **Per-MB caches:** `fenc_hadamard_cache` and `fenc_satd_cache` hold source complexity values; a stored value is offset by one so zero means "not computed". Cached SATD values are `SATD - DC/2` of the source block.
- **Ownership:** all snapshots and node arrays are stack-local; the global tables are read-only after `x264_rdo_init`. DCT coefficient arrays are modified in place by the quantization functions.

---

## 5. Algorithm / behaviour

### 5.1 Bit-counting twins

`RDO_SKIP_BS` is defined before including the entropy writers, converting them into pure bit counters. CAVLC's `bs_write*` calls add 1 or n bits and Golomb sizes; CABAC's decision calls add `x264_cabac_size_decision*` results to a fractional accumulator. Terminal adds 7 fractional units, bypass adds 256 (one whole bit), and Exp-Golomb bypass adds `(size - e) << 8`. The resulting counts are used only for comparison, never written to the bitstream.

### 5.2 Distortion with psy-RD

`ssd_plane(h, size, p, x, y)`:

1. Computes ordinary SSD between source and reconstructed plane for the given block size.
2. If the plane is luma and `i_psy_rd != 0`, adds a complexity-difference term. For blocks 8x8 or smaller, the complexity is the pair of 32-bit halves of the Hadamard AC transform (`hadamard_ac`); for larger blocks it is `SATD - DC/2`. Source complexity is fetched from a cache, reconstruction complexity computed on the spot. The absolute difference of the two components (halved for the 8x8 case) is multiplied by `psy_rd * psy_rd_lambda`, rounded with `+128 >> 8`, and capped at `COST_MAX`.
3. `ssd_mb` adds luma SSD and chroma SSD; chroma SSD is scaled by `i_chroma_lambda2_offset` (a table indexed by the chroma/luma QP difference, value 256 when psy is disabled) with an `+128 >> 8` rounding.

Using both SATD and SA8D (Hadamard of 8x8) avoids biasing the 8x8-transform decision in either direction; the source notes that using only one would over- or under-use 8x8dct.

### 5.3 RD cost at different granularities

**`rd_cost_mb` (full macroblock).** Encodes the current MB, optionally deblocks it (`b_deblock_rdo` at subme >= 9 with deblocking enabled), measures MB SSD, then counts bits: skip costs exactly one bit; CABAC counts through a state snapshot; CAVLC uses the bit-count twin. Result is `min(SSD + lambda2 * bits >> shift, COST_MAX)` — CABAC's fractional accumulator shifts by 16, CAVLC's integer counter by 8. The transform-8x8 flag and MB type are restored after the call, so the caller's state is untouched.

**`rd_cost_subpart` (8x4/4x8/4x4).** Encodes the one, two or four 4x4 blocks covered by the partition, measures luma (and, in 4:4:4, chroma) SSD, counts subpartition bits, and returns `(SSD << 8) + bits`. The extra eight fractional bits reduce rounding error at low QP.

**`x264_rd_cost_part`.** Dispatch by block class: 16x16 goes through `rd_cost_mb`; classes larger than 8x8 go through `rd_cost_subpart`; 8x8/16x8/8x16 encode the relevant 8x8 block(s), measure the covered region including chroma (scaled), count partition bits via the CABAC/CAVLC twin, and return `(SSD << 8) + bits`.

**`rd_cost_i8x8` / `rd_cost_i4x4` / `rd_cost_chroma`.** Encode the candidate intra prediction with the corresponding transform size, measure SSD over the block, count the syntax bits for that specific mode, and return the same fixed-point format. `rd_cost_chroma` can skip re-encoding the residual (`b_dct = 0`) when only the prediction mode bits need recounting.

### 5.4 CABAC trellis quantization

Constants: `CABAC_SIZE_BITS = 8`, `LAMBDA_BITS = 4`; node score bias `1<<60`; invalid score all-ones.

1. A conventional quantizer first produces a candidate level array; if the whole block quantises to zero, the function returns zero immediately.
2. DC-only shortcut: for a block whose only nonzero candidate is the DC coefficient, try levels `q-1`, `q` and (implicitly) zero, evaluate distortion with a special DC rounding formula, include significance/last-flag costs plus the unary level cost and sign, and pick the best.
3. Main loop processes coefficients **in reverse zigzag order**, because absolute levels are coded from the last nonzero back to the start; significance/last flags are reversed to match. Coefficient positions with a zero candidate level are skipped (their cost contribution is subtracted from the zero-context node rather than recomputed).
4. For each nonzero coefficient, two candidate magnitudes are considered: `q-1` and `q`, where `q` is the conventional quantizer result. Source comments note that trying larger increases helps only about 0.003 dB but roughly doubles runtime, so it is omitted. Distortion for each candidate is `(coef - unquant(level))^2` weighted per frequency by a DCT weight table; psy-trellis subtracts a term proportional to `psy_trellis * |predicted_coef + signed_unquant_level|` for luma AC, biasing toward richer reconstruction.
5. Entropy cost per candidate includes significance flag, last flag (where applicable), the level-1 context entropy, and for levels >1 the unary prefix/suffix cost from the precomputed tables. Cost is converted to score units by multiplying by `lambda2` and shifting right by `CABAC_SIZE_BITS - LAMBDA_BITS` (i.e. 4).
6. Dynamic programming keeps one best node per CABAC context state (eight states). Nodes store the four relevant context bytes so state transitions are modelled; early coefficient positions use the encoder's initial state. A dedicated path handles the case where no level greater than one has yet been seen (node context zero), and exits as soon as that path fails.
7. After the last coefficient, the lowest-scoring live node is chosen and the level tree is walked backwards to write the selected signed levels into the DCT array. If the zero-context node wins, the block is emptied (a full 4x4 clear for 4x4 AC; implicit empty via zero NNZ for 8x8 and DC).
8. SIMD: on x86-64 with MMX, specialised assembly routines handle 4x4 AC, 8x8 AC (interlaced and not), 4:2:2 chroma DC and luma/chroma DC, with separate psy variants that take the source DCT coefficients and psy strength.

### 5.5 CAVLC "trellis" (QNS)

The source explicitly calls this a "gigantic hack": CAVLC has five coupled state machines (coeff_token, total_zeros, run_before, level state, plus trailing-ones), so an exact DP is considered intractable. Instead:

1. Find the last coefficient whose magnitude times the quantiser factor reaches the deadzone threshold; if none, the block is empty.
2. For each coefficient up to that point, compute two rounding options (nearest and nearest-minus-one) and the distortion *difference* between them, weighted per frequency; initial rounding is a deadzone halfway between the two options, which empirically works better than either extreme. Psy-trellis adjusts the delta similarly to the CABAC path.
3. Count the bits for the initial coefficient vector using the real CAVLC residual routine with bit counting enabled; this is the starting score.
4. Greedily repeat: for each coefficient with a nonzero delta, temporarily flip its rounding, recount the whole block's bits, and compute `delta_distortion + lambda2 * delta_bits`. Apply the flip with the best (most negative) score; mark that coefficient as already adjusted so it is not changed twice. Stop when no flip improves the score.
5. Write the surviving levels back. Because 8x8 DCT with CAVLC is represented as four interleaved 4x4 blocks, the input/output step and start/end indices are remapped; after the four quantisations the 8x8 NNZ cache is explicitly zeroed.

### 5.6 Quantization wrappers

- Luma DC: quantiser factor halved and bias doubled (matching H.264's DC scaling), 4x4 zigzag; CAVLC uses the generic `DCT_LUMA_DC` context.
- Chroma DC: 2x2 zigzag for 4:2:0, 2x4 for 4:2:2; quantiser category selected from `CQM_4IC + 1 - intra`; chroma trellis lambda from the [chroma][intra] table.
- 4x4 AC: context category decides whether the DC coefficient is treated separately (`ctx_ac` masks); CAVLC and CABAC dispatch to the corresponding trellis engine.
- 8x8 AC: CABAC uses one 64-coefficient trellis; CAVLC splits into four 4x4 trellis calls with `b_8x8 = 1` remapping and ORs the nonzero results.

### 5.7 Fixed-point arithmetic summary

All cost comparisons in this file use fixed-point integers. An independent implementation must reproduce the shifts exactly, because a one-bit difference can flip a mode decision:

| Quantity | Units | Conversion into score |
|---|---|---|
| CAVLC bit count | whole bits | `bits * lambda2`, then `+128 >> 8` for MB/chroma; partition costs keep the product unshifted and are added to `SSD << 8`. |
| CABAC bit count | 8 fractional bits (`f8_bits_encoded`) | `f8_bits * lambda2`, then `+32768 >> 16` for MB costs; `+128 >> 8` for partition/chroma costs. |
| Trellis entropy cost | 8 fractional bits | `f8_bits * lambda2 >> (8 - 4)`, i.e. multiply by lambda2 and shift right four. Sign always costs exactly one whole bit (`1 << 8`). |
| Plane SSD | sample-error squared | summed directly; chroma scaled by `i_chroma_lambda2_offset` with `+128 >> 8`. |
| Psy-RD penalty | SATD/SA8D difference | `satd * psy_rd * psy_rd_lambda + 128 >> 8`, clamped to `COST_MAX`. |
| Psy-trellis coefficient bias | source-DCT magnitude | `psy_trellis * |predicted_coef ± unquant_level| * coef_weight1`, subtracted from the squared-error term. |

The `+128` / `+32768` constants are round-to-nearest offsets for an 8-bit or 16-bit right shift. `lambda2` is defined as roughly `0.9 * lambda^2 * 256`; the 256 factor compensates the 8 fractional bits used by CABAC and by partition costs.

### 5.8 Trellis dynamic program in words

The CABAC trellis is a shortest-path problem over coefficient positions. Each node records a partial score and a CABAC state; an edge corresponds to choosing a level for the current coefficient. The recurrence is:

`score_next[state'] = min over (previous node, level) of score_prev[state] + distortion(coef, level) + lambda2 * bits(level, state -> state')`

where `bits` includes the significance flag, last-flag where applicable, the level-1 decision and the unary/suffix cost of the magnitude. Only eight states are kept because only contexts 0, 4, 8 and 9 of the level coding evolve in ways that matter for the next coefficient. The two loop variants correspond to (a) states reachable before any magnitude above one has been seen, where the live set is contiguous from state zero, and (b) states after such a magnitude, where each group of four contexts must be scanned separately. Candidate levels are limited to the nearest integer quantisation and the one below it; that restriction is a deliberate speed/quality compromise.

The level tree stores the chosen magnitude for each (position, state) pair. Because a level choice appends a tree node, the final path is recovered by walking `next` links from the winning node back to the root; signs are re-applied from the original DCT coefficients. A zero-level path is represented implicitly by the root's self-loop, which is why a zero-context winner can empty the block without touching the tree.

For DC-only blocks the DP collapses to three candidate levels and a simpler cost: significance bit + last bit + unary level bits + sign, against a distortion computed with a round-to-multiple-of-16 reconstruction. This shortcut is asserted not to change output in the reference.

### 5.9 Ordering and state discipline

Several details are observable only indirectly but are required for equivalence:

1. RD functions must restore `h->mb.i_type` and `h->mb.b_transform_8x8` after the trial encode; callers rely on their own values surviving.
2. CABAC state snapshots copy only the bit counter plus the first 460 (or 1024+12) state bytes; the rest of `h->cabac` is untouched. Restoring a stale full state would change later context probabilities.
3. Partition RD clears `i_cbp_luma` before encoding one 8x8 cell; otherwise a neighbouring cell's CBP leaks into the bit count.
4. `rd_cost_i8x8` clears the current 8x8 CBP bit before encoding and forces the 8x8 transform; `rd_cost_i4x4` does not touch the transform flag.
5. In chroma RD, setting `b_dct = 0` while counting a different prediction mode assumes the residual coefficients from the previous call are still in `p_fdec`/`h->dct`; this is why the caller performs chroma prediction before, not after, the call.
6. Trellis writes NNZ cache entries even for CAVLC's four-way 8x8 split; the caller's later CAVLC pass uses those entries as context, so they must agree with the returned nonzero flag.
7. Saturation: `rd_cost_mb` clamps the final sum to `COST_MAX`; partition functions use 64-bit arithmetic and do not clamp, since they are compared against `COST_MAX64`.

### 5.10 Interactions with encoding settings

- **Trellis level.** Trellis is active during RD trials only when `i_trellis > 1` and `i_mbrd` is nonzero; the final encode uses the configured trellis value. Therefore trial costs at trellis=1 are computed with deadzone quantisation even though the final residual will be trellis-quantised — an intentional approximation.
- **Psy-RD zero.** With psy-RD disabled, chroma lambda scaling is exactly 256 and source complexity caches are never populated; `ssd_plane` reduces to plain SSD.
- **Psy-trellis.** The source DCT arrays (`fenc_dct4`, `fenc_dct8`) are prepared by `analyse.c` once per MB when trellis=2 or when trellis=1 with psy-trellis enabled. Their absence makes the psy bias term zero, restoring ordinary trellis behaviour.
- **Deblocking in RD.** At subme >= 9, `rd_cost_mb` deblocks the trial reconstruction before measuring SSD, so RD costs reflect what later frames will actually see. This changes the measured distortion but not the entropy count.
- **8x8 transform.** RD trials can flip `b_transform_8x8`; the cost functions themselves do not decide the transform, but the CAVLC trellis path splits an 8x8 block into four interleaved 4x4 quantisations, so the reported nonzero flag is an OR.
- **Lossless.** RD cost paths are not used for lossless blocks; `analyse.c` keeps `i_satd_pcm` at the sentinel and trellis is effectively bypassed.

### 5.11 Numeric edge cases

1. When `lambda2` reaches its capped value (134217727 at QP 70+), `lambda2 * bits` can approach 2^31; partition costs therefore use 64-bit arithmetic, and `rd_cost_mb` clamps to `COST_MAX`.
2. `bs_size_ue_big` switches formula at value 255; this matters for very large MVDs in trellis suffix costs.
3. The unary prefix is capped at 14: levels 15 and above share the same prefix but add an Exp-Golomb suffix, so the marginal bit cost of a large level grows logarithmically, not linearly.
4. `TRELLIS_SCORE_BIAS = 1<<60` is large enough that even many negative psy contributions cannot make a valid score negative; scores near `1<<60` are compared with unsigned arithmetic, while zero-coefficient paths subtract cost from the zero node.
5. Chroma QP offsets are not added inside this file except via `h->mb.i_chroma_qp` set by the analyser; a clean implementation must not recompute them here.

---

## 6. Dependencies

- **Called by:** `analyse.c` (all RD mode decisions, intra RD refinement, QP-RD, transform decision), `me.c` (RD-driven sub-pel and bidirectional refinement).
- **Calls:** `x264_macroblock_encode`, `x264_macroblock_encode_p8x8`, `x264_macroblock_encode_p4x4`, `x264_mb_encode_chroma` (`macroblock.c`); `x264_macroblock_deblock`; the counting twins of `cavlc.c`/`cabac.c`; SIMD quantizers (`h->quantf.*`), DCT/IDCT (`h->dctf.*`), scan functions (`h->zigzagf.*`), `h->pixf.ssd/satd/hadamard_ac/sad`; tables from `common/tables.c` (`lambda2`, `trellis_lambda2`, `chroma_lambda2_offset`, DCT weight tables, zigzags, CABAC entropy tables).
- **Shared state:** reads and snapshots `h->cabac`; temporarily writes `h->mb` encoding state, restoring `i_type` and `b_transform_8x8`; writes NNZ cache entries for split 8x8 CAVLC.

---

## 7. Constants and tables

- `CABAC_SIZE_BITS = 8`, `LAMBDA_BITS = 4`, `TRELLIS_SCORE_MAX = ~0ULL`, `TRELLIS_SCORE_BIAS = 1ULL<<60`.
- Cost rounding constants: `+128 >> 8` for integer-bit costs, `+32768 >> 16` for CABAC fractional costs.
- Unary prefix limit 14; levels >= 15 add `bs_size_ue_big(level-15)` suffix bits.
- Five-ones pattern for large MVD levels (five binary ones plus sign).
- DC-only distortion uses a rounding mask of `~15` with `+8`, i.e. round-to-nearest multiple of 16 in signed magnitude.
- Trellis level-tree capacity 64*8*2 entries; two node arrays of eight entries.
- `i_psy_rd` enters squared (`psy_rd * psy_rd_lambda`) for plane distortion, but linearly for per-coefficient psy-trellis bias.
- DCT weight tables distinguish 4x4 and 8x8 transforms; luma DC-only blocks use a fixed weight of 256.
- The `ctx_ac` mask selects which block categories add the AC base offset (values 0 or 1 by category index).

---

## 8. Performance notes

1. **Bit counting is the dominant cost at high subme.** Each RD candidate re-runs a full CABAC/CAVLC pass. The bit-count twins are deliberately duplicated code so the hot path avoids writingbytes; preserving this structure (or an equally cheap counting model) is essential [1]. Quality trade-off: approximating CABAC bit costs with analytic tables is much faster but changes mode decisions at low QP.
2. **CABAC state snapshotting [2].** `COPY_CABAC` copies only the live fraction of the context array (460 bytes, or 1024+12 in 4:4:4) plus the bit counter; copying the entire encoder state per candidate would be far slower.
3. **Trellis SIMD [3].** The x86-64 C/assembly dispatch selects specialised trellis routines; losing them costs a large fraction of trellis=1/2 performance. The C fallback must produce identical coefficient choices for equivalence.
4. **DC-only shortcuts.** Both trellis paths and chroma encoding include DC-only fast paths; they are pure optimisations but must be kept for identical rounding behaviour.
5. **Psy-RD cost.** The complexity term requires Hadamard transforms of source and reconstruction. Caching source complexity (with a "value+1" sentinel) avoids repeated transforms; reconstruction complexity cannot be cached [4].
6. **CAVLC QNS greedy loop.** Recounting the whole block for every candidate flip is quadratic in coefficient count; the `delta_distortion == 0` skip and "do not adjust twice" rule are important speed measures with a small, empirically measured quality effect.
7. **Concurrency.** RD functions temporarily mutate `h->mb` and `h->cabac`, so they are not thread-safe on a shared encoder context; frame threading must give each thread its own context. The read-only global tables are safe to share [5].

---

## 9. References

[1] ITU-T Rec. H.264 / ISO-IEC 14496-10 — syntax elements counted here (MB type, CBP, partition, MVD, residual).

[2] CABAC context modelling and arithmetic coding as specified in the same standard; x264 developer notes on fractional-bit estimation.

[3] G. Bjøntegaard and others on trellis-based quantization; the classic "trellis-coded quantization" idea applied to H.264 by the x264 project (source comments credit the approach and note the CAVLC compromise).

[4] Psy-RD / psy-trellis: x264 developer documentation on preserving high-frequency energy (complexity = SATD/SA8D), and the general rate-distortion objective `D + lambda*R`.

[5] x264 frame-threading documentation.

---

## 10. Verification checklist

1. With `psy_rd = 0`, `ssd_mb` equals the sum of plane SSDs with chroma scaled by 256.
2. Skip MB bit cost is exactly `lambda2/256` rounded with +128, i.e. one bit.
3. For fixed MB state, `rd_cost_mb` returns identical values under CABAC and CAVLC twins to those obtained by instrumenting the real writers (bit counts only).
4. Trellis: for a synthetic block with a single nonzero DC coefficient, the DC shortcut chooses the same level as the full trellis.
5. Trellis: no chosen absolute level is negative; sign is restored from the original coefficient; block emptiness agrees with the returned NNZ.
6. Psy-trellis: enabling it can only increase the selected AC magnitudes (observable on a synthetic ringing pattern).
7. CAVLC QNS: the returned coefficient vector is one of the two rounding choices per position, and the score never increases across iterations.
8. Fixed-point formats: partition costs are `(SSD<<8)+bits*lambda2` (or `bits` unscaled for CAVLC partitions), MB cost is unscaled `SSD + lambda2*bits`.
9. State restoration: after an RD call, `h->mb.i_type`, `h->mb.b_transform_8x8` and CABAC state are unchanged.
10. Saturation: very low QP/high lambda does not overflow the 64-bit RD accumulator; `rd_cost_mb` clamps at `COST_MAX`.

---

## 11. Open questions

- **UNKNOWN:** the exact source and derivation of the DCT coefficient weight tables (`x264_dct4_weight_tab`, `x264_dct8_weight_tab`, and their squared variants); only their role as perceptual frequency weights is evident here.
- **UNKNOWN:** why the chroma lambda offset table index uses `qp - effective_chroma_qp + 12` clipped at `MAX_CHROMA_LAMBDA_OFFSET`; the numeric table is in `common/tables.c`, its fitting procedure is not documented.
- **UNKNOWN:** how much of the claimed ~0.003 dB gain from trying coefficient magnitudes above `q` was measured, and under which presets; the number appears only in a source comment.
- **UNKNOWN:** whether treating 8x8 CAVLC as four interleaved 4x4 blocks is bit-exact with a single 8x8 bit-count model; the reference explicitly chooses the four-block route.
- **UNKNOWN:** precise fractional-bit precision of `x264_cabac_size_decision*` for very long symbol runs; source notes it is finite precision but not how many bits of accuracy are retained.
