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

