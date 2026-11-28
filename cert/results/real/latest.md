# Real-video A/B benchmark

_Generated: 2026-11-28T03:43:32+00:00 (run `20261128T034332Z`)_

**Status:** ok — **candidate CERTIFIED**

**Publishable speedups:** yes (candidate decoded cleanly on every real-video case)

## Reference-only table (oracle x264, black box)

| Clip | Resolution | Frames | Preset | Oracle fps (median) | Median ms | Decode | PSNR dB | SSIM |
|---|---|---:|---|---:|---:|:---:|---:|---:|
| fortnite-raw-1080p | 1920x1080 | 8 | ultrafast | 162.42 | 49.3 | yes | 44.88 | 0.9874 |
| fortnite-raw-1080p | 1920x1080 | 8 | medium | 39.16 | 204.3 | yes | 42.64 | 0.9849 |
| fortnite-raw-1080p | 1920x1080 | 8 | veryslow | 14.05 | 569.5 | yes | 42.29 | 0.9839 |
| twitter-640x360 | 640x360 | 8 | ultrafast | 389.81 | 20.5 | yes | 45.82 | 0.9915 |
| twitter-640x360 | 640x360 | 8 | medium | 238.86 | 33.5 | yes | 44.79 | 0.9935 |
| twitter-640x360 | 640x360 | 8 | veryslow | 102.45 | 78.1 | yes | 45.24 | 0.9936 |
| twitter-1080p | 1920x1080 | 8 | ultrafast | 159.41 | 50.2 | yes | 64.62 | 0.9998 |
| twitter-1080p | 1920x1080 | 8 | medium | 73.78 | 108.4 | yes | 60.36 | 0.9997 |
| twitter-1080p | 1920x1080 | 8 | veryslow | 54.94 | 145.6 | yes | 59.88 | 0.9997 |
| twitter-720p60 | 1280x720 | 8 | ultrafast | 255.16 | 31.4 | yes | 49.93 | 0.9958 |
| twitter-720p60 | 1280x720 | 8 | medium | 136.36 | 58.7 | yes | 48.09 | 0.9966 |
| twitter-720p60 | 1280x720 | 8 | veryslow | 70.06 | 114.2 | yes | 47.78 | 0.9961 |

## A/B table (reference vs candidate)

| Clip | Preset | Oracle fps | Candidate fps | Speedup | Candidate decode | Certification |
|---|---|---:|---:|---:|:---:|---|
| fortnite-raw-1080p | ultrafast | 162.42 | 156.84 | 0.966x | yes | certified |
| fortnite-raw-1080p | medium | 39.16 | 154.51 | 3.946x | yes | certified |
| fortnite-raw-1080p | veryslow | 14.05 | 149.87 | 10.668x | yes | certified |
| twitter-640x360 | ultrafast | 389.81 | 605.34 | 1.553x | yes | certified |
| twitter-640x360 | medium | 238.86 | 549.11 | 2.299x | yes | certified |
| twitter-640x360 | veryslow | 102.45 | 574.63 | 5.609x | yes | certified |
| twitter-1080p | ultrafast | 159.41 | 159.33 | 1.000x | yes | certified |
| twitter-1080p | medium | 73.78 | 158.12 | 2.143x | yes | certified |
| twitter-1080p | veryslow | 54.94 | 162.88 | 2.965x | yes | certified |
| twitter-720p60 | ultrafast | 255.16 | 304.91 | 1.195x | yes | certified |
| twitter-720p60 | medium | 136.36 | 314.91 | 2.309x | yes | certified |
| twitter-720p60 | veryslow | 70.06 | 301.84 | 4.308x | yes | certified |

The candidate column stays `n/a` / `NOT CERTIFIED` unless the candidate binary exists, runs and decodes cleanly on **every** case. A measured ratio is shown only for a cleanly decoding candidate; no speedup is inferred from a failed or absent binary.

## Quality vs exact source I420 (ffmpeg PSNR / SSIM)

| Clip | Preset | Oracle PSNR dB | Oracle SSIM | Candidate PSNR dB | Candidate SSIM |
|---|---|---:|---:|---:|---:|
| fortnite-raw-1080p | ultrafast | 44.88 | 0.9874 | 99.00 | 1.0000 |
| fortnite-raw-1080p | medium | 42.64 | 0.9849 | 99.00 | 1.0000 |
| fortnite-raw-1080p | veryslow | 42.29 | 0.9839 | 99.00 | 1.0000 |
| twitter-640x360 | ultrafast | 45.82 | 0.9915 | 99.00 | 1.0000 |
| twitter-640x360 | medium | 44.79 | 0.9935 | 99.00 | 1.0000 |
| twitter-640x360 | veryslow | 45.24 | 0.9936 | 99.00 | 1.0000 |
| twitter-1080p | ultrafast | 64.62 | 0.9998 | 99.00 | 1.0000 |
| twitter-1080p | medium | 60.36 | 0.9997 | 99.00 | 1.0000 |
| twitter-1080p | veryslow | 59.88 | 0.9997 | 99.00 | 1.0000 |
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
