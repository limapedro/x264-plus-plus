# `encoder/macroblock.c` — macroblock encoding and skip probing

**Role (one sentence):** This file turns the encoder's chosen macroblock mode into prediction residuals, transformed and quantized coefficients, reconstructed samples and CBP/NNZ side information, and provides the fast "could this be a skip?" probe used by mode decision.

> **Dirty-side behavioural specification.** Written from the reference source and public literature for the clean-room reimplementation. No source code or near-verbatim pseudocode is reproduced; only behaviour, structure, constants and decision conditions are described.

---

## 1. Header

- Source path: `encoder/macroblock.c` (about 1425 lines).
- Role: the write side of a macroblock: lossless and lossy 16x16/8x8/4x4 intra encoding, inter residual encoding with 4x4 or 8x8 transforms, chroma encoding (including chroma DC Hadamard and decimation), skip-mode motion compensation, skip probing, noise-reduction parameter updates and RD-only partial-block encoders.
- The bitstream writers themselves live in `cabac.c`/`cavlc.c`; this file declares them in `macroblock.h` but does not implement them.

---

## 2. Purpose

Analysis (`analyse.c`) decides *what* to encode; this file performs the encoding. It must produce three things consistently:

1. The quantised coefficient arrays used by the entropy coder (`h->dct.luma4x4`, `luma8x8`, `luma16x16_dc`, `chroma_dc`).
2. The reconstructed image in `p_fdec`, used for deblocking, as reference for later frames, and for RD distortion measurement.
3. Side information (`i_cbp_luma`, `i_cbp_chroma`, NNZ cache, `h->mb.cbp[]`, skip/mv/reference caches) used by context selection in CABAC/CAVLC and by neighbouring MB prediction.

Because RD calls encode a candidate many times, the file also exposes "partial" encoders for one 8x8 or one 4x4 block, and supports a `b_skip_mc` flag to avoid repeating motion compensation that was already performed during analysis.

---

## 3. Public interface

| Symbol | Contract |
|---|---|
| `void x264_macroblock_encode(x264_t*)` | Encode the current MB according to `h->mb.i_type` and `i_partition`, including chroma and CBP storage. |
| `void x264_macroblock_encode_p8x8(x264_t*, int i8)` | RD-only: encode one 8x8 luma block (and its corresponding chroma) after motion compensation by `x264_mb_mc_8x8`; clears that block's CBP bit first. |
| `void x264_macroblock_encode_p4x4(x264_t*, int i4)` | RD-only: encode one 4x4 block (luma only for subsampled formats), no motion compensation. |
| `void x264_mb_encode_chroma(x264_t*, int b_inter, int i_qp)` | Encode both chroma planes with either 4:2:0 or 4:2:2 layout. |
| `int x264_macroblock_probe_skip(h, b_bidir)` | Cheaply decide whether the MB can be P_SKIP (b_bidir=0) or B_SKIP (b_bidir=1). Returns 1 on success and sets `b_skip_mc = 1`; returns 0 at the first failing test. |
| `void x264_predict_lossless_4x4 / _8x8 / _16x16 / _chroma` | Reproduce exact (qpel-free, source-neighbour) predictions for lossless coding, including the special vertical/horizontal cases that copy source pixels directly. |
| `void x264_noise_reduction_update(x264_t*)` | Recompute adaptive DCT-domain noise-reduction offsets from accumulated residual sums and counts. |
| Trellis quantisation entry points | Declared in `macroblock.h`, implemented in `rdo.c`; called through the inline `x264_quant_4x4`/`x264_quant_8x8` helpers. |

Static helpers include `mb_encode_i16x16`, `mb_encode_chroma_internal`, `macroblock_encode_skip`, `macroblock_encode_internal`, `macroblock_probe_skip_internal`, `array_non_zero`, `dct2x2dc`, `idct_dequant_2x2_dc`, `mb_optimize_chroma_dc`, `macroblock_encode_p8x8_internal`, `macroblock_encode_p4x4_internal`, and the 2x2/2x4 chroma DC scans and IDCTs.

---

## 4. State and data structures

