# Chroma reconstruction — derivation and self-check (2026-11-28)

## Normative chain (ITU-T Rec. H.264, 01/2012 edition)

For 4:2:0 (`ChromaArrayType == 1`) the decoder processes each chroma DC 2x2
block as follows:

1. **8.5.4 / Eq. 8-304** — `ChromaDCLevel[0..3]` are mapped by the inverse
   raster scan to `c00, c01, c10, c11`.
2. **8.5.11.1 / Eq. 8-324** — `f = A * c * A^T`, `A = [[1,1],[1,-1]]`:
   ```
   f00 = c00+c01+c10+c11
   f01 = c00-c01+c10-c11
   f10 = c00+c01-c10-c11
   f11 = c00-c01-c10+c11
   ```
3. **8.5.11.2 / Eq. 8-326** — `dcCij = (fij * LevelScale4x4(qP%6,0,0))
   << (qP/6) >> 5`. `LevelScale4x4` includes the default `ScalingList4x4`
   value 16 (8.5.9), i.e. `16 * {10,11,13,14,16,18}[qP%6]`.
4. **Figure 8-7 / Eq. 8-335 / 8.5.12** — `dcC00..dcC11` become the DC
   coefficients of `chroma4x4BlkIdx 0..3`; they are not scaled again by
   8.5.12.1, and the inverse 4x4 transform adds `(dcC+32)>>6` to each sample.

The encoder needs `dcC_k = 4 * cv_k`, where `cv_k` is the forward 4x4 DCT DC
coefficient of chroma block `k` (`cv_k = 16*r` for a constant residual `r`,
while the decoder's DC-only IDCT adds `dcC_k/64 = r`).

## Defects found and fixed in `src/core/encode.c`

1. **Hadamard index order (dominant chroma-DC defect).** `dct.c`'s generic
   2x2 butterfly emits `[f00, f10, f01, f11]` — its second output is the
   *column* difference, its third the *row* difference. The decoder's `dcC`
   raster order is `[f00, f01, f10, f11]`. Using the raw butterfly swapped
   chroma blocks 1 and 2, turning a known pattern into a checkerboard.
   `hadamard2_dc` / `hadamard2_dc_inv` now reorder both directions.
2. **Cr neighbours taken from Cb.** `gather_neighbors()` was called once with
   `plane = 1` and the same prediction was applied to both chroma planes.
   Every directional/DC prediction of V therefore used U neighbour samples,
   inflating the Cr residual and compounding down each row of macroblocks
   (decoded V climbed 141 -> 173 -> 207 -> 246 -> 255 over the first five
   MBs). Chroma prediction now gathers U and V neighbours separately and
   scores one shared `intra_chroma_pred_mode` against both correct predictors.
3. **CAVLC `TotalCoeff == 1` (clause 9.2.1).** A lone chroma-DC level is
   always decoded as +/-1, so a nearly uniform plane cannot signal a large
   offset. `chroma_dc_cavlc_fix()` adds one opposite-signed trailing one when
   a single level has magnitude > 1.
4. **Rounding / QP table.** Sign-preserving `round_div(x,16)` replaces
   asymmetric truncation; chroma QP uses ITU-T Table 8-15 directly (the
   table in `quant.c` differs for `qPI >= 45`).

The `>>4` before `quant2` and `*16` before the Hadamard inverse are retained:
`quant.c` dequantises with `v[qP%6] << (qP/6-4)`, omitting the default
scaling-list factor 16 that the decoder applies in 8.5.11.2. The encoder's
mirror of the decoder must restore that factor; deleting either factor was
verified to break reconstruction.

`X264PP_SLICE_QP_DELTA` is now a compile-time override so the QP 23 and QP 45
operating points can be measured without touching the default build.

## Self-check

`chroma_recon_check.py` writes a single 16x16 I420 frame with Y=128 and
asymmetric chroma quadrants U=`64,128,192,64`, V=`128,64,192,128`, encodes it
with the gated path at decoder QP 23 (`--crf 23` plus
`-DX264PP_SLICE_QP_DELTA=0`), decodes it with ffmpeg, and compares the four
quadrant values to the source with a tolerance of 4 samples. Any index swap
or 4/16 scaling error changes the quadrants dramatically.

```
make -f src/Makefile BUILD_DIR=build-gate0 \
    CPPFLAGS="-DX264PP_FORCE_PCM=0 -DX264PP_SLICE_QP_DELTA=0"
python src/verification/chroma_recon_check.py --encoder build-gate0/x264pp.exe
```

Result after the fix:

```
expected U: 64 128 192 64     decoded U: 64 129 192  64
expected V: 128 64 192 128    decoded V: 128 65 191 128
max absolute chroma error: 1 -> PASS
```

## Measured end-to-end result (8x1920x1080 Fortnite I420)

| decoder QP | file size | Y | U | V | average |
| --- | --- | --- | --- | --- | --- |
| 23 (before) | 2,204,665 B | 20.32 | 23.80 | 7.87 | 14.67 dB |
| 23 (after)  | 2,009,600 B | 16.86 | 31.27 | 31.38 | 18.54 dB |
| 45 (before) | 390,087 B | 14.97 | 20.72 | 7.75 | 12.96 dB |
| 45 (after)  | 302,691 B | 14.92 | 27.29 | 29.59 | 16.58 dB |

V-plane PSNR improves by 23.5 dB at QP23 and 21.8 dB at QP45; the 25 dB
*average* target is not reached because luma remains at 14.9-16.9 dB (the
empirical `ac_scale48` luma AC scaling is a separate, unchanged limitation).
The oracle reference is 42.64 dB. All four streams decode with zero ffmpeg
errors and probe as 1920x1080 yuv420p. The default I_PCM build is unchanged
and still hashes to `83C71EBCC7A01E991F052CDDFC8BFE8E7B4BA27DC7A498A103DC7EE8C00F0DB1`.
