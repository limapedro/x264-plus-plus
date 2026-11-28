# I_NxN neighbour-context integration (feat/cavlc-neighbours)

**Status: acceptance NOT met.** The I_PCM fallback is retained. This branch adds
a complete, source-level I_NxN CAVLC path and fixes the frame-crop unit bug;
the residual path is disabled because an independent decoder still rejects it
on textured input.

## 1. What was implemented

* `src/core/params.c`: for `frame_mbs_only_flag=1` and 4:2:0 the crop offsets
  are now divided by two (H.264 7.4.2.1.3). 1920x1080 now reports 1920x1080
  instead of 1920x1072.
* `src/core/encode.h` / `encode.c`:
  * per-block neighbour fetching (`gather_neighbors4`) with explicit
    top / left / top-left / top-right availability across macroblock boundaries
    and the standard top-right / bottom-left edge replication;
  * H.264 8.3.1.1 `prev_intra4x4_pred_mode_flag` / `rem_intra4x4_pred_mode`
    derived from the real best mode and the left/above modes (DC when either
    neighbour is unavailable);
  * `nC` from left/top non-zero counts using the decoder's rule
    `(nA+nB+1)>>1` when both are present, the single count when one is present
    and 0 when neither is present;
  * `coded_block_pattern` via `x264pp_cavlc_cbp_code()` (Table 9-4 inverse),
    chroma DC for `CodedBlockPatternChroma > 0` and chroma AC only for value 2,
    all residuals through the fixed `x264pp_cavlc_residual()` writer.
  * reconstruction into `fdec` before residual addition, so later blocks and
    later macroblocks predict from the decoder's own samples.
* `X264PP_FORCE_PCM 1` at the top of `encode.c` selects the known-good fallback.

## 2. Three fix iterations, and the first failing block

1. **Iteration 1 — first I_NxN build.** ffmpeg: `top block unavailable for
   requested intra mode -1` at MB 0 0; diag: invalid `coeff_token` and
   `run_before=12 > zerosLeft=11` in `CbAC[0]`.
   Fixes: chroma DC is coded for `cbp_chroma > 0` (2 means DC **and** AC), and
   `x264pp_pred8c` uses V/H/DC/plane order while H.264
   `intra_chroma_pred_mode` uses DC/H/V/plane (`chroma_signal_mode`).
2. **Iteration 2.** The 4x4 predicted-mode inference was corrected to DC when
   either neighbour is unavailable; ffmpeg's picture-edge legality checks
   (blocks 0,1,4,5 for top and 0,2,8,10 for left in edge macroblocks) were
   modelled by restricting mode selection at those positions.
3. **Iteration 3.** The mode errors disappeared. The first remaining failure
   is the residual of **macroblock 0,0 (first luma/chroma block sequence)**:
   `ffmpeg -v error` reports `negative number of zero coeffs at 0 0` followed
   by `error while decoding MB 0 0`, repeated once per frame.

A copy of the cert diagnostic patched *outside the repository* with the two
missing normative rules (`nc_table(-1) = chroma-DC table`, chroma DC gated on
`cbp_chroma > 0`) parses **all 8 macroblocks of a frame with 0 errors**, so the
divergence is between that independent syntax reader and ffmpeg's CAVLC
decoder, not in the WVS packet layout, SPS/PPS, slice header, mode syntax or
CBP mapping. The unmodified `cert/bitstream_diag.py` still mis-parses chroma
DC (`nc_table(-1)` maps to the 4x4 table) and therefore cannot certify this
path; those cert bugs were not edited (cert/ is not owned by this branch).

Per the three-iteration limit the CAVLC path is disabled and the I_PCM
fallback restored. The next step is to compare ffmpeg's `decode_residual()`
state machine block-by-block against the emitted chroma DC / AC bit strings,
especially the `run_before` table state after `zeros_left` is exhausted.

## 3. Final-tree measurements (I_PCM fallback)

| Check | Result |
| --- | --- |
| `ffmpeg -v error -i out.264 -f null -`, 64x64 `testsrc2`, 8 frames | **0 errors** |
| `ffmpeg -v error -i out.264 -f null -`, 1920x1080 Fortnite, 8 frames | **0 errors** |
| `ffprobe` geometry | **1920x1080**, yuv420p (crop fix verified) |
| File size, 8x1080p | **25,287,467 bytes** (limit 400,000 — **not met**) |
| PSNR vs raw, 8x1080p | **inf dB** (I_PCM is lossless; limit 39 dB — met) |
| `cert/bitstream_diag.py --max-mbs 16`, 1080p | **0 errors**, 16 MBs parsed |

No reference encoder source or dirty stash was read. Only `spec/`,
`docs/rebuild/`, `src/`, `cert/`, public H.264 material and public FFmpeg
decoder source were consulted; the only binaries run were our own encoder,
ffmpeg/ffprobe and `cert/bitstream_diag.py`.