- **Source/reconstruction planes:** `h->mb.pic.p_fenc[p]` (FENC_STRIDE) and `p_fdec[p]` (FDEC_STRIDE), plus full-frame planes `p_fenc_plane[p]` used by lossless prediction.
- **Coefficient storage:** `h->dct.luma4x4[16*planes+idx]`, `luma8x8[4*planes+idx]`, `luma16x16_dc[p]`, `chroma_dc[ch]`. Coefficients are stored zigzag-scanned; dequantisation and IDCT operate on natural-order arrays.
- **Side information:** `h->mb.cache.non_zero_count[x264_scan8[...]]` (0/1 for most blocks, exact count in CAVLC), `i_cbp_luma` (four bits per 8x8 quadrant), `i_cbp_chroma` (0 none, 1 DC only, 2 DC+AC), `h->mb.cbp[mb_xy]` (packed final value).
- **Intra reuse buffers:** `i4x4_fdec_buf`, `i4x4_nnz_buf`, `i4x4_cbp`, `i4x4_dct_buf` and the analogous `i8x8_*` buffers. `h->mb.i_skip_intra` values: 0 = no reuse, 1 = reuse reconstruction/NNZ/CBP, 2 = same plus restore DCT coefficients (needed in RD mode).
- **Noise reduction state:** `nr_residual_sum[cat][coef]`, `nr_count[cat]`, `nr_offset[cat][coef]` with four categories (4x4 luma, 8x8 luma, 4x4 chroma, and luma again for 4:4:4); separate "emergency" buffers for out-of-spec QPs.
- **Invariants:** all encoding functions must write correct CBP and NNZ even when CBP is zero (NNZ is used for CABAC context selection); NNZ need only be 0/1 outside CAVLC. Stored deblocking/reconstruction must be identical whether or not a fast path was taken.

---

## 5. Algorithm / behaviour

### 5.1 Chroma DC transforms and helper routines

Small fixed transforms are implemented locally because they have no SIMD counterparts:

- 2x2 chroma DC: a four-point Hadamard; the forward transform extracts DC from four 4x4 blocks, zeroing their DC coefficients; the inverse with dequantisation uses `dmf = dequant_mf[qp%6][0] << qp/6` and a `>>5` shift with the four butterfly sums/differences.
- 2x4 chroma DC (4:2:2): analogous eight-point transform using two 2x2 stages; separate forward (`dct2x4dc`) and inverse variants with/without embedding into 4x4 blocks.
- Scans: 2x2 DC zigzag order `(0,0),(0,1),(1,0),(1,1)` in raster-of-the-2x2; 2x4 DC order `{0,2,1,4,6,3,5,7}`.
- `array_non_zero` tests 64-bit or 32-bit words for any nonzero coefficient.
- Lossless intra predictors copy from the source frame rather than the reconstructed frame: for vertical/horizontal modes the row/column above/left comes from `p_fenc_plane` (with interlaced stride adjustment), otherwise the normal predictor is used. The 8x8 case takes top edge bytes from the precomputed `edge[36]` array.

### 5.2 I16x16 encoding

1. Predict the whole 16x16 luma block (lossless variant if enabled), then transform into sixteen 4x4 DCT blocks.
2. Extract the 16 DC values into a separate 4x4 DC block (reordered by block index mapping) and zero the AC blocks' DC entries.
3. Quantise the 16 AC blocks, either with per-block trellis or in groups of four with the SIMD "quant 4x4x4" primitive. Nonzero blocks set `block_cbp = 0xf` (all four 8x8 quadrants coded) and accumulate a decimation score from the scanned coefficients, but only while the score is below 6.
4. **DCT decimation:** if the accumulated score stays below 6, all 16 AC NNZ values are cleared and `block_cbp` reset. Rationale: writing 16 CBP bits is expensive, so weak residuals are discarded.
5. Transform/quantise the 4x4 DC block (with the DC-specific halved quantiser factor in the non-trellis path); store its NNZ. If DC is nonzero, reconstruct it (IDCT, dequantise, scatter into the 16 block DCs). Finally apply the 16x16 IDCT for AC if `block_cbp` is nonzero, otherwise a DC-only IDCT if DC survived.
6. In lossless mode the residual is computed directly in the zigzag domain, CBP is set from the per-block nonzero flags, and the DC block is stored separately.

### 5.3 Chroma encoding

