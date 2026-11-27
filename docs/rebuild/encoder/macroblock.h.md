# `encoder/macroblock.h` — macroblock encoding interface

**Role (one sentence):** This header declares the encoding-side macroblock entry points, the inline 4x4/8x8 quantization helpers and the NNZ-cache manipulation macros shared by analysis, RD and bitstream writing.

> **Dirty-side behavioural specification.** Written from the reference source and public literature for the clean-room reimplementation. No source code or near-verbatim pseudocode is reproduced; only behaviour, structure, constants and contracts are described.

---

## 1. Header

- Source path: `encoder/macroblock.h` (about 215 lines).
- Role: glue between `analyse.c`, `rdo.c`, `me.c`, `encoder.c` and the entropy writers. It includes `common/macroblock.h`, so the clean implementation inherits the MB type/partition enumerations, scan tables and cache helpers defined there.
- Each exported function is wrapped in `x264_template(...)`.

---

## 2. Purpose

The header exists for three reasons: to declare the encoder-side operations that `analyse.c` and `rdo.c` call; to inline the hot quantisation decision (trellis vs deadzone, with noise reduction) so no function call is needed on the encode path; and to provide compact, branch-free manipulation of the nonzero-count cache and of sparse bit masks (the `FOREACH_BIT` loop).

---

## 3. Public interface

### Declared functions

| Symbol | Contract |
|---|---|
| `void x264_rdo_init(void)` | Initialise the CABAC bit-cost tables used by trellis quantisation. |
| `int x264_macroblock_probe_skip(h, int b_bidir)` | Probe whether the current MB can be P_SKIP (`b_bidir=0`) or B_SKIP (`b_bidir=1`). `x264_macroblock_probe_pskip(h)` / `_bskip(h)` are aliases. |
| `void x264_predict_lossless_4x4(h, pixel *dst, int p, int idx, int mode)` | Exact source-based 4x4 intra prediction for lossless coding. |
| `void x264_predict_lossless_8x8(h, pixel *dst, int p, int idx, int mode, pixel edge[36])` | Same for 8x8; `edge` carries already-filtered neighbours. |
| `void x264_predict_lossless_16x16(h, int p, int mode)` | Same for a whole 16x16 plane. |
| `void x264_predict_lossless_chroma(h, int mode)` | Same for both chroma planes. |
| `void x264_macroblock_encode(h)` | Full MB encode. |
| `void x264_macroblock_write_cabac(h, x264_cabac_t *cb)` / `x264_macroblock_write_cavlc(h)` | Entropy writers, implemented in `cabac.c` / `cavlc.c`. |
| `void x264_macroblock_encode_p8x8(h, int i8)` / `x264_macroblock_encode_p4x4(h, int i4)` | RD-only partial encoders. |
| `void x264_mb_encode_chroma(h, int b_inter, int i_qp)` | Chroma encode. |
| `void x264_cabac_mb_skip(h, int b_skip)` | Write/measure the MB skip flag. |
| `x264_cabac_block_residual_c`, `..._rd_c`, `..._8x8_rd_c` | Scalar fallback residual coder variants; SIMD versions are dispatched elsewhere. |
| `x264_quant_luma_dc_trellis`, `x264_quant_chroma_dc_trellis`, `x264_quant_4x4_trellis`, `x264_quant_8x8_trellis` | Trellis quantisers implemented in `rdo.c`. |
| `void x264_noise_reduction_update(h)` | Refresh adaptive DCT-domain denoising offsets. |

### Inline helpers

- **`x264_quant_4x4(h, dct[16], qp, ctx_block_cat, b_intra, p, idx)`** selects the quantizer category from the plane and intra flags (`CQM_4IY`/`CQM_4IC` for intra luma/chroma, `CQM_4PY`/`CQM_4PC` for inter), optionally denoises the DCT using the plane-specific residual-sum/offset buffers, then either calls the trellis quantiser (with block index `idx + p*16`) or the SIMD deadzone quantizer with the QP's factor and bias. Returns whether any coefficient is nonzero.
- **`x264_quant_8x8(h, dct[64], ...)`** is the analogue for 64-coefficient blocks using the 8x8 categories and `idx + p*4` as the trellis index.

### Macros

- **`STORE_8x8_NNZ(p, idx, nz)`** writes the same 0/1 value into the four 2x2 NNZ slots that make up an 8x8 block, using two aligned 16-bit writes.
- **`CLEAR_16x16_NNZ(p)`** clears all sixteen 4x4 NNZ entries of one plane with four aligned 32-bit writes.
- **`FOREACH_BIT(idx, start, mask)`** is a branchless loop over the set bits of a four-bit mask: it finds the lowest set bit with a count-trailing-zeros helper, advances the index by that amount, clears the bit and repeats; the loop body sees only indices whose bit was set.
- **`x264_mb_encode_i4x4` / `x264_mb_encode_i8x8`** (inline): predict (unless `b_predict` is false), transform residual, quantise, store zigzag coefficients and NNZ/CBP, dequantise and add back the IDCT. Lossless mode skips transform/quantisation entirely and computes the residual directly in scan order. The 8x8 variant accepts precomputed `edge` samples and filters them when absent.

---

## 4. State and data structures

The header itself defines no structs, but its helpers operate on:

- `h->mb.b_trellis`, `b_lossless`, `b_noise_reduction`, `i_cbp_luma`;
- `h->quant4_mf`, `quant4_bias`, `quant8_mf`, `quant8_bias`, `dequant4_mf`, `dequant8_mf`, indexed by quantizer category and QP;
- `h->nr_residual_sum[0..3]`, `nr_offset[0..3]`, indexed by plane and transform size;
- `h->mb.cache.non_zero_count` through the `x264_scan8[]` mapping (block index → cache slot), and `h->dct.luma4x4` / `luma8x8`;
- `h->mb.pic.p_fenc[p]` / `p_fdec[p]` at `FENC_STRIDE` / `FDEC_STRIDE`.

