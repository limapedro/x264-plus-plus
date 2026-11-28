# Real-video A/B benchmark

_Generated: 2026-11-28T02:46:34+00:00 (run `20261128T024634Z`)_

**Status:** ok — **candidate CERTIFIED**

**Publishable speedups:** yes (candidate decoded cleanly on every real-video case)

## Reference-only table (oracle x264, black box)

| Clip | Resolution | Frames | Preset | Oracle fps (median) | Median ms | Decode | PSNR dB | SSIM |
|---|---|---:|---|---:|---:|:---:|---:|---:|
| fortnite-raw-1080p | 1920x1080 | 8 | ultrafast | 158.09 | 50.6 | yes | 44.88 | 0.9874 |
| fortnite-raw-1080p | 1920x1080 | 8 | medium | 48.42 | 165.2 | yes | 42.63 | 0.9848 |
| fortnite-raw-1080p | 1920x1080 | 8 | veryslow | 16.69 | 479.5 | yes | 42.30 | 0.9839 |
| twitter-640x360 | 640x360 | 8 | ultrafast | 378.05 | 21.2 | yes | 45.82 | 0.9915 |
| twitter-640x360 | 640x360 | 8 | medium | 242.63 | 33.0 | yes | 44.79 | 0.9934 |
| twitter-640x360 | 640x360 | 8 | veryslow | 110.76 | 72.2 | yes | 45.31 | 0.9936 |
| twitter-1080p | 1920x1080 | 8 | ultrafast | 166.04 | 48.2 | yes | 64.62 | 0.9998 |
| twitter-1080p | 1920x1080 | 8 | medium | 82.81 | 96.6 | yes | 60.36 | 0.9997 |
| twitter-1080p | 1920x1080 | 8 | veryslow | 68.00 | 117.6 | yes | 60.21 | 0.9997 |
| twitter-720p60 | 1280x720 | 8 | ultrafast | 244.84 | 32.7 | yes | 49.93 | 0.9958 |
| twitter-720p60 | 1280x720 | 8 | medium | 137.75 | 58.1 | yes | 48.09 | 0.9966 |
| twitter-720p60 | 1280x720 | 8 | veryslow | 69.72 | 114.7 | yes | 47.78 | 0.9961 |

## A/B table (reference vs candidate)

| Clip | Preset | Oracle fps | Candidate fps | Speedup | Candidate decode | Certification |
|---|---|---:|---:|---:|:---:|---|
| fortnite-raw-1080p | ultrafast | 158.09 | 72.16 | 0.456x | yes | certified |
| fortnite-raw-1080p | medium | 48.42 | 74.69 | 1.542x | yes | certified |
| fortnite-raw-1080p | veryslow | 16.69 | 73.04 | 4.377x | yes | certified |
| twitter-640x360 | ultrafast | 378.05 | 339.92 | 0.899x | yes | certified |
| twitter-640x360 | medium | 242.63 | 407.49 | 1.679x | yes | certified |
| twitter-640x360 | veryslow | 110.76 | 329.77 | 2.977x | yes | certified |
| twitter-1080p | ultrafast | 166.04 | 94.52 | 0.569x | yes | certified |
| twitter-1080p | medium | 82.81 | 85.91 | 1.038x | yes | certified |
| twitter-1080p | veryslow | 68.00 | 85.64 | 1.259x | yes | certified |
| twitter-720p60 | ultrafast | 244.84 | 161.17 | 0.658x | yes | certified |
| twitter-720p60 | medium | 137.75 | 171.28 | 1.243x | yes | certified |
| twitter-720p60 | veryslow | 69.72 | 165.45 | 2.373x | yes | certified |

The candidate column stays `n/a` / `NOT CERTIFIED` unless the candidate binary exists, runs and decodes cleanly on **every** case. A measured ratio is shown only for a cleanly decoding candidate; no speedup is inferred from a failed or absent binary.

## Quality vs exact source I420 (ffmpeg PSNR / SSIM)

| Clip | Preset | Oracle PSNR dB | Oracle SSIM | Candidate PSNR dB | Candidate SSIM |
|---|---|---:|---:|---:|---:|
| fortnite-raw-1080p | ultrafast | 44.88 | 0.9874 | — | — |
| fortnite-raw-1080p | medium | 42.63 | 0.9848 | — | — |
| fortnite-raw-1080p | veryslow | 42.30 | 0.9839 | — | — |
| twitter-640x360 | ultrafast | 45.82 | 0.9915 | — | — |
| twitter-640x360 | medium | 44.79 | 0.9934 | — | — |
| twitter-640x360 | veryslow | 45.31 | 0.9936 | — | — |
| twitter-1080p | ultrafast | 64.62 | 0.9998 | — | — |
| twitter-1080p | medium | 60.36 | 0.9997 | — | — |
| twitter-1080p | veryslow | 60.21 | 0.9997 | — | — |
| twitter-720p60 | ultrafast | 49.93 | 0.9958 | 99.00 | 1.0000 |
| twitter-720p60 | medium | 48.09 | 0.9966 | 99.00 | 1.0000 |
| twitter-720p60 | veryslow | 47.78 | 0.9961 | 99.00 | 1.0000 |

## Documented reference anchors (historical, not re-derived here)

| Case | fps | Source |
|---|---:|---|
| 640x360, 120 f, medium, CRF 23, 6 threads (synthetic testsrc2) | 609.14 | `benchmarks.md / README.md` |
| 1920x1080, 8 f, ultrafast, CRF 23, 6 threads (Fortnite I420) | 126.98 | `benchmarks.md / README.md` |
| 1920x1080, 8 f, medium, CRF 23, 6 threads (Fortnite I420) | 40.61 | `benchmarks.md / README.md` |
| 1920x1080, 8 f, veryslow, CRF 23, 6 threads (Fortnite I420) | 13.84 | `benchmarks.md / README.md` |

These four numbers are the already-established reference measurements published in `benchmarks.md` / `README.md`. The table above contains this run's fresh oracle measurements and is the authoritative result for this run.

## Summary

- Cases: 12; oracle-decoded: 12; candidate-certified: 12
- Presets: ultrafast, medium, veryslow; frames/case: 8; reps: 2; warm-ups: 1
- Machine: Intel64 Family 6 Model 151 Stepping 5, GenuineIntel, 12 logical CPUs
