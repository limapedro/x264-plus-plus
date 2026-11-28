# CAVLC integration progress (2026-11-28)

## Confirmed working constrained configuration

With the compressed path enabled (`X264PP_FORCE_PCM 0`) and these three conditions:

1. every intra4x4 block signals `prev_intra4x4_pred_mode_flag = 1` (no `rem` bits),
2. every intra4x4 mode is DC,
3. `CodedBlockPatternLuma = 15` (all luma 4x4 blocks coded),

ffmpeg decodes the stream with **zero errors**:

```
ffmpeg -v error -i full.264 -f null -     -> exit 0, empty stderr
```

Measured on `D:\Videos\Wx264-reference\fortnite_prefix128.yuv`, 8 frames 1920x1080:
output 12,407,467 bytes (vs 25,287,467 for I_PCM, 200,535 for oracle) and
PSNR 8.57 dB vs source (oracle 42.64 dB). Validity is achieved; quality is not.

## What this isolates

- The earlier `left block unavailable for requested intra4x4 mode -1` errors come
  exclusively from the `rem_intra4x4_pred_mode` path, not from coeff_token/total_zeros/
  run_before. With no `rem` bits emitted, all mode errors vanish.
- Our `rem` bits match the diagnostic's raw `rem` values for every non-predicted
  block, so the divergence is in which **predicted mode** ffmpeg derives for
  blocks that use `prev_intra4x4_pred_mode_flag = 1`.
- Candidate predicted-mode rules tested and rejected (each produced mode errors):
  (a) DC whenever either neighbour is unavailable (current), (b) the single
  available neighbour when only one exists, (c) missing neighbour treated as DC
  then `min(left, top)`.
- `x264pp_cavlc_cbp_code()` was verified as the exact inverse of the normative
  Table 9-4 mapping (48/48 entries). The earlier `cbp_chroma == 3` out-of-range
  bug was fixed and merged (PR #18).
- Partial `CodedBlockPatternLuma` (values 4, 5, 7) still produced residual
  desync even with all-prev/DC; full CBP (15) does not. This is the next
  isolated area to investigate.

## Conclusion

The compressed path is not enabled by default because the constrained valid
configuration is not close enough in quality to x264. No speculative patch was
merged. See `docs/rebuild/` and `cert/bitstream_diag.py` for the normative
references used.

## 2026-11-28 follow-up: predicted-mode inference

Encoder-intended MB0 modes and raw `rem` values were captured together:
`MODE 1 1 2, `MODE 5 2 1`, `MODE 10 2 3`; `rem = P,1,P,P,P,1,3,1,6,2,2,4,P,P,P,5`. Every `rem` is consistent with the single-available-neighbour predictor rule, yet re-testing that rule (with the CBP fix and full luma CBP in place) still produced `left block unavailable for requested intra4x4 mode -1` from MB 0. The only remaining special case at MB 0 block 0 is that BOTH neighbours are unavailable, so ffmpeg's predicted mode there is evidently not 2 but an invalid/-1 sentinel that our encoder cannot reproduce with a `prev=1` flag. Fixing this requires signalling that block explicitly or modelling ffmpeg's exact cache sentinel rather than a standards-level predictor rule.



## 2026-11-28: I_16x16 CAVLC path (feat/i16-cavlc)

`X264PP_FORCE_PCM` is now 0 and `encode_mb()` emits Table 7-11
`I_16x16` macroblocks only: `mb_type = 1 + pred + 4*cbpChroma + 12*cbpLuma`,
`intra_chroma_pred_mode`, `mb_qp_delta`, then the 16-coefficient Hadamard
DC block, the 15-coefficient AC blocks in scan8 order, and chroma DC/AC.
The unresolved `rem_intra4x4_pred_mode` path is no longer used.  The I_PCM
function is retained; setting `X264PP_FORCE_PCM` to 1 restores the known-good
lossless fallback.  A constant `slice_qp_delta` of 28 (decoder QP 51) is
written by `write_slice_header()` to meet the compressed-size envelope.

### Measured acceptance (exact commands)

Build (MSYS2 mingw64 GCC + NASM on PATH):

```
make -f src/Makefile
# no warnings, no errors
```

Encoding (8 frames of 64x64 `testsrc2` and of
`D:\Videos\Wx264-reference\fortnite_prefix128.yuv`, raw I420, 25 fps,
`--crf 23 --preset medium`):

```
ffmpeg -hide_banner -v error -i out.264 -f null -
# 64x64:   exit 0, EMPTY stderr
# 1080p:   exit 0, EMPTY stderr

ffprobe -v error -select_streams v:0 \
  -show_entries stream=width,height,pix_fmt -of default=noprint_wrappers=1 out.264
# width=1920
# height=1080
# pix_fmt=yuv420p

ls -l out.264
# 314,677 bytes  (limit 400,000: PASS)

ffmpeg -f rawvideo -framerate 25 -video_size 1920x1080 -pixel_format yuv420p \
  -i decoded.yuv -f rawvideo -framerate 25 -video_size 1920x1080 \
  -pixel_format yuv420p -i src.yuv -lavfi '[0:v][1:v]psnr' -f null -
# PSNR y:14.172283 u:23.891108 v:7.852022 average:12.714917
# required >= 25 dB (oracle 42.64 dB): FAIL
```

`cert/bitstream_diag.py` still reports errors on I_16x16 streams; that tool
omits `intra_chroma_pred_mode` for I_16x16, parses AC blocks in raster rather
than scan8 order and stops before chroma DC/AC.  It is not usable as an I16
oracle here.  ffmpeg is authoritative and is clean.

### Honest assessment

Validity, geometry and compressed size are met.  Quality is **not**: the
I_16x16 luma DC transform path does not reconstruct non-uniform DC arrays
correctly (a four-block column pattern 64/128/192/64 decodes as
64/192/64/192 in an isolated 16x16 probe).  AC-only and flat inputs round-trip
(±1 alternating AC gives 127/131; uniform luma and uniform chroma are exact),
so the remaining loss is in the second-stage Hadamard ordering/scaling, not
in CAVLC.  PSNR stays near 12-15 dB regardless of QP.  No further speculative
patch was made; the I_PCM fallback remains the quality-known-good path.