`mb_encode_chroma_internal` first clears all chroma AC NNZ entries. It supports an **early termination** when the MB is inter, `b_dct_decimate` is set, QP >= 12 (trellis) or >= 18 (no trellis), and noise reduction is off:

- Threshold from `lambda2` at the chroma QP: `(lambda2 + 32) >> 6` for 4:2:0 or `(lambda2 + 16) >> 5` for 4:2:2.
- A variance/SSD primitive compares source and prediction; if the summed variance over both planes is below `4*threshold`, encode only DC: compute the 2x2/2x4 chroma DC transform, quantise DC (with a +3 QP offset in 4:2:2), optionally optimise DC rounding, apply DC-only inverse and set `i_cbp_chroma = 1`, then return.

Otherwise, for each chroma plane:

1. Compute 4x4 DCTs of each 4x4 sub-block (8 in 4:2:2), apply noise reduction if enabled, and fold DC into the chroma DC transform.
2. Quantise AC blocks (trellis or SIMD 4-at-a-time), accumulating a decimation score capped at 7.
3. Quantise the DC block; if AC decimation score < 7 or DC-only survived, drop all AC coefficients and keep only DC (with the same DC rounding optimisation). Otherwise set `i_cbp_chroma = 2` and reconstruct the full chroma block.
4. `mb_optimize_chroma_dc` performs an exhaustive lossless rounding search for DC-only blocks when the dequantisation factor is at most `32*64`; above that QP it returns immediately. It calls a SIMD routine specialised for 2x2 or 2x4 layout.
5. Final CBP: 0 none, 1 DC only, 2 DC+AC, computed by ORing DC NNZ flags with the AC-derived value.

### 5.4 Full macroblock encode flow

`macroblock_encode_internal(h, plane_count, chroma)` (called with 3 planes for 4:4:4, 1 plane plus chroma for subsampled formats, 1 plane for monochrome):

1. Initialise luma CBP and DC NNZ. For **I_PCM**, copy source samples straight into `p_fdec` and return — no coefficients.
2. If `b_allow_skip` is false, force skip types back to their non-skip equivalents (P_SKIP → P_L0, B_SKIP → B_DIRECT) and remember not to re-apply skip detection later.
3. **P_SKIP:** unless `b_skip_mc` is set, perform full luma MC from ref 0 with clipped MV, and chroma MC with the special zero-MV path (direct load/deinterleave when MV is zero), then apply weighted prediction if configured. Clear all NNZ/CBP and return.
4. **B_SKIP:** rely on `x264_mb_mc` (already done in analysis unless `b_skip_mc`), clear NNZ/CBP and return.
5. **I_16x16:** force 4x4 transform, call `mb_encode_i16x16` for each luma plane (chroma QP for 4:4:4 planes).
6. **I_8x8:** force 8x8 transform; if `i_skip_intra` is set, restore 3 of 4 previously encoded blocks from the reuse buffers (and restore DCT data when `i_skip_intra == 2`); encode the remaining 8x8 blocks from cached prediction modes.
7. **I_4x4:** analogous reuse of 15 blocks; emulate missing top-right samples by repeating the top-right pixel when only the top neighbour exists; encode each 4x4 block.
8. **Inter:** skip MC if already done, then either compute residuals directly in zigzag domain (lossless), use 8x8 transforms (with decimation and trellis; note that 8x8 trellis with CABAC is already an optimal decimation, so explicit decimation is disabled), or use 4x4 transforms. In the 4x4 path each 8x8 quadrant accumulates a decimation score capped at 6; quadrants with score < 4 are zeroed, and if the whole MB's accumulated score is < 6 the entire plane is zeroed. Only surviving blocks are dequantised and added back via IDCT.
9. Encode chroma (with intra prediction first for intra MBs).
10. Pack `cbp = chroma<<4 | luma`, and for CABAC add the luma/chroma DC NNZ bits at positions 8/9/10; store in `h->mb.cbp[]`.
11. **Skip re-detection:** if allowed, P_L0 with 16x16 partition, no residual, MV equal to the P-skip predictor and ref 0 becomes P_SKIP; B_DIRECT with no residual becomes B_SKIP.

### 5.5 Skip probing

