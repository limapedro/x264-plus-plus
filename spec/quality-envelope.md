# Quality envelope — observable acceptance contract

Status: **specification of what a conforming clean-room encoder's output must look like**, derived from black-box measurements of `oracle/x264.exe`. It intentionally does **not** require bitstream byte equality. All numbers below are observations or engineering tolerances; any item not measured is marked **UNKNOWN**.

## 1. Scope

This envelope applies to CRF-mode, progressive 8-bit 4:2:0 encodings produced by the CLI shapes in `usage-model.md`:

```
x264 --input-res WxH --fps 30 --preset P --crf C --threads T -o ref.264 case.yuv
<candidate>                                      -o cand.264 case.yuv
```

ABR/two-pass, `--qp 0` lossless, 10-bit/4:2:2/4:4:4, interlaced, and explicit `--tune` behaviour are **UNKNOWN** for this envelope. Speed (the ≥2× throughput goal) is measured by the certification harness and published in `score.md`; it is not a bitstream property.

## 2. Measurement protocol

1. Generate the input YUV deterministically with the ffmpeg `lavfi` sources in `spec/probes/measure-quality.ps1` (same seed/source/frame count for reference and candidate). Store media outside the repo.
2. Encode reference and candidate with the **same** state drawn from `usage-model.md` (resolution, frames, preset, CRF, threads, content).
3. Decode validity check on `cand.264`:

```
ffmpeg -v error -i cand.264 -f null -
ffprobe -v error -show_entries stream=width,height,pix_fmt,codec_name -show_entries stream=nb_read_frames -count_frames cand.264
```

   The decode must emit **no** error lines; `codec_name=h264`, `width/height` equal the input, `pix_fmt=yuv420p`, and the decoded frame count equal the input frame count. The bitstream must contain parseable SPS/PPS and VCL NAL units as specified in `h264-syntax.md`.
4. Quality metrics (ffmpeg filters, reference decoder):

```
ffmpeg -i cand.264 -f rawvideo -pixel_format yuv420p -video_size WxH -framerate 30 -i case.yuv -lavfi "[0:v][1:v]psnr"  -f null -
ffmpeg -i cand.264 -f rawvideo -pixel_format yuv420p -video_size WxH -framerate 30 -i case.yuv -lavfi "[0:v][1:v]ssim"  -f null -
```

   Use the filter's sequence-average `PSNR average:` (dB) and `SSIM All:` (0…1). The same commands applied to `ref.264` give `P_ref` and `S_ref`.
5. Size ratio: `r = bytes(cand.264) / bytes(ref.264)`, using raw Annex B file sizes in bytes.

Per-frame minima and frame-alignment diagnostics may be reported by the harness but are **not** part of the acceptance numbers below (threshold **UNKNOWN**).

## 3. Reference measurements used to set the band

Produced by `spec/probes/measure-quality.ps1` against `oracle/x264.exe` (`x264 0.164.x`). All cases use 30 fps, raw YUV420p input, default GOP/keyint settings.

| Case | Resolution | Frames | Content | Preset | CRF | Threads | Bytes | PSNR avg (dB) | SSIM All |
|---|---|---:|---|---|---:|---:|---:|---:|---:|
| c01 | 320×180 | 30 | synthetic | medium | 18 | 1 | 45,755 | 46.452210 | 0.997010 |
| c02 | 320×180 | 30 | synthetic | medium | 23 | 1 | 31,528 | 41.530419 | 0.992109 |
| c03 | 320×180 | 30 | synthetic | medium | 28 | 1 | 19,190 | 37.269488 | 0.981190 |
| c04 | 320×180 | 30 | synthetic | medium | 23 | 4 | 31,528 | 41.530419 | 0.992109 |
| c05 | 640×360 | 30 | natural-like | medium | 23 | 1 | 688,598 | 36.309760 | 0.983195 |
| c06 | 640×360 | 30 | natural-like | medium | 28 | 1 | 445,118 | 31.718736 | 0.962544 |
| c07 | 640×360 | 30 | noisy | veryfast | 28 | 1 | 80,060 | 30.112471 | 0.591888 |
| c08 | 320×180 | 30 | static | medium | 23 | 1 | 1,255 | 63.422032 | 0.999931 |
| c09 | 1280×720 | 10 | synthetic | fast | 28 | 4 | 65,531 | 39.809660 | 0.987200 |
| c10 | 640×360 | 30 | natural-like | medium | 23 | 4 | 691,536 | 36.328693 | 0.983286 |
| c11 | 640×360 | 30 | noisy | veryfast | 28 | 4 | 80,060 | 30.112471 | 0.591888 |
| c12 | 320×180 | 30 | synthetic | slow | 23 | 1 | 30,648 | 41.613479 | 0.992267 |

Observed CRF trend (c01→c03): −9.18 dB PSNR and −58.1% bytes from CRF 18 to 28. Observed content sensitivity: the noisy proxy at CRF 28 reached only 0.5919 SSIM while synthetic 320×180 at the same CRF reached 0.9812, so **the band must be relative to the reference on the same input**, never an absolute SSIM/PSNR floor across content types.

