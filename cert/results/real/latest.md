# Real-video A/B benchmark

_Generated: 2026-11-28T02:45:21+00:00 (run `20261128T024521Z`)_

**Status:** ok — **candidate CERTIFIED**

**Publishable speedups:** yes (candidate decoded cleanly on every real-video case)

## Reference-only table (oracle x264, black box)

| Clip | Resolution | Frames | Preset | Oracle fps (median) | Median ms | Decode | PSNR dB | SSIM |
|---|---|---:|---|---:|---:|:---:|---:|---:|
| fortnite-raw-1080p | 1920x1080 | 8 | ultrafast | 158.63 | 50.4 | yes | 44.88 | 0.9874 |
| fortnite-raw-1080p | 1920x1080 | 8 | medium | 38.39 | 208.4 | yes | 42.64 | 0.9849 |
| fortnite-raw-1080p | 1920x1080 | 8 | veryslow | 13.90 | 575.7 | yes | 42.29 | 0.9839 |
| twitter-640x360 | 640x360 | 8 | ultrafast | 391.99 | 20.4 | yes | 45.82 | 0.9915 |
| twitter-640x360 | 640x360 | 8 | medium | 239.95 | 33.3 | yes | 44.79 | 0.9935 |
| twitter-640x360 | 640x360 | 8 | veryslow | 105.25 | 76.0 | yes | 45.24 | 0.9936 |
| twitter-1080p | 1920x1080 | 8 | ultrafast | 172.24 | 46.4 | yes | 64.62 | 0.9998 |
| twitter-1080p | 1920x1080 | 8 | medium | 73.65 | 108.6 | yes | 60.36 | 0.9997 |
| twitter-1080p | 1920x1080 | 8 | veryslow | 56.73 | 141.0 | yes | 59.88 | 0.9997 |
| twitter-720p60 | 1280x720 | 8 | ultrafast | 253.66 | 31.5 | yes | 49.93 | 0.9958 |
| twitter-720p60 | 1280x720 | 8 | medium | 137.99 | 58.0 | yes | 48.09 | 0.9966 |
| twitter-720p60 | 1280x720 | 8 | veryslow | 68.74 | 116.4 | yes | 47.78 | 0.9961 |

## A/B table (reference vs candidate)

| Clip | Preset | Oracle fps | Candidate fps | Speedup | Candidate decode | Certification |
|---|---|---:|---:|---:|:---:|---|
| fortnite-raw-1080p | ultrafast | 158.63 | 62.20 | 0.392x | yes | certified |
| fortnite-raw-1080p | medium | 38.39 | 63.04 | 1.642x | yes | certified |
| fortnite-raw-1080p | veryslow | 13.90 | 62.83 | 4.522x | yes | certified |
| twitter-640x360 | ultrafast | 391.99 | 368.54 | 0.940x | yes | certified |
| twitter-640x360 | medium | 239.95 | 364.10 | 1.517x | yes | certified |
| twitter-640x360 | veryslow | 105.25 | 369.33 | 3.509x | yes | certified |
| twitter-1080p | ultrafast | 172.24 | 85.89 | 0.499x | yes | certified |
| twitter-1080p | medium | 73.65 | 80.04 | 1.087x | yes | certified |
| twitter-1080p | veryslow | 56.73 | 77.45 | 1.365x | yes | certified |
| twitter-720p60 | ultrafast | 253.66 | 144.89 | 0.571x | yes | certified |
| twitter-720p60 | medium | 137.99 | 138.75 | 1.006x | yes | certified |
| twitter-720p60 | veryslow | 68.74 | 148.41 | 2.159x | yes | certified |

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