`macroblock_probe_skip_internal` performs the cheapest possible sequence of tests, returning 0 as soon as a residual would survive:

1. For P-skip, motion-compensate luma with the P-skip MV (or rely on the already-completed MC for B-skip).
2. For each luma 8x8 quadrant, compute a 4x4 DCT, apply noise reduction if active, quantise the four blocks with the ordinary deadzone quantizer, and accumulate `decimate_score16` over nonzero blocks; a cumulative score >= 6 fails the probe.
3. For chroma (4:2:0/4:2:2), motion-compensate (with the zero-MV shortcut) and apply weights. Per plane compute SSD first; if it is below `threshold`, skip further work. Otherwise compute DC-only transform and quantise DC — any nonzero DC fails. If SSD is below `4*threshold`, accept the plane without AC checking; else compute AC DCTs and fail when the decimation score reaches 7.
4. On success set `b_skip_mc = 1` so the subsequent encode does not repeat MC, and return 1.

The SSD thresholds keep most chroma cases cheap: the source comments note that termination during chroma is rare, so an SSD pre-check avoids the actual transform.

### 5.6 RD-only partial encoders

`macroblock_encode_p8x8_internal` encodes one 8x8 cell (luma and, for subsampled formats, its chroma counterpart). It clears the cell's CBP bit, performs MC via `x264_mb_mc_8x8` unless `b_skip_mc`, then:

- In lossless mode, computes residuals directly in scan order (8x8 or four 4x4s) and sets chroma CBP to 2.
- In 8x8-transform mode, quantises one 64-coefficient block; decimation requires a `decimate_score64 >= 4`, unless trellis+CABAC makes it unnecessary.
- In 4x4-transform mode, quantises four blocks, decimating the 8x8 when `decimate_score16` sums below 4; chroma AC coefficients are computed per 4x4 with a 4:2:2 index remapping trick and reconstructed immediately.

`macroblock_encode_p4x4_internal` is strictly luma-only (for 4:2:0 RD), performs no MC, and produces one quantised 4x4 block plus its reconstruction.

### 5.7 Noise reduction update

For each coefficient category, if the accumulated count exceeds `1<<16` (8x8) or `1<<18` (4x4), the residual sums and count are halved to avoid overflow. Each offset is then computed as `NR_strength * count + residual_sum/2` divided by `residual_sum * weight/256 + 1`; the DC offset is forced to zero so DC is never denoised. Out-of-spec QPs use precomputed "emergency" offsets instead.

---

## 6. Dependencies

- **Called by:** `analyse.c` (via `rd_cost_mb`, `rd_cost_i4x4`, etc., and for skip probing), `encoder.c` (final encode), `me.c` indirectly through RD.
- **Calls:** prediction functions `h->predict_4x4/8x8/16x16/chroma`, `x264_predict_lossless_*`, `x264_mb_mc`, `x264_mb_mc_8x8`, `x264_macroblock_cache_*`, `h->quantf.*` (quant/dequant/denoise/decimate/optimize_chroma_dc), `h->dctf.*` (DCT/IDCT/Hadamard), `h->zigzagf.*`, `h->pixf.var2/ssd`, trellis quantisers from `rdo.c`, `x264_lambda2_tab`.
- **Shared config:** `b_trellis`, `b_lossless`, `b_noise_reduction`, `b_dct_decimate`, `b_deblock_rdo`, `b_allow_skip`, `b_skip_mc`, `i_skip_intra`, QP/chroma QP, chroma format and interlacing.

---

## 7. Constants and tables

- Decimation thresholds: I16x16 score < 6, chroma AC score < 7, inter 4x4 score < 6 per 8x8 and < 6 per MB, with per-quadrant cutoff 4; inter 8x8 `decimate_score64 >= 4`; p8x8 RD 4x4 threshold 4.
- Chroma early-termination QP thresholds: 12 with trellis, 18 without; SSD threshold `(lambda2+32)>>6` (4:2:0) or `(lambda2+16)>>5` (4:2:2), with `4*threshold` as the "skip AC" bound.
- `mb_optimize_chroma_dc` cut-off: dequant factor > `32*64` disables optimisation.
- Chroma DC QP offset: +3 for 4:2:2.
- Noise-reduction halving counts `1<<16` / `1<<18`; IDCT butterfly shift `>>5`.
- `i_skip_intra` values 0/1/2 as described; P_SKIP/B_SKIP detection conditions listed in §5.4.

