# Real-video A/B benchmark

_Generated: 2026-11-27T22:07:50+00:00 (run `20261127T220750Z`)_

**Status:** blocked — **candidate NOT CERTIFIED**

**Publishable speedups:** no (NOT CERTIFIED — candidate binary missing: E:\_wt\x264pp-bench\build\x264pp.exe (NOT CERTIFIED))

## Blocked

candidate binary missing: E:\_wt\x264pp-bench\build\x264pp.exe (NOT CERTIFIED)

## Reference-only table (oracle x264, black box)

| Clip | Resolution | Frames | Preset | Oracle fps (median) | Median ms | Decode | PSNR dB | SSIM |
|---|---|---:|---|---:|---:|:---:|---:|---:|
| fortnite-raw-1080p | 1920x1080 | 8 | ultrafast | 158.29 | 50.5 | yes | 44.88 | 0.9874 |
| fortnite-raw-1080p | 1920x1080 | 8 | medium | 39.89 | 200.5 | yes | 42.64 | 0.9849 |
| fortnite-raw-1080p | 1920x1080 | 8 | veryslow | 13.72 | 582.9 | yes | 42.29 | 0.9839 |
| twitter-640x360 | 640x360 | 8 | ultrafast | 365.77 | 21.9 | yes | 45.82 | 0.9915 |
| twitter-640x360 | 640x360 | 8 | medium | 242.66 | 33.0 | yes | 44.79 | 0.9935 |
| twitter-640x360 | 640x360 | 8 | veryslow | 102.97 | 77.7 | yes | 45.24 | 0.9936 |
| twitter-1080p | 1920x1080 | 8 | ultrafast | 168.57 | 47.5 | yes | 64.62 | 0.9998 |
| twitter-1080p | 1920x1080 | 8 | medium | 75.10 | 106.5 | yes | 60.36 | 0.9997 |
| twitter-1080p | 1920x1080 | 8 | veryslow | 57.30 | 139.6 | yes | 59.88 | 0.9997 |
| twitter-720p60 | 1280x720 | 8 | ultrafast | 256.38 | 31.2 | yes | 49.93 | 0.9958 |
| twitter-720p60 | 1280x720 | 8 | medium | 125.98 | 63.5 | yes | 48.09 | 0.9966 |
| twitter-720p60 | 1280x720 | 8 | veryslow | 67.73 | 118.1 | yes | 47.78 | 0.9961 |

## A/B table (reference vs candidate)

| Clip | Preset | Oracle fps | Candidate fps | Speedup | Candidate decode | Certification |
|---|---|---:|---:|---:|:---:|---|
| fortnite-raw-1080p | ultrafast | 158.29 | n/a (binary missing) | — | n/a | NOT CERTIFIED |
| fortnite-raw-1080p | medium | 39.89 | n/a (binary missing) | — | n/a | NOT CERTIFIED |
| fortnite-raw-1080p | veryslow | 13.72 | n/a (binary missing) | — | n/a | NOT CERTIFIED |
| twitter-640x360 | ultrafast | 365.77 | n/a (binary missing) | — | n/a | NOT CERTIFIED |
| twitter-640x360 | medium | 242.66 | n/a (binary missing) | — | n/a | NOT CERTIFIED |
| twitter-640x360 | veryslow | 102.97 | n/a (binary missing) | — | n/a | NOT CERTIFIED |
| twitter-1080p | ultrafast | 168.57 | n/a (binary missing) | — | n/a | NOT CERTIFIED |
| twitter-1080p | medium | 75.10 | n/a (binary missing) | — | n/a | NOT CERTIFIED |
| twitter-1080p | veryslow | 57.30 | n/a (binary missing) | — | n/a | NOT CERTIFIED |
| twitter-720p60 | ultrafast | 256.38 | n/a (binary missing) | — | n/a | NOT CERTIFIED |
| twitter-720p60 | medium | 125.98 | n/a (binary missing) | — | n/a | NOT CERTIFIED |
| twitter-720p60 | veryslow | 67.73 | n/a (binary missing) | — | n/a | NOT CERTIFIED |

The candidate column stays `n/a` / `NOT CERTIFIED` unless the candidate binary exists, runs and decodes cleanly on **every** case. A measured ratio is shown only for a cleanly decoding candidate; no speedup is inferred from a failed or absent binary.

## Quality vs exact source I420 (ffmpeg PSNR / SSIM)

| Clip | Preset | Oracle PSNR dB | Oracle SSIM | Candidate PSNR dB | Candidate SSIM |
|---|---|---:|---:|---:|---:|
| fortnite-raw-1080p | ultrafast | 44.88 | 0.9874 | n/a | n/a |
| fortnite-raw-1080p | medium | 42.64 | 0.9849 | n/a | n/a |
| fortnite-raw-1080p | veryslow | 42.29 | 0.9839 | n/a | n/a |
| twitter-640x360 | ultrafast | 45.82 | 0.9915 | n/a | n/a |
| twitter-640x360 | medium | 44.79 | 0.9935 | n/a | n/a |
| twitter-640x360 | veryslow | 45.24 | 0.9936 | n/a | n/a |
| twitter-1080p | ultrafast | 64.62 | 0.9998 | n/a | n/a |
| twitter-1080p | medium | 60.36 | 0.9997 | n/a | n/a |
| twitter-1080p | veryslow | 59.88 | 0.9997 | n/a | n/a |
| twitter-720p60 | ultrafast | 49.93 | 0.9958 | n/a | n/a |
| twitter-720p60 | medium | 48.09 | 0.9966 | n/a | n/a |
| twitter-720p60 | veryslow | 47.78 | 0.9961 | n/a | n/a |

## Documented reference anchors (historical, not re-derived here)

| Case | fps | Source |
|---|---:|---|
| 640x360, 120 f, medium, CRF 23, 6 threads (synthetic testsrc2) | 609.14 | `benchmarks.md / README.md` |
| 1920x1080, 8 f, ultrafast, CRF 23, 6 threads (Fortnite I420) | 126.98 | `benchmarks.md / README.md` |
| 1920x1080, 8 f, medium, CRF 23, 6 threads (Fortnite I420) | 40.61 | `benchmarks.md / README.md` |
| 1920x1080, 8 f, veryslow, CRF 23, 6 threads (Fortnite I420) | 13.84 | `benchmarks.md / README.md` |

These four numbers are the already-established reference measurements published in `benchmarks.md` / `README.md`. The table above contains this run's fresh oracle measurements and is the authoritative result for this run.

## Summary

- Cases: 12; oracle-decoded: 12; candidate-certified: 0
- Presets: ultrafast, medium, veryslow; frames/case: 8; reps: 2; warm-ups: 1
- Machine: Intel64 Family 6 Model 151 Stepping 5, GenuineIntel, 12 logical CPUs
