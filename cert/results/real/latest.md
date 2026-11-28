# Real-video A/B benchmark

_Generated: 2026-11-28T04:01:47+00:00 (run `20261128T040147Z`)_

**Status:** ok — **candidate CERTIFIED**

**Publishable speedups:** yes (candidate decoded cleanly on every real-video case)

## Reference-only table (oracle x264, black box)

| Clip | Resolution | Frames | Preset | Oracle fps (median) | Median ms | Decode | PSNR dB | SSIM |
|---|---|---:|---|---:|---:|:---:|---:|---:|
| fortnite-raw-1080p | 1920x1080 | 8 | ultrafast | 161.53 | 49.5 | yes | 44.88 | 0.9874 |
| twitter-640x360 | 640x360 | 8 | ultrafast | 385.92 | 20.7 | yes | 45.82 | 0.9915 |
| twitter-1080p | 1920x1080 | 8 | ultrafast | 141.69 | 56.5 | yes | 64.62 | 0.9998 |
| twitter-720p60 | 1280x720 | 8 | ultrafast | 254.35 | 31.5 | yes | 49.93 | 0.9958 |

## A/B table (reference vs candidate)

| Clip | Preset | Oracle fps | Candidate fps | Speedup | Candidate decode | Certification |
|---|---|---:|---:|---:|:---:|---|
| fortnite-raw-1080p | ultrafast | 161.53 | 164.72 | 1.020x | yes | certified |
| twitter-640x360 | ultrafast | 385.92 | 595.00 | 1.542x | yes | certified |
| twitter-1080p | ultrafast | 141.69 | 159.99 | 1.129x | yes | certified |
| twitter-720p60 | ultrafast | 254.35 | 299.11 | 1.176x | yes | certified |

The candidate column stays `n/a` / `NOT CERTIFIED` unless the candidate binary exists, runs and decodes cleanly on **every** case. A measured ratio is shown only for a cleanly decoding candidate; no speedup is inferred from a failed or absent binary.

## Quality vs exact source I420 (ffmpeg PSNR / SSIM)

| Clip | Preset | Oracle PSNR dB | Oracle SSIM | Candidate PSNR dB | Candidate SSIM |
|---|---|---:|---:|---:|---:|
| fortnite-raw-1080p | ultrafast | 44.88 | 0.9874 | 99.00 | 1.0000 |
| twitter-640x360 | ultrafast | 45.82 | 0.9915 | 99.00 | 1.0000 |
| twitter-1080p | ultrafast | 64.62 | 0.9998 | 99.00 | 1.0000 |
| twitter-720p60 | ultrafast | 49.93 | 0.9958 | 99.00 | 1.0000 |

## Documented reference anchors (historical, not re-derived here)

| Case | fps | Source |
|---|---:|---|
| 640x360, 120 f, medium, CRF 23, 6 threads (synthetic testsrc2) | 609.14 | `benchmarks.md / README.md` |
| 1920x1080, 8 f, ultrafast, CRF 23, 6 threads (Fortnite I420) | 126.98 | `benchmarks.md / README.md` |
| 1920x1080, 8 f, medium, CRF 23, 6 threads (Fortnite I420) | 40.61 | `benchmarks.md / README.md` |
| 1920x1080, 8 f, veryslow, CRF 23, 6 threads (Fortnite I420) | 13.84 | `benchmarks.md / README.md` |

These four numbers are the already-established reference measurements published in `benchmarks.md` / `README.md`. The table above contains this run's fresh oracle measurements and is the authoritative result for this run.

## Summary

- Cases: 4; oracle-decoded: 4; candidate-certified: 4
- Presets: ultrafast; frames/case: 8; reps: 1; warm-ups: 0
- Machine: Intel64 Family 6 Model 151 Stepping 5, GenuineIntel, 12 logical CPUs
