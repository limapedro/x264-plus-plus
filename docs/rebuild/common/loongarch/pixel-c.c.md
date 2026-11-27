# `common/loongarch/pixel-c.c` — LoongArch Hadamard wrappers and intra metric helpers

**Dirty-side behavioural specification.** Source path: `E:\_x264_dirty_stash\x264-source\common\loongarch\pixel-c.c`. Role: C-side wrappers that convert the raw Hadamard-AC core results into x264's packed format and assemble the intra prediction + distortion combined helpers from the LSX predictors and LSX/LASX metrics. Compiled only for 8-bit builds.

## 1. Header

The title line above gives the source path and the file's role in one sentence; the adjacent bold marker identifies this document as a **dirty-side behavioural specification** written from the reference source and public literature. No reference source or near-verbatim pseudocode is reproduced.

## 2. Purpose

The heavy 8x8 Hadamard and metric kernels live in `pixel-a.S` and `sad-a.S`. This file supplies the small amount of C glue needed to (a) scale and pack the Hadamard-AC result into a `uint64_t` with AC in the low word and DC in the high word, and (b) generate the three-candidate intra decisions used by macroblock analysis, reusing the appropriate predictor and metric symbols.

## 3. Public interface

**Hadamard-AC wrappers**
- `x264_pixel_hadamard_ac_8x8_lsx`, `8x16_lsx`, `16x8_lsx`, `16x16_lsx` and the four `_lasx` analogues: `uint64_t (uint8_t *p_pix, intptr_t i_stride)`.

**Intra combined helpers**
- `x264_intra_sa8d_x3_8x8_lsx/_lasx(uint8_t *p_enc, uint8_t edge[36], int32_t sad[3])`
- `x264_intra_satd_x3_4x4_lsx`, `_16x16_lsx`, `_16x16_lasx`, `_8x8c_lsx(uint8_t*, uint8_t*, int32_t[3])`
- `x264_intra_sad_x3_4x4_lsx`, `_16x16_lsx`, `_8x8_lsx(p_enc, edge[36], sad[3])`, `_8x8c_lsx`

All names are mangled with `x264_template`. Return values: Hadamard functions pack two 32-bit costs; intra helpers fill `sad[0..2]` in the order V, H, DC (or DC-4blk, H, V for the 8x8c case).

## 4. State and data structures

No globals. Each 8x8 edge-array helper allocates a 16-byte-aligned scratch block `uint8_t pix[8 * FDEC_STRIDE]` (8 rows at reconstruction stride) using `ALIGNED_ARRAY_16`. The non-edge helpers predict directly into `p_dec`.

## 5. Algorithm / behaviour

### Hadamard-AC packing

Each wrapper calls the corresponding 8x8 core (e.g. `x264_hadamard_ac_8x8_lsx`) once per 8x8 sub-block and sums the raw `uint64_t` results:

- 8x8: one core at `p_pix`.
- 8x16: cores at `p_pix` and `p_pix + 8*i_stride`.
- 16x8: cores at `p_pix` and `p_pix + 8`.
- 16x16: cores at `p_pix`, `+8`, `+8*i_stride`, `+8*i_stride+8`.

The summed value is transformed by `((sum >> 34) << 32) + ((uint32_t)sum >> 1)`: the low word is the AC cost halved, the high word is the DC cost quartered. This matches x264's `x264_pixel_hadamard_ac_*` packing convention.

### Intra combined helpers

Each helper performs three predict-then-score steps:

1. **4x4 SAD/SATD:** call `x264_predict_4x4_v_lsx(p_dec)`, score with `x264_pixel_sad_4x4_lsx`/`x264_pixel_satd_4x4_lsx(p_dec, FDEC_STRIDE, p_enc, FENC_STRIDE)`; repeat with `..._h_lsx` and `..._dc_lsx`. Results in `sad[0]=V, [1]=H, [2]=DC`.
2. **16x16 SAD/SATD:** same pattern with `x264_predict_16x16_*_lsx` and the matching 16x16 metric; the LASX SATD variant uses LSX predictors and the LASX metric.
3. **8x8 (edge array):** allocate `pix`, call `x264_predict_8x8_v_lsx(pix, edge)`, score with `x264_pixel_sad_8x8_lsx` or `x264_pixel_sa8d_8x8_lsx/lasx`; repeat with `..._h_lsx/lasx` and `..._dc_lsx`.
4. **8x8c:** call `x264_predict_8x8c_dc_lsx`, `..._h_lsx`, `..._v_lsx` (in that order) and score with the 8x8 SATD or SAD metric; results in `sad[0]=DC, [1]=H, [2]=V`.

The LASX SA8D x3 helper uses `x264_predict_8x8_v_lsx` and `x264_predict_8x8_dc_lsx` (LSX) with the LASX SA8D metric — a deliberate mixed-ISA choice.

## 6. Dependencies

- **Calls:** the 8x8 Hadamard cores `x264_hadamard_ac_8x8_lsx/_lasx` from `pixel-a.S`, the predictors from `predict.h`, and the SAD/SATD/SA8D functions from `sad-a.S`/`pixel-a.S`.
- **Called by:** `x264_pixel_init()` in `common/pixel.c`.
- **Requires:** `ALIGNED_ARRAY_16`, `FDEC_STRIDE`, `FENC_STRIDE`, `x264_template`, `pixel` types.

## 7. Constants and tables

- Packing shifts: `>>34` for DC cost, `>>1` for AC cost.
- 8x8 tile offsets: 8 bytes horizontally, `8*stride` vertically.
- Scratch block size: `8 * FDEC_STRIDE` bytes.

## 8. Performance notes

- **Hotness:** moderate; invoked for intra mode decisions, more often at slow presets.
- **Optimization tips:** (a) the scratch buffer is stack-local and aligned — keep it; (b) the three predictions write the same buffer, so the first metric can start while later predictions are generated only if the compiler reorders (not guaranteed) — a fused assembly loop could improve throughput; (c) reuse LSX predictors even in LASX helpers, as the current code does, to avoid duplicating predictor implementations.
## 9. References

x264 `common/pixel.c`, `common/predict.c`; ITU-T H.264 §8.3/§8.4.

## 10. Verification checklist

1. For each Hadamard size, compare packed AC/DC words with the scalar `x264_pixel_hadamard_ac_*_c`.
2. For an all-zero 8x8 block, verify the raw core returns a result whose packed form is zero.
3. For each intra helper, compare the three output costs with a manual scalar predict-then-metric sequence.
4. Verify predictor order for 8x8c (DC, H, V) against the generic dispatcher's expectation.
5. ASan-check the aligned scratch buffer and edge array (36 bytes, no over-read).
6. Confirm all wrappers are compiled out in high-bit-depth builds.

## 11. Open questions

- UNKNOWN: exact semantic difference between the raw `x264_hadamard_ac_8x8_*` output and the packed `x264_pixel_hadamard_ac_*` output beyond the two shifts; validate with a non-uniform 8x8 pattern.
- UNKNOWN: why the LASX SA8D x3 helper uses LSX predictors while the LASX SATD x3 helper does too — likely intentional reuse, but confirm no LASX predictor symbols were intended.