### 3.1 Reference self-variability (the empirical anchor)

| Comparison | Δ bytes | Δ PSNR (dB) | Δ SSIM |
|---|---:|---:|---:|
| c02 (threads 1) vs c04 (threads 4) | 0.00% | 0.000 | 0.000000 |
| c07 (threads 1) vs c11 (threads 4) | 0.00% | 0.000 | 0.000000 |
| c05 (threads 1) vs c10 (threads 4) | +0.43% | +0.019 | +0.000091 |
| c02 (medium) vs c12 (slow), same CRF | −2.80% | +0.083 | +0.000158 |

The reference itself varies by **less than 0.5% in size, 0.1 dB in PSNR and 0.001 in SSIM** when only thread count or preset changes at fixed CRF. The tolerances below are therefore much wider than reference nondeterminism, leaving room for a genuinely different speed-first implementation without masking a regression that a decoder-visible metric would catch.

## 4. Acceptable envelope (per drawn case)

Let `P_c`, `S_c`, `B_c` be candidate PSNR average (dB), SSIM All, and byte count; `P_ref`, `S_ref`, `B_ref` the reference values on the same input and CLI state.

### 4.1 Hard validity requirements (every case)

- `cand.264` decodes with `ffmpeg -v error` producing **zero** error lines.
- Decoded `codec_name=h264`, `pix_fmt=yuv420p`, `width×height` equals the input resolution, and decoded frame count equals `N`.
- NAL units are Annex B conformant with the SPS/PPS/slice structures of `h264-syntax.md` (a decoder must find the active parameter sets).
- No frame is missing, duplicated, or output in the wrong display order: the decoded presentation timestamps/frame sequence must correspond 1:1 with the source frames. `ffprobe -show_frames` picture-type sequences may differ from the reference and are **not** required to match.
- A validity failure is a **hard fail** for that case and cannot be offset by quality or size.

### 4.2 Quality and size bands

| Metric | Contract |
|---|---|
| PSNR (sequence average) | `P_ref − 0.75 dB ≤ P_c ≤ P_ref + 1.50 dB` |
| SSIM (All, sequence average) | `S_ref − 0.010 ≤ S_c ≤ min(1.0, S_ref + 0.020)` |
| Size ratio | `0.70 ≤ B_c / B_ref ≤ 1.40` |
| Bitstream bytes | **No equality requirement.** Hashes may differ; do not compare bytes for scoring. |

Notes:

- Applying the band **relative to the reference on the same case** is essential: absolute PSNR/SSIM depends far more on content and CRF (see §3) than on encoder choice.
- The lower PSNR/SSIM bounds are the perceptual contract; the upper bounds plus the size ratio prevent a trivially "high quality" candidate that simply dumps bits. A candidate that is better than the reference within these bounds passes.
- 0.75 dB PSNR and 0.010 SSIM are **engineering margins, not measured JND thresholds (UNKNOWN)**. They were chosen to exceed observed reference self-variability by roughly one order of magnitude while remaining small relative to the 4–6 dB swings produced by a 5-point CRF change.
- The 0.70–1.40 size band allows entropy-mode changes (CAVLC vs CABAC), different SEI metadata, different mb_type/mode decisions, and different rate-control implementations. It is intentionally asymmetric-generous for speed-first designs but tight enough to catch runaway bitrate.
- If `P_ref` is infinite (lossless configs outside this usage model), the band is **UNKNOWN**; do not apply these numbers.

### 4.3 Aggregate certification rule

For a batch of `M` cases drawn from `usage-model.md`:

- **100%** of cases must satisfy §4.1 (validity).
- At least **95%** of cases must satisfy all three bands in §4.2; every band violation must be logged with its case state and measured deltas.
- The realized marginal distribution of the drawn states must be published so the 95% figure can be interpreted against the usage model.
- A different confidence/sample-size policy may be published by the certification team, but it must be declared as a **new** envelope version; this file fixes the default contract.

## 5. Rationale summary

- Speed-first project goal ⇒ byte-for-byte identity is explicitly **not** required; only decoder-visible behaviour and efficiency are contracted.
- Reference self-variability is tiny, so tolerances are intentionally a much larger, documented margin rather than a re-statement of x264 implementation details.
- Content and CRF dominate PSNR/SSIM, so the band is per-case relative, not a global threshold.
- The size band is the proxy for "same CRF means roughly same rate"; it is the only place where encoder efficiency differences are allowed to show up.

## 6. UNKNOWN / excluded

- Per-frame PSNR/SSIM minima, temporal quality consistency, and artifact-specific measures (blocking, ringing, banding) — thresholds **UNKNOWN**.
- Subjective/MOS equivalence — **UNKNOWN**; no subjective tests were run.
- Exact behaviour of options not in the usage model (`--tune`, `--profile`, `--aq-*`, `--bframes` overrides, VBV, 2-pass, `--qp`, high bit depth, interlaced) — **UNKNOWN**.
- Encoding speed/throughput targets — out of scope of this bitstream/quality envelope; see `score.md` and the process documents owned by other teams.
