# Real-video A/B benchmark

_Generated: 2026-11-28T02:31:24+00:00 (run `20261128T023124Z`)_

**Status:** ok — **candidate CERTIFIED**

**Publishable speedups:** yes (candidate decoded cleanly on every real-video case)

## Reference-only table (oracle x264, black box)

| Clip | Resolution | Frames | Preset | Oracle fps (median) | Median ms | Decode | PSNR dB | SSIM |
|---|---|---:|---|---:|---:|:---:|---:|---:|
| fortnite-raw-1080p | 1920x1080 | 8 | ultrafast | 149.15 | 53.6 | yes | 44.88 | 0.9874 |
| fortnite-raw-1080p | 1920x1080 | 8 | medium | 38.37 | 208.5 | yes | 42.64 | 0.9849 |
| fortnite-raw-1080p | 1920x1080 | 8 | veryslow | 13.28 | 602.4 | yes | 42.29 | 0.9839 |
| twitter-640x360 | 640x360 | 8 | ultrafast | 376.36 | 21.3 | yes | 45.82 | 0.9915 |
| twitter-640x360 | 640x360 | 8 | medium | 237.99 | 33.6 | yes | 44.79 | 0.9935 |
| twitter-640x360 | 640x360 | 8 | veryslow | 100.66 | 79.5 | yes | 45.24 | 0.9936 |
| twitter-1080p | 1920x1080 | 8 | ultrafast | 157.08 | 50.9 | yes | 64.62 | 0.9998 |
| twitter-1080p | 1920x1080 | 8 | medium | 71.14 | 112.5 | yes | 60.36 | 0.9997 |
| twitter-1080p | 1920x1080 | 8 | veryslow | 56.40 | 141.8 | yes | 59.88 | 0.9997 |
| twitter-720p60 | 1280x720 | 8 | ultrafast | 253.53 | 31.6 | yes | 49.93 | 0.9958 |
| twitter-720p60 | 1280x720 | 8 | medium | 127.04 | 63.0 | yes | 48.09 | 0.9966 |
| twitter-720p60 | 1280x720 | 8 | veryslow | 66.96 | 119.5 | yes | 47.78 | 0.9961 |

## A/B table (reference vs candidate)

| Clip | Preset | Oracle fps | Candidate fps | Speedup | Candidate decode | Certification |
|---|---|---:|---:|---:|:---:|---|
| fortnite-raw-1080p | ultrafast | 149.15 | 29.32 | 0.197x | yes | certified |
| fortnite-raw-1080p | medium | 38.37 | 28.64 | 0.747x | yes | certified |
| fortnite-raw-1080p | veryslow | 13.28 | 29.27 | 2.204x | yes | certified |
| twitter-640x360 | ultrafast | 376.36 | 225.38 | 0.599x | yes | certified |
| twitter-640x360 | medium | 237.99 | 222.59 | 0.935x | yes | certified |
| twitter-640x360 | veryslow | 100.66 | 220.62 | 2.192x | yes | certified |
| twitter-1080p | ultrafast | 157.08 | 45.95 | 0.292x | yes | certified |
| twitter-1080p | medium | 71.14 | 44.90 | 0.631x | yes | certified |
| twitter-1080p | veryslow | 56.40 | 44.70 | 0.792x | yes | certified |
| twitter-720p60 | ultrafast | 253.53 | 77.41 | 0.305x | yes | certified |
| twitter-720p60 | medium | 127.04 | 78.54 | 0.618x | yes | certified |
| twitter-720p60 | veryslow | 66.96 | 76.98 | 1.150x | yes | certified |

The candidate column stays `n/a` / `NOT CERTIFIED` unless the candidate binary exists, runs and decodes cleanly on **every** case. A measured ratio is shown only for a cleanly decoding candidate; no speedup is inferred from a failed or absent binary.

## Quality vs exact source I420 (ffmpeg PSNR / SSIM)

| Clip | Preset | Oracle PSNR dB | Oracle SSIM | Candidate PSNR dB | Candidate SSIM |
|---|---|---:|---:|---:|---:|
| fortnite-raw-1080p | ultrafast | 44.88 | 0.9874 | — | — |
| fortnite-raw-1080p | medium | 42.64 | 0.9849 | — | — |
| fortnite-raw-1080p | veryslow | 42.29 | 0.9839 | — | — |
| twitter-640x360 | ultrafast | 45.82 | 0.9915 | — | — |
| twitter-640x360 | medium | 44.79 | 0.9935 | — | — |
| twitter-640x360 | veryslow | 45.24 | 0.9936 | — | — |
| twitter-1080p | ultrafast | 64.62 | 0.9998 | — | — |
| twitter-1080p | medium | 60.36 | 0.9997 | — | — |
| twitter-1080p | veryslow | 59.88 | 0.9997 | — | — |
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
- Presets: ultrafast, medium, veryslow; frames/case: 8; reps: 3; warm-ups: 1
- Machine: Intel64 Family 6 Model 151 Stepping 5, GenuineIntel, 12 logical CPUs