---

## 8. Performance notes

1. **Skip probing is the biggest fast-path win in P/B frames** [1]. The staged tests (luma decimation with early fail, chroma SSD pre-check, DC-only before full AC) must stay in this order; reordering changes both speed and which MBs are skipped.
2. **Intra block reuse.** `i_skip_intra` saves 15 of 16 (or 3 of 4) predictions/quantisations when analysis already encoded the block; RD mode needs the DCT buffer restore to keep CABAC contexts identical.
3. **SIMD quantisation and transforms.** `quant_4x4x4`, `decimate_score15/16/64`, DCT/IDCT and the chroma DC optimiser are SIMD-accelerated; a scalar fallback is slower but must be numerically identical [2].
4. **Decimation trade-off.** Dropping weak AC coefficients saves CBP bits and context work at a small PSNR cost; the fixed scores (6, 7, 4) are empirical. Disabling decimation improves quality slightly but changes the bitstream and slower CABAC contexts.
5. **Reuse of MC.** `b_skip_mc` avoids a second interpolation pass in RD; the flag must be restored on every exit path in `me.c`.
6. **Concurrency.** Encoding mutates `h->mb` and frame reconstruction planes; separate encoder contexts per thread are required. Read-only lambdas and tables can be shared [3].
7. **Cache locality.** NNZ entries are written in four-byte chunks using `x264_scan8` so CABAC state lookups are contiguous; changing the scan layout would break both speed and bitstream equivalence.

---

## 9. References

[1] x264 developer notes on fast P-skip/B-skip and DCT decimation (codex.wordpress.com, x264 mailing lists).

[2] SIMD implementations in `common/x86/`, `common/arm/`; the C reference functions in `common/dct.c` and `common/quant.c` define numerical behaviour.

[3] x264 frame-threading architecture documentation.

[4] ITU-T Rec. H.264 / ISO-IEC 14496-10 — macroblock prediction, CBP semantics, chroma DC Hadamard transform, I_PCM.

[5] "Adaptive deadzone / DCT-domain noise reduction" (libavcodec-derived technique, credited in source comments).

---

## 10. Verification checklist

1. For every MB type, the reconstructed `p_fdec` matches after a full encode vs. an encode that took a skip/intra-reuse path.
2. CBP and NNZ caches are correct even when CBP is zero (check CABAC context selection on neighbouring blocks).
3. Chroma CBP transitions 0→1→2 match the presence of DC and AC coefficients.
4. I_PCM copies all samples including chroma and produces no coefficients.
5. Lossless paths produce bit-exact reconstruction and correct zigzag residuals.
6. Skip probe returns 1 exactly when a full encode would produce all-zero coefficients with the skip MV/prediction; verify at QP boundaries 12 and 18.
7. Noise-reduction halving occurs at the documented counts; DC offset is always zero.
8. P_SKIP/B_SKIP re-detection at the end of encoding matches the probe decision.
9. With `b_transform_8x8` toggled, the chosen transform does not change reconstruction outside quantisation differences.
10. RD partial encoders clear and restore the correct CBP bit for their 8x8 cell and leave neighbouring NNZ untouched.

---

## 11. Open questions

- **UNKNOWN:** the empirical derivation of every decimation threshold (6, 7, 4) and of the chroma SSD multipliers; only outcomes are visible.
- **UNKNOWN:** exact bit-exactness of SIMD `quant_4x4x4`, `denoise_dct` and `optimize_chroma_*_dc` versus their C versions on all supported platforms.
- **UNKNOWN:** how the "emergency" noise-reduction offsets for out-of-spec QPs are generated (computed elsewhere in `set.c`/init code, not in this file).
- **UNKNOWN:** whether `i_skip_intra == 1` ever leaves stale DCT coefficients that affect CABAC context in a non-RD encode; the source comments flag a similar issue for sub8x8 RD in `analyse.c`.
- **UNKNOWN:** precise semantics of weighted chroma MC in the B-skip probe when both lists use different weights; only the helper calls are visible here.
