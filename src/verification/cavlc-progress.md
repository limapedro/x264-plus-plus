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


## Follow-up: Intra16x16 DC ordering/scaling (same PR)

Replaced the unverifiable luma DC Hadamard round-trip with a decoder-calibrated
linear inverse: `dc_sign[16][16]` is the H.264 8.5.10 Hadamard sign matrix in
the decoder's coefficient convention and `round_div()` divides with
sign-preserving rounding.  The 4x4 DCT DC values are scaled by 4 before the
inverse (the DCT DC is 16*residual while the decoder's 4x4 IDCT needs
64*residual).  Luma and chroma AC levels are now derived from decoder-measured
per-zigzag-position coefficient scales (`ac_scale48`, proportional to
`dequant(1)`), so the encoder and decoder agree on every coefficient.

### Measured evidence (final tree, slice_qp_delta = 22)

```
make -f src/Makefile                     -> clean, no warnings/errors

ffmpeg -hide_banner -v error -i out.264 -f null -
# 64x64 testsrc2, 8 frames: exit 0, EMPTY stderr
# 8x1920x1080 fortnite:     exit 0, EMPTY stderr

ffprobe -v error -select_streams v:0 \
  -show_entries stream=width,height,pix_fmt -of default=noprint_wrappers=1 out.264
# width=1920 / height=1080 / pix_fmt=yuv420p

8x1080p file size: 390,087 bytes  (limit 400,000: PASS)

ffmpeg psnr (decoded YUV and raw source both at 25 fps):
# PSNR y:14.966659 u:20.715565 v:7.750065 average:12.955281
# oracle (cert/results/real/latest.md): 42.64 dB  -> FAIL
```

16x16 DC probe (source columns 64/128/192/64):

```
# at the earlier QP 23 operating point: 64 / 129 / 192 / 64
# at the final QP 45 operating point:   65 / 135 / 191 / 65
```

The probe is now structurally correct (the previous 64/192/64/192 collapse is
gone); residual ±1–7 sample error is integer-level quantisation at QP 45, not
an ordering error.  Overall PSNR still misses the 25 dB target, mainly because
chroma V and the remaining DC/AC quantisation step at the size-compliant QP
are too coarse.  No success is claimed; the I_PCM fallback remains available
via `X264PP_FORCE_PCM`.


## Default path decision (same PR, final state)

The all-I_PCM path is the DEFAULT again:

```
#ifndef X264PP_FORCE_PCM
#define X264PP_FORCE_PCM 1
#endif
```

The complete I_16x16 CAVLC implementation remains in the tree and is enabled
only with `-DX264PP_FORCE_PCM=0` (which also raises `slice_qp_delta` to 22 for
the size envelope).  It produces valid H.264 and a clean ffmpeg decode, but it
is not competitive:

| Path | ultrafast | medium | veryslow |
| --- | --- | --- | --- |
| all-I_PCM (default) | 0.99x | 3.89x | 10.64x |
| I_16x16 CAVLC | 0.41x | 1.60x | 4.23x |

PSNR on the 8x1080p clip is 12.96 dB for the compressed path vs the 42.64 dB
oracle, so the compressed path cannot be the default on either quality or
speed grounds.

### Default-build byte-for-byte check

Build command (MSYS2 mingw64 GCC + NASM on PATH, run from the worktree root):

```
make -f src/Makefile
# clean, no warnings/errors
```

Tiny clip and encode:

```
ffmpeg -y -f lavfi -i 'testsrc2=size=64x64:rate=30' -frames:v 8 \
  -pix_fmt yuv420p -f rawvideo tiny.yuv
build/x264pp.exe --demuxer raw --input-csp i420 --input-res 64x64 \
  --fps 30 --preset medium --threads 1 --frames 8 --no-progress \
  -o baseline.264 tiny.yuv
```

```
SHA256(baseline.264) =
  83C71EBCC7A01E991F052CDDFC8BFE8E7B4BA27DC7A498A103DC7EE8C00F0DB1
# equals the certified baseline hash

ffmpeg -hide_banner -v error -i baseline.264 -f null -
# exit 0, EMPTY stderr
```

## I_16x16 QP tradeoff (2026-11-28)

With the gated I_16x16 path (-DX264PP_FORCE_PCM=0) at slice_qp_delta=0 (QP 23), 8 frames of 1920x1080 Fortnite decode cleanly but produce 2,204,665 bytes and PSNR 14.67 dB (Y 20.32, U 23.80, V 7.87). With slice_qp_delta=22 (the size-compliant setting) size drops to 390,087 bytes but PSNR stays 12.96 dB. The V-plane deficit shows the remaining loss is in chroma reconstruction, not only quantisation step. Default build remains all-I_PCM. No claim of competitive quality is made.


## AVX2 predictor wiring test (rejected)

Wiring `x264pp_intra16x16_avx2` into the gated I_16x16 path (passing the neighbour cache as raw above/left arrays) kept the default PCM build bit-identical and ffmpeg-clean, but regressed gated 1080p QP45 luma PSNR from 25.08 dB to 18.94 dB (average 26.24 -> 20.55 dB) while size changed from 293,151 to 312,559 bytes. The NASM predictor's plane-mode/sample convention differs from the normative `pred16_norm` used by this encoder, so the patch was rejected and not merged. Further assembly work must preserve exact predictor semantics.