**Ownership:** all buffers are owned by the encoder context; the inline helpers mutate DCT arrays in place (they are destroyed by quantisation and reconstructed) and write cache entries. Alignment attributes on local arrays (`ALIGNED_ARRAY_64`) are required by the SIMD transform/quant functions.

---

## 5. Algorithm / behaviour

1. **Quantizer-category selection** follows a 2x2 matrix: intra vs inter, luma vs chroma, and 4x4 vs 8x8 transform. Custom quantization matrices change the contents of those arrays but not the selection rule.
2. **Noise reduction** is applied to the DCT coefficients before quantisation, with separate accumulators for 4x4/8x8 and luma/chroma. It must be applied identically in the probe path and the final encode path.
3. **Trellis vs deadzone:** trellis is chosen when `b_trellis` is set; the block index passed to the trellis routine is used to fetch source DCT coefficients for psy-trellis and to select the CABAC context. In deadzone mode the SIMD quantizer receives the factor/bias pair for the current QP and category.
4. **Intra block encoding** order: prediction → (optional) lossless residual or DCT → quantisation → NNZ/CBP update → zigzag scan → dequantisation → IDCT added to the prediction. When the block quantises to zero, no IDCT is performed; the reconstruction remains the prediction.
5. **8x8 NNZ replication:** H.264 signals an 8x8 block's coded status through four 4x4 NNZ positions, so `STORE_8x8_NNZ` writes the same value to all four. `CLEAR_16x16_NNZ` is used before a plane is re-encoded (e.g. RD trial) to avoid stale context.
6. **`FOREACH_BIT`** preserves ascending index order and visits exactly the set bits; callers rely on this for deterministic scanning and for accumulating decimation scores in block order.

---

## 6. Dependencies

- Includes `common/macroblock.h` and thereby `common/common.h`; uses `x264_t`, `pixel`, `dctcoef`, `x264_cabac_t`, `x264_weight_t`, scan/enumeration tables.
- Included by `encoder/analyse.c`, `encoder/me.c`, `encoder/rdo.c`, `encoder/encoder.c` and the entropy writers.
- Calls into `rdo.c` for trellis, into `common/dct.c`/`common/quant.c` via function pointers, and into `analyse.c`-provided analysis results for prediction modes.

---

## 7. Constants and tables

- Quantizer categories: `CQM_4IY`, `CQM_4IC`, `CQM_4PY`, `CQM_4PC`, `CQM_8IY`, `CQM_8IC`, `CQM_8PY`, `CQM_8PC` (defined in common headers).
- Plane multipliers in trellis indices: `p*16` for 4x4 blocks, `p*4` for 8x8 blocks.
- Alignment: 64 bytes for DCT arrays and `x264_quant_*`, 8 bytes for MVs, 4 bytes for packed writes into NNZ.
- `b_predict` flag: when false the caller has already written the prediction into `p_fdec` (used when analysis cached it).
- `edge[36]`: 17 top + 17 left + 2 corner samples for 8x8 prediction.

---

## 8. Performance notes

1. Inlining the quantizer choice removes a branch from the hottest encode loop; the branch is nevertheless predictable per MB [1].
2. Writing NNZ values in 32/16-bit chunks rather than byte by byte reduces store instructions and keeps cache contexts aligned [2].
3. `FOREACH_BIT` avoids a branch per 4x4 position when only one or two blocks are nonzero — common at high QP [3].
4. The inline intra block encoders keep prediction, transform and quantisation in one place so the compiler can allocate DCT arrays in registers/aligned stack slots; preserving the alignment is required for SIMD.
5. Concurrency: these helpers only touch the current MB's context state; safe with one encoder context per thread [4].

---

## 9. References

[1] x264 source documentation on preset-dependent trellis use and quantizer categories.

[2] ITU-T Rec. H.264 — nonzero-count syntax and CABAC context selection.

[3] General branchless bit-iteration techniques (count-trailing-zeros).

[4] x264 frame-threading documentation.

---

## 10. Verification checklist

1. For each of the eight quantizer categories, deadzone and trellis quantisation return the same nonzero flag as the reference for a fixed coefficient vector and QP.
2. Noise reduction changes coefficients only when `b_noise_reduction` is set and never touches DC.
3. `STORE_8x8_NNZ` sets all four slots identically; `CLEAR_16x16_NNZ` clears exactly 16 slots.
4. `FOREACH_BIT` visits set bits in ascending order and exactly once each, for all 16 masks.
5. Lossless intra encoders reproduce reconstruction identical to source for vertical/horizontal modes.
6. Inline 4x4/8x8 encoders leave `p_fdec` unchanged for all-zero blocks (prediction only).
7. Trellis block indices for chroma and 4:4:4 planes are offset by `p*16`/`p*4` as documented (observable via psy-trellis source-DCT selection).

---

## 11. Open questions

- **UNKNOWN:** exact values of the quantizer-category enumerations and custom-QM interactions; they are defined in common headers not assigned to this document.
- **UNKNOWN:** whether `x264_cabac_block_residual_*_c` fallbacks are bit-exact with SIMD residual coders on all platforms; only their role is declared here.
- **UNKNOWN:** the intended use of `b_predict = 0` in the inline intra encoders outside RD analysis; callers not in the assigned files may rely on additional invariants.
